#pragma once

#include <QMessageBox>
#include <QStringList>

// "Save changes to: ..." prompt shared by tab close, project close and app exit.
class UnsavedChangesDialog
{
public:
    enum Result { SaveAll, Discard, Cancel };

    static Result ask(QWidget *parent, const QStringList &fileNames);
};
