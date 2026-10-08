#include "ExpoSchema.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>
#include <algorithm>

namespace {
constexpr qint64 kStaleDays = 30;

QVersionNumber versionOfFile(const QString &name)
{
    return QVersionNumber::fromString(name.mid(7, name.size() - 7 - 5)); // "schema-54.0.0.json"
}
} // namespace

ExpoSchema::ExpoSchema(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString ExpoSchema::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/QODE/expo");
}

QString ExpoSchema::pathFor(const QString &sdk)
{
    return cacheDir() + QStringLiteral("/schema-") + sdk + QStringLiteral(".json");
}

QString ExpoSchema::url(const QString &sdk)
{
    return QStringLiteral("https://exp.host/--/api/v2/project/configuration/schema/") + sdk;
}

QStringList ExpoSchema::cachedSdks()
{
    QStringList files = QDir(cacheDir()).entryList({QStringLiteral("schema-*.json")}, QDir::Files);
    std::sort(files.begin(), files.end(), [](const QString &a, const QString &b) { return versionOfFile(a) > versionOfFile(b); });
    QStringList out;
    for (const QString &f : std::as_const(files))
        out << f.mid(7, f.size() - 12);
    return out;
}

QJsonObject ExpoSchema::load(const QString &sdk)
{
    QFile f(pathFor(sdk));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

qint64 ExpoSchema::cacheSize()
{
    qint64 total = 0;
    QDirIterator it(cacheDir(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

bool ExpoSchema::clearCache()
{
    const QString dir = cacheDir();
    return !QFileInfo::exists(dir) || QDir(dir).removeRecursively();
}

void ExpoSchema::request(const QString &sdk)
{
    QString want = sdk;
    if (want.isEmpty()) {
        const QStringList have = cachedSdks();
        if (have.isEmpty()) {
            emit failed(sdk, tr("The Expo SDK version is unknown (is expo installed?)"));
            return;
        }
        want = have.first();
    }
    const QFileInfo cached(pathFor(want));
    const bool stale = cached.exists() && cached.lastModified().daysTo(QDateTime::currentDateTime()) > kStaleDays;
    if (cached.exists()) {
        emit ready(want);
        if (!stale)
            return; // an old copy is refreshed quietly below
    }
    if (m_pending == want)
        return;
    m_pending = want;
    QNetworkRequest req{QUrl(url(want))};
    req.setTransferTimeout(20000);
    req.setRawHeader("Accept", "application/json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, want, hadCache = cached.exists()] {
        reply->deleteLater();
        m_pending.clear();
        auto giveUp = [&](const QString &why) {
            if (hadCache)
                return; // keep the old copy
            const QStringList have = cachedSdks();
            if (!have.isEmpty())
                emit ready(have.first()); // another SDK's schema is better than none
            else
                emit failed(want, why);
        };
        if (reply->error() != QNetworkReply::NoError) {
            giveUp(reply->error() == QNetworkReply::ContentNotFoundError ? tr("Expo has no schema for SDK %1").arg(want)
                                                                         : tr("Could not download the Expo schema: %1").arg(reply->errorString()));
            return;
        }
        const QJsonObject inner = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("schema")).toObject();
        if (inner.isEmpty() || !inner.contains(QStringLiteral("properties"))) {
            giveUp(tr("The Expo schema answer was not understood"));
            return;
        }
        // The published schema describes the contents of the "expo" key; app.json holds {"expo": {...}}.
        QJsonObject expo{{QStringLiteral("type"), QStringLiteral("object")},
                         {QStringLiteral("description"), tr("Expo app configuration")},
                         {QStringLiteral("properties"), inner.value(QStringLiteral("properties"))},
                         {QStringLiteral("required"), inner.value(QStringLiteral("required"))},
                         {QStringLiteral("additionalProperties"), inner.value(QStringLiteral("additionalProperties"))}};
        const QJsonObject wrapped{{QStringLiteral("$schema"), QStringLiteral("http://json-schema.org/draft-07/schema#")},
                                  {QStringLiteral("title"), tr("Expo app.json (SDK %1)").arg(want)},
                                  {QStringLiteral("type"), QStringLiteral("object")},
                                  {QStringLiteral("definitions"), inner.value(QStringLiteral("definitions"))},
                                  {QStringLiteral("properties"), QJsonObject{{QStringLiteral("expo"), expo}}}};
        QDir().mkpath(cacheDir());
        QSaveFile out(pathFor(want));
        if (!out.open(QIODevice::WriteOnly)) {
            emit failed(want, tr("Could not write %1").arg(pathFor(want)));
            return;
        }
        out.write(QJsonDocument(wrapped).toJson(QJsonDocument::Compact));
        if (!out.commit()) {
            emit failed(want, tr("Could not write %1").arg(pathFor(want)));
            return;
        }
        if (!hadCache)
            emit ready(want);
    });
}

// Follows `path` through the schema (resolving "#/definitions/X" references and array items) and returns the
// "properties" object found there: {name: {description, type, enum...}}.
QJsonObject ExpoSchema::resolveRef(const QJsonObject &schema, QJsonObject node)
{
    const QJsonObject defs = schema.value(QStringLiteral("definitions")).toObject();
    for (int guard = 0; guard < 8 && node.contains(QStringLiteral("$ref")); ++guard) {
        const QString ref = node.value(QStringLiteral("$ref")).toString();
        node = defs.value(ref.mid(ref.lastIndexOf(QLatin1Char('/')) + 1)).toObject();
    }
    return node;
}

QJsonObject ExpoSchema::propertiesAt(const QJsonObject &schema, const QStringList &path)
{
    auto resolve = [&](const QJsonObject &node) { return resolveRef(schema, node); };
    QJsonObject node = resolve(schema.value(QStringLiteral("properties")).toObject().value(QStringLiteral("expo")).toObject());
    for (const QString &key : path) {
        if (key == QLatin1String("*")) { // an array element
            node = resolve(node.value(QStringLiteral("items")).toObject());
            continue;
        }
        node = resolve(node.value(QStringLiteral("properties")).toObject().value(key).toObject());
    }
    if (node.value(QStringLiteral("type")).toString() == QLatin1String("array"))
        node = resolve(node.value(QStringLiteral("items")).toObject());
    return node.value(QStringLiteral("properties")).toObject();
}
