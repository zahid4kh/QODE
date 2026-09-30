#include "Theme.h"

Theme Theme::dark_()
{
    Theme t;
    t.dark = true;
    t.window = "#1e2025";
    t.panel = "#23262d";
    t.editorBg = "#1e2127";
    t.editorFg = "#abb2bf";
    t.gutterBg = "#1e2127";
    t.gutterFg = "#4b5263";
    t.gutterActiveFg = "#abb2bf";
    t.currentLine = "#2a2f38";
    t.selection = "#3a4a66";
    t.border = "#181a1f";
    t.accent = "#61afef";
    t.textMuted = "#7f848e";
    t.keyword = "#c678dd";
    t.type = "#e5c07b";
    t.string = "#98c379";
    t.comment = "#5c6370";
    t.number = "#d19a66";
    t.preprocessor = "#e06c75";
    t.function = "#61afef";
    t.tag = "#e06c75";
    t.attribute = "#d19a66";
    t.termBg = "#191b20";
    t.termFg = "#c5c8d0";
    return t;
}

Theme Theme::light_()
{
    Theme t;
    t.dark = false;
    t.window = "#f0f0f2";
    t.panel = "#f7f7f9";
    t.editorBg = "#fafafa";
    t.editorFg = "#383a42";
    t.gutterBg = "#fafafa";
    t.gutterFg = "#a0a1a7";
    t.gutterActiveFg = "#383a42";
    t.currentLine = "#efefef";
    t.selection = "#cfe0f7";
    t.border = "#d4d4d8";
    t.accent = "#4078f2";
    t.textMuted = "#7a7c85";
    t.keyword = "#a626a4";
    t.type = "#c18401";
    t.string = "#50a14f";
    t.comment = "#a0a1a7";
    t.number = "#986801";
    t.preprocessor = "#e45649";
    t.function = "#4078f2";
    t.tag = "#e45649";
    t.attribute = "#986801";
    t.termBg = "#ffffff";
    t.termFg = "#383a42";
    return t;
}

Theme Theme::byName(const QString &name)
{
    return name == QLatin1String("light") ? light_() : dark_();
}

QString Theme::styleSheet() const
{
    const QString fg = editorFg.name();
    return QString(R"(
QMainWindow, QDialog { background: %1; color: %2; }
QWidget { color: %2; }
QMenuBar { background: %1; color: %2; border-bottom: 1px solid %3; }
QMenuBar::item { padding: 4px 9px; background: transparent; }
QMenuBar::item:selected { background: %4; }
QMenu { background: %5; color: %2; border: 1px solid %3; padding: 3px; }
QMenu::item { padding: 4px 22px 4px 18px; }
QMenu::item:selected { background: %6; }
QMenu::separator { height: 1px; background: %3; margin: 3px 6px; }
QToolBar { background: %1; border: none; border-bottom: 1px solid %3; spacing: 2px; padding: 2px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 3px; padding: 3px; }
QToolButton:hover { background: %4; }
QToolButton:checked { background: %6; }
QStatusBar { background: %1; color: %7; border-top: 1px solid %3; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 0 8px; color: %7; }
QSplitter::handle { background: %3; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QTreeView { background: %5; color: %2; border: none; outline: 0; }
QTreeView::item { padding: 2px 0; }
QTreeView::item:hover { background: %4; }
QTreeView::item:selected { background: %6; color: %2; }
QLabel#panelTitle { background: %5; color: %7; padding: 5px 10px; font-size: 8pt; letter-spacing: 1px; border-bottom: 1px solid %3; }
QTabWidget::pane { border: none; }
QTabBar { background: %1; qproperty-drawBase: 0; }
QTabBar::tab { background: %1; color: %7; padding: 5px 10px; border: none; border-right: 1px solid %3; min-width: 60px; }
QTabBar::tab:selected { background: %8; color: %2; border-top: 2px solid %9; }
QTabBar::tab:hover:!selected { background: %4; }
QTabBar::close-button { image: url(:/icons/close.svg); subcontrol-position: right; border-radius: 2px; }
QTabBar::close-button:hover { background: %6; }
QLineEdit, QSpinBox, QComboBox { background: %8; color: %2; border: 1px solid %3; border-radius: 3px; padding: 3px 5px; selection-background-color: %6; }
QLineEdit:focus { border-color: %9; }
QPushButton { background: %4; color: %2; border: 1px solid %3; border-radius: 3px; padding: 4px 12px; }
QPushButton:hover { border-color: %9; }
QPushButton:default { border-color: %9; }
QPushButton:disabled { color: %7; }
QToolButton#findBtn { padding: 2px 6px; }
QFrame#findBar { background: %5; border-bottom: 1px solid %3; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle { background: %10; border-radius: 4px; min-height: 24px; min-width: 24px; margin: 2px; }
QScrollBar::handle:hover { background: %7; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QToolTip { background: %5; color: %2; border: 1px solid %3; }
QMessageBox, QInputDialog { background: %1; }
)")
        .arg(window.name(),      // 1
             fg,                 // 2
             border.name(),      // 3
             currentLine.name(), // 4
             panel.name(),       // 5
             selection.name(),   // 6
             textMuted.name(),   // 7
             editorBg.name(),    // 8
             accent.name(),      // 9
             gutterFg.name());   // 10
}
