#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QVector>

class QWidget;

// Where QODE's startup time goes: named marks from main() to the first frame on screen. Help > Startup Time shows the
// report; QODE_STARTUP_PROFILE=1 also prints it to stderr when startup is done. Marks after finish() are kept as
// "later" rows (a panel created when first used), so the cost of what startup skips stays visible too.
class StartupProfile
{
public:
    static void begin();                    // first thing in main()
    static void mark(const QString &what);  // the time since the previous mark is charged to `what`
    static void finish();                   // the window has painted: startup is over (prints with the env var)
    static void finishOnFirstPaint(QWidget *window); // finish() once `window` has painted for the first time
    static bool finished();

    // Times a block that runs later (a panel created on first use) without charging it to the previous mark.
    class Later
    {
    public:
        explicit Later(QString what);
        ~Later();
    private:
        QString m_what;
        QElapsedTimer m_timer;
    };

    static QString report();

private:
    struct Mark {
        QString what;
        qint64 ms;      // duration
        bool later;     // a Later block
        bool early;     // a Later block that still ran before the first frame (counted in a startup row too)
    };
    static QElapsedTimer s_clock;
    static qint64 s_last, s_total, s_beforeMain;
    static bool s_done;
    static QVector<Mark> s_marks;
};
