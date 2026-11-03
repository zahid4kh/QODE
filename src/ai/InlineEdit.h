#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QJsonArray>
#include <QPointer>

class CodeEditor;
class Document;
class InlineClient;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QTimer;
class QToolButton;

// The inline edit box ("/?" in the text, Ctrl+K, or "Ask about this to make a change" on a selection): a small draggable
// box on the editor where a prompt about the marked lines is typed. It is not a chat: one stateless request with the whole
// file (the marked lines between markers) goes to Gemini or Claude, and follow-ups in the same box refine the answer.
// The reply streams into the document right below the marked lines, which turn red while the proposal is green; Accept
// keeps the proposal (one undo step from the original), Reject takes it out again. A question gets a short answer in the
// box instead. While a request or proposal is pending the editor is read-only and the document refuses to save.
class InlineEdit : public QFrame
{
    Q_OBJECT
public:
    // Opens the box for lines [firstLine, lastLine] (0-based) of `editor`, or focuses the box already open there.
    // `restoreAt` >= 0: "/?" was typed there; closing the box without asking puts the two characters back.
    // `created` tells a new box (to connect to) from the one already open.
    static InlineEdit *open(CodeEditor *editor, Document *doc, const QString &projectRoot, int firstLine, int lastLine, int restoreAt = -1,
                            bool *created = nullptr);

    ~InlineEdit() override;

signals:
    void statusMessage(const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class State { Idle, Running, Proposed, Answered };

    InlineEdit(CodeEditor *editor, Document *doc, const QString &projectRoot, int firstLine, int lastLine, int restoreAt);
    void buildUi();
    void applyTheme();
    void updateModelButton();
    void rebuildModelMenu();
    void setState(State state);
    void setStatus(const QString &text, bool error = false);
    void updateTitle();
    void reposition();
    void fitInput();

    void send();
    QString firstMessage(const QString &instruction) const;
    void onDelta(const QString &text);
    void flush(bool final);
    void onFinished(bool ok, const QString &error, const QString &note);
    void showProposal(const QString &text);
    void updateMarks();
    void documentChanged(int position, int removed, int added);
    void takeSnapshot();
    void accept();
    void reject();
    void escape();
    void close(bool restoreTrigger);
    void releaseEditor();

    QPointer<CodeEditor> m_editor;
    QPointer<Document> m_doc;
    QString m_root;

    // The marked lines are [m_start, m_start + m_origLen) of the document (whole lines, no final newline). A proposal sits
    // right after them as "\n" + text: [m_start + m_origLen, + m_newLen).
    int m_start = 0, m_origLen = 0, m_newLen = 0;
    QString m_shown;      // the proposal text in the document
    QString m_snapshot;   // the document text of the marked lines and the proposal, to tell format-only changes apart
    bool m_hasProposal = false;
    bool m_blockOpen = false; // our first edit opened the undo step that later edits join
    bool m_applying = false;  // our own edits, not tracked by documentChanged()
    int m_restoreAt = -1;     // offset from m_start where "/?" was typed, -1 for none
    bool m_asked = false;
    bool m_wasReadOnly = false;
    bool m_locked = false;    // editor read-only and saving refused by us
    bool m_insertMode = false;

    State m_state = State::Idle;
    InlineClient *m_client;
    QString m_model;
    QJsonArray m_contents; // the box's own short conversation
    QString m_reply;       // raw text of the reply being streamed
    bool m_answerMode = false;
    QString m_lastInstruction;
    QTimer *m_flushTimer;
    QTimer *m_clockTimer;
    QElapsedTimer m_clock;
    QString m_activity;

    QWidget *m_header;
    QLabel *m_title;
    QToolButton *m_modelBtn;
    QToolButton *m_closeBtn;
    QPlainTextEdit *m_input;
    QToolButton *m_sendBtn;
    QTextBrowser *m_answer;
    QWidget *m_statusRow;
    QLabel *m_status;
    QPushButton *m_keyBtn;
    QPushButton *m_stopBtn;
    QPushButton *m_rejectBtn;
    QPushButton *m_acceptBtn;
    bool m_statusError = false;

    QPoint m_dragOffset;
    bool m_dragging = false;
    bool m_dragged = false; // moved by the user: stays where it was put
    QPoint m_dragPos;
};
