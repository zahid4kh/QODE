#include "AgentJournal.h"

#include "git/GitDiff.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

namespace agent {

bool copyTree(const QString &from, const QString &to)
{
    const QFileInfo fi(from);
    if (fi.isSymLink() || fi.isFile())
        return QFile::copy(from, to);
    if (!QDir().mkpath(to))
        return false;
    for (const QFileInfo &e : QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden))
        if (!copyTree(e.absoluteFilePath(), to + QLatin1Char('/') + e.fileName()))
            return false;
    return true;
}

// --- persistence ----------------------------------------------------------------------------------------------------

QJsonObject Op::toJson() const
{
    static const char *names[] = {"text", "move", "trash", "copy"};
    QJsonObject o{{"kind", QString::fromLatin1(names[kind])}, {"seq", seq}, {"turn", turn}, {"path", path}};
    if (!from.isEmpty())
        o.insert("from", from);
    if (kind == Text) {
        o.insert("existedBefore", existedBefore);
        o.insert("existsAfter", existsAfter);
        o.insert("before", before);
        o.insert("after", after);
    }
    if (!trash.isEmpty())
        o.insert("trash", trash);
    return o;
}

Op Op::fromJson(const QJsonObject &o)
{
    Op op;
    const QString k = o.value("kind").toString();
    op.kind = k == QLatin1String("move") ? Move : k == QLatin1String("trash") ? Trash : k == QLatin1String("copy") ? Copy : Text;
    op.seq = o.value("seq").toInt();
    op.turn = o.value("turn").toInt();
    op.path = o.value("path").toString();
    op.from = o.value("from").toString();
    op.existedBefore = o.value("existedBefore").toBool();
    op.existsAfter = o.value("existsAfter").toBool();
    op.before = o.value("before").toString();
    op.after = o.value("after").toString();
    op.trash = o.value("trash").toString();
    return op;
}

void Journal::clear()
{
    m_ops.clear();
    m_redo.clear();
    m_nextSeq = 1;
}

QJsonObject Journal::toJson() const
{
    QJsonArray ops;
    for (const auto &op : m_ops)
        ops.append(op->toJson());
    return {{"next", m_nextSeq}, {"ops", ops}};
}

void Journal::load(const QJsonObject &o)
{
    clear();
    for (const QJsonValue &v : o.value("ops").toArray())
        m_ops.append(std::make_shared<Op>(Op::fromJson(v.toObject())));
    std::sort(m_ops.begin(), m_ops.end(), [](const auto &a, const auto &b) { return a->seq < b->seq; });
    m_nextSeq = qMax(o.value("next").toInt(1), m_ops.isEmpty() ? 1 : m_ops.last()->seq + 1);
}

// --- recording ------------------------------------------------------------------------------------------------------

void Journal::add(const Op &op)
{
    m_redo.clear();
    Op o = op;
    o.seq = m_nextSeq++;
    m_ops.append(std::make_shared<Op>(o));
}

void Journal::recordText(int turn, const QString &rel, bool existedBefore, const QString &before, bool existsAfter, const QString &after)
{
    // Edits of one file in a row belong together: keep the first "before" and the latest "after".
    for (int i = m_ops.size() - 1; i >= 0; --i) {
        Op &last = *m_ops.at(i);
        if (last.turn != turn || !touches(last, rel))
            continue;
        if (last.kind == Op::Text && last.path == rel && last.existsAfter) {
            m_redo.clear();
            last.after = after;
            last.existsAfter = existsAfter;
            return;
        }
        break;
    }
    Op op;
    op.kind = Op::Text;
    op.turn = turn;
    op.path = rel;
    op.existedBefore = existedBefore;
    op.before = before;
    op.existsAfter = existsAfter;
    op.after = after;
    add(op);
}

void Journal::recordMove(int turn, const QString &from, const QString &to)
{
    Op op;
    op.kind = Op::Move;
    op.turn = turn;
    op.from = from;
    op.path = to;
    add(op);
}

void Journal::recordTrash(int turn, const QString &rel, const QString &trashPath)
{
    Op op;
    op.kind = Op::Trash;
    op.turn = turn;
    op.path = rel;
    op.trash = trashPath;
    add(op);
}

void Journal::recordCopy(int turn, const QString &from, const QString &to)
{
    Op op;
    op.kind = Op::Copy;
    op.turn = turn;
    op.from = from;
    op.path = to;
    add(op);
}

// --- queries --------------------------------------------------------------------------------------------------------

int Journal::lastTurn() const
{
    return m_ops.isEmpty() ? -1 : m_ops.last()->turn;
}

QString Journal::label(const Op &op)
{
    switch (op.kind) {
    case Op::Text:
        return op.existedBefore ? (op.existsAfter ? QStringLiteral("edit of %1") : QStringLiteral("deletion of %1")).arg(op.path)
                                : QStringLiteral("creation of %1").arg(op.path);
    case Op::Move:
        return QStringLiteral("move %1 -> %2").arg(op.from, op.path);
    case Op::Trash:
        return QStringLiteral("deletion of %1").arg(op.path);
    case Op::Copy:
        return QStringLiteral("copy %1 -> %2").arg(op.from, op.path);
    }
    return op.path;
}

static Journal::Entry entryOf(const Op &op)
{
    Journal::Entry e;
    e.turn = op.turn;
    e.path = op.path;
    switch (op.kind) {
    case Op::Text: {
        e.what = !op.existedBefore ? QStringLiteral("created") : op.existsAfter ? QStringLiteral("edited") : QStringLiteral("deleted");
        const QStringList a = GitDiff::splitLines(op.before), b = GitDiff::splitLines(op.after);
        for (const GitDiff::Hunk &h : GitDiff::compute(op.existedBefore ? a : QStringList(), op.existsAfter ? b : QStringList())) {
            e.removed += h.oldCount;
            e.added += h.newCount;
        }
        break;
    }
    case Op::Move:
        e.what = QStringLiteral("moved from %1").arg(op.from);
        break;
    case Op::Trash:
        e.what = QStringLiteral("deleted");
        break;
    case Op::Copy:
        e.what = QStringLiteral("copied from %1").arg(op.from);
        break;
    }
    return e;
}

QVector<Journal::Entry> Journal::entries(int turn) const
{
    const int t = turn == Last ? lastTurn() : turn;
    QVector<Entry> out;
    for (const auto &op : m_ops)
        if (t == All || op->turn == t)
            out.append(entryOf(*op));
    return out;
}

bool Journal::canRedo(int turn) const
{
    for (const RedoGroup &g : m_redo)
        for (const RedoItem &it : g.items)
            if (turn == All || it.op->turn == turn)
                return true;
    return false;
}

QVector<Journal::Entry> Journal::redoEntries() const
{
    QVector<Entry> out;
    if (m_redo.isEmpty())
        return out;
    for (const RedoItem &it : m_redo.last().items)
        out.prepend(entryOf(*it.op));
    return out;
}

// --- undo / redo ----------------------------------------------------------------------------------------------------

bool Journal::textIs(const QString &rel, bool exists, const QString &text) const
{
    QString cur;
    const bool there = m_io.read && m_io.read(abs(rel), &cur);
    return exists ? (there && cur == text) : !there;
}

bool Journal::revertOp(Op &op, bool force, QString *why)
{
    switch (op.kind) {
    case Op::Text:
        if (!force && !textIs(op.path, op.existsAfter, op.after)) {
            *why = QStringLiteral("it was changed after the agent left it");
            return false;
        }
        if (op.existedBefore ? !m_io.write(abs(op.path), op.before) : (QFileInfo::exists(abs(op.path)) && !m_io.remove(abs(op.path)))) {
            *why = QStringLiteral("the file could not be written");
            return false;
        }
        return true;
    case Op::Move:
        if (!QFileInfo::exists(abs(op.path))) {
            *why = QStringLiteral("%1 no longer exists").arg(op.path);
            return false;
        }
        if (QFileInfo::exists(abs(op.from))) {
            *why = QStringLiteral("%1 exists again").arg(op.from);
            return false;
        }
        if (!m_io.rename(abs(op.path), abs(op.from))) {
            *why = QStringLiteral("it could not be moved back");
            return false;
        }
        return true;
    case Op::Trash:
        if (QFileInfo::exists(abs(op.path))) {
            *why = QStringLiteral("%1 exists again").arg(op.path);
            return false;
        }
        if (op.trash.isEmpty() || !QFileInfo::exists(op.trash)) {
            *why = QStringLiteral("it is no longer in the trash");
            return false;
        }
        if (!m_io.rename(op.trash, abs(op.path))) {
            *why = QStringLiteral("it could not be restored from the trash");
            return false;
        }
        return true;
    case Op::Copy:
        if (!QFileInfo::exists(abs(op.path)))
            return true; // already gone
        if (!m_io.trash(abs(op.path), &op.trash)) {
            *why = QStringLiteral("the copy could not be removed");
            return false;
        }
        return true;
    }
    return false;
}

bool Journal::applyOp(Op &op, bool force, QString *why)
{
    switch (op.kind) {
    case Op::Text:
        if (!force && !textIs(op.path, op.existedBefore, op.before)) {
            *why = QStringLiteral("it was changed in the meantime");
            return false;
        }
        if (op.existsAfter ? !m_io.write(abs(op.path), op.after) : (QFileInfo::exists(abs(op.path)) && !m_io.remove(abs(op.path)))) {
            *why = QStringLiteral("the file could not be written");
            return false;
        }
        return true;
    case Op::Move:
        if (!QFileInfo::exists(abs(op.from)) || QFileInfo::exists(abs(op.path))) {
            *why = QStringLiteral("the files are not where they were");
            return false;
        }
        if (!m_io.rename(abs(op.from), abs(op.path))) {
            *why = QStringLiteral("it could not be moved");
            return false;
        }
        return true;
    case Op::Trash:
        if (!QFileInfo::exists(abs(op.path))) {
            *why = QStringLiteral("%1 does not exist").arg(op.path);
            return false;
        }
        if (!m_io.trash(abs(op.path), &op.trash)) {
            *why = QStringLiteral("it could not be moved to the trash");
            return false;
        }
        return true;
    case Op::Copy:
        if (QFileInfo::exists(abs(op.path))) {
            *why = QStringLiteral("%1 exists").arg(op.path);
            return false;
        }
        if (!op.trash.isEmpty() && QFileInfo::exists(op.trash)) {
            if (m_io.rename(op.trash, abs(op.path)))
                return true;
        }
        if (!QFileInfo::exists(abs(op.from)) || !copyTree(abs(op.from), abs(op.path))) {
            *why = QStringLiteral("it could not be copied");
            return false;
        }
        return true;
    }
    return false;
}

Journal::Result Journal::undo(const Scope &scope)
{
    Result r;
    const int turn = scope.turn == Last ? lastTurn() : scope.turn;
    if (m_ops.isEmpty() || turn == -1) {
        r.error = QStringLiteral("the agent has not changed anything in this chat (or it was undone already)");
        return r;
    }
    if (scope.startLine > 0) {
        if (scope.path.isEmpty()) {
            r.error = QStringLiteral("a line range needs a path");
            return r;
        }
        return undoLines(scope, turn);
    }
    QVector<std::shared_ptr<Op>> selected;
    for (int i = m_ops.size() - 1; i >= 0; --i) {
        const auto &op = m_ops.at(i);
        if ((turn == All || op->turn == turn) && (scope.path.isEmpty() || touches(*op, scope.path)))
            selected.append(op);
    }
    if (selected.isEmpty()) {
        r.error = scope.path.isEmpty() ? QStringLiteral("nothing to undo in that turn")
                                       : QStringLiteral("the agent made no recorded change to %1").arg(scope.path);
        return r;
    }
    RedoGroup group;
    for (const auto &op : selected) {
        QString why;
        if (revertOp(*op, scope.force, &why)) {
            r.done << label(*op);
            group.items.append({op, false, nullptr, QString()});
            m_ops.removeOne(op);
        } else {
            r.conflicts << QStringLiteral("%1: %2").arg(label(*op), why);
            r.conflictPaths << op->path;
        }
    }
    if (!group.items.isEmpty())
        m_redo.append(group);
    return r;
}

// Reverts only the hunks of one file that touch the given lines, back to how the file was before the agent's first
// change (within the scope). The agent's own record of the file stays valid: its "after" follows the new text.
Journal::Result Journal::undoLines(const Scope &scope, int turn)
{
    Result r;
    std::shared_ptr<Op> first, last;
    for (const auto &op : m_ops) {
        if ((turn != All && op->turn != turn) || !touches(*op, scope.path))
            continue;
        if (!first)
            first = op;
        last = op;
    }
    if (!first) {
        r.error = QStringLiteral("the agent made no recorded change to %1").arg(scope.path);
        return r;
    }
    if (first->kind != Op::Text || last->kind != Op::Text || !first->existedBefore || !last->existsAfter || first->path != scope.path) {
        r.error = QStringLiteral("%1 was created, moved or deleted by the agent; undo the whole file instead").arg(scope.path);
        return r;
    }
    QString current;
    if (!m_io.read(abs(scope.path), &current)) {
        r.error = QStringLiteral("%1 does not exist").arg(scope.path);
        return r;
    }
    if (!scope.force && current != last->after) {
        r.conflicts << QStringLiteral("%1: it was changed after the agent left it").arg(scope.path);
        r.conflictPaths << scope.path;
        return r;
    }
    const QStringList oldLines = GitDiff::splitLines(first->before), newLines = GitDiff::splitLines(current);
    const int L = scope.startLine, R = qMax(scope.endLine, scope.startLine);
    QStringList out;
    int pos = 0, reverted = 0;
    for (const GitDiff::Hunk &h : GitDiff::compute(oldLines, newLines)) {
        const bool hit = h.newCount == 0 ? (h.newStart >= L - 1 && h.newStart <= R) : (h.newStart + 1 <= R && h.newStart + h.newCount >= L);
        out += newLines.mid(pos, h.newStart - pos);
        if (hit) {
            out += oldLines.mid(h.oldStart, h.oldCount);
            ++reverted;
        } else {
            out += newLines.mid(h.newStart, h.newCount);
        }
        pos = h.newStart + h.newCount;
    }
    out += newLines.mid(pos);
    if (reverted == 0) {
        r.error = QStringLiteral("none of the agent's changes in %1 touch lines %2-%3").arg(scope.path).arg(L).arg(R);
        return r;
    }
    const QString text = out.join(QLatin1Char('\n'));
    if (!m_io.write(abs(scope.path), text)) {
        r.error = QStringLiteral("%1 could not be written").arg(scope.path);
        return r;
    }
    auto step = std::make_shared<Op>();
    step->kind = Op::Text;
    step->path = scope.path;
    step->existedBefore = step->existsAfter = true;
    step->before = current;
    step->after = text;
    RedoGroup group;
    group.items.append({step, true, last, last->after});
    last->after = text;
    m_redo.append(group);
    r.done << QStringLiteral("%1 hunk(s) of %2").arg(reverted).arg(scope.path);
    return r;
}

Journal::Result Journal::redo(const QString &path, bool force)
{
    Result r;
    for (int g = m_redo.size() - 1; g >= 0; --g) {
        RedoGroup &group = m_redo[g];
        QVector<int> pick;
        for (int i = group.items.size() - 1; i >= 0; --i) // the undo went newest first, so redo goes oldest first
            if (path.isEmpty() || touches(*group.items.at(i).op, path))
                pick.append(i);
        if (pick.isEmpty())
            continue;
        QVector<int> done;
        for (int i : pick) {
            RedoItem &it = group.items[i];
            QString why;
            const bool ok = it.viaRevert ? revertOp(*it.op, force, &why) : applyOp(*it.op, force, &why);
            if (!ok) {
                r.conflicts << QStringLiteral("%1: %2").arg(label(*it.op), why);
                r.conflictPaths << it.op->path;
                continue;
            }
            r.done << label(*it.op);
            if (it.viaRevert) {
                it.patched->after = it.patchedOldAfter;
            } else {
                auto pos = std::upper_bound(m_ops.begin(), m_ops.end(), it.op, [](const auto &a, const auto &b) { return a->seq < b->seq; });
                m_ops.insert(pos, it.op);
            }
            done.append(i);
        }
        std::sort(done.begin(), done.end(), std::greater<int>());
        for (int i : done)
            group.items.removeAt(i);
        if (group.items.isEmpty())
            m_redo.removeAt(g);
        return r;
    }
    r.error = path.isEmpty() ? QStringLiteral("there is nothing to redo") : QStringLiteral("there is nothing to redo for %1").arg(path);
    return r;
}

} // namespace agent
