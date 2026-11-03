package {{package}}

import io.ktor.server.application.Application
import io.ktor.server.netty.EngineMain

// The port and the modules to load come from src/main/resources/application.yaml.
fun main(args: Array<String>) {
    EngineMain.main(args)
}

fun Application.module() {
    configureRouting()
}
