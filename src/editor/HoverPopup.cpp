#include "HoverPopup.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QPainter>
#include <QScreen>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kMaxWidth = 760;
constexpr int kMinWidth = 420;
constexpr int kMaxHeight = 380;
}

HoverPopup::HoverPopup(QWidget *editor)
    : QFrame(editor, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_view(new QTextBrowser(this))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->addWidget(m_view);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setOpenLinks(false);
    connect(m_view, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        const QString href = url.toString();
        close();
        emit linkActivated(href);
    });
    m_view->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    m_view->viewport()->setCursor(Qt::IBeamCursor);
}

// The window is translucent so the corners can be rounded; background and border are painted here.
void HoverPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(m_border, 1));
    p.setBrush(m_bg);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void HoverPopup::showHtml(const QString &html, const QPoint &global)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_bg = t.panel;
    m_border = t.border;
    m_view->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; color: %1; border: none; selection-background-color: %2; }")
                              .arg(t.editorFg.name(), t.selection.name()));
    m_view->document()->setDefaultStyleSheet(QStringLiteral("a { color: %1; }").arg(t.accent.name()));
    m_view->setHtml(html);

    QTextDocument *doc = m_view->document();
    doc->setDocumentMargin(0);
    doc->setTextWidth(kMaxWidth);
    const int idealWidth = qBound(kMinWidth, int(doc->idealWidth()) + 2, kMaxWidth);
    doc->setTextWidth(idealWidth);
    const int docHeight = int(doc->size().height()) + 2;
    const int h = qMin(kMaxHeight, docHeight);
    const bool scrolls = docHeight > kMaxHeight;
    m_view->setVerticalScrollBarPolicy(scrolls ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const QMargins m = layout()->contentsMargins();
    const int w = idealWidth + m.left() + m.right() + (scrolls ? 14 : 0);
    resize(w, h + m.top() + m.bottom());

    QScreen *screen = QGuiApplication::screenAt(global);
    const QRect avail = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    QPoint p = global + QPoint(0, 14);
    if (p.y() + height() > avail.bottom())
        p.setY(qMax(avail.top(), global.y() - height() - 6));
    p.setX(qBound(avail.left(), p.x(), avail.right() - width()));
    move(p);
    show();
}
