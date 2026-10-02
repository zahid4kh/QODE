#pragma once

#include <QDialog>

class NpmInstaller;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

// "Download and Set Up" for the web language servers (TypeScript / JavaScript, HTML, CSS, JSON): explains what will
// happen, then runs NpmInstaller with npm's output shown live. exec() returns Accepted once everything is installed.
class NpmInstallDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NpmInstallDialog(QWidget *parent = nullptr);

protected:
    void reject() override;

private:
    void begin();
    void finish(bool ok, const QString &message);

    NpmInstaller *m_installer;
    QLabel *m_status;
    QPlainTextEdit *m_log;
    QProgressBar *m_bar;
    QPushButton *m_start, *m_cancel;
};
