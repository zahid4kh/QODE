#include "spring/HttpClientView.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QSyntaxHighlighter>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

enum Role { MethodRole = Qt::UserRole + 1, DetailRole, StatusRole, PayloadRole, EndpointRole, HeaderRole };

constexpr int kMaxHistory = 80;

// Colours JSON (also fine for plain text: nothing matches).
class JsonHighlighter : public QSyntaxHighlighter
{
public:
    explicit JsonHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {}
    void setTheme(const Theme &t)
    {
        m_key = t.property;
        m_string = t.string;
        m_number = t.number;
        m_literal = t.keyword;
        rehighlight();
    }

protected:
    void highlightBlock(const QString &text) override
    {
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

private:
    QColor m_key, m_string, m_number, m_literal;
};

// Rows of the endpoint / history / saved lists: coloured method, text, muted detail on the right.
class RowDelegate : public QStyledItemDelegate
{
public:
    RowDelegate(const Theme *theme, QObject *parent) : QStyledItemDelegate(parent), m_theme(theme) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        return QSize(0, index.data(HeaderRole).toBool() ? 24 : 26);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
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
            p->setPen(methodColour(method));
            const int w = QFontMetrics(f).horizontalAdvance(QStringLiteral("DELETE")) + 8;
            p->drawText(QRect(x, r.top(), w, r.height()), Qt::AlignLeft | Qt::AlignVCenter, method);
            x += w;
        }
        p->setFont(opt.font);
        const QString detail = index.data(DetailRole).toString();
        int right = r.right() - 8;
        if (!detail.isEmpty()) {
            const int status = index.data(StatusRole).toInt();
            QFont f = opt.font;
            f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1));
            p->setFont(f);
            const QFontMetrics fm(f);
            const int w = qMin(fm.horizontalAdvance(detail) + 6, r.width() / 3);
            p->setPen(status >= 200 && status < 300 ? t.success : status >= 300 && status < 400 ? t.warning : status >= 400 ? t.danger : t.textMuted);
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

private:
    QColor methodColour(const QString &m) const
    {
        const Theme &t = *m_theme;
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
    const Theme *m_theme;
};

QString sizeText(qint64 b)
{
    if (b < 1024)
        return QStringLiteral("%1 B").arg(b);
    if (b < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(b / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(b / 1048576.0, 0, 'f', 1);
}

QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

} // namespace

HttpClientView::HttpClientView(QWidget *parent) : QWidget(parent)
{
    m_theme = Theme::byName(SettingsManager::instance().theme());

    // Text tabs: flat checkable buttons in one group.
    auto segments = [this](const QStringList &names, QButtonGroup *&group) {
        auto *row = new QWidget(this);
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        group = new QButtonGroup(row);
        group->setExclusive(true);
        for (int i = 0; i < names.size(); ++i) {
            auto *b = new QToolButton(row);
            b->setText(names.at(i));
            b->setCheckable(true);
            b->setAutoRaise(true);
            b->setProperty("seg", true);
            group->addButton(b, i);
            l->addWidget(b);
        }
        group->button(0)->setChecked(true);
        return row;
    };
    auto tool = [this](const QString &tip) {
        auto *b = new QToolButton(this);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };

    // --- Endpoints / History / Saved
    auto *listTabsRow = segments({tr("Endpoints"), tr("History"), tr("Saved")}, m_listTabs);
    m_listAction = tool(QString());
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter"));
    m_filter->setClearButtonEnabled(true);
    m_items = new QListWidget(this);
    m_items->setFrameShape(QFrame::NoFrame);
    m_items->setMouseTracking(true);
    m_items->setContextMenuPolicy(Qt::CustomContextMenu);
    m_items->setItemDelegate(new RowDelegate(&m_theme, m_items));
    m_items->setUniformItemSizes(false);

    auto *listBox = new QWidget(this);
    auto *ll = new QVBoxLayout(listBox);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    auto *listHead = new QWidget(listBox);
    auto *lh = new QHBoxLayout(listHead);
    lh->setContentsMargins(8, 2, 6, 0);
    lh->addWidget(listTabsRow);
    lh->addStretch(1);
    lh->addWidget(m_listAction);
    auto *filterWrap = new QWidget(listBox);
    auto *fw = new QHBoxLayout(filterWrap);
    fw->setContentsMargins(8, 4, 8, 4);
    fw->addWidget(m_filter);
    ll->addWidget(listHead);
    ll->addWidget(filterWrap);
    ll->addWidget(m_items, 1);

    // --- Request
    m_method = new QComboBox(this);
    for (const char *m : {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"})
        m_method->addItem(QString::fromLatin1(m));
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(tr("/path or full URL"));
    m_url->setClearButtonEnabled(true);
    m_send = new QPushButton(tr("Send"), this);
    m_send->setDefault(true);
    m_target = new QLabel(this);
    m_target->setObjectName(QStringLiteral("emptyText"));
    m_target->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *reqTabsRow = segments({tr("Headers"), tr("Body")}, m_reqTabs);
    auto *formatBtn = tool(tr("Format the body as JSON"));
    formatBtn->setText(tr("Format"));
    auto *curlBtn = tool(tr("Copy the request as a curl command"));
    curlBtn->setText(tr("cURL"));
    auto *saveBtn = tool(tr("Save this request under a name"));
    saveBtn->setText(tr("Save"));
    m_headers = new QPlainTextEdit(this);
    m_headers->setPlaceholderText(tr("Header-Name: value\nAuthorization: Bearer …"));
    m_headers->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_headers->setFrameShape(QFrame::NoFrame);
    m_body = new QPlainTextEdit(this);
    m_body->setPlaceholderText(tr("Request body (JSON)"));
    m_body->setFrameShape(QFrame::NoFrame);
    m_bodyColours = new JsonHighlighter(m_body->document());
    m_reqStack = new QStackedWidget(this);
    m_reqStack->addWidget(m_headers);
    m_reqStack->addWidget(m_body);

    auto *reqBox = new QWidget(this);
    auto *rl = new QVBoxLayout(reqBox);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    auto *urlRow = new QWidget(reqBox);
    auto *ur = new QHBoxLayout(urlRow);
    ur->setContentsMargins(8, 6, 8, 2);
    ur->setSpacing(4);
    ur->addWidget(m_method);
    ur->addWidget(m_url, 1);
    ur->addWidget(m_send);
    auto *targetRow = new QWidget(reqBox);
    auto *tr_ = new QHBoxLayout(targetRow);
    tr_->setContentsMargins(10, 0, 8, 2);
    tr_->addWidget(m_target);
    auto *reqHead = new QWidget(reqBox);
    auto *rh = new QHBoxLayout(reqHead);
    rh->setContentsMargins(8, 0, 6, 0);
    rh->setSpacing(0);
    rh->addWidget(reqTabsRow);
    rh->addStretch(1);
    rh->addWidget(formatBtn);
    rh->addWidget(curlBtn);
    rh->addWidget(saveBtn);
    rl->addWidget(urlRow);
    rl->addWidget(targetRow);
    rl->addWidget(reqHead);
    rl->addWidget(m_reqStack, 1);

    // --- Response
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::RichText);
    m_status->setWordWrap(true);
    m_status->setOpenExternalLinks(false);
    m_status->setTextInteractionFlags(Qt::TextBrowserInteraction);
    auto *respTabsRow = segments({tr("Body"), tr("Headers")}, m_respTabs);
    m_copyResponse = tool(tr("Copy the response"));
    Icons::bind(m_copyResponse, QStringLiteral(":/new-icons/copy.svg"));
    m_response = new QPlainTextEdit(this);
    m_response->setReadOnly(true);
    m_response->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_response->setFrameShape(QFrame::NoFrame);
    m_response->setPlaceholderText(tr("Send a request to see the response."));
    m_responseColours = new JsonHighlighter(m_response->document());

    auto *respBox = new QWidget(this);
    auto *pl = new QVBoxLayout(respBox);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(0);
    auto *statusWrap = new QWidget(respBox);
    auto *sw = new QHBoxLayout(statusWrap);
    sw->setContentsMargins(10, 6, 8, 2);
    sw->addWidget(m_status);
    auto *respHead = new QWidget(respBox);
    auto *ph = new QHBoxLayout(respHead);
    ph->setContentsMargins(8, 0, 6, 0);
    ph->addWidget(respTabsRow);
    ph->addStretch(1);
    ph->addWidget(m_copyResponse);
    pl->addWidget(statusWrap);
    pl->addWidget(respHead);
    pl->addWidget(m_response, 1);

    auto *split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);
    split->addWidget(listBox);
    split->addWidget(reqBox);
    split->addWidget(respBox);
    split->setSizes({260, 230, 260});
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 1);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(split);

    // --- Wiring
    connect(m_listTabs, &QButtonGroup::idClicked, this, &HttpClientView::showList);
    connect(m_reqTabs, &QButtonGroup::idClicked, m_reqStack, &QStackedWidget::setCurrentIndex);
    connect(m_respTabs, &QButtonGroup::idClicked, this, &HttpClientView::showResponseTab);
    connect(m_filter, &QLineEdit::textChanged, this, &HttpClientView::populate);
    connect(m_listAction, &QToolButton::clicked, this, [this] {
        if (m_list == 0) {
            emit refreshRequested();
        } else if (m_list == 1 && !m_history.isEmpty()) {
            m_history = QJsonArray();
            populate();
            emit stateChanged();
        }
    });
    connect(m_items, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        if (it->data(HeaderRole).toBool())
            return;
        const int ep = it->data(EndpointRole).toInt();
        if (m_list == 0 && ep >= 0 && ep < m_scan.endpoints.size())
            loadEndpoint(m_scan.endpoints.at(ep));
        else if (m_list != 0)
            loadRequest(it->data(PayloadRole).toJsonObject());
    });
    connect(m_items, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        if (!it->data(HeaderRole).toBool() && !m_url->text().contains(QLatin1Char('{')))
            send();
    });
    connect(m_items, &QWidget::customContextMenuRequested, this, &HttpClientView::listMenu);
    connect(m_send, &QPushButton::clicked, this, &HttpClientView::send);
    connect(m_url, &QLineEdit::returnPressed, this, &HttpClientView::send);
    connect(m_url, &QLineEdit::textChanged, this, &HttpClientView::updateTarget);
    connect(formatBtn, &QToolButton::clicked, this, &HttpClientView::formatBody);
    connect(curlBtn, &QToolButton::clicked, this, &HttpClientView::copyCurl);
    connect(saveBtn, &QToolButton::clicked, this, &HttpClientView::saveCurrent);
    connect(m_copyResponse, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(m_response->toPlainText()); });
    connect(m_status, &QLabel::linkActivated, this, [this](const QString &link) {
        if (link == QLatin1String("start"))
            emit startRequested();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &HttpClientView::applyTheme);

    applyTheme();
    showList(0);
    updateTarget();
}

void HttpClientView::applyTheme()
{
    m_theme = Theme::byName(SettingsManager::instance().theme());
    const Theme &t = m_theme;
    setStyleSheet(QStringLiteral("QPlainTextEdit, QListWidget { background: %1; color: %2; selection-background-color: %3; }"
                                 "QPlainTextEdit { padding: 4px; border-top: 1px solid %4; }"
                                 "QListWidget { border-top: 1px solid %4; outline: none; }"
                                 "QToolButton[seg=\"true\"] { background: transparent; border: none; border-bottom: 2px solid transparent; padding: 4px 9px; color: %5; }"
                                 "QToolButton[seg=\"true\"]:checked { background: transparent; color: %6; border-bottom: 2px solid %6; }"
                                 "QToolButton[seg=\"true\"]:hover:!checked { color: %2; }")
                      .arg(t.editorBg.name(), t.editorFg.name(), t.selection.name(), t.border.name(), t.textMuted.name(), t.accent.name()));
    QFont f = SettingsManager::instance().editorFont();
    for (QPlainTextEdit *e : {m_headers, m_body, m_response})
        e->setFont(f);
    static_cast<JsonHighlighter *>(m_bodyColours)->setTheme(t);
    static_cast<JsonHighlighter *>(m_responseColours)->setTheme(t);
    m_send->setIcon(Icons::tinted(m_reply ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/play.svg"), t.onAccent()));
    m_items->viewport()->update();
    showList(m_list); // refreshes the action icon
}

void HttpClientView::setBaseUrl(const QString &base)
{
    QString b = base.trimmed();
    while (b.endsWith(QLatin1Char('/')))
        b.chop(1);
    m_base = b.isEmpty() ? QStringLiteral("http://localhost:8080") : b;
    updateTarget();
}

void HttpClientView::setServerRunning(bool running)
{
    m_running = running;
}

void HttpClientView::setScan(const SpringScan &scan)
{
    m_scan = scan;
    m_scanning = false;
    if (m_list == 0)
        populate();
}

void HttpClientView::setScanning(bool scanning)
{
    m_scanning = scanning;
    if (m_list == 0)
        populate();
}

void HttpClientView::clearForNewProject()
{
    m_scan = SpringScan();
    m_history = QJsonArray();
    m_saved = QJsonArray();
    m_url->clear();
    m_headers->clear();
    m_body->clear();
    m_method->setCurrentIndex(0);
    m_response->clear();
    m_status->clear();
    populate();
}

QJsonObject HttpClientView::state() const
{
    QJsonObject o;
    o.insert(QStringLiteral("history"), m_history);
    o.insert(QStringLiteral("saved"), m_saved);
    const Request r = current();
    QJsonObject draft;
    draft.insert(QStringLiteral("method"), r.method);
    draft.insert(QStringLiteral("url"), r.url);
    draft.insert(QStringLiteral("headers"), r.headers);
    draft.insert(QStringLiteral("body"), r.body);
    o.insert(QStringLiteral("draft"), draft);
    return o;
}

void HttpClientView::setState(const QJsonObject &s)
{
    m_history = s.value(QStringLiteral("history")).toArray();
    m_saved = s.value(QStringLiteral("saved")).toArray();
    const QJsonObject draft = s.value(QStringLiteral("draft")).toObject();
    if (!draft.isEmpty())
        loadRequest(draft);
    showList(m_list);
}

HttpClientView::Request HttpClientView::current() const
{
    Request r;
    r.method = m_method->currentText();
    r.url = m_url->text().trimmed();
    r.headers = m_headers->toPlainText();
    r.body = m_body->toPlainText();
    return r;
}

void HttpClientView::loadRequest(const QJsonObject &o)
{
    const int i = m_method->findText(o.value(QStringLiteral("method")).toString());
    m_method->setCurrentIndex(i < 0 ? 0 : i);
    m_url->setText(o.value(QStringLiteral("url")).toString());
    m_headers->setPlainText(o.value(QStringLiteral("headers")).toString());
    m_body->setPlainText(o.value(QStringLiteral("body")).toString());
}

void HttpClientView::loadEndpoint(const SpringEndpoint &e)
{
    const int i = m_method->findText(e.method == QLatin1String("ANY") ? QStringLiteral("GET") : e.method);
    m_method->setCurrentIndex(i < 0 ? 0 : i);
    m_url->setText(e.urlTemplate());
    QStringList headers;
    const QString bodyType = e.bodyType();
    if (!bodyType.isEmpty())
        headers << QStringLiteral("Content-Type: application/json");
    for (const SpringParam &p : e.params)
        if (p.kind == SpringParam::Header && (p.required || !p.defaultValue.isEmpty()))
            headers << p.name + QStringLiteral(": ") + p.defaultValue;
    m_headers->setPlainText(headers.join(QLatin1Char('\n')));
    m_body->setPlainText(bodyType.isEmpty() ? QString() : m_scan.bodySkeleton(bodyType));
    // Show what the user most likely has to fill in.
    m_reqStack->setCurrentIndex(!bodyType.isEmpty() ? 1 : 0);
    m_reqTabs->button(!bodyType.isEmpty() ? 1 : 0)->setChecked(true);
    m_url->setFocus();
}

void HttpClientView::showList(int tab)
{
    m_list = tab;
    if (QAbstractButton *b = m_listTabs->button(tab))
        b->setChecked(true);
    m_listTabs->button(1)->setText(m_history.isEmpty() ? tr("History") : tr("History %1").arg(m_history.size()));
    m_listTabs->button(2)->setText(m_saved.isEmpty() ? tr("Saved") : tr("Saved %1").arg(m_saved.size()));
    m_listAction->setVisible(tab != 2);
    if (tab == 0) {
        m_listAction->setIcon(Icons::tinted(QStringLiteral(":/new-icons/refresh-cw.svg"), m_theme.editorFg));
        m_listAction->setToolTip(tr("Scan the controllers again"));
    } else if (tab == 1) {
        m_listAction->setIcon(Icons::tinted(QStringLiteral(":/new-icons/trash-2.svg"), m_theme.editorFg));
        m_listAction->setToolTip(tr("Clear the history"));
    }
    populate();
}

void HttpClientView::populate()
{
    m_listTabs->button(1)->setText(m_history.isEmpty() ? tr("History") : tr("History %1").arg(m_history.size()));
    m_listTabs->button(2)->setText(m_saved.isEmpty() ? tr("Saved") : tr("Saved %1").arg(m_saved.size()));
    const QStringList words = m_filter->text().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    auto matches = [&words](const QString &hay) {
        const QString l = hay.toLower();
        for (const QString &w : words)
            if (!l.contains(w))
                return false;
        return true;
    };
    auto note = [this](const QString &text) {
        auto *it = new QListWidgetItem(text, m_items);
        it->setFlags(Qt::NoItemFlags);
        it->setData(HeaderRole, true);
        it->setData(Qt::ForegroundRole, m_theme.textMuted);
    };

    m_items->setUpdatesEnabled(false);
    m_items->clear();
    if (m_list == 0) {
        QString lastFile;
        int shown = 0;
        for (int i = 0; i < m_scan.endpoints.size(); ++i) {
            const SpringEndpoint &e = m_scan.endpoints.at(i);
            if (!matches(e.title() + QLatin1Char(' ') + e.controller + QLatin1Char(' ') + e.handler))
                continue;
            if (e.file != lastFile) {
                lastFile = e.file;
                auto *h = new QListWidgetItem(e.controller, m_items);
                h->setFlags(Qt::NoItemFlags);
                h->setData(HeaderRole, true);
            }
            auto *it = new QListWidgetItem(e.path, m_items);
            it->setData(MethodRole, e.method == QLatin1String("ANY") ? QStringLiteral("ANY") : e.method);
            it->setData(DetailRole, e.handler);
            it->setData(EndpointRole, i);
            it->setToolTip(tr("%1\n%2:%3").arg(e.title(), QFileInfo(e.file).fileName()).arg(e.line));
            ++shown;
        }
        if (shown == 0)
            note(m_scanning ? tr("Scanning controllers…")
                            : m_scan.endpoints.isEmpty() ? tr("No endpoints found. Mapped functions of @RestController classes show up here.")
                                                         : tr("No endpoint matches the filter."));
    } else {
        const QJsonArray &src = m_list == 1 ? m_history : m_saved;
        int shown = 0;
        for (const QJsonValue &v : src) {
            const QJsonObject o = v.toObject();
            const QString url = o.value(QStringLiteral("url")).toString();
            const QString name = o.value(QStringLiteral("name")).toString();
            if (!matches(name + QLatin1Char(' ') + url + QLatin1Char(' ') + o.value(QStringLiteral("method")).toString()))
                continue;
            auto *it = new QListWidgetItem(m_list == 2 && !name.isEmpty() ? name : url, m_items);
            it->setData(MethodRole, o.value(QStringLiteral("method")).toString());
            it->setData(PayloadRole, o);
            it->setData(EndpointRole, -1);
            if (m_list == 1) {
                const int status = o.value(QStringLiteral("status")).toInt();
                it->setData(StatusRole, status);
                it->setData(DetailRole, status > 0 ? QStringLiteral("%1 · %2 ms").arg(status).arg(o.value(QStringLiteral("ms")).toInt())
                                                   : tr("failed"));
                it->setToolTip(QDateTime::fromString(o.value(QStringLiteral("time")).toString(), Qt::ISODate).toLocalTime().toString());
            } else {
                it->setData(DetailRole, name.isEmpty() ? QString() : url);
                it->setToolTip(url);
            }
            ++shown;
        }
        if (shown == 0)
            note(!src.isEmpty() ? tr("Nothing matches the filter.")
                                : m_list == 1 ? tr("No requests sent yet.") : tr("Nothing saved. Use Save in the request."));
    }
    m_items->setUpdatesEnabled(true);
}

QString HttpClientView::resolvedUrl() const
{
    const QString u = m_url->text().trimmed();
    if (u.isEmpty() || u.startsWith(QLatin1Char('/')) || u.startsWith(QLatin1Char('?')))
        return m_base + u;
    if (u.contains(QStringLiteral("://")))
        return u;
    if (u.startsWith(QLatin1Char(':')))
        return QStringLiteral("http://localhost") + u;
    return QStringLiteral("http://") + u;
}

void HttpClientView::updateTarget()
{
    const QString u = m_url->text().trimmed();
    const bool relative = u.isEmpty() || u.startsWith(QLatin1Char('/')) || u.startsWith(QLatin1Char('?'));
    m_target->setVisible(relative);
    if (relative)
        m_target->setText(tr("→ %1").arg(resolvedUrl()));
}

void HttpClientView::send()
{
    if (m_reply) { // the button reads "Cancel" while a request is in flight
        m_reply->abort();
        return;
    }
    static const QRegularExpression placeholder(QStringLiteral(R"(\{\w+\})"));
    const Request r = current();
    const QString url = resolvedUrl();
    const auto ph = placeholder.match(url);
    if (ph.hasMatch()) {
        showResponse(tr("Fill in %1 first").arg(ph.captured().toHtmlEscaped()), m_theme.warning, QString(), QString());
        m_url->setFocus();
        return;
    }
    const QUrl qurl(url, QUrl::TolerantMode);
    if (!qurl.isValid() || qurl.host().isEmpty()) {
        showResponse(tr("This is not a valid URL").toHtmlEscaped(), m_theme.danger, QString(), QString());
        return;
    }

    QNetworkRequest rq(qurl);
    rq.setTransferTimeout(30000);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("QODE"));
    bool hasType = false;
    for (const QString &line : r.headers.split(QLatin1Char('\n'))) {
        const QString l = line.trimmed();
        const int colon = l.indexOf(QLatin1Char(':'));
        if (l.isEmpty() || l.startsWith(QLatin1Char('#')) || colon <= 0)
            continue;
        const QString name = l.left(colon).trimmed();
        if (name.compare(QLatin1String("content-type"), Qt::CaseInsensitive) == 0)
            hasType = true;
        rq.setRawHeader(name.toUtf8(), l.mid(colon + 1).trimmed().toUtf8());
    }
    const QByteArray body = r.method == QLatin1String("HEAD") ? QByteArray() : r.body.toUtf8();
    if (!body.trimmed().isEmpty() && !hasType) {
        const char first = body.trimmed().at(0);
        rq.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(first == '{' || first == '[' ? "application/json" : "text/plain"));
    }

    m_clock.start();
    m_reply = m_net.sendCustomRequest(rq, r.method.toLatin1(), body);
    m_send->setText(tr("Cancel"));
    m_send->setIcon(Icons::tinted(QStringLiteral(":/new-icons/square.svg"), m_theme.editorFg));
    m_status->setText(tr("<span style=\"color:%1\">Waiting for %2 …</span>").arg(m_theme.textMuted.name(), url.toHtmlEscaped()));
    connect(m_reply, &QNetworkReply::finished, this, &HttpClientView::finished);
}

void HttpClientView::finished()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    reply->deleteLater();
    m_send->setText(tr("Send"));
    m_send->setIcon(Icons::tinted(QStringLiteral(":/new-icons/play.svg"), m_theme.editorFg));
    const qint64 ms = m_clock.elapsed();
    const Request r = current();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        QString why;
        switch (reply->error()) {
        case QNetworkReply::ConnectionRefusedError:
            why = tr("Connection refused: nothing listens on %1:%2.").arg(reply->url().host()).arg(reply->url().port(80));
            if (!m_running)
                why += tr(" The application is not running — <a href=\"start\" style=\"color:%1\">start it</a>.").arg(m_theme.accent.name());
            break;
        case QNetworkReply::HostNotFoundError:
            why = tr("Host not found: %1").arg(reply->url().host().toHtmlEscaped());
            break;
        case QNetworkReply::TimeoutError:
            why = tr("No answer within 30 seconds.");
            break;
        case QNetworkReply::OperationCanceledError:
            why = tr("Cancelled.");
            break;
        default:
            why = reply->errorString().toHtmlEscaped();
        }
        showResponse(why, m_theme.danger, QString(), QString());
        if (reply->error() != QNetworkReply::OperationCanceledError)
            remember(r, 0, ms);
        return;
    }

    const QByteArray raw = reply->readAll();
    const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    QString text;
    const bool binary = type.startsWith(QLatin1String("image/")) || type.startsWith(QLatin1String("video/")) || type.startsWith(QLatin1String("audio/"))
                        || type.contains(QLatin1String("octet-stream")) || type.contains(QLatin1String("pdf")) || type.contains(QLatin1String("zip"));
    if (binary) {
        text = tr("(binary data, %1)").arg(sizeText(raw.size()));
    } else {
        text = QString::fromUtf8(raw.left(2 * 1024 * 1024));
        const QByteArray trimmed = raw.trimmed();
        if (type.contains(QLatin1String("json")) || (!trimmed.isEmpty() && (trimmed.at(0) == '{' || trimmed.at(0) == '['))) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
            if (err.error == QJsonParseError::NoError && raw.size() <= 2 * 1024 * 1024)
                text = QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
        }
        if (raw.size() > 2 * 1024 * 1024)
            text += tr("\n… (truncated, %1 in total)").arg(sizeText(raw.size()));
    }
    QStringList headers;
    for (const auto &pair : reply->rawHeaderPairs())
        headers << QString::fromLatin1(pair.first) + QStringLiteral(": ") + QString::fromLatin1(pair.second);

    const QColor colour = status < 300 ? m_theme.success : status < 400 ? m_theme.warning : m_theme.danger;
    const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    showResponse(QStringLiteral("<b>%1 %2</b> <span style=\"color:%3\">· %4 ms · %5</span>")
                     .arg(status).arg(reason.toHtmlEscaped(), m_theme.textMuted.name()).arg(ms).arg(sizeText(raw.size())),
                 colour, text, headers.join(QLatin1Char('\n')));
    remember(r, status, ms);
}

// `status` is rich text (callers escape what they insert).
void HttpClientView::showResponse(const QString &status, const QColor &colour, const QString &body, const QString &headers)
{
    m_respBody = body;
    m_respHeaders = headers;
    m_status->setText(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(colour.name(), status));
    showResponseTab(m_respTab);
}

void HttpClientView::showResponseTab(int tab)
{
    m_respTab = tab;
    m_respTabs->button(tab)->setChecked(true);
    m_response->setPlainText(tab == 0 ? m_respBody : m_respHeaders);
}

void HttpClientView::remember(const Request &r, int status, qint64 ms)
{
    QJsonObject o;
    o.insert(QStringLiteral("method"), r.method);
    o.insert(QStringLiteral("url"), r.url);
    o.insert(QStringLiteral("headers"), r.headers);
    o.insert(QStringLiteral("body"), r.body);
    o.insert(QStringLiteral("status"), status);
    o.insert(QStringLiteral("ms"), int(ms));
    o.insert(QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    // The same request again moves to the top instead of piling up.
    QJsonArray next;
    next.append(o);
    for (const QJsonValue &v : std::as_const(m_history)) {
        const QJsonObject h = v.toObject();
        const bool same = h.value(QStringLiteral("method")) == o.value(QStringLiteral("method")) && h.value(QStringLiteral("url")) == o.value(QStringLiteral("url"))
                          && h.value(QStringLiteral("body")) == o.value(QStringLiteral("body")) && h.value(QStringLiteral("headers")) == o.value(QStringLiteral("headers"));
        if (!same && next.size() < kMaxHistory)
            next.append(v);
    }
    m_history = next;
    if (m_list == 1)
        populate();
    else
        showList(m_list);
    emit stateChanged();
}

void HttpClientView::saveCurrent()
{
    const Request r = current();
    if (r.url.isEmpty()) {
        m_url->setFocus();
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Request"), tr("Name:"), QLineEdit::Normal, r.method + QLatin1Char(' ') + r.url, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    QJsonObject o;
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("method"), r.method);
    o.insert(QStringLiteral("url"), r.url);
    o.insert(QStringLiteral("headers"), r.headers);
    o.insert(QStringLiteral("body"), r.body);
    m_saved.prepend(o);
    showList(2);
    emit stateChanged();
}

void HttpClientView::listMenu(const QPoint &pos)
{
    QListWidgetItem *it = m_items->itemAt(pos);
    if (!it || it->data(HeaderRole).toBool())
        return;
    QMenu menu(this);
    const QJsonObject payload = it->data(PayloadRole).toJsonObject();
    if (m_list == 0) {
        const int ep = it->data(EndpointRole).toInt();
        if (ep < 0 || ep >= m_scan.endpoints.size())
            return;
        const SpringEndpoint e = m_scan.endpoints.at(ep);
        menu.addAction(tr("Go to Source"), this, [this, e] { emit locationRequested(e.file, e.line, 1); });
        menu.addAction(tr("Copy Path"), this, [e] { QApplication::clipboard()->setText(e.path); });
        menu.addAction(tr("Send"), this, [this, e] {
            loadEndpoint(e);
            if (!m_url->text().contains(QLatin1Char('{')))
                send();
        });
    } else {
        QJsonArray &src = m_list == 1 ? m_history : m_saved;
        auto indexOfPayload = [&src, &payload] {
            for (int i = 0; i < src.size(); ++i)
                if (src.at(i).toObject() == payload)
                    return i;
            return -1;
        };
        menu.addAction(tr("Load"), this, [this, payload] { loadRequest(payload); });
        if (m_list == 1)
            menu.addAction(tr("Save…"), this, [this, payload] {
                loadRequest(payload);
                saveCurrent();
            });
        if (m_list == 2)
            menu.addAction(tr("Rename…"), this, [this, indexOfPayload, &src] {
                const int i = indexOfPayload();
                if (i < 0)
                    return;
                QJsonObject o = src.at(i).toObject();
                bool ok = false;
                const QString name = QInputDialog::getText(this, tr("Rename Request"), tr("Name:"), QLineEdit::Normal, o.value(QStringLiteral("name")).toString(), &ok).trimmed();
                if (!ok || name.isEmpty())
                    return;
                o.insert(QStringLiteral("name"), name);
                src.replace(i, o);
                populate();
                emit stateChanged();
            });
        menu.addAction(tr("Delete"), this, [this, indexOfPayload, &src] {
            const int i = indexOfPayload();
            if (i < 0)
                return;
            src.removeAt(i);
            showList(m_list);
            emit stateChanged();
        });
    }
    menu.exec(m_items->viewport()->mapToGlobal(pos));
}

QString HttpClientView::curl(const Request &r) const
{
    QStringList parts{QStringLiteral("curl")};
    if (r.method != QLatin1String("GET"))
        parts << QStringLiteral("-X") << r.method;
    parts << shellQuote(resolvedUrl());
    for (const QString &line : r.headers.split(QLatin1Char('\n'))) {
        const QString l = line.trimmed();
        if (!l.isEmpty() && !l.startsWith(QLatin1Char('#')) && l.contains(QLatin1Char(':')))
            parts << QStringLiteral("-H") << shellQuote(l);
    }
    if (!r.body.trimmed().isEmpty())
        parts << QStringLiteral("-d") << shellQuote(r.body);
    return parts.join(QLatin1Char(' '));
}

void HttpClientView::copyCurl()
{
    QApplication::clipboard()->setText(curl(current()));
    m_target->setVisible(true);
    m_target->setText(tr("curl command copied"));
}

void HttpClientView::formatBody()
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(m_body->toPlainText().toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        m_target->setVisible(true);
        m_target->setText(tr("Not valid JSON: %1").arg(err.errorString()));
        return;
    }
    m_body->setPlainText(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed());
    updateTarget();
}
