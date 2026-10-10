#include "CsvTable.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPair>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

namespace {

// Everything done between construction and destruction is one undo step.
struct EditGroup {
    explicit EditGroup(QTextDocument *doc)
        : cursor(doc)
    {
        cursor.beginEditBlock();
    }
    ~EditGroup() { cursor.endEditBlock(); }
    QTextCursor cursor;
};

bool looksNumeric(const QString &v)
{
    if (v.isEmpty())
        return false;
    const QChar c = v.front();
    if (!(c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+') || c == QLatin1Char('.')))
        return false;
    bool ok = false;
    v.toDouble(&ok);
    return ok;
}

QString delimiterName(QChar d)
{
    if (d == QLatin1Char('\t'))
        return QObject::tr("tab");
    if (d == QLatin1Char(';'))
        return QObject::tr("semicolon");
    if (d == QLatin1Char('|'))
        return QObject::tr("pipe");
    return QObject::tr("comma");
}

} // namespace

// --- CsvModel ---------------------------------------------------------------------------------------

CsvModel::CsvModel(QTextDocument *doc, QObject *parent)
    : QAbstractTableModel(parent)
    , m_doc(doc)
{
}

void CsvModel::setFileName(const QString &fileName)
{
    m_fileName = fileName;
}

void CsvModel::setHeaderRow(bool on)
{
    if (m_header == on)
        return;
    beginResetModel();
    m_header = on;
    endResetModel();
}

void CsvModel::recount()
{
    m_columns = 0;
    for (const CsvData::Row &r : std::as_const(m_rows))
        m_columns = qMax(m_columns, int(r.cells.size()));
}

void CsvModel::reparse()
{
    const QString text = m_doc->toPlainText();
    m_delimiter = CsvData::detectDelimiter(text.left(20000), m_fileName);
    m_rows = CsvData::parse(text, m_delimiter);
    recount();
}

void CsvModel::reload()
{
    beginResetModel();
    reparse();
    endResetModel();
}

QString CsvModel::valueAt(int record, int column) const
{
    if (record < 0 || record >= m_rows.size())
        return {};
    const auto &cells = m_rows.at(record).cells;
    return column >= 0 && column < cells.size() ? cells.at(column).value : QString();
}

int CsvModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : qMax(0, int(m_rows.size()) - headerOffset());
}

int CsvModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_columns;
}

QVariant CsvModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const QString v = valueAt(index.row() + headerOffset(), index.column());
    switch (role) {
    case Qt::DisplayRole: {
        const int nl = v.indexOf(QLatin1Char('\n'));
        return nl < 0 ? v : v.left(nl) + QStringLiteral(" ⏎");
    }
    case Qt::EditRole:
        return v;
    case Qt::ToolTipRole:
        return v.size() > 48 || v.contains(QLatin1Char('\n')) ? QVariant(v) : QVariant();
    case Qt::TextAlignmentRole:
        return int((looksNumeric(v) ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    default:
        return {};
    }
}

bool CsvModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || role != Qt::EditRole)
        return false;
    return setCell(index.row() + headerOffset(), index.column(), value.toString());
}

QVariant CsvModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole) {
        if (orientation == Qt::Horizontal) {
            if (m_header && !m_rows.isEmpty()) {
                const QString v = valueAt(0, section);
                if (!v.isEmpty())
                    return v.left(v.indexOf(QLatin1Char('\n')) < 0 ? v.size() : v.indexOf(QLatin1Char('\n')));
            }
            return CsvData::columnName(section);
        }
        return section + headerOffset() + 1;
    }
    if (role == Qt::ToolTipRole && orientation == Qt::Horizontal && m_header)
        return CsvData::columnName(section);
    return {};
}

Qt::ItemFlags CsvModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

void CsvModel::replaceRange(int position, int length, const QString &text)
{
    if (length == 0 && text.isEmpty())
        return;
    m_editing = true;
    QTextCursor c(m_doc);
    c.setPosition(position);
    if (length > 0)
        c.setPosition(position + length, QTextCursor::KeepAnchor);
    if (text.isEmpty())
        c.removeSelectedText();
    else
        c.insertText(text);
    m_editing = false;
}

// Everything after (record, cell) moved by `delta` characters.
void CsvModel::shiftAfter(int record, int cell, int delta)
{
    if (delta == 0)
        return;
    CsvData::Row &row = m_rows[record];
    for (int i = cell + 1; i < row.cells.size(); ++i)
        row.cells[i].start += delta;
    row.end += delta;
    for (int r = record + 1; r < m_rows.size(); ++r) {
        CsvData::Row &next = m_rows[r];
        next.start += delta;
        next.end += delta;
        for (CsvData::Cell &c : next.cells)
            c.start += delta;
    }
}

bool CsvModel::setCell(int record, int column, const QString &value)
{
    if (record < 0 || record >= m_rows.size() || column < 0)
        return false;
    CsvData::Row &row = m_rows[record];
    if (column < row.cells.size()) {
        if (row.cells.at(column).value == value)
            return true;
    } else if (value.isEmpty()) {
        return true;
    }

    EditGroup group(m_doc);
    if (column < row.cells.size()) {
        CsvData::Cell &cell = row.cells[column];
        // A field that was quoted stays quoted, so the file keeps its style.
        // Quotes that were only there because the old value needed them go away with it.
        const bool quoted = cell.length > 0 && m_doc->characterAt(cell.start) == QLatin1Char('"')
                            && !CsvData::needsQuotes(cell.value, m_delimiter);
        const QString raw = CsvData::encode(value, m_delimiter, quoted);
        replaceRange(cell.start, cell.length, raw);
        const int delta = raw.size() - cell.length;
        cell.value = value;
        cell.length = raw.size();
        shiftAfter(record, column, delta);
    } else {
        // A short row: pad it with empty fields up to the new one.
        const QString raw = CsvData::encode(value, m_delimiter);
        QString add;
        int pos = row.end;
        for (int i = row.cells.size(); i <= column; ++i) {
            add += m_delimiter;
            ++pos;
            CsvData::Cell cell;
            cell.start = pos;
            if (i == column) {
                cell.value = value;
                cell.length = raw.size();
                add += raw;
                pos += raw.size();
            }
            row.cells.append(cell);
        }
        replaceRange(row.end, 0, add);
        shiftAfter(record, row.cells.size() - 1, add.size());
    }

    const int off = headerOffset();
    if (record >= off)
        emit dataChanged(index(record - off, column), index(record - off, column));
    else
        emit headerDataChanged(Qt::Horizontal, column, column);
    return true;
}

bool CsvModel::setCells(const QVector<Change> &changes)
{
    if (changes.isEmpty())
        return true;
    int widest = 0;
    for (const Change &c : changes)
        widest = qMax(widest, c.column + 1);
    const bool grows = widest > m_columns;
    if (grows)
        beginResetModel();
    {
        EditGroup group(m_doc);
        for (const Change &c : changes)
            setCell(c.row, c.column, c.value);
    }
    if (grows) {
        recount();
        endResetModel();
    }
    return true;
}

QString CsvModel::blankRecord() const
{
    return QString(qMax(m_columns, 1) - 1, m_delimiter);
}

void CsvModel::insertRecord(int before)
{
    if (m_rows.isEmpty()) {
        beginResetModel();
        {
            EditGroup group(m_doc);
            replaceRange(0, 0, QStringLiteral("Column 1,Column 2,Column 3"));
        }
        reparse();
        endResetModel();
        return;
    }
    before = qBound(headerOffset(), before, int(m_rows.size()));
    const int modelRow = before - headerOffset();
    beginInsertRows(QModelIndex(), modelRow, modelRow);
    {
        EditGroup group(m_doc);
        if (before >= m_rows.size())
            replaceRange(m_rows.last().end, 0, QLatin1Char('\n') + blankRecord());
        else
            replaceRange(m_rows.at(before).start, 0, blankRecord() + QLatin1Char('\n'));
    }
    reparse();
    endInsertRows();
}

void CsvModel::ensureRecords(int count)
{
    if (m_rows.isEmpty() || count <= m_rows.size())
        return;
    beginInsertRows(QModelIndex(), rowCount(), count - headerOffset() - 1);
    {
        EditGroup group(m_doc);
        QString add;
        for (int i = m_rows.size(); i < count; ++i)
            add += QLatin1Char('\n') + blankRecord();
        replaceRange(m_rows.last().end, 0, add);
    }
    reparse();
    endInsertRows();
}

void CsvModel::removeRecords(QList<int> records)
{
    std::sort(records.begin(), records.end(), std::greater<int>());
    records.erase(std::unique(records.begin(), records.end()), records.end());
    if (records.isEmpty() || m_rows.isEmpty())
        return;
    const QSet<int> doomed(records.begin(), records.end());
    beginResetModel();
    {
        EditGroup group(m_doc);
        for (const int r : std::as_const(records)) {
            if (r < 0 || r >= m_rows.size())
                continue;
            bool becomesLast = true;
            for (int i = r + 1; i < m_rows.size() && becomesLast; ++i)
                becomesLast = doomed.contains(i);
            if (!becomesLast) {
                replaceRange(m_rows.at(r).start, m_rows.at(r + 1).start - m_rows.at(r).start, QString());
            } else if (r == 0) {
                replaceRange(0, m_doc->characterCount() - 1, QString());
            } else {
                replaceRange(m_rows.at(r - 1).end, m_rows.at(r).end - m_rows.at(r - 1).end, QString());
            }
        }
    }
    reparse();
    endResetModel();
}

void CsvModel::insertColumn(int before)
{
    before = qBound(0, before, m_columns);
    beginInsertColumns(QModelIndex(), before, before);
    {
        EditGroup group(m_doc);
        for (int r = m_rows.size() - 1; r >= 0; --r) {
            const CsvData::Row &row = m_rows.at(r);
            if (before < row.cells.size())
                replaceRange(row.cells.at(before).start, 0, QString(m_delimiter));
            else if (before == row.cells.size())
                replaceRange(row.end, 0, QString(m_delimiter));
        }
    }
    reparse();
    endInsertColumns();
}

void CsvModel::removeColumns(QList<int> columns)
{
    std::sort(columns.begin(), columns.end(), std::greater<int>());
    columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
    if (columns.isEmpty())
        return;
    beginResetModel();
    {
        EditGroup group(m_doc);
        for (const int c : std::as_const(columns)) {
            for (int r = m_rows.size() - 1; r >= 0; --r) {
                const CsvData::Row &row = m_rows.at(r);
                if (c >= row.cells.size())
                    continue;
                const CsvData::Cell &cell = row.cells.at(c);
                if (row.cells.size() == 1) {
                    replaceRange(cell.start, cell.length, QString());
                } else if (c + 1 < row.cells.size()) { // the field and the delimiter after it
                    replaceRange(cell.start, row.cells.at(c + 1).start - cell.start, QString());
                } else { // the last field and the delimiter in front of it
                    const CsvData::Cell &prev = row.cells.at(c - 1);
                    const int from = prev.start + prev.length;
                    replaceRange(from, cell.start + cell.length - from, QString());
                }
            }
            // The offsets are stale after this column; read the file again before the next one.
            m_rows = CsvData::parse(m_doc->toPlainText(), m_delimiter);
        }
    }
    reparse();
    endResetModel();
}

void CsvModel::sortBy(int column, bool ascending)
{
    const int first = headerOffset();
    if (m_rows.size() - first < 2)
        return;
    const QString text = m_doc->toPlainText();
    QVector<int> order;
    bool numeric = true;
    for (int r = first; r < m_rows.size(); ++r) {
        order.append(r);
        const QString v = valueAt(r, column);
        if (!v.isEmpty() && !looksNumeric(v))
            numeric = false;
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const QString va = valueAt(a, column), vb = valueAt(b, column);
        if (va.isEmpty() != vb.isEmpty())
            return !va.isEmpty(); // blanks stay at the bottom either way
        if (va.isEmpty())
            return false;
        const int cmp = numeric ? (va.toDouble() < vb.toDouble() ? -1 : va.toDouble() > vb.toDouble() ? 1 : 0)
                                : va.compare(vb, Qt::CaseInsensitive);
        return ascending ? cmp < 0 : cmp > 0;
    });
    QStringList lines;
    for (const int r : std::as_const(order))
        lines.append(text.mid(m_rows.at(r).start, m_rows.at(r).end - m_rows.at(r).start));
    const int from = m_rows.at(first).start;
    {
        EditGroup group(m_doc);
        replaceRange(from, m_rows.last().end - from, lines.join(QLatin1Char('\n')));
    }
    reparse();
    if (rowCount() > 0 && m_columns > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, m_columns - 1));
}

int CsvModel::offsetOf(const QModelIndex &index) const
{
    const int record = index.row() + headerOffset();
    if (!index.isValid() || record >= m_rows.size())
        return -1;
    const CsvData::Row &row = m_rows.at(record);
    return index.column() < row.cells.size() ? row.cells.at(index.column()).start : row.end;
}

QModelIndex CsvModel::indexAtOffset(int position) const
{
    if (m_rows.isEmpty() || rowCount() == 0)
        return {};
    auto it = std::upper_bound(m_rows.begin(), m_rows.end(), position,
                               [](int pos, const CsvData::Row &r) { return pos < r.start; });
    int record = it == m_rows.begin() ? 0 : int(it - m_rows.begin()) - 1;
    const CsvData::Row &row = m_rows.at(record);
    int column = 0;
    for (int i = 0; i < row.cells.size(); ++i)
        if (row.cells.at(i).start <= position)
            column = i;
    record = qMax(record, headerOffset());
    return index(record - headerOffset(), column);
}

// --- CsvDelegate -----------------------------------------------------------------------------------

QWidget *CsvDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &index) const
{
    if (index.data(Qt::EditRole).toString().contains(QLatin1Char('\n'))) {
        auto *box = new QPlainTextEdit(parent);
        box->setFrameShape(QFrame::Box);
        return box;
    }
    auto *edit = new QLineEdit(parent);
    edit->setFrame(false);
    return edit;
}

void CsvDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &) const
{
    QRect r = option.rect;
    if (qobject_cast<QPlainTextEdit *>(editor)) {
        r.setWidth(qMax(r.width(), 280));
        r.setHeight(qMax(r.height(), 96));
    }
    editor->setGeometry(r);
}

bool CsvDelegate::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (auto *box = qobject_cast<QPlainTextEdit *>(object)) {
                if (key->modifiers() & Qt::ShiftModifier)
                    return false; // a line break inside the field
                emit enterPressed();
                emit commitData(box);
                emit closeEditor(box, QAbstractItemDelegate::SubmitModelCache);
                return true;
            }
            if (qobject_cast<QLineEdit *>(object))
                emit enterPressed();
        }
    }
    return QStyledItemDelegate::eventFilter(object, event);
}

// --- CsvTableView ----------------------------------------------------------------------------------

CsvTableView::CsvTableView(CsvModel *model, QWidget *parent)
    : QTableView(parent)
{
    setModel(model);
    auto *delegate = new CsvDelegate(this);
    setItemDelegate(delegate);
    connect(delegate, &CsvDelegate::enterPressed, this, [this] { m_enterMoves = true; });
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectItems);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setAlternatingRowColors(true);
    setWordWrap(false);
    setTextElideMode(Qt::ElideRight);
    setShowGrid(true);
    setCornerButtonEnabled(true);
    horizontalHeader()->setHighlightSections(false);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    horizontalHeader()->setMinimumSectionSize(40);
    horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setHighlightSections(false);
}

CsvModel *CsvTableView::csv() const
{
    return static_cast<CsvModel *>(model());
}

void CsvTableView::undo()
{
    if (state() == EditingState)
        return;
    csv()->document()->undo();
}

void CsvTableView::redo()
{
    if (state() == EditingState)
        return;
    csv()->document()->redo();
}

void CsvTableView::copy()
{
    const QModelIndexList sel = selectionModel()->selectedIndexes();
    if (sel.isEmpty())
        return;
    int r0 = INT_MAX, r1 = -1, c0 = INT_MAX, c1 = -1;
    QSet<QPair<int, int>> picked;
    for (const QModelIndex &i : sel) {
        r0 = qMin(r0, i.row());
        r1 = qMax(r1, i.row());
        c0 = qMin(c0, i.column());
        c1 = qMax(c1, i.column());
        picked.insert({i.row(), i.column()});
    }
    QStringList lines;
    for (int r = r0; r <= r1; ++r) {
        QStringList fields;
        for (int c = c0; c <= c1; ++c)
            fields.append(picked.contains({r, c}) ? CsvData::encode(model()->index(r, c).data(Qt::EditRole).toString(), QLatin1Char('\t'))
                                                  : QString());
        lines.append(fields.join(QLatin1Char('\t')));
    }
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void CsvTableView::clearSelection()
{
    QVector<CsvModel::Change> changes;
    const int off = csv()->headerOffset();
    for (const QModelIndex &i : selectionModel()->selectedIndexes())
        changes.append({i.row() + off, i.column(), QString()});
    csv()->setCells(changes);
}

void CsvTableView::cut()
{
    copy();
    clearSelection();
}

void CsvTableView::paste()
{
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty() || !currentIndex().isValid())
        return;
    if (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('\r')))
        text.chop(1);

    QVector<CsvData::Row> grid;
    if (text.contains(QLatin1Char('\t')))
        grid = CsvData::parse(text, QLatin1Char('\t'));
    else if (text.contains(QLatin1Char('\n')))
        grid = CsvData::parse(text, CsvData::detectDelimiter(text));
    CsvModel *m = csv();
    const int off = m->headerOffset();
    const QModelIndexList sel = selectionModel()->selectedIndexes();

    QVector<CsvModel::Change> changes;
    if (grid.isEmpty()) {
        // One value: into every selected cell.
        for (const QModelIndex &i : sel)
            changes.append({i.row() + off, i.column(), text});
        if (changes.isEmpty())
            changes.append({currentIndex().row() + off, currentIndex().column(), text});
    } else {
        int row0 = currentIndex().row(), col0 = currentIndex().column();
        for (const QModelIndex &i : sel) {
            row0 = qMin(row0, i.row());
            col0 = qMin(col0, i.column());
        }
        for (int r = 0; r < grid.size(); ++r)
            for (int c = 0; c < grid.at(r).cells.size(); ++c)
                changes.append({row0 + r + off, col0 + c, grid.at(r).cells.at(c).value});
    }

    QTextCursor group(m->document());
    group.beginEditBlock();
    int needed = 0;
    for (const CsvModel::Change &c : std::as_const(changes))
        needed = qMax(needed, c.row + 1);
    m->ensureRecords(needed);
    m->setCells(changes);
    group.endEditBlock();
}

void CsvTableView::keyPressEvent(QKeyEvent *event)
{
    if (state() != EditingState) {
        if (event->matches(QKeySequence::Undo)) {
            undo();
            return;
        }
        if (event->matches(QKeySequence::Redo)) {
            redo();
            return;
        }
        if (event->matches(QKeySequence::Copy)) {
            copy();
            return;
        }
        if (event->matches(QKeySequence::Cut)) {
            cut();
            return;
        }
        if (event->matches(QKeySequence::Paste)) {
            paste();
            return;
        }
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            clearSelection();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentIndex().isValid()) {
            edit(currentIndex());
            return;
        }
    }
    QTableView::keyPressEvent(event);
}

void CsvTableView::closeEditor(QWidget *editor, QAbstractItemDelegate::EndEditHint hint)
{
    const bool move = m_enterMoves && hint == QAbstractItemDelegate::SubmitModelCache;
    m_enterMoves = false;
    QTableView::closeEditor(editor, hint);
    if (move) {
        const QModelIndex next = model()->index(currentIndex().row() + 1, currentIndex().column());
        if (next.isValid())
            setCurrentIndex(next);
    }
}

void CsvTableView::contextMenuEvent(QContextMenuEvent *event)
{
    const QModelIndex i = indexAt(event->pos());
    if (i.isValid() && !selectionModel()->isSelected(i))
        setCurrentIndex(i);
    emit contextMenuRequested(event->globalPos());
}

// --- CsvTableWidget --------------------------------------------------------------------------------

CsvTableWidget::CsvTableWidget(QTextDocument *doc, const QString &fileName, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    m_model = new CsvModel(doc, this);
    m_model->setFileName(fileName);
    m_view = new CsvTableView(m_model, this);

    auto *bar = new QWidget(this);
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(8, 4, 8, 4);
    barLay->setSpacing(4);
    auto button = [&](const QString &text, const QString &icon, const QString &tip) {
        auto *b = new QToolButton(bar);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        Icons::bind(b, icon);
        barLay->addWidget(b);
        return b;
    };
    m_addRow = button(tr("Row"), QStringLiteral(":/new-icons/plus.svg"), tr("Insert a row below the current one"));
    m_addColumn = button(tr("Column"), QStringLiteral(":/new-icons/plus.svg"), tr("Insert a column to the right of the current one"));
    m_delRow = button(tr("Row"), QStringLiteral(":/new-icons/trash-2.svg"), tr("Delete the selected rows"));
    m_delColumn = button(tr("Column"), QStringLiteral(":/new-icons/trash-2.svg"), tr("Delete the selected columns"));
    barLay->addSpacing(8);
    m_headerBox = new QCheckBox(tr("First row is header"), bar);
    m_headerBox->setChecked(true);
    barLay->addWidget(m_headerBox);
    barLay->addStretch(1);
    m_info = new QLabel(bar);
    barLay->addWidget(m_info);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(bar);
    lay->addWidget(m_view, 1);
    setFocusProxy(m_view);

    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(0);
    connect(m_reloadTimer, &QTimer::timeout, this, &CsvTableWidget::reload);
    connect(doc, &QTextDocument::contentsChange, this, [this] {
        if (!m_model->editing())
            scheduleReload();
    });

    connect(m_addRow, &QToolButton::clicked, this, [this] { addRecord(true); });
    connect(m_addColumn, &QToolButton::clicked, this, [this] { addColumn(true); });
    connect(m_delRow, &QToolButton::clicked, this, &CsvTableWidget::deleteRecords);
    connect(m_delColumn, &QToolButton::clicked, this, &CsvTableWidget::deleteColumns);
    connect(m_headerBox, &QCheckBox::toggled, this, [this](bool on) {
        m_model->setHeaderRow(on);
        m_sized = false;
        autoSizeColumns();
        updateInfo();
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] { updateButtons(); });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] { updateButtons(); });
    connect(m_view, &CsvTableView::contextMenuRequested, this, &CsvTableWidget::showCellMenu);

    QHeaderView *hh = m_view->horizontalHeader();
    hh->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(hh, &QHeaderView::customContextMenuRequested, this, [this, hh](const QPoint &pos) {
        const int c = hh->logicalIndexAt(pos);
        if (c >= 0)
            showColumnMenu(c, hh->mapToGlobal(pos));
    });
    connect(hh, &QHeaderView::sectionDoubleClicked, this, &CsvTableWidget::renameColumn);
    QHeaderView *vh = m_view->verticalHeader();
    vh->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(vh, &QHeaderView::customContextMenuRequested, this, [this, vh](const QPoint &pos) {
        const int r = vh->logicalIndexAt(pos);
        if (r >= 0)
            showRowMenu(r, vh->mapToGlobal(pos));
    });

    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &CsvTableWidget::applyTheme);
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, [this] {
        applyTheme();
        m_sized = false;
        autoSizeColumns();
    });
    applyTheme();
    updateButtons();
}

void CsvTableWidget::setFileName(const QString &fileName)
{
    if (m_model->fileName() == fileName)
        return;
    m_model->setFileName(fileName);
    scheduleReload();
}

void CsvTableWidget::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const QFont f = SettingsManager::instance().editorFont();
    m_view->setFont(f);
    m_view->verticalHeader()->setDefaultSectionSize(QFontMetrics(f).height() + 10);
    const QColor alt = QColor::fromRgbF((t.editorBg.redF() + t.currentLine.redF()) / 2, (t.editorBg.greenF() + t.currentLine.greenF()) / 2,
                                        (t.editorBg.blueF() + t.currentLine.blueF()) / 2);
    m_view->setStyleSheet(
        QStringLiteral("QTableView { background: %1; alternate-background-color: %2; color: %3; gridline-color: %4; border: none;"
                       " selection-background-color: %5; selection-color: %3; }"
                       "QTableView::item { padding: 0 6px; border: none; }"
                       "QTableView::item:selected { background: %5; color: %3; }"
                       "QTableView::item:focus { border: 1px solid %6; }"
                       "QHeaderView::section { background: %7; color: %8; border: none; border-right: 1px solid %4;"
                       " border-bottom: 1px solid %4; padding: 2px 8px; font-weight: 600; }"
                       "QTableCornerButton::section { background: %7; border: none; border-right: 1px solid %4; border-bottom: 1px solid %4; }")
            .arg(t.editorBg.name(), alt.name(), t.editorFg.name(), t.border.name(), t.selection.name(), t.accent.name(), t.panel.name(),
                 t.textMuted.name()));
    QPalette pal = m_info->palette();
    pal.setColor(QPalette::WindowText, t.textMuted);
    m_info->setPalette(pal);
    setStyleSheet(QStringLiteral("CsvTableWidget { background: %1; }").arg(t.editorBg.name()));
}

void CsvTableWidget::scheduleReload()
{
    m_stale = true;
    if (isVisible())
        m_reloadTimer->start();
}

void CsvTableWidget::reload()
{
    if (!m_stale)
        return;
    m_stale = false;
    const QModelIndex cur = m_view->currentIndex();
    const int curRow = cur.row(), curCol = cur.column();
    const int scrollX = m_view->horizontalScrollBar()->value(), scrollY = m_view->verticalScrollBar()->value();
    const QVector<int> widths = columnWidths();

    m_model->reload();

    if (m_sized)
        restoreWidths(widths);
    else
        autoSizeColumns();
    updateInfo();
    if (cur.isValid() && m_model->rowCount() > 0)
        m_view->setCurrentIndex(m_model->index(qMin(curRow, m_model->rowCount() - 1), qMin(curCol, qMax(0, m_model->columnCount() - 1))));
    m_view->horizontalScrollBar()->setValue(scrollX);
    m_view->verticalScrollBar()->setValue(scrollY);
}

void CsvTableWidget::autoSizeColumns()
{
    if (m_sized)
        return;
    const QFontMetrics fm(m_view->font());
    QFont bold = m_view->font();
    bold.setBold(true);
    const QFontMetrics fmBold(bold);
    const int rows = qMin(m_model->records(), 300);
    for (int c = 0; c < m_model->columnCount(); ++c) {
        int w = fmBold.horizontalAdvance(m_model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString()) + 24;
        for (int r = 0; r < rows; ++r) {
            const QString v = m_model->valueAt(r, c);
            w = qMax(w, fm.horizontalAdvance(v.left(80)) + 20);
        }
        m_view->setColumnWidth(c, qBound(72, w, 380));
    }
    m_sized = true;
}

void CsvTableWidget::updateInfo()
{
    const int rows = m_model->rowCount();
    m_info->setText(tr("%n row(s)", nullptr, rows) + QStringLiteral(" × ") + tr("%n column(s)", nullptr, m_model->columns())
                    + QStringLiteral("  ·  ") + tr("delimiter: %1").arg(delimiterName(m_model->delimiter())));
}

void CsvTableWidget::updateButtons()
{
    const bool any = m_model->records() > 0;
    const bool sel = m_view->currentIndex().isValid();
    m_addRow->setEnabled(true);
    m_addColumn->setEnabled(any);
    m_delRow->setEnabled(sel);
    m_delColumn->setEnabled(sel);
}

QList<int> CsvTableWidget::selectedRecords() const
{
    QSet<int> set;
    for (const QModelIndex &i : m_view->selectionModel()->selectedIndexes())
        set.insert(i.row() + m_model->headerOffset());
    if (set.isEmpty() && m_view->currentIndex().isValid())
        set.insert(m_view->currentIndex().row() + m_model->headerOffset());
    QList<int> out(set.begin(), set.end());
    std::sort(out.begin(), out.end());
    return out;
}

QList<int> CsvTableWidget::selectedColumns() const
{
    QSet<int> set;
    for (const QModelIndex &i : m_view->selectionModel()->selectedIndexes())
        set.insert(i.column());
    if (set.isEmpty() && m_view->currentIndex().isValid())
        set.insert(m_view->currentIndex().column());
    QList<int> out(set.begin(), set.end());
    std::sort(out.begin(), out.end());
    return out;
}

void CsvTableWidget::select(int record, int column)
{
    const int row = record - m_model->headerOffset();
    if (row < 0 || row >= m_model->rowCount() || column < 0 || column >= m_model->columnCount())
        return;
    const QModelIndex i = m_model->index(row, column);
    m_view->clearSelection();
    m_view->setCurrentIndex(i);
    m_view->selectionModel()->select(i, QItemSelectionModel::ClearAndSelect);
    m_view->scrollTo(i);
}

QVector<int> CsvTableWidget::columnWidths() const
{
    QVector<int> widths;
    for (int c = 0; c < m_model->columnCount(); ++c)
        widths.append(m_view->columnWidth(c));
    return widths;
}

void CsvTableWidget::restoreWidths(const QVector<int> &widths)
{
    if (widths.size() != m_model->columnCount() || widths.isEmpty()) {
        m_sized = false;
        autoSizeColumns();
        return;
    }
    for (int c = 0; c < widths.size(); ++c)
        m_view->setColumnWidth(c, widths.at(c));
}

void CsvTableWidget::addRecord(bool below)
{
    const int off = m_model->headerOffset();
    const QModelIndex cur = m_view->currentIndex();
    int at = m_model->records(); // no current cell: at the end
    if (cur.isValid())
        at = cur.row() + off + (below ? 1 : 0);
    if (m_model->records() > 0)
        at = qMax(at, off); // never above the header
    const int col = cur.isValid() ? cur.column() : 0;
    const bool wasEmpty = m_model->records() == 0;
    m_model->insertRecord(at);
    if (wasEmpty) {
        m_sized = false;
        autoSizeColumns();
    }
    updateInfo();
    select(wasEmpty ? 0 : qMin(at, m_model->records() - 1), col);
}

void CsvTableWidget::addColumn(bool right)
{
    const QModelIndex cur = m_view->currentIndex();
    const int at = cur.isValid() ? cur.column() + (right ? 1 : 0) : m_model->columns();
    const int row = cur.isValid() ? cur.row() : 0;
    m_model->insertColumn(at);
    m_view->setColumnWidth(qMin(at, m_model->columnCount() - 1), 110);
    updateInfo();
    select(row + m_model->headerOffset(), at);
}

void CsvTableWidget::deleteRecords()
{
    const QList<int> rows = selectedRecords();
    if (rows.isEmpty())
        return;
    const int col = qMax(0, m_view->currentIndex().column());
    const QVector<int> widths = columnWidths();
    const int scroll = m_view->verticalScrollBar()->value();
    m_model->removeRecords(rows);
    restoreWidths(widths);
    m_view->verticalScrollBar()->setValue(scroll);
    updateInfo();
    select(qMin(rows.first(), m_model->records() - 1), col);
}

void CsvTableWidget::deleteColumns()
{
    const QList<int> cols = selectedColumns();
    if (cols.isEmpty())
        return;
    const int row = qMax(0, m_view->currentIndex().row());
    const QVector<int> widths = columnWidths();
    m_model->removeColumns(cols);
    int target = 0;
    for (int c = 0; c < widths.size(); ++c) {
        if (cols.contains(c))
            continue;
        if (target < m_model->columnCount())
            m_view->setColumnWidth(target, widths.at(c));
        ++target;
    }
    updateInfo();
    select(row + m_model->headerOffset(), qMin(cols.first(), m_model->columnCount() - 1));
}

void CsvTableWidget::renameColumn(int column)
{
    if (!m_model->headerRow() || m_model->records() == 0)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Column"), tr("Column name:"), QLineEdit::Normal,
                                               m_model->valueAt(0, column), &ok);
    if (ok)
        m_model->setCells({{0, column, name}});
}

void CsvTableWidget::showCellMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    const bool sel = m_view->currentIndex().isValid();
    menu.addAction(tr("Cut"), m_view, &CsvTableView::cut)->setEnabled(sel);
    menu.addAction(tr("Copy"), m_view, &CsvTableView::copy)->setEnabled(sel);
    menu.addAction(tr("Paste"), m_view, &CsvTableView::paste)->setEnabled(sel);
    menu.addAction(tr("Clear Cells"), m_view, &CsvTableView::clearSelection)->setEnabled(sel);
    menu.addSeparator();
    menu.addAction(tr("Insert Row Above"), this, [this] { addRecord(false); });
    menu.addAction(tr("Insert Row Below"), this, [this] { addRecord(true); });
    menu.addAction(tr("Delete Row(s)"), this, &CsvTableWidget::deleteRecords)->setEnabled(sel);
    menu.addSeparator();
    menu.addAction(tr("Insert Column Left"), this, [this] { addColumn(false); })->setEnabled(sel);
    menu.addAction(tr("Insert Column Right"), this, [this] { addColumn(true); })->setEnabled(sel);
    menu.addAction(tr("Delete Column(s)"), this, &CsvTableWidget::deleteColumns)->setEnabled(sel);
    menu.exec(globalPos);
}

void CsvTableWidget::showColumnMenu(int column, const QPoint &globalPos)
{
    if (!m_view->selectionModel()->isColumnSelected(column))
        m_view->selectColumn(column);
    if (m_model->rowCount() > 0)
        m_view->setCurrentIndex(m_model->index(qMax(0, m_view->currentIndex().row()), column));
    QMenu menu(this);
    if (m_model->headerRow())
        menu.addAction(tr("Rename Column…"), this, [this, column] { renameColumn(column); });
    menu.addAction(tr("Sort A → Z"), this, [this, column] { m_model->sortBy(column, true); updateInfo(); });
    menu.addAction(tr("Sort Z → A"), this, [this, column] { m_model->sortBy(column, false); updateInfo(); });
    menu.addSeparator();
    menu.addAction(tr("Insert Column Left"), this, [this] { addColumn(false); });
    menu.addAction(tr("Insert Column Right"), this, [this] { addColumn(true); });
    menu.addAction(tr("Delete Column(s)"), this, &CsvTableWidget::deleteColumns);
    menu.exec(globalPos);
}

void CsvTableWidget::showRowMenu(int row, const QPoint &globalPos)
{
    if (!m_view->selectionModel()->isRowSelected(row))
        m_view->selectRow(row);
    m_view->setCurrentIndex(m_model->index(row, qMax(0, m_view->currentIndex().column())));
    QMenu menu(this);
    menu.addAction(tr("Insert Row Above"), this, [this] { addRecord(false); });
    menu.addAction(tr("Insert Row Below"), this, [this] { addRecord(true); });
    menu.addAction(tr("Delete Row(s)"), this, &CsvTableWidget::deleteRecords);
    menu.exec(globalPos);
}

void CsvTableWidget::activate(int position)
{
    m_stale = true;
    reload();
    QModelIndex target;
    if (position >= 0)
        target = m_model->indexAtOffset(position);
    if (!target.isValid() && m_model->rowCount() > 0)
        target = m_model->index(0, 0);
    if (target.isValid()) {
        m_view->clearSelection();
        m_view->setCurrentIndex(target);
        m_view->selectionModel()->select(target, QItemSelectionModel::ClearAndSelect);
        m_view->scrollTo(target, QAbstractItemView::PositionAtCenter);
    }
    m_view->setFocus();
}

int CsvTableWidget::currentPosition() const
{
    return m_model->offsetOf(m_view->currentIndex());
}
