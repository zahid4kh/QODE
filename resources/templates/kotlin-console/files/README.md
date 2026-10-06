# {{appName}}

A Kotlin console application built with Gradle.

```bash
./gradlew run                    # run the app
./gradlew run --args="a b c"     # pass arguments
./gradlew test                   # run the tests
./gradlew build                  # compile, test and package
```

The Gradle wrapper (`gradlew`) downloads the right Gradle version on first use; only a JDK is required.
Dependencies are declared in `gradle/libs.versions.toml`.
