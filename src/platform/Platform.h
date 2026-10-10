#pragma once

#include <QString>

// Where QODE keeps its own files, and the one place that knows the per-OS layout. Qt Core only. Callers append
// sub-folders ("/lsp/web", "/run") but never spell out ~/.cache or ~/.local themselves.
//
//   Linux    dataDir  ~/.local/share/QODE      cacheDir  ~/.cache/QODE
//   Windows  dataDir  %LOCALAPPDATA%\QODE      cacheDir  %LOCALAPPDATA%\QODE\cache
//
//   Linux    configDir ~/.config/QODE         (QODE.conf, themes/, projects/)
//   Windows  configDir %LOCALAPPDATA%\QODE    (same folder as dataDir)
//
// (On Windows the cache lives inside dataDir so removing one folder removes everything QODE keeps.) Nothing in this
// module creates a folder.
namespace Platform {

// Settings file, user themes and per-project JSON (SettingsManager decides the names inside).
QString configDir();
// Downloaded and installed things: language servers (<dataDir>/lsp/<id>), the private Python environment.
QString dataDir();
// Anything that can be rebuilt: server indexes, extracted library sources, the Expo schema, generated init scripts.
QString cacheDir();
// Small scripts QODE writes for the tools it starts (Gradle init scripts); under cacheDir().
QString runDir();
// The folder a downloaded command is linked into (~/.local/bin). Only meaningful on Linux.
QString userBinDir();

} // namespace Platform
