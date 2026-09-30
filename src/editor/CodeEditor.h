#pragma once

#include "git/GitDiff.h"
#include "git/GitTypes.h"

#include <QElapsedTimer>
#include <QPlainTextEdit>

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

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
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
    void trackBlameEdit(int position, int removed, int added);
    void positionMinimap();
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
