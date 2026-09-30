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
    parser.process(app);

    MainWindow window;
    window.show();
    window.openInitialPaths(parser.positionalArguments());

    return app.exec();
}
