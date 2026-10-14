#include <Windows.h>
#include "game.h"
#include "bmvr_flags.h"

static LONG CALLBACK FaultVeh(PEXCEPTION_POINTERS info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000005 && code != 0xC000001D && code != 0xC0000094)
        return EXCEPTION_CONTINUE_SEARCH;
    static DWORD seen[8];
    static volatile LONG count = 0;
    const DWORD eip = info->ContextRecord->Eip;
    const LONG n = InterlockedIncrement(&count);
    if (n > 8)
        return EXCEPTION_CONTINUE_SEARCH;
    seen[n - 1] = eip;
    for (LONG i = 0; i < n - 1; ++i)
    {
        if (seen[i] == eip)
            return EXCEPTION_CONTINUE_SEARCH;
    }

    char body[1600];
    int used = snprintf(body, sizeof(body), "fault eip=%08X code=%08X\n", eip, code);
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
        __try
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
        AddVectoredExceptionHandler(1, FaultVeh);
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
