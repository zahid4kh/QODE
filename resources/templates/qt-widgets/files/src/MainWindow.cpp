#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenuBar>
#include <QPushButton>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("{{name}}"));
    resize(420, 280);

    m_label = new QLabel(tr("Hello from {{name}}!"), this);
    m_label->setAlignment(Qt::AlignCenter);
    auto *button = new QPushButton(tr("Click me"), this);
    connect(button, &QPushButton::clicked, this, &MainWindow::clicked);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->addStretch(1);
    layout->addWidget(m_label);
    layout->addWidget(button, 0, Qt::AlignCenter);
    layout->addStretch(1);
    setCentralWidget(central);

    QMenu *file = menuBar()->addMenu(tr("&File"));
    QAction *quit = file->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);
    quit->setMenuRole(QAction::QuitRole);
}

void MainWindow::clicked()
{
    ++m_count;
    m_label->setText(tr("Clicked %n time(s)", nullptr, m_count));
}
