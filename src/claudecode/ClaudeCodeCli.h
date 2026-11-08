#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QString>

class QProcess;

// The `claude` command line tool (Claude Code) that the Claude Code view drives: where it is, the environment it runs
// in, and its sign-in (`claude auth status / login / logout`). Claude Code keeps its own credentials (~/.claude); QODE
// never reads them. See dev-docs/claude-code.md.
namespace claudecode {

// The executable: PATH first, then where the installers put it (a desktop launch often lacks ~/.local/bin in PATH).
// Empty when Claude Code is not installed.
QString executable();
// The install command shown when it is missing.
QString installCommand();
QString installHint(); // rich text for the "not installed" page (Windows: also when Git for Windows is missing)

// How Claude Code signs in: with the account it is logged in with (Claude subscription or Console), or with the Claude
// API key QODE already has (`claude.key` / ANTHROPIC_API_KEY), handed over in the environment only.
enum class AuthMode { Account, ApiKey };
AuthMode authMode();
void setAuthMode(AuthMode mode);

// The environment for every `claude` process: ~/.local/bin on PATH, and the API key only in ApiKey mode (an
// ANTHROPIC_API_KEY inherited from the shell would otherwise override the account the user picked).
QProcessEnvironment environment(const QString &projectRoot);

struct AuthStatus {
    bool checked = false;   // false until the first `claude auth status` came back
    bool installed = false;
    bool loggedIn = false;
    QString method;         // "claude.ai", "console", "api_key", …
    QString email, org, plan;
    QString error;          // the status could not be read
};

// Sign-in state and the login flow. `claude auth login` without a terminal prints the sign-in URL and then waits for
// the code the browser shows ("Paste code here if prompted >"): the view shows the link and a field for that code,
// which goes to the process's stdin.
class Auth : public QObject
{
    Q_OBJECT
public:
    explicit Auth(QObject *parent = nullptr);
    ~Auth() override;

    const AuthStatus &status() const { return m_status; }
    void setProjectRoot(const QString &root) { m_root = root; }
    void refresh();                       // async; statusChanged when done
    bool refreshing() const { return m_statusProc != nullptr; }

    void login(bool console);             // console: Anthropic Console (API billing) instead of a Claude subscription
    bool loggingIn() const { return m_loginProc != nullptr; }
    QString loginUrl() const { return m_url; }
    void submitCode(const QString &code);
    void cancelLogin();
    void logout();

signals:
    void statusChanged();
    void loginUrlReady(const QString &url);
    void loginWaitingForCode(); // again after a code that was not accepted
    void loginFinished(bool ok, const QString &message);

private:
    void readLogin();

    AuthStatus m_status;
    QString m_root;
    QProcess *m_statusProc = nullptr;
    QProcess *m_loginProc = nullptr;
    QByteArray m_loginOut;
    QString m_url;
    int m_prompts = 0; // "Paste code" prompts seen
    bool m_cancelled = false;
};

} // namespace claudecode
