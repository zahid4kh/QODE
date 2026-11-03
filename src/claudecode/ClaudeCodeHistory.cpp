#include "ClaudeCodeHistory.h"

#include "ai/AiQuote.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <algorithm>

namespace claudecode::history {

namespace {
QString configDir()
{
    const QString custom = QProcessEnvironment::systemEnvironment().value(QStringLiteral("CLAUDE_CONFIG_DIR"));
    return custom.isEmpty() ? QDir::homePath() + QStringLiteral("/.claude") : custom;
}

// A prompt the user typed, as opposed to command output, reminders and other text the CLI adds as user messages.
QString promptText(const QJsonValue &content)
{
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else {
        for (const QJsonValue &b : content.toArray()) {
            const QJsonObject o = b.toObject();
            if (o.value(QStringLiteral("type")).toString() == QLatin1String("tool_result"))
                return {};
            // Blocks that start with '<' are added context (an editor selection, reminders), not what was typed; an
            // `@path` block is an attached file.
            const QString t = o.value(QStringLiteral("text")).toString();
            if (o.value(QStringLiteral("type")).toString() == QLatin1String("text") && !t.trimmed().startsWith(QLatin1Char('<')) &&
                fileOfBlock(t).isEmpty())
                text += t;
        }
    }
    text = text.trimmed();
    if (text.startsWith(QLatin1Char('<')) || text.startsWith(QLatin1String("Caveat:")))
        return {};
    return text;
}

QString titleOf(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    // The newest title is near the end; a big file is not read whole for it.
    if (f.size() > 512 * 1024)
        f.seek(f.size() - 512 * 1024);
    QString title;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (!line.contains("\"ai-title\"") && !line.contains("\"summary\""))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const QString t = o.value(QStringLiteral("type")).toString();
        if (t == QLatin1String("ai-title"))
            title = o.value(QStringLiteral("aiTitle")).toString();
        else if (t == QLatin1String("summary") && title.isEmpty())
            title = o.value(QStringLiteral("summary")).toString();
    }
    if (!title.isEmpty())
        return title;
    f.seek(0);
    for (int n = 0; n < 400 && !f.atEnd(); ++n) {
        const QByteArray line = f.readLine();
        if (!line.contains("\"type\":\"user\""))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.value(QStringLiteral("isMeta")).toBool() || o.value(QStringLiteral("isSidechain")).toBool())
            continue;
        const QString p = promptText(o.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")));
        if (!p.isEmpty())
            return p.simplified().left(80);
    }
    return {};
}
} // namespace

QString fileBlock(const QString &relativePath)
{
    return relativePath.contains(QLatin1Char(' ')) ? QStringLiteral("@\"%1\"").arg(relativePath) : QLatin1Char('@') + relativePath;
}

QString fileOfBlock(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^@(?:\"([^\"\\n]+)\"|(\\S+))$"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1) + m.captured(2) : QString();
}

QString folderFor(const QString &projectRoot)
{
    QString name = QDir::cleanPath(projectRoot);
    for (QChar &c : name)
        if (!c.isLetterOrNumber() || c.unicode() > 127)
            c = QLatin1Char('-');
    return configDir() + QStringLiteral("/projects/") + name;
}

QVector<Info> list(const QString &projectRoot, int max)
{
    QVector<Info> out;
    if (projectRoot.isEmpty())
        return out;
    const QFileInfoList files = QDir(folderFor(projectRoot)).entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
    for (const QFileInfo &fi : files) {
        if (out.size() >= max)
            break;
        Info i;
        i.id = fi.completeBaseName();
        i.title = titleOf(fi.absoluteFilePath());
        i.updated = fi.lastModified();
        if (!i.title.isEmpty()) // nothing was asked in it (an empty or aborted start)
            out.append(i);
    }
    return out;
}

Conversation load(const QString &projectRoot, const QString &id)
{
    Conversation conv;
    QVector<QJsonObject> &out = conv.entries;
    QFile f(folderFor(projectRoot) + QLatin1Char('/') + id + QStringLiteral(".jsonl"));
    if (!f.open(QIODevice::ReadOnly))
        return conv;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (line.contains("\"compact_boundary\"")) {
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            if (o.value(QStringLiteral("subtype")).toString() == QLatin1String("compact_boundary") && !o.value(QStringLiteral("isSidechain")).toBool())
                conv.contextTokens = -1; // the size is known again after the next reply
            continue;
        }
        if (!line.contains("\"type\":\"user\"") && !line.contains("\"type\":\"assistant\""))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const QString type = o.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("assistant") && !o.value(QStringLiteral("isSidechain")).toBool()) {
            // Every request's prompt is the whole context so far; with its output that is what the next one starts from.
            const QJsonObject u = o.value(QStringLiteral("message")).toObject().value(QStringLiteral("usage")).toObject();
            const int prompt = u.value(QStringLiteral("input_tokens")).toInt() + u.value(QStringLiteral("cache_read_input_tokens")).toInt() +
                               u.value(QStringLiteral("cache_creation_input_tokens")).toInt();
            if (prompt > 0)
                conv.contextTokens = prompt + u.value(QStringLiteral("output_tokens")).toInt();
        }
        if ((type != QLatin1String("user") && type != QLatin1String("assistant")) || o.value(QStringLiteral("isSidechain")).toBool() ||
            o.value(QStringLiteral("isMeta")).toBool() || o.value(QStringLiteral("isCompactSummary")).toBool())
            continue;
        const QJsonObject msg = o.value(QStringLiteral("message")).toObject();
        QJsonObject e{{"role", type}, {"content", msg.value(QStringLiteral("content"))}, {"model", msg.value(QStringLiteral("model"))},
                      {"id", msg.value(QStringLiteral("id"))}};
        if (type == QLatin1String("user")) {
            const QJsonValue c = msg.value(QStringLiteral("content"));
            const bool toolResult = c.isArray() && !c.toArray().isEmpty() &&
                                    c.toArray().first().toObject().value(QStringLiteral("type")).toString() == QLatin1String("tool_result");
            if (!toolResult) {
                const QString p = promptText(c);
                // What went with the prompt: pasted images, editor selections and quotes (QODE's attachments).
                QJsonArray images, selections, files;
                QString quote;
                for (const QJsonValue &b : c.toArray()) {
                    const QJsonObject block = b.toObject();
                    const QString type = block.value(QStringLiteral("type")).toString();
                    const QString text = block.value(QStringLiteral("text")).toString();
                    if (type == QLatin1String("image"))
                        images.append(block.value(QStringLiteral("source")));
                    else if (type == QLatin1String("text") && text.startsWith(QLatin1String("<ide_selection>")))
                        selections.append(text);
                    else if (type == QLatin1String("text") && !fileOfBlock(text).isEmpty())
                        files.append(fileOfBlock(text));
                    else if (type == QLatin1String("text") && quote.isEmpty())
                        quote = AiQuote::ofBlock(text);
                }
                if (p.isEmpty() && images.isEmpty())
                    continue;
                e.insert(QStringLiteral("content"), p);
                if (!images.isEmpty())
                    e.insert(QStringLiteral("images"), images);
                if (!selections.isEmpty())
                    e.insert(QStringLiteral("selections"), selections);
                if (!files.isEmpty())
                    e.insert(QStringLiteral("files"), files);
                if (!quote.isEmpty())
                    e.insert(QStringLiteral("quote"), quote);
            }
            const QJsonValue r = o.value(QStringLiteral("toolUseResult"));
            if (r.isObject())
                e.insert(QStringLiteral("toolUseResult"), r);
        }
        out.append(e);
    }
    return conv;
}

QStringList typedPrompts(const QString &projectRoot, int max)
{
    QStringList out;
    QFile f(configDir() + QStringLiteral("/history.jsonl"));
    if (projectRoot.isEmpty() || !f.open(QIODevice::ReadOnly))
        return out;
    // Appended to forever: only its end is read.
    if (f.size() > 2 * 1024 * 1024) {
        f.seek(f.size() - 2 * 1024 * 1024);
        f.readLine(); // the rest of a cut line
    }
    const QString root = QDir::cleanPath(projectRoot);
    while (!f.atEnd()) {
        const QJsonObject o = QJsonDocument::fromJson(f.readLine()).object();
        if (QDir::cleanPath(o.value(QStringLiteral("project")).toString()) != root)
            continue;
        // Prompts with pasted text are left out: they show it as a "[Pasted text #1]" placeholder, which would be sent as is.
        if (!o.value(QStringLiteral("pastedContents")).toObject().isEmpty())
            continue;
        const QString text = o.value(QStringLiteral("display")).toString().trimmed();
        if (!text.isEmpty() && (out.isEmpty() || out.last() != text))
            out << text;
    }
    return out.mid(qMax(0, int(out.size()) - max));
}

} // namespace claudecode::history
