#include "RefactorCtx.h"

#include <QSet>

namespace MoveRefactor {

namespace {

bool isJvmSource(const QString &ext)
{
    return ext == QLatin1String("java") || ext == QLatin1String("kt");
}

struct JFile {
    QString path;
    QString pkg;       // as declared
    QString srcRoot;   // folder the package starts at (conventional layout only)
    bool conventional = false;
    bool kotlin = false;
    QStringList names; // top-level declarations (plus <File>Kt for Kotlin)
};

struct JvmState {
    QHash<QString, JFile> files;          // by path
    QHash<QString, QString> nameToFile;   // "pkg.Name" -> path
    QHash<QString, QStringList> pkgFiles; // package -> files
    QStringList roots;
    QHash<QString, QString> newPkg;       // file -> package after the move
};

// Comments and string contents blanked (same length), except Kotlin ${...} expressions.
QString maskJvm(const QString &t, bool kotlin)
{
    QString out = t;
    const int n = t.size();
    int i = 0;
    auto blank = [&out](int from, int to) {
        for (int k = from; k < to; ++k)
            if (out.at(k) != QLatin1Char('\n'))
                out[k] = QLatin1Char(' ');
    };
    while (i < n) {
        const QChar c = t.at(i);
        if (c == QLatin1Char('/') && i + 1 < n && t.at(i + 1) == QLatin1Char('/')) {
            int e = t.indexOf(QLatin1Char('\n'), i);
            if (e < 0)
                e = n;
            blank(i, e);
            i = e;
        } else if (c == QLatin1Char('/') && i + 1 < n && t.at(i + 1) == QLatin1Char('*')) {
            int e = t.indexOf(QLatin1String("*/"), i + 2);
            e = e < 0 ? n : e + 2;
            blank(i, e);
            i = e;
        } else if (c == QLatin1Char('"')) {
            const bool triple = t.mid(i, 3) == QLatin1String("\"\"\"");
            int j = i + (triple ? 3 : 1);
            const int contentStart = j;
            int depth = 0;
            while (j < n) {
                const QChar d = t.at(j);
                if (kotlin && depth == 0 && d == QLatin1Char('$') && j + 1 < n && t.at(j + 1) == QLatin1Char('{')) {
                    depth = 1;
                    j += 2;
                    continue;
                }
                if (depth > 0) {
                    if (d == QLatin1Char('{'))
                        ++depth;
                    else if (d == QLatin1Char('}'))
                        --depth;
                    ++j;
                    continue;
                }
                if (!triple && d == QLatin1Char('\\')) {
                    j += 2;
                    continue;
                }
                if (triple ? t.mid(j, 3) == QLatin1String("\"\"\"") : d == QLatin1Char('"'))
                    break;
                if (!triple && d == QLatin1Char('\n'))
                    break;
                ++j;
            }
            // blank literal text outside ${...}
            int k = contentStart, d2 = 0;
            while (k < j) {
                if (kotlin && d2 == 0 && t.at(k) == QLatin1Char('$') && k + 1 < j && t.at(k + 1) == QLatin1Char('{')) {
                    d2 = 1;
                    k += 2;
                    continue;
                }
                if (d2 > 0) {
                    if (t.at(k) == QLatin1Char('{'))
                        ++d2;
                    else if (t.at(k) == QLatin1Char('}'))
                        --d2;
                    ++k;
                    continue;
                }
                if (out.at(k) != QLatin1Char('\n'))
                    out[k] = QLatin1Char(' ');
                ++k;
            }
            i = qMin(n, j + (triple ? 3 : 1));
        } else if (c == QLatin1Char('\'')) {
            int j = i + 1;
            while (j < n && t.at(j) != QLatin1Char('\'') && t.at(j) != QLatin1Char('\n')) {
                if (t.at(j) == QLatin1Char('\\'))
                    ++j;
                ++j;
            }
            blank(i + 1, qMin(j, n));
            i = qMin(n, j + 1);
        } else {
            ++i;
        }
    }
    return out;
}

void readFileInfo(Ctx &ctx, const QString &path, JFile *jf)
{
    const QString &raw = ctx.text(path);
    jf->path = path;
    jf->kotlin = Ctx::suffixOf(path) == QLatin1String("kt");
    const QString t = maskJvm(raw, jf->kotlin);
    static const QRegularExpression pkgRe(QStringLiteral("^[ \\t]*package[ \\t]+([\\w.]+)"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch pm = pkgRe.match(t);
    jf->pkg = pm.hasMatch() ? pm.captured(1) : QString();
    const QString dir = Ctx::dirOf(path);
    const QString pkgPath = QString(jf->pkg).replace(QLatin1Char('.'), QLatin1Char('/'));
    if (pkgPath.isEmpty()) {
        jf->conventional = true;
        jf->srcRoot = dir;
    } else if (dir.endsWith(QLatin1Char('/') + pkgPath)) {
        jf->conventional = true;
        jf->srcRoot = dir.left(dir.size() - pkgPath.size() - 1);
    }
    static const QRegularExpression javaTop(QStringLiteral(
        "^(?:@\\w+(?:\\([^)]*\\))?\\s+)*(?:(?:public|protected|private|abstract|final|static|sealed|non-sealed|strictfp)\\s+)*(?:class|interface|enum|record|@interface)\\s+(\\w+)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression ktTop(QStringLiteral(
        "^(?:@\\w+(?:\\([^)]*\\))?\\s+)*(?:(?:public|private|internal|protected|abstract|open|final|sealed|data|enum|annotation|inner|inline|value|const|lateinit|suspend|operator|infix|tailrec|external|expect|actual)\\s+)*(?:fun\\s+interface|class|interface|object|typealias|fun|val|var)\\s+(?:<[^>]*>\\s+)?(?:[\\w.<>?]+\\.)?(\\w+)"),
        QRegularExpression::MultilineOption);
    auto it = (jf->kotlin ? ktTop : javaTop).globalMatch(t);
    while (it.hasNext())
        jf->names << it.next().captured(1);
    if (jf->kotlin)
        jf->names << Ctx::stemOf(path) + QStringLiteral("Kt");
    jf->names.removeDuplicates();
}

QString fqn(const QString &pkg, const QString &name)
{
    return pkg.isEmpty() ? name : pkg + QLatin1Char('.') + name;
}

JvmState &prepare(Ctx &ctx)
{
    auto st = std::make_shared<JvmState>();
    for (const QString &f : ctx.in.files) {
        if (!isJvmSource(Ctx::suffixOf(f)) || !ctx.readable(f))
            continue;
        JFile jf;
        readFileInfo(ctx, f, &jf);
        for (const QString &n : std::as_const(jf.names))
            st->nameToFile.insert(fqn(jf.pkg, n), f);
        st->pkgFiles[jf.pkg] << f;
        if (jf.conventional && !st->roots.contains(jf.srcRoot))
            st->roots << jf.srcRoot;
        st->files.insert(f, jf);
    }
    // New package of every file that moves.
    for (auto it = st->files.constBegin(); it != st->files.constEnd(); ++it) {
        const JFile &jf = it.value();
        QString np = jf.pkg;
        const QString nf = ctx.map(jf.path);
        if (nf != jf.path && jf.conventional) {
            const QString nd = Ctx::dirOf(nf);
            QString root;
            if (Ctx::under(nd, jf.srcRoot))
                root = jf.srcRoot;
            else
                for (const QString &r : std::as_const(st->roots))
                    if (Ctx::under(nd, r) && r.size() > root.size())
                        root = r;
            if (!root.isEmpty())
                np = nd == root ? QString() : Ctx::relative(root, nd).replace(QLatin1Char('/'), QLatin1Char('.'));
        }
        st->newPkg.insert(jf.path, np);
    }
    ctx.jvmState = st;
    return *st;
}

bool pkgChanged(const JvmState &st, const QString &file)
{
    auto it = st.files.constFind(file);
    return it != st.files.constEnd() && st.newPkg.value(file) != it->pkg;
}

struct Imp {
    int lineStart = 0, lineEnd = 0; // whole line including the newline
    int fqnStart = 0, fqnLen = 0;
    QString fqn;
    bool wildcard = false, isStatic = false;
    QString alias;
};

QVector<Imp> parseImports(const QString &masked)
{
    QVector<Imp> out;
    static const QRegularExpression re(QStringLiteral("^[ \\t]*import[ \\t]+(static[ \\t]+)?([\\w.]+?)(\\.\\*)?(?:[ \\t]+as[ \\t]+(\\w+))?[ \\t]*;?[ \\t]*(?:\\r?\\n|$)"),
                                       QRegularExpression::MultilineOption);
    auto it = re.globalMatch(masked);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        Imp i;
        i.lineStart = m.capturedStart(0);
        i.lineEnd = m.capturedEnd(0);
        i.fqnStart = m.capturedStart(2);
        i.fqnLen = m.capturedLength(2);
        i.fqn = m.captured(2);
        i.wildcard = !m.captured(3).isEmpty();
        i.isStatic = !m.captured(1).isEmpty();
        i.alias = m.captured(4);
        out.append(i);
    }
    return out;
}

// The project class an imported name refers to: longest prefix that is a known top-level name.
bool classOf(const JvmState &st, const QString &name, QString *file, QString *cls, QString *rest)
{
    QString prefix = name;
    for (;;) {
        auto it = st.nameToFile.constFind(prefix);
        if (it != st.nameToFile.constEnd()) {
            *file = it.value();
            *cls = prefix;
            *rest = name.mid(prefix.size());
            return true;
        }
        const int dot = prefix.lastIndexOf(QLatin1Char('.'));
        if (dot < 0)
            return false;
        prefix = prefix.left(dot);
    }
}

QString simpleName(const QString &cls)
{
    return cls.mid(cls.lastIndexOf(QLatin1Char('.')) + 1);
}

int firstCodeOffset(const QString &t)
{
    int i = 0;
    const int n = t.size();
    for (;;) {
        while (i < n && t.at(i).isSpace())
            ++i;
        if (t.mid(i, 2) == QLatin1String("//")) {
            const int e = t.indexOf(QLatin1Char('\n'), i);
            i = e < 0 ? n : e + 1;
        } else if (t.mid(i, 2) == QLatin1String("/*")) {
            const int e = t.indexOf(QLatin1String("*/"), i + 2);
            i = e < 0 ? n : e + 2;
        } else {
            return i;
        }
    }
}

void scanJvmFile(Ctx &ctx, JvmState &st, const QString &file)
{
    const JFile &jf = st.files[file];
    const QString &raw = ctx.text(file);
    const QString masked = maskJvm(raw, jf.kotlin);
    const QString newPkgG = st.newPkg.value(file);
    const bool gChanged = newPkgG != jf.pkg;
    Emitter &em = ctx.emitter(file);
    const QString semi = jf.kotlin ? QString() : QStringLiteral(";");

    // package statement
    if (gChanged) {
        static const QRegularExpression pkgRe(QStringLiteral("^([ \\t]*package[ \\t]+)([\\w.]+)([ \\t]*;?[ \\t]*(?:\\r?\\n|$))"), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch pm = pkgRe.match(masked);
        if (pm.hasMatch()) {
            if (newPkgG.isEmpty())
                em.add(pm.capturedStart(0), pm.capturedLength(0), QString());
            else
                em.add(pm.capturedStart(2), pm.capturedLength(2), newPkgG);
        } else if (!newPkgG.isEmpty()) {
            em.add(firstCodeOffset(raw), 0, QStringLiteral("package ") + newPkgG + semi + QStringLiteral("\n\n"));
        }
        ++em.references;
    }

    QVector<Imp> imports = parseImports(masked);
    QSet<QString> explicitNew;            // fqn imported after the rewrite
    QSet<QString> wildcardNew;            // packages imported with .* after the rewrite
    QHash<QString, QString> explicitOld;  // simple name -> old fqn
    QSet<QString> removedLines;
    QVector<QPair<int, int>> removals;

    for (const Imp &imp : std::as_const(imports)) {
        if (imp.wildcard) {
            const QString w = imp.fqn;
            QString np = w;
            const QStringList members = st.pkgFiles.value(w);
            bool all = !members.isEmpty(), same = true;
            QString target;
            for (const QString &m : members) {
                if (!pkgChanged(st, m)) {
                    all = false;
                    break;
                }
                if (target.isEmpty())
                    target = st.newPkg.value(m);
                else if (target != st.newPkg.value(m))
                    same = false;
            }
            if (all && same && !target.isEmpty()) {
                np = target;
                em.add(imp.fqnStart, imp.fqnLen, np);
                ++em.references;
            }
            wildcardNew.insert(np);
            continue;
        }
        QString x, cls, rest;
        if (!classOf(st, imp.fqn, &x, &cls, &rest)) {
            explicitNew.insert(imp.fqn);
            explicitOld.insert(imp.alias.isEmpty() ? simpleName(imp.fqn) : imp.alias, imp.fqn);
            continue;
        }
        explicitOld.insert(imp.alias.isEmpty() ? simpleName(imp.fqn) : imp.alias, imp.fqn);
        const QString np = st.newPkg.value(x);
        const QString newName = fqn(np, cls.mid(cls.lastIndexOf(QLatin1Char('.')) + 1)) + rest;
        const bool xChanged = pkgChanged(st, x);
        // now in the same package: the import is redundant
        if (!imp.isStatic && rest.isEmpty() && imp.alias.isEmpty() && np == newPkgG && (xChanged || gChanged) && x != file) {
            removals.append({imp.lineStart, imp.lineEnd});
            continue;
        }
        explicitNew.insert(newName);
        if (newName != imp.fqn) {
            em.add(imp.fqnStart, imp.fqnLen, newName);
            ++em.references;
        }
    }
    for (const auto &r : std::as_const(removals)) {
        em.add(r.first, r.second - r.first, QString());
        ++em.references;
    }

    // names used without an import (same package, wildcard) that live elsewhere now
    QStringList additions;
    QSet<QString> seenWords;
    static const QRegularExpression wordRe(QStringLiteral("\\b[A-Za-z_]\\w*\\b"));
    // only the part after the imports matters for usage
    int bodyStart = 0;
    if (!imports.isEmpty())
        bodyStart = imports.last().lineEnd;
    auto wit = wordRe.globalMatch(masked, bodyStart);
    QSet<QString> declared(jf.names.begin(), jf.names.end());
    while (wit.hasNext()) {
        const QString w = wit.next().captured(0);
        if (seenWords.contains(w))
            continue;
        seenWords.insert(w);
        if (declared.contains(w))
            continue;
        QString x;
        // how the name resolved before the move
        if (explicitOld.contains(w))
            continue; // the import line itself was rewritten above
        x = st.nameToFile.value(fqn(jf.pkg, w));
        if (x.isEmpty() || x == file) {
            x.clear();
            for (const Imp &imp : std::as_const(imports)) {
                if (!imp.wildcard)
                    continue;
                const QString cand = st.nameToFile.value(fqn(imp.fqn, w));
                if (!cand.isEmpty()) {
                    x = cand;
                    break;
                }
            }
        }
        if (x.isEmpty() || x == file)
            continue;
        const QString np = st.newPkg.value(x);
        if (!pkgChanged(st, x) && !gChanged)
            continue;
        if (np == newPkgG || np.isEmpty())
            continue;
        const QString full = fqn(np, w);
        if (explicitNew.contains(full) || wildcardNew.contains(np))
            continue;
        additions << QStringLiteral("import ") + full + semi;
        explicitNew.insert(full);
        if (!st.files.value(x).kotlin) {
            const QRegularExpression pub(QStringLiteral("^public\\s+(?:\\w+\\s+)*(?:class|interface|enum|record|@interface)\\s+") + QRegularExpression::escape(w) + QStringLiteral("\\b"),
                                         QRegularExpression::MultilineOption);
            if (!pub.match(ctx.text(x)).hasMatch())
                ctx.note(QStringLiteral("%1 is not public, but %2 now sits in another package").arg(w, Ctx::nameOf(jf.path)));
        }
    }
    if (!additions.isEmpty()) {
        additions.sort();
        int at;
        QString prefix;
        if (!imports.isEmpty()) {
            at = imports.last().lineEnd;
            if (at > 0 && raw.at(at - 1) != QLatin1Char('\n'))
                prefix = QStringLiteral("\n");
        } else {
            static const QRegularExpression pkgRe(QStringLiteral("^[ \\t]*package[ \\t]+[\\w.]+[ \\t]*;?[ \\t]*(?:\\r?\\n|$)"), QRegularExpression::MultilineOption);
            const QRegularExpressionMatch pm = pkgRe.match(masked);
            if (pm.hasMatch()) {
                at = pm.capturedEnd(0);
                prefix = at > 0 && raw.at(at - 1) != QLatin1Char('\n') ? QStringLiteral("\n\n") : QStringLiteral("\n");
            } else {
                at = firstCodeOffset(raw);
            }
        }
        const bool crlf = raw.contains(QLatin1String("\r\n"));
        QString block = additions.join(crlf ? QStringLiteral("\r\n") : QStringLiteral("\n")) + (crlf ? QStringLiteral("\r\n") : QStringLiteral("\n"));
        if (at == firstCodeOffset(raw) && imports.isEmpty() && !(prefix.size()))
            block += crlf ? QStringLiteral("\r\n") : QStringLiteral("\n");
        em.add(at, 0, prefix + block);
        em.references += additions.size();
    }
}

// Fully-qualified names spelled out in code, strings and configuration files.
void scanFqn(Ctx &ctx, const QString &file, const QVector<QPair<QString, QString>> &classRenames, const QVector<QPair<QString, QString>> &pkgRenames)
{
    const QString &t = ctx.text(file);
    const bool code = isJvmSource(Ctx::suffixOf(file));
    Emitter &em = ctx.emitter(file);
    // skip package/import lines of code files, they are handled structurally
    QVector<QPair<int, int>> skip;
    if (code) {
        static const QRegularExpression line(QStringLiteral("^[ \\t]*(?:package|import)[ \\t][^\\n]*"), QRegularExpression::MultilineOption);
        auto it = line.globalMatch(t);
        while (it.hasNext()) {
            const auto m = it.next();
            skip.append({m.capturedStart(0), m.capturedEnd(0)});
        }
    }
    auto inSkip = [&skip](int pos) {
        for (const auto &s : skip)
            if (pos >= s.first && pos < s.second)
                return true;
        return false;
    };
    // string literals of code files (a package name in @ComponentScan("...") is code that names a package)
    QVector<QPair<int, int>> strings;
    bool stringsReady = false;
    auto inString = [&](int pos) {
        if (!stringsReady) {
            stringsReady = true;
            static const QRegularExpression lit(QStringLiteral("\"(?:[^\"\\\\\\n]|\\\\.)*\""));
            auto it = lit.globalMatch(t);
            while (it.hasNext()) {
                const auto m = it.next();
                strings.append({m.capturedStart(0), m.capturedEnd(0)});
            }
        }
        for (const auto &s : std::as_const(strings))
            if (pos > s.first && pos < s.second)
                return true;
        return false;
    };
    auto run = [&](const QString &oldName, const QString &newName, bool onlyInStrings) {
        if (!t.contains(oldName))
            return;
        const QRegularExpression re(QStringLiteral("(?<![\\w.])") + QRegularExpression::escape(oldName) + QStringLiteral("(?![\\w])"));
        auto it = re.globalMatch(t);
        while (it.hasNext()) {
            const auto m = it.next();
            if (inSkip(m.capturedStart(0)) || (onlyInStrings && !inString(m.capturedStart(0))))
                continue;
            if (em.add(m.capturedStart(0), m.capturedLength(0), newName))
                ++em.references;
        }
    };
    for (const auto &r : classRenames)
        run(r.first, r.second, false);
    for (const auto &r : pkgRenames)
        run(r.first, r.second, code);
}

} // namespace

void scanJvm(Ctx &ctx, const QStringList &files)
{
    if (!ctx.movedExts.contains(QStringLiteral("java")) && !ctx.movedExts.contains(QStringLiteral("kt")))
        return;
    JvmState &st = prepare(ctx);

    QVector<QPair<QString, QString>> classRenames, pkgRenames;
    for (auto it = st.files.constBegin(); it != st.files.constEnd(); ++it) {
        const JFile &jf = it.value();
        const QString np = st.newPkg.value(jf.path);
        if (np == jf.pkg)
            continue;
        for (const QString &n : jf.names)
            classRenames.append({fqn(jf.pkg, n), fqn(np, n)});
    }
    // a package that moved completely
    for (auto it = st.pkgFiles.constBegin(); it != st.pkgFiles.constEnd(); ++it) {
        if (it.key().isEmpty() || !it.key().contains(QLatin1Char('.')))
            continue;
        QString target;
        bool all = true, same = true;
        for (const QString &f : it.value()) {
            if (!pkgChanged(st, f)) {
                all = false;
                break;
            }
            if (target.isEmpty())
                target = st.newPkg.value(f);
            else if (target != st.newPkg.value(f))
                same = false;
        }
        if (all && same && !target.isEmpty())
            pkgRenames.append({it.key(), target});
    }

    // Every Java/Kotlin file takes part (a same-package user does not mention a moved file by name).
    for (auto it = st.files.constBegin(); it != st.files.constEnd(); ++it) {
        const QString &f = it.key();
        scanJvmFile(ctx, st, f);
    }
    for (const QString &f : files)
        scanFqn(ctx, f, classRenames, pkgRenames);
    // Kotlin/Java strings in files the prefilter skipped are not worth a second pass.
}

} // namespace MoveRefactor
