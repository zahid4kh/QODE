#pragma once

#include <QWidget>

class QLabel;
class QTimer;
class QToolButton;
class ShellProcess;
class TerminalView;

// Bottom panel: a header strip plus a terminal view wired to the user's shell.
class Terminal : public QWidget
{
    Q_OBJECT
public:
    explicit Terminal(QWidget *parent = nullptr);

    void setWorkingDirectory(const QString &dir);
    QString workingDirectory() const { return m_cwd; }

    // Starts the shell if it is not running yet (never runs any project command itself).
    void ensureStarted();
    // Types `command` into the shell (interrupting whatever is running) so its output shows in the panel.
    void runCommand(const QString &command);
    void restart();
    void stop();
    bool isRunning() const;
    void clear();
    void focusTerminal();

signals:
    void hideRequested();

private:
    void startShell();
    void onFinished(int exitCode);
    void flushPendingCommand();
    void updateHeader();

    ShellProcess *m_shell;
    TerminalView *m_view;
    QLabel *m_title;
    QToolButton *m_restartBtn;
    QString m_cwd;
    QString m_pendingCommand;
    QTimer *m_pendingTimer;
    bool m_awaitingPrompt = false;
};
