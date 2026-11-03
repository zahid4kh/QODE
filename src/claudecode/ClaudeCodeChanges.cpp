#include "ClaudeCodeChanges.h"

#include "explorer/FileIcons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace claudecode {

namespace {
// A file row: the whole row is clickable.
class FileRow : public QFrame
{
public:
    FileRow(QWidget *parent, std::function<void()> onClick)
        : QFrame(parent)
        , m_onClick(std::move(onClick))
    {
        setObjectName(QStringLiteral("ccFileRow"));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()))
            m_onClick();
    }

private:
    std::function<void()> m_onClick;
};

QString counts(const FileChange &c, const Theme &t)
{
    QStringList parts;
    if (c.added || c.created)
        parts << QStringLiteral("<span style='color:%1'>+%2</span>").arg(t.gitAdded.name()).arg(c.added);
    if (c.removed)
        parts << QStringLiteral("<span style='color:%1'>−%2</span>").arg(t.gitDeleted.name()).arg(c.removed);
    return parts.join(QLatin1Char(' '));
}
} // namespace

void addChange(QVector<FileChange> &changes, const FileChange &change)
{
    for (FileChange &c : changes)
        if (c.path == change.path) {
            c.added += change.added;
            c.removed += change.removed;
            c.lines += change.lines;
            return;
        }
    changes.append(change);
}

ChangesCard::ChangesCard(const QVector<FileChange> &changes, QWidget *parent)
    : QFrame(parent)
    , m_changes(changes)
{
    setObjectName(QStringLiteral("ccChanges"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(4);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("ccCardTitle"));
    m_title->setTextFormat(Qt::RichText);
    m_reviewBtn = new QPushButton(tr("Review all"), this);
    m_reviewBtn->setObjectName(QStringLiteral("agentFooterBtn"));
    m_reviewBtn->setCursor(Qt::PointingHandCursor);
    m_reviewBtn->setToolTip(tr("Open every change in the editor, one after the other"));
    connect(m_reviewBtn, &QPushButton::clicked, this, [this] { go(0); });
    head->addWidget(m_title, 1);
    head->addWidget(m_reviewBtn);
    lay->addLayout(head);

    // Every change, in the order the files were first edited and top to bottom within a file.
    for (int i = 0; i < m_changes.size(); ++i) {
        QVector<int> lines = m_changes.at(i).lines;
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        if (lines.size() > 1)
            lines.removeAll(0);
        if (lines.isEmpty())
            lines << 0;
        for (int line : std::as_const(lines))
            m_stops.append({i, line});
        m_changes[i].lines = lines;
    }

    for (int i = 0; i < m_changes.size(); ++i) {
        const FileChange &c = m_changes.at(i);
        auto *row = new FileRow(this, [this, i] {
            m_index = -1;
            m_reviewBar->hide();
            m_reviewBtn->show();
            for (QWidget *r : std::as_const(m_rows))
                r->setProperty("current", false);
            applyTheme();
            emit openRequested(m_changes.at(i).path, m_changes.at(i).lines.first());
        });
        row->setToolTip(tr("Open %1 at its first change").arg(c.display));
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(6, 3, 6, 3);
        rl->setSpacing(6);
        auto *icon = new QLabel(row);
        icon->setFixedSize(14, 14);
        icon->setScaledContents(true);
        icon->setPixmap(FileIcons::forFile(QFileInfo(c.path).fileName()).pixmap(48, 48));
        auto *name = new QLabel(c.display, row);
        name->setTextFormat(Qt::PlainText);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *tag = new QLabel(c.created ? tr("new") : QString(), row);
        tag->setObjectName(QStringLiteral("aiHint"));
        auto *count = new QLabel(row);
        count->setTextFormat(Qt::RichText);
        rl->addWidget(icon);
        rl->addWidget(name, 1);
        rl->addWidget(tag);
        rl->addWidget(count);
        lay->addWidget(row);
        m_rows.append(row);
        m_counts.append(count);
    }

    m_reviewBar = new QWidget(this);
    auto *rb = new QHBoxLayout(m_reviewBar);
    rb->setContentsMargins(0, 4, 0, 0);
    rb->setSpacing(6);
    m_prev = new QPushButton(tr("‹ Previous"), m_reviewBar);
    m_next = new QPushButton(tr("Next ›"), m_reviewBar);
    auto *done = new QPushButton(tr("Done"), m_reviewBar);
    for (QPushButton *b : {m_prev, m_next, done}) {
        b->setObjectName(QStringLiteral("agentFooterBtn"));
        b->setCursor(Qt::PointingHandCursor);
    }
    m_reviewText = new QLabel(m_reviewBar);
    m_reviewText->setObjectName(QStringLiteral("aiHint"));
    m_reviewText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(m_prev, &QPushButton::clicked, this, [this] { go(m_index - 1); });
    connect(m_next, &QPushButton::clicked, this, [this] { go(m_index + 1); });
    connect(done, &QPushButton::clicked, this, [this] { go(-1); });
    rb->addWidget(m_prev);
    rb->addWidget(m_next);
    rb->addWidget(m_reviewText, 1);
    rb->addWidget(done);
    m_reviewBar->hide();
    lay->addWidget(m_reviewBar);

    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &ChangesCard::applyTheme);
    applyTheme();
}

void ChangesCard::go(int index)
{
    if (index >= m_stops.size())
        index = int(m_stops.size()) - 1;
    m_index = index;
    m_reviewBar->setVisible(index >= 0);
    m_reviewBtn->setVisible(index < 0);
    for (int i = 0; i < m_rows.size(); ++i)
        m_rows.at(i)->setProperty("current", index >= 0 && m_stops.at(index).first == i);
    applyTheme();
    if (index < 0)
        return;
    const auto [file, line] = m_stops.at(index);
    const FileChange &c = m_changes.at(file);
    m_prev->setEnabled(index > 0);
    m_next->setEnabled(index + 1 < m_stops.size());
    m_reviewText->setText(line > 0 ? tr("Change %1 of %2  ·  %3:%4").arg(index + 1).arg(m_stops.size()).arg(QFileInfo(c.path).fileName()).arg(line)
                                   : tr("Change %1 of %2  ·  %3").arg(index + 1).arg(m_stops.size()).arg(QFileInfo(c.path).fileName()));
    emit openRequested(c.path, line);
}

void ChangesCard::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    int added = 0, removed = 0;
    for (const FileChange &c : std::as_const(m_changes)) {
        added += c.added;
        removed += c.removed;
    }
    FileChange total;
    total.added = added;
    total.removed = removed;
    m_title->setText((m_changes.size() == 1 ? tr("1 file changed") : tr("%1 files changed").arg(m_changes.size())) + QStringLiteral("  ·  ") + counts(total, t));
    for (int i = 0; i < m_counts.size(); ++i)
        m_counts.at(i)->setText(counts(m_changes.at(i), t));
    for (QWidget *r : std::as_const(m_rows)) {
        r->style()->unpolish(r);
        r->style()->polish(r);
    }
}

} // namespace claudecode
