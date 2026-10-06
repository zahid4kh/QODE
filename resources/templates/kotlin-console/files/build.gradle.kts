plugins {
    alias(libs.plugins.kotlin.jvm)
{{#if serialization}}
    alias(libs.plugins.kotlin.plugin.serialization)
{{/if}}
    application
}

group = "{{package}}"
version = "1.0.0"

repositories {
    mavenCentral()
}

dependencies {
{{#if coroutines}}
    implementation(libs.kotlinx.coroutines.core)
{{/if}}
{{#if serialization}}
    implementation(libs.kotlinx.serialization.json)
{{/if}}
    testImplementation(kotlin("test"))
}

kotlin {
    jvmToolchain({{jdk}})
}

application {
    mainClass = "{{package}}.MainKt"
}

tasks.test {
    useJUnitPlatform()
}
