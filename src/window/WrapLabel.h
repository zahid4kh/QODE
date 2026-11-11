#pragma once

#include <QFont>
#include <QLabel>
#include <QPair>
#include <QVector>

// A word-wrapped QLabel that is cheap to lay out. A wrapped QLabel lays its text out again every time a layout asks
// for its height, and nested layouts ask a dozen times per resize step, so a column of them (the tool rows of a long
// chat transcript, 600 in a long Claude Code conversation) made each step of a splitter drag cost hundreds of ms.
// This one answers from what it measured: for any width at least as wide as the text, its one-line height (measured
// once per text and font), and for narrower widths the last few heights QLabel computed. The heights are QLabel's own.
class WrapLabel : public QLabel
{
public:
    explicit WrapLabel(QWidget *parent = nullptr);
    explicit WrapLabel(const QString &text, QWidget *parent = nullptr);

    int heightForWidth(int width) const override;

protected:
    void changeEvent(QEvent *event) override;

private:
    void measure() const;

    mutable bool m_measured = false;
    mutable QString m_text; // what was measured
    mutable QFont m_font;
    mutable int m_fitWidth = 0;  // the narrowest width known to show the text unwrapped; 0 = not known
    mutable int m_fitHeight = 0; // the height at that width (and any wider one)
    mutable QVector<QPair<int, int>> m_cache; // (width, height) of the last few narrower widths asked about
};
