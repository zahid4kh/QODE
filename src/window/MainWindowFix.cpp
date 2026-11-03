// "Fix with QODE": a failed terminal command (a test run from a ▶ lens included) or errors in Tasks > Problems start a
// QODE chat with the error quoted, the project files it names pointed at, and the lines around each error spot sent
// along so the model can start on the fix without reading them first.
#include "MainWindow.h"

#include "ai/AiEnv.h"
#include "ai/AiHub.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "project/ProjectFiles.h"
#include "project/ProjectManager.h"
#include "tasks/TasksPanel.h"
#include "terminal/Terminal.h"
#include "terminal/TerminalPanel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace {
constexpr int kMaxFiles = 5;       // files pointed at
constexpr int kContext = 8;        // lines above and below an error spot
constexpr int kMaxExcerpt = 4000;  // characters per file
constexpr int kMaxProblems = 30;   // listed in the quote

// Folders whose files are not the project's own code (dependencies, build output).
bool generated(const QString &rel)
{
    static const QRegularExpression re(QStringLiteral("(^|/)(node_modules|build|dist|target|out|\\.gradle|\\.git|\\.venv|venv|site-packages|__pycache__|\\.next)/"));
    return re.match(rel).hasMatch();
}
} // namespace

void MainWindow::createFixActions()
{
    connect(m_terminal, &TerminalPanel::fixRequested, this, &MainWindow::fixFailedCommand);
    connect(m_tasks, &TasksPanel::fixRequested, this, &MainWindow::fixProblems);
}

// The project files `text` names, with the first line it names in each, in the order they come up: path:line[:col]
// (absolute, or relative to `folder` or the project), Python's File "…", line N, and JVM stack frames (Name.kt:12)
// matched by file name when only one project file has it.
QList<QPair<QString, int>> MainWindow::fixLocations(const QString &text, const QString &folder) const
{
    QList<QPair<QString, int>> out;
    const QString root = m_projects->hasProject() ? m_projects->root() : QString();
    if (root.isEmpty())
        return out;
    QHash<QString, QString> byName; // file name -> path ("" when several files have it)
    bool namesBuilt = false;
    auto add = [&](QString path, int line) {
        if (path.startsWith(QLatin1String("file://")))
            path = path.mid(7);
        QString abs;
        if (QFileInfo(path).isAbsolute()) {
            abs = path;
        } else if (!path.contains(QLatin1Char('/'))) {
            if (!namesBuilt) {
                for (const QString &f : m_projectFiles->files()) {
                    const QString name = QFileInfo(f).fileName();
                    byName.insert(name, byName.contains(name) ? QString() : f);
                }
                namesBuilt = true;
            }
            abs = QFileInfo::exists(QDir(folder).filePath(path)) ? QDir(folder).filePath(path) : byName.value(path);
        } else {
            for (const QString &base : {folder, root})
                if (!base.isEmpty() && QFileInfo::exists(QDir(base).filePath(path))) {
                    abs = QDir(base).filePath(path);
                    break;
                }
        }
        if (abs.isEmpty())
            return;
        abs = QDir::cleanPath(abs);
        if (!abs.startsWith(root + QLatin1Char('/')) || !QFileInfo(abs).isFile() || AiEnv::isSensitive(abs) || generated(abs.mid(root.size() + 1)))
            return;
        for (const auto &l : std::as_const(out))
            if (l.first == abs)
                return;
        out.append({abs, line});
    };
    static const QRegularExpression location(QStringLiteral("((?:file://)?(?:~|\\.{1,2})?[\\w./+@-]*[\\w+-]\\.[A-Za-z0-9+]{1,8}):(\\d+)"));
    static const QRegularExpression python(QStringLiteral("File \"([^\"]+)\", line (\\d+)"));
    static const QRegularExpression frame(QStringLiteral("\\(([\\w$]+\\.(?:java|kt|kts|scala|groovy)):(\\d+)\\)"));
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        // In the order of the line: whichever pattern matched earliest goes first.
        QList<QPair<int, QPair<QString, int>>> hits;
        for (const QRegularExpression *re : {&python, &frame, &location}) {
            auto it = re->globalMatch(line);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                hits.append({int(m.capturedStart()), {m.captured(1), m.captured(2).toInt()}});
            }
        }
        std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &h : std::as_const(hits))
            add(h.second.first, h.second.second);
        if (out.size() >= kMaxFiles)
            break;
    }
    return out;
}

// The lines of `path` around `lines` (1-based; 0 = none), numbered, in a code block headed by what it covers, from the
// editor's copy when the file is open.
QString MainWindow::fixExcerpt(const QString &path, const QList<int> &lines) const
{
    QString text;
    if (Document *d = m_editors->documentForPath(path)) {
        text = d->text();
    } else {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly) || f.size() > 2 * 1024 * 1024)
            return {};
        text = QString::fromUtf8(f.readAll());
    }
    const QStringList all = text.split(QLatin1Char('\n'));
    QList<QPair<int, int>> ranges; // 0-based, inclusive, merged
    QList<int> sorted = lines;
    std::sort(sorted.begin(), sorted.end());
    for (int l : std::as_const(sorted)) {
        if (l < 1 || l > all.size())
            continue;
        const int from = qMax(0, l - 1 - kContext), to = qMin(int(all.size()) - 1, l - 1 + kContext);
        if (!ranges.isEmpty() && from <= ranges.last().second + 1)
            ranges.last().second = qMax(ranges.last().second, to);
        else
            ranges.append({from, to});
    }
    if (ranges.isEmpty())
        return {};
    // What the excerpt covers, so the model knows when it has the whole file.
    int last = int(all.size());
    while (last > 0 && all.at(last - 1).trimmed().isEmpty())
        --last;
    QStringList spans;
    for (const auto &[from, to] : std::as_const(ranges))
        spans << QStringLiteral("%1-%2").arg(from + 1).arg(to + 1);
    QString out = ranges.size() == 1 && ranges.first().first == 0 && ranges.first().second + 1 >= last
                      ? QStringLiteral("the whole file\n")
                      : QStringLiteral("lines %1 of %2\n").arg(spans.join(QStringLiteral(", "))).arg(all.size());
    out += QStringLiteral("```\n");
    const int width = int(QString::number(all.size()).size());
    for (const auto &[from, to] : std::as_const(ranges)) {
        if (!out.endsWith(QLatin1String("```\n")))
            out += QStringLiteral("   …\n");
        for (int i = from; i <= to; ++i)
            out += QStringLiteral("%1| %2\n").arg(i + 1, width).arg(all.at(i));
        if (out.size() > kMaxExcerpt)
            return out.left(kMaxExcerpt) + QStringLiteral("…\n```");
    }
    return out + QStringLiteral("```");
}

// The chat: `files` are pointed at (chips in the bubble) and those with a line get their excerpt in the request.
void MainWindow::startFixChat(const QString &display, const QString &intro, const QString &quote, const QString &quoteLead,
                              const QList<QPair<QString, int>> &spots)
{
    QStringList files;
    QHash<QString, QList<int>> lines;
    for (const auto &[path, line] : spots) {
        const QString rel = ideRelative(path);
        if (!files.contains(rel)) {
            if (files.size() >= kMaxFiles)
                continue;
            files << rel;
        }
        if (line > 0 && lines.value(rel).size() < 4)
            lines[rel] << line;
    }
    QString request = intro;
    QStringList excerpts;
    for (const QString &rel : std::as_const(files)) {
        if (!lines.contains(rel))
            continue;
        const QString text = fixExcerpt(QDir(m_projects->root()).filePath(rel), lines.value(rel));
        if (!text.isEmpty())
            excerpts << QStringLiteral("%1, %2").arg(rel, text);
    }
    if (!excerpts.isEmpty())
        request += QStringLiteral("\n\nThe files' current text around the error spots (no need to read these lines again):\n\n") + excerpts.join(QStringLiteral("\n\n"));
    setAgentVisible(true);
    m_ai->startFix(display, request, files, quote, quoteLead);
}

void MainWindow::fixFailedCommand(const FailedCommand &failure)
{
    if (!m_projects->hasProject())
        return;
    const QString root = m_projects->root();
    // A test run from a ▶ lens: its test file goes first, and the request says it is about a failing test.
    const bool test = !m_lastTestClass.isEmpty() && failure.command.contains(m_lastTestClass);
    QList<QPair<QString, int>> spots;
    if (test && QFileInfo::exists(m_lastTestFile))
        spots.append({m_lastTestFile, 0});
    for (const BuildProblem &p : failure.problems)
        if (p.path.startsWith(root + QLatin1Char('/')) && !AiEnv::isSensitive(p.path))
            spots.append({p.path, p.line});
    for (const auto &l : fixLocations(failure.output, failure.folder)) {
        bool known = false;
        for (auto &s : spots)
            if (s.first == l.first) {
                if (s.second == 0)
                    s.second = l.second;
                known = true;
            }
        if (!known)
            spots.append(l);
    }

    const QString command = failure.command.isEmpty() ? tr("a command") : failure.command;
    const QString where = failure.folder == root || failure.folder.isEmpty() ? tr("the project folder") : ideRelative(failure.folder);
    QString display, intro;
    if (test) {
        const QString name = m_lastTestClass.section(QLatin1Char('.'), -1);
        display = tr("Fix the failing test in %1.").arg(name);
        intro = tr("A test run in the terminal failed (exit code %1, run in %2). The output is quoted above; the test file is %3. "
                   "Find out why the test fails. Fix the code under test when it is wrong; change the test only when the test itself is wrong, and say which it was. "
                   "Then run the test again to check.")
                    .arg(failure.exitCode).arg(where, ideRelative(m_lastTestFile));
    } else {
        display = tr("Fix this: %1 failed (exit code %2).").arg(command.left(80)).arg(failure.exitCode);
        intro = tr("This command failed in the terminal (exit code %1, run in %2): %3\n"
                   "Its output is quoted above. Find the cause and fix it, then run the command again (or a quicker one that checks the same thing) to make sure it works.")
                    .arg(failure.exitCode).arg(where, command);
    }
    const QString quote = QStringLiteral("$ %1\n%2").arg(failure.command, failure.output);
    startFixChat(display, intro, quote, QStringLiteral("The command and its output in the terminal:"), spots);
}

void MainWindow::fixProblems(const QList<BuildProblem> &problems)
{
    if (!m_projects->hasProject() || problems.isEmpty())
        return;
    QStringList listed;
    QList<QPair<QString, int>> spots;
    int errors = 0;
    for (const BuildProblem &p : problems) {
        errors += p.error ? 1 : 0;
        if (listed.size() < kMaxProblems) {
            QString at = ideRelative(p.path);
            if (p.line > 0)
                at += QStringLiteral(":%1").arg(p.line) + (p.column > 0 ? QStringLiteral(":%1").arg(p.column) : QString());
            listed << QStringLiteral("%1: %2: %3").arg(at, p.error ? QStringLiteral("error") : QStringLiteral("warning"), p.message);
        }
        if (!AiEnv::isSensitive(p.path))
            spots.append({p.path, p.line});
    }
    if (problems.size() > listed.size())
        listed << tr("… and %n more", nullptr, int(problems.size() - listed.size()));

    const BuildProblem &first = problems.first();
    const QString kind = errors > 0 ? (problems.size() == 1 ? tr("error") : tr("errors")) : (problems.size() == 1 ? tr("warning") : tr("warnings"));
    const QString display = problems.size() == 1
                                ? tr("Fix this %1 in %2.").arg(kind, QFileInfo(first.path).fileName() + (first.line > 0 ? QStringLiteral(":%1").arg(first.line) : QString()))
                                : tr("Fix these %1 %2.").arg(problems.size()).arg(kind);
    const QString intro = problems.size() == 1
                              ? tr("The compiler reported the %1 quoted above when the user built the project in the terminal. "
                                   "Fix its cause, then build again to check that it is gone.").arg(kind)
                              : tr("The compiler reported the %1 quoted above when the user built the project in the terminal. "
                                   "Fix their cause (one mistake often produces several), then build again to check that they are gone.").arg(kind);
    startFixChat(display, intro, listed.join(QLatin1Char('\n')), QStringLiteral("From Tasks > Problems:"), spots);
}
