#include "RefactorCtx.h"

#include <optional>

namespace MoveRefactor {

namespace {

struct Crate {
    QString dir, srcDir, name; // name: how other crates spell it (underscored)
};

struct ModInfo {
    int crate = -1;
    QStringList segs;
    bool valid() const { return crate >= 0; }
};

struct ModMove {
    int crate;
    QStringList from, to;
};

struct RustState {
    QVector<Crate> crates;
    QVector<ModMove> mods;
};

QVector<Crate> findCrates(Ctx &ctx)
{
    QVector<Crate> out;
    static const QRegularExpression nameRe(QStringLiteral("^\\[package\\][^\\[]*?^name[ \\t]*=[ \\t]*\"([^\"]+)\""), QRegularExpression::MultilineOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression libRe(QStringLiteral("^\\[lib\\][^\\[]*?^name[ \\t]*=[ \\t]*\"([^\"]+)\""), QRegularExpression::MultilineOption | QRegularExpression::DotMatchesEverythingOption);
    for (const QString &f : ctx.in.files) {
        if (Ctx::nameOf(f) != QLatin1String("Cargo.toml") || !ctx.readable(f))
            continue;
        const QString dir = Ctx::dirOf(f);
        if (!ctx.isFile(dir + QStringLiteral("/src/lib.rs")) && !ctx.isFile(dir + QStringLiteral("/src/main.rs")))
            continue;
        QString name;
        const QString &t = ctx.text(f);
        const QRegularExpressionMatch lm = libRe.match(t);
        const QRegularExpressionMatch pm = nameRe.match(t);
        name = lm.hasMatch() ? lm.captured(1) : (pm.hasMatch() ? pm.captured(1) : QString());
        name.replace(QLatin1Char('-'), QLatin1Char('_'));
        out.append({dir, dir + QStringLiteral("/src"), name});
    }
    return out;
}

ModInfo modInfo(const RustState &st, const QString &path)
{
    int best = -1;
    for (int i = 0; i < st.crates.size(); ++i)
        if (Ctx::under(path, st.crates.at(i).srcDir) && path != st.crates.at(i).srcDir && (best < 0 || st.crates.at(i).srcDir.size() > st.crates.at(best).srcDir.size()))
            best = i;
    if (best < 0 || !path.endsWith(QLatin1String(".rs")))
        return {};
    QString rel = Ctx::relative(st.crates.at(best).srcDir, path);
    rel.chop(3);
    QStringList segs = rel.split(QLatin1Char('/'));
    if (segs.first() == QLatin1String("bin"))
        return {};
    if (segs.size() == 1 && (segs.first() == QLatin1String("lib") || segs.first() == QLatin1String("main")))
        segs.clear();
    else if (segs.last() == QLatin1String("mod"))
        segs.removeLast();
    ModInfo mi;
    mi.crate = best;
    mi.segs = segs;
    return mi;
}

// Files that may declare module `segs` (the module whose children we look for).
QStringList moduleFiles(const RustState &st, int crate, const QStringList &segs)
{
    const QString src = st.crates.at(crate).srcDir;
    if (segs.isEmpty())
        return {src + QStringLiteral("/lib.rs"), src + QStringLiteral("/main.rs")};
    const QString base = src + QLatin1Char('/') + segs.join(QLatin1Char('/'));
    return {base + QStringLiteral("/mod.rs"), base + QStringLiteral(".rs")};
}

QString oldPathOfExisting(Ctx &ctx, const QString &candidate)
{
    const QString old = ctx.unmap(candidate);
    return ctx.isFile(old) ? old : QString();
}

int prefixLen(const QStringList &path, const QStringList &prefix)
{
    if (prefix.size() > path.size())
        return -1;
    for (int i = 0; i < prefix.size(); ++i)
        if (path.at(i) != prefix.at(i))
            return -1;
    return prefix.size();
}

// The new spelling of a path written in file `file` (segments as written), or nothing when it stays valid.
std::optional<QStringList> mapPath(Ctx &ctx, const RustState &st, const QString &file, const QStringList &segs)
{
    if (segs.size() < 1)
        return std::nullopt;
    const ModInfo gOld = modInfo(st, file), gNew = modInfo(st, ctx.map(file));
    if (!gOld.valid())
        return std::nullopt;
    int crate = gOld.crate;
    QStringList base;
    int consumed = 1;
    const QString lead = segs.first();
    enum Form { Crate, Self, Super, Extern } form;
    if (lead == QLatin1String("crate")) {
        form = Crate;
    } else if (lead == QLatin1String("self")) {
        form = Self;
        base = gOld.segs;
    } else if (lead == QLatin1String("super")) {
        form = Super;
        base = gOld.segs;
        // a file module "a/mod.rs" is module a, "a.rs" too: super is one level up either way
        int k = 0;
        while (k < segs.size() && segs.at(k) == QLatin1String("super")) {
            if (base.isEmpty())
                return std::nullopt;
            base.removeLast();
            ++k;
        }
        consumed = k;
    } else {
        form = Extern;
        crate = -1;
        for (int i = 0; i < st.crates.size(); ++i)
            if (st.crates.at(i).name == lead && i != gOld.crate)
                crate = i;
        if (crate < 0) {
            // 2018 style: the first segment is a module declared right here (use util::math;)
            const QStringList probe = gOld.segs + segs;
            bool local = false;
            for (const ModMove &mm : st.mods)
                local = local || (mm.crate == gOld.crate && prefixLen(probe, mm.from) > 0);
            if (!local)
                return std::nullopt;
            form = Self;
            crate = gOld.crate;
            base = gOld.segs;
            consumed = 0;
        }
    }
    const QStringList rest = segs.mid(consumed);
    const QStringList full = base + rest;
    // longest moved module that prefixes the path
    int bestLen = -1;
    const ModMove *best = nullptr;
    for (const ModMove &mm : st.mods)
        if (mm.crate == crate) {
            const int l = prefixLen(full, mm.from);
            if (l > bestLen) {
                bestLen = l;
                best = &mm;
            }
        }
    const bool parentMoved = form == Super && gNew.valid() && gNew.segs.mid(0, qMax(0, gNew.segs.size() - 1)) != gOld.segs.mid(0, qMax(0, gOld.segs.size() - 1));
    if (form == Self && bestLen <= 0)
        return std::nullopt; // still relative to the same file
    if (bestLen <= 0 && !parentMoved)
        return std::nullopt;
    if (bestLen < 0)
        bestLen = 0;
    const int k = qMax(0, bestLen - base.size());
    const QStringList covered = full.mid(0, base.size() + k);
    QStringList newCovered = covered;
    if (best && bestLen > 0)
        newCovered = best->to + covered.mid(best->from.size());
    QStringList out;
    out << (form == Extern ? lead : QStringLiteral("crate"));
    out += newCovered;
    out += segs.mid(consumed + k);
    if (out == segs)
        return std::nullopt;
    return out;
}

struct Leaf {
    QStringList segs;
    QString alias;
    bool glob = false;
};

// Recursive parse of a use tree starting at `pos`; fills leaves; returns the end offset or -1.
int parseUseTree(const QString &t, int pos, const QStringList &prefix, QVector<Leaf> *leaves)
{
    const int n = t.size();
    auto skipSpace = [&]() {
        while (pos < n && t.at(pos).isSpace())
            ++pos;
    };
    QStringList path = prefix;
    skipSpace();
    for (;;) {
        skipSpace();
        if (pos < n && t.at(pos) == QLatin1Char('{')) {
            ++pos;
            for (;;) {
                skipSpace();
                if (pos < n && t.at(pos) == QLatin1Char('}')) {
                    ++pos;
                    break;
                }
                pos = parseUseTree(t, pos, path, leaves);
                if (pos < 0)
                    return -1;
                skipSpace();
                if (pos < n && t.at(pos) == QLatin1Char(','))
                    ++pos;
            }
            return pos;
        }
        if (pos < n && t.at(pos) == QLatin1Char('*')) {
            ++pos;
            Leaf l;
            l.segs = path;
            l.glob = true;
            leaves->append(l);
            return pos;
        }
        const int s = pos;
        while (pos < n && (t.at(pos).isLetterOrNumber() || t.at(pos) == QLatin1Char('_')))
            ++pos;
        if (pos == s)
            return -1;
        const QString ident = t.mid(s, pos - s);
        skipSpace();
        if (pos + 1 < n && t.at(pos) == QLatin1Char(':') && t.at(pos + 1) == QLatin1Char(':')) {
            path << ident;
            pos += 2;
            continue;
        }
        Leaf l;
        l.segs = path;
        if (ident != QLatin1String("self") || path.isEmpty())
            l.segs << ident;
        // "as alias"
        if (t.mid(pos, 2) == QLatin1String("as") && pos + 2 < n && t.at(pos + 2).isSpace()) {
            pos += 2;
            skipSpace();
            const int a = pos;
            while (pos < n && (t.at(pos).isLetterOrNumber() || t.at(pos) == QLatin1Char('_')))
                ++pos;
            l.alias = t.mid(a, pos - a);
        }
        leaves->append(l);
        return pos;
    }
}

QString leafText(const Leaf &l)
{
    QString s = l.segs.join(QStringLiteral("::"));
    if (l.glob)
        s += QStringLiteral("::*");
    if (!l.alias.isEmpty())
        s += QStringLiteral(" as ") + l.alias;
    return s;
}

void scanRustFile(Ctx &ctx, const RustState &st, const QString &file)
{
    const QString &t = ctx.text(file);
    Emitter &em = ctx.emitter(file);
    const QString newFile = ctx.map(file);
    QVector<QPair<int, int>> useSpans;

    static const QRegularExpression useRe(QStringLiteral("^([ \\t]*)((?:pub(?:\\([^)]*\\))?[ \\t]+)?)use[ \\t]+"), QRegularExpression::MultilineOption);
    auto uit = useRe.globalMatch(t);
    while (uit.hasNext()) {
        const QRegularExpressionMatch m = uit.next();
        QVector<Leaf> leaves;
        int p = parseUseTree(t, m.capturedEnd(0), {}, &leaves);
        if (p < 0 || leaves.isEmpty())
            continue;
        while (p < t.size() && (t.at(p) == QLatin1Char(' ') || t.at(p) == QLatin1Char('\t')))
            ++p;
        if (p >= t.size() || t.at(p) != QLatin1Char(';'))
            continue;
        ++p;
        useSpans.append({m.capturedStart(0), p});
        bool changed = false;
        QStringList lines;
        for (const Leaf &l : std::as_const(leaves)) {
            Leaf nl = l;
            const auto mapped = mapPath(ctx, st, file, l.segs);
            if (mapped) {
                nl.segs = *mapped;
                changed = true;
            }
            lines << m.captured(2) + QStringLiteral("use ") + leafText(nl) + QLatin1Char(';');
        }
        if (!changed)
            continue;
        const int start = m.capturedStart(0) + m.captured(1).size();
        if (em.add(start, p - start, lines.join(QLatin1Char('\n') + m.captured(1))))
            ++em.references;
    }
    auto inUse = [&useSpans](int pos) {
        for (const auto &s : useSpans)
            if (pos >= s.first && pos < s.second)
                return true;
        return false;
    };

    // paths in code
    static const QRegularExpression chain(QStringLiteral("(?<![\\w:$])((?:crate|self|super|[a-z_][a-z0-9_]*)(?:::[A-Za-z_][A-Za-z0-9_]*)+)"));
    auto cit = chain.globalMatch(t);
    while (cit.hasNext()) {
        const QRegularExpressionMatch m = cit.next();
        if (inUse(m.capturedStart(1)))
            continue;
        const QStringList segs = m.captured(1).split(QStringLiteral("::"));
        const auto mapped = mapPath(ctx, st, file, segs);
        if (mapped && em.add(m.capturedStart(1), m.capturedLength(1), mapped->join(QStringLiteral("::"))))
            ++em.references;
    }

    // file paths: include_str!("../x"), #[path = "x.rs"]
    static const QRegularExpression pathRe(QStringLiteral("(?:\\binclude(?:_str|_bytes)?!\\s*\\(\\s*|#\\[path\\s*=\\s*)\"([^\"\\n]+)\""));
    auto pit = pathRe.globalMatch(t);
    while (pit.hasNext()) {
        const QRegularExpressionMatch m = pit.next();
        const QString spec = m.captured(1);
        const QString target = QDir::cleanPath(Ctx::dirOf(file) + QLatin1Char('/') + spec);
        if (!ctx.exists(target))
            continue;
        const QString nt = ctx.map(target);
        if (nt == target && newFile == file)
            continue;
        QString rel = Ctx::relative(Ctx::dirOf(newFile), nt);
        if (rel != spec && em.add(m.capturedStart(1), m.capturedLength(1), rel))
            ++em.references;
    }
}

void moveDeclarations(Ctx &ctx, const RustState &st)
{
    for (const ModMove &mm : st.mods) {
        if (mm.from.isEmpty() || mm.to.isEmpty())
            continue;
        const QString name = mm.from.last(), newName = mm.to.last();
        const QStringList parentOld = mm.from.mid(0, mm.from.size() - 1), parentNew = mm.to.mid(0, mm.to.size() - 1);
        if (parentOld == parentNew && name == newName)
            continue;
        // the file that declares it now
        QString declFile;
        QString declText;
        int declStart = 0, declLen = 0;
        const QRegularExpression decl(QStringLiteral("^(?:[ \\t]*#\\[[^\\n]*\\][ \\t]*\\r?\\n)*[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?mod[ \\t]+") + QRegularExpression::escape(name) + QStringLiteral("[ \\t]*;[ \\t]*(?:\\r?\\n|$)"),
                                       QRegularExpression::MultilineOption);
        for (const QString &c : moduleFiles(st, mm.crate, parentOld)) {
            const QString f = oldPathOfExisting(ctx, c);
            if (f.isEmpty() || !ctx.readable(f))
                continue;
            const QRegularExpressionMatch m = decl.match(ctx.text(f));
            if (m.hasMatch()) {
                declFile = f;
                declText = m.captured(0);
                declStart = m.capturedStart(0);
                declLen = m.capturedLength(0);
                break;
            }
        }
        if (declFile.isEmpty())
            continue;
        if (parentOld == parentNew) { // renamed within the same parent
            QString nt = declText;
            nt.replace(QRegularExpression(QStringLiteral("\\bmod[ \\t]+") + QRegularExpression::escape(name)), QStringLiteral("mod ") + newName);
            Emitter &e = ctx.emitter(declFile);
            if (e.add(declStart, declLen, nt))
                ++e.references;
            continue;
        }
        // the module that gets it
        QString target;
        for (const QString &c : moduleFiles(st, mm.crate, parentNew)) {
            const QString f = oldPathOfExisting(ctx, c);
            if (!f.isEmpty() && ctx.readable(f)) {
                target = f;
                break;
            }
        }
        if (target.isEmpty()) {
            ctx.note(QStringLiteral("Rust: module %1 now belongs to %2, which has no file to declare it in - add `mod %1;` yourself")
                         .arg(newName, parentNew.isEmpty() ? QStringLiteral("the crate root") : parentNew.join(QStringLiteral("::"))));
            continue;
        }
        Emitter &oe = ctx.emitter(declFile);
        if (!oe.add(declStart, declLen, QString()))
            continue;
        ++oe.references;
        QString nt = declText;
        if (newName != name)
            nt.replace(QRegularExpression(QStringLiteral("\\bmod[ \\t]+") + QRegularExpression::escape(name)), QStringLiteral("mod ") + newName);
        if (!nt.endsWith(QLatin1Char('\n')))
            nt += QLatin1Char('\n');
        const QString &tt = ctx.text(target);
        static const QRegularExpression anyMod(QStringLiteral("^[ \\t]*(?:pub(?:\\([^)]*\\))?[ \\t]+)?mod[ \\t]+\\w+[ \\t]*;[ \\t]*(?:\\r?\\n|$)"), QRegularExpression::MultilineOption);
        int at = -1;
        auto it = anyMod.globalMatch(tt);
        while (it.hasNext())
            at = it.next().capturedEnd(0);
        QString ins = nt;
        if (at < 0) {
            // after inner attributes and file docs
            at = 0;
            int pos = 0;
            while (pos < tt.size()) {
                int e = tt.indexOf(QLatin1Char('\n'), pos);
                const int lineEnd = e < 0 ? tt.size() : e + 1;
                const QString line = tt.mid(pos, lineEnd - pos).trimmed();
                if (line.startsWith(QLatin1String("//!")) || line.startsWith(QLatin1String("#![")) || line.isEmpty() || line.startsWith(QLatin1String("//"))) {
                    at = lineEnd;
                    pos = lineEnd;
                    if (e < 0)
                        break;
                } else {
                    break;
                }
            }
            ins += QLatin1Char('\n');
        } else if (at > 0 && tt.at(at - 1) != QLatin1Char('\n')) {
            ins.prepend(QLatin1Char('\n'));
        }
        Emitter &te = ctx.emitter(target);
        if (te.add(at, 0, ins))
            ++te.references;
    }
}

} // namespace

void scanRust(Ctx &ctx, const QStringList &files)
{
    if (!ctx.movedExts.contains(QStringLiteral("rs")))
        return;
    auto st = std::make_shared<RustState>();
    st->crates = findCrates(ctx);
    if (st->crates.isEmpty())
        return;
    for (const QString &f : ctx.movedFiles) {
        if (Ctx::suffixOf(f) != QLatin1String("rs"))
            continue;
        const ModInfo o = modInfo(*st, f), n = modInfo(*st, ctx.map(f));
        if (!o.valid())
            continue;
        if (!n.valid() || o.crate != n.crate) {
            ctx.note(QStringLiteral("%1 left its crate's src folder: module declarations and paths were not updated").arg(Ctx::nameOf(f)));
            continue;
        }
        if (o.segs != n.segs)
            st->mods.append({o.crate, o.segs, n.segs});
        // a module file with children folder that stays behind
        const QString stem = Ctx::stemOf(f);
        if (stem != QLatin1String("mod") && stem != QLatin1String("lib") && stem != QLatin1String("main")) {
            const QString childDir = Ctx::dirOf(f) + QLatin1Char('/') + stem;
            if (ctx.isDir(childDir) && ctx.map(childDir) == childDir)
                ctx.note(QStringLiteral("%1 has submodules in %2/ which did not move with it").arg(Ctx::nameOf(f), stem));
        }
    }
    moveDeclarations(ctx, *st);
    for (const QString &f : files)
        scanRustFile(ctx, *st, f);
}

} // namespace MoveRefactor
