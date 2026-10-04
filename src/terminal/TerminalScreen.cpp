#include "TerminalScreen.h"

#include <QStringView>

namespace {
const TerminalScreen::Line kEmptyLine;
}

TerminalScreen::TerminalScreen(int cols, int rows, QObject *parent)
    : QObject(parent)
    , m_cols(qMax(2, cols))
    , m_rows(qMax(1, rows))
{
    reset();
}

void TerminalScreen::reset()
{
    m_pen = Cell();
    m_screen = QVector<Line>(m_rows, blankLine());
    m_savedPrimary.clear();
    m_alt = false;
    m_mouseTracking = m_sgrMouse = false;
    m_row = m_col = 0;
    m_wrapPending = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_cursorVisible = true;
    m_appCursor = false;
    m_autoWrap = true;
    m_bracketedPaste = false;
    m_originMode = false;
    m_tabStops = QVector<bool>(m_cols, false);
    for (int i = 8; i < m_cols; i += 8)
        m_tabStops[i] = true;
    m_state = State::Ground;
    emit changed();
}

TerminalScreen::Cell TerminalScreen::blankCell() const
{
    Cell c;
    c.bg = m_pen.bg;
    return c;
}

TerminalScreen::Line TerminalScreen::blankLine() const
{
    return Line(m_cols, blankCell());
}

const TerminalScreen::Line &TerminalScreen::lineAt(int i) const
{
    if (i < 0)
        return kEmptyLine;
    if (i < m_scrollback.size())
        return m_scrollback.at(i);
    const int r = i - m_scrollback.size();
    return r < m_screen.size() ? m_screen.at(r) : kEmptyLine;
}

void TerminalScreen::clearScrollback()
{
    m_scrollback.clear();
    emit changed();
}

void TerminalScreen::resize(int cols, int rows)
{
    cols = qMax(2, cols);
    rows = qMax(1, rows);
    if (cols == m_cols && rows == m_rows)
        return;

    auto fit = [cols](Line &l) {
        if (l.size() != cols)
            l.resize(cols);
    };
    // Rows shrinking: push lines above the cursor into scrollback (primary screen only).
    while (m_rows > rows) {
        if (!m_alt && m_row > 0) {
            m_scrollback.append(m_screen.takeFirst());
            --m_row;
        } else {
            m_screen.removeLast();
        }
        --m_rows;
    }
    // Rows growing: pull history back in, otherwise append blanks.
    while (m_rows < rows) {
        if (!m_alt && !m_scrollback.isEmpty()) {
            m_screen.prepend(m_scrollback.takeLast());
            ++m_row;
        } else {
            m_screen.append(Line(cols));
        }
        ++m_rows;
    }
    m_cols = cols;
    for (Line &l : m_screen)
        fit(l);
    for (Line &l : m_savedPrimary)
        fit(l);
    m_row = qBound(0, m_row, m_rows - 1);
    m_col = qMin(m_col, m_cols - 1);
    m_wrapPending = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_tabStops = QVector<bool>(m_cols, false);
    for (int i = 8; i < m_cols; i += 8)
        m_tabStops[i] = true;
    emit changed();
}

// --- Input -----------------------------------------------------------------

void TerminalScreen::feed(const QByteArray &data)
{
    m_pending.append(data);
    // Hold back an incomplete trailing UTF-8 sequence until the next chunk.
    int keep = 0;
    for (int back = 1; back <= 3 && back <= m_pending.size(); ++back) {
        const uchar b = uchar(m_pending.at(m_pending.size() - back));
        if ((b & 0xC0) == 0x80)
            continue;                 // continuation byte, keep looking for the lead
        if (b >= 0xC0) {
            const int need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
            if (need > back)
                keep = back;
        }
        break;
    }
    const QByteArray complete = m_pending.left(m_pending.size() - keep);
    m_pending = m_pending.right(keep);

    const QString text = QString::fromUtf8(complete);
    const QStringView v(text);
    for (int i = 0; i < v.size(); ++i) {
        char32_t c = v.at(i).unicode();
        if (QChar::isHighSurrogate(c) && i + 1 < v.size()) {
            c = QChar::surrogateToUcs4(QChar(c), v.at(i + 1));
            ++i;
        }
        processChar(c);
    }
    emit changed();
}

void TerminalScreen::processChar(char32_t c)
{
    switch (m_state) {
    case State::Ground:
        if (c == 0x1b)
            m_state = State::Escape;
        else if (c < 0x20 || c == 0x7f)
            execute(c);
        else
            print(c);
        return;

    case State::Escape:
        if (c == '[') {
            m_state = State::Csi;
            m_csiBuf.clear();
        } else if (c == ']') {
            m_state = State::Osc;
            m_oscBuf.clear();
        } else if (c == 'P' || c == '_' || c == '^' || c == 'X') {
            m_state = State::StringIgnore;
        } else if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%' || c == ' ') {
            m_intermediate = c;
            m_state = State::EscapeIntermediate;
        } else if (c < 0x20) {
            execute(c);
        } else {
            m_state = State::Ground;
            handleEscape(c);
        }
        return;

    case State::EscapeIntermediate:
        // e.g. ESC ( B — charset selection; consume and ignore. ESC # 8 = screen alignment test.
        if (m_intermediate == '#' && c == '8') {
            for (Line &l : m_screen)
                l = Line(m_cols, [] { Cell x; x.ch = U'E'; return x; }());
        }
        m_state = State::Ground;
        return;

    case State::Csi:
        if (c >= 0x40 && c <= 0x7e) {
            m_state = State::Ground;
            // Parse "?1;2;3" style parameter string.
            m_params.clear();
            QString s = m_csiBuf;
            const bool priv = s.startsWith(QLatin1Char('?')) || s.startsWith(QLatin1Char('>')) || s.startsWith(QLatin1Char('='));
            const QChar lead = priv ? s.at(0) : QChar();
            if (priv)
                s.remove(0, 1);
            const QStringList parts = s.split(QLatin1Char(';'), Qt::KeepEmptyParts);
            for (const QString &p : parts) {
                // Sub-parameters (a:b:c) are flattened.
                const QStringList subs = p.split(QLatin1Char(':'));
                for (const QString &sp : subs)
                    m_params.append(sp.isEmpty() ? -1 : sp.toInt());
            }
            if (m_params.isEmpty())
                m_params.append(-1);
            if (lead == QLatin1Char('>') || lead == QLatin1Char('=')) {
                if (c == 'c') // secondary device attributes
                    emit reply(QByteArrayLiteral("\x1b[>0;0;0c"));
                return;
            }
            if (priv && (c == 'h' || c == 'l')) {
                for (int p : std::as_const(m_params))
                    setMode(true, p, c == 'h');
                return;
            }
            if (priv)
                return;
            handleCsi(c);
        } else if (c == 0x1b) {
            m_state = State::Escape;
        } else if (c < 0x20) {
            execute(c);
        } else {
            m_csiBuf.append(QChar(char16_t(c)));
        }
        return;

    case State::Osc:
        if (c == 0x07) {
            handleOsc();
            m_state = State::Ground;
        } else if (c == 0x1b) {
            m_state = State::StringEsc;
        } else {
            m_oscBuf.append(QChar::fromUcs4(c));
        }
        return;

    case State::StringIgnore:
        if (c == 0x07)
            m_state = State::Ground;
        else if (c == 0x1b)
            m_state = State::StringEsc;
        return;

    case State::StringEsc:
        // ESC \ terminates a string; anything else starts a new escape.
        if (!m_oscBuf.isEmpty())
            handleOsc();
        m_oscBuf.clear();
        m_state = State::Ground;
        if (c != '\\')
            processChar(c);
        return;
    }
}

int TerminalScreen::param(int i, int def) const
{
    if (i >= m_params.size() || m_params.at(i) <= 0)
        return def;
    return m_params.at(i);
}

void TerminalScreen::execute(char32_t c)
{
    switch (c) {
    case 0x07: break;                                   // BEL
    case 0x08: if (m_col > 0) --m_col; m_wrapPending = false; break; // BS
    case 0x09: {                                        // HT
        int c2 = qMin(m_col, m_cols - 1) + 1;
        while (c2 < m_cols - 1 && !m_tabStops.value(c2))
            ++c2;
        m_col = qMin(c2, m_cols - 1);
        m_wrapPending = false;
        break;
    }
    case 0x0a: case 0x0b: case 0x0c: lineFeed(); break; // LF VT FF
    case 0x0d: carriageReturn(); break;                 // CR
    default: break;
    }
}

void TerminalScreen::print(char32_t c)
{
    if (m_wrapPending && m_autoWrap) {
        carriageReturn();
        lineFeed();
    }
    m_wrapPending = false;
    if (m_col >= m_cols)
        m_col = m_cols - 1;
    Cell cell = m_pen;
    cell.ch = c;
    m_screen[m_row][m_col] = cell;
    if (m_col == m_cols - 1)
        m_wrapPending = true;
    else
        ++m_col;
}

void TerminalScreen::lineFeed()
{
    if (m_row == m_scrollBottom)
        scrollUp(1);
    else if (m_row < m_rows - 1)
        ++m_row;
}

void TerminalScreen::reverseIndex()
{
    if (m_row == m_scrollTop)
        scrollDown(1);
    else if (m_row > 0)
        --m_row;
}

void TerminalScreen::scrollUp(int n)
{
    for (int i = 0; i < n; ++i) {
        Line top = m_screen.takeAt(m_scrollTop);
        if (!m_alt && m_scrollTop == 0) {
            m_scrollback.append(top);
            if (m_scrollback.size() > kMaxScrollback)
                m_scrollback.removeFirst();
        }
        m_screen.insert(m_scrollBottom, blankLine());
    }
}

void TerminalScreen::scrollDown(int n)
{
    for (int i = 0; i < n; ++i) {
        m_screen.removeAt(m_scrollBottom);
        m_screen.insert(m_scrollTop, blankLine());
    }
}

void TerminalScreen::moveTo(int row, int col)
{
    if (m_originMode)
        row += m_scrollTop;
    m_row = qBound(0, row, m_rows - 1);
    m_col = qBound(0, col, m_cols - 1);
    m_wrapPending = false;
}

void TerminalScreen::eraseInLine(int mode)
{
    Line &l = m_screen[m_row];
    const int col = qMin(m_col, m_cols - 1);
    int from = 0, to = m_cols;
    if (mode == 0)
        from = col;
    else if (mode == 1)
        to = col + 1;
    for (int i = from; i < to; ++i)
        l[i] = blankCell();
}

void TerminalScreen::eraseInDisplay(int mode)
{
    if (mode == 0) {
        eraseInLine(0);
        for (int r = m_row + 1; r < m_rows; ++r)
            m_screen[r] = blankLine();
    } else if (mode == 1) {
        eraseInLine(1);
        for (int r = 0; r < m_row; ++r)
            m_screen[r] = blankLine();
    } else if (mode == 2) {
        for (int r = 0; r < m_rows; ++r)
            m_screen[r] = blankLine();
    } else if (mode == 3) {
        m_scrollback.clear();
    }
}

void TerminalScreen::insertLines(int n)
{
    if (m_row < m_scrollTop || m_row > m_scrollBottom)
        return;
    n = qMin(n, m_scrollBottom - m_row + 1);
    for (int i = 0; i < n; ++i) {
        m_screen.removeAt(m_scrollBottom);
        m_screen.insert(m_row, blankLine());
    }
    carriageReturn();
}

void TerminalScreen::deleteLines(int n)
{
    if (m_row < m_scrollTop || m_row > m_scrollBottom)
        return;
    n = qMin(n, m_scrollBottom - m_row + 1);
    for (int i = 0; i < n; ++i) {
        m_screen.removeAt(m_row);
        m_screen.insert(m_scrollBottom, blankLine());
    }
    carriageReturn();
}

void TerminalScreen::insertChars(int n)
{
    Line &l = m_screen[m_row];
    const int col = qMin(m_col, m_cols - 1);
    n = qMin(n, m_cols - col);
    l.remove(m_cols - n, n);
    l.insert(col, n, blankCell());
}

void TerminalScreen::deleteChars(int n)
{
    Line &l = m_screen[m_row];
    const int col = qMin(m_col, m_cols - 1);
    n = qMin(n, m_cols - col);
    l.remove(col, n);
    l.append(Line(n, blankCell()));
}

void TerminalScreen::eraseChars(int n)
{
    Line &l = m_screen[m_row];
    const int col = qMin(m_col, m_cols - 1);
    for (int i = col; i < qMin(m_cols, col + n); ++i)
        l[i] = blankCell();
}

// --- Escape / CSI dispatch --------------------------------------------------

void TerminalScreen::handleEscape(char32_t c)
{
    switch (c) {
    case '7': m_savedRow = m_row; m_savedCol = qMin(m_col, m_cols - 1); m_savedPen = m_pen; break;
    case '8': m_row = qMin(m_savedRow, m_rows - 1); m_col = qMin(m_savedCol, m_cols - 1); m_pen = m_savedPen; m_wrapPending = false; break;
    case 'D': lineFeed(); break;
    case 'E': carriageReturn(); lineFeed(); break;
    case 'M': reverseIndex(); break;
    case 'H': if (m_col < m_tabStops.size()) m_tabStops[qMin(m_col, m_cols - 1)] = true; break;
    case 'c': reset(); break;
    case '=': case '>': break; // keypad modes
    default: break;
    }
}

void TerminalScreen::handleOsc()
{
    const int semi = m_oscBuf.indexOf(QLatin1Char(';'));
    if (semi < 0)
        return;
    const int code = m_oscBuf.left(semi).toInt();
    if (code == 0 || code == 2) {
        m_title = m_oscBuf.mid(semi + 1);
        emit titleChanged(m_title);
    }
}

void TerminalScreen::setMode(bool priv, int mode, bool on)
{
    if (!priv)
        return;
    switch (mode) {
    case 1: m_appCursor = on; break;
    case 6: m_originMode = on; moveTo(0, 0); break;
    case 7: m_autoWrap = on; break;
    case 25: m_cursorVisible = on; break;
    case 47: case 1047: enterAlt(on, false); break;
    case 1049: enterAlt(on, true); break;
    case 1000: case 1002: case 1003: m_mouseTracking = on; break;
    case 1006: m_sgrMouse = on; break;
    case 2004: m_bracketedPaste = on; break;
    default: break;
    }
}

void TerminalScreen::enterAlt(bool on, bool saveCursor)
{
    if (on == m_alt)
        return;
    if (on) {
        if (saveCursor) {
            m_savedRow = m_row;
            m_savedCol = qMin(m_col, m_cols - 1);
            m_savedPen = m_pen;
        }
        m_savedPrimary = m_screen;
        m_screen = QVector<Line>(m_rows, blankLine());
        m_alt = true;
    } else {
        m_screen = m_savedPrimary;
        m_savedPrimary.clear();
        m_alt = false;
        if (saveCursor) {
            m_row = qMin(m_savedRow, m_rows - 1);
            m_col = qMin(m_savedCol, m_cols - 1);
            m_pen = m_savedPen;
        }
    }
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_wrapPending = false;
}

void TerminalScreen::handleCsi(char32_t f)
{
    const int p0 = param(0, 1);
    switch (f) {
    case 'A': m_row = qMax(m_scrollTop > m_row ? 0 : m_scrollTop, m_row - p0); m_wrapPending = false; break;
    case 'B': m_row = qMin(m_row + p0, m_row > m_scrollBottom ? m_rows - 1 : m_scrollBottom); m_wrapPending = false; break;
    case 'C': m_col = qMin(qMin(m_col, m_cols - 1) + p0, m_cols - 1); m_wrapPending = false; break;
    case 'D': m_col = qMax(0, qMin(m_col, m_cols - 1) - p0); m_wrapPending = false; break;
    case 'E': m_row = qMin(m_row + p0, m_rows - 1); carriageReturn(); break;
    case 'F': m_row = qMax(0, m_row - p0); carriageReturn(); break;
    case 'G': case '`': m_col = qBound(0, p0 - 1, m_cols - 1); m_wrapPending = false; break;
    case 'H': case 'f': moveTo(param(0, 1) - 1, param(1, 1) - 1); break;
    case 'd': moveTo(p0 - 1, qMin(m_col, m_cols - 1)); break;
    case 'J': eraseInDisplay(qMax(0, m_params.value(0, 0))); break;
    case 'K': eraseInLine(qMax(0, m_params.value(0, 0))); break;
    case 'L': insertLines(p0); break;
    case 'M': deleteLines(p0); break;
    case '@': insertChars(p0); break;
    case 'P': deleteChars(p0); break;
    case 'X': eraseChars(p0); break;
    case 'S': scrollUp(p0); break;
    case 'T': scrollDown(p0); break;
    case 'm': applySgr(); break;
    case 'r': {
        const int top = param(0, 1) - 1;
        const int bottom = param(1, m_rows) - 1;
        if (top < bottom && bottom < m_rows) {
            m_scrollTop = top;
            m_scrollBottom = bottom;
        } else {
            m_scrollTop = 0;
            m_scrollBottom = m_rows - 1;
        }
        moveTo(0, 0);
        break;
    }
    case 's': m_savedRow = m_row; m_savedCol = qMin(m_col, m_cols - 1); break;
    case 'u': m_row = qMin(m_savedRow, m_rows - 1); m_col = qMin(m_savedCol, m_cols - 1); m_wrapPending = false; break;
    case 'g': if (m_params.value(0, 0) == 3) m_tabStops.fill(false); break;
    case 'n':
        if (m_params.value(0) == 5)
            emit reply(QByteArrayLiteral("\x1b[0n"));
        else if (m_params.value(0) == 6)
            emit reply(QStringLiteral("\x1b[%1;%2R").arg(m_row + 1).arg(qMin(m_col, m_cols - 1) + 1).toUtf8());
        break;
    case 'c':
        if (m_params.value(0, 0) <= 0)
            emit reply(QByteArrayLiteral("\x1b[?1;2c"));
        break;
    case 'h': case 'l': break; // ANSI modes we don't track (insert mode etc.)
    default: break;
    }
}

void TerminalScreen::applySgr()
{
    for (int i = 0; i < m_params.size(); ++i) {
        int p = m_params.at(i);
        if (p < 0)
            p = 0;
        switch (p) {
        case 0: m_pen = Cell(); break;
        case 1: m_pen.attr |= Bold; break;
        case 2: m_pen.attr |= Dim; break;
        case 3: m_pen.attr |= Italic; break;
        case 4: m_pen.attr |= Underline; break;
        case 7: m_pen.attr |= Inverse; break;
        case 22: m_pen.attr &= ~(Bold | Dim); break;
        case 23: m_pen.attr &= ~Italic; break;
        case 24: m_pen.attr &= ~Underline; break;
        case 27: m_pen.attr &= ~Inverse; break;
        case 39: m_pen.fg = 0; break;
        case 49: m_pen.bg = 0; break;
        case 38: case 48: {
            quint32 &target = (p == 38) ? m_pen.fg : m_pen.bg;
            const int kind = m_params.value(i + 1, -1);
            if (kind == 5 && i + 2 < m_params.size()) {
                target = 0x02000000u | quint32(qBound(0, m_params.at(i + 2), 255));
                i += 2;
            } else if (kind == 2 && i + 4 < m_params.size()) {
                // Skip an optional colour-space id when sub-parameters were flattened (38:2::r:g:b).
                int j = i + 2;
                if (i + 5 < m_params.size() && m_params.at(j) < 0)
                    ++j;
                const quint32 r = qBound(0, m_params.value(j), 255);
                const quint32 g = qBound(0, m_params.value(j + 1), 255);
                const quint32 b = qBound(0, m_params.value(j + 2), 255);
                target = 0x01000000u | (r << 16) | (g << 8) | b;
                i = j + 2;
            }
            break;
        }
        default:
            if (p >= 30 && p <= 37)
                m_pen.fg = 0x02000000u | quint32(p - 30);
            else if (p >= 40 && p <= 47)
                m_pen.bg = 0x02000000u | quint32(p - 40);
            else if (p >= 90 && p <= 97)
                m_pen.fg = 0x02000000u | quint32(p - 90 + 8);
            else if (p >= 100 && p <= 107)
                m_pen.bg = 0x02000000u | quint32(p - 100 + 8);
            break;
        }
    }
}

QString TerminalScreen::textInRange(int startLine, int startCol, int endLine, int endCol) const
{
    if (startLine > endLine || (startLine == endLine && startCol > endCol)) {
        qSwap(startLine, endLine);
        qSwap(startCol, endCol);
    }
    QString out;
    for (int ln = startLine; ln <= endLine && ln < totalLines(); ++ln) {
        const Line &l = lineAt(ln);
        const int from = ln == startLine ? startCol : 0;
        int to = ln == endLine ? qMin(endCol + 1, int(l.size())) : l.size();
        QString row;
        for (int c = from; c < to; ++c)
            row += QString::fromUcs4(&l.at(c).ch, 1);
        // Trailing blanks are padding, not text.
        while (row.endsWith(QLatin1Char(' ')) && (ln != endLine || to >= l.size()))
            row.chop(1);
        out += row;
        if (ln != endLine)
            out += QLatin1Char('\n');
    }
    return out;
}
