#pragma once

#include <QObject>
#include <QStringList>

class QProcess;

// Sets up the web language servers (TypeScript / JavaScript, HTML, CSS, JSON, plus the ESLint and Tailwind CSS companions) with `npm install --prefix`
// into ~/.local/share/QODE/lsp/web: nothing global, no root needed, and removing the folder removes everything.
// Needs Node.js with npm on the system; the servers are Node scripts.
class NpmInstaller : public QObject
{
    Q_OBJECT
public:
    static QString installRoot(); // ~/.local/share/QODE/lsp/web
    static QStringList packages();
    // The npm command shown to the user, e.g. "npm install --prefix ~/.local/share/QODE/lsp/web typescript …".
    static QString commandLine();
    static bool isInstalled();

    explicit NpmInstaller(QObject *parent = nullptr);
    ~NpmInstaller() override;

    void start();
    void cancel();
    bool isRunning() const { return m_proc != nullptr; }

signals:
    void output(const QString &text); // npm's output as it arrives
    void finished();
    void failed(const QString &error);

private:
    void abort(const QString &error);

    QProcess *m_proc = nullptr;
};
