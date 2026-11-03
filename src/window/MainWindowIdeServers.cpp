// MainWindow's side of the chat's server tools (group "servers"): the servers QODE runs for the project - the web dev
// server, the Expo app's web Metro, Spring Boot and Ktor - listed, started, stopped and their logs read through the
// same bars as the toolbar buttons. The calls return at once; AgentTools waits for a start by polling server_status.
#include "MainWindow.h"

#include "expo/ExpoBar.h"
#include "expo/ExpoWebServer.h"
#include "ktor/KtorBar.h"
#include "ktor/KtorServer.h"
#include "project/ProjectManager.h"
#include "spring/SpringBar.h"
#include "spring/SpringServer.h"
#include "webdev/DevServer.h"
#include "webdev/DevServerBar.h"

#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>

namespace {
QJsonObject error(const QString &message)
{
    return QJsonObject{{"error", message}};
}

QString stateName(DevServer::State s)
{
    switch (s) {
    case DevServer::State::Starting: return QStringLiteral("starting");
    case DevServer::State::Running: return QStringLiteral("running");
    case DevServer::State::Failed: return QStringLiteral("failed");
    default: return QStringLiteral("stopped");
    }
}
} // namespace

QVector<MainWindow::IdeServer> MainWindow::ideServers()
{
    QVector<IdeServer> out;
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    auto rel = [&root](const QString &dir) {
        const QString r = root.isEmpty() ? dir : QDir(root).relativeFilePath(dir);
        return r == QLatin1String(".") ? QString() : r;
    };
    if (m_serverBar && m_serverBar->isAvailable()) {
        IdeServer s;
        s.id = QStringLiteral("web");
        s.name = m_serverBar->project().framework.isEmpty() ? QStringLiteral("web dev server") : m_serverBar->project().framework;
        s.folder = rel(m_serverBar->workDir());
        s.command = m_serverBar->command();
        s.server = m_serverBar->server();
        s.start = [this] { m_serverBar->start(); };
        s.stop = [this] { m_serverBar->stop(); };
        s.restart = [this] { m_serverBar->restart(); };
        out.append(s);
    }
    if (m_expoBar) {
        ExpoWebServer *metro = m_expoBar->webServer();
        for (const ExpoApp &a : m_expoBar->apps()) {
            IdeServer s;
            s.id = QStringLiteral("expo:") + a.rel;
            s.name = QStringLiteral("%1 (Expo app, web version for the preview)").arg(a.name);
            s.folder = a.rel;
            s.command = QStringLiteral("npx expo start --web (headless Metro for the preview)");
            s.problem = a.web ? QString() : QStringLiteral("the app has no web support (react-native-web, react-dom); the user can add it from the preview panel");
            s.server = metro && metro->appDir() == a.dir ? metro : nullptr; // one Metro serves the app it was started for
            const QString id = s.id, dir = a.dir;
            s.start = [this, id] { startPreviewTarget(id); };
            s.stop = [this, dir] {
                ExpoWebServer *m = m_expoBar->webServer();
                if (m && m->appDir() == dir)
                    m->stop();
            };
            s.restart = [this, id, dir] {
                ExpoWebServer *m = m_expoBar->webServer();
                if (m && m->appDir() == dir && m->isActive())
                    m->restart();
                else
                    startPreviewTarget(id);
            };
            out.append(s);
        }
    }
    if (m_springBar && m_springBar->isAvailable()) {
        IdeServer s;
        s.id = QStringLiteral("spring");
        s.name = QStringLiteral("Spring Boot application");
        s.command = m_springBar->command();
        s.server = m_springBar->server();
        s.phase = [this] {
            SpringServer *srv = m_springBar->server();
            return srv && srv->state() == DevServer::State::Starting ? srv->phaseText() : QString();
        };
        s.start = [this] { m_springBar->start(); };
        s.stop = [this] { m_springBar->stop(); };
        s.restart = [this] { m_springBar->restart(); };
        out.append(s);
    }
    if (m_ktorBar && m_ktorBar->isAvailable()) {
        IdeServer s;
        s.id = QStringLiteral("ktor");
        s.name = QStringLiteral("Ktor server");
        s.command = m_ktorBar->command();
        s.server = m_ktorBar->server();
        s.phase = [this] {
            KtorServer *srv = m_ktorBar->server();
            return srv && srv->state() == DevServer::State::Starting ? srv->phaseText() : QString();
        };
        s.start = [this] { m_ktorBar->start(); };
        s.stop = [this] { m_ktorBar->stop(); };
        s.restart = [this] { m_ktorBar->restart(); };
        out.append(s);
    }
    return out;
}

// The server `id` names, or the only one when `id` is empty; nullptr with `err` set otherwise.
const MainWindow::IdeServer *MainWindow::ideFindServer(const QVector<IdeServer> &servers, const QString &id, QString *err)
{
    if (servers.isEmpty()) {
        *err = QStringLiteral("QODE runs no server in this project");
        return nullptr;
    }
    QStringList ids;
    for (const IdeServer &s : servers) {
        if (s.id == id || (id.isEmpty() && servers.size() == 1))
            return &s;
        ids << s.id;
    }
    *err = id.isEmpty() ? QStringLiteral("the project has several servers; pass server: %1").arg(ids.join(QStringLiteral(", ")))
                        : QStringLiteral("no server '%1'; available: %2").arg(id, ids.join(QStringLiteral(", ")));
    return nullptr;
}

QJsonObject MainWindow::ideServerInfo(const IdeServer &s)
{
    QJsonObject o{{"server", s.id}, {"name", s.name}, {"command", s.command}};
    if (!s.folder.isEmpty())
        o.insert("folder", s.folder);
    const DevServer::State state = s.server ? s.server->state() : DevServer::State::Stopped;
    o.insert("state", stateName(state));
    o.insert("starts", s.server ? s.server->starts() : 0); // AgentTools waits for this to move (not shown to the model)
    if (s.phase && state == DevServer::State::Starting && !s.phase().isEmpty())
        o.insert("phase", s.phase().remove(QChar(0x2026)).trimmed());
    if (state == DevServer::State::Running && s.server) {
        o.insert("port", s.server->port());
        o.insert("url", s.server->url());
    }
    if (state == DevServer::State::Failed && s.server && !s.server->errorString().isEmpty())
        o.insert("failure", s.server->errorString()); // not "error": that key means the IDE call itself failed
    if (!s.problem.isEmpty())
        o.insert("problem", s.problem);
    return o;
}

QJsonObject MainWindow::ideServerStatus(const QString &id)
{
    const QVector<IdeServer> servers = ideServers();
    if (!id.isEmpty()) {
        QString err;
        const IdeServer *s = ideFindServer(servers, id, &err);
        return s ? ideServerInfo(*s) : error(err);
    }
    QJsonArray list;
    QStringList bits;
    for (const IdeServer &s : servers) {
        const QJsonObject o = ideServerInfo(s);
        list.append(o);
        bits << QStringLiteral("%1 %2").arg(s.id, o.value("state").toString());
    }
    return QJsonObject{{"servers", list}, {"summary", bits.isEmpty() ? QStringLiteral("no servers") : bits.join(QStringLiteral(" · "))}};
}

QJsonObject MainWindow::ideServerControl(const QString &id, const QString &what)
{
    const QVector<IdeServer> servers = ideServers();
    QString err;
    const IdeServer *s = ideFindServer(servers, id, &err);
    if (!s)
        return error(err);
    const bool active = s->server && s->server->isActive();
    if (what == QLatin1String("stop")) {
        if (!active)
            return QJsonObject{{"server", s->id}, {"state", "stopped"}, {"summary", QStringLiteral("was not running")}};
        s->stop();
        return QJsonObject{{"server", s->id}, {"summary", QStringLiteral("stopping")}};
    }
    if (!s->problem.isEmpty())
        return error(s->problem);
    if (what == QLatin1String("start") && active) {
        QJsonObject o = ideServerInfo(*s);
        o.insert("already", true);
        return o;
    }
    what == QLatin1String("restart") && active ? s->restart() : s->start();
    return QJsonObject{{"server", s->id}, {"summary", what == QLatin1String("restart") ? QStringLiteral("restarting") : QStringLiteral("starting")}};
}

QJsonObject MainWindow::ideServerLog(const QString &id, int lines, const QString &grep)
{
    const QVector<IdeServer> servers = ideServers();
    QString err;
    const IdeServer *s = ideFindServer(servers, id, &err);
    if (!s)
        return error(err);
    if (!s->server)
        return error(QStringLiteral("%1 has not run since QODE started; there is no log").arg(s->id));
    QStringList all = s->server->log().split(QLatin1Char('\n'));
    while (!all.isEmpty() && all.constLast().trimmed().isEmpty())
        all.removeLast();
    int matched = all.size();
    if (!grep.isEmpty()) {
        const QRegularExpression re(grep, QRegularExpression::CaseInsensitiveOption);
        QStringList keep;
        for (const QString &l : std::as_const(all))
            if (re.isValid() ? re.match(l).hasMatch() : l.contains(grep, Qt::CaseInsensitive))
                keep << l;
        all = keep;
        matched = keep.size();
    }
    const int n = qBound(1, lines, 400);
    QStringList tail = all.mid(qMax(0, int(all.size()) - n));
    QString text = tail.join(QLatin1Char('\n'));
    if (text.size() > 16000)
        text = QStringLiteral("…") + text.right(16000);
    QJsonObject o = ideServerInfo(*s);
    o.insert("log", text);
    o.insert("lines", int(tail.size()));
    if (matched > tail.size())
        o.insert("earlier_lines", matched - int(tail.size()));
    o.insert("summary", QStringLiteral("%1 lines").arg(tail.size()));
    return o;
}
