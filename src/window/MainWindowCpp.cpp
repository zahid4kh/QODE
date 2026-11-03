// MainWindow's C / C++ conveniences: Switch Header/Source (Alt+O), Generate Definition (Alt+Enter on a declaration), the
// folders #include paths are completed from without a language server, and file locations in the terminal output.
#include "MainWindow.h"

#include "cpp/CppTools.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "filesystem/FileManager.h"
#include "lsp/LspManager.h"
#include "project/ProjectFiles.h"
#include "project/ProjectManager.h"
#include "tasks/TasksPanel.h"
#include "settings/SettingsManager.h"
#include "terminal/TerminalPanel.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QTextBlock>

namespace {
// Where Generate Definition writes: the declaring file itself (templates, inline, anything declared in a source file), else
// the matching source file; `create` is set when that file does not exist yet (it is made next to the header).
QString definitionTarget(const QString &path, const CppTools::Declaration &d, const QStringList &projectFiles, bool *create)
{
    *create = false;
    if (d.keepInFile || CppTools::isSource(path))
        return path;
    for (const QString &other : CppTools::counterparts(path, projectFiles))
        if (CppTools::isSource(other))
            return other;
    *create = true;
    const QFileInfo fi(path);
    // A C project (.c files beside the header, no C++ ones) gets a .c file.
    const QDir dir = fi.absoluteDir();
    const bool c = !dir.entryList({QStringLiteral("*.c")}, QDir::Files).isEmpty() &&
                   dir.entryList({QStringLiteral("*.cpp"), QStringLiteral("*.cc"), QStringLiteral("*.cxx")}, QDir::Files).isEmpty();
    return dir.filePath(fi.completeBaseName() + (c ? QStringLiteral(".c") : QStringLiteral(".cpp")));
}
}

void MainWindow::createCppActions()
{
    m_switchHeaderAct = new QAction(tr("Switch Header/Source"), this);
    m_switchHeaderAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_O));
    m_switchHeaderAct->setToolTip(tr("Open the header of this source file, or the source of this header"));
    m_generateDefAct = new QAction(tr("Generate Definition"), this);
    m_generateDefAct->setToolTip(tr("Write the definition of the function declared at the caret into the source file"));
    connect(m_switchHeaderAct, &QAction::triggered, this, &MainWindow::switchHeaderSource);
    connect(m_generateDefAct, &QAction::triggered, this, &MainWindow::generateDefinition);

    // gcc / clang errors (and any "file:line:col") in the terminal open with Ctrl+click.
    connect(m_terminal, &TerminalPanel::openFileRequested, this, [this](const QString &path, int line, int column) {
        m_editorHiddenByUser = false;
        if (m_editors->openFileAt(path, line, qMax(1, column))) {
            setEditorColumnVisible(true);
            m_editors->focusEditor();
        }
    });
    // ... and are listed under Tasks > Problems until that terminal runs its next command.
    connect(m_terminal, &TerminalPanel::problemsFound, m_tasks, &TasksPanel::addProblems);
    connect(m_terminal, &TerminalPanel::problemsCleared, m_tasks, &TasksPanel::clearProblems);
}

void MainWindow::wireCpp(Document *doc, CodeEditor *ed)
{
    ed->setContextActions({m_switchHeaderAct, m_generateDefAct, m_copyRefAct, m_copyImageAct, m_localHistoryAct});
    // #include completion without clangd: the file's folder ("..." only), the project's include paths, the qmake project's
    // -I folders and the project root, then the compiler's own folders (<...> only).
    ed->setIncludeFolders([this, doc](QChar opener) {
        QStringList folders;
        if (opener == QLatin1Char('"') && !doc->filePath().isEmpty())
            folders << QFileInfo(doc->filePath()).absolutePath();
        folders += m_lsp->includePaths();
        for (const QString &flag : m_lsp->detectedFlags())
            if (flag.startsWith(QLatin1String("-I")) && flag.size() > 2)
                folders << flag.mid(2);
        if (m_projects->hasProject())
            folders << m_projects->root();
        if (opener == QLatin1Char('<'))
            folders += CppTools::systemIncludeDirs();
        return folders;
    });
}

void MainWindow::switchHeaderSource()
{
    Document *doc = m_editors->currentDocument();
    const QString path = doc ? doc->filePath() : QString();
    if (!CppTools::isCpp(path)) {
        statusBar()->showMessage(tr("Switch Header/Source works in C and C++ files"), 4000);
        return;
    }
    auto open = [this](const QString &other) {
        m_editorHiddenByUser = false;
        if (m_editors->openFile(other))
            setEditorColumnVisible(true);
    };
    m_projectFiles->ensureFresh();
    const QStringList found = CppTools::counterparts(path, m_projectFiles->files());
    if (!found.isEmpty()) {
        open(found.first());
        return;
    }
    // Names that differ: clangd's index may still know the pair.
    const bool header = CppTools::isHeader(path);
    const QString name = QFileInfo(path).fileName();
    m_lsp->switchSourceHeader(doc, [this, open, header, name](const QString &other) {
        if (!other.isEmpty() && QFileInfo::exists(other))
            open(other);
        else
            statusBar()->showMessage(header ? tr("No source file found for %1").arg(name) : tr("No header found for %1").arg(name), 4000);
    });
}

QString MainWindow::definitionActionTitle(Document *doc, CodeEditor *editor) const
{
    if (!doc || !editor || !CppTools::isCpp(doc->filePath()) || editor->textCursor().hasSelection())
        return {};
    const CppTools::Declaration d = CppTools::declarationAt(editor->toPlainText(), editor->textCursor().position());
    if (!d.ok)
        return {};
    bool create = false;
    const QString target = definitionTarget(doc->filePath(), d, m_projectFiles->files(), &create);
    if (target == doc->filePath())
        return tr("Generate Definition");
    const QString name = QFileInfo(target).fileName();
    return create ? tr("Generate Definition in %1 (new file)").arg(name) : tr("Generate Definition in %1").arg(name);
}

void MainWindow::generateDefinition()
{
    CodeEditor *ed = m_editors->currentEditor();
    Document *doc = m_editors->currentDocument();
    if (!ed || !doc || !CppTools::isCpp(doc->filePath())) {
        statusBar()->showMessage(tr("Generate Definition works in C and C++ files"), 4000);
        return;
    }
    const CppTools::Declaration d = CppTools::declarationAt(ed->toPlainText(), ed->textCursor().position());
    if (!d.ok) {
        statusBar()->showMessage(d.error, 5000);
        return;
    }
    bool create = false;
    const QString target = definitionTarget(doc->filePath(), d, m_projectFiles->files(), &create);
    if (create) {
        QString error;
        const QString starter = CppTools::newFileText(target, m_projects->hasProject() ? m_projects->root() : QString());
        if (!FileManager::writeFile(target, starter.toUtf8(), &error)) {
            QMessageBox::warning(this, tr("Generate Definition"), tr("Could not create %1:\n%2").arg(FileManager::displayPath(target), error));
            return;
        }
        m_projectFiles->invalidate();
    }
    const bool same = target == doc->filePath();
    if (!same) {
        m_editorHiddenByUser = false;
        if (!m_editors->openFile(target))
            return;
        setEditorColumnVisible(true);
    }
    Document *targetDoc = m_editors->documentForPath(target);
    CodeEditor *te = targetDoc ? m_editors->editorFor(targetDoc) : nullptr;
    if (!te || te->isReadOnly())
        return;
    auto &cfg = SettingsManager::instance();
    const QString indent = cfg.useSpaces() ? QString(cfg.tabSize(), QLatin1Char(' ')) : QStringLiteral("\t");
    const CppTools::Insertion ins = CppTools::insertionFor(d, te->toPlainText(), same, indent);
    QTextCursor c(te->document());
    const QString name = QFileInfo(target).fileName();
    if (ins.exists) {
        c.setPosition(ins.position);
        te->setTextCursor(c);
        te->centerCursor();
        te->setFocus();
        statusBar()->showMessage(tr("%1 is already defined in %2").arg(d.name, name), 4000);
        return;
    }
    c.beginEditBlock();
    c.setPosition(ins.position);
    c.insertText(ins.text);
    c.endEditBlock();
    c.setPosition(ins.position + ins.caret);
    te->setTextCursor(c);
    te->centerCursor();
    te->setFocus();
    statusBar()->showMessage(tr("Definition of %1 added to %2").arg(d.name, name), 4000);
}
