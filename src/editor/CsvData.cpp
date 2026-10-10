#include "CsvData.h"

#include <QFileInfo>

namespace CsvData {

QChar detectDelimiter(const QString &sample, const QString &fileName)
{
    if (QFileInfo(fileName).suffix().compare(QLatin1String("tsv"), Qt::CaseInsensitive) == 0)
        return QLatin1Char('\t');

    const QChar candidates[] = {QLatin1Char(','), QLatin1Char(';'), QLatin1Char('\t'), QLatin1Char('|')};
    int counts[4] = {0, 0, 0, 0};
    bool quoted = false;
    int lines = 0;
    for (const QChar c : sample) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (!quoted) {
            if (c == QLatin1Char('\n')) {
                if (++lines >= 5)
                    break;
            }
            for (int i = 0; i < 4; ++i)
                if (c == candidates[i])
                    ++counts[i];
        }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (counts[i] > counts[best])
            best = i;
    return counts[best] > 0 ? candidates[best] : QLatin1Char(',');
}

QVector<Row> parse(const QString &text, QChar delimiter)
{
    QVector<Row> rows;
    const int n = text.size();
    int i = 0;
    while (i < n) {
        Row row;
        row.start = i;
        for (;;) {
            Cell cell;
            cell.start = i;
            if (i < n && text.at(i) == QLatin1Char('"')) {
                ++i;
                for (;;) {
                    if (i >= n)
                        break; // unterminated: the field runs to the end of the file
                    const QChar c = text.at(i);
                    if (c == QLatin1Char('"')) {
                        if (i + 1 < n && text.at(i + 1) == QLatin1Char('"')) {
                            cell.value += QLatin1Char('"');
                            i += 2;
                            continue;
                        }
                        ++i; // closing quote
                        break;
                    }
                    cell.value += c;
                    ++i;
                }
                // Stray text after the closing quote stays part of the value ("a"b -> ab).
                while (i < n && text.at(i) != delimiter && text.at(i) != QLatin1Char('\n')) {
                    cell.value += text.at(i);
                    ++i;
                }
            } else {
                const int from = i;
                while (i < n && text.at(i) != delimiter && text.at(i) != QLatin1Char('\n'))
                    ++i;
                cell.value = text.mid(from, i - from);
            }
            cell.length = i - cell.start;
            // A CR in front of the line break belongs to the line ending, not to the last field.
            if ((i >= n || text.at(i) == QLatin1Char('\n')) && cell.value.endsWith(QLatin1Char('\r'))
                && cell.length > 0 && text.at(i - 1) == QLatin1Char('\r')) {
                cell.value.chop(1);
                --cell.length;
            }
            row.cells.append(cell);
            if (i < n && text.at(i) == delimiter) {
                ++i;
                if (i >= n) { // the file ends right after a delimiter: one more, empty field
                    Cell last;
                    last.start = i;
                    row.cells.append(last);
                }
                continue;
            }
            break;
        }
        row.end = row.cells.last().start + row.cells.last().length;
        rows.append(row);
        if (i < n && text.at(i) == QLatin1Char('\n'))
            ++i;
    }
    return rows;
}

bool needsQuotes(const QString &value, QChar delimiter)
{
    if (value.isEmpty())
        return false;
    for (const QChar c : value)
        if (c == delimiter || c == QLatin1Char('"') || c == QLatin1Char('\n') || c == QLatin1Char('\r'))
            return true;
    return value.front().isSpace() || value.back().isSpace();
}

QString encode(const QString &value, QChar delimiter, bool forceQuotes)
{
    if (!forceQuotes && !needsQuotes(value, delimiter))
        return value;
    QString out = value;
    out.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

QString columnName(int column)
{
    QString s;
    for (int c = column; c >= 0; c = c / 26 - 1)
        s.prepend(QChar(u'A' + c % 26));
    return s;
}

} // namespace CsvData
