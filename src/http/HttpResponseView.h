#pragma once

#include "http/HttpFile.h"
#include "settings/Theme.h"

#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class HttpJsonHighlighter;

// The answer to a request: status line (code, time, size), then Body / Headers / Request tabs. JSON is formatted and
// coloured, images are shown. Copy, save the body to a file, open the text in an editor tab, wrap long lines.
class HttpResponseView : public QWidget
{
    Q_OBJECT
public:
    explicit HttpResponseView(QWidget *parent = nullptr);

    void showResult(const HttpResult &r);
    // A status line without a response; `html` is rich text (escape what goes in). Links come out as linkActivated().
    void showMessage(const QString &html, const QColor &colour);
    void showBusy(const QString &text);
    void clear();
    bool hasResult() const { return m_hasResult; }
    const HttpResult &result() const { return m_result; }
    void setPlaceholder(const QString &text);
    // Puts a button of the owner on the tab row, left of the view's own ones.
    void addTool(QToolButton *button);

signals:
    void linkActivated(const QString &link);
    void openTextRequested(const QString &text); // "Open in editor"

private:
    void applyTheme();
    void showTab(int tab);
    QString bodyText() const;
    void saveBody();

    Theme m_theme;
    HttpResult m_result;
    bool m_hasResult = false;
    int m_tab = 0;
    QLabel *m_status;
    QButtonGroup *m_tabs;
    QHBoxLayout *m_toolRow;
    int m_toolIndex = 0;
    QToolButton *m_wrap, *m_copy, *m_save, *m_open;
    QStackedWidget *m_stack;
    QPlainTextEdit *m_text;
    QScrollArea *m_imageScroll;
    QLabel *m_image;
    HttpJsonHighlighter *m_colours;
};
