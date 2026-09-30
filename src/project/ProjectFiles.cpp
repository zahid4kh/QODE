#include "ProjectFiles.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

namespace {

constexpr int kMaxFiles = 300000;

const QSet<QString> &skippedDirs()
{
    static const QSet<QString> s = {QStringLiteral(".git"),     QStringLiteral(".hg"),      QStringLiteral(".svn"),
                                    QStringLiteral("node_modules"), QStringLiteral("__pycache__"), QStringLiteral(".cache"),
                                    QStringLiteral(".idea"),    QStringLiteral(".venv"),    QStringLiteral("venv"),
                                    QStringLiteral("target"),   QStringLiteral("dist"),     QStringLiteral("build"),
                                    QStringLiteral(".gradle"),  QStringLiteral(".next")};
    return s;
}

bool gitList(const QString &root, QStringList *out)
{
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        return false;
    QProcess p;
    p.setWorkingDirectory(root);
    p.start(git, {QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--cached"), QStringLiteral("--others"),
                  QStringLiteral("--exclude-standard")});
    if (!p.waitForStarted(5000) || !p.waitForFinished(60000) || p.exitCode() != 0)
        return false;
    const QDir dir(root);
    for (const QByteArray &rel : p.readAllStandardOutput().split('\0')) {
        if (rel.isEmpty())
            continue;
        const QString abs = dir.filePath(QString::fromUtf8(rel));
        const QFileInfo fi(abs);
        // ls-files also lists deleted-but-unstaged files and submodule directories.
        if (fi.isFile() && out->size() < kMaxFiles)
            out->append(fi.absoluteFilePath());
    }
    return true;
}

// Depth-first walk that never enters skipped directories.
void walk(const QString &dirPath, QStringList *out)
{
    const QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo &fi : entries) {
        if (out->size() >= kMaxFiles)
            return;
        if (fi.isDir()) {
            if (!skippedDirs().contains(fi.fileName()))
                walk(fi.absoluteFilePath(), out);
        } else {
            out->append(fi.absoluteFilePath());
        }
    }
}

} // namespace

ProjectFiles::ProjectFiles(QObject *parent)
    : QObject(parent)
{
}

ProjectFiles::~ProjectFiles()
{
    ++m_generation;
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

QStringList ProjectFiles::scan(const QString &root)
{
    QStringList files;
    if (root.isEmpty())
        return files;
    if (!gitList(root, &files)) {
        files.clear();
        walk(root, &files);
    }
    files.sort(Qt::CaseInsensitive);
    return files;
}

void ProjectFiles::setRoot(const QString &root)
{
    if (m_root == root)
        return;
    m_root = root;
    m_files.clear();
    ++m_generation; // drop any scan still running for the old root
    m_dirty = true;
    emit updated();
    if (!root.isEmpty())
        rescan();
}

void ProjectFiles::invalidate()
{
    m_dirty = true;
}

void ProjectFiles::ensureFresh()
{
    if (m_dirty && !m_root.isEmpty())
        rescan();
}

void ProjectFiles::rescan()
{
    m_dirty = false;
    if (m_thread) {
        m_again = true; // run once more when the current scan ends
        return;
    }
    m_scanning = true;
    const int gen = m_generation;
    const QString root = m_root;
    QPointer<ProjectFiles> self(this);
    m_thread = QThread::create([self, root, gen] {
        QStringList files = ProjectFiles::scan(root);
        QMetaObject::invokeMethod(
            self.data(),
            [self, files = std::move(files), gen]() mutable {
                if (!self)
                    return;
                if (gen == self->m_generation)
                    self->m_files = std::move(files);
                self->m_scanning = self->m_again; // another pass is already queued
                emit self->updated();
            },
            Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_thread = nullptr;
        if (m_again) {
            m_again = false;
            rescan();
        }
    });
    m_thread->start();
}
