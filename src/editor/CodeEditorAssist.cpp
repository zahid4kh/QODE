// The editor side of the language server assists: inlay hints, symbol highlights, the light bulb and signature help.
// The requests themselves are made by MainWindow (through the signals declared in CodeEditor.h); this file keeps the state
// that follows edits and paints it.
#include "CodeEditor.h"

#include "CompletionPopup.h"
#include "HoverPopup.h"
#include "HoverRender.h"
#include "SignaturePopup.h"
#include "SyntaxHighlighter.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QDesktopServices>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>

namespace {
constexpr int kBulbSize = 13;
constexpr int kHintPad = 5;    // inside the pill, each side
constexpr int kHintMargin = 3; // between the pill and the neighbouring text, each side
}

void CodeEditor::setupAssists()
{
    m_hintTimer = new QTimer(this);
    m_hintTimer->setSingleShot(true);
    m_hintTimer->setInterval(500);
    connect(m_hintTimer, &QTimer::timeout, this, [this] {
        if (!m_hintsWanted)
            return;
        int first = 0, last = 0;
        visibleBlockRange(&first, &last);
        emit inlayHintsRequested(qMax(0, first - 80), last + 80);
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (m_hintsWanted)
            m_hintTimer->start();
        hideSignatureHelp();
    });
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (m_hintsWanted && !m_gapUpdating)
            m_hintTimer->start();
    });

    m_caretTimer = new QTimer(this);
    m_caretTimer->setSingleShot(true);
    m_caretTimer->setInterval(300);
    connect(m_caretTimer, &QTimer::timeout, this, &CodeEditor::caretSettled);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        m_caretTimer->start();
        if (!m_highlights.isEmpty()) { // keep the tint only while the caret is on one of the uses
            const int pos = textCursor().position();
            bool inside = false;
            for (const Highlight &h : std::as_const(m_highlights))
                if (pos >= h.from.position() && pos <= h.to.position()) {
                    inside = true;
                    break;
                }
            if (!inside)
                clearSymbolHighlights();
        }
        if (m_signatureOpen)
            scheduleSignature(3, {}, true);
    });

    m_signatureTimer = new QTimer(this);
    m_signatureTimer->setSingleShot(true);
    m_signatureTimer->setInterval(40);

    m_docsTimer = new QTimer(this);
    m_docsTimer->setSingleShot(true);
    m_docsTimer->setInterval(140);
    connect(m_docsTimer, &QTimer::timeout, this, &CodeEditor::updateCompletionDocs);
}

// --- Completion documentation -----------------------------------------------------------------------

// The text beside the list: where an auto-import comes from, the full signature and the documentation. Empty when the
// item has nothing to say beyond its label.
QString CodeEditor::completionDocsHtml(const LspCompletionItem &item) const
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const QString lang = m_language.toLower();
    QString html;
    QString detail = item.longDetail.isEmpty() ? QString() : item.longDetail;
    if (!detail.isEmpty()) {
        QStringList lines = detail.split(QLatin1Char('\n'));
        // typescript-language-server: "Auto import from './utils'" on top, then the declaration.
        static const QRegularExpression from(QStringLiteral("^(?:Auto import from|Import from)\\s+['\"]?(.+?)['\"]?$"));
        const auto m = from.match(lines.first());
        if (m.hasMatch()) {
            html += QStringLiteral("<div style=\"color:%1\">%2 <b>%3</b></div>").arg(t.textMuted.name(), tr("Auto import from"), m.captured(1).toHtmlEscaped());
            lines.removeFirst();
        }
        const QString rest = lines.join(QLatin1Char('\n')).trimmed();
        if (!rest.isEmpty())
            html += HoverRender::codeBlock(rest, lang, t, font());
    }
    if (!item.documentation.isEmpty())
        html += HoverRender::markdownHtml(item.documentation, t, font(), lang);
    return html;
}

void CodeEditor::updateCompletionDocs()
{
    if (!m_completion || !m_completion->isVisible())
        return;
    const LspCompletionItem *picked = m_completion->current();
    if (!picked || picked->builtin) {
        m_completion->showDocs({});
        return;
    }
    const QString key = picked->server + QLatin1Char('\x1f') + picked->label + QLatin1Char('\x1f') + picked->detail;
    const auto cached = m_docCache.constFind(key);
    if (cached != m_docCache.constEnd()) {
        m_completion->showDocs(completionDocsHtml(cached.value()));
        return;
    }
    const QString html = completionDocsHtml(*picked);
    if (picked->resolved || picked->resolveData.isEmpty() || !m_resolver) {
        m_completion->showDocs(html);
        return;
    }
    // Ask the server for the documentation; the list keeps working meanwhile, and a late answer for an entry that is no longer
    // selected is only cached.
    m_completion->showDocs(html);
    QPointer<CodeEditor> guard(this);
    m_resolver(*picked, [guard, key](const LspCompletionItem &resolved) {
        if (!guard)
            return;
        guard->m_docCache.insert(key, resolved);
        if (!guard->m_completion || !guard->m_completion->isVisible())
            return;
        const LspCompletionItem *now = guard->m_completion->current();
        if (now && now->server + QLatin1Char('\x1f') + now->label + QLatin1Char('\x1f') + now->detail == key)
            guard->m_completion->showDocs(guard->completionDocsHtml(resolved));
    });
}

// --- Inlay hints ---------------------------------------------------------------------------------

QFont CodeEditor::hintFont() const
{
    QFont f = font();
    f.setPointSizeF(qMax(6.5, f.pointSizeF() * 0.88));
    f.setItalic(false);
    f.setBold(false);
    return f;
}

void CodeEditor::setInlayHintsWanted(bool on)
{
    if (m_hintsWanted == on)
        return;
    m_hintsWanted = on;
    if (!on)
        setInlayHints({});
    else
        m_hintTimer->start(0);
}

void CodeEditor::requestInlayHintsSoon()
{
    if (m_hintsWanted && m_hintTimer)
        m_hintTimer->start();
}

void CodeEditor::setInlayHints(const QVector<LspInlayHint> &hints)
{
    const QFontMetrics fm(hintFont());
    QVector<Hint> next;
    next.reserve(hints.size());
    for (const LspInlayHint &h : hints) {
        const QTextBlock b = document()->findBlockByNumber(h.line);
        // A hint needs a character in front of it to hang its room on, and one behind (the end of a line has no room).
        if (!b.isValid() || h.column <= 0 || h.column >= b.length() - 1)
            continue;
        Hint hint;
        hint.at = QTextCursor(document());
        hint.at.setPosition(b.position() + h.column);
        hint.at.setKeepPositionOnInsert(true);
        hint.text = h.label;
        hint.kind = h.kind;
        hint.width = fm.horizontalAdvance(h.label) + 2 * kHintPad + 2 * kHintMargin;
        next.append(hint);
    }
    if (next.isEmpty() && m_hints.isEmpty())
        return;
    // The same hints again (the usual answer after scrolling or a pause): changing nothing keeps the lines from being laid
    // out again.
    if (next.size() == m_hints.size()) {
        bool same = true;
        for (int i = 0; same && i < next.size(); ++i)
            same = next.at(i).at.position() == m_hints.at(i).at.position() && next.at(i).text == m_hints.at(i).text;
        if (same)
            return;
    }
    QSet<int> blocks;
    for (const Hint &old : std::as_const(m_hints))
        if (!old.at.isNull())
            blocks.insert(old.at.blockNumber());
    for (const Hint &n : std::as_const(next))
        blocks.insert(n.at.blockNumber());
    m_hints = next;
    m_gapRevision = -1;
    applyGapProvider(blocks);
    viewport()->update();
}

void CodeEditor::paintInlayHints()
{
    for (Hint &h : m_hints)
        h.rect = QRect();
    if (m_hints.isEmpty())
        return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const QFont f = hintFont();
    const QFontMetrics fm(f);
    p.setFont(f);
    QColor fill = t.textMuted;
    fill.setAlpha(t.dark ? 34 : 28);
    QColor ink = t.textMuted;
    const int viewBottom = viewport()->height();
    // cursorRect() lays out the line it asks about: only ask for the lines on screen, or every repaint lays out the whole
    // hinted range (scrolling and selecting turn sluggish).
    int firstBlock = 0, lastBlock = 0;
    visibleBlockRange(&firstBlock, &lastBlock);
    for (Hint &h : m_hints) {
        if (h.at.isNull())
            continue;
        const int bn = h.at.blockNumber();
        if (bn < firstBlock || bn > lastBlock || !h.at.block().isVisible())
            continue;
        const QRect r = cursorRect(h.at);
        if (r.bottom() < 0 || r.top() > viewBottom)
            continue;
        const int pillH = qMin(r.height() - 2, fm.height() + 2);
        const QRect pill(r.left() - h.width + kHintMargin, r.top() + (r.height() - pillH) / 2, h.width - 2 * kHintMargin, pillH);
        h.rect = pill;
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(pill, 4, 4);
        p.setPen(ink);
        p.drawText(pill, Qt::AlignCenter, h.text);
    }
}

void CodeEditor::requestAssistsSoon()
{
    if (m_caretTimer)
        m_caretTimer->start();
}

// --- Symbol highlights ------------------------------------------------------------------------------

void CodeEditor::setSymbolHighlights(const QVector<LspHighlight> &list)
{
    m_highlights.clear();
    // A lone use (the symbol under the caret and nothing else) is not worth tinting.
    if (list.size() > 1)
        for (int i = 0; i < list.size() && i < 600; ++i) {
            const LspHighlight &h = list.at(i);
            const QTextBlock a = document()->findBlockByNumber(h.startLine), b = document()->findBlockByNumber(h.endLine);
            if (!a.isValid() || !b.isValid())
                continue;
            Highlight hl;
            hl.from = QTextCursor(document());
            hl.from.setPosition(a.position() + qMin(h.startColumn, qMax(0, a.length() - 1)));
            hl.to = QTextCursor(document());
            hl.to.setPosition(b.position() + qMin(h.endColumn, qMax(0, b.length() - 1)));
            hl.kind = h.kind;
            m_highlights.append(hl);
        }
    refreshSelections();
}

void CodeEditor::clearSymbolHighlights()
{
    if (m_highlights.isEmpty())
        return;
    m_highlights.clear();
    refreshSelections();
}

// --- Light bulb ------------------------------------------------------------------------------------

void CodeEditor::setLightBulb(int line)
{
    if (m_bulbLine == line)
        return;
    m_bulbLine = line;
    m_lineArea->update();
}

void CodeEditor::setLightBulbEnabled(bool on)
{
    if (m_bulbEnabled == on)
        return;
    m_bulbEnabled = on;
    if (!on)
        m_bulbLine = -1;
    updateLineNumberAreaWidth();
    m_lineArea->update();
}

void CodeEditor::paintLightBulb(QPainter &painter, int top, int lineHeight)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const int x = blameWidth() + 4;
    m_bulbRect = QRect(x, top + (lineHeight - kBulbSize) / 2, kBulbSize, kBulbSize);
    painter.drawPixmap(m_bulbRect.topLeft(), Icons::pixmap(QStringLiteral(":/new-icons/lightbulb.svg"), t.warning, kBulbSize));
}

void CodeEditor::requestCodeActions()
{
    hideCompletion();
    hideSignatureHelp();
    const QTextCursor c = textCursor();
    const QTextBlock a = document()->findBlock(c.selectionStart()), b = document()->findBlock(c.selectionEnd());
    emit codeActionsRequested(a.blockNumber(), c.selectionStart() - a.position(), b.blockNumber(), c.selectionEnd() - b.position());
}

// --- Signature help ----------------------------------------------------------------------------------

bool CodeEditor::signatureVisible() const { return m_signature && m_signature->isVisible(); }

void CodeEditor::hideSignatureHelp()
{
    ++m_signatureToken; // an answer still on its way is stale now
    m_signatureOpen = false;
    if (m_signatureTimer)
        m_signatureTimer->stop();
    if (m_signature)
        m_signature->hide();
}

void CodeEditor::scheduleSignature(int kind, const QString &trigger, bool retrigger)
{
    m_signatureTimer->disconnect(this);
    connect(m_signatureTimer, &QTimer::timeout, this, [this, kind, trigger, retrigger] {
        if (!m_signatureTriggers)
            return;
        const QTextCursor c = textCursor();
        emit signatureHelpRequested(c.blockNumber(), c.positionInBlock(), kind, trigger, retrigger, ++m_signatureToken);
    });
    m_signatureTimer->start();
}

void CodeEditor::triggerSignatureHelp()
{
    QString triggers, retriggers;
    if (!m_signatureTriggers || !m_signatureTriggers(&triggers, &retriggers))
        return;
    scheduleSignature(1, {}, signatureVisible());
}

// Typing a trigger character ("(" or ",") opens the popup; while it is open every caret move asks again (see setupAssists),
// and the server answers with nothing as soon as the caret has left the call.
void CodeEditor::signatureTyped(QKeyEvent *event, bool edited)
{
    if (!m_signatureTriggers || !edited || isReadOnly())
        return;
    QString triggers, retriggers;
    if (!m_signatureTriggers(&triggers, &retriggers)) {
        hideSignatureHelp();
        return;
    }
    const QString text = event->text();
    if (text.size() != 1)
        return;
    const QChar ch = text.at(0);
    const int cur = textCursor().position();
    if (triggers.contains(ch)) {
        if (!inStringOrComment(cur - 1)) // "(" inside a string is no call
            scheduleSignature(2, QString(ch), signatureVisible());
    } else if (signatureVisible() && retriggers.contains(ch)) {
        scheduleSignature(2, QString(ch), true);
    }
}

void CodeEditor::showSignatureHelp(const LspSignatureHelp &help, int token)
{
    if (token != m_signatureToken)
        return;
    if (help.isEmpty() || !hasFocus()) {
        if (m_signature)
            m_signature->hide();
        m_signatureOpen = false;
        return;
    }
    if (!m_signature)
        m_signature = new SignaturePopup(this);
    m_signature->setHelp(help, font(), m_language.toLower());
    m_signatureOpen = true;
    // Anchored at the opening of the call's line, not the caret: the popup stays put while the arguments are typed.
    const QRect r = cursorRect();
    m_signature->popup(viewport()->mapToGlobal(r.topLeft() - QPoint(0, 4)), viewport()->mapToGlobal(r.bottomLeft() + QPoint(0, 4)));
}

// --- Hover popup links -----------------------------------------------------------------------------

void CodeEditor::applyHoverLink(const QString &href)
{
    if (href.startsWith(QLatin1String("http://")) || href.startsWith(QLatin1String("https://")))
        QDesktopServices::openUrl(QUrl(href));
}
