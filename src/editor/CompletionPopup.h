#pragma once

#include "lsp/LspTypes.h"

#include <QColor>
#include <QFrame>
#include <QVector>

class QListView;
class QTextBrowser;
class QFrame;

// The suggestion list under the caret. It never takes focus: CodeEditor keeps it, feeds the typed prefix in
// with setPrefix() and forwards navigation keys. Items are filtered and ranked locally as the prefix grows.
class CompletionPopup : public QFrame
{
    Q_OBJECT
public:
    explicit CompletionPopup(QWidget *editor);

    // `extra` (QODE's snippets) is listed after `items` without copying the server's list into a bigger one first.
    void setItems(const QVector<LspCompletionItem> &items, const QVector<LspCompletionItem> &extra = {});
    // Filters by `prefix` (case-insensitive prefix first, then camel-case / subsequence matches). Returns the
    // number of items left.
    int setPrefix(const QString &prefix);
    int count() const;
    void moveSelection(int delta);
    const LspCompletionItem *current() const;
    // Shows the list with its top-left at `below` (global); flips above `above` when it would leave the screen.
    void popup(const QPoint &below, const QPoint &above);

    // Documentation panel beside the list for the selected entry (rich text); empty hides it. The popup places it to the
    // right of the list, or to the left when the screen has no room.
    void showDocs(const QString &html);
    bool docsVisible() const;

signals:
    void accepted(); // an entry was clicked
    void currentItemChanged(); // the selection moved (arrow keys, filtering, hover-free)

protected:
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void applyTheme();
    QColor m_bg, m_border;

    void placeDocs();
    class Model;
    class DocsFrame;
    Model *m_model;
    QListView *m_view;
    DocsFrame *m_docs = nullptr;
};
