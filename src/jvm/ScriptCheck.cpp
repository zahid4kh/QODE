#include "jvm/ScriptCheck.h"
#include "platform/AppPaths.h"

#include "jvm/JvmTools.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

ScriptCheck::ScriptCheck(QObject *parent)
    : QObject(parent)
{
}

ScriptCheck::~ScriptCheck()
{
    for (QProcess *p : std::as_const(m_running)) {
        p->disconnect(this);
        p->kill();
        p->waitForFinished(500);
        delete p;
    }
}

bool ScriptCheck::available()
{
    return !QStandardPaths::findExecutable(QStringLiteral("kotlinc")).isEmpty();
}

void ScriptCheck::check(const QString &path)
{
    if (!available() || !JvmTools::isKotlinScript(path))
        return;
    if (m_running.contains(path)) {
        m_again.insert(path);
        return;
    }
    run(path);
}

void ScriptCheck::forget(const QString &path)
{
    m_results.remove(path);
    m_again.remove(path);
    if (QProcess *p = m_running.take(path)) {
        p->disconnect(this);
        p->kill();
        p->deleteLater();
    }
}

void ScriptCheck::run(const QString &path)
{
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    const QString out = AppPaths::cache(QStringLiteral("kts/check-")) + hash;
    QDir().mkpath(out);
    QStringList args;
    if (JvmTools::isMainKts(path)) {
        const QString jar = JvmTools::mainKtsJar();
        if (!jar.isEmpty())
            args << QStringLiteral("-cp") << jar;
    }
    args << path << QStringLiteral("-d") << out;
    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(QFileInfo(path).absolutePath());
    m_running.insert(path, proc);
    emit started(path);
    auto *timeout = new QTimer(proc);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, proc, [proc] { proc->kill(); });
    timeout->start(180000);
    connect(proc, &QProcess::finished, this, [this, proc, path] {
        const QString output = QString::fromUtf8(proc->readAll());
        m_running.remove(path);
        proc->deleteLater();
        const QVector<LspDiagnostic> list = parse(output, path);
        m_results.insert(path, list);
        int errors = 0, warnings = 0;
        for (const LspDiagnostic &d : list)
            (d.severity == LspDiagnostic::Error ? errors : warnings)++;
        emit checked(path, errors, warnings);
        if (m_again.remove(path))
            run(path);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, path](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        m_running.remove(path);
        proc->deleteLater();
    });
    proc->start(QStringLiteral("kotlinc"), args);
}

QVector<LspDiagnostic> ScriptCheck::parse(const QString &output, const QString &path)
{
    QVector<LspDiagnostic> out;
    static const QRegularExpression located(QStringLiteral(R"(^(.+?):(\d+):(\d+): (error|warning|info): (.*)$)"));
    static const QRegularExpression general(QStringLiteral(R"(^(error): (.*)$)")); // general warnings are about the class path, not the script
    const QString name = QFileInfo(path).fileName();
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        LspDiagnostic d;
        d.source = QStringLiteral("kotlinc");
        d.server = QStringLiteral("kotlinc");
        if (const QRegularExpressionMatch m = located.match(line); m.hasMatch()) {
            const QString file = m.captured(1);
            if (file != path && QFileInfo(file).fileName() != name)
                continue;
            d.startLine = d.endLine = m.captured(2).toInt() - 1;
            d.startColumn = m.captured(3).toInt() - 1;
            d.endColumn = d.startColumn + 1;
            // The caret line under the quoted code marks how far the problem reaches ("^^^^").
            if (i + 2 < lines.size()) {
                const QString carets = lines.at(i + 2);
                const int first = int(carets.indexOf(QLatin1Char('^')));
                if (first == d.startColumn)
                    d.endColumn = first + qMax(1, int(carets.lastIndexOf(QLatin1Char('^'))) - first + 1);
            }
            d.severity = m.captured(4) == QLatin1String("error") ? LspDiagnostic::Error
                         : m.captured(4) == QLatin1String("warning") ? LspDiagnostic::Warning
                                                                     : LspDiagnostic::Information;
            d.message = m.captured(5).trimmed();
            out << d;
        } else if (const QRegularExpressionMatch g = general.match(line); g.hasMatch()) {
            if (g.captured(2).startsWith(QLatin1String("compilation failed")))
                continue;
            d.severity = LspDiagnostic::Error;
            d.endColumn = 1;
            d.message = g.captured(2).trimmed();
            out << d;
        }
    }
    return out;
}
