#include <Windows.h>
#include "game.h"
#include "bmvr_flags.h"
#include <cstdint>
#include <cstring>

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
        }
        return EXCEPTION_CONTINUE_SEARCH;
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

    bmvr::InitFromDisk();
    char path[MAX_PATH]{};
    GetModuleFileNameA(static_cast<HMODULE>(param), path, MAX_PATH);
    bmvr::Log("BMVR d3d9.dll loaded (L4D2VR architecture, Black Mesa offsets) from %s",
        path[0] ? path : "?");
    g_Game = new Game();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
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
        AddVectoredExceptionHandler(1, CountFaults);
        bmvr::StartWatchdog();
        bmvr::SetStage("dll_attach");
        CreateThread(nullptr, 0, InitBMVR, hModule, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        RestoreDesktopDisplayModes();
        L4D2VR_ShutdownReShadeVRBridge();
        L4D2VR_ShutdownSystemMouseInputSuppression();
        break;
    default:
        break;
    }
    return TRUE;
}
