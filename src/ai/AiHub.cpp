#include "AiHub.h"

#include "agent/AgentPanel.h"
#include "ai/AiPanel.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

AiHub::AiHub(QWidget *parent)
    : QWidget(parent)
    , m_tabs(new QTabBar(this))
    , m_stack(new QStackedWidget(this))
    , m_chat(new AiPanel(this))
    , m_agent(new agent::AgentPanel(this))
{
    m_tabs->addTab(tr("Chat"));
    m_tabs->addTab(tr("Agent"));
    m_tabs->setTabToolTip(0, tr("Plan and edit: Gemini plans the change, returns the edits and QODE applies them"));
    m_tabs->setTabToolTip(1, tr("Agent: Gemini explores the project, edits files, runs commands and checks its work on its own"));
    m_tabs->setExpanding(true);
    m_tabs->setDrawBase(false);
    m_tabs->setDocumentMode(true);
    m_tabs->setCursor(Qt::PointingHandCursor);
    m_chat->setTitle(tr("PLAN & EDIT"));
    m_stack->addWidget(m_chat);
    m_stack->addWidget(m_agent);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_tabs);
    lay->addWidget(m_stack, 1);

    connect(m_agent, &agent::AgentPanel::projectChanged, this, &AiHub::projectChanged);
    connect(m_tabs, &QTabBar::currentChanged, this, &AiHub::currentTabChanged);
    for (QObject *panel : {static_cast<QObject *>(m_chat), static_cast<QObject *>(m_agent)}) {
        connect(panel, SIGNAL(closeRequested()), this, SIGNAL(closeRequested()));
        connect(panel, SIGNAL(openFileRequested(QString)), this, SIGNAL(openFileRequested(QString)));
        connect(panel, SIGNAL(gitInitRequested()), this, SIGNAL(gitInitRequested()));
    }
}

void AiHub::currentTabChanged(int index)
{
    m_stack->setCurrentIndex(index);
    if (index == 1)
        m_agent->focusInput();
    else
        m_chat->focusInput();
}

void AiHub::setEditors(EditorManager *editors)
{
    m_chat->setEditors(editors);
    m_agent->setEditors(editors);
}

void AiHub::setProject(const QString &root)
{
    m_chat->setProject(root);
    m_agent->setProject(root);
}

void AiHub::setGitRepo(bool isRepo)
{
    m_chat->setGitRepo(isRepo);
    m_agent->setGitRepo(isRepo);
}

void AiHub::setLsp(LspManager *lsp)
{
    m_agent->setLsp(lsp);
}

void AiHub::setProjectFiles(const QStringList &absolutePaths)
{
    m_chat->setProjectFiles(absolutePaths);
    m_agent->setProjectFiles(absolutePaths);
}

void AiHub::focusInput()
{
    if (m_tabs->currentIndex() == 1)
        m_agent->focusInput();
    else
        m_chat->focusInput();
}

void AiHub::editApiKey()
{
    if (m_tabs->currentIndex() == 1)
        m_agent->editApiKey();
    else
        m_chat->editApiKey();
}
