#include "ktor/KtorPlugins.h"

#include "project/BuildFileEdit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>

namespace KtorPlugins {

namespace {

Plugin make(const char *id, const char *title, const char *description, const char *category, QStringList keys, const char *install,
            bool serialization = false)
{
    Plugin p;
    p.id = QLatin1String(id);
    p.title = QString::fromUtf8(title);
    p.description = QString::fromUtf8(description);
    p.category = QLatin1String(category);
    p.keys = keys;
    p.install = QString::fromUtf8(install);
    p.serialization = serialization;
    return p;
}

QStringList k(std::initializer_list<const char *> keys)
{
    QStringList out;
    for (const char *key : keys)
        out << QLatin1String(key);
    return out;
}

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QString catalogPath(const KtorProject &project)
{
    QDir d(project.moduleDir);
    for (int i = 0; i < 3; ++i) {
        const QString toml = d.filePath(QStringLiteral("gradle/libs.versions.toml"));
        if (QFileInfo::exists(toml))
            return toml;
        if (!d.cdUp())
            break;
    }
    return {};
}

// Adds the kotlinx.serialization compiler plugin next to the Kotlin one, in the same style. Returns false (with a note)
// when the build declares Kotlin in a way this does not know.
bool addSerializationPlugin(const KtorProject &project, Change &c)
{
    if (c.text.contains(QStringLiteral("plugin.serialization")) || c.text.contains(QStringLiteral("kotlinx-serialization"))
        || c.text.contains(QStringLiteral("plugin-serialization")) || c.text.contains(QStringLiteral("kotlin(\"plugin.serialization\")")))
        return true;
    // kotlin("jvm") version "2.2.0"  ->  kotlin("plugin.serialization") version "2.2.0"
    static const QRegularExpression kotlinJvm(QStringLiteral(R"re(^([ \t]*)kotlin\("(?:jvm|multiplatform)"\)([^\n]*)$)re"), QRegularExpression::MultilineOption);
    static const QRegularExpression idJvm(QStringLiteral(R"re(^([ \t]*)id\(?\s*["']org\.jetbrains\.kotlin\.(?:jvm|multiplatform)["']\s*\)?([^\n]*)$)re"), QRegularExpression::MultilineOption);
    static const QRegularExpression aliasJvm(QStringLiteral(R"(^([ \t]*)alias\(libs\.plugins\.([\w.]*?)(?:jvm|multiplatform)\))"), QRegularExpression::MultilineOption);
    QRegularExpressionMatch m = kotlinJvm.match(c.text);
    if (m.hasMatch()) {
        c.text.insert(m.capturedEnd(), QLatin1Char('\n') + m.captured(1) + QStringLiteral("kotlin(\"plugin.serialization\")") + m.captured(2));
        return true;
    }
    if ((m = idJvm.match(c.text)).hasMatch()) {
        const bool kts = c.file.endsWith(QLatin1String(".kts"));
        c.text.insert(m.capturedEnd(), QLatin1Char('\n') + m.captured(1)
                                           + (kts ? QStringLiteral("id(\"org.jetbrains.kotlin.plugin.serialization\")") : QStringLiteral("id 'org.jetbrains.kotlin.plugin.serialization'"))
                                           + m.captured(2));
        return true;
    }
    if ((m = aliasJvm.match(c.text)).hasMatch()) {
        // `kotlin-jvm = { id = "org.jetbrains.kotlin.jvm", version.ref = "kotlin" }` gets a serialization sibling.
        const QString tomlPath = catalogPath(project);
        QString toml = readText(tomlPath);
        for (const auto &e : std::as_const(c.extra))
            if (e.first == tomlPath)
                toml = e.second;
        static const QRegularExpression entry(QStringLiteral(R"re(^([\w-]+)\s*=\s*\{\s*id\s*=\s*"org\.jetbrains\.kotlin\.(?:jvm|multiplatform)"([^\n]*)$)re"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch tm = entry.match(toml);
        if (tm.hasMatch()) {
            QString key = tm.captured(1);
            key.replace(QRegularExpression(QStringLiteral("(jvm|multiplatform)$")), QStringLiteral("plugin-serialization"));
            if (!toml.contains(QStringLiteral("org.jetbrains.kotlin.plugin.serialization")))
                toml.insert(tm.capturedEnd(), QLatin1Char('\n') + key + QStringLiteral(" = { id = \"org.jetbrains.kotlin.plugin.serialization\"") + tm.captured(2));
            bool replaced = false;
            for (auto &e : c.extra)
                if (e.first == tomlPath) {
                    e.second = toml;
                    replaced = true;
                }
            if (!replaced)
                c.extra << qMakePair(tomlPath, toml);
            QString accessor = key;
            accessor.replace(QLatin1Char('-'), QLatin1Char('.')).replace(QLatin1Char('_'), QLatin1Char('.'));
            c.text.insert(m.capturedEnd(), QLatin1Char('\n') + m.captured(1) + QStringLiteral("alias(libs.plugins.") + accessor + QLatin1Char(')'));
            return true;
        }
    }
    return false;
}

} // namespace

const QList<Plugin> &catalog()
{
    static const QList<Plugin> list = {
        make("json", "Content Negotiation + JSON", "kotlinx.serialization JSON bodies: call.receive<T>() and call.respond(t) for @Serializable classes",
             "Serialization", k({"server-contentNegotiation", "serialization-kotlinx-json"}), "install(ContentNegotiation) { json() }", true),
        make("jackson", "Content Negotiation + Jackson", "JSON through Jackson, for plain Kotlin / Java classes", "Serialization",
             k({"server-contentNegotiation", "serialization-jackson"}), "install(ContentNegotiation) { jackson() }"),
        make("gson", "Content Negotiation + Gson", "JSON through Gson", "Serialization", k({"server-contentNegotiation", "serialization-gson"}),
             "install(ContentNegotiation) { gson() }"),
        make("resources", "Resources", "Type-safe routing with @Resource classes: get<Articles.Id> { }", "Routing", k({"server-resources"}),
             "install(Resources)", true),
        make("status-pages", "Status Pages", "Answer exceptions and status codes in one place", "Routing", k({"server-statusPages"}),
             "install(StatusPages) { exception<Throwable> { call, cause -> … } }"),
        make("request-validation", "Request Validation", "Validate received bodies before the handler runs", "Routing", k({"server-requestValidation"}),
             "install(RequestValidation) { validate<T> { … } }"),
        make("auto-head", "Auto HEAD Response", "Answers HEAD for every GET route", "Routing", k({"server-autoHeadResponse"}), "install(AutoHeadResponse)"),
        make("double-receive", "Double Receive", "Read the request body more than once", "Routing", k({"server-doubleReceive"}), "install(DoubleReceive)"),
        make("websockets", "WebSockets", "webSocket(\"/ws\") { } sessions", "Routing", k({"server-websockets"}), "install(WebSockets)"),
        make("sse", "Server-Sent Events", "sse(\"/events\") { send(…) } streams", "Routing", k({"server-sse"}), "install(SSE)"),
        make("cors", "CORS", "Cross-origin requests from browser front ends", "HTTP", k({"server-cors"}), "install(CORS) { anyHost() }"),
        make("compression", "Compression", "gzip / deflate responses", "HTTP", k({"server-compression"}), "install(Compression)"),
        make("default-headers", "Default Headers", "Date / Server headers on every response", "HTTP", k({"server-defaultHeaders"}), "install(DefaultHeaders)"),
        make("caching-headers", "Caching Headers", "Cache-Control and Expires per content type", "HTTP", k({"server-cachingHeaders"}), "install(CachingHeaders)"),
        make("conditional-headers", "Conditional Headers", "ETag / Last-Modified and 304 answers", "HTTP", k({"server-conditionalHeaders"}),
             "install(ConditionalHeaders)"),
        make("forwarded", "Forwarded Headers", "Client address and scheme behind a reverse proxy", "HTTP", k({"server-forwardedHeader"}),
             "install(XForwardedHeaders)"),
        make("hsts", "HSTS", "Strict-Transport-Security header", "HTTP", k({"server-hsts"}), "install(HSTS)"),
        make("https-redirect", "HTTPS Redirect", "Send plain HTTP requests to HTTPS", "HTTP", k({"server-httpRedirect"}), "install(HttpsRedirect)"),
        make("partial-content", "Partial Content", "Range requests (resumable downloads, video)", "HTTP", k({"server-partialContent"}), "install(PartialContent)"),
        make("rate-limit", "Rate Limit", "Limit requests per client", "HTTP", k({"server-rateLimit"}), "install(RateLimit) { global { rateLimiter(…) } }"),
        make("body-limit", "Body Limit", "Reject request bodies above a size", "HTTP", k({"server-bodyLimit"}), "install(RequestBodyLimit) { bodyLimit { … } }"),
        make("auth", "Authentication", "Basic, form, session and bearer providers; authenticate { } blocks", "Security", k({"server-auth"}),
             "install(Authentication) { basic(\"auth-basic\") { … } }"),
        make("jwt", "Authentication JWT", "JSON Web Token bearer authentication", "Security", k({"server-auth-jwt", "server-auth"}),
             "install(Authentication) { jwt(\"auth-jwt\") { … } }"),
        make("sessions", "Sessions", "Cookie or header sessions", "Security", k({"server-sessions"}), "install(Sessions) { cookie<UserSession>(\"session\") }"),
        make("csrf", "CSRF", "Cross-site request forgery protection", "Security", k({"server-csrf"}), "install(CSRF) { … }"),
        make("call-logging", "Call Logging", "One log line per request", "Monitoring", k({"server-callLogging"}), "install(CallLogging)"),
        make("call-id", "Call ID", "Request ids for tracing", "Monitoring", k({"server-callId"}), "install(CallId) { generate { … } }"),
        make("micrometer", "Micrometer Metrics", "Request metrics for Prometheus and others", "Monitoring", k({"server-metrics-micrometer"}),
             "install(MicrometerMetrics) { registry = … }"),
        make("swagger", "Swagger UI", "Interactive API docs from an OpenAPI file", "API docs", k({"server-swagger"}),
             "routing { swaggerUI(path = \"swagger\", swaggerFile = \"openapi/documentation.yaml\") }"),
        make("openapi", "OpenAPI", "Serve the OpenAPI document", "API docs", k({"server-openapi"}),
             "routing { openAPI(path = \"openapi\", swaggerFile = \"openapi/documentation.yaml\") }"),
        make("html", "HTML DSL", "Build HTML pages with kotlinx.html: call.respondHtml { }", "Templating", k({"server-htmlBuilder"}), "call.respondHtml { body { … } }"),
        make("thymeleaf", "Thymeleaf", "Server-side HTML templates", "Templating", k({"server-thymeleaf"}), "install(Thymeleaf) { setTemplateResolver(…) }"),
        make("freemarker", "FreeMarker", "Server-side templates", "Templating", k({"server-freemarker"}), "install(FreeMarker) { templateLoader = … }"),
    };
    return list;
}

QString artifactOf(const QString &key)
{
    QString out = QStringLiteral("ktor-");
    for (const QChar c : key) {
        if (c.isUpper()) {
            out += QLatin1Char('-');
            out += c.toLower();
        } else {
            out += c;
        }
    }
    return out;
}

QString accessorOf(const QString &key)
{
    QString a = key;
    return a.replace(QLatin1Char('-'), QLatin1Char('.'));
}

QStringList installed(const KtorProject &project)
{
    QStringList out;
    for (const Plugin &p : catalog()) {
        // A plugin counts as added when its distinctive module is there (json: the serialization module, not the
        // content negotiation it shares with jackson / gson).
        const QString key = p.keys.size() > 1 && p.keys.first() == QLatin1String("server-contentNegotiation") ? p.keys.at(1) : p.keys.first();
        if (project.hasArtifact(artifactOf(key), accessorOf(key)))
            out << p.id;
    }
    return out;
}

Change plan(const KtorProject &project, const QStringList &ids)
{
    Change c;
    c.file = project.buildFile();
    c.text = readText(c.file);
    if (c.file.isEmpty() || c.text.isEmpty()) {
        c.error = QObject::tr("The build file could not be read.");
        return c;
    }
    QStringList keys, installs;
    bool serialization = false;
    const QStringList have = installed(project);
    for (const Plugin &p : catalog()) {
        if (!ids.contains(p.id) || have.contains(p.id))
            continue;
        c.added << p.title;
        installs << p.install;
        serialization = serialization || p.serialization;
        for (const QString &key : p.keys)
            if (!keys.contains(key) && !project.hasArtifact(artifactOf(key), accessorOf(key)))
                keys << key;
    }
    if (c.added.isEmpty()) {
        c.error = QObject::tr("Nothing selected.");
        return c;
    }

    bool done = false;
    if (project.maven) {
        // Copy the version (and the -jvm suffix) of the ktor-server-core dependency.
        static const QRegularExpression core(QStringLiteral(R"(<artifactId>\s*ktor-server-core(-jvm)?\s*</artifactId>(?:(?!</dependency>)[\s\S])*?<version>\s*([^<\s]+)\s*</version>)"));
        const QRegularExpressionMatch m = core.match(c.text);
        if (!m.hasMatch()) {
            c.error = QObject::tr("pom.xml has no ktor-server-core dependency to take the version from.");
            return c;
        }
        QList<BuildFileEdit::MavenDependency> deps;
        for (const QString &key : std::as_const(keys))
            deps << BuildFileEdit::MavenDependency{QStringLiteral("io.ktor"), artifactOf(key) + m.captured(1), m.captured(2), {}, false};
        done = BuildFileEdit::insertMavenDependencies(c.text, deps);
    } else {
        QStringList lines;
        const bool kts = c.file.endsWith(QLatin1String(".kts"));
        static const QRegularExpression coord(QStringLiteral(R"re(["']io\.ktor:ktor-server-core(-jvm)?(:[^"']*)?["'])re"));
        const QString tomlPath = catalogPath(project);
        const QString toml = readText(tomlPath);
        static const QRegularExpression tomlCore(QStringLiteral(R"re(^([\w-]+)\s*=\s*\{([^\n]*?)io\.ktor[^\n]*?ktor-server-core(-jvm)?"([^\n]*)$)re"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch cm = coord.match(c.text);
        const QRegularExpressionMatch tm = tomlCore.match(toml);
        if (project.usesKtorCatalog()) {
            for (const QString &key : std::as_const(keys))
                lines << QStringLiteral("implementation(ktorLibs.%1)").arg(accessorOf(key));
        } else if (tm.hasMatch() && c.text.contains(QStringLiteral("libs.") + QString(tm.captured(1)).replace(QLatin1Char('-'), QLatin1Char('.')).replace(QLatin1Char('_'), QLatin1Char('.')))) {
            // The project's own catalog names Ktor modules: add entries shaped like its ktor-server-core one.
            QString newToml = toml;
            int at = tm.capturedEnd();
            QString prefix = tm.captured(1);
            prefix.chop(QStringLiteral("server-core").size());
            if (prefix.endsWith(QLatin1Char('-')) || prefix.endsWith(QLatin1Char('_')) || prefix.isEmpty()) {
                for (const QString &key : std::as_const(keys)) {
                    const QString alias = prefix + artifactOf(key).mid(5); // "ktor-" + "server-cors"
                    if (newToml.contains(QRegularExpression(QStringLiteral("^") + QRegularExpression::escape(alias) + QStringLiteral(R"(\s*=)"), QRegularExpression::MultilineOption)))
                        continue;
                    QString line = tm.captured(0);
                    line.replace(tm.captured(1), alias);
                    line.replace(QStringLiteral("ktor-server-core"), artifactOf(key));
                    newToml.insert(at, QLatin1Char('\n') + line);
                    at += line.size() + 1;
                    QString accessor = alias;
                    lines << QStringLiteral("implementation(libs.%1)").arg(accessor.replace(QLatin1Char('-'), QLatin1Char('.')).replace(QLatin1Char('_'), QLatin1Char('.')));
                }
                c.extra << qMakePair(tomlPath, newToml);
            }
        } else if (cm.hasMatch()) {
            for (const QString &key : std::as_const(keys)) {
                const QString coordinate = QStringLiteral("io.ktor:") + artifactOf(key) + cm.captured(1) + cm.captured(2);
                lines << (kts ? QStringLiteral("implementation(\"%1\")").arg(coordinate) : QStringLiteral("implementation \"%1\"").arg(coordinate));
            }
        }
        if (lines.isEmpty() && !keys.isEmpty()) {
            c.error = QObject::tr("The build declares Ktor in a way QODE does not recognise (no ktor-server-core to copy the version from). Add the modules by hand: %1.")
                          .arg([&keys] {
                              QStringList a;
                              for (const QString &key : keys)
                                  a << artifactOf(key);
                              return a.join(QStringLiteral(", "));
                          }());
            return c;
        }
        done = lines.isEmpty() || BuildFileEdit::insertGradleLines(c.text, lines);
        if (done && serialization && !addSerializationPlugin(project, c))
            c.note = QObject::tr("@Serializable classes need the kotlinx.serialization compiler plugin: add the plugin “org.jetbrains.kotlin.plugin.serialization” (same version as the Kotlin plugin).")
                     + QStringLiteral("\n\n");
    }
    if (!done) {
        c.error = QObject::tr("No place for the dependencies was found in the build file.");
        return c;
    }
    c.note += QObject::tr("Turn them on in a module, e.g.:\n%1").arg(installs.join(QLatin1Char('\n')));
    c.ok = true;
    return c;
}

} // namespace KtorPlugins
