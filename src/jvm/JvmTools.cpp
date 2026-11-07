#include "jvm/JvmTools.h"
#include "platform/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace JvmTools {

namespace {

QString readText(const QString &path, qint64 max = 512 * 1024)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(max));
}

int lineAt(const QString &text, int offset)
{
    return int(QStringView(text).left(qMax(0, offset)).count(QLatin1Char('\n')));
}

// Comments are blanked (offsets kept) so commented-out declarations do not count.
QString withoutComments(const QString &text)
{
    QString out = text;
    const int n = out.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = out.at(i);
        if (c == QLatin1Char('"')) { // skip strings (a "//" inside a URL is not a comment)
            for (++i; i < n && out.at(i) != QLatin1Char('"') && out.at(i) != QLatin1Char('\n'); ++i)
                if (out.at(i) == QLatin1Char('\\'))
                    ++i;
            continue;
        }
        if (c != QLatin1Char('/') || i + 1 >= n)
            continue;
        if (out.at(i + 1) == QLatin1Char('/')) {
            while (i < n && out.at(i) != QLatin1Char('\n'))
                out[i++] = QLatin1Char(' ');
        } else if (out.at(i + 1) == QLatin1Char('*')) {
            const int end = out.indexOf(QStringLiteral("*/"), i + 2);
            const int stop = end < 0 ? n : end + 2;
            for (; i < stop; ++i)
                if (out.at(i) != QLatin1Char('\n'))
                    out[i] = QLatin1Char(' ');
            --i;
        }
    }
    return out;
}

QStringList moduleDirs(const QString &root)
{
    QStringList dirs{root};
    for (const QString &d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (d.startsWith(QLatin1Char('.')) || d == QLatin1String("build") || d == QLatin1String("gradle") || d == QLatin1String("node_modules")
            || d == QLatin1String("target") || d == QLatin1String("src"))
            continue;
        dirs << root + QLatin1Char('/') + d;
    }
    return dirs;
}

bool firstMatch(const QString &file, const QList<QRegularExpression> &patterns, Found *out)
{
    const QString raw = readText(file);
    if (raw.isEmpty())
        return false;
    const bool props = file.endsWith(QLatin1String(".properties")) || file.endsWith(QLatin1String(".toml")) || file.endsWith(QLatin1String(".xml"));
    const QString text = props ? raw : withoutComments(raw);
    for (const QRegularExpression &re : patterns) {
        const QRegularExpressionMatch m = re.match(text);
        if (m.hasMatch()) {
            out->value = m.captured(1);
            out->file = file;
            out->line = lineAt(text, int(m.capturedStart(1))) + 1;
            return true;
        }
    }
    return false;
}

QString normalizeJava(QString v)
{
    v.replace(QLatin1Char('_'), QLatin1Char('.'));
    if (v.startsWith(QLatin1String("VERSION.")))
        v = v.mid(8);
    return v;
}

} // namespace

QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

// --- Kotlin scripts -------------------------------------------------------------------------------------------------

bool isKotlinScript(const QString &path)
{
    const QString lower = path.toLower();
    return lower.endsWith(QLatin1String(".kts")) && !lower.endsWith(QLatin1String(".gradle.kts"));
}

bool isMainKts(const QString &path)
{
    return path.toLower().endsWith(QLatin1String(".main.kts"));
}

QString shebang(const QString &text)
{
    if (!text.startsWith(QLatin1String("#!")))
        return {};
    const int nl = text.indexOf(QLatin1Char('\n'));
    return (nl < 0 ? text : text.left(nl)).trimmed();
}

QString kotlinHome()
{
    auto valid = [](const QString &dir) { return !dir.isEmpty() && QFileInfo(dir + QStringLiteral("/lib")).isDir(); };
    const QString env = qEnvironmentVariable("KOTLIN_HOME");
    if (valid(env))
        return QDir::cleanPath(env);
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("kotlinc"));
    if (!exe.isEmpty()) {
        if (exe.startsWith(QLatin1String("/snap/bin/")) && valid(QStringLiteral("/snap/kotlin/current")))
            return QStringLiteral("/snap/kotlin/current");
        const QString home = QFileInfo(QFileInfo(exe).canonicalFilePath()).absolutePath() + QStringLiteral("/..");
        if (valid(home))
            return QDir::cleanPath(home);
    }
    const QString sdk = QDir::homePath() + QStringLiteral("/.sdkman/candidates/kotlin/current");
    return valid(sdk) ? sdk : QString();
}

QString mainKtsJar()
{
    const QString home = kotlinHome();
    const QString jar = home + QStringLiteral("/lib/kotlin-main-kts.jar");
    return !home.isEmpty() && QFileInfo::exists(jar) ? jar : QString();
}

QString scriptRunCommand(const QString &path, const QString &text, bool *chmodOffer)
{
    if (chmodOffer)
        *chmodOffer = false;
    const QString bang = shebang(text);
    if (bang.contains(QLatin1String("kotlin")) && Platform::hasExecBit()) { // Windows never runs a file by its #! line
        if (QFileInfo(path).isExecutable())
            return QStringLiteral("{file}");
        if (chmodOffer)
            *chmodOffer = true;
    }
    return QStringLiteral("kotlinc -script {file}");
}

// --- main functions -------------------------------------------------------------------------------------------------

QString fileFacade(const QString &path, const QString &text)
{
    static const QRegularExpression jvmName(QStringLiteral(R"re(@file:JvmName\(\s*"(\w+)"\s*\))re"));
    if (const QString jn = jvmName.match(text).captured(1); !jn.isEmpty())
        return jn;
    QString stem = QFileInfo(path).fileName();
    stem = stem.left(stem.indexOf(QLatin1Char('.')));
    return stem.left(1).toUpper() + stem.mid(1) + QStringLiteral("Kt");
}

QList<MainEntry> findMains(const QString &path, const QString &raw)
{
    QList<MainEntry> out;
    const bool kt = path.endsWith(QLatin1String(".kt"));
    const bool java = path.endsWith(QLatin1String(".java"));
    if (!kt && !java)
        return out;
    const QString text = withoutComments(raw);
    const QString pkg = declaredPackage(text);
    auto qualify = [&pkg](const QString &cls) { return pkg.isEmpty() ? cls : pkg + QLatin1Char('.') + cls; };
    if (java) {
        static const QRegularExpression javaMain(QStringLiteral(R"(\bstatic\s+(?:final\s+)?(?:public\s+)?void\s+main\s*\()"));
        static const QRegularExpression javaMain2(QStringLiteral(R"(\bpublic\s+static\s+void\s+main\s*\()"));
        static const QRegularExpression typeRe(QStringLiteral(R"(\b(?:class|interface|enum|record)\s+(\w+))"));
        QRegularExpressionMatchIterator it = javaMain.globalMatch(text);
        if (!it.hasNext())
            it = javaMain2.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            // The types whose body holds the main, outermost first (a record or class declared above it, but closed before
            // it, is not one of them); nested ones give the binary name Outer$Inner.
            QStringList enclosing;
            QRegularExpressionMatchIterator ti = typeRe.globalMatch(text.left(m.capturedStart()));
            while (ti.hasNext()) {
                const QRegularExpressionMatch t = ti.next();
                const int open = int(text.indexOf(QLatin1Char('{'), t.capturedEnd()));
                if (open < 0 || open > m.capturedStart())
                    continue;
                int depth = 0, close = -1;
                for (int i = open; i < text.size(); ++i) {
                    if (text.at(i) == QLatin1Char('{'))
                        ++depth;
                    else if (text.at(i) == QLatin1Char('}') && --depth == 0) {
                        close = i;
                        break;
                    }
                }
                if (close < 0 || close > m.capturedStart())
                    enclosing << t.captured(1);
            }
            QString cls = enclosing.join(QLatin1Char('$'));
            if (cls.isEmpty())
                cls = QFileInfo(path).completeBaseName();
            out.append({lineAt(text, int(m.capturedStart())), qualify(cls)});
        }
        return out;
    }
    static const QRegularExpression ktMain(QStringLiteral(R"(^([ \t]*)((?:@JvmStatic\s+)?)(?:(?:public|internal|suspend)\s+)*fun\s+main\s*\()"),
                                           QRegularExpression::MultilineOption);
    static const QRegularExpression objRe(QStringLiteral(R"(\b(companion\s+)?object(?:\s+(\w+))?|\bclass\s+(\w+))"));
    QRegularExpressionMatchIterator it = ktMain.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int line = lineAt(text, int(m.capturedStart() + m.captured(0).lastIndexOf(QLatin1String("fun")))); // the `fun`, not an annotation above
        if (m.captured(1).isEmpty()) {
            out.append({line, qualify(fileFacade(path, raw))});
            continue;
        }
        // Indented: only a @JvmStatic main of an object / companion object starts a program.
        // (the annotation may stand on the line above)
        const int start = int(m.capturedStart());
        const int prevStart = start >= 2 ? text.lastIndexOf(QLatin1Char('\n'), start - 2) + 1 : 0;
        if (m.captured(2).isEmpty() && !text.mid(prevStart, start - prevStart).contains(QLatin1String("@JvmStatic")))
            continue;
        QString cls, lastClass;
        QRegularExpressionMatchIterator oi = objRe.globalMatch(text.left(m.capturedStart()));
        while (oi.hasNext()) {
            const QRegularExpressionMatch o = oi.next();
            if (!o.captured(3).isEmpty()) {
                lastClass = o.captured(3);
                cls = lastClass;
            } else if (!o.captured(1).isEmpty()) {
                cls = lastClass; // a companion's static main lands in the enclosing class
            } else if (!o.captured(2).isEmpty()) {
                cls = o.captured(2);
            }
        }
        if (!cls.isEmpty())
            out.append({line, qualify(cls)});
    }
    return out;
}

// --- tests ------------------------------------------------------------------------------------------------------------

namespace {
// The text with comments and the insides of string / char literals blanked (same length, newlines kept), so braces
// in a JSON string of a test do not count.
QString codeOnly(const QString &raw)
{
    QString out = withoutComments(raw);
    const int n = out.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = out.at(i);
        if (c != QLatin1Char('"') && c != QLatin1Char('\''))
            continue;
        const bool raw3 = c == QLatin1Char('"') && out.mid(i, 3) == QLatin1String("\"\"\"");
        if (raw3) {
            const int end = out.indexOf(QLatin1String("\"\"\""), i + 3);
            const int stop = end < 0 ? n : end;
            for (int j = i + 3; j < stop; ++j)
                if (out.at(j) != QLatin1Char('\n'))
                    out[j] = QLatin1Char(' ');
            i = end < 0 ? n : end + 2;
            continue;
        }
        int j = i + 1;
        for (; j < n && out.at(j) != c && out.at(j) != QLatin1Char('\n'); ++j) {
            if (out.at(j) == QLatin1Char('\\') && j + 1 < n)
                out[j++] = QLatin1Char(' ');
            out[j] = QLatin1Char(' ');
        }
        i = j;
    }
    return out;
}

// Offset of the '}' closing the '{' at `open`, or the end of the text.
int closingBrace(const QString &text, int open)
{
    int depth = 0;
    for (int i = open; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('{'))
            ++depth;
        else if (text.at(i) == QLatin1Char('}') && --depth == 0)
            return i;
    }
    return int(text.size());
}
} // namespace

QList<TestEntry> findTests(const QString &path, const QString &raw)
{
    QList<TestEntry> out;
    const bool kt = path.endsWith(QLatin1String(".kt"));
    if ((!kt && !path.endsWith(QLatin1String(".java"))) || !raw.contains(QLatin1String("Test")))
        return out;
    const QString text = codeOnly(raw);
    const QString pkg = declaredPackage(raw);

    // Classes and their bodies.
    struct Cls {
        QString name;
        int line = 0, open = 0, close = 0;
        bool hasTests = false;
    };
    QVector<Cls> classes;
    static const QRegularExpression classRe(QStringLiteral(R"(\bclass\s+(\w+))"));
    static const QRegularExpression declRe(QStringLiteral(R"(\b(?:class|fun|object|interface)\b|;)"));
    for (QRegularExpressionMatchIterator it = classRe.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart() >= 2 && text.mid(m.capturedStart() - 2, 2) == QLatin1String("::"))
            continue; // Foo::class
        const int open = text.indexOf(QLatin1Char('{'), int(m.capturedEnd()));
        if (open < 0)
            continue;
        // A class without a body: the next brace belongs to another declaration.
        if (declRe.match(text.mid(m.capturedEnd(), open - m.capturedEnd())).hasMatch())
            continue;
        classes.append({m.captured(1), lineAt(text, int(m.capturedStart())), open, closingBrace(text, open)});
    }

    static const QRegularExpression annRe(QStringLiteral(R"(@(?:[\w.]+\.)?(?:Test|ParameterizedTest|RepeatedTest|TestFactory|TestTemplate)\b)"));
    static const QRegularExpression otherAnn(QStringLiteral(R"(@[\w.]+(?:\s*\((?:[^()]|\([^()]*\))*\))?)"));
    static const QRegularExpression ktFun(QStringLiteral(R"((?:(?:public|internal|open|override|suspend|private|protected)\s+)*fun\s+(?:`([^`]+)`|(\w+))\s*\()"));
    static const QRegularExpression javaMethod(QStringLiteral(
        R"((?:(?:public|protected|private|static|final|synchronized)\s+)*(?:<[^>]*>\s*)?[\w.$]+(?:<[^>]*>)?(?:\[\])*\s+(\w+)\s*\()"));
    auto skipSpace = [&text](int pos) {
        while (pos < text.size() && text.at(pos).isSpace())
            ++pos;
        return pos;
    };
    QList<TestEntry> tests;
    for (QRegularExpressionMatchIterator it = annRe.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch a = it.next();
        // The annotation's own arguments and any further annotations come before the function.
        int pos = int(a.capturedStart());
        for (;;) {
            pos = skipSpace(pos);
            const QRegularExpressionMatch other = otherAnn.match(text, pos, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
            if (!other.hasMatch())
                break;
            pos = int(other.capturedEnd());
        }
        const QRegularExpressionMatch d = (kt ? ktFun : javaMethod).match(text, pos, QRegularExpression::NormalMatch,
                                                                          QRegularExpression::AnchorAtOffsetMatchOption);
        if (!d.hasMatch())
            continue;
        const QString method = kt && !d.captured(1).isEmpty() ? d.captured(1) : d.captured(kt ? 2 : 1);
        // The classes around it, outermost first.
        QStringList chain;
        int inner = -1;
        for (int i = 0; i < classes.size(); ++i)
            if (classes.at(i).open < pos && pos < classes.at(i).close) {
                chain.append(classes.at(i).name);
                inner = i;
            }
        if (inner < 0)
            continue;
        classes[inner].hasTests = true;
        const QString cls = chain.join(QLatin1Char('$'));
        tests.append({lineAt(text, int(d.capturedStart())), pkg.isEmpty() ? cls : pkg + QLatin1Char('.') + cls, method});
    }
    for (int i = 0; i < classes.size(); ++i) {
        if (!classes.at(i).hasTests)
            continue;
        QStringList chain;
        for (const Cls &c : std::as_const(classes))
            if (c.open < classes.at(i).open && classes.at(i).open < c.close)
                chain.append(c.name);
        chain.append(classes.at(i).name);
        const QString cls = chain.join(QLatin1Char('$'));
        out.append({classes.at(i).line, pkg.isEmpty() ? cls : pkg + QLatin1Char('.') + cls, QString()});
    }
    out += tests;
    std::stable_sort(out.begin(), out.end(), [](const TestEntry &a, const TestEntry &b) { return a.line < b.line; });
    return out;
}

// --- packages ---------------------------------------------------------------------------------------------------------

QString declaredPackage(const QString &text, int *line)
{
    static const QRegularExpression re(QStringLiteral(R"(^[ \t]*package\s+([\w.`]+)\s*;?)"), QRegularExpression::MultilineOption);
    const QString clean = withoutComments(text);
    const QRegularExpressionMatch m = re.match(clean);
    if (line)
        *line = m.hasMatch() ? lineAt(clean, int(m.capturedStart(1))) : -1;
    QString pkg = m.captured(1);
    pkg.remove(QLatin1Char('`'));
    return pkg;
}

QString folderPackage(const QString &path, QString *sourceRoot)
{
    static const QRegularExpression re(QStringLiteral(R"(^(.*/src/[^/]+/(?:java|kotlin))/(?:(.+)/)?[^/]+$)"));
    const QRegularExpressionMatch m = re.match(QDir::cleanPath(path));
    if (!m.hasMatch())
        return QString();
    if (sourceRoot)
        *sourceRoot = m.captured(1);
    QString dir = m.captured(2);
    dir.replace(QLatin1Char('/'), QLatin1Char('.'));
    return dir.isNull() ? QStringLiteral("") : dir;
}

bool packageMatches(const QString &path, const QString &declared, const QString &folder)
{
    if (folder.isNull() || declared == folder)
        return true;
    if (!path.endsWith(QLatin1String(".kt")))
        return false;
    // Kotlin: the folders may leave out the common root package.
    return folder.isEmpty() ? true : declared.endsWith(QLatin1Char('.') + folder);
}

QString withPackage(const QString &path, const QString &text, const QString &pkg)
{
    const bool java = path.endsWith(QLatin1String(".java"));
    const QString statement = pkg.isEmpty() ? QString() : QStringLiteral("package ") + pkg + (java ? QStringLiteral(";") : QString());
    int line = -1;
    declaredPackage(text, &line);
    QStringList lines = text.split(QLatin1Char('\n'));
    if (line >= 0 && line < lines.size()) {
        if (statement.isEmpty()) {
            lines.removeAt(line);
            if (line < lines.size() && lines.at(line).trimmed().isEmpty())
                lines.removeAt(line);
        } else {
            static const QRegularExpression re(QStringLiteral(R"(package\s+[\w.`]+\s*;?)"));
            lines[line].replace(re, statement);
        }
        return lines.join(QLatin1Char('\n'));
    }
    if (statement.isEmpty())
        return text;
    // After a leading comment block (a license header), else at the top.
    int at = 0;
    bool inBlock = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines.at(i).trimmed();
        if (inBlock) {
            if (t.contains(QLatin1String("*/"))) {
                inBlock = false;
                at = i + 1;
            }
            continue;
        }
        if (t.startsWith(QLatin1String("/*"))) {
            inBlock = !t.contains(QLatin1String("*/"));
            at = i + 1;
        } else if (t.startsWith(QLatin1String("//")) || t.startsWith(QLatin1String("#!")) || t.startsWith(QLatin1String("@file:"))) {
            at = i + 1;
        } else if (!t.isEmpty()) {
            break;
        }
    }
    lines.insert(at, statement);
    if (at + 1 < lines.size() && !lines.at(at + 1).trimmed().isEmpty())
        lines.insert(at + 1, QString());
    return lines.join(QLatin1Char('\n'));
}

// --- versions ---------------------------------------------------------------------------------------------------------

bool isJvmBuild(const QString &root)
{
    if (root.isEmpty())
        return false;
    for (const char *f : {"build.gradle.kts", "build.gradle", "settings.gradle.kts", "settings.gradle", "pom.xml", "gradlew"})
        if (QFileInfo::exists(root + QLatin1Char('/') + QLatin1String(f)))
            return true;
    return false;
}

Versions detectVersions(const QString &root)
{
    Versions v;
    if (!isJvmBuild(root))
        return v;
    v.maven = !QFileInfo::exists(root + QStringLiteral("/gradlew")) && QFileInfo::exists(root + QStringLiteral("/pom.xml"))
              && !QFileInfo::exists(root + QStringLiteral("/build.gradle")) && !QFileInfo::exists(root + QStringLiteral("/build.gradle.kts"));
    auto rx = [](const char *p) { return QRegularExpression(QLatin1String(p), QRegularExpression::MultilineOption); };

    QStringList buildFiles, settingsFiles;
    for (const QString &dir : moduleDirs(root))
        for (const char *f : {"build.gradle.kts", "build.gradle", "pom.xml"})
            if (QFileInfo::exists(dir + QLatin1Char('/') + QLatin1String(f)))
                buildFiles << dir + QLatin1Char('/') + QLatin1String(f);
    for (const char *f : {"settings.gradle.kts", "settings.gradle"})
        if (QFileInfo::exists(root + QLatin1Char('/') + QLatin1String(f)))
            settingsFiles << root + QLatin1Char('/') + QLatin1String(f);
    const QString toml = root + QStringLiteral("/gradle/libs.versions.toml");
    const QString props = root + QStringLiteral("/gradle.properties");

    // Kotlin: the plugin version where it is applied, then the catalog / properties it comes from.
    const QList<QRegularExpression> kotlinBuild = {
        rx(R"re(kotlin\s*\(\s*"[\w.-]+"\s*\)\s*version\s*"([^"]+)")re"),
        rx(R"re(id\s*\(?\s*["']org\.jetbrains\.kotlin\.[\w.-]+["']\s*\)?\s*version\s*\(?\s*["']([^"'$]+)["'])re"),
        rx(R"re(kotlin-gradle-plugin:([\d][\w.-]*))re"),
        rx(R"re(<kotlin\.version>\s*([^<\s]+)\s*<)re"),
    };
    for (const QString &f : settingsFiles + buildFiles)
        if (firstMatch(f, kotlinBuild, &v.kotlin))
            break;
    if (v.kotlin.isEmpty())
        firstMatch(toml, {rx(R"re(^\s*kotlin(?:[-_]?[Vv]ersion)?\s*=\s*"([^"]+)")re")}, &v.kotlin);
    if (v.kotlin.isEmpty())
        firstMatch(props, {rx(R"re(^\s*kotlin(?:_version|Version|\.version)\s*=\s*(\S+))re")}, &v.kotlin);

    // Java: the toolchain, then the target release.
    const QList<QRegularExpression> javaPatterns = {
        rx(R"re(jvmToolchain\s*\(\s*(\d+)\s*\))re"),
        rx(R"re(JavaLanguageVersion\.of\s*\(\s*"?(\d+)"?\s*\))re"),
        rx(R"re(<maven\.compiler\.release>\s*(\d[\d.]*)\s*<)re"),
        rx(R"re(<java\.version>\s*(\d[\d.]*)\s*<)re"),
        rx(R"re(<release>\s*(\d[\d.]*)\s*</release>)re"),
        rx(R"re(<maven\.compiler\.source>\s*(\d[\d.]*)\s*<)re"),
        rx(R"re(sourceCompatibility\s*=\s*(?:JavaVersion\.)?(VERSION_[\d_]+|['"]?[\d.]+))re"),
        rx(R"re(jvmTarget(?:\.set\s*\(|\s*=)\s*(?:JvmTarget\.JVM_)?["']?([\d._]+))re"),
    };
    for (const QString &f : buildFiles)
        if (firstMatch(f, javaPatterns, &v.java))
            break;
    if (!v.java.isEmpty()) {
        v.java.value.remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
        v.java.value = normalizeJava(v.java.value);
    }
    if (v.java.file.endsWith(QLatin1String(".kts")) || v.java.file.endsWith(QLatin1String(".gradle"))) {
        const QString text = readText(v.java.file);
        static const QRegularExpression tc(QStringLiteral(R"re((?:jvmToolchain\s*\(|JavaLanguageVersion\.of\s*\(\s*"?)\s*(\d+))re"));
        v.toolchain = tc.match(withoutComments(text)).captured(1).toInt();
    }
    for (const QString &f : settingsFiles)
        if (readText(f).contains(QLatin1String("foojay-resolver")))
            v.autoProvision = true;

    // Build tool: the Gradle wrapper, else the Maven wrapper.
    v.build = wrapperVersion(root);
    if (v.build.isEmpty()) {
        Found mvn;
        if (firstMatch(root + QStringLiteral("/.mvn/wrapper/maven-wrapper.properties"), {rx(R"re(apache-maven-([\d][\w.-]*?)-bin)re")}, &mvn))
            v.build = mvn;
    }
    return v;
}

// --- Gradle wrapper ---------------------------------------------------------------------------------------------------

QString wrapperProperties(const QString &dir)
{
    const QString p = dir + QStringLiteral("/gradle/wrapper/gradle-wrapper.properties");
    return QFileInfo::exists(p) ? p : QString();
}

Found wrapperVersion(const QString &dir)
{
    Found f;
    const QString p = wrapperProperties(dir);
    if (!p.isEmpty())
        firstMatch(p, {QRegularExpression(QStringLiteral(R"re(gradle-([\d][\w.-]*?)-(?:bin|all)\.zip)re"))}, &f);
    return f;
}

bool wrapperNeedsExecBit(const QString &dir)
{
    return Platform::wrapperNeedsExecBit(dir, QStringLiteral("gradlew"));
}

} // namespace JvmTools
