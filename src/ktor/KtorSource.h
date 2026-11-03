#pragma once

#include "spring/SpringEndpoints.h"

#include <QString>
#include <QStringList>
#include <QVector>

// One route of the routing DSL. `ep` carries the method, the full path (prefixes of enclosing route("…") blocks and of
// the routes that mount a `fun Route.x()` included), the declaring function as `controller`, file / line and the
// parameters the handler reads, so the Requests tab can use it like a Spring endpoint.
struct KtorRoute
{
    enum Kind { Http, WebSocket, Sse, Static, Docs };
    SpringEndpoint ep;
    Kind kind = Http;
    QStringList auth;   // providers of the authenticate("…") blocks around it ("" = the default provider)
    QString resource;   // type-safe routing: the @Resource class (get<Articles.Id>)
    bool mounted = true; // false: declared in a `fun Route.x()` that no scanned code calls
    bool notServed = false;   // the running server's routing tree has no such route
    bool runtimeOnly = false; // only the running server has it (built by code the scan cannot follow)

    QString kindName() const;
};

struct KtorInstall
{
    QString plugin;   // ContentNegotiation, CORS, Authentication …
    QString function; // where it is installed (configureSerialization)
    QString file;
    int line = 1;
};

struct KtorAuthProvider
{
    QString type; // jwt, basic, form, session, oauth, bearer, digest, ldap, apiKey
    QString name; // "" = the default provider
    QString file;
    int line = 1;
};

// `fun Application.x()`: what application.yaml / .conf can list under ktor.application.modules.
struct KtorModuleFunction
{
    QString name;
    QString fqn; // com.example.ApplicationKt.module, the way the configuration names it
    QString file;
    int line = 1;
};

struct KtorScan
{
    QVector<KtorRoute> routes; // by file, then line
    QVector<KtorInstall> installs;
    QVector<KtorAuthProvider> authProviders;
    QVector<KtorModuleFunction> modules;
    SpringScan http; // the HTTP routes as endpoints plus the Kotlin classes (JSON body skeletons) for the Requests tab
    // embeddedServer(Netty, port = 8080 …) in code, when the server is started that way.
    QString embeddedEngine;
    int embeddedPort = 0;
    QString embeddedFile;
    int embeddedLine = 0;
    // embeddedServer(…, module = Application::module) naming a `suspend fun`: Ktor loads that module again by name on a
    // development-mode reload. A lambda ({ module() }) or a reference the compiler has to adapt to `suspend` (a plain
    // `fun`) is called as it was at start, so a reload keeps the old code.
    bool embeddedModuleReloadable = false;
    bool live = false; // the routes are the running server's (KtorLive::merge), not only what the scan found
    int files = 0; // Kotlin files read
};

// Reads the Kotlin sources of a Ktor server. Plain text scanning (comments and string contents are masked first, so
// braces in them do not count), no compiler involved, so it also works while the project does not build. Qt Core only.
namespace KtorSource {

KtorScan scan(const QStringList &sourceRoots);
// For tests and single files: the routes of the given texts (path -> text), resolved together.
KtorScan scanTexts(const QList<QPair<QString, QString>> &files);

} // namespace KtorSource
