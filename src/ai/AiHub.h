#pragma once

#include <QWidget>

class AiPanel;
class EditorManager;
class QStackedWidget;
class QTabBar;

namespace agent {
class AgentPanel;
}

// The AI side panel: two tabs that share nothing but the API key and the model setting.
//   Chat   - plan-and-edit (AiPanel): Gemini plans, returns edits, QODE applies them
//   Agent  - function calling (AgentPanel): Gemini explores, edits, runs commands and verifies on its own
class AiHub : public QWidget
{
    Q_OBJECT
public:
    explicit AiHub(QWidget *parent = nullptr);

    void setEditors(EditorManager *editors);
    void setProject(const QString &root);
    void setGitRepo(bool isRepo);
    void setProjectFiles(const QStringList &absolutePaths);
    void focusInput();
    void editApiKey();

signals:
    void closeRequested();
    void openFileRequested(const QString &absolute);
    void gitInitRequested();

private:
    void currentTabChanged(int index);

    QTabBar *m_tabs;
    QStackedWidget *m_stack;
    AiPanel *m_chat;
    agent::AgentPanel *m_agent;
    QStringList m_files;
};
