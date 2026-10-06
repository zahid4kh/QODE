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
        mainClass = "{{package}}.MainKt"

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

compose.resources {
    publicResClass = false
    packageOfResClass = "{{package}}.resources"
    generateResClass = auto
}
