#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QProcess;

// What the slash commands need from the window showing them; implemented by CliView.
class CliHost
{
public:
    virtual ~CliHost() = default;
    virtual QString projectRoot() const = 0;
    virtual QString projectName() const = 0;
    virtual QString cwd() const = 0;
    virtual QString shellName() const = 0;
    virtual int columns() const = 0;
    virtual QStringList history() const = 0;
    virtual void clearHistory() = 0;
    virtual void clearScreen() = 0;
    virtual void restartShell() = 0;
    virtual void changeDirectory(const QString &directory, std::function<void(int, const QString &)> done) = 0;
    virtual void leaveMode() = 0;
    virtual void openInEditor(const QString &path, int line) = 0;
    virtual QString toggleShortcut() const = 0; // text of the shortcut that leaves the mode
};

// One running slash command. Output is styled text (ANSI sequences); `done()` ends it. Commands that
// finish immediately just return; ones that wait for a process or a worker call beginAsync() first.
class CliCall : public QObject
{
    Q_OBJECT
public:
    CliCall(CliHost &host, const QString &name, const QStringList &args, QObject *parent = nullptr);

    CliHost &host;
    const QString name;
    const QStringList args;

    QString cwd() const { return host.cwd(); }
    QString root() const { return host.projectRoot(); }
    int columns() const { return host.columns(); }
    // Relative paths are taken from the shell's directory; "~" is the home folder.
    QString resolve(const QString &argument) const;

    void print(const QString &ansi);          // "\n" is converted for the terminal
    void println(const QString &ansi = {});
    void error(const QString &message);       // red text; the command fails
    void done(int status = -1);               // -1 keeps the status error() set
    void beginAsync() { m_async = true; }
    bool isAsync() const { return m_async; }
    bool isFinished() const { return m_finished; }
    bool isCanceled() const { return m_canceled; }
    int status() const { return m_status; }
    void cancel();                            // Ctrl+C

    // Runs a program with merged output shown as it arrives; stops after `maxLines` lines (0 = unlimited).
    void runProcess(const QString &program, const QStringList &arguments, const QString &workDir, int maxLines = 0);
    // Runs `work` on another thread; its result is printed and the command ends.
    void background(std::function<QString()> work);

signals:
    void output(const QString &text);
    void finished(int status);

private:
    QPointer<QProcess> m_process;
    bool m_async = false;
    bool m_finished = false;
    bool m_canceled = false;
    int m_status = 0;
};

struct CliCandidate {
    QString text;   // what replaces the word being completed
    QString label;  // shown in the list
    QString detail; // dim text after the label
    bool directory = false;
};

struct CliCompletion {
    int start = 0;                 // index in the line where the replaced word begins
    QVector<CliCandidate> items;
};

struct CliCommand {
    enum class Arg { None, Path, Directory, Command, Theme };

    QString name;
    QString group;    // heading in /help
    QStringList aliases;
    QString usage;    // e.g. "/ls [-a] [-l] [path]"
    QString summary;  // one line, shown in the list and /help
    QString help;     // longer text for /help <command> (may use ANSI)
    Arg arg = Arg::None;
    int argFrom = 0;  // first argument index that `arg` applies to
    std::function<void(CliCall &)> run;
};

class CliCommands
{
public:
    static const CliCommands &instance();

    const CliCommand *find(const QString &name) const; // by name or alias, without the "/"
    const QList<CliCommand> &all() const { return m_commands; }

    // Tab completion for a line the user is typing (cursor at `cursor`).
    CliCompletion complete(const QString &line, int cursor, const QString &cwd) const;
    // Shell lines: executables for the first word, paths afterwards.
    static CliCompletion completeShell(const QString &line, int cursor, const QString &cwd);
    static CliCompletion completePath(const QString &line, int wordStart, int cursor, const QString &cwd, bool directoriesOnly);

    // Splits like a shell: quotes group, backslash escapes.
    static QStringList splitArgs(const QString &line);

private:
    CliCommands();
    QList<CliCommand> m_commands;
};
