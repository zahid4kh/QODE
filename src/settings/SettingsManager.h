#pragma once

#include <QByteArray>
#include <QFont>
#include <QObject>
#include <QSettings>
#include <QStringList>

class SettingsManager : public QObject
{
    Q_OBJECT
public:
    static SettingsManager &instance();

    QFont editorFont() const;
    void setEditorFont(const QFont &font);

    int tabSize() const;
    void setTabSize(int size);

    bool useSpaces() const;
    void setUseSpaces(bool on);

    bool wordWrap() const;
    void setWordWrap(bool on);

    bool indentGuides() const;
    void setIndentGuides(bool on);

    QString theme() const; // "dark" | "light"
    void setTheme(const QString &theme);

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);

    QByteArray windowState() const;
    void setWindowState(const QByteArray &state);

    int explorerWidth() const;
    void setExplorerWidth(int w);

    int terminalHeight() const;
    void setTerminalHeight(int h);

    bool terminalVisible() const;
    void setTerminalVisible(bool v);

    QString lastProject() const;
    void setLastProject(const QString &path);

    QStringList openFiles() const;
    void setOpenFiles(const QStringList &files);

    QString activeFile() const;
    void setActiveFile(const QString &file);

    QString lastDirectory() const;
    void setLastDirectory(const QString &dir);

    QStringList recentCommands() const; // most recent first
    void setRecentCommands(const QStringList &commands);

signals:
    void editorSettingsChanged();
    void themeChanged(const QString &theme);

private:
    SettingsManager();
    QSettings m_settings;
};
