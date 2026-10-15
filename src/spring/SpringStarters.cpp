#include "spring/SpringStarters.h"

#include <QFile>
#include <QRegularExpression>

namespace SpringStarters {

namespace {

const char *kBoot = "org.springframework.boot";

Starter make(const char *id, const char *title, const char *description, const char *category, QList<Dep> deps, bool jpa = false)
{
    Starter s;
    s.id = QString::fromUtf8(id);
    s.title = QString::fromUtf8(title);
    s.description = QString::fromUtf8(description);
    s.category = QLatin1String(category);
    s.deps = deps;
    s.kotlinJpa = jpa;
    return s;
}

Dep boot(const char *artifact, const char *config = "implementation")
{
    return {QLatin1String(config), QLatin1String(kBoot), QLatin1String(artifact), {}};
}

Dep lib(const char *config, const char *group, const char *artifact, const char *version = "")
{
    return {QLatin1String(config), QLatin1String(group), QLatin1String(artifact), QLatin1String(version)};
}

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

const QList<Starter> &catalog()
{
    static const QList<Starter> list = {
        make("web", "Spring Web", "REST APIs with Spring MVC and embedded Tomcat", "Web", {boot("spring-boot-starter-web")}),
        make("webflux", "Spring Reactive Web", "Reactive REST APIs with WebFlux and Netty", "Web", {boot("spring-boot-starter-webflux")}),
        make("validation", "Validation", "Bean Validation with Hibernate Validator (@Valid, @NotBlank …)", "Web", {boot("spring-boot-starter-validation")}),
        make("thymeleaf", "Thymeleaf", "Server-side HTML templates", "Web", {boot("spring-boot-starter-thymeleaf")}),
        make("openapi", "OpenAPI / Swagger UI", "springdoc: API docs at /v3/api-docs and /swagger-ui.html", "Web",
             {lib("implementation", "org.springdoc", "springdoc-openapi-starter-webmvc-ui", "2.8.9")}),
        make("security", "Spring Security", "Authentication and authorization", "Security", {boot("spring-boot-starter-security")}),
        make("oauth2-resource", "OAuth2 Resource Server", "Protect the API with JWT bearer tokens", "Security", {boot("spring-boot-starter-oauth2-resource-server")}),
        make("jpa", "Spring Data JPA", "Hibernate and repositories", "SQL", {boot("spring-boot-starter-data-jpa")}, true),
        make("jdbc", "JDBC", "Plain JDBC with JdbcTemplate and Hikari", "SQL", {boot("spring-boot-starter-jdbc")}),
        make("h2", "H2 Database", "In-memory database for development", "SQL", {lib("runtimeOnly", "com.h2database", "h2")}),
        make("postgresql", "PostgreSQL Driver", "JDBC driver", "SQL", {lib("runtimeOnly", "org.postgresql", "postgresql")}),
        make("mysql", "MySQL Driver", "JDBC driver", "SQL", {lib("runtimeOnly", "com.mysql", "mysql-connector-j")}),
        make("flyway", "Flyway Migration", "Versioned database migrations", "SQL", {lib("implementation", "org.flywaydb", "flyway-core")}),
        make("liquibase", "Liquibase Migration", "Database change sets", "SQL", {lib("implementation", "org.liquibase", "liquibase-core")}),
        make("mongodb", "Spring Data MongoDB", "Document database", "NoSQL", {boot("spring-boot-starter-data-mongodb")}),
        make("redis", "Spring Data Redis", "Key-value store and cache", "NoSQL", {boot("spring-boot-starter-data-redis")}),
        make("cache", "Cache", "@Cacheable and cache managers", "NoSQL", {boot("spring-boot-starter-cache")}),
        make("actuator", "Actuator", "Health, metrics, beans and mappings endpoints", "Operations", {boot("spring-boot-starter-actuator")}),
        make("prometheus", "Prometheus Metrics", "Micrometer registry, /actuator/prometheus", "Operations", {lib("runtimeOnly", "io.micrometer", "micrometer-registry-prometheus")}),
        make("devtools", "Developer Tools", "Automatic restart and live reload", "Developer", {boot("spring-boot-devtools", "developmentOnly")}),
        make("docker-compose", "Docker Compose Support", "Starts compose.yaml services with the application", "Developer", {boot("spring-boot-docker-compose", "developmentOnly")}),
        make("amqp", "RabbitMQ", "AMQP messaging", "Messaging", {boot("spring-boot-starter-amqp")}),
        make("kafka", "Apache Kafka", "Event streaming", "Messaging", {lib("implementation", "org.springframework.kafka", "spring-kafka")}),
        make("mail", "Mail", "JavaMailSender", "Messaging", {boot("spring-boot-starter-mail")}),
    };
    return list;
}

QStringList installed(const SpringProject &project)
{
    QString text = readText(project.buildFile());
    // Catalog accessors (libs.spring.boot.starter.web) count when the catalog names the artifact.
    const QString toml = readText(project.workDir + QStringLiteral("/gradle/libs.versions.toml"));
    text += QLatin1Char('\n') + toml;
    QStringList out;
    for (const Starter &s : catalog())
        if (!s.deps.isEmpty() && text.contains(s.deps.first().artifact))
            out << s.id;
    return out;
}

namespace {

// Offset of the '}' closing the block whose '{' is at `open`, or -1.
int matchBrace(const QString &s, int open)
{
    int depth = 0;
    for (int i = open; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('{'))
            ++depth;
        else if (s[i] == QLatin1Char('}') && --depth == 0)
            return i;
    }
    return -1;
}

QString gradleLine(const Dep &d, bool kotlin, const QString &indent)
{
    QString coord = d.group + QLatin1Char(':') + d.artifact;
    if (!d.version.isEmpty())
        coord += QLatin1Char(':') + d.version;
    return kotlin ? QStringLiteral("%1%2(\"%3\")").arg(indent, d.config, coord) : QStringLiteral("%1%2 '%3'").arg(indent, d.config, coord);
}

bool editGradle(QString &text, const QList<Dep> &deps, bool kotlin)
{
    static const QRegularExpression block(QStringLiteral(R"(^dependencies\s*\{)"), QRegularExpression::MultilineOption);
    QStringList lines;
    int close = -1;
    QRegularExpressionMatch m = block.match(text);
    // The module's block is the last top-level one (buildscript { dependencies { } } is indented).
    for (QRegularExpressionMatchIterator it = block.globalMatch(text); it.hasNext();)
        m = it.next();
    if (m.hasMatch()) {
        close = matchBrace(text, m.capturedEnd() - 1);
        if (close < 0)
            return false;
    }
    QString indent = QStringLiteral("    ");
    for (const Dep &d : deps)
        lines << gradleLine(d, kotlin, indent);
    if (close < 0) {
        if (!text.endsWith(QLatin1Char('\n')))
            text += QLatin1Char('\n');
        text += QStringLiteral("\ndependencies {\n") + lines.join(QLatin1Char('\n')) + QStringLiteral("\n}\n");
        return true;
    }
    // After the last implementation / runtimeOnly / developmentOnly line (the test ones stay last); else at the end.
    int at = close;
    static const QRegularExpression mainDep(QStringLiteral(R"(^[ \t]*(?:implementation|api|runtimeOnly|developmentOnly|compileOnly|annotationProcessor)\b[^\n]*\n)"), QRegularExpression::MultilineOption);
    int last = -1;
    for (QRegularExpressionMatchIterator it = mainDep.globalMatch(text, m.capturedEnd()); it.hasNext();) {
        const QRegularExpressionMatch dm = it.next();
        if (dm.capturedStart() > close)
            break;
        last = dm.capturedEnd();
    }
    if (last >= 0) {
        text.insert(last, lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
        return true;
    }
    // Before the closing brace, after the last line of the block.
    while (at > 0 && (text[at - 1] == QLatin1Char(' ') || text[at - 1] == QLatin1Char('\t')))
        --at;
    if (at > 0 && text[at - 1] != QLatin1Char('\n'))
        lines.prepend(QString());
    text.insert(at, lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    return true;
}

bool editMaven(QString &text, const QList<Dep> &deps)
{
    // Skip <dependencyManagement> and <build> (plugin dependencies).
    auto inside = [&](int pos, const char *open, const char *close) {
        const int a = text.indexOf(QLatin1String(open));
        const int b = text.indexOf(QLatin1String(close));
        return a >= 0 && b > a && pos > a && pos < b;
    };
    int pos = -1;
    for (int from = 0;;) {
        const int p = text.indexOf(QStringLiteral("</dependencies>"), from);
        if (p < 0)
            break;
        if (!inside(p, "<dependencyManagement>", "</dependencyManagement>") && !inside(p, "<build>", "</build>")) {
            pos = p;
            break;
        }
        from = p + 1;
    }
    QString unit = QStringLiteral("    ");
    QString base = unit; // indentation of <dependency>
    if (pos >= 0) {
        const int d = text.lastIndexOf(QStringLiteral("<dependency>"), pos);
        if (d >= 0) {
            const int ls = text.lastIndexOf(QLatin1Char('\n'), d) + 1;
            base = text.mid(ls, d - ls);
            // Indentation step of the file: how far <groupId> sits inside <dependency>.
            const int g = text.indexOf(QStringLiteral("<groupId>"), d);
            if (g > d) {
                const int gl = text.lastIndexOf(QLatin1Char('\n'), g) + 1;
                const QString gi = text.mid(gl, g - gl);
                if (gi.size() > base.size() && gi.startsWith(base))
                    unit = gi.mid(base.size());
            }
        } else {
            const int ls = text.lastIndexOf(QLatin1Char('\n'), pos) + 1;
            base = text.mid(ls, pos - ls) + unit;
        }
    }
    QString xml;
    for (const Dep &d : deps) {
        xml += base + QStringLiteral("<dependency>\n");
        xml += base + unit + QStringLiteral("<groupId>%1</groupId>\n").arg(d.group);
        xml += base + unit + QStringLiteral("<artifactId>%1</artifactId>\n").arg(d.artifact);
        if (!d.version.isEmpty())
            xml += base + unit + QStringLiteral("<version>%1</version>\n").arg(d.version);
        if (d.config == QLatin1String("runtimeOnly"))
            xml += base + unit + QStringLiteral("<scope>runtime</scope>\n");
        else if (d.config == QLatin1String("developmentOnly"))
            xml += base + unit + QStringLiteral("<scope>runtime</scope>\n") + base + unit + QStringLiteral("<optional>true</optional>\n");
        xml += base + QStringLiteral("</dependency>\n");
    }
    if (pos < 0) {
        const int end = text.lastIndexOf(QStringLiteral("</project>"));
        if (end < 0)
            return false;
        text.insert(end, QStringLiteral("    <dependencies>\n") + xml + QStringLiteral("    </dependencies>\n"));
        return true;
    }
    const int ls = text.lastIndexOf(QLatin1Char('\n'), pos) + 1;
    text.insert(ls, xml);
    return true;
}

} // namespace

Change plan(const SpringProject &project, const QStringList &ids)
{
    Change c;
    c.file = project.buildFile();
    c.text = readText(c.file);
    if (c.file.isEmpty() || c.text.isEmpty()) {
        c.error = QObject::tr("The build file could not be read.");
        return c;
    }
    const bool kotlinDsl = c.file.endsWith(QLatin1String(".kts"));
    QList<Dep> deps;
    bool jpa = false;
    const QStringList have = installed(project);
    for (const Starter &s : catalog()) {
        if (!ids.contains(s.id) || have.contains(s.id))
            continue;
        c.added << s.title;
        deps << s.deps;
        jpa = jpa || s.kotlinJpa;
    }
    if (deps.isEmpty()) {
        c.error = QObject::tr("Nothing selected.");
        return c;
    }
    // Spring Boot 4 renamed the MVC starter.
    if (c.text.contains(QStringLiteral("starter-webmvc")) || readText(project.workDir + QStringLiteral("/gradle/libs.versions.toml")).contains(QStringLiteral("starter-webmvc")))
        for (Dep &d : deps)
            if (d.artifact == QLatin1String("spring-boot-starter-web"))
                d.artifact = QStringLiteral("spring-boot-starter-webmvc");
    const bool done = project.maven ? editMaven(c.text, deps) : editGradle(c.text, deps, kotlinDsl);
    if (!done) {
        c.error = QObject::tr("No place for the dependencies was found in the build file.");
        return c;
    }
    if (jpa && !project.maven && !c.text.contains(QStringLiteral("plugin.jpa")) && !c.text.contains(QStringLiteral("plugin.jpa"))) {
        // Entities need a no-arg constructor: copy the Kotlin Spring compiler plugin line as the JPA one.
        static const QRegularExpression springPlugin(QStringLiteral(R"(^([ \t]*)kotlin\("plugin\.spring"\)([^\n]*)$)"), QRegularExpression::MultilineOption);
        static const QRegularExpression springAlias(QStringLiteral(R"(^([ \t]*)alias\(libs\.plugins\.([\w.]*)spring\))"), QRegularExpression::MultilineOption);
        QRegularExpressionMatch m = springPlugin.match(c.text);
        if (m.hasMatch()) {
            c.text.insert(m.capturedEnd(), QLatin1Char('\n') + m.captured(1) + QStringLiteral("kotlin(\"plugin.jpa\")") + m.captured(2));
        } else if ((m = springAlias.match(c.text)).hasMatch() && m.captured(2).contains(QStringLiteral("kotlin"))) {
            // Version catalog: add `kotlin-plugin-jpa = { id = "org.jetbrains.kotlin.plugin.jpa", version.ref = "…" }` next to the spring one.
            const QString tomlPath = project.workDir + QStringLiteral("/gradle/libs.versions.toml");
            QString toml = readText(tomlPath);
            static const QRegularExpression entry(QStringLiteral(R"(^([\w-]+)\s*=\s*\{\s*id\s*=\s*"org\.jetbrains\.kotlin\.plugin\.spring"([^\n]*)$)"), QRegularExpression::MultilineOption);
            const QRegularExpressionMatch tm = entry.match(toml);
            if (tm.hasMatch() && !toml.contains(QStringLiteral("org.jetbrains.kotlin.plugin.jpa"))) {
                QString key = tm.captured(1);
                key.replace(QStringLiteral("spring"), QStringLiteral("jpa"));
                toml.insert(tm.capturedEnd(), QLatin1Char('\n') + key + QStringLiteral(" = { id = \"org.jetbrains.kotlin.plugin.jpa\"") + tm.captured(2));
                c.extra << qMakePair(tomlPath, toml);
                QString accessor = m.captured(2) + QStringLiteral("jpa");
                c.text.insert(m.capturedEnd(), QLatin1Char('\n') + m.captured(1) + QStringLiteral("alias(libs.plugins.") + accessor + QLatin1Char(')'));
            } else if (!tm.hasMatch()) {
                c.note = QObject::tr("Kotlin entities need the JPA compiler plugin: add the plugin “org.jetbrains.kotlin.plugin.jpa” (same version as the Kotlin plugin).");
            }
        } else if (c.text.contains(QStringLiteral("kotlin"))) {
            c.note = QObject::tr("Kotlin entities need the JPA compiler plugin: add the plugin “org.jetbrains.kotlin.plugin.jpa” (same version as the Kotlin plugin).");
        }
    }
    c.ok = true;
    return c;
}

} // namespace SpringStarters
