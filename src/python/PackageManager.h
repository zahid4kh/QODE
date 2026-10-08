#pragma once

#include "PythonTools.h"

#include <QList>
#include <QObject>
#include <QString>

class QProcess;

// Adds, removes and lists the packages of one Python virtual environment by running pip (or uv, when installed) against
// the environment's own interpreter. Works the same whether or not a shell has activated the environment. At most one
// change runs at a time; listing runs on its own so it never blocks one. Qt Core only.
class PackageManager : public QObject
{
    Q_OBJECT
public:
    explicit PackageManager(QObject *parent = nullptr);
    ~PackageManager() override;

    // `venv` is the environment's root-relative folder name inside `projectRoot`.
    void setEnvironment(const QString &projectRoot, const QString &venv);
    QString projectRoot() const { return m_root; }
    QString venv() const { return m_venv; }
    QString python() const; // the environment's interpreter, "" when it has none
    bool usesUv() const { return m_useUv; }
    QString tool() const { return m_useUv ? QStringLiteral("uv") : QStringLiteral("pip"); }
    bool isBusy() const { return m_op != nullptr; }

    void refresh();                                           // installed packages, then the outdated ones
    void install(const QStringList &specs, bool upgrade = false);
    void uninstall(const QStringList &names);
    void installFromFile(const QString &file);                // requirements*.txt (-r) or pyproject.toml / setup.py (-e .)
    void createVenv(const QString &name);                     // inside the project root, then setEnvironment is up to the caller
    void cancel();

signals:
    void installedListed(const QList<PythonTools::Package> &packages);
    void outdatedListed(const QList<PythonTools::Package> &packages);
    void listFailed(const QString &error);
    void output(const QString &text);                  // commands ("$ …") and their output as they arrive
    void operationStarted(const QString &title);
    void operationFinished(bool ok, const QString &title, const QString &error);

private:
    void run(const PythonTools::Command &cmd, const QString &title);
    void listInto(bool outdated);

    QString m_root, m_venv;
    bool m_useUv = false;
    QProcess *m_op = nullptr;
    QProcess *m_list = nullptr;
    QString m_title;
    QString m_collected;
};
