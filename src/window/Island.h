#pragma once

#include <QWidget>

// Wraps a widget in a rounded "island": an overlay repaints the outer corners in the frame colour so the
// content (whose own widgets are square) appears clipped with smooth, antialiased edges.
class Island : public QWidget
{
    Q_OBJECT
public:
    explicit Island(QWidget *content, QWidget *parent = nullptr);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    class Overlay;
    Overlay *m_overlay;
};
