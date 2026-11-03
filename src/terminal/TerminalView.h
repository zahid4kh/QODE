#pragma once

#include "TerminalScreen.h"

#include <QAbstractScrollArea>

#include <functional>

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
    void sendPaste(QString text);
    void selectAll();
    void clearSelection();
    void scrollToBottom();
    void applySettings();

    // Bytes a terminal program expects for a key press (`appCursor`: DECCKM application cursor keys).
    static QByteArray encodeKey(QKeyEvent *event, bool appCursor);
    // Clipboard -> bytes for a program on the pty (bracketed text, copied files as paths, or the
    // empty paste / Ctrl+V that makes Claude Code read an image itself).
    static QByteArray pasteBytes(bool bracketed);
    static QByteArray textPasteBytes(QString text, bool bracketed);
    static bool clipboardIsImageOnly();
    // Folders a relative file location in the output is looked up in (the shell's current folder, the project), asked when
    // the mouse rests on one. A make "Entering directory" line above it is tried first.
    void setLinkFolders(std::function<QStringList()> provider) { m_linkFolders = std::move(provider); }

signals:
    void input(const QByteArray &data); // bytes to send to the shell
    void sizeChanged(int cols, int rows);
    void focusGained();
    void returnPressedWhileInactive();  // user hit Enter with no shell running
    // Ctrl+click on a file location in the output ("src/main.cpp:12:5: error: ..."); 1-based line, column 0 when absent.
    void fileLinkActivated(const QString &path, int line, int column);

public:
    void setShellActive(bool active) { m_shellActive = active; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
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
    bool selectionActive() const { return m_selStart != m_selEnd; }
    QString lineText(int absoluteLine, QVector<int> *columns = nullptr) const; // columns: cell of each character
    // A file location in the output that exists on disk: the cells it covers and where it points.
    struct Link {
        int line = -1, start = 0, end = 0; // absolute line, cells [start, end)
        QString path;
        int row = 0, column = 0;
        bool operator==(const Link &o) const { return line == o.line && start == o.start && end == o.end; }
    };
    Link linkAt(const QPoint &cell) const;
    QString resolveLink(const QString &path, int line) const;
    void setHoveredLink(const Link &link, bool ctrl);

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
    QColor m_sel;
    std::function<QStringList()> m_linkFolders;
    Link m_link; // under the mouse
};
