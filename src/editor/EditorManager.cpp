#include "EditorManager.h"

#include "CodeEditor.h"
#include "Breadcrumbs.h"
#include "Document.h"
#include "WelcomePage.h"
#include "explorer/FileIcons.h"
#include "FindBar.h"
#include "dialogs/UnsavedChangesDialog.h"
#include "filesystem/FileManager.h"
#include "format/Formatter.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QGuiApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr qint64 kLargeFileBytes = 20 * 1024 * 1024;

// Replaces only the part of the document that differs, in one undo step, so the caret and scroll position
// survive a reformat.
void replaceDocumentText(QTextDocument *doc, const QString &oldText, const QString &newText)
{
    if (oldText == newText)
        return;
    const int maxPre = qMin(oldText.size(), newText.size());
    int pre = 0;
    while (pre < maxPre && oldText.at(pre) == newText.at(pre))
        ++pre;
    int suf = 0;
    while (suf < maxPre - pre && oldText.at(oldText.size() - 1 - suf) == newText.at(newText.size() - 1 - suf))
        ++suf;
    QTextCursor c(doc);
    c.beginEditBlock();
    c.setPosition(pre);
    c.setPosition(int(oldText.size() - suf), QTextCursor::KeepAnchor);
    c.insertText(newText.mid(pre, newText.size() - pre - suf));
    c.endEditBlock();
}

// What a tab shows: the breadcrumb bar on top of the editor.
class EditorPane : public QWidget
{
public:
    EditorPane(CodeEditor *ed, QWidget *parent)
        : QWidget(parent)
        , editor(ed)
        , crumbs(new Breadcrumbs(this))
    {
        ed->setParent(this);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(crumbs);
        lay->addWidget(ed, 1);
        setFocusProxy(ed);
    }
    CodeEditor *editor;
    Breadcrumbs *crumbs;
};
}

EditorManager::EditorManager(QWidget *parent)
    : QWidget(parent)
{
    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setElideMode(Qt::ElideRight);
    m_tabs->tabBar()->setUsesScrollButtons(true);
    m_tabs->tabBar()->installEventFilter(this);

    m_find = new FindBar(this);

    auto *editorPage = new QWidget(this);
    auto *editorLayout = new QVBoxLayout(editorPage);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    editorLayout->addWidget(m_tabs);

    m_welcome = new WelcomePage(this);
    connect(m_welcome, &WelcomePage::newProjectRequested, this, &EditorManager::newProjectRequested);
    connect(m_welcome, &WelcomePage::openProjectRequested, this, &EditorManager::openProjectRequested);
    connect(m_welcome, &WelcomePage::newFileRequested, this, &EditorManager::newFileRequested);
    connect(m_welcome, &WelcomePage::openFileRequested, this, &EditorManager::openFileRequested);
    connect(m_welcome, &WelcomePage::openRecentRequested, this, &EditorManager::openRecentProjectRequested);
    connect(m_welcome, &WelcomePage::removeRecentRequested, this, &EditorManager::removeRecentProjectRequested);
    connect(m_welcome, &WelcomePage::clearRecentRequested, this, &EditorManager::clearRecentProjectsRequested);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_welcome);
    m_stack->addWidget(editorPage);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_find);
    layout->addWidget(m_stack, 1);

    m_watcher = new QFileSystemWatcher(this);
    m_changeTimer = new QTimer(this);
    m_changeTimer->setSingleShot(true);
    m_changeTimer->setInterval(200);
    connect(m_changeTimer, &QTimer::timeout, this, &EditorManager::processPendingChanges);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &EditorManager::onWatchedFileChanged);

    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setSingleShot(true);
    connect(m_autoSaveTimer, &QTimer::timeout, this, &EditorManager::autoSaveAll);
    connect(&SettingsManager::instance(), &SettingsManager::saveSettingsChanged, this, &EditorManager::applySaveSettings);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState st) {
        if (st == Qt::ApplicationInactive && SettingsManager::instance().autoSaveMode() == SettingsManager::AutoSaveOnFocusChange)
            autoSaveAll();
    });
    applySaveSettings();
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, &EditorManager::updateAllCrumbs);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] {
        for (Document *d : documents())
            updateTabTitle(d);
    });
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &EditorManager::onTabCloseRequested);
    connect(m_tabs, &QTabWidget::currentChanged, this, &EditorManager::onCurrentTabChanged);
    updateStack();
}

// --- Lookup helpers ----------------------------------------------------------

int EditorManager::indexOf(Document *doc) const
{
    for (int i = 0; i < m_tabs->count(); ++i)
        if (entryAt(i).doc == doc)
            return i;
    return -1;
}

EditorManager::Entry EditorManager::entryAt(int index) const
{
    auto *pane = static_cast<EditorPane *>(m_tabs->widget(index));
    CodeEditor *ed = pane ? pane->editor : nullptr;
    return {ed ? m_docForEditor.value(ed) : nullptr, ed};
}

Document *EditorManager::currentDocument() const
{
    return entryAt(m_tabs->currentIndex()).doc;
}

CodeEditor *EditorManager::currentEditor() const
{
    auto *pane = static_cast<EditorPane *>(m_tabs->currentWidget());
    return pane ? pane->editor : nullptr;
}

CodeEditor *EditorManager::editorFor(Document *doc) const
{
    const int i = indexOf(doc);
    return i < 0 ? nullptr : entryAt(i).editor;
}

QList<Document *> EditorManager::documents() const
{
    QList<Document *> out;
    for (int i = 0; i < m_tabs->count(); ++i)
        out << entryAt(i).doc;
    return out;
}

QList<Document *> EditorManager::modifiedDocuments() const
{
    QList<Document *> out;
    for (Document *d : documents())
        if (d->isModified())
            out << d;
    return out;
}

QStringList EditorManager::openFilePaths() const
{
    QStringList out;
    for (Document *d : documents())
        if (!d->isUntitled())
            out << d->filePath();
    return out;
}

int EditorManager::count() const
{
    return m_tabs->count();
}

void EditorManager::updateStack()
{
    m_stack->setCurrentIndex(m_tabs->count() == 0 ? 0 : 1);
}

// --- Opening -----------------------------------------------------------------

bool EditorManager::openFile(const QString &pathIn)
{
    const QFileInfo fi(pathIn);
    const QString path = fi.absoluteFilePath();

    // Already open? Activate the existing tab.
    for (Document *d : documents()) {
        if (d->filePath() == path) {
            m_tabs->setCurrentIndex(indexOf(d));
            focusEditor();
            return true;
        }
    }
    if (fi.isDir()) {
        QMessageBox::warning(this, tr("Unable to open file"), tr("\"%1\" is a directory.").arg(path));
        return false;
    }
    if (fi.size() > kLargeFileBytes) {
        const auto r = QMessageBox::question(this, tr("Large file"),
                                             tr("\"%1\" is %2 MB. Opening very large files may be slow.\n\nOpen anyway?")
                                                 .arg(fi.fileName())
                                                 .arg(fi.size() / (1024 * 1024)));
        if (r != QMessageBox::Yes)
            return false;
    }

    auto *doc = new Document(this);
    QString err;
    if (!doc->load(path, &err)) {
        QMessageBox::warning(this, tr("Unable to open file"),
                             tr("Unable to open file.\n\nPath:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
        delete doc;
        return false;
    }
    if (doc->text().contains(QChar(0))) {
        const auto r = QMessageBox::question(this, tr("Binary file"),
                                             tr("\"%1\" appears to be a binary file. Open it as text anyway?").arg(fi.fileName()));
        if (r != QMessageBox::Yes) {
            delete doc;
            return false;
        }
    }
    addDocument(doc);
    watch(path);
    focusEditor();
    return true;
}

bool EditorManager::openFileAt(const QString &path, int line, int column, int length)
{
    if (!openFile(path))
        return false;
    gotoLine(line, column, length);
    return true;
}

Document *EditorManager::documentForPath(const QString &path) const
{
    for (Document *d : documents())
        if (!d->isUntitled() && d->filePath() == path)
            return d;
    return nullptr;
}

void EditorManager::gotoLine(int line, int column, int length)
{
    CodeEditor *e = currentEditor();
    if (!e || line < 1)
        return;
    QTextCursor c(e->document()->findBlockByNumber(qMin(line, e->blockCount()) - 1));
    if (column > 1)
        c.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, qMin(column - 1, c.block().length() - 1));
    if (length > 0)
        c.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, qMin(length, c.block().length() - 1 - c.positionInBlock()));
    e->setTextCursor(c);
    e->centerCursor();
    e->setFocus();
}

void EditorManager::newUntitled()
{
    auto *doc = new Document(this);
    addDocument(doc);
    focusEditor();
}

EditorManager::Entry EditorManager::addDocument(Document *doc)
{
    auto *editor = new CodeEditor;
    auto *pane = new EditorPane(editor, m_tabs);
    editor->attachDocument(doc->textDocument());
    editor->setIndentAfterColon(doc->languageName() == QLatin1String("Python"));
    m_docForEditor.insert(editor, doc);

    connect(doc, &Document::stateChanged, this, [this, doc] {
        updateTabTitle(doc);
        emit documentStateChanged();
    });
    connect(doc->textDocument(), &QTextDocument::contentsChanged, this, [this] {
        if (SettingsManager::instance().autoSaveMode() == SettingsManager::AutoSaveAfterDelay)
            m_autoSaveTimer->start(); // restarts, so it fires after the user pauses typing
    });
    auto *crumbTimer = new QTimer(pane);
    crumbTimer->setSingleShot(true);
    crumbTimer->setInterval(120);
    connect(crumbTimer, &QTimer::timeout, this, [this, doc] { updateCrumbs(doc); });
    connect(editor, &QPlainTextEdit::cursorPositionChanged, crumbTimer, qOverload<>(&QTimer::start));
    connect(pane->crumbs, &Breadcrumbs::lineRequested, this, [editor](int line) {
        QTextCursor c(editor->document()->findBlockByNumber(line));
        editor->setTextCursor(c);
        editor->centerCursor();
        editor->setFocus();
    });
    connect(doc, &Document::pathChanged, this, [this, doc, editor](const QString &) {
        updateCrumbs(doc);
        updateTabTitle(doc);
        editor->setIndentAfterColon(doc->languageName() == QLatin1String("Python"));
        emit documentStateChanged();
        emit documentPathChanged(doc);
    });
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, [this, editor] {
        if (editor == currentEditor())
            emit cursorInfoChanged();
    });
    connect(editor, &CodeEditor::overwriteModeToggled, this, [this]() { emit cursorInfoChanged(); });
    connect(editor, &CodeEditor::filesDropped, this, [this](const QStringList &paths) {
        for (const QString &p : paths)
            if (QFileInfo(p).isFile())
                openFile(p);
    });

    const int idx = m_tabs->addTab(pane, doc->fileName());
    m_tabs->setCurrentIndex(idx);
    updateTabTitle(doc);
    updateCrumbs(doc);
    updateStack();
    emit countChanged(m_tabs->count());
    emit documentAdded(doc);
    return {doc, editor};
}

void EditorManager::updateCrumbs(Document *doc)
{
    const int i = indexOf(doc);
    if (i < 0)
        return;
    auto *pane = static_cast<EditorPane *>(m_tabs->widget(i));
    const bool show = SettingsManager::instance().showBreadcrumbs();
    pane->crumbs->setVisible(show);
    if (!show)
        return;
    QList<Breadcrumbs::Crumb> path = doc->isUntitled() ? QList<Breadcrumbs::Crumb>{{tr("Untitled"), -1}}
                                                       : Breadcrumbs::pathCrumbs(doc->filePath(), m_projectRoot);
    pane->crumbs->setCrumbs(path, Breadcrumbs::symbolChain(doc->textDocument(), pane->editor->textCursor().blockNumber(), doc->languageName()));
}

void EditorManager::updateAllCrumbs()
{
    for (Document *d : documents())
        updateCrumbs(d);
}

void EditorManager::setRecentProjects(const QStringList &paths)
{
    m_welcome->setRecentProjects(paths);
}

void EditorManager::setProjectRoot(const QString &root)
{
    m_projectRoot = root;
    updateAllCrumbs();
}

void EditorManager::updateTabTitle(Document *doc)
{
    const int i = indexOf(doc);
    if (i < 0)
        return;
    m_tabs->setTabText(i, doc->fileName() + (doc->isModified() ? QStringLiteral(" *") : QString()));
    m_tabs->setTabIcon(i, FileIcons::forFile(doc->fileName()));
    m_tabs->setTabToolTip(i, doc->isUntitled() ? tr("Untitled") : doc->filePath());
}

void EditorManager::onCurrentTabChanged(int index)
{
    const Entry e = entryAt(index);
    if (SettingsManager::instance().autoSaveMode() == SettingsManager::AutoSaveOnFocusChange && m_lastDoc && m_lastDoc != e.doc)
        autoSaveDocument(m_lastDoc);
    m_lastDoc = e.doc;
    m_find->setEditor(e.editor);
    emit currentChanged();
    emit cursorInfoChanged();
}

void EditorManager::removeAt(int index)
{
    const Entry e = entryAt(index);
    if (!e.doc)
        return;
    if (!e.doc->isUntitled())
        unwatch(e.doc->filePath());
    QWidget *pane = m_tabs->widget(index);
    m_tabs->removeTab(index);
    m_docForEditor.remove(e.editor);
    // Delete the view (the pane owns it) before the document it displays.
    delete pane;
    delete e.doc;
    updateStack();
    emit countChanged(m_tabs->count());
    if (m_tabs->count() == 0) {
        m_find->setEditor(nullptr);
        emit currentChanged();
        emit cursorInfoChanged();
    }
}

// --- Saving ------------------------------------------------------------------

bool EditorManager::saveDocument(Document *doc)
{
    if (doc->isUntitled())
        return saveDocumentAs(doc);
    prepareForSave(doc, false, doc->filePath());
    while (true) {
        QString err;
        if (doc->save(&err)) {
            // Atomic-replace saves drop inotify watches; re-arm.
            watch(doc->filePath());
            return true;
        }
        QMessageBox box(QMessageBox::Warning, tr("Unable to save file"),
                        tr("Unable to save file.\n\nPath:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(doc->filePath()), err),
                        QMessageBox::NoButton, this);
        QPushButton *retry = box.addButton(tr("Retry"), QMessageBox::AcceptRole);
        QPushButton *saveAs = box.addButton(tr("Save As…"), QMessageBox::ActionRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == retry)
            continue;
        if (box.clickedButton() == saveAs)
            return saveDocumentAs(doc);
        return false;
    }
}

bool EditorManager::saveDocumentAs(Document *doc)
{
    QString start = doc->filePath();
    if (start.isEmpty())
        start = QDir(SettingsManager::instance().lastDirectory().isEmpty() ? QDir::homePath()
                                                                           : SettingsManager::instance().lastDirectory())
                    .filePath(doc->fileName());
    while (true) {
        QString path = QFileDialog::getSaveFileName(this, tr("Save As"), start);
        if (path.isEmpty())
            return false;
        // Another open tab already owns that path?
        for (Document *d : documents()) {
            if (d != doc && d->filePath() == path) {
                QMessageBox::warning(this, tr("Save As"), tr("\"%1\" is already open in another tab. Close it first.").arg(QFileInfo(path).fileName()));
                start = path;
                path.clear();
                break;
            }
        }
        if (path.isEmpty())
            continue;
        const QString old = doc->filePath();
        prepareForSave(doc, false, path);
        QString err;
        if (!doc->saveAs(path, &err)) {
            QMessageBox::warning(this, tr("Unable to save file"),
                                 tr("Unable to save file.\n\nPath:\n%1\n\nReason:\n%2").arg(FileManager::displayPath(path), err));
            start = path;
            continue;
        }
        if (!old.isEmpty())
            unwatch(old);
        watch(path);
        return true;
    }
}

void EditorManager::prepareForSave(Document *doc, bool automatic, const QString &path)
{
    auto &s = SettingsManager::instance();
    // Auto save while the user pauses typing must not reshuffle the text under their fingers.
    const bool whileTyping = automatic && s.autoSaveMode() == SettingsManager::AutoSaveAfterDelay;
    QTextDocument *td = doc->textDocument();

    if (s.formatOnSave() && !whileTyping && !path.isEmpty() && Formatter::isAvailable(path)) {
        const QString before = doc->text();
        const Formatter::Result r = Formatter::format(path, before);
        if (r.ok)
            replaceDocumentText(td, before, r.text);
        else
            emit statusMessage(tr("Format on save skipped: %1").arg(r.error));
    }

    const bool trim = s.trimTrailingWhitespace() && doc->languageName() != QLatin1String("Markdown"); // "  " is a line break there
    const bool newline = s.insertFinalNewline();
    if (!trim && !newline)
        return;
    CodeEditor *ed = editorFor(doc);
    const int caretBlock = whileTyping && ed ? ed->textCursor().blockNumber() : -1;

    QTextCursor c(td);
    c.beginEditBlock();
    if (trim) {
        for (QTextBlock b = td->begin(); b.isValid(); b = b.next()) {
            if (b.blockNumber() == caretBlock)
                continue;
            const QString text = b.text();
            int end = text.size();
            while (end > 0 && (text.at(end - 1) == QLatin1Char(' ') || text.at(end - 1) == QLatin1Char('\t')))
                --end;
            if (end == text.size())
                continue;
            QTextCursor r(td);
            r.setPosition(b.position() + end);
            r.setPosition(b.position() + text.size(), QTextCursor::KeepAnchor);
            r.removeSelectedText();
        }
    }
    if (newline && td->characterCount() > 1 && !td->lastBlock().text().isEmpty()) {
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\n"));
    }
    c.endEditBlock();
}

bool EditorManager::formatCurrent()
{
    Document *d = currentDocument();
    if (!d)
        return false;
    if (d->isUntitled()) {
        emit statusMessage(tr("Save the file first so QODE knows which formatter to use."));
        return false;
    }
    const QString before = d->text();
    const Formatter::Result r = Formatter::format(d->filePath(), before);
    if (!r.ok) {
        emit statusMessage(r.error);
        return false;
    }
    if (r.text == before) {
        emit statusMessage(tr("Already formatted (%1)").arg(r.tool));
        return true;
    }
    replaceDocumentText(d->textDocument(), before, r.text);
    emit statusMessage(tr("Formatted with %1").arg(r.tool));
    return true;
}

void EditorManager::applySaveSettings()
{
    auto &s = SettingsManager::instance();
    m_autoSaveTimer->setInterval(s.autoSaveDelayMs());
    if (s.autoSaveMode() != SettingsManager::AutoSaveAfterDelay)
        m_autoSaveTimer->stop();
    else if (!modifiedDocuments().isEmpty())
        m_autoSaveTimer->start();
}

// Silent save for auto save: never opens a dialog, and never overwrites a file that changed on disk.
bool EditorManager::autoSaveDocument(Document *doc)
{
    if (!doc || !doc->isModified() || doc->isUntitled() || m_prompting)
        return false;
    if (!doc->existsOnDisk() || doc->changedOnDisk())
        return false; // the external-change prompt decides what happens to this one
    prepareForSave(doc, true, doc->filePath());
    QString err;
    if (!doc->save(&err)) {
        emit statusMessage(tr("Auto save failed for %1: %2").arg(doc->fileName(), err));
        return false;
    }
    watch(doc->filePath());
    return true;
}

void EditorManager::autoSaveAll()
{
    for (Document *d : modifiedDocuments())
        autoSaveDocument(d);
}

bool EditorManager::saveCurrent()
{
    Document *d = currentDocument();
    return d ? saveDocument(d) : false;
}

bool EditorManager::saveCurrentAs()
{
    Document *d = currentDocument();
    return d ? saveDocumentAs(d) : false;
}

bool EditorManager::saveAll()
{
    bool ok = true;
    for (Document *d : modifiedDocuments())
        ok = saveDocument(d) && ok;
    return ok;
}

// --- Closing -----------------------------------------------------------------

bool EditorManager::maybeSave(Document *doc)
{
    if (!doc->isModified())
        return true;
    switch (UnsavedChangesDialog::ask(this, {doc->fileName()})) {
    case UnsavedChangesDialog::SaveAll:
        return saveDocument(doc);
    case UnsavedChangesDialog::Discard:
        return true;
    default:
        return false;
    }
}

bool EditorManager::closeDocument(Document *doc)
{
    const int i = indexOf(doc);
    if (i < 0)
        return true;
    m_tabs->setCurrentIndex(i);
    if (!maybeSave(doc))
        return false;
    removeAt(i);
    return true;
}

bool EditorManager::closeCurrent()
{
    Document *d = currentDocument();
    return d ? closeDocument(d) : false;
}

void EditorManager::onTabCloseRequested(int index)
{
    closeDocument(entryAt(index).doc);
}

bool EditorManager::confirmDiscardOrSaveAll()
{
    const QList<Document *> dirty = modifiedDocuments();
    if (dirty.isEmpty())
        return true;
    QStringList names;
    for (Document *d : dirty)
        names << d->fileName();
    switch (UnsavedChangesDialog::ask(this, names)) {
    case UnsavedChangesDialog::SaveAll:
        return saveAll();
    case UnsavedChangesDialog::Discard:
        return true;
    default:
        return false;
    }
}

bool EditorManager::closeAll()
{
    if (!confirmDiscardOrSaveAll())
        return false;
    while (m_tabs->count() > 0)
        removeAt(m_tabs->count() - 1);
    return true;
}

void EditorManager::closeDocumentsUnder(const QString &path)
{
    const QString prefix = path + QLatin1Char('/');
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        Document *d = entryAt(i).doc;
        if (d && !d->isUntitled() && (d->filePath() == path || d->filePath().startsWith(prefix)))
            removeAt(i);
    }
}

void EditorManager::pathRenamed(const QString &oldPath, const QString &newPath)
{
    const QString prefix = oldPath + QLatin1Char('/');
    for (Document *d : documents()) {
        if (d->isUntitled())
            continue;
        QString target;
        if (d->filePath() == oldPath)
            target = newPath;
        else if (d->filePath().startsWith(prefix))
            target = newPath + QLatin1Char('/') + d->filePath().mid(prefix.size());
        if (target.isEmpty())
            continue;
        unwatch(d->filePath());
        d->setPath(target);
        watch(target);
    }
}

// --- Navigation --------------------------------------------------------------

void EditorManager::nextTab()
{
    if (m_tabs->count() > 1)
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
}

void EditorManager::previousTab()
{
    if (m_tabs->count() > 1)
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
}

void EditorManager::showFind()
{
    if (currentEditor())
        m_find->showFind();
}

void EditorManager::showReplace()
{
    if (currentEditor())
        m_find->showReplace();
}

void EditorManager::focusEditor()
{
    if (CodeEditor *e = currentEditor())
        e->setFocus();
}

bool EditorManager::eventFilter(QObject *obj, QEvent *event)
{
    // Middle-click on a tab closes it.
    if (obj == m_tabs->tabBar() && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::MiddleButton) {
            const int i = m_tabs->tabBar()->tabAt(me->position().toPoint());
            if (i >= 0) {
                onTabCloseRequested(i);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

// --- External modification ---------------------------------------------------

void EditorManager::watch(const QString &path)
{
    if (!path.isEmpty() && !m_watcher->files().contains(path))
        m_watcher->addPath(path);
}

void EditorManager::unwatch(const QString &path)
{
    if (!path.isEmpty())
        m_watcher->removePath(path);
}

void EditorManager::onWatchedFileChanged(const QString &path)
{
    if (!m_pendingChanges.contains(path))
        m_pendingChanges << path;
    m_changeTimer->start();
}

void EditorManager::processPendingChanges()
{
    if (m_prompting) {
        m_changeTimer->start(500);
        return;
    }
    const QStringList paths = m_pendingChanges;
    m_pendingChanges.clear();

    for (const QString &path : paths) {
        Document *doc = nullptr;
        for (Document *d : documents())
            if (d->filePath() == path)
                doc = d;
        if (!doc)
            continue;

        if (!doc->existsOnDisk()) {
            // Deleted or moved away. Keep the buffer, mark it modified so it can be re-saved.
            m_prompting = true;
            QMessageBox::warning(this, tr("File removed"),
                                 tr("The file \"%1\" no longer exists on disk.\n\nThe editor content is kept; save it to recreate the file.")
                                     .arg(doc->fileName()));
            m_prompting = false;
            doc->setModified(true);
            continue;
        }
        watch(path); // atomic replace drops the watch
        if (!doc->changedOnDisk())
            continue; // our own save

        m_prompting = true;
        m_tabs->setCurrentIndex(indexOf(doc));
        QMessageBox box(QMessageBox::Question, tr("File changed"), QString(), QMessageBox::NoButton, this);
        QString text = tr("The file \"%1\" has been modified outside QODE.").arg(doc->fileName());
        if (doc->isModified())
            text += tr("\n\nYou have unsaved changes in the editor; reloading will discard them.");
        box.setText(text);
        QPushButton *reload = box.addButton(tr("Reload"), QMessageBox::AcceptRole);
        QPushButton *keep = box.addButton(tr("Keep Current"), QMessageBox::RejectRole);
        box.setDefaultButton(doc->isModified() ? keep : reload);
        box.exec();
        m_prompting = false;

        if (box.clickedButton() == reload) {
            QString err;
            if (!doc->reload(&err))
                QMessageBox::warning(this, tr("Unable to reload"), tr("Reason:\n%1").arg(err));
        } else {
            // Keep the editor text; treat it as diverged from disk so the user is not asked again
            // until the file changes once more, and saving will overwrite deliberately.
            doc->setPath(doc->filePath());
            doc->setModified(true);
        }
    }
}
