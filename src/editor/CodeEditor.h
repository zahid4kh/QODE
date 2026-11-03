#pragma once

#include <functional>
#include "git/GitDiff.h"
#include "Breadcrumbs.h"
#include "git/GitTypes.h"
#include "lsp/LspTypes.h"
#include "refactor/MoveRefactor.h"

#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QPointer>
#include <QVector>

class CompletionPopup;
class QAction;
class HoverPopup;
class SignaturePopup;
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

    void setLanguage(const QString &name);
    // A large file (Document::isLarge) shows no minimap and makes no edit predictions. Set before attachDocument().
    void setLargeFile(bool on) { m_largeFile = on; }
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
    void duplicateLine(); // selection: the selected text; otherwise the caret line, below it (one undo step)
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
    // C/C++: the folders an `#include` path is completed from when no language server serves the file ('<' or '"' opener).
    void setIncludeFolders(std::function<QStringList(QChar opener)> provider) { m_includeFolders = std::move(provider); }
    // Extra entries at the end of the context menu (Switch Header/Source ...); disabled ones are left out.
    void setContextActions(const QList<QAction *> &actions) { m_contextActions = actions; }
    // Entries at the top of the context menu while text is selected (Ask about this to make a change); disabled ones are left out.
    void setSelectionContextActions(const QList<QAction *> &actions) { m_selectionActions = actions; }
    // Line backgrounds painted over everything else (the inline edit's removed / proposed lines); empty clears them.
    void setInlineMarks(const QList<QTextEdit::ExtraSelection> &marks);
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

    // --- Language server assists (CodeEditorAssist.cpp) ---------------------------------------------
    // Inlay hints: text drawn inside the code (inferred types, parameter names). The list replaces the previous one; the
    // hints follow edits until the next list arrives. Switched off, the editor neither asks for nor shows them.
    void setInlayHints(const QVector<LspInlayHint> &hints);
    void setInlayHintsWanted(bool on);
    bool inlayHintsWanted() const { return m_hintsWanted; }
    int inlayHintCount() const { return m_hints.size(); }
    void requestInlayHintsSoon();
    void requestAssistsSoon(); // ask for the symbol highlights and the light bulb again (after the diagnostics changed)
    // Other uses of the symbol under the caret, tinted in the text. They are dropped when the caret leaves them.
    void setSymbolHighlights(const QVector<LspHighlight> &list);
    void clearSymbolHighlights();
    bool hasSymbolHighlights() const { return !m_highlights.isEmpty(); }
    // The light bulb in the gutter: the line (0-based) the server has quick fixes for, -1 for none. Clicking it behaves
    // like Alt+Enter. `enabled` also reserves the gutter room for it.
    void setLightBulb(int line);
    int lightBulbLine() const { return m_bulbLine; }
    void setLightBulbEnabled(bool on);
    // Parameter hints: asked after the server's trigger characters, kept up to date while the call is edited.
    using SignatureTriggers = std::function<bool(QString *triggers, QString *retriggers)>;
    void setSignatureTriggers(SignatureTriggers f) { m_signatureTriggers = std::move(f); }
    void showSignatureHelp(const LspSignatureHelp &help, int token);
    void hideSignatureHelp();
    void triggerSignatureHelp(bool fallbackToInfo = false); // Ctrl+P / Ctrl+Shift+Space
    void showQuickInfo();                                   // hover text of the symbol at the caret (Ctrl+Shift+I)
    bool signatureVisible() const;
    // Opens the Alt+Enter menu for the caret / selection.
    void requestCodeActions();

    // --- Run markers (.http requests) --------------------------------------------------------------------
    // Play markers in the gutter, 0-based lines. Clicking one, or Ctrl+Enter anywhere while there are markers, emits
    // runRequested(line). They share the light bulb's column, which wins on its own line.
    void setRunMarkers(const QList<int> &lines, const QString &tooltip);
    bool hasRunMarkers() const { return !m_runLines.isEmpty(); }
    bool runMarkerAt(const QPoint &gutterPos, int *line = nullptr) const;
    QString runMarkerTip() const { return m_runTip; }
    // Tooltip text for a text position (0-based line / column), asked before the language server's hover; "" = none.
    void setTooltipProvider(std::function<QString(int line, int column)> f) { m_tooltipProvider = std::move(f); }

    // --- Bookmarks ----------------------------------------------------------
    // 0-based, sorted lines. They follow the text while it is edited; setBookmarks() does not emit.
    const QList<int> &bookmarks() const { return m_bookmarks; }
    void setBookmarks(const QList<int> &lines);
    void toggleBookmark(int line = -1); // -1: the caret line

    // --- Breakpoints (JVM debugger) -------------------------------------------
    // While enabled, clicking a line number toggles a breakpoint there. 0-based, sorted lines that follow the text like
    // bookmarks; setBreakpoints() does not emit.
    void setBreakpointsEnabled(bool on);
    bool breakpointsEnabled() const { return m_breakpointsOn; }
    const QList<int> &breakpoints() const { return m_breakpoints; }
    void setBreakpoints(const QList<int> &lines);
    void toggleBreakpoint(int line = -1); // -1: the caret line
    // The line the debugger is stopped at (0-based; -1 = none): tinted, with an arrow over its number.
    void setExecutionLine(int line);
    int executionLine() const { return m_execLine; }
    bool breakpointAreaAt(const QPoint &gutterPos) const;

    // --- Minimap support ------------------------------------------------------
    void visibleBlockRange(int *first, int *last) const; // block numbers on screen
    void scrollBlockToCenter(int blockNumber);
    void scrollBlockToTop(int blockNumber); // the block becomes the first visible one
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
    // Folds the comment above the first code (a license header) and/or every /** doc comment */ of at least three lines;
    // the region holding the caret stays open. For files with C-style comments only (Kotlin, Java, C/C++, JS/TS …).
    void foldComments(bool header, bool docs);
    static bool hasBlockComments(const QString &path);
    bool foldIconAt(const QPoint &gutterPos) const;
    bool hasFolds() const { return m_foldedCount > 0; }
    void setGutterHover(bool on);

    // --- Semantic highlighting, folding ranges, symbols from the language server (CodeEditorAssist.cpp) ----------------
    // The identifiers' meaning (parameter, property, class ...) painted over the regex colours. `revision` is the document
    // revision the request was made at: an answer for older text is refused (false) and the caller asks again.
    bool setSemanticTokens(const LspSemanticTokens &tokens, int revision);
    void clearSemanticTokens();
    // Folding ranges of the server. They are combined with QODE's own detection: a line folds when either finds a region
    // there, and the server's end wins where both do. The ranges follow edits until the next list arrives.
    bool setServerFolds(const QVector<LspFoldingRange> &ranges, int revision);
    void clearServerFolds();
    int serverFoldCount() const { return m_serverFolds.size(); }
    // The declarations of the file (breadcrumbs, pinned lines). Without them the indentation heuristics are used.
    void setDocumentSymbols(const QVector<LspSymbol> &symbols);
    void clearDocumentSymbols();
    bool hasDocumentSymbols() const { return m_hasSymbols; }
    const QVector<LspSymbol> &documentSymbols() const { return m_symbols; }
    QList<Breadcrumbs::Crumb> symbolChainAt(int blockNumber) const;
    // The text settled (or a server finished starting): serverDataRequested() asks for tokens, folds and symbols again.
    void requestServerDataSoon();

    // --- Next edit predictions (CodeEditorPredict.cpp) ------------------------------------------------
    // After the user renames an identifier or changes a function's parameters, the other places the change belongs
    // to are marked; Tab jumps to the next one and applies it, Ctrl+Shift+Enter applies them all, Escape drops them.
    bool hasPredictions() const { return !m_predictions.isEmpty(); }
    int predictionCount() const { return m_predictions.size(); }
    bool acceptPrediction();      // Tab: jump to the next prediction (when off screen) or apply it
    void acceptAllPredictions();  // one undo step
    void clearPredictions();

    // --- Multiple cursors (CodeEditorMulti.cpp) --------------------------------------------------------
    // Carets beside textCursor(), which stays the main one (the newest). Typing, deleting, moving and copy / cut / paste
    // act at every caret, one undo step per key. Alt+click adds or removes a caret, Alt+drag selects a column; Escape or
    // a plain click goes back to one caret.
    bool hasMultipleCursors() const { return !m_extraCursors.isEmpty(); }
    int cursorCount() const { return int(m_extraCursors.size()) + 1; }
    void addNextOccurrence();            // Alt+J; with nothing selected it first selects the word at the caret
    void removeLastOccurrence();         // Alt+Shift+J: drops the newest caret
    void selectAllOccurrences();         // Ctrl+Alt+Shift+J / Ctrl+Shift+L
    void addCursorVertically(bool down); // Ctrl+Alt+Up / Down (also Alt+Shift+Up / Down)
    void addCursorsToLineEnds();         // Alt+Shift+I: one caret at the end of every selected line
    void clearExtraCursors();

    // --- Links (CodeEditorLinks.cpp) -------------------------------------------------------------------------
    // Ctrl+click on a URL opens the browser; on a path that exists (absolute, ~/, or relative to the file / project, with an
    // optional :line[:column]) it emits fileLinkRequested(). Links the language server reports (textDocument/documentLink)
    // come first. The resolver turns the raw text into an absolute path, or "" when nothing exists there.
    struct DocumentLink {
        int startLine = 0, startColumn = 0, endLine = 0, endColumn = 0;
        QString target; // a URL; file:// targets open in QODE
    };
    void setDocumentLinks(const QVector<DocumentLink> &links); // follow edits until the next list arrives
    void setPathResolver(std::function<QString(const QString &raw)> f) { m_pathResolver = std::move(f); }

    // --- Code lenses (CodeEditorLens.cpp) --------------------------------------------------------------------
    // Action labels after the text of a line ("▶ Run | Debug", "3 references | 1 implementation"), from the language server
    // and from QODE itself. The list replaces the previous one and follows edits; a lens that is not resolved yet keeps the
    // label shown at the same spot before, so labels do not blink while typing. Unresolved lenses are asked for once they
    // are on screen (codeLensResolveRequested); a click on a label emits codeLensClicked().
    void setCodeLenses(const QVector<LspCodeLens> &lenses);
    void resolveCodeLens(int generation, int index, const LspCodeLens &lens); // the answer to codeLensResolveRequested
    void clearCodeLenses();
    int codeLensCount() const { return int(m_lenses.size()); }
    LspCodeLens codeLens(int index) const;
    QString codeLensText(int line) const; // the labels shown after a line (0-based), "" for none
    static QString codeLensLabel(const QString &title); // "$(play) Run" -> "▶ Run"; other $(icon) markers are dropped

    // --- .editorconfig ---------------------------------------------------------------------------------
    // indent_style / indent_size / tab_width of the file; 0 / -1 leave the setting (or the indentation detected in the
    // text) in charge. useTabs: -1 unset, 0 spaces, 1 tabs.
    void setIndentOverride(int useTabs, int indentSize, int tabWidth);
    int tabWidth() const; // tab_width, else the setting

signals:
    void filesDropped(const QStringList &paths);
    void searchResultsChanged();
    void overwriteModeToggled();
    void blameCommitRequested(const QString &hash);
    void bookmarksChanged();
    void breakpointsChanged();
    void importsFoldedChanged(bool folded); // the user folded/unfolded the import list
    void hoverRequested(int line, int column); // mouse rests on an identifier (0-based line / UTF-16 column)
    void definitionRequested(int line, int column); // Ctrl+click on an identifier
    // triggerKind: 1 invoked / typing, 2 trigger character, 3 the previous list was incomplete (LSP numbering)
    void completionRequested(int line, int column, int triggerKind, const QString &triggerChar, int token);
    // Alt+Enter: quick fixes / actions for the selection (or the caret when it is empty); 0-based line / UTF-16 column.
    void removeUnusedImportsRequested();
    void codeActionsRequested(int startLine, int startColumn, int endLine, int endColumn);
    void inlayHintsRequested(int firstLine, int lastLine); // 0-based, the visible lines plus some margin
    void caretSettled();             // the caret rested for a moment: document highlights, light bulb
    // triggerKind: 1 invoked, 2 trigger character, 3 content change (LSP numbering); `token` goes back to showSignatureHelp()
    void signatureHelpRequested(int line, int column, int triggerKind, const QString &triggerChar, bool retrigger, int token);
    void referencesRequested(int line, int column); // "Find References" in the hover popup
    void symbolsChanged();           // the symbol tree changed: breadcrumbs and pinned lines may differ
    void serverDataRequested();      // semantic tokens, folding ranges and document symbols are out of date
    void colorsRequested();          // the text settled after an edit: ask the server for colour literals again
    void swatchClicked(int index);   // index into the list given to setColorSwatches()
    void runRequested(int line);     // a run marker was clicked, or Ctrl+Enter (0-based caret line)
    // "/?" was typed: the '/' is removed again and the inline edit box should open for the caret line. `position` is where
    // the two characters were, so closing the box without asking can put them back.
    void inlineAskRequested(int position);
    void cursorsChanged(); // carets were added or removed
    // Ctrl+click on a path (absolute) or a file:// link; line / column are 0-based, -1 when the link names none.
    void fileLinkRequested(const QString &path, int line, int column);
    void codeLensResolveRequested(int generation, int index); // an unresolved lens came on screen
    void codeLensClicked(int index);                          // index into the list given to setCodeLenses()

protected:
    bool event(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
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
    void finishCompletion(const LspCompletionItem &item, const QTextCursor &mark);
    void insertCompletion(const LspCompletionItem &item, int anchor, int cur);
    bool smartArrow(const QVector<LspCompletionItem> &items, QVector<LspCompletionItem> *shifted); // C/C++ "p." -> "p->"
    void finishInclude();
    bool includeAt(int position, QChar *opener = nullptr, QString *typed = nullptr) const; // inside `#include <...` / "...
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

    int serverFoldEnd(const QTextBlock &header) const; // last block hidden by the server's range starting at `header`, or -1
    void shiftServerData(int edited, int delta);
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
    QList<int> m_breakpoints;
    bool m_breakpointsOn = false;
    int m_execLine = -1;
    QVector<LspDiagnostic> m_diagnostics;
    void updateLink(bool ctrlDown);
    std::function<bool()> m_canGoToDefinition;
    CompletionCommandRunner m_commandRunner;
    CompletionResolver m_resolver;
    LocalCompletions m_localCompletions;
    std::function<QStringList(QChar)> m_includeFolders;
    QList<QAction *> m_contextActions;
    QList<QAction *> m_selectionActions;
    QList<QTextEdit::ExtraSelection> m_inlineMarks;
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
    QVector<QPair<int, int>> gapColumns(const QTextBlock &block) const; // swatches and inlay hints: (column, pixels)
    mutable int m_gapRevision = -1;
    mutable QHash<int, QVector<QPair<int, int>>> m_gapMap; // block number -> gaps (valid for m_gapRevision)
    void applyGapProvider(const QSet<int> &blocks);
    bool m_gapUpdating = false; // re-highlighting for swatches / hints: Qt reports that as a text change, which it is not
    void paintSwatches();
    QList<QTextCursor> m_unusedImports; // each selects the text of one import line
    int importRunEnd(const QTextBlock &header) const; // last block of the import list starting at `header`, or -1
    QPair<int, int> m_link{-1, -1}; // identifier under the mouse while Ctrl is held
    bool m_hoverFromCaret = false; // the pending hover was asked for by key, not by the mouse
    bool m_signatureFallback = false; // no signature at the caret: show the symbol's hover text instead
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
    bool m_largeFile = false;
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

    // Language server data that follows edits: folding ranges (start line -> end line) and the symbol tree.
    QHash<int, int> m_serverFolds;
    QVector<LspSymbol> m_symbols;
    bool m_hasSymbols = false;
    QTimer *m_serverTimer = nullptr;

    // Language server assists
    struct Hint {
        QTextCursor at; // the hint sits in front of this position
        QString text;
        int kind = 0, width = 0; // width: pixels reserved in the text (and used by the pill)
        QRect rect;              // of the last paint
    };
    QVector<Hint> m_hints;
    struct Lens {
        QTextCursor at; // the start of the range it belongs to
        LspCodeLens lens;
        QString shown;  // the label painted ("" = nothing yet)
        bool asked = false; // resolving was requested
        QRect rect;     // of the last paint
    };
    QVector<Lens> m_lenses;
    int m_lensGeneration = 0;
    int m_lensHover = -1;
    QHash<int, int> m_lensEnd; // block number -> right edge of its labels in the last paint (inline blame goes after them)
    void paintCodeLenses();
    int codeLensAt(const QPoint &pos) const; // a clickable label under the mouse, -1 for none
    bool m_hintsWanted = false;
    QTimer *m_hintTimer = nullptr;
    QFont hintFont() const;
    void paintInlayHints();
    struct Highlight {
        QTextCursor from, to;
        int kind = 1;
    };
    QVector<Highlight> m_highlights;
    QTimer *m_caretTimer = nullptr;
    QList<int> m_runLines;
    QString m_runTip;
    std::function<QString(int, int)> m_tooltipProvider;
    int m_bulbLine = -1;
    bool m_bulbEnabled = true;
    QRect m_bulbRect;
    SignatureTriggers m_signatureTriggers;
    SignaturePopup *m_signature = nullptr;
    QTimer *m_signatureTimer = nullptr;
    int m_signatureToken = 0;
    bool m_signatureOpen = false; // a call is being edited: keep asking
    void signatureTyped(QKeyEvent *event, bool edited);
    void scheduleSignature(int kind, const QString &trigger, bool retrigger);
    void setupAssists();
    QTimer *m_docsTimer = nullptr;
    QHash<QString, LspCompletionItem> m_docCache; // resolved items of the current list, by label + detail
    void updateCompletionDocs();
    QString completionDocsHtml(const LspCompletionItem &item) const;
    void applyHoverLink(const QString &href);
    void paintLightBulb(QPainter &painter, int top, int lineHeight);
    void paintRunMarker(QPainter &painter, int top, int lineHeight);
    CompletionPopup *m_completion = nullptr;
    QTimer *m_completionTimer = nullptr;
    int m_completionToken = 0;
    int m_completionAnchor = -1; // document position where the typed prefix starts
    bool m_completionIncomplete = false;
    int m_pendingKind = 1;
    QString m_pendingTrigger;

    // Next edit predictions. The session is the user's current edit: [start, end) of the text was `before` when it began;
    // an edit next to it grows it, an edit elsewhere starts a new one. m_shadow is the text before the latest change.
    struct PredictSession {
        int start = -1, end = -1;
        QString before;
        bool dirty = false;
    };
    struct PendingEdit {
        QTextCursor from, to; // the range to replace (equal for an insertion)
        QString expect, text; // what the range holds now, and what goes there
        int selectFrom = -1, selectLength = 0;
    };
    PredictSession m_predictSession;
    QString m_shadow;
    QVector<PendingEdit> m_predictions;
    int m_predictKind = 0;     // EditPredict::Kind of the list
    QString m_predictFrom, m_predictTo;
    int m_predictTotal = 0;    // size of the list when it was made ("2 of 5")
    bool m_predictEnabled = true;
    bool m_predictApplying = false; // our own edits do not start a session
    bool m_predictUndo = false;     // undo / redo do not either
    QTextCursor m_acceptedFrom, m_acceptedTo; // the last applied prediction: typing a placeholder there changes nothing
    QTimer *m_predictTimer = nullptr;
    QMetaObject::Connection m_predictConn;
    QRect m_predictPill; // of the last paint, clickable
    void setupPredictions();
    void resetPredictionText();
    void predictionChange(int pos, int removed, int added);
    void computePredictions();
    int nextPrediction() const;
    bool predictionsWanted() const;
    void appendPredictionSelections(QList<QTextEdit::ExtraSelection> &extra) const;
    void paintPredictions();

    // Multiple cursors. m_extraCursors keeps the order the carets were added in (the main caret is the newest).
    QList<QTextCursor> m_extraCursors;
    struct ColumnDrag {
        bool active = false, moved = false;
        int anchorBlock = 0;
        qreal anchorX = 0; // content x (scrolling does not move it)
        QPoint pressPos;
    } m_columnDrag;
    bool m_occurrenceWhole = false; // Alt+J started from a word at the caret: match whole words
    QPair<int, int> m_occurrenceSeed{-1, -1};
    int m_verticalX = -1;           // layout x the carets added by Ctrl+Alt+Up / Down line up on
    QStringList m_multiClip;        // what the last multi-caret copy put on the clipboard, one piece per caret
    QString m_multiClipText;
    bool multiCursorShortcut(QKeyEvent *event); // Alt+J & co., with one caret or many
    bool multiCursorKey(QKeyEvent *event);      // keys while there are several carets
    // Runs `edit` at every caret (the index is the caret's place in document order) as one undo step.
    void editAtCursors(const std::function<void(QTextCursor &, int)> &edit);
    QList<QTextCursor> allCursorsInOrder() const;
    void setCursors(const QList<QTextCursor> &extras, const QTextCursor &main);
    void normalizeCursors(); // drops carets that overlap another one
    void copyAtCursors(bool cut);
    void pasteAtCursors();
    void columnSelect(const QPoint &viewportPos);
    void toggleCursorAt(const QPoint &viewportPos);
    void appendCursorSelections(QList<QTextEdit::ExtraSelection> &extra) const;
    void paintExtraCursors();

    // Links
    struct LinkMark {
        QTextCursor from, to;
        QString target;
    };
    QVector<LinkMark> m_docLinks;
    std::function<QString(const QString &)> m_pathResolver;
    struct LinkHit {
        int start = -1, end = -1; // document range
        QString url;              // a web link, or
        QString path;             // a file, with
        int line = -1, column = -1;
        bool valid() const { return start >= 0; }
    };
    LinkHit m_linkHit; // what Ctrl+click on m_link opens; invalid = Go to Definition
    mutable QPair<QString, QString> m_resolved; // last raw path -> its resolution (Ctrl+mouse move asks often)
    LinkHit linkAt(int position) const;
    void openLink(const LinkHit &hit);

    // .editorconfig
    int m_ecTabs = -1, m_ecIndent = 0, m_ecTabWidth = 0;
};
