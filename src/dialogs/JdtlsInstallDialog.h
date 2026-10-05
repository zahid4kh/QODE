#pragma once

#include <QDialog>

class JdtlsInstaller;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;

// "Download and Set Up" for the Java language server (Eclipse JDT LS): explains what will happen, then runs
// JdtlsInstaller with a progress bar. exec() returns Accepted once the server is unpacked.
class JdtlsInstallDialog : public QDialog
{
    Q_OBJECT
public:
    explicit JdtlsInstallDialog(QWidget *parent = nullptr);

    QString executable() const { return m_executable; }

private:
    void begin();
    void finish(bool ok, const QString &message);

    JdtlsInstaller *m_installer;
    QCheckBox *m_latest;
    QLabel *m_stage;
    QProgressBar *m_bar;
    QPushButton *m_start, *m_cancel;
    QString m_executable;
};
