#pragma once

#include "ExpoProject.h"

#include <QWidget>

class QFileSystemWatcher;
class QLabel;
class QMenu;
class QTimer;
class QToolButton;

// Toolbar strip for an Expo project: a Run button (click = the last used configuration, arrow = all of them: Android,
// iOS, Metro / Expo Go, prebuild, React Native DevTools) and a button that opens the app.json editor. Hidden unless
// ExpoProject finds an app; in a monorepo the menu picks which app the commands run in.
class ExpoBar : public QWidget
{
    Q_OBJECT
public:
    explicit ExpoBar(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root); // re-detects
    bool isAvailable() const { return !m_apps.isEmpty(); }
    const QList<ExpoApp> &apps() const { return m_apps; }
    const ExpoApp *currentApp() const;
    QStringList appDirs() const;
    // The app a file belongs to (the deepest app folder containing it), or nullptr.
    const ExpoApp *appForFile(const QString &path) const;

    void runLast();      // F5
    void runId(const QString &id);
    void useApp(const QString &dir);

signals:
    void availabilityChanged();
    // A shell command line (already `cd`-ed into the app) for the terminal; `id` names the configuration.
    void commandRequested(const QString &command, const QString &id);
    void devToolsRequested();
    void configureRequested();
    void schemaCacheRequested();
    void appChanged();

private:
    struct Entry
    {
        QString id, text, command, tip;
        bool enabled = true;
    };
    QList<Entry> entries() const;
    void redetect();
    void rebuildMenu();
    void refresh();
    QString shellCommand(const QString &command) const;

    QList<ExpoApp> m_apps;
    QString m_root;
    QFileSystemWatcher *m_watcher;
    QTimer *m_redetect;
    QToolButton *m_run, *m_config;
    QLabel *m_app;
    QMenu *m_menu;
};
