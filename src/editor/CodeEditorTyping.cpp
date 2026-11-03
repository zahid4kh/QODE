// What typing does beyond inserting the character: indentation, auto-closed pairs and tags, linked tag names,
// snippet tab stops, Backspace in indentation, Duplicate Line.
#include "CodeEditor.h"

#include "CodeEditorText.h"
#include "cpp/CppTools.h"
#include "settings/SettingsManager.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>

using namespace CodeEditorText;

namespace {

bool isBlankBefore(const QString &text, int pos)
{
    for (int i = 0; i < pos; ++i)
        if (!text.at(i).isSpace())
            return false;
    return true;
}

// The indentation the file itself uses (tabs, or the most common step between nested lines), so new lines line up with
// the existing ones whatever the global tab size says. Empty when the file shows no clear convention.
QString detectIndent(QTextDocument *doc)
{
    int tabs = 0, spaces = 0, prev = 0;
    QHash<int, int> steps;
    int seen = 0;
    for (QTextBlock b = doc->begin(); b.isValid() && seen < 1500; b = b.next(), ++seen) {
        const QString t = b.text();
        int n = 0;
        while (n < t.size() && (t.at(n) == QLatin1Char(' ') || t.at(n) == QLatin1Char('\t')))
            ++n;
        if (n == t.size())
            continue; // blank
        if (t.at(0) == QLatin1Char('\t')) {
            ++tabs;
            prev = 0;
            continue;
        }
        if (n > 0) {
            ++spaces;
            const int d = qAbs(n - prev);
            if (d >= 1 && d <= 8)
                ++steps[d];
        }
        prev = n;
    }
    if (tabs > spaces)
        return QStringLiteral("\t");
    int best = 0, bestCount = 0;
    for (auto it = steps.constBegin(); it != steps.constEnd(); ++it)
        if (it.value() > bestCount || (it.value() == bestCount && it.key() < best)) {
            best = it.key();
            bestCount = it.value();
        }
    return bestCount >= 2 ? QString(best, QLatin1Char(' ')) : QString();
}

} // namespace

QString CodeEditor::indentUnit() const
{
    // .editorconfig first, then what the file already does, then the settings.
    auto &s = SettingsManager::instance();
    if (m_ecTabs == 1)
        return QStringLiteral("\t");
    if (m_ecTabs == 0)
        return QString(m_ecIndent > 0 ? m_ecIndent : m_ecTabWidth > 0 ? m_ecTabWidth : s.tabSize(), QLatin1Char(' '));
    const QString detected = detectIndent(document());
    if (!detected.isEmpty())
        return detected;
    if (m_ecIndent > 0)
        return s.useSpaces() ? QString(m_ecIndent, QLatin1Char(' ')) : QStringLiteral("\t");
    return s.useSpaces() ? QString(s.tabSize(), QLatin1Char(' ')) : QStringLiteral("\t");
}

int CodeEditor::tabWidth() const
{
    if (m_ecTabWidth > 0)
        return m_ecTabWidth;
    if (m_ecTabs == 1 && m_ecIndent > 0)
        return m_ecIndent; // indent_style = tab with indent_size: the tab is that wide
    return qMax(1, SettingsManager::instance().tabSize());
}

void CodeEditor::setIndentOverride(int useTabs, int indentSize, int tabWidth)
{
    m_ecTabs = useTabs;
    m_ecIndent = indentSize;
    m_ecTabWidth = tabWidth;
    setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * this->tabWidth());
    viewport()->update();
}

namespace {

bool isMarkupLanguage(const QString &lang)
{
    return lang == QLatin1String("TypeScript") || lang == QLatin1String("JavaScript") || lang == QLatin1String("HTML") ||
           lang == QLatin1String("XML");
}

// How the text of a line (up to the caret) leaves an HTML / JSX tag:
//  Attributes - an opening tag is started but not finished (`<motion.a`), so the next line holds its attributes;
//  Children   - the line ends an opening tag (`<div>`, or the lone `>` closing a multi-line tag), so content goes inside.
enum class TagState { None, Attributes, Children };

// Walks `t` (the text up to a position) tracking the last HTML / JSX tag opened: where it starts, and whether it is still
// unfinished (inside its attributes) or was closed (`>` or `/>`).
struct TagScan {
    int start = -1;
    bool open = false, selfClosed = false;
};

TagScan scanTags(const QString &t)
{
    TagScan r;
    int braces = 0;
    QChar quote;
    for (int i = 0; i < t.size(); ++i) {
        const QChar ch = t.at(i);
        if (!quote.isNull()) {
            if (ch == quote)
                quote = QChar();
            continue;
        }
        if (r.open) {
            if (ch == QLatin1Char('"') || ch == QLatin1Char('\''))
                quote = ch;
            else if (ch == QLatin1Char('{'))
                ++braces;
            else if (ch == QLatin1Char('}'))
                --braces;
            else if (ch == QLatin1Char('>') && braces <= 0 && !(i > 0 && t.at(i - 1) == QLatin1Char('='))) {
                r.selfClosed = i > 0 && t.at(i - 1) == QLatin1Char('/');
                r.open = false;
            }
            continue;
        }
        if (ch == QLatin1Char('<')) {
            const QChar prev = i > 0 ? t.at(i - 1) : QLatin1Char(' ');
            const QChar next = i + 1 < t.size() ? t.at(i + 1) : QLatin1Char('>');
            if (next == QLatin1Char('/')) { // `</div>` closes whatever was open on this line
                r.start = -1;
                continue;
            }
            // `Array<string>` is a generic, `a < b` a comparison and `<!--` a comment: not openers.
            if (prev.isLetterOrNumber() || prev == QLatin1Char('_') || prev == QLatin1Char(')'))
                continue;
            if (!(next.isLetter() || next == QLatin1Char('>')))
                continue;
            r.start = i;
            r.open = true;
            braces = 0;
            r.selfClosed = false;
        }
    }
    return r;
}

QString tagNameAt(const QString &t, int start)
{
    int e = start + 1;
    while (e < t.size() && (t.at(e).isLetterOrNumber() || t.at(e) == QLatin1Char('.') || t.at(e) == QLatin1Char('-') ||
                            t.at(e) == QLatin1Char(':') || t.at(e) == QLatin1Char('_')))
        ++e;
    return t.mid(start + 1, e - start - 1);
}

bool isVoidTag(const QString &name)
{
    static const QSet<QString> voidTags = {QStringLiteral("br"),    QStringLiteral("hr"),     QStringLiteral("img"),
                                           QStringLiteral("input"), QStringLiteral("meta"),   QStringLiteral("link"),
                                           QStringLiteral("area"),  QStringLiteral("base"),   QStringLiteral("col"),
                                           QStringLiteral("embed"), QStringLiteral("source"), QStringLiteral("track"),
                                           QStringLiteral("wbr"),   QStringLiteral("param")};
    return voidTags.contains(name.toLower());
}

TagState tagState(const QString &before, bool *closerBelow, const QString &after)
{
    *closerBelow = false;
    const QString t = before.trimmed();
    if (t.isEmpty())
        return TagState::None;
    const TagScan scan = scanTags(t);
    if (scan.start < 0)
        return TagState::None;
    if (scan.open)
        return scan.start == 0 ? TagState::Attributes : TagState::None;
    if (scan.selfClosed || isVoidTag(tagNameAt(t, scan.start)) || !t.endsWith(QLatin1Char('>')))
        return TagState::None;
    *closerBelow = after.trimmed().startsWith(QLatin1String("</"));
    return TagState::Children;
}

} // namespace

namespace {
bool isTagNameChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char(':');
}

// End of the tag whose attributes start at `from` (position of the '>'), or -1.
int tagEnd(const QString &t, int from, bool *selfClosed)
{
    int braces = 0;
    QChar quote;
    const int limit = qMin(int(t.size()), from + 6000);
    for (int i = from; i < limit; ++i) {
        const QChar ch = t.at(i);
        if (!quote.isNull()) {
            if (ch == quote)
                quote = QChar();
        } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            quote = ch;
        } else if (ch == QLatin1Char('{')) {
            ++braces;
        } else if (ch == QLatin1Char('}')) {
            --braces;
        } else if (ch == QLatin1Char('>') && braces <= 0 && !(i > 0 && t.at(i - 1) == QLatin1Char('='))) {
            *selfClosed = i > 0 && t.at(i - 1) == QLatin1Char('/');
            return i;
        } else if (ch == QLatin1Char('<') && braces <= 0) {
            return -1; // a new tag starts: this one was never finished
        }
    }
    return -1;
}

// Start of the name that pairs with the tag name at `nameStart` (`<name` <-> `</name`), or -1.
int findTagPair(const QString &t, int nameStart, const QString &name)
{
    const QRegularExpression re(QStringLiteral("<(/?)") + QRegularExpression::escape(name) + QStringLiteral("(?![\\w.:-])"));
    QVector<int> stack; // name starts of the open tags
    for (auto it = re.globalMatch(t); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const int start = int(m.capturedEnd() - name.size());
        if (m.captured(1).isEmpty()) {
            const QChar prev = m.capturedStart() > 0 ? t.at(m.capturedStart() - 1) : QLatin1Char(' ');
            if (prev.isLetterOrNumber() || prev == QLatin1Char('_') || prev == QLatin1Char(')'))
                continue; // a generic or a comparison
            bool selfClosed = false;
            if (tagEnd(t, m.capturedEnd(), &selfClosed) < 0 || selfClosed)
                continue; // `<name />` has no partner
            stack.append(start);
        } else if (!stack.isEmpty()) {
            const int open = stack.takeLast();
            if (start == nameStart)
                return open;
            if (open == nameStart)
                return start;
        }
    }
    return -1;
}
} // namespace

// Starts a linked-rename session when the selection lies in a tag name that has a partner.
bool CodeEditor::beginTagLink()
{
    const QTextCursor c = textCursor();
    const QTextBlock block = c.block();
    const QString line = block.text();
    const int s = c.selectionStart() - block.position(), e = c.selectionEnd() - block.position();
    if (c.selectionStart() < block.position() || c.selectionEnd() > block.position() + line.size())
        return false;
    int ns = s, ne = e;
    while (ns > 0 && isTagNameChar(line.at(ns - 1)))
        --ns;
    while (ne < line.size() && isTagNameChar(line.at(ne)))
        ++ne;
    const bool closing = ns >= 2 && line.at(ns - 1) == QLatin1Char('/') && line.at(ns - 2) == QLatin1Char('<');
    const bool opening = ns >= 1 && line.at(ns - 1) == QLatin1Char('<');
    if (ne == ns || (!closing && !opening))
        return false; // cheap rejection: this is not a tag name
    const QString name = line.mid(ns, ne - ns);
    const QString all = toPlainText();
    const int nameStart = block.position() + ns;
    const int pair = findTagPair(all, nameStart, name);
    if (pair < 0)
        return false;
    auto cursorAt = [this](int pos, bool keep) {
        QTextCursor t(document());
        t.setPosition(pos);
        t.setKeepPositionOnInsert(keep);
        return t;
    };
    m_tagLink.aFrom = cursorAt(nameStart, true);
    m_tagLink.aTo = cursorAt(nameStart + name.size(), false);
    m_tagLink.bFrom = cursorAt(pair, true);
    m_tagLink.bTo = cursorAt(pair + name.size(), false);
    m_tagLink.active = true;
    return true;
}

void CodeEditor::syncTagLink()
{
    if (!m_tagLink.active)
        return;
    const int af = m_tagLink.aFrom.position(), at = m_tagLink.aTo.position();
    const int bf = m_tagLink.bFrom.position(), bt = m_tagLink.bTo.position();
    if (at < af || bt < bf) {
        m_tagLink.active = false;
        return;
    }
    QTextCursor a(document());
    a.setPosition(af);
    a.setPosition(at, QTextCursor::KeepAnchor);
    const QString name = a.selectedText();
    for (const QChar ch : name)
        if (!isTagNameChar(ch)) { // the edit left the name (a space, `>` ...): the rename is over
            m_tagLink.active = false;
            return;
        }
    QTextCursor b(document());
    b.setPosition(bf);
    b.setPosition(bt, QTextCursor::KeepAnchor);
    if (b.selectedText() == name)
        return;
    b.insertText(name);
}

void CodeEditor::beforeKeyEdit(QKeyEvent *event)
{
    if (isReadOnly())
        return;
    const QString t = event->text();
    const bool edits = event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete ||
                       event->matches(QKeySequence::Paste) || event->matches(QKeySequence::Cut) ||
                       (t.size() == 1 && (isTagNameChar(t.at(0)) || t.at(0).isPrint()));
    if (!edits)
        return;
    if (isMarkupLanguage(m_language)) {
        bool keep = false;
        if (m_tagLink.active) { // still inside the name being renamed?
            const int p = textCursor().selectionStart(), q = textCursor().selectionEnd();
            keep = p >= m_tagLink.aFrom.position() && q <= m_tagLink.aTo.position();
            if (!keep)
                m_tagLink.active = false;
        }
        if (!keep && t.size() <= 1 && (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete ||
                                      event->matches(QKeySequence::Paste) || event->matches(QKeySequence::Cut) ||
                                      (t.size() == 1 && isTagNameChar(t.at(0)))))
            beginTagLink();
    }
    const bool mirrors = m_snippetAt >= 0 && m_snippetAt < m_snippetStops.size() && !m_snippetStops.at(m_snippetAt).mirrors.isEmpty();
    if ((m_tagLink.active || mirrors) && !m_keyGroupOpen) {
        // The typed character and the copies made after it are one undo step.
        m_keyGroup = QTextCursor(document());
        m_keyGroup.beginEditBlock();
        m_keyGroupOpen = true;
    }
}

void CodeEditor::afterKeyEdit()
{
    syncSnippetMirrors();
    syncTagLink();
    if (m_keyGroupOpen) {
        m_keyGroup.endEditBlock();
        m_keyGroupOpen = false;
    }
}

bool CodeEditor::autoCloseTag()
{
    if (!isMarkupLanguage(m_language) || textCursor().hasSelection() || inStringOrComment(textCursor().position()))
        return false;
    QTextCursor c = textCursor();
    // A window of the lines above is enough: an opening tag rarely spans more than a few.
    QTextCursor w(document());
    w.setPosition(document()->findBlockByNumber(qMax(0, c.blockNumber() - 60)).position());
    w.setPosition(c.position(), QTextCursor::KeepAnchor);
    const QString text = w.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    const TagScan scan = scanTags(text);
    if (scan.start < 0 || !scan.open || text.endsWith(QLatin1Char('/')) || text.endsWith(QLatin1Char('=')))
        return false;
    const QString name = tagNameAt(text, scan.start);
    // `<T>` in TypeScript is a generic, not a tag.
    if (isVoidTag(name) || (name.size() == 1 && name.at(0).isUpper()))
        return false;
    const QString line = c.block().text();
    if (line.mid(c.positionInBlock()).startsWith(QStringLiteral("</") + name))
        return false;
    c.beginEditBlock();
    c.insertText(QStringLiteral(">"));
    const int pos = c.position();
    c.insertText(QStringLiteral("</") + name + QLatin1Char('>'));
    c.setPosition(pos);
    c.endEditBlock();
    setTextCursor(c);
    return true;
}

void CodeEditor::clearSnippet()
{
    m_snippetStops.clear();
    m_snippetAt = -1;
}

void CodeEditor::selectSnippetStop(int i)
{
    QTextCursor c(document());
    c.setPosition(m_snippetStops.at(i).from.position());
    c.setPosition(qMax(m_snippetStops.at(i).to.position(), c.position()), QTextCursor::KeepAnchor);
    setTextCursor(c);
}

// Copies the text of the current tab stop into its mirrors (called after every key press).
void CodeEditor::syncSnippetMirrors()
{
    if (m_snippetAt < 0 || m_snippetAt >= m_snippetStops.size() || m_snippetStops.at(m_snippetAt).mirrors.isEmpty())
        return;
    const SnippetStop &stop = m_snippetStops.at(m_snippetAt);
    QTextCursor src(document());
    src.setPosition(stop.from.position());
    src.setPosition(qMax(stop.to.position(), src.position()), QTextCursor::KeepAnchor);
    const QString text = src.selectedText();
    for (const auto &m : stop.mirrors) {
        QTextCursor c(document());
        c.setPosition(m.first.position());
        c.setPosition(qMax(m.second.position(), c.position()), QTextCursor::KeepAnchor);
        if (c.selectedText() != text)
            c.insertText(text);
    }
}

bool CodeEditor::snippetJump(int direction)
{
    if (m_snippetStops.isEmpty())
        return false;
    const int cur = textCursor().position();
    if (cur < m_snippetStart.position() || cur > m_snippetEnd.position()) {
        clearSnippet();
        return false;
    }
    const int next = m_snippetAt + direction;
    if (next < 0)
        return false;
    if (next >= m_snippetStops.size()) {
        clearSnippet();
        return false;
    }
    m_snippetAt = next;
    selectSnippetStop(next);
    ensureCursorVisible();
    if (next == m_snippetStops.size() - 1)
        clearSnippet();
    return true;
}

void CodeEditor::insertNewlineWithIndent()
{
    QTextCursor c = textCursor();
    const QString line = c.block().text();
    const int col = c.positionInBlock();
    const QString before = line.left(col);
    const QString after = line.mid(col);

    QString indent;
    for (const QChar ch : line) {
        if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t'))
            indent += ch;
        else
            break;
    }
    if (col < indent.size())
        indent = indent.left(col);

    const QString trimmedBefore = before.trimmed();
    bool tagCloserBelow = false;
    TagState tag = TagState::None;
    if (isMarkupLanguage(m_language)) {
        tag = tagState(before, &tagCloserBelow, after);
        // The lone `>` / `/>` line that finishes a multi-line tag: the line it started on is what children hang from.
        if (tag == TagState::None && trimmedBefore == QLatin1String(">")) {
            tag = TagState::Children;
            tagCloserBelow = after.trimmed().startsWith(QLatin1String("</"));
        }
    }
    const bool opens = tag != TagState::None
        || (!trimmedBefore.isEmpty()
        && (trimmedBefore.endsWith(QLatin1Char('{')) || trimmedBefore.endsWith(QLatin1Char('(')) ||
            trimmedBefore.endsWith(QLatin1Char('[')) ||
            (m_indentAfterColon && trimmedBefore.endsWith(QLatin1Char(':')))));
    c.beginEditBlock();
    c.removeSelectedText();
    if (opens && !after.trimmed().isEmpty() && (tagCloserBelow || after.trimmed().startsWith(QLatin1Char('}')) ||
                                                 after.trimmed().startsWith(QLatin1Char(')')) ||
                                                 after.trimmed().startsWith(QLatin1Char(']')))) {
        // Cursor sits between a bracket pair: open an indented empty line and push the closer below.
        c.insertText(QStringLiteral("\n") + indent + indentUnit());
        const int pos = c.position();
        c.insertText(QStringLiteral("\n") + indent);
        c.setPosition(pos);
    } else {
        c.insertText(QStringLiteral("\n") + indent + (opens ? indentUnit() : QString()));
    }
    c.endEditBlock();
    setTextCursor(c);
    ensureCursorVisible();
}

void CodeEditor::indentSelection(bool outdent)
{
    QTextCursor c = textCursor();
    const QString unit = indentUnit();
    const int tab = unit.startsWith(QLatin1Char(' ')) ? unit.size() : tabWidth();

    QTextCursor start(document());
    start.setPosition(c.selectionStart());
    QTextCursor end(document());
    end.setPosition(c.selectionEnd());
    int firstBlock = start.blockNumber();
    int lastBlock = end.blockNumber();
    // A selection ending at column 0 of a line doesn't include that line.
    if (c.hasSelection() && end.positionInBlock() == 0 && lastBlock > firstBlock)
        --lastBlock;

    c.beginEditBlock();
    for (int b = firstBlock; b <= lastBlock; ++b) {
        QTextCursor lc(document()->findBlockByNumber(b));
        if (outdent) {
            const QString t = lc.block().text();
            int remove = 0;
            if (t.startsWith(QLatin1Char('\t')))
                remove = 1;
            else
                while (remove < tab && remove < t.size() && t.at(remove) == QLatin1Char(' '))
                    ++remove;
            if (remove > 0) {
                lc.movePosition(QTextCursor::StartOfBlock);
                lc.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, remove);
                lc.removeSelectedText();
            }
        } else if (!lc.block().text().isEmpty()) {
            lc.movePosition(QTextCursor::StartOfBlock);
            lc.insertText(unit);
        }
    }
    c.endEditBlock();
}

namespace {
QChar closerFor(QChar open)
{
    switch (open.unicode()) {
    case '(': return QLatin1Char(')');
    case '[': return QLatin1Char(']');
    case '{': return QLatin1Char('}');
    case '"': return QLatin1Char('"');
    case '\'': return QLatin1Char('\'');
    case '`': return QLatin1Char('`');
    default: return QChar();
    }
}
bool isQuote(QChar c) { return c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`'); }
bool isCloser(QChar c) { return c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}'); }
}

bool CodeEditor::autoPair(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
    const QString text = event->text();
    if (mods != Qt::NoModifier || text.size() != 1 || overwriteMode())
        return false;
    const QChar ch = text[0];
    if (ch == QLatin1Char('<') || ch == QLatin1Char('>')) { // `#include <|>`: the angle brackets pair there

        QTextCursor c = textCursor();
        const QString before = c.block().text().left(c.positionInBlock());
        if (c.hasSelection())
            return false;
        if (ch == QLatin1Char('<') && CppTools::atIncludeOpener(before) && !isIdentifierChar(document()->characterAt(c.position()))) {
            c.beginEditBlock();
            c.insertText(QStringLiteral("<>"));
            c.movePosition(QTextCursor::Left);
            c.endEditBlock();
            setTextCursor(c);
            return true;
        }
        if (ch == QLatin1Char('>') && document()->characterAt(c.position()) == ch && CppTools::includeContext(before)) {
            c.movePosition(QTextCursor::Right);
            setTextCursor(c);
            return true;
        }
        return false;
    }
    const QChar close = closerFor(ch);
    if (close.isNull() && !isCloser(ch))
        return false;

    QTextCursor c = textCursor();
    QTextDocument *doc = document();
    const int pos = c.position();
    const QChar next = c.hasSelection() ? QChar() : doc->characterAt(pos); // '\0' at the end of the document
    const QChar prev = pos > 0 ? doc->characterAt(pos - 1) : QChar();

    if (c.hasSelection()) { // wrap the selection and keep it selected
        if (close.isNull())
            return false;
        const int s = c.selectionStart(), e = c.selectionEnd();
        c.beginEditBlock();
        c.setPosition(e);
        c.insertText(QString(close));
        c.setPosition(s);
        c.insertText(QString(ch));
        c.setPosition(s + 1);
        c.setPosition(e + 1, QTextCursor::KeepAnchor);
        c.endEditBlock();
        setTextCursor(c);
        return true;
    }
    // Typing the closer that is already there just steps over it.
    if ((isCloser(ch) || isQuote(ch)) && next == ch) {
        c.movePosition(QTextCursor::Right);
        setTextCursor(c);
        return true;
    }
    if (close.isNull())
        return false;
    // Pair only where it is welcome: not right before a word, not inside strings/comments, and not an
    // apostrophe after a letter (don't) or a Rust lifetime.
    if (isIdentifierChar(next) || (next == ch))
        return false;
    if (isQuote(ch) && (inStringOrComment(pos) || isIdentifierChar(prev)))
        return false;
    if (ch == QLatin1Char('\'') && m_language.compare(QLatin1String("rust"), Qt::CaseInsensitive) == 0)
        return false;
    if (inStringOrComment(pos))
        return false;
    c.beginEditBlock();
    c.insertText(QString(ch) + QString(close));
    c.movePosition(QTextCursor::Left);
    c.endEditBlock();
    setTextCursor(c);
    return true;
}

bool CodeEditor::eraseEmptyPair()
{
    QTextCursor c = textCursor();
    if (c.hasSelection() || c.position() == 0)
        return false;
    const QChar prev = document()->characterAt(c.position() - 1);
    const QChar next = document()->characterAt(c.position());
    const bool includeAngles = prev == QLatin1Char('<') && next == QLatin1Char('>') &&
                               CppTools::includeContext(c.block().text().left(c.positionInBlock()));
    if (!includeAngles && (closerFor(prev).isNull() || closerFor(prev) != next))
        return false;
    c.beginEditBlock();
    c.deleteChar();
    c.deletePreviousChar();
    c.endEditBlock();
    setTextCursor(c);
    return true;
}

void CodeEditor::handleBackspaceInIndent(QKeyEvent *event)
{
    QTextCursor c = textCursor();
    const QString unit = indentUnit();
    const int tab = unit.startsWith(QLatin1Char(' ')) ? unit.size() : tabWidth();
    if (!c.hasSelection() && unit.startsWith(QLatin1Char(' ')) && c.positionInBlock() > 0) {
        const QString line = c.block().text();
        const int col = c.positionInBlock();
        if (isBlankBefore(line, col) && line.left(col).indexOf(QLatin1Char('\t')) < 0) {
            int remove = col % tab;
            if (remove == 0)
                remove = tab;
            remove = qMin(remove, col);
            c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, remove);
            c.removeSelectedText();
            setTextCursor(c);
            return;
        }
    }
    QPlainTextEdit::keyPressEvent(event);
}

void CodeEditor::handleKey(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Control)
        updateLink(true);
    if (isReadOnly()) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    if (event->text() == QLatin1String("?") && !textCursor().hasSelection() && textCursor().position() > 0 &&
        document()->characterAt(textCursor().position() - 1) == QLatin1Char('/')) {
        // "/?" opens the inline edit box; the '?' is never inserted and the '/' goes away again.
        QTextCursor c = textCursor();
        c.deletePreviousChar();
        setTextCursor(c);
        hideCompletion();
        emit inlineAskRequested(c.position());
        return;
    }
    if (autoPair(event))
        return;

    if (event->key() == Qt::Key_D && mods == Qt::ControlModifier) {
        duplicateLine();
        return;
    }
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
            insertNewlineWithIndent();
            return;
        }
        break;
    case Qt::Key_Greater:
        if ((mods == Qt::NoModifier || mods == Qt::ShiftModifier) && autoCloseTag())
            return;
        break;
    case Qt::Key_Escape:
        clearSnippet();
        if (hasPredictions()) {
            clearPredictions();
            return;
        }
        break;
    case Qt::Key_Tab:
        if (mods == Qt::NoModifier && snippetJump(1))
            return;
        if (mods == Qt::NoModifier && (hasPredictions() || m_predictSession.dirty) &&
            (!textCursor().hasSelection() || document()->findBlock(textCursor().selectionStart()) ==
                                                 document()->findBlock(textCursor().selectionEnd())) &&
            acceptPrediction())
            return;
        if (mods == Qt::NoModifier && m_emmetMode && expandEmmet())
            return;
        if (mods == Qt::NoModifier) {
            if (textCursor().hasSelection() &&
                textCursor().document()->findBlock(textCursor().selectionStart()) !=
                    textCursor().document()->findBlock(textCursor().selectionEnd())) {
                indentSelection(false);
            } else {
                QTextCursor c = textCursor();
                const QString unit = indentUnit();
                if (unit.startsWith(QLatin1Char(' '))) {
                    // Advance to the next tab stop rather than always inserting N spaces.
                    const int col = c.selectionStart() - c.document()->findBlock(c.selectionStart()).position();
                    c.insertText(QString(unit.size() - (col % unit.size()), QLatin1Char(' ')));
                } else {
                    c.insertText(QStringLiteral("\t"));
                }
                setTextCursor(c);
            }
            return;
        }
        break;
    case Qt::Key_Backtab:
        if (snippetJump(-1))
            return;
        indentSelection(true);
        return;
    case Qt::Key_Backspace:
        if (mods == Qt::NoModifier) {
            if (eraseEmptyPair())
                return;
            handleBackspaceInIndent(event);
            return;
        }
        break;
    case Qt::Key_Slash:
        // `<` then `/` on a whitespace-only line starts a closing tag: it lines up with its opener, one level out.
        if (isMarkupLanguage(m_language) && !textCursor().hasSelection()) {
            QTextCursor c = textCursor();
            const QString line = c.block().text();
            const QString unit = indentUnit();
            if (line.trimmed() == QLatin1String("<") && c.positionInBlock() == line.size() && line.size() > unit.size() + 1 &&
                line.startsWith(unit)) {
                c.movePosition(QTextCursor::StartOfBlock);
                c.setPosition(c.position() + unit.size(), QTextCursor::KeepAnchor);
                c.removeSelectedText();
                c.movePosition(QTextCursor::EndOfBlock);
                setTextCursor(c);
            }
        }
        break;
    case Qt::Key_ParenRight:
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight:
        if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
            // Typing a closing bracket on a whitespace-only line dedents one level.
            QTextCursor c = textCursor();
            const QString line = c.block().text();
            if (!c.hasSelection() && !line.isEmpty() && line.trimmed().isEmpty() && c.positionInBlock() == line.size()) {
                const QString unit = indentUnit();
                if (line.endsWith(unit)) {
                    c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, unit.size());
                    c.removeSelectedText();
                    setTextCursor(c);
                }
            }
        }
        break;
    default:
        break;
    }
    const bool wasOverwrite = overwriteMode();
    QPlainTextEdit::keyPressEvent(event);
    if (wasOverwrite != overwriteMode())
        emit overwriteModeToggled();
}

void CodeEditor::duplicateLine()
{
    if (isReadOnly())
        return;
    QTextCursor c = textCursor();
    c.beginEditBlock();
    if (c.hasSelection()) {
        const int end = c.selectionEnd();
        QString text = c.selectedText();
        text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        c.setPosition(end);
        c.insertText(text);
        c.setPosition(end, QTextCursor::MoveAnchor);
        c.setPosition(end + text.size(), QTextCursor::KeepAnchor);
    } else {
        const int col = c.positionInBlock();
        QTextCursor line(c.block());
        line.movePosition(QTextCursor::EndOfBlock);
        const QString text = c.block().text();
        line.insertText(QStringLiteral("\n") + text);
        c.setPosition(line.position() - text.size() + qMin(col, int(text.size())));
    }
    c.endEditBlock();
    setTextCursor(c);
    ensureCursorVisible();
}
