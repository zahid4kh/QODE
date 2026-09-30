#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;

// Edits the command that the toolbar's Run button executes for files of the current type.
class RunConfigDialog : public QDialog
{
    Q_OBJECT
public:
    RunConfigDialog(const QString &filePath, const QString &command, QWidget *parent = nullptr);

    QString command() const;

    // Settings key for a file: its extension (or its whole name for files like "Makefile").
    static QString keyFor(const QString &filePath);
    // A sensible starting command for the file's type, or an empty string.
    static QString suggestion(const QString &filePath);
    // Substitutes {file}, {dir}, {name} and {project} (shell-quoted).
    static QString expand(const QString &command, const QString &filePath, const QString &projectRoot);

private:
    void updatePreview();

    QString m_file;
    QLineEdit *m_command;
    QLabel *m_preview;
};
