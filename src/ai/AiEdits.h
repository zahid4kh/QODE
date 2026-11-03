#pragma once

#include <QJsonObject>
#include <QUrl>
#include <QStringList>
#include <QVector>

// The data model of one AI turn: the plan Gemini returns, the file edits it proposes and what happened to each.
// Qt Core only, so it can be tested without a window.
namespace AiEdits {

struct Plan {
    bool needsEdits = false;
    QString answer;     // markdown; the whole reply when no edit is needed
    QStringList steps;  // numbered steps, in order
    QStringList files;  // project-relative files the steps read or change
};

struct Edit {
    enum Op { Replace, Insert, Create };
    QString path;  // project-relative
    Op op = Replace;
    int startLine = 0, endLine = 0; // 1-based, inclusive; Insert puts the code in front of startLine (lineCount + 1 appends)
    QString code;  // without line-number prefixes, "\n" line ends
};

struct Outcome {
    Edit edit;
    bool ok = false;
    QString note;  // why it failed, or a short description of what was done
    QString before; // the lines a replace removed (for the diff shown in the chat)
};

QJsonObject planSchema();
QJsonObject editSchema();
QString systemPrompt();

// Both return false and fill `error` when the text is not the expected JSON.
bool parsePlan(const QString &json, Plan *plan, QString *error);
bool parseEdits(const QString &json, QVector<Edit> *edits, QString *error);

// "  12 | text" for every line, the form the model sees (and is told not to reproduce).
QString numbered(const QString &text);
// Removes leading "12 | " prefixes the model copied by mistake (only when every non-empty line has one).
QString stripLineNumbers(const QString &code);

// Resolves a project-relative path to an absolute one; false when it escapes the project root.
bool resolveInside(const QString &root, const QString &relative, QString *absolute);
// A file link clicked in a chat reply: `qode-open:`, `file:` or a bare path (project-relative or absolute), with an
// optional line as "#L42", "#L42-L51" or ":42". False for web links and paths that are not an existing file.
bool resolveFileLink(const QString &root, const QUrl &url, QString *absolute, int *line);

QString opName(Edit::Op op);

} // namespace AiEdits
