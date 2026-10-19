#include "MarkdownView.h"

#include "MarkdownHtml.h"
#include "editor/HoverRender.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QCursor>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <functional>

namespace {
constexpr int kOrigWidth = QTextFormat::UserProperty + 8; // the width the document asked for
constexpr int kCodePadding = 7;                           // cell padding that marks a code box (see MarkdownHtml)
const QColor kInlineCodeMark(1, 2, 3);                    // the stylesheet paints `code` with this; polish() turns it into a pill

bool isCodeBox(const QTextTable *t) { return t && t->format().cellPadding() == kCodePadding; }
}

MarkdownView::MarkdownView(QWidget *parent)
    : QTextBrowser(parent)
    , m_resizeTimer(new QTimer(this))
{
    setFrameShape(QFrame::NoFrame);
    setOpenLinks(false);
    setLineWrapMode(QTextEdit::WidgetWidth); // never scroll sideways
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    document()->setDocumentMargin(16);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(120);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] { fitImages(); });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &MarkdownView::applyTheme);
    applyTheme();
}

void MarkdownView::setBaseDirectory(const QString &dir)
{
    document()->setBaseUrl(dir.isEmpty() ? QUrl() : QUrl::fromLocalFile(dir + QLatin1Char('/')));
}

void MarkdownView::setAutoHeight(bool on)
{
    m_autoHeight = on;
    setVerticalScrollBarPolicy(on ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    if (on)
        updateHeight();
}

void MarkdownView::updateHeight()
{
    if (!m_autoHeight)
        return;
    document()->setTextWidth(viewport()->width());
    setFixedHeight(int(document()->size().height()) + 2 * frameWidth() + 2);
}

void MarkdownView::setReadingWidth(int px)
{
    m_readingWidth = px;
    applyReadingWidth();
}

// The text column is the root frame's side margins (the viewport itself keeps its full width, so the background and
// the scroll bar are untouched).
void MarkdownView::applyReadingWidth()
{
    int extra = 0;
    if (m_readingWidth > 0 && !m_autoHeight)
        extra = qMax(0, (viewport()->width() - m_readingWidth) / 2);
    const int side = int(document()->documentMargin()) + extra; // the root frame's margin is the document margin by default
    QTextFrame *root = document()->rootFrame();
    QTextFrameFormat ff = root->frameFormat();
    m_sideMargin = extra;
    if (int(ff.leftMargin()) == side && int(ff.rightMargin()) == side)
        return;
    ff.setLeftMargin(side);
    ff.setRightMargin(side);
    root->setFrameFormat(ff);
}

void MarkdownView::resizeEvent(QResizeEvent *e)
{
    QTextBrowser::resizeEvent(e);
    applyReadingWidth();
    m_resizeTimer->start();
    if (m_autoHeight && e->size().width() != e->oldSize().width())
        updateHeight();
}

void MarkdownView::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QPalette pal = palette();
    pal.setColor(QPalette::Base, t.editorBg);
    pal.setColor(QPalette::Text, t.editorFg);
    pal.setColor(QPalette::Link, t.accent);
    pal.setColor(QPalette::LinkVisited, t.accent);
    setPalette(pal);
    viewport()->setPalette(pal);
    document()->setDefaultStyleSheet(
        QStringLiteral("a { color: %1; } code { font-family: '%2'; background-color: #010203; }"
                       "pre { font-family: '%2'; white-space: pre-wrap; margin: 0px; }"
                       "pre code { background-color: transparent; }"
                       "small { color: %4; font-size: 11px; letter-spacing: 1px; }"
                       "blockquote { background-color: %5; margin-left: 12px; margin-right: 0px; }"
                       "h1, h2 { color: %3; } h1 { font-size: 26px; } h2 { font-size: 21px; } h3 { font-size: 17px; }"
                       "h1 { margin-top: 18px; margin-bottom: 10px; } h2 { margin-top: 22px; margin-bottom: 8px; }"
                       "h3, h4, h5, h6 { margin-top: 18px; margin-bottom: 6px; }"
                       "p { margin-top: 6px; margin-bottom: 12px; line-height: 160%; }"
                       "li { margin-top: 3px; margin-bottom: 5px; line-height: 160%; } li ul, li ol { margin-top: 2px; margin-bottom: 0px; }"
                       "blockquote { line-height: 160%; }")
            .arg(t.accent.name(), SettingsManager::instance().editorFont().family(), t.editorFg.name(), t.textMuted.name(), t.border.lighter(115).name()));
    if (!m_markdown.isEmpty())
        showMarkdown(m_markdown);
}

void MarkdownView::showMarkdown(const QString &markdown)
{
    m_markdown = markdown;
    QScrollBar *sb = verticalScrollBar();
    const int pos = sb->value();
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    m_sideMargin = 0;
    setHtml(MarkdownHtml::convert(markdown, [&theme](const QString &code, const QString &lang) {
        if (HoverRender::extensionFor(lang.toLower()).isEmpty())
            return QString();
        return HoverRender::codeInline(code, lang.toLower(), theme);
    }));
    applyReadingWidth();
    polish();
    fitImages();
    updateHeight();
    sb->setValue(pos); // keep the reading position while the source changes
}

void MarkdownView::clearMarkdown()
{
    m_markdown.clear();
    clear();
    updateHeight();
}

QVector<QTextTable *> MarkdownView::codeBoxes() const
{
    QVector<QTextTable *> out;
    std::function<void(QTextFrame *)> visit = [&](QTextFrame *f) {
        for (QTextFrame *c : f->childFrames()) {
            if (auto *t = qobject_cast<QTextTable *>(c); isCodeBox(t))
                out << t;
            visit(c);
        }
    };
    visit(document()->rootFrame());
    return out;
}

void MarkdownView::paintEvent(QPaintEvent *e)
{
    {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
        const QRectF visible(0, verticalScrollBar()->value(), viewport()->width(), viewport()->height());
        QAbstractTextDocumentLayout *lay = document()->documentLayout();
        for (QTextTable *t : codeBoxes()) {
            // The box is the text of its two rows plus the cell padding (the frame rect carries margins we do not want).
            const QTextBlock head = t->cellAt(0, 0).firstCursorPosition().block();
            const QTextBlock body = t->cellAt(1, 0).firstCursorPosition().block();
            const QTextBlock last = t->cellAt(1, 0).lastCursorPosition().block();
            const QRectF hr = lay->blockBoundingRect(head), br = lay->blockBoundingRect(body), lr = lay->blockBoundingRect(last);
            const QRectF r(hr.left() - kCodePadding, hr.top() - kCodePadding, br.right() - hr.left() + 2 * kCodePadding,
                           lr.bottom() - hr.top() + 2 * kCodePadding);
            if (!r.intersects(visible))
                continue;
            QPainterPath path;
            path.addRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
            p.fillPath(path, m_boxBg);
            const qreal hy = hr.bottom() + kCodePadding;
            p.save();
            p.setClipPath(path);
            p.fillRect(QRectF(r.left(), r.top(), r.width(), hy - r.top()), m_headBg);
            p.setPen(m_edge);
            p.drawLine(QPointF(r.left(), hy), QPointF(r.right(), hy));
            p.restore();
            p.setPen(QPen(m_edge, 1));
            p.drawPath(path);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Text));
        for (const auto &bullet : m_bullets) {
            const QTextBlock b = document()->findBlockByNumber(bullet.first);
            if (!b.isValid() || !b.layout() || b.layout()->lineCount() == 0)
                continue;
            const QRectF br = lay->blockBoundingRect(b);
            if (!br.intersects(visible))
                continue;
            const QTextLine line = b.layout()->lineAt(0);
            const qreal cx = br.left() + line.cursorToX(0) - 15, cy = br.top() + line.y() + line.height() / 2;
            const qreal rad = 2.6;
            if (bullet.second == QTextListFormat::ListSquare) {
                p.drawRect(QRectF(cx - rad, cy - rad, 2 * rad, 2 * rad));
            } else if (bullet.second == QTextListFormat::ListCircle) {
                p.setPen(QPen(palette().color(QPalette::Text), 1.2));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(QPointF(cx, cy), rad, rad);
                p.setPen(Qt::NoPen);
                p.setBrush(palette().color(QPalette::Text));
            } else {
                p.drawEllipse(QPointF(cx, cy), rad, rad);
            }
        }
        p.setBrush(m_pillBg);
        for (const auto &pill : m_pills) {
            const QTextBlock b = document()->findBlock(pill.first);
            if (!b.isValid() || !b.layout())
                continue;
            const QRectF br = lay->blockBoundingRect(b);
            if (!br.intersects(visible))
                continue;
            const int from = pill.first - b.position(), to = qMin(from + pill.second, b.length() - 1);
            for (int i = 0; i < b.layout()->lineCount(); ++i) {
                const QTextLine line = b.layout()->lineAt(i);
                const int a = qMax(from, line.textStart()), z = qMin(to, line.textStart() + line.textLength());
                if (a >= z)
                    continue;
                const qreal x1 = line.cursorToX(a), x2 = line.cursorToX(z);
                const QRectF r(br.left() + qMin(x1, x2), br.top() + line.y(), qAbs(x2 - x1), line.height());
                p.drawRoundedRect(r.adjusted(0, 1, 0, -1), 4, 4);
            }
        }
    }
    QTextBrowser::paintEvent(e);
}

// The Markdown importer ignores the palette for links and gives tables no width or padding.
void MarkdownView::polish()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QTextDocument *doc = document();
    doc->setUndoRedoEnabled(false);
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.charFormat().isAnchor())
                continue;
            QTextCursor c(doc);
            c.setPosition(f.position());
            c.setPosition(f.position() + f.length(), QTextCursor::KeepAnchor);
            QTextCharFormat fmt;
            fmt.setForeground(t.accent);
            c.mergeCharFormat(fmt);
        }
    std::function<void(QTextFrame *)> visit = [&](QTextFrame *frame) {
        for (QTextFrame *child : frame->childFrames()) {
            if (auto *table = qobject_cast<QTextTable *>(child); isCodeBox(table)) {
                QTextTableFormat tf = table->format();
                tf.setBorder(0);
                tf.setLeftMargin(0);
                tf.setRightMargin(0);
                tf.setTopMargin(6);
                tf.setBottomMargin(10);
                tf.setBackground(Qt::transparent);
                table->setFormat(tf);
            } else if (table) {
                QTextTableFormat tf = table->format();
                tf.setWidth(QTextLength(QTextLength::PercentageLength, 100));
                tf.setCellPadding(9);
                tf.setCellSpacing(0);
                tf.setBorder(1);
                tf.setBorderBrush(t.border);
                tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                table->setFormat(tf);
            }
            visit(child);
        }
    };
    visit(doc->rootFrame());

    // Inline code: the stylesheet marks it with a sentinel background; the browser paints a rounded pill instead.
    const bool dark = t.dark;
    m_boxBg = dark ? QColor::fromHsl(t.editorBg.hslHue(), t.editorBg.hslSaturation(), qMin(255, t.editorBg.lightness() + 14))
                       : QColor::fromHsl(t.editorBg.hslHue(), t.editorBg.hslSaturation(), qMax(0, t.editorBg.lightness() - 10));
    m_headBg = dark ? m_boxBg.lighter(125) : m_boxBg.darker(104);
    m_edge = t.border;
    m_pillBg = dark ? QColor::fromHsl(t.editorBg.hslHue(), t.editorBg.hslSaturation(), qMin(255, t.editorBg.lightness() + 28))
                        : QColor::fromHsl(t.editorBg.hslHue(), t.editorBg.hslSaturation(), qMax(0, t.editorBg.lightness() - 20));
    m_pills.clear();
    // Bullets: Qt draws them hard against the text, so the list drops its own marker and paintEvent draws one with a real gap.
    m_bullets.clear();
    QSet<QTextList *> seen;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (QTextList *l = b.textList()) {
            const auto st = l->format().style();
            if (st != QTextListFormat::ListDisc && st != QTextListFormat::ListCircle && st != QTextListFormat::ListSquare)
                continue;
            seen.insert(l);
            if (b.text().startsWith(QChar(0x2610)) || b.text().startsWith(QChar(0x2611)))
                continue; // task item: its check box is the marker
            m_bullets.append({b.blockNumber(), int(st)}); // block numbers: the pill padding below shifts positions
        }
    for (QTextList *l : seen) {
        QTextListFormat lf = l->format();
        lf.setStyle(QTextListFormat::ListStyleUndefined);
        l->setFormat(lf);
    }
    struct Code { int pos, len; bool pill; QTextCharFormat fmt; };
    QVector<Code> codes;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.charFormat().background().style() == Qt::NoBrush || f.charFormat().background().color() != kInlineCodeMark)
                continue;
            codes.append({f.position(), f.length(), !isCodeBox(QTextCursor(b).currentTable()), f.charFormat()});
        }
    // Back to front so earlier positions stay valid. A pill gets a no-break narrow space on each side as padding: it
    // cannot wrap away from its brackets and punctuation, and the pill is painted around it in paintEvent.
    const QChar pad(0x202F);
    for (int k = int(codes.size()) - 1; k >= 0; --k) {
        Code &cd = codes[k];
        QTextCharFormat none = cd.fmt;
        none.setBackground(Qt::NoBrush);
        QTextCursor c(doc);
        c.setPosition(cd.pos);
        c.setPosition(cd.pos + cd.len, QTextCursor::KeepAnchor);
        c.mergeCharFormat(none);
        if (cd.pill) {
            c.setPosition(cd.pos + cd.len);
            c.insertText(pad, none);
            c.setPosition(cd.pos);
            c.insertText(pad, none);
        }
    }
    int shift = 0;
    for (const Code &cd : std::as_const(codes))
        if (cd.pill) {
            m_pills.append({cd.pos + shift, cd.len + 2});
            shift += 2;
        }
    // Task list items: no bullet next to the check box, and a ticked box in the accent colour.
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.textList() && b.text().startsWith(QChar(0x2611))) {
            QTextCursor c(doc);
            c.setPosition(b.position());
            c.setPosition(b.position() + 1, QTextCursor::KeepAnchor);
            QTextCharFormat tick;
            tick.setForeground(t.accent);
            c.mergeCharFormat(tick);
        }
    doc->setUndoRedoEnabled(true);
}

// Images keep their size but never get wider than the view, so there is no horizontal scrolling.
void MarkdownView::fitImages()
{
    QTextDocument *doc = document();
    const int avail = qMax(60, viewport()->width() - int(2 * doc->documentMargin()) - 2 * m_sideMargin - 4);
    doc->setUndoRedoEnabled(false);
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.charFormat().isImageFormat())
                continue;
            QTextImageFormat img = f.charFormat().toImageFormat();
            const QUrl url = doc->baseUrl().resolved(QUrl(img.name()));
            QSize natural;
            if (url.isLocalFile())
                natural = QImageReader(url.toLocalFile()).size();
            if (!natural.isValid())
                continue;
            if (!img.hasProperty(kOrigWidth)) // remember what the author asked for before we shrink it
                img.setProperty(kOrigWidth, img.width() > 0 ? img.width() : natural.width());
            const int w = qMin(img.property(kOrigWidth).toInt(), avail);
            if (qFuzzyCompare(img.width() + 1.0, w + 1.0) && img.hasProperty(kOrigWidth) && img.width() == w)
                continue;
            img.setWidth(w);
            img.setHeight(qreal(natural.height()) * w / natural.width());
            QTextCursor c(doc);
            c.setPosition(f.position());
            c.setPosition(f.position() + f.length(), QTextCursor::KeepAnchor);
            c.setCharFormat(img);
        }
    doc->setUndoRedoEnabled(true);
}


bool MarkdownView::handleCopyLink(const QUrl &url)
{
    if (url.scheme() != QLatin1String("qode-copy"))
        return false;
    const QPoint at = viewport()->mapFromGlobal(QCursor::pos());
    if (auto *table = cursorForPosition(at).currentTable(); isCodeBox(table)) {
        QTextCursor c = table->cellAt(1, 0).firstCursorPosition();
        c.setPosition(table->cellAt(1, 0).lastCursorPosition().position(), QTextCursor::KeepAnchor);
        QGuiApplication::clipboard()->setText(c.selection().toPlainText());
        QToolTip::showText(QCursor::pos(), tr("Copied"), this);
    }
    return true;
}
