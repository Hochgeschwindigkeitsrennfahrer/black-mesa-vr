#include "bmvr_flags.h"
#include "openxr_bridge_protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <share.h>
#include <string.h>
#include <string>
#include <vector>

namespace bmvr
{
    uint32_t g_RecommendedEyeWidth = 0;
    uint32_t g_RecommendedEyeHeight = 0;
    uint32_t g_FramebufferWidth = 0;
    uint32_t g_FramebufferHeight = 0;
    bool g_OpenVRInitedFromCreateDevice = false;
    float g_RenderScale = 1.f;
    float g_TurnSpeed = 0.25f;
    bool g_SnapTurning = false;
    float g_SnapTurnAngle = 45.f;
    float g_ViewmodelPosOffsetX = 0.f;
    float g_ViewmodelPosOffsetY = 0.f;
    float g_ViewmodelPosOffsetZ = 0.f;
    float g_ViewmodelPosOffsetXTouch = 5.5f;
    float g_ViewmodelAngOffsetX = 0.f;
    float g_ViewmodelAngOffsetY = 0.f;
    float g_ViewmodelAngOffsetZ = 0.f;
    float g_ControllerPitchTilt = -35.f;
    float g_ControllerPitchTiltTouch = 0.f;
    float g_ControllerPitchTiltIndex = 0.f;
    float g_ControllerPitchTiltVive = -45.f;
    float g_AimPitchOffset = 0.f;
    bool g_DisableRecoilAim = true;
    float g_IPDScale = 1.f;
    float g_HeightOffset = 0.f;
    bool g_AutoMatQueueMode = false;
    bool g_MulticoreMode = true;
    uint32_t g_AntiAliasing = 0;
    bool g_Haptics = true;
    bool g_HideCrosshair = true;
    bool g_MatchHmdHz = true;
    bool g_DisableViewBob = true;
    bool g_LeftHanded = false;
    bool g_RecenterResetsYaw = true;
    bool g_HideLocalPlayerModel = true;
    bool g_HideViewmodelArms = true;
    bool g_VrHandsGlovesEnabled = true;
    bool g_VrHandsRightEnabled = false;
    float g_VrHandsModelScale = 0.85f;
    bool g_VrHandsDebugBoxes = false;
    bool g_HandHud = true;
    bool g_VrCrosshair = false;
    float g_VrCrosshairScale = 1.f;
    bool g_HideHandsWithoutSuit = true;
    bool g_ScopeUsesHmdAim = true;
    float g_ScopeZoomFovScale = 0.28f;
    float g_ScopeZoomSmoothSec = 0.16f;
    float g_VrHandsPoseRotX = 0.f;
    float g_VrHandsPoseRotY = 180.f;
    float g_VrHandsPoseRotZ = 0.f;
    float g_VrHandsIndexRollDeg = 90.f;
    // ~one hand-length back along aim after yaw 180 (user: gloves sat ahead
    // of the controller). Flip the Z sign in VR/config.txt if this goes the
    // wrong way on a given headset.
    // Hands lower in view (controller local +Y = up). Separate from weapon.
    float g_VrHandsPoseOffX = 0.f;
    // Raised from hanging under the controller. +Y is up. Config and
    // install.ps1 must match this or they overwrite it on every install.
    float g_VrHandsPoseOffY = -0.008f;
    float g_VrHandsPoseOffZ = -0.10f;
    // Additive on top of VrHandsPoseOffsetMeters for Quest/Touch.
    float g_VrHandsTouchOffX = 0.f;
    float g_VrHandsTouchOffY = 0.f;
    float g_VrHandsTouchOffZ = 0.f;
    float g_VrHandsLeftPoseOffX = 0.f;
    float g_VrHandsLeftPoseOffY = 0.f;
    float g_VrHandsLeftPoseOffZ = 0.f;
    float g_VrHandsRightPoseOffX = 0.f;
    float g_VrHandsRightPoseOffY = 0.f;
    float g_VrHandsRightPoseOffZ = 0.f;
    // Additive local Rx,Ry,Rz (deg) for the right glove only while a gun is
    // held. L4D2VR local correction is R=Rz*Ry*Rx on basis [R|U|-F].
    // -90 still left the thumb up / palm on the slide. One more 90° clockwise
    // on the same axis: Rz = -180. Do not add Rx.
    float g_VrHandsRightGripRotX = 0.f;
    float g_VrHandsRightGripRotY = 0.f;
    float g_VrHandsRightGripRotZ = -180.f;
    bool g_VrHandsUseHevGloves = true;
    bool g_IsBlueShift = false;
    uint32_t g_FullFrameActualWidth = 0;
    uint32_t g_FullFrameActualHeight = 0;
    uint32_t g_GbActualWidth = 0;
    uint32_t g_GbActualHeight = 0;
    // false = runtime PostPresentHandoff. true = L4D2VR app handoff (default).
    // Runtime mode lets the pose-waiter WaitGetPoses touch the Vulkan queue and
    // stall ~110ms (~20fps). App mode is the 90fps path. First-shot PostPresentHandoff
    // hitch is separate; do not put WaitGetPoses on the RenderView thread.
    bool g_CompositorPostPresentHandoff = true;
    bool g_ForceOpenVis = false;
    bool g_HiddenAreaMesh = false;
    bool g_MaterialPredraw = false;
    bool g_StereoBlitGpuFlush = true;
    bool g_OpenXrDeferredPublish = false;
    float g_OpenXrSlotCoolingMs = 4.f;
    uint32_t g_OpenXrMaxPending = 1;
    // Match L4D2VR weapon tables (designed for VRScale 43.2) to BM 39.37:
    // 0.91 was the 39.37/43.2 ratio; user 2026-08-25: ~25% smaller than that
    // (0.91 x 0.75 = 0.68). Crowbar forced to 1.0 in DME. Hands slightly larger.
    float g_ViewmodelScale = 0.68f;
    float g_HudMaxFov = 60.f;
    float g_HudDisplayRatio = 0.82f;
    // Overlay quad in HMD space. 1.3 m × 1.3 m filled the FOV with a black
    // rectangle when VGUI never painted. Smaller / closer keeps HUD central.
    float g_HudDistance = 1.05f;
    float g_HudSize = 0.70f;
    float g_MenuPanelScale = 0.70f;
    float g_MenuForwardMeters = 0.65f;
    float g_MenuWidthMeters = 1.05f;
    float g_MenuHeightOffsetMeters = -0.02f;
    float g_MenuCursorSmoothSec = 0.18f;

    static HMODULE g_Module = nullptr;
    static std::mutex g_LogMutex;
    static bool g_TryHmdSwapchain = true;
    static bool g_TryHmdFramebuffer = true;
    static bool g_TryHmdNative = true;
    static bool g_TryNamedRT = true;
    static bool g_TryNamedStereoWrap = false;
    static bool g_TryStereoRV = true;
    static bool g_TryWaitIdle = true;
    static bool g_TryAbsView = true;
    static bool g_TryMenuCompositor = true;
    static bool g_TryRelativeHmdLook = true;
    static bool g_TryStereoCopy = false;
    static bool g_TryStereoFov = false;
    static bool g_TryMatQueue = true;
    static bool g_TryHl2vrQueuedWgp = true;
    static bool g_TryHl2vrStereoEye = false;
    static bool g_TryHl2vrHam = false;
    static bool g_TryHl2vrPixelVis = false;
    static bool g_TryHl2vrPredraw = false;
    static bool g_TryHl2vrPlayerCull = false;
    static bool g_TryStereoVisReset = false;
    static bool g_Hl2vrHamBlocked = false;
    static bool g_Hl2vrPixelVisBlocked = false;
    static bool g_Hl2vrPlayerCullBlocked = false;
    static bool g_TryCtrlPose = true;
    static bool g_TryCtrlVm = true;
    static bool g_TryVrGloves = true;
    static bool g_TryHandOverlay = true;
    static bool g_TrySteamVrEyeRt = false;
    // Private eyes at OpenVR recommended * RenderScale. World RTs stay at
    // the HWND (gbmatch squash-blit). LITERAL FullFrame/G-buffer at rec
    // filled only the top HWND slice of the eyes (2026-08-26, user miss).
    static bool g_TryOffscreenHmd = true;
    static bool g_TryOffscreenWorldGrow = false;
    // WorldRenderAtEyeSize=true in VR/config.txt opts back into the world-RT
    // grow. Set it back to false to return to the window-sized world + eye
    // upscale. The opt-in also overrides the policy hmd_world skip. Leftover
    // bmvr_in_hmd_world.flag (install killing bms.exe) is deleted, not honored.
    static bool g_WorldEyeSizeOptIn = false;
    static bool g_GameplayWorldRts = false;
    // `_rt_FullFrameFB*` is created at map-load *before* client LevelInit.
    // Waiting for that hook (2026-09-01) left FullFrame at 2560x1440 while
    // SetRenderTargetFrameBufferSizeOverrides(eye) made `_rt_gb*` 3168x3104.
    // That mismatch is flashlight-dead + ghost world. FullFrame now grows at
    // CreateNamedRT on the same load; the override stays HWND until FullFrame
    // actually matches the eyes. background* still blocks grow.
    static bool g_WorldRtGrowArmed = false;
    static bool g_EngineMapIsBackground = false;
    static char g_NotedEngineMap[96] = {};
    static bool g_TryHudOverlay = true;
    static bool g_TryVguiPaint = true;
    static bool g_TryGameUiActivate = true;
    static bool g_TryMeleeTrace = true;
    static bool g_TryFbOverride = true;
    // 2026-08-20 HMD: LITERAL FullFrame grow (2560x2144) + eyes 2192x2144
    // warped/smeared the scene and put gun/hands behind the player. Sticky
    // ff_stereo already persisted; keep the attempt OFF. Do not grow FullFrame.
    static bool g_TryFullFrameStereo = false;
    // 47777b5 same-buffer: size FullFrame to fitted eye size, not 16:9 HWND.
    // Verified fail 2026-08-22: LITERAL 1584 FullFrame + G-buffer while PushRT
    // still used 2560 viewports; unbind copied A2R10 HDR (fmt=35) into LDR
    // eyes → white untextured HMD, textured desktop, ~20fps. Keep OFF.
    static bool g_TryHmdFitFullFrame = false;
    // Same-buffer retry that also LITERAL-sized `_rt_gb*` to 1584. Verified
    // miss 2026-08-22: alloc succeeded, process died on background04 before
    // 120 stereo frames (crash-sticky). Do not retry.
    static bool g_TryEyeFitWorldRts = false;
    // Keep stereo CViewSetup/viewport at 2560 GB size so deferred flashlight
    // apply runs; HMD fov+aspect+IPD; squash-blit into 1584 eyes. Not an RT
    // resize (ff_hmdfit / ff_gbfit). Sticky fl_gbmatch. Default on; leftover
    // 16:9 is skipped via gb_leftskip so this is 2 renders, not 3.
    static bool g_TryFlashlightGbMatch = true;
    static bool g_TryGbLeftSkip = true;
    bool g_DesktopLeftoverRender = false;
    static bool g_TryDrawHud = true;
    static bool g_TryDme = true;
    static bool g_Inited = false;
    static bool g_WatchdogStarted = false;
    static char g_Stage[64] = "dll_attach";

    static std::wstring ModuleDir()
    {
        wchar_t path[MAX_PATH]{};
        HMODULE mod = g_Module;
        if (!mod)
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ModuleDir), &mod);
        if (!mod)
            return L".";
        GetModuleFileNameW(mod, path, MAX_PATH);
        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return L".";
        return full.substr(0, slash);
    }

    static std::wstring ExeDir()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return L".";
        return full.substr(0, slash);
    }

    static std::wstring SkipPath()
    {
        return ExeDir() + L"\\bmvr_skip.txt";
    }

    static std::vector<std::wstring> FlagDirs()
    {
        std::vector<std::wstring> dirs;
        const std::wstring exe = ExeDir();
        const std::wstring mod = ModuleDir();
        dirs.push_back(exe);
        if (_wcsicmp(exe.c_str(), mod.c_str()) != 0)
            dirs.push_back(mod);
        return dirs;
    }

    static bool FileExists(const std::wstring& path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    static void WriteUtf8File(const std::wstring& path, const char* text, bool append)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), append ? L"ab" : L"wb");
        if (!f)
            return;
        fputs(text, f);
        if (text[0] && text[strlen(text) - 1] != '\n')
            fputc('\n', f);
        fflush(f);
        fclose(f);
    }

    static void WriteAll(const wchar_t* fileName, const char* text, bool append)
    {
        for (const auto& dir : FlagDirs())
            WriteUtf8File(dir + L"\\" + fileName, text, append);
    }

    static bool ParseCfgBool(const char* val, bool fallback)
    {
        if (!val || !*val)
            return fallback;
        char buf[16];
        size_t n = 0;
        while (val[n] && n + 1 < sizeof(buf))
        {
            buf[n] = static_cast<char>(tolower(static_cast<unsigned char>(val[n])));
            ++n;
        }
        buf[n] = 0;
        if (std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0
            || std::strcmp(buf, "yes") == 0 || std::strcmp(buf, "on") == 0)
            return true;
        if (std::strcmp(buf, "0") == 0 || std::strcmp(buf, "false") == 0
            || std::strcmp(buf, "no") == 0 || std::strcmp(buf, "off") == 0)
            return false;
        return fallback;
    }

    static std::string ReadConfigFileText(const std::wstring& path)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"rb");
        if (!f)
            return {};
        if (fseek(f, 0, SEEK_END) != 0)
        {
            fclose(f);
            return {};
        }
        const long sz = ftell(f);
        if (sz <= 0 || sz > 1024 * 1024)
        {
            fclose(f);
            return {};
        }
        if (fseek(f, 0, SEEK_SET) != 0)
        {
            fclose(f);
            return {};
        }
        std::string raw(static_cast<size_t>(sz), '\0');
        const size_t got = fread(&raw[0], 1, static_cast<size_t>(sz), f);
        fclose(f);
        if (got == 0)
            return {};
        raw.resize(got);

        auto utf16leToAscii = [](const char* data, size_t bytes) -> std::string {
            if (bytes < 2)
                return {};
            const size_t nchars = bytes / 2;
            const auto* w = reinterpret_cast<const wchar_t*>(data);
            const int needed = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(nchars),
                nullptr, 0, nullptr, nullptr);
            if (needed <= 0)
                return {};
            std::string out(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(nchars),
                &out[0], needed, nullptr, nullptr);
            return out;
        };

        if (got >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF
            && static_cast<unsigned char>(raw[1]) == 0xFE)
            return utf16leToAscii(raw.data() + 2, got - 2);
        if (got >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF
            && static_cast<unsigned char>(raw[1]) == 0xBB
            && static_cast<unsigned char>(raw[2]) == 0xBF)
            return raw.substr(3);

        int nulEven = 0;
        const size_t probe = got < 64 ? got : 64;
        for (size_t i = 1; i + 1 < probe; i += 2)
        {
            if (raw[i] == 0)
                ++nulEven;
        }
        if (nulEven >= 16)
            return utf16leToAscii(raw.data(), got);
        return raw;
    }

    static void ReadUserConfig(const std::wstring& path)
    {
        std::string text = ReadConfigFileText(path);
        if (text.empty())
            return;
        for (char& c : text)
        {
            if (c == '\r')
                c = '\n';
        }
        bool sawWorldEyeSize = false;
        size_t cursor = 0;
        while (cursor < text.size())
        {
            size_t eol = text.find('\n', cursor);
            if (eol == std::string::npos)
                eol = text.size();
            char line[256];
            const size_t len = (eol - cursor) < (sizeof(line) - 1) ? (eol - cursor) : (sizeof(line) - 1);
            memcpy(line, text.data() + cursor, len);
            line[len] = 0;
            cursor = eol + 1;
            char* n = line;
            while (*n == ' ' || *n == '\t')
                ++n;
            if (*n == '#' || *n == '\r' || *n == '\n' || !*n)
                continue;
            char* eq = strchr(n, '=');
            if (!eq)
                continue;
            *eq = 0;
            char* val = eq + 1;
            for (char* p = n + strlen(n); p > n && (p[-1] == ' ' || p[-1] == '\t'); --p)
                p[-1] = 0;
            while (*val == ' ' || *val == '\t')
                ++val;
            for (char* p = val; *p; ++p)
            {
                if (*p == '\r' || *p == '\n' || *p == '#')
                {
                    *p = 0;
                    break;
                }
            }
            if (std::strcmp(n, "RenderScale") == 0 || std::strcmp(n, "VRRenderScale") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.5f && s <= 2.0f)
                    g_RenderScale = s;
            }
            else if (std::strcmp(n, "TurnSpeed") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.01f && s <= 5.f)
                    g_TurnSpeed = s;
            }
            else if (std::strcmp(n, "SnapTurning") == 0)
                g_SnapTurning = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "SnapTurnAngle") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 10.f && s <= 180.f)
                    g_SnapTurnAngle = s;
            }
            else if (std::strcmp(n, "ViewmodelPosOffsetX") == 0)
                g_ViewmodelPosOffsetX = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelPosOffsetY") == 0)
                g_ViewmodelPosOffsetY = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelPosOffsetZ") == 0)
                g_ViewmodelPosOffsetZ = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelPosOffsetXTouch") == 0)
                g_ViewmodelPosOffsetXTouch = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelAngOffsetX") == 0)
                g_ViewmodelAngOffsetX = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelAngOffsetY") == 0)
                g_ViewmodelAngOffsetY = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelAngOffsetZ") == 0)
                g_ViewmodelAngOffsetZ = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ControllerPitchTilt") == 0)
                g_ControllerPitchTilt = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ControllerPitchTiltTouch") == 0)
                g_ControllerPitchTiltTouch = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ControllerPitchTiltIndex") == 0)
                g_ControllerPitchTiltIndex = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ControllerPitchTiltVive") == 0)
                g_ControllerPitchTiltVive = static_cast<float>(atof(val));
            else if (std::strcmp(n, "AimPitchOffset") == 0)
                g_AimPitchOffset = static_cast<float>(atof(val));
            else if (std::strcmp(n, "DisableRecoilAim") == 0)
                g_DisableRecoilAim = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "IPDScale") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.5f && s <= 2.f)
                    g_IPDScale = s;
            }
            else if (std::strcmp(n, "HeightOffset") == 0)
                g_HeightOffset = static_cast<float>(atof(val));
            else if (std::strcmp(n, "AutoMatQueueMode") == 0)
                g_AutoMatQueueMode = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "MulticoreMode") == 0)
                g_MulticoreMode = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "AntiAliasing") == 0 || std::strcmp(n, "msaa") == 0)
            {
                const int nAa = atoi(val);
                g_AntiAliasing = (nAa == 2 || nAa == 4 || nAa == 8 || nAa == 16)
                    ? static_cast<uint32_t>(nAa) : 0u;
            }
            else if (std::strcmp(n, "Haptics") == 0)
                g_Haptics = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "HideCrosshair") == 0)
                g_HideCrosshair = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "MatchHmdHz") == 0)
                g_MatchHmdHz = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "DisableViewBob") == 0)
                g_DisableViewBob = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "LeftHanded") == 0)
                g_LeftHanded = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "RecenterResetsYaw") == 0)
                g_RecenterResetsYaw = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "HideLocalPlayerModel") == 0)
                g_HideLocalPlayerModel = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "HideViewmodelArms") == 0)
                g_HideViewmodelArms = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "VrHandsGlovesEnabled") == 0)
                g_VrHandsGlovesEnabled = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "VrHandsRightEnabled") == 0)
                g_VrHandsRightEnabled = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "VrHandsModelScale") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.2f && s <= 2.f)
                    g_VrHandsModelScale = s;
            }
            else if (std::strcmp(n, "VrHandsDebugBoxes") == 0)
                g_VrHandsDebugBoxes = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "VrHandHud") == 0)
                g_HandHud = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "VrCrosshair") == 0)
                g_VrCrosshair = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "VrCrosshairScale") == 0)
                g_VrCrosshairScale = static_cast<float>(atof(val));
            else if (std::strcmp(n, "VrHideHandsWithoutSuit") == 0)
                g_HideHandsWithoutSuit = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "VrScopeUsesHmdAim") == 0)
                g_ScopeUsesHmdAim = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "ScopeZoomFovScale") == 0)
                g_ScopeZoomFovScale = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ScopeZoomSmoothSec") == 0)
                g_ScopeZoomSmoothSec = static_cast<float>(atof(val));
            else if (std::strcmp(n, "VrHandsPoseRotationOffset") == 0)
            {
                float x = 0.f, y = 180.f, z = 0.f;
                if (sscanf(val, "%f,%f,%f", &x, &y, &z) == 3)
                {
                    g_VrHandsPoseRotX = x;
                    g_VrHandsPoseRotY = y;
                    g_VrHandsPoseRotZ = z;
                }
            }
            else if (std::strcmp(n, "VrHandsIndexRollDeg") == 0)
                g_VrHandsIndexRollDeg = static_cast<float>(atof(val));
            else if (std::strcmp(n, "VrHandsPoseOffsetMeters") == 0)
            {
                float x = 0.f, y = 0.f, z = -0.20f;
                if (sscanf(val, "%f,%f,%f", &x, &y, &z) == 3)
                {
                    g_VrHandsPoseOffX = x;
                    g_VrHandsPoseOffY = y;
                    g_VrHandsPoseOffZ = z;
                }
            }
            else if (std::strcmp(n, "QuestHandsPoseOffsetMeters") == 0)
            {
                float x = 0.f, y = 0.f, z = 0.f;
                if (sscanf(val, "%f,%f,%f", &x, &y, &z) == 3)
                {
                    g_VrHandsTouchOffX = x;
                    g_VrHandsTouchOffY = y;
                    g_VrHandsTouchOffZ = z;
                }
            }
            else if (std::strcmp(n, "VrHandsLeftPoseOffsetMeters") == 0)
            {
                float x = 0.f, y = 0.f, z = 0.f;
                if (sscanf(val, "%f,%f,%f", &x, &y, &z) == 3)
                {
                    g_VrHandsLeftPoseOffX = x;
                    g_VrHandsLeftPoseOffY = y;
                    g_VrHandsLeftPoseOffZ = z;
                }
            }
            else if (std::strcmp(n, "VrHandsRightPoseOffsetMeters") == 0)
            {
                float x = 0.f, y = 0.f, z = 0.f;
                if (sscanf(val, "%f,%f,%f", &x, &y, &z) == 3)
                {
                    g_VrHandsRightPoseOffX = x;
                    g_VrHandsRightPoseOffY = y;
                    g_VrHandsRightPoseOffZ = z;
                }
            }
            else if (std::strcmp(n, "VrHandsUseHevGloves") == 0)
                g_VrHandsUseHevGloves = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "CompositorPostPresentHandoff") == 0)
                g_CompositorPostPresentHandoff = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "DesktopLeftoverRender") == 0)
                g_DesktopLeftoverRender = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "ForceOpenVis") == 0)
                g_ForceOpenVis = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "HiddenAreaMesh") == 0)
                g_HiddenAreaMesh = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "MaterialPredraw") == 0)
                g_MaterialPredraw = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "StereoBlitGpuFlush") == 0)
                g_StereoBlitGpuFlush = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "OpenXrDeferredPublish") == 0)
                g_OpenXrDeferredPublish = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "OpenXrSlotCoolingMs") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.f && s <= 50.f)
                    g_OpenXrSlotCoolingMs = s;
            }
            else if (std::strcmp(n, "OpenXrMaxPending") == 0)
            {
                const int v = atoi(val);
                if (v >= 1 && v <= 3)
                    g_OpenXrMaxPending = static_cast<uint32_t>(v);
            }
            else if (std::strcmp(n, "ViewmodelScale") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.2f && s <= 1.5f)
                    g_ViewmodelScale = s;
            }
            else if (std::strcmp(n, "HudDistance") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.4f && s <= 4.f)
                    g_HudDistance = s;
            }
            else if (std::strcmp(n, "HudSize") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.4f && s <= 4.f)
                    g_HudSize = s;
            }
            else if (std::strcmp(n, "MenuPanelScale") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.2f && s <= 1.f)
                    g_MenuPanelScale = s;
            }
            else if (std::strcmp(n, "MenuForwardMeters") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.2f && s <= 4.f)
                    g_MenuForwardMeters = s;
            }
            else if (std::strcmp(n, "MenuWidthMeters") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.4f && s <= 4.f)
                    g_MenuWidthMeters = s;
            }
            else if (std::strcmp(n, "MenuHeightOffsetMeters") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= -0.5f && s <= 0.5f)
                    g_MenuHeightOffsetMeters = s;
            }
            else if (std::strcmp(n, "MenuCursorSmoothSec") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.02f && s <= 0.6f)
                    g_MenuCursorSmoothSec = s;
            }
            else if (std::strcmp(n, "HudMaxFov") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 30.f && s <= 90.f)
                    g_HudMaxFov = s;
            }
            else if (std::strcmp(n, "HudDisplayRatio") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.4f && s <= 1.f)
                    g_HudDisplayRatio = s;
            }
            else if (std::strcmp(n, "ViewmodelDisableMoveBob") == 0)
                g_DisableViewBob = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "WorldRenderAtEyeSize") == 0)
            {
                sawWorldEyeSize = true;
                g_WorldEyeSizeOptIn = ParseCfgBool(val, false);
                g_TryOffscreenWorldGrow = g_WorldEyeSizeOptIn;
            }
        }
        Log("VR config %ls RenderScale=%.2f TurnSpeed=%.2f snap=%d vm=(%.1f,%.1f,%.1f) touchOx=%.1f tilt=%.1f ipd=%.2f autoQueue=%d multicore=%d aa=%u worldEyeSize=%d key=%s",
            path.c_str(), g_RenderScale, g_TurnSpeed, g_SnapTurning ? 1 : 0,
            g_ViewmodelPosOffsetX, g_ViewmodelPosOffsetY, g_ViewmodelPosOffsetZ,
            g_ViewmodelPosOffsetXTouch,
            g_ControllerPitchTilt, g_IPDScale, g_AutoMatQueueMode ? 1 : 0, g_MulticoreMode ? 1 : 0, g_AntiAliasing,
            g_WorldEyeSizeOptIn ? 1 : 0,
            sawWorldEyeSize ? (g_WorldEyeSizeOptIn ? "true" : "false") : "missing");
        if (!g_WorldEyeSizeOptIn)
        {
            Log("WorldRenderAtEyeSize is off: world RTs stay at the game window and are "
                "upscaled into SteamVR/OpenXR eyes. Raising the SteamVR SS slider will not "
                "add world pixels. Set WorldRenderAtEyeSize=true in that config.txt and restart.");
        }
    }

    static void ApplySkipName(const std::string& name, const char* via)
    {
        if (name == "hmd_swap")
            g_TryHmdSwapchain = false;
        else if (name == "hmd_fb")
            g_TryHmdFramebuffer = false;
        else if (name == "hmd_native")
            g_TryHmdNative = false;
        else if (name == "named_rt")
            g_TryNamedRT = false;
        else if (name == "stereo_rv")
            g_TryStereoRV = false;
        else if (name == "named_bind" || name == "named_l4d" || name == "named_eye" || name == "named_push")
            g_TryNamedStereoWrap = false;
        else if (name == "wait_idle")
            g_TryWaitIdle = false;
        else if (name == "abs_view")
            g_TryAbsView = false;
        else if (name == "menu_vr")
        {
            Log("Ignoring menu_vr skip (%s): empty-map GameUI Submit must stay on",
                via ? via : "skip file");
            return;
        }
        else if (name == "rel_look")
            g_TryRelativeHmdLook = false;
        else if (name == "stereo_copy")
            g_TryStereoCopy = false;
        else if (name == "stereo_fov")
            g_TryStereoFov = false;
        else if (name == "mat_queue")
        {
            g_TryMatQueue = false;
        }
        else if (name == "steamvr_rt")
            g_TrySteamVrEyeRt = false;
        else if (name == "hmd_offscreen")
            g_TryOffscreenHmd = false;
        else if (name == "hmd_world")
        {
            // GitHub path: WorldRenderAtEyeSize=true is an explicit retry.
            if (g_WorldEyeSizeOptIn)
            {
                Log("Ignoring hmd_world skip (%s): WorldRenderAtEyeSize=true", via ? via : "skip file");
                return;
            }
            g_TryOffscreenWorldGrow = false;
        }
        else if (name == "hud_overlay")
            g_TryHudOverlay = false;
        else if (name == "vgui_paint")
            g_TryVguiPaint = false;
        else if (name == "gameui")
            g_TryGameUiActivate = false;
        else if (name == "melee_trace")
        {
            // Legacy tip-origin ban. Do not disable the viewmodel-origin
            // TraceRay rewrite (melee_vm). Tip path is gone.
        }
        else if (name == "melee_vm")
            g_TryMeleeTrace = false;
        else if (name == "fb_override")
            g_TryFbOverride = false;
        else if (name == "ff_stereo")
            g_TryFullFrameStereo = false;
        else if (name == "ff_hmdfit")
            g_TryHmdFitFullFrame = false;
        else if (name == "ff_gbfit")
            g_TryEyeFitWorldRts = false;
        else if (name == "fl_gbmatch")
            g_TryFlashlightGbMatch = false;
        else if (name == "gb_leftskip")
            g_TryGbLeftSkip = false;
        else if (name == "drawhud")
            g_TryDrawHud = false;
        else if (name == "dme")
            g_TryDme = false;
        else if (name == "hl2vr_wgp")
            g_TryHl2vrQueuedWgp = false;
        else if (name == "hl2vr_eye")
            g_TryHl2vrStereoEye = false;
        else if (name == "stereo_vis")
            g_TryStereoVisReset = false;
        else if (name == "hl2vr_ham")
        {
            g_TryHl2vrHam = false;
            g_Hl2vrHamBlocked = true;
            g_HiddenAreaMesh = false;
        }
        else if (name == "hl2vr_pixelvis")
        {
            g_TryHl2vrPixelVis = false;
            g_Hl2vrPixelVisBlocked = true;
        }
        else if (name == "hl2vr_playercull")
        {
            // First stereo BeginRisky overlapped this with hmd_fb / quit, so
            // a later death skip-filed it. Player-cull crash-sticky is
            // one-launch only (persist=false).
            Log("Ignoring %s skip (%s): not a permanent ban",
                name.c_str(), via ? via : "skip file");
            return;
        }
        else if (name == "hl2vr_predraw")
            g_TryHl2vrPredraw = false;
        else if (name == "hl2vr_proj" || name == "hl2vr_cproj" || name == "hl2vr_neye")
        {
            // Leftover from the reverted native OpenVR stereo experiment.
            return;
        }
        else if (name == "ctrl_pose" || name == "ctrl_vm" || name == "vr_gloves"
            || name == "hand_overlay")
        {
            // Overlay after gloves AVed while ctrl_pose was still armed
            // (2026-09-04). That skip-filed tracking and the next launch had
            // no hands or wrist HUD. One-launch sticky only.
            Log("Ignoring %s skip (%s): not a permanent ban",
                name.c_str(), via ? via : "skip file");
            return;
        }
        else
            return;
        Log("Skip %s (%s)", name.c_str(), via ? via : "skip file");
    }

    static void ReadSkipFile(const std::wstring& path)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"r");
        if (!f)
            return;
        char line[128];
        while (fgets(line, sizeof(line), f))
        {
            char* n = line;
            while (*n == ' ' || *n == '\t')
                ++n;
            for (char* p = n; *p; ++p)
            {
                if (*p == '\r' || *p == '\n' || *p == '#')
                {
                    *p = 0;
                    break;
                }
            }
            if (!*n)
                continue;
            // named_rt was persisted because GetBackBufferFormat's vtable slot
            // is wrong on BM, not because CreateNamed crashed. Retry with the
            // verified RVAs. Crash-sticky bmvr_in_named_rt.flag still wins.
            // Named wrap onto HDR leftEye0 died in CSimpleWorldView after
            // three rewritten PushRT(NULL) even at 2560x1440. That skip must
            // not disable double RenderView / HMD-aspect G-buffer.
            if (std::strcmp(n, "hmd_fb") == 0)
            {
                // Matching swapchain still died after LevelInit. 2026-08-17
                // evening: GetScreenSize fix produced RenderView setup=1584,
                // then first stereo left RenderView during spawn crashed.
                // Crash-sticky is cleared without disabling the framebuffer.
                Log("Ignoring hmd_fb skip-file entry (1584 world RV works; stereo wrote height into m_eStereoEye at 0x1C)");
                continue;
            }
            if (std::strcmp(n, "named_rt") == 0)
            {
                Log("Ignoring named_rt skip-file entry (retry CreateNamedEx by RVA)");
                continue;
            }
            // rel_look sticky was left by a WaitGetPoses hang on the Present
            // thread (BeginRisky still set). That hang is not the look copy.
            if (std::strcmp(n, "rel_look") == 0)
            {
                Log("Ignoring rel_look skip-file entry (WaitGetPoses hang, not look)");
                continue;
            }
            // DME BeginRisky was held from createHook until the first gameplay
            // DrawModelExecute. Any later crash (ff_hmdfit, stereo, etc.)
            // persisted `dme` and skipped the bad197a viewmodel yFix/scale/arms
            // hide. Crash-sticky during createHook itself still wins.
            if (std::strcmp(n, "dme") == 0)
            {
                Log("Ignoring dme skip-file entry (startup-wide BeginRisky false-banned viewmodel hook)");
                continue;
            }
            if (std::strcmp(n, "mat_queue") == 0)
            {
                Log("Ignoring mat_queue skip-file entry (other-build policy; own crash-sticky still honored)");
                continue;
            }
            if (std::strcmp(n, "menu_vr") == 0)
            {
                Log("Ignoring menu_vr skip-file entry (retry 2D capture Submit on background*)");
                continue;
            }
            if (std::strcmp(n, "fl_gbmatch") == 0)
            {
                Log("Ignoring fl_gbmatch skip-file entry (other-build policy; own crash-sticky still honored)");
                continue;
            }
            if (std::strcmp(n, "gb_leftskip") == 0)
            {
                Log("Ignoring gb_leftskip skip-file entry (other-build policy; own crash-sticky still honored)");
                continue;
            }
            // Named-RT stereo_rv / named_push deaths. Retry named_l4d.
            if (std::strcmp(n, "stereo_rv") == 0)
            {
                Log("Ignoring stereo_rv skip-file entry (retry L4D2 wrap + null rewrite)");
                continue;
            }
            // Outer L4D2 PushRT(eye) then BM PushRT(NULL,0,0,0,0) died.
            // named_l4d keeps the wrap and rewrites those null PushRTs.
            if (std::strcmp(n, "named_push") == 0)
            {
                Log("Ignoring named_push skip-file entry (retry L4D2 wrap + null rewrite)");
                continue;
            }
            // Rewrite-only (no wrap) was not the L4D2 path. RenderView's
            // prologue GetRT / SetRT(+0x18) restore needs the eye already
            // pushed or it puts the backbuffer back.
            if (std::strcmp(n, "named_bind") == 0)
            {
                Log("Ignoring named_bind skip-file entry (retry framebuffer-sized named RT)");
                continue;
            }
            if (std::strcmp(n, "named_l4d") == 0)
            {
                Log("Ignoring named_l4d skip-file entry (retry framebuffer-sized named RT)");
                continue;
            }
            if (std::strcmp(n, "named_eye") == 0)
            {
                Log("Ignoring named_eye skip-file entry (named PushRT wrap is off; HMD-aspect G-buffer retry)");
                continue;
            }
            ApplySkipName(n, "bmvr_skip.txt");
        }
        fclose(f);
    }

    void PersistSkip(const char* name, const char* reason)
    {
        if (!name || !name[0])
            return;
        ApplySkipName(name, reason);

        std::string existing;
        FILE* f = nullptr;
        _wfopen_s(&f, SkipPath().c_str(), L"r");
        if (f)
        {
            char buf[1024];
            while (fgets(buf, sizeof(buf), f))
                existing += buf;
            fclose(f);
        }
        if (existing.find(name) != std::string::npos)
            return;

        char line[256];
        snprintf(line, sizeof(line), "%s\n", name);
        WriteUtf8File(SkipPath(), line, true);
        WriteUtf8File(ModuleDir() + L"\\bmvr_skip.txt", line, true);
        if (reason)
            Log("Persisted skip '%s': %s", name, reason);
    }

    void SetDllModule(HMODULE module)
    {
        g_Module = module;
    }

    HMODULE DllModule()
    {
        return g_Module;
    }

    void Log(const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_LogMutex);
        // One OutputDebugString per line, not three: each call raises
        // DBG_PRINTEXCEPTION_C through the kernel and probes the DBWin mutex
        // even with no debugger attached (tens of microseconds apiece).
        char dbg[1040];
        snprintf(dbg, sizeof(dbg), "[BMVR] %s\n", buf);
        OutputDebugStringA(dbg);
        printf("%s", dbg);

        // Persistent append handles + batched flush. Opening/flushing every
        // line on the render thread was a stutter source during compositor
        // spirals (OpenCode 2026-08-24).
        static std::wstring exeLog;
        static std::wstring modLog;
        static FILE* s_exe = nullptr;
        static FILE* s_mod = nullptr;
        static DWORD s_lastFlush = 0;
        static int s_unflushed = 0;
        if (exeLog.empty())
        {
            exeLog = ExeDir() + L"\\bmvr_log.txt";
            modLog = ModuleDir() + L"\\bmvr_log.txt";
        }
        static std::string exeLogN;
        static std::string modLogN;
        if (exeLogN.empty())
        {
            auto narrow = [](const std::wstring& w) {
                std::string s;
                if (!w.empty())
                {
                    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    if (n > 1)
                    {
                        s.resize(static_cast<size_t>(n - 1));
                        WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n - 1, nullptr, nullptr);
                    }
                }
                return s;
            };
            exeLogN = narrow(exeLog);
            modLogN = narrow(modLog);
        }
        // Each process start replaces the log so a tester can send the whole
        // file. Append mode stacked old launches; testers always pasted the header.
        if (!s_exe)
            s_exe = _fsopen(exeLogN.c_str(), "wb", _SH_DENYNO);
        if (s_exe)
        {
            fputs(buf, s_exe);
            fputc('\n', s_exe);
        }
        if (_stricmp(exeLogN.c_str(), modLogN.c_str()) != 0)
        {
            if (!s_mod)
                s_mod = _fsopen(modLogN.c_str(), "wb", _SH_DENYNO);
            if (s_mod)
            {
                fputs(buf, s_mod);
                fputc('\n', s_mod);
            }
        }
        ++s_unflushed;
        const DWORD nowMs = GetTickCount();
        const bool crashBoundary = strstr(buf, "Stereo HMD-fb") != nullptr
            || strstr(buf, "Stereo frame pose latch") != nullptr
            || strstr(buf, "first controller") != nullptr
            || strstr(buf, "ctrl_vm") != nullptr
            || strstr(buf, "VR gloves") != nullptr
            || strstr(buf, "controller poses") != nullptr
            || strstr(buf, "Controller tracking") != nullptr
            || strstr(buf, "Skip hand overlay") != nullptr
            || strstr(buf, "Hand overlay") != nullptr
            || strstr(buf, "Hand markers done") != nullptr;
        if (crashBoundary || s_unflushed >= 16 || (s_unflushed > 0 && nowMs - s_lastFlush >= 500))
        {
            if (s_exe)
                fflush(s_exe);
            if (s_mod)
                fflush(s_mod);
            s_unflushed = 0;
            s_lastFlush = nowMs;
        }
    }

    static void WriteHeartbeat()
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "stage=%s pid=%lu tick=%lu\n",
            g_Stage, GetCurrentProcessId(), GetTickCount());
        WriteUtf8File(ExeDir() + L"\\bmvr_heartbeat.txt", buf, false);
    }

    void SetStage(const char* stage)
    {
        if (!stage)
            return;
        strncpy_s(g_Stage, stage, _TRUNCATE);
        WriteHeartbeat();
    }

    static DWORD WINAPI WatchdogThread(LPVOID)
    {
        Sleep(200);
        while (true)
        {
            WriteHeartbeat();
            Sleep(500);
        }
        return 0;
    }

    void StartWatchdog()
    {
        if (g_WatchdogStarted)
            return;
        g_WatchdogStarted = true;
        CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    }

    static void ConsumeIfStuck(const wchar_t* name, bool& enabled, const char* key, const char* label, bool persist = true)
    {
        bool stuck = false;
        for (const auto& dir : FlagDirs())
        {
            const std::wstring path = dir + L"\\bmvr_in_" + name + L".flag";
            if (!FileExists(path))
                continue;
            stuck = true;
            DeleteFileW(path.c_str());
        }
        if (!stuck)
            return;
        enabled = false;
        if (persist)
        {
            PersistSkip(key, "previous launch died while trying it");
            Log("Disabled %s: previous launch died while trying it", label);
        }
        else
            Log("Disabled %s this launch only (previous launch died; not skip-filed)", label);
    }

    float EffectiveControllerPitchTilt(uint32_t controllerFamily)
    {
        switch (controllerFamily)
        {
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH:
            return g_ControllerPitchTiltTouch;
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES:
            return g_ControllerPitchTiltIndex;
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE:
            return g_ControllerPitchTiltVive;
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2:
        default:
            return g_ControllerPitchTilt;
        }
    }

    void EffectiveVrHandsPoseOffset(uint32_t controllerFamily, float& x, float& y, float& z)
    {
        x = g_VrHandsPoseOffX;
        y = g_VrHandsPoseOffY;
        z = g_VrHandsPoseOffZ;
        if (controllerFamily == L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH)
        {
            x += g_VrHandsTouchOffX;
            y += g_VrHandsTouchOffY;
            z += g_VrHandsTouchOffZ;
        }
    }

    bool IsBlueShift()
    {
        return g_IsBlueShift;
    }

    static bool PathLeafIsBshift(const wchar_t* tok)
    {
        if (!tok || !tok[0])
            return false;
        const wchar_t* leaf = tok;
        for (const wchar_t* p = tok; *p; ++p)
        {
            if (*p == L'/' || *p == L'\\')
                leaf = p + 1;
        }
        return _wcsicmp(leaf, L"bshift") == 0;
    }

    static void DetectBlueShiftSession()
    {
        g_IsBlueShift = false;
        const wchar_t* p = GetCommandLineW();
        if (!p)
            return;
        wchar_t tok[MAX_PATH]{};
        auto nextTok = [&]() -> bool {
            while (*p == L' ' || *p == L'\t')
                ++p;
            if (!*p)
                return false;
            size_t n = 0;
            if (*p == L'"')
            {
                ++p;
                while (*p && *p != L'"' && n + 1 < MAX_PATH)
                    tok[n++] = *p++;
                if (*p == L'"')
                    ++p;
            }
            else
            {
                while (*p && *p != L' ' && *p != L'\t' && n + 1 < MAX_PATH)
                    tok[n++] = *p++;
            }
            tok[n] = 0;
            return true;
        };
        if (!nextTok())
            return;
        while (nextTok())
        {
            if (_wcsicmp(tok, L"-game") != 0)
                continue;
            if (nextTok() && PathLeafIsBshift(tok))
            {
                g_IsBlueShift = true;
                Log("Blue Shift session (-game bshift)");
                return;
            }
        }
    }

    void InitFromDisk()
    {
        if (g_Inited)
            return;
        g_Inited = true;
        StartWatchdog();
        DetectBlueShiftSession();
        ReadUserConfig(ExeDir() + L"\\VR\\config.txt");
        ReadSkipFile(SkipPath());
        ReadSkipFile(ModuleDir() + L"\\bmvr_skip.txt");
        ConsumeIfStuck(L"hmd_swap", g_TryHmdSwapchain, "hmd_swap", "HMD-sized swapchain");
        ConsumeIfStuck(L"hmd_native", g_TryHmdNative, "hmd_native", "L4D2VR recommended G-buffer size");
        // Last launch: 8 pass-through 1584 RenderViews (zNear=7) succeeded,
        // then stereo wrote 1440 into CViewSetup+0x1C (m_eStereoEye on BM).
        // RenderView indexes this+0x744 by that field. Do not disable hmd_fb.
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_hmd_fb.flag").c_str());
        ConsumeIfStuck(L"named_rt", g_TryNamedRT, "named_rt", "MaterialSystem named eye RTs");
        ConsumeIfStuck(L"wait_idle", g_TryWaitIdle, "wait_idle", "WaitDeviceIdle");
        ConsumeIfStuck(L"abs_view", g_TryAbsView, "abs_view", "absolute HMD CViewSetup");
        // 2026-08-16 hang persist-skipped this. Skip-file is already ignored.
        // 2026-09-03: BeginRisky stayed armed until LevelInit, so quitting (or
        // an install killing bms.exe) from GameUI left bmvr_in_menu_vr.flag.
        // Next launch then set menuVR=0, createdRT=0, helper submitted=0,
        // HMD black. Do not ConsumeIfStuck — retry 2D capture every launch.
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_menu_vr.flag").c_str());
        Log("Ignoring menu_vr crash-sticky (empty-map GameUI Submit must stay on)");
        // First gameplay RenderView BeginRisky(rel_look) then DME/FindMaterial
        // died (2026-08-19). That is not the look copy. Same as skip-file ignore.
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_rel_look.flag").c_str());
        ConsumeIfStuck(L"stereo_copy", g_TryStereoCopy, "stereo_copy", "double RenderView blit stereo");
        ConsumeIfStuck(L"stereo_fov", g_TryStereoFov, "stereo_fov", "same-size HMD-FOV double RenderView");
        // Skip-file mat_queue is ignored. Do not PersistSkip a leftover flag
        // from an EndFrame/gbmatch death — that false-bans SetThreadMode.
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_mat_queue.flag").c_str());
        ConsumeIfStuck(L"steamvr_rt", g_TrySteamVrEyeRt, "steamvr_rt", "SteamVR recommended eye RT (offscreen)");
        // WorldRenderAtEyeSize keeps private eyes at recommended size.
        // Predraw's dummy DrawScreenSpaceRectangle AV'd after the first
        // stereo pair (2026-09-05, shaderapidx9+412FF eip=0) while
        // hmd_offscreen was still armed for 120 frames. That leftover
        // flag dropped the next launch onto HWND 2560 gbmatch (worldMatch=0,
        // eyes 1584). Same class as hmd_world leftover — ignore the sticky.
        if (g_WorldEyeSizeOptIn)
        {
            for (const auto& dir : FlagDirs())
                DeleteFileW((dir + L"\\bmvr_in_hmd_offscreen.flag").c_str());
            Log("Ignoring hmd_offscreen crash-sticky (WorldRenderAtEyeSize; predraw false-banned eyes)");
        }
        else
        {
            ConsumeIfStuck(L"hmd_offscreen", g_TryOffscreenHmd, "hmd_offscreen",
                "offscreen HMD eyes + gameplay FullFrame/G-buffer grow", false);
        }
        // WorldRenderAtEyeSize is an explicit config opt-in. Do not treat a
        // leftover in-progress flag as a miss: install.ps1 killing bms.exe
        // mid-stereo left this flag and the next launch drew the HWND into
        // the eyes (soft 16:9 stretch). Revert with the config key.
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_hmd_world.flag").c_str());
        ConsumeIfStuck(L"hud_overlay", g_TryHudOverlay, "hud_overlay", "L4D2VR SteamVR HUD overlay");
        ConsumeIfStuck(L"vgui_paint", g_TryVguiPaint, "vgui_paint", "VGui_Paint redirect onto bmvrHUD");
        ConsumeIfStuck(L"gameui", g_TryGameUiActivate, "gameui", "gameui_activate from engine thread");
        // Tip-origin rewrite crash-stickied as melee_trace. Viewmodel-origin
        // rewrite (L4D2 adaptation) uses a fresh sticky key so the tip ban
        // does not permanently disable the researched path.
        ConsumeIfStuck(L"melee_vm", g_TryMeleeTrace, "melee_vm",
            "crowbar TraceRay viewmodel-origin rewrite");
        ConsumeIfStuck(L"fb_override", g_TryFbOverride, "fb_override",
            "IMaterialSystem::SetRenderTargetFrameBufferSizeOverrides");
        ConsumeIfStuck(L"ff_stereo", g_TryFullFrameStereo, "ff_stereo",
            "stereo at FullFrame/OpenVR recommended size (swapchain unchanged)");
        ConsumeIfStuck(L"ff_hmdfit", g_TryHmdFitFullFrame, "ff_hmdfit",
            "47777b5 HMD-fit FullFrame LITERAL (window-capped, not grow)");
        ConsumeIfStuck(L"ff_gbfit", g_TryEyeFitWorldRts, "ff_gbfit",
            "LITERAL FullFrame+G-buffer at eye size (HDR unbind still skipped)");
        ConsumeIfStuck(L"fl_gbmatch", g_TryFlashlightGbMatch, "fl_gbmatch",
            "2560 GB-match stereo view (HMD fov/aspect, squash-blit eyes)");
        ConsumeIfStuck(L"gb_leftskip", g_TryGbLeftSkip, "gb_leftskip",
            "skip leftover 16:9 main under gbmatch");
        ConsumeIfStuck(L"drawhud", g_TryDrawHud, "drawhud",
            "leftover RenderView with RENDERVIEW_DRAWHUD");
        ConsumeIfStuck(L"hl2vr_wgp", g_TryHl2vrQueuedWgp, "hl2vr_wgp",
            "HL2VR queued WaitGetPoses on ICallQueue");
        ConsumeIfStuck(L"hl2vr_eye", g_TryHl2vrStereoEye, "hl2vr_eye",
            "HL2VR CViewSetup.m_eStereoEye LEFT/RIGHT");
        PersistSkip("hl2vr_eye",
            "first stereo RenderView died after writing m_eStereoEye LEFT/RIGHT (OpenXR, 2026-09-04); hl2vr native retry presented white freeze (2026-09-05)");
        for (const auto& dir : FlagDirs())
        {
            DeleteFileW((dir + L"\\bmvr_in_hl2vr_ham.flag").c_str());
            DeleteFileW((dir + L"\\bmvr_in_hl2vr_pixelvis.flag").c_str());
            DeleteFileW((dir + L"\\bmvr_in_stereo_vis.flag").c_str());
        }
        PersistSkip("hl2vr_ham",
            "OpenXR visibility-mask HAM drew a black overlay on the world; no FPS gain (2026-09-05)");
        PersistSkip("hl2vr_pixelvis",
            "PixelVisibility_EndCurrentView skip on the second stereo eye; one-eye skybox + flashlight through walls (2026-09-05)");
        PersistSkip("stereo_vis",
            "CViewRender+0x744 is m_rbTakeFreezeFrame; zeroing it draws an empty freeze (white void) while m_flFreezeFrameUntil is live (2026-09-05)");
        ConsumeIfStuck(L"hl2vr_predraw", g_TryHl2vrPredraw, "hl2vr_predraw",
            "HL2VR DrawAllMaterials pipeline predraw");
        ConsumeIfStuck(L"hl2vr_playercull", g_TryHl2vrPlayerCull, "hl2vr_playercull",
            "HL2VR local-player GetRenderBoundsWorldspace", false);
        ConsumeIfStuck(L"ctrl_pose", g_TryCtrlPose, "ctrl_pose",
            "first OpenXR/OpenVR controller pose apply", false);
        ConsumeIfStuck(L"ctrl_vm", g_TryCtrlVm, "ctrl_vm",
            "CalcViewModelView controller hard-lock", false);
        ConsumeIfStuck(L"vr_gloves", g_TryVrGloves, "vr_gloves",
            "GLB glove draw into stereo eyes", false);
        ConsumeIfStuck(L"hand_overlay", g_TryHandOverlay, "hand_overlay",
            "wrist HUD / weapon menu FVF overlay", false);
        // Same false-ban as skip-file `dme`. Clear in-progress flags without
        // disabling the viewmodel hook; createHook still uses a short window.
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_dme.flag").c_str());
        // 2026-08-18: 3296x3216 private eyes + SetRT/depth redirect over the
        // 2560x1440 deferred G-buffer. Stereo pair logged redirected=1, blit
        // skipped, Present ~90fps, audio OK, Escape menu OK, world black on
        // desktop and HMD. Crash-sticky never fired (process stayed alive).
        PersistSkip("steamvr_rt", "3296 offscreen SetRT over 2560 G-buffer blacked world");
        // 16:9 blit stereo is not fused. Do not keep offering it.
        // Verified on this DLL 2026-08-16: CreateDevice + Reset forced 3168x3100,
        // desktop went black, one OpenVR Submit then rt0=null / waiting room.
        if (g_TryHmdSwapchain)
            PersistSkip("hmd_swap", "black desktop + SteamVR waiting room after Reset");
        // TransferSurface(..., TRUE) + WaitDeviceIdle during loading with
        // compositor DoNotHaveFocus coincided with the 1 FPS crash.
        if (g_TryWaitIdle)
            PersistSkip("wait_idle", "WaitDeviceIdle during loading crash");
        // Absolute HMD yaw/pitch on the live CViewSetup replaced the tram's
        // scripted camera (e.g. yaw 96.5 → HMD 14.7) and the headset went
        // black after the load logo. L4D2VR only writes HMD on stereo copies.
        if (g_TryAbsView)
            PersistSkip("abs_view", "absolute HMD on live CViewSetup blacked gameplay after load logo");
        if (g_TryStereoCopy)
            PersistSkip("stereo_copy", "16:9 blit stereo is not fused");
        PersistSkip("stereo_fov", "HMD FOV in 16:9 pixels is magnified and not fused");
        PersistSkip("ff_hmdfit", "unbind A2R10 FullFrame whites HMD; G-buffer 1584 vs PushRT 2560");
        PersistSkip("ff_gbfit", "LITERAL FullFrame+G-buffer 1584 died on background04 before stereo; user miss");
        // 2026-09-04: 8 HMD-fb pass-throughs at 3168 OpenXR succeeded, then
        // the first stereo pair died after pose latch. Copies wrote
        // m_eStereoEye LEFT/RIGHT and drew RIGHT then LEFT. BM RenderView
        // indexes this+0x744 by [ebx+0x1C]. Leave MONO (0); LEFT then RIGHT.
        // 2026-09-05: hl2vr native ignored that skip, wrote stereoEye=2 on
        // the first RIGHT copy, and presented white freeze. Always skip.
        PersistSkip("hl2vr_eye",
            "first stereo RenderView died after writing m_eStereoEye LEFT/RIGHT (OpenXR, 2026-09-04); hl2vr native retry presented white freeze (2026-09-05)");
        PersistSkip("hl2vr_predraw",
            "DrawAllMaterials dummy DrawScreenSpaceRectangle null-called shaderapidx9 (C0000005 eip=0, 2026-09-05)");
        for (const auto& dir : FlagDirs())
        {
            DeleteFileW((dir + L"\\bmvr_in_hl2vr_proj.flag").c_str());
            DeleteFileW((dir + L"\\bmvr_in_hl2vr_cproj.flag").c_str());
            DeleteFileW((dir + L"\\bmvr_in_hl2vr_neye.flag").c_str());
        }
        if (!g_WorldEyeSizeOptIn)
            PersistSkip("hmd_world", "LITERAL FullFrame+G-buffer at SteamVR rec still PushRT 2560x1440; HMD warp + bottom garbage");
        // Do not PersistSkip fl_gbmatch — other builds wrote a policy skip
        // that silently disabled the flashlight path. Crash-sticky only.
        // Named HDR wrap + rewriting CSimpleWorldView PushRT(NULL) dies after
        // the third bind even when the named RT is 2560x1440. Do not retry it.
        g_TryNamedStereoWrap = false;
        Log("hmd_offscreen=%d hmd_world=%d optIn=%d (%s)",
            g_TryOffscreenHmd ? 1 : 0, g_TryOffscreenWorldGrow ? 1 : 0,
            g_WorldEyeSizeOptIn ? 1 : 0,
            g_TryOffscreenWorldGrow
                ? "world FullFrame/G-buffer grown to eye size"
                : "eyes at SteamVR rec; world RTs stay HWND; gbmatch blit");
        SetStage("init");
    }

    bool TryHmdSwapchain() { return g_TryHmdSwapchain; }
    bool TryHmdFramebuffer() { return g_TryHmdFramebuffer; }
    bool TryHmdNative() { return g_TryHmdNative; }
    bool TryNamedRenderTargets() { return g_TryNamedRT; }
    bool TryNamedStereoWrap() { return g_TryNamedStereoWrap; }
    bool TryStereoRenderView() { return g_TryStereoRV; }
    bool TryWaitDeviceIdle() { return g_TryWaitIdle; }
    bool TryAbsoluteHmdView() { return g_TryAbsView; }
    bool TryMenuCompositor() { return g_TryMenuCompositor; }
    bool TryRelativeHmdLook() { return g_TryRelativeHmdLook; }
    bool TryStereoCopy() { return g_TryStereoCopy; }
    bool TryStereoFov() { return g_TryStereoFov; }
    bool TryMatQueue() { return g_TryMatQueue; }
    bool TrySteamVrEyeRt() { return g_TrySteamVrEyeRt; }
    bool TryOffscreenHmd() { return g_TryOffscreenHmd; }
    bool TryOffscreenWorldGrow() { return g_TryOffscreenWorldGrow; }
    static bool MapNameIsGameplay(const char* map)
    {
        if (!map || !map[0])
            return false;
        const char* slash = strrchr(map, '/');
        const char* bslash = strrchr(map, '\\');
        if (bslash && (!slash || bslash > slash))
            slash = bslash;
        const char* base = slash ? slash + 1 : map;
        std::string name(base);
        const auto dot = name.rfind('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        for (char& c : name)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (name == "dedicated" || name.rfind("background", 0) == 0)
            return false;
        return true;
    }

    void NoteEngineMapName(const char* map)
    {
        if (!map || !map[0])
            return;
        if (g_NotedEngineMap[0] && std::strcmp(g_NotedEngineMap, map) == 0)
            return;
        std::strncpy(g_NotedEngineMap, map, sizeof(g_NotedEngineMap) - 1);
        g_NotedEngineMap[sizeof(g_NotedEngineMap) - 1] = 0;
        const bool gameplay = MapNameIsGameplay(map);
        g_EngineMapIsBackground = !gameplay;
        Log("World RT map=%s gameplay=%d backgroundBlock=%d",
            map, gameplay ? 1 : 0, g_EngineMapIsBackground ? 1 : 0);
        if (!g_IsBlueShift)
        {
            const char* slash = strrchr(map, '/');
            const char* bslash = strrchr(map, '\\');
            if (bslash && (!slash || bslash > slash))
                slash = bslash;
            const char* base = slash ? slash + 1 : map;
            if ((base[0] == 'b' || base[0] == 'B')
                && (base[1] == 's' || base[1] == 'S')
                && base[2] == '_')
            {
                g_IsBlueShift = true;
                Log("Blue Shift session (map %s)", map);
            }
        }
    }

    void SetGameplayWorldRts(bool gameplayMap)
    {
        g_GameplayWorldRts = gameplayMap;
        if (gameplayMap && g_TryOffscreenWorldGrow && !g_WorldRtGrowArmed)
        {
            g_WorldRtGrowArmed = true;
            Log("World RT gameplay map (FullFrame grow is CreateNamedRT, not next-load)");
        }
    }
    bool TryHudOverlay() { return g_TryHudOverlay; }
    bool TryVguiPaint() { return g_TryVguiPaint; }
    bool TryGameUiActivate() { return g_TryGameUiActivate; }
    bool TryMeleeTrace() { return g_TryMeleeTrace; }
    bool TryFramebufferOverride() { return g_TryFbOverride; }
    bool TryFullFrameStereo() { return g_TryFullFrameStereo; }
    bool TryHmdFitFullFrame() { return g_TryHmdFitFullFrame; }
    bool TryEyeFitWorldRts() { return g_TryEyeFitWorldRts; }
    bool TryFlashlightGbMatch() { return g_TryFlashlightGbMatch; }
    bool TryGbLeftSkip() { return g_TryGbLeftSkip; }
    bool TryDrawHud() { return g_TryDrawHud; }
    bool TryDrawModelExecute() { return g_TryDme; }
    bool TryHl2vrQueuedWgp() { return g_TryHl2vrQueuedWgp; }
    bool TryHl2vrStereoEye() { return g_TryHl2vrStereoEye; }
    bool TryHl2vrHam() { return g_TryHl2vrHam; }
    bool TryHl2vrPixelVis() { return g_TryHl2vrPixelVis; }
    bool TryHl2vrPredraw() { return g_TryHl2vrPredraw; }
    bool TryHl2vrPlayerCull() { return g_TryHl2vrPlayerCull; }
    bool TryStereoVisReset() { return g_TryStereoVisReset; }
    bool TryCtrlPose() { return g_TryCtrlPose; }
    bool TryCtrlVm() { return g_TryCtrlVm; }
    bool TryVrGloves() { return g_TryVrGloves; }
    bool TryHandOverlay() { return g_TryHandOverlay; }

    void DisableNamedRenderTargets(const char* reason)
    {
        PersistSkip("named_rt", reason);
    }

    void DisableStereoRenderView(const char* reason)
    {
        PersistSkip("stereo_rv", reason);
    }

    void DisableStereoFov(const char* reason)
    {
        PersistSkip("stereo_fov", reason);
    }

    // In-process record of which crash-sticky flags are currently on disk.
    // Callers re-issue EndRisky every frame (dRenderView clears nine flags per
    // stereo pair once s_hmdFbFrames >= 120, ctrl_pose / hand_overlay clear
    // per frame). Each real EndRisky is N DeleteFileW + a heartbeat file
    // rewrite on the render thread — ~0.3-1 ms per call, several ms per
    // frame. The disk state only changes on arm/disarm transitions, so only
    // those touch the filesystem. Disk semantics are unchanged: a flag file
    // exists exactly while the attempt is armed.
    static std::mutex g_RiskyMutex;
    static std::vector<std::wstring> g_RiskyArmed;

    static bool RiskyArmedLocked(const wchar_t* name)
    {
        for (const auto& n : g_RiskyArmed)
            if (n == name)
                return true;
        return false;
    }

    void BeginRisky(const wchar_t* name)
    {
        if (!name)
            return;
        {
            std::lock_guard<std::mutex> lock(g_RiskyMutex);
            if (RiskyArmedLocked(name))
                return;
            g_RiskyArmed.emplace_back(name);
        }
        char ascii[64]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, ascii, sizeof(ascii) - 1, nullptr, nullptr);
        char stage[80];
        snprintf(stage, sizeof(stage), "in_%s", ascii);
        SetStage(stage);

        for (const auto& dir : FlagDirs())
        {
            const std::wstring path = dir + L"\\bmvr_in_" + name + L".flag";
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
    }

    void EndRisky(const wchar_t* name)
    {
        if (!name)
            return;
        {
            std::lock_guard<std::mutex> lock(g_RiskyMutex);
            bool found = false;
            for (size_t i = 0; i < g_RiskyArmed.size(); ++i)
            {
                if (g_RiskyArmed[i] == name)
                {
                    g_RiskyArmed.erase(g_RiskyArmed.begin() + static_cast<std::ptrdiff_t>(i));
                    found = true;
                    break;
                }
            }
            if (!found)
                return;
        }
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_" + name + L".flag").c_str());
        SetStage("ok");
    }

    void FitHmdAspectInWindow(uint32_t winW, uint32_t winH, float aspect, uint32_t& eyeW, uint32_t& eyeH);

    void ComputeHmdFramebufferSize(uint32_t recW, uint32_t recH, uint32_t winW, uint32_t winH, float projAspect)
    {
        if (g_FramebufferWidth >= 640 && g_FramebufferHeight >= 360)
            return;
        if (recW < 640 || recH < 360)
            return;

        // L4D2VR/Portal2: m_RenderWidth/Height = GetRecommendedRenderTargetSize().
        // Named RTs at that size died on BM (PushRT NULL = backbuffer). Matching
        // G-buffer + swapchain is the equivalent. HWND stays the desktop window.
        // Exclusive 3k hmd_swap blacked the desktop; this path stays windowed.
        // Crash-sticky hmd_native falls back to fitting HMD aspect in the window.
        if (g_TryHmdNative)
        {
            uint32_t eyeW = (recW + 15u) & ~15u;
            uint32_t eyeH = (recH + 15u) & ~15u;
            if (eyeW >= 640 && eyeH >= 360)
            {
                // Index/G2 rec is often taller than a 1080p HWND (e.g. 1444x1800
                // vs 1920x1080). Stamping that as the engine G-buffer made
                // GetBackBufferDimensions 1456x1808 on a 1080 swapchain and
                // the world could not run. Eyes still use ComputeOffscreenEyeSize
                // (not HWND-clamped); world RTs stay at the window.
                const bool overWindow = winW >= 640 && winH >= 360
                    && (eyeW > winW + 32 || eyeH > winH + 32);
                if (overWindow)
                {
                    static bool s_loggedNativeOverWindow;
                    if (!s_loggedNativeOverWindow)
                    {
                        s_loggedNativeOverWindow = true;
                        Log("Skip hmd_native size %ux%u over window %ux%u (world stays HWND; eyes stay SteamVR rec)",
                            eyeW, eyeH, winW, winH);
                    }
                }
                else
                {
                    static bool s_stampedNative;
                    if (!s_stampedNative)
                    {
                        s_stampedNative = true;
                        BeginRisky(L"hmd_native");
                        Log("HMD native G-buffer %ux%u (OpenVR recommended %ux%u, window %ux%u)",
                            eyeW, eyeH, recW, recH, winW, winH);
                    }
                    g_FramebufferWidth = eyeW;
                    g_FramebufferHeight = eyeH;
                    return;
                }
            }
        }

        if (winW < 640)
            winW = recW;
        if (winH < 360)
            winH = recH;
        float aspect = projAspect;
        if (!(aspect > 0.5f && aspect < 3.f))
            aspect = static_cast<float>(recW) / static_cast<float>(recH);
        uint32_t eyeH = winH & ~1u;
        uint32_t eyeW = (static_cast<uint32_t>(static_cast<float>(eyeH) * aspect + 0.5f) + 1u) & ~1u;
        if (eyeW > winW)
        {
            eyeW = winW & ~1u;
            eyeH = (static_cast<uint32_t>(static_cast<float>(eyeW) / aspect + 0.5f) + 1u) & ~1u;
        }
        if (eyeW > recW || eyeH > recH)
        {
            const float sx = static_cast<float>(recW) / static_cast<float>(eyeW);
            const float sy = static_cast<float>(recH) / static_cast<float>(eyeH);
            const float s = sx < sy ? sx : sy;
            eyeW = (static_cast<uint32_t>(static_cast<float>(eyeW) * s) + 1u) & ~1u;
            eyeH = (static_cast<uint32_t>(static_cast<float>(eyeH) * s) + 1u) & ~1u;
        }
        if (eyeW < 640 || eyeH < 360)
            return;
        // Deferred MRT / HUD downsample want 16-pixel pitch. 1576 (1440*1.097
        // rounded even) is 1576%16=8. Menu 2D survived; first 3D/HUD did not.
        eyeW = (eyeW + 15u) & ~15u;
        eyeH = (eyeH + 15u) & ~15u;
        if (g_RenderScale > 1.001f || g_RenderScale < 0.999f)
        {
            uint32_t scaledW = (static_cast<uint32_t>(static_cast<float>(eyeW) * g_RenderScale + 0.5f) + 15u) & ~15u;
            uint32_t scaledH = (static_cast<uint32_t>(static_cast<float>(eyeH) * g_RenderScale + 0.5f) + 15u) & ~15u;
            const uint32_t recAlignW = (recW + 15u) & ~15u;
            const uint32_t recAlignH = (recH + 15u) & ~15u;
            if (scaledW > recAlignW)
                scaledW = recAlignW;
            if (scaledH > recAlignH)
                scaledH = recAlignH;
            if (scaledW >= 640 && scaledH >= 360)
            {
                Log("RenderScale %.2f G-buffer %ux%u -> %ux%u (OpenVR rec %ux%u)",
                    g_RenderScale, eyeW, eyeH, scaledW, scaledH, recW, recH);
                if (scaledW >= 2400 || scaledH >= 2400)
                    Log("RenderScale size is near the 2544 first-stereo crash. Lower RenderScale if this launch dies.");
                eyeW = scaledW;
                eyeH = scaledH;
            }
        }
        // RenderScale 1.5 of 1584x1440 is 2384x2160. That is taller than a
        // 1440p HWND. GetScreenSize 2160 on a 1440 swapchain smears the
        // bottom of the menu and spawn (2026-08-18) and stereo RenderView
        // at 2384 hung after pass-through 8/8.
        if (eyeW > winW || eyeH > winH)
        {
            const uint32_t beforeW = eyeW, beforeH = eyeH;
            FitHmdAspectInWindow(winW, winH, aspect, eyeW, eyeH);
            Log("G-buffer %ux%u exceeds window %ux%u, fitted HMD aspect %ux%u",
                beforeW, beforeH, winW, winH, eyeW, eyeH);
        }
        g_FramebufferWidth = eyeW;
        g_FramebufferHeight = eyeH;
    }

    void FitHmdAspectInWindow(uint32_t winW, uint32_t winH, float aspect, uint32_t& eyeW, uint32_t& eyeH)
    {
        if (winW < 640)
            winW = 1280;
        if (winH < 360)
            winH = 720;
        if (!(aspect > 0.5f && aspect < 3.f))
            aspect = 1.1f;
        uint32_t h = winH & ~1u;
        uint32_t w = (static_cast<uint32_t>(static_cast<float>(h) * aspect + 0.5f) + 1u) & ~1u;
        if (w > winW)
        {
            w = winW & ~1u;
            h = (static_cast<uint32_t>(static_cast<float>(w) / aspect + 0.5f) + 1u) & ~1u;
        }
        w = (w + 15u) & ~15u;
        h = (h + 15u) & ~15u;
        while (w > winW && w >= 16)
            w -= 16;
        while (h > winH && h >= 16)
            h -= 16;
        if (w < 640)
            w = winW & ~15u;
        if (h < 360)
            h = winH & ~15u;
        eyeW = w;
        eyeH = h;
    }

    // FindWindowA walks the desktop window list under the user32 lock. The D3D
    // SetViewport / SetScissorRect / Clear / StretchRect hooks, the
    // IMaterialSystem size hooks and DrawScreenSpaceRectangle all asked for the
    // client size per call — hundreds of window-list walks per frame on the
    // render thread. Cache the HWND (re-validated with IsWindow once a second,
    // re-searched at most every 250 ms while missing) and the client rect for
    // a few milliseconds. The window only changes size on a video-mode Reset.
    static std::atomic<HWND> g_GameHwnd{ nullptr };
    static std::atomic<DWORD> g_GameHwndCheckMs{ 0 };
    static std::atomic<uint64_t> g_ClientSizePacked{ 0 };
    static std::atomic<int64_t> g_ClientSizeQpc{ 0 };

    static int64_t QpcTicksPerMs()
    {
        static int64_t s_perMs = 0;
        if (s_perMs == 0)
        {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            s_perMs = f.QuadPart > 1000 ? f.QuadPart / 1000 : 1;
        }
        return s_perMs;
    }

    HWND GameWindow()
    {
        HWND hwnd = g_GameHwnd.load(std::memory_order_relaxed);
        const DWORD now = GetTickCount();
        const DWORD lastCheck = g_GameHwndCheckMs.load(std::memory_order_relaxed);
        if (hwnd)
        {
            if (now - lastCheck < 1000)
                return hwnd;
            if (IsWindow(hwnd))
            {
                g_GameHwndCheckMs.store(now, std::memory_order_relaxed);
                return hwnd;
            }
            g_GameHwnd.store(nullptr, std::memory_order_relaxed);
            g_ClientSizePacked.store(0, std::memory_order_relaxed);
        }
        else if (lastCheck != 0 && now - lastCheck < 250)
            return nullptr;
        g_GameHwndCheckMs.store(now ? now : 1, std::memory_order_relaxed);
        hwnd = FindWindowA("Valve001", nullptr);
        if (!hwnd)
            hwnd = FindWindowA(nullptr, "Black Mesa");
        g_GameHwnd.store(hwnd, std::memory_order_relaxed);
        return hwnd;
    }

    bool QueryWindowClientSize(uint32_t& width, uint32_t& height)
    {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        const int64_t last = g_ClientSizeQpc.load(std::memory_order_acquire);
        if (last != 0 && (t.QuadPart - last) < 4 * QpcTicksPerMs())
        {
            const uint64_t packed = g_ClientSizePacked.load(std::memory_order_relaxed);
            if (packed == 0)
                return false;
            width = static_cast<uint32_t>(packed >> 32);
            height = static_cast<uint32_t>(packed & 0xffffffffu);
            return true;
        }
        uint64_t packed = 0;
        HWND hwnd = GameWindow();
        RECT rc{};
        if (hwnd && GetClientRect(hwnd, &rc))
        {
            const uint32_t w = static_cast<uint32_t>(rc.right - rc.left);
            const uint32_t h = static_cast<uint32_t>(rc.bottom - rc.top);
            if (w >= 640 && h >= 360)
                packed = (static_cast<uint64_t>(w) << 32) | h;
        }
        g_ClientSizePacked.store(packed, std::memory_order_relaxed);
        g_ClientSizeQpc.store(t.QuadPart ? t.QuadPart : 1, std::memory_order_release);
        if (packed == 0)
            return false;
        width = static_cast<uint32_t>(packed >> 32);
        height = static_cast<uint32_t>(packed & 0xffffffffu);
        return true;
    }

    static bool SizesNear(uint32_t a, uint32_t b, int slop = 48)
    {
        const int d = static_cast<int>(a) - static_cast<int>(b);
        return d > -slop && d < slop;
    }

    bool ComputeOffscreenEyeSize(uint32_t& width, uint32_t& height)
    {
        if (!g_TryOffscreenHmd)
            return false;
        const uint32_t recW = g_RecommendedEyeWidth;
        const uint32_t recH = g_RecommendedEyeHeight;
        if (recW < 640 || recH < 360)
            return false;
        float s = g_RenderScale;
        if (!(s > 0.24f && s < 4.f))
            s = 1.f;
        uint32_t w = (static_cast<uint32_t>(static_cast<float>(recW) * s + 0.5f) + 15u) & ~15u;
        uint32_t h = (static_cast<uint32_t>(static_cast<float>(recH) * s + 0.5f) + 15u) & ~15u;
        if (w > 4096)
            w = 4096;
        if (h > 4096)
            h = 4096;
        if (w < 640)
            w = (recW + 15u) & ~15u;
        if (h < 360)
            h = (recH + 15u) & ~15u;
        width = w;
        height = h;
        return w >= 640 && h >= 360;
    }

    bool WorldRtGrowActive()
    {
        // FullFrame is created at SetMode (inside CreateDevice), often before
        // any map name exists. Blocking on background* left G-buffers at the
        // HWND after New Game: background01 allocated 1920x1080, then bm_c0a0a
        // reused those RTs (G2 log 2026-09-05, worldMatch=0). Grow whenever
        // the user opted in; OffscreenWorldMatchesEyes still waits for a
        // gameplay map so the menu stays on the 2D letterbox path.
        return g_TryOffscreenWorldGrow && g_TryOffscreenHmd;
    }

    bool ComputeGrownWorldFramebuffer(uint32_t& width, uint32_t& height)
    {
        if (!WorldRtGrowActive())
            return false;
        return ComputeOffscreenEyeSize(width, height);
    }

    bool ComputeWorldRtOverrideSize(uint32_t& width, uint32_t& height)
    {
        if (!g_TryOffscreenWorldGrow || !g_TryOffscreenHmd)
            return false;
        uint32_t eyeW = 0, eyeH = 0;
        if (!ComputeOffscreenEyeSize(eyeW, eyeH))
            return false;
        if (g_FullFrameActualWidth < 640 || g_FullFrameActualHeight < 360)
            return false;
        if (!SizesNear(g_FullFrameActualWidth, eyeW) || !SizesNear(g_FullFrameActualHeight, eyeH))
            return false;
        width = eyeW;
        height = eyeH;
        return true;
    }

    bool ComputeGrownWorldGbuffer(uint32_t& width, uint32_t& height)
    {
        uint32_t eyeW = 0, eyeH = 0;
        if (!ComputeGrownWorldFramebuffer(eyeW, eyeH))
            return false;
        // Eye-sized G-buffers behind a window-sized FullFrame is the 2026-08-26
        // miss: worldMatch stays 0, the view stays 2560x1440, and the scene
        // lands in the top slice of the G-buffer. Stay on the working path
        // until the level load actually grew FullFrame.
        if (!SizesNear(g_FullFrameActualWidth, eyeW) || !SizesNear(g_FullFrameActualHeight, eyeH))
        {
            static uint32_t s_loggedW, s_loggedH;
            if (s_loggedW != g_FullFrameActualWidth || s_loggedH != g_FullFrameActualHeight)
            {
                s_loggedW = g_FullFrameActualWidth;
                s_loggedH = g_FullFrameActualHeight;
                Log("World RT grow held: FullFrame %ux%u != eye %ux%u; override stays HWND "
                    "so G-buffers cannot outgrow FullFrame",
                    g_FullFrameActualWidth, g_FullFrameActualHeight, eyeW, eyeH);
            }
            return false;
        }
        width = eyeW;
        height = eyeH;
        return true;
    }

    bool OffscreenWorldMatchesEyes()
    {
        if (!g_TryOffscreenWorldGrow || !g_TryOffscreenHmd || !g_GameplayWorldRts)
            return false;
        uint32_t eyeW = 0, eyeH = 0;
        if (!ComputeOffscreenEyeSize(eyeW, eyeH))
            return false;
        if (g_FullFrameActualWidth < 640 || g_FullFrameActualHeight < 360)
            return false;
        if (g_GbActualWidth < 640 || g_GbActualHeight < 360)
            return false;
        return SizesNear(g_FullFrameActualWidth, eyeW) && SizesNear(g_FullFrameActualHeight, eyeH)
            && SizesNear(g_GbActualWidth, eyeW) && SizesNear(g_GbActualHeight, eyeH);
    }

    void SetOpenXrHelperSession(bool active)
    {
        (void)active;
        // Session flag used to bypass gbmatch; that froze 2480-tall views
        // into a 1440 G-buffer (2026-08-30). Keep the hook for logs.
    }

    bool UseGbMatchViewLock()
    {
        // When FullFrame+G-buffer+eyes match, stereo views must be that size
        // so flashlight apply hits the deferred buffers. Mismatch (GB 3728 /
        // view 2560) was the 2026-08-26 flashlight/ghost failure.
        // Do not bypass this on OpenXR: CViewSetup height 2480 into a 1440
        // G-buffer froze the frame with last-scanline smear (2026-08-30).
        return g_TryFlashlightGbMatch && !OffscreenWorldMatchesEyes();
    }

    bool HaveHmdFramebufferSize(uint32_t& width, uint32_t& height)
    {
        if (!g_TryHmdFramebuffer || g_FramebufferWidth < 640 || g_FramebufferHeight < 360)
            return false;
        // Size is latched at CreateDevice. Do not refit to the live HWND:
        // spawn Reset made GetClientRect 1920x1080 while eyes were 1584x1440,
        // EnsureStereoEyeSurfaces rebuilt 1200x1072 mid-frame and Present
        // died (2026-08-18).
        width = g_FramebufferWidth;
        height = g_FramebufferHeight;
        return true;
    }

    bool ApplyHmdAspectBackbuffer(uint32_t& width, uint32_t& height)
    {
        uint32_t w = 0, h = 0;
        if (!HaveHmdFramebufferSize(w, h))
            return false;
        if (width == w && height == h)
            return false;
        width = w;
        height = h;
        return true;
    }
}
