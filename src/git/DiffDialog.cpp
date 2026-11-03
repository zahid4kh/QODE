#include "DiffDialog.h"

#include "DiffPane.h"
#include "GitRepository.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

// --- DiffDialog -------------------------------------------------------------------------------

DiffDialog::DiffDialog(GitRepository *repo, const QString &absPath, GitDiffMode mode, QWidget *parent)
    : QDialog(parent, Qt::Window)
    , m_repo(repo)
    , m_path(absPath)
    , m_mode(mode)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1180, 720);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_stats = new QLabel(this);
    m_stats->setObjectName(QStringLiteral("emptyText"));
    m_prev = new QPushButton(tr("▲ Previous"), this);
    m_next = new QPushButton(tr("▼ Next"), this);
    m_action = new QPushButton(this);
    m_action->setVisible(mode != GitDiffMode::Head);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(8, 6, 8, 6);
    bar->addWidget(m_stats, 1);
    bar->addWidget(m_prev);
    bar->addWidget(m_next);
    bar->addWidget(m_action);

    m_pane = new DiffPane(this);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_title);
    root->addLayout(bar);
    root->addWidget(m_pane, 1);

    connect(m_prev, &QPushButton::clicked, this, [this] { m_pane->gotoChange(false); });
    connect(m_next, &QPushButton::clicked, this, [this] { m_pane->gotoChange(true); });
    connect(m_action, &QPushButton::clicked, this, [this] {
        if (m_mode == GitDiffMode::Staged)
            m_repo->unstage({m_path});
        else
            m_repo->stage({m_path});
    });
    connect(m_repo, &GitRepository::statusChanged, this, &DiffDialog::load);

    const QString name = QFileInfo(m_path).fileName();
    const QString modeText = mode == GitDiffMode::Staged ? tr("Staged Changes") : mode == GitDiffMode::Unstaged ? tr("Changes") : tr("Changes vs HEAD");
    setWindowTitle(tr("%1 (%2)").arg(name, modeText));
    m_title->setText(tr("%1  —  %2").arg(m_repo->relativePath(m_path), modeText));
    m_action->setText(mode == GitDiffMode::Staged ? tr("Unstage File") : tr("Stage File"));
    m_pane->setTitles(mode == GitDiffMode::Unstaged ? tr("INDEX") : tr("HEAD"), mode == GitDiffMode::Staged ? tr("INDEX (STAGED)") : tr("WORKING TREE"));
    load();
}

void DiffDialog::load()
{
    const QString rel = m_repo->relativePath(m_path);
    const GitFileChange *c = m_repo->changeFor(m_path);
    const QString headRel = c && !c->origRelPath.isEmpty() ? c->origRelPath : rel;
    const bool conflict = c && c->conflicted;

    QString oldSpec, newSpec;
    switch (m_mode) {
    case GitDiffMode::Unstaged: oldSpec = conflict ? QStringLiteral("HEAD:") + rel : QStringLiteral(":") + rel; break;
    case GitDiffMode::Staged:   oldSpec = QStringLiteral("HEAD:") + headRel; newSpec = QStringLiteral(":") + rel; break;
    case GitDiffMode::Head:     oldSpec = QStringLiteral("HEAD:") + headRel; break;
    }

    m_pending = 2;
    m_binary = false;
    auto done = [this] {
        if (--m_pending == 0)
            populate(m_oldText, m_newText);
    };
    auto decode = [this](const QByteArray &b, QString &out) {
        if (b.left(8000).contains('\0'))
            m_binary = true;
        out = QString::fromUtf8(b);
    };
    m_oldText.clear();
    m_newText.clear();
    m_repo->readBlob(oldSpec, this, [=](const GitRepository::Result &r) {
        if (r.ok())
            decode(r.out, m_oldText);
        done();
    });
    if (newSpec.isEmpty()) {
        QFile f(m_path);
        if (f.open(QIODevice::ReadOnly))
            decode(f.readAll(), m_newText);
        done();
    } else {
        m_repo->readBlob(newSpec, this, [=](const GitRepository::Result &r) {
            if (r.ok())
                decode(r.out, m_newText);
            done();
        });
    }
}

void DiffDialog::populate(const QString &oldText, const QString &newText)
{
    if (m_binary) {
        m_pane->setMessage(tr("Binary file"));
        m_stats->setText(tr("Binary files cannot be compared."));
        m_prev->setEnabled(false);
        m_next->setEnabled(false);
        return;
    }
    const DiffPane::Stats st = m_pane->setTexts(oldText, newText);
    m_stats->setText(st.hunks == 0 ? tr("No differences.") : tr("%n change(s):  +%1  −%2", nullptr, st.hunks).arg(st.added).arg(st.removed));
    m_prev->setEnabled(st.hunks > 0);
    m_next->setEnabled(st.hunks > 0);
}
