#pragma once

#include <QVector>
#include <QWidget>

class DiffView;
class QLabel;

// Two versions of a text side by side: aligned rows, changed lines tinted, scrolling linked, previous / next change.
// Used by the git diff window and the local history.
class DiffPane : public QWidget
{
    Q_OBJECT
public:
    explicit DiffPane(QWidget *parent = nullptr);

    struct Stats {
        int hunks = 0, added = 0, removed = 0;
    };
    void setTitles(const QString &oldTitle, const QString &newTitle);
    Stats setTexts(const QString &oldText, const QString &newText); // keeps the scroll position for the same texts
    void setMessage(const QString &text); // instead of a comparison (binary files ...)
    bool hasChanges() const { return !m_hunkRows.isEmpty(); }
    void gotoChange(bool next);
    void firstChange();

private:
    void applyTheme();

    QLabel *m_oldLabel, *m_newLabel;
    DiffView *m_left, *m_right;
    QVector<int> m_hunkRows;
    QString m_shownOld, m_shownNew;
    bool m_haveShown = false;
    Stats m_stats;
};
