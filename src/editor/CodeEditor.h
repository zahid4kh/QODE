#pragma once

#include "git/GitDiff.h"

#include <QElapsedTimer>
#include <QPlainTextEdit>

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

    // --- Brackets -----------------------------------------------------------
    void gotoMatchingBracket();

signals:
    void filesDropped(const QStringList &paths);
    void searchResultsChanged();
    void overwriteModeToggled();

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

    QColor m_gutterBg, m_gutterFg, m_gutterActive, m_currentLine, m_matchBg, m_border;
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
