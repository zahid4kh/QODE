#include "SearchPanel.h"

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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum Role {
    RoleKind = Qt::UserRole + 1, // 0 file, 1 match
    RolePath,
    RoleLine,
    RoleColumn,
    RoleLength,
    RolePreview,
    RolePreviewStart,
    RoleCount
};

class ResultDelegate : public QStyledItemDelegate
{
public:
    explicit ResultDelegate(QObject *parent)
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
            paintMatch(p, option, index);
        p->restore();
    }

private:
    void refresh() { m_theme = Theme::byName(SettingsManager::instance().theme()); }

    void paintFile(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        const QRect r = option.rect;
        const QString path = index.data(RolePath).toString();
        const QFileInfo fi(path);
        const QString rel = index.data(Qt::DisplayRole).toString();
        const QString dir = rel.contains(QLatin1Char('/')) ? rel.left(rel.lastIndexOf(QLatin1Char('/'))) : QString();
        int right = r.right() - 8;

        const QString count = QString::number(index.data(RoleCount).toInt());
        QFont pf = option.font;
        pf.setPointSizeF(qMax(7.0, pf.pointSizeF() - 1.5));
        pf.setWeight(QFont::Bold);
        const QFontMetrics pfm(pf);
        const int pw = qMax(18, pfm.horizontalAdvance(count) + 12);
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
            const QFontMetrics dfm(df);
            p->setFont(df);
            p->setPen(m_theme.textMuted);
            p->drawText(QRect(left, r.top(), right - left, r.height()), Qt::AlignVCenter | Qt::AlignLeft, dfm.elidedText(dir, Qt::ElideLeft, right - left));
        }
    }

    void paintMatch(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        const QRect r = option.rect;
        const QString text = index.data(RolePreview).toString();
        const int start = index.data(RolePreviewStart).toInt();
        const int len = index.data(RoleLength).toInt();

        QFont f = option.font;
        f.setFamily(SettingsManager::instance().editorFont().family());
        f.setPointSizeF(qMax(8.0, option.font.pointSizeF() - 1));
        const QFontMetrics fm(f);

        QFont nf = f;
        nf.setPointSizeF(qMax(7.0, f.pointSizeF() - 1));
        p->setFont(nf);
        p->setPen(m_theme.gutterFg);
        const QString num = QString::number(index.data(RoleLine).toInt());
        const int numW = 34;
        p->drawText(QRect(r.left() + 6, r.top(), numW, r.height()), Qt::AlignVCenter | Qt::AlignRight, num);

        const int x0 = r.left() + 6 + numW + 8;
        p->setClipRect(QRect(x0, r.top(), qMax(0, r.right() - x0 - 4), r.height()));
        const int mx = x0 + fm.horizontalAdvance(text.left(start));
        const int mw = fm.horizontalAdvance(text.mid(start, len));
        QColor hl = m_theme.accent;
        hl.setAlpha(m_theme.dark ? 70 : 55);
        p->setPen(Qt::NoPen);
        p->setBrush(hl);
        p->drawRoundedRect(QRect(mx - 1, r.top() + 3, mw + 2, r.height() - 6), 3, 3);
        p->setFont(f);
        p->setPen(m_theme.editorFg);
        p->drawText(QRect(x0, r.top(), 4000, r.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    }

    Theme m_theme;
};

QString results(int n)
{
    return n == 1 ? QObject::tr("1 result") : QObject::tr("%1 results").arg(n);
}

} // namespace

SearchPanel::SearchPanel(QWidget *parent)
    : QWidget(parent)
{
    m_search = new ProjectSearch(this);
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(320);
    connect(m_debounce, &QTimer::timeout, this, &SearchPanel::runSearch);

    auto toggle = [this](const QString &text, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setObjectName(QStringLiteral("findBtn"));
        b->setText(text);
        b->setCheckable(true);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(26, 24);
        b->setStyleSheet(QStringLiteral("QToolButton { padding: 0; }"));
        return b;
    };
    auto iconButton = [this](const QString &icon, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setObjectName(QStringLiteral("findBtn"));
        Icons::bind(b, icon);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setAutoRaise(true);
        b->setFixedSize(24, 24);
        b->setStyleSheet(QStringLiteral("QToolButton { padding: 0; }"));
        return b;
    };

    auto *title = new QLabel(tr("SEARCH"), this);
    title->setObjectName(QStringLiteral("panelTitle"));

    m_replaceToggle = iconButton(QStringLiteral(":/new-icons/chevron-right.svg"), tr("Toggle Replace"));
    m_replaceToggle->setCheckable(true);
    m_query = new QLineEdit(this);
    m_query->setPlaceholderText(tr("Search in project"));
    m_query->setClearButtonEnabled(true);
    m_case = toggle(QStringLiteral("Aa"), tr("Match case"));
    m_word = toggle(QStringLiteral("ab"), tr("Match whole word"));
    m_regex = toggle(QStringLiteral(".*"), tr("Use regular expression"));

    auto *queryRow = new QHBoxLayout;
    queryRow->setContentsMargins(0, 0, 0, 0);
    queryRow->setSpacing(3);
    queryRow->addWidget(m_replaceToggle);
    queryRow->addWidget(m_query, 1);
    queryRow->addWidget(m_case);
    queryRow->addWidget(m_word);
    queryRow->addWidget(m_regex);

    m_replaceRow = new QWidget(this);
    m_replace = new QLineEdit(m_replaceRow);
    m_replace->setPlaceholderText(tr("Replace"));
    m_replaceAll = new QPushButton(tr("Replace All"), m_replaceRow);
    m_replaceAll->setAutoDefault(false);
    auto *rl = new QHBoxLayout(m_replaceRow);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(3);
    rl->addSpacing(m_replaceToggle->minimumWidth());
    rl->addWidget(m_replace, 1);
    rl->addWidget(m_replaceAll);
    m_replaceRow->hide();

    m_filterRow = new QWidget(this);
    m_include = new QLineEdit(m_filterRow);
    m_include->setPlaceholderText(tr("Files to include, e.g. *.cpp, src/*"));
    m_exclude = new QLineEdit(m_filterRow);
    m_exclude->setPlaceholderText(tr("Files to exclude, e.g. *.min.js, tests"));
    auto *fl = new QVBoxLayout(m_filterRow);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(4);
    fl->addWidget(m_include);
    fl->addWidget(m_exclude);
    m_filterRow->hide();

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("emptyText"));
    m_summary->setWordWrap(true);
    m_filterToggle = iconButton(QStringLiteral(":/new-icons/ellipsis.svg"), tr("Toggle file filters"));
    m_filterToggle->setCheckable(true);
    m_refresh = iconButton(QStringLiteral(":/new-icons/refresh-cw.svg"), tr("Search again"));
    m_collapse = iconButton(QStringLiteral(":/new-icons/chevron-down.svg"), tr("Collapse all"));
    m_clear = iconButton(QStringLiteral(":/new-icons/x.svg"), tr("Clear results"));

    auto *barRow = new QHBoxLayout;
    barRow->setContentsMargins(0, 0, 0, 0);
    barRow->setSpacing(2);
    barRow->addWidget(m_summary, 1);
    barRow->addWidget(m_filterToggle);
    barRow->addWidget(m_refresh);
    barRow->addWidget(m_collapse);
    barRow->addWidget(m_clear);

    auto *form = new QWidget(this);
    auto *fv = new QVBoxLayout(form);
    fv->setContentsMargins(8, 8, 8, 4);
    fv->setSpacing(5);
    fv->addLayout(queryRow);
    fv->addWidget(m_replaceRow);
    fv->addWidget(m_filterRow);
    fv->addLayout(barRow);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setUniformRowHeights(true);
    m_tree->setIndentation(6);
    m_tree->setRootIsDecorated(true);
    m_tree->setMouseTracking(true);
    m_tree->setItemDelegate(new ResultDelegate(m_tree));
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->setExpandsOnDoubleClick(false);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(title);
    lay->addWidget(form);
    lay->addWidget(m_tree, 1);

    connect(m_search, &ProjectSearch::results, this, &SearchPanel::onResults);
    connect(m_search, &ProjectSearch::finished, this, &SearchPanel::onFinished);
    connect(m_query, &QLineEdit::textChanged, this, &SearchPanel::scheduleSearch);
    connect(m_query, &QLineEdit::returnPressed, this, &SearchPanel::runSearch);
    for (QLineEdit *e : {m_include, m_exclude}) {
        connect(e, &QLineEdit::textChanged, this, &SearchPanel::scheduleSearch);
        connect(e, &QLineEdit::returnPressed, this, &SearchPanel::runSearch);
    }
    for (QToolButton *b : {m_case, m_word, m_regex})
        connect(b, &QToolButton::toggled, this, &SearchPanel::scheduleSearch);
    connect(m_replaceToggle, &QToolButton::toggled, this, [this](bool on) {
        m_replaceRow->setVisible(on);
        Icons::bind(m_replaceToggle, on ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"));
        if (on)
            m_replace->setFocus();
    });
    connect(m_filterToggle, &QToolButton::toggled, m_filterRow, &QWidget::setVisible);
    connect(m_refresh, &QToolButton::clicked, this, &SearchPanel::runSearch);
    connect(m_clear, &QToolButton::clicked, this, [this] {
        m_query->clear();
        m_search->cancel();
        clearResults();
        m_searching = false;
        updateSummary();
        m_query->setFocus();
    });
    connect(m_collapse, &QToolButton::clicked, this, [this] {
        m_collapsed = !m_collapsed;
        if (m_collapsed)
            m_tree->collapseAll();
        else
            m_tree->expandAll();
        Icons::bind(m_collapse, m_collapsed ? QStringLiteral(":/new-icons/chevron-right.svg") : QStringLiteral(":/new-icons/chevron-down.svg"));
        m_collapse->setToolTip(m_collapsed ? tr("Expand all") : tr("Collapse all"));
    });
    connect(m_replaceAll, &QPushButton::clicked, this, &SearchPanel::replaceAll);
    connect(m_tree, &QTreeWidget::itemClicked, this, &SearchPanel::onItemClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &SearchPanel::showContextMenu);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { applyTheme(); });
    applyTheme();
    updateSummary();
}

void SearchPanel::applyTheme()
{
    m_tree->viewport()->update();
    updateSummary();
}

SearchOptions SearchPanel::options() const
{
    SearchOptions o;
    o.query = m_query->text();
    o.caseSensitive = m_case->isChecked();
    o.wholeWord = m_word->isChecked();
    o.regex = m_regex->isChecked();
    o.include = m_include->text();
    o.exclude = m_exclude->text();
    return o;
}

QString SearchPanel::replacement() const
{
    return m_replace->text();
}

void SearchPanel::setProjectRoot(const QString &root)
{
    m_root = root;
    m_search->cancel();
    clearResults();
    m_searching = false;
    updateSummary();
    if (!root.isEmpty() && !m_query->text().isEmpty())
        scheduleSearch();
}

void SearchPanel::focusQuery(const QString &prefill)
{
    if (!prefill.isEmpty())
        m_query->setText(prefill);
    m_query->setFocus();
    m_query->selectAll();
    if (!prefill.isEmpty())
        runSearch();
}

void SearchPanel::scheduleSearch()
{
    if (m_query->text().isEmpty()) {
        m_debounce->stop();
        m_search->cancel();
        clearResults();
        m_searching = false;
        updateSummary();
        return;
    }
    m_debounce->start();
}

void SearchPanel::rerun()
{
    runSearch();
}

void SearchPanel::runSearch()
{
    m_debounce->stop();
    const SearchOptions o = options();
    m_search->cancel();
    clearResults();
    if (o.query.isEmpty() || m_root.isEmpty()) {
        m_searching = false;
        updateSummary();
        return;
    }
    m_searching = true;
    m_error.clear();
    updateSummary();
    m_search->start(m_root, o, m_overrides ? m_overrides() : QHash<QString, QString>());
}

void SearchPanel::clearResults()
{
    m_tree->clear();
    m_files = m_matches = 0;
    m_truncated = false;
    m_error.clear();
    m_collapsed = false;
    Icons::bind(m_collapse, QStringLiteral(":/new-icons/chevron-down.svg"));
}

void SearchPanel::onResults(const QList<SearchFileResult> &batch)
{
    m_tree->setUpdatesEnabled(false);
    const QDir root(m_root);
    for (const SearchFileResult &fr : batch) {
        auto *fileItem = new QTreeWidgetItem(m_tree);
        fileItem->setData(0, Qt::DisplayRole, root.relativeFilePath(fr.path));
        fileItem->setData(0, RoleKind, 0);
        fileItem->setData(0, RolePath, fr.path);
        fileItem->setData(0, RoleCount, fr.matches.size());
        fileItem->setToolTip(0, QDir::toNativeSeparators(fr.path));
        for (const SearchMatch &m : fr.matches) {
            auto *it = new QTreeWidgetItem(fileItem);
            it->setData(0, RoleKind, 1);
            it->setData(0, RolePath, fr.path);
            it->setData(0, RoleLine, m.line);
            it->setData(0, RoleColumn, m.column);
            it->setData(0, RoleLength, m.length);
            it->setData(0, RolePreview, m.preview);
            it->setData(0, RolePreviewStart, m.previewStart);
        }
        fileItem->setExpanded(!m_collapsed);
        ++m_files;
        m_matches += fr.matches.size();
    }
    m_tree->setUpdatesEnabled(true);
    updateSummary();
}

void SearchPanel::onFinished(int files, int matches, bool truncated, const QString &error)
{
    m_searching = false;
    m_error = error;
    m_truncated = truncated;
    if (error.isEmpty() && !m_query->text().isEmpty()) {
        m_files = files;
        m_matches = matches;
    }
    updateSummary();
}

void SearchPanel::updateSummary()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QString text;
    QColor color = t.textMuted;
    if (m_root.isEmpty()) {
        text = tr("Open a project to search its files.");
    } else if (!m_error.isEmpty()) {
        text = m_error;
        color = t.gitConflict;
    } else if (m_query->text().isEmpty()) {
        text = tr("Search every file in the project.");
    } else if (m_searching) {
        text = m_matches > 0 ? tr("Searching… %1 so far").arg(results(m_matches)) : tr("Searching…");
    } else if (m_matches == 0) {
        text = tr("No results found.");
    } else {
        text = tr("%1 in %2").arg(results(m_matches), m_files == 1 ? tr("1 file") : tr("%1 files").arg(m_files));
        if (m_truncated)
            text += tr(" — limited to the first %1").arg(m_matches);
    }
    m_summary->setText(text);
    QPalette pal = m_summary->palette();
    pal.setColor(QPalette::WindowText, color);
    m_summary->setPalette(pal);
    m_summary->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    const bool has = m_matches > 0 && !m_searching;
    m_replaceAll->setEnabled(has);
    m_collapse->setEnabled(m_tree->topLevelItemCount() > 0);
    m_clear->setEnabled(!m_query->text().isEmpty() || m_tree->topLevelItemCount() > 0);
}

void SearchPanel::onItemClicked(QTreeWidgetItem *item)
{
    if (!item)
        return;
    if (item->data(0, RoleKind).toInt() == 0) {
        item->setExpanded(!item->isExpanded());
        return;
    }
    emit openMatch(item->data(0, RolePath).toString(), item->data(0, RoleLine).toInt(), item->data(0, RoleColumn).toInt(),
                   item->data(0, RoleLength).toInt());
}

QStringList SearchPanel::listedPaths(int *matchCount) const
{
    QStringList paths;
    int matches = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_tree->topLevelItem(i);
        paths << it->data(0, RolePath).toString();
        matches += it->data(0, RoleCount).toInt();
    }
    if (matchCount)
        *matchCount = matches;
    return paths;
}

void SearchPanel::confirmAndReplace(const QStringList &paths, int matches)
{
    if (paths.isEmpty())
        return;
    const QString repl = m_replace->text();
    const QString count = matches == 1 ? tr("1 occurrence") : tr("%1 occurrences").arg(matches);
    const QString what = repl.isEmpty() ? tr("Delete %1").arg(count) : tr("Replace %1").arg(count);
    const QString files = paths.size() == 1 ? tr("1 file") : tr("%1 files").arg(paths.size());
    const auto answer = QMessageBox::question(this, tr("Replace in Files"),
                                              tr("%1 in %2?\n\nFiles that are open are edited in the editor (undoable, not saved). "
                                                 "Other files are rewritten on disk.").arg(what, files),
                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Yes)
        emit replaceRequested(paths, options(), repl);
}

void SearchPanel::replaceAll()
{
    int matches = 0;
    const QStringList paths = listedPaths(&matches);
    confirmAndReplace(paths, matches);
}

void SearchPanel::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item)
        return;
    const QString path = item->data(0, RolePath).toString();
    QTreeWidgetItem *fileItem = item->parent() ? item->parent() : item;
    QMenu menu(this);
    menu.addAction(tr("Open File"), this, [this, path] { emit openMatch(path, 0, 0, 0); });
    menu.addAction(tr("Replace in This File…"), this, [this, path, fileItem] {
        confirmAndReplace({path}, fileItem->data(0, RoleCount).toInt());
    });
    menu.addAction(tr("Dismiss"), this, [this, fileItem] {
        m_matches -= fileItem->data(0, RoleCount).toInt();
        --m_files;
        delete fileItem;
        updateSummary();
    });
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(path); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}
