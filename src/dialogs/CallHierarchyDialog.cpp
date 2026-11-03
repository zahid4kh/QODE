#include "CallHierarchyDialog.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QRadioButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
enum Role { RoleIndex = Qt::UserRole + 1, RoleState };
enum State { Loaded, NotLoaded, Loading, Recursive };
}

CallHierarchyDialog::CallHierarchyDialog(Kind kind, QWidget *parent)
    : QDialog(parent), m_kind(kind)
{
    const bool calls = kind == Kind::Calls;
    setWindowTitle(calls ? tr("Call Hierarchy") : tr("Type Hierarchy"));
    setModal(false);
    resize(720, 460);
    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    m_title = new QLabel(this);
    top->addWidget(m_title, 1);
    m_up = new QRadioButton(calls ? tr("Callers") : tr("Supertypes"), this);
    m_down = new QRadioButton(calls ? tr("Callees") : tr("Subtypes"), this);
    m_up->setChecked(true);
    m_up->setToolTip(calls ? tr("Who calls this function") : tr("The classes and interfaces this type extends or implements"));
    m_down->setToolTip(calls ? tr("What this function calls") : tr("The types that extend or implement this one"));
    top->addWidget(m_up);
    top->addWidget(m_down);
    layout->addLayout(top);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setIndentation(16);
    layout->addWidget(m_tree, 1);
    connect(m_tree, &QTreeWidget::itemExpanded, this, &CallHierarchyDialog::expand);
    connect(m_tree, &QTreeWidget::itemActivated, this, &CallHierarchyDialog::activate);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &CallHierarchyDialog::activate);
    connect(m_up, &QRadioButton::toggled, this, [this](bool on) {
        if (on)
            rebuild();
    });
    connect(m_down, &QRadioButton::toggled, this, [this](bool on) {
        if (on)
            rebuild();
    });
}

void CallHierarchyDialog::setRoot(const LspCallItem &item, const QString &root, bool down)
{
    m_root = item;
    m_projectRoot = root;
    setWindowTitle(m_kind == Kind::Calls ? tr("Call Hierarchy of %1").arg(item.name) : tr("Type Hierarchy of %1").arg(item.name));
    {
        QSignalBlocker b1(m_up), b2(m_down);
        (down ? m_down : m_up)->setChecked(true);
    }
    rebuild();
}

QTreeWidgetItem *CallHierarchyDialog::makeNode(const LspCallItem &item, QTreeWidgetItem *parent, bool placeholder)
{
    auto *node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    const QString file = m_projectRoot.isEmpty() ? item.path : QDir(m_projectRoot).relativeFilePath(item.path);
    QString text = item.name;
    if (!item.detail.isEmpty())
        text += QStringLiteral("  ") + item.detail;
    if (!item.path.isEmpty()) // a library class without source has none
        text += QStringLiteral("    %1:%2").arg(QFileInfo(file).fileName()).arg(item.line + 1);
    if (item.callSites.size() > 1)
        text += QStringLiteral("  ×%1").arg(item.callSites.size());
    node->setText(0, text);
    QString tip = QStringLiteral("%1:%2").arg(item.path).arg(item.line + 1);
    if (!item.callSites.isEmpty()) {
        QStringList lines;
        for (const auto &site : item.callSites)
            lines << QString::number(site.first + 1);
        tip += QStringLiteral("\n") + tr("Call sites in %1: line %2").arg(QFileInfo(item.callSitePath).fileName(), lines.join(QStringLiteral(", ")));
    }
    node->setToolTip(0, tip);
    m_items.append(item);
    node->setData(0, RoleIndex, int(m_items.size()) - 1);
    node->setData(0, RoleState, placeholder ? NotLoaded : Loaded);
    if (placeholder)
        node->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator); // opened on demand
    return node;
}

void CallHierarchyDialog::rebuild()
{
    m_tree->clear();
    m_items.clear();
    const bool up = m_up->isChecked();
    if (m_kind == Kind::Calls)
        m_title->setText(up ? tr("Callers of “%1”").arg(m_root.name) : tr("Functions called by “%1”").arg(m_root.name));
    else
        m_title->setText(up ? tr("Supertypes of “%1”").arg(m_root.name) : tr("Subtypes of “%1”").arg(m_root.name));
    QTreeWidgetItem *root = makeNode(m_root, nullptr, true);
    QFont f = root->font(0);
    f.setBold(true);
    root->setFont(0, f);
    root->setExpanded(true);
    expand(root); // loads the first level (a no-op when the expansion signal already did)
}

void CallHierarchyDialog::expand(QTreeWidgetItem *node)
{
    if (node->data(0, RoleState).toInt() != NotLoaded || !m_fetch)
        return;
    node->setData(0, RoleState, Loading);
    auto *wait = new QTreeWidgetItem(node);
    wait->setText(0, tr("Loading…"));
    wait->setData(0, RoleState, Loaded);
    const LspCallItem item = m_items.value(node->data(0, RoleIndex).toInt());
    const bool up = m_up->isChecked();
    QPointer<CallHierarchyDialog> guard(this);
    const int index = node->data(0, RoleIndex).toInt();
    m_fetch(item, up, [guard, index, up](const QVector<LspCallItem> &calls, bool ok) {
        if (!guard || guard->m_up->isChecked() != up)
            return; // the direction changed meanwhile: that tree is gone
        // Find the node again: the tree may have been rebuilt.
        QTreeWidgetItem *target = nullptr;
        QList<QTreeWidgetItem *> stack;
        for (int i = 0; i < guard->m_tree->topLevelItemCount(); ++i)
            stack << guard->m_tree->topLevelItem(i);
        while (!stack.isEmpty()) {
            QTreeWidgetItem *n = stack.takeLast();
            if (n->data(0, RoleState).toInt() == Loading && n->data(0, RoleIndex).toInt() == index) {
                target = n;
                break;
            }
            for (int i = 0; i < n->childCount(); ++i)
                stack << n->child(i);
        }
        if (!target)
            return;
        qDeleteAll(target->takeChildren());
        target->setData(0, RoleState, Loaded);
        if (!ok) {
            auto *err = new QTreeWidgetItem(target);
            err->setText(0, tr("The language server could not answer"));
            return;
        }
        guard->fill(target, calls);
    });
}

void CallHierarchyDialog::fill(QTreeWidgetItem *node, const QVector<LspCallItem> &calls)
{
    if (calls.isEmpty()) {
        auto *none = new QTreeWidgetItem(node);
        if (m_kind == Kind::Calls)
            none->setText(0, m_up->isChecked() ? tr("No callers found") : tr("Calls nothing"));
        else
            none->setText(0, m_up->isChecked() ? tr("No supertypes") : tr("No subtypes found"));
        none->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
        return;
    }
    // A function (type) already on the path to the root is shown but not expanded again (recursion).
    QList<QPair<QString, QPair<int, int>>> path;
    for (QTreeWidgetItem *p = node; p; p = p->parent()) {
        const LspCallItem &it = m_items.at(p->data(0, RoleIndex).toInt());
        path.append({it.path, {it.line, it.column}});
    }
    for (const LspCallItem &call : calls) {
        const bool recursive = path.contains(qMakePair(call.path, qMakePair(call.line, call.column)));
        QTreeWidgetItem *child = makeNode(call, node, !recursive);
        if (recursive) {
            child->setText(0, child->text(0) + tr("  (recursive)"));
            child->setData(0, RoleState, Recursive);
        }
    }
}

void CallHierarchyDialog::activate(QTreeWidgetItem *node)
{
    if (!node || node->data(0, RoleIndex).isNull())
        return;
    const LspCallItem &it = m_items.at(node->data(0, RoleIndex).toInt());
    if (!it.path.isEmpty())
        emit openRequested(it.path, it.line, it.column);
}
