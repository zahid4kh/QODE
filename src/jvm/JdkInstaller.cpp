#include "jvm/JdkInstaller.h"

#include "jvm/JdkScanner.h"
#include "platform/AppPaths.h"
#include "platform/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace {
constexpr int kNetworkTimeoutMs = 30000;

QString adoptiumOs()
{
    const QString tag = Platform::osTag();
    if (tag == QLatin1String("win"))
        return QStringLiteral("windows");
    return tag == QLatin1String("linux") ? QStringLiteral("linux") : QStringLiteral("mac");
}

QString adoptiumArch()
{
    const QString cpu = QSysInfo::currentCpuArchitecture();
    if (cpu == QLatin1String("x86_64"))
        return QStringLiteral("x64");
    if (cpu.startsWith(QLatin1String("arm64")) || cpu == QLatin1String("aarch64"))
        return QStringLiteral("aarch64");
    return {};
}

QString javaIn(const QString &home)
{
    return home + QStringLiteral("/bin/java") + Platform::exeSuffix();
}

QString replyError(QNetworkReply *r)
{
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return status >= 400 ? QObject::tr("HTTP %1 from %2").arg(status).arg(r->url().host()) : r->errorString();
}
} // namespace

QString JdkInstaller::defaultRoot()
{
    // Portable QODE (AppPaths::isPortable) writes nothing outside its own folder.
    return AppPaths::isPortable() ? AppPaths::data(QStringLiteral("jdks")) : QDir::homePath() + QStringLiteral("/.jdks");
}

QString JdkInstaller::recordFile()
{
    return AppPaths::data(QStringLiteral("downloaded-jdks.txt"));
}

QStringList JdkInstaller::downloaded()
{
    QStringList out;
    QFile file(recordFile());
    if (!file.open(QIODevice::ReadOnly))
        return out;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QString home = QDir::fromNativeSeparators(QString::fromUtf8(line).trimmed());
        if (!home.isEmpty() && QFileInfo(home).isDir()
            && std::none_of(out.cbegin(), out.cend(), [&home](const QString &h) { return Platform::samePath(h, home); }))
            out << home;
    }
    return out;
}

namespace {
void recordDownload(const QString &home)
{
    QStringList all = JdkInstaller::downloaded();
    if (std::any_of(all.cbegin(), all.cend(), [&home](const QString &h) { return Platform::samePath(h, home); }))
        return;
    all << home;
    QDir().mkpath(QFileInfo(JdkInstaller::recordFile()).absolutePath());
    QSaveFile file(JdkInstaller::recordFile());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(all.join(QLatin1Char('\n')).toUtf8() + '\n');
        file.commit();
    }
}
} // namespace

JdkInstaller::JdkInstaller(QObject *parent) : QObject(parent)
{
    m_net.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy); // GitHub release assets redirect
}

JdkInstaller::~JdkInstaller()
{
    if (m_active)
        cancel();
}

QString JdkInstaller::workDir() const
{
    return m_root + QStringLiteral("/.qode-download-%1").arg(m_major);
}

void JdkInstaller::start(int major, const QString &root)
{
    if (m_active)
        return;
    m_major = major;
    m_root = QDir::cleanPath(root);
    m_link.clear();
    m_expectedHash.clear();
    m_home.clear();
    m_active = true;
    if (adoptiumArch().isEmpty()) {
        abort(tr("Eclipse Temurin is published for x86-64 and ARM64 only (this is %1).").arg(QSysInfo::currentCpuArchitecture()));
        return;
    }
    Platform::removeTree(workDir()); // leftovers of an interrupted run
    if (!QDir().mkpath(workDir())) {
        abort(tr("Could not create %1").arg(QDir::toNativeSeparators(workDir())));
        return;
    }
    lookUp();
}

void JdkInstaller::cancel()
{
    if (!m_active)
        return;
    m_active = false;
    if (m_reply)
        m_reply->abort();
    if (m_proc) {
        m_proc->disconnect(this);
        Platform::killTree(m_proc);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_file.close();
    Platform::removeTree(workDir());
    emit progress(-1, 0);
    emit failed(tr("Cancelled"));
}

void JdkInstaller::abort(const QString &error)
{
    m_active = false;
    if (m_reply)
        m_reply->abort();
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_file.close();
    Platform::removeTree(workDir());
    emit progress(-1, 0);
    emit failed(error);
}

QNetworkReply *JdkInstaller::get(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(kNetworkTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QODE"));
    m_reply = m_net.get(req);
    return m_reply;
}

void JdkInstaller::lookUp()
{
    emit stageChanged(tr("Looking up the newest Eclipse Temurin %1…").arg(m_major));
    emit progress(0, 0);
    QUrl url(QStringLiteral("https://api.adoptium.net/v3/assets/latest/%1/hotspot").arg(m_major));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("architecture"), adoptiumArch());
    q.addQueryItem(QStringLiteral("image_type"), QStringLiteral("jdk"));
    q.addQueryItem(QStringLiteral("os"), adoptiumOs());
    q.addQueryItem(QStringLiteral("vendor"), QStringLiteral("eclipse"));
    url.setQuery(q);
    QNetworkReply *r = get(url);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (!m_active)
            return;
        if (r->error() != QNetworkReply::NoError) {
            abort(tr("Could not look up the release at api.adoptium.net: %1").arg(replyError(r)));
            return;
        }
        const QJsonArray list = QJsonDocument::fromJson(r->readAll()).array();
        const QJsonObject asset = list.isEmpty() ? QJsonObject() : list.first().toObject();
        const QJsonObject pkg = asset.value(QStringLiteral("binary")).toObject().value(QStringLiteral("package")).toObject();
        m_link = pkg.value(QStringLiteral("link")).toString();
        m_name = pkg.value(QStringLiteral("name")).toString();
        m_size = qint64(pkg.value(QStringLiteral("size")).toDouble());
        m_expectedHash = pkg.value(QStringLiteral("checksum")).toString().toLower();
        // "21.0.13+9" -> folder temurin-21.0.13 (IntelliJ's naming)
        m_version = asset.value(QStringLiteral("version")).toObject().value(QStringLiteral("openjdk_version")).toString();
        const QString plain = m_version.section(QLatin1Char('+'), 0, 0).section(QLatin1Char('-'), 0, 0);
        static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
        static const QRegularExpression safeName(QStringLiteral("^[\\w.+-]+\\.(zip|tar\\.gz)$"));
        if (m_link.isEmpty() || !QUrl(m_link).scheme().startsWith(QLatin1String("https")) || !hex.match(m_expectedHash).hasMatch()
            || !safeName.match(m_name).hasMatch() || !QRegularExpression(QStringLiteral("^\\d[\\d.]*$")).match(plain).hasMatch()) {
            abort(tr("Adoptium has no Eclipse Temurin %1 JDK for %2 / %3.").arg(m_major).arg(adoptiumOs(), adoptiumArch()));
            return;
        }
        m_home = m_root + QStringLiteral("/temurin-") + plain;
        if (JdkScanner::fromHome(m_home).isValid()) { // that release is already there
            Platform::removeTree(workDir());
            m_active = false;
            emit progress(-1, 0);
            emit finished(m_home);
            return;
        }
        const QStorageInfo disk(m_root);
        const qint64 need = qMax<qint64>(m_size, 100LL << 20) * 4; // archive + unpacked copy + slack
        if (disk.isValid() && disk.bytesAvailable() < need) {
            abort(tr("Not enough free disk space: about %1 are needed, %2 available.")
                      .arg(QLocale().formattedDataSize(need), QLocale().formattedDataSize(disk.bytesAvailable())));
            return;
        }
        download();
    });
}

void JdkInstaller::download()
{
    emit stageChanged(tr("Downloading %1…").arg(m_name));
    m_file.setFileName(workDir() + QLatin1Char('/') + m_name);
    if (!m_file.open(QIODevice::WriteOnly)) {
        abort(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(m_file.fileName()), m_file.errorString()));
        return;
    }
    m_hash.reset();
    QNetworkReply *r = get(QUrl(m_link));
    connect(r, &QNetworkReply::readyRead, this, [this, r] {
        if (!m_active || r->error() != QNetworkReply::NoError)
            return;
        const QByteArray chunk = r->readAll();
        m_hash.addData(chunk);
        if (m_file.write(chunk) != chunk.size())
            abort(tr("Could not write the download: %1").arg(m_file.errorString()));
    });
    connect(r, &QNetworkReply::downloadProgress, this, [this](qint64 done, qint64 total) {
        if (m_active)
            emit progress(done, total > 0 ? total : m_size);
    });
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (!m_active)
            return;
        if (r->error() == QNetworkReply::NoError) { // what readyRead has not taken yet
            const QByteArray rest = r->readAll();
            m_hash.addData(rest);
            m_file.write(rest);
        }
        m_file.close();
        if (r->error() != QNetworkReply::NoError) {
            abort(tr("Download failed: %1").arg(replyError(r)));
            return;
        }
        emit progress(-1, 0);
        emit stageChanged(tr("Verifying checksum…"));
        if (QString::fromLatin1(m_hash.result().toHex()) != m_expectedHash) {
            abort(tr("The downloaded file is corrupt (checksum mismatch). Try again."));
            return;
        }
        extract();
    });
}

void JdkInstaller::extract()
{
    emit stageChanged(tr("Unpacking…"));
    emit progress(0, 0);
    const QString out = workDir() + QStringLiteral("/unpacked");
    QDir().mkpath(out);
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (m_active && e == QProcess::FailedToStart)
            abort(tr("Could not run tar to unpack the JDK."));
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_active)
            return;
        const QString output = Platform::decodeOutput(m_proc->readAll()).trimmed();
        m_proc->deleteLater();
        m_proc = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
            abort(tr("Unpacking failed: %1").arg(output.isEmpty() ? tr("tar exited with code %1").arg(code) : output));
            return;
        }
        finalize();
    });
    Platform::setUnpackCommand(m_proc, m_file.fileName(), out); // Windows: System32\tar.exe, which also reads .zip
    m_proc->start();
}

void JdkInstaller::finalize()
{
    emit stageChanged(tr("Setting up…"));
    const QDir unpacked(workDir() + QStringLiteral("/unpacked"));
    // The archive holds one folder, jdk-21.0.13+9 (with bin/java).
    QString top;
    for (const QString &d : unpacked.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo::exists(javaIn(unpacked.filePath(d))))
            top = unpacked.filePath(d);
    if (top.isEmpty()) {
        abort(tr("The archive does not have the expected layout (no %1).").arg(QStringLiteral("bin/java") + Platform::exeSuffix()));
        return;
    }
    if (QFileInfo::exists(m_home)) // a broken earlier copy (fromHome found no JDK there)
        Platform::removeTree(m_home);
    if (QFileInfo::exists(m_home) || !QDir().rename(top, m_home)) {
        abort(tr("Could not move the JDK to %1").arg(QDir::toNativeSeparators(m_home)));
        return;
    }
    Platform::removeTree(workDir());
    smokeTest();
}

void JdkInstaller::smokeTest()
{
    emit stageChanged(tr("Testing the JDK…"));
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    auto *timeout = new QTimer(m_proc);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, m_proc, &QProcess::kill);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (m_active && e == QProcess::FailedToStart)
            abort(tr("The JDK was installed in %1 but java does not start on this system.").arg(QDir::toNativeSeparators(m_home)));
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_active)
            return;
        const QString output = Platform::decodeOutput(m_proc->readAll()).trimmed();
        m_proc->deleteLater();
        m_proc = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
            abort(tr("The JDK was installed in %1 but does not run: %2").arg(QDir::toNativeSeparators(m_home), output));
            return;
        }
        recordDownload(m_home);
        m_active = false;
        emit progress(-1, 0);
        emit finished(m_home);
    });
    m_proc->start(javaIn(m_home), {QStringLiteral("-version")});
    timeout->start(60000);
}
