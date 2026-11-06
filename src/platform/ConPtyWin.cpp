#include "platform/ConPty.h"

#include <QProcess>

#include <memory>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The ConPTY functions are looked up at run time: the SDK only declares them for NTDDI_WIN10_RS5 and later, and a
// Windows without them (before 10 1809) then gets an error message in the terminal instead of a QODE that won't start.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

namespace {

using CreateFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, void **);
using ResizeFn = HRESULT(WINAPI *)(void *, COORD);
using CloseFn = void(WINAPI *)(void *);

struct Api {
    CreateFn create = nullptr;
    ResizeFn resize = nullptr;
    CloseFn close = nullptr;
};

const Api &api()
{
    static const Api a = [] {
        Api r;
        if (const HMODULE k = ::GetModuleHandleW(L"kernel32.dll")) {
            r.create = reinterpret_cast<CreateFn>(reinterpret_cast<void *>(::GetProcAddress(k, "CreatePseudoConsole")));
            r.resize = reinterpret_cast<ResizeFn>(reinterpret_cast<void *>(::GetProcAddress(k, "ResizePseudoConsole")));
            r.close = reinterpret_cast<CloseFn>(reinterpret_cast<void *>(::GetProcAddress(k, "ClosePseudoConsole")));
        }
        return r;
    }();
    return a;
}

COORD size(int cols, int rows)
{
    return COORD{SHORT(qBound(1, cols, 32767)), SHORT(qBound(1, rows, 32767))};
}

void closeHandle(void *&h)
{
    if (h) {
        ::CloseHandle(h);
        h = nullptr;
    }
}

} // namespace

ConPty::ConPty(QObject *parent)
    : QObject(parent)
{
}

ConPty::~ConPty()
{
    close();
}

bool ConPty::open(int cols, int rows, QString *error)
{
    const Api &a = api();
    if (!a.create || !a.resize || !a.close) {
        if (error)
            *error = QStringLiteral("This version of Windows has no pseudo console (ConPTY needs Windows 10 1809 or later).");
        return false;
    }
    // Two anonymous pipes, not inheritable: the console gets its own copies of its ends, and no other child QODE
    // starts can keep them open.
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!::CreatePipe(&inRead, &inWrite, nullptr, 0) || !::CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        if (error)
            *error = QStringLiteral("Could not create the console pipes (error %1).").arg(::GetLastError());
        for (HANDLE h : {inRead, inWrite, outRead, outWrite})
            if (h)
                ::CloseHandle(h);
        return false;
    }
    void *console = nullptr;
    const HRESULT hr = a.create(size(cols, rows), inRead, outWrite, 0, &console);
    // The console duplicated its ends: ours are not needed any more, and the output pipe must break when it ends.
    ::CloseHandle(inRead);
    ::CloseHandle(outWrite);
    if (FAILED(hr)) {
        if (error)
            *error = QStringLiteral("Could not create a pseudo console (0x%1).").arg(quint32(hr), 8, 16, QLatin1Char('0'));
        ::CloseHandle(inWrite);
        ::CloseHandle(outRead);
        return false;
    }
    m_console = console;
    m_input = inWrite;
    m_output = outRead;

    // Anonymous pipes cannot be waited on for data, so a thread blocks in ReadFile and hands the chunks over. It ends
    // when the console has gone (the pipe breaks), which close() waits for.
    m_reader = std::thread([this, out = HANDLE(m_output)] {
        char buf[65536];
        DWORD n = 0;
        while (::ReadFile(out, buf, sizeof buf, &n, nullptr) && n > 0) {
            bool notify = false;
            {
                QMutexLocker lock(&m_lock);
                m_buffer.append(buf, qsizetype(n));
                notify = !std::exchange(m_notified, true);
            }
            if (notify)
                QMetaObject::invokeMethod(this, &ConPty::readyRead, Qt::QueuedConnection);
        }
    });
    return true;
}

void ConPty::attach(QProcess *process)
{
    struct Start {
        std::vector<char> attrs; // the PROC_THREAD_ATTRIBUTE_LIST
        STARTUPINFOEXW info = {};
    };
    auto start = std::make_shared<Start>();
    void *const console = m_console;
    process->setCreateProcessArgumentsModifier([start, console](QProcess::CreateProcessArguments *args) {
        if (!console)
            return;
        SIZE_T bytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        start->attrs.assign(bytes, 0);
        auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(start->attrs.data());
        if (!::InitializeProcThreadAttributeList(list, 1, 0, &bytes)
            || !::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof console,
                                            nullptr, nullptr))
            return;
        // The console's standard handles, not QProcess's pipes. No handles are inherited: the console needs none.
        start->info.StartupInfo = *args->startupInfo;
        start->info.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        start->info.StartupInfo.dwFlags &= ~DWORD(STARTF_USESTDHANDLES);
        start->info.StartupInfo.hStdInput = nullptr;
        start->info.StartupInfo.hStdOutput = nullptr;
        start->info.StartupInfo.hStdError = nullptr;
        start->info.lpAttributeList = list;
        args->startupInfo = &start->info.StartupInfo;
        args->inheritHandles = FALSE;
        // Qt asks for CREATE_NO_WINDOW in a GUI app; with it the process would not be put on the pseudo console.
        args->flags &= ~DWORD(CREATE_NO_WINDOW | CREATE_NEW_CONSOLE | DETACHED_PROCESS);
        args->flags |= EXTENDED_STARTUPINFO_PRESENT;
    });
}

void ConPty::write(const QByteArray &data)
{
    const char *p = data.constData();
    qsizetype left = data.size();
    while (m_input && left > 0) {
        DWORD n = 0;
        if (!::WriteFile(m_input, p, DWORD(qMin<qsizetype>(left, 1 << 20)), &n, nullptr) || n == 0)
            return; // the console has gone
        p += n;
        left -= n;
    }
}

void ConPty::resize(int cols, int rows)
{
    if (m_console)
        api().resize(m_console, size(cols, rows));
}

QByteArray ConPty::takeOutput()
{
    QMutexLocker lock(&m_lock);
    m_notified = false;
    return std::exchange(m_buffer, QByteArray());
}

void ConPty::close()
{
    // Closing the input first is what Windows Terminal does; ClosePseudoConsole may still send a last frame, which the
    // reader drains (on older Windows it would otherwise wait for that forever).
    closeHandle(m_input);
    if (m_console) {
        api().close(m_console);
        m_console = nullptr;
    }
    if (m_reader.joinable())
        m_reader.join();
    closeHandle(m_output);
}
