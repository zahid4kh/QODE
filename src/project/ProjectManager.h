#pragma once

#include "Project.h"

#include <QObject>

class ProjectManager : public QObject
{
    Q_OBJECT
public:
    enum class State { NoProject, Opening, Open, Closing };

    explicit ProjectManager(QObject *parent = nullptr);

    State state() const { return m_state; }
    bool hasProject() const { return m_state == State::Open; }
    const Project &project() const { return m_project; }
    QString root() const { return m_project.root; }

    // Create `location/name` then open it.
    bool createProject(const QString &name, const QString &location, QString *error);
    // Opens any directory.
    bool openProject(const QString &path, QString *error);
    void closeProject();

signals:
    void projectOpened(const Project &project);
    void projectAboutToClose(const Project &project);
    void projectClosed();

private:
    State m_state = State::NoProject;
    Project m_project;
};
