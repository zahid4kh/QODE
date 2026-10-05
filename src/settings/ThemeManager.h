#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include "Theme.h"

// Knows every theme: the built-in "dark", "light" and "darcula" plus the JSON files in <config>/themes. A theme's id is
// a built-in id or the file's base name; SettingsManager stores that id. While the theme editor is open a
// preview theme stands in for whichever theme is active, so the whole window follows the edits live.
class ThemeManager : public QObject {
    Q_OBJECT
public:
    struct Info {
        QString id;
        QString name;
        bool builtin = false;
        bool dark = true;
    };

    static ThemeManager &instance();
    static bool isBuiltin(const QString &id);

    QList<Info> themes() const;
    bool contains(const QString &id) const;
    Theme theme(const QString &id) const; // unknown ids give the dark theme; a preview wins over everything
    QString themesDir() const;

    // Writes <id>.json (a new id is derived from the name when `id` is empty or built-in) and returns the id.
    QString save(const Theme &theme, const QString &id);
    bool remove(const QString &id);
    QString uniqueId(const QString &name) const;

    void setPreview(const Theme &theme);
    void clearPreview();
    bool previewing() const { return m_previewing; }
    int revision() const { return m_revision; } // bumps whenever any theme's colours may have changed

signals:
    void listChanged();

private:
    ThemeManager();
    void reload();
    void announce();

    QHash<QString, Theme> m_user;
    QStringList m_order;
    Theme m_preview;
    bool m_previewing = false;
    int m_revision = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
