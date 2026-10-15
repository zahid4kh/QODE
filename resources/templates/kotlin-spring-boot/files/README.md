# {{appName}}

A Spring Boot backend written in Kotlin, built with Gradle. No database: add the starters you need
(`spring-boot-starter-data-jpa`, security, …) to `gradle/libs.versions.toml` and `build.gradle.kts`.

```bash
./gradlew bootRun    # start the server on http://localhost:8080
./gradlew test       # run the tests
./gradlew build      # compile, test and build the runnable jar (build/libs)
```

Try it: `curl "http://localhost:8080/?name=Kotlin"` → `{"message":"Hello, Kotlin!"}`

Settings live in `src/main/resources/application.properties`. The Gradle wrapper (`gradlew`)
downloads the right Gradle version on first use; only a JDK is required.
