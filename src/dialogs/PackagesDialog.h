#pragma once

#include "python/PythonTools.h"

#include <QDialog>
#include <QHash>

class PackageManager;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// "Python Packages": install, upgrade and uninstall the packages of the project's virtual environment. Lists what is
// installed (and, once the package index has answered, what has a newer version), runs pip - or uv, when it is installed -
// against the environment's own interpreter and shows the command output in a log. A project without a virtual
// environment gets an empty state that creates one. Modeless; MainWindow restarts the Python language server on the signals.
class PackagesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PackagesDialog(const QString &projectRoot, QWidget *parent = nullptr);

    // Fills the install field with `specs` and installs them as soon as the environment is ready (the Alt+Enter fix for
    // an unresolved import).
    void installNow(const QStringList &specs);

signals:
    void packagesChanged();                      // an install / upgrade / uninstall succeeded
    void environmentChanged();                   // another venv was chosen
    void venvCreated(const QString &name);       // root-relative folder name

protected:
    void reject() override; // closing while a change runs would leave pip half-way: cancel it first

private:
    void applyTheme();
    void loadEnvironment();                      // reads the project's venvs, fills the header, starts a refresh
    void showEmptyState(bool noVenv);
    void rebuildList();
    void applyFilter();
    void updateButtons();
    void setBusy(bool busy, const QString &text = QString());
    void installTyped();
    void installFromFile();
    void upgradeSelected();
    void upgradeOutdated();
    void uninstallSelected();
    void showContextMenu(const QPoint &pos);
    QStringList selectedNames() const;
    QStringList outdatedNames() const;
    void appendLog(const QString &text);

    QString m_root;
    PackageManager *m_pm;
    QList<PythonTools::Package> m_installed;
    QHash<QString, QString> m_latest; // lower-case name -> newest version
    bool m_checkedUpdates = false;
    bool m_busy = false;

    QStackedWidget *m_pages;
    QLabel *m_title, *m_subtitle, *m_count, *m_status, *m_emptyText;
    QComboBox *m_venvCombo;
    QToolButton *m_refresh;
    QLineEdit *m_installEdit, *m_filter;
    QPushButton *m_installBtn, *m_fileBtn, *m_upgradeBtn, *m_upgradeAllBtn, *m_uninstallBtn, *m_cancelBtn, *m_createBtn, *m_logBtn;
    QCheckBox *m_outdatedOnly;
    QTreeWidget *m_tree;
    QProgressBar *m_bar;
    QPlainTextEdit *m_log;
};
