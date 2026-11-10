#pragma once

#include "terminal/Terminal.h"

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QMenu;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QToolButton;
class Terminal;

// The bottom panel: a tab strip of sessions. Each tab is a splitter holding one or more shell panes
// (`Terminal`), which can be split right or down. Tabs can be renamed; the list of tabs (with the names the user
// gave) is reported through `tabsChanged` so it can be saved per project.
class TerminalPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalPanel(QWidget *parent = nullptr);

    void setWorkingDirectory(const QString &dir); // for new and restarted sessions
    QString workingDirectory() const { return m_cwd; }
    void setStartupCommandProvider(std::function<QString(const QString &)> provider);
    // Tabs to create the next time the panel starts with no sessions: one entry per tab, "" = automatic name.
    void setSavedTabs(const QStringList &names) { m_saved = names; }
    QStringList tabNames() const; // custom names, "" = automatic

    // Starts the current session's shell (creating the saved / first session when there is none).
    void ensureStarted();
    // Types `command` into the current pane (interrupting whatever is running there).
    void runCommand(const QString &command);
    // Writes raw input to the current pane (a key for the program running there).
    void sendInput(const QByteArray &data);
    // The shell file of the current pane ("bash", "pwsh.exe" …, Terminal::shellFile), else of the shell new tabs get.
    QString currentShellFile() const;
    void newSession();
    void newSessionWith(const QString &program); // a tab running `program` ("" = the default shell)
    // The shell new tabs run ("" or a program that is gone = ShellProcess::defaultShell()).
    void setDefaultShell(const QString &program);
    void splitCurrent(Qt::Orientation orientation);
    void restart();    // the current pane
    void restartAll(); // every running pane (the project changed)
    void stop();       // every pane
    bool isRunning() const;
    void clear();
    void focusTerminal();
    int sessionCount() const;
    // Every pane with its tab's title, the current one marked (the chat's terminal_output).
    struct Pane {
        QString tab;
        Terminal *terminal = nullptr;
        bool current = false;
    };
    QVector<Pane> panes() const;

signals:
    void hideRequested();
    void maximizeToggled(bool maximized);
    void tabsChanged(const QStringList &names);
    void openFileRequested(const QString &path, int line, int column); // Ctrl+click on a file location in a pane's output
    // Compiler errors / warnings printed in a pane (`source`), and their end when it starts a new command or closes.
    void problemsFound(QObject *source, const QList<BuildProblem> &problems);
    void problemsCleared(QObject *source);
    void fixRequested(const FailedCommand &failure); // a pane's "Fix with QODE"
    void defaultShellChanged(const QString &program); // chosen in the + menu (Windows)

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Terminal *current() const;
    QSplitter *pageAt(int index) const;
    QSplitter *pageOf(QWidget *w) const;
    QList<Terminal *> panesOf(QSplitter *page) const;
    Terminal *createPane(const QString &program);
    int createSession(const QString &customName, const QString &program = QString());
    void fillShellMenu(QMenu *menu);
    void requestCloseSession(int index); // asks first when a command still runs in one of the tab's panes
    bool confirmClose(const QList<Terminal *> &panes);
    void closeSession(int index);
    void closePane(Terminal *t);
    void renameTab(int index);
    void tabMenu(const QPoint &pos);
    void refreshTabs();
    void notifyTabs();

    QTabBar *m_tabs;
    QStackedWidget *m_stack;
    QToolButton *m_maxBtn;
    QString m_cwd;
    QString m_defaultShell;
    QStringList m_saved;
    std::function<QString(const QString &)> m_startupProvider;
    QHash<QSplitter *, QPointer<Terminal>> m_active; // last focused pane of each tab
    bool m_restoring = false;
};
