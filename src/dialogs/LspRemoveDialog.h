#pragma once

#include <QDialog>
#include <QList>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;

// "Remove language server": shows exactly what goes away and runs it with a visible log.
// - A server QODE downloaded itself (Kotlin, Java) is deleted step by step (each command and its result is shown).
// - The web servers (installed with npm into one private folder) are deleted the same way.
// - A system package (clangd) needs administrator rights, so the dialog shows the package manager commands and can
//   type them into QODE's terminal, where the user enters the password.
// Either way the dialog ends by asking to restart QODE (restartRequested).
class LspRemoveDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LspRemoveDialog(const QString &serverId, QWidget *parent = nullptr);

signals:
    void removalStarting();                           // stop the running server before its files go away
    void terminalCommandRequested(const QString &cmd); // run this in QODE's terminal
    void restartRequested();

protected:
    void reject() override; // not while files are being deleted

private:
    struct Step {
        QString text;    // what it does
        QString display; // the shell command shown in the log
        QString path;    // what `rm -rf` deletes ("" = not a delete step)
        bool onlyLink = false; // a symlink: `rm` instead of `rm -rf`
    };

    void buildManaged();
    void buildNpm();
    void buildSystem();
    void startManaged();
    void runNext();
    void finish(bool ok);
    void log(const QString &line, const QString &color = QString());
    void askRestart(const QString &message);

    QString m_id;
    QLabel *m_intro, *m_note;
    QCheckBox *m_library = nullptr;
    QPlainTextEdit *m_view; // the plan / commands, then the live log
    QPushButton *m_primary, *m_secondary, *m_close, *m_restart;
    QList<Step> m_steps;
    int m_next = 0;
    bool m_failed = false;
    bool m_running = false;
    QString m_command; // clangd: the command for the terminal
    QProcess *m_proc = nullptr;
};
