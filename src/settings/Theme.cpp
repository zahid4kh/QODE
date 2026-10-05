#include "Theme.h"

Theme Theme::dark_()
{
    Theme t;
    t.dark = true;
    t.frame = "#14161a";
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
    t.gitAdded = "#81b88b";
    t.gitModified = "#e2c08d";
    t.gitDeleted = "#c74e39";
    t.gitUntracked = "#73c991";
    t.gitRenamed = "#56b6c2";
    t.gitConflict = "#e4676b";
    t.gitIgnored = "#5c6370";
    t.diffAddBg = "#1f3a2a";
    t.diffDelBg = "#442328";
    t.diffFillBg = "#262a31";
    return t;
}

Theme Theme::light_()
{
    Theme t;
    t.dark = false;
    t.frame = "#dcdce1";
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
    t.gitAdded = "#2e7d32";
    t.gitModified = "#946a00";
    t.gitDeleted = "#c0392b";
    t.gitUntracked = "#1e8e4e";
    t.gitRenamed = "#0e7c86";
    t.gitConflict = "#d32f2f";
    t.gitIgnored = "#a0a1a7";
    t.diffAddBg = "#dff3e2";
    t.diffDelBg = "#fbe0e0";
    t.diffFillBg = "#ececef";
    return t;
}

Theme Theme::byName(const QString &name)
{
    return name == QLatin1String("light") ? light_() : dark_();
}

QColor Theme::gitColor(GitKind kind) const
{
    switch (kind) {
    case GitKind::Added: return gitAdded;
    case GitKind::Modified: return gitModified;
    case GitKind::Deleted: return gitDeleted;
    case GitKind::Untracked: return gitUntracked;
    case GitKind::Renamed: return gitRenamed;
    case GitKind::Conflicted: return gitConflict;
    case GitKind::Ignored: return gitIgnored;
    case GitKind::None: break;
    }
    return editorFg;
}

QString Theme::styleSheet() const
{
    const QString fg = editorFg.name();
    return QString(R"(
QMainWindow { background: %15; color: %2; }
QDialog { background: %1; color: %2; }
QWidget { color: %2; }
QMenuBar { background: %15; color: %2; border-bottom: 1px solid %3; }
QMenuBar::item { padding: 4px 9px; background: transparent; }
QMenuBar::item:selected { background: %4; border-radius: 4px; }
QMenu { background: %5; color: %2; border: 1px solid %3; padding: 3px; }
QMenu::item { padding: 4px 22px 4px 18px; }
QMenu::item:selected { background: %6; }
QMenu::separator { height: 1px; background: %3; margin: 3px 6px; }
QToolBar { background: %15; border: none; spacing: 2px; padding: 2px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 3px; padding: 3px; }
QToolButton:hover { background: %4; }
QToolButton:checked { background: %6; }
QStatusBar { background: %15; color: %7; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 0 8px; color: %7; }
QWidget#island { background: %8; }
QSplitter { background: %15; }
QSplitter::handle { background: transparent; }
QTreeView { background: %5; color: %2; border: none; outline: 0; }
QTreeView::item { padding: 2px 0; }
QTreeView::item:hover { background: %4; }
QTreeView::item:selected { background: %6; color: %2; }
QLabel#panelTitle { background: %5; color: %7; padding: 5px 10px; font-size: 8pt; letter-spacing: 1px; border-bottom: 1px solid %3; }
QLabel#emptyTitle { color: %9; font-size: 22pt; font-weight: bold; }
QLabel#emptyText { color: %7; }
QTabWidget::pane { border: none; }
QTabBar { background: %1; qproperty-drawBase: 0; }
QTabBar::tab { background: %1; color: %7; padding: 5px 10px; border: none; border-right: 1px solid %3; min-width: 60px; }
QTabBar::tab:selected { background: %8; color: %2; border-top: 2px solid %9; }
QTabBar::tab:hover:!selected { background: %4; }
QTabBar#terminalTabs { background: transparent; }
QTabBar#terminalTabs::tab { background: transparent; color: %7; padding: 3px 8px; margin: 4px 2px 4px 0; border: 1px solid transparent; border-radius: 6px; min-width: 0; }
QTabBar#terminalTabs::tab:selected { background: %8; color: %2; border: 1px solid %3; }
QTabBar#terminalTabs::tab:hover:!selected { background: %4; color: %2; }
QTabBar::close-button { image: url(%14); subcontrol-position: right; border-radius: 2px; }
QTabBar::close-button:hover { background: %6; }
QLineEdit, QSpinBox, QComboBox { background: %8; color: %2; border: 1px solid %3; border-radius: 3px; padding: 3px 5px; selection-background-color: %6; }
QLineEdit:focus { border-color: %9; }
QPushButton { background: %4; color: %2; border: 1px solid %3; border-radius: 3px; padding: 4px 12px; }
QPushButton:hover { border-color: %9; }
QPushButton:default { border-color: %9; }
QPushButton:disabled { color: %7; }
QToolButton::menu-indicator { image: none; width: 0; }
QToolButton { padding: 4px 5px; }
QToolButton#sectionHeader { background: %1; color: %7; border: none; border-radius: 0; border-bottom: 1px solid %3; padding: 5px 8px; text-align: left; font-size: 8pt; font-weight: 600; letter-spacing: 1px; }
QToolButton#sectionHeader:hover { background: %4; color: %2; }
QTabBar#scmTabs { background: %1; }
QTabBar#scmTabs::tab { background: transparent; color: %7; padding: 7px 12px; border: none; border-bottom: 2px solid transparent; min-width: 0; }
QTabBar#scmTabs::tab:selected { background: transparent; color: %2; border-top: none; border-bottom: 2px solid %9; }
QTabBar#scmTabs::tab:hover:!selected { background: %4; }
QPushButton#primaryBtn { background: %9; color: %11; border: 1px solid %9; border-radius: 6px; padding: 6px 12px; font-weight: 600; }
QPushButton#primaryBtn:hover { background: %12; border-color: %12; }
QPushButton#primaryBtn:disabled { background: %4; color: %7; border-color: %3; }
QPushButton#flatBtn { background: transparent; border: none; border-top: 1px solid %3; border-radius: 0; padding: 8px 10px; text-align: left; }
QPushButton#flatBtn:hover { background: %4; }
QFrame#branchPopup, QFrame#palettePopup { background: %5; border: 1px solid %10; border-radius: 8px; }
QFrame#branchPopup QListWidget, QFrame#palettePopup QListWidget { background: transparent; outline: 0; }
QPlainTextEdit { background: %8; border: 1px solid %3; border-radius: 6px; padding: 4px; selection-background-color: %6; }
QPlainTextEdit:focus { border-color: %9; }
QLabel#countPill { background: %13; color: %9; border-radius: 8px; padding: 0 7px; font-weight: 700; font-size: 8pt; }
QToolButton#findBtn { padding: 2px 6px; }
QFrame#findBar { background: %5; border-bottom: 1px solid %3; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle { background: %10; border-radius: 4px; min-height: 24px; min-width: 24px; margin: 2px; }
QScrollBar::handle:hover { background: %7; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QFrame#explorerNotice { background: %5; border: 1px solid %10; border-radius: 10px; }
QFrame#explorerNotice[error="true"] { border: 1px solid #e5534b; }
QFrame#explorerNotice QLabel { background: transparent; color: %2; }
QToolButton#noticeAction { background: %13; color: %9; border: none; border-radius: 6px; padding: 3px 11px; font-weight: 600; }
QToolButton#noticeAction:hover { background: %9; color: %11; }
QToolButton#noticeSecondary { background: transparent; color: %7; border: none; border-radius: 6px; padding: 3px 8px; }
QToolButton#noticeSecondary:hover { background: %4; color: %2; }
QToolTip { background: %5; color: %2; border: 1px solid %10; border-radius: 8px; padding: 6px 8px; }
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
             gutterFg.name(),    // 10
             onAccent().name(),  // 11
             accent.lighter(dark ? 115 : 108).name(), // 12
             QColor(accent.red(), accent.green(), accent.blue(), dark ? 48 : 36).name(QColor::HexArgb), // 13
             dark ? QStringLiteral(":/new-icons/x-dark.svg") : QStringLiteral(":/new-icons/x-light.svg"), // 14
             frame.name()); // 15
}
