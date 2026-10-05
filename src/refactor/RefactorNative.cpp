#include "RefactorCtx.h"

namespace MoveRefactor {

namespace {

QString joinClean(const QString &dir, const QString &rel)
{
    return QDir::cleanPath(dir + QLatin1Char('/') + rel);
}

void addRef(Ctx &ctx, const QString &file, int start, int length, const QString &text)
{
    Emitter &e = ctx.emitter(file);
    if (e.add(start, length, text))
        ++e.references;
}

// --- C / C++ ---------------------------------------------------------------

QStringList includeBases(Ctx &ctx, const QString &file, bool newWorld, bool quote)
{
    QStringList l;
    auto add = [&l](const QString &b) {
        if (!l.contains(b))
            l << b;
    };
    auto dirExists = [&](const QString &d) { return ctx.isDir(newWorld ? ctx.unmap(d) : d); };
    const QString dir = Ctx::dirOf(file);
    if (quote)
        add(dir);
    for (QString d = dir; Ctx::under(d, ctx.root); d = Ctx::dirOf(d)) {
        add(d);
        for (const QString &sub : {QStringLiteral("/include"), QStringLiteral("/src"), QStringLiteral("/inc")})
            if (dirExists(d + sub))
                add(d + sub);
        if (d == ctx.root)
            break;
    }
    return l;
}

void scanInclude(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    const QString newFile = ctx.map(file);
    static const QRegularExpression re(QStringLiteral("^[ \\t]*#[ \\t]*(?:include|import)[ \\t]*([<\"])([^>\"\\n]+)[>\"]"), QRegularExpression::MultilineOption);
    auto it = re.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const bool quote = m.captured(1) == QLatin1String("\"");
        const QString spec = m.captured(2);
        QString base, target;
        for (const QString &b : includeBases(ctx, file, false, quote)) {
            const QString cand = joinClean(b, spec);
            if (ctx.isFile(cand)) {
                base = b;
                target = cand;
                break;
            }
        }
        if (target.isEmpty())
            continue;
        const QString newTarget = ctx.map(target);
        if (newTarget == target && newFile == file)
            continue;
        QString spec2;
        if (quote && base == Ctx::dirOf(file)) {
            spec2 = Ctx::relative(Ctx::dirOf(newFile), newTarget);
        } else {
            const QStringList bases = includeBases(ctx, newFile, true, quote);
            if (bases.contains(base) && Ctx::under(newTarget, base) && newTarget != base)
                spec2 = Ctx::relative(base, newTarget);
            for (int i = 0; spec2.isEmpty() && i < bases.size(); ++i)
                if (Ctx::under(newTarget, bases.at(i)) && newTarget != bases.at(i) && (!quote || bases.at(i) != Ctx::dirOf(newFile)))
                    spec2 = Ctx::relative(bases.at(i), newTarget);
            if (spec2.isEmpty() && quote)
                spec2 = Ctx::relative(Ctx::dirOf(newFile), newTarget);
            if (spec2.isEmpty()) {
                ctx.note(QStringLiteral("%1: #include <%2> no longer resolves from its include directories").arg(Ctx::nameOf(file), spec));
                continue;
            }
        }
        if (spec2 != spec)
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), spec2);
    }
}

// --- build files: CMake, qmake, Make, Meson, Cargo, qrc, ui ---------------------

struct Var {
    const char *text;
    bool root; // relative to the project root instead of the file's folder
};

bool rewriteToken(Ctx &ctx, const QString &file, const QString &token, bool allowBare, QString *out)
{
    static const Var vars[] = {{"${CMAKE_CURRENT_SOURCE_DIR}/", false}, {"${CMAKE_CURRENT_LIST_DIR}/", false}, {"${CMAKE_SOURCE_DIR}/", true},
                               {"${PROJECT_SOURCE_DIR}/", true},        {"$$PWD/", false},                     {"$${PWD}/", false},
                               {"$$_PRO_FILE_PWD_/", false},            {"$(CURDIR)/", false},                 {"${CURDIR}/", false}};
    QString prefix, rest = token;
    bool rootBase = false;
    for (const Var &v : vars) {
        if (token.startsWith(QLatin1String(v.text))) {
            prefix = QLatin1String(v.text);
            rest = token.mid(prefix.size());
            rootBase = v.root;
            break;
        }
    }
    if (rest.isEmpty() || rest.contains(QLatin1Char('$')) || rest.contains(QLatin1Char('{')) || rest.contains(QLatin1Char('*')) || rest.startsWith(QLatin1Char('-'))
        || rest.startsWith(QLatin1Char('/')) || rest.startsWith(QLatin1Char('~')) || rest.startsWith(QLatin1Char('@')))
        return false;
    if (!allowBare && !rest.contains(QLatin1Char('/')) && !rest.contains(QLatin1Char('.')))
        return false;
    const QString base = rootBase ? ctx.root : Ctx::dirOf(file);
    const QString target = joinClean(base, rest);
    if (!ctx.exists(target))
        return false;
    const QString newBase = rootBase ? ctx.root : Ctx::dirOf(ctx.map(file));
    const QString newTarget = ctx.map(target);
    if (newTarget == target && newBase == base)
        return false;
    QString rel = Ctx::relative(newBase, newTarget);
    if (rel.isEmpty())
        rel = QStringLiteral(".");
    if (rest.startsWith(QLatin1String("./")) && !rel.startsWith(QLatin1String("../")) && rel != QLatin1String("."))
        rel = QStringLiteral("./") + rel;
    if (rest.endsWith(QLatin1Char('/')) && !rel.endsWith(QLatin1Char('/')))
        rel += QLatin1Char('/');
    *out = prefix + rel;
    return *out != token;
}

// All path-like tokens of text[from, to).
void rewriteTokens(Ctx &ctx, const QString &file, int from, int to, bool allowBare)
{
    const QString &t = ctx.text(file);
    static const QRegularExpression tok(QStringLiteral("[^\\s\"'\\\\;,()<>=:]+"));
    auto it = tok.globalMatch(t, from);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedEnd(0) > to)
            break;
        QString out;
        if (rewriteToken(ctx, file, m.captured(0), allowBare, &out))
            addRef(ctx, file, m.capturedStart(0), m.capturedLength(0), out);
    }
}

void scanCMake(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    static const QRegularExpression cmd(QStringLiteral("\\b([A-Za-z_][A-Za-z0-9_]*)[ \\t]*\\(([^)]*)\\)"));
    static const QSet<QString> dirCommands = {QStringLiteral("add_subdirectory"), QStringLiteral("include_directories"), QStringLiteral("link_directories"),
                                               QStringLiteral("target_include_directories")};
    auto it = cmd.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        rewriteTokens(ctx, file, m.capturedStart(2), m.capturedEnd(2), dirCommands.contains(m.captured(1).toLower()));
    }
}

void scanQmake(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    static const QRegularExpression stmt(QStringLiteral("^[ \\t]*([A-Za-z_][\\w.]*)[ \\t]*[+*~-]?=((?:[^\\n\\\\]|\\\\[ \\t]*\\r?\\n|\\\\.)*)"), QRegularExpression::MultilineOption);
    static const QSet<QString> pathVars = {QStringLiteral("SOURCES"),   QStringLiteral("HEADERS"),     QStringLiteral("FORMS"),     QStringLiteral("RESOURCES"),
                                           QStringLiteral("INCLUDEPATH"), QStringLiteral("DEPENDPATH"), QStringLiteral("VPATH"),    QStringLiteral("SUBDIRS"),
                                           QStringLiteral("OTHER_FILES"), QStringLiteral("DISTFILES"),  QStringLiteral("TRANSLATIONS"), QStringLiteral("PRECOMPILED_HEADER"),
                                           QStringLiteral("LEXSOURCES"),  QStringLiteral("YACCSOURCES"), QStringLiteral("ICON"),   QStringLiteral("RC_FILE")};
    auto it = stmt.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString var = m.captured(1);
        if (pathVars.contains(var) || var.endsWith(QLatin1String(".files")) || var.endsWith(QLatin1String(".subdir")))
            rewriteTokens(ctx, file, m.capturedStart(2), m.capturedEnd(2), true);
    }
    static const QRegularExpression inc(QStringLiteral("\\b(?:include|load)\\(([^)]*)\\)"));
    auto it2 = inc.globalMatch(t);
    while (it2.hasNext()) {
        const QRegularExpressionMatch m = it2.next();
        rewriteTokens(ctx, file, m.capturedStart(1), m.capturedEnd(1), false);
    }
}

void scanMake(Ctx &ctx, const QString &file)
{
    rewriteTokens(ctx, file, 0, ctx.text(file).size(), false);
}

void scanQrcUi(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    static const QRegularExpression re(QStringLiteral("<(?:file|include)(?:\\s[^>]*)?>([^<]+)</(?:file|include)>|<include\\s+location=\"([^\"]+)\"\\s*/>"));
    auto it = re.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int g = m.capturedStart(1) >= 0 ? 1 : 2;
        QString out;
        const QString tok = m.captured(g).trimmed();
        const int lead = m.captured(g).indexOf(tok);
        if (rewriteToken(ctx, file, tok, false, &out))
            addRef(ctx, file, m.capturedStart(g) + lead, tok.size(), out);
    }
}

void scanMeson(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    static const QRegularExpression re(QStringLiteral("(['\"])([^'\"\\n]+)\\1"));
    auto it = re.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QString out;
        // 'subdir(...)' takes a bare folder name
        const bool subdir = t.mid(qMax(0, m.capturedStart(0) - 8), 8).contains(QLatin1String("subdir("));
        if (rewriteToken(ctx, file, m.captured(2), subdir, &out))
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), out);
    }
}

void scanCargo(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    static const QRegularExpression re(QStringLiteral("(?:\\bpath\\s*=\\s*\"([^\"\\n]+)\")|(?:\\b(?:members|exclude)\\s*=\\s*\\[([^\\]]*)\\])"));
    auto it = re.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart(1) >= 0) {
            QString out;
            if (rewriteToken(ctx, file, m.captured(1), true, &out))
                addRef(ctx, file, m.capturedStart(1), m.capturedLength(1), out);
        } else {
            static const QRegularExpression str(QStringLiteral("\"([^\"\\n]+)\""));
            auto sit = str.globalMatch(m.captured(2));
            while (sit.hasNext()) {
                const QRegularExpressionMatch sm = sit.next();
                QString out;
                if (rewriteToken(ctx, file, sm.captured(1), true, &out))
                    addRef(ctx, file, m.capturedStart(2) + sm.capturedStart(1), sm.capturedLength(1), out);
            }
        }
    }
}

// --- QML ---------------------------------------------------------------------

void scanQml(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    const QString newFile = ctx.map(file);
    static const QRegularExpression re(QStringLiteral("(?:^[ \\t]*import[ \\t]+|\\b(?:source|sourceComponent|iconSource|icon\\.source|fontSource)[ \\t]*:[ \\t]*|resolvedUrl\\([ \\t]*)\"([^\"\\n:]+)\""),
                                       QRegularExpression::MultilineOption);
    auto it = re.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString spec = m.captured(1);
        if (spec.startsWith(QLatin1Char('/')) || spec.contains(QLatin1Char('$')))
            continue;
        const QString target = joinClean(Ctx::dirOf(file), spec);
        if (!ctx.exists(target))
            continue;
        const QString newTarget = ctx.map(target);
        if (newTarget == target && newFile == file)
            continue;
        QString rel = Ctx::relative(Ctx::dirOf(newFile), newTarget);
        if (rel.isEmpty())
            rel = QStringLiteral(".");
        if (spec.startsWith(QLatin1String("./")) && !rel.startsWith(QLatin1String("../")) && rel != QLatin1String("."))
            rel = QStringLiteral("./") + rel;
        if (rel != spec)
            addRef(ctx, file, m.capturedStart(1), m.capturedLength(1), rel);
    }
}

// Types of a QML folder are visible to each other without imports; moving one out needs an import.
void scanQmlTypes(Ctx &ctx, const QStringList &qmlFiles)
{
    struct Mv {
        QString file, stem;
    };
    QVector<Mv> moves;
    for (const QString &f : ctx.movedFiles)
        if (Ctx::suffixOf(f) == QLatin1String("qml") && Ctx::stemOf(f).at(0).isUpper())
            moves.append({f, Ctx::stemOf(f)});
    if (moves.isEmpty())
        return;
    // every QML type of the project by folder
    QHash<QString, QStringList> byDir;
    for (const QString &f : ctx.in.files)
        if (Ctx::suffixOf(f) == QLatin1String("qml") && Ctx::stemOf(f).at(0).isUpper())
            byDir[Ctx::dirOf(f)] << f;
    auto importLine = [&ctx](const QString &fromDir, const QString &toDir) {
        QString rel = Ctx::relative(fromDir, toDir);
        if (rel.isEmpty())
            rel = QStringLiteral(".");
        if (!rel.startsWith(QLatin1Char('.')))
            rel = QStringLiteral("./") + rel;
        return QStringLiteral("import \"%1\"").arg(rel);
    };
    QHash<QString, QSet<QString>> wanted; // file -> import lines to add
    for (const QString &g : ctx.in.files) {
        if (Ctx::suffixOf(g) != QLatin1String("qml") || !ctx.readable(g))
            continue;
        const QString &t = ctx.text(g);
        const QString newG = ctx.map(g);
        const QString oldDirG = Ctx::dirOf(g), newDirG = Ctx::dirOf(newG);
        // types of the old folder that G used implicitly
        for (const QString &other : byDir.value(oldDirG)) {
            if (other == g)
                continue;
            const QString stem = Ctx::stemOf(other);
            const QString newOther = ctx.map(other);
            if (Ctx::dirOf(newOther) == newDirG)
                continue; // still together
            const QRegularExpression use(QStringLiteral("\\b") + QRegularExpression::escape(stem) + QStringLiteral("\\s*\\{"));
            if (use.match(t).hasMatch())
                wanted[g].insert(importLine(newDirG, Ctx::dirOf(newOther)));
        }
        // types that used to be beside G and are not any more are handled above; types now beside G need nothing
    }
    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
        const QString &g = it.key();
        const QString &t = ctx.text(g);
        // skip imports the file already has (compared by the folder they name)
        QStringList lines;
        for (const QString &l : it.value()) {
            if (!t.contains(l))
                lines << l;
        }
        if (lines.isEmpty())
            continue;
        lines.sort();
        static const QRegularExpression imp(QStringLiteral("^[ \\t]*import[ \\t][^\\n]*\\n"), QRegularExpression::MultilineOption);
        int at = 0;
        auto m = imp.globalMatch(t);
        while (m.hasNext())
            at = m.next().capturedEnd(0);
        Emitter &em = ctx.emitter(g);
        if (em.add(at, 0, lines.join(QLatin1Char('\n')) + QLatin1Char('\n')))
            em.references += lines.size();
    }
    Q_UNUSED(qmlFiles)
}

// qmldir lines: "Button 1.0 Button.qml"
void scanQmldir(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    static const QRegularExpression re(QStringLiteral("^(?:(?:singleton|internal)[ \\t]+)?\\w+[ \\t]+[\\d.]+[ \\t]+(\\S+)"), QRegularExpression::MultilineOption);
    auto it = re.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QString out;
        if (rewriteToken(ctx, file, m.captured(1), true, &out))
            addRef(ctx, file, m.capturedStart(1), m.capturedLength(1), out);
    }
}

// --- Go ----------------------------------------------------------------------

struct GoMod {
    QString dir, module;
};

bool goModFor(Ctx &ctx, const QString &file, GoMod *out)
{
    for (QString d = Ctx::dirOf(file); Ctx::under(d, ctx.root); d = Ctx::dirOf(d)) {
        const QString mod = d + QStringLiteral("/go.mod");
        if (ctx.isFile(mod) && ctx.readable(mod)) {
            static const QRegularExpression re(QStringLiteral("^module[ \\t]+(\\S+)"), QRegularExpression::MultilineOption);
            const QRegularExpressionMatch m = re.match(ctx.text(mod));
            if (!m.hasMatch())
                return false;
            out->dir = d;
            out->module = m.captured(1).remove(QLatin1Char('"'));
            return true;
        }
        if (d == ctx.root)
            break;
    }
    return false;
}

QString goPackageName(Ctx &ctx, const QString &dir, const QString &skipFile)
{
    static const QRegularExpression re(QStringLiteral("^package[ \\t]+(\\w+)"), QRegularExpression::MultilineOption);
    for (const QString &f : ctx.in.files) {
        if (Ctx::dirOf(f) != dir || Ctx::suffixOf(f) != QLatin1String("go") || f == skipFile || ctx.moved(f) || f.endsWith(QLatin1String("_test.go")))
            continue;
        if (!ctx.readable(f))
            continue;
        const QRegularExpressionMatch m = re.match(ctx.text(f));
        if (m.hasMatch())
            return m.captured(1);
    }
    return {};
}

void scanGo(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    const QString newFile = ctx.map(file);
    // package clause of a file that changes folders
    // A package that moves as a whole keeps its name; only a file that changes packages takes the name of its new one.
    bool packageMoves = true;
    for (const QString &f : ctx.in.files)
        if (Ctx::dirOf(f) == Ctx::dirOf(file) && Ctx::suffixOf(f) == QLatin1String("go") && Ctx::dirOf(ctx.map(f)) != Ctx::dirOf(ctx.map(file)))
            packageMoves = false;
    if (newFile != file && Ctx::dirOf(newFile) != Ctx::dirOf(file) && !packageMoves) {
        static const QRegularExpression pk(QStringLiteral("^package[ \\t]+(\\w+)"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = pk.match(t);
        if (m.hasMatch()) {
            const QString newDir = Ctx::dirOf(newFile);
            QString name = goPackageName(ctx, ctx.unmap(newDir), file);
            if (name.isEmpty()) {
                name = Ctx::nameOf(newDir);
                name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
                if (name.isEmpty() || name.at(0).isDigit())
                    name = QStringLiteral("main");
            }
            const bool test = file.endsWith(QLatin1String("_test.go")) && m.captured(1).endsWith(QLatin1String("_test"));
            if (test)
                name += QStringLiteral("_test");
            if (name != m.captured(1)) {
                addRef(ctx, file, m.capturedStart(1), m.capturedLength(1), name);
                ctx.note(QStringLiteral("%1 changed package; code that referred to its declarations through the old package name needs updating").arg(Ctx::nameOf(file)));
            }
        }
    }
    GoMod gm;
    if (!goModFor(ctx, file, &gm))
        return;
    // import paths of packages whose folder moved
    static const QRegularExpression block(QStringLiteral("\\bimport\\s*\\(([^)]*)\\)"));
    static const QRegularExpression single(QStringLiteral("^[ \\t]*import[ \\t]+(?:[\\w.]+[ \\t]+)?\"([^\"\\n]+)\""), QRegularExpression::MultilineOption);
    static const QRegularExpression str(QStringLiteral("\"([^\"\\n]+)\""));
    auto fix = [&](int offset, const QString &spec) {
        if (!spec.startsWith(gm.module))
            return;
        const QString tail = spec.mid(gm.module.size());
        if (!tail.isEmpty() && !tail.startsWith(QLatin1Char('/')))
            return;
        const QString dir = QDir::cleanPath(gm.dir + tail);
        if (!ctx.isDir(dir))
            return;
        const QString nd = ctx.map(dir);
        if (nd == dir || !Ctx::under(nd, gm.dir))
            return;
        const QString rel = nd == gm.dir ? QString() : QLatin1Char('/') + Ctx::relative(gm.dir, nd);
        const QString spec2 = gm.module + rel;
        if (spec2 != spec)
            addRef(ctx, file, offset, spec.size(), spec2);
    };
    auto it = block.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        auto sit = str.globalMatch(m.captured(1));
        while (sit.hasNext()) {
            const QRegularExpressionMatch sm = sit.next();
            fix(m.capturedStart(1) + sm.capturedStart(1), sm.captured(1));
        }
    }
    auto it2 = single.globalMatch(t);
    while (it2.hasNext()) {
        const QRegularExpressionMatch m = it2.next();
        fix(m.capturedStart(1), m.captured(1));
    }
}

} // namespace

void scanNative(Ctx &ctx, const QStringList &files)
{
    QStringList qml;
    for (const QString &f : files) {
        const QString ext = Ctx::suffixOf(f);
        const QString name = Ctx::nameOf(f).toLower();
        if (ext == QLatin1String("c") || ext == QLatin1String("cc") || ext == QLatin1String("cpp") || ext == QLatin1String("cxx") || ext == QLatin1String("c++")
            || ext == QLatin1String("h") || ext == QLatin1String("hh") || ext == QLatin1String("hpp") || ext == QLatin1String("hxx") || ext == QLatin1String("inl")
            || ext == QLatin1String("ipp") || ext == QLatin1String("tpp") || ext == QLatin1String("ino"))
            scanInclude(ctx, f);
        else if (ext == QLatin1String("qml")) {
            scanQml(ctx, f);
            qml << f;
        } else if (ext == QLatin1String("go"))
            scanGo(ctx, f);
        else if (ext == QLatin1String("pro") || ext == QLatin1String("pri"))
            scanQmake(ctx, f);
        else if (ext == QLatin1String("cmake") || name == QLatin1String("cmakelists.txt"))
            scanCMake(ctx, f);
        else if (ext == QLatin1String("mk") || name == QLatin1String("makefile"))
            scanMake(ctx, f);
        else if (ext == QLatin1String("qrc") || ext == QLatin1String("ui"))
            scanQrcUi(ctx, f);
        else if (name == QLatin1String("meson.build"))
            scanMeson(ctx, f);
        else if (name == QLatin1String("cargo.toml"))
            scanCargo(ctx, f);
        else if (name == QLatin1String("qmldir"))
            scanQmldir(ctx, f);
    }
    scanQmlTypes(ctx, qml);
    QStringList rust;
    for (const QString &f : files)
        if (Ctx::suffixOf(f) == QLatin1String("rs"))
            rust << f;
    scanRust(ctx, rust);
}

} // namespace MoveRefactor
