#pragma once

#include "webdev/DevServer.h"

// Metro (`npx expo start`) run headless for the web preview: an Expo app in the browser, shown in a phone mockup.
// It runs on its own free port (19006 and up), so a Metro started in the terminal for a device keeps 8081. A Node
// preload (resources/expo/web-preview.cjs) serves a web.output "static" app as a single-page app without touching
// app.json: rendering routes in Node crashes Metro as soon as app code touches `window` or a native module.
class ExpoWebServer : public DevServer
{
    Q_OBJECT
public:
    using DevServer::DevServer;

    void startApp(const QString &appDir);
    QString appDir() const { return m_appDir; } // the app it runs (or last ran)

protected:
    void scan(const QString &line) override;
    void adjustEnvironment(QProcessEnvironment &env) override;

private:
    QString m_appDir;
    bool m_bundleFailed = false; // the next line holds the reason
};
