#include "WebProject.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

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

WebProject WebProject::detect(const QString &root)
{
    WebProject w;
    QFile f(root + QStringLiteral("/package.json"));
    if (root.isEmpty() || !f.open(QIODevice::ReadOnly))
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

    // Package manager: the lockfile tells which one installed the dependencies; "packageManager" (corepack) is the fallback.
    const QString dir = root + QLatin1Char('/');
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
    return w;
}

QString WebProject::command(const QString &manager, const QString &script, int port) const
{
    QString cmd = manager == QLatin1String("npm") ? QStringLiteral("npm run ") + script : manager + QLatin1Char(' ') + script;
    if (port > 0 && !portFlag.isEmpty()) {
        // npm needs "--" to pass flags on to the script; the others forward them.
        cmd += (manager == QLatin1String("npm") ? QStringLiteral(" -- ") : QStringLiteral(" ")) + portFlag + QLatin1Char(' ') +
               QString::number(port);
    }
    return cmd;
}
