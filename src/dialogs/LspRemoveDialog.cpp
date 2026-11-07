#include "LspRemoveDialog.h"
#include "platform/Platform.h"
#include "platform/AppPaths.h"

#include "lsp/JarSource.h"
#include "lsp/JdtlsInstaller.h"
#include "lsp/LspInstaller.h"
#include "lsp/NpmInstaller.h"
#include "lsp/PipInstaller.h"
#include "lsp/LspServers.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include "settings/Theme.h"
#include <QProcess>
#include <QRegularExpression>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {

qint64 dirSize(const QString &path)
{
    qint64 total = 0;
    QStringList stack{path};
    while (!stack.isEmpty()) {
        const QDir dir(stack.takeLast());
        for (const QFileInfo &e : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks)) {
            if (!e.isDir())
                total += e.size();
            else if (!e.isJunction()) // Windows: `current` is a junction to a version folder, counted once
                stack << e.absoluteFilePath();
        }
    }
    return total;
}

QString shown(const QString &path)
{
    const QString home = QDir::homePath();
    return Platform::isUnder(path, home) ? QStringLiteral("~") + path.mid(home.size()) : path;
}

// The server's own per-project indexes / workspace data (QODE/lsp/<id>/<hash of project>).
QString serverCacheDir(const QString &id)
{
    return AppPaths::cache(QStringLiteral("lsp/")) + id;
}

QString libraryCacheDir()
{
    return AppPaths::cache(QStringLiteral("lsp/library-sources"));
}

// Also inside a portable QODE's own data folder, which may be on another drive (a USB stick).
bool underHome(const QString &path)
{
    if (path.isEmpty())
        return false;
    const QString clean = QDir::cleanPath(path);
    return Platform::isUnder(clean, QDir::homePath()) || (AppPaths::isPortable() && Platform::isUnder(clean, AppPaths::data()));
}

} // namespace

LspRemoveDialog::LspRemoveDialog(const QString &serverId, QWidget *parent) : QDialog(parent), m_id(serverId)
{
    const LspServerSpec *spec = LspServers::byId(serverId);
    const QString name = spec ? spec->displayName : serverId;
    setWindowTitle(tr("Remove %1").arg(name));
    setMinimumWidth(640);
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    m_intro = new QLabel(this);
    m_intro->setWordWrap(true);
    m_intro->setTextFormat(Qt::RichText);
    layout->addWidget(m_intro);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setFont(SettingsManager::instance().editorFont());
    m_view->setMinimumHeight(190);
    layout->addWidget(m_view, 1);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setTextFormat(Qt::RichText);
    m_note->hide();
    layout->addWidget(m_note);

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    m_secondary = new QPushButton(this);
    m_primary = new QPushButton(this);
    m_restart = new QPushButton(tr("Restart QODE"), this);
    m_close = new QPushButton(tr("Cancel"), this);
    m_restart->hide();
    row->addWidget(m_secondary);
    row->addWidget(m_primary);
    row->addWidget(m_restart);
    row->addWidget(m_close);
    layout->addLayout(row);

    connect(m_close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_restart, &QPushButton::clicked, this, [this] {
        emit restartRequested();
        close();
    });

    if (spec && (spec->installer == LspServerSpec::Installer::Download || spec->installer == LspServerSpec::Installer::Jdtls))
        buildManaged();
    else if (spec && spec->installer == LspServerSpec::Installer::Npm)
        buildNpm();
    else if (spec && spec->installer == LspServerSpec::Installer::Pip)
        buildPip();
    else
        buildSystem();
}

void LspRemoveDialog::reject()
{
    if (!m_running)
        QDialog::reject();
}

// Comments ("# ...") are muted italics, commands ("$ ...") are bold in the accent colour with a muted prompt,
// results are green / red; anything else is plain text.
void LspRemoveDialog::log(const QString &line, const QString &)
{
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    QTextCursor c(m_view->document());
    c.movePosition(QTextCursor::End);
    if (!m_view->document()->isEmpty())
        c.insertBlock();
    QTextCharFormat plain;
    plain.setForeground(theme.editorFg);
    QTextCharFormat fmt = plain;
    QString text = line;
    const QString trimmed = line.trimmed();
    if (line.startsWith(QLatin1String("# "))) {
        fmt.setForeground(theme.comment);
        fmt.setFontItalic(true);
    } else if (line.startsWith(QLatin1String("$ "))) {
        QTextCharFormat prompt = plain;
        prompt.setForeground(theme.textMuted);
        c.insertText(QStringLiteral("$ "), prompt);
        text = line.mid(2);
        fmt.setForeground(theme.accent);
        fmt.setFontWeight(QFont::Bold);
    } else if (trimmed == tr("done") || trimmed == tr("already gone") || trimmed.startsWith(tr("Finished. "))) {
        fmt.setForeground(theme.gitAdded);
    } else if (trimmed.startsWith(tr("failed")) || trimmed.startsWith(tr("skipped")) || trimmed.startsWith(tr("could not")) ||
               trimmed.startsWith(tr("Finished with errors"))) {
        fmt.setForeground(theme.gitConflict);
    }
    c.insertText(text, fmt);
    m_view->setTextCursor(c);
}

// --- A server QODE downloaded (Kotlin, Java) ----------------------------------------------------------------

void LspRemoveDialog::buildManaged()
{
    const bool java = m_id == QLatin1String("java");
    const QString root = java ? JdtlsInstaller::installRoot() : LspInstaller::installRoot();
    const QString version = java ? JdtlsInstaller::installedVersion() : LspInstaller::installedVersion();
    const QString cacheDir = serverCacheDir(m_id);
    // Only Kotlin's install adds a ~/.local/bin command link (Windows: kotlin-lsp.cmd).
    const QFileInfo link(java ? QString() : LspInstaller::linkPath());
    const QString linkTarget = java ? QString() : Platform::commandLinkTarget(link.filePath());
    const bool ownLink = !linkTarget.isEmpty() && LspInstaller::isManaged(linkTarget);
    const QLocale loc;

    m_intro->setText(tr("<b>Remove the %1 language server</b> that QODE downloaded%2. "
                        "Nothing outside your home folder is touched and no administrator rights are needed. "
                        "These are the exact commands that will run:")
                         .arg(java ? tr("Java") : tr("Kotlin"), version.isEmpty() ? QString() : tr(" (version %1)").arg(version.toHtmlEscaped())));

    m_steps.clear();
    if (ownLink)
        m_steps.append({tr("Remove the command link"), Platform::removeCommandText(link.filePath(), shown(link.filePath()), true), link.filePath(), true});
    m_steps.append({tr("Delete the downloaded server (about %1)").arg(loc.formattedDataSize(dirSize(root))),
                    Platform::removeCommandText(root, shown(root), false), root, false});
    if (QFileInfo::exists(cacheDir))
        m_steps.append({tr("Delete the server's project index and cache (about %1)").arg(loc.formattedDataSize(dirSize(cacheDir))),
                        Platform::removeCommandText(cacheDir, shown(cacheDir), false), cacheDir, false});

    m_library = new QCheckBox(tr("Also delete cached library sources opened with Go to Definition (about %1)")
                                  .arg(loc.formattedDataSize(dirSize(libraryCacheDir()))),
                              this);
    m_library->setChecked(true);
    m_library->setVisible(QFileInfo::exists(libraryCacheDir()));
    static_cast<QVBoxLayout *>(layout())->insertWidget(2, m_library);

    auto plan = [this] {
        m_view->clear();
        QList<Step> steps = m_steps;
        if (m_library && m_library->isChecked())
            steps.append({QString(), Platform::removeCommandText(libraryCacheDir(), shown(libraryCacheDir()), false), libraryCacheDir(), false});
        for (const Step &s : std::as_const(steps)) {
            if (!s.text.isEmpty())
                log(QStringLiteral("# %1").arg(s.text));
            else
                log(QStringLiteral("# %1").arg(tr("Delete cached library sources")));
            log(QStringLiteral("$ %1").arg(s.display));
        }
        log(QStringLiteral("# %1").arg(tr("Forget the saved server path (if it pointed to this install)")));
    };
    plan();
    connect(m_library, &QCheckBox::toggled, this, plan);

    m_secondary->hide();
    m_primary->setText(tr("Remove"));
    m_primary->setDefault(true);
    connect(m_primary, &QPushButton::clicked, this, &LspRemoveDialog::startManaged);
}

void LspRemoveDialog::startManaged()
{
    if (m_library && m_library->isChecked())
        m_steps.append({tr("Delete cached library sources"), Platform::removeCommandText(libraryCacheDir(), shown(libraryCacheDir()), false), libraryCacheDir(), false});
    m_primary->setEnabled(false);
    m_close->setEnabled(false);
    if (m_library)
        m_library->setEnabled(false);
    m_running = true;
    m_view->clear();
    log(tr("Stopping the running language server…"));
    emit removalStarting();
    m_next = 0;
    runNext();
}

void LspRemoveDialog::runNext()
{
    if (m_next >= m_steps.size()) {
        // The saved path would otherwise keep pointing at a file that no longer exists.
        if (const LspServerSpec *spec = LspServers::byId(m_id))
            for (const LspServerSpec *s : LspServers::sharingInstall(*spec)) {
                const QString configured = SettingsManager::instance().lspServerPath(s->id);
                if (!configured.isEmpty() && LspServers::isManaged(*s, configured))
                    SettingsManager::instance().setLspServerPath(s->id, {});
            }
        finish(!m_failed);
        return;
    }
    const Step &s = m_steps.at(m_next++);
    log(QString());
    log(QStringLiteral("# %1").arg(s.text));
    log(QStringLiteral("$ %1").arg(s.display));
    if (!underHome(s.path)) { // never run rm outside the home folder, whatever the settings say
        log(tr("  skipped: %1 is not inside your home folder").arg(s.path));
        m_failed = true;
        runNext();
        return;
    }
    if (!QFileInfo(s.path).exists() && !QFileInfo(s.path).isSymLink()) {
        log(tr("  already gone"));
        runNext();
        return;
    }
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QString out = Platform::decodeOutput(m_proc->readAll()).trimmed();
        if (!out.isEmpty())
            log(out);
        if (status == QProcess::NormalExit && code == 0) {
            log(tr("  done"));
        } else {
            log(tr("  failed (exit code %1)").arg(code));
            m_failed = true;
        }
        m_proc->deleteLater();
        m_proc = nullptr;
        runNext();
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        log(tr("  could not start the command"));
        m_failed = true;
        m_proc->deleteLater();
        m_proc = nullptr;
        runNext();
    });
    Platform::setRemoveCommand(m_proc, s.path, s.onlyLink); // Unix: rm; Windows: rmdir /s /q, which never follows a junction
    m_proc->start();
}

void LspRemoveDialog::finish(bool ok)
{
    m_running = false;
    log(QString());
    const LspServerSpec *spec = LspServers::byId(m_id);
    const QString removed = spec && spec->managedId == QLatin1String("bash")                ? tr("Bash language server")
                            : spec && spec->installer == LspServerSpec::Installer::Npm ? tr("web language servers")
                            : spec && spec->installer == LspServerSpec::Installer::Pip ? tr("Python language tools")
                            : m_id == QLatin1String("java")                          ? tr("Java language server")
                                                                                     : tr("Kotlin language server");
    log(ok ? tr("Finished. Removed the %1.").arg(removed) : tr("Finished with errors. Some files could not be removed; see the messages above."));
    m_primary->hide();
    m_close->setEnabled(true);
    askRestart(ok ? tr("Restart QODE so the language server state is fully reset?")
                  : tr("Restart QODE after checking the messages above?"));
}

// --- Servers installed with npm (TypeScript / JavaScript, HTML, CSS, JSON in one folder; Bash in another) -----

void LspRemoveDialog::buildNpm()
{
    const LspServerSpec *spec = LspServers::byId(m_id);
    const QString root = NpmInstaller::installRoot(spec->managedId);
    if (spec->managedId == QLatin1String("bash"))
        m_intro->setText(tr("<b>Remove the Bash language server</b> that QODE installed with npm. ShellCheck and the web language "
                            "servers are not touched, nothing outside your home folder is touched and no administrator rights "
                            "are needed. This is the exact command that will run:"));
    else
        m_intro->setText(tr("<b>Remove the web language servers</b> that QODE installed with npm: TypeScript / JavaScript, HTML, "
                            "CSS and JSON share one folder, so all four go together. Nothing outside your home folder is touched "
                            "and no administrator rights are needed. This is the exact command that will run:"));
    m_steps.clear();
    m_steps.append({tr("Delete the installed servers (about %1)").arg(QLocale().formattedDataSize(dirSize(root))),
                    Platform::removeCommandText(root, shown(root), false), root, false});
    for (const Step &s : std::as_const(m_steps)) {
        log(QStringLiteral("# %1").arg(s.text));
        log(QStringLiteral("$ %1").arg(s.display));
    }
    log(QStringLiteral("# %1").arg(tr("Forget the saved server paths (if they pointed to this install)")));

    m_secondary->hide();
    m_primary->setText(tr("Remove"));
    m_primary->setDefault(true);
    connect(m_primary, &QPushButton::clicked, this, &LspRemoveDialog::startManaged);
}

// --- Servers installed into a private virtual environment (Python: basedpyright and ruff) ------------------

void LspRemoveDialog::buildPip()
{
    const QString root = PipInstaller::installRoot();
    m_intro->setText(tr("<b>Remove the Python language tools</b> that QODE installed: the language server (basedpyright) and Ruff "
                        "live in one private virtual environment, so both go together. Your own projects and their virtual "
                        "environments are not touched, nothing outside your home folder is touched and no administrator rights "
                        "are needed. This is the exact command that will run:"));
    m_steps.clear();
    m_steps.append({tr("Delete the installed tools (about %1)").arg(QLocale().formattedDataSize(dirSize(root))),
                    Platform::removeCommandText(root, shown(root), false), root, false});
    for (const Step &s : std::as_const(m_steps)) {
        log(QStringLiteral("# %1").arg(s.text));
        log(QStringLiteral("$ %1").arg(s.display));
    }
    log(QStringLiteral("# %1").arg(tr("Forget the saved server paths (if they pointed to this install)")));

    m_secondary->hide();
    m_primary->setText(tr("Remove"));
    m_primary->setDefault(true);
    connect(m_primary, &QPushButton::clicked, this, &LspRemoveDialog::startManaged);
}

// --- A system package (clangd) -------------------------------------------------------------------------

void LspRemoveDialog::buildSystem()
{
    const LspServerSpec *spec = LspServers::byId(m_id);
    const QString exe = spec ? LspServers::locate(*spec, SettingsManager::instance().lspServerPath(m_id)) : QString();
    const QString resolved = exe.isEmpty() ? QString() : QFileInfo(exe).canonicalFilePath();
    static const QRegularExpression versioned(QStringLiteral("^clangd-(\\d+)$"));
    const auto match = versioned.match(QFileInfo(resolved).fileName());
    const QString aptPackage = match.hasMatch() ? QStringLiteral("clangd-%1").arg(match.captured(1)) : QStringLiteral("clangd");

    struct Candidate { QString tool, command, note; };
    QList<Candidate> found, all;
    if (Platform::osTag() == QLatin1String("win")) {
        buildSystemWindows(resolved);
        return;
    }
    all.append({QStringLiteral("apt"), QStringLiteral("sudo apt remove %1 && sudo apt autoremove").arg(aptPackage), tr("Debian, Ubuntu, Mint")});
    all.append({QStringLiteral("dnf"), QStringLiteral("sudo dnf remove clang-tools-extra"), tr("Fedora, RHEL")});
    all.append({QStringLiteral("pacman"), QStringLiteral("sudo pacman -Rns clang"), tr("Arch, Manjaro (this removes the whole clang package)")});
    all.append({QStringLiteral("zypper"), QStringLiteral("sudo zypper remove clang"), tr("openSUSE")});
    for (const Candidate &c : std::as_const(all))
        if (!QStandardPaths::findExecutable(c.tool).isEmpty())
            found.append(c);

    m_intro->setText(tr("<b>Remove clangd (C/C++)</b>. clangd is a system package, so removing it needs administrator rights. "
                        "QODE can type the command into its terminal, where you enter your password; copy it if you prefer another terminal.%1")
                         .arg(resolved.isEmpty() ? QString() : tr("<br>Installed at <b>%1</b>.").arg(resolved.toHtmlEscaped())));

    const QList<Candidate> &shownList = found.isEmpty() ? all : found;
    for (const Candidate &c : shownList) {
        log(QStringLiteral("# %1").arg(c.note));
        log(QStringLiteral("$ %1").arg(c.command));
        log(QString());
    }
    if (!resolved.isEmpty() && !resolved.startsWith(QLatin1String("/usr/")) && !resolved.startsWith(QLatin1String("/bin/")))
        log(tr("# clangd is installed outside the system folders (%1); delete that file or folder yourself.").arg(resolved));
    if (!found.isEmpty())
        m_command = found.first().command;

    m_secondary->setText(tr("Copy Command"));
    m_secondary->setEnabled(!m_command.isEmpty());
    m_primary->setText(tr("Run in QODE Terminal"));
    m_primary->setEnabled(!m_command.isEmpty());
    m_primary->setDefault(true);
    m_close->setText(tr("Close"));
    connect(m_secondary, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_command);
        m_secondary->setText(tr("Copied"));
    });
    connect(m_primary, &QPushButton::clicked, this, [this] {
        emit terminalCommandRequested(m_command);
        askRestart(tr("The command is running in the QODE terminal: enter your password there and wait for it to finish. "
                      "Then restart QODE so it stops looking for clangd."));
    });
}

// Windows: clangd comes from LLVM's installer (winget LLVM.LLVM, Program Files\LLVM) or from MSYS2 (pacman). The
// command is chosen by where clangd.exe is; anything else is removed in Settings > Apps.
void LspRemoveDialog::buildSystemWindows(const QString &resolved)
{
    static const QRegularExpression msys(QStringLiteral("^(.*)/(ucrt64|clang64|mingw64)/bin/clangd\\.exe$"),
                                         QRegularExpression::CaseInsensitiveOption);
    const auto m = msys.match(resolved);
    QString note;
    if (m.hasMatch()) {
        const QString env = m.captured(2).toLower();
        const QString package = env == QLatin1String("ucrt64")    ? QStringLiteral("mingw-w64-ucrt-x86_64-clang-tools-extra")
                                : env == QLatin1String("clang64") ? QStringLiteral("mingw-w64-clang-x86_64-clang-tools-extra")
                                                                  : QStringLiteral("mingw-w64-x86_64-clang-tools-extra");
        m_command = QStringLiteral("%1/usr/bin/pacman.exe -Rns %2").arg(m.captured(1), package);
        note = tr("MSYS2 (%1)").arg(env);
    } else if (!QStandardPaths::findExecutable(QStringLiteral("winget")).isEmpty() &&
               (resolved.isEmpty() || resolved.contains(QLatin1String("/LLVM/"), Qt::CaseInsensitive))) {
        m_command = QStringLiteral("winget uninstall --id LLVM.LLVM");
        note = tr("LLVM, installed with winget or LLVM's installer (this removes all of LLVM)");
    }

    m_intro->setText(tr("<b>Remove clangd (C/C++)</b>. clangd was installed system-wide, so removing it may show a Windows "
                        "prompt for administrator rights. QODE can type the command into its terminal; copy it if you prefer "
                        "another terminal.%1")
                         .arg(resolved.isEmpty() ? QString()
                                                 : tr("<br>Installed at <b>%1</b>.").arg(QDir::toNativeSeparators(resolved).toHtmlEscaped())));
    if (!m_command.isEmpty()) {
        log(QStringLiteral("# %1").arg(note));
        log(QStringLiteral("$ %1").arg(m_command));
        log(QString());
    } else {
        log(tr("# QODE does not know how this clangd was installed. Remove it in Settings > Apps > Installed apps "
               "(LLVM), with the package manager that installed it, or delete %1 yourself.")
                .arg(resolved.isEmpty() ? QStringLiteral("clangd.exe") : QDir::toNativeSeparators(resolved)));
    }

    m_secondary->setText(tr("Copy Command"));
    m_secondary->setEnabled(!m_command.isEmpty());
    m_primary->setText(tr("Run in QODE Terminal"));
    m_primary->setEnabled(!m_command.isEmpty());
    m_primary->setDefault(true);
    m_close->setText(tr("Close"));
    connect(m_secondary, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_command);
        m_secondary->setText(tr("Copied"));
    });
    connect(m_primary, &QPushButton::clicked, this, [this] {
        emit terminalCommandRequested(m_command);
        askRestart(tr("The command is running in the QODE terminal: answer its questions (and the Windows prompt, if one "
                      "appears) and wait for it to finish. Then restart QODE so it stops looking for clangd."));
    });
}

void LspRemoveDialog::askRestart(const QString &message)
{
    m_note->setText(message);
    m_note->show();
    m_restart->show();
    m_close->setText(tr("Later"));
    m_close->setEnabled(true);
}
