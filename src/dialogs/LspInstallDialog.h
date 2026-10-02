#pragma once

#include <QDialog>

class LspInstaller;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;

// "Download and Set Up" for the Kotlin language server: explains what will happen, then runs LspInstaller with a
// progress bar. exec() returns Accepted once the server is installed and passed its test run.
class LspInstallDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LspInstallDialog(QWidget *parent = nullptr);

    QString executable() const { return m_executable; }

private:
    void begin();
    void finish(bool ok, const QString &message);

    LspInstaller *m_installer;
    QCheckBox *m_link, *m_latest;
    QLabel *m_stage;
    QProgressBar *m_bar;
    QPushButton *m_start, *m_cancel;
    QString m_executable;
};
