// openvr_api.dll is delay-loaded (CMakeLists.txt /DELAYLOAD), so the OpenXR backend never loads it.
//
// Why: Black Mesa's own bin\sourcevr.dll imports the old VR_Init / VR_Shutdown from openvr_api.dll. A process
// holds one module per DLL name, so once our modern openvr_api.dll (VR_InitInternal2 etc., no VR_Init) is loaded,
// sourcevr.dll fails to bind and Windows shows "entry point VR_Shutdown could not be located" twice at startup.
// Players used to rename sourcevr.dll by hand. Now only VRRuntimeBackend=openvr loads OpenVR, from VR\openvr_api.dll
// (outside the game's DLL search path, so the game's stock bin\openvr_api.dll stays what sourcevr.dll binds to).

#include <Windows.h>
#include <delayimp.h>

#include <string>

#include "bmvr_flags.h"

namespace
{
    FARPROC WINAPI BmvrDelayLoadHook(unsigned notify, PDelayLoadInfo info)
    {
        if (notify != dliNotePreLoadLibrary || !info || !info->szDll)
            return nullptr;
        if (lstrcmpiA(info->szDll, "openvr_api.dll") != 0)
            return nullptr;

        const std::string rel = bmvr::VrFilePath("openvr_api.dll");
        char full[MAX_PATH]{};
        const DWORD n = GetFullPathNameA(rel.c_str(), MAX_PATH, full, nullptr);
        if (n > 0 && n < MAX_PATH && GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES)
        {
            if (HMODULE mod = LoadLibraryExA(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
                return reinterpret_cast<FARPROC>(mod);
        }
        // Older installs carry the modern copy next to bms.exe / in bin\: default search.
        return nullptr;
    }
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = BmvrDelayLoadHook;
