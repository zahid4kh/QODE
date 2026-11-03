#pragma once

#include <QJsonObject>
#include <QMap>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QVector>

// MCP servers for the QODE chat: programs that speak the Model Context Protocol over stdin / stdout and give the agent
// more tools (Blender, databases, browsers, ...). This file is where they are configured: QODE's own lists (all projects
// in ~/.config/QODE-AGENTIC/mcp.json, one project in its data folder, both mode 0600 because `env` often holds tokens)
// and what can be imported from Claude Code's configuration. Nothing is written inside the project.
namespace mcp {

struct ServerConfig {
    QString name;
    bool global = false;          // every project, or only the open one
    QString command;              // a program on PATH or a full path ("uvx", "npx", "/opt/x/server")
    QStringList args;
    QMap<QString, QString> env;   // added to the environment; never shown to the model
    QString cwd;                  // "" = the project folder
    bool enabled = true;
    bool trusted = false;         // its tools run without asking first (tools marked read-only never ask)

    // Claude Code's shape ({command, args, env, cwd}) plus QODE's `disabled` / `trusted`.
    QJsonObject toJson() const;
    static ServerConfig fromJson(const QString &name, const QJsonObject &o);
    bool sameLaunch(const ServerConfig &o) const; // command, args, env and cwd: a change needs a restart
    QString commandLine() const;  // "uvx blender-mcp", quoted where needed
};

// A server found in Claude Code's configuration.
struct Found {
    ServerConfig config;
    QString where;  // "user settings", "project ~/game", "this project's .mcp.json"
    QString type;   // stdio | http | sse (only stdio can be used)
};

// QODE's servers: the global ones, then the project's (a project server hides a global one with the same name).
QVector<ServerConfig> load(const QString &projectDataDir);
bool save(const QVector<ServerConfig> &servers, const QString &projectDataDir, QString *error);

// Servers in ~/.claude.json (user scope, and the local scope of every project there) and in the open project's .mcp.json.
QVector<Found> claudeCodeServers(const QString &projectRoot);

// A pasted snippet as READMEs show it: {"mcpServers": {...}}, {"name": {"command": ...}} or one {"command": ...}.
QVector<ServerConfig> parseSnippet(const QString &text, QString *error);

// Arguments as one line that QProcess::splitCommand reads back: quoted where needed, a quote inside as """.
QString joinArgs(const QStringList &args);

// Letters, digits, '-' and '_': the name becomes part of the tool names the model sees.
bool validName(const QString &name);

// The environment a server starts with: QODE's own, a PATH that also has the usual per-user tool folders (a desktop
// launch often lacks ~/.local/bin, where uvx lives), then the configured variables with ${VAR} / ${VAR:-default} expanded.
QProcessEnvironment environment(const ServerConfig &config);
QString expand(const QString &text, const QProcessEnvironment &env);

} // namespace mcp
