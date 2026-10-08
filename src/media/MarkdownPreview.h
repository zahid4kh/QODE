#pragma once

#include <QWidget>

class QLabel;
class QTextBrowser;
class QToolButton;
class QUrl;
class QTimer;

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
    void openFileRequested(const QString &path); // a link to a file on disk was clicked

private:
    void applyTheme();
    void polish();
    void followLink(const QUrl &url);
    void fitImages();
    bool eventFilter(QObject *obj, QEvent *e) override;

    QLabel *m_title;
    QTextBrowser *m_view;
    QString m_path;
    QString m_text;
    QTimer *m_resizeTimer;
};
