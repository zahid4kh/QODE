#include "JdtlsInstaller.h"

#include "LspServers.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QUrl>
#include <QVersionNumber>

#include <unistd.h>

namespace {
const char *kPinnedVersion = "1.61.0";
const char *kPinnedArchive = "jdt-language-server-1.61.0-202609031315.tar.gz";
// SHA-256 of the pinned archive, checked in addition to the checksum Eclipse publishes next to it.
const char *kPinnedSha256 = "338e7e73d61836651ba2453919a0d34fa763eb4e7c03342092309bffb8934c64";
const char *kMilestones = "https://download.eclipse.org/jdtls/milestones";
constexpr qint64 kNeededBytes = 400LL * 1024 * 1024; // archive (~50 MB) + unpacked copy + slack
constexpr int kNetworkTimeoutMs = 30000;

// Replaces `link` with a symlink to `target` in one step (a half-written link is never visible).
bool replaceSymlink(const QString &target, const QString &link)
{
    const QString tmp = link + QStringLiteral(".new");
    ::unlink(QFile::encodeName(tmp).constData());
    if (::symlink(QFile::encodeName(target).constData(), QFile::encodeName(tmp).constData()) != 0)
        return false;
    if (::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(link).constData()) != 0) {
        ::unlink(QFile::encodeName(tmp).constData());
        return false;
    }
    return true;
}

QString replyError(QNetworkReply *r)
{
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return status >= 400 ? QObject::tr("HTTP %1 from %2").arg(status).arg(r->url().host()) : r->errorString();
}
} // namespace

// --- Locations ----------------------------------------------------------------------------------

QString JdtlsInstaller::pinnedVersion()
{
    return QString::fromLatin1(kPinnedVersion);
}

QString JdtlsInstaller::installRoot()
{
    return LspServers::managedDir(QStringLiteral("jdtls"));
}

QString JdtlsInstaller::installedVersion()
{
    const QFileInfo cur(installRoot() + QStringLiteral("/current"));
    return cur.isSymLink() ? QFileInfo(cur.symLinkTarget()).fileName() : QString();
}

QString JdtlsInstaller::workDir() const
{
    return installRoot() + QStringLiteral("/.work");
}

QUrl JdtlsInstaller::versionUrl(const QString &file) const
{
    return QUrl(QStringLiteral("%1/%2/%3").arg(QString::fromLatin1(kMilestones), m_version, file));
}

// --- Flow ------------------------------------------------------------------------------------------

JdtlsInstaller::JdtlsInstaller(QObject *parent) : QObject(parent)
{
    m_net.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

JdtlsInstaller::~JdtlsInstaller()
{
    if (m_active)
        cancel();
}

void JdtlsInstaller::start(bool latest)
{
    if (m_active)
        return;
    m_version = pinnedVersion();
    m_archive = QString::fromLatin1(kPinnedArchive);
    m_expectedHash.clear();
    m_note.clear();
    m_active = true;
    if (!QDir().mkpath(workDir())) {
        abort(tr("Could not create %1").arg(workDir()));
        return;
    }
    QDir(workDir()).removeRecursively(); // leftovers of an interrupted run
    QDir().mkpath(workDir());
    if (latest)
        resolveLatest();
    else
        fetchChecksum();
}

void JdtlsInstaller::cancel()
{
    if (!m_active)
        return;
    m_active = false; // handlers of the pieces killed below see this and stay quiet
    if (m_reply)
        m_reply->abort();
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(500);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_file.close();
    QDir(workDir()).removeRecursively();
    emit failed(tr("Cancelled"));
}

void JdtlsInstaller::abort(const QString &error)
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
    QDir(workDir()).removeRecursively();
    emit progress(-1, 0);
    emit failed(error);
}

QNetworkReply *JdtlsInstaller::get(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(kNetworkTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QODE"));
    m_reply = m_net.get(req);
    return m_reply;
}

// The newest folder of the milestones listing, then the archive's name from that folder's latest.txt.
void JdtlsInstaller::resolveLatest()
{
    emit stageChanged(tr("Looking up the latest release…"));
    QNetworkReply *r = get(QUrl(QString::fromLatin1(kMilestones) + QLatin1Char('/')));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (!m_active)
            return;
        QVersionNumber best;
        if (r->error() == QNetworkReply::NoError) {
            const QString html = QString::fromUtf8(r->readAll());
            QRegularExpressionMatchIterator it = QRegularExpression(QStringLiteral("/jdtls/milestones/(\\d+\\.\\d+\\.\\d+)'")).globalMatch(html);
            while (it.hasNext())
                best = qMax(best, QVersionNumber::fromString(it.next().captured(1)));
        }
        if (best.isNull()) {
            m_note = tr("Could not look up the latest release (%1); installed the tested version instead.").arg(replyError(r));
            fetchChecksum();
            return;
        }
        m_version = best.toString();
        fetchFileName();
    });
}

void JdtlsInstaller::fetchFileName()
{
    if (m_version == pinnedVersion()) {
        fetchChecksum();
        return;
    }
    QNetworkReply *r = get(versionUrl(QStringLiteral("latest.txt")));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (!m_active)
            return;
        const QString name = QString::fromUtf8(r->readAll()).trimmed();
        if (r->error() != QNetworkReply::NoError || !QRegularExpression(QStringLiteral("^jdt-language-server-[\\w.\\-]+\\.tar\\.gz$")).match(name).hasMatch()) {
            m_note = tr("Could not look up the latest release (%1); installed the tested version instead.").arg(replyError(r));
            m_version = pinnedVersion();
            m_archive = QString::fromLatin1(kPinnedArchive);
        } else {
            m_archive = name;
        }
        fetchChecksum();
    });
}

void JdtlsInstaller::fetchChecksum()
{
    emit stageChanged(tr("Fetching checksum for version %1…").arg(m_version));
    QNetworkReply *r = get(versionUrl(m_archive + QStringLiteral(".sha256")));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (!m_active)
            return;
        if (r->error() != QNetworkReply::NoError) {
            abort(tr("Could not fetch the checksum: %1").arg(replyError(r)));
            return;
        }
        const auto m = QRegularExpression(QStringLiteral("^([0-9a-fA-F]{64})")).match(QString::fromLatin1(r->readAll()).trimmed());
        if (!m.hasMatch()) {
            abort(tr("The published checksum file is not in the expected format."));
            return;
        }
        m_expectedHash = m.captured(1).toLower();
        if (m_version == pinnedVersion() && m_expectedHash != QLatin1String(kPinnedSha256)) {
            abort(tr("The published checksum for the tested version no longer matches what QODE expects. Refusing to install."));
            return;
        }
        const QStorageInfo disk(workDir());
        if (disk.isValid() && disk.bytesAvailable() < kNeededBytes) {
            abort(tr("Not enough free disk space: about %1 are needed, %2 available.")
                      .arg(QLocale().formattedDataSize(kNeededBytes), QLocale().formattedDataSize(disk.bytesAvailable())));
            return;
        }
        download();
    });
}

void JdtlsInstaller::download()
{
    emit stageChanged(tr("Downloading %1…").arg(m_archive));
    m_file.setFileName(workDir() + QLatin1Char('/') + m_archive);
    if (!m_file.open(QIODevice::WriteOnly)) {
        abort(tr("Could not write %1: %2").arg(m_file.fileName(), m_file.errorString()));
        return;
    }
    m_hash.reset();
    QNetworkReply *r = get(versionUrl(m_archive));
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
            emit progress(done, qMax<qint64>(total, 0));
    });
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (!m_active)
            return;
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

void JdtlsInstaller::extract()
{
    emit stageChanged(tr("Unpacking…"));
    emit progress(0, 0); // busy indicator
    const QString out = workDir() + QStringLiteral("/unpacked");
    QDir().mkpath(out);
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (m_active && e == QProcess::FailedToStart)
            abort(tr("Could not run tar. Install it with your package manager."));
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_active)
            return;
        const QString output = QString::fromLocal8Bit(m_proc->readAll()).trimmed();
        m_proc->deleteLater();
        m_proc = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
            abort(tr("Unpacking failed: %1").arg(output.isEmpty() ? tr("tar exited with code %1").arg(code) : output));
            return;
        }
        emit progress(-1, 0);
        finalize();
    });
    m_proc->start(QStringLiteral("tar"), {QStringLiteral("-xzf"), m_file.fileName(), QStringLiteral("-C"), out});
}

void JdtlsInstaller::finalize()
{
    emit stageChanged(tr("Setting up…"));
    const QDir unpacked(workDir() + QStringLiteral("/unpacked"));
    // The archive holds bin/, plugins/ and config_*/ directly (no top-level folder).
    if (!unpacked.exists(QStringLiteral("bin/jdtls")) || unpacked.entryList({QStringLiteral("config_linux")}, QDir::Dirs).isEmpty() ||
        QDir(unpacked.filePath(QStringLiteral("plugins"))).entryList({QStringLiteral("org.eclipse.equinox.launcher_*.jar")}, QDir::Files).isEmpty()) {
        abort(tr("The archive does not have the expected layout (no bin/jdtls, config_linux or equinox launcher)."));
        return;
    }
    const QString target = installRoot() + QLatin1Char('/') + m_version;
    QDir(target).removeRecursively();
    if (!QDir().rename(unpacked.absolutePath(), target)) {
        abort(tr("Could not move the server to %1").arg(target));
        return;
    }
    QFile launcher(target + QStringLiteral("/bin/jdtls"));
    launcher.setPermissions(launcher.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    QFile::remove(m_file.fileName());
    if (!replaceSymlink(m_version, installRoot() + QStringLiteral("/current"))) {
        abort(tr("Could not create the link %1/current").arg(installRoot()));
        return;
    }
    // Older versions are no longer needed once the new one is in place.
    const QDir root(installRoot());
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (name != m_version && QRegularExpression(QStringLiteral("^\\d[\\d.]*$")).match(name).hasMatch())
            QDir(root.filePath(name)).removeRecursively();
    QDir(workDir()).removeRecursively();

    const LspServers::JavaRuntime java = LspServers::findJava(21);
    if (java.executable.isEmpty())
        m_note += (java.major > 0 ? tr("The newest Java found is %1, but the server needs Java 21 or newer. ").arg(java.major)
                                  : tr("No Java was found, and the server needs Java 21 or newer. ")) +
                  tr("Install a JDK 21+ (LSP > Java > How to Install) before using it.");
    m_active = false;
    emit progress(-1, 0);
    emit finished(LspServers::managedExecutable(*LspServers::byId(QStringLiteral("java"))), m_note.trimmed());
}
