#pragma once

#include <QObject>
#include <QStringList>

class QProcess;

// Sets up language servers that are npm packages with `npm install --prefix` into ~/.local/share/QODE-AGENTIC/lsp/<managedId>:
// nothing global, no root needed, and removing the folder removes everything. Two installs exist: "web" (TypeScript /
// JavaScript, HTML, CSS, JSON, plus the ESLint and Tailwind CSS companions) and "bash" (bash-language-server).
// Needs Node.js with npm on the system; the servers are Node scripts.
class NpmInstaller : public QObject
{
    Q_OBJECT
public:
    static QString installRoot(const QString &managedId); // ~/.local/share/QODE-AGENTIC/lsp/<managedId>
    static QStringList packages(const QString &managedId);
    // The npm command shown to the user, e.g. "npm install --prefix ~/.local/share/QODE-AGENTIC/lsp/web typescript …".
    static QString commandLine(const QString &managedId);
    static bool isInstalled(const QString &managedId);

    explicit NpmInstaller(const QString &managedId, QObject *parent = nullptr);
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

    QString m_id;
    QProcess *m_proc = nullptr;
};
