#include "Breadcrumbs.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QDir>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>

namespace {

int indentOf(const QString &text, int tab)
{
    int cols = 0;
    for (const QChar c : text) {
        if (c == QLatin1Char(' '))
            ++cols;
        else if (c == QLatin1Char('\t'))
            cols += tab - cols % tab;
        else
            break;
    }
    return cols;
}

bool isControlKeyword(const QString &w)
{
    static const QSet<QString> k = {QStringLiteral("if"),     QStringLiteral("for"),    QStringLiteral("while"),   QStringLiteral("switch"),
                                    QStringLiteral("catch"),  QStringLiteral("else"),   QStringLiteral("return"),  QStringLiteral("sizeof"),
                                    QStringLiteral("foreach"), QStringLiteral("do"),    QStringLiteral("elif"),    QStringLiteral("with"),
                                    QStringLiteral("until"),  QStringLiteral("try"),    QStringLiteral("throw"),   QStringLiteral("delete"),
                                    QStringLiteral("new"),    QStringLiteral("case"),   QStringLiteral("defined"), QStringLiteral("assert"),
                                    QStringLiteral("emit"),   QStringLiteral("connect")};
    return k.contains(w);
}

// The name a definition line introduces ("MainWindow", "CodeEditor::applyTheme()", "compute()"), or "".
QString symbolFromHeader(const QString &raw, const QString &language)
{
    const QString line = raw.trimmed();
    if (line.isEmpty() || line == QLatin1String("{"))
        return {};
    const bool python = language == QLatin1String("Python");
    if (line.startsWith(QStringLiteral("//")) || line.startsWith(QStringLiteral("/*")) || line.startsWith(QLatin1Char('*')) ||
        (!python && line.startsWith(QLatin1Char('#')) && language != QLatin1String("Markdown")))
        return {};

    if (language == QLatin1String("JSON")) {
        static const QRegularExpression key(QStringLiteral("^\"([^\"]+)\"\\s*:\\s*[\\{\\[]$"));
        const auto m = key.match(line);
        return m.hasMatch() ? m.captured(1) : QString();
    }
    if (language == QLatin1String("CSS")) {
        if (line.endsWith(QLatin1Char('{')))
            return line.left(line.size() - 1).trimmed();
        return {};
    }

    static const QRegularExpression pyDef(QStringLiteral("^(?:async\\s+)?def\\s+(\\w+)"));
    static const QRegularExpression typeDef(
        QStringLiteral("\\b(?:class|struct|namespace|enum(?:\\s+class|\\s+struct)?|interface|union|trait|impl|module|object|protocol|extension)\\s+([A-Za-z_][\\w:.<>]*)"));
    if (python) {
        if (const auto m = pyDef.match(line); m.hasMatch())
            return m.captured(1) + QStringLiteral("()");
        static const QRegularExpression pyClass(QStringLiteral("^class\\s+(\\w+)"));
        if (const auto m = pyClass.match(line); m.hasMatch())
            return m.captured(1);
        return {};
    }
    if (const auto m = typeDef.match(line); m.hasMatch() && !line.endsWith(QLatin1Char(';')))
        return m.captured(1);

    // JavaScript / TypeScript
    static const QRegularExpression jsFunc(QStringLiteral("^(?:export\\s+)?(?:default\\s+)?(?:async\\s+)?function\\*?\\s*(\\w+)"));
    static const QRegularExpression jsArrow(
        QStringLiteral("^(?:export\\s+)?(?:const|let|var)\\s+(\\w+)\\s*=\\s*(?:async\\s*)?(?:\\([^)]*\\)|\\w+)\\s*=>"));
    if (const auto m = jsFunc.match(line); m.hasMatch())
        return m.captured(1) + QStringLiteral("()");
    if (const auto m = jsArrow.match(line); m.hasMatch())
        return m.captured(1) + QStringLiteral("()");

    // C-like function header: [type] name(args) [qualifiers] [{]
    if (line.endsWith(QLatin1Char(';')) || line.endsWith(QLatin1Char(',')))
        return {};
    static const QRegularExpression fn(QStringLiteral("^([\\w:<>,*&~\\s]*?)([A-Za-z_~][\\w:~]*)\\s*\\("));
    const auto m = fn.match(line);
    if (!m.hasMatch())
        return {};
    const QString prefix = m.captured(1).trimmed();
    const QString name = m.captured(2);
    if (isControlKeyword(name) || isControlKeyword(prefix))
        return {};
    static const QRegularExpression bareMethod(QStringLiteral("^\\w+\\s*\\([^()]*\\)\\s*(?:const\\s*)?\\{$"));
    if (prefix.isEmpty() && !name.contains(QStringLiteral("::")) && !bareMethod.match(line).hasMatch())
        return {}; // most likely a call that opens a lambda or block
    return name + QStringLiteral("()");
}

} // namespace

QList<Breadcrumbs::Crumb> Breadcrumbs::symbolChain(const QTextDocument *doc, int blockNumber, const QString &language)
{
    QList<Crumb> chain;
    const int tab = qMax(1, SettingsManager::instance().tabSize());
    QTextBlock cur = doc->findBlockByNumber(blockNumber);
    if (!cur.isValid())
        return chain;

    if (language == QLatin1String("Markdown")) {
        // Headings nest by level.
        int level = 7;
        int scanned = 0;
        QList<Crumb> rev;
        for (QTextBlock b = cur; b.isValid() && scanned < 20000 && level > 1; b = b.previous(), ++scanned) {
            const QString t = b.text();
            if (!t.startsWith(QLatin1Char('#')))
                continue;
            int n = 0;
            while (n < t.size() && t.at(n) == QLatin1Char('#'))
                ++n;
            if (n > 6 || n >= t.size() || t.at(n) != QLatin1Char(' ') || n >= level)
                continue;
            level = n;
            rev.prepend({t.mid(n + 1).trimmed(), b.blockNumber()});
        }
        return rev.mid(qMax(0, rev.size() - 4));
    }

    // Walk outwards by indentation: each step finds the nearest earlier line that is indented less.
    QString text = cur.text();
    int level = indentOf(text, tab);
    if (text.trimmed().isEmpty()) {
        // Blank line: borrow the indentation of the code above.
        for (QTextBlock b = cur.previous(); b.isValid(); b = b.previous())
            if (!b.text().trimmed().isEmpty()) {
                level = indentOf(b.text(), tab);
                cur = b;
                break;
            }
    } else if (text.trimmed().startsWith(QLatin1Char('}')) || text.trimmed().startsWith(QLatin1Char(')')) ||
               text.trimmed().startsWith(QLatin1Char(']'))) {
        ++level; // a closing brace still belongs to the scope it ends
    }

    int scanned = 0;
    QList<Crumb> rev;
    for (QTextBlock b = cur.previous(); b.isValid() && level > 0 && scanned < 30000 && rev.size() < 8; b = b.previous(), ++scanned) {
        const QString t = b.text();
        if (t.trimmed().isEmpty())
            continue;
        const int ind = indentOf(t, tab);
        if (ind >= level)
            continue;
        static const QRegularExpression accessLabel(QStringLiteral("^\\s*(?:public|private|protected|signals|slots|Q_SIGNALS|Q_SLOTS|default)\\b[^;{]*:\\s*$"));
        if (accessLabel.match(t).hasMatch())
            continue; // "public:" sits at the class's indentation but is not a scope of its own
        level = ind;
        QTextBlock header = b;
        QString headerText = t;
        // Allman style: the scope opens on a lone "{", the definition is the line above it.
        if (t.trimmed() == QLatin1String("{") || t.trimmed().startsWith(QLatin1Char('{'))) {
            for (QTextBlock p = b.previous(); p.isValid(); p = p.previous())
                if (!p.text().trimmed().isEmpty()) {
                    header = p;
                    headerText = p.text();
                    break;
                }
        }
        const QString name = symbolFromHeader(headerText, language);
        if (!name.isEmpty())
            rev.prepend({name, header.blockNumber()});
    }
    return rev.mid(qMax(0, rev.size() - 4));
}

QList<Breadcrumbs::Crumb> Breadcrumbs::pathCrumbs(const QString &filePath, const QString &projectRoot)
{
    QList<Crumb> out;
    if (filePath.isEmpty())
        return out;
    QString rel = filePath;
    if (!projectRoot.isEmpty() && filePath.startsWith(projectRoot + QLatin1Char('/'))) {
        rel = filePath.mid(projectRoot.size() + 1);
    } else {
        const QString home = QDir::homePath();
        if (filePath.startsWith(home + QLatin1Char('/')))
            rel = QStringLiteral("~/") + filePath.mid(home.size() + 1);
        else
            rel = filePath.startsWith(QLatin1Char('/')) ? filePath.mid(1) : filePath;
    }
    for (const QString &part : rel.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        out.append({part, -1});
    return out;
}

// --- Widget ----------------------------------------------------------------------------------------

Breadcrumbs::Breadcrumbs(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(24);
    setMouseTracking(true);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void Breadcrumbs::setCrumbs(const QList<Crumb> &path, const QList<Crumb> &symbols)
{
    m_path = path;
    m_symbols = symbols;
    m_hover = -1;
    update();
}

int Breadcrumbs::crumbAt(const QPoint &pos) const
{
    for (int i = 0; i < m_shown.size(); ++i)
        if (m_shown.at(i).rect.contains(pos))
            return i;
    return -1;
}

void Breadcrumbs::paintEvent(QPaintEvent *)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QPainter p(this);
    p.fillRect(rect(), t.editorBg);
    p.setPen(t.border);
    p.drawLine(0, height() - 1, width(), height() - 1);

    QFont f = font();
    f.setPointSizeF(qMax(7.5, f.pointSizeF() - 1));
    p.setFont(f);
    const QFontMetrics fm(f);
    const int sepW = 18;
    const int avail = width() - 20;

    QList<Crumb> all = m_path;
    all += m_symbols;
    // Drop leading segments (replaced by an ellipsis) until everything fits.
    int firstShown = 0;
    auto totalWidth = [&](int from) {
        int w = from > 0 ? fm.horizontalAdvance(QChar(0x2026)) + sepW : 0;
        for (int i = from; i < all.size(); ++i)
            w += fm.horizontalAdvance(all.at(i).text) + (i + 1 < all.size() ? sepW : 0);
        return w;
    };
    while (firstShown < all.size() - 1 && totalWidth(firstShown) > avail)
        ++firstShown;

    m_shown.clear();
    int x = 10;
    const int cy = height() / 2;
    const QPixmap chevron = Icons::pixmap(QStringLiteral(":/new-icons/chevron-right.svg"), t.gutterFg, 12);
    auto sep = [&] {
        p.drawPixmap(x + (sepW - 12) / 2, cy - 6, chevron);
        x += sepW;
    };
    if (firstShown > 0) {
        p.setPen(t.textMuted);
        p.drawText(QRect(x, 0, fm.horizontalAdvance(QChar(0x2026)), height()), Qt::AlignVCenter, QString(QChar(0x2026)));
        x += fm.horizontalAdvance(QChar(0x2026));
        sep();
    }
    for (int i = firstShown; i < all.size(); ++i) {
        const Crumb &c = all.at(i);
        const int w = fm.horizontalAdvance(c.text);
        const QRect r(x, 0, w, height());
        const bool last = i == all.size() - 1;
        const bool clickable = c.line >= 0;
        const int shownIndex = m_shown.size();
        QFont cf = f;
        cf.setUnderline(clickable && shownIndex == m_hover);
        p.setFont(cf);
        p.setPen(last || (clickable && shownIndex == m_hover) ? t.editorFg : t.textMuted);
        p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, c.text);
        m_shown.append({r, c.line, c.text});
        x += w;
        if (i + 1 < all.size())
            sep();
    }
}

void Breadcrumbs::mouseMoveEvent(QMouseEvent *event)
{
    const int i = crumbAt(event->position().toPoint());
    const int hover = i >= 0 && m_shown.at(i).line >= 0 ? i : -1;
    if (hover != m_hover) {
        m_hover = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void Breadcrumbs::leaveEvent(QEvent *)
{
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
}

void Breadcrumbs::mousePressEvent(QMouseEvent *event)
{
    const int i = crumbAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && i >= 0 && m_shown.at(i).line >= 0)
        emit lineRequested(m_shown.at(i).line);
}
