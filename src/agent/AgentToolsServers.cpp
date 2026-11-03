// The "servers" tool group: the servers QODE runs for the project (web dev server, Expo web Metro, Spring Boot, Ktor),
// listed, started, restarted, stopped and their logs read through Hooks::ide (MainWindowIdeServers.cpp). A start waits
// here, polling the server's state, until it runs or fails, so the model gets the outcome (and the log on a failure)
// in one call.
#include "AgentToolHelpers.h"

namespace agent {

namespace {
constexpr int kDefaultWait = 120, kMaxWait = 300; // seconds; a Spring Boot / Ktor build can take a while

// The answer without the internal start counter.
QJsonObject shown(QJsonObject o)
{
    o.remove("starts");
    o.remove("summary");
    return o;
}

QString serverName(const QJsonObject &a)
{
    return str(a, "server").isEmpty() ? QStringLiteral("the server") : str(a, "server");
}

// The status of the server `id` names; with no id, of the only server (server_status without an id lists them all).
QJsonObject serverInfo(const std::function<QJsonObject(const QString &, const QJsonObject &)> &ide, const QString &id)
{
    if (!ide)
        return QJsonObject{{"error", "the IDE cannot be controlled right now"}};
    const QJsonObject r = ide(QStringLiteral("server_status"), QJsonObject{{"server", id}});
    if (!r.contains("servers"))
        return r;
    const QJsonArray list = r.value("servers").toArray();
    if (list.size() == 1)
        return list.first().toObject();
    QStringList ids;
    for (const QJsonValue &v : list)
        ids << v.toObject().value("server").toString();
    return QJsonObject{{"error", list.isEmpty() ? QStringLiteral("QODE runs no server in this project")
                                                : QStringLiteral("the project has several servers; pass server: %1").arg(ids.join(QStringLiteral(", ")))}};
}
} // namespace

ToolResult AgentTools::serverStatusTool(const QJsonObject &a)
{
    if (!m_hooks.ide)
        return fail(QStringLiteral("the IDE cannot be controlled right now"));
    QJsonObject r = m_hooks.ide(QStringLiteral("server_status"), QJsonObject{{"server", str(a, "server")}});
    if (r.contains("error"))
        return fail(r.value("error").toString());
    if (r.contains("servers")) {
        QJsonArray list;
        for (const QJsonValue &v : r.value("servers").toArray())
            list.append(shown(v.toObject()));
        return success(QJsonObject{{"servers", list}}, r.value("summary").toString());
    }
    return success(shown(r), r.value("state").toString());
}

ToolResult AgentTools::serverControlTool(const QString &what, const QJsonObject &a)
{
    if (!m_hooks.ide)
        return fail(QStringLiteral("the IDE cannot be controlled right now"));
    const QJsonObject before = serverInfo(m_hooks.ide, str(a, "server"));
    if (before.contains("error"))
        return fail(before.value("error").toString());
    const QString server = before.value("server").toString();
    if (what != QLatin1String("stop"))
        flushEdits(); // the server builds what is on disk
    const QJsonObject r = m_hooks.ide(QStringLiteral("server_control"), QJsonObject{{"server", server}, {"what", what}});
    if (r.contains("error"))
        return fail(r.value("error").toString());
    if (r.value("already").toBool())
        return success(QJsonObject{{"server", server}, {"state", "running"}, {"url", r.value("url")}, {"note", "it was already running; use server_restart to restart it"}},
                       QStringLiteral("already running"));

    QJsonObject now;
    auto status = [&] {
        now = m_hooks.ide(QStringLiteral("server_status"), QJsonObject{{"server", server}});
        return now.value("state").toString();
    };
    if (what == QLatin1String("stop")) {
        waitUntil([&] { return status() != QLatin1String("running") && now.value("state").toString() != QLatin1String("starting"); }, 10000, m_cancelled);
        return success(shown(now), now.value("state").toString());
    }

    const int started = before.value("starts").toInt();
    const int waitSec = qBound(0, a.value("wait_seconds").toInt(kDefaultWait), kMaxWait);
    const bool settled = waitUntil([&] { return status() != QLatin1String("starting") && now.value("starts").toInt() > started; }, waitSec * 1000, m_cancelled);
    QJsonObject out = shown(now);
    const QString state = now.value("state").toString();
    // The log's tail tells why it failed (or what it is doing when it is still starting).
    auto tail = [&](int lines) {
        const QJsonObject log = m_hooks.ide(QStringLiteral("server_log"), QJsonObject{{"server", server}, {"lines", lines}});
        return log.value("log").toString();
    };
    if (!settled) {
        out.insert("note", m_cancelled ? QStringLiteral("stopped waiting (the user pressed Stop); the server keeps starting")
                                       : QStringLiteral("still starting after %1 s; check again with server_status or server_log").arg(waitSec));
        out.insert("log_tail", tail(25));
        return success(out, m_cancelled ? QStringLiteral("stopped waiting") : QStringLiteral("still starting"));
    }
    if (state != QLatin1String("running")) {
        ToolResult f = fail(QStringLiteral("%1 %2").arg(server, state == QLatin1String("failed") ? QStringLiteral("failed to start") : QStringLiteral("stopped")));
        f.response = out;
        f.response.insert("error", f.summary + (now.contains("failure") ? QStringLiteral(": ") + now.value("failure").toString() : QString()));
        f.response.insert("log_tail", tail(60));
        return f;
    }
    return success(out, QStringLiteral("running at %1").arg(now.value("url").toString()));
}

ToolResult AgentTools::serverLogTool(const QJsonObject &a)
{
    if (!m_hooks.ide)
        return fail(QStringLiteral("the IDE cannot be controlled right now"));
    QJsonObject args{{"server", str(a, "server")}, {"lines", a.value("lines").toInt(80)}};
    if (!str(a, "grep").isEmpty())
        args.insert("grep", str(a, "grep"));
    const QJsonObject r = m_hooks.ide(QStringLiteral("server_log"), args);
    if (r.contains("error"))
        return fail(r.value("error").toString());
    return success(shown(r), r.value("summary").toString());
}

void AgentTools::registerServerTools()
{
    m_groupText.insert(QStringLiteral("servers"),
                       QStringLiteral("Servers\n"
                                      "- QODE runs this project's servers (web dev server, an Expo app's web version, Spring Boot, Ktor). Use "
                                      "server_status / server_start / server_restart / server_stop / server_log for them, never run_command: "
                                      "run_command waits for the command to end, and a server never ends.\n"
                                      "- When the user reports an error from the running app, or after a change the server does not reload by "
                                      "itself (build files, configuration, server code without hot reload), read server_log, restart it and "
                                      "check that it comes back up."));
    auto serverParam = Param{"server", "string", "Server id from server_status (web, expo:<folder>, spring, ktor). Optional when there is only one.", false};
    auto detail = [this](const QString &verb) {
        return [this, verb](const QJsonObject &a) {
            const QJsonObject s = serverInfo(m_hooks.ide, str(a, "server"));
            QString text = QStringLiteral("%1 %2").arg(verb, s.value("name").toString(serverName(a)));
            if (!s.value("folder").toString().isEmpty())
                text += QStringLiteral(" in %1").arg(s.value("folder").toString());
            return text + QStringLiteral(":\n\n") + s.value("command").toString();
        };
    };

    Spec status{"server_status", "The servers QODE runs for this project: id, name, state (stopped, starting, running, failed), port and url "
                                 "when running, the command it runs.",
                makeSchema({serverParam}), Risk::ReadOnly, [this](const QJsonObject &a) { return serverStatusTool(a); },
                [](const QJsonObject &a) { return str(a, "server").isEmpty() ? QStringLiteral("Check the servers") : QStringLiteral("Check %1").arg(str(a, "server")); }, {}};
    status.group = QStringLiteral("servers");
    add(status);

    Spec start{"server_start", "Start a server the way QODE's toolbar does (its configured command, environment and port) and wait until it "
                               "runs or fails; a failure answers with the end of its log. Unsaved edits are saved first. The user approves it.",
               makeSchema({serverParam, {"wait_seconds", "integer", "How long to wait for it to come up. Default 120, max 300.", false}}),
               Risk::Dangerous, [this](const QJsonObject &a) { return serverControlTool(QStringLiteral("start"), a); },
               [](const QJsonObject &a) { return QStringLiteral("Start %1").arg(serverName(a)); }, detail(QStringLiteral("Start"))};
    start.group = QStringLiteral("servers");
    add(start);

    Spec restart{"server_restart", "Stop a server and start it again (or start it when it is stopped), then wait like server_start. For changes it "
                                   "does not pick up by itself.",
                 makeSchema({serverParam, {"wait_seconds", "integer", "Default 120, max 300.", false}}),
                 Risk::Dangerous, [this](const QJsonObject &a) { return serverControlTool(QStringLiteral("restart"), a); },
                 [](const QJsonObject &a) { return QStringLiteral("Restart %1").arg(serverName(a)); }, detail(QStringLiteral("Restart"))};
    restart.group = QStringLiteral("servers");
    add(restart);

    Spec stop{"server_stop", "Stop a running server.", makeSchema({serverParam}), Risk::Edit,
              [this](const QJsonObject &a) { return serverControlTool(QStringLiteral("stop"), a); },
              [](const QJsonObject &a) { return QStringLiteral("Stop %1").arg(serverName(a)); }, {}};
    stop.group = QStringLiteral("servers");
    add(stop);

    Spec log{"server_log", "The end of a server's output (this run, or the last one): build errors, stack traces, requests.",
             makeSchema({serverParam, {"lines", "integer", "How many of the last lines. Default 80, max 400.", false},
                         {"grep", "string", "Only lines matching this (case-insensitive regular expression), e.g. \"error|exception\".", false}}),
             Risk::ReadOnly, [this](const QJsonObject &a) { return serverLogTool(a); },
             [](const QJsonObject &a) {
                 const QString s = QStringLiteral("Read the log of %1").arg(serverName(a));
                 return str(a, "grep").isEmpty() ? s : s + QStringLiteral(" (“%1”)").arg(shorten(str(a, "grep"), 30));
             }, {}};
    log.group = QStringLiteral("servers");
    add(log);
}

} // namespace agent
