#include "WelcomePage.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
constexpr int kRowHeight = 46;
constexpr int kMaxRows = 6;
enum Role { RolePath = Qt::UserRole + 1, RoleName, RoleShort };

QString shortPath(const QString &path)
{
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QStringLiteral("~") + path.mid(home.size());
    return path;
}

class RecentDelegate : public QStyledItemDelegate
{
public:
    explicit RecentDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
        refresh();
        connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { refresh(); });
    }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(0, kRowHeight); }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QRect r = option.rect;
        const bool hover = option.state & QStyle::State_MouseOver;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme.currentLine);
            p->drawRoundedRect(r.adjusted(2, 2, -2, -2), 6, 6);
        }
        p->drawPixmap(r.left() + 14, r.center().y() - 9, Icons::pixmap(QStringLiteral(":/new-icons/folder.svg"), m_theme.accent, 18));
        const int left = r.left() + 44;
        int right = r.right() - 14;
        if (hover) {
            right -= 22;
            p->drawPixmap(r.right() - 30, r.center().y() - 7, Icons::pixmap(QStringLiteral(":/new-icons/x.svg"), m_theme.textMuted, 14));
        }
        QFont nf = option.font;
        nf.setWeight(QFont::DemiBold);
        p->setFont(nf);
        p->setPen(m_theme.editorFg);
        const QFontMetrics nfm(nf);
        p->drawText(QRect(left, r.top() + 5, right - left, 18), Qt::AlignVCenter | Qt::AlignLeft,
                    nfm.elidedText(index.data(RoleName).toString(), Qt::ElideRight, right - left));
        QFont pf = option.font;
        pf.setPointSizeF(qMax(7.5, pf.pointSizeF() - 1));
        p->setFont(pf);
        p->setPen(m_theme.textMuted);
        const QFontMetrics pfm(pf);
        p->drawText(QRect(left, r.top() + 23, right - left, 16), Qt::AlignVCenter | Qt::AlignLeft,
                    pfm.elidedText(index.data(RoleShort).toString(), Qt::ElideMiddle, right - left));
        p->restore();
    }

private:
    void refresh() { m_theme = Theme::byName(SettingsManager::instance().theme()); }
    Theme m_theme;
};
} // namespace

// A plain list whose rows open a project on click and have a hover "remove" button on the right.
class RecentList : public QListWidget
{
public:
    explicit RecentList(WelcomePage *page)
        : QListWidget(page)
        , m_page(page)
    {
        setFrameShape(QFrame::NoFrame);
        setItemDelegate(new RecentDelegate(this));
        setMouseTracking(true);
        setSelectionMode(QAbstractItemView::NoSelection);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        viewport()->setAutoFillBackground(false);
        setStyleSheet(QStringLiteral("QListWidget { background: transparent; }"));
        setFixedWidth(460);
        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
            QListWidgetItem *it = itemAt(pos);
            QMenu menu(this);
            if (it) {
                const QString path = it->data(RolePath).toString();
                menu.addAction(tr("Open"), this, [this, path] { emit m_page->openRecentRequested(path); });
                menu.addAction(tr("Remove from Recent"), this, [this, path] { emit m_page->removeRecentRequested(path); });
                menu.addSeparator();
            }
            menu.addAction(tr("Clear Recent Projects"), this, [this] { emit m_page->clearRecentRequested(); });
            menu.exec(viewport()->mapToGlobal(pos));
        });
    }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QListWidgetItem *it = itemAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton && it) {
            const QString path = it->data(RolePath).toString();
            if (event->position().toPoint().x() > viewport()->width() - 36)
                emit m_page->removeRecentRequested(path);
            else
                emit m_page->openRecentRequested(path);
            return;
        }
        QListWidget::mouseReleaseEvent(event);
    }

private:
    WelcomePage *m_page;
};

WelcomePage::WelcomePage(QWidget *parent)
    : QWidget(parent)
{
    auto *wl = new QVBoxLayout(this);
    wl->setAlignment(Qt::AlignCenter);
    wl->setSpacing(10);
    auto *title = new QLabel(QStringLiteral("QODE"), this);
    title->setObjectName(QStringLiteral("emptyTitle"));
    title->setAlignment(Qt::AlignCenter);
    wl->addWidget(title);
    auto *heading = new QLabel(tr("No project or file is open.\nCreate or select one to get started."), this);
    heading->setAlignment(Qt::AlignCenter);
    heading->setObjectName(QStringLiteral("emptyText"));
    wl->addWidget(heading);
    auto addButton = [&](const QString &text, void (WelcomePage::*sig)()) {
        auto *b = new QPushButton(text, this);
        b->setMinimumWidth(200);
        connect(b, &QPushButton::clicked, this, sig);
        wl->addWidget(b, 0, Qt::AlignCenter);
    };
    addButton(tr("New Project…"), &WelcomePage::newProjectRequested);
    addButton(tr("Open Project…"), &WelcomePage::openProjectRequested);
    addButton(tr("New File"), &WelcomePage::newFileRequested);
    addButton(tr("Open File…"), &WelcomePage::openFileRequested);

    wl->addSpacing(14);
    m_recentTitle = new QLabel(tr("RECENT PROJECTS"), this);
    m_recentTitle->setObjectName(QStringLiteral("emptyText"));
    QFont tf = m_recentTitle->font();
    tf.setPointSizeF(qMax(7.5, tf.pointSizeF() - 1.5));
    tf.setWeight(QFont::Bold);
    tf.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    m_recentTitle->setFont(tf);
    m_recentTitle->setAlignment(Qt::AlignCenter);
    wl->addWidget(m_recentTitle);
    m_recent = new RecentList(this);
    wl->addWidget(m_recent, 0, Qt::AlignCenter);

    auto *hint = new QLabel(tr("<span>Ctrl+Shift+O open project &nbsp;·&nbsp; Ctrl+O open file &nbsp;·&nbsp; Ctrl+Shift+P command palette &nbsp;·&nbsp; Ctrl+J terminal</span>"), this);
    hint->setAlignment(Qt::AlignCenter);
    hint->setObjectName(QStringLiteral("emptyText"));
    wl->addSpacing(6);
    wl->addWidget(hint);
    setRecentProjects({});
}

void WelcomePage::setRecentProjects(const QStringList &paths)
{
    m_recent->clear();
    int shown = 0;
    for (const QString &path : paths) {
        if (shown >= kMaxRows)
            break;
        if (!QFileInfo(path).isDir())
            continue; // moved or deleted since it was last opened
        auto *it = new QListWidgetItem(m_recent);
        it->setData(RolePath, path);
        it->setData(RoleName, QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName());
        it->setData(RoleShort, shortPath(path));
        it->setToolTip(path);
        it->setSizeHint(QSize(0, kRowHeight));
        ++shown;
    }
    m_recent->setFixedHeight(shown * kRowHeight + 4);
    m_recent->setVisible(shown > 0);
    m_recentTitle->setVisible(shown > 0);
}
