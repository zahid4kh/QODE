#pragma once

#include "spring/SpringProject.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QSpinBox;

// Spring Boot run settings of the open project: profiles, port, arguments, or a command of your own.
class SpringConfigDialog : public QDialog
{
    Q_OBJECT
public:
    SpringConfigDialog(const SpringProject &project, const SpringProject::Options &options, QWidget *parent = nullptr);
    SpringProject::Options options() const;

private:
    void updatePreview();

    SpringProject m_project;
    QLineEdit *m_profiles, *m_arguments, *m_command;
    QSpinBox *m_port;
    QLabel *m_preview;
};
