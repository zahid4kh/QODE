#include "RefactorCtx.h"

namespace MoveRefactor {

namespace {

bool validIdent(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z_]\\w*$"));
    return re.match(s).hasMatch();
}

bool isPyExt(const QString &ext)
{
    return ext == QLatin1String("py") || ext == QLatin1String("pyi") || ext == QLatin1String("pyw");
}

// Directory above the outermost package that contains `path`.
QString pkgRoot(Ctx &ctx, const QString &path, bool newWorld)
{
    QString d = Ctx::dirOf(path);
    while (Ctx::under(d, ctx.root) && d != ctx.root) {
        const QString probe = (newWorld ? ctx.unmap(d) : d) + QStringLiteral("/__init__.py");
        if (!ctx.isFile(probe))
            break;
        d = Ctx::dirOf(d);
    }
    return d;
}

QStringList pyRoots(Ctx &ctx, const QString &file, bool newWorld)
{
    QStringList roots;
    auto add = [&roots](const QString &r) {
        if (!r.isEmpty() && !roots.contains(r))
            roots << r;
    };
    add(pkgRoot(ctx, file, newWorld));
    add(ctx.root);
    add(ctx.root + QStringLiteral("/src"));
    QString d = Ctx::dirOf(file);
    add(d);
    while (Ctx::under(d, ctx.root) && d != ctx.root) {
        d = Ctx::dirOf(d);
        add(d);
    }
    return roots;
}

struct PyMod {
    QString root;
    QString path; // module file, or the package folder
    bool isDir = false;
    int consumed = 0;
};

// Resolves the longest prefix of `parts`; with `full` only the whole list counts.
bool resolvePy(Ctx &ctx, const QStringList &parts, const QStringList &roots, bool full, PyMod *out)
{
    for (const QString &root : roots) {
        QString p = root;
        PyMod best;
        for (int i = 0; i < parts.size(); ++i) {
            const QString next = p + QLatin1Char('/') + parts.at(i);
            PyMod here;
            here.root = root;
            here.consumed = i + 1;
            if (ctx.isDir(next) && ctx.isFile(next + QStringLiteral("/__init__.py"))) {
                here.path = next;
                here.isDir = true;
            } else if (ctx.isFile(next + QStringLiteral(".py"))) {
                here.path = next + QStringLiteral(".py");
            } else if (ctx.isFile(next + QStringLiteral(".pyi"))) {
                here.path = next + QStringLiteral(".pyi");
            } else if (ctx.isDir(next)) {
                here.path = next;
                here.isDir = true;
            } else {
                break;
            }
            best = here;
            if (!here.isDir)
                break; // a module file has no submodules
            p = next;
        }
        if (best.consumed > 0 && (!full || best.consumed == parts.size())) {
            *out = best;
            return true;
        }
    }
    return false;
}

QString stripPyExt(const QString &path, bool isDir)
{
    if (isDir)
        return path;
    const QString ext = Ctx::suffixOf(path);
    return ext.isEmpty() ? path : path.left(path.size() - ext.size() - 1);
}

// "pkg.sub.mod" for a module file / package folder, preferring `preferred` roots; empty when no root fits.
QString dottedFor(const QString &newPath, bool isDir, const QStringList &roots)
{
    const QString base = stripPyExt(newPath, isDir);
    for (const QString &root : roots) {
        if (!Ctx::under(base, root) || base == root)
            continue;
        const QStringList parts = Ctx::relative(root, base).split(QLatin1Char('/'));
        bool ok = true;
        for (const QString &p : parts)
            ok = ok && validIdent(p);
        if (ok)
            return parts.join(QLatin1Char('.'));
    }
    return {};
}

// Relative form (".", "..x.y") from the module at `newFile`, or empty when it would leave the project.
QString relativeFor(Ctx &ctx, const QString &newFile, const QString &newPath, bool isDir)
{
    const QString target = stripPyExt(newPath, isDir);
    QString cur = Ctx::dirOf(newFile);
    int up = 0;
    while (!Ctx::under(target, cur)) {
        if (cur == ctx.root || !Ctx::under(cur, ctx.root))
            return {};
        cur = Ctx::dirOf(cur);
        ++up;
    }
    if (!Ctx::under(cur, ctx.root))
        return {};
    QString rest = target == cur ? QString() : Ctx::relative(cur, target);
    for (const QString &p : rest.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        if (!validIdent(p))
            return {};
    rest.replace(QLatin1Char('/'), QLatin1Char('.'));
    return QString(up + 1, QLatin1Char('.')) + rest;
}

struct Name {
    QString name, alias;
};

// Parses the names after "import" starting at `pos`; returns the end offset (after a closing paren / last name).
int parseNames(const QString &t, int pos, QVector<Name> *names, bool *paren)
{
    const int n = t.size();
    while (pos < n && (t.at(pos) == QLatin1Char(' ') || t.at(pos) == QLatin1Char('\t')))
        ++pos;
    *paren = pos < n && t.at(pos) == QLatin1Char('(');
    int end;
    QString body;
    if (*paren) {
        end = t.indexOf(QLatin1Char(')'), pos);
        if (end < 0)
            return -1;
        body = t.mid(pos + 1, end - pos - 1);
        ++end;
    } else {
        end = pos;
        while (end < n && t.at(end) != QLatin1Char('\n')) {
            if (t.at(end) == QLatin1Char('\\') && end + 1 < n && (t.at(end + 1) == QLatin1Char('\n') || t.at(end + 1) == QLatin1Char('\r'))) {
                end += (t.at(end + 1) == QLatin1Char('\r') && end + 2 < n) ? 3 : 2;
                continue;
            }
            if (t.at(end) == QLatin1Char('#') || t.at(end) == QLatin1Char(';'))
                break;
            ++end;
        }
        body = t.mid(pos, end - pos);
        while (end > pos && t.at(end - 1).isSpace())
            --end;
        body = t.mid(pos, end - pos);
    }
    static const QRegularExpression comment(QStringLiteral("#[^\\n]*"));
    body.remove(comment);
    body.remove(QLatin1Char('\\'));
    static const QRegularExpression item(QStringLiteral("^\\s*([A-Za-z_]\\w*|\\*)(?:\\s+as\\s+([A-Za-z_]\\w*))?\\s*$"));
    for (const QString &part : body.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (part.trimmed().isEmpty())
            continue;
        const QRegularExpressionMatch m = item.match(part);
        if (!m.hasMatch())
            return -1;
        names->append({m.captured(1), m.captured(2)});
    }
    return end;
}

QString nameText(const Name &n)
{
    return n.alias.isEmpty() ? n.name : n.name + QStringLiteral(" as ") + n.alias;
}

struct DottedMove {
    QString oldName, newName;
};

QVector<DottedMove> movedModules(Ctx &ctx)
{
    QVector<DottedMove> out;
    for (const PathMove &m : ctx.moves()) {
        const bool dir = QFileInfo(m.from).isDir();
        if (!dir && !isPyExt(Ctx::suffixOf(m.from)))
            continue;
        if (dir) {
            bool any = false;
            for (const QString &f : ctx.movedFiles)
                any = any || (Ctx::under(f, m.from) && isPyExt(Ctx::suffixOf(f)));
            if (!any)
                continue;
        }
        const QString oldRoot = pkgRoot(ctx, m.from + (dir ? QStringLiteral("/x") : QString()), false);
        QStringList newRoots = {oldRoot, pkgRoot(ctx, m.to + (dir ? QStringLiteral("/x") : QString()), true), ctx.root, ctx.root + QStringLiteral("/src")};
        const QString o = dottedFor(m.from, dir, {oldRoot});
        const QString n = dottedFor(m.to, dir, newRoots);
        if (!o.isEmpty() && !n.isEmpty() && o != n)
            out.append({o, n});
    }
    return out;
}

void edit(Ctx &ctx, const QString &file, int start, int length, const QString &text)
{
    Emitter &e = ctx.emitter(file);
    if (e.add(start, length, text))
        ++e.references;
}

void scanPyFile(Ctx &ctx, const QString &file, const QVector<DottedMove> &mods)
{
    const QString &t = ctx.text(file);
    const QString newFile = ctx.map(file);
    const QStringList oldRoots = pyRoots(ctx, file, false);
    const QStringList newRoots = pyRoots(ctx, newFile, true);
    const bool isPy = isPyExt(Ctx::suffixOf(file));
    if (!isPy) {
        for (const DottedMove &dm : mods) {
            if (!dm.oldName.contains(QLatin1Char('.')))
                continue;
            const QRegularExpression re(QStringLiteral("(?<![\\w.])") + QRegularExpression::escape(dm.oldName) + QStringLiteral("(?![\\w])"));
            auto cit = re.globalMatch(t);
            while (cit.hasNext()) {
                const QRegularExpressionMatch cm = cit.next();
                edit(ctx, file, cm.capturedStart(0), cm.capturedLength(0), dm.newName);
            }
        }
        return;
    }

    // --- from X import a, b ---
    static const QRegularExpression fromRe(QStringLiteral("^([ \\t]*)from[ \\t]+(\\.*)([A-Za-z_][\\w.]*)?[ \\t]+import(?=[\\s(*])"), QRegularExpression::MultilineOption);
    auto it = fromRe.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString indent = m.captured(1);
        const int dots = m.captured(2).size();
        const QString modText = m.captured(3);
        const QStringList modParts = modText.isEmpty() ? QStringList() : modText.split(QLatin1Char('.'));
        QVector<Name> names;
        bool paren = false;
        const int end = parseNames(t, m.capturedEnd(0), &names, &paren);
        if (end < 0 || names.isEmpty() || (names.size() == 1 && names.first().name == QLatin1String("*") && false))
            continue;

        // The module the statement names.
        PyMod mod;
        bool resolved = false;
        if (dots > 0) {
            QString pkg = Ctx::dirOf(file);
            for (int i = 1; i < dots; ++i)
                pkg = Ctx::dirOf(pkg);
            if (modParts.isEmpty()) {
                mod.root = pkg;
                mod.path = pkg;
                mod.isDir = true;
                resolved = true;
            } else {
                resolved = resolvePy(ctx, modParts, {pkg}, true, &mod);
            }
        } else {
            resolved = resolvePy(ctx, modParts, oldRoots, true, &mod);
        }
        if (!resolved)
            continue;

        QVector<QString> exprs;
        QVector<bool> plainImport;
        const QString modNewPath = ctx.map(mod.path);
        for (const Name &n : std::as_const(names)) {
            QString sub;
            bool subDir = false;
            if (mod.isDir && n.name != QLatin1String("*")) {
                const QString base = mod.path + QLatin1Char('/') + n.name;
                if (ctx.isDir(base) && ctx.isFile(base + QStringLiteral("/__init__.py"))) {
                    sub = base;
                    subDir = true;
                } else if (ctx.isFile(base + QStringLiteral(".py"))) {
                    sub = base + QStringLiteral(".py");
                } else if (ctx.isFile(base + QStringLiteral(".pyi"))) {
                    sub = base + QStringLiteral(".pyi");
                } else if (ctx.isDir(base)) {
                    sub = base;
                    subDir = true;
                }
            }
            QString holderPath;
            bool holderIsDir;
            if (!sub.isEmpty()) {
                holderPath = Ctx::dirOf(ctx.map(sub)); // the package the name now lives in
                holderIsDir = true;
            } else {
                holderPath = modNewPath;
                holderIsDir = mod.isDir;
            }
            Q_UNUSED(subDir)
            QString expr;
            bool plain = false;
            if (dots > 0) {
                expr = relativeFor(ctx, newFile, holderPath, holderIsDir);
                if (expr.isEmpty())
                    expr = dottedFor(holderPath, holderIsDir, QStringList() << mod.root << newRoots);
            } else {
                QStringList prefer = QStringList() << mod.root << newRoots;
                expr = dottedFor(holderPath, holderIsDir, prefer);
                if (expr.isEmpty() && holderIsDir) {
                    for (const QString &r : std::as_const(prefer))
                        if (holderPath == r)
                            plain = true;
                }
            }
            if (expr.isEmpty() && !plain)
                expr = QString(); // cannot express: leave this name alone (keeps the statement)
            exprs.append(expr);
            plainImport.append(plain);
        }
        // Names that cannot be expressed keep the statement as it is.
        bool unusable = false;
        for (int i = 0; i < names.size(); ++i)
            unusable = unusable || (exprs.at(i).isEmpty() && !plainImport.at(i));
        if (unusable)
            continue;

        const QString written = m.captured(2) + modText;
        bool same = true;
        for (int i = 0; i < names.size(); ++i)
            same = same && !plainImport.at(i) && exprs.at(i) == written;
        if (same)
            continue;

        bool oneGroup = true;
        for (int i = 1; i < names.size(); ++i)
            oneGroup = oneGroup && exprs.at(i) == exprs.at(0) && plainImport.at(i) == plainImport.at(0);
        if (oneGroup && !plainImport.at(0)) {
            edit(ctx, file, m.capturedStart(2), m.captured(2).size() + modText.size(), exprs.at(0));
            continue;
        }
        // Names go to different places now: one statement per destination.
        QStringList order;
        QHash<QString, QStringList> byExpr;
        for (int i = 0; i < names.size(); ++i) {
            const QString key = plainImport.at(i) ? QStringLiteral("\x01") : exprs.at(i);
            if (!byExpr.contains(key))
                order << key;
            byExpr[key] << nameText(names.at(i));
        }
        QStringList lines;
        for (const QString &key : std::as_const(order)) {
            if (key == QLatin1String("\x01"))
                for (const QString &n : byExpr.value(key))
                    lines << QStringLiteral("import ") + n;
            else
                lines << QStringLiteral("from ") + key + QStringLiteral(" import ") + byExpr.value(key).join(QStringLiteral(", "));
        }
        const int start = m.capturedStart(0) + indent.size();
        edit(ctx, file, start, end - start, lines.join(QLatin1Char('\n') + indent));
    }

    // --- import a.b.c [as x], d ---
    static const QRegularExpression impRe(QStringLiteral("^([ \\t]*)import[ \\t]+((?:[A-Za-z_][\\w.]*)(?:[ \\t]+as[ \\t]+\\w+)?(?:[ \\t]*,[ \\t]*[A-Za-z_][\\w.]*(?:[ \\t]+as[ \\t]+\\w+)?)*)"),
                                          QRegularExpression::MultilineOption);
    auto it2 = impRe.globalMatch(t);
    while (it2.hasNext()) {
        const QRegularExpressionMatch m = it2.next();
        const int listStart = m.capturedStart(2);
        static const QRegularExpression partRe(QStringLiteral("([A-Za-z_][\\w.]*)(?:[ \\t]+as[ \\t]+(\\w+))?"));
        auto pit = partRe.globalMatch(m.captured(2));
        while (pit.hasNext()) {
            const QRegularExpressionMatch pm = pit.next();
            const QString dotted = pm.captured(1);
            const QStringList parts = dotted.split(QLatin1Char('.'));
            PyMod mod;
            if (!resolvePy(ctx, parts, oldRoots, false, &mod))
                continue;
            const QString newPath = ctx.map(mod.path);
            if (newPath == mod.path)
                continue; // the target stays put: an absolute import does not depend on this file's place
            QString expr = dottedFor(newPath, mod.isDir, QStringList() << mod.root << newRoots);
            if (expr.isEmpty())
                continue;
            const QStringList rest = parts.mid(mod.consumed);
            if (!rest.isEmpty())
                expr += QLatin1Char('.') + rest.join(QLatin1Char('.'));
            if (expr == dotted)
                continue;
            const int start = listStart + pm.capturedStart(1);
            edit(ctx, file, start, dotted.size(), expr);
            if (pm.captured(2).isEmpty()) {
                // "import a.b.c" binds "a": the code spells the path out, so spell the new path out too.
                const QRegularExpression use(QStringLiteral("(?<![\\w.])") + QRegularExpression::escape(dotted) + QStringLiteral("(?![\\w])"));
                auto uit = use.globalMatch(t);
                while (uit.hasNext()) {
                    const QRegularExpressionMatch um = uit.next();
                    if (um.capturedStart(0) == start)
                        continue;
                    ctx.emitter(file).add(um.capturedStart(0), um.capturedLength(0), expr);
                }
            }
        }
    }

    // --- "pkg.mod.attr" strings (mock.patch, importlib, settings) ---
    {
        static const QRegularExpression strRe(QStringLiteral("(['\"])([A-Za-z_]\\w*(?:\\.[A-Za-z_]\\w*)+)\\1"));
        auto sit = strRe.globalMatch(t);
        while (sit.hasNext()) {
            const QRegularExpressionMatch sm = sit.next();
            const QString dotted = sm.captured(2);
            const QStringList parts = dotted.split(QLatin1Char('.'));
            PyMod mod;
            if (!resolvePy(ctx, parts, oldRoots, false, &mod))
                continue;
            const QString newPath = ctx.map(mod.path);
            if (newPath == mod.path)
                continue;
            QString expr = dottedFor(newPath, mod.isDir, QStringList() << mod.root << newRoots);
            if (expr.isEmpty())
                continue;
            const QStringList rest = parts.mid(mod.consumed);
            if (!rest.isEmpty())
                expr += QLatin1Char('.') + rest.join(QLatin1Char('.'));
            if (expr != dotted)
                edit(ctx, file, sm.capturedStart(2), dotted.size(), expr);
        }
    }
}

} // namespace

void scanPython(Ctx &ctx, const QStringList &files)
{
    bool relevant = false;
    for (const QString &ext : std::as_const(ctx.movedExts))
        relevant = relevant || isPyExt(ext);
    // A Python file that moves has relative imports to fix even when nothing it imports moves.
    if (!relevant)
        return;
    const QVector<DottedMove> mods = movedModules(ctx);
    for (const QString &f : files)
        scanPyFile(ctx, f, mods);
}

} // namespace MoveRefactor
