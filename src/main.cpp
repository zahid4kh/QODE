#include "app/Application.h"
#include "window/MainWindow.h"

#include <QCommandLineParser>

int main(int argc, char *argv[])
{
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
