#pragma once

#include <QPointer>
#include <QTextBrowser>
#include <QTextTable>

class CopyButton;
class QTimer;
class QToolButton;

// QTextBrowser that renders Markdown the way QODE shows it everywhere (preview panel, AI chat): themed colours, rounded
// code boxes with a Copy button that shows while the pointer is over the box, inline code pills, tables with borders,
// images never wider than the view.
class MarkdownView : public QTextBrowser
{
    Q_OBJECT
public:
    explicit MarkdownView(QWidget *parent = nullptr);

    // Renders `markdown` (see MarkdownHtml) and polishes the result. The vertical scroll position is kept.
    void showMarkdown(const QString &markdown);
    void clearMarkdown();
    const QString &markdown() const { return m_markdown; }
    // Anchors relative images; empty = none.
    void setBaseDirectory(const QString &dir);

    // Palette and default stylesheet from the active theme; re-renders the current text.
    void applyTheme();

    // A chat bubble: no scrollbars, the height follows the content.
    void setAutoHeight(bool on);

    // Caps the text column at `px` (centred when the view is wider); 0 = use the full width.
    void setReadingWidth(int px);

    // Handles the `qode-copy` links of code boxes (copies the box under the cursor). True when `url` was one.
    bool handleCopyLink(const QUrl &url);

    // A chat reply: the context menu also offers "Ask about this" and "Quote this" for the selected text.
    void setChatActions(bool on) { m_chatActions = on; }
    QString selectedPlainText() const;

    // Ctrl+wheel zooms the text in 10% steps; a badge with the percentage shows while it is not 100% and resets it when
    // clicked. setZoom (another view following this one) shows no badge.
    int zoom() const { return m_zoom; }
    void setZoom(int percent);

signals:
    void askRequested(const QString &text);
    void quoteRequested(const QString &text);
    void zoomChanged(int percent);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    bool viewportEvent(QEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    QVector<QTextTable *> codeBoxes() const;
    QRectF codeBoxRect(QTextTable *table, qreal *headerBottom = nullptr) const; // in document coordinates
    static QString codeOf(QTextTable *table);
    QTextTable *codeBoxAt(const QPoint &viewportPos) const;
    void updateCopyButton(const QPoint &viewportPos); // over the code box under the pointer, else hidden
    void polish();
    void fitImages();
    void updateHeight();
    void applyReadingWidth();
    void zoomTo(int percent, bool badge);
    void applyZoomFont();
    void placeZoomBadge();

    QString m_markdown;
    QTimer *m_resizeTimer;
    CopyButton *m_copy;
    QPointer<QTextTable> m_copyBox; // the code box the Copy button belongs to
    bool m_autoHeight = false;
    bool m_chatActions = false;
    int m_readingWidth = 0;
    int m_sideMargin = 0;
    int m_zoom = 100;
    int m_wheelRest = 0;            // wheel delta short of a step (touchpads)
    QToolButton *m_zoomBadge = nullptr;
    bool m_badgeOn = false;         // the zoom was the user's own here (not followed from another view)
    bool m_followsScroll = false;   // the badge follows the scroll areas around the view
    QColor m_boxBg, m_headBg, m_edge, m_pillBg, m_pillEdge;
    QVector<QPair<int, int>> m_pills; // (position, length) of inline code
    QVector<QPair<int, int>> m_bullets; // (block number, QTextListFormat::Style) of list items we draw ourselves
};
