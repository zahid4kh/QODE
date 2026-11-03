#pragma once

#include <QFrame>
#include <QJsonArray>

class QPushButton;
class QVBoxLayout;

namespace claudecode {

// Claude Code's to-do list (its TodoWrite calls), pinned above the input while a turn runs: one row per step with
// pending / in progress / done, or folded to "3/7 done · what it is doing now". Long lists show a window of rows
// around the step in progress.
class TaskList : public QFrame
{
    Q_OBJECT
public:
    explicit TaskList(QWidget *parent = nullptr);

    void setTodos(const QJsonArray &todos); // the full list of the latest TodoWrite call
    void clear();                           // the turn ended

private:
    void rebuild();
    void updateHeader();

    QJsonArray m_todos;
    bool m_collapsed = false;
    QPushButton *m_header;
    QWidget *m_body;
    QVBoxLayout *m_rows;
};

} // namespace claudecode
