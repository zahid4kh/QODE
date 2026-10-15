#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// One HTTP endpoint found in a Spring controller's source.
struct SpringParam
{
    enum Kind { Path, Query, Header, Body };
    Kind kind = Query;
    QString name;
    QString type;
    QString defaultValue;
    bool required = true;
};

struct SpringEndpoint
{
    QString method;     // GET POST PUT DELETE PATCH HEAD OPTIONS, "ANY" when the mapping names none
    QString path;       // class mapping + method mapping, always starts with '/'
    QString controller; // class name
    QString handler;    // function name
    QString file;
    int line = 1;       // 1-based line of the mapping annotation
    QVector<SpringParam> params;

    QString title() const { return method + QLatin1Char(' ') + path; }
    QString bodyType() const;
    // The path followed by the query parameters that are required or have a default ("/users?page=0").
    QString urlTemplate() const;
};

// Reads @RestController / @Controller classes (Kotlin and Java) and lists their mapped functions. Plain text scanning
// (comments are blanked first), no compiler involved, so it also works while the project does not build. Qt Core only.
struct SpringScan
{
    struct Field
    {
        QString name, type;
    };
    QVector<SpringEndpoint> endpoints;
    QHash<QString, QVector<Field>> types; // Kotlin classes and Java records by simple name

    // A JSON document with a placeholder for every field of the type ("List<User>" gives an array).
    QString bodySkeleton(const QString &type) const;
};

namespace SpringEndpoints {

SpringScan scan(const QStringList &sourceRoots);
// One file; the file's classes are added to `into.types`, its endpoints to `into.endpoints`.
void parseFile(const QString &path, const QString &text, SpringScan &into);

} // namespace SpringEndpoints
