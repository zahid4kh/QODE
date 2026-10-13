#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// A saved conversation. Qt Core only.
struct AiChange {
    QString path;   // project-relative
    QString op;     // replace / insert / create
    int startLine = 0, endLine = 0;
    bool ok = false;
    QString note;
    QString before, after; // removed / added text, shown as a diff in the chat

    QJsonObject toJson() const;
    static AiChange fromJson(const QJsonObject &o);
};

struct AiMessage {
    QString role;               // "user" | "assistant" | "error"
    QString text;               // the prompt, or the final markdown reply
    QStringList files;          // user: project-relative files attached to the prompt
    QStringList plan;           // assistant: the numbered steps
    QVector<AiChange> changes;  // assistant: what happened to each edit
    QString model;              // assistant: model that answered
    QDateTime time;

    QJsonObject toJson() const;
    static AiMessage fromJson(const QJsonObject &o);
};

struct AiSession {
    QString id;
    QString title;
    QString model;
    QDateTime created, updated;
    QVector<AiMessage> messages;

    static AiSession create(const QString &model);
    // Gemini `contents` for the earlier turns: the prompts and the final replies (file contents are re-sent per turn).
    QJsonArray history() const;
    QJsonObject toJson() const;
    static AiSession fromJson(const QJsonObject &o);
};

// The chat sessions of one project, one JSON file each in <project data dir>/chats/.
class AiChatStore
{
public:
    struct Info {
        QString id, title;
        QDateTime updated;
    };

    void setDirectory(const QString &dir) { m_dir = dir; }
    bool valid() const { return !m_dir.isEmpty(); }
    QVector<Info> list() const; // newest first
    bool load(const QString &id, AiSession *out) const;
    bool save(const AiSession &session) const;
    bool remove(const QString &id) const;

private:
    QString pathOf(const QString &id) const;
    QString m_dir;
};
