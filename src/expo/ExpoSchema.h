#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// Expo publishes the JSON schema of app.json per SDK version (exp.host). It is downloaded once, wrapped to describe
// the {"expo": {...}} file and cached in ~/.cache/QODE/expo, then handed to the JSON language server for completion,
// hover docs and validation. Nothing is ever written into a project.
class ExpoSchema : public QObject
{
    Q_OBJECT
public:
    explicit ExpoSchema(QObject *parent = nullptr);

    // Makes sure the schema of `sdk` ("54.0.0") is cached: emits ready() at once when it is, else downloads it. An
    // unknown or unavailable SDK falls back to the newest cached schema.
    void request(const QString &sdk);

    static QString cacheDir();
    static QString pathFor(const QString &sdk);
    static QJsonObject load(const QString &sdk); // the cached, wrapped schema (empty when not cached)
    static qint64 cacheSize();
    static QStringList cachedSdks();             // newest first
    static bool clearCache();
    // The documentation of properties (for tooltips): the object `path` ("android", "android/adaptiveIcon") describes.
    static QJsonObject propertiesAt(const QJsonObject &schema, const QStringList &path);
    static QJsonObject resolveRef(const QJsonObject &schema, QJsonObject node); // follows "$ref": "#/definitions/X"
    static QString url(const QString &sdk);

signals:
    void ready(const QString &sdk);
    void failed(const QString &sdk, const QString &message);

private:
    QNetworkAccessManager *m_net;
    QString m_pending;
};
