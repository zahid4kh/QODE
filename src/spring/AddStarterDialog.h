#pragma once

#include "spring/SpringProject.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Tick the Spring Boot starters to add; the caller applies SpringStarters::plan(project, selectedIds()).
class AddStarterDialog : public QDialog
{
    Q_OBJECT
public:
    AddStarterDialog(const SpringProject &project, QWidget *parent = nullptr);
    QStringList selectedIds() const; // newly ticked ones only

private:
    void filter(const QString &text);
    void updateSummary();

    SpringProject m_project;
    QLineEdit *m_search;
    QTreeWidget *m_tree;
    QLabel *m_summary;
    QPushButton *m_ok = nullptr;
};
