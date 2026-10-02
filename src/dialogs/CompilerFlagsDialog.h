#pragma once

#include <QDialog>

class QPlainTextEdit;

// Edits the compiler flags clangd falls back to for a project that has no compile_commands.json /
// compile_flags.txt. Stored in QODE's per-project settings, never inside the project folder.
class CompilerFlagsDialog : public QDialog
{
    Q_OBJECT
public:
    // `detected` are flags worked out from the project's build file `detectedSource` (e.g. "app.pro"); they are
    // shown for reference and are always used, the text below is added after them.
    CompilerFlagsDialog(const QString &text, const QString &detectedSource, const QStringList &detected, QWidget *parent = nullptr);

    QString text() const;

    // The commented template shown when nothing is saved yet.
    static QString templateText();

private:
    void addQtFlags();

    QPlainTextEdit *m_edit;
};
