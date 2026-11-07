#include "app/Application.h"
#include "app/StartupProfile.h"
#include "platform/Platform.h"
#include "platform/ProcessTree.h"
#include "window/MainWindow.h"

#include <QCommandLineParser>

int main(int argc, char *argv[])
{
    // The watchdog that stops QODE's servers when QODE itself ends abruptly (Unix), and the helper that sends a server
    // Ctrl+C (Windows), are this same binary.
    if (ProcessTree::runIfWatchdog(argc, argv) || Platform::runIfHelper(argc, argv))
        return 0;
    StartupProfile::begin();
    // Writing to a child process (git, the shell) that already exited must fail with an error, not kill the editor.
    Platform::initProcess();
    // Qt WebEngine (the web preview) shares OpenGL contexts with the widgets; this must be set before the application exists.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    Application app(argc, argv);
    Platform::initGui();
    StartupProfile::mark(QStringLiteral("Qt application (display, fonts, style, theme)"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("QODE - native Qt code editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("Project directory or file to open."));
    const QCommandLineOption newWindow(QStringList{QStringLiteral("n"), QStringLiteral("new-window")},
                                       QStringLiteral("Start with an empty window instead of restoring the last session."));
    parser.addOption(newWindow);
    parser.process(app);

    StartupProfile::mark(QStringLiteral("command line"));
    MainWindow window;
    if (parser.isSet(newWindow))
        window.move(window.pos() + QPoint(32, 32)); // cascade so it does not sit exactly on the other window
    StartupProfile::finishOnFirstPaint(&window);
    window.show();
    StartupProfile::mark(QStringLiteral("show window"));
    window.openInitialPaths(parser.positionalArguments(), !parser.isSet(newWindow));
    StartupProfile::mark(QStringLiteral("open project / restore session"));

    return app.exec();
}
