#pragma once

#include "ktor/KtorSource.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// One route of the routing tree of the running server, as Ktor itself holds it (RoutingRoot and its RoutingNode
// children): what the server really answers, whatever code built it (loops, computed paths, plugins).
struct KtorLiveRoute
{
    QString method;           // GET … ("" = any method)
    QString path;             // /api/v1/users/{id}
    QStringList auth;         // providers of the authenticate("…") nodes above it ("" = the default provider)
    QStringList conditions;   // other selectors: header("X-Api", "1"), accept(…), param("q") …
    KtorRoute::Kind kind = KtorRoute::Http;
    QString handlerClass;     // com.example.RoutingKt$configureRouting$1$1: the project's lambda when one was found
    QString sourceFile;       // Routing.kt (from the class's debug information)
    int line = 0;             // first line of the handler's body, 0 when unknown
    QString loader;           // class loader of the handler's class (jdk.internal.loader.ClassLoaders$AppClassLoader …)
};

struct KtorLiveResult
{
    bool ok = false;
    QString error;
    QVector<KtorLiveRoute> routes;

    // Whether Ktor's development-mode reload replaces the project's code: the project's classes then come from Ktor's
    // OverridingClassLoader. A module Ktor cannot load again by name (embeddedServer(…) { module() }, or a reference
    // the compiler adapts to `suspend`) keeps the classes of the start, and a reload changes nothing. -1 when no
    // handler of the project was found.
    int codeReloadable() const;
};

// Reads the routing tree of a running Ktor server through the JVM's debug interface (JDWP): QODE starts the server
// with -agentlib:jdwp=…,server=y,suspend=n,address=127.0.0.1:<a free port it picked> and connects there. Only fields are
// read (no code runs in the server, nothing is suspended); the routing tree is found as the RoutingRoot of the
// application the server currently runs (EmbeddedServer.applicationInstance, so the tree of an application replaced by
// an auto-reload is skipped). Ktor 3 (RoutingRoot / RoutingNode) and Ktor 2 (Routing / Route). Qt Core + Network.
namespace KtorLive {

// Blocking: connect, read, detach. Call it from a worker thread.
KtorLiveResult read(int port, int timeoutMs = 8000);

// The routes to show: the running server's, each joined with the route the source scan found for it (its parameters,
// request body, declaring function and line); routes the code declares but the server does not serve are kept and
// marked. `resolve` turns a package path and file name ("com/example", "Routing.kt") into a project file.
using Resolver = std::function<QString(const QString &pkgPath, const QString &fileName)>;
KtorScan merge(const KtorScan &code, const KtorLiveResult &live, const Resolver &resolve);

} // namespace KtorLive
