#include "Ansi.h"

namespace Ansi {

QString fg(int i)
{
    return QStringLiteral("\x1b[38;5;%1m").arg(i);
}

QString rgb(const QColor &c)
{
    return QStringLiteral("\x1b[38;2;%1;%2;%3m").arg(c.red()).arg(c.green()).arg(c.blue());
}

QString reset()
{
    return QStringLiteral("\x1b[0m");
}

QString bold()
{
    return QStringLiteral("\x1b[1m");
}

QString paint(const QString &text, int i, bool boldText)
{
    return (boldText ? bold() : QString()) + fg(i) + text + reset();
}

QString dim(const QString &text)
{
    return paint(text, Muted);
}

QString sanitize(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if (u == '\x1b')
            out += QChar(0x241B); // ␛
        else if (u == '\r')
            continue;
        else if (u < 0x20 && u != '\t' && u != '\n')
            out += QChar(0x2400 + u);
        else if (u == 0x7f)
            out += QChar(0x2421);
        else
            out += c;
    }
    return out;
}

QString crlf(const QString &text)
{
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    return t;
}

bool isBlank(const TerminalScreen::Line &line)
{
    for (const TerminalScreen::Cell &c : line)
        if (c.ch != U' ' || c.bg != 0 || (c.attr & TerminalScreen::Inverse))
            return false;
    return true;
}

QString plainText(const TerminalScreen::Line &line)
{
    QString s;
    for (const TerminalScreen::Cell &c : line)
        s += QString::fromUcs4(&c.ch, 1);
    while (s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    return s;
}

QVector<TerminalScreen::Line> allLines(const TerminalScreen &screen)
{
    int last = screen.totalLines() - 1;
    while (last >= 0 && isBlank(screen.lineAt(last)))
        --last;
    QVector<TerminalScreen::Line> lines;
    lines.reserve(last + 1);
    for (int i = 0; i <= last; ++i)
        lines.append(screen.lineAt(i));
    return lines;
}

QVector<TerminalScreen::Line> toLines(const QString &ansiText, int cols)
{
    TerminalScreen screen(qMax(10, cols), 1);
    screen.feed(crlf(ansiText).toUtf8());
    return allLines(screen);
}

} // namespace Ansi
