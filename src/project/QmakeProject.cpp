#include "QmakeProject.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

namespace {

QString shellWord(const QString &s)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_./+=:@%-]+$"));
    if (plain.match(s).hasMatch())
        return s;
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString findQmake()
{
    for (const char *name : {"qmake6", "qmake-qt6", "qmake"}) {
        const QString p = QStandardPaths::findExecutable(QLatin1String(name));
        if (!p.isEmpty())
            return p;
    }
    return {};
}

// Output of a quick helper process, "" when it fails. Never blocks for long.
QString runTool(const QString &program, const QStringList &args)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(3000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (p.state() != QProcess::NotRunning)
            p.kill();
        return {};
    }
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

// Where the Qt headers live ("" when qmake isn't installed), and the Qt major version.
struct QtInfo {
    QString headers;
    int major = 0;
};
const QtInfo &qtInfo()
{
    static const QtInfo info = [] {
        QtInfo i;
        const QString qmake = findQmake();
        if (!qmake.isEmpty()) {
            i.headers = runTool(qmake, {QStringLiteral("-query"), QStringLiteral("QT_INSTALL_HEADERS")});
            i.major = runTool(qmake, {QStringLiteral("-query"), QStringLiteral("QT_VERSION")}).section(QLatin1Char('.'), 0, 0).toInt();
        }
        return i;
    }();
    return info;
}

QString moduleDirName(const QString &module)
{
    static const QHash<QString, QString> special = {
        {QStringLiteral("testlib"), QStringLiteral("QtTest")},
        {QStringLiteral("dbus"), QStringLiteral("QtDBus")},
        {QStringLiteral("opengl"), QStringLiteral("QtOpenGL")},
        {QStringLiteral("openglwidgets"), QStringLiteral("QtOpenGLWidgets")},
        {QStringLiteral("printsupport"), QStringLiteral("QtPrintSupport")},
        {QStringLiteral("svgwidgets"), QStringLiteral("QtSvgWidgets")},
        {QStringLiteral("core5compat"), QStringLiteral("QtCore5Compat")},
        {QStringLiteral("uitools"), QStringLiteral("QtUiTools")},
        {QStringLiteral("quickwidgets"), QStringLiteral("QtQuickWidgets")},
        {QStringLiteral("multimediawidgets"), QStringLiteral("QtMultimediaWidgets")},
        {QStringLiteral("websockets"), QStringLiteral("QtWebSockets")},
        {QStringLiteral("serialport"), QStringLiteral("QtSerialPort")},
        {QStringLiteral("statemachine"), QStringLiteral("QtStateMachine")},
    };
    const auto it = special.constFind(module);
    if (it != special.constEnd())
        return it.value();
    return QStringLiteral("Qt") + module.left(1).toUpper() + module.mid(1);
}

} // namespace

// Parses one .pro / .pri file into variables. Conditions are evaluated for Linux with the installed Qt.
class QmakeParser
{
public:
    using Vars = QHash<QString, QStringList>;

    QmakeParser(const QString &topFile) : m_topDir(QFileInfo(topFile).absolutePath()) {}

    void parse(const QString &file, Vars &vars, int depth = 0)
    {
        QFile f(file);
        if (depth > 6 || !f.open(QIODevice::ReadOnly | QIODevice::Text))
            return;
        const QString dir = QFileInfo(file).absolutePath();
        const QStringList lines = joinLines(QTextStream(&f).readAll());

        struct Block {
            bool active; // this block's own condition
            bool taken;  // for a following "else"
        };
        QList<Block> stack;
        auto allActive = [&stack] {
            for (const Block &b : stack)
                if (!b.active)
                    return false;
            return true;
        };
        bool lastCondition = false; // result of the block that just closed, for "else"

        static const QRegularExpression assign(QStringLiteral(R"(^([A-Za-z_][\w.]*)\s*(\+=|-=|\*=|~=|=)\s*(.*)$)"));
        static const QRegularExpression scoped(QStringLiteral(R"(^(.*?):\s*([A-Za-z_][\w.]*\s*(?:\+=|-=|\*=|~=|=).*)$)"));
        static const QRegularExpression includeCall(QStringLiteral(R"(^include\s*\(\s*(.+?)\s*\)\s*$)"));

        for (QString line : lines) {
            // "} else {" and "}" close blocks; the rest of the line is processed after.
            while (line.startsWith(QLatin1Char('}'))) {
                if (!stack.isEmpty()) {
                    lastCondition = stack.last().taken;
                    stack.removeLast();
                }
                line = line.mid(1).trimmed();
            }
            if (line.isEmpty())
                continue;
            if (line.endsWith(QLatin1Char('{'))) {
                QString header = line.left(line.size() - 1).trimmed();
                bool cond;
                if (header == QLatin1String("else"))
                    cond = !lastCondition;
                else if (header.startsWith(QLatin1String("else")) && header.mid(4).trimmed().startsWith(QLatin1Char(':')))
                    cond = !lastCondition && evalCondition(header.mid(4).trimmed().mid(1).trimmed(), vars);
                else
                    cond = evalCondition(header, vars);
                stack.append({cond, cond});
                continue;
            }
            if (!allActive())
                continue;
            QRegularExpressionMatch m = assign.match(line);
            if (!m.hasMatch()) {
                const QRegularExpressionMatch ic = includeCall.match(line);
                if (ic.hasMatch()) {
                    QString inc = expand(ic.captured(1), vars, dir);
                    inc.remove(QLatin1Char('"'));
                    parse(QDir::isAbsolutePath(inc) ? inc : dir + QLatin1Char('/') + inc, vars, depth + 1);
                    continue;
                }
                const QRegularExpressionMatch sc = scoped.match(line);
                if (!sc.hasMatch() || !evalCondition(sc.captured(1), vars))
                    continue;
                m = assign.match(sc.captured(2));
                if (!m.hasMatch())
                    continue;
            }
            apply(vars, m.captured(1), m.captured(2), expand(m.captured(3), vars, dir));
        }
    }

private:
    static QStringList joinLines(const QString &text)
    {
        QStringList out;
        QString cur;
        for (QString line : text.split(QLatin1Char('\n'))) {
            // Strip a comment (a '#' outside double quotes).
            bool quoted = false;
            for (int i = 0; i < line.size(); ++i) {
                if (line.at(i) == QLatin1Char('"'))
                    quoted = !quoted;
                else if (line.at(i) == QLatin1Char('#') && !quoted) {
                    line.truncate(i);
                    break;
                }
            }
            line = line.trimmed();
            if (line.endsWith(QLatin1Char('\\'))) {
                cur += line.chopped(1) + QLatin1Char(' ');
                continue;
            }
            cur += line;
            out << cur.trimmed();
            cur.clear();
        }
        if (!cur.isEmpty())
            out << cur.trimmed();
        return out;
    }

    QString expand(QString value, const Vars &vars, const QString &dir) const
    {
        static const QRegularExpression ref(QStringLiteral(R"(\$\$(?:\{([\w.]+)\}|([\w.]+)))"));
        for (int guard = 0; guard < 4; ++guard) {
            QString out;
            int last = 0;
            bool changed = false;
            auto it = ref.globalMatch(value);
            while (it.hasNext()) {
                const auto m = it.next();
                const QString name = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
                QString repl;
                if (name == QLatin1String("PWD"))
                    repl = dir;
                else if (name == QLatin1String("_PRO_FILE_PWD_"))
                    repl = m_topDir;
                else if (name == QLatin1String("QT_MAJOR_VERSION"))
                    repl = QString::number(qtInfo().major ? qtInfo().major : 6);
                else
                    repl = vars.value(name).join(QLatin1Char(' '));
                out += value.mid(last, m.capturedStart() - last) + repl;
                last = m.capturedEnd();
                changed = true;
            }
            if (!changed)
                break;
            value = out + value.mid(last);
        }
        value.remove(QRegularExpression(QStringLiteral(R"(\$\$\[[^\]]*\])"))); // $$[QT_INSTALL_...]
        return value;
    }

    static void apply(Vars &vars, const QString &name, const QString &op, const QString &value)
    {
        // qmake writes a quote that must reach the compiler as \\\" ; keep it literal through the word splitting.
        QString text = value;
        text.replace(QRegularExpression(QStringLiteral(R"(\\+")")), QString(QChar(1)));
        QStringList items = QProcess::splitCommand(text);
        for (QString &i : items)
            i.replace(QChar(1), QLatin1Char('"'));
        QStringList &list = vars[name];
        if (op == QLatin1String("=")) {
            list = items;
        } else if (op == QLatin1String("+=")) {
            list += items;
        } else if (op == QLatin1String("*=")) {
            for (const QString &i : items)
                if (!list.contains(i))
                    list << i;
        } else if (op == QLatin1String("-=")) {
            for (const QString &i : items)
                list.removeAll(i);
        }
    }

    // Conditions are evaluated for Linux with GCC and a release build: platform names, a few functions
    // (greaterThan(QT_MAJOR_VERSION, N), ...), '!' negation, ':' (and) and '|' (or). Anything else is false.
    bool evalCondition(const QString &cond, const Vars &vars) const
    {
        for (const QString &orPart : splitTop(cond, QLatin1Char('|'))) {
            bool all = true;
            for (QString term : splitTop(orPart, QLatin1Char(':'))) {
                term = term.trimmed();
                bool negate = false;
                while (term.startsWith(QLatin1Char('!'))) {
                    negate = !negate;
                    term = term.mid(1).trimmed();
                }
                all = all && (evalTerm(term, vars) != negate);
            }
            if (all)
                return true;
        }
        return false;
    }

    static QStringList splitTop(const QString &s, QChar sep)
    {
        QStringList out;
        int depth = 0, start = 0;
        for (int i = 0; i < s.size(); ++i) {
            if (s.at(i) == QLatin1Char('('))
                ++depth;
            else if (s.at(i) == QLatin1Char(')'))
                --depth;
            else if (s.at(i) == sep && depth == 0) {
                out << s.mid(start, i - start);
                start = i + 1;
            }
        }
        out << s.mid(start);
        return out;
    }

    bool evalTerm(const QString &term, const Vars &vars) const
    {
        static const QSet<QString> yes = {QStringLiteral("unix"), QStringLiteral("linux"), QStringLiteral("linux-g++"),
                                          QStringLiteral("linux-g++-64"), QStringLiteral("release"), QStringLiteral("gcc"),
                                          QStringLiteral("posix"), QStringLiteral("true")};
        if (yes.contains(term))
            return true;
        static const QRegularExpression fn(QStringLiteral(R"(^(\w+)\s*\((.*)\)$)"));
        const auto m = fn.match(term);
        if (!m.hasMatch())
            return false;
        const QString name = m.captured(1);
        const QStringList args = splitTop(m.captured(2), QLatin1Char(','));
        auto arg = [&](int i) { return i < args.size() ? args.at(i).trimmed() : QString(); };
        if (name == QLatin1String("greaterThan") || name == QLatin1String("lessThan") || name == QLatin1String("equals")) {
            const QString var = arg(0);
            const QString left = var == QLatin1String("QT_MAJOR_VERSION") ? QString::number(qtInfo().major ? qtInfo().major : 6)
                                                                         : vars.value(var).join(QLatin1Char(' '));
            bool ok1, ok2;
            const int a = left.toInt(&ok1), b = arg(1).toInt(&ok2);
            if (name == QLatin1String("equals"))
                return left == arg(1);
            if (!ok1 || !ok2)
                return false;
            return name == QLatin1String("greaterThan") ? a > b : a < b;
        }
        if (name == QLatin1String("qtHaveModule"))
            return true;
        if (name == QLatin1String("contains")) {
            const QStringList values = vars.value(arg(0));
            return values.contains(arg(1));
        }
        if (name == QLatin1String("CONFIG")) // CONFIG(release, debug|release)
            return arg(0) != QLatin1String("debug");
        return false;
    }

    QString m_topDir;
};

namespace {

// Variables of one .pro file, ready to turn into a QmakeProject::Part.
QmakeParser::Vars parseVars(const QString &file)
{
    QmakeParser::Vars vars;
    vars[QStringLiteral("QT")] = {QStringLiteral("core"), QStringLiteral("gui")};
    QmakeParser(file).parse(file, vars);
    return vars;
}

QString resolvePath(const QString &p, const QString &dir)
{
    return QDir::cleanPath(QDir::isAbsolutePath(p) ? p : dir + QLatin1Char('/') + p);
}

} // namespace

QString QmakeProject::proFileName() const
{
    return QFileInfo(m_proFile).fileName();
}

QmakeProject QmakeProject::detect(const QString &root)
{
    QmakeProject out;
    if (root.isEmpty())
        return out;
    const QDir rootDir(root);
    const QStringList pros = rootDir.entryList({QStringLiteral("*.pro")}, QDir::Files, QDir::Name);
    if (pros.isEmpty())
        return out;
    QString main = pros.first();
    for (const QString &p : pros)
        if (QFileInfo(p).completeBaseName() == rootDir.dirName())
            main = p;
    out.m_root = rootDir.absolutePath();
    out.m_proFile = rootDir.filePath(main);

    // The top file, plus every sub-project of a SUBDIRS template (a few levels deep).
    QStringList todo{out.m_proFile};
    QSet<QString> seen;
    while (!todo.isEmpty() && out.m_parts.size() < 40) {
        const QString file = todo.takeFirst();
        if (seen.contains(file))
            continue;
        seen.insert(file);
        const QmakeParser::Vars vars = parseVars(file);
        const QString dir = QFileInfo(file).absolutePath();
        const QString tmpl = vars.value(QStringLiteral("TEMPLATE")).value(0, QStringLiteral("app"));
        if (tmpl == QLatin1String("subdirs")) {
            for (const QString &entry : vars.value(QStringLiteral("SUBDIRS"))) {
                const QString path = resolvePath(entry, dir);
                if (entry.endsWith(QLatin1String(".pro"))) {
                    todo << path;
                    continue;
                }
                const QString byName = path + QLatin1Char('/') + QFileInfo(path).fileName() + QStringLiteral(".pro");
                if (QFileInfo::exists(byName)) {
                    todo << byName;
                } else {
                    const QStringList inner = QDir(path).entryList({QStringLiteral("*.pro")}, QDir::Files, QDir::Name);
                    if (!inner.isEmpty())
                        todo << path + QLatin1Char('/') + inner.first();
                }
            }
            continue;
        }
        Part part;
        part.file = file;
        part.dir = dir;
        part.app = tmpl == QLatin1String("app");
        part.target = vars.value(QStringLiteral("TARGET")).value(0, QFileInfo(file).completeBaseName());
        part.destDir = vars.value(QStringLiteral("DESTDIR")).value(0);
        part.config = vars.value(QStringLiteral("CONFIG"));
        part.qt = vars.value(QStringLiteral("QT"));
        part.defines = vars.value(QStringLiteral("DEFINES"));
        part.pkgConfig = vars.value(QStringLiteral("PKGCONFIG"));
        for (const QString &flag : vars.value(QStringLiteral("QMAKE_CXXFLAGS")) + vars.value(QStringLiteral("QMAKE_CFLAGS")))
            part.cxxFlags << flag;
        for (const QString &inc : vars.value(QStringLiteral("INCLUDEPATH")))
            part.includes << resolvePath(inc, dir);
        out.m_parts << part;
    }

    // Executable: the first application, relative to an out-of-source build folder.
    for (const Part &p : std::as_const(out.m_parts)) {
        if (!p.app || p.config.contains(QStringLiteral("lib")))
            continue;
        const QString sub = QDir(out.m_root).relativeFilePath(p.dir);
        QString exe = p.target;
        if (!p.destDir.isEmpty()) {
            exe = QDir::isAbsolutePath(p.destDir) ? QDir::cleanPath(p.destDir + QLatin1Char('/') + p.target)
                                                  : QDir::cleanPath(sub + QLatin1Char('/') + p.destDir + QLatin1Char('/') + p.target);
        } else {
            exe = QDir::cleanPath(sub + QLatin1Char('/') + p.target);
        }
        out.m_runPath = exe;
        break;
    }
    return out;
}

QString QmakeProject::runCommand() const
{
    if (!isValid() || !isApp())
        return {};
    const QString found = QFileInfo(findQmake()).fileName();
    const QString qmake = found.isEmpty() ? QStringLiteral("qmake6") : found;
    const QString rel = QDir(m_root).relativeFilePath(m_proFile);
    const QString exe = QDir::isAbsolutePath(m_runPath) ? shellWord(m_runPath) : QStringLiteral("./") + shellWord(m_runPath);
    return QStringLiteral("cd {project} && mkdir -p build && cd build && %1 ../%2 && make -j$(nproc) && %3")
        .arg(qmake, shellWord(rel), exe);
}

QStringList QmakeProject::compilerFlags() const
{
    QStringList flags;
    if (!isValid())
        return flags;
    auto add = [&flags](const QString &f) {
        if (!flags.contains(f))
            flags << f;
    };
    static const QRegularExpression cxxStd(QStringLiteral(R"(^c\+\+(\d\d|1z|1y|2a|2b|2c)$)"));
    QString std;
    QStringList qtModules;
    for (const Part &p : m_parts) {
        for (const QString &c : p.config) {
            const auto m = cxxStd.match(c);
            if (m.hasMatch())
                std = QStringLiteral("-std=c++") + m.captured(1);
        }
        for (const QString &q : p.qt)
            if (!qtModules.contains(q))
                qtModules << q;
    }
    const QtInfo &info = qtInfo();
    if (std.isEmpty() && info.major >= 6)
        std = QStringLiteral("-std=c++17");
    if (!std.isEmpty())
        add(std);
    add(QStringLiteral("-fPIC"));
    add(QStringLiteral("-I") + m_root);

    for (const Part &p : m_parts) {
        add(QStringLiteral("-I") + p.dir);
        for (const QString &inc : p.includes)
            add(QStringLiteral("-I") + inc);
        for (const QString &d : p.defines)
            add(QStringLiteral("-D") + d);
        for (const QString &f : p.cxxFlags)
            if (f.startsWith(QLatin1String("-std=")) || f.startsWith(QLatin1String("-D")) || f.startsWith(QLatin1String("-I")) ||
                f.startsWith(QLatin1String("-U")) || f.startsWith(QLatin1String("-isystem")))
                add(f);
    }

    // Qt module headers (qmake adds these for every module in QT).
    if (!info.headers.isEmpty() && !qtModules.isEmpty()) {
        if (qtModules.contains(QStringLiteral("widgets")) && !qtModules.contains(QStringLiteral("gui")))
            qtModules << QStringLiteral("gui");
        if (!qtModules.contains(QStringLiteral("core")))
            qtModules << QStringLiteral("core");
        add(QStringLiteral("-isystem") + info.headers);
        for (const QString &q : std::as_const(qtModules)) {
            const QString dir = info.headers + QLatin1Char('/') + moduleDirName(q);
            if (QFileInfo(dir).isDir()) {
                add(QStringLiteral("-isystem") + dir);
                add(QStringLiteral("-DQT_%1_LIB").arg(q.toUpper()));
            }
        }
    }

    // pkg-config packages (CONFIG += link_pkgconfig, PKGCONFIG += ...).
    QStringList packages;
    for (const Part &p : m_parts)
        for (const QString &pkg : p.pkgConfig)
            if (!packages.contains(pkg))
                packages << pkg;
    if (!packages.isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("pkg-config")).isEmpty()) {
        packages = packages.mid(0, 8);
        for (const QString &f : QProcess::splitCommand(runTool(QStringLiteral("pkg-config"), QStringList{QStringLiteral("--cflags")} + packages)))
            add(f);
    }
    return flags;
}
