// MainWindow's editing extras: multiple cursors (Edit menu), Ctrl+click links (paths, URLs, the language server's document
// links) and the local history window.
#include "MainWindow.h"

#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/ProjectExplorer.h"
#include "filesystem/FileManager.h"
#include "history/LocalHistory.h"
#include "history/LocalHistoryDialog.h"
#include "lsp/LspManager.h"
#include "media/CodeImageDialog.h"
#include "project/ProjectManager.h"
#include "settings/Icons.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QPointer>
#include <QTimer>

void MainWindow::createEditingActions()
{
    // The keys are the editor's own (CodeEditor::multiCursorShortcut): as window shortcuts they would also fire in the
    // terminal, so the menu only shows them through the tab column.
    auto make = [this](const QString &text, const QString &keys, void (CodeEditor::*slot)()) {
        auto *a = new QAction(text + QLatin1Char('\t') + keys, this);
        connect(a, &QAction::triggered, this, [this, slot] {
            if (CodeEditor *e = m_editors->currentEditor()) {
                (e->*slot)();
                e->setFocus();
            }
        });
        m_cursorActs << a;
        return a;
    };
    make(tr("Add Next Occurrence"), QStringLiteral("Alt+J"), &CodeEditor::addNextOccurrence);
    make(tr("Remove Last Occurrence"), QStringLiteral("Alt+Shift+J"), &CodeEditor::removeLastOccurrence);
    make(tr("Select All Occurrences"), QStringLiteral("Ctrl+Shift+L"), &CodeEditor::selectAllOccurrences);
    auto *above = new QAction(tr("Add Caret Above") + QStringLiteral("\tCtrl+Alt+Up"), this);
    auto *below = new QAction(tr("Add Caret Below") + QStringLiteral("\tCtrl+Alt+Down"), this);
    connect(above, &QAction::triggered, this, [this] { if (CodeEditor *e = m_editors->currentEditor()) e->addCursorVertically(false); });
    connect(below, &QAction::triggered, this, [this] { if (CodeEditor *e = m_editors->currentEditor()) e->addCursorVertically(true); });
    m_cursorActs << above << below;
    make(tr("Add Carets to Line Ends"), QStringLiteral("Alt+Shift+I"), &CodeEditor::addCursorsToLineEnds);
    make(tr("Single Caret"), QStringLiteral("Escape"), &CodeEditor::clearExtraCursors);

    m_localHistoryAct = new QAction(tr("Local History…"), this);
    m_localHistoryAct->setToolTip(tr("Earlier versions of this file, kept on every save and before QODE or a refactoring changes it"));
    connect(m_localHistoryAct, &QAction::triggered, this, [this] {
        if (Document *d = m_editors->currentDocument(); d && !d->isUntitled())
            showLocalHistory(d->filePath());
        else if (m_projects->hasProject())
            showLocalHistory(m_projects->project().root);
    });
    connect(m_explorer, &ProjectExplorer::localHistoryRequested, this, &MainWindow::showLocalHistory);

    m_copyImageAct = new QAction(tr("Copy as Image…"), this);
    m_copyImageAct->setToolTip(tr("A picture of the selected code (or the whole file) to share: background, window buttons, QODE mark"));
    Icons::bind(m_copyImageAct, QStringLiteral(":/new-icons/image.svg"));
    connect(m_copyImageAct, &QAction::triggered, this, [this] {
        CodeEditor *e = m_editors->currentEditor();
        Document *d = m_editors->currentDocument();
        if (!e || !d)
            return;
        CodeImageDialog::show(CodeImage::fromDocument(e->document(), e->textCursor(), d->language(), d->fileName()), this);
    });
    // Versions older than 30 days go once QODE has settled.
    QTimer::singleShot(30000, this, [] { LocalHistory::prune(); });
}

void MainWindow::wireEditing(Document *doc, CodeEditor *ed)
{
    QPointer<Document> docGuard(doc);
    ed->setPathResolver([this, docGuard](const QString &raw) -> QString {
        QString p = raw;
        if (p.startsWith(QLatin1String("~/")))
            p = QDir::homePath() + p.mid(1);
        QStringList candidates;
        if (QDir::isAbsolutePath(p)) {
            candidates << p;
        } else {
            if (docGuard && !docGuard->isUntitled())
                candidates << QFileInfo(docGuard->filePath()).absoluteDir().filePath(p);
            if (m_projects->hasProject())
                candidates << QDir(m_projects->project().root).filePath(p);
        }
        for (const QString &c : std::as_const(candidates))
            if (QFileInfo(c).isFile())
                return QDir::cleanPath(c);
        return {};
    });
    connect(ed, &CodeEditor::fileLinkRequested, this, [this](const QString &path, int line, int column) {
        if (line >= 0)
            m_editors->openFileAt(path, line + 1, qMax(0, column) + 1);
        else
            m_editors->openFile(path);
    });
    connect(ed, &CodeEditor::colorsRequested, this, [this, doc, ed] { requestLinks(doc, ed); }); // the text settled
}

void MainWindow::requestLinks(Document *doc, CodeEditor *editor)
{
    if (!m_lsp->isServed(doc))
        return;
    QPointer<CodeEditor> guard(editor);
    m_lsp->documentLinks(doc, [guard](const QVector<LspDocumentLink> &links) {
        if (!guard)
            return;
        QVector<CodeEditor::DocumentLink> out;
        out.reserve(links.size());
        for (const LspDocumentLink &l : links)
            out.append({l.startLine, l.startColumn, l.endLine, l.endColumn, l.target});
        guard->setDocumentLinks(out);
    });
}

void MainWindow::showLocalHistory(const QString &path)
{
    LocalHistoryDialog::Hooks hooks;
    hooks.currentText = [this](const QString &file, bool *exists) -> QString {
        if (Document *d = m_editors->documentForPath(file)) {
            *exists = true;
            return d->text();
        }
        QFile f(file);
        *exists = QFileInfo(file).isFile();
        if (!*exists || !f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    };
    hooks.restore = [this](const QString &file, const QString &text) {
        Document *open = m_editors->documentForPath(file);
        // What gets replaced stays in the history: the unsaved text, else the file's bytes (as the next save compares them).
        if (open && open->isModified())
            LocalHistory::record(file, open->text().toUtf8(), LocalHistory::Kind::BeforeRestore);
        else
            LocalHistory::recordDisk(file, LocalHistory::Kind::BeforeRestore);
        if (Document *d = open) {
            CodeEditor *ed = m_editors->editorFor(d);
            if (!ed || ed->isReadOnly())
                return false;
            LspTextEdit all; // the whole text, one undo step in the editor
            all.endLine = qMax(0, ed->document()->blockCount() - 1);
            all.endColumn = 1 << 28;
            all.text = text;
            if (!ed->applyTextEdits({all}))
                return false;
            m_editors->saveQuietly(d); // no format on save: the version comes back as it was
            return true;
        }
        QDir().mkpath(QFileInfo(file).absolutePath());
        QString error;
        const QByteArray data = text.toUtf8();
        LocalHistory::recordWrite(file, data);
        if (!FileManager::writeFile(file, data, &error))
            return false;
        m_editors->openFile(file);
        return true;
    };
    auto *dlg = new LocalHistoryDialog(path, hooks, this);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}
