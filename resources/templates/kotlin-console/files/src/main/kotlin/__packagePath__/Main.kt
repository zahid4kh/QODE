package {{package}}
{{#if coroutines}}

import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
{{/if}}
{{#if serialization}}

import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

@Serializable
data class Greeting(val message: String, val arguments: List<String>)
{{/if}}

fun greeting(name: String): String = "Hello, $name!"

{{#if coroutines}}
fun main(args: Array<String>) = runBlocking {
    delay(100)
{{/if}}
{{#if !coroutines}}
fun main(args: Array<String>) {
{{/if}}
    println(greeting("{{appName}}"))
    if (args.isNotEmpty()) println("Arguments: ${args.joinToString()}")
{{#if serialization}}
    println(Json.encodeToString(Greeting(greeting("{{appName}}"), args.toList())))
{{/if}}
}
