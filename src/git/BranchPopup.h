#pragma once

#include <QFrame>

class GitRepository;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

// Searchable branch switcher shown under the branch buttons: local and remote branches, the active
// one marked with a green dot, plus "Create New Branch…".
class BranchPopup : public QFrame
{
    Q_OBJECT
public:
    // Shows the popup below `anchor` (or above it when `above` is set). It deletes itself when closed.
    static void open(GitRepository *repo, QWidget *anchor, bool above);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    BranchPopup(GitRepository *repo, QWidget *anchor);
    void rebuild();
    void activate(QListWidgetItem *item);
    void selectFirstBranch(int from, int step);
    void showItemMenu(const QPoint &pos);

    GitRepository *m_repo;
    QWidget *m_anchor;
    QLineEdit *m_filter;
    QListWidget *m_list;
};
