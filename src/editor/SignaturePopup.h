#pragma once

#include "lsp/LspTypes.h"

#include <QColor>
#include <QFont>
#include <QFrame>

class QLabel;

// The parameter hint under or above the caret while a call is typed: the signature with the active parameter in bold, the
// parameter's documentation and the function's. It never takes focus; CodeEditor feeds it the server's answer after every
// keystroke and forwards Up/Down when the function has several overloads.
class SignaturePopup : public QFrame
{
    Q_OBJECT
public:
    explicit SignaturePopup(QWidget *editor);

    // Shows `help` (keeps the overload the user picked while the same call is edited).
    void setHelp(const LspSignatureHelp &help, const QFont &mono, const QString &language);
    int overloads() const { return m_help.signatures.size(); }
    void cycle(int delta);
    // Placed so the call stays visible: above `above` (global), or below `below` when there is no room.
    void popup(const QPoint &above, const QPoint &below);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void render();
    LspSignatureHelp m_help;
    int m_index = 0;
    QFont m_mono;
    QString m_language;
    QLabel *m_label;
    QColor m_bg, m_border;
};
