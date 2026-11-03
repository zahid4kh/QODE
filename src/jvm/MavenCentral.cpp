#include "jvm/MavenCentral.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

QNetworkRequest request(const QUrl &url, int timeoutMs = 20000)
{
    QNetworkRequest r(url);
    r.setTransferTimeout(timeoutMs);
    r.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QODE/%1").arg(QStringLiteral(QODE_VERSION)));
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return r;
}

QString errorOf(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 404)
        return QObject::tr("not found on Maven Central");
    return status > 0 ? QObject::tr("Maven Central answered HTTP %1").arg(status) : reply->errorString();
}

} // namespace

MavenCentral::MavenCentral(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

void MavenCentral::search(const QString &query, std::function<void(const QList<Artifact> &, const QString &)> done)
{
    QString q = query.trimmed();
    if (q.count(QLatin1Char(':')) >= 1) {
        const QString g = q.section(QLatin1Char(':'), 0, 0).trimmed(), a = q.section(QLatin1Char(':'), 1, 1).trimmed();
        QStringList parts;
        if (!g.isEmpty())
            parts << QStringLiteral("g:%1*").arg(g);
        if (!a.isEmpty())
            parts << QStringLiteral("a:%1*").arg(a);
        q = parts.join(QStringLiteral(" AND "));
    }
    QUrl url(QStringLiteral("https://search.maven.org/solrsearch/select"));
    QUrlQuery uq;
    uq.addQueryItem(QStringLiteral("q"), q);
    uq.addQueryItem(QStringLiteral("rows"), QStringLiteral("40"));
    uq.addQueryItem(QStringLiteral("wt"), QStringLiteral("json"));
    url.setQuery(uq);
    fetchSearch(url, query, 0, std::move(done));
}

// search.maven.org answers most queries in well under a second but now and then takes half a minute: a short timeout
// and a couple of retries get a result sooner than one long wait.
void MavenCentral::fetchSearch(const QUrl &url, const QString &query, int attempt,
                               std::function<void(const QList<Artifact> &, const QString &)> done)
{
    QNetworkReply *reply = m_net->get(request(url, 8000));
    connect(reply, &QNetworkReply::finished, this, [this, reply, done, query, url, attempt] {
        reply->deleteLater();
        const auto e = reply->error();
        if ((e == QNetworkReply::OperationCanceledError || e == QNetworkReply::TimeoutError) && attempt < 2) {
            fetchSearch(url, query, attempt + 1, done);
            return;
        }
        if (e != QNetworkReply::NoError) {
            done({}, errorOf(reply));
            return;
        }
        QList<Artifact> out;
        const QJsonArray docs = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("response")).toObject()
                                    .value(QStringLiteral("docs")).toArray();
        for (const QJsonValue &v : docs) {
            const QJsonObject o = v.toObject();
            Artifact a;
            a.group = o.value(QStringLiteral("g")).toString();
            a.artifact = o.value(QStringLiteral("a")).toString();
            a.latest = o.value(QStringLiteral("latestVersion")).toString();
            a.versionCount = o.value(QStringLiteral("versionCount")).toInt();
            if (!a.group.isEmpty() && !a.artifact.isEmpty())
                out << a;
        }
        // The search's own order, except that artifacts named exactly like a word of the query come first, those whose
        // group also names it before the others (gson -> com.google.code.gson:gson before the many forks named gson).
        const QStringList words = query.toLower().split(QRegularExpression(QStringLiteral("[\\s:]+")), Qt::SkipEmptyParts);
        auto rank = [&words](const Artifact &a) {
            const QString name = a.artifact.toLower();
            if (!words.contains(name))
                return 2;
            return a.group.toLower().contains(name) ? 0 : 1;
        };
        std::stable_sort(out.begin(), out.end(), [&rank](const Artifact &x, const Artifact &y) { return rank(x) < rank(y); });
        done(out, QString());
    });
}

void MavenCentral::versions(const QString &group, const QString &artifact, std::function<void(const QStringList &, const QString &)> done)
{
    QString path = group;
    path.replace(QLatin1Char('.'), QLatin1Char('/'));
    const QUrl url(QStringLiteral("https://repo1.maven.org/maven2/%1/%2/maven-metadata.xml").arg(path, artifact));
    QNetworkReply *reply = m_net->get(request(url));
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, errorOf(reply));
            return;
        }
        QStringList out;
        QXmlStreamReader xml(reply->readAll());
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("version"))
                out << xml.readElementText().trimmed();
        }
        out.removeAll(QString());
        std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) { return compareVersions(a, b) > 0; });
        out.removeDuplicates();
        done(out, out.isEmpty() ? QObject::tr("no versions listed") : QString());
    });
}

bool MavenCentral::isStable(const QString &version)
{
    static const QRegularExpression pre(QStringLiteral(R"((?:alpha|beta|\brc\b|[.-]rc\d|[.-]cr\d|[.-]m\d|milestone|preview|eap|[.-]dev|snapshot|[.-]ea\b|-b\d))"),
                                        QRegularExpression::CaseInsensitiveOption);
    return !pre.match(version).hasMatch();
}

int MavenCentral::compareVersions(const QString &a, const QString &b)
{
    static const QRegularExpression split(QStringLiteral("[.\\-+_]"));
    const QStringList pa = a.split(split, Qt::SkipEmptyParts), pb = b.split(split, Qt::SkipEmptyParts);
    for (int i = 0; i < qMax(pa.size(), pb.size()); ++i) {
        const QString x = pa.value(i), y = pb.value(i);
        bool nx = false, ny = false;
        const qlonglong ix = x.toLongLong(&nx), iy = y.toLongLong(&ny);
        if (nx && ny) {
            if (ix != iy)
                return ix < iy ? -1 : 1;
            continue;
        }
        // A missing part ranks above a qualifier (1.0 > 1.0-RC1) and below a number (1.0 < 1.0.1).
        if (x.isEmpty())
            return ny ? -1 : 1;
        if (y.isEmpty())
            return nx ? 1 : -1;
        if (nx != ny)
            return nx ? 1 : -1;
        if (const int c = x.compare(y, Qt::CaseInsensitive); c != 0)
            return c < 0 ? -1 : 1;
    }
    return 0;
}

QString MavenCentral::newestFor(const QStringList &versions, const QString &current)
{
    const bool wantStable = current.isEmpty() || isStable(current);
    for (const QString &v : versions)
        if (!wantStable || isStable(v))
            return v;
    return versions.value(0);
}
