#include "UnsavedChangesDialog.h"

#include <QPushButton>

UnsavedChangesDialog::Result UnsavedChangesDialog::ask(QWidget *parent, const QStringList &fileNames)
{
    QMessageBox box(QMessageBox::Warning, QObject::tr("Unsaved Changes"), QString(), QMessageBox::NoButton, parent);
    QString list;
    for (const QString &n : fileNames)
        list += QStringLiteral("• ") + n.toHtmlEscaped() + QStringLiteral("<br>");
    box.setTextFormat(Qt::RichText);
    box.setText(QObject::tr("Save changes to:<br><br>%1").arg(list));
    QPushButton *save = box.addButton(fileNames.size() > 1 ? QObject::tr("Save All") : QObject::tr("Save"),
                                      QMessageBox::AcceptRole);
    QPushButton *discard = box.addButton(QObject::tr("Discard"), QMessageBox::DestructiveRole);
    QPushButton *cancel = box.addButton(QObject::tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(save);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() == save)
        return SaveAll;
    if (box.clickedButton() == discard)
        return Discard;
    return Cancel;
}
