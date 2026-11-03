#pragma once

#include "ktor/KtorConfigView.h"
#include "ktor/KtorLive.h"
#include "ktor/KtorSource.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTimer;
class QToolButton;
class HttpClientView;
class KtorBar;
class KtorRoutesView;
class SpringLogView;

// Right-hand panel of a Ktor server: its state (building, starting, answering on which port, uptime, startup time) and
// four tabs: the live log with level / text filters, the routes, the Requests tab (HTTP client with those routes) and
// Config (engine, port, modules, installed plugins, the configuration file's values). The routes are the running
// server's own routing tree (KtorLive, read again after every reload) joined with what the source scan knows; while no
// server runs, the scan's. Start, stop, restart and auto-reload are the bar's; the panel only observes them.
class KtorPanel : public QWidget
{
    Q_OBJECT
public:
    explicit KtorPanel(QWidget *parent = nullptr);

    void attach(KtorBar *bar);
    SpringLogView *log() const { return m_log; }
    // A Kotlin source or the configuration of the server changed: the routes / config tabs are read again (soon, when shown).
    void sourcesChanged();
    // Project file of a class's source ("com/example", "Routing.kt"); also used for the log's stack frames.
    void setResolver(const KtorLive::Resolver &resolver);
    // The address requests go to: the running port, else the configured one.
    QString baseUrl() const;

signals:
    void closeRequested();
    void locationRequested(const QString &path, int line, int column);
    void openFileRequested(const QString &path);
    void openTextRequested(const QString &text); // a response to show in a new editor tab
    void addPluginsRequested();

protected:
    void showEvent(QShowEvent *event) override;

private:
    enum Page { LogPage, RoutesPage, RequestsPage, ConfigPage };

    void refresh();
    void onStateChanged();
    void updateCounts();
    void repaintIcons();
    void loadRequests();
    void showPage(int page);
    void rescan();
    void rescanIfShown();
    void applyScan();     // m_scan = the source scan, joined with the running server's routes when there are any
    void readLive();      // reads the running server's routing tree on a worker thread
    void dropLive();
    void updateConfig();
    void configure();
    KtorConfigView::Port configuredPort() const;

    KtorBar *m_bar = nullptr;
    int m_lastState = -1;
    KtorScan m_code; // what the source scan found
    KtorScan m_scan; // what the tabs show
    KtorLiveResult m_live;
    KtorLive::Resolver m_resolver;
    bool m_liveBusy = false, m_liveAgain = false;
    int m_liveGeneration = 0;
    int m_lastReloaderState = 0;
    bool m_stale = true;
    QLabel *m_logo, *m_title, *m_dot, *m_status, *m_detail, *m_count;
    QToolButton *m_toggle, *m_restart, *m_config, *m_closeBtn;
    QComboBox *m_level;
    QLineEdit *m_filter;
    QToolButton *m_build, *m_pause, *m_clear, *m_copy, *m_end;
    SpringLogView *m_log;
    KtorRoutesView *m_routes;
    HttpClientView *m_http;
    KtorConfigView *m_configView;
    QStackedWidget *m_stack;
    QList<QToolButton *> m_tabs;
    QTimer *m_uptime, *m_filterTimer, *m_rescanTimer;
};
