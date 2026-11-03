#include "http/HttpUi.h"

#include <QPainter>
#include <QRegularExpression>

namespace HttpUi {

QString sizeText(qint64 b)
{
    if (b < 1024)
        return QStringLiteral("%1 B").arg(b);
    if (b < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(b / 1048576.0, 0, 'f', 1);
}

QColor methodColour(const Theme &t, const QString &m)
{
    if (m == QLatin1String("GET"))
        return t.success;
    if (m == QLatin1String("POST"))
        return t.accent;
    if (m == QLatin1String("PUT"))
        return t.warning;
    if (m == QLatin1String("PATCH"))
        return t.type;
    if (m == QLatin1String("DELETE"))
        return t.danger;
    return t.textMuted;
}

QColor statusColour(const Theme &t, int status)
{
    if (status >= 200 && status < 300)
        return t.success;
    if (status >= 300 && status < 400)
        return t.warning;
    if (status >= 400)
        return t.danger;
    return t.textMuted;
}

static QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString curl(const HttpRequestData &r)
{
    QStringList parts{QStringLiteral("curl")};
    if (r.method != QLatin1String("GET"))
        parts << QStringLiteral("-X") << r.method;
    parts << shellQuote(r.url.toString());
    for (const auto &h : r.headers)
        parts << QStringLiteral("-H") << shellQuote(QString::fromUtf8(h.first + ": " + h.second));
    if (!r.body.trimmed().isEmpty())
        parts << QStringLiteral("--data-binary") << shellQuote(QString::fromUtf8(r.body));
    if (!r.followRedirects)
        parts << QStringLiteral("--max-redirs") << QStringLiteral("0");
    return parts.join(QLatin1Char(' '));
}

QString styleSheet(const Theme &t)
{
    return QStringLiteral("QPlainTextEdit, QListWidget { background: %1; color: %2; selection-background-color: %3; }"
                          "QPlainTextEdit { padding: 4px; border-top: 1px solid %4; }"
                          "QListWidget { border-top: 1px solid %4; outline: none; }"
                          "QScrollArea#httpImage { background: %1; border: none; border-top: 1px solid %4; }"
                          "QToolButton[seg=\"true\"] { background: transparent; border: none; border-bottom: 2px solid transparent; padding: 4px 9px; color: %5; }"
                          "QToolButton[seg=\"true\"]:checked { background: transparent; color: %6; border-bottom: 2px solid %6; }"
                          "QToolButton[seg=\"true\"]:hover:!checked { color: %2; }")
        .arg(t.editorBg.name(), t.editorFg.name(), t.selection.name(), t.border.name(), t.textMuted.name(), t.accent.name());
}

} // namespace HttpUi

void HttpJsonHighlighter::setTheme(const Theme &t)
{
    m_key = t.property;
    m_string = t.string;
    m_number = t.number;
    m_literal = t.keyword;
    rehighlight();
}

void HttpJsonHighlighter::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    rehighlight();
}

void HttpJsonHighlighter::highlightBlock(const QString &text)
{
    if (!m_enabled)
        return;
    static const QRegularExpression number(QStringLiteral(R"(-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)"));
    static const QRegularExpression literal(QStringLiteral(R"(\b(?:true|false|null)\b)"));
    static const QRegularExpression string(QStringLiteral(R"("(?:[^"\\]|\\.)*"(\s*:)?)"));
    auto paint = [&](const QRegularExpression &re, const QColor &c) {
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), c);
        }
    };
    paint(number, m_number);
    paint(literal, m_literal);
    auto it = string.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        setFormat(m.capturedStart(), m.capturedLength(), m.capturedStart(1) >= 0 ? m_key : m_string);
    }
}

QSize HttpRowDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    return QSize(0, index.data(HttpUi::HeaderRole).toBool() ? 24 : 26);
}

void HttpRowDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    using namespace HttpUi;
    const Theme &t = *m_theme;
    p->save();
    const QRect r = opt.rect;
    if (index.data(HeaderRole).toBool()) {
        QFont f = opt.font;
        f.setBold(true);
        f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1));
        p->setFont(f);
        p->setPen(t.textMuted);
        p->drawText(r.adjusted(8, 4, -6, 0), Qt::AlignLeft | Qt::AlignVCenter, index.data().toString().toUpper());
        p->restore();
        return;
    }
    if (opt.state & QStyle::State_Selected)
        p->fillRect(r, t.selection);
    else if (opt.state & QStyle::State_MouseOver)
        p->fillRect(r, t.currentLine);

    int x = r.left() + 8;
    const QString method = index.data(MethodRole).toString();
    if (!method.isEmpty()) {
        QFont f = opt.font;
        f.setBold(true);
        f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
        p->setFont(f);
        p->setPen(methodColour(t, method));
        const int w = QFontMetrics(f).horizontalAdvance(QStringLiteral("DELETE")) + 8;
        p->drawText(QRect(x, r.top(), w, r.height()), Qt::AlignLeft | Qt::AlignVCenter, method);
        x += w;
    }
    p->setFont(opt.font);
    const QString detail = index.data(DetailRole).toString();
    int right = r.right() - 8;
    if (!detail.isEmpty()) {
        const QVariant status = index.data(StatusRole);
        QFont f = opt.font;
        f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1));
        p->setFont(f);
        const QFontMetrics fm(f);
        const int w = qMin(fm.horizontalAdvance(detail) + 6, r.width() / 3);
        p->setPen(status.isValid() ? statusColour(t, status.toInt()) : t.textMuted);
        p->drawText(QRect(right - w, r.top(), w, r.height()), Qt::AlignRight | Qt::AlignVCenter, fm.elidedText(detail, Qt::ElideRight, w));
        right -= w + 8;
        p->setFont(opt.font);
    }
    p->setPen(t.editorFg);
    const QString text = index.data().toString();
    p->drawText(QRect(x, r.top(), qMax(0, right - x), r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(opt.font).elidedText(text, Qt::ElideMiddle, qMax(0, right - x)));
    p->restore();
}
