#pragma once

#include "search/ProjectSearch.h"

#include <QHash>
#include <QWidget>

#include <functional>

class QLabel;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// The "Tasks" side tab: TODO / FIXME style comments found in the project, and the user's bookmarks.
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

    int todoCount() const { return m_todoCount; }
    int bookmarkCount() const { return m_bookmarkCount; }

signals:
    void openLocation(const QString &path, int line, int column, int length); // line is 1-based
    void removeBookmark(const QString &path, int line);                       // 0-based
    void clearBookmarksRequested();
    void countsChanged();

private:
    void scanTodos();
    void onResults(const QList<SearchFileResult> &batch);
    void onFinished(int files, int matches, bool truncated, const QString &error);
    void rebuildBookmarks();
    void updateSummary();
    void activate(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    void applyFilter();
    QString relative(const QString &path) const;

    ProjectSearch *m_search;
    QString m_root;
    std::function<QHash<QString, QString>()> m_overrides;
    std::function<QString(const QString &, int)> m_lineText;
    QHash<QString, QList<int>> m_bookmarks;

    QToolButton *m_todoBtn, *m_markBtn, *m_refresh;
    QToolButton *m_filterBtn = nullptr;
    QLabel *m_summary;
    QStackedWidget *m_stack;
    QTreeWidget *m_todoTree, *m_markTree;
    QTimer *m_debounce;
    QString m_tagFilter; // empty = all tags

    int m_todoCount = 0, m_bookmarkCount = 0;
    bool m_scanning = false, m_truncated = false;
    QString m_error;
};
