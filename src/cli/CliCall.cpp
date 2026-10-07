#include "CliCommands.h"

#include "Ansi.h"

#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringDecoder>
#include <QThread>

#include <memory>

CliCall::CliCall(CliHost &h, const QString &n, const QStringList &a, QObject *parent)
    : QObject(parent)
    , host(h)
    , name(n)
    , args(a)
{
}

QString CliCall::resolve(const QString &argument) const
{
    QString a = argument;
    if (a == QLatin1String("~"))
        return QDir::homePath();
    if (a.startsWith(QLatin1String("~/")))
        return QDir::cleanPath(QDir::homePath() + a.mid(1));
    if (QDir::isAbsolutePath(a))
        return QDir::cleanPath(a);
    return QDir::cleanPath(cwd() + QLatin1Char('/') + a);
}

void CliCall::print(const QString &ansi)
{
    if (!m_finished && !ansi.isEmpty())
        emit output(Ansi::crlf(ansi));
}

void CliCall::println(const QString &ansi)
{
    print(ansi + QLatin1Char('\n'));
}

void CliCall::error(const QString &message)
{
    m_status = 1;
    println(Ansi::paint(message, Ansi::Red));
}

void CliCall::done(int status)
{
    if (m_finished)
        return;
    if (status >= 0)
        m_status = status;
    m_finished = true;
    emit finished(m_status);
}

void CliCall::cancel()
{
    if (m_finished)
        return;
    m_canceled = true;
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
    }
    println(Ansi::dim(QStringLiteral("^C")));
    done(130);
}

void CliCall::runProcess(const QString &program, const QStringList &arguments, const QString &workDir, int maxLines)
{
    beginAsync();
    auto *p = new QProcess(this);
    m_process = p;
    p->setProcessChannelMode(QProcess::MergedChannels);
    p->setWorkingDirectory(workDir);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    env.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    env.insert(QStringLiteral("PAGER"), QStringLiteral("cat"));
    p->setProcessEnvironment(env);

    auto decoder = std::make_shared<QStringDecoder>(QStringConverter::Utf8);
    auto lines = std::make_shared<int>(0);
    connect(p, &QProcess::readyRead, this, [this, p, decoder, lines, maxLines] {
        QString text = (*decoder)(p->readAll());
        if (maxLines > 0) {
            const int room = maxLines - *lines;
            int n = 0, cut = -1;
            for (int i = 0; i < text.size(); ++i) {
                if (text.at(i) == QLatin1Char('\n') && ++n >= room) {
                    cut = i + 1;
                    break;
                }
            }
            *lines += n;
            if (cut >= 0) {
                print(text.left(cut));
                println(Ansi::dim(tr("… output cut after %1 lines").arg(maxLines)));
                p->disconnect(this);
                p->kill();
                done(0);
                return;
            }
        }
        print(text);
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        print((QString::fromUtf8(p->readAll())));
        done(code);
    });
    connect(p, &QProcess::errorOccurred, this, [this, program](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            error(tr("%1: command not found").arg(program));
            done(127);
        }
    });
    p->start(program, arguments);
}

void CliCall::background(std::function<QString()> work)
{
    beginAsync();
    auto result = std::make_shared<QString>();
    QThread *t = QThread::create([result, work] { *result = work(); });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    connect(t, &QThread::finished, this, [this, result] {
        if (m_canceled)
            return;
        print(*result);
        done(0);
    });
    t->start();
}
