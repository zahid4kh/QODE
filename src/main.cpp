#include "app/Application.h"
#include "window/MainWindow.h"

#include <QCommandLineParser>

#include <csignal>

int main(int argc, char *argv[])
{
    // Writing to a child process (git, the shell) that already exited must fail with an error, not kill the editor.
    std::signal(SIGPIPE, SIG_IGN);
    Application app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("QODE - native Qt code editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("Project directory or file to open."));
    const QCommandLineOption newWindow(QStringList{QStringLiteral("n"), QStringLiteral("new-window")},
                                       QStringLiteral("Start with an empty window instead of restoring the last session."));
    parser.addOption(newWindow);
    parser.process(app);

    MainWindow window;
    if (parser.isSet(newWindow))
        window.move(window.pos() + QPoint(32, 32)); // cascade so it does not sit exactly on the other window
    window.show();
    window.openInitialPaths(parser.positionalArguments(), !parser.isSet(newWindow));

    return app.exec();
}
