#include "GitItemDelegate.h"

#include "git/GitRepository.h"
#include "project/ProjectModel.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QPainter>

GitItemDelegate::GitItemDelegate(GitRepository *repo, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_repo(repo)
    , m_theme(Theme::byName(SettingsManager::instance().theme()))
{
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this,
            [this](const QString &name) { m_theme = Theme::byName(name); });
}

void GitItemDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const GitPathState st = m_repo && m_repo->isRepo() ? m_repo->stateOf(index.data(ProjectModel::PathRole).toString()) : GitPathState{};
    if (st.kind == GitKind::None) {
        QStyledItemDelegate::paint(p, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget *w = opt.widget;
    QStyle *style = w ? w->style() : QApplication::style();
    const QString text = opt.text;
    QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w);
    opt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w); // background, selection, icon

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setFont(opt.font);
    const QFontMetrics fm(opt.font);
    const QColor color = m_theme.gitColor(st.kind);
    const int cy = opt.rect.center().y();
    int right = opt.rect.right() - 8;

    if (st.kind != GitKind::Ignored) {
        if (st.isFolder) {
            p->setPen(Qt::NoPen);
            p->setBrush(color);
            p->drawEllipse(QPoint(right - 3, cy), 3, 3);
            right -= 12;
        } else {
            // Unstaged / untracked marker (plain letter), then the staged one (filled pill) to its left.
            const QChar unstaged = st.untracked ? QLatin1Char('U') : st.unstaged;
            if (st.kind == GitKind::Conflicted || st.isUnstaged() || st.untracked) {
                const QChar letter = st.kind == GitKind::Conflicted ? QLatin1Char('!') : unstaged;
                const GitKind k = st.kind == GitKind::Conflicted ? GitKind::Conflicted : gitKindOfCode(st.untracked ? QLatin1Char('?') : st.unstaged);
                p->setPen(m_theme.gitColor(k));
                QFont f = opt.font;
                f.setBold(true);
                p->setFont(f);
                const int wd = QFontMetrics(f).horizontalAdvance(letter);
                p->drawText(QRect(right - wd, opt.rect.top(), wd, opt.rect.height()), Qt::AlignCenter, QString(letter));
                right -= wd + 6;
            }
            if (st.isStaged()) {
                const QColor fill = m_theme.gitColor(gitKindOfCode(st.staged));
                const QRect pill(right - 14, cy - 7, 14, 14);
                p->setPen(Qt::NoPen);
                p->setBrush(fill);
                p->drawRoundedRect(pill, 3, 3);
                QFont f = opt.font;
                f.setBold(true);
                f.setPointSizeF(qMax(6.0, opt.font.pointSizeF() - 1.5));
                p->setFont(f);
                p->setPen(m_theme.window);
                p->drawText(pill, Qt::AlignCenter, QString(gitBadgeLetter(st.staged)));
                right -= 20;
            }
        }
    }

    textRect.setRight(qMin(textRect.right(), right));
    p->setFont(opt.font);
    p->setPen(color);
    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(text, Qt::ElideRight, textRect.width()));
    p->restore();
}
