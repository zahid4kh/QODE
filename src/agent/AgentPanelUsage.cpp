// The QODE chat's usage meter: tokens, cache hits and an estimated cost for the current (or last) run and the whole chat,
// in the header, with the breakdown in its menu.
#include "AgentPanel.h"

#include "agent/AgentTurnView.h"
#include "ai/AiModels.h"
#include "ai/AiPricing.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QWidgetAction>

namespace agent {

namespace {

// What some requests cost, summed.
struct Totals {
    int requests = 0;
    qint64 prompt = 0, cached = 0, cacheWrite = 0, output = 0, thinking = 0;
    double cost = 0;
    bool priced = true;     // every request's model has a known price
    bool estimated = false; // a count is still the live estimate

    bool isEmpty() const { return requests == 0; }
    void add(const RequestStats &r, const QString &model)
    {
        ++requests;
        prompt += r.prompt;
        cached += r.cached;
        cacheWrite += r.cacheWrite;
        output += r.output;
        thinking += r.thinking;
        estimated = estimated || !r.exact;
        double dollars = 0;
        if (AiPricing::cost(model, r.prompt, r.cached, r.cacheWrite, r.output, &dollars))
            cost += dollars;
        else
            priced = false;
    }
    // A finished turn; chats saved before the request log was kept have only the turn's totals.
    void add(const Message &m, const QString &fallbackModel)
    {
        const QString model = m.model.isEmpty() ? fallbackModel : m.model;
        if (!m.requestLog.isEmpty()) {
            for (const RequestStats &r : m.requestLog)
                add(r, model);
            return;
        }
        if (m.promptTokens <= 0 && m.outputTokens <= 0)
            return;
        RequestStats r;
        r.prompt = m.promptTokens;
        r.cached = m.cachedTokens;
        r.output = m.outputTokens;
        add(r, model);
        requests += qMax(1, m.requests) - 1;
    }
};

// `run`: the live turn when there is one, else the chat's last turn. `chat`: every turn, /compact included.
void collect(const Session &session, const QString &model, const Message *live, const RequestStats &liveRequest, Totals *run, Totals *chat)
{
    for (const Message &m : session.messages) {
        if (m.role == QLatin1String("assistant")) {
            *run = Totals();
            run->add(m, model);
            chat->add(m, model);
        } else if (m.role == QLatin1String("summary")) {
            chat->add(m, model);
        }
    }
    if (!live)
        return;
    *run = Totals();
    QVector<RequestStats> requests = live->requestLog;
    if (liveRequest.prompt > 0 || liveRequest.output > 0)
        requests.append(liveRequest); // the request being streamed
    for (const RequestStats &r : std::as_const(requests)) {
        run->add(r, live->model);
        chat->add(r, live->model);
    }
}

// "820", "4.2k", "99k", "1.2M"
QString tokens(qint64 n)
{
    if (n < 1000)
        return QString::number(n);
    if (n < 10000)
        return QString::number(n / 1000.0, 'f', 1) + QLatin1Char('k');
    if (n < 1000000)
        return QString::number(qRound64(n / 1000.0)) + QLatin1Char('k');
    return QString::number(n / 1e6, 'f', 1) + QLatin1Char('M');
}

QString percent(qint64 part, qint64 whole)
{
    return whole > 0 ? QStringLiteral("%1%").arg(qRound(100.0 * part / whole)) : QString();
}

QString costText(const Totals &t)
{
    return t.priced ? AiPricing::format(t.cost) : QString();
}

} // namespace

void AgentPanel::updateUsage()
{
    Totals run, chat;
    const QString model = m_session.model.isEmpty() ? m_modelName : m_session.model;
    collect(m_session, model, m_busy && !m_compacting ? &m_live : nullptr, m_liveRequest, &run, &chat);
    m_usageBtn->setVisible(!chat.isEmpty());
    if (chat.isEmpty())
        return;
    // "Run 18k · 33% cached  |  Chat 99k · $0.012"
    const QString approx = run.estimated ? QStringLiteral("~") : QString();
    QString text = tr("Run %1").arg(approx + tokens(run.prompt + run.output));
    if (run.cached > 0)
        text += tr(" · %1 cached").arg(percent(run.cached, run.prompt));
    text += tr("  |  Chat %1").arg(tokens(chat.prompt + chat.output));
    if (chat.priced)
        text += QStringLiteral(" · ") + costText(chat);
    m_usageBtn->setText(text);
    m_usageBtn->setToolTip(tr("Tokens, cache hits and estimated cost of %1 run and of this chat").arg(m_busy ? tr("the current") : tr("the last")));
}

// The breakdown: one column for the run, one for the chat.
void AgentPanel::rebuildUsageMenu()
{
    QMenu *menu = m_usageBtn->menu();
    menu->clear();
    Totals run, chat;
    const QString model = m_session.model.isEmpty() ? m_modelName : m_session.model;
    collect(m_session, model, m_busy && !m_compacting ? &m_live : nullptr, m_liveRequest, &run, &chat);

    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    const QString muted = t.textMuted.name();
    auto cell = [](const QString &s, bool strong = false) {
        return QStringLiteral("<td align=right style='padding: 1px 0 1px 18px'>%1</td>").arg(strong ? QStringLiteral("<b>%1</b>").arg(s) : s);
    };
    auto row = [&](const QString &label, const QString &a, const QString &b, bool sub = false, bool strong = false) {
        return QStringLiteral("<tr><td style='padding: 1px 0; %1'>%2</td>%3%4</tr>")
            .arg(sub ? QStringLiteral("color: %1; padding-left: 12px").arg(muted) : QString(), label, cell(a, strong), cell(b, strong));
    };
    auto withShare = [](qint64 part, qint64 whole) {
        return part > 0 ? QStringLiteral("%1 (%2)").arg(tokens(part), percent(part, whole)) : QStringLiteral("0");
    };
    QString html = QStringLiteral("<table cellspacing=0>");
    html += QStringLiteral("<tr><td></td>%1%2</tr>")
                .arg(cell(QStringLiteral("<span style='color:%1'>%2</span>").arg(muted, m_busy ? tr("This run") : tr("Last run"))),
                     cell(QStringLiteral("<span style='color:%1'>%2</span>").arg(muted, tr("This chat"))));
    html += row(tr("Requests"), QString::number(run.requests), QString::number(chat.requests));
    html += row(tr("Input"), tokens(run.prompt), tokens(chat.prompt));
    html += row(tr("from cache"), withShare(run.cached, run.prompt), withShare(chat.cached, chat.prompt), true);
    if (chat.cacheWrite > 0)
        html += row(tr("written to cache"), tokens(run.cacheWrite), tokens(chat.cacheWrite), true);
    html += row(tr("Output"), (run.estimated ? QStringLiteral("~") : QString()) + tokens(run.output),
                (chat.estimated ? QStringLiteral("~") : QString()) + tokens(chat.output));
    if (chat.thinking > 0)
        html += row(tr("thinking"), tokens(run.thinking), tokens(chat.thinking), true);
    if (chat.priced)
        html += row(tr("Estimated cost"), costText(run), costText(chat), false, true);
    html += QStringLiteral("</table>");
    const QString note = chat.priced
        ? tr("At %1 list prices (standard paid tier). A free tier costs nothing; your provider's bill is what counts.")
              .arg(AiModels::label(model))
        : tr("No price is known for %1, so no cost is shown.").arg(model);
    html += QStringLiteral("<p style='color:%1; margin-top: 8px'>%2</p>").arg(muted, note.toHtmlEscaped());

    auto *label = new QLabel(html);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setFixedWidth(340);
    label->setContentsMargins(12, 8, 12, 8);
    useTabularDigits(label);
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(label);
    menu->addAction(action);
}

} // namespace agent
