#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Facts about Java / Kotlin files and builds that need no language server: Kotlin scripts, `main` functions, the package
// a file's folder implies, and the Kotlin / Java / Gradle versions a build declares. Qt Core only, nothing is written.
namespace JvmTools {

// --- Kotlin scripts -------------------------------------------------------------------------------------------------
bool isKotlinScript(const QString &path); // *.kts, but not Gradle's *.gradle.kts
bool isMainKts(const QString &path);      // *.main.kts (@file:DependsOn / @file:Repository work there)
QString shebang(const QString &text);     // the first line when it starts with "#!", else ""
// The folder kotlinc belongs to ($KOTLIN_HOME, SDKMAN, the snap, or the folder above kotlinc's bin/), "" when not found.
QString kotlinHome();
QString mainKtsJar(); // <kotlin home>/lib/kotlin-main-kts.jar, "" when missing
// How F5 runs a script: `{file}` itself when it has a `#!…kotlin` line and is executable, else `kotlinc -script {file}`.
// `chmodOffer` is set when the script has the shebang but is not executable yet.
QString scriptRunCommand(const QString &path, const QString &text, bool *chmodOffer = nullptr);

// --- main functions -------------------------------------------------------------------------------------------------
struct MainEntry {
    int line = 0;  // 0-based line of the declaration
    QString cls;   // fully qualified class the JVM starts (MainKt, Foo for @JvmStatic in object Foo, …)
};
// `fun main` (top level, or @JvmStatic in an object / companion) in Kotlin; `static void main(` in Java.
QList<MainEntry> findMains(const QString &path, const QString &text);
// --- tests ------------------------------------------------------------------------------------------------------------
struct TestEntry {
    int line = 0;   // 0-based line of the declaration: the test function, or the class
    QString cls;    // fully qualified class (nested classes joined with '$', as the JVM names them)
    QString method; // the test function; empty for the class itself (all of its tests)
};
// JUnit 4 / 5 and kotlin.test: functions annotated @Test (also @ParameterizedTest, @RepeatedTest, @TestFactory,
// @TestTemplate) and, before them, each class that holds some. Sorted by line.
QList<TestEntry> findTests(const QString &path, const QString &text);
// The class name of a Kotlin file's top-level functions (FooKt, or @file:JvmName).
QString fileFacade(const QString &path, const QString &text);

// --- packages ---------------------------------------------------------------------------------------------------------
// The declared package and its 0-based line (-1 when the file has no package statement).
QString declaredPackage(const QString &text, int *line = nullptr);
// The package the file's folder implies below its source root (src/<set>/java|kotlin/…); null QString when the file
// is not below such a root. `sourceRoot` receives the root folder.
QString folderPackage(const QString &path, QString *sourceRoot = nullptr);
// Whether the declared package fits the folder. Java needs an exact match; Kotlin also accepts a folder path that leaves
// out a common root package (src/main/kotlin/Main.kt with `package com.example`), as its coding conventions allow.
bool packageMatches(const QString &path, const QString &declared, const QString &folder);
// The text with its package statement changed to `pkg` (added after leading comments, removed when `pkg` is empty).
QString withPackage(const QString &path, const QString &text, const QString &pkg);

// --- versions ---------------------------------------------------------------------------------------------------------
struct Found {
    QString value;
    QString file; // where it is declared
    int line = 0; // 1-based
    bool isEmpty() const { return value.isEmpty(); }
};
struct Versions {
    Found kotlin;     // Kotlin Gradle plugin / kotlin.version
    Found java;       // toolchain or target release ("21", "1.8")
    Found build;      // Gradle wrapper or Maven wrapper version
    bool maven = false;
    int toolchain = 0;          // jvmToolchain(N) / JavaLanguageVersion.of(N); 0 = none
    bool autoProvision = false; // the foojay toolchain resolver can download a missing toolchain
};
// Reads the root and its direct sub-folders (the places JvmProject looks).
Versions detectVersions(const QString &root);
bool isJvmBuild(const QString &root); // Gradle or Maven build at the root

// --- Gradle wrapper ---------------------------------------------------------------------------------------------------
QString wrapperProperties(const QString &dir); // gradle/wrapper/gradle-wrapper.properties, "" when missing
Found wrapperVersion(const QString &dir);
// Points the wrapper at another Gradle release (the distributionUrl in gradle-wrapper.properties; a distributionSha256Sum
// of the old release is dropped). Works when the old Gradle cannot start (on a JDK too new for it), which
// `gradlew wrapper --gradle-version` needs. False with `error` set when the file is missing or cannot be written.
bool setWrapperVersion(const QString &dir, const QString &version, QString *error = nullptr);

// --- Which Java runs which Gradle (docs.gradle.org/current/userguide/compatibility.html, Gradle 9.9) -------------------
int compareVersions(const QString &a, const QString &b); // <0, 0, >0 for "8.14.3" vs "9.1" ("9.0-rc-1" counts as 9.0)
int gradleMinJava(const QString &gradle); // 17 for Gradle 9 and newer, else 8
// The newest Java `gradle` runs on, 0 when the release is newer than the table (then any Java from gradleMinJava is
// taken to work).
int gradleMaxJava(const QString &gradle);
bool gradleRunsOn(const QString &gradle, int java);
QString gradleForJava(int java); // the first Gradle release that runs on `java` ("9.1.0"), "" beyond the table
// gradlew exists but is not executable (a zip download or a checkout on a file system without modes).
bool wrapperNeedsExecBit(const QString &dir);

QString shellQuote(const QString &s);

} // namespace JvmTools
