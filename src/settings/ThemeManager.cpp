#include "ThemeManager.h"

#include "SettingsManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

bool ThemeManager::isBuiltin(const QString &id)
{
    return id == QLatin1String("dark") || id == QLatin1String("light") || id == QLatin1String("darcula");
}

ThemeManager &ThemeManager::instance()
{
    static ThemeManager m;
    return m;
}

ThemeManager::ThemeManager()
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        reload();
        emit listChanged();
        announce();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    reload();
}

QString ThemeManager::themesDir() const
{
    return QFileInfo(SettingsManager::instance().configFilePath()).absolutePath() + QStringLiteral("/themes");
}

void ThemeManager::reload()
{
    m_user.clear();
    m_order.clear();
    const QDir dir(themesDir());
    QStringList watched;
    for (const QFileInfo &fi : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name | QDir::IgnoreCase)) {
        const QString id = fi.completeBaseName();
        if (isBuiltin(id))
            continue;
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        Theme t = Theme::fromJson(doc.object());
        if (doc.object().value(QStringLiteral("name")).toString().trimmed().isEmpty())
            t.name = id;
        m_user.insert(id, t);
        m_order.append(id);
        watched << fi.absoluteFilePath();
    }
    // Watch the folder (new files) and each file (in-place edits); the folder may not exist yet.
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (dir.exists())
        watched << dir.absolutePath();
    else
        watched << QFileInfo(themesDir()).absolutePath();
    m_watcher.addPaths(watched);
}

void ThemeManager::announce()
{
    ++m_revision;
    SettingsManager::instance().refreshTheme();
}

QList<ThemeManager::Info> ThemeManager::themes() const
{
    QList<Info> out;
    out.append({QStringLiteral("dark"), QStringLiteral("Dark"), true, true});
    out.append({QStringLiteral("light"), QStringLiteral("Light"), true, false});
    out.append({QStringLiteral("darcula"), QStringLiteral("Darcula"), true, true});
    for (const QString &id : m_order) {
        const Theme &t = m_user.value(id);
        out.append({id, t.name, false, t.dark});
    }
    return out;
}

bool ThemeManager::contains(const QString &id) const
{
    return isBuiltin(id) || m_user.contains(id);
}

Theme ThemeManager::theme(const QString &id) const
{
    if (m_previewing)
        return m_preview;
    if (id == QLatin1String("light"))
        return Theme::light_();
    if (id == QLatin1String("darcula"))
        return Theme::darcula_();
    const auto it = m_user.constFind(id);
    return it != m_user.constEnd() ? it.value() : Theme::dark_();
}

QString ThemeManager::uniqueId(const QString &name) const
{
    QString base;
    for (const QChar c : name.trimmed().toLower())
        base += (c.isLetterOrNumber() ? c : QLatin1Char('-'));
    while (base.contains(QLatin1String("--")))
        base.replace(QLatin1String("--"), QLatin1String("-"));
    base = base.mid(base.startsWith(QLatin1Char('-')) ? 1 : 0);
    while (base.endsWith(QLatin1Char('-')))
        base.chop(1);
    if (base.isEmpty())
        base = QStringLiteral("theme");
    QString id = base;
    for (int n = 2; contains(id); ++n)
        id = base + QLatin1Char('-') + QString::number(n);
    return id;
}

QString ThemeManager::save(const Theme &theme, const QString &id)
{
    const QString useId = (id.isEmpty() || isBuiltin(id)) ? uniqueId(theme.name) : id;
    QDir().mkpath(themesDir());
    QSaveFile f(themesDir() + QLatin1Char('/') + useId + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(QJsonDocument(theme.toJson()).toJson(QJsonDocument::Indented));
    if (!f.commit())
        return {};
    m_debounce.stop();
    reload();
    emit listChanged();
    announce();
    return useId;
}

bool ThemeManager::remove(const QString &id)
{
    if (isBuiltin(id) || !m_user.contains(id))
        return false;
    if (!QFile::remove(themesDir() + QLatin1Char('/') + id + QStringLiteral(".json")))
        return false;
    m_debounce.stop();
    reload();
    emit listChanged();
    announce();
    return true;
}

void ThemeManager::setPreview(const Theme &theme)
{
    m_preview = theme;
    m_previewing = true;
    announce();
}

void ThemeManager::clearPreview()
{
    if (!m_previewing)
        return;
    m_previewing = false;
    announce();
}
