#include "ExpoRoutes.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>

namespace {

QJsonObject readObject(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024)
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

// ["expo-router", {"root": "src/screens"}] in app.json's plugins.
QString pluginRoot(const QString &appDir)
{
    for (const char *name : {"app.json", "app.config.json"}) {
        QJsonObject cfg = readObject(appDir + QLatin1Char('/') + QLatin1String(name));
        if (cfg.contains(QStringLiteral("expo")))
            cfg = cfg.value(QStringLiteral("expo")).toObject();
        for (const QJsonValue &p : cfg.value(QStringLiteral("plugins")).toArray()) {
            const QJsonArray entry = p.toArray();
            if (entry.size() >= 2 && entry.at(0).toString() == QLatin1String("expo-router"))
                return entry.at(1).toObject().value(QStringLiteral("root")).toString();
        }
    }
    return {};
}

} // namespace

namespace ExpoRoutes {

QString routesDir(const QString &appDir)
{
    const QJsonObject pkg = readObject(appDir + QStringLiteral("/package.json"));
    if (!pkg.value(QStringLiteral("dependencies")).toObject().contains(QStringLiteral("expo-router")) &&
        !pkg.value(QStringLiteral("devDependencies")).toObject().contains(QStringLiteral("expo-router")))
        return {};
    QString root = pluginRoot(appDir);
    if (root.startsWith(QLatin1String("./")))
        root = root.mid(2);
    if (!root.isEmpty() && QFileInfo(appDir + QLatin1Char('/') + root).isDir())
        return QDir::cleanPath(root);
    // As Expo Router decides: src/app wins over app.
    if (QFileInfo(appDir + QStringLiteral("/src/app")).isDir())
        return QStringLiteral("src/app");
    if (QFileInfo(appDir + QStringLiteral("/app")).isDir())
        return QStringLiteral("app");
    return {};
}

QList<ExpoRoute> scan(const QString &appDir)
{
    const QString rel = routesDir(appDir);
    if (rel.isEmpty())
        return {};
    const QDir root(appDir + QLatin1Char('/') + rel);
    static const QRegularExpression source(QStringLiteral(R"(\.(tsx|ts|jsx|js)$)"));
    static const QRegularExpression platform(QStringLiteral(R"(\.(ios|android|native|web)$)"));
    QMap<QString, ExpoRoute> routes; // by path, sorted
    QDirIterator it(root.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
    int seen = 0;
    while (it.hasNext() && ++seen < 5000) {
        const QString file = root.relativeFilePath(it.next());
        if (file.contains(QLatin1String("node_modules")) || file.endsWith(QLatin1String(".d.ts")))
            continue;
        QString name = file;
        const QRegularExpressionMatch ext = source.match(name);
        if (!ext.hasMatch())
            continue;
        name.chop(ext.capturedLength());
        const QRegularExpressionMatch os = platform.match(name);
        if (os.hasMatch()) {
            if (os.captured(1) != QLatin1String("web") && os.captured(1) != QLatin1String("native"))
                continue; // .ios / .android sit next to the file the web uses
            name.chop(os.capturedLength());
        }
        const QStringList parts = name.split(QLatin1Char('/'));
        QStringList segments, params;
        bool skip = false;
        for (int i = 0; i < parts.size() && !skip; ++i) {
            const QString &p = parts.at(i);
            const bool last = i == parts.size() - 1;
            if (p.startsWith(QLatin1Char('_')) || p.startsWith(QLatin1Char('+')) || p.endsWith(QLatin1String("+api")) || p.startsWith(QLatin1Char('.')))
                skip = true; // _layout, +html, +not-found, an API route, a hidden folder
            else if (p.startsWith(QLatin1Char('(')) && p.endsWith(QLatin1Char(')')))
                continue; // a group: not part of the URL
            else if (last && p == QLatin1String("index"))
                continue;
            else {
                if (p.startsWith(QLatin1Char('[')) && p.endsWith(QLatin1Char(']')))
                    params << p.mid(1, p.size() - 2);
                segments << p;
            }
        }
        if (skip)
            continue;
        ExpoRoute r;
        r.path = QLatin1Char('/') + segments.join(QLatin1Char('/'));
        r.file = rel + QLatin1Char('/') + file;
        r.params = params;
        // "(tabs)/index" and "index" are both "/": the first found is as good as any, but a .web file wins.
        if (!routes.contains(r.path) || (os.hasMatch() && os.captured(1) == QLatin1String("web")))
            routes.insert(r.path, r);
    }
    return routes.values();
}

} // namespace ExpoRoutes
