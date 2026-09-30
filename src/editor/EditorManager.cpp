#include "EditorManager.h"

#include "CodeEditor.h"
#include "Document.h"
#include "FindBar.h"
#include "dialogs/UnsavedChangesDialog.h"
#include "filesystem/FileManager.h"
#include "settings/SettingsManager.h"

#include <QApplication>
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
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr qint64 kLargeFileBytes = 20 * 1024 * 1024;
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

    auto *welcome = new QLabel(tr("<div style='text-align:center'><h2>QODE</h2>"
                                  "<p>Open a file from the Project Explorer, or press<br>"
                                  "<b>Ctrl+O</b> to open a file &nbsp;·&nbsp; <b>Ctrl+Shift+O</b> to open a project<br>"
                                  "<b>Ctrl+J</b> toggles the terminal</p></div>"),
                               this);
    welcome->setAlignment(Qt::AlignCenter);
    welcome->setStyleSheet(QStringLiteral("color: palette(mid);"));

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(welcome);
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

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &EditorManager::onTabCloseRequested);
    connect(m_tabs, &QTabWidget::currentChanged, this, &EditorManager::onCurrentTabChanged);
    updateStack();
}

// --- Lookup helpers ----------------------------------------------------------

int EditorManager::indexOf(Document *doc) const
{
    for (int i = 0; i < m_tabs->count(); ++i)
        if (m_docForEditor.value(qobject_cast<CodeEditor *>(m_tabs->widget(i))) == doc)
            return i;
    return -1;
}

EditorManager::Entry EditorManager::entryAt(int index) const
{
    auto *ed = qobject_cast<CodeEditor *>(m_tabs->widget(index));
    return {ed ? m_docForEditor.value(ed) : nullptr, ed};
}

Document *EditorManager::currentDocument() const
{
    return entryAt(m_tabs->currentIndex()).doc;
}

CodeEditor *EditorManager::currentEditor() const
{
    return qobject_cast<CodeEditor *>(m_tabs->currentWidget());
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

void EditorManager::newUntitled()
{
    auto *doc = new Document(this);
    addDocument(doc);
    focusEditor();
}

EditorManager::Entry EditorManager::addDocument(Document *doc)
{
    auto *editor = new CodeEditor(m_tabs);
    editor->setDocument(doc->textDocument());
    editor->setIndentAfterColon(doc->languageName() == QLatin1String("Python"));
    m_docForEditor.insert(editor, doc);

    connect(doc, &Document::stateChanged, this, [this, doc] {
        updateTabTitle(doc);
        emit documentStateChanged();
    });
    connect(doc, &Document::pathChanged, this, [this, doc, editor](const QString &) {
        updateTabTitle(doc);
        editor->setIndentAfterColon(doc->languageName() == QLatin1String("Python"));
        emit documentStateChanged();
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

    const int idx = m_tabs->addTab(editor, doc->fileName());
    m_tabs->setCurrentIndex(idx);
    updateTabTitle(doc);
    updateStack();
    emit countChanged(m_tabs->count());
    return {doc, editor};
}

void EditorManager::updateTabTitle(Document *doc)
{
    const int i = indexOf(doc);
    if (i < 0)
        return;
    m_tabs->setTabText(i, doc->fileName() + (doc->isModified() ? QStringLiteral(" *") : QString()));
    m_tabs->setTabToolTip(i, doc->isUntitled() ? tr("Untitled") : doc->filePath());
}

void EditorManager::onCurrentTabChanged(int index)
{
    const Entry e = entryAt(index);
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
    m_tabs->removeTab(index);
    m_docForEditor.remove(e.editor);
    // Delete the view before the document it displays.
    delete e.editor;
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
