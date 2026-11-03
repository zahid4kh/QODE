// MainWindow's Java / Kotlin conveniences: Kotlin scripts (F5 runs them, kotlinc checks them), run markers on main
// functions, the package-vs-folder fix, the dependency manager, the project JDK, the version buttons in the status bar
// and the task timings of the Gradle builds QODE starts.
#include "MainWindow.h"

#include "debug/DebugSession.h"
#include "dialogs/RunConfigDialog.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "gradle/GradleBuildView.h"
#include "gradle/GradleEvents.h"
#include "gradle/GradlePanel.h"
#include "maven/MavenPanel.h"
#include "jvm/DependencyDialog.h"
#include "jvm/DependencyEdit.h"
#include "jvm/JdkDialog.h"
#include "jvm/JdkScanner.h"
#include "jvm/JvmTools.h"
#include "jvm/ScriptCheck.h"
#include "project/JvmProject.h"
#include "project/ProjectFiles.h"
#include "project/ProjectManager.h"
#include "refactor/MoveController.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "terminal/TerminalPanel.h"

#include <QAction>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>

namespace {

bool isJvmSource(const QString &path)
{
    return path.endsWith(QLatin1String(".kt")) || path.endsWith(QLatin1String(".java"));
}

// Replaces the editor's text with `text` as one undo step, touching only the range that differs.
void replaceText(CodeEditor *ed, const QString &text)
{
    const QString old = ed->toPlainText();
    int prefix = 0;
    while (prefix < old.size() && prefix < text.size() && old.at(prefix) == text.at(prefix))
        ++prefix;
    int suffix = 0;
    while (suffix < old.size() - prefix && suffix < text.size() - prefix && old.at(old.size() - 1 - suffix) == text.at(text.size() - 1 - suffix))
        ++suffix;
    QTextCursor c(ed->document());
    c.beginEditBlock();
    c.setPosition(prefix);
    c.setPosition(int(old.size()) - suffix, QTextCursor::KeepAnchor);
    c.insertText(text.mid(prefix, text.size() - prefix - suffix));
    c.endEditBlock();
}

struct PackageState {
    bool mismatch = false;
    QString declared, folder, sourceRoot;
};

PackageState packageState(const QString &path, const QString &text)
{
    PackageState s;
    if (!isJvmSource(path))
        return s;
    s.folder = JvmTools::folderPackage(path, &s.sourceRoot);
    if (s.folder.isNull())
        return s;
    s.declared = JvmTools::declaredPackage(text);
    s.mismatch = !JvmTools::packageMatches(path, s.declared, s.folder);
    return s;
}

} // namespace

void MainWindow::createJvmActions()
{
    m_dependenciesAct = new QAction(tr("Dependencies…"), this);
    m_dependenciesAct->setToolTip(tr("Search Maven Central and add a library to the build (or @file:DependsOn to a script)"));
    connect(m_dependenciesAct, &QAction::triggered, this, [this] { showDependencies(false); });
    m_depUpdatesAct = new QAction(tr("Check Dependency Updates…"), this);
    m_depUpdatesAct->setToolTip(tr("List the declared libraries that have a newer version on Maven Central"));
    connect(m_depUpdatesAct, &QAction::triggered, this, [this] { showDependencies(true); });
    m_jdkAct = new QAction(tr("Project JDK…"), this);
    m_jdkAct->setToolTip(tr("The installed JDKs, and which one this project's terminals and runs use"));
    connect(m_jdkAct, &QAction::triggered, this, &MainWindow::showJdkManager);
}

void MainWindow::setupJvm()
{
    m_scriptCheck = new ScriptCheck(this);
    connect(m_scriptCheck, &ScriptCheck::checked, this, [this](const QString &path, int errors, int warnings) {
        applyDiagnostics(path);
        if (m_editors->currentDocument() && m_editors->currentDocument()->filePath() == path) {
            const QString name = QFileInfo(path).fileName();
            statusBar()->showMessage(errors + warnings == 0 ? tr("✓ %1 compiles").arg(name)
                                                            : tr("%1: %2 error(s), %3 warning(s) (kotlinc)").arg(name).arg(errors).arg(warnings),
                                     5000);
        }
    });
    connect(m_scriptCheck, &ScriptCheck::started, this, [this](const QString &path) {
        if (m_editors->currentDocument() && m_editors->currentDocument()->filePath() == path)
            statusBar()->showMessage(tr("Checking %1 with kotlinc…").arg(QFileInfo(path).fileName()), 3000);
    });
    connect(m_editors, &EditorManager::documentSaved, this, [this](Document *doc) {
        const QString path = doc->filePath();
        if (JvmTools::isKotlinScript(path) && !m_lsp->isServed(doc))
            m_scriptCheck->check(path);
        if (isJvmSource(path))
            checkPackage(doc);
        if (LspManager::isJvmBuildFile(path) || path.endsWith(QLatin1String("libs.versions.toml"))
            || path.endsWith(QLatin1String("gradle-wrapper.properties")))
            updateJvmStatus();
    });

    // Gradle: the Build tab follows the builds QODE starts; compiler errors of the terminal go with them.
    connect(m_terminal, &TerminalPanel::problemsFound, this, [this](QObject *, const QList<BuildProblem> &problems) {
        m_gradle->buildView()->addProblems(problems);
    });
    connect(m_gradle->buildView(), &GradleBuildView::locationRequested, this, [this](const QString &path, int line, int column) {
        m_editorHiddenByUser = false;
        if (m_editors->openFileAt(path, line, column))
            setEditorColumnVisible(true);
    });
    connect(m_gradle->buildView(), &GradleBuildView::buildFinished, this, [this](bool failed) {
        if (failed && !(m_rightStack->parentWidget()->isVisible() && m_rightStack->currentWidget() == m_gradle))
            statusBar()->showMessage(tr("Gradle build failed: the Gradle panel's Build tab shows where"), 8000);
    });
    connect(m_projects, &ProjectManager::projectOpened, this, [this] { updateJvmStatus(); });
    connect(m_projects, &ProjectManager::projectClosed, this, [this] { updateJvmStatus(); });

    // Status bar: Kotlin / Java / Gradle versions of the project.
    m_jvmStatus = new QWidget(this);
    auto *l = new QHBoxLayout(m_jvmStatus);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    auto button = [this, l] {
        auto *b = new QToolButton(m_jvmStatus);
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(12, 12));
        b->setStyleSheet(QStringLiteral("QToolButton { padding: 1px 5px; }"));
        l->addWidget(b);
        return b;
    };
    m_kotlinBtn = button();
    m_javaBtn = button();
    m_buildToolBtn = button();
    auto openFound = [this](const QString &file, int line) {
        if (file.isEmpty())
            return;
        m_editorHiddenByUser = false;
        if (m_editors->openFileAt(file, line, 1))
            setEditorColumnVisible(true);
    };
    connect(m_kotlinBtn, &QToolButton::clicked, this, [this, openFound] {
        const JvmTools::Versions v = JvmTools::detectVersions(m_projects->root());
        openFound(v.kotlin.file, v.kotlin.line);
    });
    connect(m_javaBtn, &QToolButton::clicked, this, &MainWindow::showJdkManager);
    connect(m_buildToolBtn, &QToolButton::clicked, this, [this, openFound] {
        const JvmTools::Versions v = JvmTools::detectVersions(m_projects->root());
        openFound(v.build.file, v.build.line);
    });
    statusBar()->insertPermanentWidget(0, m_jvmStatus);
    updateJvmStatus();
}

void MainWindow::wireJvm(Document *doc, CodeEditor *ed)
{
    const QString path = doc->filePath();
    const bool jvm = (isJvmSource(path) || JvmTools::isKotlinScript(path)) && !JarSource::isLibraryPath(path);
    ed->setBreakpointsEnabled(jvm);
    if (jvm)
        ed->setBreakpoints(m_breakpoints.value(path));
    connect(ed, &CodeEditor::breakpointsChanged, this, [this, doc, ed] { breakpointsEdited(doc, ed); });
    connect(ed, &CodeEditor::runRequested, this, [this, doc](int line) { showMainMenu(doc, line); });
    if (JvmTools::isKotlinScript(doc->filePath()))
        QTimer::singleShot(800, this, [this, doc = QPointer<Document>(doc)] {
            if (doc && !m_lsp->isServed(doc))
                m_scriptCheck->check(doc->filePath());
        });
    connect(doc, &QObject::destroyed, this, [this, path = doc->filePath()] {
        if (!m_dying && m_scriptCheck)
            m_scriptCheck->forget(path);
    });
}

void MainWindow::applyDiagnostics(const QString &path)
{
    Document *d = m_editors->documentForPath(path);
    CodeEditor *ed = d ? m_editors->editorFor(d) : nullptr;
    if (!ed)
        return;
    // A script the Kotlin server analyses too would show each error twice: the server's list wins when it has one.
    QVector<LspDiagnostic> list = m_lsp->diagnostics(path);
    if (list.isEmpty() && m_scriptCheck)
        list = m_scriptCheck->diagnostics(path);
    ed->setDiagnostics(list);
}

// --- main functions -----------------------------------------------------------------------------------------------------

void MainWindow::updateMainMarkers(Document *doc)
{
    CodeEditor *ed = m_editors->editorFor(doc);
    if (!ed)
        return;
    const QString path = doc->filePath();
    QList<int> lines;
    if (isJvmSource(path) && !JarSource::isLibraryPath(path) && doc->text().contains(QLatin1String("main")))
        for (const JvmTools::MainEntry &m : JvmTools::findMains(path, doc->text()))
            lines << m.line;
    ed->setRunMarkers(lines, tr("Run or debug this program"));
}

void MainWindow::showMainMenu(Document *doc, int line)
{
    if (!isJvmSource(doc->filePath()))
        return;
    QString cls;
    for (const JvmTools::MainEntry &m : JvmTools::findMains(doc->filePath(), doc->text()))
        if (m.line == line)
            cls = m.cls;
    if (cls.isEmpty())
        return;
    const QString simple = cls.section(QLatin1Char('.'), -1);
    QMenu menu(this);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    menu.addAction(Icons::tinted(QStringLiteral(":/new-icons/play.svg"), t.success), tr("Run '%1'").arg(simple), this,
                   [this, doc, cls] { runMain(doc, cls, false); });
    menu.addAction(Icons::tinted(QStringLiteral(":/new-icons/bug.svg"), t.warning), tr("Debug '%1'").arg(simple), this,
                   [this, doc, cls] { runMain(doc, cls, true); });
    menu.exec(QCursor::pos());
}

void MainWindow::runMain(Document *doc, const QString &cls, bool debug)
{
    if (!m_editors->saveAll())
        return;
    const QString path = doc->filePath();
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    const int port = debug ? DebugSession::freePort() : 0;
    QString cmd = JvmProject::mainCommand(root, path, cls, debug ? QStringList{debugInitScript(port)} : QStringList{});
    QString workDir;
    if (cmd.isEmpty()) {
        // No Gradle / Maven build around it: the file alone.
        // The classes go to the cache, never next to the file; javac -g keeps the local variables for the debugger.
        const QString agent = debug ? DebugSession::agentOption(port) + QLatin1Char(' ') : QString();
        const QString file = JvmTools::shellQuote(path);
        const QString dir = QDir::homePath() + QStringLiteral("/.cache/QODE-AGENTIC/run");
        QDir().mkpath(dir);
        const QString mainCls = cls.contains(QLatin1Char('$')) ? JvmTools::shellQuote(cls) : cls;
        if (path.endsWith(QLatin1String(".java"))) {
            const QString out = JvmTools::shellQuote(dir + QStringLiteral("/java-") + QFileInfo(path).completeBaseName());
            cmd = QStringLiteral("javac -g -d %1 %2 && java %3-cp %1 %4").arg(out, file, agent, mainCls);
        } else {
            const QString jar = JvmTools::shellQuote(dir + QLatin1Char('/') + QFileInfo(path).completeBaseName() + QStringLiteral(".jar"));
            cmd = QStringLiteral("kotlinc %1 -include-runtime -d %2 && java %3-cp %2 %4").arg(file, jar, agent, mainCls);
        }
        workDir = QFileInfo(path).absolutePath();
        m_debugFile = path;
    } else {
        workDir = root;
        m_debugFile.clear();
    }
    if (debug) {
        launchDebug(cmd, cls.section(QLatin1Char('.'), -1), workDir, port);
        return;
    }
    runInTerminal(cmd, workDir);
}

void MainWindow::runTest(Document *doc, const QString &testClass, const QString &method, bool debug)
{
    if (!m_editors->saveAll())
        return;
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    const int port = debug ? DebugSession::freePort() : 0;
    const QString cmd = JvmProject::testCommand(root, doc->filePath(), testClass, method,
                                                debug ? QStringList{debugInitScript(port, true)} : QStringList{},
                                                debug ? DebugSession::agentOption(port) : QString());
    if (cmd.isEmpty()) {
        statusBar()->showMessage(tr("Tests run through the project's Gradle or Maven build — open the project folder that holds it"), 6000);
        return;
    }
    if (debug) {
        m_debugFile.clear();
        const QString simple = testClass.section(QLatin1Char('.'), -1);
        launchDebug(cmd, method.isEmpty() ? simple : simple + QLatin1Char('.') + method, root, port);
        return;
    }
    m_lastTestClass = testClass;
    m_lastTestFile = doc->filePath();
    runInTerminal(cmd, root);
}

void MainWindow::runInTerminal(const QString &command, const QString &workDir)
{
    QString cmd = command;
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    if (GradleEvents::runsGradle(cmd) && !root.isEmpty()) {
        cmd = GradleEvents::inject(cmd, root);
        m_gradle->buildView()->follow();
    }
    if (!workDir.isEmpty() && workDir != m_terminal->workingDirectory()) {
        QString quoted = workDir;
        quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        cmd = QStringLiteral("(cd '%1' && %2)").arg(quoted, cmd);
    }
    showTerminal();
    m_terminal->runCommand(cmd);
    m_terminal->focusTerminal();
}

// --- Kotlin scripts -----------------------------------------------------------------------------------------------------

bool MainWindow::runKotlinScript(Document *doc, bool debug)
{
    if (!doc || !JvmTools::isKotlinScript(doc->filePath()))
        return false;
    if ((doc->isModified() || doc->isUntitled()) && !m_editors->saveCurrent())
        return true;
    const QString path = doc->filePath();
    bool chmodOffer = false;
    QString command = JvmTools::scriptRunCommand(path, doc->text(), &chmodOffer);
    if (chmodOffer && !debug && !m_chmodAsked.contains(path)) {
        m_chmodAsked.insert(path);
        QMessageBox box(QMessageBox::Question, tr("Run Kotlin Script"),
                        tr("%1 starts with a #!…kotlin line but is not executable. Make it executable, so it runs as a program "
                           "(./%1) here and in any shell?")
                            .arg(QFileInfo(path).fileName()),
                        QMessageBox::NoButton, this);
        QPushButton *exec = box.addButton(tr("Make Executable"), QMessageBox::AcceptRole);
        box.addButton(tr("Run with kotlinc"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == exec) {
            QFile f(path);
            if (f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther))
                command = QStringLiteral("{file}");
        }
    }
    if (JvmTools::isMainKts(path) && command.startsWith(QLatin1String("kotlinc -script")) && !JvmTools::mainKtsJar().isEmpty())
        command = QStringLiteral("kotlinc -script {file}"); // kotlinc finds kotlin-main-kts.jar itself for *.main.kts
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    const QString dir = QFileInfo(path).absolutePath();
    if (debug) {
        const int port = DebugSession::freePort();
        // kotlinc runs the script in its own JVM: JAVA_OPTS reaches it (a shebang run goes through kotlinc too).
        const QString cmd = QStringLiteral("JAVA_OPTS=%1 kotlinc -script %2").arg(JvmTools::shellQuote(DebugSession::agentOption(port)), JvmTools::shellQuote(path));
        m_debugFile = path;
        launchDebug(cmd, QFileInfo(path).fileName(), dir, port);
        return true;
    }
    runInTerminal(RunConfigDialog::expand(command, path, root), dir);
    return true;
}

// --- packages -----------------------------------------------------------------------------------------------------------

void MainWindow::checkPackage(Document *doc)
{
    const PackageState s = packageState(doc->filePath(), doc->text());
    if (!s.mismatch)
        return;
    statusBar()->showMessage(tr("%1: package %2 does not match its folder (%3). Alt+Enter offers to fix it.")
                                 .arg(QFileInfo(doc->filePath()).fileName(), s.declared.isEmpty() ? tr("(default)") : s.declared,
                                      s.folder.isEmpty() ? tr("the source root") : s.folder),
                             8000);
}

QList<QPair<QString, std::function<void()>>> MainWindow::localCodeActions(Document *doc, CodeEditor *editor)
{
    QList<QPair<QString, std::function<void()>>> out;
    if (!doc || !editor)
        return out;
    const QString path = doc->filePath();
    if (const QString definition = definitionActionTitle(doc, editor); !definition.isEmpty())
        out << qMakePair(definition, std::function<void()>([this] { generateDefinition(); }));
    const PackageState s = packageState(path, doc->text());
    if (s.mismatch) {
        QPointer<CodeEditor> guard(editor);
        const QString folder = s.folder;
        out << qMakePair(folder.isEmpty() ? tr("Remove the package statement (the file is in the source root)") : tr("Change package to '%1'").arg(folder),
                         std::function<void()>([guard, path, folder] {
                             if (guard)
                                 replaceText(guard, JvmTools::withPackage(path, guard->toPlainText(), folder));
                         }));
        if (!s.declared.isEmpty()) {
            const QString target = s.sourceRoot + QLatin1Char('/') + QString(s.declared).replace(QLatin1Char('.'), QLatin1Char('/'));
            out << qMakePair(tr("Move file to %1/").arg(QDir(m_projects->hasProject() ? m_projects->root() : s.sourceRoot).relativeFilePath(target)),
                             std::function<void()>([this, doc, path, target] {
                                 if (doc->isModified() && !m_editors->saveCurrent())
                                     return;
                                 if (!QDir().mkpath(target)) {
                                     QMessageBox::warning(this, tr("Move File"), tr("Could not create %1.").arg(target));
                                     return;
                                 }
                                 m_mover->move({path}, target);
                             }));
        }
    }
    if (JvmTools::isMainKts(path))
        out << qMakePair(tr("Add a Dependency (@file:DependsOn)…"), std::function<void()>([this] { showDependencies(false); }));
    return out;
}

// --- dependencies -------------------------------------------------------------------------------------------------------

void MainWindow::showDependencies(bool updates)
{
    Document *doc = m_editors->currentDocument();
    const QString path = doc ? doc->filePath() : QString();
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    const bool script = JvmTools::isMainKts(path) || (JvmTools::isKotlinScript(path) && !JvmTools::isJvmBuild(root));
    if (!script && !JvmTools::isJvmBuild(root)) {
        QMessageBox::information(this, tr("Dependencies"),
                                 tr("Open a Gradle or Maven project, or a Kotlin script (*.main.kts), to manage its dependencies."));
        return;
    }
    auto *dlg = new DependencyDialog(root, path, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    QPointer<Document> scriptDoc = script ? doc : nullptr;
    if (script)
        dlg->setScript(path, doc->text());
    connect(dlg, &DependencyDialog::addRequested, this,
            [this, dlg, root](const QString &file, const QString &group, const QString &artifact, const QString &version, const QString &scope, bool catalog) {
                for (const QString &f : {file, DependencyEdit::catalogPath(root)})
                    if (Document *open = f.isEmpty() ? nullptr : m_editors->documentForPath(f); open && open->isModified()) {
                        dlg->setStatus(tr("%1 has unsaved changes. Save it first.").arg(QFileInfo(f).fileName()), true);
                        return;
                    }
                const DependencyEdit::Change c = DependencyEdit::add(root, file, group, artifact, version, scope, catalog);
                if (!c.ok) {
                    dlg->setStatus(c.error, true);
                    return;
                }
                QList<QPair<QString, QString>> writes = c.extra;
                writes.prepend(qMakePair(c.file, c.text));
                for (const auto &w : std::as_const(writes)) {
                    QFile f(w.first);
                    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        dlg->setStatus(tr("Could not write %1: %2").arg(w.first, f.errorString()), true);
                        return;
                    }
                    f.write(w.second.toUtf8());
                }
                dlg->setStatus(tr("Added %1 to %2.").arg(c.summary, QDir(root).relativeFilePath(c.file)));
                statusBar()->showMessage(tr("Added %1").arg(c.summary), 6000);
                if (m_lsp->hasBuildFiles())
                    m_lsp->syncBuildFiles(true);
            });
    connect(dlg, &DependencyDialog::dependsOnRequested, this, [this, dlg, scriptDoc](const QString &coordinate) {
        CodeEditor *ed = scriptDoc ? m_editors->editorFor(scriptDoc) : nullptr;
        if (!ed) {
            dlg->setStatus(tr("The script is no longer open."), true);
            return;
        }
        if (ed->toPlainText().contains(QLatin1Char('"') + coordinate + QLatin1Char('"'))) {
            dlg->setStatus(tr("%1 is already there.").arg(coordinate), true);
            return;
        }
        replaceText(ed, DependencyEdit::addDependsOn(ed->toPlainText(), coordinate));
        dlg->setStatus(tr("Added @file:DependsOn(\"%1\"). Save the script to check it.").arg(coordinate));
    });
    connect(dlg, &DependencyDialog::updatesRequested, this, [this, dlg, scriptDoc](const QList<QPair<DependencyEdit::Declared, QString>> &list) {
        if (scriptDoc) {
            CodeEditor *ed = m_editors->editorFor(scriptDoc);
            if (!ed)
                return;
            QString text = ed->toPlainText();
            auto sorted = list;
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first.start > b.first.start; });
            for (const auto &u : std::as_const(sorted))
                if (QStringView(text).mid(u.first.start, u.first.length) == u.first.version)
                    text.replace(u.first.start, u.first.length, u.second);
            replaceText(ed, text);
            dlg->setStatus(tr("Updated %n version(s) in the script.", nullptr, int(list.size())));
            return;
        }
        for (const auto &u : list)
            if (Document *open = m_editors->documentForPath(u.first.file); open && open->isModified()) {
                dlg->setStatus(tr("%1 has unsaved changes. Save it first.").arg(QFileInfo(u.first.file).fileName()), true);
                return;
            }
        for (const auto &w : DependencyEdit::applyUpdates(list)) {
            QFile f(w.first);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(w.second.toUtf8());
        }
        dlg->setStatus(tr("Updated %n version(s). Check the release notes of major upgrades.", nullptr, int(list.size())));
        if (m_lsp->hasBuildFiles())
            m_lsp->syncBuildFiles(true);
        updateJvmStatus();
    });
    dlg->show();
    if (updates)
        dlg->showUpdates();
}

// --- JDK ----------------------------------------------------------------------------------------------------------------

QString MainWindow::jdkStartupCommand(const QString &shell) const
{
    if (!m_projects->hasProject())
        return {};
    const QString home = SettingsManager::instance().projectJdk();
    if (home.isEmpty() || !QFileInfo::exists(home + QStringLiteral("/bin/java")))
        return {};
    const QString q = JvmTools::shellQuote(home);
    const QString bin = JvmTools::shellQuote(home + QStringLiteral("/bin"));
    if (shell == QLatin1String("fish"))
        return QStringLiteral("set -gx JAVA_HOME %1; set -gx PATH %2 $PATH").arg(q, bin);
    return QStringLiteral("export JAVA_HOME=%1 PATH=%2:\"$PATH\"").arg(q, bin);
}

void MainWindow::showJdkManager()
{
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    const JvmTools::Versions v = JvmTools::detectVersions(root);
    const QString before = SettingsManager::instance().projectJdk();
    JdkDialog dlg(before, v.toolchain, v.autoProvision, this);
    if (!m_projects->hasProject())
        dlg.setWindowTitle(tr("Installed JDKs"));
    if (dlg.exec() != QDialog::Accepted || !m_projects->hasProject() || dlg.chosenHome() == before)
        return;
    SettingsManager::instance().setProjectJdk(dlg.chosenHome());
    updateJvmStatus();
    if (m_terminal->isRunning()) {
        const auto answer = QMessageBox::question(this, tr("Project JDK"),
                                                  tr("New terminal sessions use the new JDK. Restart the open sessions now? Programs running in "
                                                     "them are stopped."),
                                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes)
            m_terminal->restartAll();
    }
}

// --- status bar -----------------------------------------------------------------------------------------------------

void MainWindow::updateJvmStatus()
{
    if (!m_jvmStatus)
        return;
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    if (!JvmTools::isJvmBuild(root)) {
        m_jvmStatus->hide();
        return;
    }
    const JvmTools::Versions v = JvmTools::detectVersions(root);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    auto where = [&root](const JvmTools::Found &f) { return QStringLiteral("%1:%2").arg(QDir(root).relativeFilePath(f.file)).arg(f.line); };
    m_kotlinBtn->setVisible(!v.kotlin.isEmpty());
    m_kotlinBtn->setText(QStringLiteral("Kotlin %1").arg(v.kotlin.value));
    m_kotlinBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/kotlin.svg"), t.keyword));
    m_kotlinBtn->setToolTip(tr("Kotlin version, declared in %1 (click to open)").arg(where(v.kotlin))
                            + QStringLiteral("\n\nCopyright © 2026 JetBrains s.r.o. Kotlin and the Kotlin logo are trademarks of JetBrains s.r.o."));

    const QString chosen = SettingsManager::instance().projectJdk();
    const QList<JdkScanner::Jdk> jdks = JdkScanner::scan();
    bool toolchainMissing = v.toolchain > 0 && !v.autoProvision;
    for (const JdkScanner::Jdk &j : jdks)
        if (j.major == v.toolchain && !j.jre)
            toolchainMissing = false;
    const JdkScanner::Jdk used = chosen.isEmpty() ? JdkScanner::defaultJdk(jdks) : JdkScanner::fromHome(chosen);
    QString javaText = v.java.isEmpty() ? (used.isValid() ? QStringLiteral("JDK %1").arg(used.major) : tr("JDK")) : QStringLiteral("Java %1").arg(v.java.value);
    QStringList tip;
    if (!v.java.isEmpty())
        tip << (v.toolchain > 0 ? tr("Toolchain %1, declared in %2").arg(v.java.value, where(v.java)) : tr("Java target %1, declared in %2").arg(v.java.value, where(v.java)));
    tip << (used.isValid() ? tr("%1 JDK: %2 (%3)").arg(chosen.isEmpty() ? tr("System") : tr("Project"), used.label(), used.home) : tr("No JDK found"));
    if (toolchainMissing) {
        javaText = QStringLiteral("⚠ ") + javaText;
        tip << tr("No Java %1 toolchain is installed: Gradle will fail to compile.").arg(v.toolchain);
    }
    tip << tr("Click to choose the project JDK.");
    m_javaBtn->setText(javaText);
    m_javaBtn->setToolTip(tip.join(QLatin1Char('\n')));
    m_javaBtn->setStyleSheet(toolchainMissing ? QStringLiteral("QToolButton { padding: 1px 5px; color: %1; }").arg(t.warning.name())
                                              : QStringLiteral("QToolButton { padding: 1px 5px; }"));

    m_buildToolBtn->setVisible(!v.build.isEmpty());
    m_buildToolBtn->setText(QStringLiteral("%1 %2").arg(v.maven ? QStringLiteral("Maven") : QStringLiteral("Gradle"), v.build.value));
    m_buildToolBtn->setIcon(Icons::tinted(v.maven ? QStringLiteral(":/new-icons/apachemaven.svg") : QStringLiteral(":/new-icons/gradle.svg"), t.number));
    m_buildToolBtn->setToolTip(tr("Wrapper version, declared in %1 (click to open)").arg(where(v.build)));
    m_jvmStatus->show();
    if (m_gradle)
        m_gradle->updateWrapper();
    if (m_maven)
        m_maven->updateWrapper();
}
