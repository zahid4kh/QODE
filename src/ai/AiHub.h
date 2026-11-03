#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QWidget>
#include <functional>

class LspManager;
class EditorManager;

class QStackedWidget;

namespace agent {
class AgentPanel;
}
namespace claudecode {
class ClaudeCodePanel;
}

// The chat area, with two pages: the QODE chat (AgentPanel), which explores the project, edits files, runs commands and
// verifies its work on its own with Gemini or Claude models, and Claude Code (ClaudeCodePanel), a graphical front end
// for the `claude` CLI. The top bar's two buttons switch between them.
class AiHub : public QWidget
{
    Q_OBJECT
public:
    enum class Page { Qode, ClaudeCode };

    explicit AiHub(QWidget *parent = nullptr);

    Page page() const;
    void setPage(Page page); // remembered for the next start

    void setEditors(EditorManager *editors);
    void setProject(const QString &root);
    void setGitRepo(bool isRepo);
    void setGitDirty(bool dirty); // uncommitted changes (Claude Code suggests a review)
    void setProjectFiles(const QStringList &absolutePaths);
    void setLsp(LspManager *lsp);
    // Runs the chat's IDE tools (MainWindow::ideAction).
    void setIdeHandler(std::function<QJsonObject(const QString &action, const QJsonObject &args)> handler);
    void focusInput();
    void editApiKey();
    void showMcpServers(); // the QODE chat's MCP servers dialog
    // "Fix with QODE" (AgentPanel::startFix), on the QODE chat page.
    void startFix(const QString &display, const QString &request, const QStringList &files, const QString &quote, const QString &quoteLead);

signals:
    void openFileRequested(const QString &absolute, int line = 0);
    void gitInitRequested();
    void projectChanged(); // the agent or Claude Code changed files on disk
    void pageChanged(AiHub::Page page);
    void claudeCodeRequested(); // a Claude Code notification was clicked

private:
    // The Claude Code page is created when first shown (it is not needed to start): until then the hub keeps what it
    // would have been told.
    claudecode::ClaudeCodePanel *claudeCode();

    QStackedWidget *m_pages;
    agent::AgentPanel *m_agent;
    claudecode::ClaudeCodePanel *m_claudeCode = nullptr;
    EditorManager *m_editors = nullptr;
    QString m_root;
    bool m_gitDirty = false;
    QStringList m_files;
};
