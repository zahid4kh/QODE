#include "MarkdownPreview.h"

#include "MarkdownHtml.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocumentFragment>
#include <QTextLayout>
#include <QToolTip>
#include <QFileInfo>
#include <QImageReader>
#include <QResizeEvent>
#include <QTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <functional>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kOrigWidth = QTextFormat::UserProperty + 8; // the width the document asked for
constexpr int kCodePadding = 7;                           // cell padding that marks a code box (see MarkdownHtml)
const QColor kInlineCodeMark(1, 2, 3);                    // the stylesheet paints `code` with this; polish() turns it into a pill

bool isCodeBox(const QTextTable *t) { return t && t->format().cellPadding() == kCodePadding; }

QString slugOf(const QString &text)
{
    QString s;
    for (const QChar c : text.toLower()) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-'))
            s += c;
        else if (c == QLatin1Char(' '))
            s += QLatin1Char('-');
    }
    return s;
}

// QTextBrowser that paints the rounded backgrounds of code boxes and inline code behind the text.
class PreviewBrowser : public QTextBrowser
{
public:
    using QTextBrowser::QTextBrowser;
    QColor boxBg, headBg, edge, pillBg;
    QVector<QPair<int, int>> pills; // (position, length) of inline code

    QVector<QTextTable *> codeBoxes() const
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

protected:
    void paintEvent(QPaintEvent *e) override
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
                p.fillPath(path, boxBg);
                const qreal hy = hr.bottom() + kCodePadding;
                p.save();
                p.setClipPath(path);
                p.fillRect(QRectF(r.left(), r.top(), r.width(), hy - r.top()), headBg);
                p.setPen(edge);
                p.drawLine(QPointF(r.left(), hy), QPointF(r.right(), hy));
                p.restore();
                p.setPen(QPen(edge, 1));
                p.drawPath(path);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(pillBg);
            for (const auto &pill : pills) {
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
};
}

MarkdownPreview::MarkdownPreview(QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(tr("PREVIEW"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    auto *close = new QToolButton(this);
    Icons::bind(close, QStringLiteral(":/new-icons/x.svg"));
    close->setToolTip(tr("Close preview"));
    close->setAutoRaise(true);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("previewHeader"));
    header->setStyleSheet(QStringLiteral("QWidget#previewHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#previewHeader QLabel { background: transparent; border: none; }"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(m_title, 1);
    hl->addWidget(close);

    m_view = new PreviewBrowser(this);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setOpenLinks(false);
    m_view->setLineWrapMode(QTextEdit::WidgetWidth); // never scroll sideways
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->document()->setDocumentMargin(16);
    connect(m_view, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) { followLink(url); });

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addWidget(m_view, 1);

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(120);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] { fitImages(); });
    m_view->viewport()->installEventFilter(this);

    connect(close, &QToolButton::clicked, this, &MarkdownPreview::closeRequested);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &MarkdownPreview::applyTheme);
    applyTheme();
}

void MarkdownPreview::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QPalette pal = m_view->palette();
    pal.setColor(QPalette::Base, t.editorBg);
    pal.setColor(QPalette::Text, t.editorFg);
    pal.setColor(QPalette::Link, t.accent);
    pal.setColor(QPalette::LinkVisited, t.accent);
    m_view->setPalette(pal);
    m_view->viewport()->setPalette(pal);
    const QString tint = t.border.lighter(130).name();
    m_view->document()->setDefaultStyleSheet(
        QStringLiteral("a { color: %1; } code { font-family: '%2'; background-color: #010203; }"
                       "pre { font-family: '%2'; white-space: pre-wrap; margin: 0px; }"
                       "pre code { background-color: transparent; }"
                       "small { color: %4; font-size: 11px; letter-spacing: 1px; }"
                       "blockquote { background-color: %5; margin-left: 12px; margin-right: 0px; }"
                       "h1, h2 { color: %3; } h1 { font-size: 26px; } h2 { font-size: 21px; } h3 { font-size: 17px; }"
                       "p { margin-top: 4px; margin-bottom: 8px; } li { margin-bottom: 2px; } li ul, li ol { margin-top: 0px; margin-bottom: 0px; }")
            .arg(t.accent.name(), SettingsManager::instance().editorFont().family(), t.editorFg.name(), t.textMuted.name(), t.border.lighter(115).name()));
    if (!m_text.isEmpty())
        setMarkdown(m_path, m_text);
}

void MarkdownPreview::setMarkdown(const QString &filePath, const QString &text)
{
    if (filePath != m_path) {
        m_path = filePath;
        m_title->setText(filePath.isEmpty() ? tr("PREVIEW") : tr("PREVIEW — %1").arg(QFileInfo(filePath).fileName()));
        if (!filePath.isEmpty())
            m_view->document()->setBaseUrl(QUrl::fromLocalFile(QFileInfo(filePath).absolutePath() + QLatin1Char('/')));
    }
    m_text = text;
    QScrollBar *sb = m_view->verticalScrollBar();
    const int pos = sb->value();
    m_view->setHtml(MarkdownHtml::convert(text));
    polish();
    fitImages();
    sb->setValue(pos); // keep the reading position while the source changes
}

// The Markdown importer ignores the palette for links and gives tables no width or padding.
void MarkdownPreview::polish()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QTextDocument *doc = m_view->document();
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
                tf.setCellPadding(6);
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
    auto *view = static_cast<PreviewBrowser *>(m_view);
    const bool dark = t.dark;
    view->boxBg = dark ? QColor::fromHsl(t.editorBg.hslHue(), t.editorBg.hslSaturation(), qMin(255, t.editorBg.lightness() + 14))
                       : QColor::fromHsl(t.editorBg.hslHue(), t.editorBg.hslSaturation(), qMax(0, t.editorBg.lightness() - 10));
    view->headBg = dark ? view->boxBg.lighter(125) : view->boxBg.darker(104);
    view->edge = t.border;
    view->pillBg = dark ? QColor::fromHsl(t.editorBg.hslHue(), t.editorBg.hslSaturation(), qMin(255, t.editorBg.lightness() + 28))
                        : QColor::fromHsl(t.editorBg.hslHue(), t.editorBg.hslSaturation(), qMax(0, t.editorBg.lightness() - 20));
    view->pills.clear();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.charFormat().background().style() == Qt::NoBrush || f.charFormat().background().color() != kInlineCodeMark)
                continue;
            if (!isCodeBox(QTextCursor(b).currentTable()))
                view->pills.append({f.position(), f.length()});
            QTextCursor c(doc);
            c.setPosition(f.position());
            c.setPosition(f.position() + f.length(), QTextCursor::KeepAnchor);
            QTextCharFormat none;
            none.setBackground(Qt::NoBrush);
            c.mergeCharFormat(none);
        }
    doc->setUndoRedoEnabled(true);
}

// Clicking a link: copy buttons, #anchors, project files (open in the editor) or anything else (the system).
void MarkdownPreview::followLink(const QUrl &url)
{
    if (url.scheme() == QLatin1String("qode-copy")) {
        const QPoint at = m_view->viewport()->mapFromGlobal(QCursor::pos());
        if (auto *table = m_view->cursorForPosition(at).currentTable(); isCodeBox(table)) {
            QTextCursor c = table->cellAt(1, 0).firstCursorPosition();
            c.setPosition(table->cellAt(1, 0).lastCursorPosition().position(), QTextCursor::KeepAnchor);
            QGuiApplication::clipboard()->setText(c.selection().toPlainText());
            QToolTip::showText(QCursor::pos(), tr("Copied"), m_view);
        }
        return;
    }
    if (url.isRelative() && url.hasFragment() && url.path().isEmpty()) {
        const QString want = slugOf(QUrl::fromPercentEncoding(url.fragment().toUtf8()));
        for (QTextBlock b = m_view->document()->begin(); b.isValid(); b = b.next())
            if (b.blockFormat().headingLevel() > 0 && slugOf(b.text().trimmed()) == want) {
                m_view->verticalScrollBar()->setValue(int(m_view->document()->documentLayout()->blockBoundingRect(b).top()) - 8);
                return;
            }
        return;
    }
    const QUrl target = m_view->document()->baseUrl().resolved(url);
    if (target.isLocalFile()) {
        const QFileInfo fi(target.toLocalFile());
        if (fi.isFile()) {
            emit openFileRequested(fi.absoluteFilePath());
            return;
        }
        if (fi.isDir())
            return;
    }
    QDesktopServices::openUrl(target);
}

// Images keep their size but never get wider than the view, so there is no horizontal scrolling.
void MarkdownPreview::fitImages()
{
    QTextDocument *doc = m_view->document();
    const int avail = qMax(60, m_view->viewport()->width() - int(2 * doc->documentMargin()) - 4);
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

bool MarkdownPreview::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_view->viewport() && e->type() == QEvent::Resize)
        m_resizeTimer->start();
    return QWidget::eventFilter(obj, e);
}

void MarkdownPreview::clear()
{
    m_path.clear();
    m_view->clear();
}
