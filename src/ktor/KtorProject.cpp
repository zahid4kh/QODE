#include "ktor/KtorProject.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

QString readText(const QString &path, qint64 max = 256 * 1024)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(max));
}

QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString gradleFile(const QString &dir)
{
    for (const char *name : {"build.gradle.kts", "build.gradle"}) {
        const QString p = dir + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}

// gradle/libs.versions.toml next to the build or in a parent folder (multi-module builds), or "".
QString catalogFor(const QString &dir)
{
    QDir d(dir);
    for (int i = 0; i < 3; ++i) {
        const QString toml = d.filePath(QStringLiteral("gradle/libs.versions.toml"));
        if (QFileInfo::exists(toml))
            return toml;
        if (!d.cdUp())
            break;
    }
    return {};
}

// The server side of Ktor, in any of the ways a build can name it. A client-only build (ktor-client-*) does not match.
bool isKtorModule(const QString &dir, bool *maven)
{
    const QString g = gradleFile(dir);
    if (!g.isEmpty()) {
        QString t = readText(g);
        // A root build that only pins plugin versions for its modules (`alias(…) apply false`) is not the server.
        t.remove(QRegularExpression(QStringLiteral(R"(^.*apply\s+false.*$)"), QRegularExpression::MultilineOption));
        static const QRegularExpression server(QStringLiteral(
            R"(io\.ktor\.plugin|ktor-server|ktorLibs\.server\.|ktorLibs\.plugins\.ktor|libs\.ktor\.server|libs\.plugins\.ktor\b|ktor\("server)"));
        if (server.match(t).hasMatch()) {
            *maven = false;
            return true;
        }
    }
    const QString pom = dir + QStringLiteral("/pom.xml");
    if (QFileInfo::exists(pom) && readText(pom).contains(QStringLiteral("ktor-server"))) {
        *maven = true;
        return true;
    }
    return false;
}

QString unquote(QString v)
{
    v = v.trimmed();
    if (v.size() >= 2 && ((v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))) || (v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\'')))))
        v = v.mid(1, v.size() - 2);
    return v;
}

// A comment that starts outside quotes ("#" in YAML; "#" or "//" in HOCON) is cut off.
QString stripComment(const QString &line, bool hocon)
{
    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\''))
            quote = c;
        else if (c == QLatin1Char('#') && (i == 0 || line.at(i - 1).isSpace()))
            return line.left(i);
        else if (hocon && c == QLatin1Char('/') && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('/'))
            return line.left(i);
    }
    return line;
}

QVector<KtorProject::ConfigEntry> readYaml(const QString &text)
{
    QVector<KtorProject::ConfigEntry> out;
    static const QRegularExpression keyLine(QStringLiteral(R"(^(\s*)([^\s:#][^:]*?)\s*:(?:\s+(.*))?$)"));
    static const QRegularExpression itemLine(QStringLiteral(R"(^(\s*)-\s+(.*)$)"));
    QList<QPair<int, QString>> stack; // indent, key
    int listEntry = -1, listIndent = -1;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = stripComment(lines.at(i), false);
        if (line.trimmed().isEmpty() || line.trimmed() == QLatin1String("---"))
            continue;
        QRegularExpressionMatch m = itemLine.match(line);
        if (m.hasMatch() && listEntry >= 0 && m.captured(1).size() >= listIndent) {
            KtorProject::ConfigEntry &e = out[listEntry];
            e.value += (e.value.isEmpty() ? QString() : QStringLiteral(", ")) + unquote(m.captured(2));
            continue;
        }
        m = keyLine.match(line);
        if (!m.hasMatch())
            continue;
        listEntry = -1;
        const int indent = m.captured(1).size();
        while (!stack.isEmpty() && stack.last().first >= indent)
            stack.removeLast();
        QStringList path;
        for (const auto &p : std::as_const(stack))
            path << p.second;
        const QString key = unquote(m.captured(2));
        path << key;
        const QString value = m.captured(3).trimmed();
        if (value.isEmpty()) {
            stack.append({indent, key});
            // Possibly a list ("modules:" followed by "- a.b.c" lines): the entry collects the items.
            out.append({path.join(QLatin1Char('.')), QString(), i + 1});
            listEntry = int(out.size()) - 1;
            listIndent = indent;
        } else {
            out.append({path.join(QLatin1Char('.')), unquote(value), i + 1});
        }
    }
    // Mappings that turned out to be parents, not empty lists, are not values.
    QVector<KtorProject::ConfigEntry> kept;
    for (int i = 0; i < out.size(); ++i) {
        const bool parent = out[i].value.isEmpty() && i + 1 < out.size() && out[i + 1].key.startsWith(out[i].key + QLatin1Char('.'));
        if (!parent)
            kept << out[i];
    }
    return kept;
}

// HOCON as Ktor uses it: `a { b = 1 }` blocks, dotted keys, `=` or `:`, arrays (also over several lines), comments.
QVector<KtorProject::ConfigEntry> readHocon(const QString &text)
{
    QVector<KtorProject::ConfigEntry> out;
    static const QRegularExpression open(QStringLiteral(R"(^\s*("[^"]*"|[\w.\-]+)\s*[:=]?\s*\{\s*$)"));
    static const QRegularExpression assign(QStringLiteral(R"(^\s*("[^"]*"|[\w.\-]+)\s*[:=]\s*(.*?)\s*,?\s*$)"));
    QStringList stack;
    QString arrayKey, arrayValue;
    int arrayLine = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = stripComment(lines.at(i), true).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("include ")))
            continue;
        if (!arrayKey.isEmpty()) {
            const int end = line.indexOf(QLatin1Char(']'));
            arrayValue += QLatin1Char(' ') + (end < 0 ? line : line.left(end));
            if (end >= 0) {
                QStringList items;
                for (const QString &v : arrayValue.split(QLatin1Char(','), Qt::SkipEmptyParts))
                    if (!unquote(v).isEmpty())
                        items << unquote(v);
                out.append({arrayKey, items.join(QStringLiteral(", ")), arrayLine});
                arrayKey.clear();
            }
            continue;
        }
        while (line.startsWith(QLatin1Char('}'))) {
            if (!stack.isEmpty())
                stack.removeLast();
            line = line.mid(1).trimmed();
        }
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m = open.match(line);
        if (m.hasMatch()) {
            stack << unquote(m.captured(1));
            continue;
        }
        m = assign.match(line);
        if (!m.hasMatch())
            continue;
        const QString key = (stack + QStringList{unquote(m.captured(1))}).join(QLatin1Char('.'));
        QString value = m.captured(2);
        if (value.startsWith(QLatin1Char('['))) {
            const int end = value.indexOf(QLatin1Char(']'));
            if (end < 0) {
                arrayKey = key;
                arrayValue = value.mid(1);
                arrayLine = i + 1;
                continue;
            }
            QStringList items;
            for (const QString &v : value.mid(1, end - 1).split(QLatin1Char(','), Qt::SkipEmptyParts))
                if (!unquote(v).isEmpty())
                    items << unquote(v);
            value = items.join(QStringLiteral(", "));
        } else {
            value = unquote(value);
        }
        out.append({key, value, i + 1});
    }
    return out;
}

} // namespace

KtorProject KtorProject::detect(const QString &root)
{
    KtorProject p;
    if (root.isEmpty())
        return p;
    const QString top = QDir(root).absolutePath();
    QStringList candidates{top};
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (d.startsWith(QLatin1Char('.')) || d == QLatin1String("build") || d == QLatin1String("buildSrc") || d == QLatin1String("gradle")
            || d == QLatin1String("node_modules") || d == QLatin1String("target") || d == QLatin1String("src") || d == QLatin1String("out"))
            continue;
        candidates << QDir(root).filePath(d);
    }
    for (const QString &dir : std::as_const(candidates)) {
        bool maven = false;
        if (!isKtorModule(dir, &maven))
            continue;
        p.valid = true;
        p.maven = maven;
        p.moduleDir = dir;
        const bool rootBuilds = maven ? QFileInfo::exists(top + QStringLiteral("/pom.xml"))
                                      : (QFileInfo::exists(top + QStringLiteral("/gradlew")) || QFileInfo::exists(top + QStringLiteral("/settings.gradle.kts"))
                                         || QFileInfo::exists(top + QStringLiteral("/settings.gradle")));
        p.workDir = rootBuilds ? top : dir;
        if (maven) {
            const QString w = p.workDir + QStringLiteral("/mvnw");
            p.tool = QFileInfo::exists(w) ? (QFileInfo(w).isExecutable() ? QStringLiteral("./mvnw") : QStringLiteral("sh mvnw")) : QStringLiteral("mvn");
        } else {
            const QString w = p.workDir + QStringLiteral("/gradlew");
            p.tool = QFileInfo::exists(w) ? (QFileInfo(w).isExecutable() ? QStringLiteral("./gradlew") : QStringLiteral("sh gradlew")) : QStringLiteral("gradle");
        }
        p.appName = QFileInfo(dir).fileName();

        const QString build = readText(p.buildFile());
        const QString all = p.buildText().toLower();
        static const QRegularExpression engine(QStringLiteral(R"(server[-.](netty|cio|jetty|tomcat)\b)"));
        const QString e = engine.match(all).captured(1);
        p.engine = e == QLatin1String("cio") ? QStringLiteral("CIO") : e.isEmpty() ? QString() : e.left(1).toUpper() + e.mid(1);
        static const QRegularExpression gradleMain(QStringLiteral(R"re(mainClass(?:Name)?\s*(?:\.set\s*\(|=)\s*["']([\w.$]+)["'])re"));
        static const QRegularExpression mavenMain(QStringLiteral(R"(<(?:mainClass|main\.class|exec\.mainClass)>\s*([\w.$]+)\s*<)"));
        p.mainClass = (maven ? mavenMain : gradleMain).match(build).captured(1);
        if (p.engine.isEmpty() && p.mainClass.startsWith(QLatin1String("io.ktor.server.")))
            p.engine = p.mainClass.section(QLatin1Char('.'), 3, 3) == QLatin1String("cio") ? QStringLiteral("CIO") : p.mainClass.section(QLatin1Char('.'), 3, 3);

        // Ktor's version, from wherever this build keeps it.
        static const QList<QRegularExpression> versionRes = {
            QRegularExpression(QStringLiteral(R"(ktor-version-catalog:([\w.\-]+))")),
            QRegularExpression(QStringLiteral(R"re(^\s*ktor(?:[-_]?version)?\s*=\s*"([\w.\-]+)")re"), QRegularExpression::MultilineOption),
            QRegularExpression(QStringLiteral(R"(^\s*ktor[_.]?[vV]ersion\s*=\s*([\w.\-]+))"), QRegularExpression::MultilineOption),
            QRegularExpression(QStringLiteral(R"re(io\.ktor\.plugin"\)?\s*version\s*"([\w.\-]+)")re")),
            QRegularExpression(QStringLiteral(R"(<ktor[._]version>\s*([\w.\-]+)\s*<)")),
        };
        const QString versionText = p.buildText() + QLatin1Char('\n') + readText(p.workDir + QStringLiteral("/settings.gradle.kts"))
                                    + readText(p.workDir + QStringLiteral("/settings.gradle")) + QLatin1Char('\n')
                                    + readText(p.workDir + QStringLiteral("/gradle.properties"));
        for (const QRegularExpression &re : versionRes) {
            p.version = re.match(versionText).captured(1);
            if (!p.version.isEmpty() && p.version.at(0).isDigit())
                break;
            p.version.clear();
        }
        return p;
    }
    return p;
}

QString KtorProject::gradlePath() const
{
    if (moduleDir == workDir)
        return QStringLiteral(":");
    return QLatin1Char(':') + QDir(workDir).relativeFilePath(moduleDir).replace(QLatin1Char('/'), QLatin1Char(':'));
}

QStringList KtorProject::outputDirs() const
{
    if (maven)
        return {moduleDir + QStringLiteral("/target/classes/")};
    return {moduleDir + QStringLiteral("/build/classes/"), moduleDir + QStringLiteral("/build/resources/")};
}

QStringList KtorProject::jvmOptions(const Options &o, int debugPort) const
{
    QStringList out = QProcess::splitCommand(o.jvmArgs);
    if (o.development || o.autoReload) {
        // io.ktor.development is what embeddedServer reads; ktor.development (a ktor.* system property is configuration
        // to EngineMain) overrides a development: false in application.yaml / .conf.
        if (!o.jvmArgs.contains(QStringLiteral("io.ktor.development")))
            out << QStringLiteral("-Dio.ktor.development=true");
        if (!o.jvmArgs.contains(QStringLiteral("-Dktor.development")))
            out << QStringLiteral("-Dktor.development=true");
    }
    // Lets Ktor read the exact class path (else it guesses the folders from the loaded packages, and a package created
    // later is not watched).
    if (o.autoReload)
        out << QStringLiteral("--add-opens=java.base/jdk.internal.loader=ALL-UNNAMED");
    if (debugPort > 0)
        out << QStringLiteral("-agentlib:jdwp=transport=dt_socket,server=y,suspend=n,address=127.0.0.1:%1").arg(debugPort);
    return out;
}

QString KtorProject::runScriptPath() const
{
    const QByteArray key = QCryptographicHash::hash((workDir + QLatin1Char('|') + moduleDir).toUtf8(), QCryptographicHash::Sha1).toHex().left(12);
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE-AGENTIC/ktor/run-")
           + QString::fromLatin1(key) + QStringLiteral(".gradle");
}

QString KtorProject::runScript(const Options &o, int debugPort) const
{
    auto groovy = [](QString v) {
        v.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        v.replace(QLatin1Char('\''), QStringLiteral("\\'"));
        return QLatin1Char('\'') + v + QLatin1Char('\'');
    };
    QStringList options;
    for (const QString &opt : jvmOptions(o, debugPort))
        options << groovy(opt);
    return QStringLiteral(
               "// Written by QODE before each start of the Ktor server in %1.\n"
               "// JVM options for the application that `run` starts: only that JVM gets them, after the build's own\n"
               "// applicationDefaultJvmArgs (so they win).\n"
               "class QodeKtorJvmOptions implements org.gradle.process.CommandLineArgumentProvider {\n"
               "    private final List<String> options\n"
               "    QodeKtorJvmOptions(List<String> options) { this.options = options }\n"
               "    @org.gradle.api.tasks.Input\n"
               "    List<String> getOptions() { options }\n"
               "    @Override\n"
               "    Iterable<String> asArguments() { options }\n"
               "}\n"
               "\n"
               "gradle.allprojects { project ->\n"
               "    if (project.path == %2) {\n"
               "        project.tasks.withType(JavaExec).configureEach { task ->\n"
               "            if (task.name == 'run')\n"
               "                task.jvmArgumentProviders.add(new QodeKtorJvmOptions([%3]))\n"
               "        }\n"
               "    }\n"
               "}\n")
        .arg(moduleDir, groovy(gradlePath()), options.join(QStringLiteral(", ")));
}

QString KtorProject::command(const Options &o, int debugPort) const
{
    if (!o.command.trimmed().isEmpty())
        return o.command.trimmed();
    if (!valid)
        return {};
    QStringList args;
    if (o.port > 0)
        args << QStringLiteral("-port=%1").arg(o.port);
    if (o.autoReload) {
        // EngineMain's -watch: the class path entries Ktor reloads from when they change (matched as substrings).
        const QString watch = QStringLiteral("-watch=") + outputDirs().join(QLatin1Char(','));
        args << (watch.contains(QLatin1Char(' ')) ? QLatin1Char('"') + watch + QLatin1Char('"') : watch);
    }
    if (!o.arguments.trimmed().isEmpty())
        args << o.arguments.trimmed();
    const QString joined = args.join(QLatin1Char(' '));
    QString cmd;
    if (maven) {
        const QStringList jvm = jvmOptions(o, debugPort);
        if (!jvm.isEmpty())
            cmd = QStringLiteral("JAVA_TOOL_OPTIONS=") + shellQuote(jvm.join(QLatin1Char(' '))) + QLatin1Char(' ');
        cmd += tool;
        if (moduleDir != workDir)
            cmd += QStringLiteral(" -f ") + shellQuote(QDir(workDir).relativeFilePath(moduleDir) + QStringLiteral("/pom.xml"));
        cmd += QStringLiteral(" compile exec:java");
        if (!joined.isEmpty())
            cmd += QStringLiteral(" -Dexec.args=") + shellQuote(joined);
        return cmd;
    }
    cmd = tool + QStringLiteral(" --console=plain");
    if (o.liveRoutes || !jvmOptions(o, 0).isEmpty())
        cmd += QStringLiteral(" --init-script ") + shellQuote(runScriptPath());
    cmd += QLatin1Char(' ') + (moduleDir != workDir ? gradlePath() + QLatin1Char(':') : QString()) + QStringLiteral("run");
    if (!joined.isEmpty())
        cmd += QStringLiteral(" --args=") + shellQuote(joined);
    return cmd;
}

QStringList KtorProject::sourceRoots() const
{
    QStringList out;
    if (!valid)
        return out;
    for (const char *sub : {"src/main/kotlin", "src/main/java", "src/jvmMain/kotlin", "src/test/kotlin", "src/test/java"}) {
        const QString d = moduleDir + QLatin1Char('/') + QLatin1String(sub);
        if (QFileInfo(d).isDir())
            out << d;
    }
    return out;
}

QString KtorProject::resourcesDir() const
{
    if (!valid)
        return {};
    const QString jvm = moduleDir + QStringLiteral("/src/jvmMain/resources");
    return QFileInfo(jvm).isDir() && !QFileInfo(moduleDir + QStringLiteral("/src/main/resources")).isDir() ? jvm : moduleDir + QStringLiteral("/src/main/resources");
}

QString KtorProject::buildFile() const
{
    if (!valid)
        return {};
    return maven ? moduleDir + QStringLiteral("/pom.xml") : gradleFile(moduleDir);
}

QString KtorProject::configFile() const
{
    for (const char *name : {"application.yaml", "application.yml", "application.conf"}) {
        const QString p = resourcesDir() + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}

QString KtorProject::buildText() const
{
    QString t = readText(buildFile());
    if (!maven) {
        const QString toml = catalogFor(moduleDir);
        if (!toml.isEmpty())
            t += QLatin1Char('\n') + readText(toml);
    }
    return t;
}

bool KtorProject::hasArtifact(const QString &artifact, const QString &accessor) const
{
    const QString t = buildText();
    // "ktor-server-cors" must not match "ktor-server-cors-something": the next character ends the name.
    const QRegularExpression name(QRegularExpression::escape(artifact) + QStringLiteral(R"((?:-jvm)?(?![\w-]))"));
    if (name.match(t).hasMatch())
        return true;
    if (accessor.isEmpty())
        return false;
    const QRegularExpression acc(QStringLiteral(R"(ktorLibs\.)") + QRegularExpression::escape(accessor) + QStringLiteral(R"((?![\w.]))"));
    return acc.match(t).hasMatch();
}

bool KtorProject::usesKtorCatalog() const
{
    if (!valid || maven)
        return false;
    return (readText(workDir + QStringLiteral("/settings.gradle.kts")) + readText(workDir + QStringLiteral("/settings.gradle")))
        .contains(QStringLiteral("ktor-version-catalog"));
}

QVector<KtorProject::ConfigEntry> KtorProject::readConfig(const QString &path)
{
    if (path.isEmpty())
        return {};
    const QString text = readText(path);
    return path.endsWith(QLatin1String(".conf")) ? readHocon(text) : readYaml(text);
}

int KtorProject::portOf(const QString &value)
{
    bool ok = false;
    const int n = value.trimmed().toInt(&ok);
    if (ok)
        return n > 0 && n <= 65535 ? n : 0;
    // "$PORT:8080" / "${PORT:8080}": the default after the colon.
    static const QRegularExpression withDefault(QStringLiteral(R"(^\$\{?\w+:(\d{1,5})\}?$)"));
    const int d = withDefault.match(value.trimmed()).captured(1).toInt();
    return d > 0 && d <= 65535 ? d : 0;
}
