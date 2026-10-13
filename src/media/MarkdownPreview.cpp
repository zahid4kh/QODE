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
    m_view->showMarkdown(text);
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
