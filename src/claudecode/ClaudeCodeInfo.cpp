#include "ClaudeCodeInfo.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QButtonGroup>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace claudecode {

namespace {
Theme theme()
{
    return Theme::byName(SettingsManager::instance().theme()).agentView();
}

QString str(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

QLabel *label(const QString &text, QWidget *parent, const char *name = nullptr, bool wrap = false)
{
    auto *l = new QLabel(text, parent);
    l->setTextFormat(Qt::PlainText);
    l->setWordWrap(wrap);
    if (name)
        l->setObjectName(QLatin1String(name));
    return l;
}

QString capitalized(QString s)
{
    s.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!s.isEmpty())
        s[0] = s.at(0).toUpper();
    return s;
}

// "Resets at 8:49 PM" today, "Resets Sun, Nov 8 at 7:59 AM" on another day.
QString resetsText(const QString &iso)
{
    const QDateTime at = QDateTime::fromString(iso, Qt::ISODateWithMs).toLocalTime();
    if (!at.isValid())
        return {};
    const QString time = QLocale().toString(at.time(), QLocale::ShortFormat);
    if (at.date() == QDate::currentDate())
        return InfoCard::tr("Resets at %1").arg(time);
    return InfoCard::tr("Resets %1 at %2").arg(QLocale().toString(at.date(), QStringLiteral("ddd, MMM d")), time);
}

// {amount_minor, currency, exponent} as "$4.20".
QString money(const QJsonObject &m)
{
    if (m.isEmpty())
        return {};
    const double v = m.value(QLatin1String("amount_minor")).toDouble() / std::pow(10.0, m.value(QLatin1String("exponent")).toInt(2));
    const QString cur = str(m, "currency");
    return QLocale().toCurrencyString(v, cur == QLatin1String("USD") || cur.isEmpty() ? QStringLiteral("$") : cur + QLatin1Char(' '));
}

// A thin bar for a share of a limit, coloured by how close it is.
class LimitBar : public QWidget
{
public:
    LimitBar(double percent, const QString &severity, QWidget *parent)
        : QWidget(parent)
        , m_percent(percent)
        , m_severity(severity)
    {
        setFixedHeight(6);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme t = theme();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        const QRectF r = rect();
        p.setBrush(t.border);
        p.drawRoundedRect(r, 3, 3);
        const double share = qBound(0.0, m_percent / 100.0, 1.0);
        if (share <= 0)
            return;
        QColor fill = t.accent;
        if (m_severity == QLatin1String("warning") || (m_severity.isEmpty() && m_percent >= 75))
            fill = t.warning;
        else if (!m_severity.isEmpty() ? m_severity != QLatin1String("normal") : m_percent >= 90)
            fill = t.danger;
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(r.left(), r.top(), qMax(r.height(), r.width() * share), r.height()), 3, 3);
    }

private:
    double m_percent;
    QString m_severity;
};

// The coloured dot in front of an MCP server.
class StatusDot : public QWidget
{
public:
    StatusDot(const QString &status, QWidget *parent)
        : QWidget(parent)
        , m_status(status)
    {
        setFixedSize(10, 10);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme t = theme();
        QColor c = t.textMuted;
        if (m_status == QLatin1String("connected"))
            c = t.success;
        else if (m_status == QLatin1String("failed"))
            c = t.danger;
        else if (m_status == QLatin1String("needs-auth") || m_status == QLatin1String("pending"))
            c = t.warning;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QRectF(1, 1, 8, 8));
    }

private:
    QString m_status;
};

// A row that does something when clicked anywhere (the tool list of an MCP server).
class ClickRow : public QFrame
{
public:
    ClickRow(QWidget *parent, std::function<void()> onClick)
        : QFrame(parent)
        , m_onClick(std::move(onClick))
    {
        setObjectName(QStringLiteral("ccFileRow"));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()))
            m_onClick();
    }

private:
    std::function<void()> m_onClick;
};

QLabel *section(const QString &text, QWidget *parent)
{
    return label(text.toUpper(), parent, "ccInfoSection");
}

// "Email                      someone@example.com"
QWidget *keyValue(const QString &key, const QString &value, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(0, 1, 0, 1);
    l->setSpacing(12);
    l->addWidget(label(key, row, "aiHint"));
    auto *v = label(value, row);
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    l->addWidget(v, 1);
    return row;
}

// "Session (5 hr)        79%" / bar / "Resets at 8:49 PM"
QWidget *limitRow(const QString &title, double percent, const QString &severity, const QString &under, QWidget *parent)
{
    auto *w = new QWidget(parent);
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(0, 2, 0, 4);
    l->setSpacing(4);
    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    head->addWidget(label(title, w, "ccInfoStrong"), 1);
    head->addWidget(label(QStringLiteral("%1%").arg(qRound(percent)), w));
    l->addLayout(head);
    l->addWidget(new LimitBar(percent, severity, w));
    if (!under.isEmpty())
        l->addWidget(label(under, w, "ccInfoSmall"));
    return w;
}

QString limitTitle(const QJsonObject &limit)
{
    const QString kind = str(limit, "kind");
    QString title;
    if (kind == QLatin1String("session"))
        title = InfoCard::tr("Session (5 hr)");
    else if (kind == QLatin1String("weekly_all"))
        title = InfoCard::tr("Weekly (7 day)");
    else if (kind.startsWith(QLatin1String("weekly_")))
        title = InfoCard::tr("Weekly %1").arg(capitalized(kind.mid(7)));
    else
        title = capitalized(kind);
    const QString scope = str(limit, "scope");
    return scope.isEmpty() ? title : title + QStringLiteral(" · ") + scope;
}
} // namespace

// --- InfoCard -------------------------------------------------------------------------------------------------------

InfoCard::InfoCard(const QString &title, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("ccInfo"));
    setMaximumWidth(SettingsManager::instance().scaled(640));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(14, 10, 14, 12);
    lay->setSpacing(6);
    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    head->addWidget(label(title, this, "ccInfoTitle"));
    m_summary = label(QString(), this, "aiHint");
    m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    head->addWidget(m_summary, 1);
    m_refresh = new QToolButton(this);
    m_refresh->setObjectName(QStringLiteral("ccInfoRefresh"));
    m_refresh->setAutoRaise(true);
    m_refresh->setCursor(Qt::PointingHandCursor);
    m_refresh->setToolTip(tr("Refresh"));
    connect(m_refresh, &QToolButton::clicked, this, &InfoCard::refreshRequested);
    head->addWidget(m_refresh);
    lay->addLayout(head);
    m_layout = lay;
    m_message = label(QString(), this, "aiHint", true);
    m_message->hide();
    lay->addWidget(m_message);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &InfoCard::rebuild);
    rebuild();
}

void InfoCard::setSummary(const QString &text)
{
    m_summary->setText(text);
}

void InfoCard::setLoading(const QString &text)
{
    m_state = State::Loading;
    m_stateText = text;
    rebuild();
}

void InfoCard::setError(const QString &text)
{
    m_state = State::Error;
    m_stateText = text;
    rebuild();
}

void InfoCard::setMessage(const QString &text, bool error)
{
    m_message->setObjectName(QLatin1String(error ? "aiError" : "aiHint"));
    m_message->setText(text);
    m_message->setVisible(!text.isEmpty());
    m_message->style()->unpolish(m_message);
    m_message->style()->polish(m_message);
}

void InfoCard::showData()
{
    m_state = State::Ready;
    rebuild();
}

void InfoCard::rebuild()
{
    m_refresh->setIcon(Icons::tinted(QStringLiteral(":/new-icons/refresh-cw.svg"), theme().textMuted));
    m_refresh->setEnabled(m_state != State::Loading);
    // A new body each time: the old one goes with everything in it, nested layouts included.
    if (m_content) {
        m_content->hide();
        m_content->deleteLater();
    }
    m_content = new QWidget(this);
    auto *body = new QVBoxLayout(m_content);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(4);
    m_layout->insertWidget(1, m_content);
    if (m_state == State::Ready) {
        build(body);
        return;
    }
    setSummary(QString());
    body->addWidget(label(m_stateText, m_content, m_state == State::Error ? "aiError" : "aiHint", true));
}

// --- UsageCard ------------------------------------------------------------------------------------------------------

UsageCard::UsageCard(QWidget *parent)
    : InfoCard(tr("Account & Usage"), parent)
{
}

void UsageCard::setUsage(const AuthStatus &account, const QJsonObject &usage, double conversationCost)
{
    m_account = account;
    m_usage = usage;
    m_cost = conversationCost;
    showData();
}

void UsageCard::build(QVBoxLayout *body)
{
    const Theme t = theme();
    QWidget *host = body->parentWidget();
    setSummary(QString());

    // Account
    const AuthStatus &a = m_account;
    if (a.loggedIn) {
        body->addWidget(section(tr("Account"), host));
        QString method = a.method;
        if (method == QLatin1String("claude.ai"))
            method = tr("Claude account");
        else if (method == QLatin1String("console"))
            method = tr("Anthropic Console");
        else if (method == QLatin1String("api_key"))
            method = tr("API key");
        body->addWidget(keyValue(tr("Auth method"), method, host));
        if (!a.email.isEmpty())
            body->addWidget(keyValue(tr("Email"), a.email, host));
        if (!a.org.isEmpty() && a.method != QLatin1String("api_key"))
            body->addWidget(keyValue(tr("Organization"), a.org, host));
        QString plan = a.plan.isEmpty() ? str(m_usage, "subscription_type") : a.plan;
        if (!plan.isEmpty())
            body->addWidget(keyValue(tr("Plan"), tr("Claude %1").arg(capitalized(plan)), host));
        body->addSpacing(6);
    }

    // Plan limits
    body->addWidget(section(tr("Usage"), host));
    const QJsonObject limits = m_usage.value(QLatin1String("rate_limits")).toObject();
    if (m_usage.value(QLatin1String("rate_limits_available")).toBool() && !limits.isEmpty()) {
        QJsonArray rows = limits.value(QLatin1String("limits")).toArray();
        if (rows.isEmpty()) // older answers: the windows only
            for (const auto &[key, kind] : {std::pair{"five_hour", "session"}, std::pair{"seven_day", "weekly_all"},
                                            std::pair{"seven_day_opus", "weekly_opus"}, std::pair{"seven_day_sonnet", "weekly_sonnet"}}) {
                const QJsonObject w = limits.value(QLatin1String(key)).toObject();
                if (!w.isEmpty())
                    rows.append(QJsonObject{{"kind", QLatin1String(kind)}, {"percent", w.value(QLatin1String("utilization"))},
                                            {"resets_at", w.value(QLatin1String("resets_at"))}});
            }
        for (const QJsonValue &v : std::as_const(rows)) {
            const QJsonObject l = v.toObject();
            body->addWidget(limitRow(limitTitle(l), l.value(QLatin1String("percent")).toDouble(), str(l, "severity"),
                                     resetsText(str(l, "resets_at")), host));
        }
        const QJsonObject spend = limits.value(QLatin1String("spend")).toObject();
        if (spend.value(QLatin1String("enabled")).toBool()) {
            const QString used = money(spend.value(QLatin1String("used")).toObject());
            const QString cap = money(spend.value(QLatin1String("limit")).toObject());
            body->addWidget(limitRow(tr("Usage credits"), spend.value(QLatin1String("percent")).toDouble(), str(spend, "severity"),
                                     cap.isEmpty() ? tr("%1 used").arg(used) : tr("%1 of %2 used").arg(used, cap), host));
        }
        if (a.method == QLatin1String("claude.ai")) {
            auto *link = new QPushButton(tr("Manage usage on claude.ai"), host);
            link->setObjectName(QStringLiteral("ccLink"));
            link->setCursor(Qt::PointingHandCursor);
            connect(link, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://claude.ai/settings/usage"))); });
            auto *row = new QHBoxLayout;
            row->addWidget(link);
            row->addStretch(1);
            body->addLayout(row);
        }
    } else {
        body->addWidget(label(tr("Plan limits apply to Claude subscriptions. With an API key you pay per request."), host, "aiHint", true));
        body->addWidget(keyValue(tr("This conversation"), QLocale().toCurrencyString(m_cost, QStringLiteral("$")), host));
    }

    // What used it: recommendations and attributions, for the last day or week
    const QJsonObject behaviors = m_usage.value(QLatin1String("behaviors")).toObject();
    if (behaviors.isEmpty())
        return;
    body->addSpacing(6);
    body->addWidget(section(tr("What's contributing to your limits usage?"), host));
    auto *seg = new QFrame(host);
    seg->setObjectName(QStringLiteral("ccSegBox"));
    auto *sl = new QHBoxLayout(seg);
    sl->setContentsMargins(1, 1, 1, 1);
    sl->setSpacing(0);
    auto *group = new QButtonGroup(seg);
    for (int i = 0; i < 2; ++i) {
        auto *b = new QPushButton(i ? tr("Week") : tr("Day"), seg);
        b->setObjectName(QStringLiteral("ccSeg"));
        b->setCheckable(true);
        b->setChecked(m_week == (i == 1));
        b->setCursor(Qt::PointingHandCursor);
        group->addButton(b, i);
        sl->addWidget(b);
    }
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_week == (id == 1))
            return;
        m_week = id == 1;
        QTimer::singleShot(0, this, &UsageCard::rebuild); // not while the clicked button is handling its click
    });
    auto *segRow = new QHBoxLayout;
    segRow->addWidget(seg);
    segRow->addStretch(1);
    body->addLayout(segRow);

    const QJsonObject b = behaviors.value(QLatin1String(m_week ? "week" : "day")).toObject();
    body->addWidget(label(tr("Approximate · sessions on this machine only"), host, "ccInfoSmall"));
    body->addWidget(label((m_week ? tr("Last 7 days") : tr("Last 24 h")) + QStringLiteral(" · ")
                              + tr("%1 requests · %2 sessions").arg(QLocale().toString(b.value(QLatin1String("request_count")).toInt()),
                                                                    QLocale().toString(b.value(QLatin1String("session_count")).toInt())),
                          host, "ccInfoSmall"));
    body->addSpacing(4);

    const QString muted = t.textMuted.name();
    auto shareRow = [&](const QString &html, int pct) {
        auto *row = new QWidget(host);
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(0, 2, 0, 2);
        l->setSpacing(12);
        auto *text = new QLabel(html, row);
        text->setTextFormat(Qt::RichText);
        text->setWordWrap(true);
        l->addWidget(text, 1);
        if (pct >= 0)
            l->addWidget(label(QStringLiteral("%1%").arg(pct), row, "aiHint"), 0, Qt::AlignTop);
        body->addWidget(row);
    };
    for (const QJsonValue &v : b.value(QLatin1String("recommendations")).toArray()) {
        const QJsonObject r = v.toObject();
        QString html = QStringLiteral("<b>%1</b>").arg(str(r, "title").toHtmlEscaped());
        if (!str(r, "detail").isEmpty())
            html += QStringLiteral(" <span style='color:%1'>· %2</span>").arg(muted, str(r, "detail").toHtmlEscaped());
        shareRow(html, r.value(QLatin1String("pct")).toInt());
    }

    struct Share {
        QString name, kind;
        int pct;
    };
    QVector<Share> shares;
    for (const auto &[key, kind] : {std::pair{"skills", tr("skill")}, std::pair{"agents", tr("subagent")},
                                    std::pair{"plugins", tr("plugin")}, std::pair{"mcp_servers", tr("MCP server")}})
        for (const QJsonValue &v : b.value(QLatin1String(key)).toArray()) {
            const QJsonObject o = v.toObject();
            const QString name = str(o, "name").isEmpty() ? str(o, "key") : str(o, "name");
            if (!name.isEmpty())
                shares.append({name, kind, o.value(QLatin1String("pct")).toInt()});
        }
    std::stable_sort(shares.begin(), shares.end(), [](const Share &x, const Share &y) { return x.pct > y.pct; });
    shareRow(QStringLiteral("<b>%1</b>").arg(tr("Skills, subagents, plugins, and MCP servers").toHtmlEscaped())
                 + (shares.isEmpty() ? QStringLiteral("<br><span style='color:%1'>%2</span>")
                                           .arg(muted, tr("No attribution data yet · accumulates as you use Claude").toHtmlEscaped())
                                     : QString()),
             -1);
    for (const Share &s : std::as_const(shares))
        shareRow(QStringLiteral("&nbsp;&nbsp;%1 <span style='color:%2'>· %3</span>").arg(s.name.toHtmlEscaped(), muted, s.kind.toHtmlEscaped()), s.pct);
}

// --- McpCard --------------------------------------------------------------------------------------------------------

McpCard::McpCard(QWidget *parent)
    : InfoCard(tr("MCP servers"), parent)
    , m_emptyText(tr("No MCP servers are configured. Add one with `claude mcp add` in a terminal, or in a .mcp.json file."))
{
}

void McpCard::setServers(const QJsonArray &servers)
{
    m_servers = servers;
    showData();
    // Servers still connecting: look again in a moment, a few times.
    const bool pending = std::any_of(servers.begin(), servers.end(), [](const QJsonValue &v) {
        return v.toObject().value(QLatin1String("status")).toString() == QLatin1String("pending");
    });
    if (pending && m_polls < 6) {
        ++m_polls;
        QTimer::singleShot(2500, this, &McpCard::refreshRequested);
    }
}

void McpCard::build(QVBoxLayout *body)
{
    QWidget *host = body->parentWidget();
    QMap<QString, int> counts;
    for (const QJsonValue &v : std::as_const(m_servers))
        ++counts[v.toObject().value(QLatin1String("status")).toString()];
    QStringList summary;
    if (counts.value(QStringLiteral("connected")))
        summary << tr("%1 connected").arg(counts.value(QStringLiteral("connected")));
    if (counts.value(QStringLiteral("pending")))
        summary << tr("%1 connecting").arg(counts.value(QStringLiteral("pending")));
    if (counts.value(QStringLiteral("needs-auth")))
        summary << (counts.value(QStringLiteral("needs-auth")) == 1 ? tr("1 needs sign-in")
                                                                   : tr("%1 need sign-in").arg(counts.value(QStringLiteral("needs-auth"))));
    if (counts.value(QStringLiteral("failed")))
        summary << tr("%1 failed").arg(counts.value(QStringLiteral("failed")));
    if (counts.value(QStringLiteral("disabled")))
        summary << tr("%1 disabled").arg(counts.value(QStringLiteral("disabled")));
    setSummary(summary.join(QStringLiteral(" · ")));

    auto addManage = [&] {
        if (m_manageText.isEmpty())
            return;
        auto *link = new QPushButton(m_manageText, host);
        link->setObjectName(QStringLiteral("ccLink"));
        link->setCursor(Qt::PointingHandCursor);
        connect(link, &QPushButton::clicked, this, &McpCard::manageRequested);
        body->addSpacing(4);
        body->addWidget(link, 0, Qt::AlignLeft);
    };
    if (m_servers.isEmpty()) {
        body->addWidget(label(m_emptyText, host, "aiHint", true));
        addManage();
        return;
    }
    const Theme t = theme();
    for (const QJsonValue &v : std::as_const(m_servers)) {
        const QJsonObject s = v.toObject();
        const QString name = str(s, "name");
        const QString status = str(s, "status");
        const QJsonArray tools = s.value(QLatin1String("tools")).toArray();
        const bool open = m_open.contains(name);

        auto *box = new QWidget(host);
        auto *bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(2);
        auto *row = new ClickRow(box, [this, name] {
            if (!m_open.removeOne(name))
                m_open << name;
            QTimer::singleShot(0, this, &McpCard::rebuild);
        });
        row->setToolTip(tools.isEmpty() ? QString() : open ? tr("Hide the tools") : tr("Show the tools"));
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(6, 4, 6, 4);
        rl->setSpacing(8);
        rl->addWidget(new StatusDot(status, row));
        rl->addWidget(label(name, row, "ccInfoStrong"));

        QStringList detail;
        const QString transport = str(s, "transport").isEmpty() ? str(s.value(QLatin1String("config")).toObject(), "type") : str(s, "transport");
        if (transport == QLatin1String("claudeai-proxy"))
            detail << tr("claude.ai connector");
        else if (!transport.isEmpty())
            detail << transport;
        const QString scope = str(s, "scope");
        if (!scope.isEmpty() && scope != QLatin1String("claudeai") && scope != QLatin1String("dynamic"))
            detail << scope;
        if (!tools.isEmpty())
            detail << (tools.size() == 1 ? tr("1 tool") : tr("%1 tools").arg(tools.size()));
        auto *d = label(detail.join(QStringLiteral(" · ")), row, "aiHint");
        d->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rl->addWidget(d, 1);

        QString state = capitalized(status);
        QColor ink = t.textMuted;
        if (status == QLatin1String("connected")) {
            state = tr("Connected");
            ink = t.success;
        } else if (status == QLatin1String("pending")) {
            state = tr("Connecting…");
            ink = t.warning;
        } else if (status == QLatin1String("needs-auth")) {
            state = tr("Needs sign-in");
            ink = t.warning;
        } else if (status == QLatin1String("failed")) {
            state = tr("Failed");
            ink = t.danger;
        } else if (status == QLatin1String("disabled")) {
            state = tr("Disabled");
        }
        auto *st = new QLabel(QStringLiteral("<span style='color:%1'>%2</span>").arg(ink.name(), state.toHtmlEscaped()), row);
        st->setTextFormat(Qt::RichText);
        rl->addWidget(st);
        auto button = [&](const QString &text, const QString &tip, std::function<void()> f) {
            auto *b = new QPushButton(text, row);
            b->setObjectName(QStringLiteral("agentFooterBtn"));
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(tip);
            connect(b, &QPushButton::clicked, this, std::move(f));
            rl->addWidget(b);
        };
        if (status == QLatin1String("failed") || (status == QLatin1String("needs-auth") && scope != QLatin1String("claudeai")))
            button(tr("Reconnect"), tr("Try to connect to %1 again").arg(name), [this, name] { emit reconnectRequested(name); });
        if (status == QLatin1String("disabled"))
            button(tr("Enable"), tr("Use %1 in this project").arg(name), [this, name] { emit toggleRequested(name, true); });
        else
            button(tr("Disable"), tr("Stop using %1 in this project").arg(name), [this, name] { emit toggleRequested(name, false); });
        bl->addWidget(row);

        auto indent = [&](QLabel *l) {
            l->setContentsMargins(24, 0, 6, 2);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            bl->addWidget(l);
        };
        if (!str(s, "error").isEmpty())
            indent(label(str(s, "error"), box, "aiError", true));
        if (status == QLatin1String("needs-auth"))
            indent(label(scope == QLatin1String("claudeai") ? tr("Connect it in claude.ai under Settings › Connectors.")
                                                           : tr("Run /mcp in a terminal with claude to sign in."),
                         box, "ccInfoSmall", true));
        if (open && !tools.isEmpty()) {
            QStringList names;
            for (const QJsonValue &tv : tools)
                names << str(tv.toObject(), "name");
            indent(label(names.join(QStringLiteral(", ")), box, "ccInfoSmall", true));
        }
        body->addWidget(box);
    }
    addManage();
}

} // namespace claudecode
