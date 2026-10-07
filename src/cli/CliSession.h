#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <functional>

class ShellProcess;

// A persistent bash/zsh behind Terminal Only mode. The shell runs on a pty without line editing and
// with a prompt that only prints OSC 133 markers, so QODE sees exactly when the shell is ready for a
// command and how the previous one ended. QODE owns the input line; a command line is written to the
// pty only when the user submits it, and while it runs every key goes to the program unchanged.
class CliSession : public QObject
{
    Q_OBJECT
public:
    enum class State { Stopped, Booting, Idle, Running };

    explicit CliSession(QObject *parent = nullptr);
    ~CliSession() override;

    // Starts a shell in `directory` (does nothing when already running). Returns false with `error` set on failure.
    bool start(const QString &directory, QString *error = nullptr);
    void stop();

    State state() const { return m_state; }
    QString cwd() const { return m_cwd; }
    QString shellName() const { return m_shellName; }
    int lastExitCode() const { return m_lastExit; }

    // Runs one command line. While the shell is still starting it is queued.
    void run(const QString &line);
    // Runs a line without a block of its own (used by /cd); `done` receives the exit code and any output.
    void runHidden(const QString &line, std::function<void(int, const QString &)> done);
    void write(const QByteArray &bytes); // raw keys / pastes for the running program
    void interrupt();                    // Ctrl+C
    void resize(int cols, int rows);

    static QString quote(const QString &path); // POSIX single-quote escaping

signals:
    void stateChanged();
    void output(const QByteArray &data);                   // output of the running command
    void commandFinished(int exitCode, qint64 milliseconds);
    void startupOutput(const QString &text);               // anything the shell printed before its first prompt
    void backgroundOutput(const QByteArray &data);         // output nobody asked for: background jobs, prompt hooks, typed-ahead lines
    void cwdChanged(const QString &directory);
    void reply(const QByteArray &data);
    void exited(int exitCode);

private:
    void onData(const QByteArray &data);
    void plain(const QByteArray &bytes);
    void marker(const QByteArray &body);
    void setState(State s);
    bool prepareLaunch(QString *program, QStringList *args, QString *error);

    ShellProcess *m_shell;
    State m_state = State::Stopped;
    QString m_cwd;
    QString m_shellName;
    QByteArray m_carry;      // incomplete marker at the end of the previous chunk
    QByteArray m_startup;
    QByteArray m_echoSkip;   // the pty echoes the line we typed; it is dropped, not shown
    QString m_queued;
    QElapsedTimer m_clock;
    int m_cols = 80, m_rows = 24;
    int m_lastExit = 0;
    bool m_expectedExit = false;
    bool m_hidden = false;
    QByteArray m_hiddenOutput;
    std::function<void(int, const QString &)> m_hiddenDone;
};
