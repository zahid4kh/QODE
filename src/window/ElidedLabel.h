#pragma once

#include <QLabel>

// A one-line label for text of any length (a path, a URL) that never makes its window wider: it asks for no width of
// its own and shows the text elided to the width it gets, the whole text in the tooltip. A plain QLabel without word
// wrap asks for its full text width, and a panel in the right-hand island then widened the main window to fit it.
class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(QWidget *parent = nullptr, Qt::TextElideMode mode = Qt::ElideMiddle);

    void setFullText(const QString &text);
    QString fullText() const { return m_full; }

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void refit();

    QString m_full;
    Qt::TextElideMode m_mode;
};
