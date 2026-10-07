#include "CliCommands.h"

#include "settings/ThemeManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

void registerCliBuiltins(QList<CliCommand> &commands); // CliBuiltins.cpp

CliCommands::CliCommands()
{
    registerCliBuiltins(m_commands);
}

const CliCommands &CliCommands::instance()
{
    static const CliCommands registry;
    return registry;
}

const CliCommand *CliCommands::find(const QString &name) const
{
    for (const CliCommand &c : m_commands)
        if (c.name == name || c.aliases.contains(name))
            return &c;
    return nullptr;
}

QStringList CliCommands::splitArgs(const QString &line)
{
    QStringList out;
    QString cur;
    bool have = false;
    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quote.isNull() && c.isSpace()) {
            if (have)
                out << cur;
            cur.clear();
            have = false;
        } else if (quote.isNull() && (c == QLatin1Char('\'') || c == QLatin1Char('"'))) {
            quote = c;
            have = true;
        } else if (!quote.isNull() && c == quote) {
            quote = QChar();
        } else if (c == QLatin1Char('\\') && quote != QLatin1Char('\'') && i + 1 < line.size()) {
            cur += line.at(++i);
            have = true;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have)
        out << cur;
    return out;
}

namespace {

// Index where the word under the cursor starts, and how many words precede it.
int wordStart(const QString &text, int *wordsBefore)
{
    int start = 0, words = 0;
    bool inWord = false;
    QChar quote;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quote.isNull() && c.isSpace() && !(i > 0 && text.at(i - 1) == QLatin1Char('\\'))) {
            if (inWord)
                ++words;
            inWord = false;
            start = i + 1;
        } else {
            inWord = true;
            if (quote.isNull() && (c == QLatin1Char('\'') || c == QLatin1Char('"')))
                quote = c;
            else if (c == quote)
                quote = QChar();
        }
    }
    if (wordsBefore)
        *wordsBefore = words;
    return start;
}

QString escapeForShell(const QString &s)
{
    static const QString special = QStringLiteral(" \t'\"\\$&;()<>|*?[]{}#!`");
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        if (special.contains(s.at(i)))
            out += QLatin1Char('\\');
        out += s.at(i);
    }
    return out;
}

QString unescape(const QString &s)
{
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) == QLatin1Char('\\') && i + 1 < s.size())
            ++i;
        out += s.at(i);
    }
    return out;
}

const QStringList &pathExecutables()
{
    static QStringList names;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        QSet<QString> seen;
        for (const QString &dir : qEnvironmentVariable("PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
            const QFileInfoList list = QDir(dir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
            for (const QFileInfo &fi : list)
                if (fi.isExecutable())
                    seen.insert(fi.fileName());
        }
        names = QStringList(seen.begin(), seen.end());
        names.sort();
    }
    return names;
}

} // namespace

CliCompletion CliCommands::completePath(const QString &line, int start, int cursor, const QString &cwd, bool directoriesOnly)
{
    CliCompletion result;
    result.start = start;
    QString word = line.mid(start, cursor - start);
    QChar quote;
    if (word.startsWith(QLatin1Char('\'')) || word.startsWith(QLatin1Char('"'))) {
        quote = word.at(0);
        word.remove(0, 1);
    }
    const QString typed = quote.isNull() ? unescape(word) : word;
    const int slash = typed.lastIndexOf(QLatin1Char('/'));
    const QString typedDir = slash >= 0 ? typed.left(slash + 1) : QString();
    const QString prefix = typed.mid(slash + 1);

    QString dirPath;
    if (typedDir.isEmpty())
        dirPath = cwd;
    else if (typedDir.startsWith(QLatin1Char('~')))
        dirPath = QDir::homePath() + typedDir.mid(1);
    else if (QDir::isAbsolutePath(typedDir))
        dirPath = typedDir;
    else
        dirPath = cwd + QLatin1Char('/') + typedDir;

    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (directoriesOnly)
        filters = QDir::Dirs | QDir::NoDotAndDotDot;
    if (prefix.startsWith(QLatin1Char('.')))
        filters |= QDir::Hidden;
    const QFileInfoList entries = QDir(dirPath).entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    auto collect = [&](Qt::CaseSensitivity cs) {
        for (const QFileInfo &fi : entries) {
            if (!fi.fileName().startsWith(prefix, cs))
                continue;
            CliCandidate c;
            c.directory = fi.isDir();
            c.label = fi.fileName() + (c.directory ? QStringLiteral("/") : QString());
            const QString body = typedDir + fi.fileName() + (c.directory ? QStringLiteral("/") : QString());
            if (quote.isNull()) {
                // Keep a leading "~/" readable; escape the rest.
                const bool tilde = body.startsWith(QLatin1String("~/"));
                c.text = tilde ? QStringLiteral("~/") + escapeForShell(body.mid(2)) : escapeForShell(body);
            } else {
                c.text = QString(quote) + body;
            }
            result.items.append(c);
        }
    };
    collect(Qt::CaseSensitive);
    if (result.items.isEmpty() && !prefix.isEmpty())
        collect(Qt::CaseInsensitive);
    return result;
}

CliCompletion CliCommands::completeShell(const QString &line, int cursor, const QString &cwd)
{
    const QString text = line.left(cursor);
    int before = 0;
    const int start = wordStart(text, &before);
    const QString word = text.mid(start);
    if (before == 0 && !word.isEmpty() && !word.contains(QLatin1Char('/')) && !word.startsWith(QLatin1Char('.')) &&
        !word.startsWith(QLatin1Char('~'))) {
        CliCompletion result;
        result.start = start;
        for (const QString &name : pathExecutables()) {
            if (name.startsWith(word)) {
                result.items.append({escapeForShell(name), name, {}, false});
                if (result.items.size() >= 300)
                    break;
            }
        }
        return result;
    }
    return completePath(line, start, cursor, cwd, false);
}

CliCompletion CliCommands::complete(const QString &line, int cursor, const QString &cwd) const
{
    const QString text = line.left(cursor);
    if (!text.startsWith(QLatin1Char('/')))
        return completeShell(line, cursor, cwd);

    CliCompletion result;
    int before = 0;
    const int start = wordStart(text, &before);
    const QString word = text.mid(start);
    if (before == 0) {
        result.start = 0;
        const QString typed = word.mid(1);
        for (const CliCommand &c : m_commands) {
            QStringList names{c.name};
            names += c.aliases;
            for (const QString &n : names) {
                if (n.startsWith(typed, Qt::CaseInsensitive)) {
                    result.items.append({QLatin1Char('/') + n, QLatin1Char('/') + n, c.summary, false});
                    break;
                }
            }
        }
        return result;
    }

    const QStringList parts = splitArgs(text);
    const CliCommand *cmd = find(parts.value(0).mid(1));
    if (!cmd)
        return result;
    const int argIndex = before - 1;
    if (argIndex < cmd->argFrom || word.startsWith(QLatin1Char('-')))
        return result;
    switch (cmd->arg) {
    case CliCommand::Arg::Path: return completePath(line, start, cursor, cwd, false);
    case CliCommand::Arg::Directory: return completePath(line, start, cursor, cwd, true);
    case CliCommand::Arg::Command:
        result.start = start;
        for (const CliCommand &c : m_commands)
            if (c.name.startsWith(word, Qt::CaseInsensitive))
                result.items.append({c.name, c.name, c.summary, false});
        break;
    case CliCommand::Arg::Theme:
        result.start = start;
        for (const ThemeManager::Info &t : ThemeManager::instance().themes())
            if (t.id.startsWith(word, Qt::CaseInsensitive))
                result.items.append({t.id, t.id, t.name, false});
        break;
    case CliCommand::Arg::None: break;
    }
    return result;
}
