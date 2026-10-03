#include "Snippets.h"

#include <QHash>
#include <QStringList>

namespace Snippets {

namespace {

struct Def {
    const char *prefix;
    const char *description;
    const char *body;
};

using Defs = QVector<Def>;

const Defs &jsCommon()
{
    static const Defs d = {
        {"clg", "console.log()", "console.log($1);$0"},
        {"cle", "console.error()", "console.error($1);$0"},
        {"clw", "console.warn()", "console.warn($1);$0"},
        {"fn", "function declaration", "function ${1:name}(${2:params}) {\n\t$0\n}"},
        {"afn", "async function declaration", "async function ${1:name}(${2:params}) {\n\t$0\n}"},
        {"arrow", "arrow function constant", "const ${1:name} = (${2:params}) => {\n\t$0\n};"},
        {"aarrow", "async arrow function constant", "const ${1:name} = async (${2:params}) => {\n\t$0\n};"},
        {"if", "if statement", "if (${1:condition}) {\n\t$0\n}"},
        {"ife", "if / else statement", "if (${1:condition}) {\n\t$2\n} else {\n\t$0\n}"},
        {"for", "for loop", "for (let ${1:i} = 0; $1 < ${2:array}.length; $1++) {\n\t$0\n}"},
        {"forof", "for...of loop", "for (const ${1:item} of ${2:items}) {\n\t$0\n}"},
        {"forin", "for...in loop", "for (const ${1:key} in ${2:object}) {\n\t$0\n}"},
        {"foreach", "array forEach", "${1:array}.forEach((${2:item}) => {\n\t$0\n});"},
        {"map", "array map", "${1:array}.map((${2:item}) => $0)"},
        {"filter", "array filter", "${1:array}.filter((${2:item}) => $0)"},
        {"reduce", "array reduce", "${1:array}.reduce((${2:acc}, ${3:item}) => {\n\t$0\n\treturn $2;\n}, ${4:initial})"},
        {"while", "while loop", "while (${1:condition}) {\n\t$0\n}"},
        {"switch", "switch statement", "switch (${1:expression}) {\n\tcase ${2:value}:\n\t\t$0\n\t\tbreak;\n\tdefault:\n\t\tbreak;\n}"},
        {"try", "try / catch", "try {\n\t$1\n} catch (${2:error}) {\n\t$0\n}"},
        {"tryf", "try / catch / finally", "try {\n\t$1\n} catch (${2:error}) {\n\t$3\n} finally {\n\t$0\n}"},
        {"prom", "new Promise", "new Promise((resolve, reject) => {\n\t$0\n})"},
        {"imp", "import from module", "import ${2:name} from '${1:module}';$0"},
        {"impn", "import { named } from module", "import { ${2:name} } from '${1:module}';$0"},
        {"impa", "import * as", "import * as ${2:name} from '${1:module}';$0"},
        {"exp", "export default", "export default ${1:name};$0"},
        {"cls", "class declaration", "class ${1:Name} {\n\tconstructor(${2:params}) {\n\t\t$0\n\t}\n}"},
        {"desc", "describe block", "describe('${1:name}', () => {\n\t$0\n});"},
        {"it", "test case", "it('${1:should}', () => {\n\t$0\n});"},
        {"settimeout", "setTimeout", "setTimeout(() => {\n\t$0\n}, ${1:1000});"},
        {"fetch", "fetch request", "const ${1:res} = await fetch(${2:url});\nconst ${3:data} = await $1.json();$0"},
        // React
        {"rfc", "React function component", "export default function ${1:Component}() {\n\treturn (\n\t\t<div>$0</div>\n\t);\n}"},
        {"rafce", "React arrow function component + export",
         "const ${1:Component} = () => {\n\treturn (\n\t\t<div>$0</div>\n\t);\n};\n\nexport default $1;"},
        {"rafc", "React arrow function component", "export const ${1:Component} = () => {\n\treturn (\n\t\t<div>$0</div>\n\t);\n};"},
        {"cc", "'use client' directive", "\"use client\";\n$0"},
        {"imr", "import React", "import React from 'react';$0"},
        {"us", "useState hook", "const [${1:state}, set${2:State}] = useState(${3:initial});$0"},
        {"ue", "useEffect hook", "useEffect(() => {\n\t$1\n}, [$2]);$0"},
        {"uc", "useCallback hook", "const ${1:callback} = useCallback((${2:args}) => {\n\t$3\n}, [$4]);$0"},
        {"um", "useMemo hook", "const ${1:value} = useMemo(() => {\n\treturn $2;\n}, [$3]);$0"},
        {"ur", "useRef hook", "const ${1:ref} = useRef(${2:null});$0"},
        {"uctx", "useContext hook", "const ${1:value} = useContext(${2:Context});$0"},
    };
    return d;
}

const Defs &tsOnly()
{
    static const Defs d = {
        {"interface", "interface declaration", "interface ${1:Name} {\n\t$0\n}"},
        {"type", "type alias", "type ${1:Name} = $0;"},
        {"enum", "enum declaration", "enum ${1:Name} {\n\t$0\n}"},
        {"props", "React props interface", "interface ${1:Component}Props {\n\t$0\n}"},
        {"rfcp", "React function component with props",
         "interface ${1:Component}Props {\n\t$2\n}\n\nexport default function $1({ $3 }: $1Props) {\n\treturn (\n\t\t<div>$0</div>\n\t);\n}"},
    };
    return d;
}

const Defs &html()
{
    static const Defs d = {
        {"html5", "HTML5 boilerplate",
         "<!DOCTYPE html>\n<html lang=\"${1:en}\">\n<head>\n\t<meta charset=\"UTF-8\">\n\t<meta name=\"viewport\" content=\"width=device-width, "
         "initial-scale=1.0\">\n\t<title>${2:Document}</title>\n</head>\n<body>\n\t$0\n</body>\n</html>"},
        {"linkcss", "stylesheet link", "<link rel=\"stylesheet\" href=\"${1:style.css}\">$0"},
        {"scriptsrc", "external script", "<script src=\"${1:script.js}\"></script>$0"},
        {"ul", "unordered list", "<ul>\n\t<li>$1</li>\n\t<li>$0</li>\n</ul>"},
        {"ol", "ordered list", "<ol>\n\t<li>$1</li>\n\t<li>$0</li>\n</ol>"},
        {"table", "table", "<table>\n\t<thead>\n\t\t<tr>\n\t\t\t<th>$1</th>\n\t\t</tr>\n\t</thead>\n\t<tbody>\n\t\t<tr>\n\t\t\t<td>$0</td>\n\t\t</tr>\n\t</tbody>\n</table>"},
        {"form", "form", "<form action=\"${1:#}\" method=\"${2:post}\">\n\t$0\n</form>"},
        {"input", "input element", "<input type=\"${1:text}\" name=\"${2:name}\" id=\"$2\">$0"},
        {"label", "label + input", "<label for=\"${1:id}\">${2:Label}</label>\n<input type=\"${3:text}\" id=\"$1\" name=\"$1\">$0"},
        {"btn", "button", "<button type=\"${1:button}\">${2:Click}</button>$0"},
        {"a", "anchor", "<a href=\"${1:#}\">${2:text}</a>$0"},
        {"img", "image", "<img src=\"${1:src}\" alt=\"${2:description}\">$0"},
    };
    return d;
}

const Defs &css()
{
    static const Defs d = {
        {"flex", "flex container", "display: flex;\nalign-items: ${1:center};\njustify-content: ${2:center};$0"},
        {"flexcol", "flex column", "display: flex;\nflex-direction: column;\ngap: ${1:1rem};$0"},
        {"grid", "grid container", "display: grid;\ngrid-template-columns: ${1:repeat(3, 1fr)};\ngap: ${2:1rem};$0"},
        {"center", "center with flexbox", "display: flex;\nalign-items: center;\njustify-content: center;$0"},
        {"media", "media query", "@media (max-width: ${1:768px}) {\n\t$0\n}"},
        {"keyframes", "keyframes", "@keyframes ${1:name} {\n\tfrom {\n\t\t$2\n\t}\n\tto {\n\t\t$0\n\t}\n}"},
        {"var", "CSS variable", "var(--${1:name})$0"},
        {"abs", "absolute fill", "position: absolute;\ninset: 0;$0"},
        {"trans", "transition", "transition: ${1:all} ${2:0.3s} ${3:ease};$0"},
    };
    return d;
}

const Defs &python()
{
    static const Defs d = {
        {"def", "function", "def ${1:name}(${2:args}):\n\t${0:pass}"},
        {"adef", "async function", "async def ${1:name}(${2:args}):\n\t${0:pass}"},
        {"class", "class", "class ${1:Name}:\n\tdef __init__(self${2:, args}):\n\t\t${0:pass}"},
        {"dc", "dataclass", "@dataclass\nclass ${1:Name}:\n\t${0:field}: ${2:str}"},
        {"main", "if __name__ == \"__main__\"", "if __name__ == \"__main__\":\n\t${0:main()}"},
        {"if", "if statement", "if ${1:condition}:\n\t$0"},
        {"ife", "if / else", "if ${1:condition}:\n\t$2\nelse:\n\t$0"},
        {"for", "for loop", "for ${1:item} in ${2:items}:\n\t$0"},
        {"fori", "for in range", "for ${1:i} in range(${2:n}):\n\t$0"},
        {"while", "while loop", "while ${1:condition}:\n\t$0"},
        {"try", "try / except", "try:\n\t$1\nexcept ${2:Exception} as ${3:e}:\n\t$0"},
        {"with", "with statement", "with ${1:open(path)} as ${2:f}:\n\t$0"},
        {"lc", "list comprehension", "[${1:x} for ${2:x} in ${3:items}]$0"},
        {"lam", "lambda", "lambda ${1:x}: $0"},
    };
    return d;
}

const Defs &c()
{
    static const Defs d = {
        {"main", "main function", "int main(int argc, char *argv[])\n{\n\t$0\n\treturn 0;\n}"},
        {"inc", "#include <>", "#include <${1:stdio.h}>$0"},
        {"incl", "#include \"\"", "#include \"${1:header.h}\"$0"},
        {"for", "for loop", "for (int ${1:i} = 0; $1 < ${2:n}; ++$1) {\n\t$0\n}"},
        {"while", "while loop", "while (${1:condition}) {\n\t$0\n}"},
        {"if", "if statement", "if (${1:condition}) {\n\t$0\n}"},
        {"ife", "if / else", "if (${1:condition}) {\n\t$2\n} else {\n\t$0\n}"},
        {"switch", "switch", "switch (${1:expression}) {\ncase ${2:value}:\n\t$0\n\tbreak;\ndefault:\n\tbreak;\n}"},
        {"struct", "struct", "struct ${1:Name} {\n\t$0\n};"},
        {"typedef", "typedef struct", "typedef struct {\n\t$0\n} ${1:Name};"},
        {"guard", "include guard", "#ifndef ${1:HEADER_H}\n#define $1\n\n$0\n\n#endif // $1"},
        {"printf", "printf", "printf(\"${1:%s}\\n\", $2);$0"},
    };
    return d;
}

const Defs &cpp()
{
    static const Defs d = {
        {"main", "main function", "int main(int argc, char *argv[])\n{\n\t$0\n\treturn 0;\n}"},
        {"inc", "#include <>", "#include <${1:iostream}>$0"},
        {"incl", "#include \"\"", "#include \"${1:header.h}\"$0"},
        {"once", "#pragma once", "#pragma once\n$0"},
        {"guard", "include guard", "#ifndef ${1:HEADER_H}\n#define $1\n\n$0\n\n#endif // $1"},
        {"for", "for loop", "for (int ${1:i} = 0; $1 < ${2:n}; ++$1) {\n\t$0\n}"},
        {"forr", "range-based for", "for (const auto &${1:item} : ${2:container}) {\n\t$0\n}"},
        {"while", "while loop", "while (${1:condition}) {\n\t$0\n}"},
        {"if", "if statement", "if (${1:condition}) {\n\t$0\n}"},
        {"ife", "if / else", "if (${1:condition}) {\n\t$2\n} else {\n\t$0\n}"},
        {"switch", "switch", "switch (${1:expression}) {\ncase ${2:value}:\n\t$0\n\tbreak;\ndefault:\n\tbreak;\n}"},
        {"class", "class", "class ${1:Name}\n{\npublic:\n\t$1();\n\t~$1();\n\nprivate:\n\t$0\n};"},
        {"struct", "struct", "struct ${1:Name} {\n\t$0\n};"},
        {"ns", "namespace", "namespace ${1:name} {\n\n$0\n\n} // namespace $1"},
        {"cout", "std::cout", "std::cout << ${1:value} << std::endl;$0"},
        {"cerr", "std::cerr", "std::cerr << ${1:value} << std::endl;$0"},
        {"lambda", "lambda", "[${1:&}](${2:args}) {\n\t$0\n}"},
        {"uptr", "std::unique_ptr", "std::unique_ptr<${1:T}> ${2:name} = std::make_unique<$1>($3);$0"},
        {"sptr", "std::shared_ptr", "std::shared_ptr<${1:T}> ${2:name} = std::make_shared<$1>($3);$0"},
        {"try", "try / catch", "try {\n\t$1\n} catch (const ${2:std::exception} &${3:e}) {\n\t$0\n}"},
        {"tmpl", "template", "template <typename ${1:T}>\n$0"},
        {"qdebug", "qDebug()", "qDebug() << $1;$0"},
    };
    return d;
}

const Defs &java()
{
    static const Defs d = {
        {"main", "main method", "public static void main(String[] args) {\n\t$0\n}"},
        {"sout", "System.out.println", "System.out.println($1);$0"},
        {"class", "class", "public class ${1:Name} {\n\t$0\n}"},
        {"fori", "for loop", "for (int ${1:i} = 0; $1 < ${2:n}; $1++) {\n\t$0\n}"},
        {"fore", "enhanced for", "for (${1:Type} ${2:item} : ${3:items}) {\n\t$0\n}"},
        {"if", "if statement", "if (${1:condition}) {\n\t$0\n}"},
        {"ife", "if / else", "if (${1:condition}) {\n\t$2\n} else {\n\t$0\n}"},
        {"try", "try / catch", "try {\n\t$1\n} catch (${2:Exception} ${3:e}) {\n\t$0\n}"},
        {"switch", "switch", "switch (${1:expression}) {\n\tcase ${2:value}:\n\t\t$0\n\t\tbreak;\n\tdefault:\n\t\tbreak;\n}"},
    };
    return d;
}

const Defs &kotlin()
{
    static const Defs d = {
        {"main", "main function", "fun main() {\n\t$0\n}"},
        {"fun", "function", "fun ${1:name}(${2:args}): ${3:Unit} {\n\t$0\n}"},
        {"class", "class", "class ${1:Name}(${2:args}) {\n\t$0\n}"},
        {"dc", "data class", "data class ${1:Name}(val ${2:field}: ${3:String})$0"},
        {"println", "println", "println($1)$0"},
        {"when", "when expression", "when (${1:value}) {\n\t${2:cond} -> $3\n\telse -> $0\n}"},
        {"for", "for loop", "for (${1:item} in ${2:items}) {\n\t$0\n}"},
        {"if", "if statement", "if (${1:condition}) {\n\t$0\n}"},
        {"ife", "if / else", "if (${1:condition}) {\n\t$2\n} else {\n\t$0\n}"},
        {"try", "try / catch", "try {\n\t$1\n} catch (e: ${2:Exception}) {\n\t$0\n}"},
    };
    return d;
}

const Defs &shell()
{
    static const Defs d = {
        {"sh", "bash script header", "#!/usr/bin/env bash\nset -euo pipefail\n\n$0"},
        {"if", "if statement", "if [[ ${1:condition} ]]; then\n\t$0\nfi"},
        {"ife", "if / else", "if [[ ${1:condition} ]]; then\n\t$2\nelse\n\t$0\nfi"},
        {"for", "for loop", "for ${1:item} in ${2:items}; do\n\t$0\ndone"},
        {"while", "while loop", "while ${1:condition}; do\n\t$0\ndone"},
        {"case", "case statement", "case \"${1:\\$var}\" in\n\t${2:pattern})\n\t\t$0\n\t\t;;\n\t*)\n\t\t;;\nesac"},
        {"func", "function", "${1:name}() {\n\t$0\n}"},
    };
    return d;
}

const Defs &markdown()
{
    static const Defs d = {
        {"link", "link", "[${1:text}](${2:url})$0"},
        {"img", "image", "![${1:alt}](${2:path})$0"},
        {"code", "fenced code block", "```${1:lang}\n$0\n```"},
        {"table", "table", "| ${1:Column} | ${2:Column} |\n| --- | --- |\n| $0 | |"},
        {"todo", "task list item", "- [ ] $0"},
    };
    return d;
}

QVector<LspCompletionItem> build(std::initializer_list<const Defs *> lists)
{
    QVector<LspCompletionItem> out;
    for (const Defs *defs : lists)
        for (const Def &d : *defs) {
            LspCompletionItem it;
            it.label = QString::fromLatin1(d.prefix);
            it.detail = QString::fromUtf8(d.description);
            it.insertText = QString::fromUtf8(d.body);
            it.snippet = true;
            it.builtin = true;
            it.kind = 15; // Snippet
            it.sortText = QStringLiteral("!") + it.label; // ahead of the server's items with the same match quality
            out.append(it);
        }
    return out;
}

} // namespace

const QVector<LspCompletionItem> &forLanguage(const QString &language)
{
    static const QHash<QString, QVector<LspCompletionItem>> all = {
        {QStringLiteral("JavaScript"), build({&jsCommon()})},
        {QStringLiteral("TypeScript"), build({&jsCommon(), &tsOnly()})},
        {QStringLiteral("HTML"), build({&html()})},
        {QStringLiteral("CSS"), build({&css()})},
        {QStringLiteral("Python"), build({&python()})},
        {QStringLiteral("C"), build({&c()})},
        {QStringLiteral("C++"), build({&cpp()})},
        {QStringLiteral("Java"), build({&java()})},
        {QStringLiteral("Kotlin"), build({&kotlin()})},
        {QStringLiteral("Shell"), build({&shell()})},
        {QStringLiteral("Markdown"), build({&markdown()})},
    };
    static const QVector<LspCompletionItem> none;
    const auto it = all.constFind(language);
    return it == all.constEnd() ? none : it.value();
}

} // namespace Snippets
