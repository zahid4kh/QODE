#pragma once

#include <QWidget>

// Wraps a widget in a rounded "island": an overlay repaints the outer corners in the frame colour so the
// content (whose own widgets are square) appears clipped with smooth, antialiased edges.
class Island : public QWidget
{
    Q_OBJECT
public:
    explicit Island(QWidget *content, QWidget *parent = nullptr);

    // While sliding, the island may be narrower than its content's minimum: the content keeps `fullWidth`,
    // anchored to the right edge, and is clipped. Ending restores the normal layout.
    void setSliding(bool on, int fullWidth = 0);
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    class Overlay;
    Overlay *m_overlay;
    QWidget *m_content = nullptr;
    bool m_sliding = false;
    int m_fullWidth = 0;
};
