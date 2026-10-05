#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>

class QNetworkReply;
class QProcess;

// Downloads and sets up the Eclipse JDT language server (jdtls) from download.eclipse.org:
// resolve version -> checksum -> download -> verify -> unpack -> link. Everything lives under
// ~/.local/share/QODE/lsp/jdtls (no root needed) and runs asynchronously. The server itself needs a Java 21+ runtime
// on the machine; that is checked (and reported in the result note) but not installed.
class JdtlsInstaller : public QObject
{
    Q_OBJECT
public:
    // The milestone known to work with QODE; "latest" is an opt-in.
    static QString pinnedVersion();
    static QString installRoot();      // ~/.local/share/QODE/lsp/jdtls
    static QString installedVersion(); // what `current` points to, "" when nothing is installed

    explicit JdtlsInstaller(QObject *parent = nullptr);
    ~JdtlsInstaller() override;

    void start(bool latest);
    void cancel();
    bool isRunning() const { return m_active; }

signals:
    void stageChanged(const QString &text);
    void progress(qint64 done, qint64 total); // total 0 = not known, -1 = no download in progress
    void finished(const QString &executable, const QString &note);
    void failed(const QString &error);

private:
    QNetworkReply *get(const QUrl &url);
    void resolveLatest();
    void fetchFileName();
    void fetchChecksum();
    void download();
    void extract();
    void finalize();
    void abort(const QString &error);
    QString workDir() const;
    QUrl versionUrl(const QString &file = QString()) const;

    QNetworkAccessManager m_net;
    QPointer<QNetworkReply> m_reply;
    QProcess *m_proc = nullptr;
    QFile m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_version, m_archive, m_expectedHash, m_note;
    bool m_active = false;
};
