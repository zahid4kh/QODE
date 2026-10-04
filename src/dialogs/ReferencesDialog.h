#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

// Find All References: the places a symbol is used, grouped by file; double-click or Enter jumps there. Modeless, one per
// window: a new search replaces the list.
class ReferencesDialog : public QDialog
{
    Q_OBJECT
public:
    struct Entry {
        QString path;
        int line = 0, column = 0; // 0-based
        QString text;             // the line's text
    };

    explicit ReferencesDialog(QWidget *parent = nullptr);
    // `root` shortens the shown paths.
    void setReferences(const QString &symbol, const QVector<Entry> &entries, const QString &root);

signals:
    void openRequested(const QString &path, int line, int column); // 0-based

private:
    void activate(QTreeWidgetItem *item);

    QLabel *m_title;
    QTreeWidget *m_tree;
};
