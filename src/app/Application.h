#pragma once

#include <QApplication>

class Application : public QApplication
{
    Q_OBJECT
public:
    Application(int &argc, char **argv);

    void applyTheme(const QString &name);
    // Scales fonts, stylesheet sizes, icon/fixed sizes and layout margins to SettingsManager::uiZoom. A widget
    // that sizes itself for the current zoom (SettingsManager::scaled) sets the property "qodeZoomed" so its
    // minimum/maximum size is not scaled a second time. A window with the property "qodeNoZoom" (the zoom
    // popup) is never scaled; it sets its own font at 100%.
    void applyZoom();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void scaleWidget(QWidget *w, double fontRatio = 1.0);
    QString scaled(const QString &sheet) const;
    double m_zoom = 1.0;
};
