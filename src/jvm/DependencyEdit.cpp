#include "jvm/DependencyEdit.h"

#include "project/BuildFileEdit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace DependencyEdit {

namespace {

QString readText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(2 * 1024 * 1024));
}

QString relative(const QString &root, const QString &file)
{
    return QDir(root).relativeFilePath(file);
}

bool literalVersion(const QString &v)
{
    return !v.isEmpty() && v.at(0).isDigit() && !v.contains(QLatin1Char('$'));
}

void scanGradle(const QString &root, const QString &file, QList<Declared> &out)
{
    const QString text = readText(file);
    static const QRegularExpression coord(QStringLiteral(R"re(["']([\w.\-]+):([\w.\-]+):([\w.\-+]+)(?:@\w+)?["'])re"));
    static const QRegularExpression groovyMap(
        QStringLiteral(R"re(group\s*:\s*['"]([\w.\-]+)['"]\s*,\s*name\s*:\s*['"]([\w.\-]+)['"]\s*,\s*version\s*:\s*['"]([\w.\-+]+)['"])re"));
    for (const QRegularExpression *re : {&coord, &groovyMap}) {
        QRegularExpressionMatchIterator it = re->globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (!literalVersion(m.captured(3)))
                continue;
            // Skip commented-out lines.
            const int ls = text.lastIndexOf(QLatin1Char('\n'), int(m.capturedStart())) + 1;
            if (QStringView(text).mid(ls, m.capturedStart() - ls).trimmed().startsWith(QLatin1String("//")))
                continue;
            Declared d{m.captured(1), m.captured(2), m.captured(3), file, int(m.capturedStart(3)), int(m.capturedLength(3)), relative(root, file), {}};
            out << d;
        }
    }
}

void scanCatalog(const QString &file, QList<Declared> &out)
{
    const QString text = readText(file);
    if (text.isEmpty())
        return;
    // [versions]: key -> (value, offset)
    struct Ver { QString value; int start = 0; };
    QHash<QString, Ver> versions;
    QString section;
    int offset = 0;
    struct RefUse { QString group, artifact; };
    QHash<QString, QList<RefUse>> refs;
    QStringList refOrder;
    static const QRegularExpression verLine(QStringLiteral(R"re(^\s*([\w.\-]+)\s*=\s*"([^"]+)")re"));
    static const QRegularExpression libString(QStringLiteral(R"re(^\s*([\w.\-]+)\s*=\s*"([\w.\-]+):([\w.\-]+):([^"]+)")re"));
    static const QRegularExpression module(QStringLiteral(R"re(module\s*=\s*"([\w.\-]+):([\w.\-]+)")re"));
    static const QRegularExpression groupName(QStringLiteral(R"re(group\s*=\s*"([\w.\-]+)"\s*,\s*name\s*=\s*"([\w.\-]+)")re"));
    static const QRegularExpression version(QStringLiteral(R"re(\bversion\s*=\s*"([^"]+)")re"));
    static const QRegularExpression versionRef(QStringLiteral(R"re(version\.ref\s*=\s*"([\w.\-]+)"|version\s*=\s*\{\s*ref\s*=\s*"([\w.\-]+)")re"));
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString t = line.trimmed();
        if (t.startsWith(QLatin1Char('['))) {
            section = t;
        } else if (!t.startsWith(QLatin1Char('#'))) {
            if (section == QLatin1String("[versions]")) {
                if (const QRegularExpressionMatch m = verLine.match(line); m.hasMatch())
                    versions.insert(m.captured(1), {m.captured(2), offset + int(m.capturedStart(2))});
            } else if (section == QLatin1String("[libraries]")) {
                if (const QRegularExpressionMatch m = libString.match(line); m.hasMatch()) {
                    if (literalVersion(m.captured(4)))
                        out << Declared{m.captured(2), m.captured(3), m.captured(4), file, offset + int(m.capturedStart(4)), int(m.capturedLength(4)),
                                        QStringLiteral("libs.versions.toml"), {}};
                } else {
                    QRegularExpressionMatch mm = module.match(line);
                    if (!mm.hasMatch())
                        mm = groupName.match(line);
                    if (mm.hasMatch()) {
                        if (const QRegularExpressionMatch r = versionRef.match(line); r.hasMatch()) {
                            const QString key = r.captured(1).isEmpty() ? r.captured(2) : r.captured(1);
                            if (!refs.contains(key))
                                refOrder << key;
                            refs[key] << RefUse{mm.captured(1), mm.captured(2)};
                        } else if (const QRegularExpressionMatch v = version.match(line); v.hasMatch() && literalVersion(v.captured(1))) {
                            out << Declared{mm.captured(1), mm.captured(2), v.captured(1), file, offset + int(v.capturedStart(1)), int(v.capturedLength(1)),
                                            QStringLiteral("libs.versions.toml"), {}};
                        }
                    }
                }
            }
        }
        offset += int(line.size()) + 1;
    }
    // One entry per shared [versions] key, checked against its first library.
    for (const QString &key : std::as_const(refOrder)) {
        const auto v = versions.constFind(key);
        if (v == versions.constEnd() || !literalVersion(v->value))
            continue;
        const QList<RefUse> uses = refs.value(key);
        Declared d{uses.first().group, uses.first().artifact, v->value, file, v->start, int(v->value.size()),
                   QStringLiteral("libs.versions.toml [versions] %1").arg(key), {}};
        for (int i = 1; i < uses.size(); ++i)
            d.sharedBy << uses.at(i).artifact;
        out << d;
    }
}

void scanPom(const QString &root, const QString &file, QList<Declared> &out)
{
    const QString text = readText(file);
    static const QRegularExpression block(QStringLiteral(R"((<(dependency|parent)>)([\s\S]*?)</\2>)"));
    static const QRegularExpression g(QStringLiteral(R"(<groupId>\s*([^<\s]+)\s*</groupId>)"));
    static const QRegularExpression a(QStringLiteral(R"(<artifactId>\s*([^<\s]+)\s*</artifactId>)"));
    static const QRegularExpression v(QStringLiteral(R"(<version>\s*([^<\s]+)\s*</version>)"));
    QHash<QString, int> propertyIndex; // a property used by several dependencies is listed once
    QRegularExpressionMatchIterator it = block.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString body = m.captured(3);
        const int base = int(m.capturedStart(3));
        const QRegularExpressionMatch gm = g.match(body), am = a.match(body), vm = v.match(body);
        if (!gm.hasMatch() || !am.hasMatch() || !vm.hasMatch())
            continue;
        const QString ver = vm.captured(1);
        if (literalVersion(ver)) {
            out << Declared{gm.captured(1), am.captured(1), ver, file, base + int(vm.capturedStart(1)), int(ver.size()), relative(root, file), {}};
            continue;
        }
        static const QRegularExpression prop(QStringLiteral(R"(^\$\{([\w.\-]+)\}$)"));
        const QString name = prop.match(ver).captured(1);
        if (name.isEmpty())
            continue;
        if (const auto seen = propertyIndex.constFind(name); seen != propertyIndex.constEnd()) {
            out[*seen].sharedBy << am.captured(1);
            continue;
        }
        const QRegularExpression def(QStringLiteral("<%1>\\s*([^<\\s]+)\\s*</%1>").arg(QRegularExpression::escape(name)));
        const QRegularExpressionMatch dm = def.match(text);
        if (!dm.hasMatch() || !literalVersion(dm.captured(1)))
            continue;
        propertyIndex.insert(name, int(out.size()));
        out << Declared{gm.captured(1), am.captured(1), dm.captured(1), file, int(dm.capturedStart(1)), int(dm.capturedLength(1)),
                        QStringLiteral("%1 <%2>").arg(relative(root, file), name), {}};
    }
}

QString aliasFor(const QString &artifact)
{
    QString a = artifact.toLower();
    a.replace(QLatin1Char('.'), QLatin1Char('-')).replace(QLatin1Char('_'), QLatin1Char('-'));
    return a;
}

QString accessorFor(QString alias)
{
    return alias.replace(QLatin1Char('-'), QLatin1Char('.')).replace(QLatin1Char('_'), QLatin1Char('.'));
}

} // namespace

QStringList buildFiles(const QString &root)
{
    QStringList out;
    QStringList dirs{root};
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (!d.startsWith(QLatin1Char('.')) && d != QLatin1String("build") && d != QLatin1String("buildSrc") && d != QLatin1String("gradle")
            && d != QLatin1String("target") && d != QLatin1String("src") && d != QLatin1String("node_modules"))
            dirs << root + QLatin1Char('/') + d;
    for (const QString &dir : std::as_const(dirs))
        for (const char *name : {"build.gradle.kts", "build.gradle", "pom.xml"})
            if (QFileInfo::exists(dir + QLatin1Char('/') + QLatin1String(name))) {
                out << dir + QLatin1Char('/') + QLatin1String(name);
                break;
            }
    return out;
}

QStringList gradleScopes()
{
    return {QStringLiteral("implementation"), QStringLiteral("api"), QStringLiteral("compileOnly"), QStringLiteral("runtimeOnly"),
            QStringLiteral("testImplementation"), QStringLiteral("testRuntimeOnly"), QStringLiteral("ksp"), QStringLiteral("kapt")};
}

QStringList mavenScopes()
{
    return {QStringLiteral("compile"), QStringLiteral("test"), QStringLiteral("provided"), QStringLiteral("runtime")};
}

QString catalogPath(const QString &root)
{
    const QString p = root + QStringLiteral("/gradle/libs.versions.toml");
    return QFileInfo::exists(p) ? p : QString();
}

bool buildUsesCatalog(const QString &buildFile)
{
    static const QRegularExpression re(QStringLiteral(R"(\b(?:implementation|api|compileOnly|runtimeOnly|testImplementation)\s*\(?\s*libs\.)"));
    return re.match(readText(buildFile)).hasMatch();
}

QList<Declared> declared(const QString &root)
{
    QList<Declared> out;
    for (const QString &f : buildFiles(root)) {
        if (f.endsWith(QLatin1String("pom.xml")))
            scanPom(root, f, out);
        else
            scanGradle(root, f, out);
    }
    if (const QString toml = catalogPath(root); !toml.isEmpty())
        scanCatalog(toml, out);
    return out;
}

QList<Declared> declaredInScript(const QString &path, const QString &text)
{
    QList<Declared> out;
    static const QRegularExpression ann(QStringLiteral(R"(@file:DependsOn\s*\(([^)]*)\))"));
    static const QRegularExpression coord(QStringLiteral(R"re("([\w.\-]+):([\w.\-]+):([\w.\-+]+)")re"));
    QRegularExpressionMatchIterator it = ann.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QRegularExpressionMatchIterator ci = coord.globalMatch(m.captured(1));
        while (ci.hasNext()) {
            const QRegularExpressionMatch c = ci.next();
            if (literalVersion(c.captured(3)))
                out << Declared{c.captured(1), c.captured(2), c.captured(3), path, int(m.capturedStart(1) + c.capturedStart(3)),
                                int(c.capturedLength(3)), QFileInfo(path).fileName(), {}};
        }
    }
    return out;
}

Change add(const QString &root, const QString &buildFile, const QString &group, const QString &artifact, const QString &version,
           const QString &scopeIn, bool useCatalog)
{
    Change c;
    c.file = buildFile;
    c.text = readText(buildFile);
    if (c.text.isEmpty()) {
        c.error = QObject::tr("%1 could not be read.").arg(QFileInfo(buildFile).fileName());
        return c;
    }
    const QString coordinate = group + QLatin1Char(':') + artifact;
    if (buildFile.endsWith(QLatin1String("pom.xml"))) {
        if (c.text.contains(QStringLiteral("<artifactId>%1</artifactId>").arg(artifact))
            && c.text.contains(QStringLiteral("<groupId>%1</groupId>").arg(group))) {
            c.error = QObject::tr("%1 is already a dependency in pom.xml.").arg(coordinate);
            return c;
        }
        const QString scope = scopeIn == QLatin1String("compile") ? QString() : scopeIn;
        if (!BuildFileEdit::insertMavenDependencies(c.text, {BuildFileEdit::MavenDependency{group, artifact, version, scope, false}})) {
            c.error = QObject::tr("pom.xml has no </project> to add the dependency before.");
            return c;
        }
        c.summary = coordinate + QLatin1Char(':') + version + (scope.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(scope));
        c.ok = true;
        return c;
    }
    const bool kts = buildFile.endsWith(QLatin1String(".kts"));
    const QString scope = scopeIn.isEmpty() ? QStringLiteral("implementation") : scopeIn;
    QString line;
    const QString toml = catalogPath(root);
    if (useCatalog && !toml.isEmpty()) {
        QString cat = readText(toml);
        QString alias = aliasFor(artifact);
        // The catalog may hold the library already (under any alias).
        static const QRegularExpression entry(QStringLiteral(R"re(^\s*([\w.\-]+)\s*=\s*(.*)$)re"), QRegularExpression::MultilineOption);
        QString existing;
        QSet<QString> taken;
        QRegularExpressionMatchIterator it = entry.globalMatch(cat);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            taken.insert(m.captured(1));
            if (m.captured(2).contains(QLatin1Char('"') + coordinate + QLatin1Char('"')) || m.captured(2).contains(QLatin1Char('"') + coordinate + QLatin1Char(':'))
                || (m.captured(2).contains(QStringLiteral("\"%1\"").arg(group)) && m.captured(2).contains(QStringLiteral("name = \"%1\"").arg(artifact))))
                existing = m.captured(1);
        }
        if (existing.isEmpty()) {
            if (taken.contains(alias))
                alias = aliasFor(group.section(QLatin1Char('.'), -1)) + QLatin1Char('-') + alias;
            const QString libLine = QStringLiteral("%1 = { module = \"%2\", version = \"%3\" }").arg(alias, coordinate, version);
            static const QRegularExpression libs(QStringLiteral(R"(^\[libraries\]\s*$)"), QRegularExpression::MultilineOption);
            const QRegularExpressionMatch lm = libs.match(cat);
            if (!lm.hasMatch()) {
                if (!cat.endsWith(QLatin1Char('\n')))
                    cat += QLatin1Char('\n');
                cat += QStringLiteral("\n[libraries]\n") + libLine + QLatin1Char('\n');
            } else {
                static const QRegularExpression nextSection(QStringLiteral(R"(^\[)"), QRegularExpression::MultilineOption);
                const QRegularExpressionMatch nm = nextSection.match(cat, lm.capturedEnd());
                int at = nm.hasMatch() ? int(nm.capturedStart()) : int(cat.size());
                while (at > lm.capturedEnd() && cat.at(at - 1).isSpace())
                    --at; // before the blank lines that end the section
                cat.insert(at, QLatin1Char('\n') + libLine);
            }
            c.extra << qMakePair(toml, cat);
        } else {
            alias = existing;
        }
        line = kts ? QStringLiteral("%1(libs.%2)").arg(scope, accessorFor(alias)) : QStringLiteral("%1 libs.%2").arg(scope, accessorFor(alias));
    } else {
        if (c.text.contains(QLatin1Char('"') + coordinate + QLatin1Char(':')) || c.text.contains(QLatin1Char('\'') + coordinate + QLatin1Char(':'))) {
            c.error = QObject::tr("%1 is already declared in %2.").arg(coordinate, QFileInfo(buildFile).fileName());
            return c;
        }
        const QString full = coordinate + QLatin1Char(':') + version;
        line = kts ? QStringLiteral("%1(\"%2\")").arg(scope, full) : QStringLiteral("%1 '%2'").arg(scope, full);
    }
    if (c.text.contains(line)) {
        c.error = QObject::tr("%1 already has %2.").arg(QFileInfo(buildFile).fileName(), line);
        return c;
    }
    if (!BuildFileEdit::insertGradleLines(c.text, {line})) {
        c.error = QObject::tr("The dependencies block of %1 is not closed.").arg(QFileInfo(buildFile).fileName());
        return c;
    }
    c.summary = line;
    // A build that declares no repository anywhere cannot download anything: give it Maven Central.
    bool hasRepositories = c.text.contains(QLatin1String("repositories"));
    for (const char *f : {"settings.gradle.kts", "settings.gradle", "build.gradle.kts", "build.gradle"})
        hasRepositories = hasRepositories || readText(root + QLatin1Char('/') + QLatin1String(f)).contains(QLatin1String("repositories"));
    if (!hasRepositories) {
        static const QRegularExpression deps(QStringLiteral(R"(^dependencies\s*\{)"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = deps.match(c.text);
        const int at = m.hasMatch() ? int(m.capturedStart()) : int(c.text.size());
        c.text.insert(at, QStringLiteral("repositories {\n    mavenCentral()\n}\n\n"));
        c.summary += QObject::tr(" and repositories { mavenCentral() }");
    }
    c.ok = true;
    return c;
}

QString addDependsOn(const QString &text, const QString &coordinate)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    int at = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines.at(i).trimmed();
        if (t.startsWith(QLatin1String("#!")) || t.startsWith(QLatin1String("@file:")))
            at = i + 1;
        else if (!t.isEmpty() && !t.startsWith(QLatin1String("//")))
            break;
    }
    lines.insert(at, QStringLiteral("@file:DependsOn(\"%1\")").arg(coordinate));
    return lines.join(QLatin1Char('\n'));
}

QList<QPair<QString, QString>> applyUpdates(const QList<QPair<Declared, QString>> &updates)
{
    QHash<QString, QList<QPair<Declared, QString>>> byFile;
    QStringList order;
    for (const auto &u : updates) {
        if (!byFile.contains(u.first.file))
            order << u.first.file;
        byFile[u.first.file] << u;
    }
    QList<QPair<QString, QString>> out;
    for (const QString &file : std::as_const(order)) {
        QString text = readText(file);
        QList<QPair<Declared, QString>> list = byFile.value(file);
        std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first.start > b.first.start; });
        for (const auto &u : std::as_const(list))
            if (QStringView(text).mid(u.first.start, u.first.length) == u.first.version)
                text.replace(u.first.start, u.first.length, u.second);
        out << qMakePair(file, text);
    }
    return out;
}

} // namespace DependencyEdit
