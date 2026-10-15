#include "AgentRun.h"

#include "ai/GeminiClient.h"

#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace agent {

namespace {
constexpr int kMaxAttempts = 4;
constexpr int kTimeoutMs = 120000; // without any data
constexpr int kKeepRecentToolTurns = 3;
constexpr int kPruneAboveChars = 1500;
constexpr int kPreviewChars = 300;
// Gemini caches the start of a request that repeats the previous one (implicit caching), so the history must not change
// from one request to the next. Old tool results are therefore only shrunk when the context has grown large, and then
// all at once, instead of one more result with every request.
constexpr int kPruneWhenContextExceedsChars = 240000;

} // namespace

AgentRun::AgentRun(AgentTools *tools, QObject *parent)
    : QObject(parent)
    , m_tools(tools)
    , m_net(new QNetworkAccessManager(this))
    , m_retryTimer(new QTimer(this))
{
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &AgentRun::send);
}

QString AgentRun::systemPrompt(const QString &root)
{
    QString text = baseInstructions(root);
    QFile f(root + QStringLiteral("/AGENTS.md"));
    if (f.open(QIODevice::ReadOnly)) {
        QString agents = QString::fromUtf8(f.read(16000));
        if (!agents.trimmed().isEmpty())
            text += QStringLiteral("\n\nProject instructions from AGENTS.md (written for agents like you; follow them):\n%1").arg(agents.trimmed());
    }
    return text;
}

QString AgentRun::baseInstructions(const QString &root)
{
    return QStringLiteral(
               "You are the coding agent of QODE, a code editor, working inside the user's project on Linux. "
               "Project root: %1. You carry out the user's request yourself by calling tools: explore, edit, verify, then report.\n\n"
               "How to work\n"
               "- Orient first with search_text, find_files, directory_tree or read_file; never guess file contents or paths. "
               "Locate code with search_text before reading, and read only the ranges you need.\n"
               "- Make the change with the edit tools. Prefer replace_text (exact snippet, no line numbers) for small edits; use edit_range or "
               "insert_lines with line numbers taken from a fresh read. Each edit answers with the lines as they now read - rely on that instead of "
               "re-reading, and remember that line numbers below an edit shift.\n"
               "- Keep edits minimal and in the style of the surrounding code. Do not rewrite or reformat code you were not asked to touch.\n"
               "- Edits appear as unsaved, undoable changes in the user's editor tabs. run_command saves them first, so a build or test sees them.\n"
               "- After changing code, verify when a build or test command is obvious from the project (look at the build files first): run it, read the "
               "errors, fix them, run it again. Stop after a few failed attempts and report what is wrong.\n"
               "- get_diagnostics returns the language server's errors and warnings: use it after editing to check your work (cheaper than a full "
               "build). run_command is for builds and tests.\n"
               "- After moving or renaming a file, search for references to it (imports, includes, paths in build files) and fix them yourself.\n"
               "- When the user asks to undo, revert or redo changes (all of them, one file, or some lines), use undo_changes / redo_changes; "
               "list_changes shows what can be undone. Never rewrite old code from memory to undo something.\n"
               "- Issue independent tool calls together in one turn instead of one by one. Every model round trip is a rate-limited request.\n"
               "- Never touch secrets (.env, keys, credentials). Use the git_* tools for git (each asks the user); commit only when the user asked, never push or rewrite history. "
               "Commands and deletions are shown to the user for approval; if one is denied, accept it, say so and carry on without it.\n"
               "- If the request is ambiguous in a way that matters, ask one short question instead of guessing. If it is only a question about the "
               "code, answer it without editing anything.\n\n"
               "Final answer\n"
               "- Short markdown: what you changed and why, the files involved, what you verified and what you could not, and anything the user should "
               "check. Do not paste whole files or repeat diffs; the user sees every edit in the transcript. Refer to files by their project-relative path.")
        .arg(root);
}

void AgentRun::start(const Config &config)
{
    if (m_running)
        return;
    m_cfg = config;
    m_contents = config.contents;
    m_modelParts = QJsonArray();
    m_running = true;
    m_cancelled = false;
    m_anyText = false;
    m_usage = Usage();
    m_rounds = 0;
    m_attempt = 0;
    m_tools->resetCancel();
    emit status(tr("Thinking…"));
    send();
}

void AgentRun::cancel()
{
    if (!m_running)
        return;
    m_cancelled = true;
    m_tools->cancel();
    m_retryTimer->stop();
    if (m_reply)
        m_reply->abort(); // finished() reports it
    else if (!m_inTools)
        finish(tr("Stopped.")); // waiting for a retry; a running tool returns "cancelled" and runTools() notices
}

void AgentRun::finish(const QString &error)
{
    m_running = false;
    m_reply = nullptr;
    emit status(QString());
    emit usage(m_usage);
    if (error.isEmpty())
        emit finished(m_rounds);
    else
        emit failed(error);
}

void AgentRun::send()
{
    if (m_cancelled) {
        finish(tr("Stopped."));
        return;
    }
    if (m_rounds >= m_cfg.maxRounds) {
        finish(tr("Stopped after %1 steps. Send \"continue\" to let the agent carry on.").arg(m_cfg.maxRounds));
        return;
    }
    QJsonObject body;
    body.insert("contents", m_contents);
    body.insert("systemInstruction", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", m_cfg.systemInstruction}}}}});
    body.insert("tools", m_tools->toolsJson());
    body.insert("generationConfig", QJsonObject{{"thinkingConfig", QJsonObject{{"thinkingLevel", m_cfg.thinkingLevel}, {"includeThoughts", true}}}});
    QJsonArray safety;
    for (const char *category : {"HARM_CATEGORY_HARASSMENT", "HARM_CATEGORY_HATE_SPEECH", "HARM_CATEGORY_SEXUALLY_EXPLICIT", "HARM_CATEGORY_DANGEROUS_CONTENT"})
        safety.append(QJsonObject{{"category", QLatin1String(category)}, {"threshold", "OFF"}});
    body.insert("safetySettings", safety);

    QNetworkRequest req(QUrl(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/%1:streamGenerateContent?alt=sse").arg(m_cfg.model.trimmed())));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-goog-api-key", m_cfg.apiKey.toUtf8());
    req.setTransferTimeout(kTimeoutMs);

    m_buffer.clear();
    m_raw.clear();
    m_inlineError.clear();
    m_round = Usage();
    m_finishReason.clear();
    m_modelParts = QJsonArray();
    m_gotOutput = false;
    emit status(m_rounds == 0 ? tr("Thinking…") : tr("Working…"));
    m_reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &AgentRun::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &AgentRun::onReplyFinished);
}

void AgentRun::onReadyRead()
{
    if (!m_reply)
        return;
    const QByteArray chunk = m_reply->readAll();
    if (m_raw.size() < 512 * 1024)
        m_raw += chunk;
    m_buffer += chunk;
    int nl;
    while ((nl = m_buffer.indexOf('\n')) != -1) {
        QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (!line.startsWith("data:"))
            continue; // also skips an error body here; it is read from m_raw afterwards
        line = line.mid(5).trimmed();
        if (line.isEmpty() || line == "[DONE]")
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            handleEvent(doc.object());
    }
}

void AgentRun::handleEvent(const QJsonObject &event)
{
    if (event.value("error").isObject()) {
        m_inlineError = event.value("error").toObject().value("message").toString(tr("The model reported an error."));
        return;
    }
    const QString block = event.value("promptFeedback").toObject().value("blockReason").toString();
    if (!block.isEmpty())
        m_inlineError = tr("The prompt was blocked (%1).").arg(block);

    const QJsonObject meta = event.value("usageMetadata").toObject();
    if (!meta.isEmpty()) { // cumulative within one response: the last one counts
        m_round.prompt = meta.value("promptTokenCount").toInt();
        m_round.cached = meta.value("cachedContentTokenCount").toInt();
        m_round.output = meta.value("candidatesTokenCount").toInt() + meta.value("thoughtsTokenCount").toInt();
    }

    const QJsonArray candidates = event.value("candidates").toArray();
    if (candidates.isEmpty())
        return;
    const QJsonObject cand = candidates.first().toObject();
    const QString reason = cand.value("finishReason").toString();
    if (!reason.isEmpty())
        m_finishReason = reason;
    for (const QJsonValue &v : cand.value("content").toObject().value("parts").toArray()) {
        const QJsonObject part = v.toObject();
        addModelPart(part);
        if (part.contains("text")) {
            const QString text = part.value("text").toString();
            if (text.isEmpty())
                continue;
            m_gotOutput = true;
            if (part.value("thought").toBool()) {
                emit thoughtDelta(text);
            } else {
                m_anyText = true;
                emit status(tr("Writing…"));
                emit textDelta(text);
            }
        } else if (part.contains("functionCall")) {
            m_gotOutput = true;
        }
    }
}

// Keeps the model turn verbatim (Gemini 3 wants thought signatures echoed back with the function calls), merging the
// fragments of one streamed text.
void AgentRun::addModelPart(const QJsonObject &part)
{
    if (!m_modelParts.isEmpty() && part.contains("text") && !part.contains("functionCall")) {
        QJsonObject last = m_modelParts.last().toObject();
        if (last.contains("text") && !last.contains("thoughtSignature") && last.value("thought").toBool() == part.value("thought").toBool()) {
            last["text"] = last.value("text").toString() + part.value("text").toString();
            if (part.contains("thoughtSignature"))
                last["thoughtSignature"] = part.value("thoughtSignature");
            m_modelParts.replace(m_modelParts.size() - 1, last);
            return;
        }
    }
    m_modelParts.append(part);
}

void AgentRun::retryLater(const QString &why, int seconds)
{
    ++m_attempt;
    emit status(tr("%1 - retrying in %2 s…").arg(why).arg(seconds));
    m_retryTimer->start(seconds * 1000);
}

void AgentRun::onReplyFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    onReadyRead();
    reply->deleteLater();
    m_reply = nullptr;

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();
    const QString netText = reply->errorString();

    if (m_cancelled || netError == QNetworkReply::OperationCanceledError) {
        finish(tr("Stopped."));
        return;
    }

    // --- the request failed -------------------------------------------------------------------------------------
    if (netError != QNetworkReply::NoError || httpStatus >= 400) {
        const QJsonObject body = QJsonDocument::fromJson(m_raw).object();
        if (m_gotOutput) { // the stream broke after output was shown: a retry would repeat it
            finish(tr("The connection broke while the model was answering (%1). Send the request again.").arg(netText));
            return;
        }
        if (httpStatus == 0) {
            if (m_attempt < kMaxAttempts && netError != QNetworkReply::TimeoutError) {
                retryLater(tr("Network problem"), 3 * (m_attempt + 1));
                return;
            }
            finish(netError == QNetworkReply::TimeoutError ? tr("Gemini did not answer in time. The model \"%1\" may be overloaded; try another one.").arg(m_cfg.model)
                                                           : tr("Network error: %1").arg(netText));
            return;
        }
        const QString lower = body.value("error").toObject().value("message").toString().toLower();
        const bool daily = httpStatus == 429 && (lower.contains(QLatin1String("perday")) || lower.contains(QLatin1String("per day")));
        const bool transient = httpStatus == 429 || httpStatus == 500 || httpStatus == 503 || httpStatus == 504;
        if (transient && !daily && m_attempt < kMaxAttempts) {
            int wait = 3 * (m_attempt + 1) * (m_attempt + 1);
            if (const int asked = GeminiClient::retryDelayOf(body))
                wait = asked;
            wait = qBound(2, wait, 60);
            retryLater(httpStatus == 429 ? tr("Rate limited") : httpStatus == 503 ? tr("Model is busy") : tr("Server error %1").arg(httpStatus), wait);
            return;
        }
        finish(GeminiClient::errorText(body, httpStatus, netText, m_cfg.model));
        return;
    }
    if (!m_inlineError.isEmpty()) {
        finish(m_inlineError);
        return;
    }
    m_attempt = 0;
    ++m_rounds;
    ++m_usage.requests;
    m_usage.prompt += m_round.prompt;
    m_usage.cached += m_round.cached;
    m_usage.output += m_round.output;
    if (m_round.prompt > 0)
        m_usage.context = m_round.prompt;

    // --- the model turn is complete -------------------------------------------------------------------------------
    bool hasCalls = false;
    for (const QJsonValue &v : std::as_const(m_modelParts))
        hasCalls = hasCalls || v.toObject().contains("functionCall");

    if (hasCalls) {
        if (!runTools())
            return;
        send();
        return;
    }
    if (m_finishReason == QLatin1String("SAFETY") || m_finishReason == QLatin1String("PROHIBITED_CONTENT")) {
        finish(tr("The answer was blocked by Gemini's safety filter (%1).").arg(m_finishReason));
        return;
    }
    if (m_finishReason == QLatin1String("MALFORMED_FUNCTION_CALL")) {
        if (m_rounds < m_cfg.maxRounds && m_attempt < 2) { // ask once more instead of failing the whole run
            ++m_attempt;
            m_contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", "Your last tool call was malformed. Call the tool again with valid arguments."}}}}});
            send();
            return;
        }
        finish(tr("The model produced an invalid tool call. Try again or pick another model."));
        return;
    }
    if (m_finishReason == QLatin1String("MAX_TOKENS") && !m_anyText) {
        finish(tr("The model ran out of output tokens while thinking. Try a smaller request."));
        return;
    }
    if (!m_anyText) {
        if (m_rounds > 1) { // it did its work but said nothing: ask for the report instead of failing
            m_contents.append(QJsonObject{{"role", "model"}, {"parts", m_modelParts.isEmpty() ? QJsonArray{QJsonObject{{"text", "(done)"}}} : m_modelParts}});
            m_contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", "Summarize briefly what you did and the result."}}}}});
            if (m_attempt < 2) {
                ++m_attempt;
                send();
                return;
            }
        }
        finish(tr("Gemini returned an empty answer (finish reason: %1).").arg(m_finishReason.isEmpty() ? QStringLiteral("none") : m_finishReason));
        return;
    }
    finish(QString());
}

// Runs the function calls of the finished model turn and queues the results for the next request.
bool AgentRun::runTools()
{
    QJsonArray responses;
    for (const QJsonValue &v : std::as_const(m_modelParts)) {
        const QJsonObject call = v.toObject().value("functionCall").toObject();
        if (call.isEmpty())
            continue;
        const QString name = call.value("name").toString();
        const QJsonObject args = call.value("args").toObject();

        emit status(tr("Running %1…").arg(name));
        emit toolStarted(name, m_tools->describe(name, args));
        m_inTools = true;
        const ToolResult result = m_tools->execute(name, args);
        m_inTools = false;
        emit toolFinished(name, result);
        if (m_cancelled) {
            finish(tr("Stopped."));
            return false;
        }
        QJsonObject fr{{"name", name}, {"response", result.response}};
        if (call.contains("id"))
            fr.insert("id", call.value("id"));
        responses.append(QJsonObject{{"functionResponse", fr}});
    }
    m_contents.append(QJsonObject{{"role", "model"}, {"parts", m_modelParts}});
    m_contents.append(QJsonObject{{"role", "user"}, {"parts", responses}});
    if (QJsonDocument(m_contents).toJson(QJsonDocument::Compact).size() > kPruneWhenContextExceedsChars)
        m_contents = pruneOldResults(m_contents);
    return true;
}

// Each tool result is sent again with every later request. Big results that are more than a few turns old rarely matter
// any more, so they shrink to a preview (the model can call the tool again).
QJsonArray AgentRun::pruneOldResults(const QJsonArray &contents)
{
    QVector<int> toolTurns;
    for (int i = 0; i < contents.size(); ++i) {
        const QJsonArray parts = contents.at(i).toObject().value("parts").toArray();
        if (!parts.isEmpty() && parts.first().toObject().contains("functionResponse"))
            toolTurns.append(i);
    }
    QJsonArray result = contents;
    for (int t = 0; t + kKeepRecentToolTurns < toolTurns.size(); ++t) {
        const int index = toolTurns.at(t);
        QJsonObject turn = result.at(index).toObject();
        QJsonArray parts = turn.value("parts").toArray();
        for (int p = 0; p < parts.size(); ++p) {
            QJsonObject part = parts.at(p).toObject();
            QJsonObject fr = part.value("functionResponse").toObject();
            const QByteArray raw = QJsonDocument(fr.value("response").toObject()).toJson(QJsonDocument::Compact);
            if (raw.size() <= kPruneAboveChars || raw.contains("older result dropped"))
                continue;
            fr["response"] = QJsonObject{{"note", QStringLiteral("older result dropped to save tokens; call the tool again if you need it. Start: %1...")
                                                      .arg(QString::fromUtf8(raw.left(kPreviewChars)))}};
            part["functionResponse"] = fr;
            parts.replace(p, part);
        }
        turn["parts"] = parts;
        result.replace(index, turn);
    }
    return result;
}

} // namespace agent
