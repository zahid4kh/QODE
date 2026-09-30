#include "PalettePopup.h"

#include "FuzzyMatcher.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace {

enum Role { RoleTitle = Qt::UserRole + 1, RoleDetail, RoleHint, RoleIndex, RoleTitleHits, RoleDetailHits, RoleEmpty };

constexpr int kRowHeight = 32;
constexpr int kMaxRows = 10;
constexpr int kMaxResults = 250;

// Draws `text` in `base`, painting the characters at `hits` in `match` (bold).
void drawHighlighted(QPainter *p, const QRect &r, const QString &text, const QList<int> &hits, const QFont &font, const QColor &base,
                     const QColor &match, Qt::TextElideMode elide)
{
    QFont bold = font;
    bold.setWeight(QFont::Bold);
    const QFontMetrics fm(font), fmb(bold);
    QString shown = text;
    QList<int> shownHits = hits;
    if (fm.horizontalAdvance(text) > r.width()) {
        // Elide with plain metrics; highlights inside the kept part stay valid.
        const QString e = fm.elidedText(text, elide, r.width());
        if (elide == Qt::ElideRight) {
            const int keep = qMax(0, e.size() - 1);
            shownHits.erase(std::remove_if(shownHits.begin(), shownHits.end(), [keep](int h) { return h >= keep; }), shownHits.end());
        } else {
            shownHits.clear();
        }
        shown = e;
    }
    int x = r.left();
    const int y = r.top() + (r.height() + fm.ascent() - fm.descent()) / 2;
    for (int i = 0; i < shown.size(); ++i) {
        const bool hit = shownHits.contains(i);
        p->setFont(hit ? bold : font);
        p->setPen(hit ? match : base);
        p->drawText(x, y, QString(shown.at(i)));
        x += (hit ? fmb : fm).horizontalAdvance(shown.at(i));
    }
}

class PaletteDelegate : public QStyledItemDelegate
{
public:
    explicit PaletteDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
        refreshTheme();
        connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { refreshTheme(); });
    }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        return QSize(0, index.data(RoleEmpty).toBool() ? 40 : kRowHeight);
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QRect r = option.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (index.data(RoleEmpty).toBool()) {
            p->setPen(m_theme.textMuted);
            p->setFont(option.font);
            p->drawText(r, Qt::AlignCenter, index.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }
        if (option.state & QStyle::State_Selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme.selection);
            p->drawRoundedRect(r.adjusted(4, 1, -4, -1), 5, 5);
        } else if (option.state & QStyle::State_MouseOver) {
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme.currentLine);
            p->drawRoundedRect(r.adjusted(4, 1, -4, -1), 5, 5);
        }
        int left = r.left() + 14;
        int right = r.right() - 14;
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull())
            icon.paint(p, QRect(left, r.center().y() - 8, 16, 16));
        if (option.widget && option.widget->property("hasIcons").toBool())
            left += 26; // keep titles aligned when only some entries have an icon
        const QString hint = index.data(RoleHint).toString();
        if (!hint.isEmpty()) {
            QFont f = option.font;
            f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
            const QFontMetrics fm(f);
            const int w = fm.horizontalAdvance(hint) + 14;
            const QRect pill(right - w, r.center().y() - 10, w, 20);
            p->setPen(Qt::NoPen);
            QColor bg = m_theme.textMuted;
            bg.setAlpha(m_theme.dark ? 45 : 35);
            p->setBrush(bg);
            p->drawRoundedRect(pill, 5, 5);
            p->setFont(f);
            p->setPen(m_theme.textMuted);
            p->drawText(pill, Qt::AlignCenter, hint);
            right -= w + 10;
        }
        const QString title = index.data(RoleTitle).toString();
        const QString detail = index.data(RoleDetail).toString();
        QFont f = option.font;
        int titleW = qMin(QFontMetrics(f).horizontalAdvance(title), qMax(0, right - left));
        drawHighlighted(p, QRect(left, r.top(), qMax(0, right - left), r.height()), title, index.data(RoleTitleHits).value<QList<int>>(), f,
                        m_theme.editorFg, m_theme.accent, Qt::ElideRight);
        if (!detail.isEmpty()) {
            QFont df = option.font;
            df.setPointSizeF(qMax(7.0, df.pointSizeF() - 1));
            const int dl = left + titleW + 10;
            if (right - dl > 30)
                drawHighlighted(p, QRect(dl, r.top(), right - dl, r.height()), detail, index.data(RoleDetailHits).value<QList<int>>(), df,
                                m_theme.textMuted, m_theme.accent, Qt::ElideLeft);
        }
        p->restore();
    }

private:
    void refreshTheme() { m_theme = Theme::byName(SettingsManager::instance().theme()); }
    Theme m_theme;
};

} // namespace

PalettePopup::PalettePopup(QWidget *window)
    : QFrame(window, Qt::Popup)
    , m_window(window)
{
    setObjectName(QStringLiteral("palettePopup"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 8, 6, 6);
    lay->setSpacing(6);

    m_edit = new QLineEdit(this);
    m_edit->setClearButtonEnabled(true);
    m_edit->installEventFilter(this);
    lay->addWidget(m_edit);

    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setItemDelegate(new PaletteDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setFocusPolicy(Qt::NoFocus);
    lay->addWidget(m_list);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("emptyText"));
    m_status->setContentsMargins(8, 0, 8, 2);
    m_status->hide();
    lay->addWidget(m_status);

    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &t) {
        refilter();
        emit queryChanged(t);
    });
    connect(m_list, &QListWidget::itemClicked, this, &PalettePopup::activate);
}

void PalettePopup::setPlaceholder(const QString &text)
{
    m_edit->setPlaceholderText(text);
}

QString PalettePopup::query() const
{
    return m_edit->text();
}

void PalettePopup::setQuery(const QString &text)
{
    m_edit->setText(text);
}

void PalettePopup::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
    updateHeight();
}

void PalettePopup::setItems(const QList<Item> &items)
{
    m_items = items;
    bool icons = false;
    for (const Item &it : items)
        icons = icons || !it.icon.isNull();
    m_list->setProperty("hasIcons", icons);
    refilter();
}

void PalettePopup::popup()
{
    const int w = qBound(360, m_window->width() - 60, 640);
    setFixedWidth(w);
    updateHeight();
    const QPoint topLeft = m_window->mapToGlobal(QPoint((m_window->width() - w) / 2, qMin(70, m_window->height() / 6)));
    move(topLeft);
    show();
    m_edit->setFocus();
}

void PalettePopup::updateHeight()
{
    int rows = qMin(m_list->count(), kMaxRows);
    int h = 0;
    for (int i = 0; i < rows; ++i)
        h += m_list->sizeHintForRow(i);
    m_list->setFixedHeight(qMax(h, 4) + 4);
    adjustSize();
}

void PalettePopup::refilter()
{
    const QString raw = m_edit->text().trimmed();
    QString q = raw;
    QString lineHint;
    if (m_lineSuffix) {
        static const QRegularExpression suffix(QStringLiteral(":(\\d+)(?::(\\d+))?$"));
        const auto m = suffix.match(raw);
        if (m.hasMatch()) {
            q = raw.left(m.capturedStart()).trimmed();
            lineHint = m.captured(1);
        }
    }
    m_list->setUpdatesEnabled(false);
    m_list->clear();
    if (!lineHint.isEmpty() && q.isEmpty()) {
        // ":42" alone: nothing to filter, Enter jumps to that line of the current file.
        auto *li = new QListWidgetItem(tr("Press Enter to go to line %1").arg(lineHint), m_list);
        li->setData(RoleEmpty, true);
        li->setFlags(Qt::NoItemFlags);
        m_list->setUpdatesEnabled(true);
        updateHeight();
        return;
    }

    struct Scored {
        int score;
        int index;
        QList<int> titleHits, detailHits;
    };
    QList<Scored> out;
    out.reserve(qMin<qsizetype>(m_items.size(), 4096));
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &it = m_items.at(i);
        Scored s{0, i, {}, {}};
        if (!q.isEmpty()) {
            if (m_mode == Mode::Paths) {
                const QString key = it.detail.isEmpty() ? it.title : it.detail + QLatin1Char('/') + it.title;
                const int nameStart = it.detail.isEmpty() ? 0 : it.detail.size() + 1;
                QList<int> pos;
                s.score = FuzzyMatcher::score(q, key, nameStart, &pos);
                if (s.score < 0)
                    continue;
                for (int p : pos) {
                    if (p >= nameStart)
                        s.titleHits << p - nameStart;
                    else
                        s.detailHits << p;
                }
            } else {
                s.score = FuzzyMatcher::score(q, it.title, 0, &s.titleHits);
                if (s.score >= 0) {
                    s.score += 1000;
                } else if (!it.detail.isEmpty()) {
                    // The category can be part of the query: "git push".
                    const QString key = it.detail + QLatin1Char(' ') + it.title;
                    QList<int> pos;
                    s.score = FuzzyMatcher::score(q, key, it.detail.size() + 1, &pos);
                    if (s.score < 0)
                        continue;
                    for (int p : pos) {
                        if (p > it.detail.size())
                            s.titleHits << p - it.detail.size() - 1;
                        else
                            s.detailHits << p;
                    }
                } else {
                    continue;
                }
            }
        }
        out.append(std::move(s));
    }
    if (!q.isEmpty()) {
        const int keep = qMin<qsizetype>(kMaxResults, out.size());
        std::partial_sort(out.begin(), out.begin() + keep, out.end(), [](const Scored &a, const Scored &b) {
            return a.score != b.score ? a.score > b.score : a.index < b.index;
        });
        out.resize(keep);
    } else if (out.size() > kMaxResults) {
        out.resize(kMaxResults);
    }

    for (const Scored &s : std::as_const(out)) {
        const Item &it = m_items.at(s.index);
        auto *li = new QListWidgetItem(m_list);
        li->setData(RoleTitle, it.title);
        li->setData(RoleDetail, it.detail);
        li->setData(RoleHint, it.hint);
        li->setData(RoleIndex, s.index);
        li->setData(RoleTitleHits, QVariant::fromValue(s.titleHits));
        li->setData(RoleDetailHits, QVariant::fromValue(s.detailHits));
        if (!it.icon.isNull())
            li->setData(Qt::DecorationRole, it.icon);
        li->setSizeHint(QSize(0, kRowHeight));
    }
    if (m_list->count() == 0) {
        auto *li = new QListWidgetItem(m_emptyText.isEmpty() ? tr("No matching results") : m_emptyText, m_list);
        li->setData(RoleEmpty, true);
        li->setFlags(Qt::NoItemFlags);
    } else {
        m_list->setCurrentRow(0);
    }
    m_list->setUpdatesEnabled(true);
    updateHeight();
}

void PalettePopup::moveSelection(int delta)
{
    const int count = m_list->count();
    if (count == 0 || m_list->item(0)->data(RoleEmpty).toBool())
        return;
    int row = m_list->currentRow() + delta;
    row = qBound(0, row, count - 1);
    m_list->setCurrentRow(row);
}

void PalettePopup::activate(QListWidgetItem *item)
{
    if (!item || item->data(RoleEmpty).toBool())
        return;
    const QVariant data = m_items.at(item->data(RoleIndex).toInt()).data;
    const QString q = m_edit->text();
    close();
    emit accepted(data, q);
}

bool PalettePopup::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_edit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Down:
            moveSelection(1);
            return true;
        case Qt::Key_Up:
            moveSelection(-1);
            return true;
        case Qt::Key_PageDown:
            moveSelection(kMaxRows - 1);
            return true;
        case Qt::Key_PageUp:
            moveSelection(-(kMaxRows - 1));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_list->currentItem())
                activate(m_list->currentItem());
            else
                emit accepted(QVariant(), m_edit->text()); // nothing selected: let the owner interpret the raw query
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, event);
}
