#include "RefactorCtx.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace MoveRefactor {

namespace {

// --- helpers -----------------------------------------------------------------

void addRef(Ctx &ctx, const QString &file, int start, int length, const QString &text)
{
    Emitter &e = ctx.emitter(file);
    if (e.add(start, length, text))
        ++e.references;
}

// A relative reference from `fromDir` to `target`, written like `oldSpec` was.
QString relSpec(const QString &fromDir, const QString &target, const QString &oldSpec, bool forceDot)
{
    QString rel = Ctx::relative(fromDir, target);
    if (rel.isEmpty())
        rel = QStringLiteral(".");
    const bool dot = forceDot || oldSpec.startsWith(QLatin1String("./"));
    if (dot && rel != QLatin1String(".") && rel != QLatin1String("..") && !rel.startsWith(QLatin1String("../")))
        rel = QStringLiteral("./") + rel;
    if (oldSpec.endsWith(QLatin1Char('/')) && !rel.endsWith(QLatin1Char('/')))
        rel += QLatin1Char('/');
    return rel;
}

bool hasScheme(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z][A-Za-z0-9+.\\-]*:"));
    return re.match(s).hasMatch();
}

// Splits "path?query#hash" into the path and the tail that has to stay as it is.
void splitTail(const QString &spec, QString *path, QString *tail)
{
    int i = spec.indexOf(QLatin1Char('?'));
    const int h = spec.indexOf(QLatin1Char('#'));
    if (i < 0 || (h >= 0 && h < i))
        i = h;
    *path = i < 0 ? spec : spec.left(i);
    *tail = i < 0 ? QString() : spec.mid(i);
}

bool plainRelative(const QString &s)
{
    if (s.isEmpty() || s.startsWith(QLatin1Char('/')) || s.startsWith(QLatin1Char('#')) || s.startsWith(QLatin1Char('~')) || s.startsWith(QLatin1Char('$'))
        || s.startsWith(QLatin1Char('{')) || s.startsWith(QLatin1Char('<')) || s.startsWith(QLatin1Char('[')) || s.startsWith(QLatin1Char('@'))
        || s.startsWith(QLatin1Char('%')) || s.startsWith(QLatin1Char('\\')) || s.startsWith(QLatin1Char('(')) || s.contains(QLatin1String("${"))
        || s.contains(QLatin1String("{{")) || s.contains(QLatin1String("<?")) || s.contains(QLatin1Char('!')) || s.contains(QLatin1Char('*'))
        || s.startsWith(QLatin1String("//")) || hasScheme(s))
        return false;
    return true;
}

QString joinClean(const QString &dir, const QString &rel)
{
    return QDir::cleanPath(dir + QLatin1Char('/') + rel);
}

// --- JavaScript / TypeScript -------------------------------------------------

const QStringList &jsExts()
{
    static const QStringList l = {QStringLiteral("ts"),  QStringLiteral("tsx"), QStringLiteral("d.ts"), QStringLiteral("js"),    QStringLiteral("jsx"),
                                  QStringLiteral("mjs"), QStringLiteral("cjs"), QStringLiteral("mts"),  QStringLiteral("cts"),   QStringLiteral("json"),
                                  QStringLiteral("vue"), QStringLiteral("svelte"), QStringLiteral("astro")};
    return l;
}

struct Alias {
    QString prefix, suffix;
    bool wildcard = false;
    QStringList targets; // absolute patterns, "*" stands for the wildcard part
};

struct JsConfig {
    QString baseUrl;
    QVector<Alias> aliases;
};

struct WebState {
    QHash<QString, std::shared_ptr<JsConfig>> configs; // by directory of the nearest tsconfig/jsconfig ("" = none)
    QHash<QString, QString> nearest;                    // directory -> directory holding its config
};

WebState &state(Ctx &ctx)
{
    if (!ctx.webState)
        ctx.webState = std::make_shared<WebState>();
    return *std::static_pointer_cast<WebState>(ctx.webState);
}

// tsconfig files allow comments and trailing commas.
QByteArray stripJsonc(const QString &text)
{
    QString out;
    out.reserve(text.size());
    const int n = text.size();
    bool inString = false;
    for (int i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (inString) {
            out += c;
            if (c == QLatin1Char('\\') && i + 1 < n)
                out += text.at(++i);
            else if (c == QLatin1Char('"'))
                inString = false;
            continue;
        }
        if (c == QLatin1Char('"')) {
            inString = true;
            out += c;
        } else if (c == QLatin1Char('/') && i + 1 < n && text.at(i + 1) == QLatin1Char('/')) {
            while (i < n && text.at(i) != QLatin1Char('\n'))
                ++i;
            out += QLatin1Char('\n');
        } else if (c == QLatin1Char('/') && i + 1 < n && text.at(i + 1) == QLatin1Char('*')) {
            i += 2;
            while (i + 1 < n && !(text.at(i) == QLatin1Char('*') && text.at(i + 1) == QLatin1Char('/')))
                ++i;
            ++i;
        } else {
            out += c;
        }
    }
    static const QRegularExpression trailing(QStringLiteral(",(\\s*[}\\]])"));
    out.replace(trailing, QStringLiteral("\\1"));
    return out.toUtf8();
}

QJsonObject readJsonc(Ctx &ctx, const QString &path)
{
    if (!ctx.isFile(path) || !ctx.readable(path))
        return {};
    return QJsonDocument::fromJson(stripJsonc(ctx.text(path))).object();
}

QString configFile(Ctx &ctx, const QString &ref, const QString &fromDir)
{
    QString p = joinClean(fromDir, ref);
    if (ctx.isDir(p))
        p += QStringLiteral("/tsconfig.json");
    else if (!ctx.isFile(p) && ctx.isFile(p + QStringLiteral(".json")))
        p += QStringLiteral(".json");
    return ctx.isFile(p) ? p : QString();
}

void loadConfig(Ctx &ctx, const QString &file, JsConfig *cfg, int depth)
{
    if (depth > 6)
        return;
    const QJsonObject o = readJsonc(ctx, file);
    if (o.isEmpty())
        return;
    const QString dir = Ctx::dirOf(file);
    const QJsonValue ext = o.value(QStringLiteral("extends"));
    QStringList parents;
    if (ext.isString())
        parents << ext.toString();
    else if (ext.isArray())
        for (const QJsonValue &v : ext.toArray())
            parents << v.toString();
    for (const QString &p : std::as_const(parents)) {
        if (!p.startsWith(QLatin1Char('.')))
            continue; // a package from node_modules
        const QString f = configFile(ctx, p, dir);
        if (!f.isEmpty())
            loadConfig(ctx, f, cfg, depth + 1);
    }
    const QJsonObject co = o.value(QStringLiteral("compilerOptions")).toObject();
    if (co.contains(QStringLiteral("baseUrl")))
        cfg->baseUrl = joinClean(dir, co.value(QStringLiteral("baseUrl")).toString());
    if (co.contains(QStringLiteral("paths"))) {
        cfg->aliases.clear();
        const QString base = cfg->baseUrl.isEmpty() ? dir : cfg->baseUrl;
        const QJsonObject paths = co.value(QStringLiteral("paths")).toObject();
        for (auto it = paths.constBegin(); it != paths.constEnd(); ++it) {
            Alias a;
            const int star = it.key().indexOf(QLatin1Char('*'));
            a.wildcard = star >= 0;
            a.prefix = star >= 0 ? it.key().left(star) : it.key();
            a.suffix = star >= 0 ? it.key().mid(star + 1) : QString();
            for (const QJsonValue &v : it.value().toArray())
                a.targets << joinClean(base, v.toString());
            if (!a.targets.isEmpty())
                cfg->aliases.append(a);
        }
    }
    for (const QJsonValue &r : o.value(QStringLiteral("references")).toArray()) {
        const QString f = configFile(ctx, r.toObject().value(QStringLiteral("path")).toString(), dir);
        if (f.isEmpty())
            continue;
        JsConfig sub;
        loadConfig(ctx, f, &sub, depth + 1);
        cfg->aliases += sub.aliases;
        if (cfg->baseUrl.isEmpty())
            cfg->baseUrl = sub.baseUrl;
    }
}

const JsConfig *configFor(Ctx &ctx, const QString &file)
{
    WebState &st = state(ctx);
    const QString dir = Ctx::dirOf(file);
    auto n = st.nearest.constFind(dir);
    QString holder;
    if (n != st.nearest.constEnd()) {
        holder = n.value();
    } else {
        QString d = dir;
        for (;;) {
            if (ctx.isFile(d + QStringLiteral("/tsconfig.json")) || ctx.isFile(d + QStringLiteral("/jsconfig.json"))) {
                holder = d;
                break;
            }
            if (d == ctx.root || !Ctx::under(d, ctx.root) || d.size() <= 1)
                break;
            d = Ctx::dirOf(d);
        }
        st.nearest.insert(dir, holder);
    }
    if (holder.isEmpty())
        return nullptr;
    auto c = st.configs.constFind(holder);
    if (c == st.configs.constEnd()) {
        auto cfg = std::make_shared<JsConfig>();
        const QString f = ctx.isFile(holder + QStringLiteral("/tsconfig.json")) ? holder + QStringLiteral("/tsconfig.json") : holder + QStringLiteral("/jsconfig.json");
        loadConfig(ctx, f, cfg.get(), 0);
        // Vite style: the root tsconfig only references tsconfig.app.json / tsconfig.node.json.
        if (cfg->aliases.isEmpty()) {
            for (const QString &n2 : {QStringLiteral("tsconfig.app.json"), QStringLiteral("tsconfig.web.json"), QStringLiteral("tsconfig.base.json")}) {
                const QString f2 = holder + QLatin1Char('/') + n2;
                if (ctx.isFile(f2))
                    loadConfig(ctx, f2, cfg.get(), 0);
            }
        }
        c = st.configs.insert(holder, cfg);
    }
    return c.value().get();
}

struct JsTarget {
    enum Form { Exact, NoExt, IndexDir, JsToTs, DirOnly } form = Exact;
    QString path;      // the file (or folder) the specifier ends up at
    QString addedExt;  // NoExt: the suffix that was missing; JsToTs: the suffix written in the specifier
    int alias = -1;    // index into the config's aliases when reached through one
    int target = -1;
    bool viaBase = false;
};

bool resolveJsBase(Ctx &ctx, const QString &base, JsTarget *out)
{
    if (ctx.isFile(base)) {
        out->form = JsTarget::Exact;
        out->path = base;
        return true;
    }
    static const QHash<QString, QStringList> swap = {{QStringLiteral("js"), {QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("jsx")}},
                                                      {QStringLiteral("jsx"), {QStringLiteral("tsx")}},
                                                      {QStringLiteral("mjs"), {QStringLiteral("mts")}},
                                                      {QStringLiteral("cjs"), {QStringLiteral("cts")}}};
    const QString ext = Ctx::suffixOf(base);
    if (swap.contains(ext)) {
        const QString stem = base.left(base.size() - ext.size() - 1);
        for (const QString &e : swap.value(ext)) {
            if (ctx.isFile(stem + QLatin1Char('.') + e)) {
                out->form = JsTarget::JsToTs;
                out->path = stem + QLatin1Char('.') + e;
                out->addedExt = ext;
                return true;
            }
        }
    }
    for (const QString &e : jsExts()) {
        if (ctx.isFile(base + QLatin1Char('.') + e)) {
            out->form = JsTarget::NoExt;
            out->path = base + QLatin1Char('.') + e;
            out->addedExt = e;
            return true;
        }
    }
    if (ctx.isDir(base)) {
        for (const QString &e : jsExts()) {
            if (ctx.isFile(base + QStringLiteral("/index.") + e)) {
                out->form = JsTarget::IndexDir;
                out->path = base + QStringLiteral("/index.") + e;
                return true;
            }
        }
        out->form = JsTarget::DirOnly;
        out->path = base;
        return true;
    }
    return false;
}

// What the specifier denotes once the target sits at `newTarget`.
QString jsRefPath(const JsTarget &t, const QString &newTarget)
{
    switch (t.form) {
    case JsTarget::Exact:
    case JsTarget::DirOnly:
        return newTarget;
    case JsTarget::JsToTs: {
        const QString ext = Ctx::suffixOf(newTarget);
        return newTarget.left(newTarget.size() - ext.size()) + t.addedExt;
    }
    case JsTarget::NoExt: {
        const QString dotExt = QLatin1Char('.') + t.addedExt;
        if (newTarget.endsWith(dotExt))
            return newTarget.left(newTarget.size() - dotExt.size());
        const QString ext = Ctx::suffixOf(newTarget);
        return ext.isEmpty() ? newTarget : newTarget.left(newTarget.size() - ext.size() - 1);
    }
    case JsTarget::IndexDir: {
        if (Ctx::stemOf(newTarget) == QLatin1String("index"))
            return Ctx::dirOf(newTarget);
        const QString ext = Ctx::suffixOf(newTarget);
        return ext.isEmpty() ? newTarget : newTarget.left(newTarget.size() - ext.size() - 1);
    }
    }
    return newTarget;
}

bool resolveJs(Ctx &ctx, const QString &file, const QString &spec, const JsConfig *cfg, JsTarget *out)
{
    if (spec.startsWith(QLatin1Char('.'))) {
        QString base = joinClean(Ctx::dirOf(file), spec);
        return resolveJsBase(ctx, base, out);
    }
    if (spec.startsWith(QLatin1Char('/')) || hasScheme(spec) || !cfg)
        return false;
    for (int a = 0; a < cfg->aliases.size(); ++a) {
        const Alias &al = cfg->aliases.at(a);
        QString star;
        if (al.wildcard) {
            if (!spec.startsWith(al.prefix) || !spec.endsWith(al.suffix) || spec.size() < al.prefix.size() + al.suffix.size())
                continue;
            star = spec.mid(al.prefix.size(), spec.size() - al.prefix.size() - al.suffix.size());
        } else if (spec != al.prefix) {
            continue;
        }
        for (int t = 0; t < al.targets.size(); ++t) {
            QString cand = al.targets.at(t);
            cand.replace(QLatin1Char('*'), star);
            if (resolveJsBase(ctx, QDir::cleanPath(cand), out)) {
                out->alias = a;
                out->target = t;
                return true;
            }
        }
    }
    if (!cfg->baseUrl.isEmpty() && resolveJsBase(ctx, joinClean(cfg->baseUrl, spec), out)) {
        out->viaBase = true;
        return true;
    }
    return false;
}

QString aliasSpec(const Alias &al, const QString &pattern, const QString &ref)
{
    const int star = pattern.indexOf(QLatin1Char('*'));
    if (star < 0)
        return ref == pattern ? al.prefix : QString();
    const QString pre = pattern.left(star), post = pattern.mid(star + 1);
    if (!ref.startsWith(pre) || !ref.endsWith(post) || ref.size() <= pre.size() + post.size())
        return QString();
    return al.prefix + ref.mid(pre.size(), ref.size() - pre.size() - post.size()) + al.suffix;
}

// The new spelling of a module specifier written in `file` (empty when it stays valid or cannot be resolved).
QString jsNewSpec(Ctx &ctx, const QString &file, const QString &spec, const JsConfig *cfg)
{
    const QString newFile = ctx.map(file);
    QString path, tail;
    splitTail(spec, &path, &tail);
    if (path.isEmpty())
        return {};
    JsTarget t;
    if (!resolveJs(ctx, file, path, path.startsWith(QLatin1Char('.')) ? nullptr : cfg, &t))
        return {};
    const QString newTarget = ctx.map(t.path);
    if (newTarget == t.path && newFile == file)
        return {};
    const QString ref = jsRefPath(t, newTarget);
    QString spec2;
    if (path.startsWith(QLatin1Char('.'))) {
        spec2 = relSpec(Ctx::dirOf(newFile), ref, path, true);
    } else if (t.alias >= 0) {
        const Alias &al = cfg->aliases.at(t.alias);
        spec2 = aliasSpec(al, al.targets.at(t.target), ref);
        for (int a = 0; spec2.isEmpty() && a < cfg->aliases.size(); ++a)
            for (const QString &pat : cfg->aliases.at(a).targets) {
                spec2 = aliasSpec(cfg->aliases.at(a), pat, ref);
                if (!spec2.isEmpty())
                    break;
            }
    } else if (t.viaBase && Ctx::under(ref, cfg->baseUrl) && ref != cfg->baseUrl) {
        spec2 = Ctx::relative(cfg->baseUrl, ref);
    }
    if (spec2.isEmpty())
        spec2 = relSpec(Ctx::dirOf(newFile), ref, QStringLiteral("./"), true);
    spec2 += tail;
    return spec2 == spec ? QString() : spec2;
}

void scanJs(Ctx &ctx, const QString &file)
{
    const QString &text = ctx.text(file);
    static const QRegularExpression re(QStringLiteral(
        "(?:\\bfrom|\\bimport|\\brequire(?:\\.resolve|\\.requireActual|\\.actual)?|\\b(?:jest|vi)\\.\\w+|\\bnew\\s+URL|reference\\s+path\\s*=)\\s*\\(?\\s*(['\"`])((?:(?!\\1)[^\\n\\\\])+)\\1"));
    const JsConfig *cfg = nullptr;
    bool cfgLoaded = false;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString spec = m.captured(2);
        if (spec.contains(QLatin1String("${")) || spec.contains(QLatin1Char('!')) || spec.contains(QLatin1Char('*')))
            continue;
        if (!cfgLoaded && !spec.startsWith(QLatin1Char('.'))) {
            cfg = configFor(ctx, file);
            cfgLoaded = true;
        }
        const QString spec2 = jsNewSpec(ctx, file, spec, cfg);
        if (!spec2.isEmpty())
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), spec2);
    }
}

void scanRootUrl(Ctx &ctx, const QString &file, int start, int length, const QString &spec);

// --- CSS / SCSS / Less -------------------------------------------------------

struct CssTarget {
    QString path;
    bool noExt = false;
    bool underscore = false; // file is _name.scss, specifier says "name"
    bool indexDir = false;
};

bool resolveCss(Ctx &ctx, const QString &base, const QString &spec, CssTarget *out)
{
    if (ctx.isFile(base) && !spec.endsWith(QLatin1Char('/'))) {
        out->path = base;
        return true;
    }
    static const QStringList exts = {QStringLiteral("scss"), QStringLiteral("sass"), QStringLiteral("css"), QStringLiteral("less"), QStringLiteral("styl")};
    const QString dir = Ctx::dirOf(base), name = Ctx::nameOf(base);
    for (const QString &e : exts) {
        if (ctx.isFile(base + QLatin1Char('.') + e)) {
            out->path = base + QLatin1Char('.') + e;
            out->noExt = true;
            return true;
        }
        if (ctx.isFile(dir + QStringLiteral("/_") + name + QLatin1Char('.') + e)) {
            out->path = dir + QStringLiteral("/_") + name + QLatin1Char('.') + e;
            out->noExt = true;
            out->underscore = true;
            return true;
        }
    }
    if (ctx.isDir(base)) {
        for (const QString &e : exts)
            for (const QString &i : {QStringLiteral("/_index."), QStringLiteral("/index.")})
                if (ctx.isFile(base + i + e)) {
                    out->path = base + i + e;
                    out->indexDir = true;
                    return true;
                }
        out->path = base;
        return true;
    }
    return false;
}

QString cssRef(const CssTarget &t, const QString &newTarget)
{
    if (t.indexDir)
        return (Ctx::stemOf(newTarget) == QLatin1String("index") || Ctx::stemOf(newTarget) == QLatin1String("_index")) ? Ctx::dirOf(newTarget) : newTarget;
    QString p = newTarget;
    if (t.noExt) {
        const QString ext = Ctx::suffixOf(p);
        if (!ext.isEmpty())
            p.chop(ext.size() + 1);
        if (t.underscore) {
            const QString dir = Ctx::dirOf(p), name = Ctx::nameOf(p);
            if (name.startsWith(QLatin1Char('_')))
                p = dir + QLatin1Char('/') + name.mid(1);
        }
    }
    return p;
}

void scanCssText(Ctx &ctx, const QString &file, bool urlsOnly)
{
    const QString &text = ctx.text(file);
    const QString newFile = ctx.map(file);
    static const QRegularExpression atRule(QStringLiteral("@(?:import|use|forward)\\s+(?:\\([^)]*\\)\\s*)?(?:url\\(\\s*)?([\"'])([^\"'\\n]+)\\1"));
    static const QRegularExpression urlRe(QStringLiteral("\\burl\\(\\s*([\"']?)([^\"')\\s]+)\\1\\s*\\)"));
    auto handle = [&](const QRegularExpressionMatch &m) {
        const QString spec = m.captured(2);
        QString path, tail;
        splitTail(spec, &path, &tail);
        if (path.startsWith(QLatin1Char('/')) && !path.startsWith(QLatin1String("//"))) {
            scanRootUrl(ctx, file, m.capturedStart(2), m.capturedLength(2), spec);
            return;
        }
        if (!plainRelative(path) || path.startsWith(QLatin1String("data:")))
            return;
        CssTarget t;
        if (!resolveCss(ctx, joinClean(Ctx::dirOf(file), path), path, &t))
            return;
        const QString newTarget = ctx.map(t.path);
        if (newTarget == t.path && newFile == file)
            return;
        QString spec2 = relSpec(Ctx::dirOf(newFile), cssRef(t, newTarget), path, false) + tail;
        if (spec2 != spec)
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), spec2);
    };
    if (!urlsOnly) {
        auto it = atRule.globalMatch(text);
        while (it.hasNext())
            handle(it.next());
    }
    auto it2 = urlRe.globalMatch(text);
    while (it2.hasNext())
        handle(it2.next());
}

// --- HTML-ish attributes -----------------------------------------------------

// Percent-decoded when the written form was encoded; returns whether it was.
QString urlPath(const QString &path, bool *encoded)
{
    *encoded = path.contains(QLatin1Char('%'));
    return *encoded ? QUrl::fromPercentEncoding(path.toUtf8()) : path;
}

// "/src/main.tsx": relative to the web root, which is the project root (or public/, static/ beside it).
void scanRootUrl(Ctx &ctx, const QString &file, int start, int length, const QString &spec)
{
    QString path, tail;
    splitTail(spec, &path, &tail);
    if (path.size() < 2 || !path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1String("//")))
        return;
    const QStringList bases = {ctx.root, ctx.root + QStringLiteral("/public"), ctx.root + QStringLiteral("/static")};
    QString base, target;
    for (const QString &b : bases) {
        const QString cand = joinClean(b, path.mid(1));
        if (ctx.exists(cand)) {
            base = b;
            target = cand;
            break;
        }
    }
    if (target.isEmpty())
        return;
    const QString newTarget = ctx.map(target);
    if (newTarget == target)
        return;
    QString rel;
    if (Ctx::under(newTarget, base))
        rel = Ctx::relative(base, newTarget);
    for (int i = 0; rel.isEmpty() && i < bases.size(); ++i)
        if (Ctx::under(newTarget, bases.at(i)) && newTarget != bases.at(i))
            rel = Ctx::relative(bases.at(i), newTarget);
    if (rel.isEmpty())
        return;
    const QString spec2 = QLatin1Char('/') + rel + tail;
    if (spec2 != spec)
        addRef(ctx, file, start, length, spec2);
}

void scanUrlSpec(Ctx &ctx, const QString &file, int start, int length, const QString &spec, bool allowRoot = false)
{
    if (allowRoot && spec.startsWith(QLatin1Char('/'))) {
        scanRootUrl(ctx, file, start, length, spec);
        return;
    }
    QString path, tail;
    splitTail(spec, &path, &tail);
    if (!plainRelative(path))
        return;
    bool encoded;
    const QString decoded = urlPath(path, &encoded);
    const QString target = joinClean(Ctx::dirOf(file), decoded);
    if (!ctx.exists(target))
        return;
    const QString newFile = ctx.map(file), newTarget = ctx.map(target);
    if (newTarget == target && newFile == file)
        return;
    QString rel = relSpec(Ctx::dirOf(newFile), newTarget, path, false);
    if (encoded)
        rel = QString::fromLatin1(QUrl::toPercentEncoding(rel, "/.-_~"));
    rel += tail;
    if (rel != spec)
        addRef(ctx, file, start, length, rel);
}

void scanHtmlAttrs(Ctx &ctx, const QString &file)
{
    static const QRegularExpression re(QStringLiteral("\\b(?:src|href|poster|data-src|data-href|xlink:href|background)\\s*=\\s*([\"'])([^\"'\\n]*)\\1"));
    const QString &text = ctx.text(file);
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        scanUrlSpec(ctx, file, m.capturedStart(2), m.capturedLength(2), m.captured(2), true);
    }
}

// --- Markdown ----------------------------------------------------------------

void scanMarkdown(Ctx &ctx, const QString &file)
{
    const QString &text = ctx.text(file);
    static const QRegularExpression inlineLink(QStringLiteral("\\]\\(\\s*(<[^>\\n]*>|[^)\\s]+)(?:\\s+(?:\"[^\"\\n]*\"|'[^'\\n]*'))?\\s*\\)"));
    static const QRegularExpression refDef(QStringLiteral("^ {0,3}\\[[^\\]\\n]+\\]:[ \\t]*(<[^>\\n]+>|\\S+)"), QRegularExpression::MultilineOption);
    for (const QRegularExpression *re : {&inlineLink, &refDef}) {
        auto it = re->globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            int s = m.capturedStart(1), l = m.capturedLength(1);
            QString spec = m.captured(1);
            if (spec.startsWith(QLatin1Char('<')) && spec.endsWith(QLatin1Char('>'))) {
                ++s;
                l -= 2;
                spec = spec.mid(1, spec.size() - 2);
            }
            scanUrlSpec(ctx, file, s, l, spec);
        }
    }
}

// --- JSON --------------------------------------------------------------------

// "scripts": { "build": "node tools/build.js --out dist" }: file arguments relative to the package folder.
void scanPackageScripts(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    static const QRegularExpression block(QStringLiteral("\"scripts\"\\s*:\\s*\\{([^}]*)\\}"));
    const QRegularExpressionMatch bm = block.match(t);
    if (!bm.hasMatch())
        return;
    static const QRegularExpression valueRe(QStringLiteral("\"[^\"\\n]+\"\\s*:\\s*\"((?:[^\"\\\\\\n]|\\\\.)*)\""));
    static const QRegularExpression tokRe(QStringLiteral("[^\\s\"'`;&|<>()=]+"));
    const QString newFile = ctx.map(file);
    auto vit = valueRe.globalMatch(bm.captured(1));
    while (vit.hasNext()) {
        const QRegularExpressionMatch vm = vit.next();
        const QString value = vm.captured(1);
        auto tit = tokRe.globalMatch(value);
        while (tit.hasNext()) {
            const QRegularExpressionMatch tm = tit.next();
            const QString tok = tm.captured(0);
            if (!tok.contains(QLatin1Char('/')) || !plainRelative(tok) || tok.startsWith(QLatin1Char('-')))
                continue;
            const QString target = joinClean(Ctx::dirOf(file), tok);
            if (!ctx.exists(target))
                continue;
            const QString nt = ctx.map(target);
            if (nt == target && newFile == file)
                continue;
            QString rel = relSpec(Ctx::dirOf(newFile), nt, tok, false);
            if (rel != tok)
                addRef(ctx, file, bm.capturedStart(1) + vm.capturedStart(1) + tm.capturedStart(0), tok.size(), rel);
        }
    }
}

void scanJson(Ctx &ctx, const QString &file)
{
    const QString &text = ctx.text(file);
    const QString name = Ctx::nameOf(file).toLower();
    const bool tsconfig = name.startsWith(QLatin1String("tsconfig")) || name.startsWith(QLatin1String("jsconfig"));
    static const QRegularExpression re(QStringLiteral("\"((?:[^\"\\\\\\n]|\\\\.)*)\""));
    const QString newFile = ctx.map(file);
    if (name == QLatin1String("package.json"))
        scanPackageScripts(ctx, file);
    const bool shadcn = name == QLatin1String("components.json"); // "ui": "@/components/ui"
    const JsConfig *cfg = shadcn ? configFor(ctx, file) : nullptr;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QString spec = m.captured(1);
        if (spec.isEmpty() || spec.contains(QLatin1Char('\\')))
            continue;
        if (cfg && !spec.startsWith(QLatin1Char('.')) && spec.contains(QLatin1Char('/'))) {
            const QString s2 = jsNewSpec(ctx, file, spec, cfg);
            if (!s2.isEmpty())
                addRef(ctx, file, m.capturedStart(1), m.capturedLength(1), s2);
            continue;
        }
        const bool dotted = spec.startsWith(QLatin1String("./")) || spec.startsWith(QLatin1String("../"));
        if (!dotted && !(tsconfig && plainRelative(spec) && !spec.contains(QLatin1Char(' '))))
            continue;
        QString path = spec;
        QString globTail;
        if (path.endsWith(QLatin1String("/*")) || path.endsWith(QLatin1String("/**"))) { // tsconfig paths / include patterns
            globTail = path.mid(path.indexOf(QLatin1Char('*')) - 1);
            path.chop(globTail.size());
        }
        if (path.isEmpty())
            continue;
        QString target = joinClean(Ctx::dirOf(file), path);
        if (!ctx.exists(target) && tsconfig && ctx.isFile(target + QStringLiteral(".json")))
            target += QStringLiteral(".json");
        if (!ctx.exists(target))
            continue;
        const QString newTarget = ctx.map(target);
        if (newTarget == target && newFile == file)
            continue;
        QString ref = newTarget;
        if (target.endsWith(QLatin1String(".json")) && !path.endsWith(QLatin1String(".json")) && ref.endsWith(QLatin1String(".json")))
            ref.chop(5);
        QString spec2 = relSpec(Ctx::dirOf(newFile), ref, path, false);
        if (dotted && !spec2.startsWith(QLatin1Char('.')))
            spec2 = QStringLiteral("./") + spec2;
        spec2 += globTail;
        if (spec2 != spec)
            addRef(ctx, file, m.capturedStart(1), m.capturedLength(1), spec2);
    }
}

// --- PHP ---------------------------------------------------------------------

void scanPhp(Ctx &ctx, const QString &file)
{
    const QString &text = ctx.text(file);
    const QString newFile = ctx.map(file);
    static const QRegularExpression dirRe(QStringLiteral("__DIR__\\s*\\.\\s*(['\"])(/[^'\"\\n]*)\\1"));
    static const QRegularExpression incRe(QStringLiteral("\\b(?:require|include)(?:_once)?\\s*\\(?\\s*(['\"])([^'\"\\n$]+)\\1"));
    auto it = dirRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString spec = m.captured(2);
        const QString target = joinClean(Ctx::dirOf(file), spec);
        if (!ctx.exists(target))
            continue;
        const QString newTarget = ctx.map(target);
        if (newTarget == target && newFile == file)
            continue;
        const QString spec2 = QLatin1Char('/') + Ctx::relative(Ctx::dirOf(newFile), newTarget);
        if (spec2 != spec)
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), spec2);
    }
    auto it2 = incRe.globalMatch(text);
    while (it2.hasNext()) {
        const QRegularExpressionMatch m = it2.next();
        const QString spec = m.captured(2);
        if (!plainRelative(spec))
            continue;
        const QString target = joinClean(Ctx::dirOf(file), spec);
        if (!ctx.isFile(target))
            continue;
        const QString newTarget = ctx.map(target);
        if (newTarget == target && newFile == file)
            continue;
        const QString spec2 = relSpec(Ctx::dirOf(newFile), newTarget, spec, false);
        if (spec2 != spec)
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), spec2);
    }
}

// --- shell -------------------------------------------------------------------

void scanShell(Ctx &ctx, const QString &file)
{
    const QString &text = ctx.text(file);
    const QString newFile = ctx.map(file);
    static const QRegularExpression re(QStringLiteral("^[ \\t]*(?:source|\\.)[ \\t]+([\"']?)([^\\s\"';|&$]+)\\1"), QRegularExpression::MultilineOption);
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString spec = m.captured(2);
        if (!spec.contains(QLatin1Char('/')) || spec.startsWith(QLatin1Char('/')) || spec.startsWith(QLatin1Char('~')))
            continue;
        const QString target = joinClean(Ctx::dirOf(file), spec);
        if (!ctx.isFile(target))
            continue;
        const QString newTarget = ctx.map(target);
        if (newTarget == target && newFile == file)
            continue;
        const QString spec2 = relSpec(Ctx::dirOf(newFile), newTarget, spec, false);
        if (spec2 != spec)
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), spec2);
    }
}

// --- Dart / Ruby -----------------------------------------------------------------

void scanDart(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    const QString newFile = ctx.map(file);
    static const QRegularExpression re(QStringLiteral("^[ \\t]*(?:import|export|part(?:[ \\t]+of)?)[ \\t]+(['\"])([^'\"\\n]+)\\1"), QRegularExpression::MultilineOption);
    // package:name/... lives in the nearest pubspec's lib/
    QString pubDir, pubName;
    for (QString d = Ctx::dirOf(file); Ctx::under(d, ctx.root); d = Ctx::dirOf(d)) {
        const QString pub = d + QStringLiteral("/pubspec.yaml");
        if (ctx.isFile(pub) && ctx.readable(pub)) {
            static const QRegularExpression nm(QStringLiteral("^name:[ \\t]*(\\S+)"), QRegularExpression::MultilineOption);
            const QRegularExpressionMatch nmm = nm.match(ctx.text(pub));
            if (nmm.hasMatch()) {
                pubDir = d;
                pubName = nmm.captured(1);
            }
            break;
        }
        if (d == ctx.root)
            break;
    }
    auto it = re.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString spec = m.captured(2);
        QString target;
        bool package = false;
        if (spec.startsWith(QLatin1String("package:"))) {
            if (pubName.isEmpty() || !spec.startsWith(QStringLiteral("package:") + pubName + QLatin1Char('/')))
                continue;
            target = joinClean(pubDir + QStringLiteral("/lib"), spec.mid(8 + pubName.size() + 1));
            package = true;
        } else if (spec.startsWith(QLatin1String("dart:")) || hasScheme(spec)) {
            continue;
        } else {
            target = joinClean(Ctx::dirOf(file), spec);
        }
        if (!ctx.isFile(target))
            continue;
        const QString newTarget = ctx.map(target);
        if (newTarget == target && (package || newFile == file))
            continue;
        QString spec2;
        const QString lib = pubDir + QStringLiteral("/lib");
        if (package && Ctx::under(newTarget, lib) && newTarget != lib)
            spec2 = QStringLiteral("package:") + pubName + QLatin1Char('/') + Ctx::relative(lib, newTarget);
        else
            spec2 = relSpec(Ctx::dirOf(newFile), newTarget, QStringLiteral("x"), false);
        if (spec2 != spec)
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), spec2);
    }
}

void scanRuby(Ctx &ctx, const QString &file)
{
    const QString &t = ctx.text(file);
    const QString newFile = ctx.map(file);
    static const QRegularExpression re(QStringLiteral("\\brequire_relative[ \\t(]+(['\"])([^'\"\\n]+)\\1"));
    auto it = re.globalMatch(t);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString spec = m.captured(2);
        const QString base = joinClean(Ctx::dirOf(file), spec);
        QString target = base;
        const bool addedExt = !ctx.isFile(base) && ctx.isFile(base + QStringLiteral(".rb"));
        if (addedExt)
            target = base + QStringLiteral(".rb");
        if (!ctx.isFile(target))
            continue;
        const QString newTarget = ctx.map(target);
        if (newTarget == target && newFile == file)
            continue;
        QString ref = newTarget;
        if (addedExt && ref.endsWith(QLatin1String(".rb")))
            ref.chop(3);
        const QString spec2 = relSpec(Ctx::dirOf(newFile), ref, spec, false);
        if (spec2 != spec)
            addRef(ctx, file, m.capturedStart(2), m.capturedLength(2), spec2);
    }
}

} // namespace

void scanWeb(Ctx &ctx, const QString &file)
{
    const QString ext = Ctx::suffixOf(file);
    static const QSet<QString> js = {QStringLiteral("js"),  QStringLiteral("jsx"),  QStringLiteral("mjs"), QStringLiteral("cjs"),   QStringLiteral("ts"),
                                     QStringLiteral("tsx"), QStringLiteral("mts"),  QStringLiteral("cts"), QStringLiteral("mdx")};
    static const QSet<QString> sfc = {QStringLiteral("vue"), QStringLiteral("svelte"), QStringLiteral("astro"), QStringLiteral("html"),
                                      QStringLiteral("htm"), QStringLiteral("xhtml")};
    static const QSet<QString> css = {QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("sass"), QStringLiteral("less"), QStringLiteral("styl")};
    if (js.contains(ext))
        scanJs(ctx, file);
    if (ext == QLatin1String("jsx") || ext == QLatin1String("tsx"))
        scanHtmlAttrs(ctx, file); // <img src="/images/a.png">
    if (sfc.contains(ext)) {
        scanJs(ctx, file);
        scanHtmlAttrs(ctx, file);
        scanCssText(ctx, file, false);
    }
    if (css.contains(ext))
        scanCssText(ctx, file, false);
    if (ext == QLatin1String("md") || ext == QLatin1String("markdown") || ext == QLatin1String("mdx")) {
        scanMarkdown(ctx, file);
        scanHtmlAttrs(ctx, file);
    }
    if (ext == QLatin1String("json") || ext == QLatin1String("jsonc"))
        scanJson(ctx, file);
    if (ext == QLatin1String("php")) {
        scanPhp(ctx, file);
        scanHtmlAttrs(ctx, file);
    }
    if (ext == QLatin1String("sh") || ext == QLatin1String("bash") || ext == QLatin1String("zsh"))
        scanShell(ctx, file);
    if (ext == QLatin1String("dart"))
        scanDart(ctx, file);
    if (ext == QLatin1String("rb"))
        scanRuby(ctx, file);
}

} // namespace MoveRefactor
