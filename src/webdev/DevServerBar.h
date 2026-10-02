#pragma once

#include "WebProject.h"

#include <QWidget>

class DevServer;
class ServerLogDialog;
class QFileSystemWatcher;
class QLabel;
class QTimer;
class QToolButton;

// Toolbar strip for a web project's dev server: status dot, the address it is listening on, start / stop,
// restart, logs and settings. Hidden (isAvailable() false) when the project has no server script.
class DevServerBar : public QWidget
{
    Q_OBJECT
public:
    explicit DevServerBar(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root); // re-detects the project; stops a running server of the previous one
    bool isAvailable() const { return m_project.valid; }
    bool isActive() const;

    void start();
    void stop();
    void restart();
    void toggle();
    void configure();
    void showLog();
    void shutdown(); // blocks briefly until the server is gone (app closing)

signals:
    void availabilityChanged(bool available);
    void stateChanged();

private:
    struct Effective
    {
        QString manager, script, command;
        int port = 0;
    };
    Effective effective() const;
    void refresh();
    void redetect(); // package.json or the lockfile changed
    void repaintIcons();

    DevServer *m_server;
    WebProject m_project;
    QString m_root;
    QFileSystemWatcher *m_watcher;
    QTimer *m_redetect;
    QLabel *m_dot, *m_text;
    QToolButton *m_toggle, *m_restart, *m_log, *m_config;
    ServerLogDialog *m_logDialog = nullptr;
};
