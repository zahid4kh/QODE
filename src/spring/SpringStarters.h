#pragma once

#include "spring/SpringProject.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// The dependencies a Spring Boot project is commonly started with ("start.spring.io inside the editor") and the text
// edit that adds them to build.gradle(.kts) or pom.xml. Versions come from Spring Boot's own dependency management,
// so only coordinates are written. Qt Core only; computing a change writes nothing.
namespace SpringStarters {

struct Dep
{
    QString config; // Gradle configuration: implementation, runtimeOnly, developmentOnly, annotationProcessor
    QString group, artifact;
    QString version; // only for libraries Spring Boot does not manage
};

struct Starter
{
    QString id, title, description, category;
    QList<Dep> deps;
    bool kotlinJpa = false; // Kotlin: also apply the JPA compiler plugin (no-arg entities)
};

const QList<Starter> &catalog();
// Ids whose first dependency already appears in the build file.
QStringList installed(const SpringProject &project);

struct Change
{
    QString file;
    QString text;      // the build file as it would be after the change
    QStringList added; // titles of the starters added
    QList<QPair<QString, QString>> extra; // other files to write: path, new text (the version catalog)
    QString note;      // something the user should still do, may be empty
    bool ok = false;
    QString error;
};
Change plan(const SpringProject &project, const QStringList &ids);

} // namespace SpringStarters
