#pragma once

#include <QByteArray>
#include <QString>

// Library sources a language server points at: `jar:///path/lib-sources.jar!/pkg/Name.kt` (Gradle/Maven
// source jars, the JDK's src.zip). The editor opens real files, so the entry is unpacked into QODE's cache
// folder once and opened read-only from there. The server still knows the file under its jar URI, which is
// what `uriForPath` hands back for document sync.
namespace JarSource {

bool isJarUri(const QString &uri);
// A library as Gradle/Maven name it, taken from the jar's location in their caches.
struct Coordinates {
    QString group, artifact, version;
    bool isValid() const { return !artifact.isEmpty() && !version.isEmpty(); }
    QString text() const { return group + QLatin1Char(':') + artifact + QLatin1Char(':') + version; }
};
Coordinates coordinatesOf(const QString &jarPath);

// Unpacks the entry of a jar URI; returns the cached file's path, or an empty string (with `error` set).
QString extract(const QString &uri, QString *error = nullptr);
// Why the last extract() found nothing for a compiled class, and the jar that lacks sources (empty when the
// failure was something else). A `.class` entry is never opened: its `-sources.jar` (next to the jar in Maven,
// in a sibling folder in Gradle's cache) is searched for the Kotlin/Java file it was built from instead.
QString lastFailure();
QString lastMissingSourcesJar();
// The file a `.class` location maps to when `extract` succeeded through a sources jar.
bool lastWasClass();

// Stores the text a server returned for a location it has no file for (jdtls: `jdt://contents/…/Name.class`, via
// java/classFileContents) as a read-only cached file named after the class; returns its path, or "" on failure.
QString storeText(const QString &uri, const QByteArray &text);
// The jar URI a cached file was unpacked from, or an empty string for any other path.
QString uriForPath(const QString &path);
bool isLibraryPath(const QString &path);

} // namespace JarSource
