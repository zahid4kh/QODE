#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Source for new Spring components (controller, service, JPA entity …) written in the language and package of the
// folder they are created in. Qt Core only; nothing is written here.
namespace SpringScaffold {

enum class Kind { Controller, Service, Entity, Repository, Configuration, ExceptionHandler };

struct Result
{
    QString path;    // file to create
    QString content;
    QString error;   // set when the folder is no package folder or the name is invalid
    bool ok() const { return error.isEmpty(); }
};

QString title(Kind kind);          // "Controller"
QString suffix(Kind kind);         // "Controller", "" for entities
struct Field
{
    QString name, type; // type in Kotlin spelling ("Int"), Java gets "Integer"
};
// "name:String, price:Double" (commas or spaces); sets `error` and returns nothing useful when it is malformed.
QVector<Field> parseFields(const QString &spec, QString *error);

// `dir` is the folder the file goes in; `sourceRoots` are the module's src/main/{kotlin,java} (and test) folders.
// `fields` only matter for entities.
Result generate(Kind kind, const QString &name, const QString &dir, const QStringList &sourceRoots, const QVector<Field> &fields = {});
// Entity, repository, service and controller with the five CRUD endpoints, all in `dir`. Nothing is written; on an
// error the first result carries it.
QVector<Result> generateCrud(const QString &name, const QVector<Field> &fields, const QString &dir, const QStringList &sourceRoots);
// "User" -> "/users"
QString pathFor(const QString &name);
// Package of a folder below one of the source roots, "" when it is no folder of one.
QString packageOf(const QString &dir, const QStringList &sourceRoots, bool *kotlin = nullptr);

} // namespace SpringScaffold
