#pragma once

#include <QDateTime>
#include <QObject>

class QTextDocument;
class SyntaxHighlighter;

// One open file: the text, its on-disk identity, encoding and modified state.
// Editors (CodeEditor) are views onto the QTextDocument owned here.
class Document : public QObject
{
    Q_OBJECT
public:
    enum class State { Clean, Modified, Saving };
    enum class LineEnding { LF, CRLF };

    explicit Document(QObject *parent = nullptr);
    ~Document() override;

    // Loading. On failure returns false and fills *error.
    bool load(const QString &path, QString *error);
    bool reload(QString *error);
    // Writing. save() uses the current path (must be non-empty).
    bool save(QString *error);
    bool saveAs(const QString &path, QString *error);

    QTextDocument *textDocument() const { return m_doc; }
    QString filePath() const { return m_path; }
    QString fileName() const;
    bool isUntitled() const { return m_path.isEmpty(); }
    QString text() const;
    QString encoding() const { return m_encoding; }
    QString lineEndingName() const { return m_lineEnding == LineEnding::CRLF ? QStringLiteral("CRLF") : QStringLiteral("LF"); }
    QString languageName() const;
    State state() const { return m_state; }
    bool isModified() const { return m_state != State::Clean; }
    void setModified(bool modified);

    // True when the file on disk no longer matches what we last read/wrote.
    bool changedOnDisk() const;
    bool existsOnDisk() const;
    void setPath(const QString &path); // after rename/move

signals:
    void stateChanged(Document::State state);
    void pathChanged(const QString &path);

private:
    void applyLanguage();
    void recordDiskStamp();
    void setState(State s);
    QByteArray encode();

    QTextDocument *m_doc;
    SyntaxHighlighter *m_highlighter;
    QString m_path;
    QString m_encoding = QStringLiteral("UTF-8");
    bool m_bom = false;
    LineEnding m_lineEnding = LineEnding::LF;
    State m_state = State::Clean;
    QDateTime m_diskModified;
    qint64 m_diskSize = -1;
};
