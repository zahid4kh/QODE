// MainWindow's side of the chat's IDE tools (AgentTools::Hooks::ide): the model reads the window's state, shows and hides
// panels, switches the theme, toggles editor options, runs editor commands and checks / sets up language servers. Everything
// goes through the same QActions and functions as the menus, so the menus stay in sync. Paths arrive absolute and confined to
// the project; an "error" key in the answer goes back to the model as a failure.
#include "MainWindow.h"

#include "SideSections.h"
#include "ai/AiHub.h"
#include "debug/DebugPanel.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "expo/ExpoBar.h"
#include "jvm/JvmTools.h"
#include "ktor/KtorBar.h"
#include "project/ProjectFiles.h"
#include "spring/SpringBar.h"
#include "gradle/GradlePanel.h"
#include "maven/MavenPanel.h"
#include "lsp/LspServers.h"
#include "media/MarkdownPreview.h"
#include "media/MediaPanel.h"
#include "webdev/DevServerBar.h"
#include "webdev/WebPreview.h"
#include "project/ProjectManager.h"
#include "settings/SettingsManager.h"
#include "settings/ThemeManager.h"
#include "http/HttpPanel.h"
#include "ktor/KtorPanel.h"
#include "spring/SpringPanel.h"
#include "terminal/Terminal.h"
#include "terminal/TerminalPanel.h"

#include <QAction>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTimer>

namespace {
QJsonObject error(const QString &message)
{
    return QJsonObject{{"error", message}};
}

// The side sections, in the order MainWindow adds them.
const QStringList kSections{"explorer", "version_control", "search", "tasks"};

// "Ubuntu 24.04 LTS (ubuntu, like debian)" from /etc/os-release, so the model picks the right package manager.
QString distribution()
{
    QFile f(QStringLiteral("/etc/os-release"));
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("unknown Linux");
    QHash<QString, QString> v;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const int eq = line.indexOf('=');
        if (eq > 0)
            v.insert(QString::fromUtf8(line.left(eq)), QString::fromUtf8(line.mid(eq + 1)).remove(QLatin1Char('"')));
    }
    QString s = v.value(QStringLiteral("PRETTY_NAME"), QStringLiteral("Linux"));
    QStringList ids{v.value(QStringLiteral("ID"))};
    if (!v.value(QStringLiteral("ID_LIKE")).isEmpty())
        ids << QStringLiteral("like ") + v.value(QStringLiteral("ID_LIKE"));
    ids.removeAll(QString());
    return ids.isEmpty() ? s : QStringLiteral("%1 (%2)").arg(s, ids.join(QStringLiteral(", ")));
}

// What a server's installer needs that is not there ("" = nothing missing).
QString missingPrerequisites(const LspServerSpec &spec)
{
    switch (spec.installer) {
    case LspServerSpec::Installer::Npm:
        if (LspServers::nodeExecutable().isEmpty())
            return QStringLiteral("Node.js with npm");
        if (LspServers::npmExecutable().isEmpty())
            return QStringLiteral("npm");
        return {};
    case LspServerSpec::Installer::Pip:
        return LspServers::pythonExecutable().isEmpty() ? QStringLiteral("Python 3 (with venv)") : QString();
    case LspServerSpec::Installer::Jdtls: {
        const LspServers::JavaRuntime java = LspServers::findJava(21);
        if (!java.executable.isEmpty())
            return {};
        return java.major > 0 ? QStringLiteral("Java 21 or newer (only Java %1 found)").arg(java.major) : QStringLiteral("Java 21 or newer");
    }
    default:
        return {};
    }
}

QString statusName(LspManager::Status s, const QString &path)
{
    switch (s) {
    case LspManager::Status::Running: return QStringLiteral("running");
    case LspManager::Status::Starting: return QStringLiteral("starting");
    case LspManager::Status::NotFound: return QStringLiteral("not installed");
    case LspManager::Status::Crashed: return QStringLiteral("stopped (crashed)");
    default: return path.isEmpty() ? QStringLiteral("idle") : QStringLiteral("installed, not running (no open file needs it)");
    }
}
} // namespace

void MainWindow::wireIdeTools()
{
    m_ai->setIdeHandler([this](const QString &action, const QJsonObject &args) { return ideAction(action, args); });
}

QJsonObject MainWindow::ideAction(const QString &action, const QJsonObject &args)
{
    if (action == QLatin1String("state"))
        return ideState();
    if (action == QLatin1String("current_file")) {
        Document *doc = m_editors->currentDocument();
        return QJsonObject{{"path", doc ? doc->filePath() : QString()}};
    }
    if (action == QLatin1String("set_theme"))
        return ideSetTheme(args.value("theme").toString());
    if (action == QLatin1String("show_panel"))
        return ideShowPanel(args.value("panel").toString(), args.value("visible").toBool(true));
    if (action == QLatin1String("open_file"))
        return ideOpenFile(args.value("path").toString(), args.value("line").toInt(), args.value("where").toString());
    if (action == QLatin1String("set_option"))
        return ideSetOption(args);
    if (action == QLatin1String("editor_command"))
        return ideEditorCommand(args.value("command").toString(), args.value("path").toString(), args.value("line").toInt());
    if (action == QLatin1String("check_servers"))
        return ideCheckServers(args.value("path").toString());
    if (action == QLatin1String("install_server"))
        return ideInstallServer(args.value("server").toString());
    if (action == QLatin1String("http_send"))
        return ideHttpSend(args.value("path").toString(), args.value("request").toString(), args.value("line").toInt(), args.value("environment").toString());
    if (action == QLatin1String("http_poll"))
        return ideHttpPoll();
    if (action == QLatin1String("http_stop")) {
        ideHttpStop();
        return {};
    }
    if (action == QLatin1String("debug_breakpoints"))
        return ideBreakpoints(args.value("action").toString(), args.value("path").toString(), args.value("line").toInt());
    if (action == QLatin1String("debug_start"))
        return ideDebugStart(args.value("path").toString());
    if (action == QLatin1String("debug_state"))
        return ideDebugState(args.value("frame").toInt(), args.value("expand").toString().toULongLong(), args.value("brief").toBool());
    if (action == QLatin1String("debug_control"))
        return ideDebugControl(args.value("what").toString());
    if (action == QLatin1String("debug_evaluate"))
        return ideDebugEvaluate(args.value("expression").toString(), args.value("frame").toInt());
    if (action == QLatin1String("terminal_output"))
        return ideTerminalOutput(args.value("tab").toInt(), args.value("lines").toInt(60));
    if (action == QLatin1String("tool_groups"))
        return ideToolGroups();
    if (action.startsWith(QLatin1String("preview_")))
        return idePreviewAction(action, args);
    if (action == QLatin1String("server_status"))
        return ideServerStatus(args.value("server").toString());
    if (action == QLatin1String("server_control"))
        return ideServerControl(args.value("server").toString(), args.value("what").toString());
    if (action == QLatin1String("server_log"))
        return ideServerLog(args.value("server").toString(), args.value("lines").toInt(80), args.value("grep").toString());
    return error(QStringLiteral("unknown IDE action '%1'").arg(action));
}

// The terminal's recent output (the current pane, or the first pane of tab `tab`, 1-based).
QJsonObject MainWindow::ideTerminalOutput(int tab, int lines)
{
    const QVector<TerminalPanel::Pane> panes = m_terminal->panes();
    if (panes.isEmpty() || !m_terminal->isRunning())
        return error(QStringLiteral("the terminal is not open; nothing has run in it"));
    QStringList tabs;
    const TerminalPanel::Pane *pick = nullptr;
    for (const TerminalPanel::Pane &p : panes) {
        if (!tabs.contains(p.tab))
            tabs << p.tab;
        if (!pick && (tab > 0 ? tabs.size() == tab && tabs.last() == p.tab : p.current))
            pick = &p;
    }
    if (!pick)
        return error(QStringLiteral("no terminal tab %1; tabs: %2").arg(tab).arg(tabs.join(QStringLiteral(", "))));
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    QString cwd = pick->terminal->currentDirectory();
    if (!root.isEmpty() && (cwd == root || cwd.startsWith(root + QLatin1Char('/'))))
        cwd = cwd == root ? QStringLiteral(".") : cwd.mid(root.size() + 1);
    QJsonObject o{{"tab", pick->tab}, {"folder", cwd}, {"command_running", pick->terminal->commandRunning()},
                  {"output", pick->terminal->recentOutput(qBound(1, lines, 300))}};
    if (tabs.size() > 1)
        o.insert("tabs", QJsonArray::fromStringList(tabs));
    return o;
}

// The chat's tool groups this project offers (AgentTools::allGroups): the preview tools where the web preview has
// something to show, the server tools where QODE runs a server, HTTP requests where there are .http files or a
// Spring Boot / Ktor server, the debugger in Java / Kotlin projects. Core tools are always there.
QJsonObject MainWindow::ideToolGroups() const
{
    const bool web = m_serverBar && m_serverBar->isAvailable();
    const bool expo = m_expoBar && m_expoBar->isAvailable();
    const bool spring = m_springBar && m_springBar->isAvailable();
    const bool ktor = m_ktorBar && m_ktorBar->isAvailable();
    bool httpFiles = false, jvmFiles = false;
    for (const QString &f : m_projectFiles->files()) {
        httpFiles = httpFiles || f.endsWith(QLatin1String(".http")) || f.endsWith(QLatin1String(".rest"));
        jvmFiles = jvmFiles || f.endsWith(QLatin1String(".java")) || f.endsWith(QLatin1String(".kt"));
        if (httpFiles && jvmFiles)
            break;
    }
    QJsonArray groups;
#ifdef QODE_HAS_WEBENGINE
    if (web || expo)
        groups.append(QStringLiteral("preview"));
#endif
    if (web || expo || spring || ktor)
        groups.append(QStringLiteral("servers"));
    if (httpFiles || spring || ktor)
        groups.append(QStringLiteral("http"));
    if (jvmFiles || (m_projects->hasProject() && JvmTools::isJvmBuild(m_projects->root())))
        groups.append(QStringLiteral("jvm_debug"));
    return QJsonObject{{"groups", groups}};
}

QHash<QString, QAction *> MainWindow::ideOptionActions() const
{
    return {{"word_wrap", m_wordWrapAct},
            {"minimap", m_minimapAct},
            {"breadcrumbs", m_breadcrumbsAct},
            {"sticky_scroll", m_stickyAct},
            {"edit_predictions", m_predictAct},
            {"indent_guides", m_indentGuidesAct},
            {"inline_blame", m_blameInlineAct},
            {"blame_gutter", m_blameGutterAct},
            {"hidden_files", m_hiddenFilesAct},
            {"parameter_hints", m_hintParamsAct},
            {"type_hints", m_hintTypesAct},
            {"semantic_highlighting", m_semanticAct},
            {"symbol_highlight", m_symbolHighlightAct},
            {"light_bulb", m_lightBulbAct},
            {"format_on_save", m_formatOnSaveAct},
            {"organize_imports_on_save", m_organizeOnSaveAct},
            {"fix_all_on_save", m_fixOnSaveAct},
            {"trim_trailing_whitespace", m_trimAct},
            {"insert_final_newline", m_finalNewlineAct},
            {"auto_activate_venv", m_venvAct}};
}

QJsonObject MainWindow::ideState() const
{
    QJsonObject out;
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    if (Document *doc = m_editors->currentDocument()) {
        const QString p = doc->filePath();
        out.insert("current_file", !root.isEmpty() && p.startsWith(root + QLatin1Char('/')) ? p.mid(root.size() + 1) : p);
    }

    const QString active = SettingsManager::instance().theme();
    QJsonObject current{{"id", active}};
    QJsonArray themes;
    for (const ThemeManager::Info &info : ThemeManager::instance().themes()) {
        themes.append(QJsonObject{{"id", info.id}, {"name", info.name}, {"dark", info.dark}});
        if (info.id == active)
            current = QJsonObject{{"id", info.id}, {"name", info.name}, {"dark", info.dark}};
    }
    out.insert("theme", QJsonObject{{"current", current}, {"available", themes}});

    QJsonObject panels;
    const bool side = m_explorerAct->isChecked();
    panels.insert("sidebar", side);
    for (int i = 0; i < kSections.size(); ++i)
        panels.insert(kSections.at(i), side && m_side->isExpanded(i));
    panels.insert("terminal", m_terminal->isVisible());
    panels.insert("editor", m_editorColumn->isVisible());
    QString right = QStringLiteral("hidden");
    if (m_rightStack->parentWidget()->isVisible()) {
        QWidget *w = m_rightStack->currentWidget();
        right = w == m_md ? QStringLiteral("markdown preview")
              : w == m_gradle ? QStringLiteral("gradle")
              : w == m_maven ? QStringLiteral("maven")
              : w == m_spring ? QStringLiteral("spring_boot")
              : w == m_ktor ? QStringLiteral("ktor")
              : w == m_http ? QStringLiteral("http_client")
              : w == m_webPreview ? QStringLiteral("web_preview")
              : w == m_debugPanel ? QStringLiteral("debugger")
                              : QStringLiteral("image / video / svg preview");
    }
    panels.insert("right_panel", right);
    panels.insert("gradle_available", m_gradleAct->isVisible());
    panels.insert("maven_available", m_mavenAct->isVisible());
    panels.insert("spring_boot_available", m_springAct->isVisible());
    panels.insert("ktor_available", m_ktorAct->isVisible());
#ifdef QODE_HAS_WEBENGINE
    panels.insert("web_preview_available", m_serverBar->isAvailable());
#else
    panels.insert("web_preview_available", false);
#endif
    out.insert("panels", panels);

    QJsonObject options;
    const auto acts = ideOptionActions();
    for (auto it = acts.cbegin(); it != acts.cend(); ++it)
        options.insert(it.key(), it.value()->isChecked());
    options.insert("fullscreen", isFullScreen());
    options.insert("ui_zoom", SettingsManager::instance().uiZoom());
    out.insert("options", options);
    return out;
}

QJsonObject MainWindow::ideSetTheme(const QString &wanted)
{
    const QString w = wanted.trimmed();
    QStringList names;
    for (const ThemeManager::Info &info : ThemeManager::instance().themes()) {
        if (info.id.compare(w, Qt::CaseInsensitive) == 0 || info.name.compare(w, Qt::CaseInsensitive) == 0) {
            SettingsManager::instance().setTheme(info.id);
            return QJsonObject{{"theme", info.name}, {"dark", info.dark}, {"summary", QStringLiteral("now %1").arg(info.name)}};
        }
        names << QStringLiteral("%1 (%2)").arg(info.id, info.dark ? QStringLiteral("dark") : QStringLiteral("light"));
    }
    return error(QStringLiteral("no theme '%1'; available: %2").arg(w, names.join(QStringLiteral(", "))));
}

QJsonObject MainWindow::ideShowPanel(const QString &panel, bool visible)
{
    const QString done = visible ? QStringLiteral("shown") : QStringLiteral("hidden");
    if (panel == QLatin1String("sidebar")) {
        m_explorerAct->setChecked(visible);
        return QJsonObject{{"summary", done}};
    }
    if (const int section = kSections.indexOf(panel); section >= 0) {
        if (visible)
            m_explorerAct->setChecked(true);
        m_side->setExpanded(section, visible);
        return QJsonObject{{"summary", done}};
    }
    if (panel == QLatin1String("terminal")) {
        if (visible) {
            showTerminal();
            m_terminal->ensureStarted();
        } else if (m_terminal->isVisible()) {
            toggleTerminal();
        }
        return QJsonObject{{"summary", done}};
    }
    if (panel == QLatin1String("editor")) {
        if (!visible && !m_agentIsland->isVisible())
            return error(QStringLiteral("the editor is the only thing shown; it cannot be hidden"));
        m_editorHiddenByUser = !visible;
        setEditorColumnVisible(visible);
        return QJsonObject{{"summary", done}};
    }
    QWidget *island = m_rightStack->parentWidget();
    if (panel == QLatin1String("right_panel")) {
        if (!visible) {
            if (island->isVisible())
                hideMedia();
            return QJsonObject{{"summary", done}};
        }
        if (island->isVisible())
            return QJsonObject{{"summary", QStringLiteral("already shown")}};
        return error(QStringLiteral("the right panel needs something to show: use open_file with where=preview, or the gradle / maven / spring_boot / ktor panel"));
    }
    if (panel == QLatin1String("web_preview")) {
#ifndef QODE_HAS_WEBENGINE
        return error(QStringLiteral("this QODE was built without the web preview (Qt WebEngine)"));
#endif
        if (!m_serverBar->isAvailable())
            return error(QStringLiteral("this is not a web project with a dev server script"));
        const bool shown = rightPanelShows(m_webPreview);
        if (shown != visible)
            toggleWebPreview();
        return QJsonObject{{"summary", done}};
    }
    if (panel == QLatin1String("gradle") || panel == QLatin1String("spring_boot")) {
        const bool gradle = panel == QLatin1String("gradle");
        if (!(gradle ? m_gradleAct : m_springAct)->isVisible())
            return error(gradle ? QStringLiteral("this is not a Gradle project") : QStringLiteral("this is not a Spring Boot project"));
        const bool shown = rightPanelShows(gradle ? static_cast<QWidget *>(m_gradle) : static_cast<QWidget *>(m_spring));
        if (shown != visible)
            gradle ? toggleGradlePanel() : toggleSpringPanel();
        return QJsonObject{{"summary", done}};
    }
    if (panel == QLatin1String("maven")) {
        if (!m_mavenAct->isVisible())
            return error(QStringLiteral("this is not a Maven project"));
        const bool shown = rightPanelShows(m_maven);
        if (shown != visible)
            toggleMavenPanel();
        return QJsonObject{{"summary", done}};
    }
    if (panel == QLatin1String("ktor")) {
        if (!m_ktorAct->isVisible())
            return error(QStringLiteral("this is not a Ktor server project"));
        const bool shown = rightPanelShows(m_ktor);
        if (shown != visible)
            toggleKtorPanel();
        return QJsonObject{{"summary", done}};
    }
    return error(QStringLiteral("unknown panel '%1'").arg(panel));
}

QJsonObject MainWindow::ideOpenFile(const QString &path, int line, const QString &where)
{
    if (MediaPanel::isMedia(path)) {
        showMedia(path);
        return QJsonObject{{"summary", QStringLiteral("shown in the right panel")}};
    }
    m_editorHiddenByUser = false;
    if (!(line > 0 ? m_editors->openFileAt(path, line) : m_editors->openFile(path)))
        return error(QStringLiteral("the file could not be opened"));
    setEditorColumnVisible(true);
    if (where != QLatin1String("preview"))
        return QJsonObject{{"summary", QStringLiteral("opened in the editor")}};
    Document *doc = m_editors->currentDocument();
    if (!EditorManager::isPreviewable(doc))
        return error(QStringLiteral("only images, videos, Markdown and SVG can be previewed; the file was opened in the editor instead"));
    if (!(m_rightStack->parentWidget()->isVisible() && m_previewDoc == doc))
        togglePreview();
    return QJsonObject{{"summary", QStringLiteral("previewed in the right panel")}};
}

QJsonObject MainWindow::ideSetOption(const QJsonObject &args)
{
    const QString option = args.value("option").toString();
    if (option == QLatin1String("ui_zoom")) {
        if (!args.contains("percent"))
            return error(QStringLiteral("pass percent (50-400; 100 is normal)"));
        auto &s = SettingsManager::instance();
        s.setUiZoom(args.value("percent").toInt());
        return QJsonObject{{"ui_zoom", s.uiZoom()}, {"summary", QStringLiteral("zoom %1%").arg(s.uiZoom())}};
    }
    if (!args.contains("enabled"))
        return error(QStringLiteral("pass enabled (true or false)"));
    const bool on = args.value("enabled").toBool();
    const QString state = on ? QStringLiteral("on") : QStringLiteral("off");
    if (option == QLatin1String("fullscreen")) {
        if (on != isFullScreen())
            m_fullscreenAct->trigger();
        return QJsonObject{{"summary", state}};
    }
    QAction *act = ideOptionActions().value(option);
    if (!act)
        return error(QStringLiteral("unknown option '%1'").arg(option));
    const bool was = act->isChecked();
    act->setChecked(on); // the action's toggled() stores the setting, as the menu does
    return QJsonObject{{"summary", was == on ? QStringLiteral("was already %1").arg(state) : state}};
}

QJsonObject MainWindow::ideEditorCommand(const QString &command, const QString &path, int line)
{
    m_editorHiddenByUser = false;
    if (!m_editors->openFile(path))
        return error(QStringLiteral("the file could not be opened"));
    setEditorColumnVisible(true);
    QPointer<Document> doc = m_editors->documentForPath(path);
    CodeEditor *ed = doc ? m_editors->editorFor(doc) : nullptr;
    if (!ed)
        return error(QStringLiteral("the file is not shown in an editor"));

    if (command.endsWith(QLatin1String("fold_all"))) {
        command == QLatin1String("fold_all") ? ed->foldAll() : ed->unfoldAll();
        return QJsonObject{{"summary", QStringLiteral("done")}};
    }
    if (command == QLatin1String("fold") || command == QLatin1String("unfold")) {
        const QTextBlock b = ed->document()->findBlockByNumber(qMax(1, line) - 1);
        if (!b.isValid())
            return error(QStringLiteral("line %1 is past the end of the file").arg(line));
        QTextCursor c(b);
        ed->setTextCursor(c);
        command == QLatin1String("fold") ? ed->foldCurrent() : ed->unfoldCurrent();
        return QJsonObject{{"summary", QStringLiteral("done")}};
    }
    if (command == QLatin1String("format")) {
        QString note;
        const auto c = connect(m_editors, &EditorManager::statusMessage, this, [&note](const QString &t) { note = t; });
        const bool ok = m_editors->formatCurrent();
        disconnect(c);
        if (!ok)
            return error(note.isEmpty() ? QStringLiteral("no formatter is available for this file") : note);
        return QJsonObject{{"result", note}, {"summary", note.isEmpty() ? QStringLiteral("formatted") : note}};
    }

    static const QHash<QString, QString> kinds{{"organize_imports", "source.organizeImports"},
                                               {"remove_unused_imports", "source.removeUnusedImports"},
                                               {"add_missing_imports", "source.addMissingImports"},
                                               {"fix_all", "source.fixAll"}};
    const QString kind = kinds.value(command);
    if (kind.isEmpty())
        return error(QStringLiteral("unknown command '%1'").arg(command));
    const LspServerSpec *spec = LspServers::forFile(path);
    if (!spec)
        return error(QStringLiteral("no language server handles this kind of file"));
    // A file opened just now: give its server a few seconds to start.
    QElapsedTimer clock;
    clock.start();
    while (doc && !m_lsp->isServed(doc) && clock.elapsed() < 8000) {
        bool missing = false;
        for (const LspManager::ServerState &st : m_lsp->servers(false))
            if (st.id == spec->id && st.status == LspManager::Status::NotFound)
                missing = true;
        if (missing)
            break;
        QEventLoop wait;
        QTimer::singleShot(200, &wait, &QEventLoop::quit);
        wait.exec();
    }
    if (!doc)
        return error(QStringLiteral("the file was closed"));
    if (!m_lsp->isServed(doc))
        return error(QStringLiteral("the %1 language server is not running for this file; use check_language_servers").arg(spec->displayName));
    const int applied = m_lsp->applySourceActionsBlocking(doc, {kind}, 8000);
    return QJsonObject{{"applied", applied}, {"summary", applied > 0 ? QStringLiteral("done") : QStringLiteral("the language server had nothing to do")}};
}

QJsonObject MainWindow::ideCheckServers(const QString &path)
{
    QList<const LspServerSpec *> specs;
    QJsonObject out{{"distribution", distribution()}};
    if (!path.isEmpty()) {
        if (const LspServerSpec *main = LspServers::forFile(path))
            specs << main;
        specs << LspServers::companionsFor(path, m_projects->hasProject() ? m_projects->root() : QString());
        if (specs.isEmpty()) {
            out.insert("note", QStringLiteral("QODE knows no language server for .%1 files").arg(QFileInfo(path).suffix()));
            return out;
        }
    } else {
        for (const LspServerSpec &s : LspServers::all())
            specs << &s;
    }
    const QList<LspManager::ServerState> states = m_lsp->servers();
    QSet<QString> helpGiven; // the web servers share one install and one text
    QJsonArray rows;
    for (const LspServerSpec *spec : std::as_const(specs)) {
        LspManager::ServerState st;
        for (const LspManager::ServerState &s : states)
            if (s.id == spec->id)
                st = s;
        const bool installed = st.status != LspManager::Status::NotFound;
        QJsonObject row{{"id", spec->id}, {"name", spec->displayName}, {"status", statusName(st.status, st.path)},
                        {"installed", installed}, {"qode_can_install", spec->installable}};
        if (spec->companion)
            row.insert("companion", QStringLiteral("runs next to the file's main server"));
        if (!st.path.isEmpty())
            row.insert("executable", st.path);
        if (!st.detail.isEmpty() && st.status != LspManager::Status::Running)
            row.insert("detail", st.detail);
        const QString missing = spec->installable ? missingPrerequisites(*spec) : QString();
        if (!missing.isEmpty())
            row.insert("missing_prerequisites", missing);
        const QString key = spec->managedId.isEmpty() ? spec->id : spec->managedId;
        if ((!installed || !missing.isEmpty()) && !helpGiven.contains(key)) {
            row.insert("install_help", spec->installHelp);
            helpGiven.insert(key);
        }
        rows.append(row);
    }
    out.insert("servers", rows);
    return out;
}

QJsonObject MainWindow::ideInstallServer(const QString &id)
{
    const LspServerSpec *spec = LspServers::byId(id.trimmed());
    if (!spec)
        return error(QStringLiteral("no language server '%1'; take the id from check_language_servers").arg(id));
    if (!spec->installable)
        return error(QStringLiteral("QODE cannot set up %1 itself; give the user the install command for their distribution "
                                    "(check_language_servers has it) to run in a terminal")
                         .arg(spec->displayName));
    const QString missing = missingPrerequisites(*spec);
    if (!missing.isEmpty())
        return error(QStringLiteral("%1 must be installed first (it needs sudo): give the user the command for their distribution")
                         .arg(missing));
    if (!installLspServer(spec->id))
        return error(QStringLiteral("the user closed the installer without setting the server up; do not retry"));
    return QJsonObject{{"installed", spec->displayName}, {"summary", QStringLiteral("%1 set up").arg(spec->displayName)}};
}
