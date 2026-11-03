#pragma once

#include "http/HttpFile.h"
#include "settings/Theme.h"

#include <QStyledItemDelegate>
#include <QSyntaxHighlighter>

// Pieces shared by the .http panel and the Spring Boot Requests tab.
namespace HttpUi {

// Item data of the request lists (HttpRowDelegate).
enum Role { MethodRole = Qt::UserRole + 1, DetailRole, StatusRole, PayloadRole, EndpointRole, HeaderRole };

QString sizeText(qint64 bytes);
QColor methodColour(const Theme &t, const QString &method);
QColor statusColour(const Theme &t, int status); // 2xx green, 3xx amber, 4xx/5xx red, 0 muted
QString curl(const HttpRequestData &r);           // a shell command doing the same request
// The stylesheet of the flat text tabs (QToolButton with property seg=true) and the editors of a client view.
QString styleSheet(const Theme &t);

} // namespace HttpUi

// Colours JSON (also fine for plain text: nothing matches).
class HttpJsonHighlighter : public QSyntaxHighlighter
{
public:
    explicit HttpJsonHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {}
    void setTheme(const Theme &t);
    void setEnabled(bool on); // off for large or non-JSON text

protected:
    void highlightBlock(const QString &text) override;

private:
    QColor m_key, m_string, m_number, m_literal;
    bool m_enabled = true;
};

// Rows of the request lists: coloured method, text, muted detail (or a status-coloured one) on the right; HeaderRole
// rows are small capital section titles.
class HttpRowDelegate : public QStyledItemDelegate
{
public:
    HttpRowDelegate(const Theme *theme, QObject *parent) : QStyledItemDelegate(parent), m_theme(theme) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;

private:
    const Theme *m_theme;
};
