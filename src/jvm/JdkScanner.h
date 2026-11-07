#pragma once

#include <QList>
#include <QString>

// The JDKs installed on this machine: SDKMAN, /usr/lib/jvm, the toolchains Gradle provisioned (~/.gradle/jdks), IntelliJ's
// ~/.jdks, $JAVA_HOME, the `java` on PATH, and on Windows those in Program Files, the registry and Scoop
// (Platform::jdkFolders / jdkHomes). Each is read from its `release` file. Qt Core only.
namespace JdkScanner {

struct Jdk {
    QString home;    // canonical folder (has bin/java)
    QString version; // JAVA_VERSION, e.g. "21.0.4"
    int major = 0;   // 21 (8 for "1.8.0_402")
    QString vendor;  // IMPLEMENTOR, e.g. "Eclipse Adoptium"
    QString source;  // where it was found: "SDKMAN", "System", "Gradle", "IntelliJ", "JAVA_HOME", "PATH", "Chosen"
    bool jre = false; // no javac
    bool isValid() const { return !home.isEmpty(); }
    QString label() const; // "21.0.4 · Eclipse Adoptium"
};

QList<Jdk> scan();                     // sorted by major version, newest first; duplicates (symlinks) removed
Jdk fromHome(const QString &home);     // invalid when the folder holds no JDK
Jdk defaultJdk(const QList<Jdk> &all); // $JAVA_HOME, else the one `java` on PATH belongs to
int majorOf(const QString &version);   // "1.8.0_402" -> 8, "21.0.4" -> 21, "17" -> 17

// The JDK the project's Gradle runs on (QODE gives it to the Kotlin / Java servers for their Gradle import): the
// project JDK (`chosenHome`) when that Gradle runs on it, else the system default, else the newest installed JDK it runs
// on. Invalid when none does. `gradleVersion` "" (no wrapper) takes the project / system JDK as it is.
Jdk forGradle(const QList<Jdk> &all, const QString &gradleVersion, const QString &chosenHome);

// What a Gradle project needs from the installed JDKs, and what to download when something is missing.
struct Needs {
    bool isGradle = false;     // false (and ok()) for Maven builds and folders without a JVM build
    QString gradle;            // the wrapper's Gradle version, "" without a wrapper
    Jdk runner;                // forGradle(); invalid when no installed JDK runs this Gradle
    int toolchain = 0;         // jvmToolchain(N) the build asks for, 0 = none
    bool toolchainMissing = false; // no JDK `toolchain` installed and no foojay resolver to download it
    QList<int> downloads;      // Java versions to download that fix it (the toolchain first, since it often runs Gradle too)
    bool ok() const { return !isGradle || (runner.isValid() && !toolchainMissing); }
};
Needs needsOf(const QString &root, const QList<Jdk> &all, const QString &chosenHome);

} // namespace JdkScanner
