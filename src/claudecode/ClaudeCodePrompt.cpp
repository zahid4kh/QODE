#include "ClaudeCodePrompt.h"

#include "ai/AiQuote.h"
#include "claudecode/ClaudeCodeHistory.h"
#include "explorer/FileIcons.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QBuffer>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolButton>
#include <QVBoxLayout>

namespace claudecode {

namespace {
constexpr int kMaxSide = 1568;          // larger images are scaled down by the API anyway
constexpr int kMaxBytes = 3'500'000;    // the API takes up to 5 MB of base64 per image
constexpr int kThumbHeight = 192;

QToolButton *removeButton(QWidget *parent, const QString &tip)
{
    auto *b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("ccChipX"));
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setIcon(Icons::tinted(QStringLiteral(":/new-icons/x.svg"), Theme::byName(SettingsManager::instance().theme()).textMuted));
    b->setIconSize(QSize(10, 10));
    b->setFixedSize(16, 16);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QString clip(QString s, int max)
{
    s = s.simplified();
    return s.size() > max ? s.left(max) + QStringLiteral("…") : s;
}
} // namespace

// --- data -------------------------------------------------------------------------------------------------------------

ImageAttachment ImageAttachment::fromImage(const QImage &image)
{
    ImageAttachment a;
    QImage img = image;
    if (img.width() > kMaxSide || img.height() > kMaxSide)
        img = img.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QBuffer buf(&a.data);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    a.mediaType = QStringLiteral("image/png");
    if (a.data.size() > kMaxBytes) { // a photo: JPEG is far smaller (transparent parts become white)
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter(&flat).drawImage(0, 0, img);
        buf.close();
        buf.open(QIODevice::WriteOnly | QIODevice::Truncate);
        flat.save(&buf, "JPEG", 85);
        a.mediaType = QStringLiteral("image/jpeg");
    }
    a.size = img.size();
    a.thumb = img.height() > kThumbHeight ? img.scaledToHeight(kThumbHeight, Qt::SmoothTransformation) : img;
    return a;
}

ImageAttachment ImageAttachment::fromBase64(const QString &mediaType, const QByteArray &base64)
{
    ImageAttachment a;
    a.data = QByteArray::fromBase64(base64);
    a.mediaType = mediaType;
    const QImage img = QImage::fromData(a.data);
    a.size = img.size();
    a.thumb = img.height() > kThumbHeight ? img.scaledToHeight(kThumbHeight, Qt::SmoothTransformation) : img;
    return a;
}

QString Selection::key() const
{
    return isEmpty() ? QString() : QStringLiteral("%1:%2:%3:%4").arg(path).arg(first).arg(last).arg(qHash(text));
}

QString Selection::label() const
{
    const QString name = QFileInfo(path).fileName();
    return first == last ? QStringLiteral("%1:%2").arg(name).arg(first) : QStringLiteral("%1:%2–%3").arg(name).arg(first).arg(last);
}

// The wording Claude Code's own IDE extensions use for a selection.
QString Selection::block() const
{
    return QStringLiteral("<ide_selection>The user selected the lines %1 to %2 from %3:\n%4\n\nThis may or may not be related to the current task.</ide_selection>")
        .arg(first)
        .arg(last)
        .arg(path, text);
}

QString Selection::labelOf(const QString &block)
{
    static const QRegularExpression re(QStringLiteral("^<ide_selection>The user selected the lines (\\d+) to (\\d+) from ([^\\n]+):\\n"));
    const QRegularExpressionMatch m = re.match(block);
    if (!m.hasMatch())
        return {};
    Selection s;
    s.path = m.captured(3);
    s.first = m.captured(1).toInt();
    s.last = m.captured(2).toInt();
    return s.label();
}

QJsonValue Prompt::content() const
{
    if (images.isEmpty() && selection.isEmpty() && file.isEmpty() && quote.isEmpty())
        return text;
    QJsonArray blocks;
    for (const ImageAttachment &img : images)
        blocks.append(QJsonObject{{"type", "image"},
                                  {"source", QJsonObject{{"type", "base64"}, {"media_type", img.mediaType}, {"data", QString::fromLatin1(img.data.toBase64())}}}});
    if (!quote.isEmpty()) // starts with '<', so the history reads it as context, not as what was typed
        blocks.append(QJsonObject{{"type", "text"}, {"text", AiQuote::block(quote)}});
    if (!text.isEmpty())
        blocks.append(QJsonObject{{"type", "text"}, {"text", text}});
    if (!file.isEmpty()) // the CLI expands a mention in a text block as it does in typed text (checked with 2.1.304)
        blocks.append(QJsonObject{{"type", "text"}, {"text", history::fileBlock(file)}});
    if (!selection.isEmpty())
        blocks.append(QJsonObject{{"type", "text"}, {"text", selection.block()}});
    return blocks;
}

QString Prompt::summary() const
{
    QStringList parts;
    if (!quote.isEmpty())
        parts << QStringLiteral("“%1”").arg(clip(quote, 40));
    if (!text.isEmpty())
        parts << clip(text, 140);
    if (!images.isEmpty())
        parts << (images.size() == 1 ? QObject::tr("1 image") : QObject::tr("%1 images").arg(images.size()));
    if (!file.isEmpty())
        parts << QFileInfo(file).fileName();
    if (!selection.isEmpty())
        parts << selection.label();
    return parts.join(QStringLiteral("  ·  "));
}

QLabel *thumbnailLabel(const ImageAttachment &image, int height, QWidget *parent)
{
    auto *l = new QLabel(parent);
    l->setObjectName(QStringLiteral("ccThumb"));
    const QSize s = image.thumb.isNull() ? QSize(height, height) : image.thumb.size();
    l->setFixedSize(qMax(height / 2, qMin(height * 3, s.width() * height / qMax(1, s.height()))), height);
    l->setScaledContents(true);
    l->setPixmap(QPixmap::fromImage(image.thumb));
    l->setToolTip(QObject::tr("%1 × %2 image").arg(image.size.width()).arg(image.size.height()));
    return l;
}

// --- FlowLayout ---------------------------------------------------------------------------------------------------------

FlowLayout::FlowLayout(QWidget *parent, int spacing)
    : QLayout(parent)
{
    setSpacing(spacing);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_items.append(item);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

QSize FlowLayout::minimumSize() const
{
    QSize s;
    for (const QLayoutItem *item : m_items)
        s = s.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const
{
    return minimumSize();
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect &rect, bool apply) const
{
    const QRect r = rect.marginsRemoved(contentsMargins());
    int x = r.x(), y = r.y(), rowHeight = 0;
    for (QLayoutItem *item : m_items) {
        if (item->isEmpty())
            continue;
        const QSize s = item->sizeHint();
        if (x > r.x() && x + s.width() > r.right() + 1) {
            x = r.x();
            y += rowHeight + spacing();
            rowHeight = 0;
        }
        if (apply)
            item->setGeometry(QRect(QPoint(x, y), s));
        x += s.width() + spacing();
        rowHeight = qMax(rowHeight, s.height());
    }
    return y + rowHeight - rect.y() + contentsMargins().bottom();
}

// --- AttachmentBar ------------------------------------------------------------------------------------------------------

AttachmentBar::AttachmentBar(QWidget *parent)
    : QWidget(parent)
    , m_layout(new FlowLayout(this))
{
    m_layout->setContentsMargins(0, 2, 0, 2);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AttachmentBar::rebuild);
    rebuild();
}

void AttachmentBar::setCurrentFile(const QString &path)
{
    if (path == m_current)
        return;
    m_current = path;
    if (!m_fileAttached) {
        m_file = path;
        rebuild();
    }
}

void AttachmentBar::detachFile()
{
    if (!m_fileAttached && m_file == m_current)
        return;
    m_fileAttached = false;
    m_file = m_current;
    rebuild();
}

void AttachmentBar::setSelection(const Selection &selection)
{
    if (selection.key() == m_selection.key())
        return;
    m_selection = selection;
    rebuild();
}

bool AttachmentBar::addImage(const ImageAttachment &image)
{
    if (m_images.size() >= kMaxImages || image.data.isEmpty())
        return false;
    m_images.append(image);
    rebuild();
    return true;
}

void AttachmentBar::clearImages()
{
    if (m_images.isEmpty())
        return;
    m_images.clear();
    rebuild();
}

void AttachmentBar::rebuild()
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide(); // gone now, not on the next pass of the event loop
            w->deleteLater();
        }
        delete item;
    }
    // The file chip; a selection in the same file is more specific, so a dimmed chip gives way to it.
    const bool showFile = !m_file.isEmpty() && (m_fileAttached || m_selection.path != m_file);
    if (showFile) {
        auto *chip = new QFrame(this);
        chip->setObjectName(QStringLiteral("ccChip"));
        auto *l = new QHBoxLayout(chip);
        l->setContentsMargins(4, 1, 2, 1);
        l->setSpacing(2);
        const QString name = QFileInfo(m_file).fileName();
        auto *text = new QPushButton(FileIcons::forFile(name), name, chip);
        text->setObjectName(QStringLiteral("ccChipText"));
        text->setIconSize(QSize(14, 14));
        text->setCursor(Qt::PointingHandCursor);
        l->addWidget(text);
        if (m_fileAttached) {
            text->setToolTip(tr("Goes with your message: %1").arg(m_file));
            connect(text, &QPushButton::clicked, this, [this] { emit openRequested(m_file, 0); });
            QToolButton *x = removeButton(chip, tr("Leave the file out"));
            connect(x, &QToolButton::clicked, this, &AttachmentBar::detachFile);
            l->addWidget(x);
        } else {
            // Dimmed: the chip colours at lower opacity. Not sent until clicked.
            text->setToolTip(tr("Open in the editor. Click to send it with your next message: %1").arg(m_file));
            connect(text, &QPushButton::clicked, this, [this] {
                m_fileAttached = true;
                rebuild();
            });
            auto *dim = new QGraphicsOpacityEffect(chip);
            dim->setOpacity(0.5);
            chip->setGraphicsEffect(dim);
        }
        m_layout->addWidget(chip);
    }
    if (!m_selection.isEmpty()) {
        auto *chip = new QFrame(this);
        chip->setObjectName(QStringLiteral("ccChip"));
        auto *l = new QHBoxLayout(chip);
        l->setContentsMargins(4, 1, 2, 1);
        l->setSpacing(2);
        auto *open = new QPushButton(FileIcons::forFile(QFileInfo(m_selection.path).fileName()), m_selection.label(), chip);
        open->setObjectName(QStringLiteral("ccChipText"));
        open->setIconSize(QSize(14, 14));
        open->setCursor(Qt::PointingHandCursor);
        open->setToolTip(tr("Goes with your message: lines %1–%2 of %3").arg(m_selection.first).arg(m_selection.last).arg(m_selection.path));
        connect(open, &QPushButton::clicked, this, [this] { emit openRequested(m_selection.path, m_selection.first); });
        QToolButton *x = removeButton(chip, tr("Leave the selection out"));
        connect(x, &QToolButton::clicked, this, [this] {
            m_selection = Selection();
            rebuild();
            emit selectionDismissed();
        });
        l->addWidget(open);
        l->addWidget(x);
        m_layout->addWidget(chip);
    }
    for (int i = 0; i < m_images.size(); ++i) {
        auto *chip = new QFrame(this);
        chip->setObjectName(QStringLiteral("ccChip"));
        auto *l = new QHBoxLayout(chip);
        l->setContentsMargins(3, 3, 2, 3);
        l->setSpacing(2);
        l->addWidget(thumbnailLabel(m_images.at(i), 32, chip));
        QToolButton *x = removeButton(chip, tr("Remove the image"));
        connect(x, &QToolButton::clicked, this, [this, i] {
            if (i < m_images.size())
                m_images.removeAt(i);
            rebuild();
        });
        l->addWidget(x, 0, Qt::AlignTop);
        m_layout->addWidget(chip);
    }
    setVisible(showFile || !m_selection.isEmpty() || !m_images.isEmpty());
    emit changed();
}

// --- QueueList ------------------------------------------------------------------------------------------------------

QueueList::QueueList(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QVBoxLayout(this))
{
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(4);
    hide();
}

void QueueList::setItems(const QStringList &summaries, bool held)
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    if (held && !summaries.isEmpty()) {
        auto *note = new QLabel(tr("The last turn did not finish, so the queue waits. “Send now” sends a message."), this);
        note->setObjectName(QStringLiteral("aiHint"));
        note->setWordWrap(true);
        m_layout->addWidget(note);
    }
    for (int i = 0; i < summaries.size(); ++i) {
        auto *row = new QFrame(this);
        row->setObjectName(QStringLiteral("ccQueued"));
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(10, 4, 4, 4);
        l->setSpacing(8);
        auto *tag = new QLabel(tr("QUEUED"), row);
        tag->setObjectName(QStringLiteral("aiWho"));
        auto *text = new QLabel(summaries.at(i), row);
        text->setTextFormat(Qt::PlainText);
        text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long text is cut, never widens the panel
        text->setToolTip(summaries.at(i));
        auto *now = new QPushButton(tr("Send now"), row);
        now->setObjectName(QStringLiteral("agentFooterBtn"));
        now->setCursor(Qt::PointingHandCursor);
        now->setToolTip(tr("Stop the current turn and send this message next"));
        connect(now, &QPushButton::clicked, this, [this, i] { emit sendNowRequested(i); });
        QToolButton *x = removeButton(row, tr("Remove from the queue"));
        connect(x, &QToolButton::clicked, this, [this, i] { emit removeRequested(i); });
        l->addWidget(tag);
        l->addWidget(text, 1);
        l->addWidget(now);
        l->addWidget(x);
        m_layout->addWidget(row);
    }
    setVisible(!summaries.isEmpty());
}

} // namespace claudecode
