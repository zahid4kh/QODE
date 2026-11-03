#pragma once

#include "ktor/KtorProject.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// The Ktor server plugins a project is commonly extended with ("start.ktor.io inside the editor") and the text edit
// that adds their modules to build.gradle(.kts) / the version catalog / pom.xml, written the way the build already
// declares Ktor (Ktor's catalog accessors, the project's own libs catalog, or coordinates with the same version).
// Qt Core only; computing a change writes nothing.
namespace KtorPlugins {

struct Plugin
{
    QString id, title, description, category;
    QStringList keys;        // entries of Ktor's version catalog ("server-contentNegotiation"); the first one marks it as added
    QString install;         // what to write in a module to turn it on, shown after adding
    bool serialization = false; // needs the kotlinx.serialization compiler plugin
};

const QList<Plugin> &catalog();
QString artifactOf(const QString &key); // "server-contentNegotiation" -> "ktor-server-content-negotiation"
QString accessorOf(const QString &key); // "server-auth-jwt" -> "server.auth.jwt" (ktorLibs.server.auth.jwt)

// Ids whose first module the build already declares.
QStringList installed(const KtorProject &project);

struct Change
{
    QString file;
    QString text;      // the build file as it would be after the change
    QStringList added; // titles of the plugins added
    QList<QPair<QString, QString>> extra; // other files to write: path, new text (the version catalog)
    QString note;      // what to do next (install(…) lines), may be empty
    bool ok = false;
    QString error;
};
Change plan(const KtorProject &project, const QStringList &ids);

} // namespace KtorPlugins
