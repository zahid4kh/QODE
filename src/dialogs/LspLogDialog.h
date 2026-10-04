#pragma once

#include <QDialog>
#include <QStringList>
#include <functional>

class QLabel;
class QLineEdit;
class QPlainTextEdit;

// A language server's log in a resizable window: monospace text that can be selected and copied, a filter box,
// refresh / copy buttons, errors and warnings coloured. Modeless, so it can stay open next to the editor.
class LspLogDialog : public QDialog
{
    Q_OBJECT
public:
    // `provider` returns the server's current log lines (called again by Refresh).
    LspLogDialog(const QString &serverName, std::function<QStringList()> provider, QWidget *parent = nullptr);

private:
    void reload();
    void applyFilter();

    std::function<QStringList()> m_provider;
    QStringList m_lines;
    QLabel *m_count;
    QLineEdit *m_filter;
    QPlainTextEdit *m_view;
};
