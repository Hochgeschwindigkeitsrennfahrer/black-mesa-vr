// Fake OpenXR helper for headless tests (VRRuntimeBackend=fake in VR\config.txt).
//
// Plays the 64-bit OpenXRHelper64.exe's side of the shared bridge inside the game
// process, so the whole OpenXR helper path of this DLL runs without SteamVR or a
// headset: runtime view config (PS VR2 per-eye tangents), HMD pose with IPD,
// controller grip/aim poses, actions, and frame consumption. Nothing is shown in a
// headset; MirrorBothEyes=true puts both eye images on the desktop window, and the
// projection the real helper would submit for them is logged ("fakehelper eye N raw
// ... uv ..."), so a screenshot can be checked against it (tools/bm-projcheck.py).
//
// <VR dir>\fake.txt (re-read when it changes; Source axes x forward, y left, z up;
// metres; angles pitch yaw roll in degrees):
//   size <w> <h>                 runtime recommended per-eye size (default 2884 2940, PS VR2 at 100 %)
//   fov <outer> <inner> <up> <down>  per-eye tangents (default PS VR2: 1.826 0.947 1.330 1.330)
//   ipd <metres>                 default 0.063
//   hmd <x> <y> <z> <pitch> <yaw> <roll>
//   left <x> <y> <z> <pitch> <yaw> <roll>     controller (grip = aim)
//   right <x> <y> <z> <pitch> <yaw> <roll>
//   buttons <name> ...           attack attack2 reload jump duck use recenter pause flashlight
//                                select back next prev weaponmenu
//   move <x> <y> / turn <x> <y>  sticks
// <VR dir>\cmd.txt: console commands, one per line, appended by the test; each new
// line is run once (ClientCmd_Unrestricted on the engine thread).

#include "openxr_helper_bridge.h"
#include "bmvr_flags.h"
#include "game.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    constexpr float kPi = 3.14159265358979f;

    struct FakePose
    {
        float x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0;
    };

    struct FakeScript
    {
        uint32_t width = 2884, height = 2940;
        float tanOut = 1.826f, tanIn = 0.947f, tanUp = 1.330f, tanDown = 1.330f;
        float ipd = 0.063f;
        FakePose hmd{ 0, 0, 1.70f, 0, 0, 0 };
        FakePose left{ 0.30f, 0.20f, 1.25f, 20, 0, 0 };
        FakePose right{ 0.35f, -0.18f, 1.30f, 10, 0, 0 };
        bool buttons[L4D2VR_OPENXR_ACTION_COUNT] = {};
        float moveX = 0, moveY = 0, turnX = 0, turnY = 0;
    };

    std::mutex g_CmdMutex;
    std::deque<std::string> g_Cmds;

    // OpenXR pose (x right, y up, -z forward) from Source-axis position/angles,
    // the same convention as css-vr's FakeBackend::MakePose.
    void ToOpenXr(const FakePose& p, float pos[3], float quat[4])
    {
        const float sp = sinf(p.pitch * kPi / 180), cp = cosf(p.pitch * kPi / 180);
        const float sy = sinf(p.yaw * kPi / 180), cy = cosf(p.yaw * kPi / 180);
        const float sr = sinf(p.roll * kPi / 180), cr = cosf(p.roll * kPi / 180);
        // Source AngleVectors
        const float fwd[3] = { cp * cy, cp * sy, -sp };
        const float rgt[3] = { -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };
        const float up[3] = { cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
        auto ovr = [](const float v[3], float o[3]) { o[0] = -v[1]; o[1] = v[2]; o[2] = -v[0]; };
        float c0[3], c1[3], c2[3], back[3] = { -fwd[0], -fwd[1], -fwd[2] };
        ovr(rgt, c0);
        ovr(up, c1);
        ovr(back, c2);
        const float src[3] = { p.x, p.y, p.z };
        ovr(src, pos);
        // rotation matrix columns c0 c1 c2 -> quaternion (x y z w)
        const float m00 = c0[0], m10 = c0[1], m20 = c0[2];
        const float m01 = c1[0], m11 = c1[1], m21 = c1[2];
        const float m02 = c2[0], m12 = c2[1], m22 = c2[2];
        const float tr = m00 + m11 + m22;
        float x, y, z, w;
        if (tr > 0)
        {
            const float s = sqrtf(tr + 1.0f) * 2;
            w = 0.25f * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s;
        }
        else if (m00 > m11 && m00 > m22)
        {
            const float s = sqrtf(1.0f + m00 - m11 - m22) * 2;
            w = (m21 - m12) / s; x = 0.25f * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
        }
        else if (m11 > m22)
        {
            const float s = sqrtf(1.0f + m11 - m00 - m22) * 2;
            w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25f * s; z = (m12 + m21) / s;
        }
        else
        {
            const float s = sqrtf(1.0f + m22 - m00 - m11) * 2;
            w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25f * s;
        }
        quat[0] = x; quat[1] = y; quat[2] = z; quat[3] = w;
    }

    int ButtonAction(const std::string& b)
    {
        using A = L4D2VROpenXrActionId;
        if (b == "attack") return (int)A::PrimaryAttack;
        if (b == "attack2") return (int)A::SecondaryAttack;
        if (b == "reload") return (int)A::Reload;
        if (b == "jump") return (int)A::Jump;
        if (b == "duck") return (int)A::Crouch;
        if (b == "use") return (int)A::Use;
        if (b == "recenter") return (int)A::ResetPosition;
        if (b == "pause") return (int)A::Pause;
        if (b == "flashlight") return (int)A::Flashlight;
        if (b == "select") return (int)A::MenuSelect;
        if (b == "back") return (int)A::MenuBack;
        if (b == "next") return (int)A::NextItem;
        if (b == "prev") return (int)A::PrevItem;
        if (b == "weaponmenu") return (int)A::InventoryQuickSwitch;
        if (b == "sprint") return (int)A::CustomAction1;
        return 0;
    }

    uint64_t FileStamp(const std::string& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a))
            return 0;
        return (((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime)
            ^ ((uint64_t)a.nFileSizeLow << 1);
    }

    bool ReadScript(FakeScript& s)
    {
        std::ifstream in(bmvr::VrFilePath("fake.txt"));
        if (!in)
            return false;
        FakeScript fresh = s;
        std::fill(std::begin(fresh.buttons), std::end(fresh.buttons), false);
        fresh.moveX = fresh.moveY = fresh.turnX = fresh.turnY = 0;
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream ss(line);
            std::string key;
            ss >> key;
            auto pose = [&](FakePose& p) { ss >> p.x >> p.y >> p.z >> p.pitch >> p.yaw >> p.roll; };
            if (key == "hmd") pose(fresh.hmd);
            else if (key == "left") pose(fresh.left);
            else if (key == "right") pose(fresh.right);
            else if (key == "size") ss >> fresh.width >> fresh.height;
            else if (key == "fov") ss >> fresh.tanOut >> fresh.tanIn >> fresh.tanUp >> fresh.tanDown;
            else if (key == "ipd") ss >> fresh.ipd;
            else if (key == "move") ss >> fresh.moveX >> fresh.moveY;
            else if (key == "turn") ss >> fresh.turnX >> fresh.turnY;
            else if (key == "buttons")
            {
                std::string b;
                while (ss >> b)
                    if (const int a = ButtonAction(b))
                        fresh.buttons[a] = true;
            }
        }
        s = fresh;
        return true;
    }

    // What OpenXRHelper64 submits for a published eye texture (BuildGameProjectionFov,
    // ChooseOpenXrSwapchainExtent, ComputeContainBlitRect; SteamVR: no runtime-FOV crop),
    // expressed as the tangents the whole source texture covers in the headset.
    void LogDeclaredProjection(uint32_t eye, const L4D2VROpenXrSharedTextureDesc& d,
        const FakeScript& s, bool gameRendersRuntimeFov)
    {
        const float fovX = (std::isfinite(d.renderFovXDeg) && d.renderFovXDeg > 1.0f && d.renderFovXDeg < 179.0f)
            ? d.renderFovXDeg : 90.0f;
        const float aspect = (std::isfinite(d.renderAspect) && d.renderAspect > 0.1f && d.renderAspect < 10.0f)
            ? d.renderAspect : (d.height ? (float)d.width / (float)d.height : 1.0f);
        const bool full = fabsf(d.uMin) <= 0.0005f && fabsf(d.vMin) <= 0.0005f
            && fabsf(d.uMax - 1) <= 0.0005f && fabsf(d.vMax - 1) <= 0.0005f;
        // declared tangents over the swapchain image: left/right (x, right +), up/down (up +)
        float L, R, U, D;
        const char* mode;
        if (!gameRendersRuntimeFov && full)
        {
            const float tx = tanf(0.5f * fovX * kPi / 180.0f);
            L = -tx; R = tx; U = tx / aspect; D = -tx / aspect;
            mode = "game-symmetric";
        }
        else
        {
            L = eye == 0 ? -s.tanOut : -s.tanIn;
            R = eye == 0 ? s.tanIn : s.tanOut;
            U = s.tanUp; D = -s.tanDown;
            mode = "runtime";
        }
        // swapchain = runtime recommended size (capped 4096); source sub-rect contain-blit into it
        const uint32_t W = std::min<uint32_t>(s.width, 4096), H = std::min<uint32_t>(s.height, 4096);
        const double srcW = d.width * std::clamp(fabsf(d.uMax - d.uMin), 0.0f, 1.0f) + 0.5;
        const double srcH = d.height * std::clamp(fabsf(d.vMax - d.vMin), 0.0f, 1.0f) + 0.5;
        const uint32_t sw = (uint32_t)srcW ? (uint32_t)srcW : d.width, sh = (uint32_t)srcH ? (uint32_t)srcH : d.height;
        const double sc = std::min(1.0, std::min((double)W / sw, (double)H / sh));
        int cw = std::max(1, (int)(sw * sc + 0.5)), ch = std::max(1, (int)(sh * sc + 0.5));
        cw = std::min(cw, (int)W); ch = std::min(ch, (int)H);
        const int cx = std::max(0, ((int)W - cw) / 2), cy = std::max(0, ((int)H - ch) / 2);
        // tangents at the source rect's edges (the image shows its uv sub-rect there)
        const float l = L + (R - L) * (float)cx / W;
        const float r = L + (R - L) * (float)(cx + cw) / W;
        const float up = U - (U - D) * (float)cy / H;
        const float dn = U - (U - D) * (float)(cy + ch) / H;
        // OpenVR-style raw line (t < 0 = up edge) for the projection check
        Game::logMsg("fakehelper eye %u raw l %.4f r %.4f t %.4f b %.4f mode %s submitFov(L %.4f R %.4f U %.4f D %.4f) "
            "src %ux%u fovX %.3f aspect %.4f swapchain %ux%u contain %d,%d %dx%d uv %.4f-%.4f %.4f-%.4f",
            eye, l, r, -up, -dn, mode, L, R, U, D, d.width, d.height, fovX, aspect, W, H, cx, cy, cw, ch,
            d.uMin, d.uMax, d.vMin, d.vMax);
    }

    void FakeHelperThread(L4D2VROpenXrBridgeState* state, bool gameRendersRuntimeFov)
    {
        FakeScript script;
        ReadScript(script);
        const std::string fakePath = bmvr::VrFilePath("fake.txt");
        const std::string cmdPath = bmvr::VrFilePath("cmd.txt");
        uint64_t fakeStamp = FileStamp(fakePath);
        size_t cmdLinesDone = 0;

        auto writeViewConfig = [&]() {
            L4D2VROpenXrRuntimeViewConfigDesc c{};
            c.valid = 1;
            c.viewCount = 2;
            c.reserved0 = L4D2VR_OPENXR_RUNTIME_VIEW_FOV_LOCATED;
            for (uint32_t e = 0; e < 2; ++e)
            {
                L4D2VROpenXrRuntimeViewDesc& v = c.views[e];
                v.valid = 1;
                v.width = std::min<uint32_t>(script.width, 4096);
                v.height = std::min<uint32_t>(script.height, 4096);
                v.recommendedSampleCount = 1;
                v.angleLeft = -atanf(e == 0 ? script.tanOut : script.tanIn);
                v.angleRight = atanf(e == 0 ? script.tanIn : script.tanOut);
                v.angleUp = atanf(script.tanUp);
                v.angleDown = -atanf(script.tanDown);
            }
            ++state->runtimeViewConfigGeneration;
            std::atomic_thread_fence(std::memory_order_release);
            state->runtimeViewConfig = c;
            std::atomic_thread_fence(std::memory_order_release);
            ++state->runtimeViewConfigGeneration;
        };
        writeViewConfig();
        state->helperPid = GetCurrentProcessId();
        state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::SessionRunning);
        Game::logMsg("fakehelper: runtime views %ux%u tangents out %.3f in %.3f up %.3f down %.3f ipd %.4f",
            script.width, script.height, script.tanOut, script.tanIn, script.tanUp, script.tanDown, script.ipd);

        bool prevButtons[L4D2VR_OPENXR_ACTION_COUNT] = {};
        uint32_t lastTexGen = 0, lastFrameId = 0, consumedCount = 0;
        uint32_t loggedDecl = 0;
        L4D2VROpenXrSharedTextureDesc lastDecl[2] = {};
        int64_t t = 0;
        for (;;)
        {
            Sleep(8); // ~120 Hz like the PS VR2
            ++t;
            const uint64_t st = FileStamp(fakePath);
            if (st != fakeStamp)
            {
                fakeStamp = st;
                const uint32_t w = script.width, h = script.height;
                if (ReadScript(script))
                {
                    Game::logMsg("fakehelper: fake.txt reloaded hmd (%.3f %.3f %.3f / %.2f %.2f %.2f) right (%.3f %.3f %.3f / %.2f %.2f %.2f)",
                        script.hmd.x, script.hmd.y, script.hmd.z, script.hmd.pitch, script.hmd.yaw, script.hmd.roll,
                        script.right.x, script.right.y, script.right.z, script.right.pitch, script.right.yaw, script.right.roll);
                    if (w != script.width || h != script.height)
                        writeViewConfig();
                }
            }
            // console commands
            {
                std::ifstream in(cmdPath);
                if (in)
                {
                    std::string line;
                    size_t n = 0;
                    while (std::getline(in, line))
                    {
                        if (++n <= cmdLinesDone)
                            continue;
                        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                            line.pop_back();
                        if (!line.empty())
                        {
                            std::lock_guard<std::mutex> lock(g_CmdMutex);
                            g_Cmds.push_back(line);
                        }
                        cmdLinesDone = n;
                    }
                }
            }

            // HMD pose (centre eye) + IPD
            L4D2VROpenXrPoseDesc hmd{};
            hmd.valid = 1;
            hmd.viewStateFlags = 0xF;
            hmd.displayTime = t;
            ToOpenXr(script.hmd, hmd.position, hmd.orientation);
            std::memcpy(&hmd.reserved0, &script.ipd, sizeof(float));
            ++state->trackingPoseGeneration;
            std::atomic_thread_fence(std::memory_order_release);
            state->hmdPose = hmd;
            std::atomic_thread_fence(std::memory_order_release);
            ++state->trackingPoseGeneration;

            // controllers + actions
            L4D2VROpenXrInputStateDesc in{};
            in.valid = 1;
            in.featureFlags = L4D2VR_OPENXR_INPUT_FEATURE_CONTROLLERS;
            in.actionCount = L4D2VR_OPENXR_ACTION_COUNT;
            in.reserved0 = L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH; // PS VR2 Sense on SteamVR = Touch emulation
            const FakePose* hands[2] = { &script.left, &script.right };
            for (int h = 0; h < 2; ++h)
            {
                L4D2VROpenXrControllerPoseDesc p{};
                p.valid = 1;
                p.active = 1;
                p.locationFlags = 0xF;
                p.displayTime = t;
                ToOpenXr(*hands[h], p.position, p.orientation);
                in.controllerPoses[h] = p;
                in.controllerAimPoses[h] = p;
            }
            for (uint32_t a = 1; a < L4D2VR_OPENXR_ACTION_COUNT; ++a)
            {
                L4D2VROpenXrDigitalActionDesc& d = in.digitalActions[a];
                d.active = 1;
                d.state = script.buttons[a] ? 1 : 0;
                d.changed = script.buttons[a] != prevButtons[a] ? 1 : 0;
                d.lastChangeTime = t;
                prevButtons[a] = script.buttons[a];
                L4D2VROpenXrAnalogActionDesc& an = in.analogActions[a];
                an.active = 1;
            }
            in.analogActions[(int)L4D2VROpenXrActionId::Walk].x = script.moveX;
            in.analogActions[(int)L4D2VROpenXrActionId::Walk].y = script.moveY;
            in.analogActions[(int)L4D2VROpenXrActionId::Turn].x = script.turnX;
            in.analogActions[(int)L4D2VROpenXrActionId::Turn].y = script.turnY;
            ++state->inputStateGeneration;
            std::atomic_thread_fence(std::memory_order_release);
            state->inputState = in;
            std::atomic_thread_fence(std::memory_order_release);
            ++state->inputStateGeneration;

            // consume published frames (the game recycles publish slots on this)
            const uint32_t texGen = state->sharedTextureGeneration;
            if (!(texGen & 1u) && texGen != lastTexGen
                && (state->sharedTexturesReadyMask & L4D2VR_OPENXR_EYES_READY_MASK) == L4D2VR_OPENXR_EYES_READY_MASK)
            {
                L4D2VROpenXrSharedTextureDesc eyes[2] = { state->eyeTextures[0], state->eyeTextures[1] };
                std::atomic_thread_fence(std::memory_order_acquire);
                if (state->sharedTextureGeneration == texGen)
                {
                    lastTexGen = texGen;
                    for (uint32_t e = 0; e < 2; ++e)
                    {
                        const L4D2VROpenXrSharedTextureDesc& d = eyes[e];
                        const L4D2VROpenXrSharedTextureDesc& o = lastDecl[e];
                        const bool differs = d.width != o.width || d.height != o.height
                            || d.renderFovXDeg != o.renderFovXDeg || d.renderAspect != o.renderAspect
                            || d.uMin != o.uMin || d.uMax != o.uMax || d.vMin != o.vMin || d.vMax != o.vMax;
                        if (differs && loggedDecl < 64)
                        {
                            ++loggedDecl;
                            LogDeclaredProjection(e, d, script, gameRendersRuntimeFov);
                        }
                        lastDecl[e] = d;
                    }
                }
            }
            const uint32_t frameId = state->sharedTextureFrameId;
            if (frameId != 0 && frameId != lastFrameId)
            {
                lastFrameId = frameId;
                state->helperConsumingFrameId = frameId;
                std::atomic_thread_fence(std::memory_order_release);
                state->helperConsumedFrameId = frameId;
                state->helperConsumedCount = ++consumedCount;
                ++state->submittedFrames;
                state->status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::SubmittedFrame);
                if (consumedCount == 1 || consumedCount % 600 == 0)
                    Game::logMsg("fakehelper: consumed frame %u (count %u)", frameId, consumedCount);
            }
            state->heartbeatTickMs = GetTickCount64();
        }
    }
}

void L4D2VR_StartFakeOpenXrHelper(L4D2VROpenXrBridgeState* state, bool gameRendersRuntimeFov)
{
    std::thread(FakeHelperThread, state, gameRendersRuntimeFov).detach();
}

bool L4D2VR_FakeHelperPopCommand(char* out, size_t outSize)
{
    std::lock_guard<std::mutex> lock(g_CmdMutex);
    if (g_Cmds.empty() || !out || outSize == 0)
        return false;
    std::snprintf(out, outSize, "%s", g_Cmds.front().c_str());
    g_Cmds.pop_front();
    return true;
}
