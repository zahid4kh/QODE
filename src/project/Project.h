#pragma once

#include <QDir>
#include <QString>

struct Project {
    QString root; // absolute, canonical
    QString name;

    bool isValid() const { return !root.isEmpty(); }
    QString metadataDir() const { return QDir(root).filePath(QStringLiteral(".qode")); }
    QString metadataFile() const { return QDir(metadataDir()).filePath(QStringLiteral("project.json")); }
};
