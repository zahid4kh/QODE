plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "{{foojayVersion}}"
}

dependencyResolutionManagement {
    repositories {
        mavenCentral()
    }
    versionCatalogs {
        // Ktor's own catalog: every ktor-server-* / ktor-client-* module and the Ktor Gradle plugin, at one version.
        create("ktorLibs").from("io.ktor:ktor-version-catalog:{{ktorVersion}}")
    }
}

rootProject.name = "{{nameId}}"
