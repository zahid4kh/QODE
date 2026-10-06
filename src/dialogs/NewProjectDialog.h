#pragma once

#include "project/ProjectTemplates.h"

#include <QDialog>
#include <QHash>
#include <QSet>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// File > New Project: a template list on the left (Empty Project first), the project's name, location and the selected
// template's own options on the right.
class NewProjectDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewProjectDialog(const QString &defaultLocation, QWidget *parent = nullptr);

    QString projectName() const;
    QString location() const;
    // Id of the chosen template; empty for an empty project.
    QString templateId() const { return m_templateId; }
    // The project "name" plus one entry per option of the chosen template.
    ProjectTemplates::Values values() const;
    bool initGit() const;

private:
    void browse();
    void validate();
    void templateChosen();
    void rebuildOptions();
    void refreshDefaults();
    QWidget *makeField(const ProjectTemplates::Option &o);
    void browseImage(QLineEdit *edit);
    void updatePreview(const QString &path);

    QListWidget *m_list;
    QLabel *m_title;
    QLabel *m_description;
    QFormLayout *m_form;
    QLineEdit *m_name;
    QLineEdit *m_location;
    QLabel *m_requirement;
    QCheckBox *m_git;
    QLabel *m_hint;
    QPushButton *m_create;

    QString m_templateId;
    int m_staticRows = 0;                       // rows of the form that belong to every template
    QHash<QString, QWidget *> m_fields;          // option key -> input widget
    QHash<QString, QLabel *> m_previews;         // image option key -> thumbnail
    QSet<QString> m_edited;                      // options the user changed by hand (their default no longer follows the name)
    bool m_updating = false;
};
