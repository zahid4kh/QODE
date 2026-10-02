#pragma once

#include "WebProject.h"

#include <QDialog>
#include <QJsonObject>

class DevServer;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

// Dev server settings of the open web project. Everything is optional: "Automatic" follows the lockfile and package.json.
class ServerConfigDialog : public QDialog
{
    Q_OBJECT
public:
    ServerConfigDialog(const WebProject &project, const QJsonObject &config, QWidget *parent = nullptr);
    QJsonObject config() const;

private:
    void updatePreview();

    WebProject m_project;
    QComboBox *m_manager, *m_script;
    QSpinBox *m_port;
    QLineEdit *m_command;
    QLabel *m_preview;
};

// Modeless live output of the dev server.
class ServerLogDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ServerLogDialog(DevServer *server, QWidget *parent = nullptr);

private:
    QPlainTextEdit *m_view;
};
