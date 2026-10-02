#pragma once

#include <QColor>
#include <QFrame>

class QTextBrowser;

// The language server's hover text. Unlike a tooltip it stays put when the mouse leaves, its text can be
// selected and copied, and it closes on a click outside it (or Esc).
class HoverPopup : public QFrame
{
    Q_OBJECT
public:
    explicit HoverPopup(QWidget *editor);

    // Shows `html` with its top-left near `global`, kept on screen.
    void showHtml(const QString &html, const QPoint &global);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QColor m_bg, m_border;
    QTextBrowser *m_view;
};
