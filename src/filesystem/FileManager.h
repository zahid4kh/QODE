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

bool createFile(const QString &path, QString *error);
bool createFolder(const QString &path, QString *error);
bool renamePath(const QString &from, const QString &to, QString *error);
bool removePath(const QString &path, QString *error);

// Returns an error message when `name` can't be used as a single path component.
QString validateName(const QString &name);

void revealInFileManager(const QString &path);
QString displayPath(const QString &path); // replaces $HOME with ~

} // namespace FileManager
