#include "Application.h"

#ifndef QODE_VERSION
#define QODE_VERSION "0.1.0"
#endif

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

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
    setWindowIcon(QIcon(QStringLiteral(":/icons/qode.svg")));
    setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    applyTheme(SettingsManager::instance().theme());
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &Application::applyTheme);
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
