package {{package}}

import kotlin.test.Test
import kotlin.test.assertEquals

class MainTest {
    @Test
    fun greetsByName() {
        assertEquals("Hello, Kotlin!", greeting("Kotlin"))
    }
}
