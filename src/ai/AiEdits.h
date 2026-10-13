#pragma once

#include <QJsonObject>
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

QString opName(Edit::Op op);

} // namespace AiEdits
