#pragma once

#include <QToolButton>
#include <functional>

class QTimer;

// A small Copy button: copies what its source returns, then shows a tick and "Copied" for 1.5 s. It can be shown only
// while the pointer is over a host widget (code boxes, chat turns and bubbles), keeping its place in the layout.
class CopyButton : public QToolButton
{
    Q_OBJECT
public:
    explicit CopyButton(QWidget *parent = nullptr, bool withText = true);

    void setSource(std::function<QString()> source) { m_source = std::move(source); }
    // Visible while the pointer is over `host`, or always while pinned.
    void revealOnHover(QWidget *host);
    void setPinned(bool pinned);
    // Nothing to copy: never shown.
    void setAvailable(bool available);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void copy();
    void updateLook();
    void updateVisibility();

    std::function<QString()> m_source;
    QTimer *m_doneTimer;
    QWidget *m_host = nullptr;
    bool m_withText;
    bool m_pinned = false;
    bool m_available = true;
};
