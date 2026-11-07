#pragma once

#include "app/QodeFiles.h"

#include <QDialog>
#include <QHash>
#include <QPair>

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Help > QODE's Files: every folder and file QODE created on this computer (QodeFiles::created), with sizes and what
// is in each, and what the installer or package put in place (Platform::installedItems), plus how to remove it all
// (Platform::removalHint). Sizes are counted on a worker thread; the list shows at once.
class QodeFilesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit QodeFilesDialog(QWidget *parent = nullptr);

    // A folder's entries one level down: name, bytes, is a folder.
    struct Entry {
        QString name;
        qint64 size = 0;
        bool folder = false;
    };
    struct Scan {
        QList<QodeFiles::Item> items;
        QHash<QString, QList<Entry>> children; // by item path
    };

private:
    void showCreated(const Scan &scan, bool counted);
    void openSelected();
    QString selectedPath() const;
    QString listText() const;

    QTreeWidget *m_tree = nullptr;
    QTreeWidgetItem *m_created = nullptr;
    QTreeWidgetItem *m_installed = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_copy = nullptr;
};
