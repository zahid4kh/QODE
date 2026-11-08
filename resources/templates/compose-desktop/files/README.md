# {{appName}}

A Compose Multiplatform desktop application (Material 3, Koin, kotlinx-serialization).

{{#if !os=win}}
```bash
./gradlew run                                          # run the app
./gradlew :hotRun --mainClass {{className}} --auto   # run with hot reload: edit, save, see it update
./gradlew packageDeb                                   # Linux installer (build/compose/binaries)
./gradlew packageMsi                                   # Windows installer (on Windows)
./gradlew packageDmg                                   # macOS installer (on macOS)
```
{{/if}}
{{#if os=win}}
```bat
.\gradlew.bat run                                          # run the app
.\gradlew.bat :hotRun --mainClass {{className}} --auto   # run with hot reload: edit, save, see it update
.\gradlew.bat packageMsi                                   # Windows installer (build\compose\binaries)
.\gradlew.bat packageExe                                   # Windows .exe installer
.\gradlew.bat packageDeb                                   # Linux installer (on Linux)
```
{{/if}}

The window and installer icons live in `icons/`; the window icon is also in `src/main/composeResources/drawable/`.
Before shipping a Windows installer keep `upgradeUuid` in `build.gradle.kts` unchanged so updates replace older versions.
