#include "http/HttpFile.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace {

bool isComment(const QString &trimmed)
{
    return trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QLatin1String("//"));
}

// "> {% … %}", "> handler.js", "<> previous-response.json", ">> out.json": the tail of a block after its body.
bool isTailLine(const QString &line)
{
    static const QRegularExpression tail(QStringLiteral(R"(^(?:>>!?\s+\S|>\s*\{%|>\s+\S+\.js\s*$|<>\s+\S))"));
    return tail.match(line).hasMatch();
}

void finish(HttpRequestSpec &r)
{
    while (!r.bodyLines.isEmpty() && r.bodyLines.last().trimmed().isEmpty())
        r.bodyLines.removeLast();
    int tail = -1;
    for (int i = 0; i < r.bodyLines.size(); ++i)
        if (isTailLine(r.bodyLines.at(i))) {
            tail = i;
            break;
        }
    if (tail >= 0) {
        static const QRegularExpression save(QStringLiteral(R"(^>>(!?)\s+(.+?)\s*$)"));
        for (int i = tail; i < r.bodyLines.size(); ++i) {
            const QString &l = r.bodyLines.at(i);
            const auto m = save.match(l);
            if (m.hasMatch()) {
                r.saveTo = m.captured(2);
                r.overwrite = !m.captured(1).isEmpty();
            } else if (l.startsWith(QLatin1Char('>')) && !l.startsWith(QLatin1String(">>"))) {
                r.handlerScript = true;
            }
        }
        r.bodyLines = r.bodyLines.mid(0, tail);
        while (!r.bodyLines.isEmpty() && r.bodyLines.last().trimmed().isEmpty())
            r.bodyLines.removeLast();
    }
}

int timeoutOf(const QString &arg)
{
    static const QRegularExpression re(QStringLiteral(R"(^(\d+)\s*(ms|s|m)?$)"));
    const auto m = re.match(arg.trimmed());
    if (!m.hasMatch())
        return 0;
    const int n = m.captured(1).toInt();
    const QString unit = m.captured(2);
    return unit == QLatin1String("ms") ? n : unit == QLatin1String("m") ? n * 60000 : n * 1000;
}

} // namespace

QString HttpRequestSpec::label() const
{
    if (!name.isEmpty())
        return name;
    if (!title.isEmpty())
        return title;
    return method + QLatin1Char(' ') + url;
}

int HttpFileParse::requestAt(int line) const
{
    for (int i = 0; i < requests.size(); ++i)
        if (line >= requests.at(i).firstLine && line <= requests.at(i).lastLine)
            return i;
    return -1;
}

int HttpFileParse::indexOfName(const QString &name) const
{
    for (int i = 0; i < requests.size(); ++i)
        if (requests.at(i).name == name)
            return i;
    return -1;
}

QList<int> HttpFileParse::requestLines() const
{
    QList<int> lines;
    for (const HttpRequestSpec &r : requests)
        lines << r.line;
    return lines;
}

QByteArray HttpResult::header(const QByteArray &wanted) const
{
    for (const auto &h : headers)
        if (h.first.compare(wanted, Qt::CaseInsensitive) == 0)
            return h.second;
    return {};
}

QString HttpResult::contentType() const
{
    QString t = QString::fromLatin1(header("Content-Type")).toLower();
    const int semi = t.indexOf(QLatin1Char(';'));
    if (semi >= 0)
        t.truncate(semi);
    return t.trimmed();
}

namespace HttpFile {

const QStringList &methods()
{
    static const QStringList m{QStringLiteral("GET"),     QStringLiteral("POST"),    QStringLiteral("PUT"),
                               QStringLiteral("PATCH"),   QStringLiteral("DELETE"),  QStringLiteral("HEAD"),
                               QStringLiteral("OPTIONS"), QStringLiteral("TRACE"),   QStringLiteral("CONNECT"),
                               QStringLiteral("WEBSOCKET"), QStringLiteral("GRPC")};
    return m;
}

bool isHttpFile(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("http") || ext == QLatin1String("rest");
}

HttpFileParse parse(const QString &text)
{
    static const QRegularExpression separator(QStringLiteral("^###"));
    static const QRegularExpression directive(QStringLiteral(R"(^(?:#|//)\s*@([\w-]+)\s*=?\s*(.*)$)"));
    static const QRegularExpression variable(QStringLiteral(R"(^@([\w.$-]+)\s*=\s*(.*?)\s*$)"));
    static const QRegularExpression requestLine(QStringLiteral(R"(^([A-Za-z]+)\s+(.+?)(?:\s+HTTP/[\d.]+)?\s*$)"));
    static const QRegularExpression version(QStringLiteral(R"(\s+HTTP/[\d.]+\s*$)"));

    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &l : lines)
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);

    HttpFileParse out;
    enum State { Pre, Url, Headers, Body };
    int start = 0;
    while (start < lines.size()) {
        int end = start + 1;
        while (end < lines.size() && !separator.match(lines.at(end)).hasMatch())
            ++end;
        HttpRequestSpec r;
        r.firstLine = start;
        r.lastLine = end - 1;
        int i = start;
        if (separator.match(lines.at(start)).hasMatch()) {
            QString title = lines.at(start);
            while (title.startsWith(QLatin1Char('#')))
                title.remove(0, 1);
            r.title = title.trimmed();
            ++i;
        }
        State state = Pre;
        for (; i < end; ++i) {
            const QString &line = lines.at(i);
            const QString t = line.trimmed();
            if (state == Pre) {
                if (t.isEmpty())
                    continue;
                if (isComment(t)) {
                    const auto d = directive.match(t);
                    if (!d.hasMatch())
                        continue;
                    const QString key = d.captured(1).toLower(), arg = d.captured(2).trimmed();
                    if (key == QLatin1String("name"))
                        r.name = arg.section(QLatin1Char(' '), 0, 0);
                    else if (key == QLatin1String("no-redirect"))
                        r.noRedirect = true;
                    else if (key == QLatin1String("no-cookie-jar"))
                        r.noCookieJar = true;
                    else if (key == QLatin1String("timeout"))
                        r.timeoutMs = timeoutOf(arg);
                    else if (key == QLatin1String("prompt") && !arg.isEmpty())
                        r.prompts.append({arg.section(QLatin1Char(' '), 0, 0), arg.section(QLatin1Char(' '), 1).trimmed()});
                    continue;
                }
                if (const auto v = variable.match(t); v.hasMatch()) {
                    out.variables.append({v.captured(1), v.captured(2), i});
                    continue;
                }
                const auto m = requestLine.match(t);
                if (m.hasMatch() && methods().contains(m.captured(1).toUpper())) {
                    r.method = m.captured(1).toUpper();
                    r.url = m.captured(2).trimmed();
                } else {
                    r.url = t;
                    r.url.remove(version);
                }
                r.line = i;
                state = Url;
                continue;
            }
            if (state == Url) {
                if (t.startsWith(QLatin1Char('?')) || t.startsWith(QLatin1Char('&'))) {
                    r.url += t;
                    continue;
                }
                state = Headers;
            }
            if (state == Headers) {
                if (t.isEmpty()) {
                    state = Body;
                    continue;
                }
                if (isComment(t))
                    continue;
                const int colon = t.indexOf(QLatin1Char(':'));
                if (colon > 0) {
                    r.headers.append({t.left(colon).trimmed(), t.mid(colon + 1).trimmed()});
                    continue;
                }
                state = Body; // no blank line before the body
            }
            r.bodyLines << line;
        }
        if (r.line >= 0) {
            finish(r);
            out.requests.append(r);
        }
        start = end;
    }
    return out;
}

QString format(const QString &name, const QString &method, const QString &url, const QString &headers, const QString &body)
{
    QString out = QStringLiteral("### %1\n").arg(name.isEmpty() ? method + QLatin1Char(' ') + url : name);
    out += method + QLatin1Char(' ') + url + QLatin1Char('\n');
    for (const QString &line : headers.split(QLatin1Char('\n'))) {
        const QString l = line.trimmed();
        if (!l.isEmpty() && !l.startsWith(QLatin1Char('#')) && l.contains(QLatin1Char(':')))
            out += l + QLatin1Char('\n');
    }
    if (!body.trimmed().isEmpty())
        out += QLatin1Char('\n') + body.trimmed() + QLatin1Char('\n');
    return out;
}

} // namespace HttpFile
