// MainWindow's JVM debugger: the actions and their shortcuts, the debug panel, breakpoints in the editors, the current
// line while paused, and turning a run command into one whose JVM waits for the debugger (see dev-docs/jvm.md).
#include "MainWindow.h"

#include "app/StartupProfile.h"
#include "debug/DebugPanel.h"
#include "debug/DebugSession.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "gradle/GradleBuildView.h"
#include "gradle/GradleEvents.h"
#include "gradle/GradlePanel.h"
#include "jvm/JvmTools.h"
#include "project/JvmProject.h"
#include "project/ProjectFiles.h"
#include "project/ProjectManager.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStackedWidget>
#include <QStatusBar>

namespace {
const QString kProjectRun = QStringLiteral("@project"); // RunConfigDialog's key of the project-wide command

QString readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(256 * 1024)) : QString();
}
} // namespace

void MainWindow::setupDebugger()
{
    m_debug = new DebugSession(this);
    auto make = [this](const QString &text, const QKeySequence &key, const QString &icon, const QString &tip) {
        auto *a = new QAction(text, this);
        if (!key.isEmpty())
            a->setShortcut(key);
        if (!icon.isEmpty())
            Icons::bind(a, icon);
        a->setToolTip(tip);
        return a;
    };
    m_debugAct = make(tr("Debug"), QKeySequence(Qt::SHIFT | Qt::Key_F9), QStringLiteral(":/new-icons/bug.svg"),
                      tr("Debug this program: the main function here, the Kotlin script, or the project (Shift+F9)"));
    m_resumeAct = make(tr("Resume"), QKeySequence(Qt::Key_F9), QStringLiteral(":/new-icons/play.svg"), tr("Resume (F9)"));
    m_pauseAct = make(tr("Pause"), {}, QStringLiteral(":/new-icons/pause.svg"), tr("Pause the program"));
    m_stepOverAct = make(tr("Step Over"), QKeySequence(Qt::Key_F8), QStringLiteral(":/new-icons/step-over.svg"), tr("Step over (F8)"));
    m_stepIntoAct = make(tr("Step Into"), QKeySequence(Qt::Key_F7), QStringLiteral(":/new-icons/step-into.svg"), tr("Step into (F7)"));
    m_stepOutAct = make(tr("Step Out"), QKeySequence(Qt::SHIFT | Qt::Key_F8), QStringLiteral(":/new-icons/step-out.svg"), tr("Step out (Shift+F8)"));
    m_stopDebugAct = make(tr("Stop Debugging"), QKeySequence(Qt::SHIFT | Qt::Key_F5), QStringLiteral(":/new-icons/square.svg"),
                          tr("Stop the program (Shift+F5)"));
    m_breakpointAct = make(tr("Toggle Breakpoint"), QKeySequence(Qt::CTRL | Qt::Key_F8), {}, tr("Add or remove a breakpoint on the caret line (Ctrl+F8)"));
    m_debugPanelAct = make(tr("Debug Panel"), {}, QStringLiteral(":/new-icons/bug.svg"), tr("Frames, variables and breakpoints"));
    connect(m_debugAct, &QAction::triggered, this, &MainWindow::startDebug);
    connect(m_resumeAct, &QAction::triggered, m_debug, &DebugSession::resume);
    connect(m_pauseAct, &QAction::triggered, m_debug, &DebugSession::pause);
    connect(m_stepOverAct, &QAction::triggered, m_debug, &DebugSession::stepOver);
    connect(m_stepIntoAct, &QAction::triggered, m_debug, &DebugSession::stepInto);
    connect(m_stepOutAct, &QAction::triggered, m_debug, &DebugSession::stepOut);
    connect(m_stopDebugAct, &QAction::triggered, this, [this] { m_debug->stop(true); });
    connect(m_breakpointAct, &QAction::triggered, this, [this] {
        CodeEditor *ed = m_editors->currentEditor();
        if (ed && ed->breakpointsEnabled())
            ed->toggleBreakpoint();
        else
            statusBar()->showMessage(tr("Breakpoints work in Kotlin, Java and Kotlin script files"), 4000);
    });
    connect(m_debugPanelAct, &QAction::triggered, this, [this] {
        if (rightPanelShows(m_debugPanel))
            hideMedia();
        else
            showDebugPanel();
    });

    connect(m_debug, &DebugSession::stateChanged, this, [this] {
        updateDebugActions();
        if (m_debug->state() != DebugSession::State::Suspended)
            clearExecutionLine();
    });
    connect(m_debug, &DebugSession::attached, this, [this](const QString &vm) {
        statusBar()->showMessage(tr("Debugger attached (%1)").arg(vm), 4000);
    });
    connect(m_debug, &DebugSession::finished, this, [this](const QString &reason) {
        m_debugEndReason = reason;
        statusBar()->showMessage(tr("Debug session ended: %1").arg(reason), 6000);
    });
    // A failed build never starts the program: stop waiting for it.
    connect(m_gradle->buildView(), &GradleBuildView::buildFinished, this, [this](bool failed) {
        if (failed && m_debug->state() == DebugSession::State::Connecting) {
            m_debug->stop(true, tr("The build failed: Tasks > Problems and the Gradle panel's Build tab show why."));
        }
    });
    connect(m_debug, &DebugSession::stopped, this, [this](const DebugStop &) {
        ++m_debugStops;
        // Paused: bring QODE forward, the program may have been in another window.
        if (!isActiveWindow())
            activateWindow();
    });
    updateDebugActions();
}

// Created the first time it is shown or a debug session starts.
DebugPanel *MainWindow::debugPanel()
{
    if (m_debugPanel)
        return m_debugPanel;
    StartupProfile::Later timing(QStringLiteral("debug panel"));
    m_debugPanel = new DebugPanel(m_debug, this);
    m_rightStack->addWidget(m_debugPanel);
    m_debugPanel->setActions({m_resumeAct, m_pauseAct, m_stepOverAct, m_stepIntoAct, m_stepOutAct, m_stopDebugAct});
    m_debugPanel->setResolver([this](const QString &pkgPath, const QString &source) { return resolveSource(pkgPath, source); });
    connect(m_debugPanel, &DebugPanel::closeRequested, this, &MainWindow::hideMedia);
    connect(m_debugPanel, &DebugPanel::frameShown, this, [this](const QString &path, int line) {
        clearExecutionLine();
        if (path.isEmpty() || line <= 0)
            return;
        m_editorHiddenByUser = false;
        if (!m_editors->openFileAt(path, line, 1))
            return;
        setEditorColumnVisible(true);
        if (Document *d = m_editors->documentForPath(path))
            if (CodeEditor *ed = m_editors->editorFor(d)) {
                ed->setExecutionLine(line - 1);
                m_execPath = path;
            }
    });
    connect(m_debugPanel, &DebugPanel::locationRequested, this, [this](const QString &path, int line) {
        m_editorHiddenByUser = false;
        if (m_editors->openFileAt(path, line, 1))
            setEditorColumnVisible(true);
    });
    auto setLines = [this](const QString &path, const QList<int> &lines) {
        if (lines.isEmpty())
            m_breakpoints.remove(path);
        else
            m_breakpoints.insert(path, lines);
        if (Document *d = m_editors->documentForPath(path))
            if (CodeEditor *ed = m_editors->editorFor(d))
                ed->setBreakpoints(lines);
    };
    connect(m_debugPanel, &DebugPanel::removeBreakpointRequested, this, [this, setLines](const QString &path, int line) {
        QList<int> lines = m_breakpoints.value(path);
        lines.removeAll(line - 1);
        setLines(path, lines);
        breakpointsEdited(nullptr, nullptr);
    });
    connect(m_debugPanel, &DebugPanel::clearBreakpointsRequested, this, [this, setLines] {
        const QStringList paths = m_breakpoints.keys();
        for (const QString &p : paths)
            setLines(p, {});
        breakpointsEdited(nullptr, nullptr);
    });
    connect(m_debugPanel, &DebugPanel::breakOnUncaughtChanged, this, [this] {
        if (m_debug->isActive())
            statusBar()->showMessage(tr("Applies to the next debug session"), 4000);
    });
    breakpointsEdited(nullptr, nullptr); // its breakpoint list
    return m_debugPanel;
}

void MainWindow::updateDebugActions()
{
    const DebugSession::State s = m_debug->state();
    const bool suspended = s == DebugSession::State::Suspended;
    m_resumeAct->setEnabled(suspended);
    m_stepOverAct->setEnabled(suspended);
    m_stepIntoAct->setEnabled(suspended);
    m_stepOutAct->setEnabled(suspended);
    m_pauseAct->setEnabled(s == DebugSession::State::Running);
    m_stopDebugAct->setEnabled(s != DebugSession::State::Idle);
}

void MainWindow::showDebugPanel()
{
    showInRightPanel(debugPanel());
}

void MainWindow::clearExecutionLine()
{
    if (m_execPath.isEmpty())
        return;
    if (Document *d = m_editors->documentForPath(m_execPath))
        if (CodeEditor *ed = m_editors->editorFor(d))
            ed->setExecutionLine(-1);
    m_execPath.clear();
}

void MainWindow::breakpointsEdited(Document *doc, CodeEditor *editor)
{
    if (doc && editor && !doc->isUntitled()) {
        if (editor->breakpoints().isEmpty())
            m_breakpoints.remove(doc->filePath());
        else
            m_breakpoints.insert(doc->filePath(), editor->breakpoints());
    }
    SettingsManager::instance().setBreakpoints(m_breakpoints);
    QHash<QString, QList<int>> shown;
    for (auto it = m_breakpoints.constBegin(); it != m_breakpoints.constEnd(); ++it) {
        QList<int> oneBased;
        for (int l : it.value())
            oneBased << l + 1;
        shown.insert(it.key(), oneBased);
    }
    if (m_debugPanel)
        m_debugPanel->setBreakpoints(shown);
    if (m_debug->isActive())
        m_debug->setBreakpoints(breakpointSpecs());
}

QList<DebugSourceBreakpoints> MainWindow::breakpointSpecs() const
{
    QList<DebugSourceBreakpoints> out;
    for (auto it = m_breakpoints.constBegin(); it != m_breakpoints.constEnd(); ++it) {
        if (it.value().isEmpty())
            continue;
        Document *d = m_editors->documentForPath(it.key());
        const QString text = d ? d->text() : readFile(it.key());
        DebugSourceBreakpoints s;
        s.path = it.key();
        s.sourceName = QFileInfo(it.key()).fileName();
        s.package = JvmTools::declaredPackage(text);
        for (int l : it.value())
            s.lines << l + 1;
        out << s;
    }
    return out;
}

// A frame's class (package path + SourceFile) -> the file in the editor.
QString MainWindow::resolveSource(const QString &pkgPath, const QString &sourceName) const
{
    if (sourceName.isEmpty())
        return {};
    if (!m_debugFile.isEmpty() && QFileInfo(m_debugFile).fileName() == sourceName)
        return m_debugFile;
    const QString suffix = QLatin1Char('/') + (pkgPath.isEmpty() ? sourceName : pkgPath + QLatin1Char('/') + sourceName);
    QStringList candidates;
    for (Document *d : m_editors->documents())
        if (d->filePath().endsWith(QLatin1Char('/') + sourceName))
            candidates << d->filePath();
    for (const QString &f : m_projectFiles->files())
        if (f.endsWith(QLatin1Char('/') + sourceName) && !candidates.contains(f) && !JarSource::isLibraryPath(f))
            candidates << f;
    for (const QString &c : std::as_const(candidates))
        if (c.endsWith(suffix))
            return c;
    // Kotlin allows folders that do not follow the package: compare the declared package.
    const QString pkg = QString(pkgPath).replace(QLatin1Char('/'), QLatin1Char('.'));
    for (const QString &c : std::as_const(candidates)) {
        Document *d = m_editors->documentForPath(c);
        if (JvmTools::declaredPackage(d ? d->text() : readFile(c)) == pkg)
            return c;
    }
    return {};
}

QString MainWindow::debugInitScript(int port, bool tests) const
{
    const QString dir = QDir::homePath() + QStringLiteral("/.cache/QODE-AGENTIC/debug");
    QDir().mkpath(dir);
    const QString path = dir + (tests ? QStringLiteral("/debug-test.init.gradle") : QStringLiteral("/debug.init.gradle"));
    // Only the program's JVMs (JavaExec: run, qodeRun, bootRun, …) wait for the debugger, not Gradle's own. For tests it
    // is the test JVM, and only one: two forks would both wait on the same port.
    const QByteArray body = (tests ? QStringLiteral("// Written by QODE for a debug session: the test JVM waits for the debugger.\n"
                                                    "allprojects {\n"
                                                    "    tasks.withType(Test).configureEach {\n"
                                                    "        maxParallelForks = 1\n"
                                                    "        forkEvery = 0\n"
                                                    "        jvmArgs('%1')\n"
                                                    "    }\n"
                                                    "}\n")
                                   : QStringLiteral("// Written by QODE for a debug session: the program's JVM waits for the debugger.\n"
                                                    "allprojects {\n"
                                                    "    tasks.withType(JavaExec).configureEach {\n"
                                                    "        jvmArgs('%1')\n"
                                                    "    }\n"
                                                    "}\n"))
                                .arg(DebugSession::agentOption(port))
                                .toUtf8();
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(body);
        out.commit();
    }
    return path;
}

QString MainWindow::debugCommandFor(const QString &runCommand, int port) const
{
    const QString agent = DebugSession::agentOption(port);
    static const QRegularExpression gradle(QStringLiteral(R"((?:^|[\s;&|(])(?:sh\s+)?(?:\./gradlew|gradlew|gradle)(?=\s|$))"));
    if (const QRegularExpressionMatch m = gradle.match(runCommand); m.hasMatch()) {
        QString out = runCommand;
        out.insert(m.capturedEnd(), QStringLiteral(" --init-script ") + JvmTools::shellQuote(debugInitScript(port)));
        return out;
    }
    static const QRegularExpression maven(QStringLiteral(R"(^\s*(?:\./mvnw|sh\s+mvnw|mvn)\b)"));
    if (maven.match(runCommand).hasMatch())
        return QStringLiteral("MAVEN_OPTS=%1 %2").arg(JvmTools::shellQuote(agent), runCommand);
    static const QRegularExpression java(QStringLiteral(R"((^|[\s;&|(])java\s)"));
    if (const QRegularExpressionMatch m = java.match(runCommand); m.hasMatch()) {
        QString out = runCommand;
        out.insert(m.capturedEnd(), agent + QLatin1Char(' '));
        return out;
    }
    if (runCommand.contains(QLatin1String("kotlinc")) || runCommand.contains(QLatin1String("kotlin ")))
        return QStringLiteral("JAVA_OPTS=%1 %2").arg(JvmTools::shellQuote(agent), runCommand);
    return QStringLiteral("JAVA_TOOL_OPTIONS=%1 %2").arg(JvmTools::shellQuote(agent), runCommand);
}

void MainWindow::startDebug()
{
    Document *doc = m_editors->currentDocument();
    const QString path = doc ? doc->filePath() : QString();
    if (doc && JvmTools::isKotlinScript(path)) {
        runKotlinScript(doc, true);
        return;
    }
    if (doc && (path.endsWith(QLatin1String(".kt")) || path.endsWith(QLatin1String(".java")))) {
        const QList<JvmTools::MainEntry> mains = JvmTools::findMains(path, doc->text());
        if (!mains.isEmpty()) {
            // The main at or above the caret, else the first one.
            const int caret = m_editors->currentEditor() ? m_editors->currentEditor()->textCursor().blockNumber() : 0;
            JvmTools::MainEntry pick = mains.first();
            for (const JvmTools::MainEntry &m : mains)
                if (m.line <= caret)
                    pick = m;
            runMain(doc, pick.cls, true);
            return;
        }
    }
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    if (JvmTools::isJvmBuild(root)) {
        QString cmd = SettingsManager::instance().runCommand(kProjectRun);
        QString target = tr("the project");
        if (cmd.isEmpty()) {
            const JvmProject jvm = JvmProject::detect(root);
            cmd = jvm.runCommand();
            if (!jvm.configs().isEmpty())
                target = jvm.configs().first().name;
        }
        if (!cmd.isEmpty()) {
            m_debugFile.clear();
            const int port = DebugSession::freePort();
            launchDebug(debugCommandFor(cmd, port), target, root, port);
            return;
        }
    }
    statusBar()->showMessage(tr("Nothing to debug here: open a Kotlin or Java file with a main function, a Kotlin script, or a Gradle / Maven project"),
                             6000);
}

void MainWindow::launchDebug(const QString &command, const QString &target, const QString &workDir, int port)
{
    if (m_debug->isActive()) {
        const auto answer = QMessageBox::question(this, tr("Debug"), tr("A debug session is running. Stop it and start a new one?"));
        if (answer != QMessageBox::Yes)
            return;
        m_debug->stop(true);
    }
    if (!m_editors->saveAll())
        return;
    debugPanel()->setTarget(target);
    breakpointsEdited(nullptr, nullptr); // the panel's list is current
    m_debug->start(port, breakpointSpecs(), m_debugPanel->breakOnUncaught());
    showDebugPanel();
    runInTerminal(command, workDir);
}
