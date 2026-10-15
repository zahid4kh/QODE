#include "spring/ActuatorView.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "spring/SpringProperties.h"

#include <QButtonGroup>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString scalar(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::String: return v.toString();
    case QJsonValue::Bool: return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: return QString::number(v.toDouble(), 'g', 15);
    case QJsonValue::Null: return QStringLiteral("null");
    case QJsonValue::Array: {
        QStringList parts;
        for (const QJsonValue &e : v.toArray())
            parts << scalar(e);
        return QLatin1Char('[') + parts.join(QStringLiteral(", ")) + QLatin1Char(']');
    }
    default: return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }
}

QTreeWidgetItem *row(QTreeWidget *tree, QTreeWidgetItem *parent, const QStringList &cols)
{
    return parent ? new QTreeWidgetItem(parent, cols) : new QTreeWidgetItem(tree, cols);
}

QString prettyClass(const QString &type)
{
    return type.mid(type.lastIndexOf(QLatin1Char('.')) + 1);
}

} // namespace

ActuatorView::ActuatorView(QWidget *parent) : QWidget(parent)
{
    m_names = {QStringLiteral("health"), QStringLiteral("beans"), QStringLiteral("mappings"), QStringLiteral("env"), QStringLiteral("info"), QStringLiteral("metrics")};
    const QStringList titles = {tr("Health"), tr("Beans"), tr("Mappings"), tr("Environment"), tr("Info"), tr("Metrics")};
    m_endpoints = new QButtonGroup(this);
    auto *segs = new QWidget(this);
    auto *segLayout = new QHBoxLayout(segs);
    segLayout->setContentsMargins(8, 0, 8, 0);
    segLayout->setSpacing(0);
    for (int i = 0; i < titles.size(); ++i) {
        auto *b = new QToolButton(segs);
        b->setText(titles.at(i));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setProperty("seg", true);
        m_endpoints->addButton(b, i);
        segLayout->addWidget(b);
    }
    segLayout->addStretch(1);
    m_endpoints->button(0)->setChecked(true);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter…"));
    m_filter->setClearButtonEnabled(true);
    m_reload = new QToolButton(this);
    m_reload->setAutoRaise(true);
    m_reload->setToolTip(tr("Load again"));
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setOpenExternalLinks(false);
    m_info->setContentsMargins(10, 6, 10, 6);
    m_info->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_tree = new QTreeWidget(this);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(false);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setIndentation(14);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(8, 6, 8, 6);
    bar->setSpacing(4);
    bar->addWidget(m_filter, 1);
    bar->addWidget(m_reload);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addSpacing(2);
    layout->addWidget(segs);
    layout->addLayout(bar);
    layout->addWidget(m_info);
    layout->addWidget(m_tree, 1);

    connect(m_endpoints, &QButtonGroup::idClicked, this, [this] { m_filter->clear(); load(); });
    connect(m_reload, &QToolButton::clicked, this, &ActuatorView::refresh);
    connect(m_filter, &QLineEdit::textChanged, this, &ActuatorView::filter);
    connect(m_info, &QLabel::linkActivated, this, [this](const QString &link) {
        if (link == QLatin1String("expose"))
            exposeAll();
        else if (link == QLatin1String("start"))
            emit startRequested();
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        if (currentEndpoint() != QLatin1String("metrics") || it->parent() || it->childCount() > 0)
            return;
        const QString name = it->text(0);
        fetch(QStringLiteral("metrics"), name, [this, it, name](const QJsonValue &v) {
            const QJsonArray ms = v.toObject().value(QStringLiteral("measurements")).toArray();
            QStringList parts;
            for (const QJsonValue &m : ms)
                parts << QStringLiteral("%1 %2").arg(m.toObject().value(QStringLiteral("statistic")).toString(), scalar(m.toObject().value(QStringLiteral("value"))));
            it->setText(1, parts.join(QStringLiteral(" · ")) + (v.toObject().value(QStringLiteral("baseUnit")).isString() ? QStringLiteral("  ") + v.toObject().value(QStringLiteral("baseUnit")).toString() : QString()));
            Q_UNUSED(name)
        });
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { applyTheme(); });
    applyTheme();
    message(tr("Start the application to inspect it."));
}

QString ActuatorView::currentEndpoint() const
{
    return m_names.value(m_endpoints->checkedId());
}

void ActuatorView::applyTheme()
{
    m_theme = Theme::byName(SettingsManager::instance().theme());
    const Theme &t = m_theme;
    setStyleSheet(QStringLiteral("QTreeWidget { background: %1; color: %2; selection-background-color: %3; border: none; border-top: 1px solid %4; outline: none; }"
                                 "QTreeWidget::item { padding: 2px 0; }"
                                 "QHeaderView::section { background: %1; color: %5; border: none; border-bottom: 1px solid %4; padding: 3px 6px; }"
                                 "QToolButton[seg=\"true\"] { background: transparent; border: none; border-bottom: 2px solid transparent; padding: 4px 9px; color: %5; }"
                                 "QToolButton[seg=\"true\"]:checked { background: transparent; color: %6; border-bottom: 2px solid %6; }"
                                 "QToolButton[seg=\"true\"]:hover:!checked { color: %2; }")
                      .arg(t.editorBg.name(), t.editorFg.name(), t.selection.name(), t.border.name(), t.textMuted.name(), t.accent.name()));
    m_reload->setIcon(Icons::tinted(QStringLiteral(":/new-icons/refresh-cw.svg"), t.editorFg));
    m_info->setStyleSheet(QStringLiteral("QLabel { color: %1; }").arg(t.textMuted.name()));
}

void ActuatorView::setTarget(const QString &base, const QString &resourcesDir, bool running)
{
    const bool changed = base != m_base || running != m_running;
    m_base = base;
    m_resources = resourcesDir;
    const bool started = running && !m_running;
    m_running = running;
    if (!running) {
        message(tr("The application is not running. <a href=\"start\">Start it</a> to inspect it."));
        m_tree->clear();
    } else if (started || (changed && m_tree->topLevelItemCount() == 0)) {
        load();
    }
}

void ActuatorView::refresh()
{
    load();
}

QString ActuatorView::endpointBase() const
{
    const QMap<QString, QString> cfg = SpringProperties::load(m_resources);
    QString base = m_base;
    const int port = cfg.value(QStringLiteral("management.server.port")).toInt();
    if (port > 0) {
        const int colon = base.lastIndexOf(QLatin1Char(':'));
        if (colon > 6)
            base = base.left(colon) + QLatin1Char(':') + QString::number(port);
    }
    QString path = cfg.value(QStringLiteral("management.endpoints.web.base-path"), QStringLiteral("/actuator"));
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return base + (path == QLatin1String("/") ? QString() : path);
}

void ActuatorView::message(const QString &html)
{
    m_info->setText(html);
    m_info->setVisible(!html.isEmpty());
}

void ActuatorView::load()
{
    if (!m_running) {
        message(tr("The application is not running. <a href=\"start\">Start it</a> to inspect it."));
        return;
    }
    const QString ep = currentEndpoint();
    message(tr("Loading…"));
    fetch(ep, {}, [this, ep](const QJsonValue &v) { show(ep, v); });
}

void ActuatorView::fetch(const QString &endpoint, const QString &sub, std::function<void(const QJsonValue &)> done)
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    QString url = endpointBase() + QLatin1Char('/') + endpoint;
    if (!sub.isEmpty())
        url += QLatin1Char('/') + sub;
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Accept", "application/vnd.spring-boot.actuator.v3+json, application/json");
    req.setTransferTimeout(20000);
    const int token = ++m_token;
    m_reply = m_net.get(req);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, token, done, endpoint] {
        reply->deleteLater();
        if (reply == m_reply)
            m_reply = nullptr;
        if (token != m_token)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // Health answers 503 when something is down, but still with a body.
        const QByteArray body = reply->readAll();
        if (status == 0) {
            if (reply->error() == QNetworkReply::ConnectionRefusedError)
                message(tr("Connection refused — the application (or its management port) is not accepting connections."));
            else
                message(tr("Request failed: %1").arg(reply->errorString().toHtmlEscaped()));
            return;
        }
        if (status == 404 || (status == 401 || status == 403)) {
            if (status == 404) {
                message(tr("<b>%1</b> is not available. Either spring-boot-starter-actuator is missing, or the endpoint is not exposed over HTTP "
                           "(only <i>health</i> is by default). <a href=\"expose\">Expose all endpoints</a> in application.properties, then restart.")
                            .arg(endpoint.toHtmlEscaped()));
            } else {
                message(tr("Access denied (%1). Spring Security protects the actuator endpoints.").arg(status));
            }
            m_tree->clear();
            return;
        }
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError) {
            message(tr("The answer (%1) is not JSON.").arg(status));
            return;
        }
        message({});
        done(doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array()));
    });
}

void ActuatorView::show(const QString &endpoint, const QJsonValue &json)
{
    m_tree->setSortingEnabled(false);
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    m_tree->setRootIsDecorated(true);
    if (endpoint == QLatin1String("health"))
        showHealth(json);
    else if (endpoint == QLatin1String("beans"))
        showBeans(json);
    else if (endpoint == QLatin1String("mappings"))
        showMappings(json);
    else if (endpoint == QLatin1String("env"))
        showEnv(json);
    else if (endpoint == QLatin1String("metrics"))
        showMetrics(json);
    else
        showGeneric(json);
    m_tree->setUpdatesEnabled(true);
    for (int i = 0; i < m_tree->columnCount() - 1; ++i)
        m_tree->resizeColumnToContents(i);
    filter(m_filter->text());
}

void ActuatorView::showHealth(const QJsonValue &v)
{
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Component"), tr("Status")});
    auto statusColour = [this](const QString &s) { return s == QLatin1String("UP") ? m_theme.success : s == QLatin1String("DOWN") || s == QLatin1String("OUT_OF_SERVICE") ? m_theme.danger : m_theme.warning; };
    std::function<void(QTreeWidgetItem *, const QString &, const QJsonObject &)> add = [&](QTreeWidgetItem *parent, const QString &name, const QJsonObject &o) {
        const QString status = o.value(QStringLiteral("status")).toString();
        QTreeWidgetItem *it = row(m_tree, parent, {name, status});
        it->setForeground(1, statusColour(status));
        const QJsonObject details = o.value(QStringLiteral("details")).toObject();
        for (auto d = details.begin(); d != details.end(); ++d)
            row(m_tree, it, {d.key(), scalar(d.value())});
        const QJsonObject comps = o.value(QStringLiteral("components")).toObject();
        for (auto c = comps.begin(); c != comps.end(); ++c)
            add(it, c.key(), c.value().toObject());
        it->setExpanded(true);
    };
    add(nullptr, tr("application"), v.toObject());
}

void ActuatorView::showBeans(const QJsonValue &v)
{
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Bean"), tr("Type"), tr("Scope")});
    const QJsonObject contexts = v.toObject().value(QStringLiteral("contexts")).toObject();
    for (auto c = contexts.begin(); c != contexts.end(); ++c) {
        const QJsonObject beans = c.value().toObject().value(QStringLiteral("beans")).toObject();
        for (auto b = beans.begin(); b != beans.end(); ++b) {
            const QJsonObject o = b.value().toObject();
            QTreeWidgetItem *it = row(m_tree, nullptr, {b.key(), prettyClass(o.value(QStringLiteral("type")).toString()), o.value(QStringLiteral("scope")).toString()});
            it->setToolTip(1, o.value(QStringLiteral("type")).toString());
            if (o.value(QStringLiteral("resource")).isString())
                row(m_tree, it, {tr("resource"), o.value(QStringLiteral("resource")).toString(), {}});
            const QJsonArray deps = o.value(QStringLiteral("dependencies")).toArray();
            if (!deps.isEmpty()) {
                QStringList names;
                for (const QJsonValue &d : deps)
                    names << d.toString();
                row(m_tree, it, {tr("depends on"), names.join(QStringLiteral(", ")), {}});
            }
        }
    }
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    m_tree->setSortingEnabled(true);
    message(tr("%n bean(s)", nullptr, m_tree->topLevelItemCount()));
}

void ActuatorView::showMappings(const QJsonValue &v)
{
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Path"), tr("Methods"), tr("Handler")});
    m_tree->setRootIsDecorated(false);
    const QJsonObject contexts = v.toObject().value(QStringLiteral("contexts")).toObject();
    for (auto c = contexts.begin(); c != contexts.end(); ++c) {
        const QJsonObject mappings = c.value().toObject().value(QStringLiteral("mappings")).toObject();
        const QJsonObject servlets = mappings.value(QStringLiteral("dispatcherServlets")).toObject();
        const QJsonObject handlers = mappings.value(QStringLiteral("dispatcherHandlers")).toObject();
        for (const QJsonObject &group : {servlets, handlers}) {
            for (auto g = group.begin(); g != group.end(); ++g) {
                for (const QJsonValue &mv : g.value().toArray()) {
                    const QJsonObject m = mv.toObject();
                    const QJsonObject conds = m.value(QStringLiteral("details")).toObject().value(QStringLiteral("requestMappingConditions")).toObject();
                    QStringList patterns, methods;
                    for (const QJsonValue &p : conds.value(QStringLiteral("patterns")).toArray())
                        patterns << p.toString();
                    for (const QJsonValue &p : conds.value(QStringLiteral("methods")).toArray())
                        methods << p.toString();
                    if (patterns.isEmpty()) // functional routes and resource handlers only have a predicate
                        patterns << m.value(QStringLiteral("predicate")).toString();
                    QString handler = m.value(QStringLiteral("handler")).toString();
                    const QJsonObject hd = m.value(QStringLiteral("details")).toObject().value(QStringLiteral("handlerMethod")).toObject();
                    if (hd.contains(QStringLiteral("className")))
                        handler = prettyClass(hd.value(QStringLiteral("className")).toString()) + QLatin1Char('.') + hd.value(QStringLiteral("name")).toString();
                    row(m_tree, nullptr, {patterns.join(QStringLiteral(", ")), methods.isEmpty() ? QStringLiteral("*") : methods.join(QLatin1Char(',')), handler});
                }
            }
        }
    }
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    message(tr("%n mapping(s)", nullptr, m_tree->topLevelItemCount()));
}

void ActuatorView::showEnv(const QJsonValue &v)
{
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Property"), tr("Value")});
    const QJsonObject o = v.toObject();
    QStringList profiles;
    for (const QJsonValue &p : o.value(QStringLiteral("activeProfiles")).toArray())
        profiles << p.toString();
    if (!profiles.isEmpty())
        message(tr("Active profiles: <b>%1</b>").arg(profiles.join(QStringLiteral(", ")).toHtmlEscaped()));
    for (const QJsonValue &sv : o.value(QStringLiteral("propertySources")).toArray()) {
        const QJsonObject src = sv.toObject();
        const QJsonObject props = src.value(QStringLiteral("properties")).toObject();
        QTreeWidgetItem *group = row(m_tree, nullptr, {src.value(QStringLiteral("name")).toString(), tr("%n property(ies)", nullptr, props.size())});
        for (auto p = props.begin(); p != props.end(); ++p)
            row(m_tree, group, {p.key(), scalar(p.value().toObject().value(QStringLiteral("value")))});
    }
}

void ActuatorView::showMetrics(const QJsonValue &v)
{
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Metric"), tr("Value (double-click)")});
    m_tree->setRootIsDecorated(false);
    QStringList names;
    for (const QJsonValue &n : v.toObject().value(QStringLiteral("names")).toArray())
        names << n.toString();
    names.sort();
    for (const QString &n : std::as_const(names))
        row(m_tree, nullptr, {n, {}});
}

void ActuatorView::showGeneric(const QJsonValue &v)
{
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Key"), tr("Value")});
    std::function<void(QTreeWidgetItem *, const QString &, const QJsonValue &)> add = [&](QTreeWidgetItem *parent, const QString &key, const QJsonValue &val) {
        if (val.isObject()) {
            QTreeWidgetItem *it = row(m_tree, parent, {key, {}});
            const QJsonObject o = val.toObject();
            for (auto e = o.begin(); e != o.end(); ++e)
                add(it, e.key(), e.value());
            it->setExpanded(true);
        } else {
            row(m_tree, parent, {key, scalar(val)});
        }
    };
    const QJsonObject o = v.toObject();
    for (auto e = o.begin(); e != o.end(); ++e)
        add(nullptr, e.key(), e.value());
    if (o.isEmpty())
        message(tr("Nothing to show (the application publishes no info)."));
}

void ActuatorView::filter(const QString &text)
{
    std::function<bool(QTreeWidgetItem *)> apply = [&](QTreeWidgetItem *it) {
        bool match = text.isEmpty();
        for (int c = 0; c < m_tree->columnCount() && !match; ++c)
            match = it->text(c).contains(text, Qt::CaseInsensitive);
        bool childMatch = false;
        for (int i = 0; i < it->childCount(); ++i)
            childMatch = apply(it->child(i)) || childMatch;
        const bool visible = match || childMatch;
        it->setHidden(!visible);
        if (childMatch && !text.isEmpty())
            it->setExpanded(true);
        return visible;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        apply(m_tree->topLevelItem(i));
}

void ActuatorView::exposeAll()
{
    const QString file = m_resources + QStringLiteral("/application.properties");
    const bool hasYaml = QFile::exists(m_resources + QStringLiteral("/application.yml")) || QFile::exists(m_resources + QStringLiteral("/application.yaml"));
    if (!QFile::exists(file) && hasYaml) {
        // YAML has to be edited by hand (nesting); open it with the line to add in the message.
        message(tr("Add <tt>management.endpoints.web.exposure.include: \"*\"</tt> to your application.yml, then restart."));
        emit openFileRequested(QFile::exists(m_resources + QStringLiteral("/application.yml")) ? m_resources + QStringLiteral("/application.yml") : m_resources + QStringLiteral("/application.yaml"));
        return;
    }
    QFile f(file);
    if (!f.open(QIODevice::ReadWrite | QIODevice::Text)) {
        message(tr("Could not write %1.").arg(file.toHtmlEscaped()));
        return;
    }
    const QByteArray existing = f.readAll();
    if (existing.contains("management.endpoints.web.exposure.include")) {
        f.close();
        message(tr("management.endpoints.web.exposure.include is already set in application.properties — restart the application."));
        emit openFileRequested(file);
        return;
    }
    f.seek(f.size());
    f.write(QByteArray(existing.isEmpty() || existing.endsWith('\n') ? "" : "\n") + "management.endpoints.web.exposure.include=*\n");
    f.close();
    message(tr("Added <tt>management.endpoints.web.exposure.include=*</tt> to application.properties. Restart the application to apply it."));
    emit openFileRequested(file);
}
