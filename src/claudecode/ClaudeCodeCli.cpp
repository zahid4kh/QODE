#include "ClaudeCodeCli.h"

#include "ai/AiEnv.h"
#include "platform/Platform.h"
#include "settings/SettingsManager.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

namespace claudecode {

namespace {
QString stripAnsi(QString s)
{
    static const QRegularExpression ansi(QStringLiteral("\\x1b\\[[0-9;?]*[A-Za-z]|\\x1b\\][^\\x07]*\\x07|\\r"));
    return s.remove(ansi);
}

// The output's last few meaningful lines: the error a failed login printed.
QString tail(const QString &output, const QString &skip)
{
    QStringList lines;
    for (const QString &l : output.split(QLatin1Char('\n'))) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && (skip.isEmpty() || !t.contains(skip)) && !t.startsWith(QLatin1String("Paste code")) && !t.startsWith(QLatin1String("Opening browser")) &&
            !t.startsWith(QLatin1String("If the browser")) && !t.startsWith(QLatin1String("Invalid code")))
            lines << t;
    }
    return lines.mid(qMax(0, int(lines.size()) - 4)).join(QLatin1Char('\n'));
}

// PATH with ~/.local/bin (where the native installer puts `claude`) in front when it is missing: a desktop launch
// often lacks it, and would then find an older npm copy in /usr/local/bin instead of the one the terminal runs.
QStringList searchPath()
{
    // Windows: the native installer uses %USERPROFILE%\.local\bin too. Native separators, so the check below matches.
    const QString local = QDir::toNativeSeparators(QDir::homePath() + QStringLiteral("/.local/bin"));
    QStringList path = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    if (!path.contains(local))
        path.prepend(local);
    // Windows: npm's global folder (claude.cmd from npm install -g) and the other per-user tool folders. Unix: none.
    for (const QString &dir : Platform::extraBinDirs())
        if (!path.contains(dir, Qt::CaseInsensitive))
            path << dir;
    return path;
}
} // namespace

QString executable()
{
    const QString found = QStandardPaths::findExecutable(QStringLiteral("claude"), searchPath());
    if (!found.isEmpty())
        return found;
    const QString home = QDir::homePath();
    for (const QString &p : {home + QStringLiteral("/.claude/local/claude"),
                             home + QStringLiteral("/.npm-global/bin/claude"), home + QStringLiteral("/.bun/bin/claude"),
                             QStringLiteral("/usr/local/bin/claude"), QStringLiteral("/usr/bin/claude")}) {
        const QFileInfo fi(p);
        if (fi.isFile() && fi.isExecutable())
            return fi.absoluteFilePath();
    }
    return {};
}

QString installCommand()
{
    // Windows: Anthropic's PowerShell installer, written so that it also runs from cmd or Git Bash.
    if (Platform::osTag() == QLatin1String("win"))
        return QStringLiteral("powershell -NoProfile -ExecutionPolicy Bypass -Command \"irm https://claude.ai/install.ps1 | iex\"");
    return QStringLiteral("curl -fsSL https://claude.ai/install.sh | bash");
}

QString installHint()
{
    if (Platform::osTag() != QLatin1String("win"))
        return QObject::tr("<p>Claude Code is not installed. QODE runs the <code>claude</code> command line tool for you, "
                           "so install it first: run this in a terminal (it goes to <code>~/.local/bin</code>), then check again.</p>");
    QString text = QObject::tr("<p>Claude Code is not installed. QODE runs the <code>claude</code> command line tool for you, "
                               "so install it first: run this in a terminal (it goes to <code>%USERPROFILE%\\.local\\bin</code>; "
                               "<code>npm install -g @anthropic-ai/claude-code</code> works too), then check again.</p>");
    if (Platform::gitBashExecutable().isEmpty())
        text += QObject::tr("<p>On Windows Claude Code also needs <b>Git for Windows</b> (it runs its commands in Git Bash): "
                            "install it from <a href=\"https://git-scm.com/downloads/win\">git-scm.com</a> or with "
                            "<code>winget install Git.Git</code>.</p>");
    return text;
}

AuthMode authMode()
{
    return SettingsManager::instance().claudeCode(QStringLiteral("auth")) == QLatin1String("apiKey") ? AuthMode::ApiKey : AuthMode::Account;
}

void setAuthMode(AuthMode mode)
{
    SettingsManager::instance().setClaudeCode(QStringLiteral("auth"), mode == AuthMode::ApiKey ? QStringLiteral("apiKey") : QStringLiteral("account"));
}

QProcessEnvironment environment(const QString &projectRoot)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"), searchPath().join(QDir::listSeparator()));
    if (authMode() == AuthMode::ApiKey) {
        const QString key = AiEnv::apiKey(AiEnv::Provider::Claude, projectRoot);
        if (!key.isEmpty())
            env.insert(QStringLiteral("ANTHROPIC_API_KEY"), key);
    } else {
        env.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    }
    // The CLI offers its to-do tool (TodoWrite, shown as the task list above the input) only to older models unless
    // this is set. A value the user set is kept.
    if (!env.contains(QStringLiteral("CLAUDE_CODE_ENABLE_TODO_TOOLS")))
        env.insert(QStringLiteral("CLAUDE_CODE_ENABLE_TODO_TOOLS"), QStringLiteral("1"));
    // Windows: the CLI runs its commands in Git Bash and stops at once when it cannot find bash.exe (Git not on PATH).
    // A value the user set is kept. Unix: no Git Bash, nothing set.
    if (!env.contains(QStringLiteral("CLAUDE_CODE_GIT_BASH_PATH")))
        if (const QString bash = Platform::gitBashExecutable(); !bash.isEmpty())
            env.insert(QStringLiteral("CLAUDE_CODE_GIT_BASH_PATH"), QDir::toNativeSeparators(bash));
    return env;
}

Auth::Auth(QObject *parent)
    : QObject(parent)
{
}

Auth::~Auth()
{
    for (QProcess *p : {m_statusProc, m_loginProc})
        if (p) {
            p->disconnect(this);
            Platform::killTree(p); // Windows: npm's claude.cmd runs node inside cmd
            p->waitForFinished(500);
        }
}

void Auth::refresh()
{
    if (m_statusProc)
        return;
    const QString exe = executable();
    if (exe.isEmpty()) {
        m_status = AuthStatus();
        m_status.checked = true;
        emit statusChanged();
        return;
    }
    auto *p = m_statusProc = new QProcess(this);
    p->setProcessEnvironment(environment(m_root));
    Platform::setCommand(p, exe, {QStringLiteral("auth"), QStringLiteral("status"), QStringLiteral("--json")}); // Windows: claude.cmd too
    QTimer::singleShot(20000, p, [p] { Platform::killTree(p); });
    connect(p, &QProcess::finished, this, [this, p] {
        AuthStatus s;
        s.checked = true;
        s.installed = true;
        const QByteArray out = p->readAllStandardOutput();
        const QJsonObject o = QJsonDocument::fromJson(out.mid(out.indexOf('{'))).object();
        if (o.isEmpty()) {
            s.error = stripAnsi(QString::fromUtf8(p->readAllStandardError() + out)).trimmed().left(400);
            if (s.error.isEmpty())
                s.error = tr("`claude auth status` gave no answer.");
        } else {
            s.loggedIn = o.value(QStringLiteral("loggedIn")).toBool();
            s.method = o.value(QStringLiteral("authMethod")).toString();
            s.email = o.value(QStringLiteral("email")).toString();
            s.org = o.value(QStringLiteral("orgName")).toString();
            s.plan = o.value(QStringLiteral("subscriptionType")).toString();
        }
        m_status = s;
        m_statusProc = nullptr;
        p->deleteLater();
        emit statusChanged();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return; // finished follows
        m_status = AuthStatus();
        m_status.checked = true;
        m_status.error = p->errorString();
        m_statusProc = nullptr;
        p->deleteLater();
        emit statusChanged();
    });
    p->start();
}

void Auth::login(bool console)
{
    if (m_loginProc)
        return;
    const QString exe = executable();
    if (exe.isEmpty()) {
        emit loginFinished(false, tr("Claude Code is not installed."));
        return;
    }
    m_loginOut.clear();
    m_url.clear();
    m_prompts = 0;
    m_cancelled = false;
    auto *p = m_loginProc = new QProcess(this);
    p->setProcessEnvironment(environment(m_root));
    p->setProcessChannelMode(QProcess::MergedChannels);
    Platform::setCommand(p, exe, {QStringLiteral("auth"), QStringLiteral("login"), console ? QStringLiteral("--console") : QStringLiteral("--claudeai")});
    connect(p, &QProcess::readyRead, this, &Auth::readLogin);
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        readLogin();
        const QString out = stripAnsi(QString::fromUtf8(m_loginOut));
        m_loginProc = nullptr;
        p->deleteLater();
        const bool ok = !m_cancelled && st == QProcess::NormalExit && code == 0;
        QString message;
        if (!ok && !m_cancelled) {
            message = tail(out, m_url);
            if (message.isEmpty())
                message = tr("Sign-in did not finish (exit code %1).").arg(code);
        }
        m_url.clear();
        emit loginFinished(ok, message);
        refresh();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        m_loginProc = nullptr;
        p->deleteLater();
        emit loginFinished(false, p->errorString());
    });
    p->start();
}

void Auth::readLogin()
{
    if (!m_loginProc)
        return;
    m_loginOut += m_loginProc->readAll();
    const QString out = stripAnsi(QString::fromUtf8(m_loginOut));
    if (m_url.isEmpty()) {
        static const QRegularExpression url(QStringLiteral("https://\\S+"));
        const QRegularExpressionMatch m = url.match(out);
        // The whole URL has arrived once the line it is on ends.
        if (m.hasMatch() && out.indexOf(QLatin1Char('\n'), m.capturedStart()) > 0) {
            m_url = m.captured();
            emit loginUrlReady(m_url);
        }
    }
    // A code the CLI cannot parse gets "Invalid code. …" and it waits for another one (no new prompt); a parsed but
    // wrong code ends the process with "Login failed: …".
    const int prompts = int(out.count(QLatin1String("Paste code"), Qt::CaseInsensitive) + out.count(QLatin1String("Invalid code"), Qt::CaseInsensitive));
    if (prompts > m_prompts) {
        m_prompts = prompts;
        emit loginWaitingForCode();
    }
}

void Auth::submitCode(const QString &code)
{
    if (!m_loginProc || code.trimmed().isEmpty())
        return;
    m_loginProc->write(code.trimmed().toUtf8() + '\n');
}

void Auth::cancelLogin()
{
    if (!m_loginProc)
        return;
    m_cancelled = true;
    Platform::killTree(m_loginProc);
}

void Auth::logout()
{
    const QString exe = executable();
    if (exe.isEmpty())
        return;
    auto *p = new QProcess(this);
    p->setProcessEnvironment(environment(m_root));
    connect(p, &QProcess::finished, this, [this, p] {
        p->deleteLater();
        refresh();
    });
    Platform::setCommand(p, exe, {QStringLiteral("auth"), QStringLiteral("logout")});
    p->start();
}

} // namespace claudecode
