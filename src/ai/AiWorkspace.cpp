#include "AiWorkspace.h"

#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "lsp/LspTypes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QTextBlock>
#include <QTextDocument>
#include <algorithm>

namespace {
constexpr qint64 kMaxFileBytes = 1024 * 1024;
constexpr int kEndOfLine = 1 << 28; // column past the end: the editor clamps it
}

bool AiWorkspace::readFile(const QString &absolute, QString *text) const
{
    if (Document *doc = m_editors->documentForPath(absolute)) {
        *text = doc->text();
        return true;
    }
    QFile f(absolute);
    if (!QFileInfo(f).isFile() || f.size() > kMaxFileBytes || !f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.readAll();
    if (data.left(8000).contains('\0'))
        return false;
    *text = QString::fromUtf8(data);
    return true;
}

QVector<AiEdits::Outcome> AiWorkspace::apply(const QVector<AiEdits::Edit> &edits) const
{
    using AiEdits::Edit;
    QVector<AiEdits::Outcome> outcomes(edits.size());
    for (int i = 0; i < edits.size(); ++i)
        outcomes[i].edit = edits.at(i);

    auto fail = [&](int i, const QString &why) {
        outcomes[i].ok = false;
        outcomes[i].note = why;
    };

    // Indexes of the edits per file, in the order the model gave them.
    QMap<QString, QVector<int>> perFile;
    for (int i = 0; i < edits.size(); ++i) {
        QString abs;
        if (!AiEdits::resolveInside(m_root, edits.at(i).path, &abs)) {
            fail(i, QStringLiteral("the path is outside the project"));
            continue;
        }
        perFile[abs].append(i);
    }

    for (auto it = perFile.begin(); it != perFile.end(); ++it) {
        const QString abs = it.key();
        const QVector<int> idx = it.value();
        const bool exists = QFileInfo(abs).isFile();

        // --- a new file ---------------------------------------------------------------------------------------
        if (edits.at(idx.first()).op == Edit::Create) {
            if (exists) {
                for (int i : idx)
                    fail(i, QStringLiteral("the file already exists; use replace or insert"));
                continue;
            }
            QDir().mkpath(QFileInfo(abs).absolutePath());
            QFile f(abs);
            QString code = edits.at(idx.first()).code;
            if (!code.isEmpty() && !code.endsWith(QLatin1Char('\n')))
                code += QLatin1Char('\n');
            if (!f.open(QIODevice::WriteOnly) || f.write(code.toUtf8()) < 0) {
                for (int i : idx)
                    fail(i, QStringLiteral("the file could not be written"));
                continue;
            }
            f.close();
            m_editors->openFile(abs);
            outcomes[idx.first()].ok = true;
            outcomes[idx.first()].note = QStringLiteral("created (%1 lines)").arg(code.count(QLatin1Char('\n')));
            for (int k = 1; k < idx.size(); ++k)
                fail(idx.at(k), QStringLiteral("only one create per file is allowed"));
            continue;
        }

        // --- edits of an existing file --------------------------------------------------------------------------
        if (!exists) {
            for (int i : idx)
                fail(i, QStringLiteral("the file does not exist"));
            continue;
        }
        if (!m_editors->openFile(abs)) {
            for (int i : idx)
                fail(i, QStringLiteral("the file could not be opened"));
            continue;
        }
        Document *doc = m_editors->documentForPath(abs);
        CodeEditor *editor = doc ? m_editors->editorFor(doc) : nullptr;
        if (!editor || editor->isReadOnly()) {
            for (int i : idx)
                fail(i, QStringLiteral("the file is not editable"));
            continue;
        }

        QTextDocument *td = editor->document();
        const int blocks = td->blockCount();
        const bool endsWithNewline = td->findBlockByNumber(blocks - 1).length() <= 1;
        const int lines = endsWithNewline ? blocks - 1 : blocks; // lines as the model counted them

        // Validate ranges, then reject overlaps (the earlier edit wins).
        QVector<int> accepted;
        for (int i : idx) {
            const Edit &e = edits.at(i);
            if (e.op == Edit::Create) {
                fail(i, QStringLiteral("create used for an existing file"));
            } else if (e.op == Edit::Replace && (e.startLine < 1 || e.endLine < e.startLine || e.endLine > lines)) {
                fail(i, QStringLiteral("line range %1-%2 is outside the file (%3 lines)").arg(e.startLine).arg(e.endLine).arg(lines));
            } else if (e.op == Edit::Insert && (e.startLine < 1 || e.startLine > lines + 1)) {
                fail(i, QStringLiteral("insert position %1 is outside the file (%2 lines)").arg(e.startLine).arg(lines));
            } else {
                accepted.append(i);
            }
        }
        QVector<int> usable;
        for (int i : accepted) {
            const Edit &e = edits.at(i);
            bool clash = false;
            for (int j : usable) {
                const Edit &o = edits.at(j);
                const int as = e.startLine, ae = e.op == Edit::Insert ? e.startLine - 1 : e.endLine;
                const int bs = o.startLine, be = o.op == Edit::Insert ? o.startLine - 1 : o.endLine;
                if (e.op == Edit::Insert && o.op == Edit::Insert)
                    continue; // inserts at one spot keep their order
                if (e.op == Edit::Insert) // inside (bs, be], not at its start
                    clash = clash || (as > bs && as <= be);
                else if (o.op == Edit::Insert)
                    clash = clash || (bs > as && bs <= ae);
                else
                    clash = clash || (as <= be && bs <= ae);
            }
            if (clash)
                fail(i, QStringLiteral("overlaps another edit of this file"));
            else
                usable.append(i);
        }
        if (usable.isEmpty())
            continue;

        QVector<LspTextEdit> list;
        for (int i : usable) {
            const Edit &e = edits.at(i);
            LspTextEdit t;
            QString code = e.code;
            if (e.op == Edit::Replace) {
                t.startLine = e.startLine - 1;
                t.startColumn = 0;
                if (e.endLine < blocks) {
                    t.endLine = e.endLine;
                    t.endColumn = 0;
                    t.text = code.isEmpty() ? QString() : (code.endsWith(QLatin1Char('\n')) ? code : code + QLatin1Char('\n'));
                } else { // runs to the very end of the document, which has no newline after it
                    t.endLine = blocks - 1;
                    t.endColumn = kEndOfLine;
                    if (code.endsWith(QLatin1Char('\n')))
                        code.chop(1);
                    t.text = code;
                    if (code.isEmpty() && e.startLine > 1) { // also drop the line break in front of the deleted tail
                        t.startLine = e.startLine - 2;
                        t.startColumn = kEndOfLine;
                    }
                }
            } else if (endsWithNewline || e.startLine <= lines) { // in front of an existing block
                t.startLine = t.endLine = e.startLine - 1;
                t.text = code.endsWith(QLatin1Char('\n')) ? code : code + QLatin1Char('\n');
            } else { // append behind a last line without newline
                t.startLine = t.endLine = blocks - 1;
                t.startColumn = t.endColumn = kEndOfLine;
                t.text = QLatin1Char('\n') + (code.endsWith(QLatin1Char('\n')) ? code.chopped(1) : code);
            }
            list.append(t);
        }

        QHash<int, QString> beforeText;
        for (int i : usable) {
            const Edit &e = edits.at(i);
            if (e.op != Edit::Replace)
                continue;
            QStringList old;
            for (int n = e.startLine - 1; n < e.endLine; ++n)
                old << td->findBlockByNumber(n).text();
            beforeText.insert(i, old.join(QLatin1Char('\n')));
        }

        if (!editor->applyTextEdits(list)) {
            for (int i : usable)
                fail(i, QStringLiteral("the editor refused the edit"));
            continue;
        }
        for (int i : usable) {
            const Edit &e = edits.at(i);
            outcomes[i].ok = true;
            const int added = e.code.isEmpty() ? 0 : e.code.count(QLatin1Char('\n')) + (e.code.endsWith(QLatin1Char('\n')) ? 0 : 1);
            if (e.op == Edit::Insert) {
                outcomes[i].note = QStringLiteral("inserted %1 line(s)").arg(added);
            } else {
                const int removed = e.endLine - e.startLine + 1;
                outcomes[i].note = added == 0 ? QStringLiteral("deleted %1 line(s)").arg(removed)
                                              : QStringLiteral("replaced %1 line(s) with %2").arg(removed).arg(added);
                outcomes[i].before = beforeText.value(i);
            }
        }
    }
    return outcomes;
}
