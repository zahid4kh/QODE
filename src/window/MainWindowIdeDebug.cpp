// MainWindow's side of the chat's debugger tools (group "jvm_debug"): breakpoints, starting a debug run the way Debug
// (Shift+F9) does, the stop's frames and variables, stepping and evaluating, all through the same DebugSession as the
// debug panel. Calls return at once; AgentTools waits for a stop by polling debug_state.
#include "MainWindow.h"
#include "platform/Platform.h"

#include "debug/DebugSession.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "editor/CodeEditor.h"
#include "project/ProjectManager.h"
#include "settings/SettingsManager.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>

namespace {
QJsonObject error(const QString &message)
{
    return QJsonObject{{"error", message}};
}

QString stateName(DebugSession::State s)
{
    switch (s) {
    case DebugSession::State::Connecting: return QStringLiteral("starting");
    case DebugSession::State::Running: return QStringLiteral("running");
    case DebugSession::State::Suspended: return QStringLiteral("paused");
    default: return QStringLiteral("not running");
    }
}

bool debuggable(const QString &path)
{
    return path.endsWith(QLatin1String(".java")) || path.endsWith(QLatin1String(".kt")) || path.endsWith(QLatin1String(".kts"));
}

QJsonObject variableJson(const DebugVariable &v)
{
    QJsonObject o{{"name", v.name}, {"type", v.type}, {"value", v.value.left(300)}};
    if (v.objectId > 0)
        o.insert("object_id", QString::number(v.objectId)); // expandable: fields / elements
    return o;
}
} // namespace

QString MainWindow::ideRelative(const QString &path) const
{
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    return !root.isEmpty() && Platform::isUnder(path, root) ? path.mid(root.size() + 1) : path;
}

void MainWindow::setBreakpointLines(const QString &path, const QList<int> &zeroBased)
{
    if (zeroBased.isEmpty())
        m_breakpoints.remove(path);
    else
        m_breakpoints.insert(path, zeroBased);
    if (Document *d = m_editors->documentForPath(path))
        if (CodeEditor *ed = m_editors->editorFor(d))
            ed->setBreakpoints(zeroBased);
    breakpointsEdited(nullptr, nullptr); // saved, the panel's list, a running session
}

QJsonObject MainWindow::ideBreakpoints(const QString &action, const QString &path, int line)
{
    if (action == QLatin1String("add") || action == QLatin1String("remove")) {
        if (!debuggable(path))
            return error(QStringLiteral("breakpoints work in .java, .kt and .kts files"));
        if (line < 1)
            return error(QStringLiteral("pass line (1-based)"));
        QList<int> lines = m_breakpoints.value(path);
        if (action == QLatin1String("add") && !lines.contains(line - 1))
            lines.append(line - 1);
        if (action == QLatin1String("remove"))
            lines.removeAll(line - 1);
        std::sort(lines.begin(), lines.end());
        setBreakpointLines(path, lines);
    } else if (action == QLatin1String("clear")) {
        const QStringList paths = path.isEmpty() ? m_breakpoints.keys() : QStringList{path};
        for (const QString &p : paths)
            setBreakpointLines(p, {});
    }
    QJsonArray list;
    int count = 0;
    for (auto it = m_breakpoints.constBegin(); it != m_breakpoints.constEnd(); ++it) {
        QJsonArray lines;
        for (int l : it.value())
            lines.append(l + 1);
        count += lines.size();
        list.append(QJsonObject{{"path", ideRelative(it.key())}, {"lines", lines}});
    }
    return QJsonObject{{"breakpoints", list}, {"summary", count == 1 ? QStringLiteral("1 breakpoint") : QStringLiteral("%1 breakpoints").arg(count)}};
}

QJsonObject MainWindow::ideDebugStart(const QString &path)
{
    if (m_debug->isActive()) { // a new run replaces the old one, without the question Debug asks
        m_debug->stop(true, tr("replaced by a new debug run"));
        QElapsedTimer clock;
        clock.start();
        while (m_debug->isActive() && clock.elapsed() < 5000) {
            QEventLoop pause;
            QTimer::singleShot(100, &pause, &QEventLoop::quit);
            pause.exec();
        }
    }
    if (!path.isEmpty()) {
        m_editorHiddenByUser = false;
        if (!m_editors->openFile(path))
            return error(QStringLiteral("the file could not be opened"));
        setEditorColumnVisible(true);
    }
    m_debugEndReason.clear();
    const int stops = m_debugStops;
    startDebug();
    if (!m_debug->isActive())
        return error(QStringLiteral("nothing to debug there: pass a Kotlin / Java file with a main function, a Kotlin script, or leave path out in a Gradle / Maven project"));
    return QJsonObject{{"stops", stops}, {"summary", QStringLiteral("starting")}};
}

QJsonObject MainWindow::ideDebugState(int frame, quint64 expand, bool brief)
{
    const DebugSession::State state = m_debug->state();
    QJsonObject out{{"state", stateName(state)}, {"stops", m_debugStops}};
    if (state == DebugSession::State::Idle) {
        if (!m_debugEndReason.isEmpty())
            out.insert("ended", m_debugEndReason);
        out.insert("summary", m_debugEndReason.isEmpty() ? QStringLiteral("not running") : QStringLiteral("ended: %1").arg(m_debugEndReason));
        return out;
    }
    if (state != DebugSession::State::Suspended || brief) {
        out.insert("summary", stateName(state));
        return out;
    }
    const DebugStop &stop = m_debug->lastStop();
    out.insert("reason", stop.reason);
    out.insert("thread", stop.threadName);
    if (!stop.exception.isEmpty())
        out.insert("exception", stop.exception);
    QJsonArray frames;
    for (int i = 0; i < stop.frames.size() && i < 15; ++i) {
        const DebugFrame &f = stop.frames.at(i);
        const QString file = resolveSource(f.pkgPath, f.sourceName);
        QJsonObject o{{"index", i}, {"method", QStringLiteral("%1.%2").arg(f.className.section(QLatin1Char('.'), -1), f.method)}};
        o.insert("at", file.isEmpty() ? QStringLiteral("%1:%2 (not a project file)").arg(f.sourceName.isEmpty() ? f.className : f.sourceName).arg(f.line)
                                      : QStringLiteral("%1:%2").arg(ideRelative(file)).arg(f.line));
        frames.append(o);
    }
    if (stop.frames.size() > 15)
        out.insert("more_frames", int(stop.frames.size()) - 15);
    out.insert("frames", frames);
    if (frame < 0 || frame >= stop.frames.size())
        frame = 0;

    // The variables of the frame (or the fields of one object), as the debug panel gets them.
    const int token = expand > 0 ? m_debug->expand(expand) : m_debug->selectFrame(stop.thread, frame);
    QList<DebugVariable> vars;
    bool got = false;
    QEventLoop loop;
    const auto conn = connect(m_debug, &DebugSession::variables, &loop, [&](int t, const QList<DebugVariable> &v) {
        if (t != token)
            return;
        vars = v;
        got = true;
        loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    disconnect(conn);
    QJsonArray list;
    for (int i = 0; i < vars.size() && i < 60; ++i)
        list.append(variableJson(vars.at(i)));
    out.insert(expand > 0 ? "fields" : "variables", list);
    if (!got)
        out.insert("note", QStringLiteral("the variables did not arrive within 5 s"));
    if (expand == 0)
        out.insert("frame", frame);
    if (!stop.frames.isEmpty()) {
        const DebugFrame &f = stop.frames.at(frame);
        const QString file = resolveSource(f.pkgPath, f.sourceName);
        out.insert("summary", QStringLiteral("paused (%1) at %2:%3").arg(stop.reason, file.isEmpty() ? f.sourceName : QFileInfo(file).fileName()).arg(f.line));
    }
    return out;
}

QJsonObject MainWindow::ideDebugControl(const QString &what)
{
    const DebugSession::State state = m_debug->state();
    if (what == QLatin1String("stop")) {
        if (state != DebugSession::State::Idle)
            m_debug->stop(true, tr("stopped from the chat"));
        return QJsonObject{{"stops", m_debugStops}, {"summary", QStringLiteral("stopping")}};
    }
    if (what == QLatin1String("pause")) {
        if (state != DebugSession::State::Running)
            return error(QStringLiteral("the program is %1, not running").arg(stateName(state)));
        m_debug->pause();
        return QJsonObject{{"stops", m_debugStops}};
    }
    if (state != DebugSession::State::Suspended)
        return error(QStringLiteral("the program is %1; stepping needs it paused at a breakpoint").arg(stateName(state)));
    const int stops = m_debugStops;
    if (what == QLatin1String("over"))
        m_debug->stepOver();
    else if (what == QLatin1String("into"))
        m_debug->stepInto();
    else if (what == QLatin1String("out"))
        m_debug->stepOut();
    else if (what == QLatin1String("continue"))
        m_debug->resume();
    else
        return error(QStringLiteral("unknown action '%1'").arg(what));
    return QJsonObject{{"stops", stops}};
}

QJsonObject MainWindow::ideDebugEvaluate(const QString &expression, int frame)
{
    if (m_debug->state() != DebugSession::State::Suspended)
        return error(QStringLiteral("the program is %1; evaluating needs it paused").arg(stateName(m_debug->state())));
    const DebugStop &stop = m_debug->lastStop();
    if (frame < 0 || frame >= stop.frames.size())
        frame = 0;
    const int token = m_debug->evaluate(stop.thread, frame, expression);
    DebugVariable result;
    QString failure;
    bool got = false;
    QEventLoop loop;
    const auto conn = connect(m_debug, &DebugSession::evaluated, &loop, [&](int t, const DebugVariable &v, const QString &err) {
        if (t != token)
            return;
        result = v;
        failure = err;
        got = true;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    disconnect(conn);
    if (!got)
        return error(QStringLiteral("no answer within 10 s"));
    if (!failure.isEmpty())
        return error(failure);
    QJsonObject o = variableJson(result);
    o.insert("summary", QStringLiteral("%1 = %2").arg(expression.left(40), result.value.left(60)));
    return o;
}
