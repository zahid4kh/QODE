plugins {
    alias(libs.plugins.kotlin.jvm)
    alias(ktorLibs.plugins.ktor)
}

group = "{{package}}"
version = "0.0.1-SNAPSHOT"

application {
    mainClass = "{{package}}.ApplicationKt"
}

kotlin {
    jvmToolchain({{jdk}})
}

dependencies {
    implementation(ktorLibs.server.core)
    implementation(ktorLibs.server.netty)
    implementation(ktorLibs.server.config.yaml)
    implementation(libs.logback.classic)

    testImplementation(ktorLibs.server.testHost)
    testImplementation(libs.kotlin.test.junit)
}
