#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

// What QODE knows about a JavaScript / TypeScript project's dev server, read from package.json and the lockfile.
struct WebProject
{
    bool valid = false;        // there is a package.json with something that starts a server
    QString packageManager;    // bun | pnpm | yarn | npm
    QStringList scripts;       // candidate server scripts found in package.json (dev, start, ...), best first
    QString framework;         // "Next.js", "Vite", ... or empty
    int defaultPort = 0;       // the framework's usual port, 0 when unknown
    QString portFlag;          // flag that sets the port ("--port"), empty when only $PORT works
    QMap<QString, int> scriptPorts; // scripts that hardcode their port ("next dev -p 4000"): QODE must not override it
    int configPort = 0;        // port set in the framework's config file or .env (informational)
    QString configPortSource;  // the file configPort came from

    // `dir` is the folder holding package.json (the project root, or a nested folder such as docs/).
    static WebProject detect(const QString &dir);
    // Folders below `root` (at most two levels, relative paths) that hold a web project, shallowest first.
    static QStringList findNested(const QString &root);
    // The port a script is pinned to, 0 when it takes whatever is given.
    int fixedPort(const QString &script) const { return scriptPorts.value(script); }
    // `bun dev`, `npm run dev`, `pnpm dev`, ...; a non-zero port adds the port flag.
    QString command(const QString &manager, const QString &script, int port) const;
    static QStringList managers() { return {QStringLiteral("npm"), QStringLiteral("pnpm"), QStringLiteral("yarn"), QStringLiteral("bun")}; }
};
