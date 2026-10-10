#pragma once

#include <QWidget>

#include "gradle/GradleTasks.h"

class QLabel;
class QLineEdit;
class QProcess;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Right-hand side panel listing the Gradle tasks of the open project by group (Build, Verification, Signing ...).
// Double-click, Enter or the context menu run a task; the panel only emits the command, the window runs it in the terminal.
class GradlePanel : public QWidget
{
    Q_OBJECT
public:
    explicit GradlePanel(QWidget *parent = nullptr);

    // Folder that holds the Gradle build ("" = no Gradle project). A different folder clears the list.
    void setBuildDir(const QString &dir);
    // Reads the tasks when the list is empty or older than the build files.
    void ensureLoaded();
    void reload();

signals:
    void runRequested(const QString &command, const QString &workDir);
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void populate();
    void showState(const QString &title, const QString &text, bool retry);
    void applyFilter(const QString &text);
    void runTask(QTreeWidgetItem *item, const QString &extraArgs = QString());
    QString commandFor(QTreeWidgetItem *item, const QString &extraArgs = QString()) const;
    void showContextMenu(const QPoint &pos);
    void updateDetail();
    qint64 buildStamp() const;

    QString m_dir;
    QList<GradleTasks::Group> m_groups;
    qint64 m_loadedStamp = -1;
    bool m_all = false;
    QProcess *m_proc = nullptr;

    QLabel *m_title;
    QLabel *m_folder;
    QToolButton *m_refresh;
    QToolButton *m_allBtn;
    QLineEdit *m_filter;
    QStackedWidget *m_stack;
    QTreeWidget *m_tree;
    QLabel *m_stateTitle;
    QLabel *m_stateText;
    QToolButton *m_retry;
    QLabel *m_detail;
};
