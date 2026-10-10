#include "app/InstanceRegistry.h"
#include "EditorManager.h"

#include "CodeEditor.h"
#include "CsvTable.h"
#include "Breadcrumbs.h"
#include "EditorGroup.h"
#include "Document.h"
#include "WelcomePage.h"
#include "explorer/FileIcons.h"
#include "FindBar.h"
#include "dialogs/UnsavedChangesDialog.h"
#include "filesystem/FileManager.h"
#include "media/MediaPanel.h"
#include "format/Formatter.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QGuiApplication>
#include <QDir>
#include <QDrag>
#include <QJsonArray>
#include <QMenu>
#include <QMimeData>
#include <QSplitter>
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
#include <functional>

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
    CsvTableWidget *table = nullptr; // CSV / TSV files can be shown as a grid instead of the text; created on first use

    bool tableShown() const { return table && table->isVisible(); }
    // Switches between the text and the grid view, carrying the caret / current cell across.
    void showTable(bool on, QTextDocument *doc, const QString &fileName)
    {
        if (on == tableShown())
            return;
        auto *lay = static_cast<QVBoxLayout *>(layout());
        if (on) {
            if (!table) {
                table = new CsvTableWidget(doc, fileName, this);
                lay->insertWidget(1, table, 1);
            }
            table->setFileName(fileName);
            const int position = editor->textCursor().position();
            editor->hide();
            table->show();
            setFocusProxy(table->view());
            table->activate(position);
        } else {
            const int position = table->currentPosition();
            table->hide();
            editor->show();
            setFocusProxy(editor);
            if (position >= 0) {
                QTextCursor c(doc);
                c.setPosition(qMin(position, qMax(0, doc->characterCount() - 1)));
                editor->setTextCursor(c);
                editor->centerCursor();
            }
            editor->setFocus();
        }
    }
};
}

EditorManager::EditorManager(QWidget *parent)
    : QWidget(parent)
{
    m_root = new QSplitter(Qt::Horizontal, this);
    m_root->setChildrenCollapsible(false);
    m_root->setHandleWidth(4);
    m_active = createGroup();
    m_root->addWidget(m_active);

    m_find = new FindBar(this);

    auto *editorPage = new QWidget(this);
    auto *editorLayout = new QVBoxLayout(editorPage);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    editorLayout->addWidget(m_root);

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
    // Typing in a group makes it the one new files open in.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (QWidget *w = now; w; w = w->parentWidget()) {
            if (auto *g = qobject_cast<EditorGroup *>(w)) {
                if (g != m_active && g->parentWidget() && m_root->isAncestorOf(g))
                    setActiveGroup(g);
                return;
            }
        }
    });
    updateStack();
}

// --- Lookup helpers ----------------------------------------------------------

QList<EditorGroup *> EditorManager::groups() const
{
    QList<EditorGroup *> out;
    std::function<void(QWidget *)> walk = [&](QWidget *w) {
        if (auto *g = qobject_cast<EditorGroup *>(w))
            out << g;
        else if (auto *sp = qobject_cast<QSplitter *>(w))
            for (int i = 0; i < sp->count(); ++i)
                walk(sp->widget(i));
    };
    walk(m_root);
    return out;
}

int EditorManager::groupCount() const
{
    return groups().size();
}

EditorManager::Entry EditorManager::entryIn(EditorGroup *group, int index) const
{
    auto *pane = group ? static_cast<EditorPane *>(group->tabs()->widget(index)) : nullptr;
    CodeEditor *ed = pane ? pane->editor : nullptr;
    return {ed ? m_docForEditor.value(ed) : nullptr, ed};
}

EditorManager::Loc EditorManager::locate(Document *doc) const
{
    for (EditorGroup *g : groups())
        for (int i = 0; i < g->tabs()->count(); ++i)
            if (entryIn(g, i).doc == doc)
                return {g, i};
    return {};
}

Document *EditorManager::currentDocument() const
{
    return entryIn(m_active, m_active->tabs()->currentIndex()).doc;
}

CodeEditor *EditorManager::currentEditor() const
{
    auto *pane = static_cast<EditorPane *>(m_active->tabs()->currentWidget());
    return pane ? pane->editor : nullptr;
}

CodeEditor *EditorManager::editorFor(Document *doc) const
{
    const Loc l = locate(doc);
    return l.group ? entryIn(l.group, l.index).editor : nullptr;
}

QList<Document *> EditorManager::documents() const
{
    QList<Document *> out;
    for (EditorGroup *g : groups())
        for (int i = 0; i < g->tabs()->count(); ++i)
            out << entryIn(g, i).doc;
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

// Tell other QODE windows which files this one has open.
void EditorManager::syncFileClaims()
{
    InstanceRegistry::instance().setClaims(InstanceRegistry::File, openFilePaths());
}

int EditorManager::count() const
{
    int n = 0;
    for (EditorGroup *g : groups())
        n += g->tabs()->count();
    return n;
}

void EditorManager::updateStack()
{
    m_stack->setCurrentIndex(count() == 0 ? 0 : 1);
}

// --- Opening -----------------------------------------------------------------

bool EditorManager::openFile(const QString &pathIn)
{
    const QFileInfo fi(pathIn);
    const QString path = fi.absoluteFilePath();

    if (fi.isFile() && MediaPanel::isMedia(path)) {
        emit mediaRequested(path);
        return true;
    }

    // Already open? Activate the existing tab.
    for (Document *d : documents()) {
        if (d->filePath() == path) {
            activate(d);
            focusEditor();
            return true;
        }
    }
    if (fi.isDir()) {
        QMessageBox::warning(this, tr("Unable to open file"), tr("\"%1\" is a directory.").arg(path));
        return false;
    }
    if (InstanceRegistry::instance().isHeldElsewhere(InstanceRegistry::File, path)) {
        QMessageBox::information(this, tr("File already open"),
                                 tr("\"%1\" is currently open in another QODE window.\n\nClose it there first, or switch to that window.")
                                     .arg(fi.fileName()));
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

// Emmet works in HTML files and in React files (.jsx / .tsx): 1 = HTML, 2 = JSX, 0 = off.
static int emmetModeFor(const Document *doc)
{
    const QString ext = QFileInfo(doc->filePath()).suffix().toLower();
    if (ext == QLatin1String("jsx") || ext == QLatin1String("tsx"))
        return 2;
    return doc->languageName() == QLatin1String("HTML") ? 1 : 0;
}

EditorManager::Entry EditorManager::addDocument(Document *doc)
{
    auto *editor = new CodeEditor;
    auto *pane = new EditorPane(editor, m_active->tabs());
    editor->attachDocument(doc->textDocument());
    editor->setIndentAfterColon(doc->languageName() == QLatin1String("Python"));
    editor->setLanguage(doc->languageName());
    editor->setEmmetMode(emmetModeFor(doc));
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
    connect(pane->crumbs, &Breadcrumbs::previewRequested, this, &EditorManager::previewRequested);
    connect(pane->crumbs, &Breadcrumbs::tableToggleRequested, this, [this, doc, pane] {
        pane->showTable(!pane->tableShown(), doc->textDocument(), doc->filePath());
        updateCrumbs(doc);
    });
    connect(editor, &CodeEditor::blameCommitRequested, this, &EditorManager::blameCommitRequested);
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
        editor->setLanguage(doc->languageName());
        editor->setEmmetMode(emmetModeFor(doc));
        emit documentStateChanged();
        syncFileClaims();
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

    const int idx = m_active->tabs()->addTab(pane, doc->fileName());
    m_active->tabs()->setCurrentIndex(idx);
    updateTabTitle(doc);
    updateCrumbs(doc);
    updateStack();
    emit countChanged(count());
    syncFileClaims();
    emit documentAdded(doc);
    return {doc, editor};
}

bool EditorManager::isTabular(const Document *doc)
{
    return doc && (doc->languageName() == QLatin1String("CSV") || doc->languageName() == QLatin1String("TSV"));
}

bool EditorManager::isPreviewable(const Document *doc)
{
    return doc && (doc->languageName() == QLatin1String("Markdown") || doc->filePath().endsWith(QLatin1String(".svg"), Qt::CaseInsensitive));
}

void EditorManager::updateCrumbs(Document *doc)
{
    const Loc l = locate(doc);
    if (!l.group)
        return;
    auto *pane = static_cast<EditorPane *>(l.group->tabs()->widget(l.index));
    const bool show = SettingsManager::instance().showBreadcrumbs();
    pane->crumbs->setVisible(show);
    pane->crumbs->setPreviewAvailable(isPreviewable(doc));
    if (pane->table && !isTabular(doc))
        pane->showTable(false, doc->textDocument(), doc->filePath()); // renamed away from .csv
    else if (pane->table)
        pane->table->setFileName(doc->filePath());
    pane->crumbs->setTableToggle(isTabular(doc), pane->tableShown());
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

namespace {
void applyTabTitle(QTabWidget *tabs, int i, Document *doc)
{
    tabs->setTabText(i, doc->fileName() + (doc->isModified() ? QStringLiteral(" *") : QString()));
    tabs->setTabIcon(i, FileIcons::forFile(doc->fileName()));
    tabs->setTabToolTip(i, doc->isUntitled() ? QObject::tr("Untitled") : doc->filePath());
}
}

void EditorManager::updateTabTitle(Document *doc)
{
    const Loc l = locate(doc);
    if (l.group)
        applyTabTitle(l.group->tabs(), l.index, doc);
}

void EditorManager::onActiveTabChanged()
{
    const Entry e = entryIn(m_active, m_active->tabs()->currentIndex());
    if (SettingsManager::instance().autoSaveMode() == SettingsManager::AutoSaveOnFocusChange && m_lastDoc && m_lastDoc != e.doc)
        autoSaveDocument(m_lastDoc);
    m_lastDoc = e.doc;
    m_find->setEditor(e.editor);
    updateGroupMarkers();
    emit currentChanged();
    emit cursorInfoChanged();
}

void EditorManager::updateGroupMarkers()
{
    const QList<EditorGroup *> all = groups();
    for (EditorGroup *g : all)
        g->setActiveMarker(all.size() > 1, g == m_active);
}

void EditorManager::removeDocument(Document *doc)
{
    const Loc l = locate(doc);
    if (!l.group)
        return;
    const Entry e = entryIn(l.group, l.index);
    if (!doc->isUntitled())
        unwatch(doc->filePath());
    QWidget *pane = l.group->tabs()->widget(l.index);
    l.group->tabs()->removeTab(l.index);
    m_docForEditor.remove(e.editor);
    // Delete the view (the pane owns it) before the document it displays.
    delete pane;
    delete doc;
    removeGroupIfEmpty(l.group);
    syncFileClaims();
    updateStack();
    emit countChanged(count());
    if (count() == 0) {
        m_find->setEditor(nullptr);
        emit currentChanged();
        emit cursorInfoChanged();
    }
}

// --- Groups / splitting ---------------------------------------------------------

EditorGroup *EditorManager::createGroup()
{
    auto *g = new EditorGroup;
    QTabWidget *tabs = g->tabs();
    tabs->tabBar()->installEventFilter(this);
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this, g](int i) { closeDocument(entryIn(g, i).doc); });
    connect(tabs, &QTabWidget::currentChanged, this, [this, g] {
        if (g == m_active)
            onActiveTabChanged();
    });
    connect(tabs, &QTabWidget::tabBarClicked, this, [this, g] { setActiveGroup(g); });
    connect(g, &EditorGroup::tabDragStarted, this, [this, g](int i) { startTabDrag(g, i); });
    connect(g, &EditorGroup::tabContextMenuRequested, this, [this, g](int i, const QPoint &p) { showTabMenu(g, i, p); });
    connect(g, &EditorGroup::tabDropped, this, [this, g](EditorGroup::Zone z) {
        if (m_dragDoc)
            moveDocument(m_dragDoc, g, z);
    });
    return g;
}

void EditorManager::setActiveGroup(EditorGroup *group)
{
    if (!group || group == m_active)
        return;
    m_active = group;
    onActiveTabChanged();
}

// Brings a document's tab to the front of its group and makes that group the active one.
void EditorManager::activate(Document *doc)
{
    const Loc l = locate(doc);
    if (!l.group)
        return;
    l.group->tabs()->setCurrentIndex(l.index);
    setActiveGroup(l.group);
}

namespace {
// Moves the tab holding `pane` into `dest` without touching the document or view.
void movePane(QTabWidget *from, QWidget *pane, QTabWidget *to, Document *doc)
{
    from->removeTab(from->indexOf(pane));
    const int idx = to->addTab(pane, QString());
    applyTabTitle(to, idx, doc);
    to->setCurrentIndex(idx);
}

void setupSplitter(QSplitter *sp)
{
    sp->setChildrenCollapsible(false);
    sp->setHandleWidth(4);
}
}

void EditorManager::moveDocument(Document *doc, EditorGroup *target, EditorGroup::Zone zone)
{
    const Loc src = locate(doc);
    if (!src.group || !target)
        return;
    EditorGroup *dest = target;
    if (zone == EditorGroup::Center) {
        if (src.group == target)
            return;
    } else {
        if (src.group == target && target->tabs()->count() == 1)
            return; // nothing would be left behind
        const Qt::Orientation o = zone == EditorGroup::Left || zone == EditorGroup::Right ? Qt::Horizontal : Qt::Vertical;
        const bool after = zone == EditorGroup::Right || zone == EditorGroup::Bottom;
        dest = createGroup();
        auto *parent = qobject_cast<QSplitter *>(target->parentWidget());
        const int idx = parent->indexOf(target);
        const int extent = o == Qt::Horizontal ? target->width() : target->height();
        if (parent->count() == 1) {
            parent->setOrientation(o);
            parent->insertWidget(after ? idx + 1 : idx, dest);
            parent->setSizes({extent / 2, extent - extent / 2});
        } else if (parent->orientation() == o) {
            QList<int> sizes = parent->sizes();
            const int total = sizes.value(idx);
            parent->insertWidget(after ? idx + 1 : idx, dest);
            sizes.insert(after ? idx + 1 : idx, total / 2);
            sizes[after ? idx : idx + 1] = total - total / 2;
            parent->setSizes(sizes);
        } else {
            auto *sub = new QSplitter(o);
            setupSplitter(sub);
            const QList<int> sizes = parent->sizes();
            parent->replaceWidget(idx, sub);
            sub->addWidget(target);
            sub->insertWidget(after ? 1 : 0, dest);
            sub->show();
            target->show();
            sub->setSizes({extent / 2, extent - extent / 2});
            parent->setSizes(sizes);
        }
        dest->show();
    }
    movePane(src.group->tabs(), src.group->tabs()->widget(src.index), dest->tabs(), doc);
    removeGroupIfEmpty(src.group);
    setActiveGroup(dest);
    onActiveTabChanged();
    focusEditor();
}

void EditorManager::removeGroupIfEmpty(EditorGroup *g)
{
    if (g->tabs()->count() > 0 || groups().size() <= 1)
        return;
    const int pos = groups().indexOf(g);
    auto *parent = qobject_cast<QSplitter *>(g->parentWidget());
    g->hide();
    g->setParent(nullptr);
    g->deleteLater();
    // A splitter left with one child is replaced by that child.
    while (parent && parent != m_root && parent->count() == 1) {
        auto *grand = qobject_cast<QSplitter *>(parent->parentWidget());
        QWidget *only = parent->widget(0);
        const QList<int> sizes = grand->sizes();
        grand->replaceWidget(grand->indexOf(parent), only);
        only->show();
        parent->hide();
        parent->setParent(nullptr);
        parent->deleteLater();
        grand->setSizes(sizes);
        parent = grand;
    }
    normalizeRoot();
    if (g == m_active) {
        const QList<EditorGroup *> rest = groups();
        m_active = rest.value(qMin(pos, int(rest.size()) - 1));
        onActiveTabChanged();
    }
    updateGroupMarkers();
}

// The root splitter never holds just another splitter: its contents are lifted into it.
void EditorManager::normalizeRoot()
{
    while (m_root->count() == 1) {
        auto *sub = qobject_cast<QSplitter *>(m_root->widget(0));
        if (!sub)
            break;
        const QList<int> sizes = sub->sizes();
        QList<QWidget *> kids;
        for (int i = 0; i < sub->count(); ++i)
            kids << sub->widget(i);
        m_root->setOrientation(sub->orientation());
        for (QWidget *k : kids)
            m_root->addWidget(k);
        sub->hide();
        sub->setParent(nullptr);
        sub->deleteLater();
        m_root->setSizes(sizes);
    }
}

void EditorManager::startTabDrag(EditorGroup *group, int index)
{
    Document *doc = entryIn(group, index).doc;
    if (!doc)
        return;
    m_dragDoc = doc;
    for (EditorGroup *g : groups())
        g->setDragActive(true);
    auto *drag = new QDrag(this);
    auto *mime = new QMimeData;
    mime->setData(EditorGroup::mimeType(), QByteArray("tab"));
    drag->setMimeData(mime);
    QTabBar *bar = group->tabs()->tabBar();
    drag->setPixmap(bar->grab(bar->tabRect(index)));
    drag->exec(Qt::MoveAction);
    for (EditorGroup *g : groups())
        g->setDragActive(false);
    m_dragDoc = nullptr;
}

void EditorManager::showTabMenu(EditorGroup *group, int index, const QPoint &globalPos)
{
    QPointer<Document> doc = entryIn(group, index).doc;
    if (!doc)
        return;
    QMenu menu(this);
    menu.addAction(tr("Close"), this, [this, doc] {
        if (doc)
            closeDocument(doc);
    });
    menu.addSeparator();
    const bool canSplit = group->tabs()->count() > 1;
    auto splitTo = [this, doc, group](EditorGroup::Zone z) {
        return [this, doc, group, z] {
            if (doc)
                moveDocument(doc, group, z);
        };
    };
    menu.addAction(tr("Split Right"), this, splitTo(EditorGroup::Right))->setEnabled(canSplit);
    menu.addAction(tr("Split Down"), this, splitTo(EditorGroup::Bottom))->setEnabled(canSplit);
    const QList<EditorGroup *> all = groups();
    if (all.size() > 1) {
        EditorGroup *other = all.at((all.indexOf(group) + 1) % all.size());
        menu.addAction(tr("Move to Next Group"), this, [this, doc, other] {
            if (doc)
                moveDocument(doc, other, EditorGroup::Center);
        });
    }
    menu.exec(globalPos);
}

void EditorManager::splitCurrent(Qt::Orientation orientation)
{
    Document *doc = currentDocument();
    if (!doc)
        return;
    if (m_active->tabs()->count() < 2) {
        emit statusMessage(tr("Open another file first: the split needs a second file to show."));
        return;
    }
    moveDocument(doc, m_active, orientation == Qt::Horizontal ? EditorGroup::Right : EditorGroup::Bottom);
}

// --- Layout persistence -----------------------------------------------------------

QJsonObject EditorManager::layoutState() const
{
    std::function<QJsonObject(QWidget *)> node = [&](QWidget *w) {
        QJsonObject o;
        if (auto *g = qobject_cast<EditorGroup *>(w)) {
            QJsonArray tabs;
            QString active;
            for (int i = 0; i < g->tabs()->count(); ++i) {
                Document *d = entryIn(g, i).doc;
                if (!d || d->isUntitled())
                    continue;
                tabs.append(d->filePath());
                if (i == g->tabs()->currentIndex())
                    active = d->filePath();
            }
            o.insert(QStringLiteral("tabs"), tabs);
            o.insert(QStringLiteral("active"), active);
        } else if (auto *sp = qobject_cast<QSplitter *>(w)) {
            QJsonArray kids, sizes;
            for (int i = 0; i < sp->count(); ++i) {
                kids.append(node(sp->widget(i)));
                sizes.append(sp->sizes().value(i));
            }
            o.insert(QStringLiteral("orientation"), sp->orientation() == Qt::Horizontal ? QStringLiteral("h") : QStringLiteral("v"));
            o.insert(QStringLiteral("children"), kids);
            o.insert(QStringLiteral("sizes"), sizes);
        }
        return o;
    };
    QJsonObject root = node(m_root);
    root.insert(QStringLiteral("activeGroup"), int(groups().indexOf(m_active)));
    return root;
}

QWidget *EditorManager::buildLayout(const QJsonObject &node, QHash<QString, Document *> &docs)
{
    if (node.contains(QStringLiteral("tabs"))) {
        QList<Document *> list;
        for (const QJsonValue &v : node.value(QStringLiteral("tabs")).toArray()) {
            const auto it = docs.find(v.toString());
            if (it != docs.end()) {
                list << it.value();
                docs.erase(it);
            }
        }
        if (list.isEmpty())
            return nullptr;
        EditorGroup *g = createGroup();
        const QString active = node.value(QStringLiteral("active")).toString();
        for (Document *d : list) {
            const Loc src = locate(d);
            movePane(src.group->tabs(), src.group->tabs()->widget(src.index), g->tabs(), d);
        }
        for (int i = 0; i < g->tabs()->count(); ++i)
            if (entryIn(g, i).doc && entryIn(g, i).doc->filePath() == active)
                g->tabs()->setCurrentIndex(i);
        return g;
    }
    QList<QWidget *> kids;
    const QJsonArray children = node.value(QStringLiteral("children")).toArray();
    for (const QJsonValue &c : children)
        if (QWidget *w = buildLayout(c.toObject(), docs))
            kids << w;
    if (kids.isEmpty())
        return nullptr;
    if (kids.size() == 1)
        return kids.first();
    auto *sp = new QSplitter(node.value(QStringLiteral("orientation")).toString() == QLatin1String("v") ? Qt::Vertical : Qt::Horizontal);
    setupSplitter(sp);
    for (QWidget *k : kids)
        sp->addWidget(k);
    const QJsonArray sizes = node.value(QStringLiteral("sizes")).toArray();
    if (sizes.size() == kids.size()) {
        QList<int> sz;
        for (const QJsonValue &v : sizes)
            sz << qMax(1, v.toInt());
        sp->setSizes(sz);
    }
    return sp;
}

void EditorManager::restoreLayout(const QJsonObject &state)
{
    if (state.isEmpty() || groups().size() != 1)
        return;
    QHash<QString, Document *> docs;
    for (Document *d : documents())
        if (!d->isUntitled())
            docs.insert(d->filePath(), d);
    QWidget *built = buildLayout(state, docs);
    if (!built)
        return;
    EditorGroup *old = groups().first();
    m_root->addWidget(built);
    built->show();
    EditorGroup *first = nullptr;
    for (EditorGroup *g : groups())
        if (g != old && !first)
            first = g;
    // Files that are open but were not part of the saved layout go to the first group.
    for (Document *d : documents())
        if (locate(d).group == old && first) {
            const Loc src = locate(d);
            movePane(src.group->tabs(), src.group->tabs()->widget(src.index), first->tabs(), d);
        }
    m_active = first ? first : old;
    removeGroupIfEmpty(old);
    normalizeRoot();
    const QList<EditorGroup *> gs = groups();
    EditorGroup *wanted = gs.value(state.value(QStringLiteral("activeGroup")).toInt(), gs.first());
    m_active = wanted ? wanted : gs.first();
    onActiveTabChanged();
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
            emit documentSaved(doc);
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

    if (s.formatOnSave() && !whileTyping && !path.isEmpty()) {
        if (Formatter::isAvailable(path)) {
            const QString before = doc->text();
            const Formatter::Result r = Formatter::format(path, before);
            if (r.ok)
                replaceDocumentText(td, before, r.text);
            else
                emit statusMessage(tr("Format on save skipped: %1").arg(r.error));
        } else if (path == doc->filePath()) {
            formatWithLsp(doc); // language server formatting; silently nothing when none serves the file
        }
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

bool EditorManager::formatWithLsp(Document *doc)
{
    CodeEditor *ed = editorFor(doc);
    return m_lspFormatter && ed && m_lspFormatter(doc, ed);
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
    if (!Formatter::isAvailable(d->filePath()) && formatWithLsp(d)) {
        emit statusMessage(d->text() == before ? tr("Already formatted (language server)") : tr("Formatted with the language server"));
        return true;
    }
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
bool EditorManager::saveQuietly(Document *doc)
{
    if (!doc || doc->isUntitled() || !doc->isModified())
        return false;
    QString err;
    if (!doc->save(&err)) {
        emit statusMessage(tr("Could not save %1: %2").arg(doc->fileName(), err));
        return false;
    }
    watch(doc->filePath());
    emit documentSaved(doc);
    return true;
}

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
    emit documentSaved(doc);
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
    if (!locate(doc).group)
        return true;
    activate(doc);
    if (!maybeSave(doc))
        return false;
    removeDocument(doc);
    return true;
}

bool EditorManager::closeCurrent()
{
    Document *d = currentDocument();
    return d ? closeDocument(d) : false;
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
    for (Document *d : documents())
        removeDocument(d);
    return true;
}

void EditorManager::closeDocumentsUnder(const QString &path)
{
    const QString prefix = path + QLatin1Char('/');
    for (Document *d : documents())
        if (!d->isUntitled() && (d->filePath() == path || d->filePath().startsWith(prefix)))
            removeDocument(d);
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
    QTabWidget *t = m_active->tabs();
    if (t->count() > 1)
        t->setCurrentIndex((t->currentIndex() + 1) % t->count());
}

void EditorManager::previousTab()
{
    QTabWidget *t = m_active->tabs();
    if (t->count() > 1)
        t->setCurrentIndex((t->currentIndex() + t->count() - 1) % t->count());
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
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::MiddleButton) {
            for (EditorGroup *g : groups()) {
                if (obj != g->tabs()->tabBar())
                    continue;
                const int i = g->tabs()->tabBar()->tabAt(me->position().toPoint());
                if (i >= 0) {
                    closeDocument(entryIn(g, i).doc);
                    return true;
                }
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
        activate(doc);
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
