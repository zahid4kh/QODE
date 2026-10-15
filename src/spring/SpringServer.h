#pragma once

#include "webdev/DevServer.h"

#include <QElapsedTimer>

// A Spring Boot application started through Gradle (bootRun) or Maven (spring-boot:run). Same process handling as
// DevServer (own process group, SIGTERM then SIGKILL); what differs is how it reads the output: Running begins at the
// embedded server's "started on port N" line, a failed start (port in use, build error) gives a readable reason.
class SpringServer : public DevServer
{
    Q_OBJECT
public:
    enum class Phase { Idle, Building, Starting, Up };

    explicit SpringServer(QObject *parent = nullptr);

    Phase phase() const { return m_phase; }
    // "Building…" / "Starting…" while State::Starting.
    QString phaseText() const;
    // Seconds Spring reported for "Started XApplication in N seconds", 0 until then.
    double startupSeconds() const { return m_startup; }
    // Milliseconds since the application reported it was up; 0 when not running.
    qint64 uptimeMs() const;

protected:
    void scan(const QString &line) override;
    void adjustEnvironment(QProcessEnvironment &env) override;

private:
    void setPhase(Phase p);

    Phase m_phase = Phase::Idle;
    double m_startup = 0;
    QElapsedTimer m_up;
    bool m_seenState = false;
};
