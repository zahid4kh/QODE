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

    Project p;
    p.root = QFileInfo(path).canonicalFilePath();
    p.name = name.trimmed();
    if (!QDir().mkpath(p.metadataDir())) {
        if (error)
            *error = QStringLiteral("Could not create the .qode directory in %1.").arg(path);
        return false;
    }
    writeMetadata(p);
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

    // Optional metadata: use the stored name if present and well-formed.
    QFile meta(p.metadataFile());
    if (meta.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(meta.readAll()).object();
        const QString n = o.value(QStringLiteral("name")).toString();
        if (!n.isEmpty())
            p.name = n;
    }

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

void ProjectManager::writeMetadata(const Project &p) const
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), p.name);
    o.insert(QStringLiteral("version"), 1);
    FileManager::writeFile(p.metadataFile(), QJsonDocument(o).toJson(QJsonDocument::Indented), nullptr);
}
