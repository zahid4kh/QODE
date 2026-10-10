#include "LspInstaller.h"

#include "LspServers.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>

#include <unistd.h>

namespace {
const char *kPinnedVersion = "263.6379.0";
// SHA-256 of the pinned x86_64 archive, checked in addition to the checksum JetBrains publishes next to it.
const char *kPinnedSha256X64 = "ab8ca4455dc2fc5fe1a24db2bccc46c104254d2c465155c4251ee65df8f3f7cc";
constexpr qint64 kNeededBytes = 1800LL * 1024 * 1024; // archive (~370 MB) + unpacked copy (~1.2 GB) + slack
constexpr int kNetworkTimeoutMs = 30000;

bool isArm()
{
    return QSysInfo::currentCpuArchitecture().startsWith(QLatin1String("arm64")) ||
           QSysInfo::currentCpuArchitecture() == QLatin1String("aarch64");
}

bool isIntel()
{
    return QSysInfo::currentCpuArchitecture() == QLatin1String("x86_64");
}

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

void makeExecutable(const QString &path)
{
    QFile f(path);
    if (f.exists())
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
}
} // namespace

// --- Locations ----------------------------------------------------------------------------------

QString LspInstaller::pinnedVersion()
{
    return QString::fromLatin1(kPinnedVersion);
}

QString LspInstaller::installRoot()
{
    return LspServers::managedDir(QStringLiteral("kotlin"));
}

QString LspInstaller::linkPath()
{
    return QDir::homePath() + QStringLiteral("/.local/bin/kotlin-lsp");
}

QString LspInstaller::installedVersion()
{
    const QFileInfo cur(installRoot() + QStringLiteral("/current"));
    return cur.isSymLink() ? QFileInfo(cur.symLinkTarget()).fileName() : QString();
}

bool LspInstaller::isManaged(const QString &path)
{
    const QString real = QFileInfo(path).canonicalFilePath();
    const QString root = QFileInfo(installRoot()).canonicalFilePath();
    return !real.isEmpty() && !root.isEmpty() && real.startsWith(root + QLatin1Char('/'));
}

bool LspInstaller::remove(QString *error)
{
    const QFileInfo link(linkPath());
    if (link.isSymLink() && isManaged(link.symLinkTarget()))
        QFile::remove(link.filePath());
    QDir dir(installRoot());
    if (dir.exists() && !dir.removeRecursively()) {
        if (error)
            *error = QObject::tr("Could not delete %1").arg(installRoot());
        return false;
    }
    return true;
}

QString LspInstaller::workDir() const
{
    return installRoot() + QStringLiteral("/.work");
}

QString LspInstaller::archiveName() const
{
    return QStringLiteral("kotlin-server-%1%2.tar.gz").arg(m_version, isArm() ? QStringLiteral("-aarch64") : QString());
}

QUrl LspInstaller::archiveUrl() const
{
    return QUrl(QStringLiteral("https://download.jetbrains.com/language-server/kotlin-server/%1/%2").arg(m_version, archiveName()));
}

// --- Flow ------------------------------------------------------------------------------------------

LspInstaller::LspInstaller(QObject *parent) : QObject(parent)
{
    m_net.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

LspInstaller::~LspInstaller()
{
    if (m_active)
        cancel();
}

void LspInstaller::start(bool latest, bool createLink)
{
    if (m_active)
        return;
    m_latest = latest;
    m_createLink = createLink;
    m_version = pinnedVersion();
    m_expectedHash.clear();
    m_note.clear();
    m_active = true;
    if (!isIntel() && !isArm()) {
        abort(tr("The Kotlin language server is only published for x86-64 and ARM64 Linux (this is %1). Download it by hand "
                 "from https://github.com/Kotlin/kotlin-lsp/releases and use LSP > Set Server Path.")
                  .arg(QSysInfo::currentCpuArchitecture()));
        return;
    }
    if (!QDir().mkpath(workDir())) {
        abort(tr("Could not create %1").arg(workDir()));
        return;
    }
    QDir(workDir()).removeRecursively(); // leftovers of an interrupted run
    QDir().mkpath(workDir());
    resolveVersion();
}

void LspInstaller::cancel()
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

void LspInstaller::abort(const QString &error)
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

QNetworkReply *LspInstaller::get(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(kNetworkTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QODE"));
    m_reply = m_net.get(req);
    return m_reply;
}

static QString replyError(QNetworkReply *r)
{
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return status >= 400 ? QObject::tr("HTTP %1 from %2").arg(status).arg(r->url().host()) : r->errorString();
}

void LspInstaller::resolveVersion()
{
    if (!m_latest) {
        fetchChecksum();
        return;
    }
    emit stageChanged(tr("Looking up the latest release…"));
    QNetworkReply *r = get(QUrl(QStringLiteral("https://api.github.com/repos/Kotlin/kotlin-lsp/releases/latest")));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (!m_active)
            return;
        // Tag looks like "kotlin-lsp/v263.4702.0".
        const QString tag = QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("tag_name")).toString();
        const auto m = QRegularExpression(QStringLiteral("(\\d+(?:\\.\\d+)+)$")).match(tag);
        if (r->error() == QNetworkReply::NoError && m.hasMatch()) {
            m_version = m.captured(1);
        } else {
            m_note = tr("Could not look up the latest release (%1); installed the tested version instead.").arg(replyError(r));
            m_version = pinnedVersion();
        }
        fetchChecksum();
    });
}

void LspInstaller::fetchChecksum()
{
    emit stageChanged(tr("Fetching checksum for version %1…").arg(m_version));
    QNetworkReply *r = get(QUrl(archiveUrl().toString() + QStringLiteral(".sha256")));
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
        if (m_version == pinnedVersion() && isIntel() && m_expectedHash != QLatin1String(kPinnedSha256X64)) {
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

void LspInstaller::download()
{
    emit stageChanged(tr("Downloading %1…").arg(archiveName()));
    m_file.setFileName(workDir() + QLatin1Char('/') + archiveName());
    if (!m_file.open(QIODevice::WriteOnly)) {
        abort(tr("Could not write %1: %2").arg(m_file.fileName(), m_file.errorString()));
        return;
    }
    m_hash.reset();
    QNetworkReply *r = get(archiveUrl());
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

void LspInstaller::extract()
{
    emit stageChanged(tr("Unpacking (this takes a moment)…"));
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

void LspInstaller::finalize()
{
    emit stageChanged(tr("Setting up…"));
    const QDir unpacked(workDir() + QStringLiteral("/unpacked"));
    const QStringList tops = unpacked.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (tops.size() != 1 || !QFileInfo::exists(unpacked.filePath(tops.first() + QStringLiteral("/bin/intellij-server")))) {
        abort(tr("The archive does not have the expected layout (no bin/intellij-server)."));
        return;
    }
    const QString target = installRoot() + QLatin1Char('/') + m_version;
    QDir(target).removeRecursively();
    if (!QDir().rename(unpacked.filePath(tops.first()), target)) {
        abort(tr("Could not move the server to %1").arg(target));
        return;
    }
    makeExecutable(target + QStringLiteral("/bin/intellij-server"));
    makeExecutable(target + QStringLiteral("/jbr/bin/java"));
    QFile::remove(m_file.fileName());

    if (!replaceSymlink(m_version, installRoot() + QStringLiteral("/current"))) {
        abort(tr("Could not create the link %1/current").arg(installRoot()));
        return;
    }
    if (m_createLink) {
        const QFileInfo link(linkPath());
        QDir().mkpath(link.absolutePath());
        // Only ever replace our own symlink, never a file or a link somebody else made.
        if (link.exists() || link.isSymLink()) {
            if (!link.isSymLink() || !isManaged(link.symLinkTarget()))
                m_note += tr("%1 already exists and was left unchanged. ").arg(linkPath());
            else
                replaceSymlink(installRoot() + QStringLiteral("/current/bin/intellij-server"), linkPath());
        } else if (!replaceSymlink(installRoot() + QStringLiteral("/current/bin/intellij-server"), linkPath())) {
            m_note += tr("Could not create %1. ").arg(linkPath());
        }
        const QStringList path = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
        if (!path.contains(link.absolutePath()))
            m_note += tr("%1 is not on your PATH, so \"kotlin-lsp\" will not work in a terminal until you add it (QODE itself does not need that). ")
                          .arg(link.absolutePath());
    }
    smokeTest();
}

// The launcher must run on this machine (glibc, runtime): ask it for its version.
void LspInstaller::smokeTest()
{
    emit stageChanged(tr("Testing the server…"));
    emit progress(0, 0);
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    auto *timeout = new QTimer(m_proc);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, m_proc, &QProcess::kill);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (m_active && e == QProcess::FailedToStart)
            abort(tr("The server does not start on this system."));
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_active)
            return;
        const QString output = QString::fromLocal8Bit(m_proc->readAll()).trimmed();
        m_proc->deleteLater();
        m_proc = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
            abort(tr("The server was installed but does not run: %1").arg(output.isEmpty() ? tr("exit code %1").arg(code) : output));
            return;
        }
        succeed();
    });
    m_proc->start(LspServers::managedExecutable(*LspServers::byId(QStringLiteral("kotlin"))), {QStringLiteral("--version")});
    timeout->start(60000);
}

void LspInstaller::succeed()
{
    // Older versions are no longer needed once the new one runs.
    const QDir root(installRoot());
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (name != m_version && QRegularExpression(QStringLiteral("^\\d[\\d.]*$")).match(name).hasMatch())
            QDir(root.filePath(name)).removeRecursively();
    QDir(workDir()).removeRecursively();
    m_active = false;
    emit progress(-1, 0);
    emit finished(LspServers::managedExecutable(*LspServers::byId(QStringLiteral("kotlin"))), m_note.trimmed());
}
