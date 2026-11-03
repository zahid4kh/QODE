#pragma once

#include "mcp/McpConfig.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVector>
#include <functional>

namespace mcp {

class Client;

// One tool of a connected server, as the model sees it.
struct Tool {
    QString qualified;   // mcp__blender__get_scene_info: [A-Za-z0-9_-], at most 64 characters (Gemini and Claude)
    QString server, name, description;
    QJsonObject schema;  // the tool's inputSchema, cleaned up for the APIs
    bool readOnly = false; // annotations.readOnlyHint: never asks
};

// What a tool call gave, shaped for the model and the transcript.
struct CallResult {
    bool ok = false;
    QJsonObject response; // {"output": text} or {"error": text}
    QString summary;      // one line
    QString output;       // the whole text
    QJsonArray images;    // [{mimeType, data (base64)}]
};

// The MCP servers of the open project (its own and the global ones): starts the enabled ones when the project opens,
// keeps a Client per server and restarts one whose launch settings change. Owned by the QODE chat.
class Manager : public QObject
{
    Q_OBJECT
public:
    explicit Manager(QObject *parent = nullptr);
    ~Manager() override;

    void setProject(const QString &root, const QString &dataDir); // "" = none: everything stops
    bool hasProject() const { return !m_root.isEmpty(); }
    QString projectRoot() const { return m_root; }

    QVector<ServerConfig> configs() const { return m_configs; }
    // Saves the list and brings the running servers in line with it.
    bool apply(const QVector<ServerConfig> &configs, QString *error);
    void setEnabled(const QString &name, bool enabled);
    void reconnect(const QString &name);
    Client *client(const QString &name) const { return m_clients.value(name); }

    // Every server in the shape the /mcp card reads: {name, status (connected | pending | failed | disabled), transport,
    // scope, tools: [{name}], error}.
    QJsonArray status() const;
    int connectedCount() const;
    // The tools of the connected servers, in configuration order.
    QVector<Tool> tools() const;
    bool trusted(const QString &server) const;
    // Appended to the system prompt: the servers, their instructions, and those that are not available.
    QString promptSection() const;

    CallResult call(const Tool &tool, const QJsonObject &args, const std::function<bool()> &cancelled);

signals:
    void changed();

private:
    void sync(); // start / stop / restart clients to match m_configs

    QString m_root, m_dataDir;
    QVector<ServerConfig> m_configs;
    QHash<QString, Client *> m_clients;
};

} // namespace mcp
