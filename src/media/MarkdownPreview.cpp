#include "MarkdownPreview.h"

#include "MarkdownView.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {
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

    m_view = new MarkdownView(this);
    connect(m_view, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) { followLink(url); });

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addWidget(m_view, 1);

    m_view->setReadingWidth(860);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (m_applying)
            return;
        const auto a = anchors();
        const double y = value;
        for (int k = 1; k < a.size(); ++k)
            if (y <= a[k].second || k == a.size() - 1) {
                const double span = a[k].second - a[k - 1].second;
                const double f = span > 0 ? qBound(0.0, (y - a[k - 1].second) / span, 1.0) : 0.0;
                emit topLineChanged(int(a[k - 1].first + f * (a[k].first - a[k - 1].first)));
                return;
            }
    });
    connect(close, &QToolButton::clicked, this, &MarkdownPreview::closeRequested);
}

void MarkdownPreview::setMarkdown(const QString &filePath, const QString &text)
{
    if (filePath != m_path) {
        m_path = filePath;
        m_title->setText(filePath.isEmpty() ? tr("PREVIEW") : tr("PREVIEW — %1").arg(QFileInfo(filePath).fileName()));
        if (!filePath.isEmpty())
            m_view->setBaseDirectory(QFileInfo(filePath).absolutePath());
    }
    // Source lines of the headings (ATX and setext, outside code fences) pair up with the rendered heading blocks.
    m_headingLines.clear();
    const QStringList lines = text.split(QLatin1Char('\n'));
    m_lineCount = int(lines.size());
    static const QRegularExpression atx(QStringLiteral("^ {0,3}#{1,6}(?:[ \\t]|$)")), fence(QStringLiteral("^ {0,3}(```|~~~)")),
        underline(QStringLiteral("^ {0,3}(?:=+|-+)[ \\t]*$")), notText(QStringLiteral("^\\s*(?:[#>|*+-]|\\d+[.)]\\s|$)"));
    bool inFence = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (fence.match(lines[i]).hasMatch()) {
            inFence = !inFence;
            continue;
        }
        if (inFence)
            continue;
        if (atx.match(lines[i]).hasMatch())
            m_headingLines << i;
        else if (i + 1 < lines.size() && !notText.match(lines[i]).hasMatch() && underline.match(lines[i + 1]).hasMatch())
            m_headingLines << i;
    }
    m_applying = true; // showMarkdown resets and restores the scroll position; that is not the user scrolling
    m_view->showMarkdown(text);
    m_applying = false;
}

QVector<QPair<double, double>> MarkdownPreview::anchors() const
{
    QVector<QPair<double, double>> a;
    a.append({0.0, 0.0});
    QVector<double> ys;
    for (QTextBlock b = m_view->document()->begin(); b.isValid(); b = b.next())
        if (b.blockFormat().headingLevel() > 0)
            ys << m_view->document()->documentLayout()->blockBoundingRect(b).top();
    if (ys.size() == m_headingLines.size()) // otherwise raw HTML headings or similar: only the ends are known
        for (int k = 0; k < ys.size(); ++k)
            if (m_headingLines[k] > a.last().first && ys[k] > a.last().second)
                a.append({double(m_headingLines[k]), ys[k]});
    const double end = qMax(1, m_view->verticalScrollBar()->maximum());
    a.append({double(qMax(m_lineCount, int(a.last().first) + 1)), qMax(end, a.last().second + 1)});
    return a;
}

void MarkdownPreview::scrollToSourceLine(int line)
{
    const auto a = anchors();
    for (int k = 1; k < a.size(); ++k)
        if (line < a[k].first || k == a.size() - 1) {
            const double span = a[k].first - a[k - 1].first;
            const double f = span > 0 ? qBound(0.0, (line - a[k - 1].first) / span, 1.0) : 0.0;
            m_applying = true;
            m_view->verticalScrollBar()->setValue(int(a[k - 1].second + f * (a[k].second - a[k - 1].second)));
            m_applying = false;
            return;
        }
}

// Clicking a link: copy buttons, #anchors, project files (open in the editor) or anything else (the system).
void MarkdownPreview::followLink(const QUrl &url)
{
    if (m_view->handleCopyLink(url))
        return;
    if (url.isRelative() && url.hasFragment() && url.path().isEmpty()) {
        const QString want = slugOf(QUrl::fromPercentEncoding(url.fragment().toUtf8()));
        for (QTextBlock b = m_view->document()->begin(); b.isValid(); b = b.next())
            if (b.blockFormat().headingLevel() > 0 && slugOf(b.text().trimmed()) == want) {
                m_view->verticalScrollBar()->setValue(int(m_view->document()->documentLayout()->blockBoundingRect(b).top()) - 8);
                return;
            }
        m_view->scrollToAnchor(QUrl::fromPercentEncoding(url.fragment().toUtf8())); // footnotes and other named anchors
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

void MarkdownPreview::clear()
{
    m_path.clear();
    m_view->clearMarkdown();
}
