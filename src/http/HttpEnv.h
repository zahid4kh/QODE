#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// Named sets of variables for .http files ("dev", "prod" …), read from the files the other clients use:
//   http-client.env.json + http-client.private.env.json (IntelliJ), the nearest folder from the .http file up to the
//   project root that has one; values of the private file win.
//   "rest-client.environmentVariables" in <project>/.vscode/settings.json (VS Code), where "$shared" applies to all.
// Nothing is written back; QODE only remembers which environment is selected.
struct HttpEnvironments
{
    QStringList names;                                  // sorted, without $shared
    QHash<QString, QHash<QString, QString>> values;     // environment -> variables ($shared already merged in)
    QHash<QString, QString> shared;                     // VS Code's $shared
    QStringList sources;                                // the files read

    QHash<QString, QString> vars(const QString &name) const; // $shared when `name` is unknown or empty
};

namespace HttpEnv {

HttpEnvironments load(const QString &fileDir, const QString &projectRoot);
// KEY=value pairs of the .env next to the .http file, else the one in the project root ({{$dotenv KEY}}).
QHash<QString, QString> dotenv(const QString &fileDir, const QString &projectRoot);
// JSON with // and /* */ comments and trailing commas (VS Code settings) made plain.
QByteArray stripJsonc(const QByteArray &text);

} // namespace HttpEnv
