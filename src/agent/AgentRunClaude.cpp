// The Claude half of AgentRun: the agent loop over the Messages API (`POST /v1/messages`, `stream: true`) with the same
// tools the Gemini half uses. One request per round; the assistant turn is rebuilt from the stream events and sent back
// verbatim (thinking blocks with their signatures included), followed by one user message holding every tool_result.
#include "AgentRun.h"

#include "ai/ClaudeClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>

namespace agent {

namespace {
constexpr int kMaxAttempts = 4;
constexpr int kTimeoutMs = 120000; // without any data; the API sends ping events while the model works

QString kindOf(const QJsonObject &block)
{
    return block.value("type").toString();
}

bool parseInput(const QString &raw, QJsonObject *out)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    *out = doc.object();
    return true;
}
} // namespace

void AgentRun::sendClaude()
{
    const QString model = m_cfg.model.trimmed();
    QJsonObject body{{"model", model},
                     {"max_tokens", ClaudeClient::maxTokensFor(model)},
                     {"stream", true},
                     {"messages", m_messages},
                     {"tools", m_tools->claudeToolsJson()},
                     // automatic caching: a breakpoint on the last block, moving forward as the conversation grows
                     {"cache_control", QJsonObject{{"type", "ephemeral"}}}};
    // The system prompt never changes within a chat, so its own breakpoint (it also covers the tools) stays readable
    // whatever happens further down.
    body.insert("system", QJsonArray{QJsonObject{{"type", "text"}, {"text", m_cfg.systemInstruction}, {"cache_control", QJsonObject{{"type", "ephemeral"}}}}});

    QStringList betas;
    QJsonObject thinking = ClaudeClient::thinkingFor(model);
    if (!thinking.isEmpty()) {
        // The history is append-only, so a mismatch should never happen; if one does anyway, drop the stale thinking
        // instead of failing the whole request.
        if (ClaudeClient::bindsThinking(model) && thinking.value("type").toString() == QLatin1String("adaptive")) {
            thinking.insert("block_binding", QJsonObject{{"prefix_mismatch_behavior", "drop_block"}});
            betas << QStringLiteral("thinking-binding-controls-2026-08-01");
        }
        body.insert("thinking", thinking);
    }
    const QString effort = ClaudeClient::effortFor(model, m_cfg.effort);
    if (!effort.isEmpty())
        body.insert("output_config", QJsonObject{{"effort", effort}});
    if (ClaudeClient::offersFallbacks(model)) {
        // A safety classifier that declines a request hands it to the model Anthropic recommends for that category
        // instead of failing the run.
        body.insert("fallbacks", "default");
        betas << QStringLiteral("server-side-fallback-2026-07-01");
    }

    QNetworkRequest req = ClaudeClient::makeRequest(m_cfg.apiKey, betas);
    req.setTransferTimeout(kTimeoutMs);

    m_buffer.clear();
    m_raw.clear();
    m_inlineError.clear();
    m_inlineErrorType.clear();
    m_stopReason.clear();
    m_stopDetails = QJsonObject();
    m_blocks.clear();
    m_partialJson.clear();
    m_badInput.clear();
    m_fallbackAt = -1;
    m_lastWasThought = false;
    m_gotOutput = false;
    emit status(m_rounds == 0 ? tr("Thinking…") : tr("Working…"));
    beginRequest();
    m_reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &AgentRun::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &AgentRun::onReplyFinished);
}

void AgentRun::handleClaudeEvent(const QJsonObject &event)
{
    const QString type = event.value("type").toString();
    if (type == QLatin1String("message_start")) {
        const QJsonObject message = event.value("message").toObject();
        const QJsonObject u = message.value("usage").toObject();
        m_round.cached = u.value("cache_read_input_tokens").toInt();
        m_round.cacheWrite = u.value("cache_creation_input_tokens").toInt();
        m_round.prompt = u.value("input_tokens").toInt() + m_round.cached + m_round.cacheWrite;
        noteReportedOutput(u.value("output_tokens").toInt(), 0);
        return;
    }
    if (type == QLatin1String("content_block_start")) {
        const int index = event.value("index").toInt();
        QJsonObject block = event.value("content_block").toObject();
        while (m_blocks.size() <= index)
            m_blocks.append(QJsonObject());
        const QString kind = kindOf(block);
        if (kind == QLatin1String("tool_use")) {
            block.insert("input", QJsonObject()); // filled from the input_json_delta events at content_block_stop
            m_partialJson.insert(index, QString());
            m_gotOutput = true;
            noteOutput(false, int(block.value("name").toString().size()));
            m_lastWasThought = false;
        } else if (kind == QLatin1String("fallback")) {
            m_fallbackAt = index;
            const QString to = block.value("to").toObject().value("model").toString();
            emit status(tr("Safety filter: continuing on %1…").arg(to.isEmpty() ? tr("another model") : to));
        } else if (kind == QLatin1String("thinking") || kind == QLatin1String("redacted_thinking")) {
            noteOutput(true, int(block.value("thinking").toString().size()));
            if (kind == QLatin1String("redacted_thinking")) {
                m_blocks[index] = block;
                return;
            }
            if (m_lastWasThought)
                emit thoughtDelta(QStringLiteral("\n\n"));
            const QString t = block.value("thinking").toString();
            if (!t.isEmpty()) {
                m_gotOutput = true;
                m_lastWasThought = true;
                emit thoughtDelta(t);
            }
        } else if (kind == QLatin1String("text")) {
            const QString t = block.value("text").toString();
            noteOutput(false, int(t.size()));
            if (!t.isEmpty()) {
                m_gotOutput = m_anyText = true;
                m_lastWasThought = false;
                emit textDelta(t);
            }
        }
        m_blocks[index] = block;
        return;
    }
    if (type == QLatin1String("content_block_delta")) {
        const int index = event.value("index").toInt();
        if (index < 0 || index >= m_blocks.size())
            return;
        QJsonObject &block = m_blocks[index];
        const QJsonObject delta = event.value("delta").toObject();
        const QString kind = delta.value("type").toString();
        if (kind == QLatin1String("text_delta")) {
            const QString t = delta.value("text").toString();
            block["text"] = block.value("text").toString() + t;
            noteOutput(false, int(t.size()));
            if (!t.isEmpty()) {
                m_gotOutput = m_anyText = true;
                m_lastWasThought = false;
                emit status(tr("Writing…"));
                emit textDelta(t);
            }
        } else if (kind == QLatin1String("thinking_delta")) {
            const QString t = delta.value("thinking").toString();
            block["thinking"] = block.value("thinking").toString() + t;
            noteOutput(true, int(t.size()));
            if (!t.isEmpty()) {
                m_gotOutput = true;
                m_lastWasThought = true;
                emit thoughtDelta(t);
            }
        } else if (kind == QLatin1String("signature_delta")) {
            block["signature"] = block.value("signature").toString() + delta.value("signature").toString();
        } else if (kind == QLatin1String("input_json_delta")) {
            const QString part = delta.value("partial_json").toString();
            m_partialJson[index] += part;
            noteOutput(false, int(part.size()));
        }
        return;
    }
    if (type == QLatin1String("content_block_stop")) {
        const int index = event.value("index").toInt();
        if (index < 0 || index >= m_blocks.size() || kindOf(m_blocks.at(index)) != QLatin1String("tool_use"))
            return;
        const QString raw = m_partialJson.take(index).trimmed();
        if (raw.isEmpty())
            return; // a tool without arguments
        QJsonObject input;
        if (parseInput(raw, &input))
            m_blocks[index]["input"] = input;
        else
            m_badInput.insert(index); // eager input streaming: the server does not validate it for us
        return;
    }
    if (type == QLatin1String("message_delta")) {
        const QJsonObject delta = event.value("delta").toObject();
        if (delta.contains("stop_reason") && !delta.value("stop_reason").isNull())
            m_stopReason = delta.value("stop_reason").toString();
        m_stopDetails = delta.value("stop_details").toObject();
        const QJsonObject u = event.value("usage").toObject();
        if (u.contains("input_tokens") && !u.value("input_tokens").isNull()) {
            m_round.cached = u.value("cache_read_input_tokens").toInt(m_round.cached);
            m_round.cacheWrite = u.value("cache_creation_input_tokens").toInt(m_round.cacheWrite);
            m_round.prompt = u.value("input_tokens").toInt() + m_round.cached + m_round.cacheWrite;
        }
        if (u.contains("output_tokens")) { // cumulative; thinking_tokens counts the raw reasoning, not the summary we saw
            const RequestStats live = currentRequest();
            noteReportedOutput(u.value("output_tokens").toInt(), u.value("output_tokens_details").toObject().value("thinking_tokens").toInt(live.thinking));
        }
        return;
    }
    if (type == QLatin1String("error")) {
        const QJsonObject e = event.value("error").toObject();
        m_inlineErrorType = e.value("type").toString();
        m_inlineError = e.value("message").toString(tr("Claude reported an error."));
        return;
    }
    // ping, message_stop and anything new: nothing to do
}

// The assistant turn as it goes back to the API. After a safety fallback switched models mid-answer, everything but text
// before the last `fallback` block belongs to the declined attempt and is left out; the fallback markers themselves are
// audit-only. Empty text blocks are rejected by the API.
QJsonArray AgentRun::claudeContent(QVector<bool> *badInput) const
{
    QJsonArray out;
    badInput->clear();
    for (int i = 0; i < m_blocks.size(); ++i) {
        const QJsonObject &b = m_blocks.at(i);
        const QString kind = kindOf(b);
        if (kind.isEmpty() || kind == QLatin1String("fallback"))
            continue;
        if (i < m_fallbackAt && kind != QLatin1String("text"))
            continue;
        if (kind == QLatin1String("text") && b.value("text").toString().isEmpty())
            continue;
        out.append(b);
        badInput->append(m_badInput.contains(i));
    }
    return out;
}

// The API rejects thinking blocks whose conversation changed. The run never edits its history, but should it happen
// anyway (an older account without block_binding, say) the one-time recovery is to send the history without them.
void AgentRun::stripClaudeThinking()
{
    for (int i = 0; i < m_messages.size(); ++i) {
        QJsonObject msg = m_messages.at(i).toObject();
        if (msg.value("role").toString() != QLatin1String("assistant") || !msg.value("content").isArray())
            continue;
        QJsonArray kept;
        for (const QJsonValue &v : msg.value("content").toArray()) {
            const QString kind = kindOf(v.toObject());
            if (kind != QLatin1String("thinking") && kind != QLatin1String("redacted_thinking"))
                kept.append(v);
        }
        if (kept.isEmpty())
            kept.append(QJsonObject{{"type", "text"}, {"text", "(working)"}});
        msg["content"] = kept;
        m_messages.replace(i, msg);
    }
}

void AgentRun::claudeReplyFinished(QNetworkReply *reply)
{
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
            finish(tr("The connection broke while Claude was answering (%1). Send the request again.").arg(netText));
            return;
        }
        if (httpStatus == 0) {
            if (m_attempt < kMaxAttempts && netError != QNetworkReply::TimeoutError) {
                retryLater(tr("Network problem"), 3 * (m_attempt + 1));
                return;
            }
            finish(netError == QNetworkReply::TimeoutError ? tr("Claude did not answer in time. The model \"%1\" may be overloaded; try another one.").arg(m_cfg.model)
                                                           : tr("Network error: %1").arg(netText));
            return;
        }
        const QString message = body.value("error").toObject().value("message").toString();
        if (httpStatus == 400 && message.contains(QLatin1String("bound to a different conversation")) && !m_strippedThinking) {
            m_strippedThinking = true;
            stripClaudeThinking();
            retryLater(tr("Resending without earlier reasoning"), 1);
            return;
        }
        // A 429 without retry-after is a spend cap that keeps failing; everything else transient is worth a retry.
        const int asked = ClaudeClient::retryAfter(reply);
        const bool transient = ClaudeClient::isTransient(httpStatus) && (httpStatus != 429 || asked > 0);
        if (transient && m_attempt < kMaxAttempts) {
            const int wait = qBound(2, asked > 0 ? asked : 3 * (m_attempt + 1) * (m_attempt + 1), 60);
            retryLater(httpStatus == 429 ? tr("Rate limited") : httpStatus == 529 ? tr("Claude is overloaded") : tr("Server error %1").arg(httpStatus), wait);
            return;
        }
        finish(ClaudeClient::errorText(body, httpStatus, netText, m_cfg.model));
        return;
    }
    if (!m_inlineError.isEmpty()) { // an error event inside a 200 stream
        const bool transient = m_inlineErrorType == QLatin1String("overloaded_error") || m_inlineErrorType == QLatin1String("api_error");
        if (transient && !m_gotOutput && m_attempt < kMaxAttempts) {
            retryLater(m_inlineErrorType == QLatin1String("overloaded_error") ? tr("Claude is overloaded") : tr("Server error"), 3 * (m_attempt + 1) * (m_attempt + 1));
            return;
        }
        finish(m_gotOutput ? tr("Claude stopped with an error while answering: %1").arg(m_inlineError) : m_inlineError);
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
    if (m_stopReason == QLatin1String("refusal")) {
        const QString category = m_stopDetails.value("category").toString();
        finish(category.isEmpty() ? tr("Claude declined this request.")
                                  : tr("Claude declined this request (safety category: %1). Rephrase it or pick another model.").arg(category));
        return;
    }
    if (m_stopReason == QLatin1String("model_context_window_exceeded")) {
        finish(tr("The conversation no longer fits in the model's context window. Use /compact or start a new chat."));
        return;
    }

    // Tool inputs whose block never saw content_block_stop (the stream ended inside them).
    for (auto it = m_partialJson.cbegin(); it != m_partialJson.cend(); ++it) {
        QJsonObject input;
        if (it.value().trimmed().isEmpty())
            continue;
        if (it.key() < m_blocks.size() && parseInput(it.value().trimmed(), &input))
            m_blocks[it.key()]["input"] = input;
        else
            m_badInput.insert(it.key());
    }
    m_partialJson.clear();

    QVector<bool> bad;
    QJsonArray content = claudeContent(&bad);

    if (m_stopReason == QLatin1String("max_tokens")) {
        // The answer was cut off: a tool call at the end is incomplete. Keep what is whole and ask the model to go on.
        for (int i = content.size() - 1; i >= 0; --i) {
            if (kindOf(content.at(i).toObject()) == QLatin1String("tool_use") && bad.at(i)) {
                content.removeAt(i);
                bad.removeAt(i);
            }
        }
    }

    bool hasCalls = false;
    for (const QJsonValue &v : std::as_const(content))
        hasCalls = hasCalls || kindOf(v.toObject()) == QLatin1String("tool_use");

    if (hasCalls) {
        if (!runClaudeTools(content, bad))
            return;
        send();
        return;
    }
    if (m_stopReason == QLatin1String("max_tokens")) {
        if (m_attempt < 2 && m_rounds < m_cfg.maxRounds) {
            ++m_attempt;
            if (!content.isEmpty())
                m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});
            m_messages.append(QJsonObject{{"role", "user"}, {"content", "Your last response was cut off at the output limit. Continue from where it stopped, "
                                                                         "and keep each tool call smaller (for example, write a large file in parts)."}});
            send();
            return;
        }
        finish(tr("Claude ran out of output tokens. Try a smaller request."));
        return;
    }
    if (m_stopReason == QLatin1String("pause_turn")) { // the server paused a long turn: send it back to continue
        if (!content.isEmpty())
            m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});
        send();
        return;
    }
    if (!m_anyText) {
        if (m_rounds > 1 && m_attempt < 2) { // it did its work but said nothing: ask for the report instead of failing
            ++m_attempt;
            if (!content.isEmpty())
                m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});
            m_messages.append(QJsonObject{{"role", "user"}, {"content", "Summarize briefly what you did and the result."}});
            send();
            return;
        }
        finish(tr("Claude returned an empty answer (stop reason: %1).").arg(m_stopReason.isEmpty() ? QStringLiteral("none") : m_stopReason));
        return;
    }
    finish(QString());
}

// Runs the tool_use blocks of the finished turn in order and queues one user message with all their results.
bool AgentRun::runClaudeTools(const QJsonArray &content, const QVector<bool> &badInput)
{
    QVector<QJsonObject> calls;
    QVector<ToolResult> toolResults;
    bool end = true; // every call finished the user's request (see AgentTools::endsTurn)
    for (int i = 0; i < content.size(); ++i) {
        const QJsonObject block = content.at(i).toObject();
        if (kindOf(block) != QLatin1String("tool_use"))
            continue;
        const QString name = block.value("name").toString();
        const QJsonObject args = block.value("input").toObject();

        ToolResult result;
        emit status(tr("Running %1…").arg(m_tools->label(name)));
        emit toolStarted(name, m_tools->describe(name, args));
        if (badInput.value(i)) {
            result.ok = false;
            result.response = QJsonObject{{"error", "the tool input was not valid JSON (it may have been cut off); call the tool again with complete arguments"}};
            result.summary = QStringLiteral("invalid arguments");
            result.call = name;
        } else {
            m_inTools = true;
            result = m_tools->execute(name, args);
            m_inTools = false;
        }
        emit toolFinished(name, result);
        if (m_cancelled) {
            finish(tr("Stopped."));
            return false;
        }
        end = end && result.ok && !badInput.value(i) && m_tools->endsTurn(name, args);
        calls.append(block);
        toolResults.append(result);
    }
    if (!end && !checkEdits(&toolResults))
        return false;
    QJsonArray results;
    for (int i = 0; i < calls.size(); ++i) {
        const ToolResult &result = toolResults.at(i);
        const QString text = QString::fromUtf8(QJsonDocument(result.response).toJson(QJsonDocument::Compact));
        QJsonObject item{{"type", "tool_result"}, {"tool_use_id", calls.at(i).value("id")}, {"content", text}};
        if (!result.images.isEmpty()) {
            QJsonArray blocks{QJsonObject{{"type", "text"}, {"text", text}}};
            for (const QJsonValue &img : result.images)
                blocks.append(QJsonObject{{"type", "image"},
                                          {"source", QJsonObject{{"type", "base64"},
                                                                 {"media_type", img.toObject().value("mimeType")},
                                                                 {"data", img.toObject().value("data")}}}});
            item.insert("content", blocks);
        }
        if (!result.ok)
            item.insert("is_error", true);
        results.append(item);
    }
    if (end) {
        finish(QString());
        return false;
    }
    m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});
    m_messages.append(QJsonObject{{"role", "user"}, {"content", results}});
    return true;
}

} // namespace agent
