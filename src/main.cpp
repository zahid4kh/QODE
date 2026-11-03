#include "app/Application.h"
#include "app/ChildReaper.h"
#include "app/StartupProfile.h"
#include "window/MainWindow.h"

#include <QCommandLineParser>

#include <csignal>

int main(int argc, char *argv[])
{
    // The watchdog that stops QODE's servers when QODE itself ends abruptly is this same binary.
    if (ChildReaper::runIfReaper(argc, argv))
        return 0;
    StartupProfile::begin();
    // Writing to a child process (git, the shell) that already exited must fail with an error, not kill the editor.
    std::signal(SIGPIPE, SIG_IGN);
    // Qt WebEngine (the web preview) shares OpenGL contexts with the widgets; this must be set before the application exists.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    Application app(argc, argv);
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
