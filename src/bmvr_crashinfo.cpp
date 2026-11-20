#include "bmvr_crashinfo.h"
#include "bmvr_flags.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    LPTOP_LEVEL_EXCEPTION_FILTER g_PrevFilter = nullptr;
    volatile LONG g_Dumped = 0;

    std::wstring ExeDirW()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash)
            *slash = 0;
        return path;
    }

    void AppendLine(const wchar_t* file, const char* text)
    {
        const std::wstring path = ExeDirW() + L"\\" + file;
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;
        DWORD wrote = 0;
        WriteFile(h, text, static_cast<DWORD>(strlen(text)), &wrote, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    }

    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    struct ExceptionInfoLite { DWORD threadId; void* pointers; BOOL clientPointers; };

    LONG WINAPI Unhandled(PEXCEPTION_POINTERS info)
    {
        char line[256];
        const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
        void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
        snprintf(line, sizeof(line), "unhandled exception code=%08lX at %p tid=%lu tick=%lu\n",
            code, addr, GetCurrentThreadId(), GetTickCount());
        AppendLine(L"bmvr_fault.txt", line);
        bmvr::crashinfo::DumpNow(info, "unhandled");
        return g_PrevFilter ? g_PrevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    }

}

namespace bmvr::crashinfo
{
    void Install()
    {
        g_PrevFilter = SetUnhandledExceptionFilter(Unhandled);
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char line[256];
        snprintf(line, sizeof(line), "=== run %04u-%02u-%02u %02u:%02u:%02u pid=%lu ===\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
        AppendLine(L"bmvr_fault.txt", line);
    }

    bool IsFatalCode(unsigned long code)
    {
        return code == 0xC0000374     // STATUS_HEAP_CORRUPTION
            || code == 0xC0000409     // STATUS_STACK_BUFFER_OVERRUN (fail fast)
            || code == 0xC00000FD     // STATUS_STACK_OVERFLOW
            || code == 0xC0000420     // STATUS_ASSERTION_FAILURE
            || code == 0xC0000602;    // STATUS_FAIL_FAST_EXCEPTION
    }

    void DumpNow(void* exceptionPointers, const char* why)
    {
        if (InterlockedCompareExchange(&g_Dumped, 1, 0) != 0)
            return;
        HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
        auto write = dbg ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf(name, 64, L"\\bmvr_crash-%04u%02u%02u-%02u%02u%02u.dmp",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        const std::wstring path = ExeDirW() + name;
        bool ok = false;
        if (write)
        {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                ExceptionInfoLite ei{ GetCurrentThreadId(), exceptionPointers, FALSE };
                // MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory
                // | MiniDumpWithThreadInfo: stacks and what they point at,
                // without the whole process (GBs).
                ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1 | 0x40 | 0x1000,
                    exceptionPointers ? &ei : nullptr, nullptr, nullptr) != FALSE;
                CloseHandle(f);
            }
        }
        char line[400];
        snprintf(line, sizeof(line), "minidump (%s): %ls %s\n", why ? why : "?", path.c_str(),
            ok ? "written" : (write ? "FAILED" : "no dbghelp"));
        AppendLine(L"bmvr_fault.txt", line);
    }

    void NoteDetach(bool processExit)
    {
        char line[160];
        snprintf(line, sizeof(line), "process detach (%s) tick=%lu\n", processExit ? "process exit" : "FreeLibrary", GetTickCount());
        AppendLine(L"bmvr_fault.txt", line);
    }
}
