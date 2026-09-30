#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QRegularExpression>
#include <QString>

#include <atomic>
#include <memory>

class QTextDocument;
class QThread;

struct SearchOptions {
    QString query;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;
    QString include; // comma separated globs ("*.cpp, src/*"); empty = everything
    QString exclude;

    // Compiles the query; the result is invalid (with an error string) for bad regular expressions.
    QRegularExpression toRegex() const;
};

struct SearchMatch {
    int line = 0;   // 1-based
    int column = 0; // 0-based, in the original line
    int length = 0;
    QString preview;   // the (trimmed, capped) line text
    int previewStart = 0; // where the match begins inside `preview`
};

struct SearchFileResult {
    QString path;
    QList<SearchMatch> matches;
};

// Project-wide text search on a background thread. Results arrive in batches so the UI can fill in
// while the search is still running. Unsaved editor buffers are searched instead of the disk copy.
class ProjectSearch : public QObject
{
    Q_OBJECT
public:
    explicit ProjectSearch(QObject *parent = nullptr);
    ~ProjectSearch() override;

    // `overrides` maps absolute paths of open documents to their current text.
    void start(const QString &root, const SearchOptions &options, const QHash<QString, QString> &overrides);
    void cancel();
    bool isRunning() const { return m_running; }

    // --- Shared with "replace in files" ------------------------------------------------------
    // Expands $1..$9, $& / $0 and $$ in `replacement` for one regex match.
    static QString expandReplacement(const QString &replacement, const QRegularExpressionMatch &m);
    // Applies the replacement to every line of `text`; returns the number of replacements.
    static int replaceInText(QString *text, const QRegularExpression &rx, const QString &replacement);
    // Same, but as edits on a live document (one undo step).
    static int replaceInDocument(QTextDocument *doc, const QRegularExpression &rx, const QString &replacement);
    // Reads a file the way the searcher does; false for binary / non UTF-8 / oversized files.
    static bool readTextFile(const QString &path, QString *text, bool *hasBom = nullptr);

signals:
    void results(const QList<SearchFileResult> &batch);
    // `error` is empty unless the query was invalid. `truncated`: the match limit was hit.
    void finished(int files, int matches, bool truncated, const QString &error);

private:
    struct Job;
    void onBatch(int generation, const QList<SearchFileResult> &batch);
    void onDone(int generation, int files, int matches, bool truncated);

    QList<QThread *> m_threads;
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_generation = 0;
    bool m_running = false;
};
