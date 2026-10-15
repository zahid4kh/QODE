#pragma once

#include "settings/Theme.h"

#include <QNetworkAccessManager>
#include <QWidget>

class QButtonGroup;
class QJsonValue;
class QLabel;
class QLineEdit;
class QNetworkReply;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Actuator endpoints of the running application as tables: health, beans, mappings, environment, info, metrics.
// Knows nothing about the project beyond the address it is given; the owner says where the application listens and
// where its configuration lives (to expose endpoints and to find a management port / base path).
class ActuatorView : public QWidget
{
    Q_OBJECT
public:
    explicit ActuatorView(QWidget *parent = nullptr);

    // `base` is "http://localhost:8080". `resourcesDir` is read for management.server.port / base-path.
    void setTarget(const QString &base, const QString &resourcesDir, bool running);
    void refresh(); // asks the selected endpoint again

signals:
    void openFileRequested(const QString &path);
    void startRequested();

private:
    void applyTheme();
    void load();
    void fetch(const QString &endpoint, const QString &sub, std::function<void(const QJsonValue &)> done);
    void show(const QString &endpoint, const QJsonValue &json);
    void message(const QString &html);
    void exposeAll();
    void filter(const QString &text);
    QString endpointBase() const;

    void showHealth(const QJsonValue &v);
    void showBeans(const QJsonValue &v);
    void showMappings(const QJsonValue &v);
    void showEnv(const QJsonValue &v);
    void showMetrics(const QJsonValue &v);
    void showGeneric(const QJsonValue &v);

    QNetworkAccessManager m_net;
    QNetworkReply *m_reply = nullptr;
    QString m_base, m_resources;
    bool m_running = false;
    Theme m_theme;
    int m_token = 0;

    QButtonGroup *m_endpoints;
    QStringList m_names;
    QString currentEndpoint() const;
    QLineEdit *m_filter;
    QToolButton *m_reload;
    QLabel *m_info;
    QTreeWidget *m_tree;
};
