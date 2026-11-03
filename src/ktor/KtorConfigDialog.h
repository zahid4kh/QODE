#pragma once

#include "ktor/KtorProject.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

// Ktor run settings of the open project: port, development mode, auto-reload, reading the running server's routes,
// arguments, JVM options, environment, or a command of your own.
class KtorConfigDialog : public QDialog
{
    Q_OBJECT
public:
    KtorConfigDialog(const KtorProject &project, const KtorProject::Options &options, QWidget *parent = nullptr);
    KtorProject::Options options() const;

signals:
    void openFileRequested(const QString &path);

private:
    void updatePreview();

    KtorProject m_project;
    QSpinBox *m_port;
    QCheckBox *m_development, *m_autoReload, *m_live;
    bool m_developmentWanted = false; // the development box's own choice (auto-reload shows it as on)
    QLineEdit *m_arguments, *m_jvm, *m_command;
    QPlainTextEdit *m_env;
    QLabel *m_preview;
};
