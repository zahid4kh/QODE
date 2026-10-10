import org.jetbrains.compose.desktop.application.dsl.TargetFormat
import java.util.Scanner

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

// Linux only: rebuilds the .deb made by Compose with the app's display name, StartupWMClass,
// maintainer, description and symlink scripts. The values above (packageName, appDisplayName,
// mainClass, maintainer, controlDescription) are shared with buildUberDeb.
val workDir = file("deb-temp")
val desktopRelativePath = "opt/$packageName/lib/$packageName-$packageName.desktop"

fun promptUserChoice(): String {
    println(
        """
        Which packaging task do you want to run?
        1 = packageDeb (default)
        2 = packageReleaseDeb
        """.trimIndent()
    )
    print("Enter your choice [1/2]: ")

    return Scanner(System.`in`).nextLine().trim().ifEmpty { "1" }
}

tasks.register("addStartupWMClassToDebDynamic") {
    group = "release"
    description = "Finds .deb file, modifies .desktop, control files, and DEBIAN scripts, and rebuilds it"

    doLast {
        val debRoot = file("build/compose/binaries")
        if (!debRoot.exists()) throw GradleException("Folder not found: ${debRoot}")

        val allDebs = debRoot.walkTopDown().filter { it.isFile && it.extension == "deb" }.toList()
        if (allDebs.isEmpty()) throw GradleException("No .deb files found under ${debRoot}")

        // picking the latest .deb file
        val originalDeb = allDebs.maxByOrNull { it.lastModified() }!!
        println("Found deb package: ${originalDeb.relativeTo(rootDir)}")

        val modifiedDeb = File(originalDeb.parentFile, originalDeb.nameWithoutExtension + "-wm.deb")

        // cleaning up "deb-temp" folder, if exists
        if (workDir.exists()) workDir.deleteRecursively()
        workDir.mkdirs()

        // Step 1: Extracting generated debian package
        exec {
            commandLine("dpkg-deb", "-R", originalDeb.absolutePath, workDir.absolutePath)
        }

        // Step 2: Modifying the desktop entry file
        val desktopFile = File(workDir, desktopRelativePath)
        if (!desktopFile.exists()) throw GradleException(".desktop file not found: ${desktopRelativePath}")

        val lines = desktopFile.readLines().toMutableList()

        // Modifying the Name field (app's display name on dock)
        var nameModified = false
        for (i in lines.indices) {
            if (lines[i].trim().startsWith("Name=")) {
                lines[i] = "Name=${appDisplayName}"
                nameModified = true
                println("Modified Name entry to: ${appDisplayName}")
                break
            }
        }

        // adding Name field if it doesn't exist
        if (!nameModified) {
            lines.add("Name=${appDisplayName}")
            println("Added Name entry: ${appDisplayName}")
        }

        for (i in lines.indices) {
            if (lines[i].trim().startsWith("StartupWMClass=")) {
                if (lines[i] != "StartupWMClass=${mainClass}") {
                    lines[i] = "StartupWMClass=${mainClass}"
                    println("Updated StartupWMClass entry to: ${mainClass}")
                } else {
                    println("StartupWMClass already correctly set to: ${mainClass}")
                }
                break
            }
        }

        // Adding StartupWMClass if it doesn't exist
        if (!lines.any { it.trim().startsWith("StartupWMClass=") }) {
            lines.add("StartupWMClass=${mainClass}")
            println("Added StartupWMClass entry: ${mainClass}")
        }

        // Writing changes back to file
        desktopFile.writeText(lines.joinToString("\n"))

        println("\nFinal .desktop file content:")
        println("--------------------------------")
        desktopFile.readLines().forEach { println(it) }
        println("--------------------------------\n")

        // Step 3: Modifying the DEBIAN/control file
        val controlFile = File(workDir, "DEBIAN/control")
        if (!controlFile.exists()) throw GradleException("control file not found: DEBIAN/control")

        val controlLines = controlFile.readLines().toMutableList()

        // Update maintainer field
        var maintainerModified = false
        for (i in controlLines.indices) {
            if (controlLines[i].trim().startsWith("Maintainer:")) {
                controlLines[i] = "Maintainer: ${maintainer}"
                maintainerModified = true
                println("Modified Maintainer entry")
                break
            }
        }

        // Add maintainer field if it doesn't exist
        if (!maintainerModified) {
            controlLines.add("Maintainer: ${maintainer}")
            println("Added Maintainer entry")
        }

        // Update description field for better info
        for (i in controlLines.indices) {
            if (controlLines[i].trim().startsWith("Description:")) {
                controlLines[i] = "Description: ${controlDescription}"
                println("Modified Description entry")
                break
            }
        }

        // Write changes back to control file
        controlFile.writeText(controlLines.joinToString("\n"))

        println("\nFinal control file content:")
        println("--------------------------------")
        controlFile.readLines().forEach { println(it) }
        println("--------------------------------\n")

        // Step 4: Modifying the DEBIAN/postinst script
        val postinstFile = File(workDir, "DEBIAN/postinst")
        if (!postinstFile.exists()) throw GradleException("postinst file not found: DEBIAN/postinst")

        val postinstContent = """#!/bin/sh
set -e
case "$1" in
    configure)
        # Install desktop menu entry
        xdg-desktop-menu install /opt/${packageName}/lib/${packageName}-${packageName}.desktop
        
        # Create symlink for terminal access
        if [ ! -L /usr/local/bin/${packageName} ]; then
            ln -sf /opt/${packageName}/bin/${packageName} /usr/local/bin/${packageName}
            echo "Created symlink: /usr/local/bin/${packageName} -> /opt/${packageName}/bin/${packageName}"
        fi
    ;;

    abort-upgrade|abort-remove|abort-deconfigure)
    ;;

    *)
        echo "postinst called with unknown argument `$1`" >&2
        exit 1
    ;;
esac

exit 0"""

        postinstFile.writeText(postinstContent)
        println("Updated postinst script to create terminal symlink")

        // Step 5: Modifying the DEBIAN/prerm script
        val prermFile = File(workDir, "DEBIAN/prerm")
        if (!prermFile.exists()) throw GradleException("prerm file not found: DEBIAN/prerm")

        val prermContent = """#!/bin/sh
set -e
case "$1" in
    remove|upgrade|deconfigure)
        # Remove desktop menu entry
        xdg-desktop-menu uninstall /opt/${packageName}/lib/${packageName}-${packageName}.desktop
        
        # Remove terminal symlink
        if [ -L /usr/local/bin/${packageName} ]; then
            rm -f /usr/local/bin/${packageName}
            echo "Removed symlink: /usr/local/bin/${packageName}"
        fi
    ;;

    failed-upgrade)
    ;;

    *)
        echo "prerm called with unknown argument `$1`" >&2
        exit 1
    ;;
esac

exit 0"""

        prermFile.writeText(prermContent)
        println("Updated prerm script to remove terminal symlink")

        // Make sure scripts are executable
        exec {
            commandLine("chmod", "+x", postinstFile.absolutePath)
        }
        exec {
            commandLine("chmod", "+x", prermFile.absolutePath)
        }

        println("\nFinal postinst script content:")
        println("--------------------------------")
        postinstFile.readLines().forEach { println(it) }
        println("--------------------------------\n")

        println("\nFinal prerm script content:")
        println("--------------------------------")
        prermFile.readLines().forEach { println(it) }
        println("--------------------------------\n")

        // Step 6: Repackaging the debian package back
        exec {
            commandLine("dpkg-deb", "-b", workDir.absolutePath, modifiedDeb.absolutePath)
        }

        println("Done: Rebuilt with Name=${appDisplayName}, StartupWMClass=${mainClass}, updated control file, and terminal symlink -> ${modifiedDeb.name}")
    }
}


tasks.register("packageDebWithWMClass") {
    group = "release"
    description = "Runs packaging task (packageDeb or packageReleaseDeb), then adds StartupWMClass"

    doLast {
        val choice = promptUserChoice()

        val packagingTask = when (choice) {
            "2" -> "packageReleaseDeb"
            else -> "packageDeb"
        }

        println("Running: ${packagingTask}")
        gradle.includedBuilds.forEach { it.task(":${packagingTask}") } // just in case of composite builds

        exec {
            commandLine("./gradlew", packagingTask)
        }

        tasks.named("addStartupWMClassToDebDynamic").get().actions.forEach { it.execute(this) }
    }
}

compose.resources {
    publicResClass = false
    packageOfResClass = "{{package}}.resources"
    generateResClass = auto
}
