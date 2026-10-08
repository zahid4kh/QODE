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
    bool isAvailable() const { return m_project.valid && !ignored(); }
    // Folders that belong to another run control (Expo apps): a server script there is not offered as a web server.
    void setIgnoredDirs(const QStringList &absoluteDirs);
    bool isActive() const;

    void start();
    void stop();
    void restart();
    void toggle();
    void configure();
    void editEnv();
    // The folder the server runs in: the project root, or the nested web project (e.g. docs/) picked for it.
    QString workDir() const;
    // Uses a nested web project (relative folder) and starts its server.
    void useFolder(const QString &relative);
    void dontAskAboutNested();
    void showLog();
    void shutdown(); // blocks briefly until the server is gone (app closing)

signals:
    void availabilityChanged(bool available);
    // The project root is no server project itself but these folders are (asked once per project open).
    void nestedProjectsFound(const QStringList &relativeDirs);
    void openFileRequested(const QString &path);
    void stateChanged();

private:
    struct Effective
    {
        QString manager, script, command;
        int port = 0;
    };
    Effective effective() const;
    void refresh();
    QString configuredDir() const;
    bool ignored() const;
    void redetect(); // package.json or the lockfile changed
    void repaintIcons();

    DevServer *m_server;
    WebProject m_project;
    QString m_root;
    QStringList m_ignored;
    QFileSystemWatcher *m_watcher;
    QTimer *m_redetect;
    QLabel *m_dot, *m_text;
    QToolButton *m_toggle, *m_restart, *m_log, *m_env, *m_config;
    ServerLogDialog *m_logDialog = nullptr;
};
