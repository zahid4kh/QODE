#include "Formatter.h"

#include "lsp/LspServers.h"

#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QStandardPaths>

namespace Formatter {

namespace {

struct Tool {
    QString program;
    QStringList extensions; // lower-case, without dot
    QStringList (*args)(const QString &path);
};

QStringList clangArgs(const QString &p)
{
    return {QStringLiteral("--assume-filename=") + p, QStringLiteral("--fallback-style=LLVM")};
}
QStringList prettierArgs(const QString &p) { return {QStringLiteral("--stdin-filepath"), p}; }
QStringList blackArgs(const QString &) { return {QStringLiteral("-q"), QStringLiteral("-")}; }
QStringList ruffArgs(const QString &p) { return {QStringLiteral("format"), QStringLiteral("--stdin-filename"), p, QStringLiteral("-")}; }
QStringList noArgs(const QString &) { return {}; }
QStringList rustfmtArgs(const QString &) { return {QStringLiteral("--emit"), QStringLiteral("stdout")}; }
QStringList styluaArgs(const QString &) { return {QStringLiteral("-")}; }

// In order of preference for a file type.
const QList<Tool> &tools()
{
    static const QList<Tool> t = {
        {QStringLiteral("clang-format"),
         {QStringLiteral("c"), QStringLiteral("h"), QStringLiteral("cpp"), QStringLiteral("cc"), QStringLiteral("cxx"), QStringLiteral("hpp"),
          QStringLiteral("hh"), QStringLiteral("hxx"), QStringLiteral("ino"), QStringLiteral("m"), QStringLiteral("mm"), QStringLiteral("java"),
          QStringLiteral("cs"), QStringLiteral("proto")},
         clangArgs},
        {QStringLiteral("prettier"),
         {QStringLiteral("js"), QStringLiteral("jsx"), QStringLiteral("mjs"), QStringLiteral("cjs"), QStringLiteral("ts"), QStringLiteral("tsx"),
          QStringLiteral("json"), QStringLiteral("jsonc"), QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("less"),
          QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("vue"), QStringLiteral("md"), QStringLiteral("markdown"),
          QStringLiteral("yaml"), QStringLiteral("yml")},
         prettierArgs},
        // clang-format understands JavaScript and JSON as well, as a fallback when prettier is missing.
        {QStringLiteral("clang-format"),
         {QStringLiteral("js"), QStringLiteral("mjs"), QStringLiteral("cjs"), QStringLiteral("ts"), QStringLiteral("json")},
         clangArgs},
        {QStringLiteral("ruff"), {QStringLiteral("py"), QStringLiteral("pyi")}, ruffArgs},
        {QStringLiteral("black"), {QStringLiteral("py"), QStringLiteral("pyi")}, blackArgs},
        {QStringLiteral("gofmt"), {QStringLiteral("go")}, noArgs},
        {QStringLiteral("rustfmt"), {QStringLiteral("rs")}, rustfmtArgs},
        {QStringLiteral("shfmt"), {QStringLiteral("sh"), QStringLiteral("bash")}, noArgs},
        {QStringLiteral("stylua"), {QStringLiteral("lua")}, styluaArgs},
    };
    return t;
}

const Tool *find(const QString &filePath, QString *exe)
{
    const QString ext = QFileInfo(filePath).suffix().toLower();
    if (ext.isEmpty())
        return nullptr;
    for (const Tool &t : tools()) {
        if (!t.extensions.contains(ext))
            continue;
        QString path = QStandardPaths::findExecutable(t.program);
        if (path.isEmpty() && t.program == QLatin1String("ruff")) { // the copy QODE installed with the Python tools
            if (const LspServerSpec *ruff = LspServers::byId(QStringLiteral("ruff")))
                path = LspServers::managedExecutable(*ruff);
        }
        if (!path.isEmpty()) {
            if (exe)
                *exe = path;
            return &t;
        }
    }
    return nullptr;
}

} // namespace

bool isAvailable(const QString &filePath)
{
    return find(filePath, nullptr) != nullptr;
}

QString suggestedTools(const QString &filePath)
{
    const QString ext = QFileInfo(filePath).suffix().toLower();
    QStringList names;
    for (const Tool &t : tools())
        if (t.extensions.contains(ext) && !names.contains(t.program))
            names << t.program;
    return names.join(QStringLiteral(", "));
}

Result format(const QString &filePath, const QString &text, int timeoutMs)
{
    Result r;
    QString exe;
    const Tool *tool = find(filePath, &exe);
    if (!tool) {
        const QString hint = suggestedTools(filePath);
        r.error = hint.isEmpty() ? QObject::tr("There is no formatter for this file type.")
                                 : QObject::tr("No formatter installed for this file type (install %1).").arg(hint);
        return r;
    }
    r.tool = tool->program;
    QProcess p;
    p.setWorkingDirectory(QFileInfo(filePath).absolutePath());
    p.start(exe, tool->args(filePath));
    if (!p.waitForStarted(3000)) {
        r.error = QObject::tr("Could not start %1.").arg(tool->program);
        return r;
    }
    p.write(text.toUtf8());
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        r.error = QObject::tr("%1 took too long.").arg(tool->program);
        return r;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        r.error = QObject::tr("%1 failed: %2").arg(tool->program, QString::fromUtf8(p.readAllStandardError()).trimmed().section(QLatin1Char('\n'), 0, 0));
        return r;
    }
    QByteArray out = p.readAllStandardOutput();
    if (out.isEmpty() && !text.trimmed().isEmpty()) {
        r.error = QObject::tr("%1 produced no output.").arg(tool->program);
        return r;
    }
    r.text = QString::fromUtf8(out);
    r.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    r.ok = true;
    return r;
}

} // namespace Formatter
