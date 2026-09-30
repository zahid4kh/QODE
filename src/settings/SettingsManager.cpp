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

QStringList SettingsManager::recentCommands() const
{
    return m_settings.value(QStringLiteral("session/recentCommands")).toStringList();
}

void SettingsManager::setRecentCommands(const QStringList &c)
{
    m_settings.setValue(QStringLiteral("session/recentCommands"), c);
}
