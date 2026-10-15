#pragma once

#include "settings/Theme.h"
#include "spring/SpringEndpoints.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QSyntaxHighlighter;

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

    QJsonObject state() const; // history, saved requests and the request being edited
    void setState(const QJsonObject &state);
    void clearForNewProject();

signals:
    void locationRequested(const QString &path, int line, int column);
    void refreshRequested(); // the endpoint list should be scanned again
    void startRequested();   // "start the application" link in a connection error
    void stateChanged();     // history or saved requests changed: persist state()

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
    void updateTarget();
    void send();
    void finished();
    void showResponse(const QString &status, const QColor &colour, const QString &body, const QString &headers);
    void showResponseTab(int tab);
    void remember(const Request &r, int status, qint64 ms);
    void saveCurrent();
    void listMenu(const QPoint &pos);
    void copyCurl();
    void formatBody();
    QString curl(const Request &r) const;

    SpringScan m_scan;
    QJsonArray m_history, m_saved;
    QString m_base = QStringLiteral("http://localhost:8080");
    bool m_running = false, m_scanning = false;
    int m_list = 0, m_respTab = 0;
    Theme m_theme;

    QNetworkAccessManager m_net;
    QNetworkReply *m_reply = nullptr;
    QElapsedTimer m_clock;
    QString m_respBody, m_respHeaders;

    QButtonGroup *m_listTabs, *m_reqTabs, *m_respTabs;
    QToolButton *m_listAction;
    QLineEdit *m_filter;
    QListWidget *m_items;
    QComboBox *m_method;
    QLineEdit *m_url;
    QPushButton *m_send;
    QLabel *m_target, *m_status;
    QStackedWidget *m_reqStack;
    QPlainTextEdit *m_headers, *m_body, *m_response;
    QToolButton *m_copyResponse;
    QSyntaxHighlighter *m_bodyColours, *m_responseColours;
};
