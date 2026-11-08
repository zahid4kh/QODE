// MainWindow's framework strips and the right-hand island: Python, Expo, Spring, the web and Markdown / media
// previews, the Gradle panel.
#include "MainWindow.h"

#include "app/StartupProfile.h"
#include "dialogs/PackagesDialog.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "expo/ExpoBar.h"
#include "expo/ExpoConfigDialog.h"
#include "expo/ExpoRoutes.h"
#include "expo/ExpoSchema.h"
#include "expo/ExpoWebServer.h"
#include "gradle/GradlePanel.h"
#include "gradle/GradleTasks.h"
#include "lsp/LspManager.h"
#include "media/MarkdownPreview.h"
#include "media/MediaPanel.h"
#include "project/ProjectFiles.h"
#include "project/ProjectManager.h"
#include "project/PythonEnv.h"
#include "settings/SettingsManager.h"
#include "spring/AddStarterDialog.h"
#include "spring/SpringBar.h"
#include "spring/SpringLogView.h"
#include "spring/SpringPanel.h"
#include "spring/SpringScaffold.h"
#include "spring/SpringStarters.h"
#include "terminal/TerminalPanel.h"
#include "webdev/DevServer.h"
#include "webdev/DevServerBar.h"
#include "webdev/WebPreview.h"
#include "filesystem/FileManager.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

void MainWindow::askNestedWebProject(const QStringList &dirs)
{
    if (!m_projects->hasProject() || dirs.isEmpty())
        return;
    QString dir = dirs.first();
    if (dirs.size() > 1) {
        bool ok = false;
        dir = QInputDialog::getItem(this, tr("Web Projects Detected"),
                                    tr("Detected %1 web projects inside this project. Which one should QODE run?").arg(dirs.size()), dirs, 0,
                                    false, &ok);
        if (!ok)
            return;
    } else {
        QMessageBox box(QMessageBox::Question, tr("Web Project Detected"),
                        tr("Detected a web project at <b>%1</b>.<br>Do you want to run its dev server?").arg(dir.toHtmlEscaped()), {}, this);
        box.setInformativeText(tr("QODE will run it from that folder; the server controls then appear in the toolbar."));
        QPushButton *run = box.addButton(tr("Run Server"), QMessageBox::AcceptRole);
        box.addButton(tr("Not Now"), QMessageBox::RejectRole);
        QPushButton *never = box.addButton(tr("Don't Ask Again"), QMessageBox::DestructiveRole);
        box.setDefaultButton(run);
        box.exec();
        if (box.clickedButton() == never)
            m_serverBar->dontAskAboutNested();
        if (box.clickedButton() != run)
            return;
    }
    m_serverBar->useFolder(dir);
}

void MainWindow::updateRunToolbar()
{
    const bool web = m_serverBar->isAvailable();
    const bool expo = m_expoBar && m_expoBar->isAvailable();
    const bool spring = m_springBar && m_springBar->isAvailable();
    const bool ktor = ktorAvailable();
    m_mainToolBar->removeAction(m_runAct);
    m_mainToolBar->removeAction(m_debugAct);
    m_mainToolBar->removeAction(m_runConfigAct);
    m_mainToolBar->removeAction(m_pythonPkgAct);
    m_mainToolBar->removeAction(m_gradleAct);
    m_mainToolBar->removeAction(m_mavenAct);
    if (m_expoBarAct)
        m_expoBarAct->setVisible(expo);
    if (m_springBarAct)
        m_springBarAct->setVisible(spring);
    if (m_ktorBarAct)
        m_ktorBarAct->setVisible(ktor);
    if (!web && !expo && !spring && !ktor) {
        m_mainToolBar->insertAction(m_serverBarAct, m_runAct);
        m_mainToolBar->insertAction(m_serverBarAct, m_debugAct);
        m_mainToolBar->insertAction(m_serverBarAct, m_runConfigAct);
    }
    m_mainToolBar->insertAction(m_serverBarAct, m_gradleAct); // only in Gradle projects (updateGradleUi)
    m_mainToolBar->insertAction(m_serverBarAct, m_mavenAct);  // only in Maven projects (updateMavenUi)
    m_mainToolBar->insertAction(m_serverBarAct, m_pythonPkgAct); // next to the run buttons; visible only with a virtual environment
    m_serverBarAct->setVisible(web);
    m_runAct->setText(web ? tr("Start / Restart Dev Server") : tr("Run File"));
    m_runAct->setToolTip(web ? tr("Start the dev server, or restart it (F5)") : tr("Run the project, or this file (F5)"));
    m_runConfigAct->setText(web ? tr("Dev Server Settings…") : tr("Run Configuration…"));
    m_runConfigAct->setToolTip(web ? tr("Package manager, script and port of the dev server")
                                   : tr("Set the command that runs the project or this type of file"));
}

// The Python Packages button sits next to Run and Run Configuration and exists only while the project has a virtual
// environment. A timer notices one created in the terminal (or deleted) a few seconds later.
void MainWindow::updatePythonUi()
{
    updateGradleUi();
    updateMavenUi();
    const QString root = m_projects->hasProject() ? m_projects->project().root : QString();
    const QString venv = root.isEmpty() ? QString() : PythonEnv::activeVenv(root);
    m_pythonPkgAct->setVisible(!venv.isEmpty());
    m_pythonPkgAct->setEnabled(m_projects->hasProject());
    if (!venv.isEmpty()) {
        const QString version = PythonEnv::venvVersion(root, venv);
        m_pythonPkgAct->setToolTip(tr("Python packages: install, upgrade and remove (%1%2)")
                                       .arg(venv, version.isEmpty() ? QString() : tr(", Python %1").arg(version)));
    }
    if (!m_venvTimer) {
        m_venvTimer = new QTimer(this);
        m_venvTimer->setInterval(3000);
        connect(m_venvTimer, &QTimer::timeout, this, &MainWindow::updatePythonUi);
    }
    if (root.isEmpty())
        m_venvTimer->stop();
    else if (!m_venvTimer->isActive())
        m_venvTimer->start();
    if (venv != m_activeVenv) {
        m_activeVenv = venv;
        restartPythonServer(); // the interpreter its imports resolve against changed
    }
}

// The language server reads the interpreter once at start: restart it after packages or the environment changed.
void MainWindow::restartPythonServer()
{
    for (const LspManager::ServerState &st : m_lsp->servers())
        if (st.id == QLatin1String("python") && (st.status == LspManager::Status::Running || st.status == LspManager::Status::Starting))
            m_lsp->restart(st.id);
}

// --- Expo -----------------------------------------------------------------------

void MainWindow::onExpoChanged()
{
    m_serverBar->setIgnoredDirs(m_expoBar->appDirs());
    updateRunToolbar();
    updateActions();
    updatePreviewTargets();
    if (!m_expoBar->isAvailable()) {
        m_lsp->setJsonSchemas({});
        return;
    }
    QSet<QString> asked;
    for (const ExpoApp &a : m_expoBar->apps())
        if (!a.appJson.isEmpty() && !asked.contains(a.sdk)) {
            asked.insert(a.sdk);
            m_expoSchema->request(a.sdk); // downloads once into ~/.cache/QODE-AGENTIC/expo; ready() pushes it to the JSON server
        }
    pushExpoSchemas();
}

// The cached schema matching an app's SDK, else the newest cached one (empty when nothing is cached yet).
static QPair<QString, QJsonObject> expoSchemaFor(const QString &sdk)
{
    QString use = ExpoSchema::cachedSdks().contains(sdk) ? sdk : QString();
    if (use.isEmpty() && !ExpoSchema::cachedSdks().isEmpty())
        use = ExpoSchema::cachedSdks().first();
    return {use, use.isEmpty() ? QJsonObject() : ExpoSchema::load(use)};
}

// Tells the JSON language server which schema describes each app.json: completion, hover docs and validation.
void MainWindow::pushExpoSchemas()
{
    QMap<QString, QJsonArray> files; // schema sdk -> file URIs
    for (const ExpoApp &a : m_expoBar->apps())
        if (!a.appJson.isEmpty()) {
            const QString use = expoSchemaFor(a.sdk).first;
            if (!use.isEmpty())
                files[use].append(QUrl::fromLocalFile(a.appJson).toString());
        }
    QJsonArray schemas;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject schema = ExpoSchema::load(it.key());
        if (!schema.isEmpty())
            schemas.append(QJsonObject{{QStringLiteral("fileMatch"), it.value()}, {QStringLiteral("schema"), schema}});
    }
    m_lsp->setJsonSchemas(schemas);
}

void MainWindow::runExpoCommand(const QString &command, const QString &id)
{
    if (!m_editors->saveAll())
        return;
    if (id == QLatin1String("start") || id.startsWith(QLatin1String("android")) || id == QLatin1String("ios"))
        m_metroStarted = true; // run:android / run:ios end in the same interactive Metro session
    showTerminal();
    m_terminal->runCommand(command);
    m_terminal->focusTerminal();
}

void MainWindow::showExpoConfig()
{
    const ExpoApp *app = m_expoBar->currentApp();
    if (!app || app->appJson.isEmpty())
        return;
    if (m_expoDialog) {
        m_expoDialog->raise();
        m_expoDialog->activateWindow();
        return;
    }
    if (!m_editors->saveAll()) // the form edits the file on disk: unsaved text in an editor tab would be overwritten
        return;
    m_expoDialog = new ExpoConfigDialog(*app, expoSchemaFor(app->sdk).second, this);
    connect(m_expoDialog, &ExpoConfigDialog::openInEditorRequested, this, [this](const QString &path) { m_editors->openFile(path); });
    connect(m_expoDialog, &ExpoConfigDialog::saved, this, [this](const QString &path) {
        statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 5000);
        m_expoBar->setProjectRoot(m_projects->project().root); // the app name may have changed
    });
    m_expoDialog->show();
}

void MainWindow::showExpoSchemaCache()
{
    const QString dir = ExpoSchema::cacheDir();
    const QStringList sdks = ExpoSchema::cachedSdks();
    QMessageBox box(QMessageBox::NoIcon, tr("Expo Schema Cache"), QString(), {}, this);
    box.setTextFormat(Qt::RichText);
    box.setText(tr("<b>Expo app.json schema</b>"));
    box.setInformativeText(
        tr("QODE downloads Expo's published schema (exp.host) once per SDK and caches it here. It powers completion, hover "
           "documentation and validation in app.json, and nothing is written into your projects.<br><br>"
           "<b>Cached:</b> %1<br><b>Folder:</b> %2 (%3)<br><br>"
           "<b>To remove it by hand:</b><br><code>rm -rf %2</code><br><br>"
           "It is downloaded again the next time you open an Expo project.")
            .arg(sdks.isEmpty() ? tr("nothing yet") : tr("SDK %1").arg(sdks.join(QStringLiteral(", "))).toHtmlEscaped(),
                 dir.toHtmlEscaped(), QLocale().formattedDataSize(ExpoSchema::cacheSize())));
    QPushButton *remove = box.addButton(tr("Delete Cache"), QMessageBox::DestructiveRole);
    QPushButton *copy = box.addButton(tr("Copy Command"), QMessageBox::ActionRole);
    box.addButton(tr("Close"), QMessageBox::RejectRole);
    remove->setEnabled(QFileInfo::exists(dir));
    box.exec();
    if (box.clickedButton() == copy) {
        QApplication::clipboard()->setText(QStringLiteral("rm -rf '%1'").arg(dir));
        statusBar()->showMessage(tr("Copied the removal command"), 4000);
    } else if (box.clickedButton() == remove) {
        if (ExpoSchema::clearCache()) {
            m_lsp->setJsonSchemas({});
            statusBar()->showMessage(tr("Deleted the Expo schema cache"), 5000);
        } else {
            QMessageBox::warning(this, tr("Expo Schema Cache"), tr("Could not delete %1").arg(dir));
        }
    }
}

void MainWindow::showPythonPackages()
{
    if (!m_projects->hasProject())
        return;
    if (m_pkgDialog) {
        m_pkgDialog->show();
        m_pkgDialog->raise();
        m_pkgDialog->activateWindow();
        return;
    }
    m_pkgDialog = new PackagesDialog(m_projects->project().root, this);
    connect(m_pkgDialog, &PackagesDialog::packagesChanged, this, &MainWindow::restartPythonServer);
    connect(m_pkgDialog, &PackagesDialog::environmentChanged, this, [this] {
        updatePythonUi();
        restartPythonServer();
    });
    connect(m_pkgDialog, &PackagesDialog::venvCreated, this, [this](const QString &name) {
        updatePythonUi();
        restartPythonServer();
        statusBar()->showMessage(tr("Created the virtual environment %1").arg(name), 6000);
        if (PythonEnv::activationCommand(m_projects->project().root, m_terminal->currentShellFile()).isEmpty())
            return;
        if (QMessageBox::question(m_pkgDialog, tr("Activate it?"),
                                  tr("Activate %1 in the terminal now? New terminals activate it automatically.").arg(name)) == QMessageBox::Yes) {
            showTerminal(); // may start the first pane: its shell decides the command
            m_terminal->runCommand(PythonEnv::activationCommand(m_projects->project().root, m_terminal->currentShellFile()));
        }
    });
    m_pkgDialog->show();
}

// The Gradle button exists only in Gradle projects (Java, Kotlin or Compose Desktop alike).
void MainWindow::updateGradleUi()
{
    const QString dir = m_projects->hasProject() ? GradleTasks::buildDir(m_projects->project().root) : QString();
    m_gradleAct->setVisible(!dir.isEmpty());
    m_gradle->setBuildDir(dir);
    if (dir.isEmpty() && rightPanelShows(m_gradle))
        hideMedia();
}

// The Spring Boot button / panel exist only in Spring Boot projects. The panel is created with the first one: it keeps
// the application's log from the start, whether it is shown or not.
void MainWindow::updateSpringUi()
{
    const bool spring = m_springBar && m_springBar->isAvailable();
    m_springAct->setVisible(spring);
    m_springStarterAct->setVisible(spring);
    if (spring)
        springPanel();
    if (!spring && rightPanelShows(m_spring))
        hideMedia();
}

void MainWindow::addSpringStarters()
{
    if (!m_springBar || !m_springBar->isAvailable())
        return;
    const SpringProject project = m_springBar->project();
    if (Document *open = m_editors->documentForPath(project.buildFile()); open && open->isModified()) {
        QMessageBox::information(this, tr("Add Spring Starters"), tr("%1 has unsaved changes. Save it first.").arg(QFileInfo(project.buildFile()).fileName()));
        return;
    }
    AddStarterDialog dlg(project, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const SpringStarters::Change change = SpringStarters::plan(project, dlg.selectedIds());
    if (!change.ok) {
        QMessageBox::warning(this, tr("Add Spring Starters"), change.error);
        return;
    }
    // In the file's own encoding (a BOM or UTF-16 stays), written atomically with Windows' busy-file retries.
    QString error;
    if (!FileManager::writeText(change.file, change.text, &error)) {
        QMessageBox::warning(this, tr("Add Spring Starters"), tr("Could not write %1: %2").arg(change.file, error));
        return;
    }
    for (const auto &extra : change.extra)
        FileManager::writeText(extra.first, extra.second);
    m_editors->openFile(change.file);
    statusBar()->showMessage(tr("Added %1 to %2").arg(change.added.join(QStringLiteral(", ")), QFileInfo(change.file).fileName()), 6000);
    if (!change.note.isEmpty())
        QMessageBox::information(this, tr("Add Spring Starters"), change.note);
    if (m_lsp->hasBuildFiles())
        m_lsp->syncBuildFiles(true);
    springPanel()->updateTabs();
    if (m_springBar->isActive())
        statusBar()->showMessage(tr("Added %1 — restart the application to use it").arg(change.added.join(QStringLiteral(", "))), 8000);
}

// "New Spring Component" in the explorer's context menu: controller, service, entity …
void MainWindow::newSpringComponent(int kindValue, const QString &dir)
{
    const auto kind = static_cast<SpringScaffold::Kind>(kindValue);
    const QStringList roots = m_springBar->project().sourceRoots();
    if (SpringScaffold::packageOf(dir, roots).isEmpty()) {
        QMessageBox::information(this, tr("New Spring Component"), tr("Create it in a package folder below src/main/kotlin or src/main/java."));
        return;
    }
    bool ok = false;
    const QString kindName = SpringScaffold::title(kind);
    const QString name = QInputDialog::getText(this, tr("New %1").arg(kindName),
                                               kind == SpringScaffold::Kind::Repository ? tr("Entity the repository is for:") : tr("Name:"),
                                               QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const SpringScaffold::Result r = SpringScaffold::generate(kind, name, dir, roots);
    if (!r.ok()) {
        QMessageBox::warning(this, tr("New %1").arg(kindName), r.error);
        return;
    }
    if (QFileInfo::exists(r.path)) {
        QMessageBox::warning(this, tr("New %1").arg(kindName), tr("%1 already exists.").arg(QFileInfo(r.path).fileName()));
        return;
    }
    QFile f(r.path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("New %1").arg(kindName), tr("Could not create %1: %2").arg(r.path, f.errorString()));
        return;
    }
    f.write(r.content.toUtf8());
    f.close();
    m_editors->openFile(r.path);
}

// Entity + repository + service + controller with list / get / create / update / delete in one go.
void MainWindow::newSpringCrud(const QString &dir)
{
    const QStringList roots = m_springBar->project().sourceRoots();
    if (SpringScaffold::packageOf(dir, roots).isEmpty())
        return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("New CRUD Set"));
    auto *form = new QFormLayout(&dlg);
    auto *name = new QLineEdit(&dlg);
    name->setPlaceholderText(tr("Product"));
    auto *fields = new QLineEdit(&dlg);
    fields->setPlaceholderText(tr("name:String, price:Double, stock:Int"));
    auto *hint = new QLabel(tr("Creates the entity, repository, service and a controller with GET, POST, PUT and DELETE endpoints. "
                               "Fields are optional; an id is added for you."), &dlg);
    hint->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(tr("Entity name:"), name);
    form->addRow(tr("Fields:"), fields);
    form->addRow(hint);
    form->addRow(buttons);
    dlg.setMinimumWidth(460);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted || name->text().trimmed().isEmpty())
        return;

    QString error;
    const auto parsed = SpringScaffold::parseFields(fields->text(), &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("New CRUD Set"), error);
        return;
    }
    const auto files = SpringScaffold::generateCrud(name->text(), parsed, dir, roots);
    if (files.isEmpty() || !files.first().ok()) {
        QMessageBox::warning(this, tr("New CRUD Set"), files.isEmpty() ? tr("Nothing to create.") : files.first().error);
        return;
    }
    QStringList taken;
    for (const auto &f : files)
        if (QFileInfo::exists(f.path))
            taken << QFileInfo(f.path).fileName();
    if (!taken.isEmpty()) {
        QMessageBox::warning(this, tr("New CRUD Set"), tr("Already exists: %1. Nothing was written.").arg(taken.join(QStringLiteral(", "))));
        return;
    }
    for (const auto &f : files) {
        QFile out(f.path);
        if (!out.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, tr("New CRUD Set"), tr("Could not create %1: %2").arg(f.path, out.errorString()));
            return;
        }
        out.write(f.content.toUtf8());
    }
    for (const auto &f : files)
        m_editors->openFile(f.path);
    m_editors->openFile(files.last().path); // the controller ends up active
    statusBar()->showMessage(tr("Created %1 files").arg(files.size()), 5000);
}

void MainWindow::toggleSpringPanel()
{
    if (rightPanelShows(m_spring)) {
        hideMedia();
        return;
    }
    showInRightPanel(springPanel());
    m_projectFiles->ensureFresh(); // stack frames are matched against the project's files
}

SpringPanel *MainWindow::springPanel()
{
    if (m_spring)
        return m_spring;
    StartupProfile::Later timing(QStringLiteral("Spring Boot panel"));
    m_spring = new SpringPanel(this);
    m_rightStack->addWidget(m_spring);
    connect(m_spring, &SpringPanel::closeRequested, this, &MainWindow::hideMedia);
    connect(m_spring, &SpringPanel::openTextRequested, this, &MainWindow::openTextInEditor);
    connect(m_spring, &SpringPanel::openFileRequested, this, [this](const QString &path) { m_editors->openFile(path); });
    connect(m_spring, &SpringPanel::locationRequested, this, [this](const QString &path, int line, int column) {
        m_editors->openFileAt(path, line, qMax(1, column));
    });
    // A stack frame "com.example.demo.HelloController.index(HelloController.kt:12)" -> the project file that holds it.
    m_spring->log()->setResolver([this](const QString &pkgPath, const QString &fileName) -> QString {
        const QString suffix = QLatin1Char('/') + (pkgPath.isEmpty() ? fileName : pkgPath + QLatin1Char('/') + fileName);
        for (const QString &f : m_projectFiles->files())
            if (f.endsWith(suffix))
                return f;
        return {};
    });
    connect(m_projectFiles, &ProjectFiles::updated, m_spring, [this] { m_spring->log()->clearResolveCache(); });
    m_spring->attach(m_springBar);
    return m_spring;
}

void MainWindow::toggleWebPreview()
{
#ifndef QODE_HAS_WEBENGINE
    return; // built without Qt WebEngine: no web / Expo preview (its buttons are hidden)
#endif
    if (rightPanelShows(m_webPreview)) {
        hideMedia();
        return;
    }
    showInRightPanel(webPreview());
}

WebPreview *MainWindow::webPreview()
{
    if (m_webPreview)
        return m_webPreview;
    StartupProfile::Later timing(QStringLiteral("web preview"));
    m_webPreview = new WebPreview(this);
    m_rightStack->addWidget(m_webPreview);
    connect(m_webPreview, &WebPreview::closeRequested, this, &MainWindow::hideMedia);
    connect(m_webPreview, &WebPreview::targetChosen, this, [](const QString &id) {
        QJsonObject cfg = SettingsManager::instance().expo();
        cfg.insert(QStringLiteral("preview"), id);
        SettingsManager::instance().setExpo(cfg);
    });
    connect(m_webPreview, &WebPreview::startRequested, this, &MainWindow::startPreviewTarget);
    connect(m_webPreview, &WebPreview::logRequested, this, [this](const QString &id) {
        if (id.startsWith(QLatin1String("expo:")))
            m_expoBar->showWebLog();
        else
            m_serverBar->showLog();
    });
    connect(m_webPreview, &WebPreview::fixRequested, this, [this](const QString &id) {
        for (const ExpoApp &a : m_expoBar->apps())
            if (id == QLatin1String("expo:") + a.rel)
                m_expoBar->installWebSupport(a.dir);
    });
    updatePreviewTargets();
    return m_webPreview;
}

// The web dev server (when the project has one) and every Expo app; the preview shows one of them. The Expo apps share
// one preview Metro, so only the app it runs gets the server.
void MainWindow::updatePreviewTargets()
{
    if (!m_expoBar || !m_webPreview)
        return; // still building the toolbar, or the preview was never opened (webPreview() calls this)
    QVector<WebPreview::Target> targets;
    if (m_serverBar->isAvailable()) {
        WebPreview::Target web;
        web.id = QStringLiteral("web");
        const QString dir = m_serverBar->workDir();
        web.name = m_serverBar->project().framework.isEmpty() ? QFileInfo(dir).fileName() : m_serverBar->project().framework;
        if (m_projects->hasProject()) {
            web.where = QDir(m_projects->project().root).relativeFilePath(dir);
            if (web.where == QLatin1String("."))
                web.where.clear();
        }
        web.server = m_serverBar->server();
        targets.append(web);
    }
    ExpoWebServer *metro = m_expoBar->webServer();
    for (const ExpoApp &a : m_expoBar->apps()) {
        WebPreview::Target t;
        t.id = QStringLiteral("expo:") + a.rel;
        t.name = a.name;
        t.where = a.rel;
        t.mobile = true;
        if (metro && metro->appDir() == a.dir && metro->state() != DevServer::State::Stopped)
            t.server = metro;
        t.routesDir = ExpoRoutes::routesDir(a.dir);
        if (!t.routesDir.isEmpty())
            t.routes = [dir = a.dir] {
                QVector<WebPreview::Route> routes;
                for (const ExpoRoute &r : ExpoRoutes::scan(dir))
                    routes.append({r.path, r.file});
                return routes;
            };
        if (a.dependencies.contains(QLatin1String("expo-sqlite"))) {
            t.minChromium = 108; // synchronous OPFS access handles
            t.minChromiumWhy = QStringLiteral("expo-sqlite");
        }
        if (!a.web)
            t.problem = tr("%1 is set up for iOS and Android only.\n\n"
                           "The preview doesn't run the native app: it runs the app's web version (react-native-web) inside a "
                           "phone frame, which needs react-native-web and react-dom. Add Web Support installs them with npx "
                           "expo install in the terminal (it changes package.json).\n\n"
                           "Native-only code (in-app purchases, Google Sign-In, Firebase's React Native persistence, …) doesn't "
                           "exist on the web, so an app built only for devices may still stop on start-up here even though it "
                           "runs fine on a phone. To see the native app, run it on a device or an emulator.")
                            .arg(a.name);
        targets.append(t);
    }
    m_webPreview->setTargets(targets, SettingsManager::instance().expo().value(QStringLiteral("preview")).toString());
}

void MainWindow::showPreviewTarget(const QString &id, bool start)
{
#ifndef QODE_HAS_WEBENGINE
    return; // see toggleWebPreview
#endif
    QJsonObject cfg = SettingsManager::instance().expo();
    cfg.insert(QStringLiteral("preview"), id);
    SettingsManager::instance().setExpo(cfg);
    updatePreviewTargets();
    if (!rightPanelShows(m_webPreview))
        toggleWebPreview();
    if (start)
        startPreviewTarget(id);
}

void MainWindow::startPreviewTarget(const QString &id)
{
    if (id == QLatin1String("web")) {
        if (!m_serverBar->isActive())
            m_serverBar->start();
        return;
    }
    for (const ExpoApp &a : m_expoBar->apps()) {
        if (id != QLatin1String("expo:") + a.rel || !a.web)
            continue;
        ExpoWebServer *metro = m_expoBar->webServer();
        if (metro && !(metro->isActive() && metro->appDir() == a.dir))
            m_expoBar->startWeb(a.dir);
        return;
    }
}

void MainWindow::toggleGradlePanel()
{
    if (rightPanelShows(m_gradle)) {
        hideMedia();
        return;
    }
    showInRightPanel(m_gradle);
    m_gradle->ensureLoaded();
}

bool MainWindow::rightPanelShows(const QWidget *panel) const
{
    return panel && m_rightStack->currentWidget() == panel && m_rightStack->parentWidget()->isVisible();
}

void MainWindow::showInRightPanel(QWidget *panel)
{
    disconnect(m_previewConn);
    disconnect(m_previewScrollConn);
    m_previewDoc = nullptr;
    m_previewTimer->stop();
    clearPreviews();
    m_rightStack->setCurrentWidget(panel);
    openRightPanel();
}

void MainWindow::clearPreviews()
{
    if (m_media)
        m_media->clear();
    if (m_md)
        m_md->clear();
}

MediaPanel *MainWindow::mediaPanel()
{
    if (m_media)
        return m_media;
    StartupProfile::Later timing(QStringLiteral("media panel"));
    m_media = new MediaPanel(this);
    m_rightStack->addWidget(m_media);
    connect(m_media, &MediaPanel::closeRequested, this, &MainWindow::hideMedia);
    if (m_projects->hasProject())
        m_media->setProjectRoot(m_projects->project().root);
    return m_media;
}

MarkdownPreview *MainWindow::markdownPreview()
{
    if (m_md)
        return m_md;
    StartupProfile::Later timing(QStringLiteral("Markdown preview"));
    m_md = new MarkdownPreview(this);
    m_rightStack->addWidget(m_md);
    connect(m_md, &MarkdownPreview::closeRequested, this, &MainWindow::hideMedia);
    connect(m_md, &MarkdownPreview::topLineChanged, this, [this](int line) {
        CodeEditor *ed = m_previewDoc ? m_editors->editorFor(m_previewDoc) : nullptr;
        if (!ed || m_scrollSync)
            return;
        m_scrollSync = true;
        ed->scrollBlockToTop(line);
        m_scrollSync = false;
    });
    connect(m_md, &MarkdownPreview::openFileRequested, this, [this](const QString &path) { m_editors->openFile(path); });
    return m_md;
}

void MainWindow::openRightPanel()
{
    QWidget *island = m_rightStack->parentWidget();
    if (island->isVisible())
        return;
    island->show();
    QList<int> sizes = m_hsplit->sizes();
    const int w = qBound(280, m_mediaWidth, qMax(280, m_hsplit->width() / 2));
    sizes[2] = w;
    sizes[1] = qMax(200, sizes[1] - w);
    m_hsplit->setSizes(sizes);
}

void MainWindow::showMedia(const QString &path)
{
    showInRightPanel(mediaPanel());
    m_media->openMedia(path);
}

void MainWindow::hideMedia()
{
    QWidget *island = m_rightStack->parentWidget();
    if (island->isVisible())
        m_mediaWidth = island->width();
    disconnect(m_previewConn);
    m_previewDoc = nullptr;
    m_previewTimer->stop();
    clearPreviews();
    island->hide();
}

// The Preview button / shortcut: shows (or closes) the live preview of the current Markdown or SVG file.
void MainWindow::togglePreview()
{
    Document *doc = m_editors->currentDocument();
    if (!EditorManager::isPreviewable(doc)) {
        statusBar()->showMessage(tr("Preview is available for Markdown and SVG files"), 3000);
        return;
    }
    if (m_rightStack->parentWidget()->isVisible() && m_previewDoc == doc) {
        hideMedia();
        return;
    }
    clearPreviews();
    m_previewDoc = doc;
    disconnect(m_previewConn);
    m_previewConn = connect(doc->textDocument(), &QTextDocument::contentsChanged, m_previewTimer, qOverload<>(&QTimer::start));
    wirePreviewScroll();
    openRightPanel();
    updatePreview();
}

void MainWindow::updatePreview()
{
    if (!m_previewDoc) {
        return;
    }
    const QString path = m_previewDoc->filePath();
    if (m_previewDoc->languageName() == QLatin1String("Markdown")) {
        m_rightStack->setCurrentWidget(markdownPreview());
        m_md->setMarkdown(path, m_previewDoc->text());
        if (CodeEditor *ed = m_editors->editorFor(m_previewDoc)) { // a new render starts at the top; put it where the editor is
            int first = 0, last = 0;
            ed->visibleBlockRange(&first, &last);
            m_scrollSync = true;
            m_md->scrollToSourceLine(first);
            m_scrollSync = false;
        }
    } else {
        m_rightStack->setCurrentWidget(mediaPanel());
        m_media->previewSvg(path, m_previewDoc->text());
    }
}

// An open preview follows the active tab when that is another Markdown / SVG file.
void MainWindow::followPreview()
{
    if (!m_previewDoc || !m_rightStack->parentWidget()->isVisible())
        return;
    Document *doc = m_editors->currentDocument();
    if (doc == m_previewDoc || !EditorManager::isPreviewable(doc))
        return;
    m_previewDoc = doc;
    disconnect(m_previewConn);
    m_previewConn = connect(doc->textDocument(), &QTextDocument::contentsChanged, m_previewTimer, qOverload<>(&QTimer::start));
    clearPreviews();
    wirePreviewScroll();
    updatePreview();
}

// Keeps a Markdown preview and its editor scrolled to the same place, whichever one the user moves.
void MainWindow::wirePreviewScroll()
{
    disconnect(m_previewScrollConn);
    CodeEditor *ed = m_previewDoc ? m_editors->editorFor(m_previewDoc) : nullptr;
    if (!ed || m_previewDoc->languageName() != QLatin1String("Markdown"))
        return;
    m_previewScrollConn = connect(ed->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, ed] {
        if (m_scrollSync || !m_md || m_rightStack->currentWidget() != m_md)
            return;
        int first = 0, last = 0;
        ed->visibleBlockRange(&first, &last);
        m_scrollSync = true;
        m_md->scrollToSourceLine(first);
        m_scrollSync = false;
    });
}

// In a Spring Boot project F5 starts (or restarts) the application, unless the file belongs to a web front end
// that has its own dev server.
bool MainWindow::springOwns(const QString &file) const
{
    if (!m_springBar || !m_springBar->isAvailable())
        return false;
    if (!m_serverBar->isAvailable() || file.isEmpty())
        return true;
    return file.startsWith(m_springBar->project().moduleDir + QLatin1Char('/'));
}
