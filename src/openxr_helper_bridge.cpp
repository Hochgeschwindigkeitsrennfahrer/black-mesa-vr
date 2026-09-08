#include "openxr_helper_bridge.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "game.h"
#include "openxr_bridge_protocol.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    std::atomic<bool> g_OpenXrHelperStarted{ false };
    std::mutex g_OpenXrBridgeStateMutex;
    L4D2VROpenXrBridgeState* g_OpenXrBridgeState = nullptr;

    void Trim(std::string& value)
    {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    }

    bool ParseBool(std::string value, bool fallback)
    {
        Trim(value);
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (value == "1" || value == "true" || value == "on" || value == "yes")
            return true;
        if (value == "0" || value == "false" || value == "off" || value == "no")
            return false;
        return fallback;
    }

    uint32_t ParseUint(std::string value, uint32_t fallback, uint32_t minValue, uint32_t maxValue)
    {
        Trim(value);
        if (value.empty())
            return fallback;

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
        if (!end || *end != '\0')
            return fallback;

        return std::clamp(static_cast<uint32_t>(parsed), minValue, maxValue);
    }

    int ParseProjectionEye(std::string value, int fallback)
    {
        Trim(value);
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (value == "none" || value == "off" || value == "disabled" || value == "-1")
            return -1;
        if (value == "left" || value == "l" || value == "0")
            return L4D2VR_OPENXR_EYE_LEFT;
        if (value == "right" || value == "r" || value == "1")
            return L4D2VR_OPENXR_EYE_RIGHT;
        return fallback;
    }

    int ParseFlipSubmitY(std::string value, int fallback)
    {
        Trim(value);
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (value == "auto" || value == "-1")
            return -1;
        if (value == "1" || value == "true" || value == "on" || value == "yes")
            return 1;
        if (value == "0" || value == "false" || value == "off" || value == "no")
            return 0;
        return fallback;
    }

    const char* FlipSubmitYName(int option)
    {
        if (option > 0)
            return "on";
        if (option == 0)
            return "off";
        return "auto";
    }

    const char* ProjectionEyeName(int eye)
    {
        if (eye == L4D2VR_OPENXR_EYE_LEFT)
            return "left";
        if (eye == L4D2VR_OPENXR_EYE_RIGHT)
            return "right";
        return "none";
    }

    std::unordered_map<std::string, std::string> ReadConfigValues()
    {
        std::unordered_map<std::string, std::string> values;

        auto parsePath = [&](const char* path)
            {
                std::ifstream stream(path);
                if (!stream)
                    return;

                std::string line;
                while (std::getline(stream, line))
                {
                    size_t cut = std::string::npos;
                    const size_t p1 = line.find("//");
                    const size_t p2 = line.find('#');
                    const size_t p3 = line.find(';');
                    if (p1 != std::string::npos) cut = p1;
                    if (p2 != std::string::npos) cut = (cut == std::string::npos) ? p2 : std::min(cut, p2);
                    if (p3 != std::string::npos) cut = (cut == std::string::npos) ? p3 : std::min(cut, p3);
                    if (cut != std::string::npos)
                        line.erase(cut);

                    Trim(line);
                    if (line.empty())
                        continue;

                    const size_t eq = line.find('=');
                    if (eq == std::string::npos)
                        continue;

                    std::string key = line.substr(0, eq);
                    std::string value = line.substr(eq + 1);
                    Trim(key);
                    Trim(value);
                    if (key.size() >= 3 &&
                        static_cast<unsigned char>(key[0]) == 0xEF &&
                        static_cast<unsigned char>(key[1]) == 0xBB &&
                        static_cast<unsigned char>(key[2]) == 0xBF)
                    {
                        key.erase(0, 3);
                        Trim(key);
                    }

                    if (!key.empty())
                        values[key] = value;
                }
            };

        parsePath("VR\\config.txt");
        parsePath("VR\\config2.txt");
        return values;
    }

    std::wstring CurrentDirectoryPath()
    {
        DWORD required = GetCurrentDirectoryW(0, nullptr);
        if (required == 0)
            return L".";

        std::wstring path(required, L'\0');
        DWORD written = GetCurrentDirectoryW(required, path.data());
        if (written == 0 || written >= required)
            return L".";

        path.resize(written);
        return path;
    }

    std::wstring QuoteArg(const std::wstring& value)
    {
        std::wstring out = L"\"";
        for (wchar_t ch : value)
        {
            if (ch == L'"')
                out += L'\\';
            out += ch;
        }
        out += L"\"";
        return out;
    }

    std::string StatusName(uint32_t status)
    {
        switch (static_cast<L4D2VROpenXrBridgeStatus>(status))
        {
        case L4D2VROpenXrBridgeStatus::Idle: return "Idle";
        case L4D2VROpenXrBridgeStatus::Starting: return "Starting";
        case L4D2VROpenXrBridgeStatus::LoaderLoaded: return "LoaderLoaded";
        case L4D2VROpenXrBridgeStatus::InstanceCreated: return "InstanceCreated";
        case L4D2VROpenXrBridgeStatus::SessionCreated: return "SessionCreated";
        case L4D2VROpenXrBridgeStatus::SessionRunning: return "SessionRunning";
        case L4D2VROpenXrBridgeStatus::SubmittedFrame: return "SubmittedFrame";
        case L4D2VROpenXrBridgeStatus::Completed: return "Completed";
        case L4D2VROpenXrBridgeStatus::Failed: return "Failed";
        case L4D2VROpenXrBridgeStatus::WaitingForSharedTextures: return "WaitingForSharedTextures";
        case L4D2VROpenXrBridgeStatus::SharedTexturesReady: return "SharedTexturesReady";
        default: return "Unknown";
        }
    }

    bool SameSharedTextureDesc(
        const L4D2VROpenXrSharedTextureDesc& lhs,
        const L4D2VROpenXrSharedTextureDesc& rhs)
    {
        return lhs.valid == rhs.valid &&
            lhs.width == rhs.width &&
            lhs.height == rhs.height &&
            lhs.format == rhs.format &&
            lhs.sampleCount == rhs.sampleCount &&
            lhs.handleType == rhs.handleType &&
            lhs.queueFamilyIndex == rhs.queueFamilyIndex &&
            lhs.kmtHandle == rhs.kmtHandle &&
            lhs.image == rhs.image &&
            lhs.reserved0 == rhs.reserved0 &&
            lhs.uMin == rhs.uMin &&
            lhs.vMin == rhs.vMin &&
            lhs.uMax == rhs.uMax &&
            lhs.vMax == rhs.vMax &&
            lhs.renderFovXDeg == rhs.renderFovXDeg &&
            lhs.renderAspect == rhs.renderAspect;
    }

    bool SameOpenXrOverlayDesc(
        const L4D2VROpenXrOverlayDesc& lhs,
        const L4D2VROpenXrOverlayDesc& rhs)
    {
        return lhs.valid == rhs.valid &&
            lhs.visible == rhs.visible &&
            lhs.widthMeters == rhs.widthMeters &&
            lhs.heightMeters == rhs.heightMeters &&
            lhs.distanceMeters == rhs.distanceMeters &&
            lhs.curvature == rhs.curvature &&
            lhs.offsetMeters[0] == rhs.offsetMeters[0] &&
            lhs.offsetMeters[1] == rhs.offsetMeters[1] &&
            lhs.offsetMeters[2] == rhs.offsetMeters[2] &&
            lhs.orientation[0] == rhs.orientation[0] &&
            lhs.orientation[1] == rhs.orientation[1] &&
            lhs.orientation[2] == rhs.orientation[2] &&
            lhs.orientation[3] == rhs.orientation[3] &&
            SameSharedTextureDesc(lhs.texture, rhs.texture);
    }
}

OpenXrHelperLaunchConfig L4D2VR_ReadOpenXrHelperLaunchConfig()
{
    OpenXrHelperLaunchConfig config{};
    const auto values = ReadConfigValues();

    auto get = [&](const char* key) -> std::string
        {
            const auto it = values.find(key);
            return it != values.end() ? it->second : std::string();
        };

    if (const std::string value = get("OpenXRHelper"); !value.empty())
        config.enabled = ParseBool(value, config.enabled);
    if (const std::string value = get("OpenXRHelperSwapProjectionEyes"); !value.empty())
        config.swapProjectionEyes = ParseFlipSubmitY(value, config.swapProjectionEyes);
    if (const std::string value = get("OpenXRHelperSwapProjectionViewOrder"); !value.empty())
        config.swapProjectionViewOrder = ParseBool(value, config.swapProjectionViewOrder);
    if (const std::string value = get("OpenXRHelperMirrorProjectionHorizontal"); !value.empty())
        config.mirrorProjectionHorizontal = ParseBool(value, config.mirrorProjectionHorizontal);
    if (const std::string value = get("OpenXRHelperDisableProjectionLayer"); !value.empty())
        config.disableProjectionLayer = ParseBool(value, config.disableProjectionLayer);
    if (const std::string value = get("OpenXRHelperUseSymmetricProjectionFov"); !value.empty())
        config.useSymmetricProjectionFov = ParseBool(value, config.useSymmetricProjectionFov);
    if (const std::string value = get("OpenXRHelperUseGameRenderPoseForProjection"); !value.empty())
        config.useGameRenderPoseForProjection = ParseBool(value, config.useGameRenderPoseForProjection);
    if (const std::string value = get("OpenXRHelperForceMonoProjectionEye"); !value.empty())
        config.forceMonoProjectionEye = ParseProjectionEye(value, config.forceMonoProjectionEye);
    if (const std::string value = get("OpenXRHelperForceMonoProjectionView"); !value.empty())
        config.forceMonoProjectionView = ParseProjectionEye(value, config.forceMonoProjectionView);
    if (const std::string value = get("OpenXRHelperFlipSubmitY"); !value.empty())
        config.flipSubmitY = ParseFlipSubmitY(value, config.flipSubmitY);
    if (const std::string value = get("OpenXRHelperSwapGameEyeOrigins"); !value.empty())
        config.swapGameEyeOrigins = ParseBool(value, config.swapGameEyeOrigins);
    if (const std::string value = get("OpenXRSwapGameEyeOrigins"); !value.empty())
        config.swapGameEyeOrigins = ParseBool(value, config.swapGameEyeOrigins);
    if (const std::string value = get("OpenXRHelperDisableQuadOverlays"); !value.empty())
        config.disableQuadOverlays = ParseBool(value, config.disableQuadOverlays);
    if (const std::string value = get("OpenXRHelperHandTracking"); !value.empty())
        config.enableHandTracking = ParseBool(value, config.enableHandTracking);
    if (const std::string value = get("OpenXRHelperSubmitTestFrames"); !value.empty())
        config.submitTestFrames = ParseUint(value, config.submitTestFrames, 0, 1000000);
    if (const std::string value = get("OpenXRHelperWaitReadySeconds"); !value.empty())
        config.waitReadySeconds = ParseUint(value, config.waitReadySeconds, 1, 600);

    return config;
}

bool L4D2VR_StartOpenXrHelper(const OpenXrHelperLaunchConfig& config)
{
    if (!config.enabled)
        return false;

    bool expected = false;
    if (!g_OpenXrHelperStarted.compare_exchange_strong(expected, true))
        return true;

    auto moduleDirectory = []() -> std::wstring
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&L4D2VR_StartOpenXrHelper),
                &module) || !module)
            {
                return {};
            }
            wchar_t path[MAX_PATH]{};
            const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return {};
            std::wstring s(path, n);
            const size_t slash = s.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
                return {};
            return s.substr(0, slash);
        };

    const std::wstring gameRoot = CurrentDirectoryPath();
    const std::wstring moduleDir = moduleDirectory();
    std::wstring helperDir = gameRoot + L"\\VR\\openxr_helper64";
    std::wstring helperExe = helperDir + L"\\OpenXRHelper64.exe";
    if (GetFileAttributesW(helperExe.c_str()) == INVALID_FILE_ATTRIBUTES && !moduleDir.empty())
    {
        helperDir = moduleDir + L"\\VR\\openxr_helper64";
        helperExe = helperDir + L"\\OpenXRHelper64.exe";
    }
    if (GetFileAttributesW(helperExe.c_str()) == INVALID_FILE_ATTRIBUTES && !moduleDir.empty())
    {
        // shaderapidx9 loads d3d9.dll from Black Mesa\bin\. Helper is next to bms.exe.
        helperDir = moduleDir + L"\\..\\VR\\openxr_helper64";
        helperExe = helperDir + L"\\OpenXRHelper64.exe";
    }
    const std::wstring helperLog = helperDir + L"\\openxr_helper64_from_game.log";

    if (GetFileAttributesW(helperExe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Game::logMsg("[VR][OpenXRHelper] helper executable not found: %ls", helperExe.c_str());
        g_OpenXrHelperStarted.store(false);
        return false;
    }

    wchar_t mappingName[128] = {};
    std::swprintf(
        mappingName,
        ARRAYSIZE(mappingName),
        L"Local\\L4D2VR_OpenXRHelper_%lu_%lu",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetTickCount()));

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(L4D2VROpenXrBridgeState),
        mappingName);
    if (!mapping)
    {
        Game::logMsg("[VR][OpenXRHelper] CreateFileMapping failed err=%lu", GetLastError());
        g_OpenXrHelperStarted.store(false);
        return false;
    }

    auto* state = static_cast<L4D2VROpenXrBridgeState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(L4D2VROpenXrBridgeState)));
    if (!state)
    {
        Game::logMsg("[VR][OpenXRHelper] MapViewOfFile failed err=%lu", GetLastError());
        CloseHandle(mapping);
        g_OpenXrHelperStarted.store(false);
        return false;
    }

    *state = L4D2VROpenXrBridgeState{};
    state->gamePid = GetCurrentProcessId();
    state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::Starting);
    state->heartbeatTickMs = GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
        g_OpenXrBridgeState = state;
    }

    std::wstringstream commandLine;
    commandLine
        << QuoteArg(helperExe)
        << L" --mapping " << QuoteArg(mappingName)
        << L" --parent " << GetCurrentProcessId()
        << L" --frames " << config.submitTestFrames
        << L" --wait-ready-sec " << config.waitReadySeconds
        << L" --swap-projection-eyes " << config.swapProjectionEyes
        << L" --swap-projection-view-order " << (config.swapProjectionViewOrder ? 1 : 0)
        << L" --mirror-projection-horizontal " << (config.mirrorProjectionHorizontal ? 1 : 0)
        << L" --disable-projection-layer " << (config.disableProjectionLayer ? 1 : 0)
        << L" --use-symmetric-projection-fov " << (config.useSymmetricProjectionFov ? 1 : 0)
        << L" --use-game-render-pose-for-projection " << (config.useGameRenderPoseForProjection ? 1 : 0)
        << L" --force-mono-projection-eye " << config.forceMonoProjectionEye
        << L" --force-mono-projection-view " << config.forceMonoProjectionView
        << L" --flip-submit-y " << config.flipSubmitY
        << L" --disable-quad-overlays " << (config.disableQuadOverlays ? 1 : 0)
        << L" --enable-hand-tracking " << (config.enableHandTracking ? 1 : 0)
        << L" --log " << QuoteArg(helperLog);

    std::wstring commandLineText = commandLine.str();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
        nullptr,
        commandLineText.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        helperDir.c_str(),
        &si,
        &pi))
    {
        const DWORD err = GetLastError();
        Game::logMsg("[VR][OpenXRHelper] CreateProcess failed err=%lu", err);
        state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::Failed);
        state->exitCode = static_cast<int32_t>(err);
        {
            std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
            if (g_OpenXrBridgeState == state)
                g_OpenXrBridgeState = nullptr;
        }
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        g_OpenXrHelperStarted.store(false);
        return false;
    }

    state->helperPid = pi.dwProcessId;
    Game::logMsg(
        "[VR][OpenXRHelper] launched pid=%lu frames=%u waitReadySeconds=%u swapProjectionEyes=%s(%d) swapProjectionViewOrder=%d mirrorProjectionHorizontal=%d disableProjectionLayer=%d useSymmetricProjectionFov=%d useGameRenderPoseForProjection=%d forceMonoProjectionEye=%s(%d) forceMonoProjectionView=%s(%d) flipSubmitY=%s(%d) disableQuadOverlays=%d exe=%ls",
        static_cast<unsigned long>(pi.dwProcessId),
        config.submitTestFrames,
        config.waitReadySeconds,
        FlipSubmitYName(config.swapProjectionEyes),
        config.swapProjectionEyes,
        config.swapProjectionViewOrder ? 1 : 0,
        config.mirrorProjectionHorizontal ? 1 : 0,
        config.disableProjectionLayer ? 1 : 0,
        config.useSymmetricProjectionFov ? 1 : 0,
        config.useGameRenderPoseForProjection ? 1 : 0,
        ProjectionEyeName(config.forceMonoProjectionEye),
        config.forceMonoProjectionEye,
        ProjectionEyeName(config.forceMonoProjectionView),
        config.forceMonoProjectionView,
        FlipSubmitYName(config.flipSubmitY),
        config.flipSubmitY,
        config.disableQuadOverlays ? 1 : 0,
        helperExe.c_str());

    CloseHandle(pi.hThread);

    std::thread([process = pi.hProcess, mapping, state]()
        {
            const DWORD wait = WaitForSingleObject(process, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeProcess(process, &exitCode);

            uint32_t status = 0;
            uint32_t submittedFrames = 0;
            char detailCopy[256] = {};
            {
                std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
                if (state)
                {
                    status = state->status;
                    submittedFrames = state->submittedFrames;
                    std::snprintf(detailCopy, sizeof(detailCopy), "%s", state->detail);
                }
                if (g_OpenXrBridgeState == state)
                    g_OpenXrBridgeState = nullptr;
            }
            Game::logMsg(
                "[VR][OpenXRHelper] exited wait=0x%08lX code=%lu status=%s submittedFrames=%u detail=%s",
                static_cast<unsigned long>(wait),
                static_cast<unsigned long>(exitCode),
                StatusName(status).c_str(),
                submittedFrames,
                detailCopy);

            if (state)
                UnmapViewOfFile(state);
            CloseHandle(mapping);
            CloseHandle(process);
            g_OpenXrHelperStarted.store(false);
        }).detach();

    return true;
}

bool L4D2VR_OpenXrHelperBridgeIsStarted()
{
    return g_OpenXrHelperStarted.load();
}

bool L4D2VR_OpenXrHelperHasSubmittedFrame()
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    return state->submittedFrames > 0 &&
        state->status == static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::SubmittedFrame);
}

bool L4D2VR_ReadOpenXrHelperSubmittedFrames(uint32_t& submittedFrames)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    submittedFrames = state->submittedFrames;
    return true;
}

bool L4D2VR_ReadOpenXrHelperConsumedFrame(uint32_t& consuming, uint32_t& consumed, uint32_t& consumedCount)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    consuming = state->helperConsumingFrameId;
    consumed = state->helperConsumedFrameId;
    consumedCount = state->helperConsumedCount;
    std::atomic_thread_fence(std::memory_order_acquire);
    return true;
}

bool L4D2VR_ReadOpenXrHmdPose(L4D2VROpenXrPoseDesc& pose, uint32_t* generation)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t gen0 = state->trackingPoseGeneration;
        if (gen0 == 0 || (gen0 & 1u))
            continue;

        L4D2VROpenXrPoseDesc snapshot = state->hmdPose;
        const uint32_t gen1 = state->trackingPoseGeneration;
        if (gen0 == gen1 && !(gen1 & 1u) && snapshot.valid)
        {
            pose = snapshot;
            if (generation)
                *generation = gen1;
            return true;
        }
    }

    return false;
}

bool L4D2VR_ReadOpenXrRuntimeViewConfig(L4D2VROpenXrRuntimeViewConfigDesc& config, uint32_t* generation)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t gen0 = state->runtimeViewConfigGeneration;
        if (gen0 == 0 || (gen0 & 1u))
            continue;

        L4D2VROpenXrRuntimeViewConfigDesc snapshot = state->runtimeViewConfig;
        const uint32_t gen1 = state->runtimeViewConfigGeneration;
        if (gen0 == gen1 && !(gen1 & 1u) && snapshot.valid)
        {
            config = snapshot;
            if (generation)
                *generation = gen1;
            return true;
        }
    }

    return false;
}

bool L4D2VR_PeekOpenXrVisibilityMaskGeneration(uint32_t* generation)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state || !generation)
        return false;

    const uint32_t gen = state->visibilityMaskGeneration;
    if (gen == 0 || (gen & 1u))
        return false;

    *generation = gen;
    return true;
}

bool L4D2VR_ReadOpenXrVisibilityMask(L4D2VROpenXrVisibilityMaskDesc& mask, uint32_t* generation)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t gen0 = state->visibilityMaskGeneration;
        if (gen0 == 0 || (gen0 & 1u))
            continue;

        L4D2VROpenXrVisibilityMaskDesc snapshot = state->visibilityMask;
        const uint32_t gen1 = state->visibilityMaskGeneration;
        if (gen0 == gen1 && !(gen1 & 1u) && snapshot.valid)
        {
            mask = snapshot;
            if (generation)
                *generation = gen1;
            return true;
        }
    }

    return false;
}

bool L4D2VR_ReadOpenXrInputState(L4D2VROpenXrInputStateDesc& inputState, uint32_t* generation)
{
    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    const L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return false;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t gen0 = state->inputStateGeneration;
        if (gen0 == 0 || (gen0 & 1u))
            continue;

        L4D2VROpenXrInputStateDesc snapshot = state->inputState;
        const uint32_t gen1 = state->inputStateGeneration;
        if (gen0 == gen1 && !(gen1 & 1u) && snapshot.valid)
        {
            inputState = snapshot;
            if (generation)
                *generation = gen1;
            return true;
        }
    }

    return false;
}

void L4D2VR_PublishOpenXrGameRenderPose(const L4D2VROpenXrPoseDesc& pose)
{
    if (!pose.valid)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    ++state->gameRenderPoseGeneration;
    state->gameRenderPose = pose;
    ++state->gameRenderPoseGeneration;
    state->heartbeatTickMs = GetTickCount64();
}

void L4D2VR_PublishOpenXrHapticRequest(uint32_t handIndex, float durationSeconds, float frequency, float amplitude)
{
    if (handIndex >= L4D2VR_OPENXR_HAND_COUNT)
        return;

    durationSeconds = std::clamp(durationSeconds, 0.0f, 0.5f);
    frequency = std::clamp(frequency, 0.0f, 320.0f);
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    if (durationSeconds <= 0.0f || amplitude <= 0.0f)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    L4D2VROpenXrHapticRequestDesc& request = state->hapticRequests[handIndex];
    uint32_t seq = request.sequence;
    if (seq & 1u)
        ++seq;

    request.sequence = seq + 1u;
    request.valid = 1;
    request.durationSeconds = durationSeconds;
    request.frequency = frequency;
    request.amplitude = amplitude;
    request.sequence = seq + 2u;
    state->heartbeatTickMs = GetTickCount64();
}

// Both eyes must land in the helper as one pair. They are published per frame
// now that the game rotates publish textures, and the helper reads them from
// shared memory without a lock, so two separate stores let it pick up left from
// one slot and right from another - each eye a different game frame, which is a
// double image as soon as the head moves. Seqlock the pair like the pose.
void L4D2VR_PublishOpenXrSharedTexturePair(
    const L4D2VROpenXrSharedTextureDesc& left,
    const L4D2VROpenXrSharedTextureDesc& right)
{
    if (!left.valid || !right.valid)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    ++state->sharedTextureGeneration;
    std::atomic_thread_fence(std::memory_order_release);
    state->eyeTextures[L4D2VR_OPENXR_EYE_LEFT] = left;
    state->eyeTextures[L4D2VR_OPENXR_EYE_RIGHT] = right;
    state->sharedTexturesReadyMask |=
        (1u << L4D2VR_OPENXR_EYE_LEFT) | (1u << L4D2VR_OPENXR_EYE_RIGHT);
    std::atomic_thread_fence(std::memory_order_release);
    ++state->sharedTextureGeneration;
    state->heartbeatTickMs = GetTickCount64();
}

void L4D2VR_PublishOpenXrSharedTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& texture)
{
    if (eyeIndex >= L4D2VR_OPENXR_EYE_COUNT || !texture.valid)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    const uint32_t readyBit = (1u << eyeIndex);
    if ((state->sharedTexturesReadyMask & readyBit) &&
        SameSharedTextureDesc(state->eyeTextures[eyeIndex], texture))
    {
        state->heartbeatTickMs = GetTickCount64();
        return;
    }

    state->eyeTextures[eyeIndex] = texture;
    state->sharedTexturesReadyMask |= readyBit;
    ++state->sharedTextureGeneration;
    state->heartbeatTickMs = GetTickCount64();

    Game::logMsg(
        "[VR][OpenXRHelper][BridgeTexture] eye=%s eyeIndex=%u gen=%u readyMask=0x%X handle=0x%llX image=0x%llX size=%ux%u format=%u samples=%u type=0x%X q=%u bounds=(%.4f %.4f %.4f %.4f) projection=(fovX=%.2f aspect=%.4f)",
        eyeIndex == L4D2VR_OPENXR_EYE_LEFT ? "left" : "right",
        eyeIndex,
        state->sharedTextureGeneration,
        state->sharedTexturesReadyMask,
        static_cast<unsigned long long>(texture.kmtHandle),
        static_cast<unsigned long long>(texture.image),
        texture.width,
        texture.height,
        texture.format,
        texture.sampleCount,
        texture.handleType,
        texture.queueFamilyIndex,
        texture.uMin,
        texture.vMin,
        texture.uMax,
        texture.vMax,
        texture.renderFovXDeg,
        texture.renderAspect);

    if ((state->sharedTexturesReadyMask & L4D2VR_OPENXR_EYES_READY_MASK) == L4D2VR_OPENXR_EYES_READY_MASK)
    {
        state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::SharedTexturesReady);
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "shared eye textures ready gen=%u L=%ux%u R=%ux%u",
            state->sharedTextureGeneration,
            state->eyeTextures[L4D2VR_OPENXR_EYE_LEFT].width,
            state->eyeTextures[L4D2VR_OPENXR_EYE_LEFT].height,
            state->eyeTextures[L4D2VR_OPENXR_EYE_RIGHT].width,
            state->eyeTextures[L4D2VR_OPENXR_EYE_RIGHT].height);
    }
    else
    {
        state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::WaitingForSharedTextures);
        std::snprintf(
            state->detail,
            sizeof(state->detail),
            "waiting for shared eye textures mask=0x%X",
            state->sharedTexturesReadyMask);
    }
}

void L4D2VR_PublishOpenXrSharedTextureFrame(uint32_t frameId)
{
    if (frameId == 0)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    if ((state->sharedTexturesReadyMask & L4D2VR_OPENXR_EYES_READY_MASK) != L4D2VR_OPENXR_EYES_READY_MASK)
        return;

    ++state->sharedTextureFrameGeneration;
    state->sharedTextureFrameId = frameId;
    ++state->sharedTextureFrameGeneration;
    state->heartbeatTickMs = GetTickCount64();

    {
        static std::atomic<int> s_openXrBridgeFrameLogBudget{ 24 };
        int remaining = s_openXrBridgeFrameLogBudget.load(std::memory_order_relaxed);
        if (remaining > 0 &&
            s_openXrBridgeFrameLogBudget.compare_exchange_strong(
                remaining,
                remaining - 1,
                std::memory_order_relaxed))
        {
            const L4D2VROpenXrSharedTextureDesc& left = state->eyeTextures[L4D2VR_OPENXR_EYE_LEFT];
            const L4D2VROpenXrSharedTextureDesc& right = state->eyeTextures[L4D2VR_OPENXR_EYE_RIGHT];
            Game::logMsg(
                "[VR][OpenXRHelper][BridgeFrame] frame=%u frameGen=%u textureGen=%u L(handle=0x%llX image=0x%llX) R(handle=0x%llX image=0x%llX)",
                frameId,
                state->sharedTextureFrameGeneration,
                state->sharedTextureGeneration,
                static_cast<unsigned long long>(left.kmtHandle),
                static_cast<unsigned long long>(left.image),
                static_cast<unsigned long long>(right.kmtHandle),
                static_cast<unsigned long long>(right.image));
        }
    }
}

void L4D2VR_PublishOpenXrOverlay(uint32_t overlayIndex, const L4D2VROpenXrOverlayDesc& overlay)
{
    if (overlayIndex >= L4D2VR_OPENXR_OVERLAY_COUNT)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    const uint32_t readyBit = 1u << overlayIndex;
    if ((state->overlayReadyMask & readyBit) &&
        SameOpenXrOverlayDesc(state->overlays[overlayIndex], overlay))
    {
        state->heartbeatTickMs = GetTickCount64();
        return;
    }

    state->overlays[overlayIndex] = overlay;
    if (overlay.valid && overlay.visible && overlay.texture.valid)
        state->overlayReadyMask |= readyBit;
    else
        state->overlayReadyMask &= ~readyBit;

    ++state->overlayGeneration;
    state->heartbeatTickMs = GetTickCount64();
}

void L4D2VR_PublishOpenXrOverlayFrame(uint32_t frameId)
{
    if (frameId == 0)
        return;

    std::lock_guard<std::mutex> lock(g_OpenXrBridgeStateMutex);
    L4D2VROpenXrBridgeState* state = g_OpenXrBridgeState;
    if (!state)
        return;

    ++state->overlayFrameGeneration;
    state->overlayFrameId = frameId;
    ++state->overlayFrameGeneration;
    state->heartbeatTickMs = GetTickCount64();
}
