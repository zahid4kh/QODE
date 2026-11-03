#pragma once

#include "webdev/DevServer.h"

#include <QElapsedTimer>

// A Ktor server started through Gradle (run) or Maven (exec:java). Same process handling as DevServer (own process
// group, SIGTERM then SIGKILL); what differs is how it reads the output: Running begins at Ktor's "Responding at
// http://host:port" line, a failed start (port in use, build error, missing module) gives a readable reason, and while it
// runs the auto-reload lines of development mode and the JVM's debug port are followed.
class KtorServer : public DevServer
{
    Q_OBJECT
public:
    enum class Phase { Idle, Building, Starting, Up };

    explicit KtorServer(QObject *parent = nullptr);

    Phase phase() const { return m_phase; }
    // "Building…" / "Starting…" while State::Starting.
    QString phaseText() const;
    // KEY=VALUE entries added to the environment of the next start.
    void setExtraEnvironment(const QStringList &entries) { m_extraEnv = entries; }
    // Seconds Ktor reported for "Application started in N seconds", 0 until then.
    double startupSeconds() const { return m_startup; }
    // Milliseconds since the server answered; 0 when not running.
    qint64 uptimeMs() const;
    // Development mode as the server reported it (its "Autoreload is disabled because the development mode is off" line).
    bool developmentMode() const { return m_development; }
    // The JDWP port the server's JVM listens on ("Listening for transport dt_socket at address: N"), 0 without one.
    int debugPort() const { return m_debugPort; }
    // Why Ktor will not reload changed classes in place, from what it printed at start ("development mode is off", "no
    // watch paths" …); "" when nothing said so.
    QString reloadBlocker() const { return m_reloadBlocker; }
    // A reload is under way: Ktor saw changed classes and is creating the application again.
    bool isReloading() const { return m_reloading; }
    // Seconds of the last "Application auto-reloaded in N seconds", 0 before the first.
    double lastReloadSeconds() const { return m_lastReload; }

signals:
    void reloadStarted();               // "Changes in application detected."
    void reloaded(double seconds);      // "Application auto-reloaded in N seconds."
    void reloadFailed();                // "Application reload failed." / "Auto-reload failed."

protected:
    void scan(const QString &line) override;
    void adjustEnvironment(QProcessEnvironment &env) override;

private:
    void setPhase(Phase p);
    void scanReload(const QString &line);

    QStringList m_extraEnv;
    Phase m_phase = Phase::Idle;
    double m_startup = 0;
    bool m_development = false;
    bool m_reloading = false;
    int m_debugPort = 0;
    double m_lastReload = 0;
    QString m_reloadBlocker;
    QElapsedTimer m_up;
};
