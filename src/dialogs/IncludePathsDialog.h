#pragma once

#include <QDialog>
#include <QStringList>

class QListWidget;

// Edits the extra C/C++ include folders the language server (clangd) uses for a project. Stored in QODE's
// per-project settings, never inside the project folder.
class IncludePathsDialog : public QDialog
{
    Q_OBJECT
public:
    // `note` is shown under the explanation (what applies to this project's build setup), may be empty.
    IncludePathsDialog(const QString &projectRoot, const QStringList &paths, const QString &note, QWidget *parent = nullptr);

    QStringList paths() const;

private:
    void addFolder();
    void addItem(const QString &path);
    void editSelected();
    void removeSelected();

    QString m_root;
    QListWidget *m_list;
};
