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
namespace AppPaths {

QString config(const QString &sub = {});
QString data(const QString &sub = {});
QString cache(const QString &sub = {});

// The settings file (QSettings, ini format): config()/QODE.conf on Unix, config()/QODE.ini on Windows (not the
// registry, so the per-project files can live beside it).
QString settingsFile();

} // namespace AppPaths
