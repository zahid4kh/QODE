#pragma once

#include <QList>
#include <QWidget>

class QTextDocument;

// The bar above an editor: where the file lives, then the classes / functions / headings around the caret.
class Breadcrumbs : public QWidget
{
    Q_OBJECT
public:
    struct Crumb {
        QString text;
        int line = -1; // 0-based line to jump to; -1 for path segments
    };

    explicit Breadcrumbs(QWidget *parent = nullptr);

    // A "Preview" button at the right end of the bar (Markdown / SVG files).
    void setPreviewAvailable(bool on);
    void setCrumbs(const QList<Crumb> &path, const QList<Crumb> &symbols);

    struct Symbol {
        QString name;
        QString parent; // enclosing definition, empty at top level
        int line = 0;   // 0-based
    };
    // Every definition in the document, in order.
    static QList<Symbol> documentSymbols(const QTextDocument *doc, const QString &language);
    // The chain of definitions (outermost first) that contain `blockNumber`.
    static QList<Crumb> symbolChain(const QTextDocument *doc, int blockNumber, const QString &language);
    // Path segments of `filePath`, relative to `projectRoot` when inside it.
    static QList<Crumb> pathCrumbs(const QString &filePath, const QString &projectRoot);

signals:
    void lineRequested(int line);
    void previewRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    QSize sizeHint() const override { return QSize(0, 24); }

private:
    struct Shown {
        QRect rect;
        int line;
        QString text;
    };
    int crumbAt(const QPoint &pos) const;

    QList<Crumb> m_path, m_symbols;
    QList<Shown> m_shown; // laid out by the last paint
    int m_hover = -1;
    bool m_preview = false;
    bool m_previewHover = false;
    QRect m_previewRect;
};
