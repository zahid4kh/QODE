#include "project/VersionCatalog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>

namespace {

struct Alias {
    QStringList segments;
    QString detail;
};

struct Catalog {
    QList<Alias> libraries, bundles, plugins, versions;
};

struct Cached {
    QDateTime modified;
    qint64 size = -1;
    Catalog catalog;
};

QString stripComment(const QString &line)
{
    bool quoted = false;
    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c == QLatin1Char('\\'))
                ++i;
            else if (c == quote)
                quoted = false;
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quoted = true;
            quote = c;
        } else if (c == QLatin1Char('#')) {
            return line.left(i);
        }
    }
    return line;
}

QString unquote(QString s)
{
    s = s.trimmed();
    if (s.size() >= 2 && (s.startsWith(QLatin1Char('"')) || s.startsWith(QLatin1Char('\''))) && s.endsWith(s.at(0)))
        return s.mid(1, s.size() - 2);
    return s;
}

// Position of the first '=' that is not inside quotes, -1 when there is none.
int equalsSign(const QString &line)
{
    bool quoted = false;
    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c == quote)
                quoted = false;
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quoted = true;
            quote = c;
        } else if (c == QLatin1Char('=')) {
            return i;
        }
    }
    return -1;
}

int balance(const QString &s)
{
    int depth = 0;
    bool quoted = false;
    QChar quote;
    for (const QChar c : s) {
        if (quoted) {
            if (c == quote)
                quoted = false;
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quoted = true;
            quote = c;
        } else if (c == QLatin1Char('[') || c == QLatin1Char('{')) {
            ++depth;
        } else if (c == QLatin1Char(']') || c == QLatin1Char('}')) {
            --depth;
        }
    }
    return depth;
}

// The value of `name = "…"` inside an inline table, e.g. module, version.ref, id.
QString tableValue(const QString &table, const QString &name)
{
    const QRegularExpression re(QLatin1String("(?:^|[{,\\s])") + QRegularExpression::escape(name) + QLatin1String("\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')"));
    const auto m = re.match(table);
    if (!m.hasMatch())
        return {};
    return m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
}

QStringList splitAlias(const QString &alias)
{
    return alias.split(QRegularExpression(QStringLiteral("[-_.]")), Qt::SkipEmptyParts);
}

Catalog parse(const QString &text)
{
    Catalog out;
    QHash<QString, QString> versionNumbers;
    struct Raw {
        QString section, key, value;
    };
    QList<Raw> raws;

    QString section;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = stripComment(lines.at(i)).trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')) && equalsSign(line) < 0) {
            section = line.mid(1, line.size() - 2).trimmed();
            continue;
        }
        const int eq = equalsSign(line);
        if (eq < 0)
            continue;
        QString value = line.mid(eq + 1).trimmed();
        while (balance(value) > 0 && i + 1 < lines.size()) // a multi-line array of a bundle
            value += QLatin1Char(' ') + stripComment(lines.at(++i)).trimmed();
        raws.append({section, unquote(line.left(eq)), value});
    }

    for (const Raw &r : std::as_const(raws))
        if (r.section == QLatin1String("versions")) {
            QString number = r.value.startsWith(QLatin1Char('{')) ? tableValue(r.value, QStringLiteral("strictly")) : unquote(r.value);
            if (number.isEmpty())
                number = tableValue(r.value, QStringLiteral("require"));
            if (number.isEmpty())
                number = tableValue(r.value, QStringLiteral("prefer"));
            versionNumbers.insert(r.key, number);
            out.versions.append({splitAlias(r.key), number});
        }

    auto versionOf = [&](const QString &table) {
        const QString ref = tableValue(table, QStringLiteral("version.ref"));
        if (!ref.isEmpty())
            return versionNumbers.value(ref, ref);
        return tableValue(table, QStringLiteral("version"));
    };

    for (const Raw &r : std::as_const(raws)) {
        const QStringList seg = splitAlias(r.key);
        if (seg.isEmpty())
            continue;
        if (r.section == QLatin1String("libraries")) {
            QString coords, version;
            if (r.value.startsWith(QLatin1Char('{'))) {
                coords = tableValue(r.value, QStringLiteral("module"));
                if (coords.isEmpty())
                    coords = tableValue(r.value, QStringLiteral("group")) + QLatin1Char(':') + tableValue(r.value, QStringLiteral("name"));
                version = versionOf(r.value);
            } else {
                coords = unquote(r.value); // "group:name:version"
                const QStringList parts = coords.split(QLatin1Char(':'));
                if (parts.size() >= 3) {
                    version = parts.at(2);
                    coords = parts.at(0) + QLatin1Char(':') + parts.at(1);
                }
            }
            out.libraries.append({seg, version.isEmpty() ? coords : coords + QLatin1Char(':') + version});
        } else if (r.section == QLatin1String("plugins")) {
            QString id, version;
            if (r.value.startsWith(QLatin1Char('{'))) {
                id = tableValue(r.value, QStringLiteral("id"));
                version = versionOf(r.value);
            } else {
                const QStringList parts = unquote(r.value).split(QLatin1Char(':')); // "id:version"
                id = parts.value(0);
                version = parts.value(1);
            }
            out.plugins.append({seg, version.isEmpty() ? id : id + QLatin1Char(':') + version});
        } else if (r.section == QLatin1String("bundles")) {
            const int n = r.value.count(QLatin1Char('"')) / 2 + r.value.count(QLatin1Char('\'')) / 2;
            out.bundles.append({seg, n == 1 ? QStringLiteral("1 library") : QStringLiteral("%1 libraries").arg(n)});
        }
    }
    return out;
}

QString catalogFileFor(const QString &scriptPath)
{
    QDir dir(QFileInfo(scriptPath).absolutePath());
    for (int i = 0; i < 8; ++i) {
        const QString candidate = dir.filePath(QStringLiteral("gradle/libs.versions.toml"));
        if (QFileInfo::exists(candidate))
            return candidate;
        if (!dir.cdUp())
            break;
    }
    return {};
}

const Catalog &load(const QString &file)
{
    static QHash<QString, Cached> cache;
    const QFileInfo info(file);
    Cached &c = cache[file];
    if (c.size != info.size() || c.modified != info.lastModified()) {
        QFile f(file);
        c.catalog = f.open(QIODevice::ReadOnly) ? parse(QString::fromUtf8(f.readAll())) : Catalog();
        c.size = info.size();
        c.modified = info.lastModified();
    }
    return c.catalog;
}

// The aliases below `path` (segments already typed): their next segment, with what they stand for.
QList<VersionCatalog::Entry> next(const QList<Alias> &aliases, const QStringList &path)
{
    QMap<QString, VersionCatalog::Entry> found;
    QStringList order;
    for (const Alias &a : aliases) {
        if (a.segments.size() <= path.size() || a.segments.mid(0, path.size()) != path)
            continue;
        const QString label = a.segments.at(path.size());
        const bool leaf = a.segments.size() == path.size() + 1;
        if (!found.contains(label)) {
            order << label;
            found[label].label = label;
        }
        VersionCatalog::Entry &e = found[label];
        if (leaf)
            e.detail = a.detail;
        else
            e.group = true;
    }
    QList<VersionCatalog::Entry> out;
    for (const QString &l : std::as_const(order)) {
        VersionCatalog::Entry e = found.value(l);
        if (e.detail.isEmpty() && e.group)
            e.detail = QStringLiteral("…");
        out << e;
    }
    return out;
}

} // namespace

bool VersionCatalog::isBuildScript(const QString &path)
{
    return path.endsWith(QLatin1String(".gradle")) || path.endsWith(QLatin1String(".gradle.kts"));
}

QList<VersionCatalog::Entry> VersionCatalog::complete(const QString &scriptPath, const QString &beforeWord)
{
    static const QRegularExpression chain(QStringLiteral("(?:^|[^A-Za-z0-9_.])libs((?:\\.[A-Za-z_][A-Za-z0-9_]*)*)\\.$"));
    const auto m = chain.match(beforeWord);
    if (!m.hasMatch() || !isBuildScript(scriptPath))
        return {};
    const QString file = catalogFileFor(scriptPath);
    if (file.isEmpty())
        return {};
    const Catalog &cat = load(file);
    QStringList path = m.captured(1).split(QLatin1Char('.'), Qt::SkipEmptyParts);

    if (path.isEmpty()) {
        QList<Entry> out = next(cat.libraries, {});
        auto section = [&](const QString &name, const QList<Alias> &list) {
            if (!list.isEmpty())
                out.append({name, QStringLiteral("[%1]").arg(name), true});
        };
        section(QStringLiteral("versions"), cat.versions);
        section(QStringLiteral("bundles"), cat.bundles);
        section(QStringLiteral("plugins"), cat.plugins);
        return out;
    }
    const QString head = path.first();
    if (head == QLatin1String("versions") || head == QLatin1String("bundles") || head == QLatin1String("plugins")) {
        path.removeFirst();
        return next(head == QLatin1String("versions") ? cat.versions : head == QLatin1String("bundles") ? cat.bundles : cat.plugins, path);
    }
    return next(cat.libraries, path);
}
