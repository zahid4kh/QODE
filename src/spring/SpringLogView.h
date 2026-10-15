#pragma once

#include <QTextEdit>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QVector>

#include "settings/Theme.h"

#include <functional>

class QTimer;

// Live view of a Spring Boot application's output. The raw text is split into entries first: one per Logback line
// (time, level, logger, message), with stack traces attached to the line that caused them, Gradle / Maven build output
// and plain output (banner, println) as entries of their own. The entries are then drawn into a read-only document,
// filtered by level and text. Stack traces fold: only your own frames and the exception lines stay visible, a click on
// the summary line shows the rest; a frame in project code is a link to the source line.
class SpringLogView : public QTextEdit
{
    Q_OBJECT
public:
    enum Level { Trace, Debug, Info, Warn, Error, Build, Output, Marker };

    explicit SpringLogView(QWidget *parent = nullptr);

    // Raw output of the process; lines may arrive in pieces.
    void feed(const QString &chunk);
    // A divider line ("Server started 10:32"), always shown. Also ends the build/app distinction of the previous run.
    void addMarker(const QString &text);
    void clearLog();
    void jumpToEnd(); // scrolls to the newest entry and follows the output again

    // Entries below this level are hidden (Trace shows everything). Build / output lines count as Info.
    void setMinLevel(Level level);
    void setTextFilter(const QString &text);
    void setShowBuild(bool show);
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }

    int totalCount() const { return int(m_entries.size()); }
    int shownCount() const { return int(m_shown.size()); }
    // Every entry (or only the shown ones) as the text the process printed.
    QString rawText(bool onlyShown) const;

    // pkgPath ("com/example/demo", empty for the default package) + file name -> absolute path of the source file, or "".
    using Resolver = std::function<QString(const QString &pkgPath, const QString &fileName)>;
    void setResolver(const Resolver &resolver);
    void clearResolveCache();

signals:
    void locationRequested(const QString &path, int line, int column);
    void countsChanged();

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct LineInfo
    {
        enum Kind { Plain, Location, Toggle };
        Kind kind = Plain;
        QString path;
        int line = 0, column = 0;
    };
    struct Entry
    {
        Level level = Output;
        bool spring = false; // a parsed Logback line (time / logger / message are filled)
        QString time, thread, logger, text;
        QStringList extra; // continuation lines (stack trace, wrapped message)
        QStringList raw;   // exactly what was printed
        QString haystack;  // lower-case text the filter looks at
        bool expanded = false;
        bool bad = false;      // Spring's "APPLICATION FAILED TO START" block
        LineInfo loc;          // compiler messages point at a source line
        int pos = -1, len = 0; // where the entry sits in the document, -1 = not drawn
        QVector<LineInfo> lines;
    };
    struct Line;

    void processLine(QString line);
    void flushPartial();
    void addEntry(Entry e);
    bool continues(const QString &line) const;
    bool passes(const Entry &e) const;
    void sync();                    // draws what changed since the last call
    void renderAll();
    void removeTail(int index);
    void drawAtEnd(int index);
    void rerender(int index);
    QList<Line> buildLines(Entry &e) const;
    int insertLines(QTextCursor &c, const QList<Line> &lines, bool first);
    QString resolveFrame(const QString &fqn, const QString &file) const;
    int shownSlotAt(int docPos) const;
    LineInfo infoAt(const QPoint &viewportPos, int *entry = nullptr) const;
    void updateTheme();
    void touchLast();

    QList<Entry> m_entries;
    QVector<int> m_shown; // indices into m_entries of the drawn entries, in order
    int m_dirtyFrom = 0;
    QString m_partial;
    QTimer *m_flush;
    bool m_inApp = false; // the application itself is running (not the build before it)
    Level m_minLevel = Trace;
    QString m_filter;
    bool m_showBuild = true;
    bool m_paused = false, m_stale = false;
    Theme m_theme;
    Resolver m_resolver;
    mutable QHash<QString, QString> m_resolved;
    QPoint m_pressPos;
};
