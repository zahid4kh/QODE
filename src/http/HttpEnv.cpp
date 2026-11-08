#include "http/HttpEnv.h"

#include "editor/TextEncoding.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QJsonObject readJson(const QString &path, bool jsonc = false)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    // Decoded like the editor: a BOM or UTF-16 (Windows PowerShell 5's >) would make fromJson fail.
    QByteArray data = TextEncoding::decode(f.readAll()).text.toUtf8();
    if (jsonc)
        data = HttpEnv::stripJsonc(data);
    return QJsonDocument::fromJson(data).object();
}

QString scalar(const QJsonValue &v)
{
    if (v.isString())
        return v.toString();
    if (v.isDouble()) {
        const double d = v.toDouble();
        return d == qint64(d) ? QString::number(qint64(d)) : QString::number(d);
    }
    if (v.isBool())
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}

void merge(HttpEnvironments &out, const QJsonObject &envs)
{
    for (auto it = envs.begin(); it != envs.end(); ++it) {
        const QJsonObject vars = it.value().toObject();
        QHash<QString, QString> &target = it.key() == QLatin1String("$shared") ? out.shared : out.values[it.key()];
        for (auto v = vars.begin(); v != vars.end(); ++v)
            if (!v.value().isObject() && !v.value().isArray())
                target.insert(v.key(), scalar(v.value()));
    }
}

// The folders from `dir` up to `root` (only `dir` when it is outside the root).
QStringList upwards(const QString &dir, const QString &root)
{
    QStringList out;
    QString d = QDir::cleanPath(dir);
    const QString r = QDir::cleanPath(root);
    while (!d.isEmpty()) {
        out << d;
        if (r.isEmpty() || d == r || !d.startsWith(r + QLatin1Char('/')))
            break;
        d = QFileInfo(d).path();
    }
    return out;
}

} // namespace

QHash<QString, QString> HttpEnvironments::vars(const QString &name) const
{
    return values.value(name, shared);
}

namespace HttpEnv {

HttpEnvironments load(const QString &fileDir, const QString &projectRoot)
{
    HttpEnvironments out;
    if (!projectRoot.isEmpty()) {
        const QString vscode = projectRoot + QStringLiteral("/.vscode/settings.json");
        const QJsonObject settings = readJson(vscode, true);
        const QJsonObject envs = settings.value(QStringLiteral("rest-client.environmentVariables")).toObject();
        if (!envs.isEmpty()) {
            merge(out, envs);
            out.sources << vscode;
        }
    }
    for (const QString &dir : upwards(fileDir, projectRoot)) {
        const QString pub = dir + QStringLiteral("/http-client.env.json");
        const QString priv = dir + QStringLiteral("/http-client.private.env.json");
        const bool hasPub = QFileInfo::exists(pub), hasPriv = QFileInfo::exists(priv);
        if (!hasPub && !hasPriv)
            continue;
        if (hasPub) {
            merge(out, readJson(pub));
            out.sources << pub;
        }
        if (hasPriv) {
            merge(out, readJson(priv));
            out.sources << priv;
        }
        break;
    }
    for (auto it = out.values.begin(); it != out.values.end(); ++it)
        for (auto s = out.shared.cbegin(); s != out.shared.cend(); ++s)
            if (!it.value().contains(s.key()))
                it.value().insert(s.key(), s.value());
    out.names = out.values.keys();
    out.names.sort(Qt::CaseInsensitive);
    return out;
}

QHash<QString, QString> dotenv(const QString &fileDir, const QString &projectRoot)
{
    QHash<QString, QString> out;
    for (const QString &dir : {fileDir, projectRoot}) {
        QFile f(dir + QStringLiteral("/.env"));
        if (dir.isEmpty() || !f.open(QIODevice::ReadOnly))
            continue;
        for (const QString &raw : TextEncoding::decode(f.readAll()).text.split(QLatin1Char('\n'))) { // CRLF: trimmed below
            QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            if (line.startsWith(QLatin1String("export ")))
                line = line.mid(7).trimmed();
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0)
                continue;
            QString value = line.mid(eq + 1).trimmed();
            if (value.size() >= 2 && (value.front() == QLatin1Char('"') || value.front() == QLatin1Char('\'')) && value.back() == value.front())
                value = value.mid(1, value.size() - 2);
            out.insert(line.left(eq).trimmed(), value);
        }
        break;
    }
    return out;
}

QByteArray stripJsonc(const QByteArray &text)
{
    QByteArray out;
    out.reserve(text.size());
    bool inString = false;
    for (int i = 0; i < text.size(); ++i) {
        const char c = text.at(i);
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < text.size())
                out += text.at(++i);
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out += c;
        } else if (c == '/' && i + 1 < text.size() && text.at(i + 1) == '/') {
            while (i < text.size() && text.at(i) != '\n')
                ++i;
            out += '\n';
        } else if (c == '/' && i + 1 < text.size() && text.at(i + 1) == '*') {
            const int close = text.indexOf("*/", i + 2);
            i = close < 0 ? text.size() : close + 1;
        } else if (c == ',') {
            int j = i + 1; // a trailing comma: the next real character closes the object / array
            while (j < text.size() && QChar::isSpace(uchar(text.at(j))))
                ++j;
            if (j >= text.size() || (text.at(j) != '}' && text.at(j) != ']'))
                out += c;
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace HttpEnv
