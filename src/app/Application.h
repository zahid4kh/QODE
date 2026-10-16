#pragma once

#include <QApplication>

class Application : public QApplication
{
    Q_OBJECT
public:
    Application(int &argc, char **argv);

    void applyTheme(const QString &name);
    void applyZoom(); // scales fonts, stylesheet sizes, icon/fixed sizes and layout margins to SettingsManager::uiZoom

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void scaleWidget(QWidget *w);
    QString scaled(const QString &sheet) const;
    double m_zoom = 1.0;
};
