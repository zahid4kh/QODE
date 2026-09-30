#pragma once

#include "ProjectSearch.h"

#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// The "Search" tab: query with case / whole word / regex toggles, include & exclude globs, a result tree
// grouped by file, and "replace in files".
class SearchPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPanel(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root); // empty => no project
    // Supplies the current text of modified open documents (they are searched instead of the disk copy).
    void setOverridesProvider(std::function<QHash<QString, QString>()> provider) { m_overrides = std::move(provider); }
    void focusQuery(const QString &prefill = {});
    void rerun(); // search again with the current settings

    SearchOptions options() const;
    QString replacement() const;

signals:
    void openMatch(const QString &path, int line, int column, int length);
    void replaceRequested(const QStringList &paths, const SearchOptions &options, const QString &replacement);

private:
    void scheduleSearch();
    void runSearch();
    void onResults(const QList<SearchFileResult> &batch);
    void onFinished(int files, int matches, bool truncated, const QString &error);
    void updateSummary();
    void applyTheme();
    void clearResults();
    void onItemClicked(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    void replaceAll();
    QStringList listedPaths(int *matchCount = nullptr) const;
    void confirmAndReplace(const QStringList &paths, int matches);

    ProjectSearch *m_search;
    QString m_root;
    std::function<QHash<QString, QString>()> m_overrides;

    QLineEdit *m_query, *m_replace, *m_include, *m_exclude;
    QToolButton *m_case, *m_word, *m_regex, *m_replaceToggle, *m_filterToggle, *m_refresh, *m_collapse, *m_clear;
    QPushButton *m_replaceAll;
    QWidget *m_replaceRow, *m_filterRow;
    QLabel *m_summary;
    QTreeWidget *m_tree;
    QTimer *m_debounce;

    int m_files = 0, m_matches = 0;
    bool m_searching = false, m_truncated = false;
    QString m_error;
    bool m_collapsed = false;
};
