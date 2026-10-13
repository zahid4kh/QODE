#pragma once

#include <QTextBrowser>

class QTextTable;
class QTimer;

// QTextBrowser that renders Markdown the way QODE shows it everywhere (preview panel, AI chat): themed colours, rounded
// code boxes with a copy link, inline code pills, tables with borders, images never wider than the view.
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

    // Handles the `qode-copy` links of code boxes (copies the box under the cursor). True when `url` was one.
    bool handleCopyLink(const QUrl &url);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    QVector<QTextTable *> codeBoxes() const;
    void polish();
    void fitImages();
    void updateHeight();

    QString m_markdown;
    QTimer *m_resizeTimer;
    bool m_autoHeight = false;
    QColor m_boxBg, m_headBg, m_edge, m_pillBg;
    QVector<QPair<int, int>> m_pills; // (position, length) of inline code
};
