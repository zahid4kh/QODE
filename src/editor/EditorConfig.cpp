#include "EditorConfig.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QVector>

namespace EditorConfig {
namespace {

struct Section {
    QString glob;
    QVector<QPair<QString, QString>> props; // lower-case key -> value (lower-cased for the known keys)
};

struct ParsedFile {
    QDateTime modified;
    bool root = false;
    QVector<Section> sections;
};

ParsedFile parse(const QByteArray &data)
{
    ParsedFile out;
    Section *current = nullptr;
    for (QString line : QString::fromUtf8(data).split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            out.sections.append({line.mid(1, line.size() - 2), {}});
            current = &out.sections.last();
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed().toLower();
        QString value = line.mid(eq + 1).trimmed();
        // Older files put comments after a value (" # ..."); no value QODE reads contains one.
        static const QRegularExpression inlineComment(QStringLiteral("\\s[#;].*$"));
        value.remove(inlineComment);
        value = value.trimmed().toLower();
        if (!current) {
            if (key == QLatin1String("root"))
                out.root = value == QLatin1String("true");
            continue;
        }
        current->props.append({key, value});
    }
    return out;
}

const ParsedFile *load(const QString &path)
{
    static QHash<QString, ParsedFile> cache;
    const QFileInfo fi(path);
    if (!fi.isFile()) {
        cache.remove(path);
        return nullptr;
    }
    auto it = cache.find(path);
    if (it != cache.end() && it->modified == fi.lastModified())
        return &it.value();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return nullptr;
    ParsedFile parsed = parse(f.read(256 * 1024));
    parsed.modified = fi.lastModified();
    return &cache.insert(path, parsed).value();
}

// Index of the brace / bracket closing the one at `open`, honouring backslash escapes and nesting; -1 when unclosed.
int closing(const QString &g, int open, QChar o, QChar c)
{
    int depth = 0;
    for (int i = open; i < g.size(); ++i) {
        const QChar ch = g.at(i);
        if (ch == QLatin1Char('\\')) {
            ++i;
        } else if (ch == o) {
            ++depth;
        } else if (ch == c && --depth == 0) {
            return i;
        }
    }
    return -1;
}

// Splits the inside of {a,b{c,d}} at its top-level commas.
QStringList splitAlternatives(const QString &inner)
{
    QStringList parts;
    int depth = 0, start = 0;
    for (int i = 0; i < inner.size(); ++i) {
        const QChar ch = inner.at(i);
        if (ch == QLatin1Char('\\'))
            ++i;
        else if (ch == QLatin1Char('{'))
            ++depth;
        else if (ch == QLatin1Char('}'))
            --depth;
        else if (ch == QLatin1Char(',') && depth == 0) {
            parts << inner.mid(start, i - start);
            start = i + 1;
        }
    }
    parts << inner.mid(start);
    return parts;
}

// Glob -> regular expression. Numeric ranges become capture groups, checked by the caller against `ranges`.
QString toRegex(const QString &g, QVector<QPair<qint64, qint64>> *ranges)
{
    QString re;
    for (int i = 0; i < g.size(); ++i) {
        const QChar ch = g.at(i);
        if (ch == QLatin1Char('\\') && i + 1 < g.size()) {
            re += QRegularExpression::escape(QString(g.at(++i)));
        } else if (ch == QLatin1Char('*')) {
            if (i + 1 < g.size() && g.at(i + 1) == QLatin1Char('*')) {
                ++i;
                if (i + 1 < g.size() && g.at(i + 1) == QLatin1Char('/')) {
                    ++i;
                    re += QStringLiteral("(?:.*/)?"); // `**/` also matches no folder at all
                } else {
                    re += QStringLiteral(".*");
                }
            } else {
                re += QStringLiteral("[^/]*");
            }
        } else if (ch == QLatin1Char('?')) {
            re += QStringLiteral("[^/]");
        } else if (ch == QLatin1Char('[')) {
            const int end = closing(g, i, QLatin1Char('['), QLatin1Char(']'));
            const QString inner = end > i ? g.mid(i + 1, end - i - 1) : QString();
            if (end < 0 || inner.contains(QLatin1Char('/')) || inner.isEmpty()) {
                re += QStringLiteral("\\[");
                continue;
            }
            QString cls = QStringLiteral("[");
            int k = 0;
            if (inner.startsWith(QLatin1Char('!')) || inner.startsWith(QLatin1Char('^'))) {
                cls += QLatin1Char('^');
                k = 1;
            }
            for (; k < inner.size(); ++k) {
                const QChar c = inner.at(k);
                if (c == QLatin1Char('\\') || c == QLatin1Char('[') || c == QLatin1Char(']') || c == QLatin1Char('^'))
                    cls += QLatin1Char('\\');
                cls += c;
            }
            re += cls + QLatin1Char(']');
            i = end;
        } else if (ch == QLatin1Char('{')) {
            const int end = closing(g, i, QLatin1Char('{'), QLatin1Char('}'));
            if (end < 0) {
                re += QStringLiteral("\\{");
                continue;
            }
            const QString inner = g.mid(i + 1, end - i - 1);
            static const QRegularExpression numeric(QStringLiteral("^([+-]?\\d+)\\.\\.([+-]?\\d+)$"));
            const QRegularExpressionMatch m = numeric.match(inner);
            if (m.hasMatch()) {
                const qint64 a = m.captured(1).toLongLong(), b = m.captured(2).toLongLong();
                ranges->append({qMin(a, b), qMax(a, b)});
                re += QStringLiteral("([+-]?\\d+)");
            } else {
                const QStringList alts = splitAlternatives(inner);
                if (alts.size() < 2) { // {single} is literal text
                    re += QStringLiteral("\\{") + toRegex(inner, ranges) + QStringLiteral("\\}");
                } else {
                    QStringList parts;
                    for (const QString &a : alts)
                        parts << toRegex(a, ranges);
                    re += QStringLiteral("(?:") + parts.join(QLatin1Char('|')) + QLatin1Char(')');
                }
            }
            i = end;
        } else {
            re += QRegularExpression::escape(QString(ch));
        }
    }
    return re;
}

} // namespace

bool globMatches(const QString &glob, const QString &relativePath)
{
    QString g = glob;
    // A glob without a slash matches the file name in any folder; one with a slash is relative to the config's folder.
    if (!g.contains(QLatin1Char('/')))
        g = QStringLiteral("**/") + g;
    else if (g.startsWith(QLatin1Char('/')))
        g = g.mid(1);
    QVector<QPair<qint64, qint64>> ranges;
    const QRegularExpression re(QLatin1Char('^') + toRegex(g, &ranges) + QLatin1Char('$'));
    if (!re.isValid())
        return false;
    const QRegularExpressionMatch m = re.match(relativePath);
    if (!m.hasMatch())
        return false;
    for (int i = 0; i < ranges.size(); ++i) {
        const qint64 n = m.captured(i + 1).toLongLong();
        if (n < ranges.at(i).first || n > ranges.at(i).second)
            return false;
    }
    return true;
}

Properties forFile(const QString &absolutePath)
{
    Properties out;
    if (absolutePath.isEmpty())
        return out;
    // The config files from the file's folder up to the root, nearest first.
    QStringList configs;
    QDir dir = QFileInfo(absolutePath).absoluteDir();
    for (int depth = 0; depth < 64; ++depth) {
        const QString candidate = dir.filePath(QStringLiteral(".editorconfig"));
        if (const ParsedFile *pf = load(candidate)) {
            configs.prepend(candidate);
            if (pf->root)
                break;
        }
        if (!dir.cdUp())
            break;
    }
    QHash<QString, QString> props;
    for (const QString &config : std::as_const(configs)) {
        const ParsedFile *pf = load(config);
        if (!pf)
            continue;
        const QString rel = QDir(QFileInfo(config).absolutePath()).relativeFilePath(absolutePath);
        bool used = false;
        for (const Section &s : pf->sections) {
            if (!globMatches(s.glob, rel))
                continue;
            used = true;
            for (const auto &p : s.props) {
                if (p.second == QLatin1String("unset"))
                    props.remove(p.first);
                else
                    props.insert(p.first, p.second);
            }
        }
        if (used)
            out.sources << config;
    }

    const QString style = props.value(QStringLiteral("indent_style"));
    out.useTabs = style == QLatin1String("tab") ? 1 : style == QLatin1String("space") ? 0 : -1;
    bool ok = false;
    const int tabWidth = props.value(QStringLiteral("tab_width")).toInt(&ok);
    if (ok && tabWidth > 0 && tabWidth <= 16)
        out.tabWidth = tabWidth;
    const QString size = props.value(QStringLiteral("indent_size"));
    if (size == QLatin1String("tab")) {
        out.indentSize = out.tabWidth;
    } else {
        const int n = size.toInt(&ok);
        if (ok && n > 0 && n <= 16) {
            out.indentSize = n;
            if (out.tabWidth == 0 && out.useTabs != 0)
                out.tabWidth = n; // the spec: tab_width defaults to indent_size
        }
    }
    const QString eol = props.value(QStringLiteral("end_of_line"));
    out.crlf = eol == QLatin1String("crlf") ? 1 : eol == QLatin1String("lf") ? 0 : -1;
    const QString cs = props.value(QStringLiteral("charset"));
    if (cs == QLatin1String("utf-8") || cs == QLatin1String("utf-8-bom") || cs == QLatin1String("latin1"))
        out.charset = cs;
    auto flag = [&props](const char *key) {
        const QString v = props.value(QLatin1String(key));
        return v == QLatin1String("true") ? 1 : v == QLatin1String("false") ? 0 : -1;
    };
    out.trimTrailingWhitespace = flag("trim_trailing_whitespace");
    out.insertFinalNewline = flag("insert_final_newline");
    return out;
}

} // namespace EditorConfig
