#include "ProjectTemplates.h"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QDataStream>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace {

const QString kRoot = QStringLiteral(":/templates");

QString readResource(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

bool truthy(const QString &v)
{
    return !v.isEmpty() && v != QLatin1String("false") && v != QLatin1String("0");
}

// "key" (set), "!key", "key=value", "!key=value".
bool condition(const ProjectTemplates::Values &vars, QString expr)
{
    const bool negate = expr.startsWith(QLatin1Char('!'));
    if (negate)
        expr.remove(0, 1);
    const int eq = expr.indexOf(QLatin1Char('='));
    const bool on = eq < 0 ? truthy(vars.value(expr)) : vars.value(expr.left(eq)) == expr.mid(eq + 1);
    return on != negate;
}

bool isBinaryName(const QString &name)
{
    static const QSet<QString> bin = {QStringLiteral("jar"), QStringLiteral("png"), QStringLiteral("ico"), QStringLiteral("icns"),
                                      QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("zip")};
    return bin.contains(QFileInfo(name).suffix().toLower());
}

// Dependency versions shared by all templates (_shared/versions.json), as {{key}} values.
const ProjectTemplates::Values &sharedVersions()
{
    static const ProjectTemplates::Values v = [] {
        ProjectTemplates::Values out;
        const QJsonObject o = QJsonDocument::fromJson(readResource(kRoot + QStringLiteral("/_shared/versions.json")).toUtf8()).object();
        for (auto it = o.begin(); it != o.end(); ++it)
            out.insert(it.key(), it.value().toString());
        return out;
    }();
    return v;
}

// `in` plus the values every template can use.
ProjectTemplates::Values withBuiltins(const ProjectTemplates::Values &in)
{
    ProjectTemplates::Values v = sharedVersions();
    for (auto it = in.begin(); it != in.end(); ++it)
        v.insert(it.key(), it.value());
    const QString name = v.value(QStringLiteral("name")).trimmed();
    if (v.value(QStringLiteral("appName")).trimmed().isEmpty())
        v.insert(QStringLiteral("appName"), name);
    v.insert(QStringLiteral("nameId"), ProjectTemplates::identifier(v.value(QStringLiteral("appName"))));
    QString pkg = v.value(QStringLiteral("package"));
    v.insert(QStringLiteral("packagePath"), pkg.replace(QLatin1Char('.'), QLatin1Char('/')));
    static const QRegularExpression angle(QStringLiteral(R"(\s*<[^>]*>\s*)"));
    QString who = v.value(QStringLiteral("maintainer"));
    who.remove(angle);
    v.insert(QStringLiteral("maintainerName"), who.trimmed());
    v.insert(QStringLiteral("year"), QString::number(QDate::currentDate().year()));
    if (!v.contains(QStringLiteral("uuid")))
        v.insert(QStringLiteral("uuid"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    v.insert(QStringLiteral("gitIdentity"), ProjectTemplates::gitIdentity());
    return v;
}

// Replaces {{key}}; keys without a value are collected in `missing` and become empty.
QString substitute(const QString &text, const ProjectTemplates::Values &vars, QStringList *missing)
{
    static const QRegularExpression re(QStringLiteral(R"(\{\{([A-Za-z_]\w*)\}\})"));
    QString out;
    int last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        const QString key = m.captured(1);
        if (vars.contains(key))
            out += vars.value(key);
        else if (missing && !missing->contains(key))
            missing->append(key);
        last = m.capturedEnd();
    }
    return out + text.mid(last);
}

// False when the manifest's "when" table switches the file (or a folder above it) off for these values.
bool wanted(const ProjectTemplates::Template &t, const QString &rel, const ProjectTemplates::Values &vars)
{
    for (auto it = t.when.begin(); it != t.when.end(); ++it) {
        const QString path = substitute(it.key(), vars, nullptr);
        if (rel != path && !rel.startsWith(path + QLatin1Char('/')))
            continue;
        if (!condition(vars, it.value()))
            return false;
    }
    return true;
}

// Renders one text file: conditional line blocks, then placeholders. False (with `error`) on an unbalanced block.
bool renderFile(const QString &text, const ProjectTemplates::Values &vars, QString *out, QString *error, QStringList *missing)
{
    static const QRegularExpression ifRe(QStringLiteral(R"(^\s*\{\{#if\s+(!?\w+(?:=[\w.+-]+)?)\}\}\s*$)"));
    static const QRegularExpression endRe(QStringLiteral(R"(^\s*\{\{/if\}\}\s*$)"));
    const QStringList lines = text.split(QLatin1Char('\n'));
    QStringList result;
    QList<bool> stack;
    bool active = true;
    for (const QString &line : lines) {
        const auto im = ifRe.match(line);
        if (im.hasMatch()) {
            QString key = im.captured(1);
            if (key.startsWith(QLatin1Char('!')))
                key.remove(0, 1);
            key = key.left(key.indexOf(QLatin1Char('=')) < 0 ? key.size() : key.indexOf(QLatin1Char('=')));
            if (!vars.contains(key) && missing && !missing->contains(key))
                missing->append(key);
            const bool on = condition(vars, im.captured(1));
            stack.append(active);
            active = active && on;
            continue;
        }
        if (endRe.match(line).hasMatch()) {
            if (stack.isEmpty()) {
                *error = QStringLiteral("a {{/if}} without a matching {{#if}}");
                return false;
            }
            active = stack.takeLast();
            continue;
        }
        if (active)
            result.append(substitute(line, vars, missing));
    }
    if (!stack.isEmpty()) {
        *error = QStringLiteral("an {{#if}} without a matching {{/if}}");
        return false;
    }
    *out = result.join(QLatin1Char('\n'));
    return true;
}

// File and folder names: "dot_x" -> ".x", "__key__" -> its value. False when a key is unknown or the path escapes.
bool renderPath(const QString &rel, const ProjectTemplates::Values &vars, QString *out)
{
    static const QRegularExpression re(QStringLiteral(R"(__([A-Za-z]\w*?)__)"));
    QStringList parts;
    for (QString seg : rel.split(QLatin1Char('/'))) {
        if (seg.startsWith(QLatin1String("dot_")))
            seg = QLatin1Char('.') + seg.mid(4);
        QString res;
        int last = 0;
        auto it = re.globalMatch(seg);
        while (it.hasNext()) {
            const auto m = it.next();
            if (!vars.contains(m.captured(1)))
                return false;
            res += seg.mid(last, m.capturedStart() - last) + vars.value(m.captured(1));
            last = m.capturedEnd();
        }
        parts.append(res + seg.mid(last));
    }
    *out = parts.join(QLatin1Char('/'));
    for (const QString &p : out->split(QLatin1Char('/')))
        if (p == QLatin1String("..") || p.isEmpty())
            return false;
    return true;
}

bool isJavaKeyword(const QString &s)
{
    static const QSet<QString> kw = {
        QStringLiteral("abstract"), QStringLiteral("assert"), QStringLiteral("boolean"), QStringLiteral("break"), QStringLiteral("byte"),
        QStringLiteral("case"), QStringLiteral("catch"), QStringLiteral("char"), QStringLiteral("class"), QStringLiteral("const"),
        QStringLiteral("continue"), QStringLiteral("default"), QStringLiteral("do"), QStringLiteral("double"), QStringLiteral("else"),
        QStringLiteral("enum"), QStringLiteral("extends"), QStringLiteral("final"), QStringLiteral("finally"), QStringLiteral("float"),
        QStringLiteral("for"), QStringLiteral("goto"), QStringLiteral("if"), QStringLiteral("implements"), QStringLiteral("import"),
        QStringLiteral("instanceof"), QStringLiteral("int"), QStringLiteral("interface"), QStringLiteral("long"), QStringLiteral("native"),
        QStringLiteral("new"), QStringLiteral("package"), QStringLiteral("private"), QStringLiteral("protected"), QStringLiteral("public"),
        QStringLiteral("return"), QStringLiteral("short"), QStringLiteral("static"), QStringLiteral("strictfp"), QStringLiteral("super"),
        QStringLiteral("switch"), QStringLiteral("synchronized"), QStringLiteral("this"), QStringLiteral("throw"), QStringLiteral("throws"),
        QStringLiteral("transient"), QStringLiteral("try"), QStringLiteral("void"), QStringLiteral("volatile"), QStringLiteral("while"),
        QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null"), QStringLiteral("_")};
    return kw.contains(s);
}

// --- Images ----------------------------------------------------------------------------------------------------

// The source image at up to 512 px (SVG is rasterised at that size). A null image means it cannot be read.
QImage loadImage(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QByteArray fmt = reader.format();
    if (fmt == "svg" || fmt == "svgz") {
        QSize s = reader.size();
        if (!s.isValid() || s.isEmpty())
            s = QSize(512, 512);
        s.scale(512, 512, Qt::KeepAspectRatio);
        reader.setScaledSize(s);
    }
    QImage img = reader.read();
    return img.isNull() ? QImage() : img.convertToFormat(QImage::Format_ARGB32);
}

// A rounded square with the first letter of the app's name, for projects created without an icon.
QImage placeholderIcon(const QString &appName)
{
    QImage img(512, 512, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    const QString id = ProjectTemplates::identifier(appName);
    const int hue = int(qHash(id) % 360);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QLinearGradient g(0, 0, 512, 512);
    g.setColorAt(0, QColor::fromHsv(hue, 150, 235));
    g.setColorAt(1, QColor::fromHsv((hue + 40) % 360, 200, 150));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(QRectF(24, 24, 464, 464), 104, 104);
    QString letter = QStringLiteral("A");
    for (const QChar c : appName)
        if (c.isLetterOrNumber()) {
            letter = QString(c.toUpper());
            break;
        }
    QFont f;
    f.setBold(true);
    f.setPixelSize(280);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, 512, 500), Qt::AlignCenter, letter);
    p.end();
    return img;
}

// `src` fitted into a transparent size x size square.
QImage squared(const QImage &src, int size)
{
    QImage canvas(size, size, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    const QImage s = src.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&canvas);
    p.drawImage((size - s.width()) / 2, (size - s.height()) / 2, s);
    return canvas;
}

QByteArray pngBytes(const QImage &img)
{
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

// A Windows icon holding one PNG-compressed frame per size (Vista and newer, and what jpackage/WiX accept).
QByteArray icoBytes(const QImage &src, const QList<int> &sizes)
{
    QList<QByteArray> frames;
    for (int s : sizes)
        frames.append(pngBytes(squared(src, s)));
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << quint16(0) << quint16(1) << quint16(sizes.size());
    quint32 offset = 6 + 16 * quint32(sizes.size());
    for (int i = 0; i < sizes.size(); ++i) {
        const quint8 dim = sizes[i] >= 256 ? 0 : quint8(sizes[i]);
        ds << dim << dim << quint8(0) << quint8(0) << quint16(1) << quint16(32) << quint32(frames[i].size()) << offset;
        offset += quint32(frames[i].size());
    }
    for (const QByteArray &f : frames)
        out.append(f);
    return out;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

} // namespace

// --- Public API ------------------------------------------------------------------------------------------------

QString ProjectTemplates::identifier(const QString &text)
{
    QString out;
    for (const QChar c : text.toLower())
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')))
            out.append(c);
    if (out.isEmpty() || out.at(0).isDigit())
        out.prepend(QStringLiteral("app"));
    return out;
}

QString ProjectTemplates::gitIdentity()
{
    static bool done = false;
    static QString cached;
    if (done)
        return cached;
    done = true;
    auto get = [](const QString &key) {
        QProcess p;
        p.start(QStringLiteral("git"), {QStringLiteral("config"), key});
        if (!p.waitForFinished(1500) || p.exitCode() != 0)
            return QString();
        return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    };
    const QString name = get(QStringLiteral("user.name"));
    const QString mail = get(QStringLiteral("user.email"));
    if (!name.isEmpty())
        cached = mail.isEmpty() ? name : QStringLiteral("%1 <%2>").arg(name, mail);
    return cached;
}

const QList<ProjectTemplates::Template> &ProjectTemplates::all()
{
    static const QList<Template> list = [] {
        QList<Template> out;
        const QStringList dirs = QDir(kRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &d : dirs) {
            if (d.startsWith(QLatin1Char('_')))
                continue;
            const QJsonObject o = QJsonDocument::fromJson(readResource(kRoot + QLatin1Char('/') + d + QStringLiteral("/template.json")).toUtf8()).object();
            if (o.isEmpty())
                continue;
            Template t;
            t.id = o.value(QStringLiteral("id")).toString(d);
            t.category = o.value(QStringLiteral("category")).toString();
            t.name = o.value(QStringLiteral("name")).toString(t.id);
            t.description = o.value(QStringLiteral("description")).toString();
            t.order = o.value(QStringLiteral("order")).toInt();
            auto strings = [&](const char *key) {
                QStringList l;
                for (const QJsonValue &v : o.value(QLatin1String(key)).toArray())
                    l << v.toString();
                return l;
            };
            t.mixins = strings("mixins");
            t.needs = strings("requires");
            t.executable = strings("executable");
            t.openFiles = strings("openFiles");
            t.runCommand = o.value(QStringLiteral("runCommand")).toString();
            const QJsonObject when = o.value(QStringLiteral("when")).toObject();
            for (auto w = when.begin(); w != when.end(); ++w)
                t.when.insert(w.key(), w.value().toString());
            for (const QJsonValue &v : o.value(QStringLiteral("options")).toArray()) {
                const QJsonObject oo = v.toObject();
                Option op;
                op.key = oo.value(QStringLiteral("key")).toString();
                op.label = oo.value(QStringLiteral("label")).toString(op.key);
                op.type = oo.value(QStringLiteral("type")).toString(QStringLiteral("text"));
                op.def = oo.value(QStringLiteral("default")).toString();
                op.hint = oo.value(QStringLiteral("hint")).toString();
                op.required = oo.value(QStringLiteral("required")).toBool();
                for (const QJsonValue &c : oo.value(QStringLiteral("choices")).toArray())
                    op.choices << c.toString();
                t.options.append(op);
            }
            const QJsonObject images = o.value(QStringLiteral("imageOutputs")).toObject();
            for (auto it = images.begin(); it != images.end(); ++it) {
                QList<ImageOutput> outs;
                for (const QJsonValue &v : it.value().toArray()) {
                    const QJsonObject oo = v.toObject();
                    ImageOutput io;
                    io.path = oo.value(QStringLiteral("path")).toString();
                    io.format = oo.value(QStringLiteral("format")).toString();
                    io.size = oo.value(QStringLiteral("size")).toInt();
                    for (const QJsonValue &s : oo.value(QStringLiteral("sizes")).toArray())
                        io.sizes << s.toInt();
                    outs.append(io);
                }
                t.imageOutputs.insert(it.key(), outs);
            }
            out.append(t);
        }
        std::stable_sort(out.begin(), out.end(), [](const Template &a, const Template &b) {
            return a.category == b.category ? a.order < b.order : a.category < b.category;
        });
        return out;
    }();
    return list;
}

const ProjectTemplates::Template *ProjectTemplates::find(const QString &id)
{
    for (const Template &t : all())
        if (t.id == id)
            return &t;
    return nullptr;
}

QString ProjectTemplates::expand(const Template &, const QString &text, const Values &values)
{
    return substitute(text, withBuiltins(values), nullptr);
}

QString ProjectTemplates::defaultValue(const Template &t, const Option &o, const Values &current)
{
    return expand(t, o.def, current);
}

QString ProjectTemplates::validate(const Template &t, const Values &values)
{
    if (values.value(QStringLiteral("name")).trimmed().isEmpty())
        return QStringLiteral("The project needs a name.");
    for (const Option &o : t.options) {
        const QString v = values.value(o.key).trimmed();
        if (o.required && v.isEmpty())
            return QStringLiteral("%1 is required.").arg(o.label);
        if (o.type == QLatin1String("text")) {
            static const QRegularExpression bad(QStringLiteral(R"(["\\$\x00-\x1f])"));
            if (bad.match(v).hasMatch())
                return QStringLiteral("%1 cannot contain quotes, backslashes, $ or control characters.").arg(o.label);
        } else if (o.type == QLatin1String("package")) {
            if (v.isEmpty())
                return QStringLiteral("%1 is required.").arg(o.label);
            static const QRegularExpression pkg(QStringLiteral(R"(^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*$)"));
            if (!pkg.match(v).hasMatch())
                return QStringLiteral("%1 must be lower-case segments separated by dots, e.g. com.example.app.").arg(o.label);
            for (const QString &seg : v.split(QLatin1Char('.')))
                if (isJavaKeyword(seg))
                    return QStringLiteral("\"%1\" is a reserved word and cannot be part of the package name.").arg(seg);
        } else if (o.type == QLatin1String("version")) {
            static const QRegularExpression ver(QStringLiteral(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,5})$)"));
            const auto m = ver.match(v);
            if (!m.hasMatch() || m.captured(1).toInt() < 1 || m.captured(1).toInt() > 255 || m.captured(2).toInt() > 255 || m.captured(3).toInt() > 65535)
                return QStringLiteral("%1 must look like 1.0.0 (major 1-255, minor up to 255, patch up to 65535).").arg(o.label);
        } else if (o.type == QLatin1String("choice")) {
            if (!o.choices.contains(v))
                return QStringLiteral("Pick a value for %1.").arg(o.label);
        } else if (o.type == QLatin1String("image") && !v.isEmpty()) {
            if (!QFileInfo(v).isFile())
                return QStringLiteral("The icon file does not exist.");
            if (loadImage(v).isNull())
                return QStringLiteral("The icon file is not an image QODE can read (use PNG, SVG or JPG).");
        }
    }
    return {};
}

ProjectTemplates::Result ProjectTemplates::instantiate(const Template &t, const Values &values, const QString &destDir)
{
    Result r;
    const QString bad = validate(t, values);
    if (!bad.isEmpty()) {
        r.error = bad;
        return r;
    }
    const QFileInfo di(destDir);
    if (di.exists()) {
        if (!di.isDir()) {
            r.error = QStringLiteral("\"%1\" already exists and is not a folder.").arg(destDir);
            return r;
        }
        if (!QDir(destDir).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            r.error = QStringLiteral("The folder \"%1\" already exists and is not empty.").arg(destDir);
            return r;
        }
    }
    if (!QDir().mkpath(di.absolutePath())) {
        r.error = QStringLiteral("Could not create %1.").arg(di.absolutePath());
        return r;
    }

    Values trimmed;
    for (auto it = values.begin(); it != values.end(); ++it)
        trimmed.insert(it.key(), it.value().trimmed());
    const Values vars = withBuiltins(trimmed);

    const QString tmp = destDir + QStringLiteral(".qode-new-") + QUuid::createUuid().toString(QUuid::Id128).left(8);
    auto fail = [&](const QString &msg) {
        QDir(tmp).removeRecursively();
        r.error = msg;
        return r;
    };
    if (!QDir().mkpath(tmp))
        return fail(QStringLiteral("Could not create %1 (permission denied?).").arg(tmp));

    QStringList roots;
    for (const QString &m : t.mixins)
        roots << kRoot + QStringLiteral("/_shared/") + m;
    roots << kRoot + QLatin1Char('/') + t.id + QStringLiteral("/files");

    QStringList missing;
    for (const QString &root : roots) {
        QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString src = it.next();
            QString rel;
            if (!renderPath(src.mid(root.size() + 1), vars, &rel))
                return fail(QStringLiteral("Template file name \"%1\" is invalid.").arg(src));
            if (!wanted(t, rel, vars))
                continue;
            const QString target = tmp + QLatin1Char('/') + rel;
            QByteArray data;
            QFile in(src);
            if (!in.open(QIODevice::ReadOnly))
                return fail(QStringLiteral("Could not read template file %1.").arg(src));
            data = in.readAll();
            if (!isBinaryName(src)) {
                QString text, err;
                if (!renderFile(QString::fromUtf8(data), vars, &text, &err, &missing))
                    return fail(QStringLiteral("Template file %1 has %2.").arg(src, err));
                if (src.endsWith(QLatin1String(".bat")))
                    text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
                data = text.toUtf8();
            }
            if (!writeFile(target, data))
                return fail(QStringLiteral("Could not write %1.").arg(target));
        }
    }
    if (!missing.isEmpty())
        return fail(QStringLiteral("Template uses unknown placeholders: %1.").arg(missing.join(QStringLiteral(", "))));

    for (const QString &x : t.executable) {
        const QString p = tmp + QLatin1Char('/') + substitute(x, vars, nullptr);
        QFile f(p);
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    }

    for (auto it = t.imageOutputs.begin(); it != t.imageOutputs.end(); ++it) {
        const QString source = trimmed.value(it.key());
        QImage master = source.isEmpty() ? placeholderIcon(vars.value(QStringLiteral("appName"))) : loadImage(source);
        if (master.isNull())
            return fail(QStringLiteral("Could not read the icon %1.").arg(source));
        for (const ImageOutput &o : it.value()) {
            QString rel = substitute(o.path, vars, nullptr);
            QByteArray data;
            if (o.format == QLatin1String("png")) {
                data = pngBytes(squared(master, o.size));
            } else if (o.format == QLatin1String("ico")) {
                data = icoBytes(master, o.sizes);
            } else if (o.format == QLatin1String("copy")) {
                if (source.isEmpty())
                    continue;
                rel.replace(QStringLiteral("{ext}"), QFileInfo(source).suffix().toLower());
                QFile in(source);
                if (!in.open(QIODevice::ReadOnly))
                    return fail(QStringLiteral("Could not read %1.").arg(source));
                data = in.readAll();
            } else {
                continue;
            }
            if (!writeFile(tmp + QLatin1Char('/') + rel, data))
                return fail(QStringLiteral("Could not write %1.").arg(rel));
        }
    }

    if (di.exists() && !QDir().rmdir(destDir))
        return fail(QStringLiteral("Could not replace the empty folder %1.").arg(destDir));
    if (!QDir().rename(tmp, destDir))
        return fail(QStringLiteral("Could not move the new project into %1.").arg(destDir));

    r.runCommand = substitute(t.runCommand, vars, nullptr);
    for (const QString &f : t.openFiles)
        r.openFiles << destDir + QLatin1Char('/') + substitute(f, vars, nullptr);
    r.ok = true;
    return r;
}
