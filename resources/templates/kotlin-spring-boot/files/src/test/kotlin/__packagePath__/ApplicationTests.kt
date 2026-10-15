package {{package}}

import org.junit.jupiter.api.Test
import org.springframework.beans.factory.annotation.Autowired
import org.springframework.boot.test.context.SpringBootTest
import org.springframework.boot.webmvc.test.autoconfigure.AutoConfigureMockMvc
import org.springframework.test.web.servlet.MockMvc
import org.springframework.test.web.servlet.get

@SpringBootTest
@AutoConfigureMockMvc
class ApplicationTests(@Autowired val mockMvc: MockMvc) {

    @Test
    fun greetsTheWorldByDefault() {
        mockMvc.get("/").andExpect {
            status { isOk() }
            jsonPath("$.message") { value("Hello, World!") }
        }
    }

    @Test
    fun greetsByName() {
        mockMvc.get("/") { param("name", "Kotlin") }.andExpect {
            status { isOk() }
            jsonPath("$.message") { value("Hello, Kotlin!") }
        }
    }
}
