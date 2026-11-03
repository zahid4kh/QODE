#pragma once

#include "agent/CodeChunks.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QThread;
class QTimer;

// The codebase index behind the chat's semantic_search tool: every text file of the project split into sections
// (CodeChunks), ranked two ways and merged with reciprocal rank fusion:
//   - keywords: BM25 over identifier parts, the section's declared names and its path; local, always on;
//   - meaning: Gemini embeddings (gemini-embedding-2, 768 dims) when a Gemini key is set and the option is on.
// Sections are rebuilt from disk on a worker thread when a file's mtime/size changes; vectors are stored under a hash of
// the text they were made from (~/.cache/QODE-AGENTIC/code-index/<project>.vec), so only new or changed sections are
// sent to Google, also across restarts. The model never sees the index, only the sections a search returns.
class CodeIndex : public QObject
{
    Q_OBJECT
public:
    struct Hit {
        QString path;            // project-relative
        int start = 0, end = 0;  // 1-based lines, inclusive
        QString symbol;
        bool keyword = false, meaning = false; // which ranking found it
    };
    struct Status {
        bool building = false;
        int files = 0, sections = 0;
        bool embeddingsOn = false, hasKey = false;
        int embedded = 0;       // sections of the project with a vector
        bool embedding = false; // requests in flight or queued
        QString error;          // last embedding failure, for the menu
    };

    explicit CodeIndex(QObject *parent = nullptr);
    ~CodeIndex() override;

    // `cacheFile` holds the vectors; "" (no project) clears everything.
    void setProject(const QString &root, const QString &cacheFile);
    // The project's files (absolute), from the file index: new and changed ones are (re)indexed in the background.
    void setFiles(const QStringList &absolute);
    // A file was saved: re-index it soon.
    void fileChanged(const QString &absolute);
    // Drops the sections (not the vectors) and indexes everything again.
    void rebuild();

    static bool embeddingsEnabled();
    void setEmbeddingsEnabled(bool on);
    // The Gemini key may have been added or removed.
    void keyChanged();

    // Runs on the UI thread and blocks (with an event loop) while a build finishes and the query is embedded.
    // `scope` is a project-relative folder or file ("" = everything); `overlay` maps project-relative paths of unsaved
    // editor buffers to their text, which is searched instead of the file on disk. `note` explains a partial result.
    QVector<Hit> search(const QString &query, const QString &scope, int max, const QHash<QString, QString> &overlay, QString *note);

    Status status() const;

signals:
    void statusChanged();

public:
    // One section in the index: its lines, its terms (sorted hash << 32 | weighted count) and the key of its vector.
    struct Entry {
        agent::codeindex::Section section;
        QVector<quint64> terms;
        int length = 0;
        QByteArray key;
    };
    struct FileData {
        qint64 mtime = 0, size = 0;
        QVector<Entry> sections;
    };
    // Reads and splits one file (any thread); false when it is not indexable text.
    static bool indexFile(const QString &root, const QString &rel, FileData *out);
    static void indexText(const QString &rel, const QString &text, FileData *out);

private:
    struct Vec {
        float scale = 0;
        QByteArray q; // int8 components
    };

    void startJob();
    void putFile(const QString &rel, FileData data);
    void dropFile(const QString &rel);
    void refreshNow(const QString &scope);
    bool waitForJob(int ms);
    double bm25(const Entry &e, const QVector<quint32> &query) const;
    double idf(quint32 term) const;

    // embeddings
    QString apiKey() const;
    void schedulePump(int ms = 1500);
    void pump();
    bool embedQuery(const QString &query, QVector<float> *out, QString *error);
    void loadVectors();
    void saveVectors();
    static Vec quantize(const QJsonArray &values);

    QString m_root, m_cacheFile;
    QStringList m_wanted;                  // project-relative, indexable names
    QSet<QString> m_wantedSet;
    QHash<QString, FileData> m_files;
    QHash<quint32, int> m_df;
    qint64 m_totalLength = 0;
    int m_sectionCount = 0;
    bool m_fullPass = false;               // every wanted file has been looked at once (vectors may be pruned)

    QThread *m_thread = nullptr;
    int m_generation = 0;
    bool m_again = false;
    QSet<QString> m_forced;                // saved files: re-read even when mtime/size look unchanged

    QHash<QByteArray, Vec> m_vectors;
    QSet<QByteArray> m_skip;               // keys that could not be embedded this session
    QNetworkAccessManager *m_net;
    QPointer<QNetworkReply> m_reply;
    QTimer *m_pumpTimer, *m_saveTimer;
    QString m_error;
    bool m_blocked = false;                // the key was rejected: wait for keyChanged()
    bool m_vectorsDirty = false;
    int m_batchChars = 60000;              // adapts to the key's per-minute token limit
    int m_gapMs = 700;                     // between batches
    QHash<QString, QVector<float>> m_queryCache;
};
