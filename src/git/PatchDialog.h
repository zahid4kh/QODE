#pragma once

#include <QDialog>

// Read-only viewer for `git show` output (commit details + unified diff) with coloured +/- lines.
class PatchDialog : public QDialog
{
    Q_OBJECT
public:
    PatchDialog(const QString &title, const QString &text, QWidget *parent = nullptr);
};
