// Ktor servers: the toolbar strip that runs the server, the right-hand panel (log, routes, requests, configuration),
// F5, Add Ktor Plugins and the configuration file's icon / completion.
#include "window/MainWindow.h"

#include "app/StartupProfile.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/FileIcons.h"
#include "explorer/ProjectExplorer.h"
#include "ktor/AddKtorPluginDialog.h"
#include "ktor/KtorBar.h"
#include "ktor/KtorPanel.h"
#include "ktor/KtorPlugins.h"
#include "lsp/LspManager.h"
#include "media/MarkdownPreview.h"
#include "media/MediaPanel.h"
#include "project/ProjectFiles.h"
#include "settings/SettingsManager.h"
#include "spring/SpringBar.h"
#include "spring/SpringLogView.h"
#include "webdev/DevServerBar.h"
#include "filesystem/FileManager.h"

#include <QAbstractItemView>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

void MainWindow::setupKtorPanel()
{
    // Routes and configuration follow the sources: a saved Kotlin file or configuration of the server is read again.
    connect(m_editors, &EditorManager::documentSaved, this, [this](Document *doc) {
        if (!ktorAvailable() || !doc->filePath().startsWith(m_ktorBar->project().moduleDir + QLatin1Char('/')))
            return;
        const QString name = QFileInfo(doc->filePath()).fileName();
        if (m_ktor && (name.endsWith(QLatin1String(".kt")) || name.startsWith(QLatin1String("application."))))
            m_ktor->sourcesChanged();
        m_ktorBar->sourceSaved(doc->filePath()); // Maven's auto-reload compiles on save
    });
}

// Created with the first Ktor project (updateKtorUi): it keeps the server's log from the start, shown or not.
KtorPanel *MainWindow::ktorPanel()
{
    if (m_ktor)
        return m_ktor;
    StartupProfile::Later timing(QStringLiteral("Ktor panel"));
    m_ktor = new KtorPanel(this);
    m_rightStack->addWidget(m_ktor);
    connect(m_ktor, &KtorPanel::closeRequested, this, &MainWindow::hideMedia);
    connect(m_ktor, &KtorPanel::openTextRequested, this, &MainWindow::openTextInEditor);
    connect(m_ktor, &KtorPanel::addPluginsRequested, this, &MainWindow::addKtorPlugins);
    connect(m_ktor, &KtorPanel::openFileRequested, this, [this](const QString &path) {
        m_editorHiddenByUser = false;
        if (m_editors->openFile(path))
            setEditorColumnVisible(true);
    });
    connect(m_ktor, &KtorPanel::locationRequested, this, [this](const QString &path, int line, int column) {
        m_editorHiddenByUser = false;
        if (m_editors->openFileAt(path, line, qMax(1, column)))
            setEditorColumnVisible(true);
    });
    // A stack frame "com.example.RoutingKt$configureRouting$1.invokeSuspend(Routing.kt:12)", or the class of a route
    // handler of the running server -> the project file.
    m_ktor->setResolver([this](const QString &pkgPath, const QString &fileName) -> QString {
        const QString suffix = QLatin1Char('/') + (pkgPath.isEmpty() ? fileName : pkgPath + QLatin1Char('/') + fileName);
        for (const QString &f : m_projectFiles->files())
            if (f.endsWith(suffix))
                return f;
        return {};
    });
    connect(m_projectFiles, &ProjectFiles::updated, m_ktor, [this] { m_ktor->log()->clearResolveCache(); });
    m_ktor->attach(m_ktorBar);
    return m_ktor;
}

void MainWindow::setupKtorBar(QToolBar *tb)
{
    m_ktorBar = new KtorBar(this);
    m_ktorBarAct = tb->addWidget(m_ktorBar);
    connect(m_ktorBar, &KtorBar::availabilityChanged, this, [this] {
        updateRunToolbar();
        updateActions();
        updateKtorUi();
    });
    connect(m_ktorBar, &KtorBar::stateChanged, this, &MainWindow::updateActions);
    connect(m_ktorBar, &KtorBar::panelRequested, this, &MainWindow::toggleKtorPanel);
    connect(m_ktorBar, &KtorBar::openFileRequested, this, [this](const QString &path) {
        m_editorHiddenByUser = false;
        if (m_editors->openFile(path))
            setEditorColumnVisible(true);
    });
    connect(m_ktorBar, &KtorBar::starting, this, [this] {
        if (!m_rightStack->parentWidget()->isVisible())
            toggleKtorPanel(); // show what the server prints, but never replace another open panel
    });
}

// A Spring Boot application wins when a build has both (it would own F5 and the toolbar anyway).
bool MainWindow::ktorAvailable() const
{
    return m_ktorBar && m_ktorBar->isAvailable() && !(m_springBar && m_springBar->isAvailable());
}

// The Ktor button, panel and plugin action exist only in Ktor projects; application.yaml / .conf get Ktor's icon there.
void MainWindow::updateKtorUi()
{
    const bool ktor = ktorAvailable();
    if (ktor)
        ktorPanel();
    m_ktorAct->setVisible(ktor);
    m_ktorPluginAct->setVisible(ktor);
    if (m_ktorBarAct)
        m_ktorBarAct->setVisible(ktor);
    if (FileIcons::setConfigBrand(ktor ? FileIcons::ConfigBrand::Ktor : FileIcons::ConfigBrand::Spring)) {
        for (QAbstractItemView *view : m_explorer->findChildren<QAbstractItemView *>())
            view->viewport()->update();
        m_editors->refreshTabIcons();
    }
    if (!ktor && rightPanelShows(m_ktor))
        hideMedia();
}

void MainWindow::toggleKtorPanel()
{
    if (rightPanelShows(m_ktor)) {
        hideMedia();
        return;
    }
    showInRightPanel(ktorPanel());
    m_projectFiles->ensureFresh(); // stack frames are matched against the project's files
}

// In a Ktor project F5 starts (or restarts) the server, unless the file belongs to a web front end with its own dev
// server.
bool MainWindow::ktorOwns(const QString &file) const
{
    if (!ktorAvailable())
        return false;
    if (!m_serverBar->isAvailable() || file.isEmpty())
        return true;
    return file.startsWith(m_ktorBar->project().moduleDir + QLatin1Char('/'));
}

void MainWindow::addKtorPlugins()
{
    if (!ktorAvailable())
        return;
    const KtorProject project = m_ktorBar->project();
    if (Document *open = m_editors->documentForPath(project.buildFile()); open && open->isModified()) {
        QMessageBox::information(this, tr("Add Ktor Plugins"), tr("%1 has unsaved changes. Save it first.").arg(QFileInfo(project.buildFile()).fileName()));
        return;
    }
    AddKtorPluginDialog dlg(project, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const KtorPlugins::Change change = KtorPlugins::plan(project, dlg.selectedIds());
    if (!change.ok) {
        QMessageBox::warning(this, tr("Add Ktor Plugins"), change.error);
        return;
    }
    for (const auto &extra : change.extra)
        if (Document *open = m_editors->documentForPath(extra.first); open && open->isModified()) {
            QMessageBox::information(this, tr("Add Ktor Plugins"), tr("%1 has unsaved changes. Save it first.").arg(QFileInfo(extra.first).fileName()));
            return;
        }
    // In the file's own encoding (a BOM or UTF-16 stays), written atomically with Windows' busy-file retries.
    QString error;
    if (!FileManager::writeText(change.file, change.text, &error)) {
        QMessageBox::warning(this, tr("Add Ktor Plugins"), tr("Could not write %1: %2").arg(change.file, error));
        return;
    }
    for (const auto &extra : change.extra)
        FileManager::writeText(extra.first, extra.second);
    m_editorHiddenByUser = false;
    if (m_editors->openFile(change.file))
        setEditorColumnVisible(true);
    statusBar()->showMessage(tr("Added %1 to %2").arg(change.added.join(QStringLiteral(", ")), QFileInfo(change.file).fileName()), 6000);
    if (!change.note.isEmpty())
        QMessageBox::information(this, tr("Add Ktor Plugins"), change.note);
    if (m_lsp->hasBuildFiles())
        m_lsp->syncBuildFiles(true);
    ktorPanel()->sourcesChanged();
    if (m_ktorBar->isActive())
        statusBar()->showMessage(tr("Added %1 — restart the server to use it").arg(change.added.join(QStringLiteral(", "))), 8000);
}
