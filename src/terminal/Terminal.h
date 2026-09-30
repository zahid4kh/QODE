#pragma once

#include <QWidget>

#include <functional>

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

    // Asked each time a fresh shell starts (argument: shell name); a non-empty result is typed in first.
    void setStartupCommandProvider(std::function<QString(const QString &)> provider) { m_startupProvider = std::move(provider); }

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
    std::function<QString(const QString &)> m_startupProvider;
    QString m_pendingCommand;
    QTimer *m_pendingTimer;
    bool m_awaitingPrompt = false;
};
