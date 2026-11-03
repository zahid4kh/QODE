#pragma once

#include "jvm/JvmNewFile.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;

// "New Kotlin Class/File" / "New Java Class": a name (or sub.package.Name) above the list of kinds, in the style of
// IntelliJ's dialog; Up / Down in the name field pick the kind, Enter creates. The package comes from the folder.
class NewJvmClassDialog : public QDialog
{
    Q_OBJECT
public:
    NewJvmClassDialog(bool kotlin, const QString &dir, QWidget *parent = nullptr);

    JvmNewFile::Result result() const { return m_result; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void update();
    void tryAccept();
    JvmNewFile::Kind currentKind() const;

    bool m_kotlin;
    QString m_dir, m_package;
    QLineEdit *m_name;
    QListWidget *m_kinds;
    QLabel *m_where, *m_error;
    JvmNewFile::Result m_result;
};
