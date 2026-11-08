// Ctrl+click links: the language server's document links, URLs and file paths in the text.
#include "CodeEditor.h"
#include "platform/Platform.h"

#include <QDesktopServices>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QUrl>

namespace {

bool isPathChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.') || c == QLatin1Char('/') ||
           c == QLatin1Char('~') || c == QLatin1Char('@') || c == QLatin1Char('+') || c == QLatin1Char('%');
}

// "#L12", "#L12,3", "#12", "#L12-L20" at the end of a file:// link: 0-based line / column.
void lineFromFragment(const QString &fragment, int *line, int *column)
{
    static const QRegularExpression re(QStringLiteral("^L?(\\d+)(?:[,:C](\\d+))?"));
    const QRegularExpressionMatch m = re.match(fragment);
    if (!m.hasMatch())
        return;
    *line = qMax(0, m.captured(1).toInt() - 1);
    if (!m.captured(2).isEmpty())
        *column = qMax(0, m.captured(2).toInt() - 1);
}

} // namespace

void CodeEditor::setDocumentLinks(const QVector<DocumentLink> &links)
{
    m_docLinks.clear();
    QTextDocument *doc = document();
    auto position = [doc](int line, int column) {
        const QTextBlock b = doc->findBlockByNumber(line);
        return b.isValid() ? b.position() + qBound(0, column, b.length() - 1) : -1;
    };
    for (const DocumentLink &l : links) {
        const int a = position(l.startLine, l.startColumn), b = position(l.endLine, l.endColumn);
        if (a < 0 || b <= a || l.target.isEmpty())
            continue;
        LinkMark m{QTextCursor(doc), QTextCursor(doc), l.target};
        m.from.setPosition(a);
        m.to.setPosition(b);
        m_docLinks.append(m);
    }
}

CodeEditor::LinkHit CodeEditor::linkAt(int position) const
{
    LinkHit hit;
    // 1. What the language server calls a link (an #include, an import path, a $schema ...).
    for (const LinkMark &m : m_docLinks)
        if (position >= m.from.position() && position < m.to.position()) {
            hit.start = m.from.position();
            hit.end = m.to.position();
            const QUrl url(m.target);
            if (url.isLocalFile()) {
                hit.path = Platform::uriToPath(m.target); // the server's drive letter may be c: or c%3A
                lineFromFragment(url.fragment(), &hit.line, &hit.column);
            } else {
                hit.url = m.target;
            }
            return hit;
        }

    const QTextBlock block = document()->findBlock(position);
    if (!block.isValid())
        return hit;
    const QString text = block.text();
    const int col = position - block.position();
    if (col >= text.size())
        return hit;

    // 2. A web address.
    static const QRegularExpression urlRe(QStringLiteral("\\b(?:https?|ftp|file)://[^\\s\"'<>`\\\\]+"));
    for (auto it = urlRe.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        int end = int(m.capturedEnd());
        // Punctuation that ends a sentence, and closers the link did not open, are not part of it.
        while (end > m.capturedStart()) {
            const QChar last = text.at(end - 1);
            const QStringView body = QStringView(text).mid(m.capturedStart(), end - m.capturedStart());
            if (QStringLiteral(".,;:!?*").contains(last) ||
                (last == QLatin1Char(')') && body.count(QLatin1Char('(')) < body.count(QLatin1Char(')'))) ||
                (last == QLatin1Char(']') && body.count(QLatin1Char('[')) < body.count(QLatin1Char(']'))) ||
                (last == QLatin1Char('}') && body.count(QLatin1Char('{')) < body.count(QLatin1Char('}'))))
                --end;
            else
                break;
        }
        if (col < m.capturedStart() || col >= end)
            continue;
        hit.start = block.position() + int(m.capturedStart());
        hit.end = block.position() + end;
        const QString url = text.mid(m.capturedStart(), end - m.capturedStart());
        if (url.startsWith(QLatin1String("file://"))) {
            const QUrl u(url);
            hit.path = Platform::uriToPath(url);
            lineFromFragment(u.fragment(), &hit.line, &hit.column);
        } else {
            hit.url = url;
        }
        return hit;
    }

    // 3. A path to a file that exists, with an optional :line[:column] (compiler output, stack traces, docs).
    if (!m_pathResolver || !isPathChar(text.at(col)))
        return hit;
    int a = col, b = col;
    while (a > 0 && isPathChar(text.at(a - 1)))
        --a;
    while (b < text.size() && isPathChar(text.at(b)))
        ++b;
    while (b > a && (text.at(b - 1) == QLatin1Char('.') || text.at(b - 1) == QLatin1Char('/')))
        --b; // "see foo/bar.txt." ends the sentence
    const QString raw = text.mid(a, b - a);
    if (raw.size() < 3 || col >= b || !(raw.contains(QLatin1Char('/')) || raw.contains(QLatin1Char('.'))) || raw.startsWith(QLatin1String("//")))
        return hit;
    if (m_resolved.first != raw)
        m_resolved = {raw, m_pathResolver(raw)};
    if (m_resolved.second.isEmpty())
        return hit;
    hit.path = m_resolved.second;
    static const QRegularExpression lineRe(QStringLiteral("^:(\\d+)(?::(\\d+))?"));
    const QRegularExpressionMatch m = lineRe.match(text.mid(b)); // not matchView: that is Qt 6.5+, and build-deb.sh builds with 6.4
    if (m.hasMatch()) {
        hit.line = qMax(0, m.captured(1).toInt() - 1);
        if (!m.captured(2).isEmpty())
            hit.column = qMax(0, m.captured(2).toInt() - 1);
        b += int(m.capturedLength());
    }
    hit.start = block.position() + a;
    hit.end = block.position() + b;
    return hit;
}

void CodeEditor::openLink(const LinkHit &hit)
{
    if (!hit.url.isEmpty())
        QDesktopServices::openUrl(QUrl(hit.url));
    else if (!hit.path.isEmpty())
        emit fileLinkRequested(hit.path, hit.line, hit.column);
}
