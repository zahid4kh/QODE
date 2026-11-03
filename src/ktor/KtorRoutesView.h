#pragma once

#include "ktor/KtorSource.h"
#include "settings/Theme.h"

#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QToolButton;

// The Routes tab of the Ktor panel: every route (full paths, method, auth, kind), grouped by the function that declares
// it: the running server's routes when it runs, else what the sources declare. Double-click goes to the source; the menu tries a route in the Requests tab, sends
// it, opens it in the browser or copies it.
class KtorRoutesView : public QWidget
{
    Q_OBJECT
public:
    explicit KtorRoutesView(QWidget *parent = nullptr);

    void setScan(const KtorScan &scan);
    void setServer(const QString &baseUrl, bool running); // http://localhost:8080, and whether it answers
    // Where the routes come from ("read from the running server" / "from the sources"), for the summary line.
    void setOrigin(const QString &text, const QString &tooltip);
    // The .http text / curl command of an HTTP route (the Requests tab builds them).
    void setFormatters(std::function<QString(const SpringEndpoint &)> asHttp, std::function<QString(const SpringEndpoint &)> asCurl);

signals:
    void locationRequested(const QString &path, int line, int column);
    void tryRequested(const SpringEndpoint &endpoint, bool sendNow); // open in the Requests tab
    void refreshRequested();

private:
    void applyTheme();
    void populate();
    void showMenu(const QPoint &pos);
    QString urlOf(const KtorRoute &r) const;

    KtorScan m_scan;
    QString m_base = QStringLiteral("http://localhost:8080");
    bool m_running = false;
    QString m_origin;
    Theme m_theme;
    std::function<QString(const SpringEndpoint &)> m_asHttp, m_asCurl;

    QLineEdit *m_filter;
    QComboBox *m_kind;
    QToolButton *m_refresh;
    QListWidget *m_list;
    QLabel *m_summary;
};
