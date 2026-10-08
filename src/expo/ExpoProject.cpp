#include "ExpoProject.h"

#include "JsonDoc.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace {

QJsonObject readObject(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024)
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool dependsOnExpo(const QJsonObject &pkg)
{
    return pkg.value(QStringLiteral("dependencies")).toObject().contains(QStringLiteral("expo")) ||
           pkg.value(QStringLiteral("devDependencies")).toObject().contains(QStringLiteral("expo"));
}

// "54.0.0" from an installed version ("54.0.33") or a range ("~54.0.33", "^52.0.0", "54.x").
QString sdkFromVersion(const QString &version)
{
    static const QRegularExpression major(QStringLiteral("(\\d+)"));
    const auto m = major.match(version);
    return m.hasMatch() ? m.captured(1) + QStringLiteral(".0.0") : QString();
}

// The installed expo package wins over the declared range; a monorepo hoists it into a parent node_modules.
QString sdkFor(const QString &dir, const QString &root, const QJsonObject &pkg)
{
    QDir d(dir);
    for (int up = 0; up < 6; ++up) {
        const QString version = readObject(d.filePath(QStringLiteral("node_modules/expo/package.json"))).value(QStringLiteral("version")).toString();
        if (!version.isEmpty())
            return sdkFromVersion(version);
        if (d.absolutePath() == QDir(root).absolutePath() || !d.cdUp())
            break;
    }
    QString range = pkg.value(QStringLiteral("dependencies")).toObject().value(QStringLiteral("expo")).toString();
    if (range.isEmpty())
        range = pkg.value(QStringLiteral("devDependencies")).toObject().value(QStringLiteral("expo")).toString();
    return sdkFromVersion(range);
}

} // namespace

namespace ExpoProject {

QList<ExpoApp> detect(const QString &root)
{
    QList<ExpoApp> apps;
    if (root.isEmpty())
        return apps;
    static const QSet<QString> skip = {QStringLiteral("node_modules"), QStringLiteral("ios"),    QStringLiteral("android"),
                                       QStringLiteral("dist"),         QStringLiteral("build"),  QStringLiteral("out"),
                                       QStringLiteral("target"),       QStringLiteral("vendor"), QStringLiteral("venv"),
                                       QStringLiteral("__pycache__"),  QStringLiteral("Pods"),   QStringLiteral("web-build")};
    const QDir base(root);
    int visited = 0;

    auto check = [&](const QString &dir) {
        const QJsonObject pkg = readObject(dir + QStringLiteral("/package.json"));
        if (pkg.isEmpty() || !dependsOnExpo(pkg))
            return false;
        ExpoApp app;
        app.dir = QDir::cleanPath(dir);
        app.rel = base.relativeFilePath(app.dir);
        if (app.rel == QLatin1String("."))
            app.rel.clear();
        for (const char *f : {"app.json", "app.config.json"})
            if (app.appJson.isEmpty() && QFileInfo::exists(dir + QLatin1Char('/') + QLatin1String(f)))
                app.appJson = dir + QLatin1Char('/') + QLatin1String(f);
        for (const char *f : {"app.config.ts", "app.config.js"})
            if (app.dynamicConfig.isEmpty() && QFileInfo::exists(dir + QLatin1Char('/') + QLatin1String(f)))
                app.dynamicConfig = dir + QLatin1Char('/') + QLatin1String(f);
        QString name;
        if (!app.appJson.isEmpty()) {
            const QJsonObject cfg = readObject(app.appJson);
            name = cfg.value(QStringLiteral("expo")).toObject().value(QStringLiteral("name")).toString();
            if (name.isEmpty())
                name = cfg.value(QStringLiteral("name")).toString();
        }
        if (name.isEmpty())
            name = pkg.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            name = QFileInfo(dir).fileName();
        app.name = name;
        app.sdk = sdkFor(dir, root, pkg);
        apps.append(app);
        return true;
    };

    // Breadth first, so shallow apps come first; an app's own subfolders are not searched.
    QList<QPair<QString, int>> queue{{root, 0}};
    for (int q = 0; q < queue.size() && visited < 3000; ++q) {
        const QString dir = queue.at(q).first;
        const int depth = queue.at(q).second;
        ++visited;
        if (check(dir) || depth >= 3)
            continue;
        const QStringList names = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &n : names)
            if (!n.startsWith(QLatin1Char('.')) && !skip.contains(n))
                queue.append({dir + QLatin1Char('/') + n, depth + 1});
    }
    return apps;
}

} // namespace ExpoProject
