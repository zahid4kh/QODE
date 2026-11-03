#pragma once

#include "terminal/ProblemScanner.h"

#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTimer;
class QToolButton;
class ShellProcess;
class TerminalView;

// A command that ended with an error, as the shell integration reported it (ShellIntegration.h).
struct FailedCommand {
    QString command;   // the command line ("" when the shell did not say)
    int exitCode = 0;
    QString output;    // what it printed, as the screen shows it (long output: its start and end)
    QString folder;    // where it ran
    QList<BuildProblem> problems; // compiler errors / warnings found in its output
};

// Bottom panel: a header strip plus a terminal view wired to the user's shell.
class Terminal : public QWidget
{
    Q_OBJECT
public:
    explicit Terminal(QWidget *parent = nullptr);

    // The panel with several sessions supplies its own header.
    void setHeaderVisible(bool on);
    QString shellName() const;
    void setWorkingDirectory(const QString &dir);
    QString workingDirectory() const { return m_cwd; }

    // Asked each time a fresh shell starts (argument: shell name); a non-empty result is typed in first.
    void setStartupCommandProvider(std::function<QString(const QString &)> provider) { m_startupProvider = std::move(provider); }

    // Starts the shell if it is not running yet (never runs any project command itself).
    void ensureStarted();
    // Types `command` into the shell (interrupting whatever is running) so its output shows in the panel.
    void runCommand(const QString &command);
    // Writes raw input (a key press such as "j" for the program running in the shell).
    void sendInput(const QByteArray &data);
    void restart();
    void stop();
    bool isRunning() const;
    void clear();
    void focusTerminal();
    // For the chat's terminal_output: the last `lines` lines of the screen and scrollback (trailing blank lines left out),
    // whether a command runs in the foreground, and the shell's current folder.
    QString recentOutput(int lines) const;
    bool commandRunning() const { return isRunning() && !shellInForeground(); }
    QString currentDirectory() const;

signals:
    void hideRequested();
    void shellExited();  // the shell ended by itself (user typed exit, crash)
    void focused();      // the terminal view got keyboard focus
    void openFileRequested(const QString &path, int line, int column); // a file location in the output was Ctrl+clicked
    // Compiler errors / warnings found in the output; cleared when a new command starts (Enter at the prompt, runCommand).
    void problemsFound(const QList<BuildProblem> &problems);
    void problemsCleared();
    // "Fix with QODE" was pressed on the last command's failure.
    void fixRequested(const FailedCommand &failure);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void startShell();
    void onFinished(int exitCode);
    void flushPendingCommand();
    void updateHeader();
    void newCommand();
    bool shellInForeground() const;
    void onShellMark(const QString &mark);
    void commandEnded(int exitCode);
    void setFailure(const FailedCommand &failure); // exitCode 0 = none: hides the button
    void placeFixButton();

    ShellProcess *m_shell;
    TerminalView *m_view;
    QLabel *m_title;
    QWidget *m_header = nullptr;
    QToolButton *m_restartBtn;
    QString m_cwd;
    std::function<QString(const QString &)> m_startupProvider;
    QString m_pendingCommand;
    QTimer *m_pendingTimer;
    bool m_booting = false;      // shell started, startup output not yet quiet
    bool m_expectedExit = false; // we terminated it (stop / restart)
    ProblemScanner m_problems;
    bool m_hasProblems = false;
    // The command running according to the shell integration: where its output starts, its line, its problems.
    bool m_inCommand = false;
    qint64 m_commandStart = 0;
    QString m_commandText;
    QString m_commandFallback;    // when the shell sends no command line: what runCommand typed, or the screen's
    QString m_written;            // the last command runCommand typed
    qint64 m_promptLine = -1;     // where the prompt ended (the first key typed after a command ended), else -1
    int m_promptCol = 0;
    bool m_awaitPrompt = true;
    QList<BuildProblem> m_commandProblems;
    FailedCommand m_failure;
    QPushButton *m_fixBtn;
};
