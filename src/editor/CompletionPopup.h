#pragma once

#include "lsp/LspTypes.h"

#include <QColor>
#include <QFrame>
#include <QVector>

class QListView;

// The suggestion list under the caret. It never takes focus: CodeEditor keeps it, feeds the typed prefix in
// with setPrefix() and forwards navigation keys. Items are filtered and ranked locally as the prefix grows.
class CompletionPopup : public QFrame
{
    Q_OBJECT
public:
    explicit CompletionPopup(QWidget *editor);

    void setItems(const QVector<LspCompletionItem> &items);
    // Filters by `prefix` (case-insensitive prefix first, then camel-case / subsequence matches). Returns the
    // number of items left.
    int setPrefix(const QString &prefix);
    int count() const;
    void moveSelection(int delta);
    const LspCompletionItem *current() const;
    // Shows the list with its top-left at `below` (global); flips above `above` when it would leave the screen.
    void popup(const QPoint &below, const QPoint &above);

signals:
    void accepted(); // an entry was clicked

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void applyTheme();
    QColor m_bg, m_border;

    class Model;
    Model *m_model;
    QListView *m_view;
};
