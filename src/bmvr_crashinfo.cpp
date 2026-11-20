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

    struct CheckJob { char map[64]; };
    DWORD WINAPI HeapCheckThread(LPVOID p)
    {
        CheckJob* job = static_cast<CheckJob*>(p);
        static const DWORD kAt[] = { 1000, 3000, 6000, 10000 };
        DWORD slept = 0;
        for (DWORD at : kAt)
        {
            Sleep(at - slept);
            slept = at;
            char where[96];
            snprintf(where, sizeof(where), "%s +%lu s", job->map, at / 1000);
            bmvr::crashinfo::CheckHeaps(where);
        }
        delete job;
        return 0;
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

    bool CheckHeaps(const char* where)
    {
        HANDLE heaps[64];
        const DWORD n = GetProcessHeaps(64, heaps);
        const ULONGLONG t0 = GetTickCount64();
        DWORD bad = 0, firstBad = 0;
        for (DWORD i = 0; i < n && i < 64; ++i)
        {
            if (!HeapValidate(heaps[i], 0, nullptr))
            {
                if (!bad)
                    firstBad = i;
                ++bad;
            }
        }
        const unsigned long ms = static_cast<unsigned long>(GetTickCount64() - t0);
        if (bad)
        {
            bmvr::Log("Heap check %s: %lu of %lu heaps CORRUPT (first #%lu) (%lu ms)", where ? where : "?", bad, n, firstBad, ms);
            char line[200];
            snprintf(line, sizeof(line), "heap check %s: %lu of %lu heaps CORRUPT tick=%lu\n", where ? where : "?", bad, n, GetTickCount());
            AppendLine(L"bmvr_fault.txt", line);
        }
        else
            bmvr::Log("Heap check %s: %lu heaps ok (%lu ms)", where ? where : "?", n, ms);
        return bad == 0;
    }

    void ScheduleHeapChecks(const char* map)
    {
        CheckJob* job = new CheckJob{};
        strncpy_s(job->map, map ? map : "?", _TRUNCATE);
        HANDLE t = CreateThread(nullptr, 0, HeapCheckThread, job, 0, nullptr);
        if (t)
            CloseHandle(t);
        else
            delete job;
    }

    void NoteDetach(bool processExit)
    {
        char line[160];
        snprintf(line, sizeof(line), "process detach (%s) tick=%lu\n", processExit ? "process exit" : "FreeLibrary", GetTickCount());
        AppendLine(L"bmvr_fault.txt", line);
    }
}
