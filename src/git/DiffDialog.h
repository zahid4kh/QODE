#pragma once

#include "GitDiff.h"
#include "GitTypes.h"

#include <QDialog>

class DiffView;
class GitRepository;
class QLabel;
class QPushButton;

// Side-by-side comparison of one file (modeless). Shows the two versions selected by `mode`
// with aligned rows, colour-coded changes and previous/next-change navigation.
class DiffDialog : public QDialog
{
    Q_OBJECT
public:
    DiffDialog(GitRepository *repo, const QString &absPath, GitDiffMode mode, QWidget *parent = nullptr);

private:
    void load();
    void populate(const QString &oldText, const QString &newText);
    void gotoChange(bool next);
    void applyTheme();

    GitRepository *m_repo;
    QString m_path;
    GitDiffMode m_mode;
    QLabel *m_title, *m_oldLabel, *m_newLabel, *m_stats;
    QPushButton *m_prev, *m_next, *m_action;
    DiffView *m_left, *m_right;
    QVector<int> m_hunkRows;
    QString m_shownOld, m_shownNew;
    bool m_haveShown = false;
    int m_pending = 0;
    QString m_oldText, m_newText;
    bool m_binary = false;
};
