#include "CompletionPopup.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QListView>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

Q_DECLARE_METATYPE(const LspCompletionItem *)

namespace {
constexpr int kRowHeight = 24;
constexpr int kMaxRows = 10;
constexpr int kBadge = 18;

struct Badge {
    QChar letter;
    int color; // 0 function, 1 type, 2 keyword, 3 variable/other
};

Badge badgeFor(int kind)
{
    switch (kind) {
    case 2: case 3: case 4: return {QChar(0x0192), 0}; // ƒ
    case 5: case 10: return {QLatin1Char('f'), 3};
    case 6: case 12: return {QLatin1Char('v'), 3};
    case 7: return {QLatin1Char('C'), 1};
    case 22: return {QLatin1Char('S'), 1};
    case 8: return {QLatin1Char('I'), 1};
    case 13: return {QLatin1Char('E'), 1};
    case 20: return {QLatin1Char('e'), 3};
    case 21: return {QLatin1Char('c'), 3};
    case 9: return {QLatin1Char('M'), 1};
    case 14: return {QLatin1Char('k'), 2};
    case 15: return {QLatin1Char('s'), 2};
    case 25: return {QLatin1Char('T'), 1};
    case 17: case 19: return {QLatin1Char('F'), 3};
    default: return {QLatin1Char('t'), 3};
    }
}

QString shownLabel(const QString &label)
{
    int i = 0;
    while (i < label.size() && (label[i].isSpace() || label[i] == QChar(0x2022))) // clangd marks "needs #include" with a bullet
        ++i;
    return label.mid(i);
}

// Lower is better, -1 = no match.
int matchScore(const QString &text, const QString &prefix)
{
    if (prefix.isEmpty())
        return 0;
    if (text.startsWith(prefix))
        return 0;
    if (text.startsWith(prefix, Qt::CaseInsensitive))
        return 1;
    // Subsequence, the first character anchored; word starts (after '_' or a lower->upper change) score better.
    if (text.isEmpty() || text[0].toLower() != prefix[0].toLower())
        return -1;
    int ti = 1, boundary = 0;
    for (int pi = 1; pi < prefix.size(); ++pi) {
        const QChar want = prefix[pi].toLower();
        bool found = false;
        for (; ti < text.size(); ++ti) {
            if (text[ti].toLower() == want) {
                if (text[ti - 1] == QLatin1Char('_') || (text[ti].isUpper() && text[ti - 1].isLower()))
                    ++boundary;
                ++ti;
                found = true;
                break;
            }
        }
        if (!found)
            return -1;
    }
    return boundary >= prefix.size() - 1 ? 2 : 3;
}
} // namespace

class CompletionPopup::Model : public QAbstractListModel
{
public:
    explicit Model(QObject *parent) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_rows.size(); }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size())
            return {};
        switch (role) {
        case Qt::DisplayRole: return shownLabel(m_all[m_rows[index.row()]].label);
        case Qt::UserRole: return QVariant::fromValue<const LspCompletionItem *>(&m_all[m_rows[index.row()]]);
        case Qt::UserRole + 1: return m_prefix;
        default: return {};
        }
    }

    void setAll(const QVector<LspCompletionItem> &items, const QVector<LspCompletionItem> &extra)
    {
        beginResetModel();
        m_all = items;
        m_all += extra;
        m_rows.clear();
        endResetModel();
    }
    void filter(const QString &prefix)
    {
        struct Hit {
            int score, index;
        };
        QVector<Hit> hits;
        hits.reserve(m_all.size());
        for (int i = 0; i < m_all.size(); ++i) {
            const LspCompletionItem &it = m_all[i];
            const int s = matchScore(it.filterText.isEmpty() ? shownLabel(it.label) : it.filterText, prefix);
            if (s >= 0 && !(it.builtin && s > 1)) // our snippets match by prefix only, never by subsequence
                hits.append({s, i});
        }
        std::stable_sort(hits.begin(), hits.end(), [this](const Hit &a, const Hit &b) {
            if (a.score != b.score)
                return a.score < b.score;
            const LspCompletionItem &x = m_all[a.index], &y = m_all[b.index];
            if (x.deprecated != y.deprecated)
                return !x.deprecated;
            return x.sortText < y.sortText;
        });
        beginResetModel();
        m_rows.clear();
        for (const Hit &h : hits)
            m_rows.append(h.index);
        m_prefix = prefix;
        endResetModel();
    }
    const LspCompletionItem &item(int row) const { return m_all[m_rows[row]]; }
    const QString &prefix() const { return m_prefix; }
    int widest(const QFontMetrics &fm) const
    {
        int w = 0;
        const int n = qMin(m_rows.size(), 200);
        for (int r = 0; r < n; ++r) {
            const LspCompletionItem &it = item(r);
            w = qMax(w, fm.horizontalAdvance(shownLabel(it.label) + it.signature) + fm.horizontalAdvance(it.detail) + 24);
        }
        return w;
    }

private:
    QVector<LspCompletionItem> m_all;
    QVector<int> m_rows;
    QString m_prefix;
};

namespace {
class Delegate : public QStyledItemDelegate
{
public:
    Delegate(const Theme &theme, QObject *parent) : QStyledItemDelegate(parent), m_theme(theme) {}
    void setTheme(const Theme &t) { m_theme = t; }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {0, kRowHeight}; }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        // The model is private to CompletionPopup; its items are reached through the roles below.
        const auto *item = index.data(Qt::UserRole).value<const LspCompletionItem *>();
        if (!item)
            return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect;
        const bool sel = opt.state & QStyle::State_Selected;
        if (sel)
            p->fillRect(r.adjusted(4, 1, -4, -1), m_theme.selection);
        else if (opt.state & QStyle::State_MouseOver)
            p->fillRect(r.adjusted(4, 1, -4, -1), m_theme.currentLine);

        // kind badge
        const Badge b = badgeFor(item->kind);
        const QColor c = b.color == 0 ? m_theme.function : b.color == 1 ? m_theme.type : b.color == 2 ? m_theme.keyword : m_theme.accent;
        const QRect br(r.left() + 10, r.top() + (r.height() - kBadge) / 2, kBadge, kBadge);
        QColor fill = c;
        fill.setAlpha(45);
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawRoundedRect(br, 5, 5);
        QFont bf = opt.font;
        bf.setPointSizeF(opt.font.pointSizeF() * 0.85);
        bf.setBold(true);
        p->setFont(bf);
        p->setPen(c);
        p->drawText(br, Qt::AlignCenter, QString(b.letter));

        // label (typed prefix emphasised), then the signature
        p->setFont(opt.font);
        const QFontMetrics fm(opt.font);
        int x = br.right() + 9;
        const QString label = shownLabel(item->label);
        const QString prefix = index.data(Qt::UserRole + 1).toString();
        int hl = label.startsWith(prefix, Qt::CaseInsensitive) ? prefix.size() : 0;
        QColor fg = m_theme.editorFg;
        if (item->deprecated) {
            QFont sf = opt.font;
            sf.setStrikeOut(true);
            p->setFont(sf);
            fg = m_theme.textMuted;
        }
        const int detailW = qMin(fm.horizontalAdvance(item->detail), r.width() / 3);
        const int limit = r.right() - 12 - (detailW > 0 ? detailW + 14 : 0);
        if (hl > 0) {
            QFont bold = p->font();
            bold.setBold(true);
            p->setFont(bold);
            p->setPen(m_theme.accent);
            const QString head = label.left(hl);
            p->drawText(QRect(x, r.top(), limit - x, r.height()), Qt::AlignVCenter | Qt::AlignLeft, head);
            x += QFontMetrics(bold).horizontalAdvance(head);
            p->setFont(item->deprecated ? p->font() : opt.font);
        }
        p->setPen(fg);
        const QString rest = label.mid(hl);
        const QFontMetrics cfm(p->font());
        p->drawText(QRect(x, r.top(), qMax(0, limit - x), r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    cfm.elidedText(rest, Qt::ElideRight, qMax(0, limit - x)));
        x += cfm.horizontalAdvance(rest);
        if (!item->signature.isEmpty() && x < limit) {
            p->setFont(opt.font);
            p->setPen(m_theme.textMuted);
            p->drawText(QRect(x, r.top(), limit - x, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                        fm.elidedText(item->signature, Qt::ElideRight, limit - x));
        }
        if (detailW > 0) {
            p->setFont(opt.font);
            p->setPen(m_theme.textMuted);
            p->drawText(QRect(r.right() - 12 - detailW, r.top(), detailW, r.height()), Qt::AlignVCenter | Qt::AlignRight,
                        fm.elidedText(item->detail, Qt::ElideLeft, detailW));
        }
        p->restore();
    }

private:
    Theme m_theme;
};
} // namespace

CompletionPopup::CompletionPopup(QWidget *editor)
    : QFrame(editor, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
      m_model(new Model(this)),
      m_view(new QListView(this))
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setObjectName(QStringLiteral("completionPopup"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->addWidget(m_view);
    m_view->setModel(m_model);
    m_view->setFocusPolicy(Qt::NoFocus);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setUniformItemSizes(true);
    m_view->setMouseTracking(true);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setItemDelegate(new Delegate(Theme::byName(SettingsManager::instance().theme()), this));
    m_view->viewport()->setCursor(Qt::PointingHandCursor);
    connect(m_view, &QListView::clicked, this, &CompletionPopup::accepted);
}

void CompletionPopup::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    static_cast<Delegate *>(m_view->itemDelegate())->setTheme(t);
    m_bg = t.panel;
    m_border = t.border;
    setStyleSheet(QStringLiteral("QListView { background: transparent; color: %1; outline: none; }").arg(t.editorFg.name()));
    update();
    m_view->setFont(font());
}

// The window is translucent so the corners can be rounded; the background and border are painted here
// (a stylesheet background is not reliable on a translucent top-level frame).
void CompletionPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(m_border, 1));
    p.setBrush(m_bg);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void CompletionPopup::setItems(const QVector<LspCompletionItem> &items, const QVector<LspCompletionItem> &extra)
{
    m_model->setAll(items, extra);
}

int CompletionPopup::setPrefix(const QString &prefix)
{
    m_model->filter(prefix);
    if (m_model->rowCount() > 0)
        m_view->setCurrentIndex(m_model->index(0));
    return m_model->rowCount();
}

int CompletionPopup::count() const { return m_model->rowCount(); }

void CompletionPopup::moveSelection(int delta)
{
    const int n = m_model->rowCount();
    if (n == 0)
        return;
    int row = m_view->currentIndex().isValid() ? m_view->currentIndex().row() : 0;
    row += delta;
    if (delta == 1 || delta == -1) // arrow keys wrap around, page keys stop at the ends
        row = (row + n) % n;
    else
        row = qBound(0, row, n - 1);
    m_view->setCurrentIndex(m_model->index(row));
    m_view->scrollTo(m_model->index(row));
}

const LspCompletionItem *CompletionPopup::current() const
{
    const QModelIndex i = m_view->currentIndex();
    return i.isValid() ? &m_model->item(i.row()) : nullptr;
}

void CompletionPopup::popup(const QPoint &below, const QPoint &above)
{
    applyTheme();
    const QFontMetrics fm(m_view->font());
    const int rows = qMin(m_model->rowCount(), kMaxRows);
    const int w = qBound(320, m_model->widest(fm) + kBadge + 48, 640);
    const int h = rows * kRowHeight + 8;
    resize(w, h);
    const QRect avail = (parentWidget() && parentWidget()->screen() ? parentWidget()->screen() : QApplication::primaryScreen())->availableGeometry();
    QPoint p = below;
    if (p.y() + h > avail.bottom())
        p.setY(above.y() - h);
    p.setX(qBound(avail.left(), p.x(), avail.right() - w));
    move(p);
    m_view->scrollToTop();
    show();
    raise();
}
