#pragma once

#include <QWidget>

#include "maven/MavenProject.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QProcess;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Right-hand side panel for Maven projects: the lifecycle phases, the plugins with their goals, the dependency tree and
// the effective POM. Options (offline, skip tests, profiles, the module) go into every command; double-click, Enter or
// the context menu run a phase or goal (several selected phases run together, in lifecycle order). The panel only emits
// commands; the window runs them in the terminal.
class MavenPanel : public QWidget
{
    Q_OBJECT
public:
    explicit MavenPanel(QWidget *parent = nullptr);

    // Folder holding the Maven build ("" = no Maven project). A different folder resets the panel and loads its options.
    void setBuildDir(const QString &dir);
    void ensureLoaded(); // re-reads the poms when they changed
    void reload();
    void updateWrapper();

signals:
    void runRequested(const QString &command, const QString &workDir);
    void openFileRequested(const QString &path, int line); // line 1-based, 0 = top
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum Page { Lifecycle, Plugins, Dependencies };

    void readPoms();
    void populateLifecycle();
    void populatePlugins();
    void populateDependencies();
    void loadEffectivePom(bool open);
    void loadDependencyTree();
    void showPageState(Page page, const QString &title, const QString &text, bool retry);
    void applyFilter();
    void updateProfilesButton();
    void updateDetail();
    void saveOptions();
    QString moduleArgs() const;
    QString optionArgs() const;
    QString commandFor(const QStringList &goals, const QString &extraArgs = QString()) const;
    QStringList goalsOf(const QList<QTreeWidgetItem *> &items) const;
    void runItems(const QList<QTreeWidgetItem *> &items, const QString &extraArgs = QString());
    void showContextMenu(QTreeWidget *tree, const QPoint &pos);
    void executeGoal();
    qint64 pomStamp() const;
    QTreeWidget *currentTree() const;
    QTreeWidget *makeTree();
    void killProcess(QProcess *&proc);
    QString cacheFile(const QString &name) const;

    QString m_dir;
    QList<MavenProject::Pom> m_poms; // root first, then modules
    QMap<QString, MavenProject::Pom> m_effective; // artifactId -> effective pom
    QList<MavenProject::TreeModule> m_tree;
    qint64 m_pomStamp = -1, m_effectiveStamp = -1, m_treeStamp = -1;
    QProcess *m_effectiveProc = nullptr, *m_treeProc = nullptr;
    bool m_openEffective = false;

    // Options (per project)
    bool m_offline = false, m_skipTests = false;
    QStringList m_profiles; // checked profile ids
    QString m_module;       // "" = the whole build

    QLabel *m_title, *m_folder;
    QWidget *m_wrapperRow;
    QLabel *m_wrapperLabel;
    QToolButton *m_execBtn;
    QToolButton *m_offlineBtn, *m_skipTestsBtn, *m_profilesBtn, *m_reloadBtn, *m_effectiveBtn, *m_goalBtn;
    QComboBox *m_moduleCombo;
    QWidget *m_moduleRow;
    QTabBar *m_tabs;
    QLineEdit *m_filter;
    QStackedWidget *m_pages; // one stack (tree | state) per page
    QTreeWidget *m_lifecycleTree, *m_pluginTree, *m_depTree;
    QStackedWidget *m_pageStacks[3];
    QLabel *m_stateTitle[3], *m_stateText[3];
    QToolButton *m_stateRetry[3];
    QLabel *m_detail;
};
