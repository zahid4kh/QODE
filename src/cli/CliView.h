#pragma once

#include "CliCommands.h"
#include "terminal/TerminalScreen.h"

#include <QAbstractScrollArea>
#include <QKeySequence>
#include <QPointer>

class CliSession;
class QTimer;

// The whole window in Terminal Only mode: scrollback made of blocks (the command line, its output, how it
// ended), a styled input line with history and completion, and a status bar. Shell commands run in a
// CliSession; lines starting with "/" are QODE commands (CliCommands).
class CliView : public QAbstractScrollArea, public CliHost
{
    Q_OBJECT
public:
    explicit CliView(QWidget *parent = nullptr);
    ~CliView() override;

    // Opens (or switches to) a project: restarts the shell in its root and loads its history. "" = none.
    void setProject(const QString &root, const QString &name);
    // The mode was shown: starts the shell if needed and takes the keyboard focus.
    void activate();
    void setToggleShortcut(const QString &text);
    bool hasRunningProgram() const;
    // Files open in the editor, shown in the prompt line (`modified` = with unsaved changes).
    void setOpenFiles(int open, int modified);
    int openFileCount() const override { return m_openFiles; }
    // Asked once per fresh shell (argument: shell name); a non-empty answer is run before the first prompt is used
    // (e.g. activating a Python virtualenv).
    void setStartupCommandProvider(std::function<QString(const QString &)> provider) { m_startupProvider = std::move(provider); }

    // CliHost
    QString projectRoot() const override { return m_root; }
    QString projectName() const override { return m_name; }
    QString cwd() const override;
    QString shellName() const override;
    int columns() const override { return m_cols; }
    QStringList history() const override { return m_history; }
    void clearHistory() override;
    void clearScreen() override;
    void restartShell() override;
    void changeDirectory(const QString &directory, std::function<void(int, const QString &)> done) override;
    void leaveMode() override { emit leaveRequested(); }
    void openInEditor(const QString &path, int line) override { emit openFileRequested(path, line); }
    QString toggleShortcut() const override { return m_toggleText; }

signals:
    void leaveRequested();
    void openFileRequested(const QString &path, int line);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;

private:
    using Line = TerminalScreen::Line;

    struct Block {
        QVector<Line> head, body, foot;
        TerminalScreen *live = nullptr; // output while it is still arriving
        int liveRows = 0;
        bool raw = false;               // a shell command: keys go to the program
        bool slash = false;
        int rows() const { return int(head.size()) + (live ? liveRows : int(body.size())) + int(foot.size()); }
        const Line *line(int i) const;
        ~Block() { delete live; }
    };

    // Session and commands
    void ensureSession();
    void startSession();
    void onSessionState();
    void submit(const QString &text);
    void runShell(const QString &text);
    void runSlash(const QString &text);
    Block *addBlock(const QString &headerAnsi, bool raw, bool slash);
    void addNotice(const QString &ansi);
    void freezeBlock(Block *b, int status, qint64 ms);
    void wipe();
    void showBanner();
    void onLiveChanged(Block *b);
    void onBackground(const QByteArray &data);
    void drainTypeahead();
    void refreshBranch();
    QString cwdShort() const;
    bool rawMode() const;
    bool inputVisible() const;

    // History and completion
    void loadHistory();
    void saveHistory() const;
    void pushHistory(const QString &text);
    QString suggestion() const;
    void historyStep(int direction);
    void editedInput();
    void tabComplete(int direction);
    void refreshPopup();
    void hidePopup();
    void applyCandidate(int index);
    void startSearch();
    void updateSearch(bool older);
    void leaveSearch(bool accept);
    bool handleSearchKey(QKeyEvent *e);
    void insertText(const QString &text);
    void deleteRange(int from, int to);
    void moveWord(int direction);
    void handleInputKey(QKeyEvent *e);
    void handleRawKey(QKeyEvent *e);
    bool handleCommonKey(QKeyEvent *e);
    void pasteIntoInput();

    // Layout and painting
    void applySettings();
    void updateMetrics();
    void recalc();
    void relayout();
    void rebuildInput();
    void scrollToEnd();
    const Line *flatLine(int row) const;
    int blockAt(int row) const;
    QColor resolve(quint32 color, bool foreground) const;
    quint32 packed(const QColor &c) const { return 0x01000000u | (quint32(c.red()) << 16) | (quint32(c.green()) << 8) | quint32(c.blue()); }
    void paintLine(QPainter &p, const Line &line, qreal y, int row, bool selectable);
    void paintStatusBar(QPainter &p);
    QPoint cellAt(const QPoint &pos) const; // (column, flat row)
    QString selectedText() const;
    bool selectionActive() const { return m_selA != m_selB; }
    void clearSelection();
    void copySelection();
    int visibleRows() const { return m_visRows; }
    bool altScreen() const;

    CliSession *m_session;
    QVector<Block *> m_blocks;
    Block *m_running = nullptr;
    Block *m_bgBlock = nullptr;     // collects output the shell prints on its own
    QStringList m_queuedLines;      // typed while a QODE command was still running
    QString m_queuedPartial;
    QPointer<CliCall> m_call;
    bool m_clearAfter = false;
    bool m_restartPending = false;
    bool m_startPending = false;
    bool m_started = false;
    bool m_runStartup = false;
    int m_openFiles = 0, m_modifiedFiles = 0;
    std::function<QString(const QString &)> m_startupProvider;

    QString m_root, m_name;
    QString m_branch;
    bool m_dirty = false;
    int m_branchGeneration = 0;
    QString m_toggleText;
    QKeySequence m_toggleKeys;

    // Input line
    QString m_input;
    int m_cursor = 0;
    QStringList m_history;
    int m_histPos = -1;      // index while walking through history, -1 = the draft
    QString m_draft;
    QVector<CliCandidate> m_cands;
    int m_candStart = 0;
    int m_candSel = -1;
    QString m_candCurrent;   // text currently occupying the word being completed
    bool m_popup = false;
    bool m_searching = false;
    QString m_searchQuery;
    int m_searchPos = -1;
    QString m_searchMatch;
    QString m_searchSavedInput;

    // Geometry
    QFont m_font;
    qreal m_cw = 8, m_ch = 16;
    int m_ascent = 12;
    int m_cols = 80, m_visRows = 24;
    int m_padX = 16, m_padTop = 10, m_barH = 26;
    QVector<int> m_starts;
    int m_blockRows = 0;
    QVector<Line> m_inputLines;
    int m_cursorRow = 0, m_cursorCol = 0;
    bool m_follow = true;
    bool m_focused = false;
    int m_wheelAcc = 0;
    bool m_selecting = false;
    QPoint m_selA {0, 0}, m_selB {0, 0}; // (column, flat row)

    // Colours
    QColor m_bg, m_fg, m_sel, m_accent, m_muted, m_panel, m_border, m_ok, m_warn, m_bad;
    QColor m_ansi[16];
};
