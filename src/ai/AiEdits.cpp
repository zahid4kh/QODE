#include "AiEdits.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace AiEdits {

namespace {
QJsonObject prop(const char *type, const char *description = nullptr)
{
    QJsonObject o{{QStringLiteral("type"), QLatin1String(type)}};
    if (description)
        o.insert(QStringLiteral("description"), QLatin1String(description));
    return o;
}

QJsonObject stringArray(const char *description)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("ARRAY")},
                       {QStringLiteral("description"), QLatin1String(description)},
                       {QStringLiteral("items"), prop("STRING")}};
}

bool parseObject(const QString &json, QJsonObject *out, QString *error)
{
    QString text = json.trimmed();
    if (text.startsWith(QStringLiteral("```"))) { // tolerate a fenced answer
        text = text.mid(text.indexOf(QLatin1Char('\n')) + 1);
        if (text.endsWith(QStringLiteral("```")))
            text.chop(3);
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &pe);
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("The answer was not valid JSON (%1).").arg(pe.errorString());
        return false;
    }
    *out = doc.object();
    return true;
}
} // namespace

QJsonObject planSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("OBJECT")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("needsEdits"), prop("BOOLEAN", "true when files must be changed or created; false for a question that only needs an answer")},
             {QStringLiteral("answer"), prop("STRING", "markdown reply when needsEdits is false, otherwise an empty string")},
             {QStringLiteral("steps"), stringArray("the numbered plan, one short imperative sentence per step, in execution order; empty when needsEdits is false")},
             {QStringLiteral("files"), stringArray("project-relative paths of every existing file the steps read or modify (and new files to create)")},
         }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("needsEdits"), QStringLiteral("answer"), QStringLiteral("steps"), QStringLiteral("files")}},
    };
}

QJsonObject editSchema()
{
    const QJsonObject edit{
        {QStringLiteral("type"), QStringLiteral("OBJECT")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("path"), prop("STRING", "project-relative file path")},
             {QStringLiteral("op"), QJsonObject{{QStringLiteral("type"), QStringLiteral("STRING")},
                                                {QStringLiteral("enum"), QJsonArray{QStringLiteral("replace"), QStringLiteral("insert"), QStringLiteral("create")}},
                                                {QStringLiteral("description"), QStringLiteral("replace lines startLine..endLine, insert before startLine, or create a new file")}}},
             {QStringLiteral("startLine"), prop("INTEGER", "1-based first line; 0 for create")},
             {QStringLiteral("endLine"), prop("INTEGER", "1-based last line (inclusive) for replace; 0 otherwise")},
             {QStringLiteral("code"), prop("STRING", "the new code for those lines, exactly as it should appear in the file, without line numbers; empty string to delete the lines")},
         }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("path"), QStringLiteral("op"), QStringLiteral("startLine"), QStringLiteral("endLine"), QStringLiteral("code")}},
    };
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("OBJECT")},
        {QStringLiteral("properties"), QJsonObject{{QStringLiteral("edits"), QJsonObject{{QStringLiteral("type"), QStringLiteral("ARRAY")}, {QStringLiteral("items"), edit}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("edits")}},
    };
}

QString systemPrompt()
{
    return QStringLiteral(
        "You are the coding assistant built into QODE, a code editor. The user gives you a request about the project that is open "
        "in the editor. You work in up to three phases; the first line of each user message names the phase.\n\n"
        "[PLAN] Decide what must be done. Answer with JSON. If the request is a question or needs no file change, set needsEdits to "
        "false and put the complete answer (markdown) in `answer`. Otherwise set needsEdits to true and list the numbered `steps` "
        "(short, concrete, in order) and every project file the work touches in `files`. The message lists the project files and "
        "includes the contents of the files the user attached, each line prefixed with its line number. Use only paths from that list "
        "(or new paths under existing folders for files to create).\n\n"
        "[EDIT] Produce the changes for the plan as JSON `edits`. Each edit has a project-relative `path` and an `op`:\n"
        "- replace: `code` replaces the lines startLine..endLine (1-based, inclusive). Empty `code` deletes them.\n"
        "- insert: `code` is inserted before line startLine (use lineCount + 1 to append at the end of the file).\n"
        "- create: `code` is the full content of a new file; startLine and endLine are 0.\n"
        "Line numbers refer to the files exactly as shown in the message, before any edit is applied; edits of one file must not "
        "overlap. `code` is the final text of those lines with the file's own indentation and style and NEVER contains the line-number "
        "prefixes shown in the listing. Replace the smallest range that does the job, and keep every line of the range that must "
        "survive. Do not touch files outside the plan.\n\n"
        "[SUMMARY] You are told which edits were applied or rejected. Write the final reply in markdown for the user: what was changed, "
        "file by file, and anything that failed or still needs attention. Be concise and factual: no greetings, no filler, no praise, "
        "no restating the request. Use short bullet points and inline code for identifiers; add a code block only when it clarifies "
        "something.\n\n"
        "Never invent files, functions or APIs that the provided context does not show. If the request is ambiguous, pick the most "
        "reasonable reading and say so in the summary.");
}

bool parsePlan(const QString &json, Plan *plan, QString *error)
{
    QJsonObject o;
    if (!parseObject(json, &o, error))
        return false;
    plan->needsEdits = o.value(QStringLiteral("needsEdits")).toBool();
    plan->answer = o.value(QStringLiteral("answer")).toString();
    plan->steps.clear();
    plan->files.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("steps")).toArray())
        if (!v.toString().trimmed().isEmpty())
            plan->steps << v.toString().trimmed();
    for (const QJsonValue &v : o.value(QStringLiteral("files")).toArray()) {
        const QString f = v.toString().trimmed();
        if (!f.isEmpty() && !plan->files.contains(f))
            plan->files << f;
    }
    if (!plan->needsEdits && plan->answer.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("The plan was empty.");
        return false;
    }
    return true;
}

bool parseEdits(const QString &json, QVector<Edit> *edits, QString *error)
{
    QJsonObject o;
    if (!parseObject(json, &o, error))
        return false;
    edits->clear();
    for (const QJsonValue &v : o.value(QStringLiteral("edits")).toArray()) {
        const QJsonObject e = v.toObject();
        Edit edit;
        edit.path = e.value(QStringLiteral("path")).toString().trimmed();
        const QString op = e.value(QStringLiteral("op")).toString();
        edit.op = op == QLatin1String("insert") ? Edit::Insert : op == QLatin1String("create") ? Edit::Create : Edit::Replace;
        edit.startLine = e.value(QStringLiteral("startLine")).toInt();
        edit.endLine = e.value(QStringLiteral("endLine")).toInt();
        edit.code = stripLineNumbers(e.value(QStringLiteral("code")).toString().replace(QStringLiteral("\r\n"), QStringLiteral("\n")));
        if (!edit.path.isEmpty())
            edits->append(edit);
    }
    return true;
}

QString numbered(const QString &text)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    const int width = QString::number(lines.size()).size();
    QString out;
    out.reserve(text.size() + lines.size() * (width + 4));
    for (int i = 0; i < lines.size(); ++i) {
        // A trailing newline yields one empty last element that is not a line of its own.
        if (i == lines.size() - 1 && lines.at(i).isEmpty() && i > 0)
            break;
        out += QString::number(i + 1).rightJustified(width) + QStringLiteral(" | ") + lines.at(i) + QLatin1Char('\n');
    }
    return out;
}

QString stripLineNumbers(const QString &code)
{
    static const QRegularExpression prefix(QStringLiteral("^\\s*\\d+ \\| ?"));
    const QStringList lines = code.split(QLatin1Char('\n'));
    int nonEmpty = 0;
    for (const QString &l : lines) {
        if (l.trimmed().isEmpty())
            continue;
        ++nonEmpty;
        if (!prefix.match(l).hasMatch())
            return code;
    }
    if (nonEmpty < 2) // a single "1 | x" line may well be real code
        return code;
    QStringList out;
    for (const QString &l : lines)
        out << (l.trimmed().isEmpty() ? l : QString(l).remove(prefix));
    return out.join(QLatin1Char('\n'));
}

bool resolveInside(const QString &root, const QString &relative, QString *absolute)
{
    if (root.isEmpty() || relative.isEmpty() || QDir::isAbsolutePath(relative))
        return false;
    const QString clean = QDir::cleanPath(QDir(root).filePath(relative));
    const QString base = QDir::cleanPath(root);
    if (clean != base && !clean.startsWith(base + QLatin1Char('/')))
        return false;
    if (clean == base)
        return false;
    if (absolute)
        *absolute = clean;
    return true;
}

QString opName(Edit::Op op)
{
    switch (op) {
    case Edit::Insert: return QStringLiteral("insert");
    case Edit::Create: return QStringLiteral("create");
    default: return QStringLiteral("replace");
    }
}

} // namespace AiEdits
