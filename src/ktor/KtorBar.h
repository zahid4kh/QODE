#pragma once

#include "ktor/KtorProject.h"
#include "ktor/KtorSource.h"

#include <QWidget>

class KtorReloader;
class KtorServer;
class QFileSystemWatcher;
class QNetworkAccessManager;
class QLabel;
class QTimer;
class QToolButton;

// Toolbar strip for a Ktor server: status dot, the address the server answers on, start / stop, restart, the
// auto-reload switch, the button that opens the Ktor panel and the settings. Hidden (isAvailable() false) when the
// project is no Ktor server. It also owns the server process and the auto-reload loop, which the panel only observes.
//
// Auto-reload: a start with it on begins a session that lasts until stop (or another project). The KtorReloader keeps
// the classes compiled; after each compile Ktor's own development-mode reload is triggered with one request (Ktor
// checks for changed classes when a call arrives), or, where that would not change the code (a module Ktor cannot
// load again by name, development mode switched off in code, nothing watched), the server is restarted. A server that
// failed to start is started again by the next successful compile.
class KtorBar : public QWidget
{
    Q_OBJECT
public:
    explicit KtorBar(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root); // re-detects; stops a running application of the previous project
    bool isAvailable() const { return m_project.valid; }
    bool isActive() const;
    KtorServer *server() const { return m_server; }
    KtorReloader *reloader() const { return m_reloader; }
    bool reloadSession() const { return m_session; }
    // "" when Ktor reloads in place, else why the server is restarted instead.
    QString restartReason() const;
    const KtorProject &project() const { return m_project; }
    KtorProject::Options options() const;
    QString command() const; // what start() runs

    void start(); // starts, or restarts when it already runs
    void stop();
    void restart();
    void toggle();
    void configure();
    void shutdown(); // blocks briefly until the application is gone (app closing)
    void setAutoReload(bool on); // the bar's switch: stored, and applied by restarting a running server
    void sourceSaved(const QString &path); // Maven compiles on save (Gradle's continuous build watches by itself)
    // What decides between reloading in place and restarting. The running server's answer (KtorLiveResult::
    // codeReloadable: 1 / 0, -1 unknown) wins over the sources' (an embeddedServer whose module Ktor cannot load again).
    void setCodeReloadable(int reloadable);
    void setSourceFacts(const KtorScan &scan);

signals:
    void availabilityChanged(bool available);
    void stateChanged();
    void panelRequested();
    void starting(); // a start was requested (the window may reveal the log)
    void openFileRequested(const QString &path); // from the settings dialog
    void projectChanged(); // detected again (a build file changed): routes and plugins may differ
    void rootChanged(); // another project was opened (the log of the previous one is stale)
    void reloadMarker(const QString &text); // a step of auto-reload for the log ("Restarting: …")

private:
    void redetect();
    void refresh();
    void launch(); // (re)starts the server with the current settings, keeping the auto-reload session
    void endSession();
    void onServerState();
    void onCompiled();
    void requestReload();

    KtorServer *m_server;
    KtorReloader *m_reloader;
    QNetworkAccessManager *m_net;
    QTimer *m_reloadWait;
    bool m_session = false;
    bool m_awaitingReload = false;
    bool m_moduleReloadable = true; // from the sources: no embeddedServer, or one whose module Ktor loads again by name
    int m_codeReloadable = -1;      // from the running server, -1 until read
    KtorProject m_project;
    QString m_root;
    QFileSystemWatcher *m_watcher;
    QTimer *m_redetect;
    QLabel *m_dot, *m_text;
    QToolButton *m_toggle, *m_restart, *m_reload, *m_panel, *m_config;
};
