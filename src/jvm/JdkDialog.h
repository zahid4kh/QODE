#pragma once

#include "jvm/JdkScanner.h"

#include <QDialog>

class QLabel;
class QTreeWidget;

// The JDK manager: the JDKs installed on this machine, which one the project uses (JAVA_HOME and PATH of the terminals
// and of what QODE runs), and a warning when the build asks for a toolchain version that is not installed.
class JdkDialog : public QDialog
{
    Q_OBJECT
public:
    // `chosen`: the project's JDK home, "" = the system default. `toolchain`: jvmToolchain(N) of the build (0 = none).
    JdkDialog(const QString &chosen, int toolchain, bool autoProvision, QWidget *parent = nullptr);
    QString chosenHome() const { return m_chosen; } // "" = system default

private:
    void populate();
    void addFolder();
    void updateWarning();

    QString m_chosen;
    int m_toolchain;
    bool m_autoProvision;
    QList<JdkScanner::Jdk> m_jdks;
    QTreeWidget *m_list;
    QLabel *m_warning;
};
