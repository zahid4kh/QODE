#pragma once

#include <QDialog>

class PipInstaller;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

// "Download and Set Up" for the Python language tools (basedpyright and ruff): explains what will
// happen, then runs PipInstaller with the output shown live. exec() returns Accepted once everything is installed.
class PipInstallDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PipInstallDialog(QWidget *parent = nullptr);

protected:
    void reject() override;

private:
    void begin();
    void finish(bool ok, const QString &message);

    PipInstaller *m_installer;
    QLabel *m_status;
    QPlainTextEdit *m_log;
    QProgressBar *m_bar;
    QPushButton *m_start, *m_cancel;
};
