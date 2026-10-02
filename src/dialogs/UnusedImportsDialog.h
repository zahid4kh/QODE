#pragma once

#include <QDialog>
#include <QStringList>
#include <QVector>

class QListWidget;

// "Remove Unused Imports": lists the imports nothing uses, all ticked; the user unticks what should stay.
class UnusedImportsDialog : public QDialog
{
    Q_OBJECT
public:
    // `lines` are 0-based line numbers, `texts` the import lines shown for them (same order).
    UnusedImportsDialog(const QString &fileName, const QVector<int> &lines, const QStringList &texts, QWidget *parent = nullptr);

    QVector<int> selectedLines() const;

private:
    QListWidget *m_list;
    QVector<int> m_lines;
};
