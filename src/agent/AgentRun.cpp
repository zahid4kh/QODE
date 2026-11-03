#include "AgentRun.h"

#include "ai/ClaudeClient.h"
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
    , m_tick(new QTimer(this))
{
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &AgentRun::send);
    m_tick->setInterval(250); // the elapsed time keeps moving while the model is quiet
    connect(m_tick, &QTimer::timeout, this, [this] { emit requestProgress(currentRequest()); });
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
               "Project root: %1 (tool paths are relative to it). You carry out the user's request yourself by calling tools: explore, edit, verify, then report.\n\n"
               "How to work\n"
               "- Orient first; never guess file contents or paths. semantic_search finds code by what it does across the whole project "
               "(use it when you do not know where something lives or what it is called); search_text finds an exact identifier or string; "
               "find_files and directory_tree show the layout. Locate code before reading, and read only the ranges you need: a "
               "semantic_search result already shows the sections it found.\n"
               "- Make the change with the edit tools. Prefer replace_text (exact snippet, no line numbers) for small edits; use edit_range or "
               "insert_lines with line numbers taken from a fresh read. Each edit answers with the lines as they now read - rely on that instead of "
               "re-reading, and remember that line numbers below an edit shift.\n"
               "- Keep edits minimal and in the style of the surrounding code. Do not rewrite or reformat code you were not asked to touch.\n"
               "- Edits appear as unsaved, undoable changes in the user's editor tabs. run_command saves them first, so a build or test sees them.\n"
               "- After changing code, verify when a build or test command is obvious from the project (look at the build files first): run it, read the "
               "errors, fix them, run it again. Stop after a few failed attempts and report what is wrong.\n"
               "- When a language server checks a file you edited, the last edit of that file in each turn answers with "
               "diagnostics_after_edit: the file's errors once your edits of the turn are in. Fix the errors you caused before you answer; leave "
               "errors marked as there before your edits alone unless the task is about them. get_diagnostics shows warnings and other files. "
               "run_command is for builds and tests.\n"
               "- After moving or renaming a file, search for references to it (imports, includes, paths in build files) and fix them yourself.\n"
               "- When the user asks to undo, revert or redo changes (all of them, one file, or some lines), use undo_changes / redo_changes; "
               "list_changes shows what can be undone. Never rewrite old code from memory to undo something.\n"
               "- Every turn of yours resends the whole conversation, so use few turns: issue all independent tool calls together in one turn "
               "(e.g. list a folder, read README and the build file at once; read several files at once) instead of one per turn.\n"
               "- Do not call git_status or git_diff to check your own edits: each edit reply already shows the result. Use the git tools only for "
               "git work the user asked for.\n"
               "- Never touch secrets (.env, keys, credentials). Use the git_* tools for git (each asks the user); commit only when the user asked, never push or rewrite history. "
               "Commands and deletions are shown to the user for approval; if one is denied, accept it, say so and carry on without it.\n"
               "- If the request is ambiguous in a way that matters, ask one short question instead of guessing. If it is only a question about the "
               "code, answer it without editing anything.\n\n"
               "The IDE around you\n"
               "- You can also operate QODE itself: get_ide_state, set_theme, show_panel, open_file, set_option, editor_command, "
               "check_language_servers, install_language_server. Use them when the user asks for something about the window or the editor "
               "(\"open the terminal\", \"show me the logo\", \"my eyes hurt\" = switch to a dark theme they are not on, \"turn on inline hints\", "
               "\"organize the imports\"). When such a call is all the request needs, make every call of that turn at once with done=true and "
               "write nothing else: the turn ends without another request and the chat shows the result. Do not change the IDE unasked: "
               "earlier requests in the chat are already done (their calls are noted in parentheses), so act only on the latest message, "
               "and a question (\"is the terminal open?\") gets an answer, not a change.\n"
               "- Language servers: check_language_servers first. If QODE can set one up, ask the user whether to download and set it up and "
               "call install_language_server only after a yes. If it needs sudo (clangd, Node.js, Python, Java), give the exact command for their "
               "distribution in a code block to copy and run themselves; never run sudo.\n\n"
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
    m_messages = claude() ? ClaudeClient::messagesFromContents(config.contents) : QJsonArray();
    m_strippedThinking = false;
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
    endRequest(true); // still open: stopped or failed
    emit status(QString());
    emit usage(m_usage);
    if (error.isEmpty())
        emit finished(m_rounds);
    else
        emit failed(error);
}

// --- request timing and token counts -------------------------------------------------------------------------------
// Both APIs report the output tokens late (Claude only in the closing message_delta, Gemini in some chunks), so the
// live count is the last reported one plus an estimate of what streamed since (about four characters a token).
// Thinking time runs from the first thought to the first answer text or tool call; when the request opens with a
// thought it counts from the moment the request was sent, since the model was thinking before the first summary came.

void AgentRun::beginRequest()
{
    m_round = RequestStats();
    m_estChars = m_estThoughtChars = 0;
    m_thinkFrom = -1;
    m_inRequest = true;
    m_clock.start();
    m_tick->start();
    emit requestProgress(currentRequest());
}

void AgentRun::noteOutput(bool thought, int chars)
{
    if (!m_inRequest)
        return;
    const qint64 now = m_clock.elapsed();
    const bool first = m_round.firstMs < 0;
    if (first)
        m_round.firstMs = now;
    if (thought) {
        if (m_thinkFrom < 0)
            m_thinkFrom = first ? 0 : now;
        m_estThoughtChars += chars;
    } else {
        closeThinking();
    }
    m_estChars += chars;
}

void AgentRun::noteReportedOutput(int output, int thinking)
{
    m_round.output = output;
    m_round.thinking = thinking;
    m_estChars = m_estThoughtChars = 0;
    if (m_inRequest)
        emit requestProgress(currentRequest());
}

void AgentRun::closeThinking()
{
    if (m_thinkFrom < 0)
        return;
    m_round.thinkMs += m_clock.elapsed() - m_thinkFrom;
    m_thinkFrom = -1;
}

RequestStats AgentRun::currentRequest() const
{
    RequestStats r = m_round;
    r.ms = m_clock.isValid() ? m_clock.elapsed() : 0;
    if (m_thinkFrom >= 0)
        r.thinkMs += r.ms - m_thinkFrom;
    r.output += (m_estChars + 3) / 4;
    r.thinking += (m_estThoughtChars + 3) / 4;
    r.exact = m_estChars == 0;
    r.done = false;
    return r;
}

void AgentRun::endRequest(bool stopped)
{
    if (!m_inRequest)
        return;
    closeThinking();
    m_inRequest = false;
    m_tick->stop();
    RequestStats r = currentRequest();
    r.done = true;
    r.stopped = stopped;
    if (stopped && r.prompt == 0 && r.output == 0)
        return; // failed before anything came back: nothing to account for
    emit requestProgress(r);
}

void AgentRun::send()
{
    if (m_cancelled) {
        finish(tr("Stopped."));
        return;
    }
    if (m_rounds >= m_cfg.maxRounds) {
        finish(tr("Stopped after %1 steps. Send \"continue\" to let QODE carry on.").arg(m_cfg.maxRounds));
        return;
    }
    if (claude()) {
        sendClaude();
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
    m_finishReason.clear();
    m_modelParts = QJsonArray();
    m_gotOutput = false;
    emit status(m_rounds == 0 ? tr("Thinking…") : tr("Working…"));
    beginRequest();
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
    if (claude()) {
        handleClaudeEvent(event);
        return;
    }
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
        if (meta.contains("candidatesTokenCount") || meta.contains("thoughtsTokenCount")) {
            const int thoughts = meta.value("thoughtsTokenCount").toInt();
            noteReportedOutput(meta.value("candidatesTokenCount").toInt() + thoughts, thoughts);
        }
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
            noteOutput(part.value("thought").toBool(), int(text.size()));
            if (part.value("thought").toBool()) {
                emit thoughtDelta(text);
            } else {
                m_anyText = true;
                emit status(tr("Writing…"));
                emit textDelta(text);
            }
        } else if (part.contains("functionCall")) {
            m_gotOutput = true;
            noteOutput(false, int(QJsonDocument(part.value("functionCall").toObject()).toJson(QJsonDocument::Compact).size()));
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
    m_inRequest = false; // the retry is a new request, timed from its own start
    m_tick->stop();
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
    if (claude()) {
        claudeReplyFinished(reply);
        return;
    }

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
    endRequest(false);
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
    QVector<QJsonObject> calls;
    QVector<ToolResult> results;
    bool end = true; // every call finished the user's request (see AgentTools::endsTurn)
    for (const QJsonValue &v : std::as_const(m_modelParts)) {
        const QJsonObject call = v.toObject().value("functionCall").toObject();
        if (call.isEmpty())
            continue;
        const QString name = call.value("name").toString();
        const QJsonObject args = call.value("args").toObject();

        emit status(tr("Running %1…").arg(m_tools->label(name)));
        emit toolStarted(name, m_tools->describe(name, args));
        m_inTools = true;
        const ToolResult result = m_tools->execute(name, args);
        m_inTools = false;
        emit toolFinished(name, result);
        if (m_cancelled) {
            finish(tr("Stopped."));
            return false;
        }
        end = end && result.ok && m_tools->endsTurn(name, args);
        calls.append(call);
        results.append(result);
    }
    if (!end && !checkEdits(&results))
        return false;
    QJsonArray responses;
    for (int i = 0; i < calls.size(); ++i) {
        const QJsonObject &call = calls.at(i);
        const ToolResult &result = results.at(i);
        QJsonObject fr{{"name", call.value("name")}, {"response", result.response}};
        if (call.contains("id"))
            fr.insert("id", call.value("id"));
        if (!result.images.isEmpty()) { // multimodal function response: the pictures go inside it
            QJsonArray parts;
            for (const QJsonValue &img : result.images)
                parts.append(QJsonObject{{"inlineData", QJsonObject{{"mimeType", img.toObject().value("mimeType")},
                                                                    {"data", img.toObject().value("data")}}}});
            fr.insert("parts", parts);
        }
        responses.append(QJsonObject{{"functionResponse", fr}});
    }
    if (end) {
        finish(QString());
        return false;
    }
    m_contents.append(QJsonObject{{"role", "model"}, {"parts", m_modelParts}});
    m_contents.append(QJsonObject{{"role", "user"}, {"parts", responses}});
    if (QJsonDocument(m_contents).toJson(QJsonDocument::Compact).size() > kPruneWhenContextExceedsChars)
        m_contents = pruneOldResults(m_contents);
    return true;
}

// Diagnostics after edits: each file the round's edits changed is checked by its language server once the round is
// over (not after every edit: a server needs a moment per check, and a later edit in the same round may fix the error).
// The answer goes into the round's last edit of the file, so the model sees its own errors and can fix them in the same
// run. Shown as a row of its own. False when the run was stopped meanwhile.
bool AgentRun::checkEdits(QVector<ToolResult> *results)
{
    const QStringList files = m_tools->filesToCheck(*results);
    for (const QString &path : files) {
        const QString name = QStringLiteral("check_errors");
        emit status(tr("Checking %1 for errors…").arg(path.section(QLatin1Char('/'), -1)));
        emit toolStarted(name, AgentTools::checkCall(path));
        m_inTools = true;
        // Several files changed together: a file's first report may predate the server's view of the others (Kotlin
        // answers, then asks for a refresh), so the first check waits for the reports to settle; the rest are then current.
        const ToolResult check = m_tools->checkEdited(path, files.size() > 1 && path == files.first());
        m_inTools = false;
        emit toolFinished(name, check);
        if (m_cancelled) {
            finish(tr("Stopped."));
            return false;
        }
        for (int i = int(results->size()) - 1; i >= 0; --i) {
            ToolResult &r = (*results)[i];
            if (r.ok && r.hasChange && r.change.path == path) {
                r.response.insert("diagnostics_after_edit", check.response);
                break;
            }
        }
    }
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
            const bool pictures = fr.contains("parts");
            fr.remove("parts");
            if (raw.size() <= kPruneAboveChars || raw.contains("older result dropped")) {
                if (pictures) {
                    part["functionResponse"] = fr;
                    parts.replace(p, part);
                }
                continue;
            }
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
