#include "http/HttpResponseView.h"

#include "http/HttpRunner.h"
#include "http/HttpUi.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr qint64 kMaxShown = 2 * 1024 * 1024;

bool isImage(const QString &type)
{
    return type.startsWith(QLatin1String("image/")) && type != QLatin1String("image/svg+xml");
}

bool isBinary(const QString &type)
{
    return type.startsWith(QLatin1String("image/")) || type.startsWith(QLatin1String("video/")) || type.startsWith(QLatin1String("audio/"))
           || type.contains(QLatin1String("octet-stream")) || type.contains(QLatin1String("pdf")) || type.contains(QLatin1String("zip"))
           || type.contains(QLatin1String("protobuf"));
}

QString extensionFor(const QString &type)
{
    if (type.contains(QLatin1String("json")))
        return QStringLiteral("json");
    if (type.contains(QLatin1String("html")))
        return QStringLiteral("html");
    if (type.contains(QLatin1String("xml")))
        return QStringLiteral("xml");
    if (type.startsWith(QLatin1String("image/")))
        return type.mid(6).section(QLatin1Char('+'), 0, 0);
    if (type.contains(QLatin1String("pdf")))
        return QStringLiteral("pdf");
    if (type.startsWith(QLatin1String("text/")))
        return QStringLiteral("txt");
    return QStringLiteral("bin");
}

} // namespace

HttpResponseView::HttpResponseView(QWidget *parent) : QWidget(parent)
{
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::RichText);
    m_status->setWordWrap(true);
    m_status->setOpenExternalLinks(false);
    m_status->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(m_status, &QLabel::linkActivated, this, &HttpResponseView::linkActivated);

    auto *tabsRow = new QWidget(this);
    auto *tl = new QHBoxLayout(tabsRow);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(0);
    m_tabs = new QButtonGroup(this);
    const QStringList names{tr("Body"), tr("Headers"), tr("Request")};
    for (int i = 0; i < names.size(); ++i) {
        auto *b = new QToolButton(tabsRow);
        b->setText(names.at(i));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setProperty("seg", true);
        m_tabs->addButton(b, i);
        tl->addWidget(b);
    }
    m_tabs->button(0)->setChecked(true);

    auto tool = [this](const QString &tip, const QString &icon) {
        auto *b = new QToolButton(this);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        Icons::bind(b, icon);
        return b;
    };
    m_wrap = tool(tr("Wrap long lines"), QStringLiteral(":/new-icons/wrap-text.svg"));
    m_wrap->setCheckable(true);
    m_copy = tool(tr("Copy what the tab shows"), QStringLiteral(":/new-icons/copy.svg"));
    m_save = tool(tr("Save the response body to a file"), QStringLiteral(":/new-icons/download.svg"));
    m_open = tool(tr("Open what the tab shows in an editor tab"), QStringLiteral(":/new-icons/external-link.svg"));

    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->setPlaceholderText(tr("Send a request to see the response."));
    m_colours = new HttpJsonHighlighter(m_text->document());
    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_imageScroll = new QScrollArea(this);
    m_imageScroll->setObjectName(QStringLiteral("httpImage"));
    m_imageScroll->setWidget(m_image);
    m_imageScroll->setWidgetResizable(true);
    m_imageScroll->setAlignment(Qt::AlignCenter);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_text);
    m_stack->addWidget(m_imageScroll);

    auto *statusWrap = new QWidget(this);
    auto *sw = new QHBoxLayout(statusWrap);
    sw->setContentsMargins(10, 6, 8, 2);
    sw->addWidget(m_status);
    auto *head = new QWidget(this);
    m_toolRow = new QHBoxLayout(head);
    m_toolRow->setContentsMargins(8, 0, 6, 0);
    m_toolRow->setSpacing(0);
    m_toolRow->addWidget(tabsRow);
    m_toolRow->addStretch(1);
    m_toolIndex = m_toolRow->count();
    for (QToolButton *b : {m_wrap, m_copy, m_save, m_open})
        m_toolRow->addWidget(b);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(statusWrap);
    l->addWidget(head);
    l->addWidget(m_stack, 1);

    connect(m_tabs, &QButtonGroup::idClicked, this, &HttpResponseView::showTab);
    connect(m_wrap, &QToolButton::toggled, this, [this](bool on) { m_text->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap); });
    connect(m_copy, &QToolButton::clicked, this, [this] {
        if (m_stack->currentWidget() == m_imageScroll)
            QApplication::clipboard()->setImage(QImage::fromData(m_result.body));
        else
            QApplication::clipboard()->setText(m_text->toPlainText());
    });
    connect(m_save, &QToolButton::clicked, this, &HttpResponseView::saveBody);
    connect(m_open, &QToolButton::clicked, this, [this] {
        if (m_stack->currentWidget() == m_text && !m_text->toPlainText().isEmpty())
            emit openTextRequested(m_text->toPlainText());
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &HttpResponseView::applyTheme);
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, &HttpResponseView::applyTheme);
    applyTheme();
    clear();
}

void HttpResponseView::applyTheme()
{
    m_theme = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(HttpUi::styleSheet(m_theme));
    m_text->setFont(SettingsManager::instance().editorFont());
    m_colours->setTheme(m_theme);
    if (m_hasResult)
        showResult(m_result); // the status colours
}

void HttpResponseView::addTool(QToolButton *button)
{
    button->setParent(this);
    m_toolRow->insertWidget(m_toolIndex++, button);
}

void HttpResponseView::setPlaceholder(const QString &text)
{
    m_text->setPlaceholderText(text);
}

void HttpResponseView::clear()
{
    m_hasResult = false;
    m_result = HttpResult();
    m_status->clear();
    m_status->hide();
    m_text->clear();
    m_image->clear();
    m_stack->setCurrentWidget(m_text);
    for (QToolButton *b : {m_copy, m_save, m_open})
        b->setEnabled(false);
}

void HttpResponseView::showBusy(const QString &text)
{
    m_status->show();
    m_status->setText(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(m_theme.textMuted.name(), text.toHtmlEscaped()));
}

void HttpResponseView::showMessage(const QString &html, const QColor &colour)
{
    clear();
    m_status->show();
    m_status->setText(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(colour.name(), html));
}

void HttpResponseView::showResult(const HttpResult &r)
{
    m_result = r;
    m_hasResult = true;
    m_status->show();
    const QString muted = m_theme.textMuted.name();
    QString html;
    if (r.ok()) {
        html = QStringLiteral("<span style=\"color:%1\"><b>%2 %3</b></span> <span style=\"color:%4\">· %5 ms · %6</span>")
                   .arg(HttpUi::statusColour(m_theme, r.status).name())
                   .arg(r.status)
                   .arg(r.reason.toHtmlEscaped(), muted)
                   .arg(r.ms)
                   .arg(HttpUi::sizeText(r.body.size()));
    } else {
        html = QStringLiteral("<span style=\"color:%1\">%2</span>").arg(m_theme.danger.name(), HttpRunner::describeError(r));
    }
    if (!r.note.isEmpty())
        html += QStringLiteral("<br><span style=\"color:%1\">%2</span>").arg(muted, r.note.toHtmlEscaped());
    m_status->setText(html);
    m_save->setEnabled(r.ok() && !r.body.isEmpty());
    showTab(m_tab);
}

QString HttpResponseView::bodyText() const
{
    const HttpResult &r = m_result;
    if (!r.ok())
        return {};
    const QString type = r.contentType();
    if (isImage(type))
        return tr("(an image QODE cannot show, %1 — save it to keep it)").arg(HttpUi::sizeText(r.body.size()));
    if (isBinary(type))
        return tr("(binary data, %1 — save it to keep it)").arg(HttpUi::sizeText(r.body.size()));
    QString text = QString::fromUtf8(r.body.left(kMaxShown));
    const QByteArray trimmed = r.body.trimmed();
    if (r.body.size() <= kMaxShown && (type.contains(QLatin1String("json")) || trimmed.startsWith('{') || trimmed.startsWith('['))) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(r.body, &err);
        if (err.error == QJsonParseError::NoError)
            text = QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
    }
    if (r.body.size() > kMaxShown)
        text += tr("\n… (truncated, %1 in total)").arg(HttpUi::sizeText(r.body.size()));
    return text;
}

void HttpResponseView::showTab(int tab)
{
    m_tab = tab;
    m_tabs->button(tab)->setChecked(true);
    const HttpResult &r = m_result;
    QPixmap px;
    const bool image = tab == 0 && r.ok() && isImage(r.contentType()) && px.loadFromData(r.body);
    if (image) {
        m_image->setPixmap(px);
        m_stack->setCurrentWidget(m_imageScroll);
    } else {
        m_stack->setCurrentWidget(m_text);
    }
    QString text;
    bool json = false;
    if (!m_hasResult) {
        text.clear();
    } else if (tab == 0) {
        text = bodyText();
        json = text.size() < kMaxShown && (r.contentType().contains(QLatin1String("json")) || text.startsWith(QLatin1Char('{')) || text.startsWith(QLatin1Char('[')));
    } else if (tab == 1) {
        QStringList lines;
        for (const auto &h : r.headers)
            lines << QString::fromLatin1(h.first) + QStringLiteral(": ") + QString::fromUtf8(h.second);
        text = lines.join(QLatin1Char('\n'));
    } else {
        QStringList lines{r.request.method + QLatin1Char(' ') + r.request.url.toString()};
        for (const auto &h : r.request.headers)
            lines << QString::fromUtf8(h.first + ": " + h.second);
        text = lines.join(QLatin1Char('\n'));
        if (!r.request.body.isEmpty()) {
            text += QStringLiteral("\n\n") + QString::fromUtf8(r.request.body.left(256 * 1024));
            if (r.request.body.size() > 256 * 1024)
                text += tr("\n… (%1 in total)").arg(HttpUi::sizeText(r.request.body.size()));
        }
        json = !r.request.body.isEmpty();
    }
    m_colours->setEnabled(json);
    m_text->setPlainText(text);
    m_copy->setEnabled(image || !text.isEmpty());
    m_open->setEnabled(!image && !text.isEmpty());
}

void HttpResponseView::saveBody()
{
    if (!m_result.ok() || m_result.body.isEmpty())
        return;
    QString name = QFileInfo(m_result.request.url.path()).completeBaseName();
    if (name.isEmpty())
        name = QStringLiteral("response");
    const QString suggested = QDir(SettingsManager::instance().lastDirectory()).filePath(name + QLatin1Char('.') + extensionFor(m_result.contentType()));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Response Body"), suggested);
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(m_result.body) != m_result.body.size()) {
        m_status->setText(m_status->text() + QStringLiteral("<br><span style=\"color:%1\">%2</span>")
                                                 .arg(m_theme.danger.name(), tr("Could not write %1").arg(path.toHtmlEscaped())));
        return;
    }
    SettingsManager::instance().setLastDirectory(QFileInfo(path).path());
}
