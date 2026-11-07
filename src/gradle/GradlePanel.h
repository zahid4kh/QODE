#pragma once

#include <QWidget>

#include "gradle/GradleTasks.h"

class GradleBuildView;
class QLabel;
class QNetworkAccessManager;
class QTabBar;
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
    GradleBuildView *buildView() const { return m_buildView; }
    void showBuild(); // the Build tab
    void updateWrapper(); // the wrapper row: version, Upgrade, a gradlew without its exec bit
    // Upgrade…: asks services.gradle.org for the current release, points gradle-wrapper.properties at it (so this works
    // when the old Gradle cannot start on the installed JDK), then runs `gradlew wrapper` to refresh the scripts and jar.
    void upgradeWrapper();

signals:
    void runRequested(const QString &command, const QString &workDir);
    void closeRequested();
    void wrapperChanged(); // gradle-wrapper.properties names another Gradle (Upgrade…)

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

    QTabBar *m_tabs;
    QStackedWidget *m_pages; // tasks | build
    GradleBuildView *m_buildView;
    QWidget *m_wrapperRow;
    QLabel *m_wrapperLabel;
    QToolButton *m_upgradeBtn, *m_execBtn;
    QNetworkAccessManager *m_net = nullptr;

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
