#include "Document.h"

#include "SyntaxHighlighter.h"
#include "filesystem/FileManager.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "editor/Language.h"

#include <QFileInfo>
#include <QPlainTextDocumentLayout>
#include <QStringConverter>
#include <QTextDocument>

Document::Document(QObject *parent)
    : QObject(parent)
{
    m_doc = new QTextDocument(this);
    m_doc->setDocumentLayout(new QPlainTextDocumentLayout(m_doc));
    m_doc->setUndoRedoEnabled(true);
    m_highlighter = new SyntaxHighlighter(m_doc);
    m_highlighter->setTheme(Theme::byName(SettingsManager::instance().theme()));

    connect(m_doc, &QTextDocument::modificationChanged, this, [this](bool modified) {
        if (m_state == State::Saving)
            return;
        setState(modified ? State::Modified : State::Clean);
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

void Document::applyLanguage()
{
    m_highlighter->setLanguage(Languages::forFile(m_path));
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
    applyLanguage();
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

    // Try UTF-8 first; if the bytes are not valid UTF-8, fall back to Latin-1 which is
    // lossless byte-for-byte, so saving reproduces the original file exactly.
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString text = utf8.decode(data);
    if (utf8.hasError()) {
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

    m_path = path;
    applyLanguage();
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
    const State before = m_state;
    setState(State::Saving);
    const bool ok = FileManager::writeFile(path, encode(), error);
    if (!ok) {
        setState(before);
        return false;
    }
    m_doc->setModified(false);
    const bool pathChangedNow = path != m_path;
    m_path = path;
    if (pathChangedNow) {
        applyLanguage();
    }
    recordDiskStamp();
    setState(State::Clean);
    if (pathChangedNow)
        emit pathChanged(m_path);
    return true;
}
