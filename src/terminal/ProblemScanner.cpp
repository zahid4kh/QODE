#include "ProblemScanner.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace {
constexpr int kMaxLine = 8192;   // a longer line without a break is not compiler output
constexpr int kMaxProblems = 2000;

// CSI / OSC / two-character escapes (gcc colours its output in a terminal).
QString stripEscapes(const QString &s)
{
    static const QRegularExpression esc(QStringLiteral("\x1b(?:\\[[0-?]*[ -/]*[@-~]|\\][^\x07\x1b]*(?:\x07|\x1b\\\\)?|[()][A-Za-z0-9]|[=>78DEHM])"));
    return s.contains(QLatin1Char('\x1b')) ? QString(s).remove(esc) : s;
}
}

void ProblemScanner::reset()
{
    m_partial.clear();
    m_makeDir.clear();
    m_rustMessage.clear();
    m_cmakePending = false;
    m_seen.clear();
}

QList<BuildProblem> ProblemScanner::feed(const QByteArray &data)
{
    QList<BuildProblem> out;
    m_partial += m_decoder.decode(data);
    int from = 0;
    for (int nl; (nl = m_partial.indexOf(QLatin1Char('\n'), from)) != -1; from = nl + 1) {
        QString line = m_partial.mid(from, nl - from);
        while (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        // A carriage return in the middle redraws the line (progress output): what is left is the text after it.
        if (const int cr = line.lastIndexOf(QLatin1Char('\r')); cr >= 0)
            line = line.mid(cr + 1);
        scanLine(stripEscapes(line), &out);
    }
    m_partial.remove(0, from);
    if (m_partial.size() > kMaxLine)
        m_partial.clear();
    return out;
}

void ProblemScanner::scanLine(const QString &line, QList<BuildProblem> *out)
{
    static const QRegularExpression entering(QStringLiteral("Entering directory [`'\"](.+)['\"]"));
    static const QRegularExpression gcc(
        QStringLiteral("^\\s*([^\\s:][^:]*?):(\\d+):(?:(\\d+):)?\\s*(fatal error|error|warning)(?:\\s*\\[[^\\]]*\\])?\\s*:\\s*(.*)$"));
    static const QRegularExpression rustHead(QStringLiteral("^(error|warning)(?:\\[\\w+\\])?: (.+)$"));
    static const QRegularExpression rustAt(QStringLiteral("^\\s*--> (.+?):(\\d+):(\\d+)\\s*$"));
    static const QRegularExpression ld(
        QStringLiteral("(?:^|: )([^\\s:()]+):\\(\\.[\\w.$]+\\+0x[0-9a-fA-F]+\\): ((?:undefined reference to|multiple definition of) .*)$"));
    static const QRegularExpression cmake(QStringLiteral("^CMake (Error|Warning)(?: \\(dev\\))? at (.+?):(\\d+) \\((\\w+)\\):"));
    // Kotlin through Gradle / Maven: "e: file:///abs/Main.kt:12:5 Unresolved reference 'x'." (also "w:", older builds
    // without the file:// prefix or with (12, 5)); Maven's javac: "[ERROR] /abs/Main.java:[12,5] cannot find symbol".
    static const QRegularExpression kotlinc(QStringLiteral("^([ew]): (?:file://)?(/[^:]+?\\.kts?)(?::(\\d+):(\\d+)|: \\((\\d+), (\\d+)\\):?) (.+)$"));
    static const QRegularExpression maven(QStringLiteral("^\\[(ERROR|WARNING)\\] (/[^:\\[]+?\\.(?:java|kt|kts|groovy|scala)):\\[(\\d+),(\\d+)\\] (.+)$"));

    if (m_cmakePending) {
        const QString text = line.trimmed();
        if (!text.isEmpty()) {
            m_cmakePending = false;
            m_cmake.message = text;
            add(m_cmake, out);
            return;
        }
    }
    if (line.size() < 6 || line.size() > kMaxLine)
        return;
    if (line.startsWith(QLatin1String("e: ")) || line.startsWith(QLatin1String("w: "))) {
        if (const QRegularExpressionMatch m = kotlinc.match(line); m.hasMatch()) {
            BuildProblem p;
            p.path = m.captured(2);
            p.line = (m.captured(3).isEmpty() ? m.captured(5) : m.captured(3)).toInt();
            p.column = (m.captured(4).isEmpty() ? m.captured(6) : m.captured(4)).toInt();
            p.error = m.captured(1) == QLatin1String("e");
            p.message = m.captured(7).trimmed();
            add(p, out);
            return;
        }
    }
    if (line.startsWith(QLatin1Char('['))) {
        if (const QRegularExpressionMatch m = maven.match(line); m.hasMatch()) {
            BuildProblem p;
            p.path = m.captured(2);
            p.line = m.captured(3).toInt();
            p.column = m.captured(4).toInt();
            p.error = m.captured(1) == QLatin1String("ERROR");
            p.message = m.captured(5).trimmed();
            add(p, out);
            return;
        }
    }
    if (line.contains(QLatin1String("ing directory"))) {
        if (const QRegularExpressionMatch m = entering.match(line); m.hasMatch()) {
            m_makeDir = m.captured(1);
            return;
        }
        if (line.contains(QLatin1String("Leaving directory"))) {
            m_makeDir.clear();
            return;
        }
    }
    if (!m_rustMessage.isEmpty()) {
        if (const QRegularExpressionMatch m = rustAt.match(line); m.hasMatch()) {
            BuildProblem p;
            p.path = m.captured(1);
            p.line = m.captured(2).toInt();
            p.column = m.captured(3).toInt();
            p.error = m_rustError;
            p.message = m_rustMessage;
            m_rustMessage.clear();
            add(p, out);
            return;
        }
        m_rustMessage.clear(); // the location comes on the very next line
    }
    if (line.contains(QLatin1String("error")) || line.contains(QLatin1String("warning"))) {
        if (const QRegularExpressionMatch m = gcc.match(line); m.hasMatch()) {
            BuildProblem p;
            p.path = m.captured(1);
            p.line = m.captured(2).toInt();
            p.column = m.captured(3).toInt();
            p.error = m.captured(4) != QLatin1String("warning");
            p.message = m.captured(5).trimmed();
            add(p, out);
            return;
        }
        if (const QRegularExpressionMatch m = rustHead.match(line); m.hasMatch()) {
            m_rustError = m.captured(1) == QLatin1String("error");
            m_rustMessage = m.captured(2).trimmed();
            return;
        }
    }
    if (line.contains(QLatin1String("+0x"))) {
        if (const QRegularExpressionMatch m = ld.match(line); m.hasMatch()) {
            BuildProblem p;
            p.path = m.captured(1);
            p.message = m.captured(2).trimmed();
            add(p, out);
            return;
        }
    }
    if (line.startsWith(QLatin1String("CMake "))) {
        if (const QRegularExpressionMatch m = cmake.match(line); m.hasMatch()) {
            m_cmake = BuildProblem();
            m_cmake.path = m.captured(2);
            m_cmake.line = m.captured(3).toInt();
            m_cmake.error = m.captured(1) == QLatin1String("Error");
            m_cmake.message = m.captured(4) + QStringLiteral("()");
            m_cmakePending = true;
        }
    }
}

void ProblemScanner::add(BuildProblem p, QList<BuildProblem> *out)
{
    if (m_seen.size() >= kMaxProblems)
        return;
    p.path = resolve(p.path);
    if (p.path.isEmpty() || p.message.isEmpty())
        return;
    const QString key = p.path + QLatin1Char(':') + QString::number(p.line) + QLatin1Char(':') + QString::number(p.column) +
                        QLatin1Char(':') + p.message;
    if (m_seen.contains(key))
        return;
    m_seen.insert(key);
    out->append(p);
}

QString ProblemScanner::resolve(const QString &raw) const
{
    QString path = raw.trimmed();
    if (path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    if (QFileInfo(path).isAbsolute())
        return QFileInfo(path).isFile() ? QDir::cleanPath(path) : QString();
    QStringList folders;
    if (!m_makeDir.isEmpty())
        folders << m_makeDir;
    if (m_folders)
        folders += m_folders();
    for (const QString &dir : std::as_const(folders)) {
        const QFileInfo fi(QDir(dir).filePath(path));
        if (fi.isFile())
            return QDir::cleanPath(fi.absoluteFilePath());
    }
    return {};
}
