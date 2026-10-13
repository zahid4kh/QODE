#pragma once

#include "ai/AiChat.h"

#include <QFrame>
#include <QPlainTextEdit>

class QLabel;
class QToolButton;
class QVariantAnimation;

// The changed lines of one edit as a unified diff: removed lines red, added lines green, a little context around.
class AiDiffView : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit AiDiffView(QWidget *parent = nullptr);
    void setChange(const AiChange &change);
    void applyTheme();

protected:
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void rebuild();
    void fitHeight();

    AiChange m_change;
};

// One row of the "changes" list in a reply: status icon, file, what happened, and a fold-out diff.
class AiChangeCard : public QFrame
{
    Q_OBJECT
public:
    explicit AiChangeCard(const AiChange &change, QWidget *parent = nullptr);

signals:
    void openRequested(const QString &relativePath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyTheme();
    void setExpanded(bool on);
    void keepInView();

    AiChange m_change;
    QLabel *m_icon;
    QLabel *m_path;
    QLabel *m_note;
    QToolButton *m_toggle;
    QWidget *m_header;
    AiDiffView *m_diff = nullptr;
    QWidget *m_clip = nullptr;
    QVariantAnimation *m_anim = nullptr;
    bool m_expanded = false;
};
