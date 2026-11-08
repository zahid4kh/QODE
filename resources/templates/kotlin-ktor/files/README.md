# {{appName}}

A [Ktor](https://ktor.io) server written in Kotlin, running on Netty and built with Gradle. Ktor is
modular: add plugins (content negotiation, serialization, authentication, …) from Ktor's version
catalog, e.g. `implementation(ktorLibs.server.contentNegotiation)` in `build.gradle.kts`.

{{#if !os=win}}
```bash
./gradlew run        # start the server on http://localhost:{{port}}
./gradlew test       # run the tests
./gradlew build      # compile, test and build the jar (build/libs)
./gradlew buildFatJar  # a runnable jar with every dependency
```
{{/if}}
{{#if os=win}}
```bat
.\gradlew.bat run          # start the server on http://localhost:{{port}}
.\gradlew.bat test         # run the tests
.\gradlew.bat build        # compile, test and build the jar (build\libs)
.\gradlew.bat buildFatJar  # a runnable jar with every dependency
```
{{/if}}

Try it: `curl "http://localhost:{{port}}/?name=Kotlin"` → `Hello, Kotlin!`, or open `requests.http`.

The port and the modules to load live in `src/main/resources/application.yaml`; routes are in
`Routing.kt`. The Gradle wrapper (`gradlew`) downloads the right Gradle version on first use;
only a JDK is required.
