#pragma once

#include "http/HttpFile.h"
#include "settings/Theme.h"
#include "spring/SpringEndpoints.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;
class HttpJsonHighlighter;
class HttpResponseView;
class HttpRunner;

// A small REST client for the Spring panel: pick an endpoint found in the controllers' source (or type a URL), edit
// headers and body, send, read the formatted response. Sent requests are kept as history and can be saved by name.
// Knows nothing about the server: the owner gives it the base address ("http://localhost:8080") and persists state().
class HttpClientView : public QWidget
{
    Q_OBJECT
public:
    explicit HttpClientView(QWidget *parent = nullptr);

    void setBaseUrl(const QString &base); // used for URLs that start with '/'
    void setServerRunning(bool running);
    void setScan(const SpringScan &scan);
    void setScanning(bool scanning);
    // Words of the endpoint list: while scanning, when nothing was found, the rescan button (Spring's by default).
    void setScanTexts(const QString &scanning, const QString &empty, const QString &rescanTip);
    // Fills the request from an endpoint (also from outside the list, e.g. the Ktor Routes tab); sends it right away
    // when asked and nothing is left to fill in.
    void openEndpoint(const SpringEndpoint &e, bool sendNow);
    QString endpointAsHttp(const SpringEndpoint &e) const; // the endpoint as a .http request
    QString endpointAsCurl(const SpringEndpoint &e) const; // and as a curl command (placeholders left in)

    QJsonObject state() const; // history, saved requests and the request being edited
    void setState(const QJsonObject &state);
    void clearForNewProject();

signals:
    void locationRequested(const QString &path, int line, int column);
    void refreshRequested(); // the endpoint list should be scanned again
    void startRequested();   // "start the application" link in a connection error
    void stateChanged();     // history or saved requests changed: persist state()
    void openTextRequested(const QString &text); // the response's "Open in editor"

private:
    struct Request
    {
        QString method = QStringLiteral("GET");
        QString url, headers, body;
    };

    void applyTheme();
    void populate();
    void showList(int tab);
    void loadEndpoint(const SpringEndpoint &e);
    void loadRequest(const QJsonObject &o);
    Request current() const;
    QString resolvedUrl() const;
    QString absoluteUrl(const QString &url) const; // '/path' against the base address
    Request requestFor(const SpringEndpoint &e) const;
    void updateTarget();
    bool requestData(const Request &r, HttpRequestData *out, QString *error) const;
    void send();
    void finished(const HttpResult &result, const Request &r);
    void showError(const QString &html);
    void setSending(bool sending);
    void remember(const Request &r, int status, qint64 ms);
    void saveCurrent();
    void listMenu(const QPoint &pos);
    void copyCurl();
    void formatBody();
    QString asHttp(const Request &r, const QString &name) const; // the request as .http text

    SpringScan m_scan;
    QJsonArray m_history, m_saved;
    QString m_base = QStringLiteral("http://localhost:8080");
    bool m_running = false, m_scanning = false;
    QString m_scanningText, m_emptyText, m_rescanTip;
    int m_list = 0;
    Theme m_theme;
    HttpRunner *m_runner;

    QButtonGroup *m_listTabs, *m_reqTabs;
    QToolButton *m_listAction;
    QLineEdit *m_filter;
    QListWidget *m_items;
    QComboBox *m_method;
    QLineEdit *m_url;
    QPushButton *m_send;
    QLabel *m_target;
    QStackedWidget *m_reqStack;
    QPlainTextEdit *m_headers, *m_body;
    HttpJsonHighlighter *m_bodyColours;
    HttpResponseView *m_response;
};
