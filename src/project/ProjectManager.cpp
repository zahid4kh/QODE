#include "ProjectManager.h"

#include "filesystem/FileManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

ProjectManager::ProjectManager(QObject *parent)
    : QObject(parent)
{
}

bool ProjectManager::createProject(const QString &name, const QString &location, QString *error)
{
    const QString nameErr = FileManager::validateName(name);
    if (!nameErr.isEmpty()) {
        if (error)
            *error = nameErr;
        return false;
    }
    const QFileInfo loc(location);
    if (location.trimmed().isEmpty() || (loc.exists() && !loc.isDir())) {
        if (error)
            *error = QStringLiteral("The project location is not a valid directory.");
        return false;
    }

    const QString path = QDir(location).filePath(name.trimmed());
    const QFileInfo target(path);
    if (target.exists() && !target.isDir()) {
        if (error)
            *error = QStringLiteral("A file named \"%1\" already exists at that location.").arg(name);
        return false;
    }
    if (!QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("Could not create directory %1 (permission denied?).").arg(path);
        return false;
    }

    return openProject(path, error);
}

bool ProjectManager::openProject(const QString &path, QString *error)
{
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isDir()) {
        if (error)
            *error = QStringLiteral("\"%1\" is not a directory.").arg(path);
        return false;
    }
    if (!fi.isReadable() || !fi.isExecutable()) {
        if (error)
            *error = QStringLiteral("Permission denied reading \"%1\".").arg(path);
        return false;
    }

    if (m_state == State::Open)
        closeProject();

    m_state = State::Opening;
    Project p;
    p.root = fi.canonicalFilePath();
    p.name = fi.fileName().isEmpty() ? p.root : QFileInfo(p.root).fileName();

    m_project = p;
    m_state = State::Open;
    emit projectOpened(m_project);
    return true;
}

void ProjectManager::closeProject()
{
    if (m_state != State::Open)
        return;
    m_state = State::Closing;
    emit projectAboutToClose(m_project);
    m_project = Project();
    m_state = State::NoProject;
    emit projectClosed();
}
