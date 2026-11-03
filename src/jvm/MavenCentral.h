#pragma once

#include <QObject>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QUrl;

// Maven Central lookups for the dependency manager: artifact search (search.maven.org) and the versions of one artifact
// (its maven-metadata.xml on repo1.maven.org, which is current; the search index can lag a few days).
class MavenCentral : public QObject
{
    Q_OBJECT
public:
    struct Artifact {
        QString group, artifact, latest;
        int versionCount = 0;
        QString coordinate() const { return group + QLatin1Char(':') + artifact; }
    };

    explicit MavenCentral(QObject *parent = nullptr);

    // Free text ("gson", "ktor client") or "group:artifact" (either part may be partial). `done` gets an error or the hits.
    void search(const QString &query, std::function<void(const QList<Artifact> &, const QString &error)> done);
    // Every published version, newest first.
    void versions(const QString &group, const QString &artifact, std::function<void(const QStringList &, const QString &error)> done);

    static bool isStable(const QString &version);              // not alpha / beta / RC / milestone / snapshot / EAP / dev
    static int compareVersions(const QString &a, const QString &b); // <0, 0, >0 (numeric segments, then qualifiers)
    // The newest version to suggest: the newest stable one, or the newest of all when `current` is itself a pre-release.
    static QString newestFor(const QStringList &versions, const QString &current = QString());

private:
    void fetchSearch(const QUrl &url, const QString &query, int attempt, std::function<void(const QList<Artifact> &, const QString &error)> done);

    QNetworkAccessManager *m_net;
};
