#pragma once

#include "terminal/TerminalScreen.h"

#include <QColor>
#include <QString>
#include <QVector>

// Helpers for building styled terminal text: every block in Terminal Only mode is rendered by a
// TerminalScreen, so QODE's own output is written as ANSI escape sequences too.
namespace Ansi {

// Palette indices (resolved against the active theme when painted).
enum Color { Black = 0, Red, Green, Yellow, Blue, Magenta, Cyan, White, Muted /* bright black */, BrightRed, BrightGreen, BrightYellow, BrightBlue, BrightMagenta, BrightCyan, BrightWhite };

QString fg(int paletteIndex);
QString rgb(const QColor &c);
QString reset();
QString bold();
// `text` in a palette colour, optionally bold, followed by a reset.
QString paint(const QString &text, int paletteIndex, bool boldText = false);
QString dim(const QString &text);

// Makes untrusted text safe to print: ESC and other control characters (except tab and newline) become visible symbols.
QString sanitize(const QString &text);
// "\n" -> "\r\n" (what a terminal expects).
QString crlf(const QString &text);

bool isBlank(const TerminalScreen::Line &line);
QString plainText(const TerminalScreen::Line &line);
// Renders `ansiText` on a screen `cols` wide and returns its lines without trailing blank ones.
QVector<TerminalScreen::Line> toLines(const QString &ansiText, int cols);
// All lines of a screen (scrollback + grid) up to the last non-blank one.
QVector<TerminalScreen::Line> allLines(const TerminalScreen &screen);

} // namespace Ansi
