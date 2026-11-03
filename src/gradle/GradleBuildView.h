#pragma once

#include "gradle/GradleEvents.h"
#include "terminal/ProblemScanner.h"

#include <QWidget>

class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

// The Gradle panel's Build tab: the tasks of the last build QODE started, in order, with their result (executed,
// up-to-date, from cache, skipped, failed) and time drawn as bars; failed tasks carry Gradle's message and the compiler
// errors the terminal printed meanwhile, which open their file.
class GradleBuildView : public QWidget
{
    Q_OBJECT
public:
    explicit GradleBuildView(QWidget *parent = nullptr);

    void setRoot(const QString &root);
    // A Gradle command was just started: follow the events file until the build ends.
    void follow();
    void refresh();
    // Compiler errors from the terminal; kept with the build that is running (or the last one).
    void addProblems(const QList<BuildProblem> &problems);

signals:
    void locationRequested(const QString &path, int line, int column);
    void buildFinished(bool failed);

private:
    void populate();

    QString m_root;
    GradleEvents::Build m_build;
    QList<BuildProblem> m_problems;
    qint64 m_followSince = 0; // ms; problems that arrive after it belong to the build being followed
    bool m_announced = true;
    QTimer *m_poll;
    QLabel *m_summary;
    QTreeWidget *m_tree;
};
