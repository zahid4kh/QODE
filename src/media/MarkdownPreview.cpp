#include "MarkdownPreview.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QDesktopServices>
#include <QFileInfo>
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

    m_view = new QTextBrowser(this);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setOpenLinks(false);
    m_view->document()->setDocumentMargin(16);
    connect(m_view, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.isRelative() && url.hasFragment() && url.path().isEmpty()) {
            m_view->scrollToAnchor(url.fragment());
            return;
        }
        QDesktopServices::openUrl(m_view->document()->baseUrl().resolved(url));
    });

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addWidget(m_view, 1);

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
    m_view->document()->setDefaultStyleSheet(
        QStringLiteral("a { color: %1; } code, pre { font-family: '%2'; } h1, h2 { color: %3; }")
            .arg(t.accent.name(), SettingsManager::instance().editorFont().family(), t.editorFg.name()));
}

void MarkdownPreview::setMarkdown(const QString &filePath, const QString &text)
{
    if (filePath != m_path) {
        m_path = filePath;
        m_title->setText(filePath.isEmpty() ? tr("PREVIEW") : tr("PREVIEW — %1").arg(QFileInfo(filePath).fileName()));
        if (!filePath.isEmpty())
            m_view->document()->setBaseUrl(QUrl::fromLocalFile(QFileInfo(filePath).absolutePath() + QLatin1Char('/')));
    }
    QScrollBar *sb = m_view->verticalScrollBar();
    const int pos = sb->value();
    m_view->setMarkdown(text);
    polish();
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
            if (auto *table = qobject_cast<QTextTable *>(child)) {
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
    doc->setUndoRedoEnabled(true);
}

void MarkdownPreview::clear()
{
    m_path.clear();
    m_view->clear();
}
