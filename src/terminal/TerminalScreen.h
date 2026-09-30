#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

// Terminal state machine: consumes the byte stream a shell writes to its pty and
// maintains a character grid + scrollback. Contains no GUI code.
class TerminalScreen : public QObject
{
    Q_OBJECT
public:
    enum Attr : quint8 { Bold = 1, Dim = 2, Italic = 4, Underline = 8, Inverse = 16 };

    // Colours: 0 = default, 0x01rrggbb = truecolour, 0x02000000|n = palette index.
    struct Cell {
        char32_t ch = U' ';
        quint32 fg = 0;
        quint32 bg = 0;
        quint8 attr = 0;
    };
    using Line = QVector<Cell>;

    explicit TerminalScreen(int cols = 80, int rows = 24, QObject *parent = nullptr);

    void feed(const QByteArray &data);
    void resize(int cols, int rows);
    void clearScrollback();
    void reset();

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    int scrollbackSize() const { return m_scrollback.size(); }
    int totalLines() const { return m_scrollback.size() + m_rows; }
    const Line &lineAt(int absoluteLine) const;
    int cursorRow() const { return m_row; }
    int cursorCol() const { return qMin(m_col, m_cols - 1); }
    bool cursorVisible() const { return m_cursorVisible; }
    bool applicationCursorKeys() const { return m_appCursor; }
    bool bracketedPaste() const { return m_bracketedPaste; }
    bool isAlternateScreen() const { return m_alt; }
    QString title() const { return m_title; }

    QString textInRange(int startLine, int startCol, int endLine, int endCol) const;

    static constexpr int kMaxScrollback = 10000;

signals:
    void changed();
    void titleChanged(const QString &title);
    void reply(const QByteArray &data); // answers to queries such as cursor-position reports

private:
    enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, StringIgnore, StringEsc };

    void processChar(char32_t c);
    void print(char32_t c);
    void execute(char32_t c);
    void handleCsi(char32_t final);
    void handleEscape(char32_t c);
    void handleOsc();
    void setMode(bool priv, int mode, bool on);
    void applySgr();

    void lineFeed();
    void reverseIndex();
    void scrollUp(int n);
    void scrollDown(int n);
    void carriageReturn() { m_col = 0; m_wrapPending = false; }
    void moveTo(int row, int col);
    void eraseInLine(int mode);
    void eraseInDisplay(int mode);
    void insertLines(int n);
    void deleteLines(int n);
    void insertChars(int n);
    void deleteChars(int n);
    void eraseChars(int n);
    Line blankLine() const;
    Cell blankCell() const;
    int param(int i, int def) const;
    void enterAlt(bool on, bool saveCursor);

    int m_cols, m_rows;
    QVector<Line> m_screen;
    QVector<Line> m_savedPrimary;
    QList<Line> m_scrollback;
    int m_row = 0, m_col = 0;
    bool m_wrapPending = false;
    int m_scrollTop = 0, m_scrollBottom = 0;
    Cell m_pen;
    int m_savedRow = 0, m_savedCol = 0;
    Cell m_savedPen;
    bool m_cursorVisible = true;
    bool m_appCursor = false;
    bool m_autoWrap = true;
    bool m_bracketedPaste = false;
    bool m_originMode = false;
    bool m_alt = false;
    QVector<bool> m_tabStops;
    QString m_title;

    State m_state = State::Ground;
    QString m_csiBuf;
    QString m_oscBuf;
    QVector<int> m_params;
    char32_t m_intermediate = 0;
    QByteArray m_pending; // incomplete UTF-8 sequence carried between feeds
};
