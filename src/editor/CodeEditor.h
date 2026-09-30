#pragma once

#include <QElapsedTimer>
#include <QPlainTextEdit>

class QTimer;

class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);

    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

    int currentLine() const { return textCursor().blockNumber() + 1; }
    int currentColumn() const { return textCursor().positionInBlock() + 1; }

    // --- Search -------------------------------------------------------------
    void setSearchTerm(const QString &term, bool caseSensitive);
    int matchCount() const { return m_matches.size(); }
    int currentMatchIndex() const; // 1-based, 0 when the selection is not a match
    bool findNext(bool backwards = false);
    bool replaceCurrent(const QString &replacement);
    int replaceAll(const QString &replacement);

    void setIndentAfterColon(bool on) { m_indentAfterColon = on; } // Python-style blocks
    void applySettings(); // font, tab size, wrapping from SettingsManager
    void applyTheme();

signals:
    void filesDropped(const QStringList &paths);
    void searchResultsChanged();
    void overwriteModeToggled();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

private:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect &rect, int dy);
    void refreshSelections();
    void recomputeMatches();
    QString indentUnit() const;
    void insertNewlineWithIndent();
    void indentSelection(bool outdent);
    void handleBackspaceInIndent(QKeyEvent *event);
    bool selectionIsMatch() const;

    QWidget *m_lineArea;
    QString m_term;
    bool m_caseSensitive = false;
    QVector<QPair<int, int>> m_matches; // (start, end)
    QTimer *m_matchTimer;
    QElapsedTimer m_tripleClickTimer;
    bool m_tripleClickArmed = false;
    bool m_indentAfterColon = false;

    QColor m_gutterBg, m_gutterFg, m_gutterActive, m_currentLine, m_matchBg, m_border;
};
