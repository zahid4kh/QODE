#include "AiChangeCard.h"

#include "git/GitDiff.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QEvent>
#include <cmath>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

namespace {
constexpr int kContext = 2;      // unchanged lines kept around a change
constexpr int kMaxRows = 400;    // rows rendered in one diff
constexpr int kMaxVisible = 12;  // rows before the view stops growing and scrolls

QStringList linesOf(const QString &text)
{
    if (text.isEmpty())
        return {};
    QStringList l = text.split(QLatin1Char('\n'));
    if (l.size() > 1 && l.last().isEmpty())
        l.removeLast();
    return l;
}
}

// --- AiDiffView ---------------------------------------------------------------------------------------------------

AiDiffView::AiDiffView(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("aiDiff"));
    setReadOnly(true);
    setFrameShape(QFrame::NoFrame);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    document()->setDocumentMargin(6);
    connect(horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this] { fitHeight(); });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AiDiffView::applyTheme);
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, &AiDiffView::applyTheme);
    applyTheme();
}

void AiDiffView::setChange(const AiChange &change)
{
    m_change = change;
    rebuild();
}

void AiDiffView::applyTheme()
{
    QFont f = SettingsManager::instance().editorFont();
    f.setPointSizeF(qMax(8.0, f.pointSizeF() - 1));
    setFont(f);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QPalette pal = palette();
    pal.setColor(QPalette::Base, t.editorBg);
    pal.setColor(QPalette::Text, t.editorFg);
    setPalette(pal);
    rebuild();
}

void AiDiffView::rebuild()
{
    Theme t = Theme::byName(SettingsManager::instance().theme());
    // Greyscale themes would make added and removed lines look alike; a diff keeps its red and green.
    if (t.diffAddBg.saturation() < 20 || t.diffDelBg.saturation() < 20) {
        const bool dark = t.editorBg.lightness() < 128;
        t.diffAddBg = dark ? QColor(QStringLiteral("#1f3a2a")) : QColor(QStringLiteral("#cfeccf"));
        t.diffDelBg = dark ? QColor(QStringLiteral("#442328")) : QColor(QStringLiteral("#f2cfcf"));
        t.success = dark ? QColor(QStringLiteral("#4ec969")) : QColor(QStringLiteral("#1a8f3c"));
        t.danger = dark ? QColor(QStringLiteral("#f26b6b")) : QColor(QStringLiteral("#c42b2b"));
    }
    struct Row {
        QChar sign;
        QString text;
    };
    QVector<Row> rows;
    const QStringList oldLines = linesOf(m_change.before);
    const QStringList newLines = linesOf(m_change.after);

    if (m_change.op == QLatin1String("create") || oldLines.isEmpty()) {
        for (const QString &l : newLines)
            rows.append({QLatin1Char('+'), l});
    } else {
        const QVector<GitDiff::Hunk> hunks = GitDiff::compute(oldLines, newLines);
        int oi = 0;
        auto context = [&](int upTo, bool first, bool last) {
            const int n = upTo - oi;
            if (n <= 0)
                return;
            // Unchanged stretch between two changes: its edges only.
            const int head = first ? 0 : kContext, tail = last ? 0 : kContext;
            if (n <= head + tail + 1) {
                for (int i = oi; i < upTo; ++i)
                    rows.append({QLatin1Char(' '), oldLines.at(i)});
            } else {
                for (int i = oi; i < oi + head; ++i)
                    rows.append({QLatin1Char(' '), oldLines.at(i)});
                rows.append({QLatin1Char('~'), QStringLiteral("⋯ %1 unchanged lines").arg(n - head - tail)});
                for (int i = upTo - tail; i < upTo; ++i)
                    rows.append({QLatin1Char(' '), oldLines.at(i)});
            }
        };
        for (int h = 0; h < hunks.size(); ++h) {
            const GitDiff::Hunk &hk = hunks.at(h);
            context(hk.oldStart, h == 0, false);
            for (int i = 0; i < hk.oldCount; ++i)
                rows.append({QLatin1Char('-'), oldLines.value(hk.oldStart + i)});
            for (int i = 0; i < hk.newCount; ++i)
                rows.append({QLatin1Char('+'), newLines.value(hk.newStart + i)});
            oi = hk.oldStart + hk.oldCount;
        }
        context(oldLines.size(), false, true);
    }

    clear();
    QTextCursor c(document());
    int shown = 0;
    for (const Row &r : std::as_const(rows)) {
        if (shown++ >= kMaxRows) {
            QTextBlockFormat bf;
            QTextCharFormat cf;
            cf.setForeground(t.textMuted);
            c.insertBlock(bf, cf);
            c.insertText(QStringLiteral("  ⋯ %1 more lines").arg(rows.size() - kMaxRows), cf);
            break;
        }
        QTextBlockFormat bf;
        QTextCharFormat cf;
        cf.setForeground(t.editorFg);
        if (r.sign == QLatin1Char('+'))
            bf.setBackground(t.diffAddBg);
        else if (r.sign == QLatin1Char('-'))
            bf.setBackground(t.diffDelBg);
        else if (r.sign == QLatin1Char('~'))
            cf.setForeground(t.textMuted);
        else
            cf.setForeground(t.textMuted);
        if (!document()->isEmpty())
            c.insertBlock(bf, cf);
        else
            c.setBlockFormat(bf);
        // The sign gets its own colour.
        QTextCharFormat signFmt = cf;
        if (r.sign == QLatin1Char('+'))
            signFmt.setForeground(t.success);
        else if (r.sign == QLatin1Char('-'))
            signFmt.setForeground(t.danger);
        c.insertText(r.sign == QLatin1Char('~') ? QStringLiteral("  ") : QString(r.sign) + QLatin1Char(' '), signFmt);
        c.insertText(r.text, cf);
    }
    fitHeight();
}

void AiDiffView::fitHeight()
{
    const int lines = qMin(kMaxVisible, qMax(1, blockCount()));
    const int h = int(std::ceil(lines * QFontMetricsF(font()).lineSpacing())) + 2 * int(document()->documentMargin()) + 8
                  + (horizontalScrollBar()->maximum() > 0 ? horizontalScrollBar()->sizeHint().height() : 0);
    if (h != height())
        setFixedHeight(h);
}

void AiDiffView::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    fitHeight();
}


void AiDiffView::wheelEvent(QWheelEvent *e)
{
    // The diff scrolls while it can; at its ends the wheel goes on to the chat.
    QScrollBar *bar = verticalScrollBar();
    const int dy = e->angleDelta().y();
    const bool horizontal = qAbs(e->angleDelta().x()) > qAbs(dy);
    const bool canScroll = horizontal || (dy > 0 ? bar->value() > bar->minimum() : bar->value() < bar->maximum());
    if (!canScroll) {
        e->ignore();
        return;
    }
    QPlainTextEdit::wheelEvent(e);
}

// --- AiChangeCard -------------------------------------------------------------------------------------------------

AiChangeCard::AiChangeCard(const AiChange &change, QWidget *parent)
    : QFrame(parent)
    , m_change(change)
{
    setObjectName(QStringLiteral("aiChange"));
    m_header = new QWidget(this);
    m_header->setCursor(Qt::PointingHandCursor);
    m_icon = new QLabel(m_header);
    m_icon->setFixedSize(16, 16);
    m_path = new QLabel(m_header);
    m_path->setObjectName(QStringLiteral("aiChangePath"));
    m_path->setText(change.path);
    m_path->setToolTip(tr("Open %1").arg(change.path));
    m_path->setCursor(Qt::PointingHandCursor);
    m_path->installEventFilter(this);
    m_note = new QLabel(change.note, m_header);
    m_note->setObjectName(QStringLiteral("aiChangeNote"));
    m_note->setWordWrap(true);
    m_toggle = new QToolButton(m_header);
    m_toggle->setAutoRaise(true);
    m_toggle->setToolTip(tr("Show the changed lines"));
    m_toggle->setVisible(change.ok && !(change.before.isEmpty() && change.after.isEmpty()));

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(8);
    top->addWidget(m_icon, 0, Qt::AlignVCenter);
    top->addWidget(m_path, 1);
    top->addWidget(m_toggle, 0, Qt::AlignVCenter);
    auto *hl = new QVBoxLayout(m_header);
    hl->setContentsMargins(10, 8, 6, 8);
    hl->setSpacing(2);
    hl->addLayout(top);
    auto *noteRow = new QHBoxLayout;
    noteRow->setContentsMargins(24, 0, 0, 0);
    noteRow->addWidget(m_note);
    hl->addLayout(noteRow);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_header);

    m_header->installEventFilter(this);
    m_note->installEventFilter(this);
    m_icon->installEventFilter(this);
    connect(m_toggle, &QToolButton::clicked, this, [this] { setExpanded(!m_expanded); });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AiChangeCard::applyTheme);
    applyTheme();
}

bool AiChangeCard::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        if (watched == m_path)
            emit openRequested(m_change.path);
        else if (m_toggle->isVisible())
            setExpanded(!m_expanded);
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

void AiChangeCard::setExpanded(bool on)
{
    m_expanded = on;
    auto *box = static_cast<QVBoxLayout *>(layout());
    if (on && !m_diff) {
        m_clip = new QWidget(this);
        auto *cl = new QVBoxLayout(m_clip);
        cl->setContentsMargins(0, 0, 0, 0);
        cl->setSpacing(0);
        m_diff = new AiDiffView(m_clip);
        m_diff->setChange(m_change);
        cl->addWidget(m_diff, 0, Qt::AlignTop);
        m_clip->setFixedHeight(0);
        box->addWidget(m_clip);

        m_anim = new QVariantAnimation(this);
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_clip->setFixedHeight(qMax(0, int(v.toReal() * m_diff->height())));
            if (m_expanded)
                QTimer::singleShot(0, this, [this] { keepInView(); });
        });
        connect(m_anim, &QVariantAnimation::finished, this, [this] {
            if (m_expanded) {
                m_clip->setMinimumHeight(0);
                m_clip->setMaximumHeight(QWIDGETSIZE_MAX);
            } else {
                m_clip->hide();
            }
        });
    }
    if (m_diff) {
        m_anim->stop();
        const qreal from = m_diff->height() > 0 ? qreal(m_clip->height()) / m_diff->height() : 0;
        if (on) {
            m_diff->verticalScrollBar()->setValue(0);
            m_clip->show();
            m_anim->setDuration(520);
            QEasingCurve spring(QEasingCurve::OutBack);
            spring.setOvershoot(1.4);
            m_anim->setEasingCurve(spring);
            m_anim->setStartValue(from);
            m_anim->setEndValue(qreal(1));
        } else {
            m_anim->setDuration(260);
            m_anim->setEasingCurve(QEasingCurve::InOutCubic);
            m_anim->setStartValue(from);
            m_anim->setEndValue(qreal(0));
        }
        m_anim->start();
    }
    applyTheme();
}

// While the card grows, the chat follows so the bottom of the diff stays on screen.
void AiChangeCard::keepInView()
{
    QWidget *w = parentWidget();
    while (w && !qobject_cast<QScrollArea *>(w))
        w = w->parentWidget();
    auto *area = static_cast<QScrollArea *>(w);
    if (!area)
        return;
    const int bottom = mapTo(area->viewport(), QPoint(0, height())).y();
    const int over = bottom + 8 - area->viewport()->height();
    if (over > 0)
        area->verticalScrollBar()->setValue(area->verticalScrollBar()->value() + over);
}

void AiChangeCard::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_icon->setPixmap(Icons::pixmap(m_change.ok ? QStringLiteral(":/new-icons/check.svg") : QStringLiteral(":/new-icons/circle-x.svg"),
                                    m_change.ok ? t.success : t.danger, 16));
    m_toggle->setIcon(Icons::tinted(m_expanded ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"), t.textMuted));
    m_toggle->setToolTip(m_expanded ? tr("Hide the changed lines") : tr("Show the changed lines"));
    setStyleSheet(QStringLiteral("QFrame#aiChange { background: %1; border: 1px solid %2; border-radius: 8px; }"
                                 "QFrame#aiChange QWidget { background: transparent; }"
                                 "QLabel#aiChangePath { color: %3; font-weight: 600; }"
                                 "QLabel#aiChangeNote { color: %4; }"
                                 "QFrame#aiChange AiDiffView { background: %5; border: none; border-top: 1px solid %2; border-bottom-left-radius: 8px; border-bottom-right-radius: 8px; }")
                      .arg(t.panel.name(), t.border.name(), t.accent.name(), t.textMuted.name(), t.editorBg.name()));
}
