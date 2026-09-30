#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>

class QSocketNotifier;
class QTimer;

// Runs the user's shell attached to a pseudo-terminal. The child is a normal QProcess;
// a child-process modifier makes it a session leader on the pty slave, and we talk to
// the master side directly.
class ShellProcess : public QObject
{
    Q_OBJECT
public:
    explicit ShellProcess(QObject *parent = nullptr);
    ~ShellProcess() override;

    static QString defaultShell();

    bool start(const QString &workingDirectory, int cols, int rows, QString *error);
    void write(const QByteArray &data);
    void resize(int cols, int rows);
    void terminate(); // SIGHUP via closing the pty, SIGKILL if it lingers
    bool isRunning() const;
    QString shell() const { return m_shell; }
    qint64 pid() const;

signals:
    void output(const QByteArray &data);
    void finished(int exitCode);

private:
    void onReadable();
    void onWritable();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void closeMaster();

    QProcess *m_process;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QTimer *m_killTimer;
    QByteArray m_writeBuffer;
    QString m_shell;
    int m_master = -1;
};
