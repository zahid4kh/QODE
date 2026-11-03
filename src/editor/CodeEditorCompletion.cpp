// The completion popup: when it opens, what it shows (the server's items, local words, #include paths), and
// inserting the chosen item (snippets, additional edits, Emmet abbreviations, the smart -> on pointers).
#include "CodeEditor.h"

#include "CodeEditorText.h"
#include "CompletionPopup.h"
#include "Emmet.h"
#include "Snippets.h"
#include "SyntaxHighlighter.h"
#include "cpp/CppTools.h"

#include <QKeyEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>

using namespace CodeEditorText;

namespace {

// Web languages complete inside strings too: import paths, class names, CSS values, attribute values.
bool completesInStrings(const QString &lang)
{
    return lang == QLatin1String("TypeScript") || lang == QLatin1String("JavaScript") || lang == QLatin1String("HTML") ||
           lang == QLatin1String("CSS") || lang == QLatin1String("JSON");
}

bool inCommentAt(const QTextDocument *doc, int position)
{
    if (position <= 0)
        return false;
    const QTextBlock block = doc->findBlock(position - 1);
    const int i = position - 1 - block.position();
    for (const QTextLayout::FormatRange &r : block.layout()->formats())
        if (r.format.property(kTokenRoleProperty).toInt() == int(TokenRole::Comment) && r.format.property(kTokenRoleProperty).isValid() &&
            i >= r.start && i < r.start + r.length)
            return true;
    return false;
}

} // namespace

bool CodeEditor::completionVisible() const { return m_completion && m_completion->isVisible(); }

bool CodeEditor::inStringOrComment(int position) const
{
    if (position <= 0)
        return false;
    const QTextBlock block = document()->findBlock(position - 1);
    const QVector<bool> mask = protectedMask(block);
    const int i = position - 1 - block.position();
    return i >= 0 && i < mask.size() && mask[i];
}

QString CodeEditor::completionPrefix() const
{
    const int cur = textCursor().position();
    if (m_completionAnchor < 0 || cur < m_completionAnchor)
        return {};
    QTextCursor c(document());
    c.setPosition(m_completionAnchor);
    c.setPosition(cur, QTextCursor::KeepAnchor);
    return c.selectedText();
}

QVector<LspCompletionItem> CodeEditor::localCompletions() const
{
    if (!m_localCompletions)
        return {};
    const int cur = textCursor().position();
    int start = cur;
    while (start > 0 && isIdentifierChar(document()->characterAt(start - 1)))
        --start;
    const QTextBlock block = document()->findBlock(start);
    return m_localCompletions(block.text().left(start - block.position()));
}

void CodeEditor::hideCompletion()
{
    if (m_completionTimer)
        m_completionTimer->stop();
    if (m_completion)
        m_completion->hide();
    ++m_completionToken; // an answer still on its way is stale now
    m_completionAnchor = -1;
}

bool CodeEditor::completionKey(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (mods != Qt::NoModifier)
            return false;
        m_completion->moveSelection(event->key() == Qt::Key_Up ? -1 : 1);
        return true;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        m_completion->moveSelection(event->key() == Qt::Key_PageUp ? -8 : 8);
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (mods != Qt::NoModifier)
            return false;
        acceptCompletion();
        return true;
    case Qt::Key_Escape:
        hideCompletion();
        return true;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Home:
    case Qt::Key_End:
        hideCompletion();
        return false;
    default:
        return false;
    }
}

void CodeEditor::completionTyped(QKeyEvent *event, bool edited)
{
    const bool served = (m_canGoToDefinition && m_canGoToDefinition()) || !localCompletions().isEmpty();
    const int cur = textCursor().position();
    if (completionVisible() && (cur < m_completionAnchor || !edited)) { // moved away from the word
        if (!edited && cur >= m_completionAnchor && completionPrefix().size() == cur - m_completionAnchor)
            return;
        hideCompletion();
        return;
    }
    if (!edited)
        return;

    const bool erase = event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete;
    const QString text = event->text();
    if (includeAt(cur)) { // `#include <dir/na|>`: the paths are completed as they are typed, folder by folder
        if (!served && !m_includeFolders)
            return;
        const QChar ch = text.isEmpty() ? QChar() : text[0];
        const bool restart = ch == QLatin1Char('/') || ch == QLatin1Char('<') || ch == QLatin1Char('"');
        if (!restart && completionVisible()) {
            if (m_completion->setPrefix(completionPrefix()) == 0)
                hideCompletion();
            return;
        }
        if (!erase) // clangd's trigger characters here are '/', '<' and '"'; anything else is a plain request
            scheduleCompletion(restart ? 2 : 1, restart ? QString(ch) : QString());
        return;
    }
    if (erase) {
        if (!completionVisible())
            return;
        if (m_completion->setPrefix(completionPrefix()) == 0)
            hideCompletion();
        return;
    }
    if (text.size() != 1 || text[0].isSpace() || text[0].category() == QChar::Other_Control) {
        hideCompletion();
        return;
    }
    const QChar ch = text[0];
    if (isIdentifierChar(ch)) {
        if (completionVisible()) {
            const QString prefix = completionPrefix();
            if (prefix.isEmpty() || m_completion->setPrefix(prefix) == 0) {
                hideCompletion();
            } else if (m_completionIncomplete) { // the server cut its list short: ask again for the longer prefix
                scheduleCompletion(3, {});
            }
            return;
        }
        // QODE's own completions (build-file keys, @file:DependsOn coordinates …) may live inside strings.
        if ((!served && Snippets::forLanguage(m_language).isEmpty()) ||
            ((completesInStrings(m_language) ? inCommentAt(document(), cur) : inStringOrComment(cur)) && localCompletions().isEmpty()))
            return;
        QTextCursor c(document());
        c.setPosition(cur);
        int start = cur;
        while (start > 0 && isIdentifierChar(document()->characterAt(start - 1)))
            --start;
        if (document()->characterAt(start).isDigit()) // a number literal, not a name
            return;
        scheduleCompletion(1, {});
        return;
    }
    hideCompletion();
    // `key=`, `@file:`, `DependsOn("`: the local provider has something right after the character.
    if (QStringLiteral("\":=@/").contains(ch) && !localCompletions().isEmpty()) {
        scheduleCompletion(1, {});
        return;
    }
    if (!served || (completesInStrings(m_language) ? inCommentAt(document(), cur - 1) : inStringOrComment(cur - 1)))
        return;
    const QChar prev = cur >= 2 ? document()->characterAt(cur - 2) : QChar();
    // Web servers: tags and closing tags (< and /), import paths and attribute values (quotes, /, @), CSS (: - @).
    if (completesInStrings(m_language)) {
        const bool css = m_language == QLatin1String("CSS");
        const bool html = m_language == QLatin1String("HTML");
        const QString triggers = css ? QStringLiteral(":-@") : html ? QStringLiteral("<\"'/=") : QStringLiteral("\"'`/@<");
        if (ch == QLatin1Char('.') || triggers.contains(ch)) {
            scheduleCompletion(2, QString(ch));
            return;
        }
    }
    if (ch == QLatin1Char('.') || (ch == QLatin1Char('>') && prev == QLatin1Char('-')) || (ch == QLatin1Char(':') && prev == QLatin1Char(':')))
        scheduleCompletion(2, ch == QLatin1Char('>') ? QStringLiteral(">") : QString(ch));
}

void CodeEditor::scheduleCompletion(int kind, const QString &triggerChar)
{
    if (!m_completionTimer) {
        m_completionTimer = new QTimer(this);
        m_completionTimer->setSingleShot(true);
        connect(m_completionTimer, &QTimer::timeout, this, [this] { requestCompletion(m_pendingKind, m_pendingTrigger); });
    }
    m_pendingKind = kind;
    m_pendingTrigger = triggerChar;
    // A re-ask for an incomplete list waits for a typing pause (the popup is already filtering locally meanwhile).
    m_completionTimer->start(kind == 3 ? 250 : 25);
}

void CodeEditor::triggerCompletion()
{
    m_completionManual = true;
    requestCompletion(1, {});
}

void CodeEditor::requestCompletion(int kind, const QString &triggerChar)
{
    const int cur = textCursor().position();
    int start = cur;
    while (start > 0 && isIdentifierChar(document()->characterAt(start - 1)))
        --start;
    QChar opener;
    QString typed;
    const bool include = includeAt(cur, &opener, &typed);
    if (include) // the name after the last '/' (it may hold '.' and '-')
        start = cur - (typed.size() - typed.lastIndexOf(QLatin1Char('/')) - 1);
    m_completionAnchor = start;
    const int token = ++m_completionToken;
    const bool served = (m_canGoToDefinition && m_canGoToDefinition()) || !localCompletions().isEmpty();
    if (include && !served) { // no language server: the folders on disk
        m_completionManual = false;
        const QString sub = typed.left(qMax(0, int(typed.lastIndexOf(QLatin1Char('/')))));
        QVector<LspCompletionItem> items;
        if (m_includeFolders) {
            const QChar closer = opener == QLatin1Char('<') ? QLatin1Char('>') : QLatin1Char('"');
            for (const CppTools::PathEntry &e : CppTools::includeEntries(m_includeFolders(opener), sub)) {
                LspCompletionItem item;
                item.label = e.dir ? e.name + QLatin1Char('/') : e.name;
                item.filterText = e.name;
                item.insertText = e.dir ? e.name + QLatin1Char('/') : e.name + closer;
                item.kind = e.dir ? 19 : 17;
                items.append(item);
            }
        }
        showCompletions(items, false, token);
        return;
    }
    if (served) {
        m_completionManual = false;
        const QTextCursor c = textCursor();
        emit completionRequested(c.blockNumber(), c.positionInBlock(), kind, triggerChar, token);
        return;
    }
    // No server: offer the words that already appear in the document, but only when asked (Ctrl+Space); while typing,
    // the popup then shows just our snippets.
    const bool manual = m_completionManual;
    m_completionManual = false;
    if (!manual) {
        showCompletions({}, false, token);
        return;
    }
    QSet<QString> seen;
    QVector<LspCompletionItem> items;
    static const QRegularExpression word(QStringLiteral("[A-Za-z_][A-Za-z0-9_]{2,}"));
    const QString all = toPlainText();
    for (auto it = word.globalMatch(all); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart() == start || seen.contains(m.captured()))
            continue;
        seen.insert(m.captured());
        LspCompletionItem item;
        item.label = m.captured();
        item.kind = 1;
        items.append(item);
        if (items.size() >= 5000)
            break;
    }
    showCompletions(items, false, token);
}

// C/C++: after '.' on a pointer, clangd lists the members with an edit that turns the '.' into "->" (it does so because the
// client says `editsNearCursor`). When every item carries that edit the receiver is a plain pointer, so the '.' becomes
// "->" right away and `shifted` gets the items rewritten for the text after it. A smart pointer (members both ways) is left
// alone: accepting an item makes the right change.
bool CodeEditor::smartArrow(const QVector<LspCompletionItem> &items, QVector<LspCompletionItem> *shifted)
{
    if ((m_language != QLatin1String("C++") && m_language != QLatin1String("C")) || items.isEmpty() || m_completionAnchor < 1)
        return false;
    const int dot = m_completionAnchor - 1;
    if (document()->characterAt(dot) != QLatin1Char('.'))
        return false;
    const QTextBlock block = document()->findBlock(dot);
    const int line = block.blockNumber(), column = dot - block.position();
    for (const LspCompletionItem &it : items)
        if (!it.hasEdit || it.edit.startLine != line || it.edit.startColumn != column || !it.edit.text.startsWith(QLatin1String("->")))
            return false;
    const int cur = textCursor().position();
    QTextCursor c(document());
    c.setPosition(dot);
    c.setPosition(dot + 1, QTextCursor::KeepAnchor);
    c.insertText(QStringLiteral("->"));
    QTextCursor caret = textCursor();
    caret.setPosition(cur + 1);
    setTextCursor(caret);
    ++m_completionAnchor;
    *shifted = items;
    for (LspCompletionItem &it : *shifted) {
        it.edit.startColumn += 2;
        if (it.edit.endLine == line)
            it.edit.endColumn += 1;
        it.edit.text = it.edit.text.mid(2);
    }
    return true;
}

void CodeEditor::showCompletions(const QVector<LspCompletionItem> &listed, bool incomplete, int token)
{
    if (token != m_completionToken || m_completionAnchor < 0 || !hasFocus())
        return;
    QVector<LspCompletionItem> arrowItems;
    const QVector<LspCompletionItem> &items = smartArrow(listed, &arrowItems) ? arrowItems : listed;
    const int cur = textCursor().position();
    const QString prefix = completionPrefix();
    const bool include = includeAt(cur);
    bool wordOnly = cur >= m_completionAnchor;
    for (const QChar c : prefix)
        wordOnly = wordOnly && (isIdentifierChar(c) || (include && c != QLatin1Char('/')));
    const QVector<LspCompletionItem> local = include ? QVector<LspCompletionItem>() : localCompletions();
    // Snippets (class, for, fun ...) make no sense right after a dot, or in an include's path.
    static const QVector<LspCompletionItem> noSnippets;
    const QChar before = m_completionAnchor > 0 ? document()->characterAt(m_completionAnchor - 1) : QChar();
    const bool afterDot = before == QLatin1Char('.') ||
                          (before == QLatin1Char('>') && m_completionAnchor > 1 && document()->characterAt(m_completionAnchor - 2) == QLatin1Char('-'));
    // QODE's own items in a string or right after `@file:` / `key=`: the language's snippets do not belong there.
    const bool localInString = !local.isEmpty() && ((!completesInStrings(m_language) && inStringOrComment(cur))
                                                    || QStringLiteral(":@\"=").contains(before));
    const QVector<LspCompletionItem> &snippets = afterDot || include || localInString ? noSnippets : Snippets::forLanguage(m_language);
    if (!wordOnly || (items.isEmpty() && local.isEmpty() && snippets.isEmpty())) {
        hideCompletion();
        return;
    }
    if (!m_completion) {
        m_completion = new CompletionPopup(this);
        m_completion->setFont(font());
        connect(m_completion, &CompletionPopup::accepted, this, &CodeEditor::acceptCompletion);
        connect(m_completion, &CompletionPopup::currentItemChanged, this, [this] {
            if (m_completion && m_completion->isVisible())
                m_docsTimer->start();
        });
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &CodeEditor::hideCompletion);
    }
    m_completionIncomplete = incomplete;
    m_docCache.clear();
    m_completion->setItems(local.isEmpty() ? items : local + items, snippets);
    if (m_completion->setPrefix(prefix) == 0) {
        hideCompletion();
        return;
    }
    QTextCursor anchor(document());
    anchor.setPosition(m_completionAnchor);
    const QRect r = cursorRect(anchor);
    m_completion->popup(viewport()->mapToGlobal(r.bottomLeft() + QPoint(0, 2)), viewport()->mapToGlobal(r.topLeft() - QPoint(0, 2)));
    m_docsTimer->start();
}

namespace {
// Parses an LSP snippet: "foo(${1:a}, $2)$0" -> text "foo(a, )" and the tab stops (index, range in the text).
// Supports $n, ${n}, ${n:placeholder} (nested), ${n|a,b|} (first choice), variables (replaced by their default or nothing)
// and the \\$ \\} \\\\ escapes. Line breaks get `indent` appended so multi-line snippets follow the current indentation.
struct SnippetStopSpec {
    int index, start, end;
};

class SnippetParser
{
public:
    SnippetParser(const QString &src, const QString &indent, const QString &unit) : s(src), m_indent(indent), m_unit(unit) { parse(false); }
    QString text;
    QVector<SnippetStopSpec> stops;

private:
    const QString &s;
    QString m_indent, m_unit;
    int i = 0;

    int readNumber()
    {
        int n = 0;
        while (i < s.size() && s.at(i).isDigit())
            n = n * 10 + s.at(i++).digitValue();
        return n;
    }
    void skipName()
    {
        while (i < s.size() && (s.at(i).isLetterOrNumber() || s.at(i) == QLatin1Char('_')))
            ++i;
    }
    void parse(bool nested)
    {
        while (i < s.size()) {
            const QChar c = s.at(i);
            if (nested && c == QLatin1Char('}'))
                return;
            if (c == QLatin1Char('\\') && i + 1 < s.size() &&
                (s.at(i + 1) == QLatin1Char('$') || s.at(i + 1) == QLatin1Char('}') || s.at(i + 1) == QLatin1Char('\\'))) {
                text += s.at(i + 1);
                i += 2;
                continue;
            }
            if (c == QLatin1Char('\n')) {
                text += c + m_indent;
                ++i;
                continue;
            }
            if (c == QLatin1Char('\t')) { // one indentation level, in the user's style
                text += m_unit;
                ++i;
                continue;
            }
            if (c != QLatin1Char('$') || i + 1 >= s.size()) {
                text += c;
                ++i;
                continue;
            }
            ++i; // the '$'
            if (s.at(i).isDigit()) {
                stops.append({readNumber(), int(text.size()), int(text.size())});
            } else if (s.at(i) == QLatin1Char('{')) {
                ++i;
                if (i < s.size() && s.at(i).isDigit()) {
                    const int n = readNumber();
                    const int start = text.size();
                    if (i < s.size() && s.at(i) == QLatin1Char(':')) {
                        ++i;
                        parse(true);
                    } else if (i < s.size() && s.at(i) == QLatin1Char('|')) {
                        ++i;
                        const int close = s.indexOf(QLatin1String("|}"), i);
                        const QString choices = close < 0 ? s.mid(i) : s.mid(i, close - i);
                        text += choices.section(QLatin1Char(','), 0, 0);
                        i = close < 0 ? s.size() : close + 1;
                    }
                    stops.append({n, start, int(text.size())});
                } else {
                    skipName(); // ${VAR} / ${VAR:default}
                    if (i < s.size() && s.at(i) == QLatin1Char(':')) {
                        ++i;
                        parse(true);
                    }
                }
                if (i < s.size() && s.at(i) == QLatin1Char('}'))
                    ++i;
            } else if (s.at(i).isLetter() || s.at(i) == QLatin1Char('_')) {
                skipName(); // $VAR
            } else {
                text += QLatin1Char('$');
            }
        }
    }
};
}

void CodeEditor::acceptCompletion()
{
    if (!m_completion)
        return;
    const LspCompletionItem *picked = m_completion->current();
    if (!picked) {
        hideCompletion();
        return;
    }
    const LspCompletionItem item = *picked;
    const int cur = textCursor().position();
    const int anchor = m_completionAnchor;
    hideCompletion();

    // Follows the edits the server and we make, so the word we completed can be found again afterwards.
    QTextCursor mark(document());
    mark.setPosition(anchor);
    mark.setKeepPositionOnInsert(true);

    if (!item.command.isEmpty() && m_commandRunner) {
        const QTextCursor c = textCursor();
        const int revision = document()->revision();
        QPointer<CodeEditor> guard(this);
        const bool taken = m_commandRunner(item, c.blockNumber(), c.positionInBlock(), [guard, item, anchor, cur, revision, mark](bool done) {
            if (!guard)
                return;
            if (!done && guard->document()->revision() == revision)
                guard->insertCompletion(item, anchor, cur);
            guard->finishCompletion(item, mark);
        });
        if (taken)
            return;
    }
    if (item.resolveOnAccept && !item.resolveData.isEmpty() && m_resolver) {
        // The editor stays usable while the server answers: insert only when nothing was typed meanwhile.
        const int revision = document()->revision();
        QPointer<CodeEditor> guard(this);
        m_resolver(item, [guard, anchor, cur, revision, mark](const LspCompletionItem &resolved) {
            if (guard && guard->document()->revision() == revision) {
                guard->insertCompletion(resolved, anchor, cur);
                guard->finishCompletion(resolved, mark);
            }
        });
        return;
    }
    const bool include = includeAt(cur);
    insertCompletion(item, anchor, cur);
    if (include)
        finishInclude();
    else
        finishCompletion(item, mark);
}

// After an include path was completed: a folder goes on to its own entries, and a file name that brought its closer along
// does not leave the one the editor paired behind it (`#include <vector>>`).
void CodeEditor::finishInclude()
{
    QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    QChar opener;
    if (before.endsWith(QLatin1Char('/')) && CppTools::includeContext(before, &opener)) {
        // clangd's edit for a folder also replaces the closer that was paired: put it back behind the caret.
        const QChar closer = opener == QLatin1Char('<') ? QLatin1Char('>') : QLatin1Char('"');
        if (document()->characterAt(c.position()) != closer) {
            c.joinPreviousEditBlock();
            c.insertText(QString(closer));
            c.endEditBlock();
            c.movePosition(QTextCursor::Left);
            setTextCursor(c);
        }
        scheduleCompletion(2, QStringLiteral("/"));
        return;
    }
    const QChar last = before.isEmpty() ? QChar() : before.back();
    if ((last == QLatin1Char('>') || last == QLatin1Char('"')) && document()->characterAt(c.position()) == last &&
        CppTools::includeContext(before.left(before.size() - 1))) {
        c.joinPreviousEditBlock();
        c.deleteChar();
        c.endEditBlock();
    }
}

bool CodeEditor::includeAt(int position, QChar *opener, QString *typed) const
{
    const QTextBlock block = document()->findBlock(position);
    if (!block.isValid() || !block.text().contains(QLatin1Char('#')))
        return false;
    return CppTools::includeContext(block.text().left(position - block.position()), opener, typed);
}

// What an IDE adds after a completed word: a function gets its parentheses with the caret between them, and a named argument
// of function type (`onClick`) becomes `onClick = {}` with the caret inside the braces. Works on the text as it is after the
// server's own edit, so it only adds what the server left out.
void CodeEditor::finishCompletion(const LspCompletionItem &item, const QTextCursor &mark)
{
    if (item.builtin || item.snippet || !m_snippetStops.isEmpty() || isReadOnly() || mark.isNull())
        return;
    const QString label = item.label.trimmed();
    QString name;
    for (const QChar ch : label) {
        if (!isIdentifierChar(ch))
            break;
        name += ch;
    }
    if (name.isEmpty())
        return;
    const QTextBlock block = mark.block();
    const QString line = block.text();
    const int col = mark.positionInBlock();
    if (!block.isValid() || !line.mid(col).startsWith(name))
        return;
    const QString first = line.trimmed();
    if (first.startsWith(QLatin1String("import ")) || first.startsWith(QLatin1String("package ")))
        return;
    const int end = col + name.size();
    const QString rest = line.mid(end);
    const int caretNow = textCursor().position();
    if (caretNow < block.position() + col || caretNow > block.position() + line.size())
        return; // the caret went elsewhere meanwhile

    const bool isFunction = item.kind == 2 || item.kind == 3 || item.kind == 4; // method, function, constructor
    const bool namedArgument = label.endsWith(QLatin1Char('='));
    const QString typeText = item.detail + item.signature + item.longDetail;
    QTextCursor c(document());
    c.joinPreviousEditBlock();
    int caret = -1;
    if (isFunction && !namedArgument && !(col >= 2 && line.mid(col - 2, 2) == QLatin1String("::"))) {
        if (rest.startsWith(QLatin1String("()"))) {
            caret = block.position() + end + 1;
        } else if (!rest.startsWith(QLatin1Char('(')) && !rest.startsWith(QLatin1String(" {"))) {
            c.setPosition(block.position() + end);
            c.insertText(QStringLiteral("()"));
            caret = block.position() + end + 1;
        }
    } else if (namedArgument && typeText.contains(QLatin1String("->"))) {
        int k = 0;
        while (k < rest.size() && rest[k].isSpace())
            ++k;
        if (k < rest.size() && rest[k] == QLatin1Char('='))
            ++k;
        while (k < rest.size() && rest[k].isSpace())
            ++k;
        const QString after = rest.mid(k);
        if (after.isEmpty() || after[0] == QLatin1Char(',') || after[0] == QLatin1Char(')')) {
            c.setPosition(block.position() + end);
            c.setPosition(block.position() + end + k, QTextCursor::KeepAnchor);
            c.insertText(QStringLiteral(" = {}"));
            caret = block.position() + end + 4;
        }
    }
    c.endEditBlock();
    if (caret >= 0) {
        QTextCursor place(document());
        place.setPosition(caret);
        setTextCursor(place);
    }
}

void CodeEditor::insertCompletion(const LspCompletionItem &item, int anchor, int cur)
{
    QTextDocument *doc = document();
    auto position = [doc](int line, int column) {
        const QTextBlock b = doc->findBlockByNumber(line);
        if (!b.isValid())
            return doc->characterCount() - 1;
        return b.position() + qMin(column, b.length() - 1);
    };
    struct Edit {
        int start, end;
        QString text;
    };
    QString text = item.hasEdit ? item.edit.text : (item.insertText.isEmpty() ? item.label.trimmed() : item.insertText);
    int start = anchor, end = cur;
    if (item.hasEdit) {
        start = position(item.edit.startLine, item.edit.startColumn);
        end = qMax(position(item.edit.endLine, item.edit.endColumn), cur); // keep what was typed since the request
    }
    QVector<SnippetStopSpec> stops;
    if (item.snippet) {
        QString indent;
        for (const QChar ch : doc->findBlock(start).text()) {
            if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t'))
                break;
            indent += ch;
        }
        const SnippetParser parsed(text, indent, indentUnit());
        text = parsed.text;
        stops = parsed.stops;
    }
    QVector<Edit> edits{{start, end, text}};
    for (const LspTextEdit &e : item.additionalEdits)
        edits.append({position(e.startLine, e.startColumn), position(e.endLine, e.endColumn), e.text});
    std::stable_sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.start > b.start; });

    int shift = 0; // how much the extra edits above the insertion moved it
    for (const Edit &e : std::as_const(edits))
        if (e.end <= start && !(e.start == start && e.end == end))
            shift += e.text.size() - (e.end - e.start);

    QTextCursor c(doc);
    c.beginEditBlock();
    for (const Edit &e : std::as_const(edits)) {
        c.setPosition(e.start);
        c.setPosition(e.end, QTextCursor::KeepAnchor);
        c.insertText(e.text);
    }
    c.endEditBlock();
    const int base = start + shift;
    clearSnippet();
    if (!stops.isEmpty()) {
        // Order: 1, 2, ... then $0 (or the end of the inserted text when there is none).
        std::stable_sort(stops.begin(), stops.end(), [](const SnippetStopSpec &a, const SnippetStopSpec &b) {
            return (a.index == 0 ? INT_MAX : a.index) < (b.index == 0 ? INT_MAX : b.index);
        });
        // One entry per index; further occurrences become mirrors of the first.
        auto range = [&](const SnippetStopSpec &st) {
            QTextCursor f(doc), t(doc);
            f.setPosition(base + st.start);
            f.setKeepPositionOnInsert(true);
            t.setPosition(base + st.end);
            return qMakePair(f, t);
        };
        QVector<int> indices;
        for (const SnippetStopSpec &st : std::as_const(stops)) {
            const auto r = range(st);
            if (!indices.isEmpty() && indices.last() == st.index) {
                m_snippetStops.last().mirrors.append(r);
            } else {
                indices.append(st.index);
                m_snippetStops.append({r.first, r.second, {}});
            }
        }
        if (indices.last() != 0) { // no $0: the caret ends after the inserted text
            const auto r = range({0, int(text.size()), int(text.size())});
            m_snippetStops.append({r.first, r.second, {}});
        }
        m_snippetStart = QTextCursor(doc);
        m_snippetStart.setPosition(base);
        m_snippetEnd = QTextCursor(doc);
        m_snippetEnd.setPosition(base + text.size());
        m_snippetAt = 0;
        selectSnippetStop(0);
        if (m_snippetStops.size() == 1)
            clearSnippet(); // only the final position
    } else {
        c.setPosition(base + text.size());
        setTextCursor(c);
    }
    ensureCursorVisible();
}

// --- Emmet -----------------------------------------------------------------------------------------

bool CodeEditor::expandEmmet()
{
    if (!m_emmetMode || textCursor().hasSelection() || completionVisible())
        return false;
    const bool jsx = m_emmetMode == 2;
    const QTextCursor caret = textCursor();
    const QString line = caret.block().text();
    const int col = caret.positionInBlock();
    if (col < line.size() && !line.at(col).isSpace() && line.at(col) != QLatin1Char('<'))
        return false; // only at the end of a word
    const int start = Emmet::findAbbreviation(line, col, jsx);
    if (start < 0)
        return false;
    const QString abbr = line.mid(start, col - start);
    const QString before = line.left(start);
    // Inside an attribute value or a JSX expression string.
    if (before.count(QLatin1Char('"')) % 2 == 1 || before.count(QLatin1Char('`')) % 2 == 1)
        return false;
    if (!jsx) {
        // Not inside <style> / <script>.
        QTextCursor back(document());
        back.setPosition(qMax(0, caret.position() - 20000));
        back.setPosition(caret.position(), QTextCursor::KeepAnchor);
        const QString text = back.selectedText();
        for (const QString &tag : {QStringLiteral("style"), QStringLiteral("script")})
            if (text.lastIndexOf(QStringLiteral("<") + tag, -1, Qt::CaseInsensitive) > text.lastIndexOf(QStringLiteral("</") + tag, -1, Qt::CaseInsensitive))
                return false;
    } else {
        // In JSX files an abbreviation is markup only where an expression can start: line start, after `(`, `>`, `return` ...
        const QString prefix = before.trimmed();
        static const QStringList enders = {QStringLiteral(">"), QStringLiteral("("), QStringLiteral("?"), QStringLiteral(":"), QStringLiteral("&&"),
                                           QStringLiteral("||"), QStringLiteral("=>"), QStringLiteral("return"), QStringLiteral("{")};
        bool ok = prefix.isEmpty();
        for (const QString &e : enders)
            ok = ok || prefix.endsWith(e);
        if (!ok)
            return false;
    }
    // A plain word must be a tag (or a custom element); anything with Emmet operators is taken as an abbreviation, but in
    // JSX its first name has to be a tag too, so `foo.bar` in code stays code.
    static const QString operators = QStringLiteral(".#>+^*[]{}()$@");
    const QString first = abbr.section(QRegularExpression(QStringLiteral("[^A-Za-z0-9:_-]")), 0, 0);
    static const QSet<QString> tags = {QStringLiteral("a"), QStringLiteral("abbr"), QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"), QStringLiteral("audio"), QStringLiteral("b"), QStringLiteral("blockquote"), QStringLiteral("body"), QStringLiteral("br"), QStringLiteral("button"), QStringLiteral("canvas"), QStringLiteral("caption"), QStringLiteral("code"), QStringLiteral("div"), QStringLiteral("dl"), QStringLiteral("dt"), QStringLiteral("dd"), QStringLiteral("em"), QStringLiteral("fieldset"), QStringLiteral("figure"), QStringLiteral("footer"), QStringLiteral("form"), QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"), QStringLiteral("h6"), QStringLiteral("head"), QStringLiteral("header"), QStringLiteral("hr"), QStringLiteral("html"), QStringLiteral("i"), QStringLiteral("iframe"), QStringLiteral("img"), QStringLiteral("input"), QStringLiteral("label"), QStringLiteral("legend"), QStringLiteral("li"), QStringLiteral("link"), QStringLiteral("main"), QStringLiteral("meta"), QStringLiteral("nav"), QStringLiteral("ol"), QStringLiteral("option"), QStringLiteral("p"), QStringLiteral("pre"), QStringLiteral("script"), QStringLiteral("section"), QStringLiteral("select"), QStringLiteral("small"), QStringLiteral("span"), QStringLiteral("strong"), QStringLiteral("style"), QStringLiteral("sub"), QStringLiteral("sup"), QStringLiteral("svg"), QStringLiteral("table"), QStringLiteral("tbody"), QStringLiteral("td"), QStringLiteral("textarea"), QStringLiteral("tfoot"), QStringLiteral("th"), QStringLiteral("thead"), QStringLiteral("title"), QStringLiteral("tr"), QStringLiteral("ul"), QStringLiteral("video"),
        // Emmet's short names
        QStringLiteral("bq"), QStringLiteral("btn"), QStringLiteral("fig"), QStringLiteral("figc"), QStringLiteral("ifr"), QStringLiteral("sect"), QStringLiteral("art"), QStringLiteral("hdr"), QStringLiteral("ftr"), QStringLiteral("inp"), QStringLiteral("str"), QStringLiteral("opt"), QStringLiteral("tarea")};
    const bool known = first.isEmpty() || tags.contains(first.section(QLatin1Char(':'), 0, 0)) || first.contains(QLatin1Char('-'));
    bool hasOperator = false;
    for (const QChar c : abbr)
        hasOperator = hasOperator || operators.contains(c);
    if (abbr == QLatin1String("!") && !jsx) {
        // the HTML5 skeleton
    } else if (!hasOperator && !known) {
        return false;
    } else if (!known && (jsx || !hasOperator)) {
        return false;
    }
    Emmet::Options options;
    options.jsx = jsx;
    options.indent = indentUnit();
    const QString expanded = Emmet::expand(abbr, options);
    if (expanded.isEmpty())
        return false;
    LspCompletionItem item;
    item.label = abbr;
    item.snippet = true;
    item.insertText = expanded;
    const int anchor = caret.block().position() + start;
    insertCompletion(item, anchor, caret.position());
    return true;
}

// --- Colour swatches ---------------------------------------------------------------------------------
