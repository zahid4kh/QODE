#include "http/HttpVars.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUuid>

namespace {

constexpr int kMaxDepth = 10;

const QRegularExpression &placeholder()
{
    static const QRegularExpression re(QStringLiteral(R"(\{\{\s*(.*?)\s*\}\})"));
    return re;
}

// Words of a system variable's arguments; '…' and "…" stay one word (without the quotes).
QStringList arguments(const QString &text)
{
    static const QRegularExpression word(QStringLiteral(R"re('([^']*)'|"([^"]*)"|(\S+))re"));
    QStringList out;
    auto it = word.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        out << (m.capturedStart(1) >= 0 ? m.captured(1) : m.capturedStart(2) >= 0 ? m.captured(2) : m.captured(3));
    }
    return out;
}

// "{{$timestamp -1 d}}": the offset after the format moves the time.
QDateTime shifted(QDateTime t, const QStringList &args, int from)
{
    if (args.size() < from + 2)
        return t;
    bool ok = false;
    const qint64 n = args.at(from).toLongLong(&ok);
    if (!ok)
        return t;
    const QString unit = args.at(from + 1);
    if (unit == QLatin1String("y"))
        return t.addYears(int(n));
    if (unit == QLatin1String("M"))
        return t.addMonths(int(n));
    if (unit == QLatin1String("w"))
        return t.addDays(n * 7);
    if (unit == QLatin1String("d"))
        return t.addDays(n);
    if (unit == QLatin1String("h"))
        return t.addSecs(n * 3600);
    if (unit == QLatin1String("m"))
        return t.addSecs(n * 60);
    if (unit == QLatin1String("s"))
        return t.addSecs(n);
    if (unit == QLatin1String("ms"))
        return t.addMSecs(n);
    return t;
}

// Day.js style format ("YYYY-MM-DD HH:mm:ss") as a Qt one.
QString qtFormat(const QString &f)
{
    static const QRegularExpression token(QStringLiteral("YYYY|YY|DD|D|SSS|A|a|\\[[^\\]]*\\]"));
    QString out;
    int last = 0;
    auto it = token.globalMatch(f);
    while (it.hasNext()) {
        const auto m = it.next();
        out += f.mid(last, m.capturedStart() - last);
        const QString t = m.captured();
        if (t == QLatin1String("YYYY"))
            out += QStringLiteral("yyyy");
        else if (t == QLatin1String("YY"))
            out += QStringLiteral("yy");
        else if (t == QLatin1String("DD"))
            out += QStringLiteral("dd");
        else if (t == QLatin1String("D"))
            out += QStringLiteral("d");
        else if (t == QLatin1String("SSS"))
            out += QStringLiteral("zzz");
        else if (t == QLatin1String("A"))
            out += QStringLiteral("AP");
        else if (t == QLatin1String("a"))
            out += QStringLiteral("ap");
        else
            out += QLatin1Char('\'') + t.mid(1, t.size() - 2) + QLatin1Char('\'');
        last = m.capturedEnd();
    }
    return out + f.mid(last);
}

QString randomText(int n, const char *alphabet)
{
    const int size = int(qstrlen(alphabet));
    QString out;
    for (int i = 0; i < qBound(0, n, 4096); ++i)
        out += QLatin1Char(alphabet[QRandomGenerator::global()->bounded(size)]);
    return out;
}

QString headerValue(const QList<QPair<QByteArray, QByteArray>> &headers, const QString &name)
{
    for (const auto &h : headers)
        if (QString::fromLatin1(h.first).compare(name, Qt::CaseInsensitive) == 0)
            return QString::fromUtf8(h.second);
    return {};
}

} // namespace

QString HttpResolver::resolve(const QString &text)
{
    return resolve(text, 0);
}

QString HttpResolver::resolve(const QString &text, int depth)
{
    if (!text.contains(QLatin1String("{{")))
        return text;
    QString out;
    int last = 0;
    auto it = placeholder().globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        QString value;
        if (lookup(m.captured(1), &value, nullptr, depth)) {
            out += value;
        } else {
            out += m.captured();
            if (!m_missing.contains(m.captured(1)))
                m_missing << m.captured(1);
        }
        last = m.capturedEnd();
    }
    return out + text.mid(last);
}

bool HttpResolver::lookup(const QString &expr, QString *value, QString *source, int depth)
{
    const QString e = expr.trimmed();
    if (e.isEmpty() || depth > kMaxDepth)
        return false;
    if (e.startsWith(QLatin1Char('$')))
        return system(e, value, source, depth);
    if (e.contains(QLatin1String(".response.")) || e.contains(QLatin1String(".request.")))
        return fromResponse(e, value, source);
    auto take = [&](const QString &raw, const QString &from) {
        *value = resolve(raw, depth + 1);
        if (source)
            *source = from;
        return true;
    };
    if (m_ctx.prompts.contains(e))
        return take(m_ctx.prompts.value(e), QObject::tr("your answer to @prompt"));
    if (m_ctx.fileVars.contains(e))
        return take(m_ctx.fileVars.value(e), QObject::tr("a variable of this file"));
    if (m_ctx.env.contains(e))
        return take(m_ctx.env.value(e), m_ctx.envName.isEmpty() ? QObject::tr("the shared environment variables")
                                                                : QObject::tr("environment \"%1\"").arg(m_ctx.envName));
    return false;
}

bool HttpResolver::system(const QString &e, QString *value, QString *source, int depth)
{
    static const QRegularExpression call(QStringLiteral(R"(^\$random\.(\w+)(?:\((.*)\))?$)"));
    const int space = e.indexOf(QLatin1Char(' '));
    const QString name = space < 0 ? e : e.left(space);
    const QStringList args = arguments(space < 0 ? QString() : e.mid(space + 1));
    if (source)
        *source = QObject::tr("made when the request is sent");
    auto *rng = QRandomGenerator::global();
    const QDateTime now = QDateTime::currentDateTimeUtc();

    if (name == QLatin1String("$guid") || name == QLatin1String("$uuid") || name == QLatin1String("$randomUUID")) {
        *value = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return true;
    }
    if (name == QLatin1String("$timestamp")) {
        *value = QString::number(shifted(now, args, 0).toSecsSinceEpoch());
        return true;
    }
    if (name == QLatin1String("$isoTimestamp")) {
        *value = now.toString(Qt::ISODate);
        return true;
    }
    if (name == QLatin1String("$randomInt")) {
        const int lo = args.value(0, QStringLiteral("0")).toInt(), hi = args.value(1, QStringLiteral("1000")).toInt();
        *value = QString::number(hi > lo ? rng->bounded(lo, hi) : lo);
        return true;
    }
    if (name == QLatin1String("$datetime") || name == QLatin1String("$localDatetime")) {
        const bool local = name == QLatin1String("$localDatetime");
        const QDateTime t = shifted(local ? QDateTime::currentDateTime() : now, args, 1);
        const QString fmt = args.value(0, QStringLiteral("iso8601"));
        if (fmt == QLatin1String("rfc1123"))
            *value = QLocale::c().toString(t.toUTC(), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
        else if (fmt == QLatin1String("iso8601"))
            *value = t.toString(Qt::ISODateWithMs);
        else
            *value = t.toString(qtFormat(fmt));
        return true;
    }
    if (name == QLatin1String("$processEnv") || name == QLatin1String("$dotenv")) {
        QString key = args.value(0);
        if (key.startsWith(QLatin1Char('%'))) { // the name of the variable is itself a variable
            QString indirect;
            if (!lookup(key.mid(1), &indirect, nullptr, depth + 1))
                return false;
            key = indirect;
        }
        if (key.isEmpty())
            return false;
        if (name == QLatin1String("$dotenv")) {
            if (!m_ctx.dotenv.contains(key))
                return false;
            *value = m_ctx.dotenv.value(key);
            if (source)
                *source = QObject::tr(".env");
            return true;
        }
        if (!qEnvironmentVariableIsSet(key.toLocal8Bit().constData()))
            return false;
        *value = qEnvironmentVariable(key.toLocal8Bit().constData());
        if (source)
            *source = QObject::tr("QODE's environment");
        return true;
    }
    if (name == QLatin1String("$shared")) {
        if (!m_ctx.env.contains(args.value(0)))
            return false;
        *value = resolve(m_ctx.env.value(args.value(0)), depth + 1);
        return true;
    }
    if (name == QLatin1String("$springBase")) {
        if (m_ctx.springBase.isEmpty())
            return false;
        *value = m_ctx.springBase;
        if (source)
            *source = QObject::tr("the Spring Boot application");
        return true;
    }
    if (const auto m = call.match(e); m.hasMatch()) {
        const QString fn = m.captured(1);
        QStringList a;
        for (const QString &s : m.captured(2).split(QLatin1Char(','), Qt::SkipEmptyParts))
            a << s.trimmed();
        if (fn == QLatin1String("uuid")) {
            *value = QUuid::createUuid().toString(QUuid::WithoutBraces);
        } else if (fn == QLatin1String("integer")) {
            const int lo = a.value(0, QStringLiteral("0")).toInt(), hi = a.value(1, QStringLiteral("1000")).toInt();
            *value = QString::number(hi > lo ? rng->bounded(lo, hi) : lo);
        } else if (fn == QLatin1String("float")) {
            const double lo = a.value(0, QStringLiteral("0")).toDouble(), hi = a.value(1, QStringLiteral("1000")).toDouble();
            *value = QString::number(lo + rng->generateDouble() * (hi - lo));
        } else if (fn == QLatin1String("alphabetic")) {
            *value = randomText(a.value(0, QStringLiteral("10")).toInt(), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
        } else if (fn == QLatin1String("alphanumeric")) {
            *value = randomText(a.value(0, QStringLiteral("10")).toInt(), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
        } else if (fn == QLatin1String("hexadecimal")) {
            *value = randomText(a.value(0, QStringLiteral("10")).toInt(), "0123456789abcdef");
        } else if (fn == QLatin1String("email")) {
            *value = randomText(8, "abcdefghijklmnopqrstuvwxyz") + QStringLiteral("@example.com");
        } else {
            return false;
        }
        return true;
    }
    return false;
}

// name.response.body.<path> | name.response.headers.<Header> | name.request.body.<path> | name.request.headers.<Header>
bool HttpResolver::fromResponse(const QString &e, QString *value, QString *source)
{
    static const QRegularExpression re(QStringLiteral(R"(^([\w-]+)\.(response|request)\.(body|headers)(?:\.(.*))?$)"));
    const auto m = re.match(e);
    if (!m.hasMatch() || !m_ctx.responses.contains(m.captured(1)))
        return false;
    const HttpResult &r = m_ctx.responses.value(m.captured(1));
    const bool response = m.captured(2) == QLatin1String("response");
    const QString path = m.captured(4);
    if (source)
        *source = QObject::tr("the last response of \"%1\"").arg(m.captured(1));
    if (m.captured(3) == QLatin1String("headers")) {
        *value = headerValue(response ? r.headers : r.request.headers, path);
        return !value->isEmpty();
    }
    const QByteArray body = response ? r.body : r.request.body;
    bool ok = false;
    *value = HttpVars::jsonPath(body, path.isEmpty() ? QStringLiteral("*") : path, &ok);
    return ok;
}

namespace HttpVars {

QStringList referencedNames(const HttpRequestSpec &spec)
{
    static const QRegularExpression ref(QStringLiteral(R"(\{\{\s*([\w-]+)\.(?:response|request)\.)"));
    QStringList names;
    auto scan = [&](const QString &text) {
        auto it = ref.globalMatch(text);
        while (it.hasNext()) {
            const QString n = it.next().captured(1);
            if (!names.contains(n))
                names << n;
        }
    };
    scan(spec.url);
    for (const auto &h : spec.headers)
        scan(h.second);
    for (const QString &l : spec.bodyLines)
        scan(l);
    return names;
}

QString jsonPath(const QByteArray &json, const QString &path, bool *ok)
{
    *ok = false;
    if (path == QLatin1String("*") || path == QLatin1String("$")) {
        *ok = true;
        return QString::fromUtf8(json);
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError)
        return {};
    QJsonValue v = doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
    QString p = path.trimmed();
    if (p.startsWith(QLatin1Char('$')))
        p.remove(0, 1);
    static const QRegularExpression step(QStringLiteral(R"re(\.([^.\[]+)|\[(\d+)\]|\['([^']*)'\]|\["([^"]*)"\])re"));
    if (!p.isEmpty() && !p.startsWith(QLatin1Char('.')) && !p.startsWith(QLatin1Char('[')))
        p.prepend(QLatin1Char('.'));
    int pos = 0;
    while (pos < p.size()) {
        const auto m = step.match(p, pos, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (!m.hasMatch())
            return {};
        if (m.capturedStart(2) >= 0) {
            if (!v.isArray())
                return {};
            const QJsonArray a = v.toArray();
            const int i = m.captured(2).toInt();
            if (i >= a.size())
                return {};
            v = a.at(i);
        } else {
            const QString key = m.capturedStart(1) >= 0 ? m.captured(1) : m.capturedStart(3) >= 0 ? m.captured(3) : m.captured(4);
            if (!v.isObject() || !v.toObject().contains(key))
                return {};
            v = v.toObject().value(key);
        }
        pos = m.capturedEnd();
    }
    *ok = true;
    if (v.isObject())
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (v.isArray())
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    if (v.isDouble()) {
        const double d = v.toDouble();
        return d == qint64(d) ? QString::number(qint64(d)) : QString::number(d, 'g', 17);
    }
    if (v.isBool())
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isNull())
        return QStringLiteral("null");
    return v.toString();
}

bool build(const HttpRequestSpec &spec, const HttpContext &ctx, HttpRequestData *out, QString *error, QString *note)
{
    static const QRegularExpression include(QStringLiteral(R"(^<(@?)\s+(.+?)\s*$)"));
    if (spec.method == QLatin1String("WEBSOCKET") || spec.method == QLatin1String("GRPC")) {
        *error = QObject::tr("%1 requests are not supported, only HTTP.").arg(spec.method);
        return false;
    }
    HttpResolver r(ctx);
    QString url = r.resolve(spec.url).trimmed();

    HttpHeaders headers;
    bool graphql = false;
    QString contentType;
    for (const auto &h : spec.headers) {
        const QString value = r.resolve(h.second);
        if (h.first.compare(QLatin1String("X-Request-Type"), Qt::CaseInsensitive) == 0 && value.compare(QLatin1String("GraphQL"), Qt::CaseInsensitive) == 0) {
            graphql = true;
            continue;
        }
        if (h.first.compare(QLatin1String("Content-Type"), Qt::CaseInsensitive) == 0)
            contentType = value.toLower();
        headers.append({r.resolve(h.first), value});
    }

    // Body: "< file" lines bring in a file as it is, "<@ file" with its variables filled in.
    QByteArray body;
    const bool multipart = contentType.startsWith(QLatin1String("multipart/"));
    const bool form = contentType.startsWith(QLatin1String("application/x-www-form-urlencoded"));
    const QByteArray newline = multipart ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\n");
    for (int i = 0; i < spec.bodyLines.size(); ++i) {
        const QString &line = spec.bodyLines.at(i);
        QByteArray chunk;
        if (const auto m = include.match(line); m.hasMatch()) {
            const QString path = QDir(ctx.dir).absoluteFilePath(r.resolve(m.captured(2)));
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                *error = QObject::tr("Cannot read %1").arg(QDir::toNativeSeparators(path));
                return false;
            }
            chunk = f.readAll();
            if (!m.captured(1).isEmpty())
                chunk = r.resolve(QString::fromUtf8(chunk)).toUtf8();
        } else {
            chunk = r.resolve(form ? line.trimmed() : line).toUtf8();
        }
        if (i > 0 && !form)
            body += newline;
        body += chunk;
    }
    if (multipart && !body.isEmpty() && !body.endsWith("\r\n"))
        body += "\r\n";

    if (!r.missing().isEmpty()) {
        QStringList names;
        for (const QString &m : r.missing())
            names << QStringLiteral("{{%1}}").arg(m);
        const QString env = ctx.envName.isEmpty() ? QObject::tr("no environment selected") : QObject::tr("environment \"%1\"").arg(ctx.envName);
        *error = QObject::tr("Not defined (%1): %2").arg(env, names.join(QStringLiteral(", ")));
        return false;
    }

    if (graphql) {
        // The query, a blank line, then the variables as JSON.
        const QString text = QString::fromUtf8(body);
        static const QRegularExpression blank(QStringLiteral("\\n\\s*\\n"));
        const auto m = blank.match(text);
        QJsonObject o;
        o.insert(QStringLiteral("query"), (m.hasMatch() ? text.left(m.capturedStart()) : text).trimmed());
        if (m.hasMatch()) {
            QJsonParseError err;
            const QJsonDocument vars = QJsonDocument::fromJson(text.mid(m.capturedEnd()).toUtf8(), &err);
            if (err.error != QJsonParseError::NoError) {
                *error = QObject::tr("The GraphQL variables are not valid JSON: %1").arg(err.errorString());
                return false;
            }
            o.insert(QStringLiteral("variables"), vars.object());
        }
        body = QJsonDocument(o).toJson(QJsonDocument::Compact);
        if (contentType.isEmpty()) {
            headers.append({QStringLiteral("Content-Type"), QStringLiteral("application/json")});
            contentType = QStringLiteral("application/json");
        }
    }

    // "GET /path" with a Host header, as in a raw HTTP message.
    if (url.startsWith(QLatin1Char('/'))) {
        QString host;
        for (const auto &h : headers)
            if (h.first.compare(QLatin1String("Host"), Qt::CaseInsensitive) == 0)
                host = h.second;
        if (!host.isEmpty())
            url = (host.contains(QStringLiteral("://")) ? host : QStringLiteral("http://") + host) + url;
        else if (!ctx.springBase.isEmpty())
            url = ctx.springBase + url;
        else {
            *error = QObject::tr("%1 has no host: write the full URL, or add a Host header").arg(url);
            return false;
        }
    } else if (!url.contains(QStringLiteral("://"))) {
        url.prepend(QStringLiteral("http://"));
    }
    const QUrl qurl(url, QUrl::TolerantMode);
    if (!qurl.isValid() || qurl.host().isEmpty()) {
        *error = QObject::tr("Not a valid URL: %1").arg(url);
        return false;
    }

    out->method = graphql && spec.method == QLatin1String("GET") ? QStringLiteral("POST") : spec.method;
    out->url = qurl;
    out->headers.clear();
    bool hasType = !contentType.isEmpty();
    for (const auto &h : std::as_const(headers))
        out->headers.append({h.first.toUtf8(), h.second.toUtf8()});
    const QByteArray trimmed = body.trimmed();
    if (!trimmed.isEmpty() && !hasType)
        out->headers.append({QByteArrayLiteral("Content-Type"),
                             trimmed.startsWith('{') || trimmed.startsWith('[') ? QByteArrayLiteral("application/json") : QByteArrayLiteral("text/plain")});
    out->body = body;
    out->followRedirects = !spec.noRedirect;
    out->useCookies = !spec.noCookieJar;
    if (spec.timeoutMs > 0)
        out->timeoutMs = spec.timeoutMs;
    if (spec.handlerScript)
        *note = QObject::tr("Response handler scripts (> {% … %}) are not run by QODE.");
    return true;
}

QString describeAt(const QString &lineText, int column, const HttpContext &ctx)
{
    auto it = placeholder().globalMatch(lineText);
    while (it.hasNext()) {
        const auto m = it.next();
        if (column < m.capturedStart() || column >= m.capturedEnd())
            continue;
        const QString expr = m.captured(1);
        HttpResolver r(ctx);
        QString value, source;
        if (r.lookup(expr, &value, &source)) {
            if (value.size() > 400)
                value = value.left(400) + QStringLiteral(" …");
            return QObject::tr("%1 = %2\nfrom %3").arg(expr, value, source);
        }
        static const QRegularExpression ref(QStringLiteral(R"(^([\w-]+)\.(?:response|request)\.)"));
        if (const auto rm = ref.match(expr); rm.hasMatch())
            return QObject::tr("%1\nSend the request \"%2\" first; its response fills this in.").arg(expr, rm.captured(1));
        const QString env = ctx.envName.isEmpty() ? QObject::tr("no environment selected") : QObject::tr("environment \"%1\"").arg(ctx.envName);
        return QObject::tr("%1 is not defined (%2)").arg(expr, env);
    }
    return {};
}

} // namespace HttpVars
