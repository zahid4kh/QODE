#pragma once

#include <QByteArray>
#include <QFont>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QVariant>

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

    // --- Saving -----------------------------------------------------------
    enum AutoSaveMode { AutoSaveOff = 0, AutoSaveAfterDelay = 1, AutoSaveOnFocusChange = 2 };
    int autoSaveMode() const;
    void setAutoSaveMode(int mode);
    int autoSaveDelayMs() const; // AutoSaveAfterDelay
    bool trimTrailingWhitespace() const;
    void setTrimTrailingWhitespace(bool on);
    bool insertFinalNewline() const;
    void setInsertFinalNewline(bool on);
    bool autoActivateVenv() const; // activate a project's Python venv in new terminals
    void setAutoActivateVenv(bool on);
    bool formatOnSave() const;
    void setFormatOnSave(bool on);

    bool showBreadcrumbs() const;
    void setShowBreadcrumbs(bool on);

    bool showHiddenFiles() const; // explorer: dot files and folders
    void setShowHiddenFiles(bool on);
    bool showMinimap() const;
    void setShowMinimap(bool on);

    // Bookmarked lines (0-based) per absolute file path (current project).
    QHash<QString, QList<int>> bookmarks() const;
    void setBookmarks(const QHash<QString, QList<int>> &bookmarks);
    // Terminal tabs of the project: one entry per tab, the name the user gave it or "" for the automatic one.
    QStringList terminalTabs() const;
    void setTerminalTabs(const QStringList &names);
    bool importsFolded(const QString &path) const;
    void setImportsFolded(const QString &path, bool folded);
    bool blameInline() const;
    void setBlameInline(bool on);
    bool blameGutter() const;
    void setBlameGutter(bool on);
    bool stickyScroll() const;
    void setStickyScroll(bool on);
    bool indentGuides() const;
    void setIndentGuides(bool on);

    // Per-project data (theme/tab size/spaces/format-on-save/venv overrides, run commands, open files, bookmarks) lives in
    // ~/.config/QODE/projects/<name>-<hash>.json, created when a project is opened. With no
    // project open these accessors work on a throwaway in-memory copy that is never saved.
    void setProject(const QString &root); // empty = no project
    QString projectFilePath() const;

    QString theme() const; // "dark" | "light"; the project's override, else the global default
    void setTheme(const QString &theme); // sets the project's override while a project is open

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);

    QByteArray windowState() const;
    void setWindowState(const QByteArray &state);

    // Which left-panel sections are expanded (Explorer, Source Control, Search, Tasks); empty = default.
    QList<bool> sideSections() const;
    void setSideSections(const QList<bool> &expanded);

    int explorerWidth() const;
    void setExplorerWidth(int w);

    int terminalHeight() const;
    void setTerminalHeight(int h);

    bool sideBarVisible() const;
    void setSideBarVisible(bool v);

    // Window layout of the open project (sidebar, terminal, sections, editor splits); empty when
    // the project has none saved yet or no project is open.
    QJsonObject projectLayout() const;
    void setProjectLayout(const QJsonObject &layout);

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

    QStringList recentProjects() const; // most recent first
    void addRecentProject(const QString &path);
    void removeRecentProject(const QString &path);
    void clearRecentProjects();

    QString runCommand(const QString &key) const; // per file type, see RunConfigDialog::keyFor
    void setRunCommand(const QString &key, const QString &command);
    // Dev server of a web project: {manager, script, port, command}; missing keys mean automatic.
    QJsonObject webServer() const;
    void setWebServer(const QJsonObject &config);

    // Language servers: executable override per server id ("" = look on PATH).
    QString lspServerPath(const QString &serverId) const;
    void setLspServerPath(const QString &serverId, const QString &path);

    // Compiler flags for the language server when the project has no compile database (raw text,
    // one flag per line, # comments). Per project; stored in the project's JSON file.
    QString lspFlagsText() const;
    void setLspFlagsText(const QString &text);

    QStringList recentCommands() const; // most recent first
    void setRecentCommands(const QStringList &commands);

signals:
    void editorSettingsChanged();
    void themeChanged(const QString &theme);
    void recentProjectsChanged();
    void saveSettingsChanged();
    void projectSettingsChanged(); // the open project changed, or a per-project toggle did

private:
    SettingsManager();
    QVariant projectValue(const QString &key, const QVariant &fallback) const;
    void setScopedValue(const QString &globalKey, const QString &projectKey, const QVariant &value);
    void saveProject() const;
    void migrateLegacyProjectData(const QString &root);

    QSettings m_settings;
    QString m_projectRoot;
    QJsonObject m_data; // the open project's JSON document
};
