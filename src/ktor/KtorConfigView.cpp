#include "ktor/KtorConfigView.h"

#include "ktor/KtorPlugins.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kFileRole = Qt::UserRole;
constexpr int kLineRole = Qt::UserRole + 1;

// Ktor modules the build names: ktorLibs.server.cors, libs.ktor.server.cors (through the catalog) or io.ktor:ktor-…
QStringList declaredModules(const KtorProject &project)
{
    const QString text = project.buildText();
    QStringList out;
    static const QRegularExpression accessor(QStringLiteral(R"(ktorLibs\.((?:server|serialization|client)\.[\w.]+))"));
    for (QRegularExpressionMatchIterator it = accessor.globalMatch(text); it.hasNext();) {
        QString key = it.next().captured(1);
        key.replace(QLatin1Char('.'), QLatin1Char('-'));
        out << KtorPlugins::artifactOf(key);
    }
    static const QRegularExpression coordinate(QStringLiteral(R"((?:io\.ktor:|name\s*=\s*")(ktor-(?:server|serialization|client)-[\w-]+))"));
    for (QRegularExpressionMatchIterator it = coordinate.globalMatch(text); it.hasNext();)
        out << it.next().captured(1);
    static const QRegularExpression maven(QStringLiteral(R"(<artifactId>\s*(ktor-(?:server|serialization|client)-[\w-]+)\s*</artifactId>)"));
    for (QRegularExpressionMatchIterator it = maven.globalMatch(text); it.hasNext();)
        out << it.next().captured(1);
    for (QString &m : out)
        if (m.endsWith(QLatin1String("-jvm")))
            m.chop(4);
    out.removeDuplicates();
    out.sort();
    return out;
}

bool isSecretKey(const QString &key)
{
    static const QRegularExpression secret(QStringLiteral(R"((?:secret|password|passwd|token|apikey|api-key|private))"), QRegularExpression::CaseInsensitiveOption);
    return secret.match(key.section(QLatin1Char('.'), -1)).hasMatch();
}
} // namespace

KtorConfigView::KtorConfigView(QWidget *parent) : QWidget(parent)
{
    m_add = new QPushButton(tr("Add Plugins…"), this);
    m_add->setToolTip(tr("Add Ktor server plugins (CORS, JSON, authentication …) to the build"));
    m_settings = new QPushButton(tr("Run Settings…"), this);
    m_settings->setToolTip(tr("Port, development mode, arguments, JVM options and environment of the run"));
    m_refresh = new QToolButton(this);
    m_refresh->setAutoRaise(true);
    m_refresh->setToolTip(tr("Read the configuration and the sources again"));
    for (QPushButton *b : {m_add, m_settings})
        b->setAutoDefault(false);

    auto *tools = new QWidget(this);
    auto *tl = new QHBoxLayout(tools);
    tl->setContentsMargins(8, 4, 6, 4);
    tl->setSpacing(4);
    tl->addWidget(m_add);
    tl->addWidget(m_settings);
    tl->addStretch(1);
    tl->addWidget(m_refresh);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(14);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("emptyText"));
    m_note->setWordWrap(true);
    m_note->setContentsMargins(10, 3, 10, 4);
    m_note->setStyleSheet(QStringLiteral("QLabel { border-top: 1px solid palette(shadow); }"));
    m_note->setText(tr("Double-click a row with a source to open it."));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tools);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_note);

    connect(m_add, &QPushButton::clicked, this, &KtorConfigView::addPluginsRequested);
    connect(m_settings, &QPushButton::clicked, this, &KtorConfigView::settingsRequested);
    connect(m_refresh, &QToolButton::clicked, this, &KtorConfigView::refreshRequested);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        const QString file = it->data(0, kFileRole).toString();
        if (!file.isEmpty())
            emit locationRequested(file, qMax(1, it->data(0, kLineRole).toInt()), 1);
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &KtorConfigView::applyTheme);
    applyTheme();
}

void KtorConfigView::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_refresh->setIcon(Icons::tinted(QStringLiteral(":/new-icons/refresh-cw.svg"), t.editorFg));
    m_add->setIcon(Icons::tinted(QStringLiteral(":/new-icons/package.svg"), t.editorFg));
    m_settings->setIcon(Icons::tinted(QStringLiteral(":/new-icons/cog.svg"), t.editorFg));
    // Values in the muted colour: the names carry the structure.
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if ((*it)->parent())
            (*it)->setForeground(1, t.textMuted);
}

QTreeWidgetItem *KtorConfigView::section(const QString &title, int count)
{
    auto *s = new QTreeWidgetItem(m_tree, QStringList{count >= 0 ? QStringLiteral("%1  %2").arg(title).arg(count) : title});
    QFont f = s->font(0);
    f.setBold(true);
    s->setFont(0, f);
    s->setFirstColumnSpanned(true);
    s->setFlags(Qt::ItemIsEnabled);
    s->setExpanded(true);
    return s;
}

QTreeWidgetItem *KtorConfigView::row(QTreeWidgetItem *parent, const QString &name, const QString &value, const QString &file, int line)
{
    auto *r = new QTreeWidgetItem(parent, QStringList{name, value});
    r->setForeground(1, Theme::byName(SettingsManager::instance().theme()).textMuted);
    if (!file.isEmpty()) {
        r->setData(0, kFileRole, file);
        r->setData(0, kLineRole, line);
        const QString where = line > 0 ? QStringLiteral("%1:%2").arg(QFileInfo(file).fileName()).arg(line) : QFileInfo(file).fileName();
        r->setToolTip(0, where);
        r->setToolTip(1, value.isEmpty() ? where : value + QLatin1Char('\n') + where);
    } else if (!value.isEmpty()) {
        r->setToolTip(1, value);
    }
    return r;
}

void KtorConfigView::setData(const KtorProject &project, const KtorProject::Options &options, const KtorScan &scan, const Port &port, bool running,
                             int runningPort, const QString &reload, const QString &routes)
{
    // Keep the sections the user collapsed.
    QSet<QString> collapsed;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        if (!m_tree->topLevelItem(i)->isExpanded())
            collapsed << m_tree->topLevelItem(i)->text(0).section(QStringLiteral("  "), 0, 0);
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    if (!project.valid) {
        m_tree->setUpdatesEnabled(true);
        return;
    }
    const QString config = project.configFile();
    const QVector<KtorProject::ConfigEntry> entries = KtorProject::readConfig(config);
    auto valueOf = [&entries](const QString &key, int *line = nullptr) {
        for (const KtorProject::ConfigEntry &e : entries)
            if (e.key == key) {
                if (line)
                    *line = e.line;
                return e.value;
            }
        return QString();
    };

    QTreeWidgetItem *server = section(tr("Server"));
    row(server, tr("Engine"), !scan.embeddedEngine.isEmpty() ? scan.embeddedEngine : !project.engine.isEmpty() ? project.engine : tr("not named in the build"),
        scan.embeddedFile, scan.embeddedLine);
    QString portText = running && runningPort > 0 ? tr("%1 (running)").arg(runningPort) : QString::number(port.port);
    portText += QStringLiteral(" · ") + port.source;
    if (!port.raw.isEmpty() && port.raw != QString::number(port.port))
        portText += QStringLiteral(" (%1)").arg(port.raw);
    int portLine = 0;
    valueOf(QStringLiteral("ktor.deployment.port"), &portLine);
    row(server, tr("Port"), portText, portLine > 0 ? config : scan.embeddedFile, portLine > 0 ? portLine : scan.embeddedLine);
    int hostLine = 0;
    const QString host = valueOf(QStringLiteral("ktor.deployment.host"), &hostLine);
    row(server, tr("Host"), host.isEmpty() ? tr("0.0.0.0 (all interfaces)") : host, hostLine > 0 ? config : QString(), hostLine);
    int devLine = 0;
    const QString devConfig = valueOf(QStringLiteral("ktor.development"), &devLine);
    const QString dev = options.autoReload ? tr("on (auto-reload)") : options.development ? tr("on (run settings)")
                        : devConfig == QLatin1String("true") ? tr("on (configuration)") : tr("off");
    row(server, tr("Development mode"), dev, devLine > 0 ? config : QString(), devLine);
    row(server, tr("Auto-reload"), reload, !scan.embeddedEngine.isEmpty() ? scan.embeddedFile : QString(), scan.embeddedLine);
    row(server, tr("Routes"), routes);
    const QString watch = valueOf(QStringLiteral("ktor.deployment.watch"));
    if (!watch.isEmpty())
        row(server, tr("Watched for reload"), watch);
    if (!project.version.isEmpty())
        row(server, tr("Ktor"), project.version, project.buildFile());
    row(server, tr("Build"), (project.maven ? tr("Maven") : tr("Gradle")) + QStringLiteral(" · ") + QFileInfo(project.buildFile()).fileName(), project.buildFile());
    QString start;
    // EngineMain reads the modules from the configuration; a main() of the project usually just hands over to it.
    const bool engineMain = project.mainClass.contains(QStringLiteral("EngineMain")) || !valueOf(QStringLiteral("ktor.application.modules")).isEmpty();
    if (engineMain && scan.embeddedEngine.isEmpty())
        start = tr("EngineMain with %1").arg(config.isEmpty() ? tr("a configuration file") : QFileInfo(config).fileName());
    else if (!scan.embeddedEngine.isEmpty())
        start = tr("embeddedServer(%1) in code").arg(scan.embeddedEngine);
    else if (!project.mainClass.isEmpty())
        start = project.mainClass;
    if (!start.isEmpty())
        row(server, tr("Starts with"), start, !scan.embeddedEngine.isEmpty() ? scan.embeddedFile : QString(), scan.embeddedLine);
    if (!config.isEmpty())
        row(server, tr("Configuration"), QFileInfo(config).fileName(), config, 1);

    // Modules the configuration loads, and the Application functions that could be.
    const QStringList listed = valueOf(QStringLiteral("ktor.application.modules")).split(QStringLiteral(", "), Qt::SkipEmptyParts);
    QTreeWidgetItem *modules = section(tr("Modules"), int(scan.modules.size()));
    for (const KtorModuleFunction &m : scan.modules) {
        const bool loaded = listed.contains(m.fqn);
        QTreeWidgetItem *r = row(modules, m.name, loaded ? tr("loaded by the configuration") : QFileInfo(m.file).fileName(), m.file, m.line);
        r->setToolTip(0, m.fqn);
        if (loaded) {
            QFont f = r->font(0);
            f.setBold(true);
            r->setFont(0, f);
        }
    }
    for (const QString &fqn : listed) {
        bool found = false;
        for (const KtorModuleFunction &m : scan.modules)
            found = found || m.fqn == fqn;
        if (!found)
            row(modules, fqn.section(QLatin1Char('.'), -1), tr("listed in the configuration but not found: %1").arg(fqn), config,
                [&] { int l = 0; valueOf(QStringLiteral("ktor.application.modules"), &l); return l; }());
    }
    if (scan.modules.isEmpty() && listed.isEmpty())
        row(modules, tr("none"), tr("fun Application.module() { … } functions show up here"));

    QTreeWidgetItem *plugins = section(tr("Installed plugins"), int(scan.installs.size()));
    for (const KtorInstall &i : scan.installs)
        row(plugins, i.plugin, QStringLiteral("%1 · %2:%3").arg(i.function, QFileInfo(i.file).fileName()).arg(i.line), i.file, i.line);
    if (scan.installs.isEmpty())
        row(plugins, tr("none"), tr("install(Plugin) calls show up here; Add Plugins… adds their modules to the build"));

    if (!scan.authProviders.isEmpty()) {
        QTreeWidgetItem *auth = section(tr("Authentication"), int(scan.authProviders.size()));
        for (const KtorAuthProvider &a : scan.authProviders)
            row(auth, a.name.isEmpty() ? tr("(default)") : a.name, QStringLiteral("%1 · %2:%3").arg(a.type, QFileInfo(a.file).fileName()).arg(a.line), a.file, a.line);
    }

    if (!config.isEmpty()) {
        QTreeWidgetItem *values = section(QFileInfo(config).fileName(), int(entries.size()));
        for (const KtorProject::ConfigEntry &e : entries) {
            // Secrets stay readable in the file itself, not in a panel someone may be looking at.
            const QString shown = isSecretKey(e.key) && !e.value.startsWith(QLatin1Char('$')) && !e.value.isEmpty() ? QStringLiteral("••••••") : e.value;
            row(values, e.key, shown, config, e.line);
        }
    }

    const QStringList deps = declaredModules(project);
    QTreeWidgetItem *depSection = section(tr("Ktor modules in the build"), int(deps.size()));
    for (const QString &d : deps)
        row(depSection, d, QString(), project.buildFile());

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        if (collapsed.contains(m_tree->topLevelItem(i)->text(0).section(QStringLiteral("  "), 0, 0)))
            m_tree->topLevelItem(i)->setExpanded(false);
    m_tree->setUpdatesEnabled(true);
}
