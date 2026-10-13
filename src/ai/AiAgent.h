#pragma once

#include "ai/AiEdits.h"
#include "ai/GeminiClient.h"

#include <QJsonArray>
#include <QObject>
#include <functional>

// Runs one user prompt as several Gemini calls, without any function calling:
//   1. PLAN    numbered steps + the files they touch (or the answer, for a question)
//   2. EDIT    structured edits (file, line range, code) for those files
//   3. apply   the edits are handed to the window through `apply`
//   4. SUMMARY the final markdown reply about what was done
// The conversation (`history`) and the session system prompt travel with every call; the file contents only with this turn.
class AiAgent : public QObject
{
    Q_OBJECT
public:
    struct Turn {
        QString apiKey;
        QString model;
        QString root;                 // project root
        QString prompt;
        QStringList attached;         // absolute paths: the focused file first, then the @-mentions
        QStringList projectFiles;     // project-relative paths offered to the planner
        QJsonArray history;           // earlier turns as Gemini contents (user prompt / final reply)
    };
    struct Hooks {
        // Text of a file (the open buffer when there is one); false when it cannot be read or is not text.
        std::function<bool(const QString &absolute, QString *text)> readFile;
        // Applies the edits to the project and reports one outcome per edit.
        std::function<QVector<AiEdits::Outcome>(const QVector<AiEdits::Edit> &)> apply;
    };

    explicit AiAgent(QObject *parent = nullptr);

    void setHooks(const Hooks &hooks) { m_hooks = hooks; }
    void run(const Turn &turn);
    void cancel();
    bool running() const { return m_running; }

signals:
    void status(const QString &text);
    void planReady(const QStringList &steps);
    void finished(const QString &markdown, const QVector<AiEdits::Outcome> &outcomes);
    void failed(const QString &error);

private:
    void plan();
    void edit();
    void summarize(const QVector<AiEdits::Outcome> &outcomes);
    void fail(const QString &error);
    GeminiClient::Request request(const QJsonObject &schema = {}) const;
    QString fileBlock(const QString &absolute, bool *added);

    static QJsonObject message(const QString &role, const QString &text);

    GeminiClient *m_client;
    Hooks m_hooks;
    Turn m_turn;
    QJsonArray m_contents;   // history + this turn so far
    QStringList m_sent;      // absolute paths whose contents are already in m_contents
    AiEdits::Plan m_plan;
    bool m_running = false;
    int m_budget = 0;        // characters of file text still allowed in this turn
};
