#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Completion for Spring Boot configuration keys in application*.properties / .yml: a built-in list of the common
// properties (with type / default) that completes one dotted segment at a time, like the version catalog accessors do.
// Qt Core only.
namespace SpringProperties {

struct Entry
{
    QString label;  // what is inserted (the rest of the segment being typed)
    QString detail; // type / default / hint
    bool group = false; // a prefix with more segments below it
};

bool isConfigFile(const QString &path); // application*.properties|yml|yaml (and bootstrap*)
bool isYaml(const QString &path);

// `beforeWord` is the line text in front of the word being typed. For properties the key is read from the line; for
// YAML `parents` is the key path of the enclosing mappings (found by indentation), `beforeWord` the current line.
QList<Entry> complete(const QString &beforeWord, bool yaml, const QStringList &parents = {});

// Settings of the default profile (application.properties / .yml) as dotted keys, for reading a port or a base path.
QMap<QString, QString> load(const QString &resourcesDir);

// Key path of the mapping a YAML line sits in: lines above, nearest first, as plain text.
QStringList yamlParents(const QStringList &linesAbove, int indent);

} // namespace SpringProperties
