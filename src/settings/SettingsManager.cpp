#include "SettingsManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

SettingsManager::SettingsManager()
    : m_settings(QStringLiteral("QODE"), QStringLiteral("QODE"))
{
}

SettingsManager &SettingsManager::instance()
{
    static SettingsManager s;
    return s;
}

QFont SettingsManager::editorFont() const
{
    QFont f(m_settings.value(QStringLiteral("editor/fontFamily"), QStringLiteral("JetBrains Mono")).toString());
    if (!QFontDatabase::families().contains(f.family()))
        f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(m_settings.value(QStringLiteral("editor/fontSize"), 11).toInt());
    f.setStyleHint(QFont::Monospace);
    return f;
}

void SettingsManager::setEditorFont(const QFont &font)
{
    m_settings.setValue(QStringLiteral("editor/fontFamily"), font.family());
    m_settings.setValue(QStringLiteral("editor/fontSize"), font.pointSize());
    emit editorSettingsChanged();
}

int SettingsManager::tabSize() const
{
    return qBound(1, projectValue(QStringLiteral("tabSize"), m_settings.value(QStringLiteral("editor/tabSize"), 4)).toInt(), 16);
}

void SettingsManager::setTabSize(int size)
{
    setScopedValue(QStringLiteral("editor/tabSize"), QStringLiteral("tabSize"), size);
    emit editorSettingsChanged();
}

bool SettingsManager::useSpaces() const
{
    return projectValue(QStringLiteral("useSpaces"), m_settings.value(QStringLiteral("editor/useSpaces"), true)).toBool();
}

void SettingsManager::setUseSpaces(bool on)
{
    setScopedValue(QStringLiteral("editor/useSpaces"), QStringLiteral("useSpaces"), on);
    emit editorSettingsChanged();
}

bool SettingsManager::wordWrap() const
{
    return m_settings.value(QStringLiteral("editor/wordWrap"), false).toBool();
}

void SettingsManager::setWordWrap(bool on)
{
    m_settings.setValue(QStringLiteral("editor/wordWrap"), on);
    emit editorSettingsChanged();
}

int SettingsManager::autoSaveMode() const
{
    return qBound(0, m_settings.value(QStringLiteral("save/autoSave"), 0).toInt(), 2);
}

void SettingsManager::setAutoSaveMode(int mode)
{
    m_settings.setValue(QStringLiteral("save/autoSave"), mode);
    emit saveSettingsChanged();
}

int SettingsManager::autoSaveDelayMs() const
{
    return qBound(200, m_settings.value(QStringLiteral("save/autoSaveDelayMs"), 1000).toInt(), 60000);
}

bool SettingsManager::trimTrailingWhitespace() const
{
    return m_settings.value(QStringLiteral("save/trimTrailingWhitespace"), false).toBool();
}

void SettingsManager::setTrimTrailingWhitespace(bool on)
{
    m_settings.setValue(QStringLiteral("save/trimTrailingWhitespace"), on);
    emit saveSettingsChanged();
}

bool SettingsManager::insertFinalNewline() const
{
    return m_settings.value(QStringLiteral("save/insertFinalNewline"), false).toBool();
}

void SettingsManager::setInsertFinalNewline(bool on)
{
    m_settings.setValue(QStringLiteral("save/insertFinalNewline"), on);
    emit saveSettingsChanged();
}

bool SettingsManager::autoActivateVenv() const
{
    return projectValue(QStringLiteral("autoActivateVenv"), m_settings.value(QStringLiteral("terminal/autoActivateVenv"), true)).toBool();
}

void SettingsManager::setAutoActivateVenv(bool on)
{
    setScopedValue(QStringLiteral("terminal/autoActivateVenv"), QStringLiteral("autoActivateVenv"), on);
    emit projectSettingsChanged();
}

bool SettingsManager::formatOnSave() const
{
    return projectValue(QStringLiteral("formatOnSave"), m_settings.value(QStringLiteral("save/formatOnSave"), false)).toBool();
}

void SettingsManager::setFormatOnSave(bool on)
{
    setScopedValue(QStringLiteral("save/formatOnSave"), QStringLiteral("formatOnSave"), on);
    emit saveSettingsChanged();
}

bool SettingsManager::showBreadcrumbs() const
{
    return m_settings.value(QStringLiteral("editor/breadcrumbs"), true).toBool();
}

void SettingsManager::setShowBreadcrumbs(bool on)
{
    m_settings.setValue(QStringLiteral("editor/breadcrumbs"), on);
    emit editorSettingsChanged();
}

bool SettingsManager::showMinimap() const
{
    return m_settings.value(QStringLiteral("editor/minimap"), true).toBool();
}

void SettingsManager::setShowMinimap(bool on)
{
    m_settings.setValue(QStringLiteral("editor/minimap"), on);
    emit editorSettingsChanged();
}

QHash<QString, QList<int>> SettingsManager::bookmarks() const
{
    QHash<QString, QList<int>> out;
    const QJsonObject o = m_data.value(QStringLiteral("bookmarks")).toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        QList<int> lines;
        for (const QJsonValue &v : it.value().toArray())
            lines.append(v.toInt());
        if (!lines.isEmpty())
            out.insert(it.key(), lines);
    }
    return out;
}

void SettingsManager::setBookmarks(const QHash<QString, QList<int>> &bookmarks)
{
    QJsonObject o;
    for (auto it = bookmarks.begin(); it != bookmarks.end(); ++it) {
        QJsonArray a;
        for (int l : it.value())
            a.append(l);
        if (!a.isEmpty())
            o.insert(it.key(), a);
    }
    m_data.insert(QStringLiteral("bookmarks"), o);
    saveProject();
}

bool SettingsManager::blameInline() const
{
    return m_settings.value(QStringLiteral("git/blameInline"), true).toBool();
}

void SettingsManager::setBlameInline(bool on)
{
    m_settings.setValue(QStringLiteral("git/blameInline"), on);
    emit editorSettingsChanged();
}

bool SettingsManager::blameGutter() const
{
    return m_settings.value(QStringLiteral("git/blameGutter"), false).toBool();
}

void SettingsManager::setBlameGutter(bool on)
{
    m_settings.setValue(QStringLiteral("git/blameGutter"), on);
    emit editorSettingsChanged();
}

bool SettingsManager::stickyScroll() const
{
    return m_settings.value(QStringLiteral("editor/stickyScroll"), true).toBool();
}

void SettingsManager::setStickyScroll(bool on)
{
    m_settings.setValue(QStringLiteral("editor/stickyScroll"), on);
    emit editorSettingsChanged();
}

bool SettingsManager::indentGuides() const
{
    return m_settings.value(QStringLiteral("editor/indentGuides"), true).toBool();
}

void SettingsManager::setIndentGuides(bool on)
{
    m_settings.setValue(QStringLiteral("editor/indentGuides"), on);
    emit editorSettingsChanged();
}

QString SettingsManager::theme() const
{
    const QString local = m_data.value(QStringLiteral("theme")).toString();
    if (local == QLatin1String("dark") || local == QLatin1String("light"))
        return local;
    return m_settings.value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString();
}

void SettingsManager::setTheme(const QString &theme)
{
    if (m_projectRoot.isEmpty()) {
        m_settings.setValue(QStringLiteral("ui/theme"), theme);
    } else {
        m_data.insert(QStringLiteral("theme"), theme);
        saveProject();
    }
    emit themeChanged(theme);
}

QByteArray SettingsManager::windowGeometry() const
{
    return m_settings.value(QStringLiteral("window/geometry")).toByteArray();
}

void SettingsManager::setWindowGeometry(const QByteArray &g)
{
    m_settings.setValue(QStringLiteral("window/geometry"), g);
}

QByteArray SettingsManager::windowState() const
{
    return m_settings.value(QStringLiteral("window/state")).toByteArray();
}

void SettingsManager::setWindowState(const QByteArray &s)
{
    m_settings.setValue(QStringLiteral("window/state"), s);
}

int SettingsManager::explorerWidth() const
{
    return m_settings.value(QStringLiteral("window/explorerWidth"), 250).toInt();
}

void SettingsManager::setExplorerWidth(int w)
{
    m_settings.setValue(QStringLiteral("window/explorerWidth"), w);
}

int SettingsManager::terminalHeight() const
{
    return m_settings.value(QStringLiteral("window/terminalHeight"), 240).toInt();
}

void SettingsManager::setTerminalHeight(int h)
{
    m_settings.setValue(QStringLiteral("window/terminalHeight"), h);
}

bool SettingsManager::terminalVisible() const
{
    return m_settings.value(QStringLiteral("window/terminalVisible"), false).toBool();
}

void SettingsManager::setTerminalVisible(bool v)
{
    m_settings.setValue(QStringLiteral("window/terminalVisible"), v);
}

QString SettingsManager::lastProject() const
{
    return m_settings.value(QStringLiteral("session/project")).toString();
}

void SettingsManager::setLastProject(const QString &p)
{
    m_settings.setValue(QStringLiteral("session/project"), p);
}

QStringList SettingsManager::openFiles() const
{
    QStringList out;
    for (const QJsonValue &v : m_data.value(QStringLiteral("session")).toObject().value(QStringLiteral("openFiles")).toArray())
        out.append(v.toString());
    return out;
}

void SettingsManager::setOpenFiles(const QStringList &f)
{
    QJsonObject s = m_data.value(QStringLiteral("session")).toObject();
    s.insert(QStringLiteral("openFiles"), QJsonArray::fromStringList(f));
    m_data.insert(QStringLiteral("session"), s);
    saveProject();
}

QString SettingsManager::activeFile() const
{
    return m_data.value(QStringLiteral("session")).toObject().value(QStringLiteral("activeFile")).toString();
}

void SettingsManager::setActiveFile(const QString &f)
{
    QJsonObject s = m_data.value(QStringLiteral("session")).toObject();
    s.insert(QStringLiteral("activeFile"), f);
    m_data.insert(QStringLiteral("session"), s);
    saveProject();
}

QString SettingsManager::lastDirectory() const
{
    return m_settings.value(QStringLiteral("session/lastDir")).toString();
}

void SettingsManager::setLastDirectory(const QString &d)
{
    m_settings.setValue(QStringLiteral("session/lastDir"), d);
}

QString SettingsManager::runCommand(const QString &key) const
{
    return m_data.value(QStringLiteral("run")).toObject().value(key).toString();
}

void SettingsManager::setRunCommand(const QString &key, const QString &command)
{
    QJsonObject r = m_data.value(QStringLiteral("run")).toObject();
    if (command.isEmpty())
        r.remove(key);
    else
        r.insert(key, command);
    m_data.insert(QStringLiteral("run"), r);
    saveProject();
}

QStringList SettingsManager::recentCommands() const
{
    return m_settings.value(QStringLiteral("session/recentCommands")).toStringList();
}

void SettingsManager::setRecentCommands(const QStringList &c)
{
    m_settings.setValue(QStringLiteral("session/recentCommands"), c);
}

QStringList SettingsManager::recentProjects() const
{
    return m_settings.value(QStringLiteral("session/recentProjects")).toStringList();
}

void SettingsManager::addRecentProject(const QString &path)
{
    QStringList list = recentProjects();
    list.removeAll(path);
    list.prepend(path);
    m_settings.setValue(QStringLiteral("session/recentProjects"), QStringList(list.mid(0, 10)));
    emit recentProjectsChanged();
}

void SettingsManager::removeRecentProject(const QString &path)
{
    QStringList list = recentProjects();
    if (list.removeAll(path) > 0) {
        m_settings.setValue(QStringLiteral("session/recentProjects"), list);
        emit recentProjectsChanged();
    }
}

void SettingsManager::clearRecentProjects()
{
    m_settings.remove(QStringLiteral("session/recentProjects"));
    emit recentProjectsChanged();
}

// --- Per-project data ---------------------------------------------------------

// Project overrides live under "settings" in the project file; `fallback` is the global value.
QVariant SettingsManager::projectValue(const QString &key, const QVariant &fallback) const
{
    const QJsonValue v = m_data.value(QStringLiteral("settings")).toObject().value(key);
    return v.isUndefined() || v.isNull() ? fallback : v.toVariant();
}

// With a project open the value becomes that project's override, otherwise the global default.
void SettingsManager::setScopedValue(const QString &globalKey, const QString &projectKey, const QVariant &value)
{
    if (m_projectRoot.isEmpty()) {
        m_settings.setValue(globalKey, value);
        return;
    }
    QJsonObject s = m_data.value(QStringLiteral("settings")).toObject();
    s.insert(projectKey, QJsonValue::fromVariant(value));
    m_data.insert(QStringLiteral("settings"), s);
    saveProject();
}

QString SettingsManager::projectFilePath() const
{
    if (m_projectRoot.isEmpty())
        return {};
    QString name = QFileInfo(m_projectRoot).fileName();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(m_projectRoot.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    const QString dir = QFileInfo(m_settings.fileName()).absolutePath() + QStringLiteral("/projects");
    return dir + QLatin1Char('/') + name + QLatin1Char('-') + hash + QStringLiteral(".json");
}

void SettingsManager::setProject(const QString &root)
{
    const QString before = theme();
    m_projectRoot = root;
    m_data = QJsonObject();
    if (!root.isEmpty()) {
        QFile f(projectFilePath());
        if (f.open(QIODevice::ReadOnly))
            m_data = QJsonDocument::fromJson(f.readAll()).object();
        else
            migrateLegacyProjectData(root);
        m_data.insert(QStringLiteral("version"), 1);
        m_data.insert(QStringLiteral("root"), root);
        saveProject();
    }
    if (theme() != before)
        emit themeChanged(theme());
    emit editorSettingsChanged();
    emit saveSettingsChanged();
    emit projectSettingsChanged();
}

void SettingsManager::saveProject() const
{
    if (m_projectRoot.isEmpty())
        return;
    const QString path = projectFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

// Run commands, bookmarks and open files used to be global keys in QODE.conf. The project that
// was open last owns them; they are moved into its file and removed from the conf.
void SettingsManager::migrateLegacyProjectData(const QString &root)
{
    if (m_settings.value(QStringLiteral("session/project")).toString() != root)
        return;
    m_settings.beginGroup(QStringLiteral("run"));
    QJsonObject run;
    for (const QString &k : m_settings.childKeys())
        run.insert(k, m_settings.value(k).toString());
    m_settings.endGroup();
    if (!run.isEmpty())
        m_data.insert(QStringLiteral("run"), run);

    const QVariant raw = m_settings.value(QStringLiteral("session/bookmarks"));
    const QString json = raw.typeId() == QMetaType::QStringList ? raw.toStringList().join(QLatin1Char(',')) : raw.toString();
    const QJsonObject bm = QJsonDocument::fromJson(json.toUtf8()).object();
    if (!bm.isEmpty())
        m_data.insert(QStringLiteral("bookmarks"), bm);

    QJsonObject session;
    session.insert(QStringLiteral("openFiles"), QJsonArray::fromStringList(m_settings.value(QStringLiteral("session/openFiles")).toStringList()));
    session.insert(QStringLiteral("activeFile"), m_settings.value(QStringLiteral("session/activeFile")).toString());
    m_data.insert(QStringLiteral("session"), session);

    m_settings.remove(QStringLiteral("run"));
    m_settings.remove(QStringLiteral("session/bookmarks"));
    m_settings.remove(QStringLiteral("session/openFiles"));
    m_settings.remove(QStringLiteral("session/activeFile"));
}
