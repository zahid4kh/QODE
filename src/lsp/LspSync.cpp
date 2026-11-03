#include "LspSync.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace LspSync {

namespace {
QJsonObject position(int line, qsizetype character)
{
    return QJsonObject{{QStringLiteral("line"), line}, {QStringLiteral("character"), int(character)}};
}
} // namespace

QJsonObject contentChange(const QString &before, const QString &after)
{
    const qsizetype common = qMin(before.size(), after.size());
    const QChar *b = before.constData(), *a = after.constData();
    qsizetype prefix = std::mismatch(b, b + common, a).first - b;
    if (prefix == before.size() && prefix == after.size())
        return {};
    if (prefix > 0 && b[prefix - 1].isHighSurrogate())
        --prefix;
    qsizetype suffix = 0;
    const qsizetype maxSuffix = common - prefix;
    while (suffix < maxSuffix && b[before.size() - 1 - suffix] == a[after.size() - 1 - suffix])
        ++suffix;
    const qsizetype end = before.size() - suffix;
    if (suffix > 0 && end < before.size() && b[end].isLowSurrogate())
        --suffix;
    const qsizetype removedEnd = before.size() - suffix;

    // Line and column of both ends in the old text (the server holds it). Only '\n' ends a line: Document::text() has
    // no '\r'.
    int line = 0;
    qsizetype lineStart = 0;
    for (qsizetype i = 0; i < prefix; ++i)
        if (b[i] == QLatin1Char('\n')) {
            ++line;
            lineStart = i + 1;
        }
    const QJsonObject start = position(line, prefix - lineStart);
    for (qsizetype i = prefix; i < removedEnd; ++i)
        if (b[i] == QLatin1Char('\n')) {
            ++line;
            lineStart = i + 1;
        }
    const QJsonObject stop = position(line, removedEnd - lineStart);
    return QJsonObject{{QStringLiteral("range"), QJsonObject{{QStringLiteral("start"), start}, {QStringLiteral("end"), stop}}},
                       {QStringLiteral("text"), after.mid(prefix, after.size() - suffix - prefix)}};
}

bool wantsIncremental(const QJsonObject &caps)
{
    const QJsonValue sync = caps.value(QStringLiteral("textDocumentSync"));
    const int kind = sync.isObject() ? sync.toObject().value(QStringLiteral("change")).toInt(1) : sync.toInt(1);
    return kind == 2;
}

QVector<quint32> tokenData(const QJsonArray &data)
{
    QVector<quint32> out;
    out.reserve(data.size());
    for (const QJsonValue &v : data)
        out.append(quint32(v.toDouble())); // modifier bits may use the 32nd bit
    return out;
}

bool applyTokenEdits(QVector<quint32> &data, const QJsonArray &edits)
{
    struct Edit {
        qsizetype start, deleteCount;
        QVector<quint32> data;
    };
    QVector<Edit> list;
    list.reserve(edits.size());
    for (const QJsonValue &v : edits) {
        const QJsonObject e = v.toObject();
        list.append({qsizetype(e.value(QStringLiteral("start")).toDouble(-1)), qsizetype(e.value(QStringLiteral("deleteCount")).toDouble(0)),
                     tokenData(e.value(QStringLiteral("data")).toArray())});
    }
    // The offsets all refer to the old array: apply from the back so earlier ones stay valid.
    std::sort(list.begin(), list.end(), [](const Edit &x, const Edit &y) { return x.start > y.start; });
    qsizetype limit = data.size();
    for (const Edit &e : std::as_const(list))
        if (e.start < 0 || e.deleteCount < 0 || e.start + e.deleteCount > limit)
            return false;
        else
            limit = e.start; // edits must not overlap
    for (const Edit &e : std::as_const(list)) {
        QVector<quint32> next;
        next.reserve(data.size() - e.deleteCount + e.data.size());
        next << data.mid(0, e.start) << e.data << data.mid(e.start + e.deleteCount);
        data = std::move(next);
    }
    return true;
}

bool globMatches(const QString &glob, const QString &path, bool ignoreCase)
{
    static QHash<QString, QRegularExpression> cache;
    const QString key = (ignoreCase ? QLatin1Char('i') : QLatin1Char('c')) + glob;
    auto it = cache.find(key);
    if (it == cache.end()) {
        QString rx;
        int braces = 0;
        for (qsizetype i = 0; i < glob.size(); ++i) {
            const QChar c = glob.at(i);
            if (c == QLatin1Char('*')) {
                if (i + 1 < glob.size() && glob.at(i + 1) == QLatin1Char('*')) {
                    const bool slash = i + 2 < glob.size() && glob.at(i + 2) == QLatin1Char('/');
                    rx += slash ? QStringLiteral("(?:.*/)?") : QStringLiteral(".*");
                    i += slash ? 2 : 1;
                } else {
                    rx += QStringLiteral("[^/]*");
                }
            } else if (c == QLatin1Char('?')) {
                rx += QStringLiteral("[^/]");
            } else if (c == QLatin1Char('{')) {
                rx += QStringLiteral("(?:");
                ++braces;
            } else if (c == QLatin1Char('}') && braces > 0) {
                rx += QLatin1Char(')');
                --braces;
            } else if (c == QLatin1Char(',') && braces > 0) {
                rx += QLatin1Char('|');
            } else if (c == QLatin1Char('[')) {
                const qsizetype close = glob.indexOf(QLatin1Char(']'), i + 1);
                if (close < 0) {
                    rx += QStringLiteral("\\[");
                    continue;
                }
                QString set = glob.mid(i + 1, close - i - 1);
                if (set.startsWith(QLatin1Char('!')))
                    set[0] = QLatin1Char('^');
                set.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
                rx += QLatin1Char('[') + set + QLatin1Char(']');
                i = close;
            } else {
                rx += QRegularExpression::escape(QString(c));
            }
        }
        while (braces-- > 0)
            rx += QLatin1Char(')');
        // A relative glob ("*.java", "src/**") may match the end of the absolute path.
        const QString anchored = glob.startsWith(QLatin1Char('/')) || glob.startsWith(QLatin1String("**"))
                                     ? QStringLiteral("\\A%1\\z").arg(rx)
                                     : QStringLiteral("(?:\\A|/)%1\\z").arg(rx);
        it = cache.insert(key, QRegularExpression(anchored, ignoreCase ? QRegularExpression::CaseInsensitiveOption
                                                                       : QRegularExpression::NoPatternOption));
    }
    return it->isValid() && it->match(path).hasMatch();
}

bool fileOperationMatches(const QJsonObject &registration, const QString &path, bool isDir)
{
    for (const QJsonValue &v : registration.value(QStringLiteral("filters")).toArray()) {
        const QJsonObject filter = v.toObject();
        const QString scheme = filter.value(QStringLiteral("scheme")).toString();
        if (!scheme.isEmpty() && scheme != QLatin1String("file"))
            continue;
        const QJsonObject pattern = filter.value(QStringLiteral("pattern")).toObject();
        const QString matches = pattern.value(QStringLiteral("matches")).toString();
        if ((matches == QLatin1String("file") && isDir) || (matches == QLatin1String("folder") && !isDir))
            continue;
        const bool ignoreCase = pattern.value(QStringLiteral("options")).toObject().value(QStringLiteral("ignoreCase")).toBool();
        if (globMatches(pattern.value(QStringLiteral("glob")).toString(), path, ignoreCase))
            return true;
    }
    return false;
}

} // namespace LspSync
