#pragma once

#include "ktor/KtorProject.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// Tick the Ktor server plugins to add; the caller applies KtorPlugins::plan(project, selectedIds()).
class AddKtorPluginDialog : public QDialog
{
    Q_OBJECT
public:
    AddKtorPluginDialog(const KtorProject &project, QWidget *parent = nullptr);
    QStringList selectedIds() const; // newly ticked ones only

private:
    void filter(const QString &text);
    void updateSummary();

    QLineEdit *m_search;
    QTreeWidget *m_tree;
    QLabel *m_summary;
    QPushButton *m_ok = nullptr;
};
