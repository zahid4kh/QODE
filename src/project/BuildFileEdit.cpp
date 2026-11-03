#include "project/BuildFileEdit.h"

#include <QRegularExpression>

namespace BuildFileEdit {

int matchBrace(const QString &s, int open)
{
    int depth = 0;
    for (int i = open; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('{'))
            ++depth;
        else if (s[i] == QLatin1Char('}') && --depth == 0)
            return i;
    }
    return -1;
}

bool insertGradleLines(QString &text, const QStringList &deps)
{
    static const QRegularExpression block(QStringLiteral(R"(^dependencies\s*\{)"), QRegularExpression::MultilineOption);
    int close = -1;
    QRegularExpressionMatch m = block.match(text);
    // The module's block is the last top-level one (buildscript { dependencies { } } is indented).
    for (QRegularExpressionMatchIterator it = block.globalMatch(text); it.hasNext();)
        m = it.next();
    if (m.hasMatch()) {
        close = matchBrace(text, m.capturedEnd() - 1);
        if (close < 0)
            return false;
    }
    QStringList lines;
    for (const QString &d : deps)
        lines << QStringLiteral("    ") + d;
    if (close < 0) {
        if (!text.endsWith(QLatin1Char('\n')))
            text += QLatin1Char('\n');
        text += QStringLiteral("\ndependencies {\n") + lines.join(QLatin1Char('\n')) + QStringLiteral("\n}\n");
        return true;
    }
    // After the last implementation / runtimeOnly / developmentOnly line (the test ones stay last); else at the end.
    int at = close;
    static const QRegularExpression mainDep(QStringLiteral(R"(^[ \t]*(?:implementation|api|runtimeOnly|developmentOnly|compileOnly|annotationProcessor)\b[^\n]*\n)"), QRegularExpression::MultilineOption);
    int last = -1;
    for (QRegularExpressionMatchIterator it = mainDep.globalMatch(text, m.capturedEnd()); it.hasNext();) {
        const QRegularExpressionMatch dm = it.next();
        if (dm.capturedStart() > close)
            break;
        last = dm.capturedEnd();
    }
    if (last >= 0) {
        text.insert(last, lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
        return true;
    }
    // Before the closing brace, after the last line of the block.
    while (at > 0 && (text[at - 1] == QLatin1Char(' ') || text[at - 1] == QLatin1Char('\t')))
        --at;
    if (at > 0 && text[at - 1] != QLatin1Char('\n'))
        lines.prepend(QString());
    text.insert(at, lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    return true;
}

bool insertMavenDependencies(QString &text, const QList<MavenDependency> &deps)
{
    // Skip <dependencyManagement> and <build> (plugin dependencies).
    auto inside = [&](int pos, const char *open, const char *close) {
        const int a = text.indexOf(QLatin1String(open));
        const int b = text.indexOf(QLatin1String(close));
        return a >= 0 && b > a && pos > a && pos < b;
    };
    int pos = -1;
    for (int from = 0;;) {
        const int p = text.indexOf(QStringLiteral("</dependencies>"), from);
        if (p < 0)
            break;
        if (!inside(p, "<dependencyManagement>", "</dependencyManagement>") && !inside(p, "<build>", "</build>")) {
            pos = p;
            break;
        }
        from = p + 1;
    }
    QString unit = QStringLiteral("    ");
    QString base = unit; // indentation of <dependency>
    if (pos >= 0) {
        const int d = text.lastIndexOf(QStringLiteral("<dependency>"), pos);
        if (d >= 0) {
            const int ls = text.lastIndexOf(QLatin1Char('\n'), d) + 1;
            base = text.mid(ls, d - ls);
            // Indentation step of the file: how far <groupId> sits inside <dependency>.
            const int g = text.indexOf(QStringLiteral("<groupId>"), d);
            if (g > d) {
                const int gl = text.lastIndexOf(QLatin1Char('\n'), g) + 1;
                const QString gi = text.mid(gl, g - gl);
                if (gi.size() > base.size() && gi.startsWith(base))
                    unit = gi.mid(base.size());
            }
        } else {
            const int ls = text.lastIndexOf(QLatin1Char('\n'), pos) + 1;
            base = text.mid(ls, pos - ls) + unit;
        }
    }
    QString xml;
    for (const MavenDependency &d : deps) {
        xml += base + QStringLiteral("<dependency>\n");
        xml += base + unit + QStringLiteral("<groupId>%1</groupId>\n").arg(d.group);
        xml += base + unit + QStringLiteral("<artifactId>%1</artifactId>\n").arg(d.artifact);
        if (!d.version.isEmpty())
            xml += base + unit + QStringLiteral("<version>%1</version>\n").arg(d.version);
        if (!d.scope.isEmpty())
            xml += base + unit + QStringLiteral("<scope>%1</scope>\n").arg(d.scope);
        if (d.optional)
            xml += base + unit + QStringLiteral("<optional>true</optional>\n");
        xml += base + QStringLiteral("</dependency>\n");
    }
    if (pos < 0) {
        const int end = text.lastIndexOf(QStringLiteral("</project>"));
        if (end < 0)
            return false;
        text.insert(end, QStringLiteral("    <dependencies>\n") + xml + QStringLiteral("    </dependencies>\n"));
        return true;
    }
    const int ls = text.lastIndexOf(QLatin1Char('\n'), pos) + 1;
    text.insert(ls, xml);
    return true;
}

} // namespace BuildFileEdit
