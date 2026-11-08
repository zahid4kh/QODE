#pragma once

#include <QString>

// Thin wrapper over QFile/QDir that reports failures as human-readable strings.
namespace FileManager {

struct ReadResult {
    bool ok = false;
    QByteArray data;
    QString error;
};

ReadResult readFile(const QString &path);
bool writeFile(const QString &path, const QByteArray &data, QString *error);

// A text file of the user's, decoded like the editor does (TextEncoding: a BOM decides, UTF-16 from Windows tools,
// else UTF-8, else the legacy encoding). Never QString::fromUtf8 on a user's file. False when it cannot be read.
bool readText(const QString &path, QString *text, QString *encoding = nullptr);
// `text` as bytes in the encoding `path` has on disk now (UTF-8 for a new file), so an edit keeps a BOM or UTF-16.
QByteArray textBytes(const QString &path, const QString &text);
// writeFile(path, textBytes(path, text)): an edit QODE makes to a file the user owns.
bool writeText(const QString &path, const QString &text, QString *error = nullptr);

bool createFile(const QString &path, QString *error);
bool createFolder(const QString &path, QString *error);
bool renamePath(const QString &from, const QString &to, QString *error);
// A name for `name` inside `dir` that is not taken: "a.txt" -> "a copy.txt" -> "a copy 2.txt".
QString uniquePath(const QString &dir, const QString &name);
// Copies a file or folder (recursively, symlinks stay links) into `targetDir` under a free name. Returns the new path, or
// an empty string with `error` set.
QString copyInto(const QString &source, const QString &targetDir, QString *error);
bool removePath(const QString &path, QString *error);

// A shell script by extension (.sh, .bash, .zsh, .ksh) or any file whose first line is a `#!`.
bool isScript(const QString &path);
// chmod +x: execute permission for everyone who may read the file.
bool makeExecutable(const QString &path, QString *error);

// Returns an error message when `name` can't be used as a single path component.
QString validateName(const QString &name);

void revealInFileManager(const QString &path);
QString displayPath(const QString &path); // replaces $HOME with ~

} // namespace FileManager
