# {{appName}}

A Compose Multiplatform desktop application (Material 3, Koin, kotlinx-serialization).

```bash
./gradlew run                                          # run the app
./gradlew :hotRun --mainClass {{className}} --auto   # run with hot reload: edit, save, see it update
./gradlew packageDeb                                   # Linux installer (build/compose/binaries)
./gradlew packageMsi                                   # Windows installer (on Windows)
./gradlew packageDmg                                   # macOS installer (on macOS)
```

The window and installer icons live in `icons/`; the window icon is also in `src/main/composeResources/drawable/`.
Before shipping a Windows installer keep `upgradeUuid` in `build.gradle.kts` unchanged so updates replace older versions.
