#include <Windows.h>
#include "game.h"
#include "bmvr_flags.h"
#include "bmvr_version.h"
#include <cstdint>
#include <cstring>
#include "bmvr_seh.h"
#include "bmvr_crashinfo.h"

static LONG CALLBACK FaultVeh(PEXCEPTION_POINTERS info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Fatal codes too (heap corruption, fail fast, stack overflow): those
    // end the process and were never written (user 2026-11-20: a crash on a
    // save load left no fault line). They also get a minidump.
    const bool fatal = bmvr::crashinfo::IsFatalCode(code);
    if (code != 0xC0000005 && code != 0xC000001D && code != 0xC0000094 && !fatal)
        return EXCEPTION_CONTINUE_SEARCH;
    if (fatal)
        bmvr::crashinfo::DumpNow(info, "fatal exception code");
    // 32 distinct addresses per process (was 8: a run's real crash could be
    // past the cap behind caught faults).
    static DWORD seen[32];
    static volatile LONG count = 0;
    const DWORD eip = info->ContextRecord->Eip;
    const LONG n = InterlockedIncrement(&count);
    if (n > 32 && !fatal)
        return EXCEPTION_CONTINUE_SEARCH;
    if (n > 32)
        InterlockedExchange(&count, 32);
    const LONG slot = n > 32 ? 31 : n - 1;
    seen[slot] = eip;
    for (LONG i = 0; i < slot; ++i)
    {
        if (seen[i] == eip)
            return EXCEPTION_CONTINUE_SEARCH;
    }

    char body[1600];
    int used = snprintf(body, sizeof(body), "fault eip=%08X code=%08X tid=%lu tick=%lu\n", eip, code,
        GetCurrentThreadId(), GetTickCount());
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(eip)), &mod) && mod)
    {
        char modPath[MAX_PATH]{};
        GetModuleFileNameA(mod, modPath, MAX_PATH);
        const char* base = strrchr(modPath, '\\');
        base = base ? base + 1 : modPath;
        used += snprintf(body + used, sizeof(body) - static_cast<size_t>(used),
            "  at %s+%08X\n", base, eip - reinterpret_cast<DWORD>(mod));
    }
    DWORD ebp = info->ContextRecord->Ebp;
    for (int frame = 0; frame < 12 && used < static_cast<int>(sizeof(body)) - 80; ++frame)
    {
        DWORD next = 0, ret = 0;
        BMVR_TRY
        {
            next = *reinterpret_cast<DWORD*>(static_cast<uintptr_t>(ebp));
            ret = *reinterpret_cast<DWORD*>(static_cast<uintptr_t>(ebp + 4));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        HMODULE sm = nullptr;
        if (ret && GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ret)), &sm) && sm)
        {
            char modPath[MAX_PATH]{};
            GetModuleFileNameA(sm, modPath, MAX_PATH);
            const char* base = strrchr(modPath, '\\');
            base = base ? base + 1 : modPath;
            used += snprintf(body + used, sizeof(body) - static_cast<size_t>(used),
                "  #%d %s+%08X\n", frame, base, ret - reinterpret_cast<DWORD>(sm));
        }
        if (next <= ebp)
            break;
        ebp = next;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return EXCEPTION_CONTINUE_SEARCH;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"bmvr_fault.txt");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD wrote = 0;
        WriteFile(h, body, static_cast<DWORD>(used), &wrote, nullptr);
        CloseHandle(h);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void __cdecl L4D2VR_ShutdownSystemMouseInputSuppression();
extern "C" void __cdecl L4D2VR_ShutdownReShadeVRBridge();

static BOOL CALLBACK RestoreMonitorMode(HMONITOR monitor, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)))
        return TRUE;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_REGISTRY_SETTINGS, &mode))
        return TRUE;
    ChangeDisplaySettingsExW(info.szDevice, &mode, nullptr, 0, nullptr);
    return TRUE;
}

static void RestoreDesktopDisplayModes()
{
    EnumDisplayMonitors(nullptr, nullptr, RestoreMonitorMode, 0);
}

// Counts access violations (caught or not) so a guarded read that faults every
// frame shows up in bmvr_log; under Wine each one costs a kernel round trip.
namespace
{
    volatile LONG g_FaultCount = 0;
    constexpr int kFaultStacks = 6;
    constexpr int kFaultStackDepth = 8;
    struct FaultStack
    {
        uint32_t addr, target, tid, espTop;
        uint32_t ret[kFaultStackDepth];
        volatile LONG ready, logged;
    };
    FaultStack g_FaultStacks[kFaultStacks]{};
    volatile LONG g_FaultStackNext = 0;
    struct FaultSlot { volatile LONG addr; volatile LONG hits; };
    FaultSlot g_FaultSlots[8]{};

    LONG CALLBACK CountFaults(PEXCEPTION_POINTERS info)
    {
        if (info && info->ExceptionRecord
            && info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        {
            InterlockedIncrement(&g_FaultCount);
            const LONG addr = static_cast<LONG>(reinterpret_cast<uintptr_t>(
                info->ExceptionRecord->ExceptionAddress));
            for (FaultSlot& s : g_FaultSlots)
            {
                if (s.addr == addr || InterlockedCompareExchange(&s.addr, addr, 0) == 0)
                {
                    InterlockedIncrement(&s.hits);
                    break;
                }
            }
            // First few faults: keep the EBP chain (return addresses) so the
            // report names the caller, not only the faulting instruction.
            // ReadProcessMemory: no SEH inside the handler.
            const LONG slot = InterlockedIncrement(&g_FaultStackNext) - 1;
            if (slot < kFaultStacks && info->ContextRecord)
            {
                FaultStack& fs = g_FaultStacks[slot];
                fs.addr = static_cast<uint32_t>(addr);
                fs.target = static_cast<uint32_t>(info->ExceptionRecord->ExceptionInformation[1]);
                fs.tid = GetCurrentThreadId();
                uint32_t ebp = static_cast<uint32_t>(info->ContextRecord->Ebp);
                // Leaf frame: the faulting function may not have pushed EBP
                // yet; [esp] is its return address in that case.
                SIZE_T got = 0;
                uint32_t top = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(info->ContextRecord->Esp)),
                        &top, sizeof(top), &got) && got == sizeof(top))
                    fs.espTop = top;
                for (int i = 0; i < kFaultStackDepth && ebp; ++i)
                {
                    uint32_t frame[2] = {};
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(ebp)),
                            frame, sizeof(frame), &got) || got != sizeof(frame))
                        break;
                    fs.ret[i] = frame[1];
                    if (frame[0] <= ebp)
                        break;
                    ebp = frame[0];
                }
                fs.ready = 1;
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void DescribeAddr(uint32_t a, char* out, size_t n)
    {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(a)), &mod) && mod)
        {
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = std::strrchr(name, '\\');
            sprintf_s(out, n, "%s+0x%X", base ? base + 1 : name,
                static_cast<unsigned>(a - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mod))));
        }
        else
            sprintf_s(out, n, "0x%08X", static_cast<unsigned>(a));
    }
}

namespace bmvr
{
    void LogFaultStats()
    {
        const LONG n = InterlockedExchange(&g_FaultCount, 0);
        LONG topAddr = 0, topHits = 0;
        for (FaultSlot& s : g_FaultSlots)
        {
            const LONG hits = InterlockedExchange(&s.hits, 0);
            const LONG addr = InterlockedExchange(&s.addr, 0);
            if (hits > topHits)
            {
                topHits = hits;
                topAddr = addr;
            }
        }
        if (n == 0)
            return;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        uintptr_t rva = static_cast<uintptr_t>(topAddr);
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(topAddr)), &mod) && mod)
        {
            GetModuleFileNameA(mod, name, MAX_PATH);
            rva -= reinterpret_cast<uintptr_t>(mod);
        }
        const char* base = std::strrchr(name, '\\');
        Log("Access violations since last report: %ld, top %ld at %s+0x%X",
            n, topHits, base ? base + 1 : name, static_cast<unsigned>(rva));
        for (FaultStack& fs : g_FaultStacks)
        {
            if (!fs.ready || InterlockedExchange(&fs.logged, 1))
                continue;
            char where[160], top[160], line[1400];
            DescribeAddr(fs.addr, where, sizeof(where));
            DescribeAddr(fs.espTop, top, sizeof(top));
            int len = sprintf_s(line, "Fault stack: %s reading 0x%08X tid=%u [esp]=%s ebp-chain:",
                where, static_cast<unsigned>(fs.target), static_cast<unsigned>(fs.tid), top);
            for (int i = 0; i < kFaultStackDepth && fs.ret[i] && len > 0 && len < static_cast<int>(sizeof(line)) - 170; ++i)
            {
                char r[160];
                DescribeAddr(fs.ret[i], r, sizeof(r));
                len += sprintf_s(line + len, sizeof(line) - len, " %s", r);
            }
            Log("%s", line);
        }
    }
}

static DWORD WINAPI InitBMVR(LPVOID param)
{
#ifdef _DEBUG
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif
    // Three install copies (game root, bin\, DXVK folder) can map as distinct
    // modules. A second Game()/pose waiter calling WaitGetPoses wedged the
    // load-to-menu (2026-08-18, two "BMVR d3d9.dll loaded" then no LevelInit).
    HANDLE initGate = CreateMutexW(nullptr, TRUE, L"Local\\BlackMesaVR-InitBMVR");
    if (!initGate)
    {
        bmvr::Log("InitBMVR CreateMutex failed err=%lu", GetLastError());
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(static_cast<HMODULE>(param), path, MAX_PATH);
        bmvr::Log("InitBMVR skipped duplicate d3d9.dll %s", path[0] ? path : "?");
        ReleaseMutex(initGate);
        CloseHandle(initGate);
        return 0;
    }

    // First log line: lets a shared bmvr_log.txt name the exact build.
    bmvr::Log("Black Mesa VR %s (commit %s %s, built %s %s)",
        BMVR_RELEASE, BMVR_COMMIT, BMVR_COMMIT_DATE, __DATE__, __TIME__);
    bmvr::InitFromDisk();
    char path[MAX_PATH]{};
    GetModuleFileNameA(static_cast<HMODULE>(param), path, MAX_PATH);
    bmvr::Log("BMVR d3d9.dll loaded (L4D2VR architecture, Black Mesa offsets) from %s",
        path[0] ? path : "?");
    // One caught fault on purpose: checks BMVR_TRY keeps EBX/ESI/EDI (bmvr_seh.h).
    bmvr::seh::SelfTest();
    // Test switch: hold back the hooks so the engine's first render target pass runs without them, the startup
    // order some real launches get (bloom targets eye-sized). Harness: BMVR_TEST_INIT_DELAY_MS.
    char delayText[16]{};
    if (GetEnvironmentVariableA("BMVR_TEST_INIT_DELAY_MS", delayText, sizeof(delayText)) > 0)
    {
        const int delayMs = atoi(delayText);
        bmvr::Log("Test: mod init held back %d ms", delayMs);
        Sleep(delayMs > 0 ? delayMs : 0);
    }
    g_Game = new Game();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // DXVK NormalizePresentParameters honors this. Exclusive fullscreen
        // after VR_Init caused a ChangeDisplaySettings Reset loop, 1 FPS,
        // crash, and a leftover lower desktop resolution (2026-08-16).
        SetEnvironmentVariableA("DXVK_FORCE_WINDOWED", "1");
        bmvr::SetDllModule(hModule);
        AddVectoredExceptionHandler(1, FaultVeh);
        AddVectoredExceptionHandler(1, CountFaults);
        bmvr::crashinfo::Install();
        bmvr::StartWatchdog();
        bmvr::SetStage("dll_attach");
        CreateThread(nullptr, 0, InitBMVR, hModule, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        bmvr::crashinfo::NoteDetach(reserved != nullptr);
        RestoreDesktopDisplayModes();
        L4D2VR_ShutdownReShadeVRBridge();
        L4D2VR_ShutdownSystemMouseInputSuppression();
        break;
    default:
        break;
    }
    return TRUE;
}
