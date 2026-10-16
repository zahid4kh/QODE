#pragma once

#include <QFrame>

class QLabel;
class QPushButton;
class QSlider;
class QToolButton;

// Small popup above the status-bar zoom button: − / + buttons, a percentage, a slider and Reset.
// It edits SettingsManager::uiZoom (per project) and follows it while open.
class ZoomPopup : public QFrame
{
    Q_OBJECT
public:
    // Shows the popup above `anchor`. It deletes itself when closed.
    static void open(QWidget *anchor);

private:
    explicit ZoomPopup(QWidget *anchor);
    void refresh();
    void step(int delta);

    QToolButton *m_minus, *m_plus;
    QLabel *m_percent, *m_size;
    QSlider *m_slider;
    QPushButton *m_reset;
};
