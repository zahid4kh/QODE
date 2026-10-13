#include "SignaturePopup.h"

#include "HoverRender.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QTextDocument>
#include <QVBoxLayout>

namespace {
constexpr int kMaxWidth = 680;
constexpr int kMaxDoc = 360;
}

SignaturePopup::SignaturePopup(QWidget *editor)
    : QFrame(editor, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_label(new QLabel(this))
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(11, 8, 11, 8);
    lay->addWidget(m_label);
    m_label->setTextFormat(Qt::RichText);
    m_label->setWordWrap(true);
    m_label->setTextInteractionFlags(Qt::NoTextInteraction);
}

void SignaturePopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(m_border, 1));
    p.setBrush(m_bg);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void SignaturePopup::setHelp(const LspSignatureHelp &help, const QFont &mono, const QString &language)
{
    // The same call being edited keeps the overload the user cycled to; a different call starts at the server's choice.
    const bool sameCall = !m_help.signatures.isEmpty() && help.signatures.size() == m_help.signatures.size() &&
                          help.signatures.first().label == m_help.signatures.first().label;
    m_index = sameCall ? qBound(0, m_index, int(help.signatures.size()) - 1) : qBound(0, help.activeSignature, int(help.signatures.size()) - 1);
    m_help = help;
    m_mono = mono;
    m_language = language;
    render();
}

void SignaturePopup::cycle(int delta)
{
    const int n = m_help.signatures.size();
    if (n < 2)
        return;
    m_index = (m_index + delta + n) % n;
    render();
}

void SignaturePopup::render()
{
    if (m_help.signatures.isEmpty())
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_bg = t.panel;
    m_border = t.border;
    const LspSignature &sig = m_help.signatures.at(m_index);
    const int active = sig.active >= 0 ? sig.active : m_help.activeParameter;

    // Signature: the text between parameters coloured as code, the active parameter bold on an accent tint.
    QString body;
    int at = 0;
    const QString &label = sig.label;
    auto code = [&](const QString &s) { return HoverRender::codeInline(s, m_language, t); };
    for (int i = 0; i < sig.params.size(); ++i) {
        const QPair<int, int> r = sig.params.at(i);
        if (r.first < at || r.second > label.size() || r.first >= r.second)
            continue;
        body += code(label.mid(at, r.first - at));
        const QString piece = code(label.mid(r.first, r.second - r.first));
        if (i == active)
            body += QStringLiteral("<span style=\"background-color:%1; font-weight:bold;\"><u>%2</u></span>").arg(t.selection.name(), piece);
        else
            body += piece;
        at = r.second;
    }
    body += code(label.mid(at));

    QString html = QStringLiteral("<div style=\"font-family:'%1'; white-space:pre-wrap; color:%2\">%3</div>").arg(m_mono.family(), t.editorFg.name(), body);
    if (m_help.signatures.size() > 1)
        html = QStringLiteral("<div style=\"color:%1\">&#9650;&#9660; %2 / %3</div>").arg(t.textMuted.name()).arg(m_index + 1).arg(m_help.signatures.size()) + html;
    if (active >= 0 && active < sig.paramDocs.size() && !sig.paramDocs.at(active).isEmpty()) {
        const QPair<int, int> r = active < sig.params.size() ? sig.params.at(active) : QPair<int, int>{-1, -1};
        const QString name = r.first >= 0 ? label.mid(r.first, r.second - r.first).section(QLatin1Char(':'), 0, 0).remove(QLatin1Char('?')).trimmed() : QString();
        html += QStringLiteral("<div style=\"margin-top:5px; color:%1\">").arg(t.editorFg.name());
        if (!name.isEmpty())
            html += QStringLiteral("<b>%1</b> — ").arg(name.toHtmlEscaped());
        html += sig.paramDocs.at(active).left(kMaxDoc).toHtmlEscaped() + QStringLiteral("</div>");
    }
    if (!sig.documentation.isEmpty()) {
        QString doc = sig.documentation;
        if (doc.size() > kMaxDoc)
            doc = doc.left(doc.lastIndexOf(QLatin1Char(' '), kMaxDoc)) + QStringLiteral("…");
        html += QStringLiteral("<div style=\"margin-top:5px; color:%1\">%2</div>").arg(t.textMuted.name(), HoverRender::markdownHtml(doc, t, m_mono, m_language));
    }
    m_label->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: %1; }").arg(t.editorFg.name()));
    m_label->setText(html);
    m_label->setMaximumWidth(kMaxWidth);
    m_label->setMinimumWidth(0);
    // A wrapping label reports a poor width hint, so measure the text with a document: its ideal width is the one-line width.
    QTextDocument measure;
    measure.setDocumentMargin(0);
    measure.setDefaultFont(m_label->font());
    measure.setHtml(html);
    const int w = qBound(240, int(measure.idealWidth()) + 4, kMaxWidth);
    m_label->setFixedWidth(w);
    m_label->adjustSize();
    adjustSize();
}

void SignaturePopup::popup(const QPoint &above, const QPoint &below)
{
    const QScreen *screen = parentWidget() && parentWidget()->screen() ? parentWidget()->screen() : QApplication::primaryScreen();
    const QRect avail = screen->availableGeometry();
    QPoint p(above.x(), above.y() - height());
    if (p.y() < avail.top())
        p = QPoint(below.x(), below.y());
    p.setX(qBound(avail.left(), p.x(), avail.right() - width()));
    move(p);
    show();
    raise();
}
