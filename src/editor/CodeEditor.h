#pragma once

#include <functional>
#include "git/GitDiff.h"
#include "git/GitTypes.h"
#include "lsp/LspTypes.h"

#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QVector>

class MiniMap;
class QTextBlock;
class QTimer;

class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);

    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);
    // Ctrl+hover underlines identifiers only while this returns true (a language server serves the file).
    void setDefinitionAvailable(std::function<bool()> check) { m_canGoToDefinition = std::move(check); }

    int currentLine() const { return textCursor().blockNumber() + 1; }
    int currentColumn() const { return textCursor().positionInBlock() + 1; }

    // --- Search -------------------------------------------------------------
    void setSearchTerm(const QString &term, bool caseSensitive);
    int matchCount() const { return m_matches.size(); }
    int currentMatchIndex() const; // 1-based, 0 when the selection is not a match
    bool findNext(bool backwards = false);
    bool replaceCurrent(const QString &replacement);
    int replaceAll(const QString &replacement);

    void setLanguage(const QString &name) { m_language = name; viewport()->update(); }
    void setIndentAfterColon(bool on) { m_indentAfterColon = on; } // Python-style blocks
    // Shows `doc` and (re)applies the editor font and tab stops to it: a QTextDocument keeps its own
    // default font, so a plain setDocument() would render in the application (UI) font.
    void attachDocument(QTextDocument *doc);
    void applySettings(); // font, tab size, wrapping from SettingsManager
    void applyTheme();

    // --- Git change markers -------------------------------------------------
    // The committed (HEAD) text of this file; the gutter marks lines that differ from it.
    void setDiffBase(const QStringList &lines);
    void clearDiffBase();
    bool hasDiffBase() const { return m_hasBase; }
    int changeCount() const { return m_hunks.size(); }
    void gotoChange(bool next);
    void gutterClicked(const QPoint &pos);
    const QVector<GitDiff::Hunk> &hunks() const { return m_hunks; }

    // --- Git blame ------------------------------------------------------------
    // Per-line authorship (one entry per line). The gutter column and the caret-line annotation are
    // separate options; unsaved edits keep the list aligned and show as uncommitted.
    void setBlame(const QVector<GitBlameLine> &lines);
    void clearBlame();
    bool hasBlame() const { return !m_blame.isEmpty(); }
    void setBlameGutter(bool on);
    void setBlameInline(bool on);

    // --- Language server diagnostics ---------------------------------------------------------------
    // Squiggles under the ranges, a tinted line number and a tooltip. The server re-sends the list after
    // every edit, so the ranges only have to survive until then.
    void setDiagnostics(const QVector<LspDiagnostic> &diagnostics);
    // Shows the language server's hover text (Markdown) for the tooltip requested at hoverRequested(); ignored when
    // the mouse has moved on. Diagnostics under the mouse stay in the tooltip too.
    void showHover(const QString &markdown);
    const QVector<LspDiagnostic> &diagnostics() const { return m_diagnostics; }

    // --- Bookmarks ----------------------------------------------------------
    // 0-based, sorted lines. They follow the text while it is edited; setBookmarks() does not emit.
    const QList<int> &bookmarks() const { return m_bookmarks; }
    void setBookmarks(const QList<int> &lines);
    void toggleBookmark(int line = -1); // -1: the caret line

    // --- Minimap support ------------------------------------------------------
    void visibleBlockRange(int *first, int *last) const; // block numbers on screen
    void scrollBlockToCenter(int blockNumber);
    int minimapWidth() const;

    // --- Brackets -----------------------------------------------------------
    void gotoMatchingBracket();

    // --- Code folding -------------------------------------------------------
    // Regions come from unmatched brackets at the end of a line, multi-line /* comments */ and
    // deeper-indented lines. The fold state lives in each header block's user data.
    void foldCurrent();    // fold the innermost region around the caret
    void unfoldCurrent();
    void foldAll();
    void unfoldAll();
    void toggleFoldAt(int blockNumber);
    bool hasFolds() const { return m_foldedCount > 0; }
    void setGutterHover(bool on);

signals:
    void filesDropped(const QStringList &paths);
    void searchResultsChanged();
    void overwriteModeToggled();
    void blameCommitRequested(const QString &hash);
    void bookmarksChanged();
    void hoverRequested(int line, int column); // mouse rests on an identifier (0-based line / UTF-16 column)
    void definitionRequested(int line, int column); // Ctrl+click on an identifier

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

private:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect &rect, int dy);
    void refreshSelections();
    void paintIndentGuides();
    int indentDepth(const QTextBlock &block, bool *blank = nullptr) const; // guides to the left of the text
    int effectiveDepth(const QTextBlock &block) const;                      // blank lines borrow from neighbours
    void updateGuideScope();

    bool isFolded(const QTextBlock &block) const;
    void setFolded(const QTextBlock &block, bool folded);
    bool isFoldable(const QTextBlock &block) const;
    int foldEnd(const QTextBlock &header) const; // last block number hidden by a fold at `header`, or -1
    int unmatchedOpener(const QTextBlock &block) const;
    QTextBlock foldHeaderFor(const QTextBlock &block, bool foldedOnly) const;
    void applyFolds();
    void paintFoldMarkers();
    QList<int> stickyLines() const; // header blocks pinned at the top of the viewport
    void paintStickyScroll();
    void paintBlameAnnotation();
    int blameWidth() const { return m_blameGutter && !m_blame.isEmpty() ? m_blameColumn : 0; }
    void trackLineEdit(int position, int removed, int added); // keeps blame and bookmarks aligned
    void positionMinimap();
    // Document range [first, second) of a diagnostic, clamped to the current text; first == -1 when invalid.
    QPair<int, int> diagnosticRange(const LspDiagnostic &d) const;
    QColor diagnosticColor(int severity) const;
    void appendBracketSelections(QList<QTextEdit::ExtraSelection> &extra) const;
    int bracketNearCursor() const;              // document position of the bracket at/before the cursor, or -1
    int findMatchingBracket(int pos) const;     // position of its partner, or -1
    void recomputeMatches();
    QString indentUnit() const;
    void insertNewlineWithIndent();
    void indentSelection(bool outdent);
    void handleBackspaceInIndent(QKeyEvent *event);
    bool selectionIsMatch() const;
    void recomputeDiff();
    void showHunkPopup(int hunkIndex, const QPoint &globalPos);
    void revertHunk(int hunkIndex);

    QWidget *m_lineArea;
    QString m_term;
    bool m_caseSensitive = false;
    QVector<QPair<int, int>> m_matches; // (start, end)
    QTimer *m_matchTimer;
    QElapsedTimer m_tripleClickTimer;
    bool m_tripleClickArmed = false;
    bool m_indentAfterColon = false;
    QString m_language;
    QVector<GitBlameLine> m_blame;
    QList<int> m_bookmarks;
    QVector<LspDiagnostic> m_diagnostics;
    void updateLink(bool ctrlDown);
    std::function<bool()> m_canGoToDefinition;
    QPair<int, int> m_link{-1, -1}; // identifier under the mouse while Ctrl is held
    int m_hoverPos = -1;       // document position of the pending hover request
    QPoint m_hoverGlobal;      // where its tooltip goes
    QString m_hoverDiagnostics; // diagnostics tooltip HTML at that position
    QHash<int, int> m_diagnosticLines; // line -> most severe severity (lowest number)
    bool m_blameGutter = false;
    bool m_blameInline = true;
    int m_blameColumn = 190;
    int m_blameBlocks = 0;
    QMetaObject::Connection m_blameConn;
    bool m_stickyScroll = true;
    QVector<QPair<QRect, int>> m_stickyRows; // last paint -> header block number

    QColor m_gutterBg, m_gutterFg, m_gutterActive, m_currentLine, m_matchBg, m_border;
    MiniMap *m_minimap;
    bool m_showMinimap = true;
    bool m_gutterHover = false;
    int m_foldedCount = 0;
    QTimer *m_foldTimer;
    QVector<QPair<QRect, int>> m_foldPills; // inline "…" markers of the last paint -> header block number
    QColor m_bracketOk, m_bracketBad, m_guide, m_guideActive;
    bool m_indentGuides = true;
    struct { int level = -1, first = 0, last = -1; } m_guideScope; // the guide of the caret's block, and the blocks it spans
    QColor m_markAdded, m_markModified, m_markDeleted, m_diffAddBg, m_diffDelBg;

    bool m_hasBase = false;
    QStringList m_base;
    QVector<GitDiff::Hunk> m_hunks;
    QHash<int, int> m_hunkAtLine; // line -> hunk index (added/modified lines)
    QHash<int, int> m_deletedAt;  // line above which lines were removed -> hunk index
    QTimer *m_diffTimer;
};
