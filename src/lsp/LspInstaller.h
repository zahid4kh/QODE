#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>

class QNetworkReply;
class QProcess;

// Downloads and sets up the Kotlin language server from JetBrains' standalone Linux archive:
// resolve version -> checksum -> download -> verify -> unpack -> link -> test run. Everything lives under
// ~/.local/share/QODE/lsp/kotlin (no root needed) and runs asynchronously; the archive carries its own Java
// runtime, so the system needs no JDK for the server itself.
class LspInstaller : public QObject
{
    Q_OBJECT
public:
    // The release known to work with QODE; "latest" is an opt-in.
    static QString pinnedVersion();
    static QString installRoot();         // ~/.local/share/QODE/lsp/kotlin
    static QString linkPath();            // ~/.local/bin/kotlin-lsp
    static QString installedVersion();    // what `current` points to, "" when nothing is installed
    static bool isManaged(const QString &path); // true when `path` resolves into installRoot()
    // Deletes the managed install and the ~/.local/bin link when it points there.
    static bool remove(QString *error = nullptr);

    explicit LspInstaller(QObject *parent = nullptr);
    ~LspInstaller() override;

    void start(bool latest, bool createLink);
    void cancel();
    bool isRunning() const { return m_active; }

signals:
    void stageChanged(const QString &text);
    void progress(qint64 done, qint64 total); // total 0 = not known, -1 = no download in progress
    void finished(const QString &executable, const QString &note);
    void failed(const QString &error);

private:
    QNetworkReply *get(const QUrl &url);
    void resolveVersion();
    void fetchChecksum();
    void download();
    void extract();
    void finalize();
    void smokeTest();
    void succeed();
    void abort(const QString &error);
    QString workDir() const;
    QString archiveName() const;
    QUrl archiveUrl() const;

    QNetworkAccessManager m_net;
    QPointer<QNetworkReply> m_reply;
    QProcess *m_proc = nullptr;
    QFile m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_version, m_expectedHash, m_note;
    bool m_active = false;
    bool m_latest = false;
    bool m_createLink = true;
};
