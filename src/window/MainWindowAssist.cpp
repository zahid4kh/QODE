// MainWindow's side of the language server assists: it forwards the editors' requests (signature help, inlay hints, symbol
// highlights, light bulb) to LspManager, owns the Edit > Source Actions menu and the options around them, and applies the
// file operations of a WorkspaceEdit.
#include "MainWindow.h"

#include "editor/Breadcrumbs.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "dialogs/CallHierarchyDialog.h"
#include "editor/EditorManager.h"
#include "project/ProjectManager.h"
#include "lsp/LspManager.h"
#include "palette/PalettePopup.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStatusBar>
#include <QTextBlock>
#include <QTimer>

namespace {
bool isWordChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$'); }

const QStringList kOrganize{QStringLiteral("source.organizeImports")};
const QStringList kRemoveUnused{QStringLiteral("source.removeUnusedImports")};
const QStringList kAddMissing{QStringLiteral("source.addMissingImports")};
const QStringList kFixAll{QStringLiteral("source.fixAll")};
}

void MainWindow::createAssistActions()
{
    auto &cfg = SettingsManager::instance();
    auto make = [this](const QString &text, const QKeySequence &key = {}) {
        auto *a = new QAction(text, this);
        if (!key.isEmpty())
            a->setShortcut(key);
        return a;
    };
    auto toggle = [make](const QString &text, bool on) {
        QAction *a = make(text);
        a->setCheckable(true);
        a->setChecked(on);
        return a;
    };
    m_organizeImportsAct = make(tr("Organize Imports"), QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_O));
    m_removeUnusedAct = make(tr("Remove Unused Imports"));
    m_addMissingAct = make(tr("Add Missing Imports"));
    m_fixAllAct = make(tr("Fix All Auto-fixable Problems"));
    m_quickFixAct = make(tr("Quick Fix / Refactor…"));
    m_quickFixAct->setToolTip(tr("Alt+Enter"));
    m_signatureAct = make(tr("Parameter Hints"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Space));
    m_typeDefAct = make(tr("Go to Type Definition"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F12));
    m_callHierarchyAct = make(tr("Call Hierarchy…"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_H));
    m_implAct = make(tr("Go to Implementation"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F12));
    m_workspaceSymbolAct = make(tr("Go to Symbol in Workspace…"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R));
    connect(m_implAct, &QAction::triggered, this, [this] {
        if (CodeEditor *e = m_editors->currentEditor())
            goToDefinition(e, e->textCursor().blockNumber(), e->textCursor().positionInBlock(), LspManager::Navigate::Implementation);
    });
    connect(m_workspaceSymbolAct, &QAction::triggered, this, &MainWindow::showWorkspaceSymbols);
    connect(m_typeDefAct, &QAction::triggered, this, [this] {
        if (CodeEditor *e = m_editors->currentEditor())
            goToDefinition(e, e->textCursor().blockNumber(), e->textCursor().positionInBlock(), LspManager::Navigate::TypeDefinition);
    });
    connect(m_callHierarchyAct, &QAction::triggered, this, &MainWindow::showCallHierarchy);
    connect(m_organizeImportsAct, &QAction::triggered, this, [this] { runSourceAction(kOrganize, tr("Organize Imports")); });
    connect(m_removeUnusedAct, &QAction::triggered, this, [this] { runSourceAction(kRemoveUnused, tr("Remove Unused Imports")); });
    connect(m_addMissingAct, &QAction::triggered, this, [this] { runSourceAction(kAddMissing, tr("Add Missing Imports")); });
    connect(m_fixAllAct, &QAction::triggered, this, [this] { runSourceAction(kFixAll, tr("Fix All")); });
    connect(m_quickFixAct, &QAction::triggered, this, [this] {
        if (CodeEditor *ed = m_editors->currentEditor())
            ed->requestCodeActions();
    });
    connect(m_signatureAct, &QAction::triggered, this, [this] {
        if (CodeEditor *ed = m_editors->currentEditor())
            ed->triggerSignatureHelp();
    });

    m_fixOnSaveAct = toggle(tr("Fix All on Save"), cfg.fixOnSave());
    m_organizeOnSaveAct = toggle(tr("Organize Imports on Save"), cfg.organizeImportsOnSave());
    connect(m_fixOnSaveAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setFixOnSave(on); });
    connect(m_organizeOnSaveAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setOrganizeImportsOnSave(on); });
    // Both can be overridden per project: follow the project that is open.
    connect(&cfg, &SettingsManager::projectSettingsChanged, this, [this] {
        auto &s = SettingsManager::instance();
        QSignalBlocker b1(m_fixOnSaveAct), b2(m_organizeOnSaveAct);
        m_fixOnSaveAct->setChecked(s.fixOnSave());
        m_organizeOnSaveAct->setChecked(s.organizeImportsOnSave());
    });

    m_hintParamsAct = toggle(tr("Parameter Names"), cfg.inlayParameterHints());
    m_hintTypesAct = toggle(tr("Inferred Types"), cfg.inlayTypeHints());
    m_semanticAct = toggle(tr("Semantic Highlighting"), cfg.semanticHighlighting());
    connect(m_semanticAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setSemanticHighlighting(on); });
    m_symbolHighlightAct = toggle(tr("Highlight Symbol Under Caret"), cfg.symbolHighlight());
    m_lightBulbAct = toggle(tr("Quick Fix Light Bulb"), cfg.lightBulb());
    connect(m_hintParamsAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setInlayParameterHints(on); });
    connect(m_hintTypesAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setInlayTypeHints(on); });
    connect(m_symbolHighlightAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setSymbolHighlight(on); });
    connect(m_lightBulbAct, &QAction::toggled, this, [](bool on) { SettingsManager::instance().setLightBulb(on); });
    connect(&cfg, &SettingsManager::editorSettingsChanged, this, &MainWindow::applyAssistSettings);

    // The language server runs its fixes before the file is written.
    m_editors->setLspFixer([this](Document *doc) {
        auto &s = SettingsManager::instance();
        QStringList kinds;
        if (s.organizeImportsOnSave())
            kinds << QStringLiteral("source.organizeImports");
        if (s.fixOnSave())
            kinds << QStringLiteral("source.fixAll");
        if (!kinds.isEmpty() && m_lsp->isServed(doc))
            m_lsp->applySourceActionsBlocking(doc, kinds, 3000);
    });

    // Semantic tokens, folding ranges and symbols are asked again whenever a server starts or asks for a refresh.
    auto *serverRetry = new QTimer(this);
    serverRetry->setSingleShot(true);
    serverRetry->setInterval(700);
    connect(serverRetry, &QTimer::timeout, this, [this] {
        for (Document *d : m_editors->documents())
            if (CodeEditor *ed = m_editors->editorFor(d))
                ed->requestServerDataSoon();
    });
    connect(m_lsp, &LspManager::statusChanged, serverRetry, qOverload<>(&QTimer::start));
    connect(m_lsp, &LspManager::semanticTokensRefresh, serverRetry, qOverload<>(&QTimer::start));

    // A server that just finished starting, or asks for fresh hints: ask every editor again.
    auto *hintRetry = new QTimer(this);
    hintRetry->setSingleShot(true);
    hintRetry->setInterval(900);
    connect(hintRetry, &QTimer::timeout, this, [this] {
        for (Document *d : m_editors->documents())
            if (CodeEditor *ed = m_editors->editorFor(d))
                ed->requestInlayHintsSoon();
    });
    connect(m_lsp, &LspManager::statusChanged, hintRetry, qOverload<>(&QTimer::start));
    connect(m_lsp, &LspManager::inlayHintsRefresh, hintRetry, qOverload<>(&QTimer::start));
    // "Extract to function" ends by asking the editor to rename the new name.
    connect(m_lsp, &LspManager::renameRequested, this, [this](const QString &path, int line, int column) {
        Document *doc = m_editors->documentForPath(path);
        CodeEditor *ed = doc ? m_editors->editorFor(doc) : nullptr;
        const QTextBlock b = ed ? ed->document()->findBlockByNumber(line) : QTextBlock();
        if (!b.isValid())
            return;
        QTextCursor c(ed->document());
        c.setPosition(b.position() + qMin(column, b.length() - 1));
        ed->setTextCursor(c);
        ed->setFocus();
        QTimer::singleShot(0, this, &MainWindow::renameSymbol);
    });
}

// What the editors show depends on the settings; called when one changes and for every new editor.
void MainWindow::applyAssistSettings()
{
    auto &s = SettingsManager::instance();
    for (Document *d : m_editors->documents())
        if (CodeEditor *ed = m_editors->editorFor(d)) {
            ed->setInlayHintsWanted(s.inlayParameterHints() || s.inlayTypeHints());
            ed->requestInlayHintsSoon(); // which kinds show may have changed
            ed->setLightBulbEnabled(s.lightBulb());
            if (!s.semanticHighlighting())
                ed->clearSemanticTokens();
            ed->requestServerDataSoon();
            if (!s.symbolHighlight())
                ed->clearSymbolHighlights();
            ed->requestAssistsSoon();
        }
}

void MainWindow::wireAssists(Document *doc, CodeEditor *ed)
{
    ed->setSignatureTriggers([this, doc](QString *triggers, QString *retriggers) { return m_lsp->signatureTriggers(doc, triggers, retriggers); });
    connect(ed, &CodeEditor::signatureHelpRequested, this,
            [this, doc, ed](int line, int column, int kind, const QString &trigger, bool retrigger, int token) {
                QPointer<CodeEditor> guard(ed);
                m_lsp->signatureHelp(doc, line, column, kind, trigger, retrigger, [guard, token](const LspSignatureHelp &help) {
                    if (guard)
                        guard->showSignatureHelp(help, token);
                });
            });
    connect(ed, &CodeEditor::referencesRequested, this, [this, ed](int line, int column) { showReferencesAt(ed, line, column); });
    connect(ed, &CodeEditor::serverDataRequested, this, [this, doc, ed] { requestServerData(doc, ed); });
    connect(ed, &CodeEditor::inlayHintsRequested, this, [this, doc, ed](int first, int last) { requestHints(doc, ed, first, last); });
    connect(ed, &CodeEditor::caretSettled, this, [this, doc, ed] {
        requestHighlights(doc, ed);
        updateLightBulb(doc, ed);
    });
    auto &s = SettingsManager::instance();
    ed->setLightBulbEnabled(s.lightBulb());
    ed->setInlayHintsWanted(s.inlayParameterHints() || s.inlayTypeHints());
    ed->requestServerDataSoon();
}

// What the language server knows about the file as a whole: which identifier is what (semantic highlighting), where it
// folds, and the tree of declarations (breadcrumbs, pinned lines). Each answer is dropped when the text changed meanwhile;
// the edit has already scheduled the next request.
void MainWindow::requestServerData(Document *doc, CodeEditor *ed)
{
    const int revision = ed->document()->revision();
    QPointer<CodeEditor> guard(ed);
    if (SettingsManager::instance().semanticHighlighting() && m_lsp->supportsSemanticTokens(doc)) {
        m_lsp->semanticTokens(doc, [guard, revision](const LspSemanticTokens &tokens) {
            if (guard && tokens.ok)
                guard->setSemanticTokens(tokens, revision);
        });
    } else {
        ed->clearSemanticTokens();
    }
    if (m_lsp->supportsFoldingRanges(doc)) {
        m_lsp->foldingRanges(doc, [guard, revision](const QVector<LspFoldingRange> &ranges, bool ok) {
            if (guard && ok)
                guard->setServerFolds(ranges, revision);
        });
    } else {
        ed->clearServerFolds();
    }
    if (m_lsp->supportsDocumentSymbols(doc)) {
        m_lsp->documentSymbols(doc, [guard, revision](const QVector<LspSymbol> &symbols, bool ok) {
            if (guard && ok && guard->document()->revision() == revision)
                guard->setDocumentSymbols(symbols);
        });
    } else {
        ed->clearDocumentSymbols();
    }
}

// Ctrl+Alt+H: the callers (and callees) of the function under the caret, in a tree that opens level by level.
void MainWindow::showCallHierarchy()
{
    CodeEditor *ed = m_editors->currentEditor();
    Document *doc = ed ? documentOf(ed) : nullptr;
    if (!doc)
        return;
    if (!m_lsp->supportsCallHierarchy(doc)) {
        statusBar()->showMessage(tr("Call Hierarchy needs a running language server that supports it for this file"), 4000);
        return;
    }
    const QTextCursor c = ed->textCursor();
    QPointer<CodeEditor> guard(ed);
    m_lsp->prepareCallHierarchy(doc, c.blockNumber(), c.positionInBlock(), [this, guard](const QVector<LspCallItem> &items) {
        if (!guard)
            return;
        if (items.isEmpty()) {
            statusBar()->showMessage(tr("No function under the caret"), 3000);
            return;
        }
        if (!m_callDialog) {
            m_callDialog = new CallHierarchyDialog(this);
            m_callDialog->setAttribute(Qt::WA_DeleteOnClose);
            m_callDialog->setFetch([this](const LspCallItem &item, bool incoming, std::function<void(const QVector<LspCallItem> &, bool)> done) {
                m_lsp->callHierarchyCalls(item, incoming, std::move(done));
            });
            connect(m_callDialog, &CallHierarchyDialog::openRequested, this,
                    [this](const QString &path, int line, int column) { m_editors->openFileAt(path, line + 1, column + 1); });
        }
        m_callDialog->setRoot(items.first(), m_projects->project().root);
        m_callDialog->show();
        m_callDialog->raise();
        m_callDialog->activateWindow();
    });
}

void MainWindow::requestHints(Document *doc, CodeEditor *ed, int firstLine, int lastLine)
{
    auto &s = SettingsManager::instance();
    if (!m_lsp->supportsInlayHints(doc)) {
        ed->setInlayHints({});
        return;
    }
    const bool params = s.inlayParameterHints(), types = s.inlayTypeHints();
    const int revision = ed->document()->revision();
    QPointer<CodeEditor> guard(ed);
    m_lsp->inlayHints(doc, firstLine, lastLine, [guard, params, types, revision](const QVector<LspInlayHint> &hints) {
        if (!guard || guard->document()->revision() != revision)
            return; // typed meanwhile: the next request has the right lines
        QVector<LspInlayHint> shown;
        for (const LspInlayHint &h : hints)
            if (h.kind == 2 ? params : types)
                shown << h;
        guard->setInlayHints(shown);
    });
}

void MainWindow::requestHighlights(Document *doc, CodeEditor *ed)
{
    if (!SettingsManager::instance().symbolHighlight() || !m_lsp->supportsHighlights(doc)) {
        ed->clearSymbolHighlights();
        return;
    }
    const QTextCursor c = ed->textCursor();
    const QString text = c.block().text();
    const int col = c.positionInBlock();
    const bool onWord = !c.hasSelection() && ((col < text.size() && isWordChar(text.at(col))) || (col > 0 && isWordChar(text.at(col - 1))));
    if (!onWord) {
        ed->clearSymbolHighlights();
        return;
    }
    const int revision = ed->document()->revision(), position = c.position();
    QPointer<CodeEditor> guard(ed);
    m_lsp->documentHighlights(doc, c.blockNumber(), col, [guard, revision, position](const QVector<LspHighlight> &list) {
        if (guard && guard->document()->revision() == revision && guard->textCursor().position() == position)
            guard->setSymbolHighlights(list);
    });
}

// The bulb appears on the caret's line when the server has a quick fix for it (or, with a selection, any action). Fixes come
// from diagnostics, so a line without one does not even cost a request.
void MainWindow::updateLightBulb(Document *doc, CodeEditor *ed)
{
    if (!SettingsManager::instance().lightBulb() || !m_lsp->isServed(doc)) {
        ed->setLightBulb(-1);
        return;
    }
    const QTextCursor c = ed->textCursor();
    const bool selection = c.hasSelection();
    const QTextBlock a = ed->document()->findBlock(c.selectionStart()), b = ed->document()->findBlock(c.selectionEnd());
    const int line = c.blockNumber();
    bool hasDiagnostic = false;
    for (const LspDiagnostic &d : m_lsp->diagnostics(doc->filePath()))
        if (d.startLine <= b.blockNumber() && d.endLine >= a.blockNumber()) {
            hasDiagnostic = true;
            break;
        }
    if (!hasDiagnostic && !selection) {
        ed->setLightBulb(-1);
        return;
    }
    const int revision = ed->document()->revision(), position = c.position();
    QPointer<CodeEditor> guard(ed);
    m_lsp->codeActions(doc, a.blockNumber(), c.selectionStart() - a.position(), b.blockNumber(), c.selectionEnd() - b.position(),
                       [guard, line, revision, position, selection](const QVector<LspCodeAction> &actions) {
                           if (!guard || guard->document()->revision() != revision || guard->textCursor().position() != position)
                               return;
                           bool any = false;
                           for (const LspCodeAction &act : actions)
                               if (selection || act.kind.isEmpty() || act.kind.startsWith(QLatin1String("quickfix")))
                                   any = true;
                           guard->setLightBulb(any ? line : -1);
                       },
                       selection ? QStringList() : QStringList{QStringLiteral("quickfix")}, true);
}

void MainWindow::runSourceAction(const QStringList &kinds, const QString &what)
{
    CodeEditor *ed = m_editors->currentEditor();
    Document *doc = ed ? documentOf(ed) : nullptr;
    if (!doc)
        return;
    if (!m_lsp->isServed(doc)) {
        statusBar()->showMessage(tr("%1 needs a running language server for this file").arg(what), 4000);
        return;
    }
    statusBar()->showMessage(tr("%1…").arg(what), 3000);
    m_lsp->applySourceActions(doc, kinds, [this, what](int applied, int found) {
        if (applied > 0)
            statusBar()->showMessage(tr("%1: done — review the changes, the file stays unsaved").arg(what), 5000);
        else if (found == 0)
            statusBar()->showMessage(tr("%1: the language server has nothing to do for this file").arg(what), 5000);
        else
            statusBar()->showMessage(tr("%1: the language server could not apply it").arg(what), 5000);
    });
}

// Creates, renames and deletes files a WorkspaceEdit asks for. Creation and renames run before its text edits ("Move to a new
// file" creates the file, then fills it); deletes after.
void MainWindow::applyFileOps(const QVector<LspManager::FileOp> &ops, bool before)
{
    for (const LspManager::FileOp &op : ops) {
        if (before && op.kind == QLatin1String("create")) {
            if (QFileInfo::exists(op.path) && !op.overwrite)
                continue;
            QDir().mkpath(QFileInfo(op.path).absolutePath());
            QFile f(op.path);
            if (f.open(QIODevice::WriteOnly))
                f.close();
        } else if (before && op.kind == QLatin1String("rename")) {
            if (QFileInfo::exists(op.target) && !op.overwrite)
                continue;
            QDir().mkpath(QFileInfo(op.target).absolutePath());
            QFile::rename(op.path, op.target);
        } else if (!before && op.kind == QLatin1String("delete")) {
            if (m_editors->documentForPath(op.path))
                m_editors->closeDocumentsUnder(op.path);
            QFileInfo(op.path).isDir() ? static_cast<void>(QDir(op.path).removeRecursively()) : static_cast<void>(QFile::remove(op.path));
        }
    }
}

// Ctrl+Alt+R: declarations anywhere in the project. The language server does the matching (workspace/symbol), so the list
// is asked again 180 ms after each keystroke; answers for an older query are dropped.
void MainWindow::showWorkspaceSymbols()
{
    Document *doc = m_editors->currentDocument();
    if (!doc || !m_lsp->supportsWorkspaceSymbols(doc)) {
        statusBar()->showMessage(tr("Go to Symbol in Workspace needs a running language server that supports it for the current file"), 4000);
        return;
    }
    auto *pop = new PalettePopup(this);
    pop->setPlaceholder(tr("Search symbols in the project (classes, functions, properties)"));
    pop->setEmptyText(tr("Type a name to search"));
    auto *timer = new QTimer(pop);
    timer->setSingleShot(true);
    timer->setInterval(180);
    auto token = std::make_shared<int>(0);
    const QString root = m_projects->project().root;
    QPointer<PalettePopup> guard(pop);
    QPointer<Document> docGuard(doc);
    connect(timer, &QTimer::timeout, pop, [this, guard, docGuard, token, root] {
        if (!guard || !docGuard)
            return;
        const QString query = guard->query().trimmed();
        if (query.isEmpty()) {
            guard->setItems({});
            guard->setStatus({});
            return;
        }
        const int mine = ++*token;
        guard->setStatus(tr("Searching…"));
        m_lsp->workspaceSymbols(docGuard, query, [guard, token, mine, root](const QVector<LspWorkspaceSymbol> &symbols, bool ok) {
            if (!guard || *token != mine)
                return;
            QList<PalettePopup::Item> items;
            for (const LspWorkspaceSymbol &s : symbols) {
                PalettePopup::Item it;
                it.title = s.name;
                const QString file = root.isEmpty() ? s.path : QDir(root).relativeFilePath(s.path);
                const QString kind = Breadcrumbs::symbolKindName(s.kind);
                it.detail = s.container.isEmpty() ? QStringLiteral("%1 · %2").arg(kind, file) : QStringLiteral("%1 · %2 · %3").arg(kind, s.container, file);
                it.hint = QString::number(s.line + 1);
                it.data = QVariantList{s.path, s.line, s.column};
                items.append(it);
            }
            guard->setItems(items);
            guard->setStatus(ok ? (items.isEmpty() ? tr("No matching symbols") : QString()) : tr("The language server could not answer"));
        });
    });
    connect(pop, &PalettePopup::queryChanged, timer, qOverload<>(&QTimer::start));
    connect(pop, &PalettePopup::accepted, this, [this](const QVariant &data, const QString &) {
        const QVariantList v = data.toList();
        if (v.size() == 3)
            QTimer::singleShot(0, this, [this, v] { m_editors->openFileAt(v[0].toString(), v[1].toInt() + 1, v[2].toInt() + 1); });
    });
    pop->popup();
}
