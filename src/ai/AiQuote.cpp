#include "AiQuote.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QToolButton>

namespace {
constexpr int kMaxLines = 4;
constexpr int kMaxChars = 360;
const QString kLead = QStringLiteral("From your earlier reply:\n");

// The first lines of a long quote; the card is a reminder of what goes along, not the whole text (that is the tooltip).
QString shortened(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    bool cut = lines.size() > kMaxLines;
    QString out = lines.mid(0, kMaxLines).join(QLatin1Char('\n'));
    if (out.size() > kMaxChars) {
        out = out.left(kMaxChars).trimmed();
        cut = true;
    }
    return cut ? out + QStringLiteral(" …") : out;
}
} // namespace

AiQuote::AiQuote(QWidget *parent, bool removable)
    : QFrame(parent)
    , m_icon(new QLabel(this))
    , m_label(new QLabel(this))
{
    setObjectName(QStringLiteral("aiQuote"));
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(13, 7, removable ? 5 : 10, 7);
    l->setSpacing(8);
    m_icon->setFixedSize(14, 14);
    l->addWidget(m_icon, 0, Qt::AlignTop);
    m_label->setTextFormat(Qt::PlainText);
    m_label->setWordWrap(true);
    m_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long lines wrap, never widen the panel
    l->addWidget(m_label, 1);
    if (removable) {
        m_remove = new QToolButton(this);
        m_remove->setAutoRaise(true);
        m_remove->setCursor(Qt::PointingHandCursor);
        m_remove->setToolTip(tr("Leave the quote out"));
        m_remove->setIconSize(QSize(10, 10));
        m_remove->setFixedSize(18, 18);
        connect(m_remove, &QToolButton::clicked, this, &AiQuote::removed);
        l->addWidget(m_remove, 0, Qt::AlignTop);
    }
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AiQuote::applyTheme);
    applyTheme();
}

void AiQuote::setText(const QString &text)
{
    m_text = text;
    m_label->setText(shortened(text));
    m_label->setToolTip(text.size() > kMaxChars || text.count(QLatin1Char('\n')) >= kMaxLines ? text : QString());
}

QString AiQuote::block(const QString &quote, const QString &lead)
{
    return QStringLiteral("<quote>%1%2\n</quote>").arg(lead.isEmpty() ? QString(kLead) : lead + QLatin1Char('\n'), quote);
}

QString AiQuote::ofBlock(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^<quote>%1([\\s\\S]*)\\n</quote>$").arg(QRegularExpression::escape(kLead)));
    const QRegularExpressionMatch m = re.match(text.trimmed());
    return m.hasMatch() ? m.captured(1) : QString();
}

QString AiQuote::askPrompt()
{
    return tr("Explain this part of your response.");
}

// A soft accent wash with a bar down the left edge.
void AiQuote::paintEvent(QPaintEvent *)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath box;
    box.addRoundedRect(r, 7, 7);
    QColor wash = t.accent;
    wash.setAlpha(t.dark ? 30 : 20);
    p.fillPath(box, wash);
    QColor edge = t.accent;
    edge.setAlpha(70);
    p.setPen(QPen(edge, 1));
    p.drawPath(box);
    p.setClipPath(box);
    p.fillRect(QRectF(r.left(), r.top(), 3, r.height()), t.accent);
}

void AiQuote::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    m_icon->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/text-quote.svg"), t.accent, 14));
    m_label->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }").arg(t.textMuted.name()));
    if (m_remove) {
        m_remove->setIcon(Icons::tinted(QStringLiteral(":/new-icons/x.svg"), t.textMuted));
        m_remove->setStyleSheet(QStringLiteral("QToolButton { background: transparent; border: none; border-radius: 4px; }"
                                               "QToolButton:hover { background: %1; }")
                                    .arg(t.border.name()));
    }
    update();
}
