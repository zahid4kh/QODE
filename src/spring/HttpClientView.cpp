#include "spring/HttpClientView.h"

#include "http/HttpResponseView.h"
#include "http/HttpRunner.h"
#include "http/HttpUi.h"
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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

using namespace HttpUi;

constexpr int kMaxHistory = 80;

} // namespace

HttpClientView::HttpClientView(QWidget *parent) : QWidget(parent), m_runner(new HttpRunner(this))
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
    m_items->setItemDelegate(new HttpRowDelegate(&m_theme, m_items));
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
    m_bodyColours = new HttpJsonHighlighter(m_body->document());
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
    m_response = new HttpResponseView(this);

    auto *split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);
    split->addWidget(listBox);
    split->addWidget(reqBox);
    split->addWidget(m_response);
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
    connect(m_response, &HttpResponseView::openTextRequested, this, &HttpClientView::openTextRequested);
    connect(m_response, &HttpResponseView::linkActivated, this, [this](const QString &link) {
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
    setStyleSheet(HttpUi::styleSheet(m_theme));
    const QFont f = SettingsManager::instance().editorFont();
    for (QPlainTextEdit *e : {m_headers, m_body})
        e->setFont(f);
    m_bodyColours->setTheme(m_theme);
    setSending(m_runner->busy());
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

void HttpClientView::setScanTexts(const QString &scanning, const QString &empty, const QString &rescanTip)
{
    m_scanningText = scanning;
    m_emptyText = empty;
    m_rescanTip = rescanTip;
    showList(m_list);
}

void HttpClientView::openEndpoint(const SpringEndpoint &e, bool sendNow)
{
    loadEndpoint(e);
    if (sendNow && !m_url->text().contains(QLatin1Char('{')))
        send();
}

QString HttpClientView::endpointAsHttp(const SpringEndpoint &e) const
{
    return asHttp(requestFor(e), e.title());
}

QString HttpClientView::endpointAsCurl(const SpringEndpoint &e) const
{
    auto quote = [](QString v) { return QLatin1Char('\'') + v.replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\''); };
    const Request r = requestFor(e);
    QStringList parts{QStringLiteral("curl")};
    if (r.method != QLatin1String("GET"))
        parts << QStringLiteral("-X") << r.method;
    parts << quote(absoluteUrl(r.url));
    for (const QString &h : r.headers.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        parts << QStringLiteral("-H") << quote(h.trimmed());
    if (!r.body.isEmpty())
        parts << QStringLiteral("--data") << quote(r.body);
    return parts.join(QLatin1Char(' '));
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

HttpClientView::Request HttpClientView::requestFor(const SpringEndpoint &e) const
{
    Request r;
    r.method = e.method == QLatin1String("ANY") ? QStringLiteral("GET") : e.method;
    r.url = e.urlTemplate();
    QStringList headers;
    const QString bodyType = e.bodyType();
    if (!bodyType.isEmpty())
        headers << QStringLiteral("Content-Type: application/json");
    for (const SpringParam &p : e.params)
        if (p.kind == SpringParam::Header && (p.required || !p.defaultValue.isEmpty()))
            headers << p.name + QStringLiteral(": ") + p.defaultValue;
    r.headers = headers.join(QLatin1Char('\n'));
    r.body = bodyType.isEmpty() ? QString() : m_scan.bodySkeleton(bodyType);
    return r;
}

void HttpClientView::loadEndpoint(const SpringEndpoint &e)
{
    const Request r = requestFor(e);
    const int i = m_method->findText(r.method);
    m_method->setCurrentIndex(i < 0 ? 0 : i);
    m_url->setText(r.url);
    m_headers->setPlainText(r.headers);
    m_body->setPlainText(r.body);
    const QString bodyType = e.bodyType();
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
        m_listAction->setToolTip(m_rescanTip.isEmpty() ? tr("Scan the controllers again") : m_rescanTip);
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
        QString lastGroup;
        int shown = 0;
        for (int i = 0; i < m_scan.endpoints.size(); ++i) {
            const SpringEndpoint &e = m_scan.endpoints.at(i);
            if (!matches(e.title() + QLatin1Char(' ') + e.controller + QLatin1Char(' ') + e.handler))
                continue;
            // A controller class, or a Ktor routing function (several can share a file).
            if (e.file + QLatin1Char('|') + e.controller != lastGroup) {
                lastGroup = e.file + QLatin1Char('|') + e.controller;
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
            note(m_scanning ? (m_scanningText.isEmpty() ? tr("Scanning controllers…") : m_scanningText)
                 : m_scan.endpoints.isEmpty() ? (m_emptyText.isEmpty() ? tr("No endpoints found. Mapped functions of @RestController classes show up here.") : m_emptyText)
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
    return absoluteUrl(m_url->text());
}

QString HttpClientView::absoluteUrl(const QString &url) const
{
    const QString u = url.trimmed();
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

bool HttpClientView::requestData(const Request &r, HttpRequestData *out, QString *error) const
{
    static const QRegularExpression placeholder(QStringLiteral(R"(\{\w+\})"));
    const QString url = absoluteUrl(r.url);
    if (const auto ph = placeholder.match(url); ph.hasMatch()) {
        *error = tr("Fill in %1 first").arg(ph.captured());
        return false;
    }
    const QUrl qurl(url, QUrl::TolerantMode);
    if (!qurl.isValid() || qurl.host().isEmpty()) {
        *error = tr("This is not a valid URL");
        return false;
    }
    out->method = r.method;
    out->url = qurl;
    out->timeoutMs = 30000;
    out->headers.clear();
    bool hasType = false;
    for (const QString &line : r.headers.split(QLatin1Char('\n'))) {
        const QString l = line.trimmed();
        const int colon = l.indexOf(QLatin1Char(':'));
        if (l.isEmpty() || l.startsWith(QLatin1Char('#')) || colon <= 0)
            continue;
        const QString name = l.left(colon).trimmed();
        if (name.compare(QLatin1String("content-type"), Qt::CaseInsensitive) == 0)
            hasType = true;
        out->headers.append({name.toUtf8(), l.mid(colon + 1).trimmed().toUtf8()});
    }
    out->body = r.method == QLatin1String("HEAD") ? QByteArray() : r.body.toUtf8();
    const QByteArray trimmed = out->body.trimmed();
    if (!trimmed.isEmpty() && !hasType)
        out->headers.append({QByteArrayLiteral("Content-Type"),
                             trimmed.startsWith('{') || trimmed.startsWith('[') ? QByteArrayLiteral("application/json") : QByteArrayLiteral("text/plain")});
    return true;
}

void HttpClientView::send()
{
    if (m_runner->busy()) { // the button reads "Cancel" while a request is in flight
        m_runner->abort();
        return;
    }
    const Request r = current();
    HttpRequestData data;
    QString error;
    if (!requestData(r, &data, &error)) {
        m_response->showMessage(error.toHtmlEscaped(), m_theme.warning);
        m_url->setFocus();
        return;
    }
    setSending(true);
    m_response->showBusy(tr("Waiting for %1 …").arg(data.url.toString()));
    m_runner->send(data, HttpResult(), [this, r](const HttpResult &result) { finished(result, r); });
}

void HttpClientView::setSending(bool sending)
{
    m_send->setText(sending ? tr("Cancel") : tr("Send"));
    m_send->setIcon(Icons::tinted(sending ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/play.svg"), m_theme.onAccent()));
}

void HttpClientView::finished(const HttpResult &result, const Request &r)
{
    setSending(false);
    if (result.ok()) {
        m_response->showResult(result);
        remember(r, result.status, result.ms);
        return;
    }
    QString why = HttpRunner::describeError(result);
    if (result.error == QNetworkReply::ConnectionRefusedError && !m_running)
        why += tr(" The application is not running — <a href=\"start\" style=\"color:%1\">start it</a>.").arg(m_theme.accent.name());
    m_response->showMessage(why, m_theme.danger);
    if (result.error != QNetworkReply::OperationCanceledError)
        remember(r, 0, result.ms);
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
        menu.addAction(tr("Copy as .http Request"), this, [this, e] { QApplication::clipboard()->setText(asHttp(requestFor(e), e.title())); });
    } else {
        QJsonArray &src = m_list == 1 ? m_history : m_saved;
        auto indexOfPayload = [&src, &payload] {
            for (int i = 0; i < src.size(); ++i)
                if (src.at(i).toObject() == payload)
                    return i;
            return -1;
        };
        menu.addAction(tr("Load"), this, [this, payload] { loadRequest(payload); });
        menu.addAction(tr("Copy as .http Request"), this, [this, payload] {
            Request r;
            r.method = payload.value(QStringLiteral("method")).toString();
            r.url = payload.value(QStringLiteral("url")).toString();
            r.headers = payload.value(QStringLiteral("headers")).toString();
            r.body = payload.value(QStringLiteral("body")).toString();
            QApplication::clipboard()->setText(asHttp(r, payload.value(QStringLiteral("name")).toString()));
        });
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

void HttpClientView::copyCurl()
{
    HttpRequestData data;
    QString error;
    m_target->setVisible(true);
    if (!requestData(current(), &data, &error)) {
        m_target->setText(error);
        return;
    }
    QApplication::clipboard()->setText(HttpUi::curl(data));
    m_target->setText(tr("curl command copied"));
}

QString HttpClientView::asHttp(const Request &r, const QString &name) const
{
    // Path variables become .http variables: {id} -> {{id}}.
    static const QRegularExpression pathVar(QStringLiteral(R"((?<!\{)\{(\w+)\}(?!\}))"));
    QString url = absoluteUrl(r.url);
    url.replace(pathVar, QStringLiteral("{{\\1}}"));
    return HttpFile::format(name, r.method, url, r.headers, r.body);
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
