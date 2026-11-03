#include "ktor/KtorRoutesView.h"

#include "http/HttpUi.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSet>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {
using namespace HttpUi;

QString paramList(const KtorRoute &r, SpringParam::Kind kind)
{
    QStringList out;
    for (const SpringParam &p : r.ep.params)
        if (p.kind == kind)
            out << (p.defaultValue.isEmpty() ? p.name : p.name + QLatin1Char('=') + p.defaultValue);
    return out.join(QStringLiteral(", "));
}
} // namespace

KtorRoutesView::KtorRoutesView(QWidget *parent) : QWidget(parent)
{
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter routes"));
    m_filter->setClearButtonEnabled(true);
    m_kind = new QComboBox(this);
    m_kind->addItem(tr("All kinds"), -1);
    m_kind->addItem(tr("HTTP"), int(KtorRoute::Http));
    m_kind->addItem(tr("WebSocket"), int(KtorRoute::WebSocket));
    m_kind->addItem(tr("Server-sent events"), int(KtorRoute::Sse));
    m_kind->addItem(tr("Static content"), int(KtorRoute::Static));
    m_kind->addItem(tr("API docs"), int(KtorRoute::Docs));
    m_refresh = new QToolButton(this);
    m_refresh->setAutoRaise(true);
    m_refresh->setToolTip(tr("Scan the routing code again"));

    auto *tools = new QWidget(this);
    auto *tl = new QHBoxLayout(tools);
    tl->setContentsMargins(8, 4, 6, 4);
    tl->setSpacing(4);
    tl->addWidget(m_filter, 1);
    tl->addWidget(m_kind);
    tl->addWidget(m_refresh);

    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setMouseTracking(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setItemDelegate(new HttpRowDelegate(&m_theme, m_list));
    m_list->setUniformItemSizes(false);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("emptyText"));
    m_summary->setContentsMargins(10, 3, 10, 4);
    m_summary->setStyleSheet(QStringLiteral("QLabel { border-top: 1px solid palette(shadow); }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tools);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_summary);

    connect(m_filter, &QLineEdit::textChanged, this, &KtorRoutesView::populate);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &KtorRoutesView::populate);
    connect(m_refresh, &QToolButton::clicked, this, &KtorRoutesView::refreshRequested);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &KtorRoutesView::showMenu);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        const int i = it->data(EndpointRole).toInt();
        if (!it->data(HeaderRole).toBool() && i >= 0 && i < m_scan.routes.size() && !m_scan.routes.at(i).ep.file.isEmpty())
            emit locationRequested(m_scan.routes.at(i).ep.file, m_scan.routes.at(i).ep.line, 1);
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &KtorRoutesView::applyTheme);
    applyTheme();
}

void KtorRoutesView::applyTheme()
{
    m_theme = Theme::byName(SettingsManager::instance().theme());
    m_refresh->setIcon(Icons::tinted(QStringLiteral(":/new-icons/refresh-cw.svg"), m_theme.editorFg));
    m_list->viewport()->update();
}

void KtorRoutesView::setScan(const KtorScan &scan)
{
    m_scan = scan;
    populate();
}

void KtorRoutesView::setOrigin(const QString &text, const QString &tooltip)
{
    m_origin = text;
    m_summary->setToolTip(tooltip);
    populate();
}

void KtorRoutesView::setServer(const QString &baseUrl, bool running)
{
    m_base = baseUrl;
    m_running = running;
}

void KtorRoutesView::setFormatters(std::function<QString(const SpringEndpoint &)> asHttp, std::function<QString(const SpringEndpoint &)> asCurl)
{
    m_asHttp = std::move(asHttp);
    m_asCurl = std::move(asCurl);
}

QString KtorRoutesView::urlOf(const KtorRoute &r) const
{
    QString base = m_base;
    if (r.kind == KtorRoute::WebSocket)
        base.replace(QStringLiteral("http"), QStringLiteral("ws"));
    return base + r.ep.urlTemplate();
}

void KtorRoutesView::populate()
{
    const QStringList words = m_filter->text().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int kind = m_kind->currentData().toInt();
    m_list->setUpdatesEnabled(false);
    m_list->clear();
    QString lastGroup;
    int shown = 0;
    QSet<QString> files;
    int unmounted = 0, notServed = 0;
    for (int i = 0; i < m_scan.routes.size(); ++i) {
        const KtorRoute &r = m_scan.routes.at(i);
        if (!r.ep.file.isEmpty())
            files.insert(r.ep.file);
        unmounted += r.mounted || m_scan.live ? 0 : 1;
        notServed += r.notServed ? 1 : 0;
        if (kind >= 0 && int(r.kind) != kind)
            continue;
        const QString hay = (r.ep.title() + QLatin1Char(' ') + r.ep.controller + QLatin1Char(' ') + r.ep.handler + QLatin1Char(' ') + r.resource).toLower();
        bool ok = true;
        for (const QString &w : words)
            ok = ok && hay.contains(w);
        if (!ok)
            continue;
        const QString group = r.ep.file + QLatin1Char('|') + r.ep.controller;
        if (group != lastGroup) {
            lastGroup = group;
            auto *h = new QListWidgetItem(r.ep.file.isEmpty() ? r.ep.controller : QStringLiteral("%1 · %2").arg(r.ep.controller, QFileInfo(r.ep.file).fileName()),
                                          m_list);
            h->setFlags(Qt::NoItemFlags);
            h->setData(HeaderRole, true);
        }
        auto *it = new QListWidgetItem(r.ep.path, m_list);
        it->setData(MethodRole, r.ep.method);
        it->setData(DetailRole, r.ep.handler);
        it->setData(EndpointRole, i);
        QStringList tip{r.ep.title()};
        if (!r.ep.file.isEmpty())
            tip << QStringLiteral("%1:%2").arg(QFileInfo(r.ep.file).fileName()).arg(r.ep.line);
        if (r.kind != KtorRoute::Http)
            tip << r.kindName();
        if (!r.resource.isEmpty())
            tip << tr("Resource: %1").arg(r.resource);
        if (const QString q = paramList(r, SpringParam::Query); !q.isEmpty())
            tip << tr("Query: %1").arg(q);
        if (const QString h = paramList(r, SpringParam::Header); !h.isEmpty())
            tip << tr("Headers: %1").arg(h);
        if (const QString b = r.ep.bodyType(); !b.isEmpty())
            tip << tr("Body: %1").arg(b);
        if (!r.auth.isEmpty())
            tip << tr("Authentication: %1").arg(r.auth.join(QStringLiteral(", ")));
        if (r.notServed)
            tip << tr("Declared in the code, but the running server does not serve it (a routing function nobody calls, "
                      "a branch not taken, a path built differently).");
        else if (r.runtimeOnly)
            tip << tr("Served by the running server; the sources do not spell it out (a loop, a computed path or a plugin).");
        else if (!r.mounted && !m_scan.live)
            tip << tr("Declared in a Route function that no scanned code calls: the path is relative.");
        if (!r.ep.file.isEmpty())
            tip << tr("Double-click to go to the source");
        it->setToolTip(tip.join(QLatin1Char('\n')));
        ++shown;
    }
    if (shown == 0) {
        auto *it = new QListWidgetItem(m_scan.routes.isEmpty() ? tr("No routes found. get(\"/\") { }, route(\"/api\") { }, webSocket and type-safe "
                                                                    "get<Resource> { } in the Kotlin sources show up here.")
                                                               : tr("No route matches the filter."),
                                       m_list);
        it->setFlags(Qt::NoItemFlags);
        it->setData(HeaderRole, true);
        it->setData(Qt::ForegroundRole, m_theme.textMuted);
    }
    m_list->setUpdatesEnabled(true);
    QString summary = tr("%n route(s)", nullptr, int(m_scan.routes.size())) + QStringLiteral(" · ") + tr("%n file(s)", nullptr, int(files.size()));
    if (shown != m_scan.routes.size() && !m_scan.routes.isEmpty())
        summary = tr("%1 of %2").arg(shown).arg(summary);
    if (unmounted > 0)
        summary += QStringLiteral(" · ") + tr("%n not mounted", nullptr, unmounted);
    if (notServed > 0)
        summary += QStringLiteral(" · ") + tr("%n not served", nullptr, notServed);
    if (!m_origin.isEmpty())
        summary += QStringLiteral(" · ") + m_origin;
    m_summary->setText(summary);
}

void KtorRoutesView::showMenu(const QPoint &pos)
{
    QListWidgetItem *it = m_list->itemAt(pos);
    if (!it || it->data(HeaderRole).toBool())
        return;
    const int i = it->data(EndpointRole).toInt();
    if (i < 0 || i >= m_scan.routes.size())
        return;
    const KtorRoute r = m_scan.routes.at(i);
    QMenu menu(this);
    menu.addAction(tr("Go to Source"), this, [this, r] { emit locationRequested(r.ep.file, r.ep.line, 1); })->setEnabled(!r.ep.file.isEmpty());
    if (r.kind == KtorRoute::Http) {
        menu.addAction(tr("Open in Requests"), this, [this, r] { emit tryRequested(r.ep, false); });
        QAction *send = menu.addAction(tr("Send"), this, [this, r] { emit tryRequested(r.ep, true); });
        send->setEnabled(m_running);
        if (!m_running)
            send->setToolTip(tr("Start the server first"));
    }
    const bool browsable = (r.kind == KtorRoute::Static || r.kind == KtorRoute::Docs || r.ep.method == QLatin1String("GET")) && !r.ep.path.contains(QLatin1Char('{'));
    if (browsable) {
        QAction *open = menu.addAction(tr("Open in Browser"), this, [this, r] { QDesktopServices::openUrl(QUrl(urlOf(r))); });
        open->setEnabled(m_running);
    }
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [r] { QApplication::clipboard()->setText(r.ep.path); });
    menu.addAction(tr("Copy URL"), this, [this, r] { QApplication::clipboard()->setText(urlOf(r)); });
    if (r.kind == KtorRoute::Http && m_asHttp)
        menu.addAction(tr("Copy as .http Request"), this, [this, r] { QApplication::clipboard()->setText(m_asHttp(r.ep)); });
    if (r.kind == KtorRoute::Http && m_asCurl)
        menu.addAction(tr("Copy as cURL"), this, [this, r] { QApplication::clipboard()->setText(m_asCurl(r.ep)); });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}
