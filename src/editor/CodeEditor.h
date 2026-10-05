#pragma once

#include <functional>
#include "git/GitDiff.h"
#include "git/GitTypes.h"
#include "lsp/LspTypes.h"
#include "refactor/MoveRefactor.h"

#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QPointer>
#include <QVector>

class CompletionPopup;
class HoverPopup;
class MiniMap;
class QTextBlock;
class QTimer;

class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);
    ~CodeEditor() override;

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

    // --- Unused imports ---------------------------------------------------------------------------
    // Lines (0-based) of imports the language server says nothing uses: drawn gray, and hovering one offers to remove
    // them all (removeUnusedImportsRequested). The marks follow edits until the next list arrives.
    void setUnusedImports(const QVector<int> &lines);
    QVector<int> unusedImportLines() const;
    void removeLines(const QVector<int> &lines); // whole lines, one undo step

    // --- Completion -------------------------------------------------------------------------------
    // Typing an identifier or '.', '->', '::' (while a server serves the file) or Ctrl+Space emits completionRequested();
    // the answer goes to showCompletions() with the same token (older answers are dropped). Without a server,
    // Ctrl+Space offers the words of the document.
    void triggerCompletion();
    void showCompletions(const QVector<LspCompletionItem> &items, bool incomplete, int token);
    bool completionVisible() const;
    // Called when an item carrying a server command is accepted. Returns true when it took the item over; it then calls
    // done(false) if it could not finish, and the editor inserts the item's text itself.
    using CompletionCommandRunner = std::function<bool(const LspCompletionItem &item, int line, int column, std::function<void(bool done)> finished)>;
    void setCompletionCommandRunner(CompletionCommandRunner runner) { m_commandRunner = std::move(runner); }
    // Completes an item that needs the server's completionItem/resolve before it is inserted (jdtls adds its import
    // there); `finished` gets the item to insert, possibly after a short wait. Only used for items with resolveData.
    using CompletionResolver = std::function<void(const LspCompletionItem &item, std::function<void(const LspCompletionItem &)> finished)>;
    void setCompletionResolver(CompletionResolver resolver) { m_resolver = std::move(resolver); }
    // Items QODE offers itself, without a server (Gradle version catalog accessors); asked with the line text in front of
    // the word being typed. They are shown with the server's items and also make the file count as served for completion.
    using LocalCompletions = std::function<QVector<LspCompletionItem>(const QString &beforeWord)>;
    void setLocalCompletions(LocalCompletions provider) { m_localCompletions = std::move(provider); }
    // Applies server text edits (0-based line / UTF-16 column, positions as of the text the server saw) as one undo step.
    bool applyTextEdits(const QVector<LspTextEdit> &edits);
    // Replaces character ranges (offsets into the document text, ascending, not overlapping) as one undo step.
    bool applyOffsetEdits(const QVector<MoveRefactor::TextEdit> &edits);

    // --- Emmet ---------------------------------------------------------------------------------
    // 0 = off, 1 = HTML, 2 = JSX. Tab after an abbreviation (`ul>li*3`) expands it like a snippet.
    void setEmmetMode(int mode) { m_emmetMode = mode; }
    bool expandEmmet(); // true when the abbreviation before the caret was expanded

    // --- Colour swatches -----------------------------------------------------------------------
    // A small square in front of each colour literal (CSS colours, Tailwind classes); clicking one emits swatchClicked().
    // The ranges follow edits until the next list arrives.
    struct ColorSwatch {
        int startLine = 0, startColumn = 0, endLine = 0, endColumn = 0;
        QColor color;
    };
    void setColorSwatches(const QVector<ColorSwatch> &swatches);
    bool hasColorSwatches() const { return !m_swatches.isEmpty(); }
    // The swatch's current range as a document selection, or an invalid cursor.
    QTextCursor swatchRange(int index) const;
    void replaceSwatch(int index, const QString &text); // one undo step

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
    void setImportsFolded(bool folded); // folds/unfolds the first import list
    bool foldIconAt(const QPoint &gutterPos) const;
    bool hasFolds() const { return m_foldedCount > 0; }
    void setGutterHover(bool on);

signals:
    void filesDropped(const QStringList &paths);
    void searchResultsChanged();
    void overwriteModeToggled();
    void blameCommitRequested(const QString &hash);
    void bookmarksChanged();
    void importsFoldedChanged(bool folded); // the user folded/unfolded the import list
    void hoverRequested(int line, int column); // mouse rests on an identifier (0-based line / UTF-16 column)
    void definitionRequested(int line, int column); // Ctrl+click on an identifier
    // triggerKind: 1 invoked / typing, 2 trigger character, 3 the previous list was incomplete (LSP numbering)
    void completionRequested(int line, int column, int triggerKind, const QString &triggerChar, int token);
    // Alt+Enter: quick fixes / actions for the selection (or the caret when it is empty); 0-based line / UTF-16 column.
    void removeUnusedImportsRequested();
    void codeActionsRequested(int startLine, int startColumn, int endLine, int endColumn);
    void colorsRequested();          // the text settled after an edit: ask the server for colour literals again
    void swatchClicked(int index);   // index into the list given to setColorSwatches()

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
    void handleKey(QKeyEvent *event);
    bool autoPair(QKeyEvent *event);   // typed bracket/quote: insert its partner, type over a closer, wrap a selection
    bool eraseEmptyPair();             // Backspace between "()" / "\"\"" removes both
    bool completionKey(QKeyEvent *event); // true when the popup consumed the key
    void completionTyped(QKeyEvent *event, bool edited);
    void scheduleCompletion(int kind, const QString &triggerChar);
    void requestCompletion(int kind, const QString &triggerChar);
    void acceptCompletion();
    void insertCompletion(const LspCompletionItem &item, int anchor, int cur);
    void hideCompletion();
    QString completionPrefix() const; // text from the anchor to the caret, empty when there is no anchor
    bool inStringOrComment(int position) const;
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
    CompletionCommandRunner m_commandRunner;
    CompletionResolver m_resolver;
    LocalCompletions m_localCompletions;
    QVector<LspCompletionItem> localCompletions() const;
    // Snippet session: Tab walks the tab stops of the last inserted snippet, in order, ending at $0.
    // A tab stop: the placeholder is [from, to] (empty for a bare $1). `from` stays put when text is typed at it, `to` moves
    // along, so the range grows to cover what is typed. `mirrors` are the other occurrences of the same $n; they copy the text.
    struct SnippetStop {
        QTextCursor from, to;
        QVector<QPair<QTextCursor, QTextCursor>> mirrors;
    };
    QVector<SnippetStop> m_snippetStops;
    int m_snippetAt = -1;
    QTextCursor m_snippetStart, m_snippetEnd;
    void clearSnippet();
    bool snippetJump(int direction); // true when the key was consumed
    void syncSnippetMirrors();
    void selectSnippetStop(int i);
    // Linked tag renaming: while the caret or selection sits in a tag name, edits to it are copied to its partner
    // (`<div>` <-> `</div>`). The session lives until the caret leaves the name.
    struct TagLink {
        QTextCursor aFrom, aTo, bFrom, bTo; // the edited name and its partner
        bool active = false;
    };
    TagLink m_tagLink;
    void beforeKeyEdit(QKeyEvent *event);
    void afterKeyEdit();
    bool beginTagLink();
    void syncTagLink();
    QTextCursor m_keyGroup; // holds the edit block that keeps a typed character and its mirrored copies one undo step
    bool m_keyGroupOpen = false;
    bool m_completionManual = false; // Ctrl+Space, as opposed to completion that popped up while typing
    bool autoCloseTag();             // typing '>' after <Tag ...: also inserts </Tag>
    int m_emmetMode = 0;
    struct Swatch {
        QTextCursor start, end;
        QColor color;
        QRect rect; // of the last paint
    };
    QVector<Swatch> m_swatches;
    QTimer *m_colorTimer = nullptr;
    int swatchGap() const;
    QVector<int> swatchGapColumns(const QTextBlock &block) const;
    mutable int m_gapRevision = -1;
    mutable QHash<int, QVector<int>> m_gapMap; // block number -> columns followed by a swatch gap (valid for m_gapRevision)
    void applyGapProvider(const QSet<int> &blocks);
    void paintSwatches();
    QList<QTextCursor> m_unusedImports; // each selects the text of one import line
    int importRunEnd(const QTextBlock &header) const; // last block of the import list starting at `header`, or -1
    QPair<int, int> m_link{-1, -1}; // identifier under the mouse while Ctrl is held
    int m_hoverPos = -1;       // document position of the pending hover request
    QPointer<HoverPopup> m_hoverPopup; // the language server's hover text; stays until clicked away
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

    CompletionPopup *m_completion = nullptr;
    QTimer *m_completionTimer = nullptr;
    int m_completionToken = 0;
    int m_completionAnchor = -1; // document position where the typed prefix starts
    bool m_completionIncomplete = false;
    int m_pendingKind = 1;
    QString m_pendingTrigger;
};
