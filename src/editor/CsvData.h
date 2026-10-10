#pragma once

#include <QString>
#include <QVector>

// Qt Core only: reads delimited text (CSV / TSV) and remembers where every field sits in the source, so the table
// view can rewrite one cell in the document without touching the rest of the file.
namespace CsvData {

struct Cell {
    QString value; // unquoted
    int start = 0; // offset of the raw field (with its quotes) in the parsed text
    int length = 0;
};

struct Row {
    QVector<Cell> cells;
    int start = 0; // first character of the record
    int end = 0;   // one past the last character, newline excluded
};

// The separator a file most likely uses: the one that is most common on the first lines (outside quotes) among
// , ; tab |. A .tsv file always gets a tab. Defaults to a comma.
QChar detectDelimiter(const QString &sample, const QString &fileName = QString());

// Splits `text` into records. Quoted fields may hold the delimiter, doubled quotes and line breaks. A final line
// break does not start another record.
QVector<Row> parse(const QString &text, QChar delimiter);

// True when `value` cannot be written as it is.
bool needsQuotes(const QString &value, QChar delimiter);
// The field as it is written to the file; `forceQuotes` keeps a field quoted that was quoted before.
QString encode(const QString &value, QChar delimiter, bool forceQuotes = false);

// "A", "B" ... "Z", "AA" ... for a 0-based column.
QString columnName(int column);

} // namespace CsvData
