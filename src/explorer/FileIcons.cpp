#include "FileIcons.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "settings/ThemeManager.h"

#include <QFileInfo>
#include <QHash>

namespace FileIcons {

namespace {

enum class Tint { Muted, Accent, Function, Type, Keyword, String, Number, Tag, Rename };

struct Spec {
    const char *icon;
    Tint tint;
};

QColor colorFor(Tint t, const Theme &th)
{
    switch (t) {
    case Tint::Muted: return th.textMuted;
    case Tint::Accent: return th.accent;
    case Tint::Function: return th.function;
    case Tint::Type: return th.type;
    case Tint::Keyword: return th.keyword;
    case Tint::String: return th.string;
    case Tint::Number: return th.number;
    case Tint::Tag: return th.tag;
    case Tint::Rename: return th.gitRenamed;
    }
    return th.textMuted;
}

struct Tables {
    QHash<QString, Spec> byExtension;
    QHash<QString, Spec> byName; // exact lower-case file names
    Tables()
    {
        auto add = [this](const Spec &s, std::initializer_list<const char *> exts) {
            for (const char *e : exts)
                byExtension.insert(QString::fromLatin1(e), s);
        };
        const Spec code{"file-code", Tint::Function};
        add(code, {"c", "h", "cpp", "cc", "cxx", "c++", "hpp", "hh", "hxx", "h++", "ipp", "tpp", "inl", "ino", "ts", "tsx", "mts", "cts",
                   "qml", "cs", "dart"});
        add({"file-code", Tint::Type}, {"py", "pyw", "pyi", "js", "mjs", "cjs", "jsx", "lua", "pl"});
        add({"file-code", Tint::Keyword}, {"rs", "go", "java", "kt", "kts", "swift", "php", "rb", "scala", "zig", "hs", "ex", "exs"});
        add({"file-code", Tint::Tag}, {"html", "htm", "xhtml", "xml", "xsl", "xslt", "ui", "qrc", "plist", "rss", "atom", "xsd", "vue"});
        add({"file-code", Tint::Number}, {"css", "scss", "sass", "less"});
        add({"file-json", Tint::Type}, {"json", "jsonc", "webmanifest", "geojson"});
        add({"cog", Tint::Number}, {"yaml", "yml", "toml", "ini", "cfg", "conf", "config", "pro", "pri", "prf", "cmake", "mk", "gradle",
                                    "properties", "desktop", "env", "editorconfig"});
        add({"file-text", Tint::Accent}, {"md", "markdown", "mdown"});
        add({"file-text", Tint::Muted}, {"txt", "log", "rst", "text", "csv", "tsv", "tex", "license", "pdf", "doc", "docx", "odt"});
        add({"file-terminal", Tint::String}, {"sh", "bash", "zsh", "ksh", "fish", "bat", "cmd", "ps1"});
        add({"image", Tint::Keyword}, {"png", "jpg", "jpeg", "gif", "webp", "bmp", "ico", "svg", "tif", "tiff", "avif", "icns"});

        byName.insert(QStringLiteral("cmakelists.txt"), {"cog", Tint::Number});
        byName.insert(QStringLiteral("makefile"), {"cog", Tint::Number});
        byName.insert(QStringLiteral("dockerfile"), {"cog", Tint::Number});
        byName.insert(QStringLiteral(".gitignore"), {"git-branch", Tint::Rename});
        byName.insert(QStringLiteral(".gitattributes"), {"git-branch", Tint::Rename});
        byName.insert(QStringLiteral(".gitmodules"), {"git-branch", Tint::Rename});
        byName.insert(QStringLiteral("license"), {"file-text", Tint::Muted});
        byName.insert(QStringLiteral("readme"), {"file-text", Tint::Accent});
        byName.insert(QStringLiteral(".bashrc"), {"file-terminal", Tint::String});
        byName.insert(QStringLiteral(".zshrc"), {"file-terminal", Tint::String});
        byName.insert(QStringLiteral(".profile"), {"file-terminal", Tint::String});
    }
};

// Icons are cached per theme: the cache empties itself when the theme changes.
struct Cache {
    QString theme;
    int revision = -1;
    Theme palette;
    QHash<QString, QIcon> icons;

    void sync()
    {
        const QString current = SettingsManager::instance().theme();
        if (current != theme || revision != ThemeManager::instance().revision()) {
            theme = current;
            revision = ThemeManager::instance().revision();
            palette = Theme::byName(current);
            icons.clear();
        }
    }
};

Cache &cache()
{
    static Cache c;
    c.sync();
    return c;
}

QIcon make(const char *icon, Tint tint, Cache &c)
{
    const QString key = QString::fromLatin1(icon) + QLatin1Char('|') + QString::number(int(tint));
    auto it = c.icons.constFind(key);
    if (it != c.icons.constEnd())
        return it.value();
    QColor col = colorFor(tint, c.palette);
    col.setAlphaF(0.92);
    const QIcon ic = Icons::tinted(QStringLiteral(":/new-icons/") + QString::fromLatin1(icon) + QStringLiteral(".svg"), col);
    c.icons.insert(key, ic);
    return ic;
}

} // namespace

QIcon forFile(const QString &fileName)
{
    static const Tables tables;
    Cache &c = cache();
    const QString lower = fileName.toLower();
    auto n = tables.byName.constFind(lower);
    if (n != tables.byName.constEnd())
        return make(n->icon, n->tint, c);
    // "README.md" style: the stem decides when the extension is generic.
    const QString ext = QFileInfo(lower).suffix();
    auto e = tables.byExtension.constFind(ext);
    if (e != tables.byExtension.constEnd())
        return make(e->icon, e->tint, c);
    return make("file", Tint::Muted, c);
}

QIcon folder()
{
    Cache &c = cache();
    return make("folder", Tint::Accent, c);
}

} // namespace FileIcons
