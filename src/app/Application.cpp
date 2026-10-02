#include "Application.h"

#ifndef QODE_VERSION
#define QODE_VERSION "0.1.0"
#endif

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QMenuBar>
#include <QTabBar>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>

Application::Application(int &argc, char **argv)
    : QApplication(argc, argv)
{
    setApplicationName(QStringLiteral("QODE"));
    setOrganizationName(QStringLiteral("QODE"));
    setApplicationVersion(QStringLiteral(QODE_VERSION));
    setDesktopFileName(QStringLiteral("qode"));
    QIcon appIcon;
    for (int size : {16, 32, 48, 64, 128, 256, 512})
        appIcon.addFile(QStringLiteral(":/icons/qode-%1.png").arg(size), QSize(size, size));
    setWindowIcon(appIcon);
    setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // Bundled fonts: Plus Jakarta Sans for the UI, JetBrains Mono for code (see SettingsManager::editorFont).
    const QStringList fonts = QDir(QStringLiteral(":/fonts")).entryList({QStringLiteral("*.ttf")});
    for (const QString &f : fonts)
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/") + f);
    QFont ui(QStringLiteral("Plus Jakarta Sans"));
    ui.setPointSizeF(9.5);
    setFont(ui);

    installEventFilter(this); // app-wide: gives clickable widgets a hand cursor (see eventFilter)
    applyTheme(SettingsManager::instance().theme());
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &Application::applyTheme);
}

// Buttons, tabs and combo boxes get the pointing-hand cursor when they are first polished, unless a
// widget already chose its own cursor.
bool Application::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Polish && watched->isWidgetType()) {
        auto *w = static_cast<QWidget *>(watched);
        if (qstrcmp(w->metaObject()->className(), "QTipLabel") == 0) { // tooltip window: transparent corners so the stylesheet's radius shows
            w->setWindowFlags(w->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
            w->setAttribute(Qt::WA_TranslucentBackground);
            return QApplication::eventFilter(watched, event);
        }
        const bool clickable = qobject_cast<QAbstractButton *>(w) || qobject_cast<QTabBar *>(w) || qobject_cast<QComboBox *>(w) ||
                               qobject_cast<QMenuBar *>(w);
        if (clickable && !w->testAttribute(Qt::WA_SetCursor))
            w->setCursor(Qt::PointingHandCursor);
    }
    return QApplication::eventFilter(watched, event);
}

void Application::applyTheme(const QString &name)
{
    const Theme t = Theme::byName(name);
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.editorFg);
    p.setColor(QPalette::Base, t.editorBg);
    p.setColor(QPalette::AlternateBase, t.panel);
    p.setColor(QPalette::Text, t.editorFg);
    p.setColor(QPalette::Button, t.panel);
    p.setColor(QPalette::ButtonText, t.editorFg);
    p.setColor(QPalette::ToolTipBase, t.panel);
    p.setColor(QPalette::ToolTipText, t.editorFg);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.editorFg);
    p.setColor(QPalette::PlaceholderText, t.textMuted);
    p.setColor(QPalette::Disabled, QPalette::Text, t.textMuted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textMuted);
    setPalette(p);
    setStyleSheet(t.styleSheet());
}
