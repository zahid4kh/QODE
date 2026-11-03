#pragma once

#include "EditorConfig.h"

#include <QDateTime>
#include <QObject>

class QTextDocument;
class SyntaxHighlighter;
struct LanguageDefinition;

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
    // Over 2 million characters or 50 000 lines when loaded. Such a file gets no language server, git gutter, minimap or
    // edit predictions: each would go over the whole text again after every edit.
    bool isLarge() const { return m_large; }
    static constexpr qsizetype kLargeChars = 2'000'000;
    static constexpr int kLargeLines = 50'000;
    QString text() const;
    QString encoding() const { return m_encoding; }
    QString lineEndingName() const { return m_lineEnding == LineEnding::CRLF ? QStringLiteral("CRLF") : QStringLiteral("LF"); }
    QString languageName() const;
    const LanguageDefinition *language() const; // null = plain text
    State state() const { return m_state; }
    bool isModified() const { return m_state != State::Clean; }
    void setModified(bool modified);

    // True when the file on disk no longer matches what we last read/wrote.
    bool changedOnDisk() const;
    bool existsOnDisk() const;
    void setPath(const QString &path); // after rename/move
    // While set, save() refuses with this reason (an inline edit shows its proposal in the text until it is accepted).
    void setSaveBlock(const QString &reason) { m_saveBlock = reason; }
    QString saveBlock() const { return m_saveBlock; }
    // The .editorconfig properties of the file, read when it is loaded or saved under a new path. end_of_line and charset
    // decide how the file is written; the editor takes the indentation, the save flow trimming and the final newline.
    const EditorConfig::Properties &editorConfig() const { return m_ec; }
    void refreshEditorConfig(); // after a .editorconfig changed

signals:
    void stateChanged(Document::State state);
    void pathChanged(const QString &path);
    void languageChanged(); // also from typing a `#!` line into a file the name gives no language

private:
    void applyLanguage(const QString &firstLine);
    void applyEditorConfigEncoding();
    void recordDiskStamp();
    void setState(State s);
    QByteArray encode();

    QTextDocument *m_doc;
    SyntaxHighlighter *m_highlighter;
    QString m_path;
    QString m_encoding = QStringLiteral("UTF-8");
    bool m_bom = false;
    bool m_large = false;
    LineEnding m_lineEnding = LineEnding::LF;
    State m_state = State::Clean;
    QDateTime m_diskModified;
    qint64 m_diskSize = -1;
    QString m_saveBlock;
    EditorConfig::Properties m_ec;
};
