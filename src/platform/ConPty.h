#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>

#include <thread>

class QProcess;

// Windows only (ConPtyWin.cpp): a pseudo console for the terminal, what the pty is on Unix. The shell is an ordinary
// QProcess that attach() makes start on this console instead of on QProcess's pipes; QODE writes keys to the console's
// input pipe and reads the VT output (UTF-8) a thread collects from its output pipe.
//
// One ConPty per shell start: close() ends it for good.
class ConPty : public QObject
{
    Q_OBJECT
public:
    explicit ConPty(QObject *parent = nullptr);
    ~ConPty() override; // close()

    // Creates the console. False with `error` set when Windows has no ConPTY (before Windows 10 1809) or it fails.
    bool open(int cols, int rows, QString *error);
    // Call before process->start(): replaces its create-process-arguments modifier.
    void attach(QProcess *process);

    void write(const QByteArray &data);
    void resize(int cols, int rows);
    // Output that arrived since the last call; readyRead() says there is some.
    QByteArray takeOutput();
    // Ends the console (programs still on it get CTRL_CLOSE_EVENT) and waits for the last output, so takeOutput()
    // afterwards holds all of it.
    void close();

signals:
    void readyRead();

private:
    void *m_console = nullptr; // HPCON
    void *m_input = nullptr; // our end of the input pipe
    void *m_output = nullptr; // our end of the output pipe
    std::thread m_reader;
    QMutex m_lock;
    QByteArray m_buffer; // guarded by m_lock
    bool m_notified = false; // guarded by m_lock: a readyRead() is on its way
};
