#pragma once

#include "ktor/KtorProject.h"
#include "ktor/KtorSource.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// The Config tab of the Ktor panel: how the server is set up (engine, port and where it comes from, development mode,
// Ktor version), the modules application.yaml / .conf loads, the plugins installed in code, the authentication
// providers, every value of the configuration file and the Ktor modules the build declares. Rows with a source go
// there on double-click.
class KtorConfigView : public QWidget
{
    Q_OBJECT
public:
    explicit KtorConfigView(QWidget *parent = nullptr);

    struct Port
    {
        int port = 8080;
        QString source; // "run settings", "application.yaml", "embeddedServer", "default"
        QString raw;    // the value as configured ("$PORT:8080")
    };
    // `reload` / `routes`: how auto-reload works for this server and where the routes come from, as sentences.
    void setData(const KtorProject &project, const KtorProject::Options &options, const KtorScan &scan, const Port &port, bool running,
                 int runningPort, const QString &reload, const QString &routes);

signals:
    void locationRequested(const QString &path, int line, int column);
    void addPluginsRequested();
    void settingsRequested();
    void refreshRequested();

private:
    QTreeWidgetItem *section(const QString &title, int count = -1);
    QTreeWidgetItem *row(QTreeWidgetItem *parent, const QString &name, const QString &value, const QString &file = QString(), int line = 0);
    void applyTheme();

    QTreeWidget *m_tree;
    QPushButton *m_add, *m_settings;
    QToolButton *m_refresh;
    QLabel *m_note;
};
