#include "GitPanel.h"

#include "BranchButton.h"
#include "BranchPopup.h"
#include "GitRepository.h"
#include "PatchDialog.h"
#include "filesystem/FileManager.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCursor>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QDir>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

enum Role {
    RolePath = Qt::UserRole + 1,
    RoleSection,
    RoleLetter, // status code shown for the row
    RoleDir,
    RoleCount,
    RoleIsSection,
    RoleHash,
    RoleMeta,
    RoleRefs
};

const char *iconFor(int action)
{
    switch (action) {
    case 0: return ":/new-icons/plus.svg";     // stage
    case 1: return ":/new-icons/minus.svg";    // unstage
    case 2: return ":/new-icons/undo-2.svg";     // discard
    default: return ":/new-icons/file-input.svg";
    }
}

QString tipFor(int action)
{
    switch (action) {
    case 0: return QObject::tr("Stage");
    case 1: return QObject::tr("Unstage");
    case 2: return QObject::tr("Discard");
    default: return QObject::tr("Open File");
    }
}

// --- Delegate for the changes tree ------------------------------------------------------------

class ChangeDelegate : public QStyledItemDelegate
{
public:
    using Handler = std::function<void(const QString &path, int section, int action, bool isSection)>;

    ChangeDelegate(QTreeWidget *view, Handler handler)
        : QStyledItemDelegate(view)
        , m_view(view)
        , m_handler(std::move(handler))
        , m_theme(Theme::byName(SettingsManager::instance().theme()))
    {
        QObject::connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this](const QString &n) {
            m_theme = Theme::byName(n);
            m_view->viewport()->update();
        });
        m_view->viewport()->installEventFilter(this);
    }

    // Track the pointer ourselves (QCursor::pos() can be stale on Wayland).
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (obj == m_view->viewport()) {
            if (event->type() == QEvent::MouseMove) {
                m_mouse = static_cast<QMouseEvent *>(event)->pos();
                m_view->viewport()->update();
            } else if (event->type() == QEvent::Leave) {
                m_mouse = QPoint(-1, -1);
                m_view->viewport()->update();
            } else if (event->type() == QEvent::ToolTip) {
                const QModelIndex idx = m_view->indexAt(m_mouse);
                if (idx.isValid()) {
                    QStyleOptionViewItem o;
                    o.rect = m_view->visualRect(idx);
                    o.font = m_view->font();
                    const Geometry g = geometry(o, idx);
                    for (const Button &b : g.buttons) {
                        if (b.rect.contains(m_mouse)) {
                            QToolTip::showText(QCursor::pos(), tipFor(b.action), m_view->viewport());
                            return true;
                        }
                    }
                }
            }
        }
        return QStyledItemDelegate::eventFilter(obj, event);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        return QSize(0, index.data(RoleIsSection).toBool() ? 30 : 26);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);

        const bool isSection = index.data(RoleIsSection).toBool();
        const QString letter = index.data(RoleLetter).toString();
        const bool hover = opt.rect.contains(m_mouse);
        const Geometry g = geometry(opt, index);
        const QFontMetrics fm(opt.font);
        const int cy = opt.rect.center().y();

        p->save();
        p->setRenderHint(QPainter::Antialiasing);

        if (isSection) {
            const bool open = m_view->isExpanded(index);
            p->drawPixmap(opt.rect.left() + 10, cy - 5, Icons::pixmap(open ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"), m_theme.textMuted, 10));
            const QString count = QString::number(index.data(RoleCount).toInt());
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme.accent);
            QColor tint = m_theme.accent;
            tint.setAlpha(m_theme.dark ? 50 : 38);
            p->setBrush(tint);
            p->drawRoundedRect(g.badge, g.badge.height() / 2.0, g.badge.height() / 2.0);
            QFont cf = opt.font;
            cf.setPointSizeF(qMax(7.0, opt.font.pointSizeF() - 1));
            cf.setWeight(QFont::Bold);
            p->setFont(cf);
            p->setPen(m_theme.accent);
            p->drawText(g.badge, Qt::AlignCenter, count);
        } else if (!letter.isEmpty()) {
            const GitKind kind = gitKindOfCode(letter.at(0));
            QColor tint = m_theme.gitColor(kind);
            tint.setAlpha(m_theme.dark ? 45 : 34);
            p->setPen(Qt::NoPen);
            p->setBrush(tint);
            p->drawRoundedRect(g.badge, 4, 4);
            QFont f = opt.font;
            f.setPointSizeF(qMax(7.0, opt.font.pointSizeF() - 1.5));
            f.setWeight(QFont::Bold);
            p->setFont(f);
            p->setPen(m_theme.gitColor(kind));
            p->drawText(g.badge, Qt::AlignCenter, QString(gitBadgeLetter(letter.at(0))));
        }

        // Hover actions sit in front of the badge.
        if (hover) {
            for (const Button &b : g.buttons) {
                if (b.rect.contains(m_mouse)) {
                    p->setPen(Qt::NoPen);
                    p->setBrush(m_theme.dark ? m_theme.selection : m_theme.border);
                    p->drawRoundedRect(b.rect, 4, 4);
                }
                p->drawPixmap(b.rect.center().x() - 7, b.rect.center().y() - 7, Icons::pixmap(QString::fromLatin1(iconFor(b.action)), m_theme.editorFg, 14));
            }
        }

        // Text
        const int textLeft = opt.rect.left() + (isSection ? 26 : 22);
        QRect textRect(textLeft, opt.rect.top(), qMax(0, g.textRight - textLeft), opt.rect.height());
        p->setClipRect(textRect);
        if (isSection) {
            QFont f = opt.font;
            f.setWeight(QFont::Bold);
            f.setPointSizeF(qMax(7.0, opt.font.pointSizeF() - 1.5));
            f.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
            p->setFont(f);
            p->setPen(m_theme.textMuted);
            p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::DisplayRole).toString().toUpper());
        } else {
            const QString name = index.data(Qt::DisplayRole).toString();
            const QString dir = index.data(RoleDir).toString();
            const bool deleted = !letter.isEmpty() && letter.at(0) == QLatin1Char('D');
            QFont f = opt.font;
            f.setStrikeOut(deleted);
            p->setFont(f);
            p->setPen(m_theme.gitColor(gitKindOfCode(letter.isEmpty() ? QChar() : letter.at(0))));
            const QString shown = fm.elidedText(name, Qt::ElideRight, textRect.width());
            p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, shown);
            const int used = fm.horizontalAdvance(shown) + 8;
            if (!dir.isEmpty() && used < textRect.width() - 20) {
                QFont df = opt.font;
                df.setPointSizeF(qMax(7.0, opt.font.pointSizeF() - 1));
                p->setFont(df);
                p->setPen(m_theme.textMuted);
                const QFontMetrics dfm(df);
                p->drawText(textRect.adjusted(used, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                            dfm.elidedText(dir, Qt::ElideLeft, textRect.width() - used));
            }
        }
        p->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                const bool isSection = index.data(RoleIsSection).toBool();
                const Geometry g = geometry(option, index);
                for (const Button &b : g.buttons) {
                    if (b.rect.contains(me->pos())) {
                        const QString path = index.data(RolePath).toString();
                        const int section = index.data(RoleSection).toInt();
                        const int action = b.action;
                        QTimer::singleShot(0, m_view, [this, path, section, action, isSection] { m_handler(path, section, action, isSection); });
                        return true;
                    }
                }
                if (isSection) {
                    const QPersistentModelIndex pi(index);
                    QTimer::singleShot(0, m_view, [this, pi] {
                        if (pi.isValid())
                            m_view->setExpanded(pi, !m_view->isExpanded(pi));
                    });
                    return true;
                }
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    struct Button {
        int action;
        QRect rect;
    };
    struct Geometry {
        QRect badge;          // status letter / count pill
        QList<Button> buttons; // hover actions, left-to-right
        int textRight = 0;
    };

    // The single source of truth for where things are, shared by paint(), hit testing and tooltips.
    Geometry geometry(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        Geometry g;
        const QFontMetrics fm(option.font);
        const QRect r = option.rect;
        const int cy = r.center().y();
        int right = r.right() - 10;
        if (index.data(RoleIsSection).toBool()) {
            const QString count = QString::number(index.data(RoleCount).toInt());
            const int w = qMax(20, fm.horizontalAdvance(count) + 12);
            g.badge = QRect(right - w + 1, cy - 9, w, 18);
        } else if (!index.data(RoleLetter).toString().isEmpty()) {
            g.badge = QRect(right - 17, cy - 9, 18, 18);
        } else {
            g.badge = QRect(right, cy, 0, 0);
        }
        right = g.badge.left() - 4;
        const bool hover = r.contains(m_mouse);
        if (hover) {
            const QList<int> acts = actionsFor(index);
            for (int i = acts.size() - 1; i >= 0; --i) {
                g.buttons.prepend({acts.at(i), QRect(right - 22, cy - 11, 22, 22)});
                right -= 24;
            }
        }
        g.textRight = right - 4;
        return g;
    }

    static QList<int> actionsFor(const QModelIndex &index)
    {
        const int section = index.data(RoleSection).toInt();
        const bool isSection = index.data(RoleIsSection).toBool();
        const bool deleted = index.data(RoleLetter).toString() == QLatin1String("D");
        QList<int> a;
        if (!isSection && !deleted)
            a << 3;
        if (section == 1) // staged
            a << 1;
        else if (section == 0) // conflicts
            a << 0;
        else
            a << 2 << 0;
        return a;
    }

    QTreeWidget *m_view;
    Handler m_handler;
    Theme m_theme;
    QPoint m_mouse{-1, -1};
};

// --- Delegate for commit history ---------------------------------------------------------------

class HistoryDelegate : public QStyledItemDelegate
{
public:
    explicit HistoryDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
        , m_theme(Theme::byName(SettingsManager::instance().theme()))
    {
        connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this](const QString &n) { m_theme = Theme::byName(n); });
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(0, QFontMetrics(option.font).height() * 2 + 14);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);

        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QFontMetrics fm(opt.font);
        const bool placeholder = index.data(RoleHash).toString().isEmpty();

        // Timeline: a rail through every row with a dot per commit (the tip commit is green).
        const int railX = opt.rect.left() + 16;
        const int dotY = opt.rect.top() + 4 + fm.height() / 2 + 1;
        if (!placeholder) {
            const QTreeWidget *tree = qobject_cast<const QTreeWidget *>(opt.widget);
            const bool last = tree && index.row() == tree->topLevelItemCount() - 1;
            p->setPen(QPen(m_theme.border.lighter(m_theme.dark ? 180 : 100), 1.5));
            p->drawLine(railX, index.row() == 0 ? dotY : opt.rect.top(), railX, last ? dotY : opt.rect.bottom() + 1);
            p->setPen(QPen(index.row() == 0 ? m_theme.gitAdded : m_theme.accent, 2));
            p->setBrush(m_theme.panel);
            p->drawEllipse(QPointF(railX, dotY), 4, 4);
        }

        QRect r = opt.rect.adjusted(placeholder ? 10 : 32, 4, -10, -4);
        p->setClipRect(r);
        int x = r.left();
        const QString refs = index.data(RoleRefs).toString();
        if (!refs.isEmpty()) {
            QFont f = opt.font;
            f.setPointSizeF(qMax(7.0, opt.font.pointSizeF() - 1.5));
            f.setWeight(QFont::Bold);
            p->setFont(f);
            const QFontMetrics rf(f);
            for (QString ref : refs.split(QLatin1String(", "), Qt::SkipEmptyParts)) {
                const bool head = ref.startsWith(QLatin1String("HEAD -> "));
                ref.replace(QLatin1String("HEAD -> "), QString());
                const int w = rf.horizontalAdvance(ref) + 12;
                if (x + w > r.right())
                    break;
                const QRect pill(x, r.top() + 1, w, fm.height() - 2);
                const QColor c = head ? m_theme.gitAdded : m_theme.accent;
                QColor bg = c;
                bg.setAlpha(m_theme.dark ? 48 : 36);
                p->setPen(Qt::NoPen);
                p->setBrush(bg);
                p->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
                p->setPen(c);
                p->drawText(pill, Qt::AlignCenter, ref);
                x += w + 6;
            }
        }
        p->setFont(opt.font);
        p->setPen(placeholder ? m_theme.textMuted : m_theme.editorFg);
        const QString subject = index.data(Qt::DisplayRole).toString();
        p->drawText(QRect(x, r.top(), r.right() - x, fm.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    fm.elidedText(subject, Qt::ElideRight, r.right() - x));
        QFont mf = opt.font;
        mf.setPointSizeF(qMax(7.0, opt.font.pointSizeF() - 1));
        p->setFont(mf);
        p->setPen(m_theme.textMuted);
        const QFontMetrics mfm(mf);
        p->drawText(QRect(r.left(), r.top() + fm.height() + 3, r.width(), mfm.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    mfm.elidedText(index.data(RoleMeta).toString(), Qt::ElideRight, r.width()));
        p->restore();
    }

private:
    Theme m_theme;
};

QToolButton *makeToolButton(QWidget *parent, const QString &tip)
{
    auto *b = new QToolButton(parent);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setIconSize(QSize(16, 16));
    return b;
}

} // namespace

// --- GitPanel -----------------------------------------------------------------------------------

GitPanel::GitPanel(GitRepository *repo, QWidget *parent)
    : QWidget(parent)
    , m_repo(repo)
{
    // Page 0: nothing to show / offer to initialise
    auto *msgPage = new QWidget(this);
    auto *msgLayout = new QVBoxLayout(msgPage);
    msgLayout->setAlignment(Qt::AlignCenter);
    m_messageLabel = new QLabel(msgPage);
    m_messageLabel->setObjectName(QStringLiteral("emptyText"));
    m_messageLabel->setAlignment(Qt::AlignCenter);
    m_messageLabel->setWordWrap(true);
    m_initButton = new QPushButton(tr("Initialize Repository"), msgPage);
    msgLayout->addWidget(m_messageLabel);
    msgLayout->addWidget(m_initButton, 0, Qt::AlignCenter);
    connect(m_initButton, &QPushButton::clicked, m_repo, &GitRepository::init);

    // Page 1: repository
    auto *repoPage = new QWidget(this);
    auto *rl = new QVBoxLayout(repoPage);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);

    auto *bar = new QWidget(repoPage);
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(8, 8, 8, 8);
    bl->setSpacing(2);
    m_branchBtn = new BranchButton(false, bar);
    m_branchBtn->setToolTip(tr("Switch or create branch"));
    connect(m_branchBtn, &QAbstractButton::clicked, this, [this] { BranchPopup::open(m_repo, m_branchBtn, false); });
    m_pullBtn = makeToolButton(bar, tr("Pull"));
    m_pushBtn = makeToolButton(bar, tr("Push"));
    m_refreshBtn = makeToolButton(bar, tr("Refresh"));
    m_moreBtn = makeToolButton(bar, tr("More actions"));
    m_moreBtn->setPopupMode(QToolButton::InstantPopup);
    m_moreBtn->setMenu(new QMenu(m_moreBtn));
    connect(m_moreBtn->menu(), &QMenu::aboutToShow, this, &GitPanel::showMoreMenu);
    m_busyLabel = new QLabel(tr("Working…"), bar);
    m_busyLabel->setObjectName(QStringLiteral("emptyText"));
    m_busyLabel->hide();
    bl->addWidget(m_branchBtn);
    bl->addSpacing(6);
    bl->addWidget(m_busyLabel);
    bl->addStretch(1);
    bl->addWidget(m_pullBtn);
    bl->addWidget(m_pushBtn);
    bl->addWidget(m_refreshBtn);
    bl->addWidget(m_moreBtn);
    connect(m_pullBtn, &QToolButton::clicked, m_repo, &GitRepository::pull);
    connect(m_pushBtn, &QToolButton::clicked, m_repo, &GitRepository::push);
    connect(m_refreshBtn, &QToolButton::clicked, m_repo, &GitRepository::refresh);

    m_tabs = new QTabBar(repoPage);
    m_tabs->setObjectName(QStringLiteral("scmTabs"));
    m_tabs->addTab(tr("Changes"));
    m_tabs->addTab(tr("History"));
    m_tabs->setExpanding(true);
    m_tabs->setDrawBase(false);

    // Changes page
    auto *changes = new QWidget(this);
    auto *cl = new QVBoxLayout(changes);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    auto *commitBox = new QWidget(changes);
    auto *cbl = new QVBoxLayout(commitBox);
    cbl->setContentsMargins(10, 10, 10, 10);
    cbl->setSpacing(8);
    m_message = new QPlainTextEdit(commitBox);
    m_message->setPlaceholderText(tr("Message (Ctrl+Enter to commit)"));
    m_message->setFixedHeight(72);
    m_message->setTabChangesFocus(true);
    auto *row = new QHBoxLayout;
    m_commitBtn = new QPushButton(tr("Commit"), commitBox);
    m_commitBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_commitBtn->setCursor(Qt::PointingHandCursor);
    m_commitBtn->setIconSize(QSize(14, 14));
    m_commitBtn->setDefault(false);
    m_amend = new QCheckBox(tr("Amend"), commitBox);
    m_amend->setToolTip(tr("Replace the last commit instead of creating a new one"));
    row->setSpacing(10);
    row->addWidget(m_commitBtn, 1);
    row->addWidget(m_amend);
    cbl->addWidget(m_message);
    cbl->addLayout(row);
    cl->addWidget(commitBox);

    m_tree = new QTreeWidget(changes);
    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setMouseTracking(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setIndentation(0);
    m_tree->setRootIsDecorated(false);
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setItemDelegate(new ChangeDelegate(m_tree, [this](const QString &path, int section, int action, bool isSection) {
        onAction(path, section, action, isSection);
    }));
    m_noChanges = new QLabel(tr("No changes — working tree clean"), changes);
    m_noChanges->setObjectName(QStringLiteral("emptyText"));
    m_noChanges->setAlignment(Qt::AlignCenter);
    cl->addWidget(m_tree, 1);
    cl->addWidget(m_noChanges, 1);

    // History page
    m_history = new QTreeWidget(this);
    m_history->setHeaderHidden(true);
    m_history->setRootIsDecorated(false);
    m_history->setUniformRowHeights(true);
    m_history->setItemDelegate(new HistoryDelegate(m_history));
    m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_body = new QStackedWidget(repoPage);
    m_body->addWidget(changes);
    m_body->addWidget(m_history);
    rl->addWidget(bar);
    rl->addWidget(m_tabs);
    rl->addWidget(m_body, 1);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(msgPage);
    m_pages->addWidget(repoPage);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_pages);

    connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
        m_body->setCurrentIndex(i);
        if (i == 1)
            loadHistory();
    });
    connect(m_commitBtn, &QPushButton::clicked, this, &GitPanel::commit);
    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_message);
    sc->setContext(Qt::WidgetShortcut);
    connect(sc, &QShortcut::activated, this, &GitPanel::commit);
    auto *sc2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), m_message);
    sc2->setContext(Qt::WidgetShortcut);
    connect(sc2, &QShortcut::activated, this, &GitPanel::commit);
    connect(m_amend, &QCheckBox::toggled, this, [this](bool on) {
        if (on && m_message->toPlainText().trimmed().isEmpty()) {
            m_repo->lastCommitMessage(this, [this](const QString &msg) {
                if (m_amend->isChecked() && m_message->toPlainText().trimmed().isEmpty()) {
                    m_prefilledAmend = msg;
                    m_message->setPlainText(msg);
                }
            });
        } else if (!on && !m_prefilledAmend.isEmpty() && m_message->toPlainText() == m_prefilledAmend) {
            m_message->clear();
            m_prefilledAmend.clear();
        }
        updateToolbar();
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) { openItem(it); });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) { openItem(it); });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &GitPanel::showTreeMenu);
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *it) { m_collapsed.insert(it->data(0, RoleSection).toInt()); });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *it) { m_collapsed.remove(it->data(0, RoleSection).toInt()); });
    connect(m_history, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        const QString hash = it->data(0, RoleHash).toString();
        const QString title = it->text(0);
        m_repo->commitPatch(hash, this, [this, hash, title](const QString &text) {
            auto *dlg = new PatchDialog(tr("%1 — %2").arg(hash.left(8), title), text, window());
            dlg->show();
        });
    });

    connect(m_repo, &GitRepository::repositoryChanged, this, &GitPanel::onRepositoryChanged);
    connect(m_repo, &GitRepository::statusChanged, this, &GitPanel::onStatusChanged);
    connect(m_repo, &GitRepository::busyChanged, this, [this] { updateToolbar(); });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { applyTheme(); });
    applyTheme();
    onRepositoryChanged();
}

void GitPanel::focusCommitMessage()
{
    showChanges();
    m_message->setFocus();
}

void GitPanel::showChanges()
{
    m_tabs->setCurrentIndex(0);
}

void GitPanel::onRepositoryChanged()
{
    if (m_repo->isRepo()) {
        m_pages->setCurrentIndex(1);
    } else {
        m_pages->setCurrentIndex(0);
        m_initButton->hide();
        if (m_repo->workDirectory().isEmpty()) {
            m_messageLabel->setText(tr("No project open."));
        } else if (!m_repo->gitAvailable()) {
            m_messageLabel->setText(tr("Git was not found on this system.\nInstall git to use source control."));
        } else if (!m_repo->isResolved()) {
            m_messageLabel->setText(tr("Looking for a repository…"));
        } else {
            m_messageLabel->setText(tr("This project is not a Git repository.\n\nInitialize one to track changes, stage files and commit."));
            m_initButton->show();
        }
    }
    m_historyHead.clear();
    onStatusChanged();
}

void GitPanel::onStatusChanged()
{
    if (!m_repo->isRepo())
        return;
    updateToolbar();
    rebuildTree();
    if (m_tabs->currentIndex() == 1 && m_repo->headOid() != m_historyHead)
        loadHistory();
}

void GitPanel::updateToolbar()
{
    const bool idle = !m_repo->busy();
    QString branch = m_repo->branch();
    if (branch.isEmpty())
        branch = tr("(no branch)");
    m_branchBtn->setLabel(m_repo->isDetached() ? tr("%1 (detached)").arg(branch) : branch);
    m_pullBtn->setText(m_repo->behind() > 0 ? QString::number(m_repo->behind()) : QString());
    m_pushBtn->setText(m_repo->ahead() > 0 ? QString::number(m_repo->ahead()) : QString());
    m_pullBtn->setToolTip(m_repo->upstream().isEmpty() ? tr("Pull") : tr("Pull from %1 (%2 behind)").arg(m_repo->upstream()).arg(m_repo->behind()));
    m_pushBtn->setToolTip(m_repo->upstream().isEmpty() ? tr("Publish branch") : tr("Push to %1 (%2 ahead)").arg(m_repo->upstream()).arg(m_repo->ahead()));
    for (QWidget *w : {static_cast<QWidget *>(m_pullBtn), static_cast<QWidget *>(m_pushBtn), static_cast<QWidget *>(m_commitBtn)})
        w->setEnabled(idle);
    m_busyLabel->setVisible(!idle);
    m_commitBtn->setText(m_amend->isChecked() ? tr("Amend Commit") : tr("Commit"));
}

void GitPanel::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_pullBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/arrow-down.svg"), t.editorFg));
    m_pushBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/arrow-up.svg"), t.editorFg));
    m_refreshBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/refresh-cw.svg"), t.editorFg));
    m_moreBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/ellipsis.svg"), t.editorFg));
    m_commitBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/check.svg"), t.onAccent(), t.onAccent()));
}

QList<GitFileChange> GitPanel::changesForSection(int section) const
{
    QList<GitFileChange> list = section == SecConflicts ? m_repo->conflicts() : section == SecStaged ? m_repo->stagedChanges() : m_repo->unstagedChanges();
    std::sort(list.begin(), list.end(), [](const GitFileChange &a, const GitFileChange &b) {
        return a.relPath.compare(b.relPath, Qt::CaseInsensitive) < 0;
    });
    return list;
}

void GitPanel::rebuildTree()
{
    // Preserve selection and scroll position across the rebuild.
    QSet<QString> selected;
    for (QTreeWidgetItem *it : m_tree->selectedItems())
        selected.insert(QString::number(it->data(0, RoleSection).toInt()) + it->data(0, RolePath).toString());
    const int scroll = m_tree->verticalScrollBar()->value();

    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    struct Def { int section; QString title; };
    const Def defs[] = {{SecConflicts, tr("Merge Conflicts")}, {SecStaged, tr("Staged Changes")}, {SecChanges, tr("Changes")}};
    int total = 0;
    for (const Def &d : defs) {
        const QList<GitFileChange> list = changesForSection(d.section);
        if (list.isEmpty())
            continue;
        total += list.size();
        auto *sec = new QTreeWidgetItem(m_tree);
        sec->setText(0, d.title);
        sec->setData(0, RoleIsSection, true);
        sec->setData(0, RoleSection, d.section);
        sec->setData(0, RoleCount, list.size());
        sec->setFlags(Qt::ItemIsEnabled);
        for (const GitFileChange &c : list) {
            auto *it = new QTreeWidgetItem(sec);
            it->setText(0, QFileInfo(c.relPath).fileName());
            const QString dir = QFileInfo(c.relPath).path();
            it->setData(0, RoleDir, dir == QLatin1String(".") ? QString() : dir);
            it->setData(0, RolePath, c.path);
            it->setData(0, RoleSection, d.section);
            QChar code = d.section == SecStaged ? c.staged : d.section == SecConflicts ? QLatin1Char('U') : c.unstaged;
            it->setData(0, RoleLetter, QString(code));
            QString tip = QDir::toNativeSeparators(c.relPath);
            if (!c.origRelPath.isEmpty())
                tip += tr("\n(renamed from %1)").arg(c.origRelPath);
            const QString what = d.section == SecConflicts ? tr("Merge conflict") : gitCodeName(code);
            it->setToolTip(0, tip + QLatin1Char('\n') + what);
            if (selected.contains(QString::number(d.section) + c.path))
                it->setSelected(true);
        }
        sec->setExpanded(!m_collapsed.contains(d.section));
    }
    m_tree->setUpdatesEnabled(true);
    m_tree->verticalScrollBar()->setValue(scroll);
    m_tree->setVisible(total > 0);
    m_noChanges->setVisible(total == 0);
}

QStringList GitPanel::selectedPaths(int *sectionOut) const
{
    QStringList out;
    int section = -1;
    for (QTreeWidgetItem *it : m_tree->selectedItems()) {
        if (it->data(0, RoleIsSection).toBool())
            continue;
        out << it->data(0, RolePath).toString();
        section = it->data(0, RoleSection).toInt();
    }
    if (sectionOut)
        *sectionOut = section;
    return out;
}

void GitPanel::openItem(QTreeWidgetItem *item)
{
    if (!item || item->data(0, RoleIsSection).toBool())
        return;
    const QString path = item->data(0, RolePath).toString();
    const int section = item->data(0, RoleSection).toInt();
    const GitFileChange *c = m_repo->changeFor(path);
    if (section == SecConflicts || (c && c->untracked)) {
        emit openFileRequested(path);
        return;
    }
    emit diffRequested(path, section == SecStaged ? GitDiffMode::Staged : GitDiffMode::Unstaged);
}

void GitPanel::onAction(const QString &path, int section, int action, bool isSection)
{
    QStringList paths;
    if (isSection) {
        for (const GitFileChange &c : changesForSection(section))
            paths << c.path;
    } else {
        paths << path;
    }
    switch (action) {
    case ActStage: m_repo->stage(paths); break;
    case ActUnstage: m_repo->unstage(paths); break;
    case ActDiscard: discardPaths(paths); break;
    case ActOpen: emit openFileRequested(path); break;
    }
}

bool GitPanel::confirmDiscard(const QList<GitFileChange> &changes, QWidget *parent)
{
    if (changes.isEmpty())
        return false;
    int untracked = 0;
    for (const GitFileChange &c : changes)
        untracked += c.untracked ? 1 : 0;
    QString text = changes.size() == 1
        ? tr("Discard changes to \"%1\"?").arg(QFileInfo(changes.first().relPath).fileName())
        : tr("Discard changes in %n file(s)?", nullptr, changes.size());
    if (untracked > 0)
        text += tr("\n\n%n untracked file(s) will be permanently deleted.", nullptr, untracked);
    text += tr("\n\nThis cannot be undone.");
    return QMessageBox::warning(parent, tr("Discard Changes"), text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

void GitPanel::discardPaths(const QStringList &paths)
{
    QList<GitFileChange> changes;
    for (const QString &p : paths)
        if (const GitFileChange *c = m_repo->changeFor(p))
            changes << *c;
    if (confirmDiscard(changes, this))
        m_repo->discard(paths);
}

void GitPanel::showTreeMenu(const QPoint &pos)
{
    QTreeWidgetItem *hit = m_tree->itemAt(pos);
    if (hit && !hit->isSelected()) {
        m_tree->clearSelection();
        if (!hit->data(0, RoleIsSection).toBool())
            hit->setSelected(true);
    }
    QMenu menu(this);
    if (hit && hit->data(0, RoleIsSection).toBool()) {
        const int section = hit->data(0, RoleSection).toInt();
        if (section == SecStaged) {
            menu.addAction(tr("Unstage All"), this, [this, section] { onAction({}, section, ActUnstage, true); });
        } else {
            menu.addAction(section == SecConflicts ? tr("Mark All as Resolved (Stage)") : tr("Stage All"), this, [this, section] { onAction({}, section, ActStage, true); });
            if (section == SecChanges)
                menu.addAction(tr("Discard All…"), this, [this, section] { onAction({}, section, ActDiscard, true); });
        }
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }
    int section = -1;
    const QStringList paths = selectedPaths(&section);
    if (paths.isEmpty())
        return;
    const QString first = paths.first();
    const bool single = paths.size() == 1;
    const GitFileChange *c = m_repo->changeFor(first);

    if (single) {
        menu.addAction(section == SecChanges && c && c->untracked ? tr("Open File") : tr("Open Changes"), this, [this] { openItem(m_tree->selectedItems().value(0)); });
        if (!(c && c->untracked) && section != SecConflicts)
            menu.addAction(tr("Open File"), this, [this, first] { emit openFileRequested(first); });
        menu.addSeparator();
    }
    if (section == SecStaged) {
        menu.addAction(tr("Unstage Changes"), this, [this, paths] { m_repo->unstage(paths); });
    } else {
        menu.addAction(section == SecConflicts ? tr("Mark as Resolved (Stage)") : tr("Stage Changes"), this, [this, paths] { m_repo->stage(paths); });
        if (section == SecChanges)
            menu.addAction(tr("Discard Changes…"), this, [this, paths] { discardPaths(paths); });
    }
    if (single) {
        menu.addSeparator();
        if (c && c->untracked)
            menu.addAction(tr("Add to .gitignore"), this, [this, first] { m_repo->addToGitignore(first); });
        menu.addAction(tr("Copy Path"), this, [first] { QApplication::clipboard()->setText(first); });
        menu.addAction(tr("Copy Relative Path"), this, [this, first] { QApplication::clipboard()->setText(m_repo->relativePath(first)); });
        menu.addAction(tr("Reveal in File Manager"), this, [first] { FileManager::revealInFileManager(first); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void GitPanel::showMoreMenu()
{
    QMenu *m = m_moreBtn->menu();
    m->clear();
    m->addAction(tr("Refresh"), m_repo, &GitRepository::refresh);
    m->addAction(tr("Fetch"), m_repo, &GitRepository::fetch);
    m->addAction(tr("Pull"), m_repo, &GitRepository::pull);
    m->addAction(tr("Push"), m_repo, &GitRepository::push);
    m->addSeparator();
    m->addAction(tr("Stage All"), this, [this] { m_repo->stageAll(); })->setEnabled(!m_repo->unstagedChanges().isEmpty());
    m->addAction(tr("Unstage All"), m_repo, &GitRepository::unstageAll)->setEnabled(!m_repo->stagedChanges().isEmpty());
    m->addAction(tr("Discard All Changes…"), this, [this] {
        QStringList paths;
        for (const GitFileChange &c : m_repo->unstagedChanges())
            paths << c.path;
        discardPaths(paths);
    })->setEnabled(!m_repo->unstagedChanges().isEmpty());
    m->addSeparator();
    m->addAction(tr("Stash Changes"), this, [this] { m_repo->stash(false); })->setEnabled(!m_repo->changes().isEmpty());
    m->addAction(tr("Stash Changes (Include Untracked)"), this, [this] { m_repo->stash(true); })->setEnabled(!m_repo->changes().isEmpty());
    m->addAction(tr("Pop Latest Stash"), m_repo, &GitRepository::stashPop);
    m->addSeparator();
    m->addAction(tr("Create Branch…"), this, [this] { promptNewBranch(m_repo, this); });
}

// --- Commit ------------------------------------------------------------------------------------------

void GitPanel::commit()
{
    const QString msg = m_message->toPlainText().trimmed();
    const bool amend = m_amend->isChecked();
    if (msg.isEmpty() && !amend) {
        QMessageBox::information(this, tr("Commit"), tr("Enter a commit message first."));
        m_message->setFocus();
        return;
    }
    if (m_repo->stagedChanges().isEmpty() && !amend) {
        if (m_repo->changes().isEmpty()) {
            QMessageBox::information(this, tr("Commit"), tr("There are no changes to commit."));
            return;
        }
        const auto answer = QMessageBox::question(this, tr("Commit"),
                                                   tr("There are no staged changes.\n\nStage all changes and commit them?"),
                                                   QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return;
        m_repo->stageAll(this, [this, msg, amend](bool ok) {
            if (ok)
                doCommit(msg, amend);
        });
        return;
    }
    doCommit(msg, amend);
}

void GitPanel::doCommit(const QString &message, bool amend)
{
    m_repo->commit(message, amend, this, [this](bool ok) {
        if (!ok)
            return;
        m_message->clear();
        m_prefilledAmend.clear();
        m_amend->setChecked(false);
    });
}

// --- History ---------------------------------------------------------------------------------------------

void GitPanel::loadHistory()
{
    m_historyHead = m_repo->headOid();
    m_repo->log(200, this, [this](const QList<GitCommitInfo> &commits) {
        m_history->clear();
        for (const GitCommitInfo &c : commits) {
            auto *it = new QTreeWidgetItem(m_history);
            it->setText(0, c.subject);
            it->setData(0, RoleHash, c.hash);
            it->setData(0, RoleRefs, c.refs);
            it->setData(0, RoleMeta, tr("%1 · %2 · %3").arg(c.shortHash, c.author, c.date));
            it->setToolTip(0, tr("%1\n%2 — %3\n\nDouble-click to view the changes").arg(c.hash, c.author, c.date));
        }
        if (commits.isEmpty()) {
            auto *it = new QTreeWidgetItem(m_history);
            it->setText(0, tr("No commits yet"));
            it->setFlags(Qt::NoItemFlags);
        }
    });
}

// --- Branch helpers (shared with the status bar) -------------------------------------------------

void GitPanel::populateBranchMenu(GitRepository *repo, QMenu *menu, QWidget *dialogParent)
{
    QMenu *remote = nullptr;
    QMenu *del = nullptr;
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    auto dot = [](const QColor &c) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        if (c.isValid()) {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QRectF(8, 8, 16, 16));
        }
        pm.setDevicePixelRatio(2.0);
        return QIcon(pm);
    };
    QSet<QString> locals;
    for (const GitBranchInfo &b : repo->branches())
        if (!b.remote)
            locals.insert(b.name);
    for (const GitBranchInfo &b : repo->branches()) {
        if (b.remote) {
            if (locals.contains(b.name.section(QLatin1Char('/'), 1)))
                continue;
            if (!remote)
                remote = new QMenu(QObject::tr("Remote Branches"), menu);
            const QString name = b.name;
            remote->addAction(name, repo, [repo, name] { repo->checkout(name); });
            continue;
        }
        const QString name = b.name;
        QAction *a = menu->addAction(dot(b.current ? theme.gitAdded : QColor()), name, repo, [repo, name] { repo->checkout(name); });
        if (b.current) {
            QFont f = menu->font();
            f.setBold(true);
            a->setFont(f);
        }
        if (!b.current) {
            if (!del)
                del = new QMenu(QObject::tr("Delete Branch"), menu);
            del->addAction(name, repo, [repo, name, dialogParent] {
                if (QMessageBox::warning(dialogParent, QObject::tr("Delete Branch"), QObject::tr("Delete the branch \"%1\"?").arg(name),
                                         QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                    repo->deleteBranch(name, false);
            });
        }
    }
    if (remote)
        menu->addMenu(remote);
    menu->addSeparator();
    menu->addAction(QObject::tr("Create New Branch…"), repo, [repo, dialogParent] { promptNewBranch(repo, dialogParent); });
    if (del)
        menu->addMenu(del);
}

void GitPanel::promptNewBranch(GitRepository *repo, QWidget *dialogParent)
{
    bool ok = false;
    const QString name = QInputDialog::getText(dialogParent, QObject::tr("Create Branch"),
                                               QObject::tr("New branch name (created from the current commit):"),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (ok && !name.isEmpty())
        repo->createBranch(name);
}
