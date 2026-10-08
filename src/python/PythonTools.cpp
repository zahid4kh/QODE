#include "PythonTools.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QRegularExpression>

#include "lsp/LspServers.h"

namespace PythonTools {

QStringList environment()
{
    return {QStringLiteral("PIP_DISABLE_PIP_VERSION_CHECK=1"), QStringLiteral("PIP_NO_INPUT=1"), QStringLiteral("PYTHONUNBUFFERED=1"),
            QStringLiteral("UV_NO_PROGRESS=1")};
}

QString displayOf(const QString &program, const QStringList &arguments)
{
    const QString home = QDir::homePath();
    auto shorten = [&home](QString s) {
        if (s.startsWith(home + QLatin1Char('/')))
            s = QStringLiteral("~") + s.mid(home.size());
        return s.contains(QLatin1Char(' ')) ? QStringLiteral("\"%1\"").arg(s) : s;
    };
    QStringList parts{QFileInfo(program).fileName() == QLatin1String("uv") ? QStringLiteral("uv") : shorten(program)};
    for (const QString &a : arguments)
        if (a != QLatin1String("--")) // the guard against option-like names is not worth showing
            parts << shorten(a);
    return parts.join(QLatin1Char(' '));
}

static Command make(const QString &program, const QStringList &args)
{
    return {program, args, displayOf(program, args)};
}

Command createVenv(const QString &dir, bool useUv, const QString &basePython)
{
    if (useUv)
        return make(LspServers::uvExecutable(), {QStringLiteral("venv"), dir});
    return make(basePython, {QStringLiteral("-m"), QStringLiteral("venv"), dir});
}

// "--" keeps a spec that starts with a dash from being read as an option.
Command install(const QString &venvPython, const QStringList &specs, bool upgrade, bool useUv)
{
    QStringList a{QStringLiteral("install")};
    if (useUv)
        a = QStringList{QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), venvPython};
    else
        a = QStringList{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install")};
    if (upgrade)
        a << QStringLiteral("--upgrade");
    a << QStringLiteral("--") << specs;
    return make(useUv ? LspServers::uvExecutable() : venvPython, a);
}

Command installRequirements(const QString &venvPython, const QString &file, bool useUv)
{
    if (useUv)
        return make(LspServers::uvExecutable(), {QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), venvPython,
                                                 QStringLiteral("-r"), file});
    return make(venvPython, {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("-r"), file});
}

Command installProject(const QString &venvPython, const QString &projectDir, bool useUv)
{
    if (useUv)
        return make(LspServers::uvExecutable(), {QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), venvPython,
                                                 QStringLiteral("-e"), projectDir});
    return make(venvPython, {QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("-e"), projectDir});
}

Command uninstall(const QString &venvPython, const QStringList &names, bool useUv)
{
    if (useUv)
        return make(LspServers::uvExecutable(), QStringList{QStringLiteral("pip"), QStringLiteral("uninstall"), QStringLiteral("--python"), venvPython,
                                                           QStringLiteral("--")} + names);
    return make(venvPython, QStringList{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("uninstall"), QStringLiteral("-y"),
                                        QStringLiteral("--")} + names);
}

Command list(const QString &venvPython, bool outdated, bool useUv)
{
    if (useUv) {
        QStringList a{QStringLiteral("pip"), QStringLiteral("list"), QStringLiteral("--python"), venvPython, QStringLiteral("--format"),
                      QStringLiteral("json")};
        if (outdated)
            a << QStringLiteral("--outdated");
        return make(LspServers::uvExecutable(), a);
    }
    QStringList a{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("list"), QStringLiteral("--format=json")};
    if (outdated)
        a << QStringLiteral("--outdated");
    return make(venvPython, a);
}

QList<Package> parseList(const QByteArray &json)
{
    QList<Package> out;
    // pip may print notices before the array; start at the first '['.
    const int start = json.indexOf('[');
    if (start < 0)
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(json.mid(start));
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("name")).toString().isEmpty())
            continue;
        out.append({o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("version")).toString(),
                    o.value(QStringLiteral("latest_version")).toString()});
    }
    return out;
}

QStringList splitRequirements(const QString &text, QString *error)
{
    // Whitespace separates packages, except that "django >= 5" / "django>= 5" stay one requirement.
    QStringList out;
    bool joinNext = false;
    for (const QString &token : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        static const QRegularExpression opStart(QStringLiteral("^[<>=!~]"));
        static const QRegularExpression opEnd(QStringLiteral("[<>=!~,]$"));
        if (!out.isEmpty() && (joinNext || opStart.match(token).hasMatch()))
            out.last() += token;
        else
            out << token;
        joinNext = opEnd.match(token).hasMatch();
    }
    static const QRegularExpression named(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*(\\[[A-Za-z0-9._,-]+\\])?([<>=!~][^\\s]*)?$"));
    static const QRegularExpression url(QStringLiteral("^([A-Za-z0-9._-]+\\+)?[A-Za-z][A-Za-z0-9+.-]*://\\S+$|^[A-Za-z0-9][A-Za-z0-9._-]*@[A-Za-z]+://\\S+$"));
    for (const QString &spec : std::as_const(out))
        if (!named.match(spec).hasMatch() && !url.match(spec).hasMatch()) {
            if (error)
                *error = QObject::tr("\"%1\" is not a package name. Use names such as requests, django>=5 or httpx[http2]; "
                                     "put several packages side by side, separated by spaces.")
                             .arg(spec);
            return {};
        }
    return out;
}

QString packageForModule(const QString &module)
{
    static const QHash<QString, QString> known = {
        {QStringLiteral("PIL"), QStringLiteral("pillow")},          {QStringLiteral("cv2"), QStringLiteral("opencv-python")},
        {QStringLiteral("yaml"), QStringLiteral("pyyaml")},         {QStringLiteral("sklearn"), QStringLiteral("scikit-learn")},
        {QStringLiteral("skimage"), QStringLiteral("scikit-image")}, {QStringLiteral("bs4"), QStringLiteral("beautifulsoup4")},
        {QStringLiteral("dateutil"), QStringLiteral("python-dateutil")}, {QStringLiteral("dotenv"), QStringLiteral("python-dotenv")},
        {QStringLiteral("attr"), QStringLiteral("attrs")},          {QStringLiteral("serial"), QStringLiteral("pyserial")},
        {QStringLiteral("OpenSSL"), QStringLiteral("pyopenssl")},   {QStringLiteral("jwt"), QStringLiteral("pyjwt")},
        {QStringLiteral("Crypto"), QStringLiteral("pycryptodome")}, {QStringLiteral("git"), QStringLiteral("gitpython")},
        {QStringLiteral("MySQLdb"), QStringLiteral("mysqlclient")}, {QStringLiteral("psycopg2"), QStringLiteral("psycopg2-binary")},
        {QStringLiteral("magic"), QStringLiteral("python-magic")},  {QStringLiteral("docx"), QStringLiteral("python-docx")},
        {QStringLiteral("pptx"), QStringLiteral("python-pptx")},    {QStringLiteral("fitz"), QStringLiteral("pymupdf")},
        {QStringLiteral("usb"), QStringLiteral("pyusb")},           {QStringLiteral("zmq"), QStringLiteral("pyzmq")},
        {QStringLiteral("google"), QStringLiteral("protobuf")},     {QStringLiteral("ruamel"), QStringLiteral("ruamel.yaml")},
        {QStringLiteral("gi"), QStringLiteral("pygobject")},        {QStringLiteral("wx"), QStringLiteral("wxpython")},
        {QStringLiteral("PyQt6"), QStringLiteral("pyqt6")},         {QStringLiteral("PySide6"), QStringLiteral("pyside6")},
        {QStringLiteral("lxml"), QStringLiteral("lxml")},           {QStringLiteral("Xlib"), QStringLiteral("python-xlib")},
        {QStringLiteral("jose"), QStringLiteral("python-jose")},    {QStringLiteral("slugify"), QStringLiteral("python-slugify")},
        {QStringLiteral("telegram"), QStringLiteral("python-telegram-bot")}, {QStringLiteral("discord"), QStringLiteral("discord.py")},
    };
    const QString top = module.section(QLatin1Char('.'), 0, 0);
    return known.value(top, top);
}

} // namespace PythonTools
