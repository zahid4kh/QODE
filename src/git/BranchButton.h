#pragma once

#include "settings/Theme.h"

#include <QAbstractButton>

// Flat "<branch icon> name ⌄" button used in the Version Control toolbar and the status bar. It is
// painted by hand so that icon, text and chevron share one vertical centre line.
class BranchButton : public QAbstractButton
{
    Q_OBJECT
public:
    explicit BranchButton(bool compact, QWidget *parent = nullptr);

    void setLabel(const QString &text);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    bool m_compact;
    QString m_label;
    Theme m_theme;
};
