#pragma once

#include "GitDiff.h"
#include "GitTypes.h"

#include <QDialog>

class DiffPane;
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

    GitRepository *m_repo;
    QString m_path;
    GitDiffMode m_mode;
    QLabel *m_title, *m_stats;
    QPushButton *m_prev, *m_next, *m_action;
    DiffPane *m_pane;
    int m_pending = 0;
    QString m_oldText, m_newText;
    bool m_binary = false;
};
