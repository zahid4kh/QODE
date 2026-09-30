#pragma once

#include <QIcon>
#include <QString>

// Type-aware icons for files and folders (explorer, tabs, Quick Open, search results): a Lucide glyph per
// kind of file, tinted with a colour from the active theme.
namespace FileIcons {

QIcon forFile(const QString &fileName); // by file name / extension; generic file icon when unknown
QIcon folder();

} // namespace FileIcons
