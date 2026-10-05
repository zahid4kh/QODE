#include "TerminalView.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>

namespace {

QColor cubeColor(int i)
{
    if (i < 16)
        return QColor();
    if (i < 232) {
        i -= 16;
        const int r = i / 36, g = (i / 6) % 6, b = i % 6;
        auto lvl = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return QColor(lvl(r), lvl(g), lvl(b));
    }
    const int v = 8 + (i - 232) * 10;
    return QColor(v, v, v);
}

} // namespace

TerminalView::TerminalView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    m_screen = new TerminalScreen(80, 24, this);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAutoFillBackground(false);
    verticalScrollBar()->setSingleStep(1);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(m_screen, &TerminalScreen::changed, this, &TerminalView::onScreenChanged);
    connect(m_screen, &TerminalScreen::reply, this, &TerminalView::input);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        m_atBottom = v >= verticalScrollBar()->maximum();
        viewport()->update();
    });
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, &TerminalView::applySettings);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { applySettings(); });
    applySettings();
}

void TerminalView::applySettings()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_bg = t.termBg;
    m_fg = t.termFg;
    for (int i = 0; i < 16; ++i)
        m_ansi[i] = t.ansi[i];
    m_sel = t.selection;
    QPalette p = palette();
    p.setColor(QPalette::Base, m_bg);
    setPalette(p);
    m_font = SettingsManager::instance().editorFont();
    updateMetrics();
    recalcSize();
    viewport()->update();
}

void TerminalView::updateMetrics()
{
    const QFontMetricsF fm(m_font);
    m_cw = fm.horizontalAdvance(QLatin1Char('M'));
    m_ch = qCeil(fm.height());
    m_ascent = qRound(fm.ascent());
}

void TerminalView::recalcSize()
{
    const int cols = qMax(2, int(viewport()->width() / m_cw));
    const int rows = qMax(1, int(viewport()->height() / m_ch));
    if (cols != m_screen->cols() || rows != m_screen->rows()) {
        m_screen->resize(cols, rows);
        emit sizeChanged(cols, rows);
    }
    updateScrollBar();
}

void TerminalView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    recalcSize();
}

void TerminalView::updateScrollBar()
{
    QScrollBar *sb = verticalScrollBar();
    const bool follow = m_atBottom;
    // The alternate screen (vim, less, TUIs) has no scrollback: pin the view to it.
    const int back = m_screen->scrollbackSize();
    sb->setRange(m_screen->isAlternateScreen() ? back : 0, back);
    sb->setPageStep(m_screen->rows());
    if (follow || m_screen->isAlternateScreen())
        sb->setValue(sb->maximum());
}

void TerminalView::onScreenChanged()
{
    updateScrollBar();
    viewport()->update();
}

void TerminalView::scrollToBottom()
{
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    m_atBottom = true;
}

// --- Painting ----------------------------------------------------------------

QColor TerminalView::resolve(quint32 c, bool fg) const
{
    if (c == 0)
        return fg ? m_fg : m_bg;
    if (c & 0x01000000u)
        return QColor((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
    const int idx = int(c & 0xff);
    return idx < 16 ? m_ansi[idx] : cubeColor(idx);
}

void TerminalView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), m_bg);
    p.setFont(m_font);

    const int first = verticalScrollBar()->value();
    const int rows = m_screen->rows();
    const QPoint selA = qMakePair(m_selStart.y(), m_selStart.x()) <= qMakePair(m_selEnd.y(), m_selEnd.x()) ? m_selStart : m_selEnd;
    const QPoint selB = selA == m_selStart ? m_selEnd : m_selStart;
    const bool hasSel = selectionActive();
    const QColor selColor = m_sel;

    for (int r = 0; r < rows; ++r) {
        const int abs = first + r;
        if (abs >= m_screen->totalLines())
            break;
        const TerminalScreen::Line &line = m_screen->lineAt(abs);
        const qreal y = r * m_ch;
        for (int c = 0; c < line.size(); ++c) {
            const TerminalScreen::Cell &cell = line.at(c);
            quint32 fgc = cell.fg, bgc = cell.bg;
            QColor fg = resolve(fgc, true), bg = resolve(bgc, false);
            if (cell.attr & TerminalScreen::Inverse)
                qSwap(fg, bg);
            if ((cell.attr & TerminalScreen::Bold) && (fgc & 0x02000000u) && (fgc & 0xff) < 8)
                fg = m_ansi[(fgc & 0xff) + 8]; // bold brightens the base colours
            if (cell.attr & TerminalScreen::Dim)
                fg.setAlphaF(0.6);

            bool selected = false;
            if (hasSel) {
                const QPair<int, int> pos(abs, c);
                selected = pos >= qMakePair(selA.y(), selA.x()) && pos <= qMakePair(selB.y(), selB.x());
            }
            const QRectF rect(c * m_cw, y, m_cw, m_ch);
            if (selected)
                p.fillRect(rect, selColor);
            else if (bg != m_bg)
                p.fillRect(rect, bg);
            if (cell.ch != U' ') {
                QFont f = m_font;
                if (cell.attr & TerminalScreen::Bold) f.setBold(true);
                if (cell.attr & TerminalScreen::Italic) f.setItalic(true);
                if (cell.attr & TerminalScreen::Underline) f.setUnderline(true);
                if (f != m_font)
                    p.setFont(f);
                p.setPen(fg);
                p.drawText(QPointF(rect.left(), y + m_ascent), QString::fromUcs4(&cell.ch, 1));
                if (f != m_font)
                    p.setFont(m_font);
            } else if (cell.attr & TerminalScreen::Underline) {
                p.setPen(fg);
                p.drawLine(QPointF(rect.left(), y + m_ch - 1.5), QPointF(rect.right(), y + m_ch - 1.5));
            }
        }
    }

    // Cursor
    if (m_screen->cursorVisible() && m_atBottom) {
        const int row = m_screen->cursorRow();
        const int col = m_screen->cursorCol();
        const QRectF cr(col * m_cw, row * m_ch, m_cw, m_ch);
        if (m_focused) {
            p.fillRect(cr, m_fg);
            const TerminalScreen::Line &line = m_screen->lineAt(m_screen->scrollbackSize() + row);
            if (col < line.size() && line.at(col).ch != U' ') {
                p.setPen(m_bg);
                p.drawText(QPointF(cr.left(), cr.top() + m_ascent), QString::fromUcs4(&line.at(col).ch, 1));
            }
        } else {
            p.setPen(m_fg);
            p.drawRect(cr.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
}

// --- Selection & clipboard -------------------------------------------------------

QPoint TerminalView::cellAt(const QPoint &pos) const
{
    const int col = qBound(0, int(pos.x() / m_cw), m_screen->cols() - 1);
    const int row = qBound(0, int(pos.y() / m_ch), m_screen->rows() - 1);
    return QPoint(col, verticalScrollBar()->value() + row);
}

QString TerminalView::selectedText() const
{
    if (!selectionActive())
        return {};
    return m_screen->textInRange(m_selStart.y(), m_selStart.x(), m_selEnd.y(), m_selEnd.x());
}

void TerminalView::clearSelection()
{
    m_selStart = m_selEnd = QPoint(0, 0);
    viewport()->update();
}

void TerminalView::selectAll()
{
    m_selStart = QPoint(0, 0);
    m_selEnd = QPoint(m_screen->cols() - 1, m_screen->totalLines() - 1);
    viewport()->update();
}

void TerminalView::copy()
{
    const QString t = selectedText();
    if (!t.isEmpty())
        QApplication::clipboard()->setText(t);
}

void TerminalView::paste()
{
    QString t = QApplication::clipboard()->text();
    if (t.isEmpty())
        return;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\r")).replace(QLatin1Char('\n'), QLatin1Char('\r'));
    QByteArray data = t.toUtf8();
    if (m_screen->bracketedPaste())
        data = QByteArrayLiteral("\x1b[200~") + data + QByteArrayLiteral("\x1b[201~");
    scrollToBottom();
    emit input(data);
}

void TerminalView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_selecting = true;
        m_selStart = m_selEnd = cellAt(e->pos());
        viewport()->update();
    } else if (e->button() == Qt::MiddleButton) {
        // X11-style primary selection paste.
        const QString t = QApplication::clipboard()->text(QClipboard::Selection);
        if (!t.isEmpty())
            emit input(t.toUtf8());
    }
    setFocus();
}

void TerminalView::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_selecting)
        return;
    m_selEnd = cellAt(e->pos());
    // Auto-scroll while dragging outside the viewport.
    if (e->pos().y() < 0)
        verticalScrollBar()->setValue(verticalScrollBar()->value() - 1);
    else if (e->pos().y() > viewport()->height())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + 1);
    viewport()->update();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_selecting = false;
    if (selectionActive()) {
        const QString t = selectedText();
        if (!t.isEmpty() && QApplication::clipboard()->supportsSelection())
            QApplication::clipboard()->setText(t, QClipboard::Selection);
    }
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Select the word under the cursor.
    const QPoint p = cellAt(e->pos());
    const TerminalScreen::Line &l = m_screen->lineAt(p.y());
    auto isWord = [&](int c) { return c >= 0 && c < l.size() && l.at(c).ch != U' '; };
    if (!isWord(p.x()))
        return;
    int a = p.x(), b = p.x();
    while (isWord(a - 1))
        --a;
    while (isWord(b + 1))
        ++b;
    m_selStart = QPoint(a, p.y());
    m_selEnd = QPoint(b, p.y());
    viewport()->update();
}

void TerminalView::wheelEvent(QWheelEvent *e)
{
    const int dy = e->angleDelta().y();
    if (m_screen->mouseReporting() || m_screen->isAlternateScreen()) {
        if (!m_shellActive || dy == 0) {
            e->accept();
            return;
        }
        // Accumulate so smooth-scrolling touchpads still produce whole steps.
        m_wheelAcc += dy;
        const int steps = m_wheelAcc / 40;
        m_wheelAcc -= steps * 40;
        const bool up = steps > 0;
        const QPoint cell = cellAt(e->position().toPoint());
        const int col = qBound(1, cell.x() + 1, 223), row = qBound(1, cell.y() - verticalScrollBar()->value() + 1, 223);
        QByteArray out;
        for (int i = 0; i < qAbs(steps); ++i) {
            if (m_screen->mouseReporting()) {
                const int btn = up ? 64 : 65;
                if (m_screen->sgrMouse())
                    out += QByteArray("\x1b[<") + QByteArray::number(btn) + ';' + QByteArray::number(col) + ';' + QByteArray::number(row) + 'M';
                else
                    out += QByteArray("\x1b[M") + char(32 + btn) + char(32 + col) + char(32 + row);
            } else {
                // No mouse mode on the alternate screen: behave like arrow keys (xterm's alternateScroll).
                out += QByteArray(m_screen->applicationCursorKeys() ? "\x1bO" : "\x1b[") + (up ? 'A' : 'B');
            }
        }
        if (!out.isEmpty())
            emit input(out);
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void TerminalView::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    QAction *copyAct = menu.addAction(tr("Copy"), this, &TerminalView::copy);
    copyAct->setEnabled(selectionActive());
    menu.addAction(tr("Paste"), this, &TerminalView::paste);
    menu.addAction(tr("Select All"), this, &TerminalView::selectAll);
    menu.exec(e->globalPos());
}

// --- Keyboard ----------------------------------------------------------------

void TerminalView::focusInEvent(QFocusEvent *e)
{
    m_focused = true;
    QAbstractScrollArea::focusInEvent(e);
    emit focusGained();
    viewport()->update();
}

void TerminalView::focusOutEvent(QFocusEvent *e)
{
    m_focused = false;
    QAbstractScrollArea::focusOutEvent(e);
    viewport()->update();
}

bool TerminalView::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        // The shell wants keys like Ctrl+C/Ctrl+W/Ctrl+F; only let a few app-wide shortcuts through.
        auto *k = static_cast<QKeyEvent *>(e);
        const Qt::KeyboardModifiers m = k->modifiers();
        const int key = k->key();
        const bool passThrough =
            (key == Qt::Key_J && m == Qt::ControlModifier) ||
            (key == Qt::Key_Q && m == Qt::ControlModifier) ||
            (key == Qt::Key_B && m == Qt::ControlModifier) ||
            (key == Qt::Key_F11) ||
            ((key == Qt::Key_Tab || key == Qt::Key_Backtab) && (m & Qt::ControlModifier)) ||
            ((m & Qt::ControlModifier) && (m & Qt::ShiftModifier) && key != Qt::Key_C && key != Qt::Key_V);
        if (!passThrough) {
            e->accept();
            return true;
        }
    } else if (e->type() == QEvent::KeyPress) {
        // Tab must reach keyPressEvent instead of moving focus.
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) {
            keyPressEvent(k);
            return true;
        }
    }
    return QAbstractScrollArea::event(e);
}

QByteArray TerminalView::encodeKey(QKeyEvent *e) const
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const bool shift = m & Qt::ShiftModifier, alt = m & Qt::AltModifier, ctrl = m & Qt::ControlModifier;
    const int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    const bool app = m_screen->applicationCursorKeys();

    auto csi = [&](char final, const char *num = "1") -> QByteArray {
        if (mod > 1)
            return QByteArrayLiteral("\x1b[") + num + ';' + QByteArray::number(mod) + final;
        return (app && final >= 'A' && final <= 'D') || (app && (final == 'H' || final == 'F'))
                   ? QByteArrayLiteral("\x1bO") + final
                   : QByteArrayLiteral("\x1b[") + final;
    };
    auto tilde = [&](int n) -> QByteArray {
        return mod > 1 ? QByteArrayLiteral("\x1b[") + QByteArray::number(n) + ';' + QByteArray::number(mod) + '~'
                       : QByteArrayLiteral("\x1b[") + QByteArray::number(n) + '~';
    };
    auto ss3 = [&](char c) -> QByteArray {
        return mod > 1 ? QByteArrayLiteral("\x1b[1;") + QByteArray::number(mod) + c : QByteArrayLiteral("\x1bO") + c;
    };

    switch (e->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: return alt ? QByteArrayLiteral("\x1b\r") : QByteArrayLiteral("\r");
    case Qt::Key_Backspace: return alt ? QByteArrayLiteral("\x1b\x7f") : (ctrl ? QByteArrayLiteral("\x08") : QByteArrayLiteral("\x7f"));
    case Qt::Key_Tab: return QByteArrayLiteral("\t");
    case Qt::Key_Backtab: return QByteArrayLiteral("\x1b[Z");
    case Qt::Key_Escape: return QByteArrayLiteral("\x1b");
    case Qt::Key_Up: return csi('A');
    case Qt::Key_Down: return csi('B');
    case Qt::Key_Right: return csi('C');
    case Qt::Key_Left: return csi('D');
    case Qt::Key_Home: return csi('H');
    case Qt::Key_End: return csi('F');
    case Qt::Key_Insert: return tilde(2);
    case Qt::Key_Delete: return tilde(3);
    case Qt::Key_PageUp: return tilde(5);
    case Qt::Key_PageDown: return tilde(6);
    case Qt::Key_F1: return ss3('P');
    case Qt::Key_F2: return ss3('Q');
    case Qt::Key_F3: return ss3('R');
    case Qt::Key_F4: return ss3('S');
    case Qt::Key_F5: return tilde(15);
    case Qt::Key_F6: return tilde(17);
    case Qt::Key_F7: return tilde(18);
    case Qt::Key_F8: return tilde(19);
    case Qt::Key_F9: return tilde(20);
    case Qt::Key_F10: return tilde(21);
    case Qt::Key_F11: return tilde(23);
    case Qt::Key_F12: return tilde(24);
    default: break;
    }

    if (ctrl) {
        const int k = e->key();
        if (k >= Qt::Key_A && k <= Qt::Key_Z)
            return QByteArray(1, char(k - Qt::Key_A + 1));
        switch (k) {
        case Qt::Key_Space: case Qt::Key_At: return QByteArray(1, '\0');
        case Qt::Key_BracketLeft: return QByteArray(1, '\x1b');
        case Qt::Key_Backslash: return QByteArray(1, '\x1c');
        case Qt::Key_BracketRight: return QByteArray(1, '\x1d');
        case Qt::Key_Slash: case Qt::Key_Underscore: return QByteArray(1, '\x1f');
        default: return {};
        }
    }
    QByteArray text = e->text().toUtf8();
    if (text.isEmpty() || (uchar(text.at(0)) < 0x20 && text.size() == 1))
        return {};
    return alt ? QByteArrayLiteral("\x1b") + text : text;
}

void TerminalView::keyPressEvent(QKeyEvent *e)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const int key = e->key();

    // Clipboard
    if ((m == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_C) ||
        (m == Qt::ControlModifier && key == Qt::Key_C && selectionActive())) {
        copy();
        clearSelection();
        return;
    }
    if ((m == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_V) ||
        (m == Qt::ControlModifier && key == Qt::Key_V) || (m == Qt::ShiftModifier && key == Qt::Key_Insert)) {
        paste();
        return;
    }
    // Scrollback navigation
    if (m == Qt::ShiftModifier && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        QScrollBar *sb = verticalScrollBar();
        sb->setValue(sb->value() + (key == Qt::Key_PageUp ? -1 : 1) * m_screen->rows() / 2);
        return;
    }

    if (!m_shellActive) {
        if (key == Qt::Key_Return || key == Qt::Key_Enter)
            emit returnPressedWhileInactive();
        return;
    }
    const QByteArray bytes = encodeKey(e);
    if (bytes.isEmpty()) {
        QAbstractScrollArea::keyPressEvent(e);
        return;
    }
    clearSelection();
    scrollToBottom();
    emit input(bytes);
}

void TerminalView::inputMethodEvent(QInputMethodEvent *e)
{
    if (!e->commitString().isEmpty() && m_shellActive) {
        scrollToBottom();
        emit input(e->commitString().toUtf8());
    }
}
