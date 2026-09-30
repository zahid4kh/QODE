#pragma once

#include <QWidget>

class QLabel;
class RecentList;

// The empty state shown when no project or file is open: quick actions and the recent projects.
class WelcomePage : public QWidget
{
    Q_OBJECT
public:
    explicit WelcomePage(QWidget *parent = nullptr);

    void setRecentProjects(const QStringList &paths); // most recent first

signals:
    void newProjectRequested();
    void openProjectRequested();
    void newFileRequested();
    void openFileRequested();
    void openRecentRequested(const QString &path);
    void removeRecentRequested(const QString &path);
    void clearRecentRequested();

private:
    QLabel *m_recentTitle;
    RecentList *m_recent;
};
