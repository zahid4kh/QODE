#pragma once

#include "spring/SpringProject.h"

#include <QWidget>

class SpringServer;
class QFileSystemWatcher;
class QLabel;
class QTimer;
class QToolButton;

// Toolbar strip for a Spring Boot project: status dot, the address the application listens on, start / stop, restart,
// the button that opens the Spring panel and the settings. Hidden (isAvailable() false) when the project is no Spring
// Boot application. It also owns the server process, which the panel only observes.
class SpringBar : public QWidget
{
    Q_OBJECT
public:
    explicit SpringBar(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root); // re-detects; stops a running application of the previous project
    bool isAvailable() const { return m_project.valid; }
    bool isActive() const;
    SpringServer *server() const { return m_server; }
    const SpringProject &project() const { return m_project; }
    SpringProject::Options options() const;
    QString command() const; // what start() runs

    void start(); // starts, or restarts when it already runs
    void stop();
    void restart();
    void toggle();
    void configure();
    void shutdown(); // blocks briefly until the application is gone (app closing)

signals:
    void availabilityChanged(bool available);
    void stateChanged();
    void panelRequested();
    void starting(); // a start was requested (the window may reveal the log)
    void openFileRequested(const QString &path); // from the settings dialog
    void rootChanged(); // another project was opened (the log of the previous one is stale)

private:
    void redetect();
    void refresh();

    SpringServer *m_server;
    SpringProject m_project;
    QString m_root;
    QFileSystemWatcher *m_watcher;
    QTimer *m_redetect;
    QLabel *m_dot, *m_text;
    QToolButton *m_toggle, *m_restart, *m_panel, *m_config;
};
