#include "AiHub.h"

#include "agent/AgentPanel.h"
#include "app/StartupProfile.h"
#include "claudecode/ClaudeCodePanel.h"
#include "settings/SettingsManager.h"

#include <QStackedWidget>
#include <QVBoxLayout>

AiHub::AiHub(QWidget *parent)
    : QWidget(parent)
    , m_pages(new QStackedWidget(this))
    , m_agent(new agent::AgentPanel(this))
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_pages, 1);
    m_pages->addWidget(m_agent);
    if (SettingsManager::instance().chatPage() == QLatin1String("claudeCode"))
        m_pages->setCurrentWidget(claudeCode());

    connect(m_agent, &agent::AgentPanel::projectChanged, this, &AiHub::projectChanged);
    connect(m_agent, &agent::AgentPanel::openFileRequested, this, &AiHub::openFileRequested);
    connect(m_agent, &agent::AgentPanel::gitInitRequested, this, &AiHub::gitInitRequested);
}

claudecode::ClaudeCodePanel *AiHub::claudeCode()
{
    if (m_claudeCode)
        return m_claudeCode;
    StartupProfile::Later timing(QStringLiteral("Claude Code view"));
    m_claudeCode = new claudecode::ClaudeCodePanel(this);
    m_pages->addWidget(m_claudeCode);
    connect(m_claudeCode, &claudecode::ClaudeCodePanel::projectChanged, this, &AiHub::projectChanged);
    connect(m_claudeCode, &claudecode::ClaudeCodePanel::openFileRequested, this, &AiHub::openFileRequested);
    connect(m_claudeCode, &claudecode::ClaudeCodePanel::showRequested, this, &AiHub::claudeCodeRequested);
    if (m_editors)
        m_claudeCode->setEditors(m_editors);
    if (!m_root.isEmpty())
        m_claudeCode->setProject(m_root);
    m_claudeCode->setGitDirty(m_gitDirty);
    if (!m_files.isEmpty())
        m_claudeCode->setProjectFiles(m_files);
    return m_claudeCode;
}

AiHub::Page AiHub::page() const
{
    return m_claudeCode && m_pages->currentWidget() == m_claudeCode ? Page::ClaudeCode : Page::Qode;
}

void AiHub::setPage(Page page)
{
    if (page == this->page())
        return;
    m_pages->setCurrentWidget(page == Page::ClaudeCode ? static_cast<QWidget *>(claudeCode()) : m_agent);
    SettingsManager::instance().setChatPage(page == Page::ClaudeCode ? QStringLiteral("claudeCode") : QStringLiteral("qode"));
    emit pageChanged(page);
}

void AiHub::setEditors(EditorManager *editors)
{
    m_editors = editors;
    m_agent->setEditors(editors);
    if (m_claudeCode)
        m_claudeCode->setEditors(editors);
}

void AiHub::setProject(const QString &root)
{
    m_root = root;
    m_agent->setProject(root);
    if (m_claudeCode)
        m_claudeCode->setProject(root);
}

void AiHub::setGitRepo(bool isRepo)
{
    m_agent->setGitRepo(isRepo);
}

void AiHub::setGitDirty(bool dirty)
{
    m_gitDirty = dirty;
    if (m_claudeCode)
        m_claudeCode->setGitDirty(dirty);
}

void AiHub::setLsp(LspManager *lsp)
{
    m_agent->setLsp(lsp);
}

void AiHub::setIdeHandler(std::function<QJsonObject(const QString &, const QJsonObject &)> handler)
{
    m_agent->setIdeHandler(std::move(handler));
}

void AiHub::setProjectFiles(const QStringList &absolutePaths)
{
    m_files = absolutePaths;
    m_agent->setProjectFiles(absolutePaths);
    if (m_claudeCode)
        m_claudeCode->setProjectFiles(absolutePaths);
}

void AiHub::focusInput()
{
    if (page() == Page::ClaudeCode)
        m_claudeCode->focusInput();
    else
        m_agent->focusInput();
}

void AiHub::editApiKey()
{
    m_agent->editApiKey();
}

void AiHub::showMcpServers()
{
    m_agent->showMcpServers();
}

void AiHub::startFix(const QString &display, const QString &request, const QStringList &files, const QString &quote, const QString &quoteLead)
{
    setPage(Page::Qode);
    m_agent->startFix(display, request, files, quote, quoteLead);
}
