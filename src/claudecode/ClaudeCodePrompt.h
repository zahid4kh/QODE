#pragma once

#include <QImage>
#include <QLayout>
#include <QJsonValue>
#include <QVector>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace claudecode {

// A pasted or dropped image, encoded as it goes to the CLI.
struct ImageAttachment {
    QByteArray data;   // PNG or JPEG bytes
    QString mediaType; // image/png | image/jpeg
    QImage thumb;      // small copy for the chips and the bubble
    QSize size;

    static ImageAttachment fromImage(const QImage &image); // scaled down to at most 1568 px on the long side
    static ImageAttachment fromBase64(const QString &mediaType, const QByteArray &base64);
};

// Lines selected in the editor, sent along with a message.
struct Selection {
    QString path; // absolute
    int first = 0, last = 0; // 1-based, inclusive
    QString text;

    bool isEmpty() const { return path.isEmpty(); }
    QString key() const;                   // identifies this selection, to remember that it was dismissed or sent
    QString label() const;                 // "CodeEditor.cpp:120–148"
    QString block() const;                 // the <ide_selection> text block
    static QString labelOf(const QString &block); // the label of a block from a saved conversation ("" when not one)
};

// What one message to Claude Code holds.
struct Prompt {
    QString text;
    QVector<ImageAttachment> images;
    Selection selection;
    QString file; // project-relative: an attached file, sent as an `@path` block that the CLI reads itself
    QString quote; // text quoted from a reply, sent as an AiQuote::block ahead of the text

    // The message content: the plain text, or content blocks (images, the quote, the text, the file, the selection) when
    // there are attachments.
    QJsonValue content() const;
    QString summary() const; // one line, for the queue
};

// Lays widgets out left to right and starts a new row when the next one does not fit, so a row of chips or thumbnails
// never makes the panel wider than its narrowest item.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int spacing = 6);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;

private:
    int arrange(const QRect &rect, bool apply) const; // the height the items take in rect's width

    QList<QLayoutItem *> m_items;
};

// The chips at the top of the input box: the file open in the editor (dimmed until clicked, which attaches it), the
// editor selection that goes with the next message, and pasted images.
class AttachmentBar : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kMaxImages = 8;

    explicit AttachmentBar(QWidget *parent = nullptr);

    // The file of the active editor tab (absolute; "" = none). It replaces a dimmed chip; an attached one stays.
    void setCurrentFile(const QString &path);
    QString attachedFile() const { return m_fileAttached ? m_file : QString(); }
    void detachFile(); // sent or left out: back to a dimmed chip of the current file
    void setSelection(const Selection &selection); // empty: no chip
    Selection selection() const { return m_selection; }
    bool addImage(const ImageAttachment &image); // false when the limit is reached
    QVector<ImageAttachment> images() const { return m_images; }
    void clearImages();
    bool hasImages() const { return !m_images.isEmpty(); }

signals:
    void selectionDismissed();
    void openRequested(const QString &absolute, int line);
    void changed();

private:
    void rebuild();

    FlowLayout *m_layout;
    QString m_current;          // the active editor's file
    QString m_file;             // the file chip's file: the current one, or the attached one
    bool m_fileAttached = false;
    Selection m_selection;
    QVector<ImageAttachment> m_images;
};

// Messages written while Claude Code works. They go out one by one when a turn ends well; "Send now" stops the turn
// and sends that one next.
class QueueList : public QWidget
{
    Q_OBJECT
public:
    explicit QueueList(QWidget *parent = nullptr);

    void setItems(const QStringList &summaries, bool held); // held: the last turn was stopped or failed

signals:
    void sendNowRequested(int index);
    void removeRequested(int index);

private:
    QVBoxLayout *m_layout;
};

// Thumbnail label for an image: keeps its aspect at `height` px (zoomed with the rest of the window).
QLabel *thumbnailLabel(const ImageAttachment &image, int height, QWidget *parent);

} // namespace claudecode
