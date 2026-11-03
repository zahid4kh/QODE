#include "http/HttpRunner.h"

#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {

constexpr qint64 kMaxBody = 64 * 1024 * 1024;

// The jar of a session; QNetworkCookieJar keeps allCookies() protected.
class SessionJar : public QNetworkCookieJar
{
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    int count() const { return allCookies().size(); }
    void clear() { setAllCookies({}); }
};

} // namespace

HttpRunner::HttpRunner(QObject *parent) : QObject(parent)
{
    m_net.setCookieJar(new SessionJar(&m_net));
}

void HttpRunner::send(const HttpRequestData &request, const HttpResult &base, Done done)
{
    abort();
    QNetworkRequest rq(request.url);
    rq.setTransferTimeout(request.timeoutMs);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QODE"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                    request.followRedirects ? QNetworkRequest::NoLessSafeRedirectPolicy : QNetworkRequest::ManualRedirectPolicy);
    if (!request.useCookies) {
        rq.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        rq.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    }
    for (const auto &h : request.headers)
        rq.setRawHeader(h.first, h.second);

    m_pending = base;
    m_pending.request = request;
    m_pending.time = QDateTime::currentDateTime();
    m_done = std::move(done);
    m_clock.start();
    m_aborted = false;
    const QByteArray body = request.method == QLatin1String("HEAD") ? QByteArray() : request.body;
    m_reply = m_net.sendCustomRequest(rq, request.method.toLatin1(), body);
    connect(m_reply, &QNetworkReply::finished, this, &HttpRunner::finished);
}

void HttpRunner::abort()
{
    if (!m_reply)
        return;
    m_aborted = true;
    m_reply->abort(); // finished() runs and reports the cancellation
}

void HttpRunner::clearCookies()
{
    static_cast<SessionJar *>(m_net.cookieJar())->clear();
}

int HttpRunner::cookieCount() const
{
    return static_cast<SessionJar *>(m_net.cookieJar())->count();
}

void HttpRunner::finished()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    reply->deleteLater();
    HttpResult r = m_pending;
    r.ms = m_clock.elapsed();
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    r.error = int(reply->error());
    if (reply->error() == QNetworkReply::OperationCanceledError && !m_aborted)
        r.error = int(QNetworkReply::TimeoutError); // the transfer timeout cancels the reply too
    if (r.status == 0) {
        r.errorText = reply->errorString();
    } else {
        r.body = reply->read(kMaxBody);
        r.headers = reply->rawHeaderPairs();
    }
    // Keep the request as it really went out (a redirect may have changed the URL).
    if (reply->url() != r.request.url)
        r.note = QObject::tr("Redirected to %1").arg(reply->url().toString());
    const Done done = std::move(m_done);
    m_done = nullptr;
    if (done)
        done(r);
}

QString HttpRunner::describeError(const HttpResult &r)
{
    const QUrl &u = r.request.url;
    switch (QNetworkReply::NetworkError(r.error)) {
    case QNetworkReply::ConnectionRefusedError:
        return tr("Connection refused: nothing listens on %1:%2.").arg(u.host().toHtmlEscaped()).arg(u.port(u.scheme() == QLatin1String("https") ? 443 : 80));
    case QNetworkReply::HostNotFoundError:
        return tr("Host not found: %1").arg(u.host().toHtmlEscaped());
    case QNetworkReply::TimeoutError:
        return tr("No answer within %1 seconds.").arg(r.request.timeoutMs / 1000);
    case QNetworkReply::OperationCanceledError:
        return tr("Cancelled.");
    case QNetworkReply::SslHandshakeFailedError:
        return tr("TLS handshake failed: %1").arg(r.errorText.toHtmlEscaped());
    default:
        return r.errorText.toHtmlEscaped();
    }
}
