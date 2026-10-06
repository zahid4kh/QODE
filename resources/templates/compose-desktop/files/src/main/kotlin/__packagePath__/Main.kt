package {{package}}

import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import {{package}}.resources.Res
import {{package}}.resources.app_icon
import org.jetbrains.compose.resources.painterResource
import org.koin.core.context.startKoin
import org.koin.java.KoinJavaComponent.getKoin
import java.awt.Dimension

fun main() {
    startKoin {
        modules(appModule)
    }
    val viewModel = getKoin().get<MainViewModel>()

    application {
        Window(
            onCloseRequest = ::exitApplication,
            state = rememberWindowState(size = DpSize(900.dp, 650.dp)),
            title = "{{appName}}",
            icon = painterResource(Res.drawable.app_icon)
        ) {
            window.minimumSize = Dimension(640, 480)
            App(viewModel)
        }
    }
}
