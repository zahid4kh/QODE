package {{package}}

import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.RequestParam
import org.springframework.web.bind.annotation.RestController

data class Greeting(val message: String)

@RestController
class HelloController {

    @GetMapping("/")
    fun index(@RequestParam(defaultValue = "World") name: String) = Greeting("Hello, $name!")
}
