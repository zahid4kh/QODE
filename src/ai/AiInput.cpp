#include "AiInput.h"

#include "explorer/FileIcons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>
#include <QTextBlock>
#include <QTextLayout>
#include <QListWidget>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextCursor>
#include <QVBoxLayout>

AiInput::AiInput(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("aiInput"));
    setFrameShape(QFrame::NoFrame);
    setTabChangesFocus(true);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { fitHeight(); });
    connect(this, &QPlainTextEdit::textChanged, this, &AiInput::detectMention);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &AiInput::detectMention);
    fitHeight();
}

void AiInput::fitHeight()
{
    const int lineH = fontMetrics().lineSpacing();
    const int content = int(document()->size().height()) * lineH;
    // document()->size() is in lines for a QPlainTextEdit
    const int h = qBound(lineH + 20, content + 20, lineH * 8 + 20);
    if (h != height())
        setFixedHeight(h);
}

void AiInput::keyPressEvent(QKeyEvent *e)
{
    if (m_mentionOpen) {
        switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Tab:
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Escape:
            emit mentionKey(e->key());
            return;
        default:
            break;
        }
    }
    if ((e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) && !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier))) {
        // Shell-style recall: an empty input, or a recalled prompt left as it was with the caret on its first (Up) or
        // last (Down) line. Anywhere else the arrows move the caret.
        const bool up = e->key() == Qt::Key_Up;
        const QTextCursor c = textCursor();
        const bool edge = up ? c.block() == document()->firstBlock() && c.block().layout()->lineForTextPosition(c.positionInBlock()).lineNumber() == 0
                             : c.block() == document()->lastBlock() &&
                                   c.block().layout()->lineForTextPosition(c.positionInBlock()).lineNumber() == c.block().layout()->lineCount() - 1;
        if (toPlainText().isEmpty() || (recalling() && edge)) {
            emit historyRequested(up ? -1 : 1);
            return;
        }
    }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ControlModifier)) {
        emit sendRequested();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

void AiInput::setRecalled(const QString &text)
{
    m_recalled = text;
    setPlainText(text);
    moveCursor(QTextCursor::End);
}

void AiInput::focusOutEvent(QFocusEvent *e)
{
    QPlainTextEdit::focusOutEvent(e);
    emit mentionClosed();
}

// An image on the clipboard (a screenshot, "Copy image") or image files from a file manager. Clipboard data that also
// has plain text (office apps add a picture of copied text) stays text.
QList<QImage> AiInput::imagesIn(const QMimeData *source, bool load) const
{
    QList<QImage> out;
    if (!m_acceptsImages || !source)
        return out;
    // A browser's "Copy image" adds the image's address (and an <img> tag) as text: still the image. Text that is more
    // than a link (cells copied from a spreadsheet also come with a picture of them) is pasted as text.
    static const QRegularExpression link(QStringLiteral("^(?:[a-z][a-z0-9+.-]*:|/)\\S*$"), QRegularExpression::CaseInsensitiveOption);
    const bool imageOnly = !source->hasText() || link.match(source->text().trimmed()).hasMatch() ||
                           (source->hasHtml() && source->html().contains(QLatin1String("<img"), Qt::CaseInsensitive));
    if (source->hasImage() && imageOnly) {
        if (!load)
            return {QImage(1, 1, QImage::Format_ARGB32)};
        const QImage img = qvariant_cast<QImage>(source->imageData());
        if (!img.isNull())
            out << img;
        return out;
    }
    if (source->hasUrls()) {
        static const QStringList kTypes{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
                                        QStringLiteral("webp"), QStringLiteral("bmp")};
        for (const QUrl &u : source->urls()) {
            if (!u.isLocalFile() || !kTypes.contains(QFileInfo(u.toLocalFile()).suffix().toLower()))
                return {}; // not only images: insert as text
            const QImage img = load ? QImage(u.toLocalFile()) : QImage(1, 1, QImage::Format_ARGB32);
            if (!img.isNull())
                out << img;
        }
    }
    return out;
}

bool AiInput::canInsertFromMimeData(const QMimeData *source) const
{
    return !imagesIn(source, false).isEmpty() || QPlainTextEdit::canInsertFromMimeData(source);
}

void AiInput::insertFromMimeData(const QMimeData *source)
{
    const QList<QImage> images = imagesIn(source);
    if (!images.isEmpty()) {
        emit imagesPasted(images);
        return;
    }
    QPlainTextEdit::insertFromMimeData(source);
}

// `@` that starts a word, followed by the characters typed so far.
void AiInput::detectMention()
{
    static const QRegularExpression re(QStringLiteral("(?:^|\\s)@([^\\s@]*)$"));
    const QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    const auto m = re.match(before);
    if (m.hasMatch() && hasFocus())
        emit mentionQuery(m.captured(1));
    else
        emit mentionClosed();
}

void AiInput::completeMention(const QString &path)
{
    QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    const int at = before.lastIndexOf(QLatin1Char('@'));
    if (at < 0)
        return;
    c.setPosition(c.block().position() + at);
    c.setPosition(textCursor().position(), QTextCursor::KeepAnchor);
    c.insertText(QLatin1Char('@') + path + QLatin1Char(' '));
    setTextCursor(c);
}

QRect AiInput::mentionAnchor() const
{
    QRect r = cursorRect();
    r.moveTopLeft(viewport()->mapToGlobal(r.topLeft()));
    return r;
}

AiMentionPopup::AiMentionPopup(QWidget *parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint)
    , m_list(new QListWidget(this))
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setObjectName(QStringLiteral("aiMention"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(1, 1, 1, 1);
    lay->addWidget(m_list);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setIconSize(QSize(16, 16));
    m_list->setUniformItemSizes(true);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { emit chosen(it->data(Qt::UserRole).toString()); });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AiMentionPopup::applyTheme);
    applyTheme();
}

void AiMentionPopup::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    setStyleSheet(QStringLiteral("QFrame#aiMention { background: %1; border: 1px solid %2; border-radius: 8px; }"
                                 "QListWidget { background: transparent; border: none; outline: none; }"
                                 "QListWidget::item { padding: 5px 8px; border-radius: 5px; }"
                                 "QListWidget::item:selected { background: %3; color: %4; }")
                      .arg(t.panel.name(), t.border.name(), t.selection.name(), t.editorFg.name()));
}

void AiMentionPopup::setItems(const QStringList &paths)
{
    m_list->clear();
    for (const QString &p : paths) {
        auto *it = new QListWidgetItem(FileIcons::forFile(QFileInfo(p).fileName()), p, m_list);
        it->setData(Qt::UserRole, p);
        it->setToolTip(p);
    }
    if (m_list->count())
        m_list->setCurrentRow(0);
}

int AiMentionPopup::count() const { return m_list->count(); }

QString AiMentionPopup::current() const
{
    auto *it = m_list->currentItem();
    return it ? it->data(Qt::UserRole).toString() : QString();
}

void AiMentionPopup::step(int delta)
{
    const int n = m_list->count();
    if (!n)
        return;
    m_list->setCurrentRow((m_list->currentRow() + delta + n) % n);
}

void AiMentionPopup::showAbove(QWidget *anchor)
{
    // Measured from the rows themselves: the zoom scales the font (and the item padding) but not this window's size.
    const int s = SettingsManager::instance().scaled(16);
    m_list->setIconSize(QSize(s, s));
    const int rows = qMin(m_list->count(), 8);
    ensurePolished();
    const QMargins m = layout()->contentsMargins();
    const int chrome = height() - contentsRect().height() + m.top() + m.bottom() + 2 * m_list->frameWidth(); // border + margins
    const int h = rows * qMax(1, m_list->sizeHintForRow(0)) + chrome;
    const QPoint topLeft = anchor->mapToGlobal(QPoint(0, 0));
    setFixedSize(anchor->width(), h);
    QFrame::move(topLeft.x(), topLeft.y() - h - 6);
    if (!isVisible())
        show();
    raise();
}
