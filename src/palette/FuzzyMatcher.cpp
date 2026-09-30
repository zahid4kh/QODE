#include "FuzzyMatcher.h"

#include <QVector>

namespace FuzzyMatcher {

namespace {

constexpr int kNone = -1000000;
constexpr int kMaxText = 320; // longer texts are matched on their tail only
constexpr int kGapPenalty = 1;
constexpr int kConsecutiveBonus = 6;
constexpr int kBoundaryBonus = 8;
constexpr int kCamelBonus = 6;
constexpr int kNameBonus = 2;

bool isSeparator(QChar c)
{
    return c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(' ') || c == QLatin1Char('_') ||
           c == QLatin1Char('-') || c == QLatin1Char('.') || c == QLatin1Char(':');
}

} // namespace

int score(const QString &patternIn, const QString &textIn, int nameStart, QList<int> *positions)
{
    if (positions)
        positions->clear();
    const int m = patternIn.size();
    if (m == 0)
        return 0;

    int skip = 0; // characters cut from the front of overly long texts
    QString text = textIn;
    if (text.size() > kMaxText) {
        skip = text.size() - kMaxText;
        text = text.mid(skip);
    }
    const int n = text.size();
    if (m > n)
        return -1;
    nameStart = qMax(0, nameStart - skip);

    QString lp, lt;
    lp.reserve(m);
    lt.reserve(n);
    for (const QChar c : patternIn)
        lp += c.toLower();
    for (const QChar c : text)
        lt += c.toLower();

    // Cheap reject before the dynamic programme.
    int pi = 0;
    for (int j = 0; j < n && pi < m; ++j)
        if (lt.at(j) == lp.at(pi))
            ++pi;
    if (pi < m)
        return -1;

    auto charBonus = [&](int j) {
        if (j == 0)
            return kBoundaryBonus;
        const QChar prev = text.at(j - 1), cur = text.at(j);
        if (isSeparator(prev))
            return kBoundaryBonus;
        if (prev.isLower() && cur.isUpper())
            return kCamelBonus;
        return 0;
    };

    QVector<int> dp(m * n, kNone);
    QVector<int> parent(m * n, -1);
    for (int i = 0; i < m; ++i) {
        int runMax = kNone, runArg = -1; // best dp[i-1][k] + k * gap for k < j - 1 ... j - 1
        for (int j = 0; j < n; ++j) {
            if (i > 0 && j > 0) {
                const int v = dp[(i - 1) * n + j - 1];
                if (v > kNone && v + (j - 1) * kGapPenalty > runMax) {
                    runMax = v + (j - 1) * kGapPenalty;
                    runArg = j - 1;
                }
            }
            if (lt.at(j) != lp.at(i))
                continue;
            int base = 1 + charBonus(j) + (text.at(j) == patternIn.at(i) ? 1 : 0);
            if (j >= nameStart)
                base += kNameBonus + (j == nameStart ? kBoundaryBonus : 0);
            if (i == 0) {
                dp[j] = base - qMin(j, 6) / 2;
                continue;
            }
            int best = kNone, arg = -1;
            if (runMax > kNone) {
                best = runMax - (j - 1) * kGapPenalty;
                arg = runArg;
            }
            if (j > 0) {
                const int consecutive = dp[(i - 1) * n + j - 1];
                if (consecutive > kNone && consecutive + kConsecutiveBonus > best) {
                    best = consecutive + kConsecutiveBonus;
                    arg = j - 1;
                }
            }
            if (best == kNone)
                continue;
            dp[i * n + j] = best + base;
            parent[i * n + j] = arg;
        }
    }

    int bestScore = kNone, bestJ = -1;
    for (int j = 0; j < n; ++j) {
        const int v = dp[(m - 1) * n + j];
        if (v > bestScore) {
            bestScore = v;
            bestJ = j;
        }
    }
    if (bestJ < 0)
        return -1;

    if (positions) {
        QList<int> pos;
        int j = bestJ;
        for (int i = m - 1; i >= 0; --i) {
            pos.prepend(j + skip);
            j = parent[i * n + j];
        }
        *positions = pos;
    }
    return bestScore - n / 16; // shorter candidates first among equals
}

} // namespace FuzzyMatcher
