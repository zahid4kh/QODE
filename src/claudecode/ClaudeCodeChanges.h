#pragma once

#include <QFrame>
#include <QVector>

class QLabel;
class QPushButton;

namespace claudecode {

// What one turn did to one file, summed over all of its edits.
struct FileChange {
    QString path;     // absolute
    QString display;  // project-relative
    int added = 0, removed = 0;
    QVector<int> lines; // 1-based first changed line of every edit (0: not known)
    bool created = false;
};

// Adds an edit to the turn's list, merging edits of the same file.
void addChange(QVector<FileChange> &changes, const FileChange &change);

// The card under a turn that changed files: "4 files changed · +62 −18", one row per file (a click opens the file at
// its first change) and "Review all", which steps through every change in the editor.
class ChangesCard : public QFrame
{
    Q_OBJECT
public:
    explicit ChangesCard(const QVector<FileChange> &changes, QWidget *parent = nullptr);

signals:
    void openRequested(const QString &absolute, int line);

private:
    void applyTheme();
    void go(int index);

    QVector<FileChange> m_changes;
    QVector<QPair<int, int>> m_stops; // file index, line
    int m_index = -1;
    QLabel *m_title;
    QPushButton *m_reviewBtn;
    QWidget *m_reviewBar;
    QLabel *m_reviewText;
    QPushButton *m_prev, *m_next;
    QVector<QWidget *> m_rows;
    QVector<QLabel *> m_counts;
};

} // namespace claudecode
