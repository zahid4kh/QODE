#include "MainWindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("QODE"));
    resize(1200, 760);
}

void MainWindow::openInitialPaths(const QStringList &)
{
}
