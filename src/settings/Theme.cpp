#include "Theme.h"

#include "ThemeManager.h"

#include <QJsonValue>

Theme Theme::dark_()
{
    Theme t;
    t.name = QStringLiteral("Dark");
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
    t.success = "#4ec969";
    t.warning = "#e5b94e";
    t.danger = "#f26b6b";
    t.idle = "#6b7078";
    t.findMatchBg = "#614d1f";
    static const char *ansi[16] = {"#3f4451", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd", "#56b6c2", "#abb2bf",
                                   "#5c6370", "#ef7a85", "#a9d98a", "#f0d08b", "#72bfff", "#d689ee", "#67c7d3", "#ffffff"};
    for (int i = 0; i < 16; ++i)
        t.ansi[i] = QColor(QLatin1String(ansi[i]));
    return t;
}

Theme Theme::light_()
{
    Theme t;
    t.name = QStringLiteral("Light");
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
    t.success = "#1a8f3c";
    t.warning = "#b07d0a";
    t.danger = "#c42b2b";
    t.idle = "#a0a4ab";
    t.findMatchBg = "#f5e08a";
    static const char *ansi[16] = {"#383a42", "#e45649", "#50a14f", "#c18401", "#4078f2", "#a626a4", "#0184bc", "#a0a1a7",
                                   "#4f525e", "#e45649", "#50a14f", "#c18401", "#4078f2", "#a626a4", "#0184bc", "#ffffff"};
    for (int i = 0; i < 16; ++i)
        t.ansi[i] = QColor(QLatin1String(ansi[i]));
    return t;
}


Theme Theme::monoDark_()
{
    Theme t;
    t.name = QStringLiteral("Monochrome Dark");
    t.dark = true;
    t.frame = "#0c0c0c";
    t.window = "#161616";
    t.panel = "#1b1b1b";
    t.editorBg = "#121212";
    t.editorFg = "#d4d4d4";
    t.gutterBg = "#121212";
    t.gutterFg = "#555555";
    t.gutterActiveFg = "#d4d4d4";
    t.currentLine = "#1c1c1c";
    t.selection = "#3a3a3a";
    t.border = "#262626";
    t.accent = "#e6e6e6";
    t.textMuted = "#8a8a8a";
    t.keyword = "#c678dd";
    t.type = "#e5c07b";
    t.string = "#98c379";
    t.comment = "#5c6370";
    t.number = "#d19a66";
    t.preprocessor = "#e06c75";
    t.function = "#61afef";
    t.tag = "#e06c75";
    t.attribute = "#d19a66";
    t.termBg = "#0e0e0e";
    t.termFg = "#cccccc";
    t.gitAdded = "#b5b5b5";
    t.gitModified = "#9a9a9a";
    t.gitDeleted = "#7a7a7a";
    t.gitUntracked = "#c8c8c8";
    t.gitRenamed = "#adadad";
    t.gitConflict = "#f0f0f0";
    t.gitIgnored = "#555555";
    t.diffAddBg = "#262626";
    t.diffDelBg = "#1a1a1a";
    t.diffFillBg = "#171717";
    t.success = "#d0d0d0";
    t.warning = "#a0a0a0";
    t.danger = "#ffffff";
    t.idle = "#5a5a5a";
    t.findMatchBg = "#4d4d4d";
    static const char *ansi[16] = {"#3f4451", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd", "#56b6c2", "#abb2bf", "#5c6370", "#ef7a85", "#a9d98a", "#f0d08b", "#72bfff", "#d689ee", "#67c7d3", "#ffffff"};
    for (int i = 0; i < 16; ++i)
        t.ansi[i] = QColor(QLatin1String(ansi[i]));
    return t;
}

Theme Theme::monoLight_()
{
    Theme t;
    t.name = QStringLiteral("Monochrome Light");
    t.dark = false;
    t.frame = "#a9a9a9";
    t.window = "#bfbfbf";
    t.panel = "#c8c8c8";
    t.editorBg = "#cfcfcf";
    t.editorFg = "#1a1a1a";
    t.gutterBg = "#cfcfcf";
    t.gutterFg = "#7c7c7c";
    t.gutterActiveFg = "#1a1a1a";
    t.currentLine = "#c6c6c6";
    t.selection = "#a4a4a4";
    t.border = "#a8a8a8";
    t.accent = "#242424";
    t.textMuted = "#555555";
    t.keyword = "#8a1c88";
    t.type = "#8a5a00";
    t.string = "#2f6f2e";
    t.comment = "#6a6e78";
    t.number = "#7a4e00";
    t.preprocessor = "#b3362b";
    t.function = "#1f55d0";
    t.tag = "#b3362b";
    t.attribute = "#7a4e00";
    t.termBg = "#d6d6d6";
    t.termFg = "#1c1c1c";
    t.gitAdded = "#3a3a3a";
    t.gitModified = "#5a5a5a";
    t.gitDeleted = "#707070";
    t.gitUntracked = "#2a2a2a";
    t.gitRenamed = "#4a4a4a";
    t.gitConflict = "#000000";
    t.gitIgnored = "#808080";
    t.diffAddBg = "#bcbcbc";
    t.diffDelBg = "#c6c6c6";
    t.diffFillBg = "#c2c2c2";
    t.success = "#2e2e2e";
    t.warning = "#585858";
    t.danger = "#000000";
    t.idle = "#7a7a7a";
    t.findMatchBg = "#9c9c9c";
    static const char *ansi[16] = {"#1a1a1a", "#b3362b", "#2f6f2e", "#8a5a00", "#1f55d0", "#8a1c88", "#00637f", "#5a5a5a", "#4a4a4a", "#c4453a", "#3a803a", "#9a6a00", "#2f65e0", "#9a2c98", "#0a7390", "#ffffff"};
    for (int i = 0; i < 16; ++i)
        t.ansi[i] = QColor(QLatin1String(ansi[i]));
    return t;
}

Theme Theme::darcula_()
{
    // JetBrains Darcula: UI from darcula.theme.json, syntax from the bundled editor scheme, terminal from the
    // published ANSI palette. Git text colours are the scheme's legible greens/blues (its gutter fills are too dark
    // to read as text).
    Theme t;
    t.name = QStringLiteral("Darcula");
    t.dark = true;
    t.frame = "#3c3f41";
    t.window = "#3c3f41";
    t.panel = "#3c3f41";
    t.editorBg = "#2b2b2b";
    t.editorFg = "#a9b7c6";
    t.gutterBg = "#313335";
    t.gutterFg = "#606366";
    t.gutterActiveFg = "#a4a3a3";
    t.currentLine = "#323232";
    t.selection = "#214283";
    t.border = "#515151";
    t.accent = "#4a88c7";
    t.textMuted = "#808080";
    t.keyword = "#cc7832";
    t.type = "#a9b7c6";
    t.string = "#6a8759";
    t.comment = "#808080";
    t.number = "#6897bb";
    t.preprocessor = "#bbb529";
    t.function = "#ffc66d";
    t.tag = "#e8bf6a";
    t.attribute = "#bababa";
    t.termBg = "#202020";
    t.termFg = "#adadad";
    t.gitAdded = "#629755";
    t.gitModified = "#6897bb";
    t.gitDeleted = "#bc3f3c";
    t.gitUntracked = "#a5c261";
    t.gitRenamed = "#33c2c1";
    t.gitConflict = "#ff6b68";
    t.gitIgnored = "#808080";
    t.diffAddBg = "#36563a";
    t.diffDelBg = "#623539";
    t.diffFillBg = "#313335";
    t.success = "#008f50";
    t.warning = "#ac7920";
    t.danger = "#e74848";
    t.idle = "#777777";
    t.findMatchBg = "#32593d";
    static const char *ansi[16] = {"#000000", "#fa5355", "#126e00", "#c2c300", "#4581eb", "#fa54ff", "#33c2c1", "#adadad",
                                   "#555555", "#fb7172", "#67ff4f", "#ffff00", "#6d9df1", "#fb82ff", "#60d3d1", "#eeeeee"};
    for (int i = 0; i < 16; ++i)
        t.ansi[i] = QColor(QLatin1String(ansi[i]));
    return t;
}

const QList<Theme::Field> &Theme::fields()
{
#define FIELD(member, key, label, group) {key, label, group, [](Theme &t) -> QColor & { return t.member; }}
#define ANSI(i, label) FIELD(ansi[i], "ansi" #i, label, "Terminal colours")
    static const QList<Field> list = {
        FIELD(frame, "frame", "Window frame", "Interface"),
        FIELD(window, "window", "Window background", "Interface"),
        FIELD(panel, "panel", "Panels and menus", "Interface"),
        FIELD(border, "border", "Borders", "Interface"),
        FIELD(accent, "accent", "Accent", "Interface"),
        FIELD(textMuted, "textMuted", "Secondary text", "Interface"),
        FIELD(success, "success", "Success / running", "Interface"),
        FIELD(warning, "warning", "Warning / starting", "Interface"),
        FIELD(danger, "danger", "Error", "Interface"),
        FIELD(idle, "idle", "Idle / stopped", "Interface"),

        FIELD(editorBg, "editorBg", "Background", "Editor"),
        FIELD(editorFg, "editorFg", "Text", "Editor"),
        FIELD(gutterBg, "gutterBg", "Line number background", "Editor"),
        FIELD(gutterFg, "gutterFg", "Line numbers", "Editor"),
        FIELD(gutterActiveFg, "gutterActiveFg", "Current line number", "Editor"),
        FIELD(currentLine, "currentLine", "Current line", "Editor"),
        FIELD(selection, "selection", "Selection", "Editor"),
        FIELD(findMatchBg, "findMatchBg", "Find match", "Editor"),

        FIELD(keyword, "keyword", "Keywords", "Syntax"),
        FIELD(type, "type", "Types", "Syntax"),
        FIELD(string, "string", "Strings", "Syntax"),
        FIELD(comment, "comment", "Comments", "Syntax"),
        FIELD(number, "number", "Numbers", "Syntax"),
        FIELD(preprocessor, "preprocessor", "Preprocessor", "Syntax"),
        FIELD(function, "function", "Functions", "Syntax"),
        FIELD(tag, "tag", "Tags", "Syntax"),
        FIELD(attribute, "attribute", "Attributes", "Syntax"),

        FIELD(termBg, "termBg", "Background", "Terminal"),
        FIELD(termFg, "termFg", "Text", "Terminal"),
        ANSI(0, "Black"), ANSI(1, "Red"), ANSI(2, "Green"), ANSI(3, "Yellow"),
        ANSI(4, "Blue"), ANSI(5, "Magenta"), ANSI(6, "Cyan"), ANSI(7, "White"),
        ANSI(8, "Bright black"), ANSI(9, "Bright red"), ANSI(10, "Bright green"), ANSI(11, "Bright yellow"),
        ANSI(12, "Bright blue"), ANSI(13, "Bright magenta"), ANSI(14, "Bright cyan"), ANSI(15, "Bright white"),

        FIELD(gitAdded, "gitAdded", "Added", "Git"),
        FIELD(gitModified, "gitModified", "Modified", "Git"),
        FIELD(gitDeleted, "gitDeleted", "Deleted", "Git"),
        FIELD(gitUntracked, "gitUntracked", "Untracked", "Git"),
        FIELD(gitRenamed, "gitRenamed", "Renamed", "Git"),
        FIELD(gitConflict, "gitConflict", "Conflict", "Git"),
        FIELD(gitIgnored, "gitIgnored", "Ignored", "Git"),
        FIELD(diffAddBg, "diffAddBg", "Added lines", "Diff"),
        FIELD(diffDelBg, "diffDelBg", "Removed lines", "Diff"),
        FIELD(diffFillBg, "diffFillBg", "Filler", "Diff"),
    };
#undef ANSI
#undef FIELD
    return list;
}

namespace {
QString colorText(const QColor &c)
{
    return c.name(c.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
}
} // namespace

QJsonObject Theme::toJson() const
{
    Theme copy = *this;
    QJsonObject colors;
    for (const Field &f : fields())
        colors.insert(QLatin1String(f.key), colorText(f.ref(copy)));
    QJsonObject o;
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("base"), dark ? QStringLiteral("dark") : QStringLiteral("light"));
    o.insert(QStringLiteral("colors"), colors);
    return o;
}

Theme Theme::fromJson(const QJsonObject &obj)
{
    const bool light = obj.value(QStringLiteral("base")).toString() == QLatin1String("light");
    Theme t = light ? light_() : dark_();
    const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
    if (!name.isEmpty())
        t.name = name;
    const QJsonObject colors = obj.value(QStringLiteral("colors")).toObject();
    for (const Field &f : fields()) {
        const QJsonValue v = colors.value(QLatin1String(f.key));
        if (!v.isString())
            continue;
        const QColor c(v.toString().trimmed());
        if (c.isValid())
            f.ref(t) = c;
    }
    return t;
}

Theme Theme::byName(const QString &name)
{
    return ThemeManager::instance().theme(name);
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
QMenu { background: %5; color: %2; border: 1px solid %10; border-radius: 10px; padding: 5px; }
QMenu::item { padding: 6px 26px 6px 12px; margin: 1px 0; border-radius: 6px; background: transparent; }
QMenu::item:selected { background: %6; color: %2; }
QMenu::item:disabled { color: %7; background: transparent; }
QMenu::icon { padding-left: 8px; }
QMenu::right-arrow { width: 8px; height: 8px; margin-right: 8px; }
QMenu::separator { height: 1px; background: %3; margin: 5px 8px; }
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
QFrame#explorerNotice[error="true"] { border: 1px solid %16; }
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
             frame.name(),       // 15
             danger.name());     // 16
}
