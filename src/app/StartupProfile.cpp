#include "StartupProfile.h"

#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <unistd.h>

QElapsedTimer StartupProfile::s_clock;
qint64 StartupProfile::s_last = 0;
qint64 StartupProfile::s_total = 0;
qint64 StartupProfile::s_beforeMain = -1;
bool StartupProfile::s_done = false;
QVector<StartupProfile::Mark> StartupProfile::s_marks;

namespace {

// How long the process ran before main(): loading and linking Qt's libraries, static initialisers. From the process's
// start time in /proc (in clock ticks, so about 10 ms precise); -1 when it cannot be read.
qint64 msBeforeMain()
{
    QFile stat(QStringLiteral("/proc/self/stat")), uptime(QStringLiteral("/proc/uptime"));
    if (!stat.open(QIODevice::ReadOnly) || !uptime.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray s = stat.readAll();
    // Fields after the command name "(…)", which may hold spaces; starttime is field 22, the 20th after it.
    const QList<QByteArray> fields = s.mid(s.lastIndexOf(')') + 2).split(' ');
    const long ticks = sysconf(_SC_CLK_TCK);
    if (fields.size() < 20 || ticks <= 0)
        return -1;
    const double started = fields.at(19).toDouble() / ticks;
    const double now = uptime.readAll().split(' ').value(0).toDouble();
    return now > 0 && now >= started ? qint64((now - started) * 1000) : -1;
}

// Calls finish() once the first paint of any widget in the watched window has been flushed to the screen (the window
// itself may never get a paint event: its children cover it).
class FirstPaint : public QObject
{
public:
    explicit FirstPaint(QWidget *window)
        : QObject(window), m_window(window) {}
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint && watched->isWidgetType() && static_cast<QWidget *>(watched)->window() == m_window) {
            qApp->removeEventFilter(this);
            QTimer::singleShot(0, [] { StartupProfile::finish(); });
            deleteLater();
        }
        return false;
    }
private:
    QWidget *m_window;
};

} // namespace

void StartupProfile::finishOnFirstPaint(QWidget *window)
{
    qApp->installEventFilter(new FirstPaint(window));
}

void StartupProfile::begin()
{
    s_beforeMain = msBeforeMain();
    s_clock.start();
}

void StartupProfile::mark(const QString &what)
{
    if (s_done || !s_clock.isValid())
        return;
    const qint64 now = s_clock.elapsed();
    s_marks.append({what, now - s_last, false, false});
    s_last = now;
}

void StartupProfile::finish()
{
    if (s_done || !s_clock.isValid())
        return;
    mark(QStringLiteral("first frame (layout, polish, paint)"));
    s_total = s_last;
    s_done = true;
    if (qEnvironmentVariableIsSet("QODE_STARTUP_PROFILE"))
        std::fprintf(stderr, "%s\n", qPrintable(report()));
}

bool StartupProfile::finished()
{
    return s_done;
}

StartupProfile::Later::Later(QString what)
    : m_what(std::move(what))
{
    m_timer.start();
}

StartupProfile::Later::~Later()
{
    s_marks.append({m_what, m_timer.elapsed(), true, !s_done});
    if (s_done && qEnvironmentVariableIsSet("QODE_STARTUP_PROFILE"))
        std::fprintf(stderr, "QODE later: %-50s %5lld ms\n", qPrintable(m_what), static_cast<long long>(m_timer.elapsed()));
}

// A plain-text table: one row per mark, the slowest flagged, then what was created after startup.
QString StartupProfile::report()
{
    QStringList lines;
    auto row = [](const QString &what, qint64 ms) {
        return QStringLiteral("  %1 %2 ms").arg(what.leftJustified(50, QLatin1Char(' '), true)).arg(ms, 5);
    };
    if (!s_done)
        lines << QStringLiteral("Startup is not finished yet.");
    else {
        lines << QStringLiteral("QODE startup: %1 ms from main() to the first frame").arg(s_total);
        if (s_beforeMain >= 0)
            lines << QStringLiteral("(plus about %1 ms before main(): loading the libraries)").arg(s_beforeMain);
    }
    lines << QString();
    for (const Mark &m : std::as_const(s_marks))
        if (!m.later)
            lines << row(m.what, m.ms);
    bool header = false;
    for (const Mark &m : std::as_const(s_marks)) {
        if (!m.later)
            continue;
        if (!header) {
            lines << QString() << QStringLiteral("Created when first used:");
            header = true;
        }
        lines << row(m.what, m.ms) + (m.early ? QStringLiteral("  (during startup)") : QString());
    }
    return lines.join(QLatin1Char('\n'));
}
