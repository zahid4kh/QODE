#pragma once

#include <QDialog>
#include <QList>
#include <QPair>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;

// Edits the command that the toolbar's Run button executes for files of the current type.
class RunConfigDialog : public QDialog
{
    Q_OBJECT
public:
    RunConfigDialog(const QString &filePath, const QString &command, QWidget *parent = nullptr);

    QString command() const;
    // An explanation shown above the command (HTML), e.g. why it was prefilled.
    void setNote(const QString &html);
    void setProjectRoot(const QString &root);
    // Detected configurations (name, command) to pick from; picking one fills in the command.
    void setSuggestions(const QList<QPair<QString, QString>> &configs);
    // Offers (or, when `checked`, applies) running the whole project with this command regardless of the open file.
    void setProjectWide(bool checked);
    bool projectWide() const;

    // Settings key for a file: its extension (or its whole name for files like "Makefile").
    static QString keyFor(const QString &filePath);
    // Ready-made commands (name, command) for the file's type, offered in the dropdown; empty when there are none.
    static QList<QPair<QString, QString>> presets(const QString &filePath);
    // The first preset, or an empty string.
    static QString suggestion(const QString &filePath);
    // Substitutes {file}, {dir}, {name} and {project} (shell-quoted).
    static QString expand(const QString &command, const QString &filePath, const QString &projectRoot);

private:
    void updatePreview();
    void updateIntro();

    QString m_file;
    QLineEdit *m_command;
    QComboBox *m_picker;
    QFormLayout *m_form = nullptr;
    QList<QPair<QString, QString>> m_configs;
    QLabel *m_preview;
    QLabel *m_note;
    QLabel *m_intro;
    QCheckBox *m_wide;
    QString m_root;
};
