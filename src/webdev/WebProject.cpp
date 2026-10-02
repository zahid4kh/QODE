#include "WebProject.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace {

struct Framework
{
    const char *dependency;
    const char *name;
    int port;
    const char *flag; // nullptr: the tool only reads $PORT
};

// Order matters: meta-frameworks before the bundlers they sit on (Astro, SvelteKit and Nuxt all depend on Vite).
const Framework kFrameworks[] = {
    {"next", "Next.js", 3000, "--port"},
    {"nuxt", "Nuxt", 3000, "--port"},
    {"@sveltejs/kit", "SvelteKit", 5173, "--port"},
    {"astro", "Astro", 4321, "--port"},
    {"@remix-run/dev", "Remix", 3000, "--port"},
    {"react-router", "React Router", 5173, "--port"},
    {"gatsby", "Gatsby", 8000, "-p"},
    {"@angular/cli", "Angular", 4200, "--port"},
    {"vite", "Vite", 5173, "--port"},
    {"react-scripts", "Create React App", 3000, nullptr},
    {"@vue/cli-service", "Vue CLI", 8080, "--port"},
    {"webpack-dev-server", "webpack", 8080, "--port"},
    {"parcel", "Parcel", 1234, "--port"},
    {"expo", "Expo", 8081, "--port"},
};

} // namespace

WebProject WebProject::detect(const QString &folder)
{
    WebProject w;
    const QString dir = folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/');
    QFile f(dir + QStringLiteral("package.json"));
    if (folder.isEmpty() || !f.open(QIODevice::ReadOnly))
        return w;
    const QJsonObject pkg = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject scripts = pkg.value(QStringLiteral("scripts")).toObject();
    for (const QString &name : {QStringLiteral("dev"), QStringLiteral("develop"), QStringLiteral("start"), QStringLiteral("serve"),
                                QStringLiteral("preview")})
        if (scripts.contains(name))
            w.scripts << name;
    if (w.scripts.isEmpty())
        return w;
    w.valid = true;
    // A script that already names its port ("--port 4000", "-p 4000", "PORT=4000 ...") keeps it.
    static const QRegularExpression pinned(QStringLiteral(R"((?:(?:^|\s)(?:--port|-p)(?:=|\s+)|\bPORT=)(\d{2,5})\b)"));
    for (auto it = scripts.begin(); it != scripts.end(); ++it) {
        const QRegularExpressionMatch m = pinned.match(it.value().toString());
        if (m.hasMatch())
            w.scriptPorts.insert(it.key(), m.captured(1).toInt());
    }

    // Package manager: the lockfile tells which one installed the dependencies; "packageManager" (corepack) is the fallback.
    auto has = [&dir](const char *file) { return QFileInfo::exists(dir + QLatin1String(file)); };
    if (has("bun.lock") || has("bun.lockb"))
        w.packageManager = QStringLiteral("bun");
    else if (has("pnpm-lock.yaml"))
        w.packageManager = QStringLiteral("pnpm");
    else if (has("yarn.lock"))
        w.packageManager = QStringLiteral("yarn");
    else if (has("package-lock.json") || has("npm-shrinkwrap.json"))
        w.packageManager = QStringLiteral("npm");
    else {
        const QString declared = pkg.value(QStringLiteral("packageManager")).toString().section(QLatin1Char('@'), 0, 0);
        w.packageManager = managers().contains(declared) ? declared : QStringLiteral("npm");
    }

    QStringList deps = pkg.value(QStringLiteral("dependencies")).toObject().keys();
    deps << pkg.value(QStringLiteral("devDependencies")).toObject().keys();
    for (const Framework &fw : kFrameworks)
        if (deps.contains(QLatin1String(fw.dependency))) {
            w.framework = QLatin1String(fw.name);
            w.defaultPort = fw.port;
            w.portFlag = fw.flag ? QLatin1String(fw.flag) : QString();
            break;
        }

    // A port written into the config or .env is only reported; the framework keeps using it.
    static const QRegularExpression inConfig(QStringLiteral(R"(\bport\s*:\s*(\d{2,5})\b)"));
    static const QRegularExpression inEnv(QStringLiteral(R"(^\s*(?:export\s+)?PORT\s*=\s*['"]?(\d{2,5}))"),
                                          QRegularExpression::MultilineOption);
    for (const char *name : {"vite.config.ts", "vite.config.js", "vite.config.mjs", "vite.config.mts", "nuxt.config.ts", "nuxt.config.js",
                             "astro.config.mjs", "astro.config.ts", "svelte.config.js", "webpack.config.js", "angular.json"}) {
        QFile cf(dir + QLatin1String(name));
        if (!cf.open(QIODevice::ReadOnly))
            continue;
        const QRegularExpressionMatch m = inConfig.match(QString::fromUtf8(cf.read(200000)));
        if (m.hasMatch()) {
            w.configPort = m.captured(1).toInt();
            w.configPortSource = QLatin1String(name);
            break;
        }
    }
    if (!w.configPort)
        for (const char *name : {".env.local", ".env.development", ".env"}) {
            QFile ef(dir + QLatin1String(name));
            if (!ef.open(QIODevice::ReadOnly))
                continue;
            const QRegularExpressionMatch m = inEnv.match(QString::fromUtf8(ef.readAll()));
            if (m.hasMatch()) {
                w.configPort = m.captured(1).toInt();
                w.configPortSource = QLatin1String(name);
                break;
            }
        }
    return w;
}

QStringList WebProject::findNested(const QString &root)
{
    static const QSet<QString> skip = {QStringLiteral("node_modules"), QStringLiteral("dist"),   QStringLiteral("build"),
                                       QStringLiteral("out"),          QStringLiteral("target"), QStringLiteral("vendor"),
                                       QStringLiteral("venv"),         QStringLiteral("__pycache__")};
    auto children = [&](const QString &rel) {
        QStringList out;
        const QStringList names = QDir(rel.isEmpty() ? root : root + QLatin1Char('/') + rel)
                                      .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &n : names)
            if (!n.startsWith(QLatin1Char('.')) && !skip.contains(n))
                out << (rel.isEmpty() ? n : rel + QLatin1Char('/') + n);
        return out;
    };
    QStringList level1 = children(QString()), found, level2;
    for (const QString &rel : std::as_const(level1))
        if (detect(root + QLatin1Char('/') + rel + QLatin1Char('/')).valid)
            found << rel;
        else
            level2 << children(rel);
    for (const QString &rel : std::as_const(level2))
        if (found.size() < 12 && detect(root + QLatin1Char('/') + rel + QLatin1Char('/')).valid)
            found << rel;
    return found;
}

QString WebProject::command(const QString &manager, const QString &script, int port) const
{
    QString cmd = manager == QLatin1String("npm") ? QStringLiteral("npm run ") + script : manager + QLatin1Char(' ') + script;
    if (port > 0 && !portFlag.isEmpty() && !fixedPort(script)) {
        // npm needs "--" to pass flags on to the script; the others forward them.
        cmd += (manager == QLatin1String("npm") ? QStringLiteral(" -- ") : QStringLiteral(" ")) + portFlag + QLatin1Char(' ') +
               QString::number(port);
    }
    return cmd;
}
