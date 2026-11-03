#pragma once

#include "debug/DebugTypes.h"

#include <QHash>
#include <QWidget>

#include <functional>

class DebugSession;
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;

// The debugger's panel (right-hand island): what the session is doing, the step buttons, the stopped thread's frames,
// the variables of the selected frame (objects open on demand: fields, collection elements, map entries), an
// evaluate box and the list of breakpoints.
class DebugPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DebugPanel(DebugSession *session, QWidget *parent = nullptr);

    // The toolbar buttons (resume, pause, step over / into / out, stop): the window's actions, so their shortcuts work everywhere.
    void setActions(const QList<QAction *> &actions);
    // A frame's class package path and source name -> the project file ("" when it is not one).
    void setResolver(std::function<QString(const QString &pkgPath, const QString &sourceName)> resolver) { m_resolve = std::move(resolver); }
    // Breakpoints of all files: path -> 1-based lines.
    void setBreakpoints(const QHash<QString, QList<int>> &all);
    bool breakOnUncaught() const;
    void setTarget(const QString &text); // what is being debugged ("MainKt", "script.main.kts" …)

signals:
    void frameShown(const QString &path, int line); // the selected frame: show the line in the editor ("" path = not a project file)
    void locationRequested(const QString &path, int line);
    void removeBreakpointRequested(const QString &path, int line);
    void clearBreakpointsRequested();
    void breakOnUncaughtChanged(bool on);
    void closeRequested();

private:
    void onStateChanged();
    void onStopped(const DebugStop &stop);
    void onVariables(int token, const QList<DebugVariable> &vars);
    void selectFrame(int index);
    void fillItem(QTreeWidgetItem *item, const DebugVariable &v);
    void evaluate();

    DebugSession *m_session;
    std::function<QString(const QString &, const QString &)> m_resolve;
    QLabel *m_status;
    QLabel *m_exception;
    QWidget *m_buttons;
    QComboBox *m_threads;
    QListWidget *m_frames;
    QTreeWidget *m_vars;
    QLineEdit *m_eval;
    QListWidget *m_breakpoints;
    QCheckBox *m_uncaught;
    QString m_target;
    QList<DebugFrame> m_frameData;
    quint64 m_thread = 0;
    int m_frameToken = 0;
    int m_evalToken = 0;
    QHash<int, QTreeWidgetItem *> m_pending; // expand token -> item waiting for its children
    bool m_fillingThreads = false;
};
