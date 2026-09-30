#include "SettingsManager.h"

#include <QFontDatabase>

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
    return m_settings.value(QStringLiteral("editor/tabSize"), 4).toInt();
}

void SettingsManager::setTabSize(int size)
{
    m_settings.setValue(QStringLiteral("editor/tabSize"), size);
    emit editorSettingsChanged();
}

bool SettingsManager::useSpaces() const
{
    return m_settings.value(QStringLiteral("editor/useSpaces"), true).toBool();
}

void SettingsManager::setUseSpaces(bool on)
{
    m_settings.setValue(QStringLiteral("editor/useSpaces"), on);
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
    return m_settings.value(QStringLiteral("terminal/autoActivateVenv"), true).toBool();
}

void SettingsManager::setAutoActivateVenv(bool on)
{
    m_settings.setValue(QStringLiteral("terminal/autoActivateVenv"), on);
}

bool SettingsManager::formatOnSave() const
{
    return m_settings.value(QStringLiteral("save/formatOnSave"), false).toBool();
}

void SettingsManager::setFormatOnSave(bool on)
{
    m_settings.setValue(QStringLiteral("save/formatOnSave"), on);
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
    return m_settings.value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString();
}

void SettingsManager::setTheme(const QString &theme)
{
    m_settings.setValue(QStringLiteral("ui/theme"), theme);
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
    return m_settings.value(QStringLiteral("session/openFiles")).toStringList();
}

void SettingsManager::setOpenFiles(const QStringList &f)
{
    m_settings.setValue(QStringLiteral("session/openFiles"), f);
}

QString SettingsManager::activeFile() const
{
    return m_settings.value(QStringLiteral("session/activeFile")).toString();
}

void SettingsManager::setActiveFile(const QString &f)
{
    m_settings.setValue(QStringLiteral("session/activeFile"), f);
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
    return m_settings.value(QStringLiteral("run/") + key).toString();
}

void SettingsManager::setRunCommand(const QString &key, const QString &command)
{
    if (command.isEmpty())
        m_settings.remove(QStringLiteral("run/") + key);
    else
        m_settings.setValue(QStringLiteral("run/") + key, command);
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
