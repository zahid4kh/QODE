#pragma once

#include "search/ProjectSearch.h"
#include "terminal/ProblemScanner.h"

#include <QHash>
#include <QWidget>

#include <functional>

class QLabel;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// The "Tasks" side tab: TODO / FIXME style comments found in the project, the user's bookmarks, and the compiler errors and
// warnings printed in the terminal (per terminal pane, `source`, replaced when that pane runs its next command).
class TasksPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TasksPanel(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root); // empty => no project
    // Current text of modified open documents (searched instead of the disk copy).
    void setOverridesProvider(std::function<QHash<QString, QString>()> provider) { m_overrides = std::move(provider); }
    // Text of a 0-based line of `path` (open document first); empty when unknown.
    void setLineTextProvider(std::function<QString(const QString &, int)> provider) { m_lineText = std::move(provider); }

    void scheduleTodoScan(); // debounced; a no-op without a project
    void setBookmarks(const QHash<QString, QList<int>> &bookmarks);
    void showBookmarks();
    void showTodos();
    void showProblems();
    void addProblems(QObject *source, const QList<BuildProblem> &problems);
    void clearProblems(QObject *source);

    int todoCount() const { return m_todoCount; }
    int bookmarkCount() const { return m_bookmarkCount; }
    int problemCount() const { return m_errorCount + m_warningCount; }

signals:
    void openLocation(const QString &path, int line, int column, int length); // line is 1-based
    void removeBookmark(const QString &path, int line);                       // 0-based
    void clearBookmarksRequested();
    void countsChanged();
    void fixRequested(const QList<BuildProblem> &problems); // "Fix with QODE"

private:
    void scanTodos();
    void onResults(const QList<SearchFileResult> &batch);
    void onFinished(int files, int matches, bool truncated, const QString &error);
    void rebuildBookmarks();
    void rebuildProblems();
    void updateSummary();
    void activate(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    void applyFilter();
    QString relative(const QString &path) const;
    // The listed problems of `path` ("" = all files); only the errors when there are any.
    QList<BuildProblem> problemsToFix(const QString &path) const;

    ProjectSearch *m_search;
    QString m_root;
    std::function<QHash<QString, QString>()> m_overrides;
    std::function<QString(const QString &, int)> m_lineText;
    QHash<QString, QList<int>> m_bookmarks;

    QToolButton *m_todoBtn, *m_markBtn, *m_probBtn, *m_refresh;
    QToolButton *m_filterBtn = nullptr;
    QToolButton *m_fixBtn = nullptr;
    QLabel *m_summary;
    QStackedWidget *m_stack;
    QTreeWidget *m_todoTree, *m_markTree, *m_probTree;
    QTimer *m_debounce;
    QTimer *m_probTimer; // problems arrive line by line: the tree is rebuilt once they pause
    QList<QPair<QObject *, BuildProblem>> m_problems;
    QString m_tagFilter; // empty = all tags

    int m_todoCount = 0, m_bookmarkCount = 0, m_errorCount = 0, m_warningCount = 0;
    bool m_scanning = false, m_truncated = false;
    QString m_error;
};
