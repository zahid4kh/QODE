#pragma once

#include "spring/SpringProject.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QLineEdit;
class QSpinBox;

// Spring Boot run settings of the open project: profiles, port, arguments, or a command of your own.
class SpringConfigDialog : public QDialog
{
    Q_OBJECT
public:
    SpringConfigDialog(const SpringProject &project, const SpringProject::Options &options, QWidget *parent = nullptr);
    SpringProject::Options options() const;

signals:
    void openFileRequested(const QString &path);

private:
    void updatePreview();
    void profilesFromChecks();
    void checksFromProfiles();

    SpringProject m_project;
    QLineEdit *m_profiles, *m_arguments, *m_command;
    QLineEdit *m_jvm;
    QPlainTextEdit *m_env;
    QList<QCheckBox *> m_profileChecks;
    bool m_syncing = false;
    QSpinBox *m_port;
    QLabel *m_preview;
};
