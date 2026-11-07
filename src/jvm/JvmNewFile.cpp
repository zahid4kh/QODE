#include "jvm/JvmNewFile.h"

#include "jvm/JvmTools.h"
#include "platform/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace JvmNewFile {

namespace {

bool isJava(Kind k) { return k >= Kind::JavaClass; }

QString readHead(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.read(16 * 1024));
}

const QStringList &keywords()
{
    static const QStringList k = {
        // Kotlin hard keywords
        QStringLiteral("as"), QStringLiteral("break"), QStringLiteral("class"), QStringLiteral("continue"), QStringLiteral("do"),
        QStringLiteral("else"), QStringLiteral("false"), QStringLiteral("for"), QStringLiteral("fun"), QStringLiteral("if"),
        QStringLiteral("in"), QStringLiteral("interface"), QStringLiteral("is"), QStringLiteral("null"), QStringLiteral("object"),
        QStringLiteral("package"), QStringLiteral("return"), QStringLiteral("super"), QStringLiteral("this"), QStringLiteral("throw"),
        QStringLiteral("true"), QStringLiteral("try"), QStringLiteral("typealias"), QStringLiteral("typeof"), QStringLiteral("val"),
        QStringLiteral("var"), QStringLiteral("when"), QStringLiteral("while"),
        // Java
        QStringLiteral("abstract"), QStringLiteral("boolean"), QStringLiteral("byte"), QStringLiteral("case"), QStringLiteral("catch"),
        QStringLiteral("char"), QStringLiteral("default"), QStringLiteral("double"), QStringLiteral("enum"), QStringLiteral("extends"),
        QStringLiteral("final"), QStringLiteral("float"), QStringLiteral("goto"), QStringLiteral("implements"), QStringLiteral("import"),
        QStringLiteral("instanceof"), QStringLiteral("int"), QStringLiteral("long"), QStringLiteral("native"), QStringLiteral("new"),
        QStringLiteral("private"), QStringLiteral("protected"), QStringLiteral("public"), QStringLiteral("short"), QStringLiteral("static"),
        QStringLiteral("switch"), QStringLiteral("synchronized"), QStringLiteral("throws"), QStringLiteral("transient"),
        QStringLiteral("void"), QStringLiteral("volatile"), QStringLiteral("const")};
    return k;
}

} // namespace

QList<KindInfo> kinds(bool kotlin)
{
    if (kotlin)
        return {
            {Kind::Class, QStringLiteral("Class"), QLatin1Char('C'), 0},
            {Kind::File, QStringLiteral("File"), QLatin1Char('K'), 4},
            {Kind::Interface, QStringLiteral("Interface"), QLatin1Char('I'), 1},
            {Kind::DataClass, QStringLiteral("Data class"), QLatin1Char('D'), 0},
            {Kind::ValueClass, QStringLiteral("Value class"), QLatin1Char('V'), 0},
            {Kind::SealedClass, QStringLiteral("Sealed class"), QLatin1Char('S'), 0},
            {Kind::SealedInterface, QStringLiteral("Sealed interface"), QLatin1Char('S'), 1},
            {Kind::EnumClass, QStringLiteral("Enum class"), QLatin1Char('E'), 2},
            {Kind::Object, QStringLiteral("Object"), QLatin1Char('O'), 2},
            {Kind::Annotation, QStringLiteral("Annotation"), QLatin1Char('@'), 3},
        };
    return {
        {Kind::JavaClass, QStringLiteral("Class"), QLatin1Char('C'), 0},
        {Kind::JavaInterface, QStringLiteral("Interface"), QLatin1Char('I'), 1},
        {Kind::JavaRecord, QStringLiteral("Record"), QLatin1Char('R'), 4},
        {Kind::JavaEnum, QStringLiteral("Enum"), QLatin1Char('E'), 2},
        {Kind::JavaAnnotation, QStringLiteral("Annotation"), QLatin1Char('@'), 3},
        {Kind::JavaAbstractClass, QStringLiteral("Abstract class"), QLatin1Char('A'), 0},
        {Kind::JavaException, QStringLiteral("Exception"), QLatin1Char('X'), 0},
    };
}

QString packageFor(const QString &dir, bool *known)
{
    if (known)
        *known = true;
    const QDir d(dir);
    // A file already in the folder says what the package is (Kotlin folders need not spell out the root package).
    for (const QString &name : d.entryList({QStringLiteral("*.kt"), QStringLiteral("*.java")}, QDir::Files, QDir::Name)) {
        int line = -1;
        const QString pkg = JvmTools::declaredPackage(readHead(d.filePath(name)), &line);
        if (line >= 0)
            return pkg;
    }
    const QString folder = JvmTools::folderPackage(dir + QStringLiteral("/X.kt"));
    if (!folder.isNull())
        return folder;
    if (known)
        *known = false;
    return QString();
}

bool isSourceFolder(const QString &dir)
{
    if (!JvmTools::folderPackage(dir + QStringLiteral("/X.kt")).isNull())
        return true;
    return !QDir(dir).entryList({QStringLiteral("*.kt"), QStringLiteral("*.java")}, QDir::Files).isEmpty();
}

bool kotlinFits(const QString &dir)
{
    QString root;
    JvmTools::folderPackage(dir + QStringLiteral("/X.kt"), &root);
    if (root.endsWith(QLatin1String("/kotlin")) || !QDir(dir).entryList({QStringLiteral("*.kt")}, QDir::Files).isEmpty())
        return true;
    if (root.isEmpty())
        return false;
    // src/<set>/java in a module whose build applies Kotlin.
    const QString module = root + QStringLiteral("/../../.."); // <module>/src/<set>/java
    for (const char *f : {"build.gradle.kts", "build.gradle", "pom.xml"}) {
        const QString text = readHead(QDir::cleanPath(module + QLatin1Char('/') + QLatin1String(f)));
        if (text.contains(QLatin1String("kotlin")))
            return true;
    }
    return false;
}

bool javaFits(const QString &dir)
{
    QString root;
    JvmTools::folderPackage(dir + QStringLiteral("/X.java"), &root);
    return root.endsWith(QLatin1String("/java")) || !QDir(dir).entryList({QStringLiteral("*.java")}, QDir::Files).isEmpty();
}

Result generate(Kind kind, const QString &dir, const QString &input)
{
    Result r;
    const bool java = isJava(kind);
    QString text = input.trimmed();
    for (const char *ext : {".kt", ".java", ".kts"})
        if (text.endsWith(QLatin1String(ext)))
            text.chop(int(qstrlen(ext)));
    if (text.isEmpty()) {
        r.error = QStringLiteral("Enter a name.");
        return r;
    }
    const QStringList parts = text.split(QLatin1Char('.'));
    static const QRegularExpression ident(QStringLiteral(R"(^[A-Za-z_$][\w$]*$)"));
    for (const QString &p : parts) {
        if (!ident.match(p).hasMatch()) {
            r.error = p.isEmpty() ? QStringLiteral("A name part is empty.") : QStringLiteral("\"%1\" is not a valid name.").arg(p);
            return r;
        }
        if (keywords().contains(p)) {
            r.error = QStringLiteral("\"%1\" is a keyword.").arg(p);
            return r;
        }
        if (const QString bad = Platform::invalidFileName(p); !bad.isEmpty()) { // Windows: a class or package "Con"
            r.error = bad;
            return r;
        }
    }
    const QString name = parts.last();
    const QStringList subPackages = parts.mid(0, parts.size() - 1);
    QString pkg = packageFor(dir);
    for (const QString &s : subPackages)
        pkg = pkg.isEmpty() ? s : pkg + QLatin1Char('.') + s;
    QString folder = dir;
    for (const QString &s : subPackages)
        folder += QLatin1Char('/') + s;
    r.path = QDir::cleanPath(folder + QLatin1Char('/') + name + (java ? QStringLiteral(".java") : QStringLiteral(".kt")));
    if (QFileInfo::exists(r.path)) {
        r.error = QStringLiteral("%1 already exists.").arg(QFileInfo(r.path).fileName());
        return r;
    }

    QString head;
    if (!pkg.isEmpty())
        head = QStringLiteral("package ") + pkg + (java ? QStringLiteral(";\n\n") : QStringLiteral("\n\n"));
    QString body;
    QString caretMarker = QStringLiteral("\x01");
    switch (kind) {
    case Kind::Class: body = QStringLiteral("class %1 {\n    \x01\n}\n"); break;
    case Kind::File: body = QStringLiteral("\x01"); break;
    case Kind::Interface: body = QStringLiteral("interface %1 {\n    \x01\n}\n"); break;
    case Kind::DataClass: body = QStringLiteral("data class %1(\n    val \x01\n)\n"); break;
    case Kind::ValueClass: body = QStringLiteral("@JvmInline\nvalue class %1(val value: \x01)\n"); break;
    case Kind::SealedClass: body = QStringLiteral("sealed class %1 {\n    \x01\n}\n"); break;
    case Kind::SealedInterface: body = QStringLiteral("sealed interface %1 {\n    \x01\n}\n"); break;
    case Kind::EnumClass: body = QStringLiteral("enum class %1 {\n    \x01\n}\n"); break;
    case Kind::Object: body = QStringLiteral("object %1 {\n    \x01\n}\n"); break;
    case Kind::Annotation: body = QStringLiteral("annotation class %1\x01\n"); break;
    case Kind::JavaClass: body = QStringLiteral("public class %1 {\n    \x01\n}\n"); break;
    case Kind::JavaAbstractClass: body = QStringLiteral("public abstract class %1 {\n    \x01\n}\n"); break;
    case Kind::JavaInterface: body = QStringLiteral("public interface %1 {\n    \x01\n}\n"); break;
    case Kind::JavaRecord: body = QStringLiteral("public record %1(\x01) {\n}\n"); break;
    case Kind::JavaEnum: body = QStringLiteral("public enum %1 {\n    \x01\n}\n"); break;
    case Kind::JavaAnnotation: body = QStringLiteral("public @interface %1 {\n    \x01\n}\n"); break;
    case Kind::JavaException:
        body = QStringLiteral("public class %1 extends RuntimeException {\n\n    public %1(String message) {\n        super(message);\x01\n    }\n}\n");
        break;
    }
    const QString all = head + body.arg(name);
    const int at = int(all.indexOf(caretMarker));
    const QString before = all.left(at);
    r.caretLine = int(before.count(QLatin1Char('\n')));
    r.caretColumn = int(before.size() - (before.lastIndexOf(QLatin1Char('\n')) + 1));
    r.content = all;
    r.content.remove(caretMarker);
    return r;
}

} // namespace JvmNewFile
