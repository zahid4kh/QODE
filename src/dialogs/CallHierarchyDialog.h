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

// Call Hierarchy: a function with the places that call it (callers) or the functions it calls (callees). Type Hierarchy: a
// class / interface with its supertypes or its subtypes. Both expand one level at a time as the tree is opened; double-click
// or Enter jumps to the declaration. Modeless, one of each per window: a new request replaces the tree.
class CallHierarchyDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Kind { Calls, Types };
    // Asks the server for the next level of an item: `up` = callers / supertypes, else callees / subtypes. The answer
    // (ok = false when the request failed) goes to `done`.
    using Fetch = std::function<void(const LspCallItem &item, bool up, std::function<void(const QVector<LspCallItem> &, bool ok)> done)>;

    explicit CallHierarchyDialog(Kind kind, QWidget *parent = nullptr);
    void setFetch(Fetch fetch) { m_fetch = std::move(fetch); }
    // `root` shortens the shown paths; `down` starts with callees / subtypes instead of callers / supertypes.
    void setRoot(const LspCallItem &item, const QString &root, bool down = false);

signals:
    void openRequested(const QString &path, int line, int column); // 0-based

private:
    void rebuild();
    void expand(QTreeWidgetItem *node);
    void fill(QTreeWidgetItem *node, const QVector<LspCallItem> &calls);
    QTreeWidgetItem *makeNode(const LspCallItem &item, QTreeWidgetItem *parent, bool placeholder);
    void activate(QTreeWidgetItem *node);

    QLabel *m_title;
    Kind m_kind;
    QRadioButton *m_up, *m_down; // callers / supertypes, callees / subtypes
    QTreeWidget *m_tree;
    Fetch m_fetch;
    LspCallItem m_root;
    QString m_projectRoot;
    QVector<LspCallItem> m_items; // what the tree nodes refer to
};
