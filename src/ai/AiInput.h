#pragma once

#include <QFrame>
#include <QPlainTextEdit>

class QListWidget;

// The prompt box of the AI chat: grows with its text, Enter sends (Shift+Enter breaks the line) and typing `@` starts a
// file mention whose query is reported to the panel.
class AiInput : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit AiInput(QWidget *parent = nullptr);

    void setMentionOpen(bool open) { m_mentionOpen = open; }
    // Replaces the `@query` in front of the caret by `@path ` .
    void completeMention(const QString &path);
    QRect mentionAnchor() const; // global rectangle of the caret

signals:
    void sendRequested();
    void mentionQuery(const QString &query); // text after the `@` in front of the caret
    void mentionClosed();
    void mentionKey(int key);                // Up / Down / Tab / Enter / Escape while the list is open

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    void detectMention();
    void fitHeight();

    bool m_mentionOpen = false;
};

// The list that pops up above the prompt while a mention is typed. It never takes the focus.
class AiMentionPopup : public QFrame
{
    Q_OBJECT
public:
    explicit AiMentionPopup(QWidget *parent = nullptr);

    void setItems(const QStringList &paths);
    void step(int delta);
    QString current() const;
    int count() const;
    void showAbove(QWidget *anchor);

signals:
    void chosen(const QString &path);

private:
    void applyTheme();

    QListWidget *m_list;
};
