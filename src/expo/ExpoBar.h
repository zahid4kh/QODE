#pragma once

#include "ExpoProject.h"

#include <QWidget>

class ExpoWebServer;
class QFileSystemWatcher;
class QDialog;
class QLabel;
class QMenu;
class QTimer;
class QToolButton;

// Toolbar strip for an Expo project: a Run button (the last used configuration) with an arrow button beside it (all of them: Android,
// iOS, Metro / Expo Go, prebuild, React Native DevTools), the phone preview button and a button that opens the app.json
// editor. Hidden unless ExpoProject finds an app; in a monorepo the menu picks which app the commands run in. It owns
// the headless Metro of the web preview (ExpoWebServer).
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

    ExpoWebServer *webServer() const { return m_web; }
    void startWeb(const QString &appDir); // the app in the web preview; another app's preview Metro is stopped first
    void installWebSupport(const QString &appDir); // `npx expo install react-native-web react-dom @expo/metro-runtime`
    void showWebLog();
    void shutdown(); // app closing: ends the preview Metro

signals:
    void availabilityChanged();
    // A shell command line (already `cd`-ed into the app) for the terminal; `id` names the configuration.
    void commandRequested(const QString &command, const QString &id);
    void devToolsRequested();
    void configureRequested();
    void schemaCacheRequested();
    void appChanged();
    void previewRequested();

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
    QString shellCommand(const QString &command, const QString &dir = QString()) const;

    QList<ExpoApp> m_apps;
    QString m_root;
    QFileSystemWatcher *m_watcher;
    QTimer *m_redetect;
    ExpoWebServer *m_web;
    QDialog *m_webLog = nullptr;
    QToolButton *m_run, *m_runMenu, *m_preview, *m_config;
    QLabel *m_app;
    QMenu *m_menu;
};
