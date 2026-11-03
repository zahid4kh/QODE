#pragma once

#include <QList>
#include <QString>

// Task timings of the Gradle builds QODE starts. An init script in ~/.cache/QODE-AGENTIC/gradle/ registers a build
// service that listens to Gradle's task completion events (BuildEventsListenerRegistry, works with the configuration
// cache) and appends one line per task to an events file of the project; the Gradle panel's Build tab reads it. The
// terminal output stays as it is. Qt Core only.
namespace GradleEvents {

struct Task {
    QString path;        // :app:compileKotlin
    QString state;       // EXECUTED, UP-TO-DATE, FROM-CACHE, SKIPPED, FAILED
    qint64 start = 0, end = 0; // ms since the epoch
    QString detail;      // skip reason / failure message
    qint64 ms() const { return qMax<qint64>(0, end - start); }
};

struct Build {
    qint64 start = 0, end = 0; // end = 0 while it runs
    QList<Task> tasks;
    bool failed() const;
};

QString eventsFile(const QString &root);
// Writes (when changed) and returns the init script of the project.
QString initScript(const QString &root);
// `command` with `--init-script <script>` after its Gradle runner (./gradlew, gradlew, sh gradlew, gradle); unchanged when
// the command does not run Gradle or already names the script.
QString inject(const QString &command, const QString &root);
bool runsGradle(const QString &command);
// The last build in the events file text (a task line after an "end" starts a new one: a configuration cache hit runs
// no init script, so no "start" line comes).
Build parseLast(const QString &text);

} // namespace GradleEvents
