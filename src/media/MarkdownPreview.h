#pragma once

#include <QWidget>

class QLabel;
class QTextBrowser;
class QToolButton;

// Right-hand panel that renders Markdown text (live, as the source is edited).
class MarkdownPreview : public QWidget
{
    Q_OBJECT
public:
    explicit MarkdownPreview(QWidget *parent = nullptr);

    // `filePath` anchors relative images and links; it may be empty for an unsaved file.
    void setMarkdown(const QString &filePath, const QString &text);
    void clear();

signals:
    void closeRequested();

private:
    void applyTheme();
    void polish();

    QLabel *m_title;
    QTextBrowser *m_view;
    QString m_path;
};
