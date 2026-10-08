#pragma once

#include "python/PythonTools.h"

#include <QList>
#include <QObject>
#include <QProcess>
#include <QStringList>



// Sets up the Python language tools (basedpyright, plus the ruff companion) in a private virtual environment,
// ~/.local/share/QODE/lsp/python: nothing global, no root needed, and removing the folder removes everything. Uses
// uv when it is installed (and falls back to python3 -m venv + pip if uv fails), plain pip otherwise.
class PipInstaller : public QObject
{
    Q_OBJECT
public:
    static QString installRoot();      // ~/.local/share/QODE/lsp/python
    static QStringList packages();     // basedpyright, ruff
    static bool usesUv();              // uv is installed, so it does the work
    static bool isInstalled();
    static QString installedVersion(); // basedpyright's version, "" when unknown
    // The commands the installer will run, as shown to the user (home folder shortened).
    static QStringList plannedCommands();

    explicit PipInstaller(QObject *parent = nullptr);
    ~PipInstaller() override;

    void start();
    void cancel();
    bool isRunning() const { return m_proc != nullptr; }

signals:
    void output(const QString &text); // the commands and their output as they arrive
    void finished();
    void failed(const QString &error);

private:
    QList<PythonTools::Command> buildSteps(bool useUv) const;
    void runNext();
    void stepFinished(int code, QProcess::ExitStatus status);
    void abort(const QString &error);
    void fail(const QString &error);

    QList<PythonTools::Command> m_steps;
    int m_next = 0;
    bool m_useUv = false;
    bool m_createdVenv = false; // this run created the folder, so a failed attempt may delete it before retrying
    bool m_retriedWithPip = false;
    QString m_collected; // output of the current step, for diagnosing the failure
    QProcess *m_proc = nullptr;
};
