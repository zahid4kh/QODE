#pragma once

#include <QFrame>

class QLabel;
class QToolButton;

// Text quoted from a reply ("Quote this" / "Ask about this" in a reply's context menu): a card with the quote icon and an
// accent bar, above the prompt (removable) and in the user's message bubble. The helpers give the wording the model gets.
class AiQuote : public QFrame
{
    Q_OBJECT
public:
    explicit AiQuote(QWidget *parent = nullptr, bool removable = false);

    void setText(const QString &text);
    const QString &text() const { return m_text; }

    // The quote as the model reads it, sent ahead of the prompt; `lead` says where it is from (default: the model's
    // earlier reply).
    static QString block(const QString &quote, const QString &lead = QString());
    // The quote of a block() ("" when `text` is not one).
    static QString ofBlock(const QString &text);
    // The prompt of "Ask about this".
    static QString askPrompt();

signals:
    void removed();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    void applyTheme();

    QString m_text;
    QLabel *m_icon;
    QLabel *m_label;
    QToolButton *m_remove = nullptr;
};
