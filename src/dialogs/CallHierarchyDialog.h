#pragma once

#include "lsp/LspTypes.h"

#include <QDialog>
#include <QString>
#include <QVector>
#include <functional>

class QLabel;
class QRadioButton;
class QTreeWidget;
class QTreeWidgetItem;

// Call Hierarchy: a function with the places that call it (callers) or the functions it calls (callees), expanded one level
// at a time as the tree is opened. Double-click or Enter jumps to the function. Modeless, one per window: a new request
// replaces the tree.
class CallHierarchyDialog : public QDialog
{
    Q_OBJECT
public:
    // Asks the server for the calls of an item; the answer (ok = false when the request failed) goes to `done`.
    using Fetch = std::function<void(const LspCallItem &item, bool incoming, std::function<void(const QVector<LspCallItem> &, bool ok)> done)>;

    explicit CallHierarchyDialog(QWidget *parent = nullptr);
    void setFetch(Fetch fetch) { m_fetch = std::move(fetch); }
    // `root` shortens the shown paths.
    void setRoot(const LspCallItem &item, const QString &root);

signals:
    void openRequested(const QString &path, int line, int column); // 0-based

private:
    void rebuild();
    void expand(QTreeWidgetItem *node);
    void fill(QTreeWidgetItem *node, const QVector<LspCallItem> &calls);
    QTreeWidgetItem *makeNode(const LspCallItem &item, QTreeWidgetItem *parent, bool placeholder);
    void activate(QTreeWidgetItem *node);

    QLabel *m_title;
    QRadioButton *m_callers, *m_callees;
    QTreeWidget *m_tree;
    Fetch m_fetch;
    LspCallItem m_root;
    QString m_projectRoot;
    QVector<LspCallItem> m_items; // what the tree nodes refer to
};
