#include "CopyButton.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QTimer>

CopyButton::CopyButton(QWidget *parent, bool withText)
    : QToolButton(parent)
    , m_doneTimer(new QTimer(this))
    , m_withText(withText)
{
    setObjectName(QStringLiteral("copyButton"));
    setCursor(Qt::PointingHandCursor);
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setToolButtonStyle(withText ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    QSizePolicy sp = sizePolicy();
    sp.setRetainSizeWhenHidden(true); // appearing on hover never shifts the row it sits in
    setSizePolicy(sp);
    m_doneTimer->setSingleShot(true);
    m_doneTimer->setInterval(1500);
    connect(m_doneTimer, &QTimer::timeout, this, &CopyButton::updateLook);
    connect(this, &QToolButton::clicked, this, &CopyButton::copy);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &CopyButton::updateLook);
    updateLook();
}

void CopyButton::revealOnHover(QWidget *host)
{
    m_host = host;
    host->installEventFilter(this);
    updateVisibility();
}

void CopyButton::setPinned(bool pinned)
{
    m_pinned = pinned;
    updateVisibility();
}

void CopyButton::setAvailable(bool available)
{
    m_available = available;
    updateVisibility();
}

bool CopyButton::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_host && (event->type() == QEvent::Enter || event->type() == QEvent::Leave))
        QTimer::singleShot(0, this, &CopyButton::updateVisibility); // underMouse() is current once the event is through
    return QToolButton::eventFilter(watched, event);
}

void CopyButton::updateVisibility()
{
    if (m_host)
        setVisible(m_available && (m_pinned || m_host->underMouse() || m_doneTimer->isActive()));
}

void CopyButton::copy()
{
    if (!m_source)
        return;
    QGuiApplication::clipboard()->setText(m_source());
    m_doneTimer->start();
    updateLook();
}

void CopyButton::updateLook()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const bool done = m_doneTimer->isActive();
    setIcon(Icons::tinted(done ? QStringLiteral(":/new-icons/check.svg") : QStringLiteral(":/new-icons/copy.svg"), done ? t.success : t.textMuted));
    const int s = SettingsManager::instance().scaled(13);
    setIconSize(QSize(s, s));
    if (m_withText)
        setText(done ? tr("Copied") : tr("Copy"));
    setToolTip(done ? tr("Copied") : tr("Copy"));
    setStyleSheet(QStringLiteral("QToolButton#copyButton { color: %1; background: transparent; border: none; border-radius: 4px; padding: 1px 4px; }"
                                 "QToolButton#copyButton:hover { background: %2; }"
                                 "QToolButton#copyButton:disabled { color: %3; }")
                      .arg(t.textMuted.name(), t.border.name(), t.border.name()));
    if (!done)
        updateVisibility();
}
