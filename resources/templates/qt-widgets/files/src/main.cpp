#include "MainWindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("{{name}}"));
{{#if resources}}
    app.setWindowIcon(QIcon(QStringLiteral(":/icon.svg")));
{{/if}}

    MainWindow window;
    window.show();
    return app.exec();
}
