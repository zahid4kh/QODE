#pragma once

#include "TerminalScreen.h"

#include <QAbstractScrollArea>

// Paints a TerminalScreen and turns keyboard/mouse input into pty bytes.
class TerminalView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit TerminalView(QWidget *parent = nullptr);

    TerminalScreen *screen() const { return m_screen; }
    QString selectedText() const;
    void copy();
    void paste();
    void selectAll();
    void clearSelection();
    void scrollToBottom();
    void applySettings();

signals:
    void input(const QByteArray &data); // bytes to send to the shell
    void sizeChanged(int cols, int rows);
    void focusGained();
    void returnPressedWhileInactive();  // user hit Enter with no shell running

public:
    void setShellActive(bool active) { m_shellActive = active; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;

private:
    void onScreenChanged();
    void updateMetrics();
    void updateScrollBar();
    void recalcSize();
    QColor resolve(quint32 color, bool foreground) const;
    QPoint cellAt(const QPoint &pos) const; // (absolute line, column)
    QByteArray encodeKey(QKeyEvent *event) const;
    bool selectionActive() const { return m_selStart != m_selEnd; }

    TerminalScreen *m_screen;
    QFont m_font;
    qreal m_cw = 8, m_ch = 16;
    int m_ascent = 12;
    bool m_atBottom = true;
    int m_wheelAcc = 0;
    bool m_shellActive = true;
    bool m_focused = false;
    bool m_selecting = false;
    QPoint m_selStart {0, 0}, m_selEnd {0, 0}; // (line, col) — stored as x=col? see cellAt: x=col, y=line
    QColor m_bg, m_fg;
    QColor m_ansi[16];
};
