#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>

class QNetworkReply;
class QProcess;

// Downloads a JDK (Eclipse Temurin, from the Adoptium API) into ~/.jdks/temurin-<version>, the folder IntelliJ uses and
// where JdkScanner, Gradle and the Kotlin language server look for JDKs too: look up the newest release of a major
// version -> download the archive (.zip on Windows, .tar.gz elsewhere) -> check its SHA-256 against the one Adoptium
// publishes -> unpack -> run `java -version`. No admin rights, nothing outside that folder; asynchronous.
class JdkInstaller : public QObject
{
    Q_OBJECT
public:
    static QString defaultRoot(); // ~/.jdks

    // The JDKs QODE itself downloaded that are still there, from <data>/downloaded-jdks.txt (one folder per line):
    // ~/.jdks is shared with IntelliJ and Gradle, so Help > QODE's Files and the Windows uninstaller offer exactly
    // these, never a JDK somebody else put there. A release that was already in place when asked for is not recorded.
    static QStringList downloaded();
    static QString recordFile();

    explicit JdkInstaller(QObject *parent = nullptr);
    ~JdkInstaller() override;

    void start(int major, const QString &root = defaultRoot());
    void cancel();
    bool isRunning() const { return m_active; }

signals:
    void stageChanged(const QString &text);
    void progress(qint64 done, qint64 total); // total 0 = busy without a size, -1 = no download in progress
    void finished(const QString &home);      // the new JDK's folder (also when that release was already there)
    void failed(const QString &error);

private:
    QNetworkReply *get(const QUrl &url);
    void lookUp();
    void download();
    void extract();
    void finalize();
    void smokeTest();
    void abort(const QString &error);
    QString workDir() const;

    QNetworkAccessManager m_net;
    QPointer<QNetworkReply> m_reply;
    QProcess *m_proc = nullptr;
    QFile m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_root, m_link, m_name, m_expectedHash, m_version, m_home;
    qint64 m_size = 0;
    int m_major = 0;
    bool m_active = false;
};
