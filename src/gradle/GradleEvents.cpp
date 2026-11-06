#include "gradle/GradleEvents.h"
#include "platform/AppPaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace GradleEvents {

namespace {

QString cacheDir()
{
    const QString dir = AppPaths::cache(QStringLiteral("gradle"));
    QDir().mkpath(dir);
    return dir;
}

QString hashOf(const QString &root)
{
    return QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(root).toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
}

QString groovyString(QString s)
{
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('\''), QStringLiteral("\\'"));
    return QLatin1Char('\'') + s + QLatin1Char('\'');
}

} // namespace

bool Build::failed() const
{
    for (const Task &t : tasks)
        if (t.state == QLatin1String("FAILED"))
            return true;
    return false;
}

QString eventsFile(const QString &root)
{
    return cacheDir() + QStringLiteral("/events-") + hashOf(root) + QStringLiteral(".log");
}

QString initScript(const QString &root)
{
    const QString path = cacheDir() + QStringLiteral("/events-") + hashOf(root) + QStringLiteral(".init.gradle");
    const QByteArray body = QStringLiteral(
        "// Written by QODE: records the result and time of each task for the Gradle panel's Build tab. The project is untouched.\n"
        "import org.gradle.api.services.BuildService\n"
        "import org.gradle.api.services.BuildServiceParameters\n"
        "import org.gradle.build.event.BuildEventsListenerRegistry\n"
        "import org.gradle.tooling.events.FinishEvent\n"
        "import org.gradle.tooling.events.OperationCompletionListener\n"
        "import org.gradle.tooling.events.task.TaskFailureResult\n"
        "import org.gradle.tooling.events.task.TaskFinishEvent\n"
        "import org.gradle.tooling.events.task.TaskSkippedResult\n"
        "import org.gradle.tooling.events.task.TaskSuccessResult\n"
        "\n"
        "abstract class QodeTaskEvents implements BuildService<QodeTaskEvents.Params>, OperationCompletionListener, AutoCloseable {\n"
        "    interface Params extends BuildServiceParameters {\n"
        "        Property<String> getFile()\n"
        "    }\n"
        "    void onFinish(FinishEvent event) {\n"
        "        if (!(event instanceof TaskFinishEvent)) return\n"
        "        def r = event.result\n"
        "        String state = 'EXECUTED'\n"
        "        String detail = ''\n"
        "        if (r instanceof TaskSkippedResult) { state = 'SKIPPED'; detail = r.skipMessage ?: '' }\n"
        "        else if (r instanceof TaskFailureResult) { state = 'FAILED'; detail = r.failures.collect { it.message ?: '' }.join(' | ') }\n"
        "        else if (r instanceof TaskSuccessResult) { state = r.fromCache ? 'FROM-CACHE' : r.upToDate ? 'UP-TO-DATE' : 'EXECUTED' }\n"
        "        detail = detail.replace('\\n', ' ').replace('\\t', ' ')\n"
        "        new File(parameters.file.get()) << \"task\\t${event.descriptor.taskPath}\\t${state}\\t${r.startTime}\\t${r.endTime}\\t${detail}\\n\"\n"
        "    }\n"
        "    void close() {\n"
        "        new File(parameters.file.get()) << \"end\\t${System.currentTimeMillis()}\\n\"\n"
        "    }\n"
        "}\n"
        "\n"
        "def qodeEvents = %1\n"
        "new File(qodeEvents).text = \"start\\t${System.currentTimeMillis()}\\n\"\n"
        "def qodeService = gradle.sharedServices.registerIfAbsent('qodeTaskEvents', QodeTaskEvents) { it.parameters.file.set(qodeEvents) }\n"
        "gradle.services.get(BuildEventsListenerRegistry).onTaskCompletion(qodeService)\n")
                                .arg(groovyString(eventsFile(root)))
                                .toUtf8();
    QFile cur(path);
    if (!cur.open(QIODevice::ReadOnly) || cur.readAll() != body) {
        cur.close();
        QSaveFile out(path);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(body);
            out.commit();
        }
    }
    return path;
}

bool runsGradle(const QString &command)
{
    static const QRegularExpression re(QStringLiteral(R"((?:^|[\s;&|(])(?:sh\s+)?(?:\./gradlew|\.\\gradlew|gradlew|gradle)(?=\s|$))"));
    return re.match(command).hasMatch();
}

QString inject(const QString &command, const QString &root)
{
    if (root.isEmpty() || (command.contains(QLatin1String("--init-script ")) && command.contains(QLatin1String("/events-"))))
        return command;
    static const QRegularExpression re(QStringLiteral(R"((?:^|[\s;&|(])(?:sh\s+)?(?:\./gradlew|\.\\gradlew|gradlew|gradle)(?=\s|$))"));
    const QRegularExpressionMatch m = re.match(command);
    if (!m.hasMatch())
        return command;
    QString script = initScript(root);
    script.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    QString out = command;
    out.insert(m.capturedEnd(), QStringLiteral(" --init-script '%1'").arg(script));
    return out;
}

Build parseLast(const QString &text)
{
    Build b;
    bool ended = false;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char('\t'));
        if (f.first() == QLatin1String("start")) {
            b = Build();
            b.start = f.value(1).toLongLong();
            ended = false;
        } else if (f.first() == QLatin1String("end")) {
            b.end = f.value(1).toLongLong();
            ended = true;
        } else if (f.first() == QLatin1String("task") && f.size() >= 5) {
            if (ended) {
                b = Build();
                ended = false;
            }
            Task t;
            t.path = f.at(1);
            t.state = f.at(2);
            t.start = f.at(3).toLongLong();
            t.end = f.at(4).toLongLong();
            t.detail = f.value(5);
            if (b.start == 0 || t.start < b.start)
                b.start = t.start;
            b.tasks << t;
        }
    }
    return b;
}

} // namespace GradleEvents
