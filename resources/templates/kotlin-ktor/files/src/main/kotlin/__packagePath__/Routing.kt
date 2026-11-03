package {{package}}

import io.ktor.server.application.Application
import io.ktor.server.response.respondText
import io.ktor.server.routing.get
import io.ktor.server.routing.routing

fun Application.configureRouting() {
    routing {
        get("/") {
            val name = call.request.queryParameters["name"] ?: "World"
            call.respondText("Hello, $name!")
        }
    }
}
