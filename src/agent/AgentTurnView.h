#pragma once

#include "agent/AgentSession.h"
#include "agent/AgentTools.h"

#include <QWidget>

class CopyButton;
class MarkdownView;
class QLabel;
class QPlainTextEdit;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace agent {

// Digits of equal width in `w`'s font (Qt 6.7+), so a running clock does not shift the text every second.
void useTabularDigits(QWidget *w);

// One assistant turn of the agent tab: the model's thoughts (folded), its text and every tool call in the order they
// happened. Used live (the append* / addTool / finishTool calls while the model streams) and for saved turns (setBlocks).
class AgentTurnView : public QWidget
{
    Q_OBJECT
public:
    // `name` heads the turn ("QODE" when empty).
    explicit AgentTurnView(const QString &model, QWidget *parent = nullptr, const QString &name = QString());

    void setBlocks(const QVector<Block> &blocks);   // a finished turn
    void appendThought(const QString &text);
    void appendText(const QString &text);
    void setModel(const QString &model); // when the model is known only after the turn started
    // The row's index, for finishToolAt. With an `input` (the call in full), the row folds out to show it and, once the
    // tool finished, its output (ToolResult::output).
    int addTool(const QString &name, const QString &call, const QString &input = QString());
    void finishTool(const ToolResult &result);              // the last tool row
    void finishToolAt(int index, const ToolResult &result); // tools that run side by side finish in any order
    void endLive();                                  // the turn is over: show everything, fold the thoughts
    // Time and tokens. Live: the finished requests so far plus the one streaming (`current`, when `streaming`) and the
    // turn's wall time; saved: from the message.
    void setLiveStats(const QVector<RequestStats> &done, const RequestStats &current, bool streaming, qint64 turnMs);
    void setStats(const Message &message);
    // The raw Markdown of the turn's text, without thoughts and tool rows: what its Copy button copies.
    QString replyText() const;
    // The Copy button at the end of the footer shows while the pointer is over the turn; pinned, always (the last turn).
    void setCopyPinned(bool pinned);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void openRequested(const QString &relativePath);
    void linkClicked(const QUrl &url);
    // "Ask about this" / "Quote this" on text selected in the turn.
    void askRequested(const QString &text);
    void quoteRequested(const QString &text);

private:
    struct Item {
        Block::Kind kind;
        QWidget *widget = nullptr;
        MarkdownView *view = nullptr;   // thought / text
        QToolButton *header = nullptr;  // thought
        QLabel *icon = nullptr;         // tool row
        QLabel *label = nullptr;        // tool row
        QLabel *chevron = nullptr;      // tool row with details
        QWidget *details = nullptr;     // tool row: IN / OUT, made when first opened
        QPlainTextEdit *inView = nullptr, *outView = nullptr;
        QString text;
        QString call;
        QString input, output;          // tool row
        bool finished = false, ok = true; // tool row
        bool dirty = false;
        bool open = false;              // thought body / tool details shown
    };

    Item &startItem(Block::Kind kind);
    void foldThoughts();
    void setThoughtOpen(int index, bool open);
    void flush();
    void showToolResult(Item &item, bool ok, const QString &summary);
    void setToolOpen(int index, bool open);
    void fillToolDetails(Item &item);
    MarkdownView *makeView(QWidget *parent, bool muted);
    void applyTheme();
    void ensureStats();
    void showStats(const QString &summary, const QStringList &rows);
    void setStatsOpen(bool open);
    void updateCopy();

    QVBoxLayout *m_layout;
    QLabel *m_who;
    QString m_name;
    QVector<Item> m_items;
    QTimer *m_flush;
    bool m_live = false;
    QWidget *m_stats = nullptr;
    QToolButton *m_statsHeader = nullptr;
    QLabel *m_statsRows = nullptr;
    bool m_statsOpen = false;
    CopyButton *m_copy = nullptr;
    bool m_copyPinned = false;
    int m_zoom = 100; // Ctrl+wheel on one of the turn's views zooms them all
};

} // namespace agent
