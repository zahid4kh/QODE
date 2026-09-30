#pragma once

#include <QDialog>

class QLineEdit;
class QPushButton;
class QLabel;

class NewProjectDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NewProjectDialog(const QString &defaultLocation, QWidget *parent = nullptr);

    QString projectName() const;
    QString location() const;

private:
    void browse();
    void validate();

    QLineEdit *m_name;
    QLineEdit *m_location;
    QLabel *m_hint;
    QPushButton *m_create;
};
