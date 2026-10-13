#pragma once

#include "ai/AiEdits.h"

#include <QString>
#include <QVector>

class EditorManager;

// The AI agent's view of the editor: reads files (open buffers first) and applies its edits as ordinary, undoable,
// unsaved edits in the editor tabs - nothing is written to disk except new files.
class AiWorkspace
{
public:
    AiWorkspace(EditorManager *editors, const QString &root)
        : m_editors(editors)
        , m_root(root)
    {
    }

    // Text of a file for the model; false for missing, binary or very large files.
    bool readFile(const QString &absolute, QString *text) const;
    QVector<AiEdits::Outcome> apply(const QVector<AiEdits::Edit> &edits) const;

private:
    EditorManager *m_editors;
    QString m_root;
};
