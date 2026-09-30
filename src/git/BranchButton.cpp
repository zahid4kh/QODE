#include "BranchButton.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QFontMetrics>
#include <QPainter>

namespace {
constexpr int kIcon = 14;
constexpr int kChevron = 10;
constexpr int kGap = 6;
} // namespace

BranchButton::BranchButton(bool compact, QWidget *parent)
    : QAbstractButton(parent)
    , m_compact(compact)
    , m_theme(Theme::byName(SettingsManager::instance().theme()))
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this](const QString &n) {
        m_theme = Theme::byName(n);
        update();
    });
}

void BranchButton::setLabel(const QString &text)
{
    if (m_label == text)
        return;
    m_label = text;
    updateGeometry();
    update();
}

QSize BranchButton::sizeHint() const
{
    QFont f = font();
    f.setWeight(QFont::DemiBold);
    const int pad = m_compact ? 8 : 10;
    const int textW = qMin(QFontMetrics(f).horizontalAdvance(m_label), m_compact ? 220 : 170);
    return QSize(pad * 2 + kIcon + kGap + textW + kGap + kChevron + 4, m_compact ? 22 : 28);
}

void BranchButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool hot = underMouse() || isDown() || hasFocus();
    if (m_compact) {
        if (hot) {
            p.setPen(Qt::NoPen);
            p.setBrush(m_theme.currentLine);
            p.drawRoundedRect(r, 4, 4);
        }
    } else {
        p.setPen(QPen(hot ? m_theme.accent : m_theme.border, 1));
        p.setBrush(isDown() ? m_theme.currentLine : m_theme.editorBg);
        p.drawRoundedRect(r, 6, 6);
    }

    const int pad = m_compact ? 8 : 10;
    int x = pad;
    const int cy = height() / 2;
    p.drawPixmap(x, cy - kIcon / 2, Icons::pixmap(QStringLiteral(":/new-icons/git-branch.svg"), m_theme.gitAdded, kIcon));
    x += kIcon + kGap;

    QFont f = font();
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(m_theme.editorFg);
    const int textW = width() - x - kGap - kChevron - pad;
    const QString shown = QFontMetrics(f).elidedText(m_label, Qt::ElideRight, textW);
    p.drawText(QRect(x, 0, textW, height()), Qt::AlignVCenter | Qt::AlignLeft, shown);

    p.drawPixmap(width() - pad - kChevron, cy - kChevron / 2, Icons::pixmap(QStringLiteral(":/new-icons/chevron-down.svg"), m_theme.textMuted, kChevron));
}
