#include "ExpoWebServer.h"

#include "ExpoSchema.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTcpServer>

namespace {

// The first port from 19006 (Expo's old web port) nothing listens on.
int freePort()
{
    for (int port = 19006; port < 19100; ++port) {
        QTcpServer probe;
        if (probe.listen(QHostAddress::Any, quint16(port)))
            return port;
    }
    return 19006;
}

// The preload as a file Node can --require, rewritten when QODE ships a different one.
QString preloadPath()
{
    QFile res(QStringLiteral(":/expo/web-preview.cjs"));
    if (!res.open(QIODevice::ReadOnly))
        return {};
    const QByteArray code = res.readAll();
    const QString path = ExpoSchema::cacheDir() + QStringLiteral("/web-preview.cjs");
    QFile f(path);
    if (f.open(QIODevice::ReadOnly) && f.readAll() == code)
        return path;
    f.close();
    QDir().mkpath(ExpoSchema::cacheDir());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(code) != code.size())
        return {};
    return path;
}

} // namespace

void ExpoWebServer::startApp(const QString &appDir)
{
    m_appDir = appDir;
    const int port = freePort();
    // No --web: that opens a browser and may install web packages; Metro serves the web app on its port anyway.
    start(QStringLiteral("npx expo start --port %1").arg(port), appDir, 0);
}

void ExpoWebServer::adjustEnvironment(QProcessEnvironment &env)
{
    const QString preload = preloadPath();
    if (preload.isEmpty())
        return;
    QString options = env.value(QStringLiteral("NODE_OPTIONS"));
    if (!options.isEmpty())
        options += QLatin1Char(' ');
    env.insert(QStringLiteral("NODE_OPTIONS"), options + QStringLiteral("--require \"%1\"").arg(preload));
}

void ExpoWebServer::scan(const QString &line)
{
    // Keep the first error Metro dies with ("CommandError: ...", "ReferenceError: window is not defined"), for the
    // preview's placeholder; errors of the app itself are printed with a "Web" prefix and do not stop it.
    static const QRegularExpression fatal(QStringLiteral(R"(^(?:\w*Error|Port \d+ is being used)\b.*)"));
    if (m_error.isEmpty()) {
        const QRegularExpressionMatch m = fatal.match(line.trimmed());
        if (m.hasMatch())
            m_error = m.captured(0).left(300);
    }
    // "Web Bundling failed 2345ms node_modules/expo-router/entry.js", a blank line, then the reason. The browser only
    // gets a blank page, so the reason goes to the preview.
    if (line.contains(QLatin1String("Bundling failed"))) {
        m_bundleFailed = true;
    } else if (m_bundleFailed && !line.trimmed().isEmpty()) {
        m_bundleFailed = false;
        QString reason = line.trimmed();
        if (reason.startsWith(QLatin1String("Metro error: ")))
            reason = reason.mid(13);
        emit buildError(reason);
    }
    DevServer::scan(line);
}
