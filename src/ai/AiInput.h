#pragma once

#include <QFrame>
#include <QImage>
#include <QPlainTextEdit>

class QListWidget;

// The prompt box of the AI chat: grows with its text, Enter sends (Shift+Enter breaks the line), typing `@` starts a
// file mention whose query is reported to the panel, and Up / Down in an empty input recall earlier prompts.
class AiInput : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit AiInput(QWidget *parent = nullptr);

    void setMentionOpen(bool open) { m_mentionOpen = open; }
    // Replaces the `@query` in front of the caret by `@path ` .
    void completeMention(const QString &path);
    QRect mentionAnchor() const; // global rectangle of the caret
    // Pasted or dropped images (and image files) are reported by imagesPasted instead of being ignored.
    void setAcceptsImages(bool on) { m_acceptsImages = on; }
    // Prompt recall: shows an earlier prompt as ordinary editable text ("" empties the input). The input stays in recall
    // mode, where Up / Down ask for the neighbouring prompts, until the text is changed.
    void setRecalled(const QString &text);
    bool recalling() const { return !m_recalled.isEmpty() && toPlainText() == m_recalled; }

signals:
    void sendRequested();
    void mentionQuery(const QString &query); // text after the `@` in front of the caret
    void mentionClosed();
    void mentionKey(int key);                // Up / Down / Tab / Enter / Escape while the list is open
    void imagesPasted(const QList<QImage> &images);
    // Up (-1) in an empty input or on the first line of a recalled prompt, Down (+1) on its last line.
    void historyRequested(int delta);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

private:
    void detectMention();
    void fitHeight();

    QList<QImage> imagesIn(const QMimeData *source, bool load = true) const; // load = false: placeholders, to test cheaply

    bool m_mentionOpen = false;
    bool m_acceptsImages = false;
    QString m_recalled;
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
