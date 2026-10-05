#include "MoveRefactor.h"

#include "RefactorCtx.h"

#include <QFile>
#include <QStringDecoder>
#include <algorithm>

namespace MoveRefactor {

namespace {

constexpr qint64 kMaxTextSize = 2 * 1024 * 1024;

bool isTokenChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('$');
}

bool wantedFile(const QString &path)
{
    static const QSet<QString> exts = {
        // web
        QStringLiteral("js"), QStringLiteral("jsx"), QStringLiteral("mjs"), QStringLiteral("cjs"), QStringLiteral("ts"), QStringLiteral("tsx"),
        QStringLiteral("mts"), QStringLiteral("cts"), QStringLiteral("vue"), QStringLiteral("svelte"), QStringLiteral("astro"),
        QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("sass"), QStringLiteral("less"), QStringLiteral("styl"),
        QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("xhtml"), QStringLiteral("md"), QStringLiteral("markdown"),
        QStringLiteral("mdx"), QStringLiteral("json"), QStringLiteral("jsonc"), QStringLiteral("php"), QStringLiteral("qml"),
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("dart"), QStringLiteral("rb"),
        // languages with their own module systems
        QStringLiteral("py"), QStringLiteral("pyi"), QStringLiteral("pyw"), QStringLiteral("java"), QStringLiteral("kt"), QStringLiteral("kts"),
        QStringLiteral("c"), QStringLiteral("cc"), QStringLiteral("cpp"), QStringLiteral("cxx"), QStringLiteral("c++"), QStringLiteral("h"),
        QStringLiteral("hh"), QStringLiteral("hpp"), QStringLiteral("hxx"), QStringLiteral("inl"), QStringLiteral("ipp"),
        QStringLiteral("tpp"), QStringLiteral("ino"), QStringLiteral("go"), QStringLiteral("rs"),
        // build and configuration files
        QStringLiteral("cmake"), QStringLiteral("pro"), QStringLiteral("pri"), QStringLiteral("qrc"), QStringLiteral("ui"),
        QStringLiteral("mk"), QStringLiteral("toml"), QStringLiteral("cfg"), QStringLiteral("ini"), QStringLiteral("yml"),
        QStringLiteral("yaml"), QStringLiteral("xml"), QStringLiteral("gradle"), QStringLiteral("properties"), QStringLiteral("txt")};
    static const QSet<QString> names = {QStringLiteral("makefile"), QStringLiteral("meson.build"), QStringLiteral("build.bazel"),
                                        QStringLiteral("cmakelists.txt"), QStringLiteral("dockerfile"), QStringLiteral("qmldir"), QStringLiteral("cargo.toml")};
    const QString name = Ctx::nameOf(path).toLower();
    return exts.contains(Ctx::suffixOf(path)) || names.contains(name);
}

} // namespace

bool Emitter::covered(int start, int length) const
{
    const int end = start + length;
    for (const TextEdit &e : edits) {
        const int eEnd = e.start + e.length;
        if (length == 0) {
            if (start > e.start && start < eEnd)
                return true;
        } else if (e.length == 0) {
            if (e.start > start && e.start < end)
                return true;
        } else if (start < eEnd && e.start < end) {
            return true;
        }
    }
    return false;
}

bool Emitter::add(int start, int length, const QString &text)
{
    if (covered(start, length))
        return false;
    edits.append({start, length, text});
    return true;
}

Ctx::Ctx(const Input &input)
    : in(input)
    , root(QDir::cleanPath(input.root))
    , m_moves(input.moves)
{
    std::sort(m_moves.begin(), m_moves.end(), [](const PathMove &a, const PathMove &b) { return a.from.size() > b.from.size(); });
    QSet<QString> seen;
    for (const PathMove &m : m_moves) {
        if (QFileInfo(m.from).isDir()) {
            const QString prefix = m.from + QLatin1Char('/');
            for (const QString &f : input.files)
                if (f.startsWith(prefix) && !seen.contains(f)) {
                    seen.insert(f);
                    movedFiles.append(f);
                }
        } else if (!seen.contains(m.from)) {
            seen.insert(m.from);
            movedFiles.append(m.from);
        }
    }
    for (const QString &f : std::as_const(movedFiles))
        movedExts.insert(suffixOf(f));
}

QString Ctx::map(const QString &path) const
{
    for (const PathMove &m : m_moves) {
        if (path == m.from)
            return m.to;
        if (path.startsWith(m.from) && path.size() > m.from.size() && path.at(m.from.size()) == QLatin1Char('/'))
            return m.to + path.mid(m.from.size());
    }
    return path;
}

QString Ctx::unmap(const QString &path) const
{
    for (const PathMove &m : m_moves) {
        if (path == m.to)
            return m.from;
        if (path.startsWith(m.to) && path.size() > m.to.size() && path.at(m.to.size()) == QLatin1Char('/'))
            return m.from + path.mid(m.to.size());
    }
    return path;
}

bool Ctx::isFile(const QString &path) const
{
    auto it = m_kind.constFind(path);
    if (it == m_kind.constEnd()) {
        const QFileInfo fi(path);
        it = m_kind.insert(path, fi.isDir() ? 2 : (fi.exists() ? 1 : 0));
    }
    return it.value() == 1;
}

bool Ctx::isDir(const QString &path) const
{
    isFile(path);
    return m_kind.value(path) == 2;
}

const QString &Ctx::text(const QString &path)
{
    static const QString null;
    auto it = m_texts.constFind(path);
    if (it != m_texts.constEnd())
        return it.value();
    if (m_unreadable.contains(path))
        return null;
    const auto ov = in.texts.constFind(path);
    QString t;
    bool ok;
    if (ov != in.texts.constEnd()) {
        t = ov.value();
        ok = true;
    } else {
        ok = readText(path, &t);
    }
    if (!ok) {
        m_unreadable.insert(path);
        return null;
    }
    return m_texts.insert(path, t).value();
}

bool Ctx::readable(const QString &path)
{
    text(path);
    return !m_unreadable.contains(path);
}

Emitter &Ctx::emitter(const QString &path)
{
    return m_emitters[path];
}

QString Ctx::dirOf(const QString &path)
{
    const int i = path.lastIndexOf(QLatin1Char('/'));
    return i <= 0 ? QStringLiteral("/") : path.left(i);
}

QString Ctx::nameOf(const QString &path)
{
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

QString Ctx::stemOf(const QString &path)
{
    const QString n = nameOf(path);
    const int i = n.lastIndexOf(QLatin1Char('.'));
    return i <= 0 ? n : n.left(i);
}

QString Ctx::suffixOf(const QString &path)
{
    const QString n = nameOf(path);
    const int i = n.lastIndexOf(QLatin1Char('.'));
    return i <= 0 ? QString() : n.mid(i + 1).toLower();
}

bool Ctx::under(const QString &path, const QString &dir)
{
    return path == dir || (path.startsWith(dir) && path.size() > dir.size() && (path.at(dir.size()) == QLatin1Char('/') || dir.endsWith(QLatin1Char('/'))));
}

QString Ctx::relative(const QString &fromDir, const QString &target)
{
    return QDir(fromDir).relativeFilePath(target);
}

bool mentions(const QString &text, const QSet<QString> &keys)
{
    const int n = text.size();
    int i = 0;
    while (i < n) {
        if (!isTokenChar(text.at(i))) {
            ++i;
            continue;
        }
        int j = i + 1;
        while (j < n && isTokenChar(text.at(j)))
            ++j;
        if (keys.contains(text.mid(i, j - i)))
            return true;
        i = j;
    }
    return false;
}

bool readText(const QString &path, QString *text, bool *bom)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxTextSize)
        return false;
    QByteArray data = f.readAll();
    if (data.contains('\0'))
        return false;
    bool hasBom = false;
    if (data.startsWith("\xEF\xBB\xBF")) {
        hasBom = true;
        data.remove(0, 3);
    }
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString t = dec.decode(data);
    if (dec.hasError())
        return false;
    *text = t;
    if (bom)
        *bom = hasBom;
    return true;
}

bool writeText(const QString &path, const QString &text, bool bom)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QByteArray data = text.toUtf8();
    if (bom)
        data.prepend("\xEF\xBB\xBF");
    return f.write(data) == data.size();
}

QString applyEdits(const QString &text, const QVector<TextEdit> &edits)
{
    QString out;
    out.reserve(text.size());
    int pos = 0;
    for (const TextEdit &e : edits) {
        out += QStringView(text).mid(pos, e.start - pos);
        out += e.text;
        pos = e.start + e.length;
    }
    out += QStringView(text).mid(pos);
    return out;
}

QVector<TextEdit> inverse(const QString &text, const QVector<TextEdit> &edits)
{
    QVector<TextEdit> out;
    int delta = 0;
    for (const TextEdit &e : edits) {
        out.append({e.start + delta, int(e.text.size()), text.mid(e.start, e.length)});
        delta += int(e.text.size()) - e.length;
    }
    return out;
}

int Plan::references() const
{
    int n = 0;
    for (const FileChange &c : changes)
        n += c.references;
    return n;
}

Plan plan(const Input &in)
{
    Ctx ctx(in);

    // Names that a text has to mention to be affected at all.
    QSet<QString> keys;
    auto addKeys = [&keys](const QString &path) {
        const QString name = Ctx::nameOf(path);
        const QString stem = Ctx::stemOf(path);
        keys.insert(name);
        keys.insert(stem);
        if (stem.startsWith(QLatin1Char('_')))
            keys.insert(stem.mid(1));
        for (const QString &part : stem.split(QLatin1Char('.'), Qt::SkipEmptyParts))
            keys.insert(part);
    };
    for (const PathMove &m : in.moves)
        addKeys(m.from);
    for (const QString &f : ctx.movedFiles)
        addKeys(f);

    QStringList web, py, jvm, native;
    for (const QString &f : in.files) {
        if (!wantedFile(f) || !ctx.readable(f))
            continue;
        if (!ctx.moved(f) && !mentions(ctx.text(f), keys))
            continue;
        web << f; // scanWeb picks what applies to the file type
        const QString ext = Ctx::suffixOf(f);
        const QString name = Ctx::nameOf(f).toLower();
        if (ext == QLatin1String("py") || ext == QLatin1String("pyi") || ext == QLatin1String("pyw") || ext == QLatin1String("toml")
            || ext == QLatin1String("cfg") || ext == QLatin1String("ini") || ext == QLatin1String("yml") || ext == QLatin1String("yaml"))
            py << f;
        if (ext == QLatin1String("java") || ext == QLatin1String("kt") || ext == QLatin1String("kts") || ext == QLatin1String("xml")
            || ext == QLatin1String("gradle") || ext == QLatin1String("properties") || ext == QLatin1String("yml") || ext == QLatin1String("yaml")
            || ext == QLatin1String("json"))
            jvm << f;
        static const QSet<QString> nativeExt = {QStringLiteral("c"),   QStringLiteral("cc"),   QStringLiteral("cpp"),  QStringLiteral("cxx"),
                                                QStringLiteral("c++"), QStringLiteral("h"),    QStringLiteral("hh"),    QStringLiteral("hpp"),
                                                QStringLiteral("hxx"), QStringLiteral("inl"),  QStringLiteral("ipp"),   QStringLiteral("tpp"),
                                                QStringLiteral("ino"), QStringLiteral("go"),   QStringLiteral("rs"),    QStringLiteral("cmake"),
                                                QStringLiteral("pro"), QStringLiteral("pri"),  QStringLiteral("qrc"),   QStringLiteral("ui"),
                                                QStringLiteral("mk"),  QStringLiteral("toml"), QStringLiteral("txt"),   QStringLiteral("qml")};
        if (nativeExt.contains(ext) || name == QLatin1String("makefile") || name == QLatin1String("meson.build")
            || name == QLatin1String("build.bazel") || name == QLatin1String("cmakelists.txt") || name == QLatin1String("qmldir"))
            native << f;
    }

    for (const QString &f : std::as_const(web))
        scanWeb(ctx, f);
    scanPython(ctx, py);
    scanJvm(ctx, jvm);
    scanNative(ctx, native);

    Plan plan;
    for (auto it = ctx.emitters().constBegin(); it != ctx.emitters().constEnd(); ++it) {
        if (it.value().edits.isEmpty())
            continue;
        FileChange c;
        c.oldPath = it.key();
        c.path = ctx.map(it.key());
        c.edits = it.value().edits;
        c.references = it.value().references;
        std::stable_sort(c.edits.begin(), c.edits.end(), [](const TextEdit &a, const TextEdit &b) { return a.start < b.start; });
        c.baseHash = qHash(ctx.text(it.key()));
        plan.changes.append(c);
    }
    std::sort(plan.changes.begin(), plan.changes.end(), [](const FileChange &a, const FileChange &b) { return a.path < b.path; });
    plan.notes = ctx.notes;
    return plan;
}

} // namespace MoveRefactor
