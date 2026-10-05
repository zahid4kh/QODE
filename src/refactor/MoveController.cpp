#include "MoveController.h"

#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/ProjectExplorer.h"
#include "project/ProjectFiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>

using MoveRefactor::PathMove;

MoveController::MoveController(ProjectExplorer *explorer, EditorManager *editors, QObject *parent)
    : QObject(parent)
    , m_explorer(explorer)
    , m_editors(editors)
{
}

void MoveController::setProjectRoot(const QString &root)
{
    m_root = QDir::cleanPath(root);
    m_hasLast = false;
    m_last = {};
}

QString MoveController::where(const QString &dir) const
{
    if (dir == m_root)
        return tr("the project root");
    return QDir(m_root).relativeFilePath(dir);
}

void MoveController::fail(const QString &text)
{
    ProjectExplorer::Notice n;
    n.text = text;
    n.error = true;
    n.timeoutMs = 12000;
    m_explorer->showNotice(n);
    m_busy = false;
}

void MoveController::move(const QStringList &sources, const QString &targetDir)
{
    if (m_busy || m_root.isEmpty())
        return;
    QVector<PathMove> moves;
    for (const QString &src : sources) {
        const QString to = QDir::cleanPath(targetDir + QLatin1Char('/') + QFileInfo(src).fileName());
        if (src == to || !QFileInfo::exists(src))
            continue;
        if (QFileInfo::exists(to)) {
            fail(tr("“%1” already exists in %2.").arg(QFileInfo(src).fileName(), where(targetDir)));
            return;
        }
        moves.append({src, to});
    }
    if (moves.isEmpty())
        return;

    m_busy = true;
    MoveRefactor::Input in;
    in.root = m_root;
    in.moves = moves;
    for (Document *d : m_editors->documents())
        if (!d->isUntitled())
            in.texts.insert(d->filePath(), d->text()); // what the editors hold, saved or not
    auto plan = std::make_shared<MoveRefactor::Plan>();
    QThread *worker = QThread::create([in, plan]() mutable {
        in.files = ProjectFiles::scan(in.root);
        *plan = MoveRefactor::plan(in);
    });
    connect(worker, &QThread::finished, this, [this, worker, moves, plan, targetDir] {
        worker->deleteLater();
        execute(moves, *plan, targetDir);
    });
    worker->start();
    // Only a slow scan is worth a message.
    QPointer<QThread> alive(worker);
    QTimer::singleShot(250, this, [this, alive] {
        if (m_busy && alive && !alive->isFinished()) {
            ProjectExplorer::Notice n;
            n.text = tr("Looking for references to update…");
            n.timeoutMs = 0;
            m_explorer->showNotice(n);
        }
    });
}

bool MoveController::applyEdits(const QString &path, const QVector<MoveRefactor::TextEdit> &edits, size_t expectedHash, Changed *record, int references)
{
    if (Document *doc = m_editors->documentForPath(path)) {
        CodeEditor *ed = m_editors->editorFor(doc);
        const QString before = doc->text();
        if (!ed || qHash(before) != expectedHash)
            return false;
        const bool wasModified = doc->isModified();
        if (!ed->applyOffsetEdits(edits))
            return false;
        if (record) {
            record->backward = MoveRefactor::inverse(before, edits);
            record->afterHash = qHash(doc->text());
        }
        if (!wasModified)
            m_editors->saveQuietly(doc);
    } else {
        QString before;
        bool bom = false;
        if (!MoveRefactor::readText(path, &before, &bom) || qHash(before) != expectedHash)
            return false;
        const QString after = MoveRefactor::applyEdits(before, edits);
        if (!MoveRefactor::writeText(path, after, bom))
            return false;
        if (record) {
            record->backward = MoveRefactor::inverse(before, edits);
            record->afterHash = qHash(after);
        }
    }
    if (record) {
        record->path = path;
        record->references = references;
    }
    return true;
}

void MoveController::execute(const QVector<PathMove> &moves, const MoveRefactor::Plan &plan, const QString &targetDir)
{
    // 1. the files themselves
    QVector<PathMove> done;
    for (const PathMove &m : moves) {
        if (QFileInfo::exists(m.to) || !QFile::rename(m.from, m.to)) {
            for (int i = done.size() - 1; i >= 0; --i) {
                QFile::rename(done.at(i).to, done.at(i).from);
                m_explorer->notifyMoved(done.at(i).to, done.at(i).from);
            }
            fail(tr("Couldn't move “%1” to %2.").arg(QFileInfo(m.from).fileName(), where(targetDir)));
            return;
        }
        done.append(m);
        m_explorer->notifyMoved(m.from, m.to); // open tabs, previews ... follow
    }

    // 2. what refers to them
    Batch batch;
    batch.moves = moves;
    batch.targetDir = targetDir;
    int skipped = 0, references = 0;
    for (const MoveRefactor::FileChange &c : plan.changes) {
        Changed rec;
        if (applyEdits(c.path, c.edits, c.baseHash, &rec, c.references)) {
            batch.changed.append(rec);
            references += c.references;
        } else {
            ++skipped;
        }
    }
    m_last = batch;
    m_hasLast = true;
    m_busy = false;

    // 3. tell the person
    ProjectExplorer::Notice n;
    const QString what = moves.size() == 1 ? tr("“%1”").arg(QFileInfo(moves.first().from).fileName()) : tr("%1 items").arg(moves.size());
    n.text = tr("Moved %1 to %2").arg(what, where(targetDir));
    if (references > 0)
        n.text += QLatin1Char('\n') + tr("Updated %1 in %2").arg(references == 1 ? tr("1 reference") : tr("%1 references").arg(references),
                                                                batch.changed.size() == 1 ? tr("1 file") : tr("%1 files").arg(batch.changed.size()));
    n.actionLabel = tr("Undo");
    n.action = [this] { undo(); };
    constexpr int kMaxListed = 25;
    int listed = 0;
    QPointer<EditorManager> editors(m_editors);
    for (const Changed &c : std::as_const(batch.changed)) {
        if (listed++ >= kMaxListed) {
            n.details.append({tr("… and %1 more").arg(batch.changed.size() - kMaxListed), nullptr});
            break;
        }
        const QString path = c.path;
        n.details.append({QStringLiteral("%1   (%2)").arg(QDir(m_root).relativeFilePath(path)).arg(c.references), [editors, path] {
                              if (editors)
                                  editors->openFile(path);
                          }});
    }
    QStringList warnings = plan.notes;
    if (skipped > 0)
        warnings << (skipped == 1 ? tr("1 file changed while the move was prepared and was left alone")
                                  : tr("%1 files changed while the move was prepared and were left alone").arg(skipped));
    if (!warnings.isEmpty()) {
        if (!n.details.isEmpty())
            n.details.append({QString(), nullptr});
        for (const QString &w : std::as_const(warnings))
            n.details.append({QStringLiteral("⚠  ") + w, nullptr});
    }
    n.timeoutMs = warnings.isEmpty() ? 14000 : 30000;
    m_explorer->showNotice(n);

    QStringList moved;
    for (const PathMove &m : moves)
        moved << m.to;
    m_explorer->revealPaths(moved);
}

void MoveController::undo()
{
    if (!m_hasLast || m_busy)
        return;
    m_busy = true;
    const Batch b = m_last;
    m_hasLast = false;
    int skipped = 0;
    for (int i = b.changed.size() - 1; i >= 0; --i) {
        const Changed &c = b.changed.at(i);
        if (!applyEdits(c.path, c.backward, c.afterHash, nullptr, 0))
            ++skipped;
    }
    QStringList back;
    for (int i = b.moves.size() - 1; i >= 0; --i) {
        const PathMove &m = b.moves.at(i);
        if (QFileInfo::exists(m.from) || !QFile::rename(m.to, m.from)) {
            fail(tr("Couldn't move “%1” back.").arg(QFileInfo(m.to).fileName()));
            return;
        }
        m_explorer->notifyMoved(m.to, m.from);
        back << m.from;
    }
    m_busy = false;
    ProjectExplorer::Notice n;
    n.text = tr("Move undone");
    if (skipped > 0) {
        n.text += QLatin1Char('\n') + (skipped == 1 ? tr("1 file was edited since and keeps its updated references")
                                                       : tr("%1 files were edited since and keep their updated references").arg(skipped));
        n.error = true;
    }
    n.timeoutMs = 7000;
    m_explorer->showNotice(n);
    m_explorer->revealPaths(back);
}
