#pragma once

#include "MoveRefactor.h"

#include <QObject>
#include <QVector>

class ProjectExplorer;
class EditorManager;

// Carries out a drag-and-drop move from the explorer: plans the reference updates on a worker thread, moves the files,
// rewrites imports / includes / links in the affected files (open ones through their editors, the rest on disk) and
// offers to undo the whole thing.
class MoveController : public QObject
{
    Q_OBJECT
public:
    MoveController(ProjectExplorer *explorer, EditorManager *editors, QObject *parent = nullptr);

    void setProjectRoot(const QString &root);
    void move(const QStringList &sources, const QString &targetDir);
    bool busy() const { return m_busy; }
    void undo(); // the last move, including the references it rewrote

private:
    struct Changed {
        QString path; // after the move
        QVector<MoveRefactor::TextEdit> backward;
        size_t afterHash = 0;
        int references = 0;
    };
    struct Batch {
        QVector<MoveRefactor::PathMove> moves;
        QVector<Changed> changed;
        QString targetDir;
    };

    void execute(const QVector<MoveRefactor::PathMove> &moves, const MoveRefactor::Plan &plan, const QString &targetDir);
    bool applyEdits(const QString &path, const QVector<MoveRefactor::TextEdit> &edits, size_t expectedHash, Changed *record, int references);
    QString where(const QString &dir) const;
    void fail(const QString &text);

    ProjectExplorer *m_explorer;
    EditorManager *m_editors;
    QString m_root;
    bool m_busy = false;
    bool m_hasLast = false;
    Batch m_last;
};
