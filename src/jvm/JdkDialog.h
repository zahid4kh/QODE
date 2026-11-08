#pragma once

#include "jvm/JdkScanner.h"

#include <QDialog>

class QLabel;
class QTreeWidget;

// The JDK manager: the JDKs installed on this machine, which one the project uses (JAVA_HOME and PATH of the terminals
// and of what QODE runs), a warning when the project's Gradle cannot run on the selected JDK or the build asks for a
// toolchain that is not installed, and Download JDK… (Eclipse Temurin into ~/.jdks, done by the caller).
class JdkDialog : public QDialog
{
    Q_OBJECT
public:
    // `chosen`: the project's JDK home, "" = the system default. `root`: the project ("" = none).
    JdkDialog(const QString &chosen, const QString &root, QWidget *parent = nullptr);
    QString chosenHome() const { return m_chosen; } // "" = system default
    int downloadMajor() const { return m_download; } // > 0 when the dialog closed through Download JDK…

private:
    void populate();
    void addFolder();
    void askDownload();
    void updateWarning();

    QString m_chosen, m_root;
    int m_download = 0;
    QList<JdkScanner::Jdk> m_jdks;
    QTreeWidget *m_list;
    QLabel *m_warning;
};
