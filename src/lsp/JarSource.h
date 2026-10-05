#pragma once

#include <QByteArray>
#include <QString>

// Library sources a language server points at: `jar:///path/lib-sources.jar!/pkg/Name.kt` (Gradle/Maven
// source jars, the JDK's src.zip). The editor opens real files, so the entry is unpacked into QODE's cache
// folder once and opened read-only from there. The server still knows the file under its jar URI, which is
// what `uriForPath` hands back for document sync.
namespace JarSource {

bool isJarUri(const QString &uri);
// Unpacks the entry of a jar URI; returns the cached file's path, or an empty string (with `error` set).
QString extract(const QString &uri, QString *error = nullptr);
// Stores the text a server returned for a location it has no file for (jdtls: `jdt://contents/…/Name.class`, via
// java/classFileContents) as a read-only cached file named after the class; returns its path, or "" on failure.
QString storeText(const QString &uri, const QByteArray &text);
// The jar URI a cached file was unpacked from, or an empty string for any other path.
QString uriForPath(const QString &path);
bool isLibraryPath(const QString &path);

} // namespace JarSource
