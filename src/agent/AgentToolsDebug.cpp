// The "jvm_debug" tool group: the JDWP debugger as the model's tool - breakpoints, a debug run started like Debug
// (Shift+F9), the stop's frames and variables, stepping and evaluating. Window side: MainWindowIdeDebug.cpp. A start or
// a step waits here, polling the session, until the program stops again or ends.
#include "AgentToolHelpers.h"

namespace agent {

namespace {
const QStringList kSteps{"over", "into", "out", "continue"};
} // namespace

// Waits until the program stopped more often than `stopsBefore` or the session ended, then answers with the full state.
ToolResult AgentTools::debugWait(int stopsBefore, int seconds, const QString &what)
{
    QJsonObject s;
    const bool settled = waitUntil([&] {
        s = m_hooks.ide(QStringLiteral("debug_state"), QJsonObject{{"brief", true}});
        const QString state = s.value("state").toString();
        return state == QLatin1String("not running") || (state == QLatin1String("paused") && s.value("stops").toInt() > stopsBefore);
    }, seconds * 1000, m_cancelled);
    QJsonObject full = m_hooks.ide(QStringLiteral("debug_state"), {});
    full.remove("stops");
    QString summary = full.take("summary").toString();
    if (!settled) {
        full.insert("note", m_cancelled ? QStringLiteral("stopped waiting (the user pressed Stop); the program keeps going")
                                        : QStringLiteral("%1 did not stop within %2 s: it may be building, waiting for input, in a long loop or never reaching a "
                                                         "breakpoint. terminal_output shows its output; debug_step continue / debug_stop, or check again "
                                                         "with debug_state.").arg(what).arg(seconds));
        summary = m_cancelled ? QStringLiteral("stopped waiting") : QStringLiteral("still %1").arg(full.value("state").toString());
    } else if (full.value("state").toString() == QLatin1String("not running")) {
        full.insert("note", QStringLiteral("the program ended without stopping again; terminal_output shows what it printed"));
    }
    return success(full, summary);
}

ToolResult AgentTools::debugTool(const QString &action, const QJsonObject &a)
{
    if (!m_hooks.ide)
        return fail(QStringLiteral("the IDE cannot be controlled right now"));
    QJsonObject args = a;
    if (!str(a, "path").trimmed().isEmpty()) {
        QString abs, err;
        if (!resolve(str(a, "path"), &abs, &err))
            return fail(err);
        args.insert("path", abs);
    }
    if (action == QLatin1String("debug_start")) {
        flushEdits();
        const QJsonObject r = m_hooks.ide(QStringLiteral("debug_start"), args);
        if (r.contains("error"))
            return fail(r.value("error").toString());
        return debugWait(r.value("stops").toInt(), qBound(5, a.value("wait_seconds").toInt(120), 600), QStringLiteral("the program"));
    }
    if (action == QLatin1String("debug_step")) {
        const QString what = str(a, "action").isEmpty() ? QStringLiteral("over") : str(a, "action");
        if (!kSteps.contains(what))
            return fail(QStringLiteral("action is one of: %1").arg(kSteps.join(QStringLiteral(", "))));
        const QJsonObject r = m_hooks.ide(QStringLiteral("debug_control"), QJsonObject{{"what", what}});
        if (r.contains("error"))
            return fail(r.value("error").toString());
        return debugWait(r.value("stops").toInt(), qBound(1, a.value("wait_seconds").toInt(what == QLatin1String("continue") ? 30 : 10), 300),
                         QStringLiteral("the program"));
    }
    if (action == QLatin1String("debug_stop")) {
        const QJsonObject r = m_hooks.ide(QStringLiteral("debug_control"), QJsonObject{{"what", "stop"}});
        if (r.contains("error"))
            return fail(r.value("error").toString());
        waitUntil([&] { return m_hooks.ide(QStringLiteral("debug_state"), QJsonObject{{"brief", true}}).value("state").toString() == QLatin1String("not running"); },
                  5000, m_cancelled);
        return success(QJsonObject{{"state", "stopped"}}, QStringLiteral("stopped"));
    }
    if (action == QLatin1String("debug_state"))
        args.insert("expand", str(a, "object_id"));
    QJsonObject r = m_hooks.ide(action, args);
    if (r.contains("error"))
        return fail(r.value("error").toString());
    r.remove("stops");
    const QString summary = r.take("summary").toString();
    return success(r, summary);
}

void AgentTools::registerDebugTools()
{
    m_groupText.insert(QStringLiteral("jvm_debug"),
                       QStringLiteral("Debugger (Java / Kotlin)\n"
                                      "- To find out why code misbehaves at run time, debug it instead of adding print statements: debug_breakpoints "
                                      "add at the lines to look at, debug_start (a file with a main function, a Kotlin script, or the Gradle / Maven "
                                      "project), then read the stop's variables, debug_step and debug_evaluate expressions. debug_stop when done; "
                                      "remove the breakpoints you added. The program runs in the terminal: terminal_output shows what it printed."));
    auto add2 = [this](Spec s) {
        s.group = QStringLiteral("jvm_debug");
        add(std::move(s));
    };
    add2({"debug_breakpoints", "Add, remove or clear breakpoints (the user sees them in the editor) and list them all. Lines are 1-based and "
                               "must hold code.",
          withEnum(makeSchema({{"action", "string", "add, remove, clear (path's or all) or list."},
                               {"path", "string", "A .java / .kt / .kts file.", false},
                               {"line", "integer", "For add / remove.", false}}),
                   "action", {"add", "remove", "clear", "list"}),
          Risk::Edit, [this](const QJsonObject &a) { return debugTool(QStringLiteral("debug_breakpoints"), a); },
          [](const QJsonObject &a) {
              const QString action = str(a, "action");
              if (action == QLatin1String("add") || action == QLatin1String("remove"))
                  return QStringLiteral("%1 breakpoint at %2:%3").arg(action == QLatin1String("add") ? QStringLiteral("Add") : QStringLiteral("Remove"), str(a, "path")).arg(a.value("line").toInt());
              return action == QLatin1String("clear") ? QStringLiteral("Clear breakpoints") : QStringLiteral("List breakpoints");
          }, {}});
    add2({"debug_start", "Build and run a program under the debugger, like Debug (Shift+F9): the main function in path (the one nearest the top), "
                         "a Kotlin script, or without path the Gradle / Maven project's run configuration. Waits until it stops at a "
                         "breakpoint (answering with frames and variables) or ends. Replaces a running debug session. The user approves it.",
          makeSchema({{"path", "string", "File with a main function, or a .kts script. Omit for the project.", false},
                      {"wait_seconds", "integer", "How long to wait for the first stop (builds count). Default 120, max 600.", false}}),
          Risk::Dangerous, [this](const QJsonObject &a) { return debugTool(QStringLiteral("debug_start"), a); },
          [](const QJsonObject &a) { return str(a, "path").isEmpty() ? QStringLiteral("Debug the project") : QStringLiteral("Debug %1").arg(str(a, "path")); },
          [](const QJsonObject &a) {
              return QStringLiteral("Build and run %1 under the debugger (in the terminal).").arg(str(a, "path").isEmpty() ? QStringLiteral("the project") : str(a, "path"));
          }});
    add2({"debug_state", "Where the paused program is: reason (breakpoint, step, exception), the stack (top 15 frames with file:line) and the "
                         "variables of a frame (values; object_id marks objects whose fields can be listed).",
          makeSchema({{"frame", "integer", "Frame index from the stack. Default 0 (the top).", false},
                      {"object_id", "string", "List this object's fields instead of the frame's variables.", false}}),
          Risk::ReadOnly, [this](const QJsonObject &a) { return debugTool(QStringLiteral("debug_state"), a); },
          [](const QJsonObject &a) { return str(a, "object_id").isEmpty() ? QStringLiteral("Look at the paused program") : QStringLiteral("Look inside an object"); }, {}});
    add2({"debug_step", "Step over, into or out of the current line, or continue to the next breakpoint; waits for the next stop (answering "
                        "like debug_state) or the end.",
          withEnum(makeSchema({{"action", "string", "over (default), into, out or continue."},
                               {"wait_seconds", "integer", "How long to wait. Default 10, continue 30, max 300.", false}}),
                   "action", kSteps),
          Risk::Edit, [this](const QJsonObject &a) { return debugTool(QStringLiteral("debug_step"), a); },
          [](const QJsonObject &a) {
              const QString w = str(a, "action").isEmpty() ? QStringLiteral("over") : str(a, "action");
              return w == QLatin1String("continue") ? QStringLiteral("Continue") : QStringLiteral("Step %1").arg(w);
          }, {}});
    add2({"debug_evaluate", "Evaluate an expression in a frame of the paused program (variables, fields, method calls such as list.size()).",
          makeSchema({{"expression", "string", "Java / Kotlin expression."}, {"frame", "integer", "Frame index. Default 0.", false}}),
          Risk::ReadOnly, [this](const QJsonObject &a) { return debugTool(QStringLiteral("debug_evaluate"), a); },
          [](const QJsonObject &a) { return QStringLiteral("Evaluate %1").arg(shorten(str(a, "expression"), 50)); }, {}});
    add2({"debug_stop", "End the debug session and the program.", makeSchema({}), Risk::Edit,
          [this](const QJsonObject &a) { return debugTool(QStringLiteral("debug_stop"), a); },
          [](const QJsonObject &) { return QStringLiteral("Stop debugging"); }, {}});
}

} // namespace agent
