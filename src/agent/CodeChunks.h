#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// The text side of the codebase index (CodeIndex): splitting a file into sections and turning text into search terms.
// Qt Core only and free of state, so the index can run it on a worker thread.
namespace agent::codeindex {

// One section of a file: lines start..end (1-based, inclusive) and the first declaration in it.
struct Section {
    int start = 0, end = 0;
    QString symbol;          // "MainWindow::openFile", "class Parser"; "" when nothing declares
    QStringList symbols;     // every declared name (weighted higher in the keyword ranking)
};

// Splits a file (lines without line breaks) into sections of about 50 lines, cutting in front of top-level
// declarations and blank lines so a function stays in one piece where it can (a comment above it goes with it).
// `prose` (Markdown and other documents, see isProse) cuts at headings and names sections after them.
QVector<Section> split(const QStringList &lines, bool prose = false);
bool isProse(const QString &rel);

// Search terms of a text: identifiers lowercased, also split at camelCase / snake_case ("escapeSqlParams" →
// escapesqlparams, escape, sql, param), lightly stemmed (sanitize / sanitized / sanitizing → sanitiz), stop words dropped.
QStringList terms(const QString &text);
quint32 termHash(const QString &term);

// What the embedding model reads for a section, and the key its vector is stored under (16 bytes of SHA-1).
QString embedText(const QString &rel, const Section &section, const QStringList &lines);
QByteArray keyOf(const QString &embedText);

// Files worth indexing: text sources, not secrets, lock files, minified bundles or binaries (judged by the name).
bool indexableName(const QString &rel);

} // namespace agent::codeindex
