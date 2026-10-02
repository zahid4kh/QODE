#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class QTimer;

// One dev server (`npm run dev`, `bun dev`, ...) running as a child of QODE in its own process group, so stopping it
// also stops the node/bun processes it spawned. The port is read from what the server prints.
class DevServer : public QObject
{
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Running, Failed };

    explicit DevServer(QObject *parent = nullptr);
    ~DevServer() override;

    // `port` > 0 is also exported as $PORT (the flag, if any, is already part of the command).
    void start(const QString &command, const QString &workDir, int port);
    void stop();
    void restart();

    State state() const { return m_state; }
    bool isActive() const { return m_state == State::Starting || m_state == State::Running; }
    int port() const { return m_port; }
    QString url() const;
    QString command() const { return m_command; }
    QString errorString() const { return m_error; }
    QString log() const { return m_log; }

signals:
    void stateChanged();
    void output(const QString &text);

private:
    void onOutput();
    void onFinished(int code, QProcess::ExitStatus status);
    void scan(const QString &line);
    void setState(State s);
    void signalGroup(int sig);

    QProcess *m_proc;
    QTimer *m_killTimer;
    State m_state = State::Stopped;
    QString m_command, m_workDir, m_error, m_log, m_pending;
    int m_port = 0, m_wantedPort = 0;
    bool m_stopping = false, m_restart = false;
};
