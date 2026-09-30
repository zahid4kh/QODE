#include "Language.h"

#include <QFileInfo>
#include <QHash>

namespace {

using RE = QRegularExpression;

RE re(const QString &p, RE::PatternOptions o = RE::NoPatternOption)
{
    return RE(p, o);
}

QString words(const QStringList &w)
{
    return QStringLiteral("\\b(?:") + w.join(QLatin1Char('|')) + QStringLiteral(")\\b");
}

const QString numberPattern = QStringLiteral(
    "\\b(?:0[xX][0-9a-fA-F']+|0[bB][01']+|\\d[\\d']*(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)[uUlLfFn]*\\b");

const QString dqString = QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*(?:\"|\\\\?$)");
const QString sqString = QStringLiteral("'(?:[^'\\\\]|\\\\.)*(?:'|\\\\?$)");

LanguageDefinition cFamily(const QString &name, bool cpp)
{
    LanguageDefinition d;
    d.name = name;
    d.lineComment = QStringLiteral("//");

    QStringList kw = {"auto", "break", "case", "const", "continue", "default", "do", "else", "enum", "extern",
                      "for", "goto", "if", "inline", "register", "restrict", "return", "sizeof", "static",
                      "struct", "switch", "typedef", "union", "volatile", "while"};
    QStringList types = {"void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
                         "size_t", "ssize_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
                         "uint32_t", "uint64_t", "bool", "FILE", "NULL"};
    if (cpp) {
        kw += {"alignas", "alignof", "asm", "catch", "class", "concept", "consteval", "constexpr", "constinit",
               "const_cast", "co_await", "co_return", "co_yield", "decltype", "delete", "dynamic_cast", "explicit",
               "export", "false", "final", "friend", "mutable", "namespace", "new", "noexcept", "nullptr",
               "operator", "override", "private", "protected", "public", "reinterpret_cast", "requires",
               "static_assert", "static_cast", "template", "this", "thread_local", "throw", "true", "try",
               "typeid", "typename", "using", "virtual", "Q_OBJECT", "signals", "slots", "emit", "foreach"};
        types += {"wchar_t", "char8_t", "char16_t", "char32_t", "string", "vector", "map", "set", "unique_ptr",
                  "shared_ptr", "QString", "QObject", "QWidget", "std"};
    }
    d.rules.append({re(words(types)), TokenRole::Type});
    d.rules.append({re(words(kw)), TokenRole::Keyword});
    d.rules.append({re(QStringLiteral("\\b[A-Z][A-Za-z0-9]*(?=\\s*[&*]?\\s+[&*]?\\w)")), TokenRole::Type});
    d.rules.append({re(QStringLiteral("\\b[A-Za-z_]\\w*(?=\\s*\\()")), TokenRole::Function});
    d.rules.append({re(numberPattern), TokenRole::Number});
    d.rules.append({re(QStringLiteral("^\\s*#\\s*[A-Za-z_]+")), TokenRole::Preprocessor});
    d.rules.append({re(QStringLiteral("^\\s*#\\s*include\\s*(<[^>\\n]*>)"), RE::NoPatternOption), TokenRole::String, 1});

    d.delimited.append({re(QStringLiteral("//[^\\n]*")), {}, TokenRole::Comment});
    d.delimited.append({re(QStringLiteral("/\\*")), re(QStringLiteral("\\*/")), TokenRole::Comment});
    if (cpp)
        d.delimited.append({re(QStringLiteral("R\"([^(\\s]*)\\(")), re(QStringLiteral("\\)[^\"\\s]*\"")), TokenRole::String});
    d.delimited.append({re(dqString), {}, TokenRole::String});
    d.delimited.append({re(sqString), {}, TokenRole::String});
    return d;
}

LanguageDefinition python()
{
    LanguageDefinition d;
    d.name = QStringLiteral("Python");
    d.lineComment = QStringLiteral("#");
    d.rules.append({re(words({"False", "None", "True", "and", "as", "assert", "async", "await", "break", "class",
                              "continue", "def", "del", "elif", "else", "except", "finally", "for", "from",
                              "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass",
                              "raise", "return", "try", "while", "with", "yield", "match", "case"})),
                    TokenRole::Keyword});
    d.rules.append({re(words({"int", "str", "float", "bool", "list", "dict", "set", "tuple", "bytes", "object",
                              "print", "len", "range", "type", "isinstance", "self", "cls", "super", "open"})),
                    TokenRole::Type});
    d.rules.append({re(QStringLiteral("\\b[A-Za-z_]\\w*(?=\\s*\\()")), TokenRole::Function});
    d.rules.append({re(QStringLiteral("^\\s*@[\\w.]+")), TokenRole::Preprocessor});
    d.rules.append({re(numberPattern), TokenRole::Number});
    d.delimited.append({re(QStringLiteral("#[^\\n]*")), {}, TokenRole::Comment});
    d.delimited.append({re(QStringLiteral("[rRbBfFuU]{0,2}\"\"\"")), re(QStringLiteral("\"\"\"")), TokenRole::String});
    d.delimited.append({re(QStringLiteral("[rRbBfFuU]{0,2}'''")), re(QStringLiteral("'''")), TokenRole::String});
    d.delimited.append({re(QStringLiteral("[rRbBfFuU]{0,2}") + dqString), {}, TokenRole::String});
    d.delimited.append({re(QStringLiteral("[rRbBfFuU]{0,2}") + sqString), {}, TokenRole::String});
    return d;
}

LanguageDefinition javascript(bool ts)
{
    LanguageDefinition d;
    d.name = ts ? QStringLiteral("TypeScript") : QStringLiteral("JavaScript");
    d.lineComment = QStringLiteral("//");
    QStringList kw = {"async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
                      "default", "delete", "do", "else", "export", "extends", "finally", "for", "function", "if",
                      "import", "in", "instanceof", "let", "new", "of", "return", "static", "super", "switch",
                      "this", "throw", "try", "typeof", "var", "void", "while", "with", "yield", "true", "false",
                      "null", "undefined", "get", "set", "from", "as"};
    QStringList types = {"Array", "Object", "String", "Number", "Boolean", "Promise", "Map", "Set", "Date", "JSON",
                         "Math", "console", "window", "document", "Error", "RegExp", "Symbol"};
    if (ts) {
        kw += {"interface", "type", "enum", "implements", "namespace", "declare", "abstract", "readonly", "private",
               "protected", "public", "keyof", "infer", "is", "satisfies"};
        types += {"string", "number", "boolean", "any", "unknown", "never", "object", "bigint", "symbol"};
    }
    d.rules.append({re(words(types)), TokenRole::Type});
    d.rules.append({re(words(kw)), TokenRole::Keyword});
    d.rules.append({re(QStringLiteral("\\b[A-Za-z_$][\\w$]*(?=\\s*\\()")), TokenRole::Function});
    d.rules.append({re(numberPattern), TokenRole::Number});
    d.delimited.append({re(QStringLiteral("//[^\\n]*")), {}, TokenRole::Comment});
    d.delimited.append({re(QStringLiteral("/\\*")), re(QStringLiteral("\\*/")), TokenRole::Comment});
    d.delimited.append({re(QStringLiteral("`")), re(QStringLiteral("(?<!\\\\)`")), TokenRole::String});
    d.delimited.append({re(dqString), {}, TokenRole::String});
    d.delimited.append({re(sqString), {}, TokenRole::String});
    return d;
}

LanguageDefinition json()
{
    LanguageDefinition d;
    d.name = QStringLiteral("JSON");
    d.rules.append({re(QStringLiteral("\\b(?:true|false|null)\\b")), TokenRole::Keyword});
    d.rules.append({re(QStringLiteral("-?\\b\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?\\b")), TokenRole::Number});
    // Keys are listed before plain strings so they win ties at the same start position.
    d.delimited.append({re(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"(?=\\s*:)")), {}, TokenRole::Attribute});
    d.delimited.append({re(dqString), {}, TokenRole::String});
    return d;
}

LanguageDefinition markup(const QString &name, bool html)
{
    LanguageDefinition d;
    d.name = name;
    d.rules.append({re(QStringLiteral("</?[A-Za-z][\\w:.-]*|/?>|<\\?[\\w-]*|\\?>")), TokenRole::Tag});
    d.rules.append({re(QStringLiteral("&[#\\w]+;")), TokenRole::Number});
    d.rules.append({re(QStringLiteral("\\b[A-Za-z_:][\\w:.-]*(?=\\s*=)")), TokenRole::Attribute});
    if (html)
        d.rules.append({re(QStringLiteral("<!DOCTYPE[^>]*>"), RE::CaseInsensitiveOption), TokenRole::Preprocessor});
    d.delimited.append({re(QStringLiteral("<!--")), re(QStringLiteral("-->")), TokenRole::Comment});
    // Strings only make sense inside tags; approximated by matching quoted values after '='.
    d.delimited.append({re(QStringLiteral("(?<==)\\s*\"[^\"]*\"?")), {}, TokenRole::String});
    d.delimited.append({re(QStringLiteral("(?<==)\\s*'[^']*'?")), {}, TokenRole::String});
    return d;
}

LanguageDefinition css()
{
    LanguageDefinition d;
    d.name = QStringLiteral("CSS");
    d.rules.append({re(QStringLiteral("[.#][A-Za-z_-][\\w-]*")), TokenRole::Type});
    d.rules.append({re(QStringLiteral("[A-Za-z-]+(?=\\s*:)")), TokenRole::Attribute});
    d.rules.append({re(QStringLiteral("@[A-Za-z-]+")), TokenRole::Keyword});
    d.rules.append({re(QStringLiteral("!important")), TokenRole::Keyword});
    d.rules.append({re(QStringLiteral("#[0-9a-fA-F]{3,8}\\b")), TokenRole::Number});
    d.rules.append({re(QStringLiteral("-?\\b\\d+(?:\\.\\d+)?(?:px|em|rem|%|vh|vw|s|ms|deg|pt|fr)?\\b")), TokenRole::Number});
    d.rules.append({re(QStringLiteral("\\b[A-Za-z-]+(?=\\()")), TokenRole::Function});
    d.delimited.append({re(QStringLiteral("/\\*")), re(QStringLiteral("\\*/")), TokenRole::Comment});
    d.delimited.append({re(dqString), {}, TokenRole::String});
    d.delimited.append({re(sqString), {}, TokenRole::String});
    return d;
}

LanguageDefinition markdown()
{
    LanguageDefinition d;
    d.name = QStringLiteral("Markdown");
    d.rules.append({re(QStringLiteral("^\\s*(?:[-*+]|\\d+[.)])\\s")), TokenRole::Keyword});
    d.rules.append({re(QStringLiteral("^>.*")), TokenRole::Comment});
    d.rules.append({re(QStringLiteral("\\*[^*\\n]+\\*|(?<!\\w)_[^_\\n]+_(?!\\w)")), TokenRole::Emphasis});
    d.rules.append({re(QStringLiteral("\\*\\*[^*\\n]+\\*\\*|__[^_\\n]+__")), TokenRole::Strong});
    d.rules.append({re(QStringLiteral("\\[[^\\]\\n]*\\]\\([^)\\n]*\\)")), TokenRole::Link});
    d.rules.append({re(QStringLiteral("`[^`\\n]+`")), TokenRole::Code});
    d.rules.append({re(QStringLiteral("^#{1,6}\\s.*")), TokenRole::Heading});
    d.rules.append({re(QStringLiteral("^(?:-{3,}|\\*{3,}|_{3,})\\s*$")), TokenRole::Comment});
    d.delimited.append({re(QStringLiteral("^\\s*```[^\\n]*")), re(QStringLiteral("^\\s*```")), TokenRole::Code});
    return d;
}

LanguageDefinition shell()
{
    LanguageDefinition d;
    d.name = QStringLiteral("Shell");
    d.lineComment = QStringLiteral("#");
    d.rules.append({re(words({"if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case",
                              "esac", "in", "function", "select", "return", "exit", "break", "continue", "local",
                              "export", "readonly", "declare", "unset", "shift", "source", "trap"})),
                    TokenRole::Keyword});
    d.rules.append({re(words({"echo", "printf", "cd", "ls", "cat", "grep", "sed", "awk", "mkdir", "rm", "cp", "mv",
                              "chmod", "chown", "test", "read", "eval", "exec", "set", "alias", "true", "false"})),
                    TokenRole::Function});
    d.rules.append({re(QStringLiteral("\\$(?:\\{[^}\\n]*\\}|[A-Za-z_]\\w*|[0-9@#?$!*-])")), TokenRole::Type});
    d.rules.append({re(numberPattern), TokenRole::Number});
    d.rules.append({re(QStringLiteral("^#!.*")), TokenRole::Preprocessor});
    d.delimited.append({re(QStringLiteral("(?<![\\w$])#[^\\n]*")), {}, TokenRole::Comment});
    d.delimited.append({re(dqString), {}, TokenRole::String});
    d.delimited.append({re(QStringLiteral("'[^']*'?")), {}, TokenRole::String});
    return d;
}

struct Registry {
    QHash<QString, LanguageDefinition> defs; // keyed by language name
    QHash<QString, QString> byExtension;     // lowercase extension -> language name
    QHash<QString, QString> byFileName;      // exact lowercase file name -> language name

    Registry()
    {
        auto add = [this](const LanguageDefinition &d, const QStringList &exts, const QStringList &names = {}) {
            defs.insert(d.name, d);
            for (const QString &e : exts)
                byExtension.insert(e, d.name);
            for (const QString &n : names)
                byFileName.insert(n, d.name);
        };
        add(cFamily(QStringLiteral("C"), false), {"c"});
        add(cFamily(QStringLiteral("C++"), true), {"cpp", "cc", "cxx", "c++", "h", "hh", "hpp", "hxx", "h++", "ipp", "tpp", "inl", "ino", "qml"});
        add(python(), {"py", "pyw", "pyi"});
        add(javascript(false), {"js", "mjs", "cjs", "jsx"});
        add(javascript(true), {"ts", "tsx", "mts", "cts"});
        add(json(), {"json", "jsonc", "webmanifest"}, {".eslintrc", ".prettierrc"});
        add(markup(QStringLiteral("HTML"), true), {"html", "htm", "xhtml"});
        add(markup(QStringLiteral("XML"), false), {"xml", "xsl", "xslt", "svg", "ui", "qrc", "ts_xml", "plist", "rss", "atom", "xsd", "pom"});
        add(css(), {"css"});
        add(markdown(), {"md", "markdown", "mdown"});
        add(shell(), {"sh", "bash", "zsh", "ksh", "env", "profile", "bashrc", "zshrc"},
            {".bashrc", ".zshrc", ".profile", ".bash_profile", ".bash_aliases"});
    }
};

const Registry &registry()
{
    static const Registry r;
    return r;
}

} // namespace

namespace Languages {

const LanguageDefinition *forFile(const QString &fileName)
{
    const Registry &r = registry();
    const QString base = QFileInfo(fileName).fileName().toLower();
    QString lang = r.byFileName.value(base);
    if (lang.isEmpty())
        lang = r.byExtension.value(QFileInfo(base).suffix());
    if (lang.isEmpty())
        return nullptr;
    auto it = r.defs.constFind(lang);
    return it == r.defs.constEnd() ? nullptr : &it.value();
}

QString nameForFile(const QString &fileName)
{
    const LanguageDefinition *d = forFile(fileName);
    return d ? d->name : QStringLiteral("Plain Text");
}

} // namespace Languages
