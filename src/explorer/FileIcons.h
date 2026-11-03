#pragma once

#include <QIcon>
#include <QString>

// Type-aware icons for files and folders (explorer, tabs, Quick Open, search results): a Lucide glyph per
// kind of file, tinted with a colour from the active theme.
namespace FileIcons {

QIcon forFile(const QString &fileName); // by file name / extension; generic file icon when unknown
QIcon folder();

// Whose logo application.yaml / .yml / .properties / .conf carry: Spring's by default, Ktor's in a Ktor project
// (where application.conf is the HOCON configuration too). True when it changed (views then repaint).
enum class ConfigBrand { Spring, Ktor };
bool setConfigBrand(ConfigBrand brand);

} // namespace FileIcons
