#include "CliCommands.h"

#include "Ansi.h"
#include "editor/Language.h"
#include "editor/SyntaxHighlighter.h"
#include "palette/FuzzyMatcher.h"
#include "project/ProjectFiles.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "settings/ThemeManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>
#include <memory>

using namespace Ansi;

namespace {

QString pad(const QString &s, int width)
{
    return s + QString(qMax(0, width - int(s.size())), QLatin1Char(' '));
}

QString padLeft(const QString &s, int width)
{
    return QString(qMax(0, width - int(s.size())), QLatin1Char(' ')) + s;
}

QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QString::number(bytes);
    const char *units = "KMGT";
    double v = double(bytes);
    int u = -1;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        ++u;
    }
    return QString::number(v, 'f', v < 10 ? 1 : 0) + QLatin1Char(units[u]);
}

// "path/to/file:42" -> path, 42 (only when the part before the colon exists, so odd file names still work).
void splitLocation(const CliCall &c, const QString &arg, QString *path, int *line)
{
    *path = c.resolve(arg);
    *line = 0;
    const int colon = arg.lastIndexOf(QLatin1Char(':'));
    if (colon > 0 && !QFileInfo::exists(*path)) {
        bool ok = false;
        const int n = arg.mid(colon + 1).toInt(&ok);
        if (ok && n > 0) {
            *path = c.resolve(arg.left(colon));
            *line = n;
        }
    }
}

struct Flags {
    QStringList options; // every character of each "-abc" argument
    QStringList words;
    bool has(QChar c) const { return options.contains(QString(c)); }
};

Flags parseFlags(const QStringList &args)
{
    Flags f;
    for (const QString &a : args) {
        if (a.size() > 1 && a.startsWith(QLatin1Char('-')) && !a.startsWith(QLatin1String("--")) && !a.at(1).isDigit())
            for (int i = 1; i < a.size(); ++i)
                f.options << QString(a.at(i));
        else
            f.words << a;
    }
    return f;
}

struct Entry {
    QString name;
    QString suffix;
    int color = -1; // Ansi::Color, -1 = default
    bool bold = false;
    QFileInfo info;
};

Entry describe(const QFileInfo &fi)
{
    Entry e;
    e.info = fi;
    e.name = fi.fileName();
    if (fi.isSymLink()) {
        e.color = Cyan;
        e.suffix = QStringLiteral("@");
        if (fi.isDir())
            e.bold = true;
    } else if (fi.isDir()) {
        e.color = Blue;
        e.bold = true;
        e.suffix = QStringLiteral("/");
    } else if (fi.isExecutable()) {
        e.color = Green;
        e.suffix = QStringLiteral("*");
    } else if (e.name.startsWith(QLatin1Char('.'))) {
        e.color = Muted;
    }
    return e;
}

QString styled(const Entry &e)
{
    const QString text = e.name + e.suffix;
    if (e.color < 0 && !e.bold)
        return text;
    return paint(text, e.color < 0 ? White : e.color, e.bold);
}

// --- /ls -----------------------------------------------------------------------

QString permissions(const QFileInfo &fi)
{
    QString s;
    s += fi.isSymLink() ? 'l' : fi.isDir() ? 'd' : '-';
    const auto p = fi.permissions();
    struct Bit { QFileDevice::Permission perm; char ch; };
    static const Bit bits[] = {{QFileDevice::ReadOwner, 'r'}, {QFileDevice::WriteOwner, 'w'}, {QFileDevice::ExeOwner, 'x'},
                               {QFileDevice::ReadGroup, 'r'}, {QFileDevice::WriteGroup, 'w'}, {QFileDevice::ExeGroup, 'x'},
                               {QFileDevice::ReadOther, 'r'}, {QFileDevice::WriteOther, 'w'}, {QFileDevice::ExeOther, 'x'}};
    for (const Bit &b : bits)
        s += (p & b.perm) ? QLatin1Char(b.ch) : QLatin1Char('-');
    return s;
}

void cmdLs(CliCall &c)
{
    const Flags f = parseFlags(c.args);
    const bool all = f.has(QLatin1Char('a')) || f.has(QLatin1Char('A'));
    const bool longForm = f.has(QLatin1Char('l'));
    const QString target = f.words.isEmpty() ? c.cwd() : c.resolve(f.words.first());
    const QFileInfo ti(target);
    if (!ti.exists() && !ti.isSymLink()) {
        c.error(QStringLiteral("ls: cannot access '%1': No such file or directory").arg(f.words.value(0)));
        return;
    }
    QList<Entry> entries;
    if (!ti.isDir()) {
        entries << describe(ti);
    } else {
        QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
        if (all)
            filters |= QDir::Hidden;
        for (const QFileInfo &fi : QDir(target).entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase))
            entries << describe(fi);
    }
    if (entries.isEmpty()) {
        c.println(dim(QStringLiteral("(empty)")));
        return;
    }

    if (longForm) {
        int sizeWidth = 1, ownerWidth = 1;
        for (const Entry &e : entries) {
            sizeWidth = qMax(sizeWidth, int(humanSize(e.info.size()).size()));
            ownerWidth = qMax(ownerWidth, int(e.info.owner().size()));
        }
        const QDateTime now = QDateTime::currentDateTime();
        for (const Entry &e : entries) {
            const QDateTime m = e.info.lastModified();
            const QString date = m.daysTo(now) > 180 ? QLocale::c().toString(m, QStringLiteral("MMM d  yyyy"))
                                                      : QLocale::c().toString(m, QStringLiteral("MMM d HH:mm"));
            QString line = dim(permissions(e.info)) + QLatin1Char(' ') + pad(e.info.owner(), ownerWidth) + QLatin1Char(' ') +
                           padLeft(e.info.isDir() ? QStringLiteral("-") : humanSize(e.info.size()), sizeWidth) + QLatin1Char(' ') +
                           dim(pad(date, 12)) + QLatin1Char(' ') + styled(e);
            if (e.info.isSymLink())
                line += dim(QStringLiteral(" -> ") + e.info.symLinkTarget());
            c.println(line);
        }
        return;
    }

    int widest = 0;
    for (const Entry &e : entries)
        widest = qMax(widest, int((e.name + e.suffix).size()));
    const int colWidth = widest + 2;
    const int cols = qMax(1, c.columns() / colWidth);
    const int rows = (entries.size() + cols - 1) / cols;
    for (int r = 0; r < rows; ++r) {
        QString line;
        for (int col = 0; col < cols; ++col) {
            const int i = col * rows + r;
            if (i >= entries.size())
                break;
            const Entry &e = entries.at(i);
            line += styled(e);
            if (i + rows < entries.size())
                line += QString(qMax(0, colWidth - int((e.name + e.suffix).size())), QLatin1Char(' '));
        }
        c.println(line);
    }
}

// --- /tree ---------------------------------------------------------------------

struct TreeState {
    bool all = false;
    int maxDepth = 3;
    int lines = 0;
    int dirs = 0;
    int files = 0;
    bool truncated = false;
    CliCall *call = nullptr;
};

bool clutter(const QString &name)
{
    static const QStringList names = {QStringLiteral(".git"), QStringLiteral("node_modules"), QStringLiteral("__pycache__"),
                                      QStringLiteral(".gradle"), QStringLiteral(".venv"), QStringLiteral("venv"),
                                      QStringLiteral(".cache"), QStringLiteral(".next"), QStringLiteral(".idea")};
    return names.contains(name);
}

void walk(TreeState &st, const QString &path, const QString &prefix, int depth)
{
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (st.all)
        filters |= QDir::Hidden;
    QFileInfoList list;
    for (const QFileInfo &fi : QDir(path).entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase))
        if (st.all || !clutter(fi.fileName()))
            list << fi;
    for (int i = 0; i < list.size(); ++i) {
        if (st.lines >= 500) {
            st.truncated = true;
            return;
        }
        const QFileInfo &fi = list.at(i);
        const bool last = i == list.size() - 1;
        const Entry e = describe(fi);
        st.call->println(dim(prefix + (last ? QStringLiteral("└── ") : QStringLiteral("├── "))) + styled(e));
        ++st.lines;
        if (fi.isDir() && !fi.isSymLink()) {
            ++st.dirs;
            if (depth < st.maxDepth)
                walk(st, fi.filePath(), prefix + (last ? QStringLiteral("    ") : QStringLiteral("│   ")), depth + 1);
        } else {
            ++st.files;
        }
    }
}

void cmdTree(CliCall &c)
{
    TreeState st;
    st.call = &c;
    QString target;
    for (int i = 0; i < c.args.size(); ++i) {
        const QString &a = c.args.at(i);
        if (a == QLatin1String("-a"))
            st.all = true;
        else if (a == QLatin1String("-L") && i + 1 < c.args.size())
            st.maxDepth = qMax(1, c.args.at(++i).toInt());
        else if (a.startsWith(QLatin1String("-L")) && a.size() > 2)
            st.maxDepth = qMax(1, a.mid(2).toInt());
        else
            target = a;
    }
    const QString path = target.isEmpty() ? c.cwd() : c.resolve(target);
    if (!QFileInfo(path).isDir()) {
        c.error(QStringLiteral("tree: %1: not a directory").arg(target));
        return;
    }
    c.println(paint(QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName() + QLatin1Char('/'), Blue, true));
    walk(st, path, QString(), 1);
    QString summary = QStringLiteral("\n%1 director%2, %3 file%4").arg(st.dirs).arg(st.dirs == 1 ? QStringLiteral("y") : QStringLiteral("ies")).arg(st.files).arg(st.files == 1 ? QString() : QStringLiteral("s"));
    if (st.truncated)
        summary += QStringLiteral(" · output limited to 500 entries; use a deeper path or -L to narrow it");
    c.println(dim(summary));
}

// --- /cat and /open ----------------------------------------------------------------

bool readText(CliCall &c, const QString &path, qint64 limit, QString *text, QString *errorMessage)
{
    const QFileInfo fi(path);
    if (!fi.exists()) {
        *errorMessage = QStringLiteral("%1: No such file or directory").arg(c.host.projectRoot().isEmpty() ? path : QDir(c.cwd()).relativeFilePath(path));
        return false;
    }
    if (fi.isDir()) {
        *errorMessage = QStringLiteral("%1: is a directory (try /ls or /tree)").arg(fi.fileName());
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *errorMessage = QStringLiteral("%1: cannot be read").arg(fi.fileName());
        return false;
    }
    const QByteArray data = f.read(limit);
    if (data.left(8000).contains('\0')) {
        *errorMessage = QStringLiteral("%1: binary file").arg(fi.fileName());
        return false;
    }
    *text = QString::fromUtf8(data);
    text->remove(QLatin1Char('\r'));
    return true;
}

void cmdCat(CliCall &c)
{
    const Flags f = parseFlags(c.args);
    if (f.words.isEmpty()) {
        c.error(QStringLiteral("usage: /cat [-n] <file> [file…]"));
        return;
    }
    const bool numbers = f.has(QLatin1Char('n'));
    for (const QString &arg : f.words) {
        const QString path = c.resolve(arg);
        QString text, err;
        if (!readText(c, path, 4 * 1024 * 1024, &text, &err)) {
            c.error(QStringLiteral("cat: ") + err);
            continue;
        }
        if (f.words.size() > 1)
            c.println(paint(QStringLiteral("==> %1 <==").arg(arg), Cyan, true));
        QStringList lines = sanitize(text).split(QLatin1Char('\n'));
        if (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        const int shown = qMin(int(lines.size()), 3000);
        const int width = QString::number(shown).size();
        for (int i = 0; i < shown; ++i)
            c.println((numbers ? dim(padLeft(QString::number(i + 1), width) + QStringLiteral("  ")) : QString()) + lines.at(i));
        if (shown < lines.size())
            c.println(dim(QStringLiteral("… %1 more lines (use /edit %2 to read the rest)").arg(lines.size() - shown).arg(arg)));
        if (c.status() != 0 && f.words.size() == 1)
            return;
    }
}

// Syntax-highlights `text` with the editor's rules and returns one ANSI string per line.
QStringList highlight(const QString &text, const QString &fileName)
{
    QTextDocument doc;
    doc.setPlainText(text);
    std::unique_ptr<SyntaxHighlighter> hl;
    if (const LanguageDefinition *lang = Languages::forFile(fileName)) {
        hl = std::make_unique<SyntaxHighlighter>(&doc);
        hl->setTheme(Theme::byName(SettingsManager::instance().theme()));
        hl->setLanguage(lang);
        hl->rehighlight();
    }
    QStringList out;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const QString t = sanitize(b.text());
        QVector<QColor> colors(t.size());
        QVector<bool> bolds(t.size(), false);
        if (hl && b.layout()) {
            for (const QTextLayout::FormatRange &r : b.layout()->formats()) {
                const bool hasColor = r.format.foreground().style() != Qt::NoBrush;
                const bool isBold = r.format.fontWeight() > QFont::Normal;
                for (int i = r.start; i < r.start + r.length && i < t.size(); ++i) {
                    if (hasColor)
                        colors[i] = r.format.foreground().color();
                    bolds[i] = isBold;
                }
            }
        }
        QString line;
        int i = 0;
        while (i < t.size()) {
            int j = i;
            while (j < t.size() && colors[j] == colors[i] && bolds[j] == bolds[i])
                ++j;
            const QString part = t.mid(i, j - i);
            if (colors[i].isValid() || bolds[i])
                line += (bolds[i] ? bold() : QString()) + (colors[i].isValid() ? rgb(colors[i]) : QString()) + part + reset();
            else
                line += part;
            i = j;
        }
        out << line;
    }
    return out;
}

void cmdOpen(CliCall &c)
{
    if (c.args.isEmpty()) {
        c.error(QStringLiteral("usage: /open <file>[:line]"));
        return;
    }
    QString path;
    int target = 0;
    splitLocation(c, c.args.first(), &path, &target);
    QString text, err;
    if (!readText(c, path, 400 * 1024, &text, &err)) {
        c.error(QStringLiteral("open: ") + err + QStringLiteral(" — use /cat or /edit for large files"));
        return;
    }
    QStringList lines = highlight(text, QFileInfo(path).fileName());
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    const int total = lines.size();
    int from = 0, to = qMin(total, 500);
    if (target > 0) {
        from = qMax(0, target - 9);
        to = qMin(total, target + 20);
    }
    c.println(paint(QDir(c.cwd()).relativeFilePath(path), Cyan, true) + dim(QStringLiteral("  ·  %1 line%2").arg(total).arg(total == 1 ? QString() : QStringLiteral("s"))));
    const int width = QString::number(to).size();
    for (int i = from; i < to; ++i) {
        const bool hit = i + 1 == target;
        const QString num = padLeft(QString::number(i + 1), width);
        c.println((hit ? paint(QStringLiteral("▶ ") + num, Yellow, true) : dim(QStringLiteral("  ") + num)) + QStringLiteral("  ") + lines.at(i));
    }
    if (to < total)
        c.println(dim(QStringLiteral("… %1 more lines · /open %2:%3 shows a window, /edit %2 opens the editor")
                          .arg(total - to)
                          .arg(c.args.first().section(QLatin1Char(':'), 0, 0))
                          .arg(to + 1)));
}

void cmdEdit(CliCall &c)
{
    if (c.args.isEmpty()) {
        c.error(QStringLiteral("usage: /edit <file>[:line]"));
        return;
    }
    QString path;
    int line = 0;
    splitLocation(c, c.args.first(), &path, &line);
    QFileInfo fi(path);
    if (fi.isDir()) {
        c.error(QStringLiteral("edit: %1 is a directory").arg(c.args.first()));
        return;
    }
    if (!fi.exists()) {
        if (!fi.dir().exists()) {
            c.error(QStringLiteral("edit: the folder %1 does not exist").arg(fi.absolutePath()));
            return;
        }
        QFile created(path);
        if (!created.open(QIODevice::WriteOnly)) {
            c.error(QStringLiteral("edit: cannot create %1").arg(c.args.first()));
            return;
        }
    }
    c.host.openInEditor(path, line);
    c.host.leaveMode();
}

// --- /find and /grep -----------------------------------------------------------------

void cmdFind(CliCall &c)
{
    if (c.args.isEmpty()) {
        c.error(QStringLiteral("usage: /find <name or part of a path> [more words]"));
        return;
    }
    const QString root = c.root();
    const QStringList words = c.args;
    c.background([root, words]() -> QString {
        struct Hit { QString rel; int score; };
        QList<Hit> hits;
        for (const QString &abs : ProjectFiles::scan(root)) {
            const QString rel = abs.mid(root.size() + 1);
            const int nameStart = rel.lastIndexOf(QLatin1Char('/')) + 1;
            int total = 0;
            bool ok = true;
            for (const QString &w : words) {
                const int s = FuzzyMatcher::score(w, rel, nameStart);
                if (s < 0) {
                    ok = false;
                    break;
                }
                total += s;
            }
            if (ok)
                hits.append({rel, total});
        }
        std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score != b.score ? a.score > b.score : a.rel < b.rel; });
        if (hits.isEmpty())
            return Ansi::dim(QStringLiteral("No file matches “%1”.\n").arg(words.join(QLatin1Char(' '))));
        QString out;
        const int shown = qMin(int(hits.size()), 60);
        for (int i = 0; i < shown; ++i) {
            const QString &rel = hits.at(i).rel;
            const int slash = rel.lastIndexOf(QLatin1Char('/')) + 1;
            out += Ansi::dim(rel.left(slash)) + Ansi::paint(rel.mid(slash), Ansi::Cyan, true) + QLatin1Char('\n');
        }
        out += Ansi::dim(QStringLiteral("\n%1 match%2%3 · /edit <path> opens one\n")
                             .arg(hits.size())
                             .arg(hits.size() == 1 ? QString() : QStringLiteral("es"))
                             .arg(shown < hits.size() ? QStringLiteral(" (showing the best %1)").arg(shown) : QString()));
        return out;
    });
}

void cmdGrep(CliCall &c)
{
    QStringList flags, rest;
    for (const QString &a : c.args) {
        if (rest.isEmpty() && a.startsWith(QLatin1Char('-')) && a.size() > 1)
            flags << a;
        else
            rest << a;
    }
    if (rest.isEmpty()) {
        c.error(QStringLiteral("usage: /grep [-i] [-w] [-F] <pattern> [path…]"));
        return;
    }
    const QString pattern = rest.takeFirst();
    QStringList paths = rest.isEmpty() ? QStringList{QStringLiteral(".")} : rest;
    const bool lower = pattern == pattern.toLower();

    const QString rg = QStandardPaths::findExecutable(QStringLiteral("rg"));
    if (!rg.isEmpty()) {
        QStringList a = {QStringLiteral("--color=always"), QStringLiteral("--line-number"), QStringLiteral("--no-heading"),
                         QStringLiteral("--with-filename"), QStringLiteral("--smart-case"), QStringLiteral("--max-columns=300")};
        a += flags;
        a << QStringLiteral("-e") << pattern << QStringLiteral("--") << paths;
        c.runProcess(rg, a, c.cwd(), 400);
        return;
    }
    QProcess probe;
    probe.setWorkingDirectory(c.cwd());
    probe.start(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("--is-inside-work-tree")});
    const bool inGit = probe.waitForFinished(1500) && probe.exitCode() == 0;
    if (inGit) {
        QStringList a = {QStringLiteral("grep"), QStringLiteral("-n"), QStringLiteral("-I"), QStringLiteral("--color=always")};
        if (lower)
            a << QStringLiteral("-i");
        a += flags;
        a << QStringLiteral("-e") << pattern << QStringLiteral("--") << paths;
        c.runProcess(QStringLiteral("git"), a, c.cwd(), 400);
        return;
    }
    QStringList a = {QStringLiteral("-rIn"), QStringLiteral("--color=always"), QStringLiteral("--exclude-dir=.git"), QStringLiteral("--exclude-dir=node_modules")};
    if (lower)
        a << QStringLiteral("-i");
    a += flags;
    a << QStringLiteral("-e") << pattern << QStringLiteral("--") << paths;
    c.runProcess(QStringLiteral("grep"), a, c.cwd(), 400);
}

// --- git ----------------------------------------------------------------------------------

void cmdStatus(CliCall &c)
{
    c.runProcess(QStringLiteral("git"), {QStringLiteral("-c"), QStringLiteral("color.ui=always"), QStringLiteral("status"), QStringLiteral("-sb")}, c.cwd());
}

void cmdDiff(CliCall &c)
{
    QStringList a = {QStringLiteral("--no-pager"), QStringLiteral("-c"), QStringLiteral("color.ui=always"), QStringLiteral("diff")};
    a += c.args;
    c.runProcess(QStringLiteral("git"), a, c.cwd(), 2500);
}

void cmdLog(CliCall &c)
{
    QStringList a = {QStringLiteral("--no-pager"), QStringLiteral("-c"), QStringLiteral("color.ui=always"), QStringLiteral("log"),
                     QStringLiteral("--graph"),
                     QStringLiteral("--pretty=format:%C(yellow)%h%C(auto)%d%C(reset) %s %C(dim white)· %an, %cr%C(reset)")};
    bool ok = false;
    const int n = c.args.value(0).toInt(&ok);
    if (ok && c.args.size() == 1)
        a << QStringLiteral("-n") << QString::number(n);
    else if (c.args.isEmpty())
        a << QStringLiteral("-n") << QStringLiteral("30");
    else
        a += c.args;
    c.runProcess(QStringLiteral("git"), a, c.cwd(), 600);
}

void cmdCommit(CliCall &c)
{
    bool all = false;
    QString message;
    QStringList rest;
    for (int i = 0; i < c.args.size(); ++i) {
        const QString &a = c.args.at(i);
        if (a == QLatin1String("-a") || a == QLatin1String("--all"))
            all = true;
        else if ((a == QLatin1String("-m") || a == QLatin1String("--message")) && i + 1 < c.args.size())
            message = c.args.at(++i);
        else
            rest << a;
    }
    if (message.isEmpty())
        message = rest.join(QLatin1Char(' '));
    if (message.trimmed().isEmpty()) {
        c.error(QStringLiteral("usage: /commit [-a] <message>   (nothing is committed without a message)"));
        return;
    }
    QStringList a = {QStringLiteral("-c"), QStringLiteral("color.ui=always"), QStringLiteral("commit")};
    if (all)
        a << QStringLiteral("-a");
    a << QStringLiteral("-m") << message;
    c.runProcess(QStringLiteral("git"), a, c.cwd());
}

// --- navigation and session --------------------------------------------------------------------

void cmdPwd(CliCall &c)
{
    c.println(c.cwd());
}

void cmdCd(CliCall &c)
{
    QString target;
    if (c.args.isEmpty()) {
        target = c.root();
    } else if (c.args.first() == QLatin1String("-")) {
        target = QStringLiteral("-");
    } else {
        target = c.resolve(c.args.first());
        if (!QFileInfo(target).isDir()) {
            c.error(QStringLiteral("cd: %1: %2").arg(c.args.first(), QFileInfo::exists(target) ? QStringLiteral("not a directory") : QStringLiteral("no such directory")));
            return;
        }
    }
    c.beginAsync();
    QPointer<CliCall> self(&c);
    c.host.changeDirectory(target, [self](int status, const QString &out) {
        if (!self)
            return;
        if (status != 0 && !out.trimmed().isEmpty())
            self->error(out.trimmed());
        self->done(status);
    });
}

void cmdClear(CliCall &c)
{
    c.host.clearScreen();
}

void cmdHistory(CliCall &c)
{
    if (c.args.value(0) == QLatin1String("clear")) {
        c.host.clearHistory();
        c.println(dim(QStringLiteral("History cleared.")));
        return;
    }
    const QStringList h = c.host.history();
    int count = 20;
    if (!c.args.isEmpty()) {
        bool ok = false;
        const int n = c.args.first().toInt(&ok);
        if (ok && n > 0)
            count = n;
    }
    const int from = qMax(0, int(h.size()) - count);
    if (h.isEmpty()) {
        c.println(dim(QStringLiteral("No history yet.")));
        return;
    }
    const int width = QString::number(h.size()).size();
    for (int i = from; i < h.size(); ++i)
        c.println(dim(padLeft(QString::number(i + 1), width) + QStringLiteral("  ")) + sanitize(h.at(i)));
}

void cmdTheme(CliCall &c)
{
    const QList<ThemeManager::Info> themes = ThemeManager::instance().themes();
    auto &settings = SettingsManager::instance();
    if (c.args.isEmpty() || c.args.first() == QLatin1String("list")) {
        const QString active = settings.theme();
        for (const ThemeManager::Info &t : themes)
            c.println((t.id == active ? paint(QStringLiteral("● "), Green) : QStringLiteral("  ")) + paint(pad(t.id, 14), Cyan, true) + dim(t.name));
        c.println(dim(QStringLiteral("\n/theme <name> switches the whole application.")));
        return;
    }
    const QString wanted = c.args.join(QLatin1Char(' '));
    for (const ThemeManager::Info &t : themes) {
        if (t.id.compare(wanted, Qt::CaseInsensitive) == 0 || t.name.compare(wanted, Qt::CaseInsensitive) == 0) {
            settings.setTheme(t.id);
            c.println(QStringLiteral("Theme set to ") + paint(t.name, Cyan, true));
            return;
        }
    }
    c.error(QStringLiteral("No theme called “%1”. /theme lists them.").arg(wanted));
}

void cmdRestart(CliCall &c)
{
    c.host.restartShell();
    c.done();
}

void cmdInfo(CliCall &c)
{
    auto row = [&](const QString &k, const QString &v) { c.println(paint(pad(k, 10), Muted) + v); };
    row(QStringLiteral("Project"), paint(c.host.projectName(), Cyan, true));
    row(QStringLiteral("Root"), c.root());
    row(QStringLiteral("Directory"), c.cwd());
    row(QStringLiteral("Shell"), c.host.shellName());
}

void cmdEditor(CliCall &c)
{
    c.host.leaveMode();
    c.done();
}

// --- /help ----------------------------------------------------------------------------------------

void helpOverview(CliCall &c)
{
    const CliCommands &reg = CliCommands::instance();
    auto heading = [&](const QString &t) { c.println(QLatin1Char('\n') + paint(t, Yellow, true)); };
    auto key = [&](const QString &k, const QString &d) { c.println(QStringLiteral("  ") + paint(pad(k, 20), Cyan, true) + d); };

    c.println(paint(QStringLiteral("QODE Terminal Mode"), Blue, true) + dim(QStringLiteral("  ·  help")));
    c.println(dim(QStringLiteral("A full-window terminal for this project — no file tree, tabs or previews.")));

    heading(QStringLiteral("How it works"));
    c.println(QStringLiteral("  • Type a command and press ") + paint(QStringLiteral("Enter"), Cyan) + QStringLiteral(". It runs in your ") + c.host.shellName() +
              QStringLiteral(" in the project folder, exactly like any terminal (cd, git, make, npm, claude…)."));
    c.println(QStringLiteral("  • A line starting with ") + paint(QStringLiteral("/"), Cyan, true) +
              QStringLiteral(" is a QODE command (below). It never reaches the shell."));
    c.println(QStringLiteral("  • While a program runs, every key goes to it — so vim, less, htop or ") + paint(QStringLiteral("claude"), Cyan) +
              QStringLiteral(" work normally and may use their own / commands."));
    c.println(QStringLiteral("  • At the prompt Ctrl+C clears the line; while something runs it interrupts it."));
    c.println(QStringLiteral("  • Copied images and files can be pasted into programs that accept them (e.g. claude)."));

    const QStringList groups = {QStringLiteral("Navigate"), QStringLiteral("Files"), QStringLiteral("Git"), QStringLiteral("Session")};
    for (const QString &g : groups) {
        heading(QStringLiteral("Commands · ") + g);
        for (const CliCommand &cmd : reg.all())
            if (cmd.group == g)
                c.println(QStringLiteral("  ") + paint(pad(cmd.usage, 30), Cyan, true) + cmd.summary);
    }

    heading(QStringLiteral("Keyboard"));
    key(QStringLiteral("Enter"), QStringLiteral("run the line"));
    key(QStringLiteral("Tab"), QStringLiteral("complete commands and paths (repeat to cycle)"));
    key(QStringLiteral("Up / Down"), QStringLiteral("walk through history (saved per project)"));
    key(QStringLiteral("Ctrl+R"), QStringLiteral("search history; Enter accepts, Esc cancels"));
    key(QStringLiteral("Right / End"), QStringLiteral("accept the grey suggestion from history"));
    key(QStringLiteral("Ctrl+A / Ctrl+E"), QStringLiteral("start / end of line"));
    key(QStringLiteral("Ctrl+U / K / W"), QStringLiteral("delete to start / to end / previous word"));
    key(QStringLiteral("Alt+B / Alt+F"), QStringLiteral("move by word (Ctrl+Left / Right too)"));
    key(QStringLiteral("Ctrl+L"), QStringLiteral("clear the screen"));
    key(QStringLiteral("PageUp / PageDown"), QStringLiteral("scroll the output (or use the mouse wheel)"));
    key(QStringLiteral("Ctrl+Shift+C / V"), QStringLiteral("copy the selection / paste"));
    key(c.host.toggleShortcut(), QStringLiteral("back to the editor (same as /editor)"));

    heading(QStringLiteral("Good to know"));
    c.println(QStringLiteral("  • ") + paint(QStringLiteral("/help <command>"), Cyan) + QStringLiteral(" shows details and examples for one command."));
    c.println(QStringLiteral("  • Your shell stays alive when you switch back to the editor, and is where you left it when you return."));
    c.println(QStringLiteral("  • ") + paint(QStringLiteral("/grep"), Cyan) + QStringLiteral(" and ") + paint(QStringLiteral("/find"), Cyan) +
              QStringLiteral(" print file:line locations; ") + paint(QStringLiteral("/edit file:line"), Cyan) + QStringLiteral(" opens one in the editor."));
}

void cmdHelp(CliCall &c)
{
    if (c.args.isEmpty()) {
        helpOverview(c);
        return;
    }
    QString name = c.args.first();
    if (name.startsWith(QLatin1Char('/')))
        name.remove(0, 1);
    const CliCommand *cmd = CliCommands::instance().find(name);
    if (!cmd) {
        c.error(QStringLiteral("There is no command /%1. Type /help for the list.").arg(name));
        return;
    }
    c.println(paint(cmd->usage, Cyan, true));
    c.println(cmd->summary);
    if (!cmd->aliases.isEmpty()) {
        QStringList a;
        for (const QString &x : cmd->aliases)
            a << QLatin1Char('/') + x;
        c.println(dim(QStringLiteral("Also: ") + a.join(QStringLiteral(", "))));
    }
    if (!cmd->help.isEmpty())
        c.println(QLatin1Char('\n') + cmd->help);
}

} // namespace

void registerCliBuiltins(QList<CliCommand> &cmds)
{
    using Arg = CliCommand::Arg;
    auto add = [&](const QString &name, const QString &group, const QString &usage, const QString &summary, const QString &help,
                   std::function<void(CliCall &)> run, Arg arg = Arg::None, int argFrom = 0, const QStringList &aliases = {}) {
        CliCommand c;
        c.name = name;
        c.group = group;
        c.usage = usage;
        c.summary = summary;
        c.help = help;
        c.run = std::move(run);
        c.arg = arg;
        c.argFrom = argFrom;
        c.aliases = aliases;
        cmds.append(c);
    };
    const QString nav = QStringLiteral("Navigate"), files = QStringLiteral("Files"), git = QStringLiteral("Git"), session = QStringLiteral("Session");

    add(QStringLiteral("ls"), nav, QStringLiteral("/ls [-a] [-l] [path]"), QStringLiteral("list a folder (default: the current one)"),
        QStringLiteral("  -a   include hidden files\n  -l   long format: permissions, owner, size, date\n\nFolders are blue, executables green, links cyan.\nExample: /ls -la src"),
        cmdLs, Arg::Path);
    add(QStringLiteral("cd"), nav, QStringLiteral("/cd [path | -]"), QStringLiteral("change directory (no argument: project root)"),
        QStringLiteral("Changes the directory of your shell, so later commands run there.\n  /cd          back to the project root\n  /cd -        the previous directory\n  /cd ~/Music  any folder, also outside the project"),
        cmdCd, Arg::Directory);
    add(QStringLiteral("pwd"), nav, QStringLiteral("/pwd"), QStringLiteral("print the current directory"), {}, cmdPwd);
    add(QStringLiteral("tree"), nav, QStringLiteral("/tree [-a] [-L depth] [path]"), QStringLiteral("show a folder as a tree (depth 3 by default)"),
        QStringLiteral("  -a        include hidden files and clutter such as .git and node_modules\n  -L depth  how many levels to descend\n\nOutput is limited to 500 entries.\nExample: /tree -L 2 src"),
        cmdTree, Arg::Directory);
    add(QStringLiteral("cat"), files, QStringLiteral("/cat [-n] <file>…"), QStringLiteral("print files as plain text"),
        QStringLiteral("  -n   number the lines\n\nBinary files are refused and control characters are shown as symbols, so a file can never garble the terminal."),
        cmdCat, Arg::Path);
    add(QStringLiteral("open"), files, QStringLiteral("/open <file>[:line]"), QStringLiteral("print a file with syntax colours"),
        QStringLiteral("Shows up to 500 lines with line numbers. With :line it shows a window around that line and marks it.\nExample: /open src/main.cpp:42"),
        cmdOpen, Arg::Path);
    add(QStringLiteral("edit"), files, QStringLiteral("/edit <file>[:line]"), QStringLiteral("open a file in the editor (leaves terminal mode)"),
        QStringLiteral("Switches back to the normal editor with the file open, at the line if you give one.\nA file that does not exist yet is created empty.\nExample: /edit src/main.cpp:42"),
        cmdEdit, Arg::Path);
    add(QStringLiteral("find"), files, QStringLiteral("/find <words>"), QStringLiteral("find project files by name (fuzzy)"),
        QStringLiteral("Every word has to match somewhere in the path; the best matches come first. Respects .gitignore.\nExample: /find term view"),
        cmdFind);
    add(QStringLiteral("grep"), files, QStringLiteral("/grep [-i|-w|-F] <pattern> [path…]"), QStringLiteral("search file contents"),
        QStringLiteral("Uses ripgrep when installed, otherwise git grep, otherwise grep. Smart case: a lower-case pattern ignores case.\nResults are file:line:text; use /edit file:line to jump to one.\nExample: /grep -w TODO src"),
        cmdGrep, Arg::Path, 1);
    add(QStringLiteral("status"), git, QStringLiteral("/status"), QStringLiteral("git status (short form with branch)"), {}, cmdStatus);
    add(QStringLiteral("diff"), git, QStringLiteral("/diff [git diff options]"), QStringLiteral("show uncommitted changes"),
        QStringLiteral("Options go straight to git diff.\nExamples: /diff   /diff --staged   /diff HEAD~1 -- src"), cmdDiff, Arg::Path);
    add(QStringLiteral("log"), git, QStringLiteral("/log [n | git log options]"), QStringLiteral("recent commits as a graph (30 by default)"),
        QStringLiteral("Examples: /log   /log 100   /log --author=me src"), cmdLog, Arg::Path, 1);
    add(QStringLiteral("commit"), git, QStringLiteral("/commit [-a] <message>"), QStringLiteral("commit the staged changes"),
        QStringLiteral("  -a   stage all tracked modified files first\n\nThe message is the rest of the line; quotes are optional.\nExample: /commit -a fix the flickering cursor"),
        cmdCommit);
    add(QStringLiteral("clear"), session, QStringLiteral("/clear"), QStringLiteral("clear the screen (Ctrl+L)"), {}, cmdClear);
    add(QStringLiteral("history"), session, QStringLiteral("/history [n | clear]"), QStringLiteral("show or clear the command history"),
        QStringLiteral("History is saved per project and shared by shell and / commands."), cmdHistory);
    add(QStringLiteral("theme"), session, QStringLiteral("/theme [name]"), QStringLiteral("list themes or switch the application theme"),
        QStringLiteral("The theme applies to the whole of QODE, not just this screen."), cmdTheme, Arg::Theme);
    add(QStringLiteral("info"), session, QStringLiteral("/info"), QStringLiteral("project, directory and shell in use"), {}, cmdInfo);
    add(QStringLiteral("restart"), session, QStringLiteral("/restart"), QStringLiteral("start a fresh shell (resets cd, variables, jobs)"), {}, cmdRestart);
    add(QStringLiteral("editor"), session, QStringLiteral("/editor"), QStringLiteral("return to the normal editor"),
        QStringLiteral("The shell keeps running; coming back to terminal mode resumes it."), cmdEditor, Arg::None, 0,
        {QStringLiteral("exit"), QStringLiteral("quit")});
    add(QStringLiteral("help"), session, QStringLiteral("/help [command]"), QStringLiteral("how this mode works, all commands and keys"),
        QStringLiteral("Without an argument: an overview of the mode, every command and the keyboard shortcuts.\nWith a command name: its usage and examples."),
        cmdHelp, Arg::Command, 0, {QStringLiteral("?")});
}
