#pragma once

#include "http/HttpFile.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>

#include <functional>

class QNetworkReply;

// Sends one request at a time and keeps cookies between requests (until clearCookies()), like a browser session.
class HttpRunner : public QObject
{
    Q_OBJECT
public:
    explicit HttpRunner(QObject *parent = nullptr);

    using Done = std::function<void(const HttpResult &)>;
    // `base` carries name / file / line into the result. A request already on its way is cancelled first.
    void send(const HttpRequestData &request, const HttpResult &base, Done done);
    void abort();
    bool busy() const { return m_reply != nullptr; }
    void clearCookies();
    int cookieCount() const;

    // A sentence for a result without a response (refused, timed out …); rich text.
    static QString describeError(const HttpResult &r);

private:
    void finished();

    QNetworkAccessManager m_net;
    QNetworkReply *m_reply = nullptr;
    bool m_aborted = false; // by abort(), not by the transfer timeout
    QElapsedTimer m_clock;
    HttpResult m_pending;
    Done m_done;
};
