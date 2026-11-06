#include "platform/ProcessTree.h"

#include <QHash>
#include <QList>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

// First version: walks the process list for the tree and terminates it. Phase 2 of the Windows port replaces this
// with a Job Object per tracked process (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so Windows itself ends the tree when
// QODE dies) and a graceful stop (CTRL_BREAK_EVENT) before the kill.

namespace {

// `root` and every process below it, children before their parents. A parent pid can be stale (its process ended
// and the id was reused); a child created before the process it claims as parent is skipped.
QList<DWORD> treeOf(DWORD root)
{
    QList<DWORD> out;
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        out << root;
        return out;
    }
    QMultiHash<DWORD, DWORD> children;
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    for (BOOL ok = ::Process32FirstW(snap, &e); ok; ok = ::Process32NextW(snap, &e))
        if (e.th32ProcessID != e.th32ParentProcessID)
            children.insert(e.th32ParentProcessID, e.th32ProcessID);
    ::CloseHandle(snap);

    // Breadth first from the root, then reversed: the deepest processes end first, so none is restarted by its parent.
    QList<DWORD> order{root};
    for (int i = 0; i < order.size() && order.size() < 4096; ++i) {
        const auto kids = children.values(order.at(i));
        for (const DWORD kid : kids)
            if (!order.contains(kid))
                order << kid;
    }
    for (auto it = order.crbegin(); it != order.crend(); ++it)
        out << *it;
    return out;
}

void terminateTree(qint64 pid)
{
    if (pid <= 0)
        return;
    const QList<DWORD> tree = treeOf(DWORD(pid));
    for (const DWORD p : tree) {
        const HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, p);
        if (!h)
            continue;
        ::TerminateProcess(h, 1);
        ::CloseHandle(h);
    }
}

} // namespace

bool ProcessTree::runIfWatchdog(int, char *[])
{
    return false; // no watchdog process on Windows
}

void ProcessTree::track(QProcess *)
{
}

void ProcessTree::stop(qint64 pid)
{
    terminateTree(pid);
}

void ProcessTree::kill(qint64 pid)
{
    terminateTree(pid);
}
