#pragma once

#include <QMainWindow>

class QLabel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void clicked();

    QLabel *m_label;
    int m_count = 0;
};
