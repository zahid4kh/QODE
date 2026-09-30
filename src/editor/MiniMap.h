#pragma once

#include <QVector>
#include <QWidget>

class CodeEditor;
class QTimer;

// Scaled-down overview of a document, drawn to the right of the editor: one 3px row per visible line
// with the syntax colours of the real text, the part currently on screen and the git change marks.
// Clicking or dragging scrolls the editor.
class MiniMap : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kWidth = 112;
    static constexpr int kRowHeight = 3;

    explicit MiniMap(CodeEditor *editor);

    void invalidate();     // the text or its folds changed
    void scheduleUpdate(); // repaint soon (coalesced)

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void rebuildRows();
    int firstShownRow() const;
    int rowForBlock(int blockNumber) const; // first row whose block number is >= blockNumber
    void scrollToY(int y);

    CodeEditor *m_editor;
    QVector<int> m_rows; // visible blocks, in order (folded lines are skipped)
    bool m_dirty = true;
    bool m_dragging = false;
    QTimer *m_timer;
};
