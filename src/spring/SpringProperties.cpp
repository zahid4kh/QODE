#include "spring/SpringProperties.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

namespace {

struct Key
{
    const char *name;
    const char *detail; // type and default
    const char *values; // '|' separated choices, or empty
};

const Key kKeys[] = {
    {"spring.application.name", "String", ""},
    {"spring.profiles.active", "List — profiles to activate", ""},
    {"spring.profiles.include", "List — profiles always on", ""},
    {"spring.profiles.group", "Map — profile groups", ""},
    {"spring.config.import", "List — extra config sources", ""},
    {"spring.config.activate.on-profile", "String — profile this document applies to", ""},
    {"spring.main.banner-mode", "console", "console|log|off"},
    {"spring.main.web-application-type", "auto-detected", "none|servlet|reactive"},
    {"spring.main.allow-bean-definition-overriding", "boolean, false", "true|false"},
    {"spring.main.lazy-initialization", "boolean, false", "true|false"},
    {"spring.threads.virtual.enabled", "boolean, false", "true|false"},
    {"spring.jackson.default-property-inclusion", "always", "always|non_null|non_absent|non_default|non_empty"},
    {"spring.jackson.property-naming-strategy", "String", "SNAKE_CASE|LOWER_CAMEL_CASE|UPPER_CAMEL_CASE|KEBAB_CASE|LOWER_CASE"},
    {"spring.jackson.serialization.write-dates-as-timestamps", "boolean", "true|false"},
    {"spring.jackson.deserialization.fail-on-unknown-properties", "boolean", "true|false"},
    {"spring.jackson.time-zone", "String", ""},
    {"spring.jackson.date-format", "String", ""},
    {"spring.mvc.static-path-pattern", "/**", ""},
    {"spring.mvc.problemdetails.enabled", "boolean, false", "true|false"},
    {"spring.mvc.throw-exception-if-no-handler-found", "boolean", "true|false"},
    {"spring.web.resources.add-mappings", "boolean, true", "true|false"},
    {"spring.servlet.multipart.enabled", "boolean, true", "true|false"},
    {"spring.servlet.multipart.max-file-size", "1MB", ""},
    {"spring.servlet.multipart.max-request-size", "10MB", ""},
    {"spring.datasource.url", "JDBC URL", ""},
    {"spring.datasource.username", "String", ""},
    {"spring.datasource.password", "String", ""},
    {"spring.datasource.driver-class-name", "String", ""},
    {"spring.datasource.hikari.maximum-pool-size", "int, 10", ""},
    {"spring.datasource.hikari.minimum-idle", "int", ""},
    {"spring.datasource.hikari.connection-timeout", "ms, 30000", ""},
    {"spring.datasource.hikari.idle-timeout", "ms, 600000", ""},
    {"spring.datasource.hikari.max-lifetime", "ms, 1800000", ""},
    {"spring.jpa.hibernate.ddl-auto", "none for real databases", "none|validate|update|create|create-drop"},
    {"spring.jpa.hibernate.naming.physical-strategy", "class name", ""},
    {"spring.jpa.show-sql", "boolean, false", "true|false"},
    {"spring.jpa.open-in-view", "boolean, true", "true|false"},
    {"spring.jpa.database-platform", "dialect class", ""},
    {"spring.jpa.generate-ddl", "boolean, false", "true|false"},
    {"spring.jpa.properties.hibernate.format_sql", "boolean", "true|false"},
    {"spring.jpa.properties.hibernate.dialect", "class name", ""},
    {"spring.jpa.properties.hibernate.jdbc.time_zone", "String", ""},
    {"spring.sql.init.mode", "embedded", "always|embedded|never"},
    {"spring.sql.init.schema-locations", "List", ""},
    {"spring.sql.init.data-locations", "List", ""},
    {"spring.h2.console.enabled", "boolean, false", "true|false"},
    {"spring.h2.console.path", "/h2-console", ""},
    {"spring.flyway.enabled", "boolean, true", "true|false"},
    {"spring.flyway.locations", "classpath:db/migration", ""},
    {"spring.flyway.baseline-on-migrate", "boolean, false", "true|false"},
    {"spring.liquibase.change-log", "classpath:/db/changelog/db.changelog-master.yaml", ""},
    {"spring.data.redis.host", "localhost", ""},
    {"spring.data.redis.port", "6379", ""},
    {"spring.data.redis.password", "String", ""},
    {"spring.data.mongodb.uri", "mongodb://localhost/test", ""},
    {"spring.data.web.pageable.default-page-size", "20", ""},
    {"spring.data.web.pageable.max-page-size", "2000", ""},
    {"spring.cache.type", "auto", "simple|redis|caffeine|none|jcache"},
    {"spring.kafka.bootstrap-servers", "List", ""},
    {"spring.kafka.consumer.group-id", "String", ""},
    {"spring.kafka.consumer.auto-offset-reset", "latest", "earliest|latest|none"},
    {"spring.rabbitmq.host", "localhost", ""},
    {"spring.rabbitmq.port", "5672", ""},
    {"spring.rabbitmq.username", "guest", ""},
    {"spring.rabbitmq.password", "guest", ""},
    {"spring.mail.host", "String", ""},
    {"spring.mail.port", "int", ""},
    {"spring.mail.username", "String", ""},
    {"spring.mail.password", "String", ""},
    {"spring.security.user.name", "user", ""},
    {"spring.security.user.password", "generated at startup", ""},
    {"spring.security.user.roles", "List", ""},
    {"spring.security.oauth2.resourceserver.jwt.issuer-uri", "URL", ""},
    {"spring.security.oauth2.resourceserver.jwt.jwk-set-uri", "URL", ""},
    {"spring.devtools.restart.enabled", "boolean, true", "true|false"},
    {"spring.devtools.livereload.enabled", "boolean, true", "true|false"},
    {"spring.lifecycle.timeout-per-shutdown-phase", "30s", ""},
    {"spring.messages.basename", "messages", ""},
    {"spring.output.ansi.enabled", "detect", "detect|always|never"},
    {"spring.docker.compose.enabled", "boolean, true", "true|false"},
    {"spring.docker.compose.lifecycle-management", "start-and-stop", "none|start-only|start-and-stop"},
    {"spring.autoconfigure.exclude", "List of auto-configuration classes", ""},
    {"server.port", "int, 8080 (0 = random)", ""},
    {"server.address", "network address", ""},
    {"server.servlet.context-path", "String", ""},
    {"server.shutdown", "immediate", "graceful|immediate"},
    {"server.error.include-message", "never", "always|on_param|never"},
    {"server.error.include-stacktrace", "never", "always|on_param|never"},
    {"server.error.include-binding-errors", "never", "always|on_param|never"},
    {"server.error.whitelabel.enabled", "boolean, true", "true|false"},
    {"server.compression.enabled", "boolean, false", "true|false"},
    {"server.http2.enabled", "boolean, false", "true|false"},
    {"server.forward-headers-strategy", "none", "none|native|framework"},
    {"server.tomcat.threads.max", "int, 200", ""},
    {"server.tomcat.max-http-form-post-size", "2MB", ""},
    {"server.ssl.enabled", "boolean", "true|false"},
    {"server.ssl.key-store", "path", ""},
    {"server.ssl.key-store-password", "String", ""},
    {"server.ssl.key-store-type", "String", "PKCS12|JKS"},
    {"logging.level.root", "INFO", "TRACE|DEBUG|INFO|WARN|ERROR|OFF"},
    {"logging.level.org.springframework.web", "level", "TRACE|DEBUG|INFO|WARN|ERROR|OFF"},
    {"logging.level.org.springframework.security", "level", "TRACE|DEBUG|INFO|WARN|ERROR|OFF"},
    {"logging.level.org.hibernate.SQL", "level", "TRACE|DEBUG|INFO|WARN|ERROR|OFF"},
    {"logging.level.org.hibernate.orm.jdbc.bind", "level", "TRACE|DEBUG|INFO|WARN|ERROR|OFF"},
    {"logging.file.name", "path of the log file", ""},
    {"logging.file.path", "folder of spring.log", ""},
    {"logging.pattern.console", "Logback pattern", ""},
    {"logging.pattern.file", "Logback pattern", ""},
    {"logging.pattern.dateformat", "yyyy-MM-dd'T'HH:mm:ss.SSSXXX", ""},
    {"logging.structured.format.console", "ecs|gelf|logstash", "ecs|gelf|logstash"},
    {"logging.group.web", "list of loggers", ""},
    {"management.endpoints.web.exposure.include", "health only by default; * = all", "*|health|health,info|health,info,metrics,prometheus"},
    {"management.endpoints.web.exposure.exclude", "List", ""},
    {"management.endpoints.web.base-path", "/actuator", ""},
    {"management.endpoint.health.show-details", "never", "never|when-authorized|always"},
    {"management.endpoint.health.show-components", "never", "never|when-authorized|always"},
    {"management.endpoint.health.probes.enabled", "boolean, false", "true|false"},
    {"management.endpoint.shutdown.enabled", "boolean, false", "true|false"},
    {"management.health.defaults.enabled", "boolean, true", "true|false"},
    {"management.info.env.enabled", "boolean, false", "true|false"},
    {"management.info.git.mode", "simple", "simple|full"},
    {"management.server.port", "int — separate management port", ""},
    {"management.metrics.tags.application", "String", ""},
    {"management.tracing.sampling.probability", "double, 0.1", ""},
    {"management.prometheus.metrics.export.enabled", "boolean", "true|false"},
    {"management.otlp.tracing.endpoint", "URL", ""},
    {"springdoc.api-docs.path", "/v3/api-docs", ""},
    {"springdoc.swagger-ui.path", "/swagger-ui.html", ""},
    {"info.app.name", "String", ""},
    {"info.app.version", "String", ""},
    {"debug", "boolean — print the auto-configuration report", "true|false"},
    {"trace", "boolean", "true|false"},
};

struct Index
{
    QMap<QString, Key> flat;               // dotted key -> data
    QMap<QString, QSet<QString>> children; // parent path ("" = root) -> segments
    Index()
    {
        for (const Key &k : kKeys) {
            const QString full = QString::fromLatin1(k.name);
            flat.insert(full, k);
            const QStringList parts = full.split(QLatin1Char('.'));
            QString parent;
            for (const QString &p : parts) {
                children[parent].insert(p);
                parent = parent.isEmpty() ? p : parent + QLatin1Char('.') + p;
            }
        }
    }
};

const Index &index()
{
    static const Index idx;
    return idx;
}

QList<SpringProperties::Entry> childrenOf(const QString &parent, const QString &partial)
{
    QList<SpringProperties::Entry> out;
    const Index &idx = index();
    const auto it = idx.children.constFind(parent);
    if (it == idx.children.constEnd())
        return out;
    QStringList names(it->begin(), it->end());
    names.sort();
    for (const QString &n : std::as_const(names)) {
        if (!n.startsWith(partial))
            continue;
        const QString full = parent.isEmpty() ? n : parent + QLatin1Char('.') + n;
        SpringProperties::Entry e;
        e.label = n.mid(partial.size());
        e.group = idx.children.contains(full);
        const auto k = idx.flat.constFind(full);
        e.detail = k != idx.flat.constEnd() ? QString::fromLatin1(k->detail) : QStringLiteral("…");
        if (e.label.isEmpty())
            continue;
        out << e;
    }
    return out;
}

QList<SpringProperties::Entry> valuesOf(const QString &key)
{
    QList<SpringProperties::Entry> out;
    const auto it = index().flat.constFind(key);
    if (it == index().flat.constEnd() || !*it->values)
        return out;
    for (const QString &v : QString::fromLatin1(it->values).split(QLatin1Char('|')))
        out << SpringProperties::Entry{v, QStringLiteral("value"), false};
    return out;
}

} // namespace

namespace SpringProperties {

bool isConfigFile(const QString &path)
{
    static const QRegularExpression re(QStringLiteral(R"(^(?:application|bootstrap)(?:-[\w.-]+)?\.(?:properties|ya?ml)$)"));
    return re.match(QFileInfo(path).fileName()).hasMatch();
}

bool isYaml(const QString &path)
{
    return path.endsWith(QLatin1String(".yml")) || path.endsWith(QLatin1String(".yaml"));
}

QMap<QString, QString> load(const QString &dir)
{
    QMap<QString, QString> out;
    auto read = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(256 * 1024)) : QString();
    };
    static const QRegularExpression prop(QStringLiteral(R"(^\s*([\w.\-\[\]]+)\s*[=:]\s*(.*?)\s*$)"));
    for (const QString &line : read(dir + QStringLiteral("/application.properties")).split(QLatin1Char('\n'))) {
        if (line.trimmed().startsWith(QLatin1Char('#')))
            continue;
        const QRegularExpressionMatch m = prop.match(line);
        if (m.hasMatch())
            out.insert(m.captured(1), m.captured(2));
    }
    static const QRegularExpression yamlLine(QStringLiteral(R"(^(\s*)([\w.\-\[\]]+)\s*:\s*(.*?)\s*$)"));
    for (const char *name : {"application.yml", "application.yaml"}) {
        const QString text = read(dir + QLatin1Char('/') + QLatin1String(name));
        if (text.isEmpty())
            continue;
        if (text.contains(QStringLiteral("\n---")))
            continue; // multi-document files: the values depend on the profile
        QList<QPair<int, QString>> stack; // indent, key
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1Char('#')))
                continue;
            const QRegularExpressionMatch m = yamlLine.match(line);
            if (!m.hasMatch())
                continue;
            const int indent = m.captured(1).size();
            while (!stack.isEmpty() && stack.last().first >= indent)
                stack.removeLast();
            QStringList path;
            for (const auto &p : std::as_const(stack))
                path << p.second;
            path << m.captured(2);
            QString value = m.captured(3);
            if (value.isEmpty()) {
                stack.append({indent, m.captured(2)});
            } else {
                if ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))
                    value = value.mid(1, value.size() - 2);
                out.insert(path.join(QLatin1Char('.')), value);
            }
        }
        break;
    }
    return out;
}

QStringList yamlParents(const QStringList &linesAbove, int indent)
{
    static const QRegularExpression keyLine(QStringLiteral(R"(^(\s*)([\w.\-\[\]]+)\s*:(?:\s|$))"));
    QStringList path;
    int need = indent;
    for (int i = linesAbove.size() - 1; i >= 0 && need > 0; --i) {
        const QRegularExpressionMatch m = keyLine.match(linesAbove[i]);
        if (!m.hasMatch() || linesAbove[i].trimmed().startsWith(QLatin1Char('#')))
            continue;
        const int ind = m.captured(1).size();
        if (ind < need) {
            path.prepend(m.captured(2));
            need = ind;
        }
    }
    return path;
}

QList<Entry> complete(const QString &beforeWord, bool yaml, const QStringList &parents)
{
    static const QRegularExpression propKey(QStringLiteral(R"(^\s*([\w.\-\[\]]*)$)"));
    static const QRegularExpression propValue(QStringLiteral(R"(^\s*([\w.\-\[\]]+)\s*[=:]\s*[\w.\-,*]*$)"));
    static const QRegularExpression yamlKey(QStringLiteral(R"(^\s*([\w.\-]*)$)"));
    static const QRegularExpression yamlValue(QStringLiteral(R"(^\s*([\w.\-]+)\s*:\s+[\w.\-,*]*$)"));

    if (yaml) {
        QRegularExpressionMatch m = yamlValue.match(beforeWord);
        if (m.hasMatch()) {
            QString key = (parents + QStringList{m.captured(1)}).join(QLatin1Char('.'));
            QList<Entry> v = valuesOf(key);
            const int cut = beforeWord.size() - beforeWord.lastIndexOf(QRegularExpression(QStringLiteral(R"([\s:])"))) - 1;
            for (Entry &e : v)
                if (cut > 0 && e.label.startsWith(beforeWord.right(cut)))
                    e.label = e.label.mid(cut);
            return v;
        }
        m = yamlKey.match(beforeWord);
        if (!m.hasMatch())
            return {};
        // "max-" : the editor's word starts after the hyphen, so only the rest of the segment is offered.
        const QString partial = m.captured(1);
        return childrenOf(parents.join(QLatin1Char('.')), partial);
    }

    QRegularExpressionMatch m = propValue.match(beforeWord);
    if (m.hasMatch()) {
        QList<Entry> v = valuesOf(m.captured(1));
        const QString tail = beforeWord.mid(beforeWord.lastIndexOf(QRegularExpression(QStringLiteral(R"([=:\s])"))) + 1);
        for (Entry &e : v)
            if (!tail.isEmpty() && e.label.startsWith(tail))
                e.label = e.label.mid(tail.size());
        return v;
    }
    m = propKey.match(beforeWord);
    if (!m.hasMatch())
        return {};
    const QString key = m.captured(1);
    const int dot = key.lastIndexOf(QLatin1Char('.'));
    const QString parent = dot < 0 ? QString() : key.left(dot);
    const QString partial = key.mid(dot + 1);
    return childrenOf(parent, partial);
}

} // namespace SpringProperties
