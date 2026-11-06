// MainWindow's language server side: the LSP menu and status, installing servers, Go to Definition, code actions,
// workspace edits, rename, references and colour swatches.
#include "MainWindow.h"
#include "platform/AppPaths.h"

#include "dialogs/CompilerFlagsDialog.h"
#include "dialogs/IncludePathsDialog.h"
#include "dialogs/JdtlsInstallDialog.h"
#include "dialogs/LspInstallDialog.h"
#include "dialogs/LspLogDialog.h"
#include "dialogs/LspRemoveDialog.h"
#include "dialogs/NpmInstallDialog.h"
#include "dialogs/PackagesDialog.h"
#include "dialogs/PipInstallDialog.h"
#include "dialogs/ReferencesDialog.h"
#include "dialogs/UnusedImportsDialog.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/ProjectExplorer.h"
#include "lsp/JarSource.h"
#include "lsp/LspManager.h"
#include "lsp/LspServers.h"
#include "project/ProjectManager.h"
#include "project/PythonEnv.h"
#include "python/PythonTools.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "terminal/TerminalPanel.h"

#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QWidgetAction>

#include <algorithm>

void MainWindow::createLspMenu(QMenu *menu)
{
    m_lspMenu = menu;
    connect(menu, &QMenu::aboutToShow, this, &MainWindow::rebuildLspMenu);
}

namespace {

// A non-clickable line inside a menu (headers, status text), styled with the theme.
void addMenuLabel(QMenu *menu, const QString &html)
{
    auto *label = new QLabel(html);
    label->setTextFormat(Qt::RichText);
    label->setStyleSheet(QStringLiteral("background: transparent; padding: 4px 14px;"));
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(label);
    menu->addAction(action);
}

// "SET UP", "REMOVE", ...: a small spaced-out caption that starts a group of items.
void addMenuSection(QMenu *menu, const QString &title, const Theme &theme, bool separator = true)
{
    if (separator)
        menu->addSeparator();
    auto *label = new QLabel(title.toUpper());
    QFont f = label->font();
    f.setBold(true);
    f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    label->setFont(f);
    label->setStyleSheet(QStringLiteral("background: transparent; color: %1; padding: 6px 14px 2px 14px;").arg(theme.textMuted.name()));
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(label);
    menu->addAction(action);
}

QIcon dotIcon(const QColor &color)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(4, 4, 8, 8));
    return QIcon(pm);
}

} // namespace

void MainWindow::rebuildLspMenu()
{
    // Submenus are children of the menu and are not removed by clear().
    const QList<QMenu *> old = m_lspMenu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly);
    qDeleteAll(old);
    m_lspMenu->clear();
    m_lspMenu->setToolTipsVisible(true);
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    using Status = LspManager::Status;

    addMenuSection(m_lspMenu, tr("Language servers"), theme, false);
    for (const LspManager::ServerState &st : m_lsp->servers()) {
        const QString id = st.id;
        QString state;
        QColor color = theme.textMuted;
        switch (st.status) {
        case Status::Running: state = tr("Running"); color = theme.gitAdded; break;
        case Status::Starting: state = tr("Starting…"); color = theme.gitModified; break;
        case Status::NotFound: state = tr("Not installed"); break;
        case Status::Crashed: state = tr("Stopped"); color = theme.gitConflict; break;
        default: state = st.path.isEmpty() ? tr("Idle") : tr("Ready"); color = theme.accent; break;
        }
        QMenu *sub = m_lspMenu->addMenu(dotIcon(color), QStringLiteral("%1    %2").arg(st.name, state));
        sub->setToolTipsVisible(true);

        // Status block
        QString head = QStringLiteral("<span style='font-size:11pt'><b>%1</b></span><br><span style='color:%2'>&#9679; %3</span>")
                           .arg(st.name.toHtmlEscaped(), color.name(), state);
        const QString where = !st.detail.isEmpty() && st.status != Status::Running ? st.detail : st.path;
        if (!where.isEmpty())
            head += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(theme.textMuted.name(), where.toHtmlEscaped());
        for (const QString &op : st.operations) // what it is busy with (import, indexing …), newest first
            head += QStringLiteral("<br><span style='color:%1'>⟳ %2</span>").arg(theme.accent.name(), op.toHtmlEscaped());
        addMenuLabel(sub, head);

        const LspServerSpec *spec = LspServers::byId(id);
        const bool managed = spec && LspServers::isManaged(*spec, st.path);
        const bool installed = st.status != Status::NotFound;

        addMenuSection(sub, tr("Set up"), theme);
        if (st.installable && !installed)
            connect(sub->addAction(tr("Download and Set Up…")), &QAction::triggered, this, [this, id] { installLspServer(id); });
        else if (managed)
            connect(sub->addAction(tr("Update / Reinstall…")), &QAction::triggered, this, [this, id] { installLspServer(id); });
        if (!installed)
            connect(sub->addAction(tr("How to Install…")), &QAction::triggered, this, [this, id] { showLspInstallHelp(id); });
        connect(sub->addAction(tr("Set Server Path…")), &QAction::triggered, this, [this, id] { chooseLspServerPath(id); });
        if (!m_lsp->serverPath(id).isEmpty())
            connect(sub->addAction(tr("Auto-detect Server on PATH")), &QAction::triggered, this, [this, id] { m_lsp->setServerPath(id, {}); });

        if (installed) {
            addMenuSection(sub, tr("Server"), theme);
            if (id == QLatin1String("java") || id == QLatin1String("kotlin")) {
                QAction *sync = sub->addAction(tr("Sync Build Files (Gradle / Maven)"));
                sync->setEnabled(st.status == Status::Running && m_lsp->hasBuildFiles());
                sync->setToolTip(tr("Make the server re-read build.gradle(.kts), settings.gradle, libs.versions.toml and pom.xml"));
                connect(sync, &QAction::triggered, this, &MainWindow::syncBuildFiles);
            }
            connect(sub->addAction(tr("Restart Server")), &QAction::triggered, this, [this, id] { m_lsp->restart(id); });
            // A stopped server keeps the log of the process that ended: that is where it says why.
            if (st.status == Status::Running || st.status == Status::Starting || !m_lsp->logOf(id).isEmpty())
                connect(sub->addAction(tr("Show Server Log…")), &QAction::triggered, this, [this, id] { showLspLog(id); });
        }

        addMenuSection(sub, tr("Remove"), theme);
        QAction *rm = sub->addAction(tr("Remove Language Server…"));
        rm->setEnabled(installed);
        rm->setToolTip(installed ? tr("Shows what is deleted or which commands to run") : tr("Not installed"));
        connect(rm, &QAction::triggered, this, [this, id] { removeLspServer(id); });
    }

    addMenuSection(m_lspMenu, tr("Project"), theme);
    {
        const bool has = m_lsp->hasBuildFiles();
        const bool syncing = [&] {
            for (const LspManager::ServerState &st : m_lsp->servers())
                if (st.syncing)
                    return true;
            return false;
        }();
        QAction *sync = m_lspMenu->addAction(syncing ? tr("⟳  Syncing Build Files…") : tr("⟳  Sync Build Files (Gradle / Maven)"));
        sync->setEnabled(has && !syncing);
        sync->setToolTip(!has ? tr("No Gradle or Maven build file in this project")
                         : !m_lsp->canSyncBuildFiles() ? tr("Open a Java or Kotlin file first so its language server starts")
                                                       : tr("Re-import the project after editing build.gradle(.kts), settings.gradle, libs.versions.toml or pom.xml"));
        connect(sync, &QAction::triggered, this, &MainWindow::syncBuildFiles);
    }
    QString detectedFrom;
    m_lsp->detectedFlags(&detectedFrom);
    QAction *flags = m_lspMenu->addAction(m_lsp->hasCompileDatabase() ? tr("Compiler Flags…")
                                          : !detectedFrom.isEmpty()   ? tr("Compiler Flags… (detected from %1)").arg(detectedFrom)
                                                                      : tr("Compiler Flags… (no compile_commands.json)"));
    flags->setEnabled(m_projects->hasProject());
    flags->setToolTip(m_projects->hasProject() ? tr("Flags used for C/C++ when the project has no compile_commands.json")
                                               : tr("Open a project first"));
    connect(flags, &QAction::triggered, this, &MainWindow::editCompilerFlags);
    QAction *includes = m_lspMenu->addAction(tr("Include Paths…"));
    includes->setEnabled(m_projects->hasProject());
    includes->setToolTip(m_projects->hasProject() ? tr("Extra folders searched for #include in this project's C/C++ files")
                                                  : tr("Open a project first"));
    connect(includes, &QAction::triggered, this, &MainWindow::editIncludePaths);

    addMenuSection(m_lspMenu, tr("Diagnostics"), theme);
    addMenuLabel(m_lspMenu, tr("<span style='color:%1'>%2 errors, %3 warnings in open files</span>")
                                .arg(theme.textMuted.name())
                                .arg(m_lsp->diagnosticCount(LspDiagnostic::Error))
                                .arg(m_lsp->diagnosticCount(LspDiagnostic::Warning)));
}

// LSP > Sync Build Files: the Java / Kotlin servers re-import the Gradle / Maven project.
void MainWindow::syncBuildFiles()
{
    if (!m_lsp->hasBuildFiles()) {
        statusBar()->showMessage(tr("No Gradle or Maven build file found in this project"), 5000);
        return;
    }
    if (!m_editors->saveAll())
        return;
    if (!m_lsp->syncBuildFiles(true)) {
        statusBar()->showMessage(tr("Open a Java or Kotlin file first — the language server starts with it and reads the build files itself"), 7000);
        return;
    }
    statusBar()->showMessage(tr("Syncing build files…"), 5000);
}

void MainWindow::updateLspStatus()
{
    using Status = LspManager::Status;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QString text = tr("LSP");
    QColor color = t.textMuted;
    QString tip = tr("Language servers");
    auto elide = [](const QString &s) { return s.size() > 40 ? s.left(39) + QStringLiteral("…") : s; };
    // The server of the file being edited first, then the others that serve an open file.
    const QString current = m_editors->currentDocument() ? m_lsp->serverIdFor(m_editors->currentDocument()) : QString();
    QList<LspManager::ServerState> order;
    for (const LspManager::ServerState &st : m_lsp->servers(false)) {
        if (st.documents == 0 || st.companion)
            continue; // nothing open that this server handles (companions report through the diagnostics counts)
        if (st.id == current)
            order.prepend(st);
        else
            order.append(st);
    }
    int percent = -2; // -2: no progress bar, -1: busy without a percentage
    if (!order.isEmpty()) {
        const LspManager::ServerState &st = order.first();
        const QString name = st.name.section(QLatin1Char(' '), 0, 0);
        switch (st.status) {
        case Status::NotFound:
            text = tr("%1 not found").arg(name);
            color = t.gitModified;
            tip = tr("%1 is not installed. Click for instructions.").arg(name);
            if (!m_lspHintShown) {
                m_lspHintShown = true;
                statusBar()->showMessage(st.installable ? tr("%1 not found — choose LSP > Download and Set Up to install it").arg(name)
                                                        : tr("%1 not found — open the LSP menu to see how to install it").arg(name),
                                         8000);
            }
            break;
        case Status::Starting:
            text = tr("%1 starting…").arg(name);
            tip = tr("%1 is starting").arg(name);
            percent = -1;
            if (!st.progress.isEmpty()) {
                text += QStringLiteral("  ⟳ ") + elide(st.progress);
                percent = st.percent;
            }
            break;
        case Status::Crashed:
            text = tr("%1 stopped").arg(name);
            color = t.gitConflict;
            tip = st.detail + tr("\nThe LSP menu can restart it and show its log");
            break;
        case Status::Running: {
            text = name;
            const int e = m_lsp->diagnosticCount(LspDiagnostic::Error), w = m_lsp->diagnosticCount(LspDiagnostic::Warning);
            if (e)
                text += QStringLiteral("  ✕ %1").arg(e);
            if (w)
                text += QStringLiteral("  ⚠ %1").arg(w);
            color = e ? t.gitConflict : (w ? t.gitModified : t.gitAdded);
            tip = tr("%1 — running").arg(st.detail.isEmpty() ? name : st.detail);
            if (st.syncing) {
                text += tr("  ⟳ Syncing build files…");
                tip += tr("\nSyncing the Gradle / Maven project — completion and errors may be incomplete until it ends");
                percent = st.progress.isEmpty() ? -1 : st.percent;
            } else if (!st.progress.isEmpty()) { // e.g. a Gradle import: the server is up but still working
                text += QStringLiteral("  ⟳ ") + elide(st.progress);
                percent = st.percent;
            }
            break;
        }
        default:
            break;
        }
    }
    // Every running operation of every server in the tooltip, the shown server's first.
    QStringList ops;
    for (const LspManager::ServerState &st : std::as_const(order))
        for (const QString &op : st.operations)
            ops << QStringLiteral("%1: %2").arg(st.name.section(QLatin1Char(' '), 0, 0), op);
    if (!ops.isEmpty())
        tip += QStringLiteral("\n\n") + ops.join(QLatin1Char('\n'));
    m_lspButton->setText(text);
    m_lspButton->setToolTip(tip);
    if (m_lspButtonColor != color) { // a style sheet change re-polishes the widget: only when the colour really changed
        m_lspButtonColor = color;
        m_lspButton->setStyleSheet(QStringLiteral("QToolButton { padding: 1px 6px; color: %1; } QToolButton::menu-indicator { image: none; }")
                                       .arg(color.name()));
    }
    // A thin bar beside the button while the server works: filled to its percentage, or moving when it gives none.
    if (percent == -2) {
        m_lspProgress->hide();
        return;
    }
    QColor track = t.textMuted;
    track.setAlpha(70);
    const QString style = QStringLiteral("QProgressBar { border: none; border-radius: 3px; background: %1; }"
                                         "QProgressBar::chunk { border-radius: 3px; background: %2; }")
                              .arg(track.name(QColor::HexArgb), t.accent.name());
    if (m_lspProgress->styleSheet() != style)
        m_lspProgress->setStyleSheet(style);
    if (percent < 0) {
        m_lspProgress->setRange(0, 0);
    } else {
        m_lspProgress->setRange(0, 100);
        m_lspProgress->setValue(percent);
    }
    m_lspProgress->setToolTip(tip);
    m_lspProgress->show();
}

// Jumps to where the symbol at (line, column) of `editor` is defined; several candidates are offered in a menu.
void MainWindow::goToDefinition(CodeEditor *editor, int line, int column, LspManager::Navigate what)
{
    Document *doc = nullptr;
    for (Document *d : m_editors->documents())
        if (m_editors->editorFor(d) == editor)
            doc = d;
    if (!doc || !m_lsp->isServed(doc)) {
        statusBar()->showMessage(what == LspManager::Navigate::TypeDefinition ? tr("Go to Type Definition needs a running language server for this file")
                                 : what == LspManager::Navigate::Implementation ? tr("Go to Implementation needs a running language server for this file")
                                                                                : tr("Go to Definition needs a running language server for this file"),
                                 4000);
        return;
    }
    if (what != LspManager::Navigate::Definition && !m_lsp->supportsNavigation(doc, what)) {
        statusBar()->showMessage(what == LspManager::Navigate::TypeDefinition ? tr("This language server cannot find type definitions")
                                                                               : tr("This language server cannot find implementations"),
                                 4000);
        return;
    }
    // The name under the caret: when a library only offers compiled classes, the source file is searched for it.
    QTextCursor at(editor->document()->findBlockByNumber(line));
    at.setPosition(at.position() + column);
    at.select(QTextCursor::WordUnderCursor);
    const QString word = at.selectedText();
    QPointer<CodeEditor> guard(editor);
    m_lsp->definition(doc, line, column, [this, guard, word, line, column, what](const QVector<LspLocation> &locations) {
        if (locations.isEmpty()) {
            const QString why = JarSource::lastFailure();
            if (why.isEmpty()) {
                statusBar()->showMessage(what == LspManager::Navigate::TypeDefinition ? tr("No type definition found")
                                         : what == LspManager::Navigate::Implementation ? tr("No implementations found")
                                                                                        : tr("No definition found"),
                                         3000);
                return;
            }
            statusBar()->showMessage(why, 6000);
            const JarSource::Coordinates c = JarSource::coordinatesOf(JarSource::lastMissingSourcesJar());
            if (guard && c.isValid() && !m_projects->project().root.isEmpty() &&
                QMessageBox::question(this, tr("Library sources"),
                                      tr("%1 has no sources jar on this computer.\n\nDownload %2 sources with Gradle? Nothing is added to your project.").arg(why, c.text())) ==
                    QMessageBox::Yes)
                fetchLibrarySources(c, guard, line, column);
            return;
        }
        auto open = [this, word](const LspLocation &loc) {
            int target = loc.line;
            int col = loc.column;
            if (loc.fromClass) {
                // Only the class was known: jump to where the name under the caret is declared, else the top.
                target = 0;
                col = 0;
                QFile f(loc.path);
                if (!word.isEmpty() && f.open(QIODevice::ReadOnly)) {
                    const QRegularExpression re(QStringLiteral("\\b(?:class|object|interface|fun|val|var|typealias|enum class|data class|sealed class)\\s+(?:<[^>]*>\\s*)?(?:[\\w.<>?, ]+\\.)?(") +
                                                QRegularExpression::escape(word) + QStringLiteral(")\\b"));
                    int n = 0;
                    while (!f.atEnd()) {
                        const QString text = QString::fromUtf8(f.readLine());
                        const QRegularExpressionMatch m = re.match(text);
                        if (m.hasMatch()) {
                            target = n;
                            col = int(m.capturedStart(1));
                            break;
                        }
                        ++n;
                    }
                }
            }
            m_editors->openFileAt(loc.path, target + 1, col + 1);
        };
        if (locations.size() == 1 || !guard) {
            open(locations.first());
            return;
        }
        QMenu menu;
        const QString root = m_projects->project().root;
        for (const LspLocation &loc : locations) {
            const QString shown = root.isEmpty() ? loc.path : QDir(root).relativeFilePath(loc.path);
            connect(menu.addAction(QStringLiteral("%1:%2").arg(shown).arg(loc.line + 1)), &QAction::triggered, this,
                    [open, loc] { open(loc); });
        }
        menu.exec(QCursor::pos());
    }, what);
}

// Downloads <coords>:sources with the project's own Gradle (its repositories), through an init script kept in QODE's
// cache so the project stays untouched, then repeats the Go to Definition that needed it.
void MainWindow::fetchLibrarySources(const JarSource::Coordinates &coords, CodeEditor *editor, int line, int column)
{
    const QString root = m_projects->project().root;
    const QString wrapper = root + QStringLiteral("/gradlew");
    const QString program = QFileInfo(wrapper).isExecutable() ? wrapper : QStandardPaths::findExecutable(QStringLiteral("gradle"));
    if (program.isEmpty()) {
        statusBar()->showMessage(tr("Gradle was not found (no gradlew in the project and no gradle on PATH)"), 6000);
        return;
    }
    const QString dir = AppPaths::cache(QStringLiteral("run"));
    QDir().mkpath(dir);
    const QString script = dir + QStringLiteral("/library-sources.init.gradle");
    QFile f(script);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("// Written by QODE: downloads the sources jar of one library into Gradle's cache.\n"
                "rootProject { p ->\n"
                "    p.tasks.register('qodeLibrarySources') {\n"
                "        doLast {\n"
                "            def dep = p.dependencies.create(p.property('qode.sources') + ':sources@jar')\n"
                "            dep.transitive = false\n"
                "            p.configurations.detachedConfiguration(dep).resolve()\n"
                "        }\n"
                "    }\n"
                "}\n");
        f.close();
    }
    statusBar()->showMessage(tr("Downloading sources of %1…").arg(coords.text()));
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(root);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QPointer<CodeEditor> guard(editor);
    connect(proc, &QProcess::finished, this, [this, proc, coords, guard, line, column](int code, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(proc->readAll());
        proc->deleteLater();
        if (code != 0) {
            const QStringList lines = out.trimmed().split(QLatin1Char('\n'));
            statusBar()->showMessage(tr("Could not download the sources of %1 — %2").arg(coords.text(), lines.isEmpty() ? QString() : lines.last().trimmed()), 10000);
            return;
        }
        statusBar()->showMessage(tr("Sources of %1 downloaded").arg(coords.text()), 4000);
        if (guard)
            goToDefinition(guard, line, column);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            statusBar()->showMessage(tr("Could not start Gradle"), 6000);
            proc->deleteLater();
        }
    });
    proc->start(program, {QStringLiteral("-q"), QStringLiteral("-I"), script, QStringLiteral("-Pqode.sources=") + coords.text(), QStringLiteral("qodeLibrarySources")});
}

// Hover link on a gray import: let the user choose which unused imports go.
void MainWindow::removeUnusedImports(CodeEditor *editor)
{
    const QVector<int> lines = editor->unusedImportLines();
    if (lines.isEmpty())
        return;
    QStringList texts;
    for (const int n : lines)
        texts << editor->document()->findBlockByNumber(n).text();
    QString name = tr("this file");
    for (Document *d : m_editors->documents())
        if (m_editors->editorFor(d) == editor)
            name = d->fileName();
    UnusedImportsDialog dlg(name, lines, texts, this);
    if (dlg.exec() == QDialog::Accepted)
        editor->removeLines(dlg.selectedLines());
}

// Alt+Enter: the server's quick fixes ("Import → java.io.File") and refactorings for the caret / selection.
void MainWindow::showCodeActions(Document *doc, CodeEditor *editor, int startLine, int startColumn, int endLine, int endColumn)
{
    // QODE's own fixes lead the menu (they need no server): Generate Definition on a C/C++ declaration, the package of
    // a Kotlin / Java file that does not match its folder, a dependency for a Kotlin script.
    const QList<QPair<QString, std::function<void()>>> local = localCodeActions(doc, editor);
    const bool served = m_lsp->isServed(doc);
    if (!served && local.isEmpty()) {
        statusBar()->showMessage(tr("Quick fixes need a running language server for this file"), 4000);
        return;
    }
    QPointer<CodeEditor> guard(editor);
    auto show = [this, doc, guard, local, startLine, startColumn, endLine, endColumn](const QVector<LspCodeAction> &found) {
        if (!guard)
            return;
        QVector<LspCodeAction> actions = found;
        // An import that no installed package satisfies: offer to install it into the project's virtual environment.
        const QStringList missing = unresolvedPythonImports(doc, startLine, startColumn, endLine, endColumn);
        // Quick fixes first, then refactorings, then source actions (organize imports ...); the server's preferred fix leads its group.
        auto group = [](const LspCodeAction &a) { return a.kind.startsWith(QLatin1String("refactor")) ? 1 : a.kind.startsWith(QLatin1String("source")) ? 2 : 0; };
        auto preferred = [](const LspCodeAction &a) { return a.json.value(QStringLiteral("isPreferred")).toBool(); };
        std::stable_sort(actions.begin(), actions.end(), [&](const LspCodeAction &a, const LspCodeAction &b) {
            if (group(a) != group(b))
                return group(a) < group(b);
            return preferred(a) && !preferred(b);
        });
        if (actions.isEmpty() && missing.isEmpty() && local.isEmpty()) {
            statusBar()->showMessage(tr("No quick fixes or refactorings available here"), 3000);
            return;
        }
        auto *menu = new QMenu(guard);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        // Rounded and bordered like the tooltips: a translucent window lets the stylesheet's radius show.
        menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        menu->setAttribute(Qt::WA_TranslucentBackground);
        const Theme theme = Theme::byName(SettingsManager::instance().theme());
        menu->setStyleSheet(QStringLiteral("QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 4px; }"
                                           "QMenu::item { padding: 5px 22px 5px 12px; border-radius: 5px; }"
                                           "QMenu::item:selected { background: %4; }"
                                           "QMenu::separator { height: 1px; background: %3; margin: 4px 8px; }")
                                .arg(theme.panel.name(), theme.editorFg.name(), theme.border.name(), theme.selection.name()));
        QAction *firstItem = nullptr;
        for (const auto &entry : local) {
            QString title = entry.first;
            title.replace(QLatin1Char('&'), QStringLiteral("&&"));
            QAction *act = menu->addAction(title);
            firstItem = firstItem ? firstItem : act;
            connect(act, &QAction::triggered, this, entry.second);
        }
        const bool hasVenv = m_projects->hasProject() && !PythonEnv::activeVenv(m_projects->project().root).isEmpty();
        int shownGroup = -1;
        auto heading = [&](int g) {
            if (shownGroup == g)
                return;
            const bool needsHeading = actions.size() + missing.size() > 4 || g != 0 || shownGroup >= 0 || !local.isEmpty();
            shownGroup = g;
            if (needsHeading) {
                static const char *titles[] = {QT_TR_NOOP("Quick Fix"), QT_TR_NOOP("Refactor"), QT_TR_NOOP("Source Actions")};
                addMenuSection(menu, tr(titles[g]), theme, menu->actions().size() > 0);
            }
        };
        for (const QString &package : missing) {
            heading(0);
            QString title = hasVenv ? tr("Install \"%1\" in the virtual environment…").arg(package)
                                    : tr("Install \"%1\" (set up a virtual environment first)…").arg(package);
            title.replace(QLatin1Char('&'), QStringLiteral("&&"));
            QAction *act = menu->addAction(title);
            firstItem = firstItem ? firstItem : act;
            connect(act, &QAction::triggered, this, [this, package, hasVenv] {
                showPythonPackages();
                if (m_pkgDialog && hasVenv)
                    m_pkgDialog->installNow({package});
            });
        }
        for (const LspCodeAction &a : std::as_const(actions)) {
            heading(group(a));
            QString title = a.title;
            title.replace(QLatin1Char('&'), QStringLiteral("&&"));
            if (preferred(a))
                title += QStringLiteral("  ★");
            QAction *act = menu->addAction(title);
            firstItem = firstItem ? firstItem : act;
            connect(act, &QAction::triggered, this, [this, doc, a] {
                m_lsp->runCodeAction(doc, a, [this](bool ok) {
                    if (!ok)
                        statusBar()->showMessage(tr("The language server could not apply that action"), 4000);
                });
            });
        }
        if (firstItem)
            menu->setActiveAction(firstItem);
        menu->popup(guard->viewport()->mapToGlobal(guard->cursorRect().bottomLeft() + QPoint(0, 2)));
    };
    if (served)
        m_lsp->codeActions(doc, startLine, startColumn, endLine, endColumn, show);
    else
        show({});
}

// The packages that would fix an "import could not be resolved" diagnostic touching the range (Python only): the module's
// top-level name mapped to its PyPI package, skipping relative imports and modules that are the project's own.
QStringList MainWindow::unresolvedPythonImports(Document *doc, int startLine, int startColumn, int endLine, int endColumn) const
{
    QStringList out;
    if (!m_projects->hasProject())
        return out;
    static const QRegularExpression re(QStringLiteral("^Import \"([^\"]+)\" could not be resolved"));
    const QString root = m_projects->project().root;
    for (const LspDiagnostic &d : m_lsp->diagnostics(doc->filePath())) {
        if (d.server != QLatin1String("python") || (d.code != QLatin1String("reportMissingImports") && d.code != QLatin1String("reportMissingModuleSource")))
            continue;
        const bool before = d.endLine < startLine || (d.endLine == startLine && d.endColumn < startColumn);
        const bool after = d.startLine > endLine || (d.startLine == endLine && d.startColumn > endColumn);
        const auto m = re.match(d.message);
        if (before || after || !m.hasMatch() || m.captured(1).startsWith(QLatin1Char('.')))
            continue;
        const QString top = m.captured(1).section(QLatin1Char('.'), 0, 0);
        const QDir dir(root);
        if (dir.exists(top) || dir.exists(top + QStringLiteral(".py")) || dir.exists(QStringLiteral("src/") + top))
            continue; // the project's own module: a path problem, not a missing package
        const QString package = PythonTools::packageForModule(m.captured(1));
        if (!out.contains(package))
            out << package;
    }
    return out;
}

// A WorkspaceEdit from the server (import added, rename, ...): open files are edited in place as one undo step each.
bool MainWindow::applyWorkspaceEdit(const QJsonObject &edit)
{
    bool ok = false;
    const QHash<QString, QVector<LspTextEdit>> byPath = LspManager::editsOf(edit, &ok, true);
    const QVector<LspManager::FileOp> ops = LspManager::fileOpsOf(edit);
    if (!ok || (byPath.isEmpty() && ops.isEmpty()))
        return false;
    applyFileOps(ops, true);
    for (auto it = byPath.constBegin(); it != byPath.constEnd(); ++it) {
        // "Move to a new file": TypeScript sends the text for a file that does not exist yet, without a create operation.
        if (!QFileInfo::exists(it.key())) {
            QDir().mkpath(QFileInfo(it.key()).absolutePath());
            QFile created(it.key());
            if (created.open(QIODevice::WriteOnly))
                created.close();
        }
        Document *doc = m_editors->documentForPath(it.key());
        if (!doc && m_editors->openFile(it.key()))
            doc = m_editors->documentForPath(it.key());
        CodeEditor *ed = doc ? m_editors->editorFor(doc) : nullptr;
        if (!ed || !ed->applyTextEdits(it.value()))
            return false;
    }
    applyFileOps(ops, false);
    return true;
}

void MainWindow::editCompilerFlags()
{
    QString source;
    const QStringList detected = m_lsp->detectedFlags(&source);
    CompilerFlagsDialog dlg(m_lsp->projectFlagsText(), source, detected, this);
    if (dlg.exec() == QDialog::Accepted)
        m_lsp->setProjectFlagsText(dlg.text());
}

void MainWindow::editIncludePaths()
{
    const QString root = m_projects->project().root;
    QString note;
    if (QFileInfo::exists(root + QStringLiteral("/compile_flags.txt")) && LspManager::compileDatabasePath(root).isEmpty())
        note = tr("This project has a <b>compile_flags.txt</b>, which the language server uses instead of these paths. "
                  "Add the <b>-I</b> lines there.");
    else if (!LspManager::compileDatabasePath(root).isEmpty())
        note = tr("This project has a <b>compile_commands.json</b>; the paths are added to every entry of a copy kept in "
                  "QODE's cache. Your project files are not changed, and your build does not use these paths.");
    IncludePathsDialog dlg(root, SettingsManager::instance().includePaths(), note, this);
    if (dlg.exec() == QDialog::Accepted)
        m_lsp->setIncludePaths(dlg.paths());
}

void MainWindow::showLspInstallHelp(const QString &serverId)
{
    QMessageBox box(QMessageBox::Information, tr("Language server not found"), m_lsp->installHelp(serverId), QMessageBox::Ok, this);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();
}

bool MainWindow::installLspServer(const QString &serverId)
{
    const LspServerSpec *spec = LspServers::byId(serverId);
    if (!spec)
        return false;
    QString what;
    if (spec->installer == LspServerSpec::Installer::Npm) {
        NpmInstallDialog dlg(spec->managedId, this);
        if (dlg.exec() != QDialog::Accepted)
            return false;
        what = spec->managedId == QLatin1String("bash") ? tr("Bash language server") : tr("Web language servers");
    } else if (spec->installer == LspServerSpec::Installer::Pip) {
        PipInstallDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return false;
        what = tr("Python language tools");
    } else if (spec->installer == LspServerSpec::Installer::Jdtls) {
        JdtlsInstallDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return false;
        what = tr("Java language server");
    } else {
        LspInstallDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return false;
        what = tr("Kotlin language server");
    }
    // A path chosen earlier would shadow the new install; clearing it also restarts the server. One install can
    // provide several servers (the web ones), so all of them are refreshed.
    for (const LspServerSpec *s : LspServers::sharingInstall(*spec))
        m_lsp->setServerPath(s->id, {});
    statusBar()->showMessage(tr("%1 installed").arg(what), 5000);
    return true;
}

void MainWindow::removeLspServer(const QString &serverId)
{
    auto *dlg = new LspRemoveDialog(serverId, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::NonModal); // the terminal must stay usable for the sudo password
    connect(dlg, &LspRemoveDialog::removalStarting, m_lsp, &LspManager::shutdown);
    connect(dlg, &LspRemoveDialog::terminalCommandRequested, this, [this](const QString &cmd) {
        showTerminal();
        m_terminal->runCommand(cmd);
    });
    connect(dlg, &LspRemoveDialog::restartRequested, this, [this] { QTimer::singleShot(0, this, &MainWindow::restartApplication); });
    dlg->show();
}

void MainWindow::chooseLspServerPath(const QString &serverId)
{
    const QString current = m_lsp->serverPath(serverId);
    const QString path = QFileDialog::getOpenFileName(this, tr("Select the language server executable"),
                                                      current.isEmpty() ? QStringLiteral("/usr/bin") : current);
    if (!path.isEmpty())
        m_lsp->setServerPath(serverId, path);
}

void MainWindow::showLspLog(const QString &serverId)
{
    const LspServerSpec *spec = LspServers::byId(serverId);
    auto *dlg = new LspLogDialog(spec ? spec->displayName : serverId, [this, serverId] { return m_lsp->logOf(serverId); }, this);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

// --- Projects -----------------------------------------------------------------

// Swatches in front of colour literals (CSS colours, Tailwind classes), from every server that knows colours.
void MainWindow::requestColors(Document *doc, CodeEditor *editor)
{
    // Throttle: a server that knows no colours answers nothing, and every status change asks again.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_lsp->isServed(doc) || (!editor->hasColorSwatches() && now - m_colorAsked.value(doc, 0) < 2500))
        return;
    m_colorAsked.insert(doc, now);
    QPointer<CodeEditor> guard(editor);
    m_lsp->documentColors(doc, [this, guard](const QVector<LspColor> &colors) {
        if (!guard)
            return;
        QVector<CodeEditor::ColorSwatch> swatches;
        swatches.reserve(colors.size());
        for (const LspColor &c : colors) {
            CodeEditor::ColorSwatch sw;
            sw.startLine = c.startLine;
            sw.startColumn = c.startColumn;
            sw.endLine = c.endLine;
            sw.endColumn = c.endColumn;
            sw.color = QColor::fromRgbF(qBound(0.0, c.red, 1.0), qBound(0.0, c.green, 1.0), qBound(0.0, c.blue, 1.0), qBound(0.0, c.alpha, 1.0));
            swatches.append(sw);
        }
        m_colorData.insert(guard, colors);
        guard->setColorSwatches(swatches);
    });
}

// Click on a swatch: pick a new colour and write it back in the notation the text already uses.
void MainWindow::pickColor(Document *doc, CodeEditor *editor, int index)
{
    const QVector<LspColor> data = m_colorData.value(editor);
    const QTextCursor range = editor->swatchRange(index);
    if (index < 0 || index >= data.size() || range.isNull())
        return;
    const LspColor lc = data.at(index);
    const QString original = range.selectedText().trimmed().toLower();
    const QColor initial = QColor::fromRgbF(qBound(0.0, lc.red, 1.0), qBound(0.0, lc.green, 1.0), qBound(0.0, lc.blue, 1.0), qBound(0.0, lc.alpha, 1.0));
    const QColor picked = QColorDialog::getColor(initial, this, tr("Pick a Color"), QColorDialog::ShowAlphaChannel);
    if (!picked.isValid())
        return;
    QPointer<CodeEditor> guard(editor);
    m_lsp->colorPresentations(doc, lc, picked, [this, guard, index, original](const QVector<LspColorPresentation> &list) {
        if (!guard)
            return;
        if (list.isEmpty()) {
            statusBar()->showMessage(tr("This colour cannot be edited with the picker"), 4000);
            return;
        }
        // Keep the notation of the original: #hex, rgb(), hsl() ...
        QString prefix;
        for (const QString &p : {QStringLiteral("#"), QStringLiteral("rgb"), QStringLiteral("hsl"), QStringLiteral("hwb")})
            if (original.startsWith(p))
                prefix = p;
        const LspColorPresentation *chosen = &list.first();
        if (!prefix.isEmpty())
            for (const LspColorPresentation &p : list)
                if (p.label.toLower().startsWith(prefix)) {
                    chosen = &p;
                    break;
                }
        guard->replaceSwatch(index, chosen->hasEdit ? chosen->edit.text : chosen->label);
    });
}

// F2: rename the symbol under the caret everywhere the language server knows it.
void MainWindow::renameSymbol()
{
    // F2 belongs to the file tree when it has focus: it renames the selected file or folder
    if (QWidget *f = QApplication::focusWidget(); f && m_explorer->isAncestorOf(f)) {
        m_explorer->renameSelected();
        return;
    }
    CodeEditor *ed = m_editors->currentEditor();
    Document *doc = ed ? documentOf(ed) : nullptr;
    if (!doc)
        return;
    if (!m_lsp->supportsRename(doc)) {
        statusBar()->showMessage(tr("Rename needs a running language server that supports it for this file"), 4000);
        return;
    }
    QTextCursor word = ed->textCursor();
    word.select(QTextCursor::WordUnderCursor);
    QString fallback = word.selectedText();
    // CSS servers rename the whole selector (".card"), so the box shows it with its dot / hash.
    const QChar lead = word.selectionStart() > 0 ? ed->document()->characterAt(word.selectionStart() - 1) : QChar();
    const QString selectorLead = (lead == QLatin1Char('.') || lead == QLatin1Char('#')) && !fallback.isEmpty() ? QString(lead) : QString();
    const bool cssLike = doc->languageName() == QLatin1String("CSS") || doc->languageName() == QLatin1String("SCSS") || doc->languageName() == QLatin1String("Less");
    if (cssLike)
        fallback.prepend(selectorLead);
    const int line = ed->textCursor().blockNumber(), column = ed->textCursor().positionInBlock();
    QPointer<CodeEditor> guard(ed);
    m_lsp->prepareRename(doc, line, column, [this, doc, guard, line, column, fallback, selectorLead, cssLike](bool ok, const QString &placeholder, const QString &error) {
        if (!guard)
            return;
        if (!ok) {
            statusBar()->showMessage(error.isEmpty() ? tr("There is nothing to rename here") : error, 4000);
            return;
        }
        const QString current = placeholder.isEmpty() ? fallback : placeholder;
        if (current.isEmpty()) {
            statusBar()->showMessage(tr("Put the caret on a name to rename it"), 3000);
            return;
        }
        bool accepted = false;
        QString name = QInputDialog::getText(this, tr("Rename Symbol"), tr("New name for “%1”:").arg(current), QLineEdit::Normal, current, &accepted).trimmed();
        if (!accepted || name.isEmpty() || name == current)
            return;
        if (!selectorLead.isEmpty() && cssLike && !name.startsWith(selectorLead))
            name.prepend(selectorLead); // the server replaces the selector including its dot / hash
        m_lsp->rename(doc, line, column, name, [this, current, name](const QJsonObject &edit, const QString &err) {
            if (!err.isEmpty()) {
                statusBar()->showMessage(tr("Rename failed: %1").arg(err), 6000);
                return;
            }
            bool editable = false;
            const QHash<QString, QVector<LspTextEdit>> byPath = LspManager::editsOf(edit, &editable);
            int places = 0;
            for (const QVector<LspTextEdit> &v : byPath)
                places += v.size();
            if (!applyWorkspaceEdit(edit)) {
                statusBar()->showMessage(tr("The language server's rename could not be applied"), 6000);
                return;
            }
            statusBar()->showMessage(tr("Renamed “%1” to “%2”: %3 change(s) in %4 file(s) — files stay unsaved, review with Save All")
                                         .arg(current, name).arg(places).arg(byPath.size()),
                                     8000);
        });
    });
}

// Shift+F12: every use of the symbol under the caret, in a list that jumps to the place.
void MainWindow::findReferences()
{
    CodeEditor *ed = m_editors->currentEditor();
    if (ed)
        showReferencesAt(ed, ed->textCursor().blockNumber(), ed->textCursor().positionInBlock());
}

void MainWindow::showReferencesAt(CodeEditor *ed, int line, int column)
{
    Document *doc = ed ? documentOf(ed) : nullptr;
    if (!doc)
        return;
    if (!m_lsp->supportsReferences(doc)) {
        statusBar()->showMessage(tr("Find All References needs a running language server that supports it for this file"), 4000);
        return;
    }
    QTextCursor word(ed->document());
    const QTextBlock block = ed->document()->findBlockByNumber(line);
    word.setPosition(block.position() + qMin(column, qMax(0, block.length() - 1)));
    word.select(QTextCursor::WordUnderCursor);
    const QString symbol = word.selectedText();
    QPointer<CodeEditor> guard(ed);
    m_lsp->references(doc, line, column, [this, guard, symbol](const QVector<LspLocation> &locations) {
        if (guard)
            showLocations(symbol, locations);
    });
}

void MainWindow::showLocations(const QString &symbol, const QVector<LspLocation> &locations, bool implementations)
{
    if (locations.isEmpty()) {
        statusBar()->showMessage(implementations ? tr("No implementations found") : tr("No references found"), 3000);
        return;
    }
    QHash<QString, QStringList> texts; // file -> lines, read once
    QVector<ReferencesDialog::Entry> entries;
    for (const LspLocation &loc : locations) {
        if (!texts.contains(loc.path)) {
            QString content;
            if (Document *open = m_editors->documentForPath(loc.path)) {
                content = open->text();
            } else {
                QFile f(loc.path);
                if (f.open(QIODevice::ReadOnly))
                    content = QString::fromUtf8(f.read(4 * 1024 * 1024));
            }
            texts.insert(loc.path, content.split(QLatin1Char('\n')));
        }
        const QStringList &lines = texts[loc.path];
        ReferencesDialog::Entry e;
        e.path = loc.path;
        e.line = loc.line;
        e.column = loc.column;
        e.text = loc.line >= 0 && loc.line < lines.size() ? lines.at(loc.line) : QString();
        entries.append(e);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const ReferencesDialog::Entry &a, const ReferencesDialog::Entry &b) {
        return a.path == b.path ? a.line < b.line : a.path < b.path;
    });
    if (!m_refsDialog) {
        m_refsDialog = new ReferencesDialog(this);
        m_refsDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_refsDialog, &ReferencesDialog::openRequested, this,
                [this](const QString &path, int line, int column) { m_editors->openFileAt(path, line + 1, column + 1); });
    }
    m_refsDialog->setReferences(symbol, entries, m_projects->project().root, implementations);
    m_refsDialog->show();
    m_refsDialog->raise();
    m_refsDialog->activateWindow();
}
