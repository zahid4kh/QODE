#pragma once

#include "settings/Theme.h"

#include <QStyledItemDelegate>

class GitRepository;

// Draws explorer rows tinted by git state plus a status badge on the right:
//   files   - a filled pill for the staged change, a plain letter for the unstaged one
//   folders - a dot when something inside changed
//   ignored - dimmed text
class GitItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    GitItemDelegate(GitRepository *repo, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    GitRepository *m_repo;
    Theme m_theme;
};
