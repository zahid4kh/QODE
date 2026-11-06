#include "platform/ProcessTree.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QProcess>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

// Windows: a Job Object per tracked process. The process is created inside its job (PROC_THREAD_ATTRIBUTE_JOB_LIST, so
// not even a quick child can escape before it is assigned), and everything it starts lands in the same job. The job has
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: QODE holds the only handle, so when QODE ends in any way (a crash, Task Manager)
// Windows closes it and ends the tree. That is what the watchdog process does on Unix, for free.
//
// When the tracked process ends by itself, the flag is dropped before the handle is closed: like on Unix, where the
// watchdog forgets a group once its leader has exited, what it left running is not ended.

namespace {

QMutex s_lock;
QHash<qint64, HANDLE> s_jobs; // root pid -> its job, while the root runs

// `root` and every process below it, children before their parents. Only for a pid that has no job (not tracked).
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

void terminateTree(DWORD pid)
{
    const QList<DWORD> tree = treeOf(pid);
    for (const DWORD p : tree) {
        const HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, p);
        if (!h)
            continue;
        ::TerminateProcess(h, 1);
        ::CloseHandle(h);
    }
}

HANDLE createJob()
{
    const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    // BREAKAWAY_OK: a tool that deliberately starts something outside its job (CREATE_BREAKAWAY_FROM_JOB) still can.
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info)) {
        ::CloseHandle(job);
        return nullptr;
    }
    return job;
}

// The root ended (or its QProcess is gone): forget the job without ending what is still in it.
void releaseJob(qint64 pid)
{
    HANDLE job = nullptr;
    {
        QMutexLocker lock(&s_lock);
        job = s_jobs.take(pid);
    }
    if (!job)
        return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info);
    ::CloseHandle(job);
}

HANDLE jobOf(qint64 pid)
{
    QMutexLocker lock(&s_lock);
    return s_jobs.value(pid, nullptr);
}

const char kCtrlCArg[] = "--qode-ctrl-c";

// What a stop does by hand in a terminal: Ctrl+C. QODE has no console, so each child QProcess starts got a hidden one
// of its own, shared by everything below it. A Ctrl+C on that console reaches the whole tree; Node, the JVM (Gradle,
// Spring Boot), Python and cmd all end on it (CTRL_BREAK would only make a JVM print a thread dump).
//
// Sending it means attaching to that console, and the event then reaches the sender as well: tried in QODE itself with
// a handler that swallows it, QODE still ended. So a helper sends it: this binary again, started with no console of its
// own (see runIfWatchdog). It is not waited for; callers kill() after their timeout anyway.
bool sendCtrlC(DWORD pid)
{
    // QODE started from a console (a debugger): the children share it with QODE and every other child, so a Ctrl+C
    // there would reach all of them. Only the hard stop is safe then.
    if (::GetConsoleWindow())
        return false;
    wchar_t exe[MAX_PATH * 4];
    const DWORD len = ::GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (len == 0 || len >= std::size(exe))
        return false;
    std::wstring cmd = L"\"" + std::wstring(exe, len) + L"\" --qode-ctrl-c " + std::to_wstring(pid);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        return false;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

} // namespace

bool ProcessTree::runIfWatchdog(int argc, char *argv[])
{
    // No watchdog process (the jobs end the trees), only the Ctrl+C helper of sendCtrlC().
    if (argc != 3 || std::strcmp(argv[1], kCtrlCArg) != 0)
        return false;
    const DWORD pid = DWORD(std::strtoul(argv[2], nullptr, 10));
    // Started with DETACHED_PROCESS: no console yet. The helper ignores the Ctrl+C it sends; that setting is inherited,
    // but the helper starts nothing.
    if (pid > 0 && ::AttachConsole(pid)) {
        ::SetConsoleCtrlHandler(nullptr, TRUE);
        ::GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        ::FreeConsole();
    }
    return true;
}

void ProcessTree::track(QProcess *process)
{
    struct Start {
        HANDLE job = nullptr;
        std::vector<char> attrs; // the PROC_THREAD_ATTRIBUTE_LIST
        STARTUPINFOEXW info = {};
        qint64 pid = 0;
    };
    auto start = std::make_shared<Start>();

    process->setCreateProcessArgumentsModifier([start](QProcess::CreateProcessArguments *args) {
        if (start->job) // a QProcess started again: the old job was released when it stopped
            ::CloseHandle(std::exchange(start->job, nullptr));
        start->job = createJob();
        if (!start->job)
            return; // runs untracked rather than not at all
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        start->attrs.assign(size, 0);
        auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(start->attrs.data());
        if (!::InitializeProcThreadAttributeList(list, 1, 0, &size)
            || !::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &start->job, sizeof(HANDLE),
                                            nullptr, nullptr)) {
            ::CloseHandle(std::exchange(start->job, nullptr));
            return;
        }
        // Qt's STARTUPINFOW (its std handles) becomes the head of an extended one that carries the job.
        start->info.StartupInfo = *args->startupInfo;
        start->info.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        start->info.lpAttributeList = list;
        args->startupInfo = &start->info.StartupInfo;
        args->flags |= EXTENDED_STARTUPINFO_PRESENT;
    });

    QObject::connect(process, &QProcess::started, process, [process, start] {
        if (!start->job)
            return;
        start->pid = process->processId();
        QMutexLocker lock(&s_lock);
        s_jobs.insert(start->pid, std::exchange(start->job, nullptr));
    });
    QObject::connect(process, &QProcess::stateChanged, process, [start](QProcess::ProcessState s) {
        if (s != QProcess::NotRunning)
            return;
        if (start->pid > 0)
            releaseJob(std::exchange(start->pid, 0));
        if (start->job) // failed to start
            ::CloseHandle(std::exchange(start->job, nullptr));
    });
    QObject::connect(process, &QObject::destroyed, [start] {
        if (start->pid > 0)
            releaseJob(std::exchange(start->pid, 0));
        if (start->job)
            ::CloseHandle(std::exchange(start->job, nullptr));
    });
}

void ProcessTree::stop(qint64 pid)
{
    if (pid <= 0)
        return;
    // Callers kill() after their timeout if the tree is still there. Without a console to send Ctrl+C to, end it now.
    if (!sendCtrlC(DWORD(pid)))
        kill(pid);
}

void ProcessTree::kill(qint64 pid)
{
    if (pid <= 0)
        return;
    if (const HANDLE job = jobOf(pid))
        ::TerminateJobObject(job, 1);
    else
        terminateTree(DWORD(pid));
}
