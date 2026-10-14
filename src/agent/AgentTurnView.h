#pragma once

#include "agent/AgentSession.h"
#include "agent/AgentTools.h"

#include <QWidget>

class MarkdownView;
class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace agent {

// One assistant turn of the agent tab: the model's thoughts (folded), its text and every tool call in the order they
// happened. Used live (the append* / addTool / finishTool calls while the model streams) and for saved turns (setBlocks).
class AgentTurnView : public QWidget
{
    Q_OBJECT
public:
    explicit AgentTurnView(const QString &model, QWidget *parent = nullptr);

    void setBlocks(const QVector<Block> &blocks);   // a finished turn
    void appendThought(const QString &text);
    void appendText(const QString &text);
    void addTool(const QString &name, const QString &call);
    void finishTool(const ToolResult &result);
    void endLive();                                  // the turn is over: show everything, fold the thoughts

signals:
    void openRequested(const QString &relativePath);
    void linkClicked(const QUrl &url);

private:
    struct Item {
        Block::Kind kind;
        QWidget *widget = nullptr;
        MarkdownView *view = nullptr;   // thought / text
        QToolButton *header = nullptr;  // thought
        QLabel *icon = nullptr;         // tool row
        QLabel *label = nullptr;        // tool row
        QString text;
        QString call;
        bool dirty = false;
        bool open = false;              // thought body shown
    };

    Item &startItem(Block::Kind kind);
    void foldThoughts();
    void setThoughtOpen(int index, bool open);
    void flush();
    void showToolResult(Item &item, bool ok, const QString &summary);
    MarkdownView *makeView(QWidget *parent, bool muted);
    void applyTheme();

    QVBoxLayout *m_layout;
    QVector<Item> m_items;
    QTimer *m_flush;
    bool m_live = false;
};

} // namespace agent
