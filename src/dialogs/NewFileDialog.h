#pragma once

#include <QDialog>

class QLineEdit;
class QLabel;
class QPushButton;

// Prompts for a single file or folder name (also reused for renaming).
class NewFileDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Kind { File, Folder, Rename };

    NewFileDialog(Kind kind, const QString &directory, const QString &initialName = {}, QWidget *parent = nullptr);

    QString name() const;

private:
    void validate();

    QString m_directory;
    QLineEdit *m_name;
    QLabel *m_hint;
    QPushButton *m_ok;
};
