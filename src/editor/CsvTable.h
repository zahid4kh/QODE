#pragma once

#include "CsvData.h"

#include <QAbstractTableModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QWidget>

class QCheckBox;
class QLabel;
class QTextDocument;
class QTimer;
class QToolButton;

// A CSV / TSV file as a grid. The model reads the text of the QTextDocument the text editor shows and writes every
// change straight back into it (one field at a time, so quoting and the rest of the file stay as they were), which
// keeps undo, the modified flag, saving and the text view in step with the table.
class CsvModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    CsvModel(QTextDocument *doc, QObject *parent = nullptr);

    struct Change {
        int row; // 0-based record, header included
        int column;
        QString value;
    };

    void setFileName(const QString &fileName);
    QString fileName() const { return m_fileName; }
    QChar delimiter() const { return m_delimiter; }
    bool headerRow() const { return m_header; }
    void setHeaderRow(bool on);
    // Re-reads the document (the table shows the text again from scratch).
    void reload();

    int records() const { return m_rows.size(); }
    int headerOffset() const { return m_header && !m_rows.isEmpty() ? 1 : 0; }
    int columns() const { return m_columns; }
    QString valueAt(int record, int column) const;

    // Writing. All rows are records of the file (header = 0) and are applied as one undo step.
    bool setCells(const QVector<Change> &changes);
    void insertRecord(int before);              // before == records() appends
    void ensureRecords(int count);              // appends blank records until there are `count`
    void removeRecords(QList<int> records);
    void insertColumn(int before);              // before == columns() appends
    void removeColumns(QList<int> columns);
    void sortBy(int column, bool ascending);    // the data rows; the header stays on top

    // Where a cell starts in the document, and the cell a document position falls into (invalid when empty).
    int offsetOf(const QModelIndex &index) const;
    QModelIndex indexAtOffset(int position) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // True while the model itself changes the document.
    bool editing() const { return m_editing; }
    QTextDocument *document() const { return m_doc; }

private:
    bool setCell(int record, int column, const QString &value);
    void replaceRange(int position, int length, const QString &text);
    void shiftAfter(int record, int cell, int delta);
    void recount();
    void reparse();
    QString blankRecord() const;

    QTextDocument *m_doc;
    QString m_fileName;
    QVector<CsvData::Row> m_rows;
    QChar m_delimiter = QLatin1Char(',');
    int m_columns = 0;
    bool m_header = true;
    bool m_editing = false;
};

// Edits a cell in a line edit, or in a small text box when the value has line breaks (Shift+Enter adds one).
class CsvDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

signals:
    void enterPressed();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
};

class CsvTableView : public QTableView
{
    Q_OBJECT
public:
    explicit CsvTableView(CsvModel *model, QWidget *parent = nullptr);

public slots:
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void clearSelection();

signals:
    void contextMenuRequested(const QPoint &globalPos);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEditor(QWidget *editor, QAbstractItemDelegate::EndEditHint hint) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    CsvModel *csv() const;
    bool m_enterMoves = false;
};

// The table with its toolbar: the widget the editor tab shows instead of the text.
class CsvTableWidget : public QWidget
{
    Q_OBJECT
public:
    CsvTableWidget(QTextDocument *doc, const QString &fileName, QWidget *parent = nullptr);

    void setFileName(const QString &fileName);
    // Re-reads the text and selects the cell holding `position` (a character offset in the document).
    void activate(int position);
    // Document position of the current cell, -1 when none.
    int currentPosition() const;
    CsvTableView *view() const { return m_view; }

private:
    void applyTheme();
    void scheduleReload();
    void reload();
    void autoSizeColumns();
    QVector<int> columnWidths() const;
    void restoreWidths(const QVector<int> &widths);
    void updateInfo();
    void updateButtons();
    QList<int> selectedRecords() const;
    QList<int> selectedColumns() const;
    void addRecord(bool below);
    void addColumn(bool right);
    void deleteRecords();
    void deleteColumns();
    void renameColumn(int column);
    void select(int record, int column);
    void showCellMenu(const QPoint &globalPos);
    void showColumnMenu(int column, const QPoint &globalPos);
    void showRowMenu(int record, const QPoint &globalPos);

    QTextDocument *m_doc;
    CsvModel *m_model;
    CsvTableView *m_view;
    QToolButton *m_addRow, *m_addColumn, *m_delRow, *m_delColumn;
    QCheckBox *m_headerBox;
    QLabel *m_info;
    QTimer *m_reloadTimer;
    bool m_stale = true;
    bool m_sized = false;
};
