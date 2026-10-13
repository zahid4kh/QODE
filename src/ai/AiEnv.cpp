#include "AiEnv.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

namespace AiEnv {

QString parseValue(const QString &text, const QString &key)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("export ")))
            line = line.mid(7).trimmed();
        if (line.startsWith(QLatin1Char('#')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || line.left(eq).trimmed() != key)
            continue;
        QString v = line.mid(eq + 1).trimmed();
        if (v.size() >= 2 && (v.startsWith(QLatin1Char('"')) || v.startsWith(QLatin1Char('\''))) && v.endsWith(v.at(0)))
            return v.mid(1, v.size() - 2);
        const int hash = v.indexOf(QStringLiteral(" #"));
        if (hash >= 0)
            v = v.left(hash).trimmed();
        return v;
    }
    // A file holding nothing but the key itself.
    QStringList bare;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')))
            bare << line;
    }
    if (bare.size() == 1 && !bare.first().contains(QLatin1Char('=')) && !bare.first().contains(QLatin1Char(' ')))
        return bare.first();
    return {};
}

QString apiKey(const QString &projectRoot, QString *source)
{
    const QString key = QStringLiteral("GEMINI_API_KEY");
    {
        QFile f(storedKeyPath());
        if (f.open(QIODevice::ReadOnly)) {
            const QString v = QString::fromUtf8(f.readAll()).trimmed();
            if (!v.isEmpty()) {
                if (source)
                    *source = QStringLiteral("saved in QODE");
                return v;
            }
        }
    }
    const QString fromEnv = qEnvironmentVariable("GEMINI_API_KEY").trimmed();
    if (!fromEnv.isEmpty()) {
        if (source)
            *source = QStringLiteral("GEMINI_API_KEY");
        return fromEnv;
    }

    QStringList dirs;
    if (!projectRoot.isEmpty())
        dirs << projectRoot;
    dirs << QDir::currentPath();
    QDir exe(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 4; ++i) { // build/ -> repository root, /usr/local/bin -> /usr/local ...
        dirs << exe.absolutePath();
        if (!exe.cdUp())
            break;
    }
    dirs << QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/QODE");

    for (const QString &dir : std::as_const(dirs)) {
        for (const QString &name : {QStringLiteral(".env.local"), QStringLiteral(".env")}) {
            QFile f(QDir(dir).filePath(name));
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            const QString v = parseValue(QString::fromUtf8(f.readAll()), key);
            if (!v.isEmpty()) {
                if (source)
                    *source = QFileInfo(f).absoluteFilePath();
                return v;
            }
        }
    }
    return {};
}

QString storedKeyPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/QODE/gemini.key");
}

bool hasStoredKey()
{
    return QFileInfo::exists(storedKeyPath());
}

bool saveKey(const QString &key, QString *error)
{
    const QString path = storedKeyPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    // QSaveFile creates its temporary file with the final mode, so the key is never readable by others, not even briefly.
    f.setDirectWriteFallback(false);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(key.trimmed().toUtf8());
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool clearKey()
{
    return !hasStoredKey() || QFile::remove(storedKeyPath());
}

bool isSensitive(const QString &path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    if (name == QLatin1String(".env") || name.startsWith(QLatin1String(".env.")) || name.endsWith(QLatin1String(".env")))
        return !(name.endsWith(QLatin1String(".example")) || name.endsWith(QLatin1String(".sample")) || name.endsWith(QLatin1String(".template")));
    static const QStringList exact = {QStringLiteral("id_rsa"), QStringLiteral("id_dsa"), QStringLiteral("id_ecdsa"), QStringLiteral("id_ed25519"),
                                      QStringLiteral(".npmrc"), QStringLiteral(".pypirc"), QStringLiteral(".netrc"), QStringLiteral(".git-credentials"),
                                      QStringLiteral("credentials"), QStringLiteral("credentials.json"), QStringLiteral("secrets.json"),
                                      QStringLiteral("secrets.yml"), QStringLiteral("secrets.yaml"), QStringLiteral("service-account.json"),
                                      QStringLiteral("gemini.key"), QStringLiteral("google-services.json"), QStringLiteral("keystore.properties")};
    if (exact.contains(name))
        return true;
    static const QStringList suffixes = {QStringLiteral(".pem"), QStringLiteral(".key"), QStringLiteral(".p12"), QStringLiteral(".pfx"),
                                         QStringLiteral(".jks"), QStringLiteral(".keystore"), QStringLiteral(".kdbx"), QStringLiteral(".ppk")};
    for (const QString &s : suffixes)
        if (name.endsWith(s))
            return true;
    return false;
}

} // namespace AiEnv
