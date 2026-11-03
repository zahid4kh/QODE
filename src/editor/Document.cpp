#include "Document.h"

#include "SyntaxHighlighter.h"
#include "filesystem/FileManager.h"
#include "history/LocalHistory.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "editor/Language.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QPlainTextDocumentLayout>
#include <QStringConverter>
#include <QTextBlock>
#include <QTextDocument>

Document::Document(QObject *parent)
    : QObject(parent)
{
    m_doc = new QTextDocument(this);
    m_doc->setDocumentLayout(new QPlainTextDocumentLayout(m_doc));
    m_doc->setUndoRedoEnabled(true);
    // The editor's font and tab stops from the start: CodeEditor then finds nothing to change when it takes the
    // document. Each change after the text is in lays every line out again (a one-line file at once), which costs
    // seconds on a minified file.
    const QFont font = SettingsManager::instance().editorFont();
    m_doc->setDefaultFont(font);
    QTextOption option = m_doc->defaultTextOption();
    option.setTabStopDistance(QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')) * SettingsManager::instance().tabSize());
    m_doc->setDefaultTextOption(option);
    m_highlighter = new SyntaxHighlighter(m_doc);
    m_highlighter->setTheme(Theme::byName(SettingsManager::instance().theme()));

    connect(m_doc, &QTextDocument::modificationChanged, this, [this](bool modified) {
        if (m_state == State::Saving)
            return;
        setState(modified ? State::Modified : State::Clean);
    });
    // A `#!` typed into the first line of a file without a known extension picks the language.
    connect(m_doc, &QTextDocument::contentsChange, this, [this](int position) {
        if (position < m_doc->firstBlock().length())
            applyLanguage(m_doc->firstBlock().text());
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this](const QString &t) {
        m_highlighter->setTheme(Theme::byName(t));
    });
}

Document::~Document() = default;

QString Document::fileName() const
{
    return m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
}

QString Document::text() const
{
    // toRawText keeps non-breaking spaces that toPlainText() would silently turn into ' '.
    QString t = m_doc->toRawText();
    t.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return t;
}

QString Document::languageName() const
{
    const LanguageDefinition *l = m_highlighter->language();
    return l ? l->name : QStringLiteral("Plain Text");
}

const LanguageDefinition *Document::language() const
{
    return m_highlighter->language();
}

void Document::applyLanguage(const QString &firstLine)
{
    const LanguageDefinition *lang = Languages::forFile(m_path, firstLine);
    if (lang == m_highlighter->language())
        return;
    m_highlighter->setLanguage(lang);
    emit languageChanged();
}

void Document::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void Document::setModified(bool modified)
{
    m_doc->setModified(modified);
    if (m_state != State::Saving)
        setState(modified ? State::Modified : State::Clean);
}

void Document::recordDiskStamp()
{
    const QFileInfo fi(m_path);
    m_diskModified = fi.lastModified();
    m_diskSize = fi.size();
}

bool Document::existsOnDisk() const
{
    return !m_path.isEmpty() && QFileInfo::exists(m_path);
}

bool Document::changedOnDisk() const
{
    if (m_path.isEmpty())
        return false;
    const QFileInfo fi(m_path);
    return fi.lastModified() != m_diskModified || fi.size() != m_diskSize;
}

void Document::setPath(const QString &path)
{
    m_path = path;
    applyLanguage(m_doc->firstBlock().text());
    if (!path.isEmpty())
        recordDiskStamp();
    emit pathChanged(path);
}

bool Document::load(const QString &path, QString *error)
{
    const FileManager::ReadResult r = FileManager::readFile(path);
    if (!r.ok) {
        if (error)
            *error = r.error;
        return false;
    }

    QByteArray data = r.data;
    m_bom = data.startsWith("\xEF\xBB\xBF");
    if (m_bom)
        data.remove(0, 3);
    m_ec = EditorConfig::forFile(path);

    // Try UTF-8 first; if the bytes are not valid UTF-8, fall back to Latin-1 which is
    // lossless byte-for-byte, so saving reproduces the original file exactly.
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString text = m_ec.charset == QLatin1String("latin1") ? QString() : utf8.decode(data);
    if (m_ec.charset == QLatin1String("latin1") || utf8.hasError()) {
        m_encoding = QStringLiteral("Latin-1");
        text = QString::fromLatin1(data);
    } else {
        m_encoding = m_bom ? QStringLiteral("UTF-8 BOM") : QStringLiteral("UTF-8");
    }

    const int crlf = text.count(QStringLiteral("\r\n"));
    const int lf = text.count(QLatin1Char('\n')) - crlf;
    m_lineEnding = (crlf > 0 && crlf >= lf) ? LineEnding::CRLF : LineEnding::LF;
    if (crlf > 0)
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    applyEditorConfigEncoding();
    if (m_ec.tabWidth > 0) { // before the text goes in: changing it afterwards lays every line out again
        QTextOption option = m_doc->defaultTextOption();
        option.setTabStopDistance(QFontMetricsF(m_doc->defaultFont()).horizontalAdvance(QLatin1Char(' ')) * m_ec.tabWidth);
        m_doc->setDefaultTextOption(option);
    }

    m_path = path;
    applyLanguage(text.section(QLatin1Char('\n'), 0, 0));
    m_large = text.size() > kLargeChars || text.count(QLatin1Char('\n')) >= kLargeLines;
    m_doc->setPlainText(text);
    m_doc->setModified(false);
    m_doc->clearUndoRedoStacks();
    recordDiskStamp();
    setState(State::Clean);
    return true;
}

bool Document::reload(QString *error)
{
    if (m_path.isEmpty())
        return true;
    return load(m_path, error);
}

// end_of_line and charset from .editorconfig decide how the file is written back.
void Document::applyEditorConfigEncoding()
{
    if (m_ec.crlf >= 0)
        m_lineEnding = m_ec.crlf ? LineEnding::CRLF : LineEnding::LF;
    if (m_ec.charset == QLatin1String("latin1")) {
        m_encoding = QStringLiteral("Latin-1");
        m_bom = false;
    } else if (m_ec.charset.startsWith(QLatin1String("utf-8"))) {
        m_bom = m_ec.charset == QLatin1String("utf-8-bom");
        m_encoding = m_bom ? QStringLiteral("UTF-8 BOM") : QStringLiteral("UTF-8");
    }
}

void Document::refreshEditorConfig()
{
    if (m_path.isEmpty())
        return;
    m_ec = EditorConfig::forFile(m_path);
    applyEditorConfigEncoding();
}

QByteArray Document::encode()
{
    QString text = this->text();
    if (m_lineEnding == LineEnding::CRLF)
        text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    QByteArray out;
    if (m_encoding == QLatin1String("Latin-1")) {
        out = text.toLatin1();
        // Characters typed after loading that Latin-1 can't represent: upgrade to UTF-8.
        if (QString::fromLatin1(out) != text) {
            out = text.toUtf8();
            m_encoding = QStringLiteral("UTF-8");
        }
    } else {
        out = text.toUtf8();
        if (m_bom)
            out.prepend("\xEF\xBB\xBF");
    }
    return out;
}

bool Document::save(QString *error)
{
    if (m_path.isEmpty()) {
        if (error)
            *error = tr("The document has no file path.");
        return false;
    }
    return saveAs(m_path, error);
}

bool Document::saveAs(const QString &path, QString *error)
{
    if (!m_saveBlock.isEmpty()) {
        if (error)
            *error = m_saveBlock;
        return false;
    }
    if (path != m_path) { // a new name can fall under other .editorconfig sections
        m_ec = EditorConfig::forFile(path);
        applyEditorConfigEncoding();
    }
    const State before = m_state;
    setState(State::Saving);
    const QByteArray data = encode();
    LocalHistory::recordWrite(path, data);
    const bool ok = FileManager::writeFile(path, data, error);
    if (!ok) {
        setState(before);
        return false;
    }
    m_doc->setModified(false);
    const bool pathChangedNow = path != m_path;
    m_path = path;
    if (pathChangedNow) {
        applyLanguage(m_doc->firstBlock().text());
    }
    recordDiskStamp();
    setState(State::Clean);
    if (pathChangedNow)
        emit pathChanged(m_path);
    return true;
}
