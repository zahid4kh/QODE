#include "GitDiff.h"

namespace GitDiff {

namespace {
// Beyond this many edits per file we stop looking for a minimal diff and report one big hunk.
constexpr int kMaxEdits = 3000;
} // namespace

QStringList splitLines(const QString &textIn)
{
    QString text = textIn;
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    return text.split(QLatin1Char('\n'));
}

QVector<Hunk> compute(const QStringList &a, const QStringList &b)
{
    const int n = a.size(), m = b.size();
    int pre = 0;
    while (pre < n && pre < m && a.at(pre) == b.at(pre))
        ++pre;
    int suf = 0;
    while (suf < n - pre && suf < m - pre && a.at(n - 1 - suf) == b.at(m - 1 - suf))
        ++suf;
    const int an = n - pre - suf, bn = m - pre - suf;

    QVector<Hunk> out;
    if (an == 0 && bn == 0)
        return out;
    if (an == 0 || bn == 0) {
        out.push_back({pre, an, pre, bn});
        return out;
    }

    const int cap = qMin(an + bn, kMaxEdits);
    const int off = cap + 1;
    QVector<int> v(2 * cap + 3, 0);
    QVector<QVector<int>> trace;
    int found = -1;
    for (int d = 0; d <= cap && found < 0; ++d) {
        // Snapshot of V for k in [-d, d] before this round, used when walking back.
        trace.push_back(QVector<int>(v.constBegin() + off - d, v.constBegin() + off + d + 1));
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
            int y = x - k;
            while (x < an && y < bn && a.at(pre + x) == b.at(pre + y)) {
                ++x;
                ++y;
            }
            v[off + k] = x;
            if (x >= an && y >= bn) {
                found = d;
                break;
            }
        }
    }
    if (found < 0) {
        out.push_back({pre, an, pre, bn});
        return out;
    }

    QVector<bool> delA(an, false), insB(bn, false);
    int x = an, y = bn;
    for (int d = found; d > 0; --d) {
        const QVector<int> &sv = trace.at(d);
        const int k = x - y;
        const int prevK = (k == -d || (k != d && sv[k - 1 + d] < sv[k + 1 + d])) ? k + 1 : k - 1;
        const int prevX = sv[prevK + d];
        const int prevY = prevX - prevK;
        while (x > prevX && y > prevY) {
            --x;
            --y;
        }
        if (x == prevX)
            insB[prevY] = true;
        else
            delA[prevX] = true;
        x = prevX;
        y = prevY;
    }

    int i = 0, j = 0;
    while (i < an || j < bn) {
        if (i < an && j < bn && !delA[i] && !insB[j]) {
            ++i;
            ++j;
            continue;
        }
        const int si = i, sj = j;
        while (i < an && delA[i])
            ++i;
        while (j < bn && insB[j])
            ++j;
        if (i == si && j == sj) { // cannot happen for a valid trace; never loop forever
            ++i;
            ++j;
            continue;
        }
        Hunk h{pre + si, i - si, pre + sj, j - sj};
        if (!out.isEmpty()) {
            Hunk &p = out.last();
            if (p.oldStart + p.oldCount == h.oldStart && p.newStart + p.newCount == h.newStart) {
                p.oldCount += h.oldCount;
                p.newCount += h.newCount;
                continue;
            }
        }
        out.push_back(h);
    }
    return out;
}

} // namespace GitDiff
