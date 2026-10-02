#pragma once

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

    static WebProject detect(const QString &root);
    // `bun dev`, `npm run dev`, `pnpm dev`, ...; a non-zero port adds the port flag.
    QString command(const QString &manager, const QString &script, int port) const;
    static QStringList managers() { return {QStringLiteral("npm"), QStringLiteral("pnpm"), QStringLiteral("yarn"), QStringLiteral("bun")}; }
};
