#pragma once

#include <QWidget>

#include <functional>

class QStackedWidget;
class QTabBar;
class QToolButton;
class Terminal;

// The bottom panel: a tab strip of independent shell sessions (each a `Terminal`) with shared controls.
class TerminalPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalPanel(QWidget *parent = nullptr);

    void setWorkingDirectory(const QString &dir); // for new and restarted sessions
    QString workingDirectory() const { return m_cwd; }
    void setStartupCommandProvider(std::function<QString(const QString &)> provider);

    // Starts the current session's shell (creating the first session when there is none).
    void ensureStarted();
    // Types `command` into the current session (interrupting whatever is running there).
    void runCommand(const QString &command);
    void newSession();
    void restart();    // the current session
    void restartAll(); // every running session (the project changed)
    void stop();       // every session
    bool isRunning() const;
    void clear();
    void focusTerminal();
    int sessionCount() const;

signals:
    void hideRequested();

private:
    Terminal *current() const;
    Terminal *createSession();
    void closeSession(int index);
    void refreshTabs();

    QTabBar *m_tabs;
    QStackedWidget *m_stack;
    QToolButton *m_restartBtn;
    QString m_cwd;
    std::function<QString(const QString &)> m_startupProvider;
    int m_counter = 0;
};
