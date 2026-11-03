#pragma once

#include <QToolButton>

namespace claudecode {

// A small ring after the model and mode pickers that fills as the context window fills (accent below 70 %, warning
// up to 90 %, then the error colour). Clicking it compacts the conversation. See dev-docs/claude-code.md.
class ContextMeter : public QToolButton
{
    Q_OBJECT
public:
    explicit ContextMeter(QWidget *parent = nullptr);

    // used < 0: not known (a new conversation, or just compacted: `compacted` says so in the tooltip).
    void setUsage(int used, int window, bool compacted = false);
    int used() const { return m_used; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateToolTip();

    int m_used = -1;
    int m_window = 0;
    bool m_compacted = false;
};

} // namespace claudecode
