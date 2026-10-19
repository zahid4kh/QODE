#pragma once

#include <QPair>
#include <QVector>
#include <QWidget>

class QLabel;
class MarkdownView;
class QToolButton;
class QUrl;

// Right-hand panel that renders Markdown text (live, as the source is edited).
class MarkdownPreview : public QWidget
{
    Q_OBJECT
public:
    explicit MarkdownPreview(QWidget *parent = nullptr);

    // `filePath` anchors relative images and links; it may be empty for an unsaved file.
    void setMarkdown(const QString &filePath, const QString &text);
    void clear();

    // Scroll sync with the editor. `line` is a 0-based source line; the preview is placed by matching headings between the
    // source and the rendered text and interpolating in between.
    void scrollToSourceLine(int line);

signals:
    void closeRequested();
    void openFileRequested(const QString &path); // a link to a file on disk was clicked
    void topLineChanged(int line);               // the user scrolled the preview; source line now at its top

private:
    void followLink(const QUrl &url);
    QVector<QPair<double, double>> anchors() const; // (source line, document y), increasing in both


    QLabel *m_title;
    MarkdownView *m_view;
    QString m_path;
    QVector<int> m_headingLines; // source lines of the headings, in order
    int m_lineCount = 0;
    bool m_applying = false;
};
