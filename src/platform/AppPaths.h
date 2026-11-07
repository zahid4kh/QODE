#pragma once

#include <QString>

// QODE's own folders. Build every path under them through these, never from QDir::homePath() or QStandardPaths
// directly: the roots differ per OS.
//
//            Unix                              Windows
//   config   ~/.config/QODE-AGENTIC            %APPDATA%\QODE-AGENTIC          settings, keys, mcp.json, projects/
//   data     ~/.local/share/QODE-AGENTIC       %LOCALAPPDATA%\QODE-AGENTIC     language servers, history, scratches
//   cache    ~/.cache/QODE-AGENTIC             %LOCALAPPDATA%\QODE-AGENTIC\cache
//
// The Unix roots follow $XDG_CONFIG_HOME / $XDG_DATA_HOME / $XDG_CACHE_HOME (QStandardPaths). Paths use '/'. `sub`
// is appended after a '/' when it is not empty; the folders are not created here.
//
// Portable mode (Windows only, the portable zip): a file named portable.txt next to qode.exe puts all three inside
// <QODE folder>\data (config = data\config, data = data\local, cache = data\local\cache). JDK downloads then go to
// data\local\jdks and no Kotlin command link is made, so a portable QODE writes nothing outside its own folder.
namespace AppPaths {

// True in portable mode. Unix: always false.
bool isPortable();

QString config(const QString &sub = {});
QString data(const QString &sub = {});
QString cache(const QString &sub = {});

// The settings file (QSettings, ini format): config()/QODE.conf on Unix, config()/QODE.ini on Windows (not the
// registry, so the per-project files can live beside it).
QString settingsFile();

// Qt's file dialog keeps its state (last folder, history) in QSettings(UserScope, "QtProject"), by default
// ~/.config/QtProject.conf on Unix and HKCU\Software\QtProject on Windows, shared with every other Qt program.
// Unix: this moves it to config()/QtProject.conf, so everything QODE writes stays in its own folders. Windows: Qt
// cannot move it (always the registry); FileDialogs restores the key after each dialog instead. Called by main
// before anything uses QSettings.
void initQtSettings();

} // namespace AppPaths
