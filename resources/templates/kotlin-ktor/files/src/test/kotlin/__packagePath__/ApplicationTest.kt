package {{package}}

import io.ktor.client.request.get
import io.ktor.client.request.parameter
import io.ktor.client.statement.bodyAsText
import io.ktor.http.HttpStatusCode
import io.ktor.server.testing.testApplication
import kotlin.test.Test
import kotlin.test.assertEquals

class ApplicationTest {

    @Test
    fun greetsTheWorldByDefault() = testApplication {
        application { module() }
        val response = client.get("/")
        assertEquals(HttpStatusCode.OK, response.status)
        assertEquals("Hello, World!", response.bodyAsText())
    }

    @Test
    fun greetsByName() = testApplication {
        application { module() }
        assertEquals("Hello, Kotlin!", client.get("/") { parameter("name", "Kotlin") }.bodyAsText())
    }
}
