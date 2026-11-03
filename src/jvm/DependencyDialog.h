#pragma once

#include "jvm/DependencyEdit.h"

#include <QDialog>

class MavenCentral;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;

// The dependency manager: search Maven Central and add a library to a module (written the way the build already
// declares dependencies: catalog alias or coordinates, Gradle or Maven), and list the declared libraries that have newer
// versions. For a Kotlin script it adds @file:DependsOn lines and checks those. The window writes the files.
class DependencyDialog : public QDialog
{
    Q_OBJECT
public:
    // `currentFile` picks the module preselected as target (the one containing it).
    DependencyDialog(const QString &root, const QString &currentFile, QWidget *parent = nullptr);
    // Script mode: the dependencies are the script's @file:DependsOn lines.
    void setScript(const QString &path, const QString &text);
    void showUpdates(); // opens on the Updates tab and starts the check
    void setStatus(const QString &text, bool error = false);

signals:
    void addRequested(const QString &buildFile, const QString &group, const QString &artifact, const QString &version, const QString &scope,
                      bool useCatalog);
    void dependsOnRequested(const QString &coordinate); // script mode
    void updatesRequested(const QList<QPair<DependencyEdit::Declared, QString>> &updates);

private:
    QWidget *buildAddTab();
    QWidget *buildUpdatesTab();
    void search();
    void selectResult();
    void fillVersions();
    void updateTarget();
    void add();
    void checkUpdates();
    void nextVersionLookup();

    QString m_root;
    QString m_script;   // script mode: its path
    QString m_scriptText;
    MavenCentral *m_central;
    QTabWidget *m_tabs;
    QLineEdit *m_query;
    QTreeWidget *m_results;
    QComboBox *m_version;
    QCheckBox *m_preRelease;
    QComboBox *m_target;
    QComboBox *m_scope;
    QCheckBox *m_catalog;
    QPushButton *m_addBtn;
    QLabel *m_status;
    QStringList m_versions; // of the selected result, newest first
    int m_searchToken = 0;

    QTreeWidget *m_updates;
    QPushButton *m_checkBtn, *m_applyBtn;
    QLabel *m_updatesState;
    QList<DependencyEdit::Declared> m_declared;
    int m_lookupNext = 0, m_lookupBusy = 0, m_found = 0;
};
