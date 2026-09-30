#include "TasksPanel.h"

#include "explorer/FileIcons.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum Role {
    RoleKind = Qt::UserRole + 1, // 0 = file row, 1 = entry row
    RolePath,
    RoleLine, // 1-based
    RoleColumn,
    RoleLength,
    RoleTag,
    RoleText,
    RoleCount
};

const QStringList &todoTags()
{
    static const QStringList tags = {QStringLiteral("TODO"), QStringLiteral("FIXME"), QStringLiteral("HACK"), QStringLiteral("XXX"),
                                     QStringLiteral("BUG")};
    return tags;
}

// A tag only counts right after a comment opener, so identifiers and strings are not picked up.
QString todoPattern()
{
    return QStringLiteral("(?://+|#+|/\\*+|\\*|--|<!--|;+|%+)\\s*@?\\b(?:") + todoTags().join(QLatin1Char('|')) + QStringLiteral(")\\b");
}

QColor tagColor(const Theme &t, const QString &tag)
{
    if (tag == QLatin1String("FIXME") || tag == QLatin1String("BUG"))
        return t.gitDeleted;
    if (tag == QLatin1String("HACK") || tag == QLatin1String("XXX"))
        return t.gitModified;
    return t.accent;
}

class TaskDelegate : public QStyledItemDelegate
{
public:
    explicit TaskDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
        refresh();
        connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { refresh(); });
    }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(0, 24); }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QRect r = option.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool sel = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        if (sel || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(sel ? m_theme.selection : m_theme.currentLine);
            p->drawRect(QRect(0, r.top(), option.widget ? option.widget->width() : r.right(), r.height()));
        }
        if (index.data(RoleKind).toInt() == 0)
            paintFile(p, option, index);
        else
            paintEntry(p, option, index);
        p->restore();
    }

private:
    void refresh() { m_theme = Theme::byName(SettingsManager::instance().theme()); }

    void paintFile(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        const QRect r = option.rect;
        const QFileInfo fi(index.data(RolePath).toString());
        const QString rel = index.data(Qt::DisplayRole).toString();
        const QString dir = rel.contains(QLatin1Char('/')) ? rel.left(rel.lastIndexOf(QLatin1Char('/'))) : QString();
        int right = r.right() - 8;

        const QString count = QString::number(index.data(RoleCount).toInt());
        QFont pf = option.font;
        pf.setPointSizeF(qMax(7.0, pf.pointSizeF() - 1.5));
        pf.setWeight(QFont::Bold);
        const int pw = qMax(18, QFontMetrics(pf).horizontalAdvance(count) + 12);
        const QRect pill(right - pw, r.center().y() - 8, pw, 16);
        QColor bg = m_theme.accent;
        bg.setAlpha(m_theme.dark ? 48 : 36);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(pill, 8, 8);
        p->setFont(pf);
        p->setPen(m_theme.accent);
        p->drawText(pill, Qt::AlignCenter, count);
        right = pill.left() - 8;

        int left = r.left() + 4;
        FileIcons::forFile(fi.fileName()).paint(p, QRect(left, r.center().y() - 7, 14, 14));
        left += 20;
        QFont nf = option.font;
        nf.setWeight(QFont::DemiBold);
        const QFontMetrics nfm(nf);
        const int nameW = qMin(nfm.horizontalAdvance(fi.fileName()) + 3, qMax(0, right - left));
        p->setFont(nf);
        p->setPen(m_theme.editorFg);
        p->drawText(QRect(left, r.top(), nameW, r.height()), Qt::AlignVCenter | Qt::AlignLeft, nfm.elidedText(fi.fileName(), Qt::ElideRight, nameW));
        left += nameW + 8;
        if (!dir.isEmpty() && right - left > 20) {
            QFont df = option.font;
            df.setPointSizeF(qMax(7.0, df.pointSizeF() - 1));
            p->setFont(df);
            p->setPen(m_theme.textMuted);
            p->drawText(QRect(left, r.top(), right - left, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                        QFontMetrics(df).elidedText(dir, Qt::ElideLeft, right - left));
        }
    }

    void paintEntry(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        const QRect r = option.rect;
        QFont nf = option.font;
        nf.setPointSizeF(qMax(7.0, nf.pointSizeF() - 1.5));
        p->setFont(nf);
        p->setPen(m_theme.gutterFg);
        p->drawText(QRect(r.left() + 6, r.top(), 34, r.height()), Qt::AlignVCenter | Qt::AlignRight, QString::number(index.data(RoleLine).toInt()));

        int x = r.left() + 6 + 34 + 8;
        const QString tag = index.data(RoleTag).toString();
        if (!tag.isEmpty()) {
            QFont tf = nf;
            tf.setWeight(QFont::Bold);
            const QFontMetrics tfm(tf);
            const int tw = tfm.horizontalAdvance(tag) + 10;
            QColor c = tagColor(m_theme, tag);
            QColor bg = c;
            bg.setAlpha(m_theme.dark ? 50 : 38);
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(QRect(x, r.center().y() - 8, tw, 16), 4, 4);
            p->setFont(tf);
            p->setPen(c);
            p->drawText(QRect(x, r.top(), tw, r.height()), Qt::AlignCenter, tag);
            x += tw + 6;
        }
        QFont f = option.font;
        f.setPointSizeF(qMax(8.0, option.font.pointSizeF() - 1));
        p->setFont(f);
        p->setPen(m_theme.editorFg);
        const int w = qMax(0, r.right() - x - 6);
        p->drawText(QRect(x, r.top(), w, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(f).elidedText(index.data(RoleText).toString(), Qt::ElideRight, w));
    }

    Theme m_theme;
};

QTreeWidget *makeTree(QWidget *parent)
{
    auto *t = new QTreeWidget(parent);
    t->setHeaderHidden(true);
    t->setFrameShape(QFrame::NoFrame);
    t->setUniformRowHeights(true);
    t->setIndentation(6);
    t->setRootIsDecorated(true);
    t->setMouseTracking(true);
    t->setItemDelegate(new TaskDelegate(t));
    t->setContextMenuPolicy(Qt::CustomContextMenu);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    t->setExpandsOnDoubleClick(false);
    return t;
}

} // namespace

TasksPanel::TasksPanel(QWidget *parent)
    : QWidget(parent)
{
    m_search = new ProjectSearch(this);
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(900);

    auto *title = new QLabel(tr("TASKS"), this);
    title->setObjectName(QStringLiteral("panelTitle"));

    auto makeTab = [this](const QString &text) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    m_todoBtn = makeTab(tr("TODO"));
    m_markBtn = makeTab(tr("Bookmarks"));
    m_todoBtn->setChecked(true);
    m_refresh = new QToolButton(this);
    Icons::bind(m_refresh, QStringLiteral(":/new-icons/refresh-cw.svg"));
    m_refresh->setToolTip(tr("Scan the project again"));
    m_refresh->setAutoRaise(true);
    m_filterBtn = new QToolButton(this);
    m_filterBtn->setText(tr("All"));
    m_filterBtn->setToolTip(tr("Filter by tag"));
    m_filterBtn->setAutoRaise(true);
    m_filterBtn->setPopupMode(QToolButton::InstantPopup);
    auto *filterMenu = new QMenu(m_filterBtn);
    auto addTag = [&](const QString &label, const QString &tag) {
        filterMenu->addAction(label, this, [this, label, tag] {
            m_tagFilter = tag;
            m_filterBtn->setText(label);
            applyFilter();
        });
    };
    addTag(tr("All"), QString());
    for (const QString &tag : todoTags())
        addTag(tag, tag);
    m_filterBtn->setMenu(filterMenu);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(8, 6, 8, 2);
    bar->setSpacing(4);
    bar->addWidget(m_todoBtn);
    bar->addWidget(m_markBtn);
    bar->addStretch(1);
    bar->addWidget(m_filterBtn);
    bar->addWidget(m_refresh);

    m_summary = new QLabel(this);
    m_summary->setContentsMargins(10, 2, 10, 4);
    m_summary->setWordWrap(true);

    m_todoTree = makeTree(this);
    m_markTree = makeTree(this);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_todoTree);
    m_stack->addWidget(m_markTree);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(title);
    lay->addLayout(bar);
    lay->addWidget(m_summary);
    lay->addWidget(m_stack, 1);

    connect(m_todoBtn, &QToolButton::clicked, this, &TasksPanel::showTodos);
    connect(m_markBtn, &QToolButton::clicked, this, &TasksPanel::showBookmarks);
    connect(m_refresh, &QToolButton::clicked, this, &TasksPanel::scanTodos);
    connect(m_debounce, &QTimer::timeout, this, &TasksPanel::scanTodos);
    connect(m_search, &ProjectSearch::results, this, &TasksPanel::onResults);
    connect(m_search, &ProjectSearch::finished, this, &TasksPanel::onFinished);
    for (QTreeWidget *t : {m_todoTree, m_markTree}) {
        connect(t, &QTreeWidget::itemClicked, this, &TasksPanel::activate);
        connect(t, &QTreeWidget::itemActivated, this, &TasksPanel::activate);
        connect(t, &QWidget::customContextMenuRequested, this, &TasksPanel::showContextMenu);
    }
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] {
        m_todoTree->viewport()->update();
        m_markTree->viewport()->update();
        updateSummary();
    });
    updateSummary();
}

QString TasksPanel::relative(const QString &path) const
{
    return m_root.isEmpty() ? path : QDir(m_root).relativeFilePath(path);
}

void TasksPanel::setProjectRoot(const QString &root)
{
    m_root = root;
    m_search->cancel();
    m_scanning = false;
    m_todoTree->clear();
    m_todoCount = 0;
    m_error.clear();
    updateSummary();
    emit countsChanged();
    if (!root.isEmpty())
        scheduleTodoScan();
}

void TasksPanel::scheduleTodoScan()
{
    if (!m_root.isEmpty())
        m_debounce->start();
}

void TasksPanel::scanTodos()
{
    m_debounce->stop();
    m_search->cancel();
    m_todoTree->clear();
    m_todoCount = 0;
    m_truncated = false;
    m_error.clear();
    if (m_root.isEmpty()) {
        m_scanning = false;
        updateSummary();
        emit countsChanged();
        return;
    }
    SearchOptions o;
    o.query = todoPattern();
    o.regex = true;
    o.caseSensitive = true;
    m_scanning = true;
    updateSummary();
    m_search->start(m_root, o, m_overrides ? m_overrides() : QHash<QString, QString>());
}

void TasksPanel::onResults(const QList<SearchFileResult> &batch)
{
    static const QRegularExpression tagRx(QStringLiteral("\\b(") + todoTags().join(QLatin1Char('|')) + QStringLiteral(")\\b[\\s:(@\\w-]*?[):]?\\s*(.*)$"));
    m_todoTree->setUpdatesEnabled(false);
    for (const SearchFileResult &fr : batch) {
        auto *fileItem = new QTreeWidgetItem(m_todoTree);
        fileItem->setData(0, Qt::DisplayRole, relative(fr.path));
        fileItem->setData(0, RoleKind, 0);
        fileItem->setData(0, RolePath, fr.path);
        fileItem->setToolTip(0, QDir::toNativeSeparators(fr.path));
        int n = 0;
        for (const SearchMatch &m : fr.matches) {
            const auto mm = tagRx.match(m.preview, qMax(0, m.previewStart));
            if (!mm.hasMatch())
                continue;
            auto *it = new QTreeWidgetItem(fileItem);
            it->setData(0, RoleKind, 1);
            it->setData(0, RolePath, fr.path);
            it->setData(0, RoleLine, m.line);
            it->setData(0, RoleColumn, m.column);
            it->setData(0, RoleLength, m.length);
            it->setData(0, RoleTag, mm.captured(1));
            const QString msg = mm.captured(2).trimmed();
            it->setData(0, RoleText, msg.isEmpty() ? m.preview.trimmed() : msg);
            ++n;
        }
        fileItem->setData(0, RoleCount, n);
        fileItem->setExpanded(true);
        m_todoCount += n;
    }
    m_todoTree->setUpdatesEnabled(true);
    applyFilter();
}

void TasksPanel::onFinished(int, int, bool truncated, const QString &error)
{
    m_scanning = false;
    m_truncated = truncated;
    m_error = error;
    applyFilter();
    emit countsChanged();
}

// Hides entries that do not match the tag filter (and files left without entries) and recounts.
void TasksPanel::applyFilter()
{
    int shown = 0;
    for (int i = 0; i < m_todoTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *f = m_todoTree->topLevelItem(i);
        int n = 0;
        for (int j = 0; j < f->childCount(); ++j) {
            QTreeWidgetItem *c = f->child(j);
            const bool vis = m_tagFilter.isEmpty() || c->data(0, RoleTag).toString() == m_tagFilter;
            c->setHidden(!vis);
            n += vis;
        }
        f->setData(0, RoleCount, n);
        f->setHidden(n == 0);
        shown += n;
    }
    Q_UNUSED(shown)
    updateSummary();
}

void TasksPanel::setBookmarks(const QHash<QString, QList<int>> &bookmarks)
{
    m_bookmarks = bookmarks;
    rebuildBookmarks();
}

void TasksPanel::rebuildBookmarks()
{
    m_markTree->clear();
    m_bookmarkCount = 0;
    QStringList paths = m_bookmarks.keys();
    std::sort(paths.begin(), paths.end());
    for (const QString &path : paths) {
        QList<int> lines = m_bookmarks.value(path);
        if (lines.isEmpty())
            continue;
        std::sort(lines.begin(), lines.end());
        auto *fileItem = new QTreeWidgetItem(m_markTree);
        fileItem->setData(0, Qt::DisplayRole, relative(path));
        fileItem->setData(0, RoleKind, 0);
        fileItem->setData(0, RolePath, path);
        fileItem->setData(0, RoleCount, lines.size());
        fileItem->setToolTip(0, QDir::toNativeSeparators(path));
        for (int line : lines) {
            auto *it = new QTreeWidgetItem(fileItem);
            it->setData(0, RoleKind, 1);
            it->setData(0, RolePath, path);
            it->setData(0, RoleLine, line + 1);
            it->setData(0, RoleText, (m_lineText ? m_lineText(path, line) : QString()).trimmed());
        }
        fileItem->setExpanded(true);
        m_bookmarkCount += lines.size();
    }
    updateSummary();
    emit countsChanged();
}

void TasksPanel::showBookmarks()
{
    m_markBtn->setChecked(true);
    m_stack->setCurrentWidget(m_markTree);
    m_filterBtn->setVisible(false);
    m_refresh->setVisible(false);
    updateSummary();
}

void TasksPanel::showTodos()
{
    m_todoBtn->setChecked(true);
    m_stack->setCurrentWidget(m_todoTree);
    m_filterBtn->setVisible(true);
    m_refresh->setVisible(true);
    updateSummary();
}

void TasksPanel::updateSummary()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QString text;
    QColor color = t.textMuted;
    const bool todos = m_stack->currentWidget() == m_todoTree;
    m_todoBtn->setText(m_todoCount > 0 ? tr("TODO (%1)").arg(m_todoCount) : tr("TODO"));
    m_markBtn->setText(m_bookmarkCount > 0 ? tr("Bookmarks (%1)").arg(m_bookmarkCount) : tr("Bookmarks"));
    if (todos) {
        int shown = 0;
        for (int i = 0; i < m_todoTree->topLevelItemCount(); ++i)
            shown += m_todoTree->topLevelItem(i)->data(0, RoleCount).toInt();
        if (m_root.isEmpty())
            text = tr("Open a project to find TODO comments.");
        else if (!m_error.isEmpty()) {
            text = m_error;
            color = t.gitDeleted;
        } else if (m_scanning)
            text = tr("Scanning…");
        else if (shown == 0)
            text = m_todoCount == 0 ? tr("No TODO, FIXME, HACK, XXX or BUG comments found.") : tr("No comments with this tag.");
        else
            text = tr("%n comment(s)", nullptr, shown) + (m_truncated ? tr(" (limit reached)") : QString());
    } else {
        text = m_bookmarkCount == 0 ? tr("No bookmarks yet. Press Ctrl+F2 on a line to add one.")
                                    : tr("%n bookmark(s)", nullptr, m_bookmarkCount);
    }
    m_summary->setText(text);
    m_summary->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
}

void TasksPanel::activate(QTreeWidgetItem *item)
{
    if (!item)
        return;
    if (item->data(0, RoleKind).toInt() == 0) {
        item->setExpanded(!item->isExpanded());
        return;
    }
    const int line = item->data(0, RoleLine).toInt();
    emit openLocation(item->data(0, RolePath).toString(), line, item->data(0, RoleColumn).toInt(), item->data(0, RoleLength).toInt());
}

void TasksPanel::showContextMenu(const QPoint &pos)
{
    auto *tree = qobject_cast<QTreeWidget *>(sender());
    if (!tree)
        return;
    QTreeWidgetItem *item = tree->itemAt(pos);
    QMenu menu(this);
    const bool marks = tree == m_markTree;
    if (item) {
        const QString path = item->data(0, RolePath).toString();
        menu.addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(path); });
        if (marks && item->data(0, RoleKind).toInt() == 1) {
            const int line = item->data(0, RoleLine).toInt() - 1;
            menu.addAction(tr("Remove Bookmark"), this, [this, path, line] { emit removeBookmark(path, line); });
        }
    }
    if (marks && m_bookmarkCount > 0)
        menu.addAction(tr("Clear All Bookmarks"), this, &TasksPanel::clearBookmarksRequested);
    if (!menu.isEmpty())
        menu.exec(tree->viewport()->mapToGlobal(pos));
}
