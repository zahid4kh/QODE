#pragma once

#include <QDate>
#include <QString>
#include <climits>

// List prices of the models QODE offers, for the chat's usage meter: an estimate at the standard paid tier, never a bill
// (free tiers, batch and priority tiers, and provider credits are not known to QODE). Claude from claude-api-docs/
// models-pricing.md, Gemini from ai.google.dev/gemini-api/docs/pricing (checked 2026-11-02). Update both when the model
// lists in AiModels change.
namespace AiPricing {

// US dollars per million tokens.
struct Rates {
    double input = 0, output = 0; // output includes thinking
    double cacheRead = 0;         // input read from the cache (Claude cache hits, Gemini implicit caching)
    double cacheWrite = 0;        // Claude: input written to the cache (5-minute TTL); Gemini has no write charge
};

// The rates for `model` and a request with `promptTokens` of input (Haiku costs more above 100k); false = unknown model.
inline bool rates(const QString &model, int promptTokens, Rates *r)
{
    const QString m = model.trimmed().toLower();
    auto is = [&m](const char *prefix) { return m.startsWith(QLatin1String(prefix)); };
    if (is("claude-fable-5-1"))
        *r = {10.0, 50.0, 0.25, 12.50};
    else if (is("claude-opus-5-5"))
        *r = {4.0, 20.0, 0.20, 5.0};
    else if (is("claude-sonnet-5-5"))
        *r = {2.0, 10.0, 0.10, 2.50};
    else if (is("claude-haiku-5-5"))
        *r = promptTokens > 100000 ? Rates{0.50, 2.50, 0.05, 0.625} : Rates{0.10, 0.50, 0.01, 0.125};
    else if (is("gemini-3.5-flash-lite"))
        *r = {0.30, 2.50, 0.03, 0};
    else if (is("gemini-3.1-flash-lite"))
        *r = {0.25, 1.50, 0.025, 0};
    // gemini-3.5-flash is retired: Google routes it to 3.6 Flash, which bills at 3.6 rates (introductory until 2027)
    else if (is("gemini-3.6-flash") || is("gemini-3.5-flash") || is("gemini-3.8-flash"))
        *r = QDate::currentDate() < QDate(2027, 1, 1) ? Rates{0.75, 3.75, 0.075, 0} : Rates{1.50, 7.50, 0.15, 0};
    else
        return false;
    return true;
}

// The cost of one request: `prompt` is all input (cached and cache writes included), `output` includes thinking.
inline bool cost(const QString &model, qint64 prompt, qint64 cached, qint64 cacheWrite, qint64 output, double *dollars)
{
    Rates r;
    if (!rates(model, int(qMin<qint64>(prompt, INT_MAX)), &r))
        return false;
    const qint64 plain = qMax<qint64>(0, prompt - cached - cacheWrite);
    *dollars = (plain * r.input + cached * r.cacheRead + cacheWrite * r.cacheWrite + output * r.output) / 1e6;
    return true;
}

// "$0.0042", "$0.12", "$3.40": small amounts keep two significant digits.
inline QString format(double dollars)
{
    if (dollars <= 0)
        return QStringLiteral("$0");
    if (dollars < 0.0001)
        return QStringLiteral("<$0.0001");
    if (dollars < 0.01)
        return QStringLiteral("$") + QString::number(dollars, 'g', 2);
    return QStringLiteral("$") + QString::number(dollars, 'f', 2);
}

} // namespace AiPricing
