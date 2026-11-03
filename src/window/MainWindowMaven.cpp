// The Maven panel in the right-hand island: lifecycle, plugins, dependency tree, effective POM.
#include "MainWindow.h"

#include "app/StartupProfile.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "maven/MavenPanel.h"
#include "maven/MavenProject.h"
#include "media/MarkdownPreview.h"
#include "media/MediaPanel.h"
#include "project/ProjectManager.h"

#include <QAction>
#include <QStackedWidget>
#include <QTimer>

void MainWindow::setupMaven()
{
    // A saved pom reloads the panel when it is showing (else on the next show).
    connect(m_editors, &EditorManager::documentSaved, this, [this](Document *) {
        if (rightPanelShows(m_maven))
            QTimer::singleShot(0, m_maven, &MavenPanel::ensureLoaded);
    });
}

// Created the first time it is shown.
MavenPanel *MainWindow::mavenPanel()
{
    if (m_maven)
        return m_maven;
    StartupProfile::Later timing(QStringLiteral("Maven panel"));
    m_maven = new MavenPanel(this);
    m_rightStack->addWidget(m_maven);
    connect(m_maven, &MavenPanel::closeRequested, this, &MainWindow::hideMedia);
    connect(m_maven, &MavenPanel::runRequested, this, [this](const QString &command, const QString &dir) {
        if (!m_editors->saveAll())
            return;
        runInTerminal(command, dir);
    });
    connect(m_maven, &MavenPanel::openFileRequested, this, [this](const QString &path, int line) {
        m_editorHiddenByUser = false;
        if (line > 0 ? m_editors->openFileAt(path, line) : m_editors->openFile(path)) {
            setEditorColumnVisible(true);
            m_editors->focusEditor();
        }
    });
    m_maven->setBuildDir(m_projects->hasProject() ? MavenProject::buildDir(m_projects->project().root) : QString());
    return m_maven;
}

// The Maven button exists only in Maven projects (a pom.xml at the root or in a direct sub-folder).
void MainWindow::updateMavenUi()
{
    if (!m_mavenAct)
        return;
    const QString dir = m_projects->hasProject() ? MavenProject::buildDir(m_projects->project().root) : QString();
    m_mavenAct->setVisible(!dir.isEmpty());
    if (m_maven)
        m_maven->setBuildDir(dir);
    if (dir.isEmpty() && rightPanelShows(m_maven))
        hideMedia();
}

void MainWindow::toggleMavenPanel()
{
    if (rightPanelShows(m_maven)) {
        hideMedia();
        return;
    }
    showInRightPanel(mavenPanel());
    m_maven->ensureLoaded();
}
