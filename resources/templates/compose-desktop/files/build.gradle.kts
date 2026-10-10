import org.jetbrains.compose.desktop.application.dsl.TargetFormat

plugins {
    alias(libs.plugins.kotlin.jvm)
    alias(libs.plugins.jetbrains.compose)
    alias(libs.plugins.kotlin.plugin.compose)
    alias(libs.plugins.kotlin.plugin.serialization)
}

group = "{{package}}"
version = "{{version}}"

repositories {
    mavenCentral()
    google()
}

dependencies {
    implementation(compose.desktop.currentOs)
    implementation(compose.material3)
    implementation(compose.components.resources)
{{#if extendedIcons}}
    implementation(compose.materialIconsExtended)
{{/if}}

    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.core)
    implementation(libs.kotlinx.coroutines.swing)
    implementation(libs.koin.core)
}

compose.desktop {
    application {
        mainClass = "{{className}}"

        nativeDistributions {
            targetFormats(TargetFormat.Deb, TargetFormat.Msi, TargetFormat.Exe, TargetFormat.Dmg)
            packageName = "{{nameId}}"
            packageVersion = "{{version}}"
            description = "{{appName}}"
            vendor = "{{maintainerName}}"

            linux {
                shortcut = true
                debMaintainer = "{{maintainer}}"
                iconFile.set(project.file("icons/linux.png"))
            }

            windows {
                shortcut = true
                dirChooser = true
                menu = true
                upgradeUuid = "{{uuid}}"
                iconFile.set(project.file("icons/windows.ico"))
            }

            macOS {
                dockName = "{{appName}}"
            }
        }
    }
}

val packageName = "{{nameId}}"
val appDisplayName = "{{appName}}"
val maintainer = "{{maintainer}}"
val controlDescription = "{{appName}}"
val mainClass = "{{className}}"

tasks.register<Exec>("buildUberDeb") {
    group = "release"
    description = "Builds a lean .deb package from the optimized uber-JAR. It automatically runs the 'packageReleaseUberJarForCurrentOS' task first, so there is no need to run it separately. The task then creates a standard Debian file structure with a launcher script, a .desktop file for application menus, and install/remove scripts for clean system integration."

    dependsOn(tasks.named("packageReleaseUberJarForCurrentOS"))

    val debianRoot = project.layout.buildDirectory.dir("debian/$packageName-${project.version}")
    val finalDebDir = project.layout.buildDirectory.dir("dist")

    val uberJar = tasks.named<AbstractArchiveTask>("packageReleaseUberJarForCurrentOS").flatMap { it.archiveFile }
    inputs.file(uberJar)
    outputs.dir(finalDebDir)

    doFirst {
        delete(debianRoot)
        mkdir(debianRoot)

        val controlFile = file("${debianRoot.get()}/DEBIAN/control")
        controlFile.parentFile.mkdirs()
        controlFile.writeText("""
            Package: $packageName
            Version: ${project.version}
            Architecture: all
            Maintainer: $maintainer
            Depends: default-jre | java17-runtime | openjdk-17-jre
            Description: $controlDescription
            
        """.trimIndent())

        val binDir = file("${debianRoot.get()}/opt/$packageName/bin")
        binDir.mkdirs()
        val launcherScript = file("$binDir/$packageName")
        launcherScript.writeText("""
            #!/bin/sh
            echo "Launching $appDisplayName..."
            exec java -jar /opt/$packageName/lib/$packageName.jar "$@"
        """.trimIndent())
        launcherScript.setExecutable(true, false)

        val libDir = file("${debianRoot.get()}/opt/$packageName/lib")
        libDir.mkdirs()
        copy {
            from(uberJar)
            into(libDir)
            rename { "$packageName.jar" }
        }

        val desktopFileDir = file("${debianRoot.get()}/usr/share/applications")
        desktopFileDir.mkdirs()
        file("$desktopFileDir/$packageName.desktop").writeText("""
            [Desktop Entry]
            Version=1.0
            Name=$appDisplayName
            Comment=$controlDescription
            Exec=/opt/$packageName/bin/$packageName
            Icon=$packageName
            Terminal=false
            Type=Application
            Categories=Utility;
            StartupWMClass=$mainClass
        """.trimIndent())

        val iconPath = "icons/linux.png"
        val iconDir = file("${debianRoot.get()}/usr/share/icons/hicolor/512x512/apps")
        iconDir.mkdirs()
        copy {
            from(iconPath)
            into(iconDir)
            rename { "$packageName.png" }
        }

        val postinstFile = file("${debianRoot.get()}/DEBIAN/postinst")
        postinstFile.writeText("""
            #!/bin/sh
            set -e
            echo "Creating symlink for terminal access..."
            ln -sf /opt/$packageName/bin/$packageName /usr/local/bin/$packageName
            echo "Symlink created: /usr/local/bin/$packageName"
            
            echo "Updating icon cache..."
            gtk-update-icon-cache -q /usr/share/icons/hicolor || true
            
            echo "Updating desktop database..."
            update-desktop-database -q /usr/share/applications || true
            
            echo "Installation of '$appDisplayName' complete."
            exit 0
        """.trimIndent())
        postinstFile.setExecutable(true, false)

        val prermFile = file("${debianRoot.get()}/DEBIAN/prerm")
        prermFile.writeText("""
            #!/bin/sh
            set -e
            echo "Removing symlink: /usr/local/bin/$packageName"
            rm -f /usr/local/bin/$packageName
            echo "Symlink removed."
            echo "Pre-removal steps for '$appDisplayName' complete."
            exit 0
        """.trimIndent())
        prermFile.setExecutable(true, false)
    }

    workingDir(debianRoot.get().asFile.parentFile)
    commandLine("dpkg-deb", "--build", "--root-owner-group", debianRoot.get().asFile.name, finalDebDir.get().asFile.path)
}

compose.resources {
    publicResClass = false
    packageOfResClass = "{{package}}.resources"
    generateResClass = auto
}
