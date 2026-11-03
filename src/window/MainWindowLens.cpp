// MainWindow's side of code lenses: the language server's lenses merged with QODE's own "▶ Run | Debug" over Java / Kotlin
// tests and main functions, resolved as they come on screen, and what a click on one does.
#include "MainWindow.h"

#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "jvm/JvmTools.h"
#include "lsp/JarSource.h"
#include "lsp/LspManager.h"
#include "project/ProjectManager.h"
#include "settings/SettingsManager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>

namespace {
// QODE's own lens commands; the one argument is an object.
const QString kRunTest = QStringLiteral("qode.runTest"); // {class, method ("" = the whole class), debug}
const QString kRunMain = QStringLiteral("qode.runMain"); // {class, debug}
// kotlin-lsp's "Run" / "Debug" over a main function: {mainClass, uri, noDebug}. QODE lists its own for the same line.
const QString kKotlinRunMain = QStringLiteral("intellij.jvm.runMain");

LspCodeLens localLens(int line, const QString &title, const QString &command, const QJsonObject &argument)
{
    LspCodeLens lens;
    lens.line = line;
    lens.title = title;
    lens.command = command;
    lens.arguments = QJsonArray{argument};
    return lens;
}

QString wordAt(CodeEditor *ed, int line, int column)
{
    const QTextBlock block = ed->document()->findBlockByNumber(line);
    if (!block.isValid())
        return {};
    QTextCursor c(block);
    c.setPosition(block.position() + qMin(column, qMax(0, block.length() - 1)));
    c.select(QTextCursor::WordUnderCursor);
    return c.selectedText();
}
} // namespace

void MainWindow::wireCodeLenses(Document *doc, CodeEditor *ed)
{
    connect(ed, &CodeEditor::codeLensResolveRequested, this, [this, ed](int generation, int index) {
        QPointer<CodeEditor> guard(ed);
        m_lsp->resolveCodeLens(ed->codeLens(index), [guard, generation, index](const LspCodeLens &lens) {
            if (!guard)
                return;
            // jdtls counts implementations on every class and method: "0 implementations" says nothing.
            LspCodeLens shown = lens;
            if (shown.title.startsWith(QLatin1String("0 implementation")))
                shown.title.clear();
            guard->resolveCodeLens(generation, index, shown);
        });
    });
    connect(ed, &CodeEditor::codeLensClicked, this, [this, doc, ed](int index) { runCodeLens(doc, ed, ed->codeLens(index)); });
}

// Called with the other file-wide requests (450 ms after an edit, and when a server starts or asks for a refresh).
void MainWindow::requestCodeLenses(Document *doc, CodeEditor *ed)
{
    if (!SettingsManager::instance().codeLenses() || doc->isLarge()) {
        ed->clearCodeLenses();
        return;
    }
    const QVector<LspCodeLens> local = localCodeLenses(doc);
    if (!m_lsp->supportsCodeLens(doc)) {
        ed->setCodeLenses(local);
        return;
    }
    const int revision = ed->document()->revision();
    QPointer<CodeEditor> guard(ed);
    m_lsp->codeLenses(doc, [guard, revision, local](const QVector<LspCodeLens> &lenses, bool ok) {
        if (!guard)
            return;
        if (!ok) { // superseded by a newer request, or the server failed: QODE's own lenses at least
            if (guard->codeLensCount() == 0)
                guard->setCodeLenses(local);
            return;
        }
        if (guard->document()->revision() != revision)
            return; // typed meanwhile: the edit asks again
        QSet<int> runLines;
        for (const LspCodeLens &l : local)
            runLines.insert(l.line);
        QVector<LspCodeLens> all = local;
        for (const LspCodeLens &l : lenses)
            if (l.command != kKotlinRunMain || !runLines.contains(l.line))
                all << l;
        guard->setCodeLenses(all);
    });
}

// "▶ Run | Debug" over JUnit / kotlin.test tests (with a Gradle / Maven build to run them) and over main functions. They
// need no server, so they show at once and in files the server has not analysed yet.
QVector<LspCodeLens> MainWindow::localCodeLenses(Document *doc) const
{
    QVector<LspCodeLens> out;
    const QString path = doc->filePath();
    if ((!path.endsWith(QLatin1String(".kt")) && !path.endsWith(QLatin1String(".java"))) || JarSource::isLibraryPath(path))
        return out;
    const QString text = doc->text();
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    if (text.contains(QLatin1String("Test")) && JvmTools::isJvmBuild(root))
        for (const JvmTools::TestEntry &t : JvmTools::findTests(path, text)) {
            const QJsonObject arg{{QStringLiteral("class"), t.cls}, {QStringLiteral("method"), t.method}};
            QJsonObject debug = arg;
            debug.insert(QStringLiteral("debug"), true);
            out << localLens(t.line, t.method.isEmpty() ? tr("$(play) Run Tests") : tr("$(play) Run"), kRunTest, arg)
                << localLens(t.line, tr("Debug"), kRunTest, debug);
        }
    if (text.contains(QLatin1String("main")))
        for (const JvmTools::MainEntry &m : JvmTools::findMains(path, text)) {
            out << localLens(m.line, tr("$(play) Run"), kRunMain, QJsonObject{{QStringLiteral("class"), m.cls}})
                << localLens(m.line, tr("Debug"), kRunMain, QJsonObject{{QStringLiteral("class"), m.cls}, {QStringLiteral("debug"), true}});
        }
    return out;
}

void MainWindow::runCodeLens(Document *doc, CodeEditor *ed, const LspCodeLens &lens)
{
    const QString cmd = lens.command;
    const QJsonObject arg = lens.arguments.at(0).toObject();
    if (cmd == kRunTest) {
        runTest(doc, arg.value(QStringLiteral("class")).toString(), arg.value(QStringLiteral("method")).toString(),
                arg.value(QStringLiteral("debug")).toBool());
        return;
    }
    if (cmd == kRunMain) {
        runMain(doc, arg.value(QStringLiteral("class")).toString(), arg.value(QStringLiteral("debug")).toBool());
        return;
    }
    if (cmd == kKotlinRunMain) {
        runMain(doc, arg.value(QStringLiteral("mainClass")).toString(), !arg.value(QStringLiteral("noDebug")).toBool());
        return;
    }
    // [uri, position, locations]: jdtls' java.show.references / java.show.implementations, VS Code's
    // editor.action.showReferences and the like. These are client commands: the places travel with the lens.
    if (lens.arguments.size() >= 3 && lens.arguments.at(2).isArray()) {
        const bool implementations = cmd.contains(QLatin1String("mplementation"));
        showLocations(wordAt(ed, lens.line, lens.column), LspManager::locationsOf(lens.arguments.at(2)), implementations);
        return;
    }
    const QString label = CodeEditor::codeLensLabel(lens.title);
    if (m_lsp->hasCommand(lens.server, cmd)) {
        m_lsp->executeCommand(lens.server, cmd, lens.arguments, [this, label](bool ok, const QString &error) {
            if (!ok)
                statusBar()->showMessage(tr("“%1” failed: %2").arg(label, error), 6000);
        });
        return;
    }
    statusBar()->showMessage(tr("QODE cannot run “%1” (%2)").arg(label, cmd), 5000);
}
