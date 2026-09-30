#pragma once

#include <QObject>
#include <QStringList>

class QThread;

// A cached, asynchronously refreshed list of every file in the open project (Quick Open, project search).
// Inside a git repository it asks `git ls-files` so .gitignore is honoured exactly; otherwise it walks the
// tree and skips well-known dependency/build directories.
class ProjectFiles : public QObject
{
    Q_OBJECT
public:
    explicit ProjectFiles(QObject *parent = nullptr);
    ~ProjectFiles() override;

    void setRoot(const QString &root); // "" => no project
    QString root() const { return m_root; }
    const QStringList &files() const { return m_files; } // absolute paths, sorted
    bool isScanning() const { return m_scanning; }

    void invalidate();   // the tree changed; the next ensureFresh() rescans
    void ensureFresh();  // rescan in the background when never scanned or invalidated

    // Blocking scan, safe to call from worker threads.
    static QStringList scan(const QString &root);

signals:
    void updated(); // a new file list is available

private:
    void rescan();

    QString m_root;
    QStringList m_files;
    QThread *m_thread = nullptr;
    int m_generation = 0;
    bool m_dirty = true;
    bool m_again = false;
    bool m_scanning = false;
};
