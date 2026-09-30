#include "BranchPopup.h"

#include "GitPanel.h"
#include "GitRepository.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {

enum Role { RoleName = Qt::UserRole + 1, RoleKind, RoleCurrent, RoleInfo }; // kind: 0 header, 1 local, 2 remote

class BranchDelegate : public QStyledItemDelegate
{
public:
    explicit BranchDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
        , m_theme(Theme::byName(SettingsManager::instance().theme()))
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        return QSize(0, index.data(RoleKind).toInt() == 0 ? 26 : 30);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const int kind = index.data(RoleKind).toInt();
        const bool current = index.data(RoleCurrent).toBool();
        const QRect r = option.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (kind == 0) {
            QFont f = option.font;
            f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
            f.setWeight(QFont::Bold);
            f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
            p->setFont(f);
            p->setPen(m_theme.textMuted);
            p->drawText(r.adjusted(14, 4, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::DisplayRole).toString().toUpper());
            p->restore();
            return;
        }
        const bool sel = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        if (sel || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(sel ? m_theme.selection : m_theme.currentLine);
            p->drawRoundedRect(r.adjusted(6, 1, -6, -1), 5, 5);
        }
        const int cy = r.center().y();
        if (current) {
            const QColor g = m_theme.gitAdded;
            QColor halo = g;
            halo.setAlpha(60);
            p->setPen(Qt::NoPen);
            p->setBrush(halo);
            p->drawEllipse(QPointF(r.left() + 22, cy), 6.5, 6.5);
            p->setBrush(g);
            p->drawEllipse(QPointF(r.left() + 22, cy), 3.8, 3.8);
        } else {
            p->drawPixmap(r.left() + 15, cy - 7, Icons::pixmap(QStringLiteral(":/new-icons/git-branch.svg"), m_theme.textMuted, 14));
        }
        const int textLeft = r.left() + 38;
        QRect infoRect;
        const QString info = index.data(RoleInfo).toString();
        int right = r.right() - 14;
        if (!info.isEmpty()) {
            QFont f = option.font;
            f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1));
            p->setFont(f);
            const QFontMetrics fm(f);
            const int w = qMin(fm.horizontalAdvance(info), (r.width() - 60) / 2);
            p->setPen(current ? m_theme.gitAdded : m_theme.textMuted);
            p->drawText(QRect(right - w, r.top(), w, r.height()), Qt::AlignVCenter | Qt::AlignRight, fm.elidedText(info, Qt::ElideLeft, w));
            right -= w + 10;
        }
        QFont f = option.font;
        f.setWeight(current ? QFont::Bold : QFont::Medium);
        p->setFont(f);
        p->setPen(m_theme.editorFg);
        p->drawText(QRect(textLeft, r.top(), qMax(0, right - textLeft), r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(f).elidedText(index.data(RoleName).toString(), Qt::ElideRight, qMax(0, right - textLeft)));
        p->restore();
    }

private:
    Theme m_theme;
};

} // namespace

void BranchPopup::open(GitRepository *repo, QWidget *anchor, bool above)
{
    auto *pop = new BranchPopup(repo, anchor);
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->adjustSize();
    QPoint pos = above ? anchor->mapToGlobal(QPoint(0, -pop->height() - 4)) : anchor->mapToGlobal(QPoint(0, anchor->height() + 4));
    const QRect avail = anchor->screen()->availableGeometry();
    pos.setX(qBound(avail.left(), pos.x(), avail.right() - pop->width()));
    pos.setY(qBound(avail.top(), pos.y(), avail.bottom() - pop->height()));
    pop->move(pos);
    pop->show();
    pop->m_filter->setFocus();
}

BranchPopup::BranchPopup(GitRepository *repo, QWidget *anchor)
    : QFrame(anchor->window(), Qt::Popup)
    , m_repo(repo)
    , m_anchor(anchor)
{
    setObjectName(QStringLiteral("branchPopup"));
    setFixedWidth(340);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 8, 6, 6);
    lay->setSpacing(6);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Switch to branch…"));
    m_filter->setClearButtonEnabled(true);
    m_filter->installEventFilter(this);
    lay->addWidget(m_filter);

    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setItemDelegate(new BranchDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setFixedHeight(280);
    lay->addWidget(m_list);

    auto *create = new QPushButton(tr("Create New Branch…"), this);
    create->setIcon(Icons::tinted(QStringLiteral(":/new-icons/plus.svg"), Theme::byName(SettingsManager::instance().theme()).editorFg));
    create->setObjectName(QStringLiteral("flatBtn"));
    create->setCursor(Qt::PointingHandCursor);
    lay->addWidget(create);

    connect(m_filter, &QLineEdit::textChanged, this, &BranchPopup::rebuild);
    connect(m_list, &QListWidget::itemClicked, this, &BranchPopup::activate);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &BranchPopup::showItemMenu);
    connect(create, &QPushButton::clicked, this, [this] {
        QWidget *parent = m_anchor->window();
        GitRepository *repo = m_repo;
        close();
        GitPanel::promptNewBranch(repo, parent);
    });
    rebuild();
}

void BranchPopup::rebuild()
{
    const QString needle = m_filter->text().trimmed();
    m_list->clear();

    QSet<QString> locals;
    for (const GitBranchInfo &b : m_repo->branches())
        if (!b.remote)
            locals.insert(b.name);

    auto addHeader = [this](const QString &title) {
        auto *h = new QListWidgetItem(title, m_list);
        h->setData(RoleKind, 0);
        h->setFlags(Qt::NoItemFlags);
    };
    for (int pass = 0; pass < 2; ++pass) {
        const bool remote = pass == 1;
        bool headerAdded = false;
        for (const GitBranchInfo &b : m_repo->branches()) {
            if (b.remote != remote || (!needle.isEmpty() && !b.name.contains(needle, Qt::CaseInsensitive)))
                continue;
            // A remote branch that already has a local counterpart would only be a confusing duplicate.
            if (remote && locals.contains(b.name.section(QLatin1Char('/'), 1)))
                continue;
            if (!headerAdded) {
                addHeader(remote ? tr("Remote branches") : tr("Local branches"));
                headerAdded = true;
            }
            auto *it = new QListWidgetItem(b.name, m_list);
            it->setData(RoleName, b.name);
            it->setData(RoleKind, remote ? 2 : 1);
            it->setData(RoleCurrent, b.current);
            it->setData(RoleInfo, b.current ? tr("current") : b.upstream);
        }
    }
    if (m_list->count() == 0) {
        auto *it = new QListWidgetItem(needle.isEmpty() ? tr("No branches yet — commit something first") : tr("No matching branches"), m_list);
        it->setData(RoleKind, 0);
        it->setFlags(Qt::NoItemFlags);
    }
    selectFirstBranch(0, 1);
}

void BranchPopup::selectFirstBranch(int from, int step)
{
    for (int i = from; i >= 0 && i < m_list->count(); i += step) {
        if (m_list->item(i)->data(RoleKind).toInt() != 0) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}

void BranchPopup::activate(QListWidgetItem *item)
{
    if (!item || item->data(RoleKind).toInt() == 0)
        return;
    const QString name = item->data(RoleName).toString();
    const bool current = item->data(RoleCurrent).toBool();
    GitRepository *repo = m_repo;
    close();
    if (!current)
        repo->checkout(name);
}

void BranchPopup::showItemMenu(const QPoint &pos)
{
    QListWidgetItem *it = m_list->itemAt(pos);
    if (!it || it->data(RoleKind).toInt() != 1 || it->data(RoleCurrent).toBool())
        return;
    const QString name = it->data(RoleName).toString();
    QMenu menu(this);
    menu.addAction(tr("Switch to \"%1\"").arg(name), this, [this, it] { activate(it); });
    menu.addAction(tr("Delete Branch…"), this, [this, name] {
        QWidget *parent = m_anchor->window();
        GitRepository *repo = m_repo;
        if (QMessageBox::warning(parent, tr("Delete Branch"), tr("Delete the branch \"%1\"?").arg(name),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes) {
            close();
            repo->deleteBranch(name, false);
        }
    });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

bool BranchPopup::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_filter && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Down:
            selectFirstBranch(m_list->currentRow() + 1, 1);
            return true;
        case Qt::Key_Up:
            selectFirstBranch(m_list->currentRow() - 1, -1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activate(m_list->currentItem());
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, event);
}
