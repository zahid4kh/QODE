#pragma once

#include "ai/AiChat.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// Data model and storage of the agent tab's conversations. Qt Core only. Deliberately separate from the plan-and-edit
// chat (AiSession / AiChatStore): other file format, other folder (<project data dir>/agent-chats/).
namespace agent {

// One step of an assistant turn, in the order it happened.
struct Block {
    enum Kind { Thought, Text, Tool };
    Kind kind = Text;
    QString text;      // Thought / Text: the markdown. Tool: the call in words ("Read src/a.cpp 1-200")
    // Tool only
    QString name;      // the function name
    bool done = false; // false while it runs (only ever seen live)
    bool ok = false;
    QString summary;   // what came out ("200 lines")
    bool hasChange = false;
    AiChange change;   // edit tools: the diff card

    QJsonObject toJson() const;
    static Block fromJson(const QJsonObject &o);
};

// One model request of a turn: what it cost and how long it took. While it streams, the output counts are estimated
// from the streamed text (about four characters a token) until the server reports the real ones (`exact`).
struct RequestStats {
    int prompt = 0, cached = 0, output = 0, thinking = 0; // output includes thinking
    int cacheWrite = 0;  // Claude: input written to the prompt cache (part of prompt; billed higher)
    qint64 ms = 0, firstMs = -1, thinkMs = 0;            // whole request / until the first output / spent thinking
    bool exact = true;
    bool done = false;    // only ever false live
    bool stopped = false; // cancelled or failed: the token counts are what had arrived

    QJsonObject toJson() const;
    static RequestStats fromJson(const QJsonObject &o);
};

// A running time as both chats show it: "12s", "1m 12s", "1h 03m".
QString formatElapsed(qint64 ms);

struct Message {
    QString role;           // "user" | "assistant" | "error" | "note" | "summary"
    QString text;           // user: what was typed; error / note: the message; summary: the compacted conversation
    QString sent;           // user: the full text sent to the model (editor context + request); replayed verbatim so the prefix stays cacheable
    QStringList files;      // user: project-relative files that were pointed at
    QString quote;          // user: text quoted from a reply (already part of `sent`)
    QVector<Block> blocks;  // assistant
    QString model;
    QDateTime time;
    int turn = -1;          // user and assistant: the number the journal files this turn's changes under
    // assistant: what the run cost
    int requests = 0, promptTokens = 0, cachedTokens = 0, outputTokens = 0, contextTokens = 0; // summary: what /compact cost
    QVector<RequestStats> requestLog; // every request in order (also the stopped one)
    qint64 ms = 0;                    // wall time of the whole turn, tools included

    // The visible answer: all text blocks of an assistant turn.
    QString answer() const;
    // Short list of what the turn did to the project ("edited a.cpp, ran `make`"), for the model's own history.
    QString actions() const;

    QJsonObject toJson() const;
    static Message fromJson(const QJsonObject &o);
};

struct Session {
    QString id;
    QString title;
    QString model;
    QDateTime created, updated;
    QVector<Message> messages;
    // The tool groups the chat's requests declare (AgentTools::setGroups). They only grow, so the declarations, which
    // are part of the cached prefix, stay the same from run to run.
    QStringList toolGroups;

    static Session create(const QString &model);
    // Gemini `contents` of the earlier turns: prompts and final answers (the tool traces are not replayed). Starts at the
    // last summary when the chat was compacted.
    QJsonArray history() const;
    QJsonObject toJson() const;
    static Session fromJson(const QJsonObject &o);
};

class Store {
public:
    struct Info {
        QString id, title;
        QDateTime updated;
    };

    void setDirectory(const QString &dir) { m_dir = dir; }
    bool valid() const { return !m_dir.isEmpty(); }
    QVector<Info> list() const; // newest first
    bool load(const QString &id, Session *out) const;
    bool save(const Session &session) const;
    bool remove(const QString &id) const;
    QString journalPath(const QString &id) const; // the undo journal of a chat

private:
    QString folder() const;
    QString pathOf(const QString &id) const;
    QString m_dir;
};

} // namespace agent
