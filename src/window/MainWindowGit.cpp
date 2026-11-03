// MainWindow's git side: the actions and menu, the repository's status, the editors' change gutters and blame, diffs.
#include "MainWindow.h"

#include "SideSections.h"
#include "ai/AiHub.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/ProjectExplorer.h"
#include "git/BranchButton.h"
#include "git/DiffDialog.h"
#include "git/GitDiff.h"
#include "git/GitPanel.h"
#include "git/GitRepository.h"
#include "git/PatchDialog.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAction>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

void MainWindow::createGitActions()
{
    using K = Qt::Key;
    const auto C = Qt::CTRL;
    const auto S = Qt::SHIFT;
    const auto A = Qt::ALT;
    auto make = [this](const QString &text, const QKeySequence &key = {}, const QString &icon = {}) {
        auto *a = new QAction(text, this);
        if (!key.isEmpty())
            a->setShortcut(key);
        if (!icon.isEmpty())
            Icons::bind(a, icon);
        return a;
    };
    m_scmAct = make(tr("Version Control"), QKeySequence(C | S | K::Key_G), QStringLiteral(":/new-icons/git-branch.svg"));
    m_gitRefreshAct = make(tr("Refresh Status"));
    m_gitFetchAct = make(tr("Fetch"));
    m_gitPullAct = make(tr("Pull"));
    m_gitPushAct = make(tr("Push"));
    m_gitInitAct = make(tr("Initialize Repository"));
    m_gitNewBranchAct = make(tr("Create Branch…"));
    m_gitStageFileAct = make(tr("Stage Current File"));
    m_gitUnstageFileAct = make(tr("Unstage Current File"));
    m_gitDiscardFileAct = make(tr("Discard Changes in Current File…"));
    m_gitDiffFileAct = make(tr("Show Changes in Current File"));
    m_nextChangeAct = make(tr("Next Change"), QKeySequence(A | K::Key_F3));
    m_prevChangeAct = make(tr("Previous Change"), QKeySequence(S | A | K::Key_F3));
    for (QAction *a : {m_nextChangeAct, m_prevChangeAct}) {
        a->setShortcutContext(Qt::WindowShortcut);
        addAction(a);
    }

    connect(m_scmAct, &QAction::triggered, this, &MainWindow::showSourceControl);
    connect(m_gitRefreshAct, &QAction::triggered, m_git, &GitRepository::refresh);
    connect(m_gitFetchAct, &QAction::triggered, m_git, &GitRepository::fetch);
    connect(m_gitPullAct, &QAction::triggered, m_git, &GitRepository::pull);
    connect(m_gitPushAct, &QAction::triggered, m_git, &GitRepository::push);
    connect(m_gitInitAct, &QAction::triggered, m_git, &GitRepository::init);
    connect(m_gitNewBranchAct, &QAction::triggered, this, [this] { GitPanel::promptNewBranch(m_git, this); });
    connect(m_gitStageFileAct, &QAction::triggered, this, [this] { m_git->stage({currentFilePath()}); });
    connect(m_gitUnstageFileAct, &QAction::triggered, this, [this] { m_git->unstage({currentFilePath()}); });
    connect(m_gitDiscardFileAct, &QAction::triggered, this, [this] { discardPaths({currentFilePath()}); });
    connect(m_gitDiffFileAct, &QAction::triggered, this, &MainWindow::showChangesForCurrentFile);
    connect(m_nextChangeAct, &QAction::triggered, this, [this] { if (auto *e = m_editors->currentEditor()) e->gotoChange(true); });
    connect(m_prevChangeAct, &QAction::triggered, this, [this] { if (auto *e = m_editors->currentEditor()) e->gotoChange(false); });
}

void MainWindow::createGitMenu(QMenu *menu)
{
    menu->addAction(m_scmAct);
    menu->addAction(m_gitRefreshAct);
    menu->addSeparator();
    menu->addAction(m_gitDiffFileAct);
    menu->addAction(m_gitStageFileAct);
    menu->addAction(m_gitUnstageFileAct);
    menu->addAction(m_gitDiscardFileAct);
    menu->addSeparator();
    menu->addAction(m_blameGutterAct);
    menu->addAction(m_blameInlineAct);
    menu->addSeparator();
    menu->addAction(m_nextChangeAct);
    menu->addAction(m_prevChangeAct);
    menu->addSeparator();
    QMenu *branches = menu->addMenu(tr("Switch Branch"));
    connect(branches, &QMenu::aboutToShow, this, [this, branches] {
        branches->clear();
        if (m_git->isRepo())
            GitPanel::populateBranchMenu(m_git, branches, this);
    });
    menu->addAction(m_gitNewBranchAct);
    menu->addSeparator();
    menu->addAction(m_gitFetchAct);
    menu->addAction(m_gitPullAct);
    menu->addAction(m_gitPushAct);
    menu->addSeparator();
    menu->addAction(m_gitInitAct);
}

void MainWindow::setupGit()
{
    connect(m_gitPanel, &GitPanel::openFileRequested, m_editors, &EditorManager::openFile);
    connect(m_gitPanel, &GitPanel::diffRequested, this, &MainWindow::showDiff);

    connect(m_explorer, &ProjectExplorer::gitStageRequested, m_git, &GitRepository::stage);
    connect(m_explorer, &ProjectExplorer::gitUnstageRequested, m_git, &GitRepository::unstage);
    connect(m_explorer, &ProjectExplorer::gitDiscardRequested, this, &MainWindow::discardPaths);
    connect(m_explorer, &ProjectExplorer::gitDiffRequested, this, &MainWindow::showDiff);
    connect(m_explorer, &ProjectExplorer::gitIgnoreRequested, m_git, &GitRepository::addToGitignore);
    connect(m_explorer, &ProjectExplorer::contentsChanged, m_git, &GitRepository::scheduleRefresh);

    connect(m_git, &GitRepository::statusChanged, this, &MainWindow::onGitStatusChanged);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { onGitStatusChanged(); });
    connect(m_git, &GitRepository::repositoryChanged, this, [this] { updateActions(); });
    connect(m_git, &GitRepository::errorOccurred, this, [this](const QString &title, const QString &detail) {
        QMessageBox::warning(this, tr("Git — %1").arg(title), detail);
    });
    connect(m_git, &GitRepository::operationFinished, this, [this](const QString &title, const QString &output) {
        const QString first = output.section(QLatin1Char('\n'), 0, 0).trimmed();
        statusBar()->showMessage(first.isEmpty() ? tr("%1 completed").arg(title) : tr("%1: %2").arg(title, first), 6000);
    });

    // Change markers in the gutter follow the committed text of each open file.
    connect(m_editors, &EditorManager::documentAdded, this, &MainWindow::refreshGutter);
    connect(m_editors, &EditorManager::documentSaved, this, &MainWindow::refreshGutter);
    connect(m_editors, &EditorManager::blameCommitRequested, this, [this](const QString &hash) {
        m_git->commitPatch(hash, this, [this, hash](const QString &text) {
            auto *dlg = new PatchDialog(tr("Commit %1").arg(hash.left(8)), text, this);
            dlg->show();
        });
    });
    connect(m_editors, &EditorManager::documentPathChanged, this, &MainWindow::refreshGutter);
    connect(m_editors, &EditorManager::documentStateChanged, m_git, &GitRepository::scheduleRefresh);

    // Pick up changes made outside the editor (terminal, other tools).
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState st) {
        if (st == Qt::ApplicationActive)
            m_git->scheduleRefresh();
    });
    auto *poll = new QTimer(this);
    poll->setInterval(10000);
    connect(poll, &QTimer::timeout, this, [this] {
        if (isActiveWindow() && !m_git->busy())
            m_git->scheduleRefresh();
    });
    poll->start();

    onGitStatusChanged();
}

void MainWindow::onGitStatusChanged()
{
    m_ai->setGitRepo(m_git->isRepo());
    m_ai->setGitDirty(m_git->isRepo() && !m_git->changes().isEmpty());
    // Status bar branch indicator
    if (m_git->isRepo()) {
        const QString name = m_git->branch().isEmpty() ? tr("(no branch)") : m_git->branch();
        m_branchButton->setLabel(m_git->isDetached() ? tr("%1 (detached)").arg(name) : name);
        const Theme t = Theme::byName(SettingsManager::instance().theme());
        m_aheadBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/arrow-up.svg"), t.textMuted));
        m_behindBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/arrow-down.svg"), t.textMuted));
        m_aheadBtn->setText(QString::number(m_git->ahead()));
        m_behindBtn->setText(QString::number(m_git->behind()));
        m_aheadBtn->setVisible(m_git->ahead() > 0);
        m_behindBtn->setVisible(m_git->behind() > 0);
        const int changes = m_git->changes().size();
        m_changesPill->setText(QString::number(changes));
        m_changesPill->setToolTip(tr("%n changed file(s)", nullptr, changes));
        m_changesPill->setVisible(changes > 0);
        m_gitStatusWidget->setVisible(statusBar()->currentMessage().isEmpty()); // a message owns the left side
    } else {
        m_gitStatusWidget->hide();
    }
    const int n = m_git->isRepo() ? m_git->changes().size() : 0;
    m_side->setTitle(1, n > 0 ? tr("Version Control (%1)").arg(n) : tr("Version Control"));

    // Committed text changed (new commit, other branch, other repository) => re-read every open file's base.
    if (m_git->headOid() != m_lastHead || m_git->root() != m_lastRoot) {
        m_lastHead = m_git->headOid();
        m_lastRoot = m_git->root();
        refreshAllGutters();
    }
    updateActions();
}

void MainWindow::refreshAllGutters()
{
    for (Document *d : m_editors->documents())
        refreshGutter(d);
}

void MainWindow::refreshGutter(Document *doc)
{
    CodeEditor *ed = m_editors->editorFor(doc);
    if (!ed)
        return;
    const QString path = doc->filePath();
    const QString rel = path.isEmpty() || !m_git->isRepo() ? QString() : m_git->relativePath(path);
    if (rel.isEmpty() || m_git->isIgnored(path) || doc->isLarge()) { // a large file would be diffed after every edit
        ed->clearDiffBase();
        ed->clearBlame();
        return;
    }
    m_git->blame(rel, ed->toPlainText().toUtf8(), ed, [ed](const QVector<GitBlameLine> &lines) {
        if (lines.isEmpty())
            ed->clearBlame();
        else
            ed->setBlame(lines);
    });
    const GitFileChange *c = m_git->changeFor(path);
    const QString headRel = c && !c->origRelPath.isEmpty() ? c->origRelPath : rel;
    m_git->readBlob(QStringLiteral("HEAD:") + headRel, ed, [ed](const GitRepository::Result &r) {
        // A file that is not in HEAD yet is entirely "added".
        ed->setDiffBase(r.ok() ? GitDiff::splitLines(QString::fromUtf8(r.out)) : QStringList());
    });
}

void MainWindow::showSourceControl()
{
    if (!m_explorerAct->isChecked())
        m_explorerAct->setChecked(true);
    m_side->setExpanded(1, true);
}

void MainWindow::showDiff(const QString &path, GitDiffMode mode)
{
    const QString key = QString::number(static_cast<int>(mode)) + path;
    if (DiffDialog *existing = m_diffs.value(key)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dlg = new DiffDialog(m_git, path, mode, this);
    m_diffs.insert(key, dlg);
    dlg->show();
}

void MainWindow::showChangesForCurrentFile()
{
    const QString path = currentFilePath();
    if (path.isEmpty())
        return;
    const GitFileChange *c = m_git->changeFor(path);
    GitDiffMode mode = GitDiffMode::Head;
    if (c)
        mode = c->isStaged() && !c->isUnstaged() ? GitDiffMode::Staged : GitDiffMode::Unstaged;
    showDiff(path, mode);
}

void MainWindow::discardPaths(const QStringList &paths)
{
    QList<GitFileChange> changes;
    for (const QString &p : paths)
        if (const GitFileChange *c = m_git->changeFor(p))
            if (c->isUnstaged() || c->untracked)
                changes << *c;
    if (GitPanel::confirmDiscard(changes, this))
        m_git->discard(paths);
}

// --- Colours, rename, references --------------------------------------------------------------------
