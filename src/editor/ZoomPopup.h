#pragma once

#include <QFrame>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;

// Small popup above the status-bar zoom button: − / + buttons, a percentage, a slider and Reset.
// It edits SettingsManager::uiZoom (per project) and follows it while open. The popup itself always stays at
// 100% so it does not grow or shrink under the pointer, and it shows a new value at once but applies it only
// once the user pauses (or lets go of the slider), since re-zooming the whole window is slow.
class ZoomPopup : public QFrame
{
    Q_OBJECT
public:
    // Shows the popup above `anchor`. It deletes itself when closed.
    static void open(QWidget *anchor);

private:
    explicit ZoomPopup(QWidget *anchor);
    void refresh();
    void hideEvent(QHideEvent *event) override; // applies a value still waiting
    void showValue(int percent); // the popup's own labels and controls
    void request(int percent);
    void apply();

    QToolButton *m_minus, *m_plus;
    QLabel *m_percent, *m_size;
    QSlider *m_slider;
    QPushButton *m_reset;
    QTimer *m_apply;
    int m_pending = 0; // the value shown but not applied yet, 0 when none
};
