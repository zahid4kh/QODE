#pragma once

#include "ai/AiEnv.h"
#include "settings/Theme.h"

#include <QColor>
#include <QString>
#include <iterator>

// The models QODE offers in its pickers (the chat and the inline edit box), per provider; any other id can be typed in.
namespace AiModels {

struct Info {
    const char *id, *label, *hint;
};
inline constexpr Info kClaude[] = {{"claude-opus-5-5", "Claude Opus 5.5", "best for agentic coding"},
                                   {"claude-sonnet-5-5", "Claude Sonnet 5.5", "fast and capable"},
                                   {"claude-haiku-5-5", "Claude Haiku 5.5", "fastest, lowest cost"},
                                   {"claude-fable-5-1", "Claude Fable 5.1", "most capable, highest cost"}};
inline constexpr Info kGemini[] = {{"gemini-3.5-flash-lite", "Gemini 3.5 Flash-Lite", ""},
                                   {"gemini-3.5-flash", "Gemini 3.5 Flash", ""},
                                   {"gemini-3.1-flash-lite", "Gemini 3.1 Flash-Lite", ""},
                                   {"gemini-3.6-flash", "Gemini 3.6 Flash", ""}};

inline QString label(const QString &id)
{
    for (const Info &m : kClaude)
        if (id == QLatin1String(m.id))
            return QString::fromLatin1(m.label);
    for (const Info &m : kGemini)
        if (id == QLatin1String(m.id))
            return QString::fromLatin1(m.label);
    return id;
}

inline QString providerIcon(AiEnv::Provider p)
{
    return p == AiEnv::Provider::Claude ? QStringLiteral(":/new-icons/claude.svg") : QStringLiteral(":/new-icons/gemini.svg");
}

inline QColor providerColor(const Theme &t, AiEnv::Provider p)
{
    return p == AiEnv::Provider::Claude ? t.brandClaude : t.brandGemini;
}

} // namespace AiModels
