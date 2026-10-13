#include "AiAgent.h"

#include <QDir>
#include <QFileInfo>

namespace {
constexpr int kContextBudget = 600000;   // characters of file text per turn
constexpr int kMaxFileChars = 250000;
constexpr int kMaxListedFiles = 2500;
}

AiAgent::AiAgent(QObject *parent)
    : QObject(parent)
    , m_client(new GeminiClient(this))
{
    connect(m_client, &GeminiClient::retrying, this, [this](const QString &why, int seconds) {
        emit status(tr("%1 — retrying in %2 s…").arg(why).arg(seconds));
    });
}

QJsonObject AiAgent::message(const QString &role, const QString &text)
{
    return QJsonObject{{QStringLiteral("role"), role},
                       {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("text"), text}}}}};
}

GeminiClient::Request AiAgent::request(const QJsonObject &schema) const
{
    GeminiClient::Request r;
    r.apiKey = m_turn.apiKey;
    r.model = m_turn.model;
    r.systemInstruction = AiEdits::systemPrompt();
    r.contents = m_contents;
    r.responseSchema = schema;
    r.thinkingBudget = 1024;
    return r;
}

void AiAgent::run(const Turn &turn)
{
    if (m_running)
        return;
    m_running = true;
    m_turn = turn;
    m_contents = turn.history;
    m_sent.clear();
    m_plan = {};
    m_budget = kContextBudget;
    plan();
}

void AiAgent::cancel()
{
    if (!m_running)
        return;
    m_running = false;
    m_client->cancel();
}

void AiAgent::fail(const QString &error)
{
    if (!m_running)
        return;
    m_running = false;
    emit failed(error);
}

// One file as the model sees it: header, numbered lines. Empty when it is unreadable or the budget is spent.
QString AiAgent::fileBlock(const QString &absolute, bool *added)
{
    *added = false;
    if (m_sent.contains(absolute))
        return {};
    QString text;
    if (!m_hooks.readFile || !m_hooks.readFile(absolute, &text))
        return {};
    bool cut = false;
    if (text.size() > kMaxFileChars) {
        text.truncate(kMaxFileChars);
        cut = true;
    }
    if (text.size() > m_budget)
        return {};
    m_budget -= text.size();
    m_sent << absolute;
    *added = true;
    const QString rel = QDir(m_turn.root).relativeFilePath(absolute);
    const int lines = text.count(QLatin1Char('\n')) + (text.endsWith(QLatin1Char('\n')) || text.isEmpty() ? 0 : 1);
    return QStringLiteral("=== FILE %1 (%2 lines%3) ===\n%4=== END FILE ===\n\n")
        .arg(rel)
        .arg(lines)
        .arg(cut ? QStringLiteral(", truncated") : QString())
        .arg(AiEdits::numbered(text));
}

void AiAgent::plan()
{
    emit status(tr("Planning…"));
    QString text = QStringLiteral("[PLAN]\nRequest:\n%1\n\n").arg(m_turn.prompt);

    QString attached;
    for (const QString &path : std::as_const(m_turn.attached)) {
        bool added = false;
        attached += fileBlock(path, &added);
    }
    if (!attached.isEmpty())
        text += QStringLiteral("Attached files (the first one is the file open in the editor):\n\n") + attached;

    QStringList files = m_turn.projectFiles;
    const bool cut = files.size() > kMaxListedFiles;
    if (cut)
        files = files.mid(0, kMaxListedFiles);
    text += QStringLiteral("Project files%1:\n%2\n").arg(cut ? QStringLiteral(" (first %1)").arg(kMaxListedFiles) : QString(), files.join(QLatin1Char('\n')));

    m_contents.append(message(QStringLiteral("user"), text));
    m_client->generate(request(AiEdits::planSchema()), [this](const GeminiClient::Result &r) {
        if (!m_running)
            return;
        if (!r.ok)
            return fail(r.error);
        QString err;
        if (!AiEdits::parsePlan(r.text, &m_plan, &err))
            return fail(err);
        m_contents.append(message(QStringLiteral("model"), r.text));
        if (!m_plan.needsEdits) {
            m_running = false;
            emit finished(m_plan.answer, {});
            return;
        }
        emit planReady(m_plan.steps);
        edit();
    });
}

void AiAgent::edit()
{
    emit status(tr("Writing the changes…"));
    QString text = QStringLiteral("[EDIT]\nWrite the edits for the plan above.\n\n");

    QString extra;
    for (const QString &rel : std::as_const(m_plan.files)) {
        QString abs;
        if (!AiEdits::resolveInside(m_turn.root, rel, &abs))
            continue;
        bool added = false;
        extra += fileBlock(abs, &added);
    }
    if (!extra.isEmpty())
        text += QStringLiteral("Additional files from the plan:\n\n") + extra;
    else
        text += QStringLiteral("The files you need were sent with the request. Line numbers are those shown there.\n");

    m_contents.append(message(QStringLiteral("user"), text));
    m_client->generate(request(AiEdits::editSchema()), [this](const GeminiClient::Result &r) {
        if (!m_running)
            return;
        if (!r.ok)
            return fail(r.error);
        QVector<AiEdits::Edit> edits;
        QString err;
        if (!AiEdits::parseEdits(r.text, &edits, &err))
            return fail(err);
        m_contents.append(message(QStringLiteral("model"), r.text));

        emit status(tr("Applying %n edit(s)…", nullptr, edits.size()));
        const QVector<AiEdits::Outcome> outcomes = m_hooks.apply ? m_hooks.apply(edits) : QVector<AiEdits::Outcome>();
        summarize(outcomes);
    });
}

void AiAgent::summarize(const QVector<AiEdits::Outcome> &outcomes)
{
    emit status(tr("Summarizing…"));
    QString text = QStringLiteral("[SUMMARY]\nResult of applying your edits:\n");
    if (outcomes.isEmpty())
        text += QStringLiteral("- no edits were produced\n");
    for (const AiEdits::Outcome &o : outcomes) {
        QString where = o.edit.op == AiEdits::Edit::Create ? QStringLiteral("new file")
                        : o.edit.op == AiEdits::Edit::Insert ? QStringLiteral("insert at line %1").arg(o.edit.startLine)
                                                              : QStringLiteral("lines %1-%2").arg(o.edit.startLine).arg(o.edit.endLine);
        text += QStringLiteral("- %1 `%2` (%3): %4\n")
                    .arg(o.ok ? QStringLiteral("APPLIED") : QStringLiteral("REJECTED"), o.edit.path, where, o.note);
    }
    text += QStringLiteral("\nThe applied changes are unsaved edits in the editor; the user reviews them there.\n");
    m_contents.append(message(QStringLiteral("user"), text));

    m_client->generate(request(), [this, outcomes](const GeminiClient::Result &r) {
        if (!m_running)
            return;
        m_running = false;
        if (r.ok) {
            emit finished(r.text.trimmed(), outcomes);
            return;
        }
        // The edits are already in the editor: report them even though the summary call failed.
        QString md = tr("The summary request failed (%1). What was done:\n\n").arg(r.error);
        for (const AiEdits::Outcome &o : outcomes)
            md += QStringLiteral("- %1 `%2`: %3\n").arg(o.ok ? QStringLiteral("✓") : QStringLiteral("✗"), o.edit.path, o.note);
        emit finished(md, outcomes);
    });
}
