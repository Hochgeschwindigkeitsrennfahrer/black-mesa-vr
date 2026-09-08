#include "vr.h"
#include "game.h"
#include "sdk.h"
#include "offsets.h"
#include "MinHook.h"
#include "d3d9_vr.h"
#include "bmvr_flags.h"
#include "hooks.h"
#include "in_buttons.h"
#include "trace.h"
#include "texture.h"
#include "vr_hands.h"
#include "vr_hud_icons.h"
#include "openxr_helper_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace
{
    using tPresent = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using tSetRenderTarget = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    using tSetDepthStencil = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*);
    using tSetViewport = HRESULT(__stdcall*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
    using tSetScissorRect = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*);
    using tSetShaderConstantF = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, const float*, UINT);
    using tStretchRect = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
        IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
    using tClear = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
    tPresent g_OrigPresent = nullptr;
    tSetRenderTarget g_OrigSetRenderTarget = nullptr;
    tSetDepthStencil g_OrigSetDepthStencil = nullptr;
    tSetViewport g_OrigSetViewport = nullptr;
    tSetScissorRect g_OrigSetScissorRect = nullptr;
    tSetShaderConstantF g_OrigSetVertexShaderConstantF = nullptr;
    tSetShaderConstantF g_OrigSetPixelShaderConstantF = nullptr;
    tStretchRect g_OrigStretchRect = nullptr;
    tClear g_OrigClear = nullptr;
    UINT g_Rt0W = 0;
    UINT g_Rt0H = 0;
    // Actual D3D RT0 as last set through the hooked SetRenderTarget (g_Rt0W/H
    // above is the material-system view, which NoteCachedRt0Size may override
    // with the eye size). D3dRt0IsEyeSized used to GetRenderTarget+GetDesc
    // per call — three DXVK-locked COM calls from every GetScreenSize /
    // SetViewport / Clear hook. RT0 only changes via the hooked entry point
    // (DXVK's own ResetSwapChain rebind goes through it too) or Reset, which
    // invalidates this cache.
    thread_local IDirect3DSurface9* g_Rt0ActualPtr = nullptr;
    thread_local UINT g_Rt0ActualW = 0;
    thread_local UINT g_Rt0ActualH = 0;
    thread_local bool g_Rt0ActualKnown = false;
    // Material-thread last full-eye viewport on the MC ring. Playback has
    // m_StereoEye == 0, so 16:9 SetViewport/Clear/Copy must not assume 0,0.
    thread_local int g_McEyeX = 0;
    thread_local int g_McEyeY = 0;
    thread_local bool g_McEyeKnown = false;
    thread_local int g_McLastRingVpX = -1;
    // Material-thread 1x eye while a queued stereo pair is playing back.
    // HWND must not be the 2x3 ring: BM deferred lighting/flashlight/fog
    // assume a 1x HMD-sized dest at 0,0 (WorldRenderAtEyeSize).
    thread_local int g_McPlayEye = 0;
    thread_local bool g_HudPaintActive = false;
    std::atomic<bool> g_McPauseHudBound{ false };
    // Swapchain backbuffer surface (pointer identity only, no reference held).
    // Stable between Resets; GetBackBuffer per SetRenderTarget/SetDepthStencil
    // during the eye blit was an extra locked COM round trip each.
    IDirect3DSurface9* g_CachedBackBuffer = nullptr;
    bool g_CachedBackBufferValid = false;
    bool g_DeviceHooksEnabled = false;
    BmVrGloves g_VrGloves;

    constexpr UINT kIDirect3DDevice9_Present = 17;
    constexpr UINT kIDirect3DDevice9_StretchRect = 34;
    constexpr UINT kIDirect3DDevice9_SetRenderTarget = 37;
    constexpr UINT kIDirect3DDevice9_SetDepthStencilSurface = 39;
    constexpr UINT kIDirect3DDevice9_Clear = 43;
    constexpr UINT kIDirect3DDevice9_SetViewport = 47;
    constexpr UINT kIDirect3DDevice9_SetScissorRect = 75;
    constexpr UINT kIDirect3DDevice9_SetVertexShaderConstantF = 94;
    constexpr UINT kIDirect3DDevice9_SetPixelShaderConstantF = 109;

    template <typename T>
    void ReleaseT(T*& p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    static bool FileExistsA(const char* path)
    {
        const DWORD a = GetFileAttributesA(path);
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    // High-resolution wall clock in ms. GetTickCount only advances every
    // ~15.6 ms, which is useless for per-Present gating.
    static double QpcNowMs()
    {
        static double s_toMs = 0.0;
        if (s_toMs == 0.0)
        {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            s_toMs = f.QuadPart ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
        }
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) * s_toMs;
    }

    // Rank scene-color RTs for stereo unbind blit. Copy LDR only.
    // A2R10/R16F FullFrame (fmt 35/111) is pre-tonemap HDR. StretchRect into
    // A8R8G8B8 eyes looks untextured-white in the HMD while the desktop
    // swapchain (post-tonemap) stays textured. 2026-08-18 also blacked the
    // tram when this copy was stretched onto the backbuffer.
    static int SceneColorRank(D3DFORMAT format)
    {
        switch (format)
        {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
            return 3;
        default:
            return 0;
        }
    }

    static std::string DirFromModulePath(const wchar_t* full)
    {
        char out[MAX_PATH]{};
        WideCharToMultiByte(CP_ACP, 0, full, -1, out, MAX_PATH, nullptr, nullptr);
        std::string s(out);
        const size_t slash = s.find_last_of("\\/");
        if (slash == std::string::npos)
            return ".";
        return s.substr(0, slash);
    }

    QAngle HmdMatrixToSourceAngles(const vr::HmdMatrix34_t& mat)
    {
        QAngle ang;
        ang.x = asinf(mat.m[1][2]) * (180.0f / 3.141592654f);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0f / 3.141592654f);
        ang.z = 0.f;
        return ang;
    }

    QAngle HmdMatrixToSourceAnglesWithRoll(const vr::HmdMatrix34_t& mat)
    {
        QAngle ang;
        ang.x = asinf(mat.m[1][2]) * (180.0f / 3.141592654f);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0f / 3.141592654f);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0f / 3.141592654f);
        return ang;
    }

    float WrapYaw(float yaw)
    {
        return yaw - 360.f * floorf((yaw + 180.f) / 360.f);
    }

    void PivotYaw(Vector& delta, float yawDeg)
    {
        const float rad = yawDeg * (3.14159265f / 180.f);
        const float s = sinf(rad);
        const float c = cosf(rad);
        const float nx = delta.x * c - delta.y * s;
        const float ny = delta.x * s + delta.y * c;
        delta.x = nx;
        delta.y = ny;
    }

    // OpenXR Y-up yaw only. Drops headset pitch/roll so a tilted head cannot
    // latch a crooked 2D GameUI panel.
    void LevelOpenXrPoseToYaw(L4D2VROpenXrPoseDesc& pose)
    {
        float x = pose.orientation[0];
        float y = pose.orientation[1];
        float z = pose.orientation[2];
        float w = pose.orientation[3];
        const float lenSq = x * x + y * y + z * z + w * w;
        if (lenSq > 0.000001f)
        {
            const float invLen = 1.f / sqrtf(lenSq);
            x *= invLen;
            y *= invLen;
            z *= invLen;
            w *= invLen;
        }
        else
        {
            x = 0.f;
            y = 0.f;
            z = 0.f;
            w = 1.f;
        }
        const float yaw = atan2f(2.f * (w * y + x * z), 1.f - 2.f * (y * y + z * z));
        const float half = yaw * 0.5f;
        pose.orientation[0] = 0.f;
        pose.orientation[1] = sinf(half);
        pose.orientation[2] = 0.f;
        pose.orientation[3] = cosf(half);
    }

    void RotateOpenXrVec(const float q[4], float vx, float vy, float vz, float out[3])
    {
        const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
        const float tx = 2.f * (qy * vz - qz * vy);
        const float ty = 2.f * (qz * vx - qx * vz);
        const float tz = 2.f * (qx * vy - qy * vx);
        out[0] = vx + qw * tx + (qy * tz - qz * ty);
        out[1] = vy + qw * ty + (qz * tx - qx * tz);
        out[2] = vz + qw * tz + (qx * ty - qy * tx);
    }

    // HL2VR collisionutils ComputeIntersectionBarycentricCoordinates, quad
    // test (u,v in 0..1) instead of triangle u+v<=1.
    bool RayHitQuadUv(const float orig[3], const float dir[3],
        const float v1[3], const float v2[3], const float v3[3],
        float& u, float& v)
    {
        const float edge1[3] = { v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2] };
        const float edge2[3] = { v3[0] - v1[0], v3[1] - v1[1], v3[2] - v1[2] };
        const float delta[3] = { dir[0] * 10.f, dir[1] * 10.f, dir[2] * 10.f };
        const float dce2[3] = {
            delta[1] * edge2[2] - delta[2] * edge2[1],
            delta[2] * edge2[0] - delta[0] * edge2[2],
            delta[0] * edge2[1] - delta[1] * edge2[0]
        };
        float denom = dce2[0] * edge1[0] + dce2[1] * edge1[1] + dce2[2] * edge1[2];
        if (fabsf(denom) < 1e-6f)
            return false;
        denom = 1.f / denom;
        const float org[3] = { orig[0] - v1[0], orig[1] - v1[1], orig[2] - v1[2] };
        u = (dce2[0] * org[0] + dce2[1] * org[1] + dce2[2] * org[2]) * denom;
        const float oce1[3] = {
            org[1] * edge1[2] - org[2] * edge1[1],
            org[2] * edge1[0] - org[0] * edge1[2],
            org[0] * edge1[1] - org[1] * edge1[0]
        };
        v = (oce1[0] * delta[0] + oce1[1] * delta[1] + oce1[2] * delta[2]) * denom;
        return u >= 0.f && v >= 0.f && u <= 1.f && v <= 1.f;
    }

    Vector HmdMatrixToSourcePos(const vr::HmdMatrix34_t& mat, float scale)
    {
        Vector pos;
        pos.x = -mat.m[2][3] * scale;
        pos.y = -mat.m[0][3] * scale;
        pos.z = mat.m[1][3] * scale;
        return pos;
    }

    vr::HmdMatrix34_t MulHmd34(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b)
    {
        vr::HmdMatrix34_t o{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
            o.m[r][3] = a.m[r][0] * b.m[0][3] + a.m[r][1] * b.m[1][3] + a.m[r][2] * b.m[2][3] + a.m[r][3];
        }
        return o;
    }

    // G2 / WMR device pose is the tracking-ring origin. OpenVR grip/tip is the
    // handle. Do not apply this to the gun (offsets were tuned on raw pose).
    vr::HmdMatrix34_t DevicePoseToGrip(vr::IVRSystem* system, vr::TrackedDeviceIndex_t idx,
        const vr::HmdMatrix34_t& devicePose)
    {
        if (!system || idx >= vr::k_unMaxTrackedDeviceCount)
            return devicePose;
        vr::IVRRenderModels* rm = vr::VRRenderModels();
        if (!rm)
            return devicePose;
        char model[vr::k_unMaxPropertyStringSize]{};
        if (system->GetStringTrackedDeviceProperty(idx, vr::Prop_RenderModelName_String,
                model, sizeof(model)) == 0 || !model[0])
            return devicePose;
        vr::VRControllerState_t cs{};
        if (!system->GetControllerState(idx, &cs, sizeof(cs)))
            memset(&cs, 0, sizeof(cs));
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t comp{};
        const char* names[] = {
            vr::k_pch_Controller_Component_OpenXR_Grip,
            vr::k_pch_Controller_Component_HandGrip,
            vr::k_pch_Controller_Component_Tip,
        };
        static int s_gripLog;
        for (const char* name : names)
        {
            if (!rm->GetComponentState(model, name, &cs, &mode, &comp))
                continue;
            if (s_gripLog < 4)
            {
                Game::logMsg("Hand grip component=%s model=%s t=(%.3f,%.3f,%.3f)",
                    name, model, comp.mTrackingToComponentRenderModel.m[0][3],
                    comp.mTrackingToComponentRenderModel.m[1][3],
                    comp.mTrackingToComponentRenderModel.m[2][3]);
                ++s_gripLog;
            }
            return MulHmd34(devicePose, comp.mTrackingToComponentRenderModel);
        }
        if (s_gripLog < 4)
        {
            Game::logMsg("Hand grip component missing model=%s — raw device pose", model);
            ++s_gripLog;
        }
        return devicePose;
    }

    bool QueryGameClientSize(UINT& w, UINT& h);
    bool CachedClientSize(UINT& w, UINT& h);

    IDirect3DSurface9* CachedBackBufferPtr(IDirect3DDevice9* device)
    {
        if (!g_CachedBackBufferValid && device)
        {
            IDirect3DSurface9* bb = nullptr;
            if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
            {
                g_CachedBackBuffer = bb;
                g_CachedBackBufferValid = true;
                bb->Release();
            }
        }
        return g_CachedBackBufferValid ? g_CachedBackBuffer : nullptr;
    }

    bool SurfaceMatchesWindowOrBackbuffer(IDirect3DDevice9* device, IDirect3DSurface9* surf)
    {
        if (!device || !surf)
            return false;
        IDirect3DSurface9* bb = CachedBackBufferPtr(device);
        if (bb && surf == bb)
            return true;
        D3DSURFACE_DESC desc{};
        if (FAILED(surf->GetDesc(&desc)) || desc.Width < 640 || desc.Height < 360)
            return false;
        UINT winW = 0, winH = 0;
        if (QueryGameClientSize(winW, winH) && desc.Width == winW && desc.Height == winH)
            return true;
        uint32_t fbW = 0, fbH = 0;
        if (bmvr::HaveHmdFramebufferSize(fbW, fbH) && desc.Width == fbW && desc.Height == fbH)
            return true;
        return false;
    }

    HRESULT __stdcall HookedPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* dirty)
    {
        if (g_Game && g_Game->m_VR)
            g_Game->m_VR->CaptureFrameBeforePresent();
        if (!g_OrigPresent)
            return D3DERR_INVALIDCALL;
        return g_OrigPresent(device, src, dst, hwnd, dirty);
    }

    HRESULT __stdcall HookedSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* rt)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        // Extra VGui_Paint of pause GameUI must land on the D3D HUD, not the
        // HWND backbuffer (that was the floating copy of the whole desktop).
        if (index == 0 && vr && device && vr->HudPaintActive() && vr->m_D9HUDSurface
            && rt != vr->m_D9HUDSurface)
        {
            bool toHud = (rt == nullptr);
            if (!toHud && rt)
            {
                IDirect3DSurface9* bb = CachedBackBufferPtr(device);
                toHud = (bb && rt == bb);
                if (!toHud)
                {
                    D3DSURFACE_DESC desc{};
                    UINT winW = 0, winH = 0;
                    if (SUCCEEDED(rt->GetDesc(&desc)) && QueryGameClientSize(winW, winH)
                        && desc.Width == winW && desc.Height == winH)
                        toHud = true;
                }
            }
            if (toHud)
                rt = vr->m_D9HUDSurface;
        }
        // Map-load stdshader hammers SetRT. GetRenderTarget/GetViewport on
        // every call was extra DXVK lock + COM during that nest. Capture
        // only matters while a stereo eye blit is in progress.
        bool redirectedToEye = false;
        // Multicore: HWND is the current 1x eye (same WorldRenderAtEyeSize
        // path as queue 0). Binding the 2x3 ring made lighting/flashlight/fog
        // run at ring UVs (log: 3168x3104 @0,0 remapped onto last-known eye,
        // CopyRTEx srcRect=16,0 / 3184,0, fog overlay on the right).
        IDirect3DSurface9* dest = nullptr;
        if (vr && !vr->m_CaptureReentry && !vr->HudPaintActive())
        {
            dest = vr->McPlaybackEyeSurface();
            // Queue 2: StereoEyeBlitActive is shared. The material thread sets
            // it while drawing eyes; GameUI on the game thread then dumped the
            // pause cursor onto the 1x world RT (stuck on the 3D view, overlay
            // flickered). Only this thread's TLS dest may steal HWND.
            if (!dest && !vr->McPipelineEnabled() && vr->StereoEyeBlitActive())
                dest = vr->StereoEyeBlitDest();
        }
        if (index == 0 && vr && device && dest)
        {
            if ((bmvr::TrySteamVrEyeRt() || bmvr::OffscreenWorldMatchesEyes()
                    || vr->McPipelineEnabled())
                && rt
                && rt != dest
                && SurfaceMatchesWindowOrBackbuffer(device, rt))
            {
                rt = dest;
                vr->NoteStereoRedirectedToEye();
                redirectedToEye = true;
                static int s_mcEyeRt;
                if (s_mcEyeRt < 8 && vr->McPipelineEnabled())
                {
                    D3DSURFACE_DESC d{};
                    UINT dw = 0, dh = 0;
                    if (SUCCEEDED(dest->GetDesc(&d)))
                    {
                        dw = d.Width;
                        dh = d.Height;
                    }
                    Game::logMsg("MC HWND SetRT -> 1x eye %ux%u (not ring)", dw, dh);
                    ++s_mcEyeRt;
                }
            }
            // CaptureGameColorOnUnbind is a no-op on the native offscreen
            // path (eyes are painted via the redirect above); do not pay
            // GetRenderTarget + GetViewport (two DXVK device locks + COM) on
            // every SetRT of every eye for it.
            if (!bmvr::OffscreenWorldMatchesEyes())
            {
                IDirect3DSurface9* oldRt = nullptr;
                D3DVIEWPORT9 vp{};
                device->GetRenderTarget(0, &oldRt);
                const bool haveVp = SUCCEEDED(device->GetViewport(&vp));
                if (oldRt && oldRt != rt)
                {
                    vr->CaptureGameColorOnUnbind(
                        oldRt,
                        haveVp ? vp.X : 0,
                        haveVp ? vp.Y : 0,
                        haveVp ? vp.Width : 0,
                        haveVp ? vp.Height : 0);
                }
                if (oldRt)
                    oldRt->Release();
            }
        }
        if (!g_OrigSetRenderTarget)
            return D3DERR_INVALIDCALL;
        const HRESULT hr = g_OrigSetRenderTarget(device, index, rt);
        // Redirecting the 2560x1440 backbuffer onto a taller eye keeps the
        // window viewport (1440). DXVK only forces full-eye size on the next
        // SetViewport while RT0 is already the eye — so do that here.
        if (SUCCEEDED(hr) && index == 0)
        {
            if (rt)
            {
                D3DSURFACE_DESC desc{};
                if (SUCCEEDED(rt->GetDesc(&desc)))
                {
                    g_Rt0W = desc.Width;
                    g_Rt0H = desc.Height;
                    g_Rt0ActualW = desc.Width;
                    g_Rt0ActualH = desc.Height;
                }
                else
                {
                    g_Rt0ActualW = 0;
                    g_Rt0ActualH = 0;
                }
            }
            else
            {
                g_Rt0W = 0;
                g_Rt0H = 0;
                g_Rt0ActualW = 0;
                g_Rt0ActualH = 0;
            }
            g_Rt0ActualPtr = rt;
            g_Rt0ActualKnown = true;
        }
        if (SUCCEEDED(hr) && redirectedToEye && vr
            && vr->m_RenderWidth >= 640 && vr->m_RenderHeight >= 360)
        {
            D3DVIEWPORT9 eyeVp{};
            eyeVp.MinZ = 0.f;
            eyeVp.MaxZ = 1.f;
            eyeVp.Width = vr->m_RenderWidth;
            eyeVp.Height = vr->m_RenderHeight;
            if (vr->SurfaceIsMcRing(rt))
            {
                int ox = 0, oy = 0;
                const bool have = vr->McResolveRingEyeOrigin(0, 0, 0, 0, ox, oy);
                if (have)
                {
                    eyeVp.X = static_cast<DWORD>(ox);
                    eyeVp.Y = static_cast<DWORD>(oy);
                    device->SetViewport(&eyeVp);
                    vr->McNoteRingEyeViewport(ox, oy,
                        static_cast<int>(eyeVp.Width), static_cast<int>(eyeVp.Height));
                }
            }
            else
            {
                eyeVp.X = 0;
                eyeVp.Y = 0;
                device->SetViewport(&eyeVp);
            }
        }
        return hr;
    }

    HRESULT __stdcall HookedSetDepthStencil(IDirect3DDevice9* device, IDirect3DSurface9* depth)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        IDirect3DSurface9* eyeDepth = nullptr;
        if (vr)
            eyeDepth = vr->McPlaybackEyeDepth();
        if (!eyeDepth && vr && !vr->McPipelineEnabled() && vr->StereoEyeBlitActive()
            && (bmvr::TrySteamVrEyeRt() || bmvr::OffscreenWorldMatchesEyes()))
        {
            if (vr->StereoEyeBlitDest() == vr->m_D9LeftEyeSurface)
                eyeDepth = vr->m_D9LeftEyeDepthSurface;
            else if (vr->StereoEyeBlitDest() == vr->m_D9RightEyeSurface)
                eyeDepth = vr->m_D9RightEyeDepthSurface;
        }
        if (vr && device && !vr->m_CaptureReentry && eyeDepth
            && depth && depth != eyeDepth
            && SurfaceMatchesWindowOrBackbuffer(device, depth))
        {
            depth = eyeDepth;
        }
        if (!g_OrigSetDepthStencil)
            return D3DERR_INVALIDCALL;
        return g_OrigSetDepthStencil(device, depth);
    }

    bool ViewportMatchesWindow(UINT w, UINT h)
    {
        UINT winW = 0, winH = 0;
        if (!QueryGameClientSize(winW, winH))
            return false;
        return std::abs(static_cast<int>(w) - static_cast<int>(winW)) <= 16
            && std::abs(static_cast<int>(h) - static_cast<int>(winH)) <= 16;
    }

    bool SurfaceIsEyeSizedWorldRt(IDirect3DSurface9* rt, VR* vr)
    {
        if (!rt || !vr || vr->m_RenderWidth < 640 || vr->m_RenderHeight < 360)
            return false;
        if (rt == vr->StereoEyeBlitDest()
            || rt == vr->m_D9LeftEyeSurface
            || rt == vr->m_D9RightEyeSurface
            || vr->SurfaceIsMcRing(rt))
            return true;
        D3DSURFACE_DESC desc{};
        if (FAILED(rt->GetDesc(&desc)))
            return false;
        return desc.Width == vr->m_RenderWidth && desc.Height == vr->m_RenderHeight;
    }

    bool ShouldExpandWindowVpOnWorldRt(IDirect3DDevice9* device, VR* vr)
    {
        (void)device;
        if (!vr || vr->m_CaptureReentry || vr->HudPaintActive())
            return false;
        if (!bmvr::OffscreenWorldMatchesEyes())
            return false;
        if (vr->D3dRt0IsMcRing())
            return vr->StereoWorldPassActive();
        if (!vr->StereoWorldPassActive())
            return false;
        return vr->D3dRt0IsEyeSized();
    }

    bool FloatNear(float a, float b, float eps)
    {
        return std::fabs(a - b) <= eps;
    }

    bool InvNear(float a, float b)
    {
        return std::fabs(a - b) <= (std::max)(1.5e-6f, std::fabs(b) * 2e-4f);
    }

    bool MapWindowSizeScalar(float x, float ww, float wh, float ew, float eh, float& out, bool& dim)
    {
        const float iw = 1.f / ww;
        const float ih = 1.f / wh;
        const float iew = 1.f / ew;
        const float ieh = 1.f / eh;
        dim = false;
        if (FloatNear(x, ww, 0.51f)) { out = ew; dim = true; return true; }
        if (FloatNear(x, wh, 0.51f)) { out = eh; dim = true; return true; }
        if (FloatNear(x, ww - 1.f, 0.51f)) { out = ew - 1.f; dim = true; return true; }
        if (FloatNear(x, wh - 1.f, 0.51f)) { out = eh - 1.f; dim = true; return true; }
        if (InvNear(x, iw)) { out = iew; return true; }
        if (InvNear(x, ih)) { out = ieh; return true; }
        if (InvNear(x, 0.5f * iw)) { out = 0.5f * iew; return true; }
        if (InvNear(x, 0.5f * ih)) { out = 0.5f * ieh; return true; }
        if (InvNear(x, -0.5f * iw)) { out = -0.5f * iew; return true; }
        if (InvNear(x, -0.5f * ih)) { out = -0.5f * ieh; return true; }
        if (InvNear(x, 2.f * iw)) { out = 2.f * iew; return true; }
        if (InvNear(x, 2.f * ih)) { out = 2.f * ieh; return true; }
        if (InvNear(x, -2.f * iw)) { out = -2.f * iew; return true; }
        if (InvNear(x, -2.f * ih)) { out = -2.f * ieh; return true; }
        return false;
    }

    // Copy-on-write view of one constant upload. Reads come from the game's
    // buffer; the first rewrite copies the block onto the caller's stack
    // scratch. Most uploads are never rewritten, so most never copy.
    struct ConstantCow
    {
        const float* src = nullptr;
        float* scratch = nullptr;
        UINT floats = 0;
        bool copied = false;
        const float* Cur() const { return copied ? scratch : src; }
        float* Write()
        {
            if (!copied)
            {
                std::memcpy(scratch, src, floats * sizeof(float));
                copied = true;
            }
            return scratch;
        }
    };

    bool ScreenspaceVertexC0Matches(const float* c, float ww, float wh)
    {
        const float iw = 1.f / ww;
        const float ih = 1.f / wh;
        const bool x2 = InvNear(c[0], 2.f * iw) || InvNear(c[0], -2.f * iw);
        const bool y2 = InvNear(c[1], 2.f * ih) || InvNear(c[1], -2.f * ih)
            || InvNear(c[1], 2.f * iw) || InvNear(c[1], -2.f * iw);
        return x2 && y2;
    }

    // Caller has checked ScreenspaceVertexC0Matches on the same values.
    void ApplyScreenspaceVertexC0(float* c, float ww, float wh, float ew, float eh)
    {
        const float iw = 1.f / ww;
        const float ih = 1.f / wh;
        const float iew = 1.f / ew;
        const float ieh = 1.f / eh;
        if (InvNear(c[0], 2.f * iw))
            c[0] = 2.f * iew;
        else
            c[0] = -2.f * iew;
        if (InvNear(c[1], 2.f * ih))
            c[1] = 2.f * ieh;
        else if (InvNear(c[1], -2.f * ih))
            c[1] = -2.f * ieh;
        else if (InvNear(c[1], 2.f * iw))
            c[1] = 2.f * iew;
        else
            c[1] = -2.f * iew;
        if (InvNear(c[2], -1.f + iw))
            c[2] = -1.f + iew;
        else if (InvNear(c[2], -1.f + ih))
            c[2] = -1.f + ieh;
        if (InvNear(c[3], 1.f - ih))
            c[3] = 1.f - ieh;
        else if (InvNear(c[3], 1.f - iw))
            c[3] = 1.f - iew;
    }

    bool RewriteWindowSizeConstants(ConstantCow& cow, UINT vec4Count, VR* vr)
    {
        if (!cow.src || vec4Count == 0 || !vr)
            return false;
        UINT winW = 0, winH = 0;
        if (!CachedClientSize(winW, winH))
            return false;
        const float ww = static_cast<float>(winW);
        const float wh = static_cast<float>(winH);
        const float ew = static_cast<float>(vr->m_RenderWidth);
        const float eh = static_cast<float>(vr->m_RenderHeight);
        if (ew < 640.f || eh < 360.f || ww < 640.f || wh < 360.f)
            return false;
        if (FloatNear(ww, ew, 0.51f) && FloatNear(wh, eh, 0.51f))
            return false;
        // Fast reject. Every rewrite below needs at least two floats of the
        // vec4 inside a window-size band: the dimension itself (ww/wh, -1), or
        // its inverse / half / double (1/ww .. 2/wh). Ordinary material
        // constants (colours, lighting, matrices) fail this in ~8 compares
        // instead of the ~150 the full MapWindowSizeScalar pass costs, and
        // this runs on every constant upload of every draw in stereo.
        const float bigLo = (std::min)(ww, wh) - 1.51f;
        const float bigHi = (std::max)(ww, wh) + 0.51f;
        const float invMin = 1.f / (std::max)(ww, wh);
        const float invMax = 1.f / (std::min)(ww, wh);
        const float smallLo = 0.5f * invMin * (1.f - 2e-4f) - 1.5e-6f;
        const float smallHi = 2.f * invMax * (1.f + 2e-4f) + 1.5e-6f;
        auto inBand = [&](float x) -> int {
            const float a = std::fabs(x);
            return ((a >= smallLo && a <= smallHi) || (x >= bigLo && x <= bigHi)) ? 1 : 0;
        };
        bool any = false;
        for (UINT i = 0; i < vec4Count; ++i)
        {
            const float* c = cow.Cur() + i * 4;
            if (inBand(c[0]) + inBand(c[1]) + inBand(c[2]) + inBand(c[3]) < 2)
                continue;
            if (ScreenspaceVertexC0Matches(c, ww, wh))
            {
                ApplyScreenspaceVertexC0(cow.Write() + i * 4, ww, wh, ew, eh);
                any = true;
                continue;
            }
            float mapped[4]{};
            bool ok[4]{};
            bool dim[4]{};
            int hits = 0;
            int dimHits = 0;
            for (int k = 0; k < 4; ++k)
            {
                ok[k] = MapWindowSizeScalar(c[k], ww, wh, ew, eh, mapped[k], dim[k]);
                if (!ok[k])
                    continue;
                ++hits;
                if (dim[k])
                    ++dimHits;
            }
            const int invHits = hits - dimHits;
            // Integers like 2560 can be world positions. Inverses must appear
            // twice in the same vec4 — a lone ~0.999 rotation at VS c58 was
            // rewritten as 1-1/1440 and warped fire/beam bones (bmvr_log).
            if (invHits < 2 && dimHits < 2)
                continue;
            float* w = cow.Write() + i * 4;
            for (int k = 0; k < 4; ++k)
            {
                if (!ok[k])
                    continue;
                w[k] = mapped[k];
                any = true;
            }
        }
        // Do not rewrite 1/512 and 1/1024 packs. Those are flashlight cookies /
        // shadow atlases, not window size (bmvr_log VS c25 = 1/256, 1/512).
        // Mapping them to 1/3168 unlit walls on one eye while props still
        // received the spotlight.
        return any;
    }

    bool RewritePerspectiveAspect(ConstantCow& cow, UINT vec4Count, float eyeAspect)
    {
        if (!cow.src || vec4Count < 4 || eyeAspect < 0.5f || eyeAspect > 2.5f)
            return false;
        auto near0 = [](float a) { return std::fabs(a) <= 1.0e-4f; };
        auto near1 = [](float a) { return std::fabs(a - 1.f) <= 1.0e-3f; };
        bool any = false;
        for (UINT i = 0; i + 4 <= vec4Count; ++i)
        {
            const float* m = cow.Cur() + i * 4;
            const bool d3dProj =
                !near0(m[0]) && near0(m[1]) && near0(m[2]) && near0(m[3])
                && near0(m[4]) && !near0(m[5]) && near0(m[6]) && near0(m[7])
                && near0(m[8]) && near0(m[9]) && !near0(m[10]) && near1(m[11])
                && near0(m[12]) && near0(m[13]) && !near0(m[14]) && near0(m[15]);
            const bool d3dInv =
                !near0(m[0]) && near0(m[1]) && near0(m[2]) && near0(m[3])
                && near0(m[4]) && !near0(m[5]) && near0(m[6]) && near0(m[7])
                && near0(m[8]) && near0(m[9]) && near0(m[10]) && !near0(m[11])
                && near0(m[12]) && near0(m[13]) && near1(m[14]) && !near0(m[15]);
            if (!d3dProj && !d3dInv)
                continue;
            const float sx = m[0];
            const float sy = m[5];
            if (std::fabs(sx) < 1e-8f || std::fabs(sy) < 1e-8f)
                continue;
            const float aspect = d3dProj ? (sy / sx) : (sx / sy);
            if (aspect < 1.55f || aspect > 2.05f)
                continue;
            if (std::fabs(aspect - eyeAspect) < 0.04f)
                continue;
            cow.Write()[i * 4] = d3dProj ? (sy / eyeAspect) : (sy * eyeAspect);
            any = true;
        }
        return any;
    }

    HRESULT __stdcall HookedSetShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data,
        UINT vec4Count, tSetShaderConstantF orig, const char* tag)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        const float* use = data;
        // D3D9 caps constants at 256 vec4 = 1024 floats; larger counts are
        // invalid and pass straight through. Stack only — this is the hottest
        // hook in the DLL (every constant upload of every draw), and the old
        // path heap-allocated for every bone palette upload (>16 vec4).
        float local[1024];
        // VS c32+ is bone palettes / skinning. Do not scan those for
        // 1/2560-style floats (c58 n=81 false-positive, 2026-09-01). Neither
        // rewrite applies there, so skip the scan as well.
        const bool skipVsBones = (tag && tag[0] == 'V' && start >= 32);
        if (data && vec4Count > 0 && vec4Count <= 256 && !skipVsBones && vr
            && bmvr::OffscreenWorldMatchesEyes()
            && vr->StereoWorldPassActive()
            && !vr->HudPaintActive() && !vr->m_CaptureReentry)
        {
            // Scans read the game's buffer; the block is only copied to the
            // stack when a rewrite actually fires (a few uploads per frame).
            ConstantCow cow{};
            cow.src = data;
            cow.scratch = local;
            cow.floats = vec4Count * 4;
            // VS c0-c31 n=4 packs are view/proj, not 1/1024 texel sizes.
            const bool skipVsViewProj = (tag && tag[0] == 'V' && start < 32 && vec4Count >= 4);
            const bool sizeRw = !skipVsViewProj
                && RewriteWindowSizeConstants(cow, vec4Count, vr);
            const float eyeAspect = static_cast<float>(vr->m_RenderWidth)
                / static_cast<float>(vr->m_RenderHeight);
            const bool projRw = RewritePerspectiveAspect(cow, vec4Count, eyeAspect);
            if (cow.copied && (sizeRw || projRw))
            {
                use = local;
                static int s_cLogPs;
                static int s_cLogVs;
                int& cap = (tag && tag[0] == 'V') ? s_cLogVs : s_cLogPs;
                if (cap < 12)
                {
                    Game::logMsg("D3D %s const[%u] n=%u size=%d proj=%d -> eye %ux%u orig=(%.6g,%.6g,%.6g,%.6g)",
                        tag, start, vec4Count, sizeRw ? 1 : 0, projRw ? 1 : 0,
                        vr->m_RenderWidth, vr->m_RenderHeight,
                        data[0], data[1], data[2], data[3]);
                    ++cap;
                }
            }
        }
        if (!orig)
            return D3DERR_INVALIDCALL;
        return orig(device, start, use, vec4Count);
    }

    HRESULT __stdcall HookedSetVertexShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data, UINT vec4Count)
    {
        return HookedSetShaderConstantF(device, start, data, vec4Count, g_OrigSetVertexShaderConstantF, "VS");
    }

    HRESULT __stdcall HookedSetPixelShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data, UINT vec4Count)
    {
        return HookedSetShaderConstantF(device, start, data, vec4Count, g_OrigSetPixelShaderConstantF, "PS");
    }

    HRESULT __stdcall HookedSetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* pViewport)
    {
        D3DVIEWPORT9 vp{};
        const D3DVIEWPORT9* use = pViewport;
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        if (pViewport && vr && vr->D3dRt0IsMcRing())
        {
            int cx = 0, cy = 0, cw = 0, ch = 0;
            if (vr->McClipAmbiguousWindowVp(
                static_cast<int>(pViewport->X), static_cast<int>(pViewport->Y),
                static_cast<int>(pViewport->Width), static_cast<int>(pViewport->Height),
                cx, cy, cw, ch))
            {
                vp = *pViewport;
                vp.X = static_cast<DWORD>(cx);
                vp.Y = static_cast<DWORD>(cy);
                vp.Width = static_cast<DWORD>(cw);
                vp.Height = static_cast<DWORD>(ch);
                use = &vp;
                static int s_clipLog;
                if (s_clipLog < 8)
                {
                    Game::logMsg("MC clip leftover viewport %ux%u @%u,%u -> %dx%d",
                        pViewport->Width, pViewport->Height, pViewport->X, pViewport->Y,
                        cw, ch);
                    ++s_clipLog;
                }
            }
        }
        if (use == pViewport && pViewport && vr && vr->D3dRt0IsMcRing())
        {
            int ox = 0, oy = 0;
            if (vr->McResolveRingEyeOrigin(
                static_cast<int>(pViewport->X), static_cast<int>(pViewport->Y),
                static_cast<int>(pViewport->Width), static_cast<int>(pViewport->Height),
                ox, oy))
            {
                const bool window = ViewportMatchesWindow(pViewport->Width, pViewport->Height);
                const bool eye = pViewport->Width == vr->m_RenderWidth
                    && pViewport->Height == vr->m_RenderHeight;
                if (window || eye)
                {
                    vp = *pViewport;
                    vp.X = static_cast<DWORD>(ox);
                    vp.Y = static_cast<DWORD>(oy);
                    vp.Width = vr->m_RenderWidth;
                    vp.Height = vr->m_RenderHeight;
                    use = &vp;
                    static int s_vpLog;
                    if (s_vpLog < 24)
                    {
                        Game::logMsg("D3D SetViewport %ux%u @%u,%u -> eye %ux%u @%u,%u (world RT) recordTid=%d",
                            pViewport->Width, pViewport->Height, pViewport->X, pViewport->Y,
                            vp.Width, vp.Height, vp.X, vp.Y,
                            vr->McOnRecordThread() ? 1 : 0);
                        ++s_vpLog;
                    }
                }
            }
        }
        else if (use == pViewport && pViewport && vr
            && ViewportMatchesWindow(pViewport->Width, pViewport->Height)
            && ShouldExpandWindowVpOnWorldRt(device, vr))
        {
            vp = *pViewport;
            vp.X = 0;
            vp.Y = 0;
            vp.Width = vr->m_RenderWidth;
            vp.Height = vr->m_RenderHeight;
            use = &vp;
            static int s_vpLogQ0;
            if (s_vpLogQ0 < 8)
            {
                Game::logMsg("D3D SetViewport %ux%u @%u,%u -> eye %ux%u @0,0 (q0 world RT)",
                    pViewport->Width, pViewport->Height, pViewport->X, pViewport->Y,
                    vp.Width, vp.Height);
                ++s_vpLogQ0;
            }
        }
        if (!g_OrigSetViewport)
            return D3DERR_INVALIDCALL;
        const HRESULT hr = g_OrigSetViewport(device, use);
        if (SUCCEEDED(hr) && vr && use && vr->D3dRt0IsMcRing())
        {
            vr->McNoteRingEyeViewport(
                static_cast<int>(use->X), static_cast<int>(use->Y),
                static_cast<int>(use->Width), static_cast<int>(use->Height));
        }
        return hr;
    }

    HRESULT __stdcall HookedSetScissorRect(IDirect3DDevice9* device, const RECT* pRect)
    {
        RECT expanded{};
        const RECT* use = pRect;
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        if (pRect && vr)
        {
            const UINT w = static_cast<UINT>(pRect->right - pRect->left);
            const UINT h = static_cast<UINT>(pRect->bottom - pRect->top);
            int cx = 0, cy = 0, cw = 0, ch = 0;
            if (vr->D3dRt0IsMcRing()
                && vr->McClipAmbiguousWindowVp(pRect->left, pRect->top,
                    static_cast<int>(w), static_cast<int>(h), cx, cy, cw, ch))
            {
                expanded.left = cx;
                expanded.top = cy;
                expanded.right = cx + cw;
                expanded.bottom = cy + ch;
                use = &expanded;
            }
            else if (pRect
                && ViewportMatchesWindow(w, h)
                && ShouldExpandWindowVpOnWorldRt(device, vr)
                && ((pRect->left <= 16 && pRect->top <= 16)
                    || vr->McOriginIsRingBand(pRect->left, pRect->top)))
            {
                LONG ox = 0;
                LONG oy = 0;
                bool rewrite = true;
                if (vr->D3dRt0IsMcRing())
                {
                    int rx = 0, ry = 0;
                    if (!vr->McResolveRingEyeOrigin(pRect->left, pRect->top,
                        static_cast<int>(w), static_cast<int>(h), rx, ry))
                        rewrite = false;
                    else
                    {
                        ox = rx;
                        oy = ry;
                    }
                }
                if (rewrite)
                {
                    expanded.left = ox;
                    expanded.top = oy;
                    expanded.right = ox + static_cast<LONG>(vr->m_RenderWidth);
                    expanded.bottom = oy + static_cast<LONG>(vr->m_RenderHeight);
                    use = &expanded;
                    static int s_scLog;
                    if (s_scLog < 16)
                    {
                        Game::logMsg("D3D SetScissor %ldx%ld @%ld,%ld -> eye %ux%u @%ld,%ld (world RT)",
                            pRect->right - pRect->left, pRect->bottom - pRect->top,
                            pRect->left, pRect->top,
                            vr->m_RenderWidth, vr->m_RenderHeight, ox, oy);
                        ++s_scLog;
                    }
                }
            }
        }
        if (!g_OrigSetScissorRect)
            return D3DERR_INVALIDCALL;
        return g_OrigSetScissorRect(device, use);
    }

    // Engine ClearBuffers uses the 16:9 CViewSetup viewport/rect even after we
    // grow the RT to the square eye. NULL-rect Clear then only wipes that band,
    // so leftover pixels trail with the HMD. Expand the viewport/rect; do not
    // add TARGET — that would black-clear between sky and world.
    HRESULT __stdcall HookedClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects,
        DWORD flags, D3DCOLOR color, float z, DWORD stencil)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        D3DRECT full{};
        const D3DRECT* useRects = rects;
        DWORD useCount = count;
        if (device && vr && ShouldExpandWindowVpOnWorldRt(device, vr))
        {
            D3DVIEWPORT9 vp{};
            if (g_OrigSetViewport && SUCCEEDED(device->GetViewport(&vp))
                && ViewportMatchesWindow(vp.Width, vp.Height))
            {
                D3DVIEWPORT9 eye = vp;
                bool rewrite = true;
                if (vr->D3dRt0IsMcRing())
                {
                    int cx = 0, cy = 0, cw = 0, ch = 0;
                    if (vr->McClipAmbiguousWindowVp(
                        static_cast<int>(vp.X), static_cast<int>(vp.Y),
                        static_cast<int>(vp.Width), static_cast<int>(vp.Height),
                        cx, cy, cw, ch))
                    {
                        eye.X = static_cast<DWORD>(cx);
                        eye.Y = static_cast<DWORD>(cy);
                        eye.Width = static_cast<DWORD>(cw);
                        eye.Height = static_cast<DWORD>(ch);
                    }
                    else
                    {
                    int ox = 0, oy = 0;
                    if (!vr->McResolveRingEyeOrigin(static_cast<int>(vp.X), static_cast<int>(vp.Y),
                        static_cast<int>(vp.Width), static_cast<int>(vp.Height), ox, oy))
                        rewrite = false;
                    else
                    {
                        eye.X = static_cast<DWORD>(ox);
                        eye.Y = static_cast<DWORD>(oy);
                        eye.Width = vr->m_RenderWidth;
                        eye.Height = vr->m_RenderHeight;
                    }
                    }
                }
                else
                {
                    eye.X = 0;
                    eye.Y = 0;
                    eye.Width = vr->m_RenderWidth;
                    eye.Height = vr->m_RenderHeight;
                }
                if (rewrite)
                {
                    g_OrigSetViewport(device, &eye);
                    vr->McNoteRingEyeViewport(static_cast<int>(eye.X), static_cast<int>(eye.Y),
                        static_cast<int>(eye.Width), static_cast<int>(eye.Height));
                    static int s_clrVp;
                    if (s_clrVp < 8)
                    {
                        Game::logMsg("D3D Clear expand viewport %ux%u @%u,%u -> %ux%u @%u,%u flags=0x%X",
                            vp.Width, vp.Height, vp.X, vp.Y, eye.Width, eye.Height, eye.X, eye.Y, flags);
                        ++s_clrVp;
                    }
                }
            }
            if (rects && count >= 1)
            {
                const LONG w = rects[0].x2 - rects[0].x1;
                const LONG h = rects[0].y2 - rects[0].y1;
                if (w > 0 && h > 0
                    && ViewportMatchesWindow(static_cast<UINT>(w), static_cast<UINT>(h))
                    && ((rects[0].x1 <= 16 && rects[0].y1 <= 16)
                        || vr->McOriginIsRingBand(rects[0].x1, rects[0].y1)))
                {
                    LONG ox = 0;
                    LONG oy = 0;
                    bool rewrite = true;
                    if (vr->D3dRt0IsMcRing())
                    {
                        int cx = 0, cy = 0, cw = 0, ch = 0;
                        if (vr->McClipAmbiguousWindowVp(rects[0].x1, rects[0].y1,
                            static_cast<int>(w), static_cast<int>(h), cx, cy, cw, ch))
                        {
                            ox = cx;
                            oy = cy;
                            full.x1 = cx;
                            full.y1 = cy;
                            full.x2 = cx + cw;
                            full.y2 = cy + ch;
                            useRects = &full;
                            useCount = 1;
                            rewrite = false;
                        }
                        else
                        {
                        int rx = 0, ry = 0;
                        if (!vr->McResolveRingEyeOrigin(rects[0].x1, rects[0].y1,
                            static_cast<int>(w), static_cast<int>(h), rx, ry))
                            rewrite = false;
                        else
                        {
                            ox = rx;
                            oy = ry;
                        }
                        }
                    }
                    if (rewrite)
                    {
                        full.x1 = ox;
                        full.y1 = oy;
                        full.x2 = ox + static_cast<LONG>(vr->m_RenderWidth);
                        full.y2 = oy + static_cast<LONG>(vr->m_RenderHeight);
                        useRects = &full;
                        useCount = 1;
                        static int s_clrRect;
                        if (s_clrRect < 8)
                        {
                            Game::logMsg("D3D Clear expand rect %ldx%ld @%ld,%ld -> %ux%u @%ld,%ld flags=0x%X",
                                w, h, rects[0].x1, rects[0].y1,
                                vr->m_RenderWidth, vr->m_RenderHeight, ox, oy, flags);
                            ++s_clrRect;
                        }
                    }
                }
            }
        }
        if (!g_OrigClear)
            return D3DERR_INVALIDCALL;
        return g_OrigClear(device, useCount, useRects, flags, color, z, stencil);
    }

    HRESULT __stdcall HookedStretchRect(IDirect3DDevice9* device, IDirect3DSurface9* pSource,
        const RECT* pSourceRect, IDirect3DSurface9* pDest, const RECT* pDestRect,
        D3DTEXTUREFILTERTYPE filter)
    {
        RECT expanded{};
        const RECT* useSrc = pSourceRect;
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        if (pSourceRect && pSource && pDest && vr
            && bmvr::OffscreenWorldMatchesEyes()
            && vr->StereoWorldPassActive()
            && !vr->HudPaintActive() && !vr->m_CaptureReentry)
        {
            const UINT rw = static_cast<UINT>(pSourceRect->right - pSourceRect->left);
            const UINT rh = static_cast<UINT>(pSourceRect->bottom - pSourceRect->top);
            const bool windowSrc = (pSourceRect->left <= 16 && pSourceRect->top <= 16
                    && ViewportMatchesWindow(rw, rh))
                || (vr->McPipelineEnabled() && vr->McOriginIsRingBand(pSourceRect->left, pSourceRect->top)
                    && ViewportMatchesWindow(rw, rh));
            if (windowSrc)
            {
                D3DSURFACE_DESC srcDesc{};
                D3DSURFACE_DESC dstDesc{};
                if (SUCCEEDED(pSource->GetDesc(&srcDesc))
                    && SUCCEEDED(pDest->GetDesc(&dstDesc)))
                {
                    const bool srcEye = srcDesc.Width == vr->m_RenderWidth
                        && srcDesc.Height == vr->m_RenderHeight;
                    const bool srcRing = vr->SurfaceIsMcRing(pSource);
                    const bool destWindow = ViewportMatchesWindow(dstDesc.Width, dstDesc.Height);
                    const bool destEye = dstDesc.Width == vr->m_RenderWidth
                        && dstDesc.Height == vr->m_RenderHeight;
                    if ((srcEye || srcRing) && (!destWindow || destEye))
                    {
                        LONG ox = 0;
                        LONG oy = 0;
                        bool rewrite = true;
                        if (srcRing)
                        {
                            int cx = 0, cy = 0, cw = 0, ch = 0;
                            if (vr->McClipAmbiguousWindowVp(pSourceRect->left, pSourceRect->top,
                                static_cast<int>(rw), static_cast<int>(rh),
                                cx, cy, cw, ch))
                            {
                                expanded.left = cx;
                                expanded.top = cy;
                                expanded.right = cx + cw;
                                expanded.bottom = cy + ch;
                                useSrc = &expanded;
                                rewrite = false;
                            }
                            else
                            {
                            int rx = 0, ry = 0;
                            if (!vr->McResolveRingEyeOrigin(pSourceRect->left, pSourceRect->top,
                                static_cast<int>(rw), static_cast<int>(rh), rx, ry))
                                rewrite = false;
                            else
                            {
                                ox = rx;
                                oy = ry;
                            }
                            }
                        }
                        if (rewrite)
                        {
                            expanded.left = ox;
                            expanded.top = oy;
                            expanded.right = ox + static_cast<LONG>(vr->m_RenderWidth);
                            expanded.bottom = oy + static_cast<LONG>(vr->m_RenderHeight);
                            useSrc = &expanded;
                            static int s_srLog;
                            if (s_srLog < 16)
                            {
                                Game::logMsg("D3D StretchRect src %ux%u @%ld,%ld -> eye %ux%u @%ld,%ld dest=%ux%u",
                                    rw, rh, pSourceRect->left, pSourceRect->top,
                                    vr->m_RenderWidth, vr->m_RenderHeight, ox, oy,
                                    dstDesc.Width, dstDesc.Height);
                                ++s_srLog;
                            }
                        }
                    }
                }
            }
        }
        if (!g_OrigStretchRect)
            return D3DERR_INVALIDCALL;
        return g_OrigStretchRect(device, pSource, useSrc, pDest, pDestRect, filter);
    }

    // Both go through the bmvr HWND / client-rect cache. The old per-call
    // FindWindowA ran from every SetViewport / SetScissorRect / Clear /
    // StretchRect / GetScreenSize hook (hundreds of desktop window-list walks
    // per frame on the render thread).
    bool QueryGameClientSize(UINT& w, UINT& h)
    {
        uint32_t cw = 0, ch = 0;
        if (!bmvr::QueryWindowClientSize(cw, ch))
            return false;
        w = cw;
        h = ch;
        return true;
    }

    bool CachedClientSize(UINT& w, UINT& h)
    {
        // Keep the last good size across brief window-less moments (Reset).
        static UINT s_w = 0;
        static UINT s_h = 0;
        UINT qw = 0, qh = 0;
        if (QueryGameClientSize(qw, qh))
        {
            s_w = qw;
            s_h = qh;
        }
        w = s_w;
        h = s_h;
        return w >= 640 && h >= 360;
    }

    using tBeginRTAlloc = void(__thiscall*)(void*);
    using tEndRTAlloc = void(__thiscall*)(void*);
    using tCreateNamedRTEx = ITexture*(__thiscall*)(void*, const char*, int, int, int, int, int, unsigned, unsigned);

    void SehOverrideAlphaWrite(IMatRenderContext* ctx, bool enable, bool write)
    {
        if (!ctx)
            return;
        __try
        {
            ctx->OverrideAlphaWriteEnable(enable, write);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // StretchRect without leaving DXVK's swapchain/viewport on the eye RT.
    // Cover-crop onto the HWND backbuffer from the material thread reset the
    // device viewport to 2560x1440, so CLensflare WorldToScreen (eye size)
    // and GetViewport (16:9) drifted apart. Also avoids Present-vs-swapchain
    // StretchRect stalls when dest is the backbuffer.
    HRESULT StretchRectKeepDeviceState(IDirect3DDevice9* device,
        IDirect3DSurface9* src, const RECT* srcRect,
        IDirect3DSurface9* dst, const RECT* dstRect,
        D3DTEXTUREFILTERTYPE filter)
    {
        if (!device || !src || !dst)
            return D3DERR_INVALIDCALL;
        IDirect3DSurface9* oldRt = nullptr;
        D3DVIEWPORT9 oldVp{};
        device->GetRenderTarget(0, &oldRt);
        device->GetViewport(&oldVp);
        if (oldRt == dst && src != dst)
        {
            if (g_OrigSetRenderTarget)
                g_OrigSetRenderTarget(device, 0, src);
            else
                device->SetRenderTarget(0, src);
        }
        HRESULT hr = E_FAIL;
        if (g_OrigStretchRect)
            hr = g_OrigStretchRect(device, src, srcRect, dst, dstRect, filter);
        else
            hr = device->StretchRect(src, srcRect, dst, dstRect, filter);
        if (oldRt == dst && oldRt)
        {
            if (g_OrigSetRenderTarget)
                g_OrigSetRenderTarget(device, 0, oldRt);
            else
                device->SetRenderTarget(0, oldRt);
        }
        if (g_OrigSetViewport)
            g_OrigSetViewport(device, &oldVp);
        else
            device->SetViewport(&oldVp);
        if (oldRt)
            oldRt->Release();
        return hr;
    }

    ITexture* SehCreateNamedEyeRT(tCreateNamedRTEx fn, void* mat, const char* name, int w, int h)
    {
        ITexture* tex = nullptr;
        if (!fn || !mat)
            return nullptr;
        __try
        {
            tex = fn(mat, name, w, h, RT_SIZE_LITERAL, IMAGE_FORMAT_RGBA16161616F,
                MATERIAL_RT_DEPTH_SHARED,
                TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT,
                CREATERENDERTARGETFLAGS_HDR);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tex = nullptr;
        }
        return tex;
    }

    void SehBeginRTAlloc(tBeginRTAlloc fn, void* mat, bool& ok)
    {
        ok = false;
        if (!fn || !mat)
            return;
        __try
        {
            fn(mat);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
    }

    void SehEndRTAlloc(tEndRTAlloc fn, void* mat)
    {
        if (!fn || !mat)
            return;
        __try
        {
            fn(mat);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    const char* SehGetLevelNameShort(IEngineClient* eng)
    {
        const char* map = nullptr;
        if (!eng)
            return "";
        __try
        {
            map = eng->GetLevelNameShort();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            map = nullptr;
        }
        return map ? map : "";
    }

    bool SehIsPaused(IEngineClient* eng)
    {
        if (!eng)
            return false;
        bool paused = false;
        __try
        {
            paused = eng->IsPaused();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            paused = false;
        }
        return paused;
    }

    bool SehIsGameUIVisible(void* engineVgui)
    {
        if (!engineVgui)
            return false;
        bool vis = false;
        __try
        {
            vis = static_cast<IEngineVGui*>(engineVgui)->IsGameUIVisible();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vis = false;
        }
        return vis;
    }

    HWND FindGameHwnd()
    {
        return bmvr::GameWindow();
    }

    void QueueKeyToHwnd(HWND hwnd, int vk)
    {
        if (!hwnd)
            return;
        PostMessageA(hwnd, WM_KEYDOWN, static_cast<WPARAM>(vk), 1);
        PostMessageA(hwnd, WM_KEYUP, static_cast<WPARAM>(vk),
            static_cast<LPARAM>(1u | (1u << 30) | (1u << 31)));
    }

    bool CopySurfaceLetterboxed(IDirect3DDevice9* device, IDirect3DSurface9* src, IDirect3DSurface9* dst, float scale)
    {
        if (!device || !src || !dst)
            return false;
        D3DSURFACE_DESC sd{};
        D3DSURFACE_DESC dd{};
        if (FAILED(src->GetDesc(&sd)) || FAILED(dst->GetDesc(&dd)))
            return false;
        if (sd.Width < 64 || sd.Height < 64 || dd.Width < 64 || dd.Height < 64)
            return false;
        device->ColorFill(dst, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
        const float srcA = static_cast<float>(sd.Width) / static_cast<float>(sd.Height);
        const float dstA = static_cast<float>(dd.Width) / static_cast<float>(dd.Height);
        LONG dw = static_cast<LONG>(dd.Width);
        LONG dh = static_cast<LONG>(dd.Height);
        LONG dx = 0;
        LONG dy = 0;
        if (srcA > dstA + 0.01f)
        {
            dh = static_cast<LONG>(static_cast<float>(dd.Width) / srcA + 0.5f);
            if (dh < 2)
                dh = 2;
            dy = (static_cast<LONG>(dd.Height) - dh) / 2;
        }
        else if (dstA > srcA + 0.01f)
        {
            dw = static_cast<LONG>(static_cast<float>(dd.Height) * srcA + 0.5f);
            if (dw < 2)
                dw = 2;
            dx = (static_cast<LONG>(dd.Width) - dw) / 2;
        }
        if (!(scale > 0.2f && scale <= 1.f))
            scale = 0.70f;
        if (scale < 0.999f)
        {
            const LONG ndw = (std::max)(2L, static_cast<LONG>(static_cast<float>(dw) * scale + 0.5f));
            const LONG ndh = (std::max)(2L, static_cast<LONG>(static_cast<float>(dh) * scale + 0.5f));
            dx += (dw - ndw) / 2;
            dy += (dh - ndh) / 2;
            dw = ndw;
            dh = ndh;
        }
        RECT dest{ dx, dy, dx + dw, dy + dh };
        const HRESULT hr = device->StretchRect(src, nullptr, dst, &dest, D3DTEXF_LINEAR);
        if (SUCCEEDED(hr))
            return true;
        Game::logMsg("Letterbox StretchRect failed hr=0x%08X src=%ux%u dst=%ux%u dest=%d,%d %dx%d — squash fallback",
            (unsigned)hr, sd.Width, sd.Height, dd.Width, dd.Height,
            (int)dx, (int)dy, (int)dw, (int)dh);
        return SUCCEEDED(device->StretchRect(src, nullptr, dst, nullptr, D3DTEXF_LINEAR));
    }
}

class Hl2vrAwaitFrameFunctor : public CFunctor
{
public:
    Hl2vrAwaitFrameFunctor(VR* vr, uint64_t frameId)
        : m_vr(vr)
        , m_frameId(frameId)
    {
    }

    int AddRef() override
    {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    int Release() override
    {
        const int n = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0)
            delete this;
        return n;
    }

    void operator()() override
    {
        if (m_vr)
            m_vr->MatAwaitFrame(m_frameId);
    }

private:
    VR* m_vr;
    uint64_t m_frameId;
    std::atomic<int> m_refs{ 0 };
};

class McMatFunctor : public CFunctor
{
public:
    enum class Kind { Bind, Composite };

    McMatFunctor(VR* vr, Kind kind, int stereoEye, uint32_t band, bool flushGpu)
        : McMatFunctor(vr, kind, stereoEye, band, flushGpu, nullptr)
    {
    }

    McMatFunctor(VR* vr, Kind kind, int stereoEye, uint32_t band, bool flushGpu,
        const L4D2VROpenXrPoseDesc* pose)
        : m_vr(vr)
        , m_kind(kind)
        , m_stereoEye(stereoEye)
        , m_band(band)
        , m_flushGpu(flushGpu)
        , m_havePose(pose != nullptr && pose->valid != 0)
    {
        if (m_havePose)
            m_pose = *pose;
    }

    int AddRef() override
    {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    int Release() override
    {
        const int n = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0)
            delete this;
        return n;
    }

    void operator()() override
    {
        if (!m_vr)
            return;
        if (m_kind == Kind::Composite)
            m_vr->McCompositeEyesToRing(m_band, m_havePose ? &m_pose : nullptr);
        else
            m_vr->McBindPlaybackEye(m_stereoEye, m_flushGpu);
    }

private:
    VR* m_vr;
    Kind m_kind;
    int m_stereoEye;
    uint32_t m_band;
    bool m_flushGpu;
    bool m_havePose = false;
    L4D2VROpenXrPoseDesc m_pose{};
    std::atomic<int> m_refs{ 0 };
};

class McPauseHudFunctor : public CFunctor
{
public:
    enum class Kind { Begin, End };

    McPauseHudFunctor(VR* vr, Kind kind)
        : m_vr(vr)
        , m_kind(kind)
    {
    }

    int AddRef() override
    {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    int Release() override
    {
        const int n = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0)
            delete this;
        return n;
    }

    void operator()() override
    {
        if (!m_vr)
            return;
        if (m_kind == Kind::Begin)
            m_vr->McPauseHudBeginOnMatThread();
        else
            m_vr->McPauseHudEndOnMatThread();
    }

private:
    VR* m_vr;
    Kind m_kind;
    std::atomic<int> m_refs{ 0 };
};

static IMatRenderContext* SehGetRenderContext(void* mat)
{
    IMatRenderContext* ctx = nullptr;
    if (!mat)
        return nullptr;
    __try
    {
        void** vt = *reinterpret_cast<void***>(mat);
        if (vt)
        {
            using Fn = IMatRenderContext*(__thiscall*)(void*);
            auto fn = reinterpret_cast<Fn>(vt[Offsets::kIMaterialSystem_GetRenderContext / 4]);
            if (fn)
                ctx = fn(mat);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ctx = nullptr;
    }
    return ctx;
}

static void SehReleaseMatContext(IMatRenderContext* ctx)
{
    if (!ctx)
        return;
    __try
    {
        void** vt = *reinterpret_cast<void***>(ctx);
        if (!vt)
            return;
        using Fn = int(__thiscall*)(void*);
        auto fn = reinterpret_cast<Fn>(vt[Offsets::kIMatRenderContext_Release / 4]);
        if (fn)
            fn(ctx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static ICallQueue* SehCallQueueFromSlot(void* ctx, int slot)
{
    ICallQueue* q = nullptr;
    if (!ctx || slot < 0)
        return nullptr;
    __try
    {
        void** vt = *reinterpret_cast<void***>(ctx);
        if (!vt)
            return nullptr;
        using Fn = ICallQueue*(__thiscall*)(void*);
        auto fn = reinterpret_cast<Fn>(vt[slot]);
        if (!fn)
            return nullptr;
        q = fn(ctx);
        if (q)
        {
            void** qvt = *reinterpret_cast<void***>(q);
            if (!qvt || !qvt[0])
                q = nullptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        q = nullptr;
    }
    return q;
}

static bool SehQueueFunctorInternal(ICallQueue* q, CFunctor* functor)
{
    if (!q || !functor)
        return false;
    bool ok = false;
    __try
    {
        void** vt = *reinterpret_cast<void***>(q);
        if (vt && vt[0])
        {
            using Fn = void(__thiscall*)(void*, CFunctor*);
            reinterpret_cast<Fn>(vt[0])(q, functor);
            ok = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    return ok;
}

VR::VR(Game* game)
    : m_Game(game)
{
    Game::logMsg("VR ctor: L4D2VR OpenVR / OpenXR helper path (Black Mesa capture)");
    const VrRuntimeBackendConfig runtimeConfig = L4D2VR_ReadRuntimeBackendConfig();
    m_RuntimeBackendFallbackToOpenVR = true;
    const VrRuntimeBackendSelection runtimeSelection =
        L4D2VR_SelectRuntimeBackend(runtimeConfig.requestedBackend, m_RuntimeBackendFallbackToOpenVR);
    m_RequestedRuntimeBackend = runtimeSelection.requested;
    m_RuntimeBackend = runtimeSelection.active;
    m_OpenXrLoaderAvailable = runtimeSelection.openXrLoaderAvailable;
    Game::logMsg(
        "[VR][Runtime] requested=%s active=%s fallback=%d usedFallback=%d openxrLoader=%d openxrBackendImplemented=%d detail=%s",
        L4D2VR_RuntimeBackendName(runtimeSelection.requested),
        L4D2VR_RuntimeBackendName(runtimeSelection.active),
        runtimeSelection.fallbackToOpenVR ? 1 : 0,
        runtimeSelection.usedFallback ? 1 : 0,
        runtimeSelection.openXrLoaderAvailable ? 1 : 0,
        runtimeSelection.openXrBackendImplemented ? 1 : 0,
        runtimeSelection.message.c_str());

    if (runtimeSelection.active == VrRuntimeBackend::OpenXR)
    {
        m_IsInitialized = InitOpenXR();
        if (m_IsInitialized)
            return;
        Game::logMsg("[VR][Runtime] OpenXR helper init failed; falling back to OpenVR (desktop must still launch)");
        m_OpenXrHelperBridgeActive = false;
        m_RuntimeBackend = VrRuntimeBackend::OpenVR;
    }
    m_IsInitialized = InitOpenVR();
}

bool VR::IsGameplayMapName(const char* map)
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

void VR::PollMapFromEngine()
{
    if (!m_Game || !m_Game->m_EngineClient)
        return;
    const char* map = SehGetLevelNameShort(m_Game->m_EngineClient);
    const size_t n = strlen(map);
    if (n > 96)
        return;
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(map[i]);
        if (ch < 32 || ch > 126)
            return;
    }
    if (m_CurrentMapName == map)
        return;
    if (!map[0])
    {
        if (!m_CurrentMapName.empty())
            OnLevelShutdown();
    m_CompositorHandoffSlowCount = 0;
        m_CurrentMapName.clear();
        return;
    }
    OnLevelInit(map);
}

void VR::OnLevelInit(const char* newmap)
{
    m_CurrentMapName = newmap ? newmap : "";
    m_GameplayEligible = IsGameplayMapName(newmap);
    bmvr::NoteEngineMapName(newmap);
    bmvr::SetGameplayWorldRts(m_GameplayEligible);
    m_SeenGameplay = false;
    m_GameplayFrames = 0;
    m_EligiblePresents = 0;
    m_SafeLookActive = false;
    m_LookApplyEnabled = false;
    m_HmdOriginLatched = false;
    m_PassThroughMainViews = 0;
    m_AutoMatQueueModeLastRequested = -999;
    ResetMcPipeline();
    m_GameUiVisible = false;
    m_GameUiActivateMs = 0;
    m_MenuLastVisibleMs = 0;
    m_MenuPanelPoseValid = false;
    m_WeaponMenuPrevEntity = 0;
    m_WeaponMenuPrevKind = 0;
    Game::logMsg("LevelInit map=%s eligible=%d", m_CurrentMapName.c_str(), m_GameplayEligible ? 1 : 0);
    m_PhysicalCrowbarModel = nullptr;
    m_PhysicalCrowbarLoadTries = 0;
    m_CrowbarLastMotionCheckMs = 0;
    m_CrowbarNextAttackMs = 0;
    VectorClear(m_CrowbarPrevTipLocal);
    if (m_GameplayEligible)
        bmvr::EndRisky(L"menu_vr");
    ApplyRenderTargetFramebufferOverride();
}

bool VR::ShouldCompositorSubmit() const
{
    if (!m_IsVREnabled)
        return false;
    if (m_GameplayEligible)
    {
        // LevelInit sets eligible before IsInGame. Keep Submit off until
        // spawn so load precache is not also capturing. Size lies stay on
        // so G-buffers and GetScreenSize match (see hooks.cpp).
        return m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    }
    // -oldgameui never loads background01. GetLevelNameShort stays empty for
    // the whole main menu (2026-09-03: ~11k Presents, map=, createdRT=0).
    // Requiring a map name left the HMD black until a real LevelInit.
    // Skip-file / crash-sticky on menu_vr also left menuVR=0, createdRT=0,
    // helper submitted=0 (black HMD, desktop cursor still worked). Always
    // capture+Submit GameUI while VR is up. Capture no-ops if the BB is tiny.
    return true;
}

void VR::OnLevelShutdown()
{
    m_GameplayEligible = false;
    bmvr::SetGameplayWorldRts(false);
    m_SeenGameplay = false;
    m_SafeLookActive = false;
    m_LookApplyEnabled = false;
    m_DirectEyeSubmit = false;
    m_StereoRenderViewActive = false;
    m_PassThroughMainViews = 0;
    Hooks::RestoreViewmodelArmHides();
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        m_HasViewmodelBake = false;
    }
    m_FirstAttackLogged = false;
    m_FirstAttackPresentTick = 0;
    m_FirstAttackSpikeLogs = 0;
    m_CompositorHandoffSlowCount = 0;
    m_EmptyHands = false;
    m_GameUiVisible = false;
    m_GameUiActivateMs = 0;
    m_MenuLastVisibleMs = 0;
    m_MenuPanelPoseValid = false;
    Game::logMsg("LevelShutdown");
}

void VR::ApplyRenderTargetFramebufferOverride(void* materialSystem)
{
    if (!bmvr::TryFramebufferOverride())
        return;
    void* mat = materialSystem;
    if (!mat && m_Game)
        mat = m_Game->m_MaterialSystem;
    if (!mat)
        return;

    static uint32_t s_setW = 0;
    static uint32_t s_setH = 0;

    uint32_t w = 0, h = 0;
    static bool s_latchedOffscreen = false;
    if (bmvr::TryOffscreenWorldGrow() && bmvr::ComputeWorldRtOverrideSize(w, h))
    {
        if (s_latchedOffscreen && s_setW != 0)
        {
            // Do not follow live SteamVR SS. G-buffers are sized at map load.
            return;
        }
    }
    else if (bmvr::TryOffscreenWorldGrow())
    {
        // Pin HWND until FullFrame itself is eye-sized. Advertising the eye
        // size here is what created 3168 G-buffers behind a 2560 FullFrame
        // (flashlight missing, ghost world, 2026-09-01).
        s_latchedOffscreen = false;
        uint32_t winW = 0, winH = 0;
        if (!bmvr::QueryWindowClientSize(winW, winH))
            return;
        w = winW;
        h = winH;
        static uint32_t s_pinFfW, s_pinFfH;
        if (s_pinFfW != bmvr::g_FullFrameActualWidth || s_pinFfH != bmvr::g_FullFrameActualHeight)
        {
            s_pinFfW = bmvr::g_FullFrameActualWidth;
            s_pinFfH = bmvr::g_FullFrameActualHeight;
            uint32_t eyeW = 0, eyeH = 0;
            bmvr::ComputeOffscreenEyeSize(eyeW, eyeH);
            Game::logMsg(
                "RT FB override pin HWND %ux%u (FullFrame %ux%u != eye %ux%u; G-buffer must not outgrow FullFrame)",
                w, h, s_pinFfW, s_pinFfH, eyeW, eyeH);
        }
    }
    else
    {
        s_latchedOffscreen = false;
        uint32_t winW = 0, winH = 0;
        if (s_setW != 0 && bmvr::QueryWindowClientSize(winW, winH)
            && (s_setW != winW || s_setH != winH))
        {
            w = winW;
            h = winH;
        }
        else
            return;
    }

    void** vt = nullptr;
    __try
    {
        vt = *reinterpret_cast<void***>(mat);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt = nullptr;
    }
    if (!vt || !vt[Offsets::kIMaterialSystem_SetRTFBOverrideVt]
        || !vt[Offsets::kIMaterialSystem_GetRTFBDimensionsVt])
        return;

    if (s_setW == w && s_setH == h)
        return;

    using SetFn = void(__thiscall*)(void*, int, int);
    using GetFn = void(__thiscall*)(void*, int&, int&);
    int gotW = 0;
    int gotH = 0;
    __try
    {
        bmvr::BeginRisky(L"fb_override");
        reinterpret_cast<SetFn>(vt[Offsets::kIMaterialSystem_SetRTFBOverrideVt])(
            mat, static_cast<int>(w), static_cast<int>(h));
        reinterpret_cast<GetFn>(vt[Offsets::kIMaterialSystem_GetRTFBDimensionsVt])(
            mat, gotW, gotH);
        s_setW = w;
        s_setH = h;
        uint32_t offW = 0, offH = 0;
        s_latchedOffscreen = bmvr::ComputeWorldRtOverrideSize(offW, offH);
        bmvr::EndRisky(L"fb_override");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Game::logMsg("SetRenderTargetFrameBufferSizeOverrides SEH %ux%u", w, h);
        return;
    }
    Game::logMsg("SetRenderTargetFrameBufferSizeOverrides %ux%u (Get %dx%d) HWND unchanged",
        w, h, gotW, gotH);
}

void VR::LogFullFrameSizeIfReady()
{
    if (!m_Game || !m_Game->m_MaterialSystem)
        return;
    // Four IMaterialSystem::FindTexture lookups per top-level RenderView. The
    // FullFrame RT only changes size on map load / mode change; 20Hz is plenty
    // for ChooseEyeRenderSize to follow it.
    static DWORD s_lastPollMs;
    const DWORD nowMs = GetTickCount();
    if (s_lastPollMs != 0 && nowMs - s_lastPollMs < 50)
        return;
    s_lastPollMs = nowMs ? nowMs : 1;
    static int s_logs;
    void** vt = nullptr;
    __try
    {
        vt = *reinterpret_cast<void***>(m_Game->m_MaterialSystem);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt = nullptr;
    }
    if (!vt || !vt[Offsets::kIMaterialSystem_FindTextureVt])
        return;
    using FindFn = ITexture*(__thiscall*)(void*, const char*, const char*, bool, int);
    const char* names[] = { "_rt_FullFrameFB", "_rt_FullFrameFB1", "_rt_gui", "_rt_Hud" };
    bool any = false;
    for (const char* name : names)
    {
        ITexture* tex = nullptr;
        int aw = 0;
        int ah = 0;
        __try
        {
            tex = reinterpret_cast<FindFn>(vt[Offsets::kIMaterialSystem_FindTextureVt])(
                m_Game->m_MaterialSystem, name, "RenderTargets", false, 0);
            if (tex)
            {
                aw = tex->GetActualWidth();
                ah = tex->GetActualHeight();
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tex = nullptr;
        }
        if (tex && aw > 0)
        {
            if (s_logs < 4)
                Game::logMsg("FindTexture %s actual %dx%d", name, aw, ah);
            any = true;
            if (name && std::strcmp(name, "_rt_FullFrameFB") == 0)
            {
                const uint32_t nw = static_cast<uint32_t>(aw);
                const uint32_t nh = static_cast<uint32_t>(ah);
                if (nw != bmvr::g_FullFrameActualWidth || nh != bmvr::g_FullFrameActualHeight)
                    Game::logMsg("FullFrame actual %ux%u -> %ux%u",
                        bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight, nw, nh);
                bmvr::g_FullFrameActualWidth = nw;
                bmvr::g_FullFrameActualHeight = nh;
            }
        }
    }
    if (any && s_logs < 8)
        ++s_logs;
}

void VR::HandleMissingRenderContext(const char* location)
{
    Game::logMsg("Missing render context at %s", location ? location : "?");
}

bool VR::InitOpenVR()
{
    ++m_OpenVRInitAttempts;
    m_AntiAliasing = bmvr::g_AntiAliasing;

    if (bmvr::g_OpenVRInitedFromCreateDevice && vr::VRSystem())
    {
        m_System = vr::VRSystem();
        Game::logMsg("OpenVR already initialized from CreateDevice");
    }
    else
    {
        vr::EVRInitError error = vr::VRInitError_None;
        m_System = vr::VR_Init(&error, vr::VRApplication_Scene);
        if (error != vr::VRInitError_None || !m_System)
        {
            Game::logMsg("VR_Init failed (%d): %s", (int)error,
                vr::VR_GetVRInitErrorAsEnglishDescription(error));
            m_System = nullptr;
            m_IsVREnabled = false;
            return false;
        }
    }

    m_Compositor = vr::VRCompositor();
    if (!m_Compositor)
    {
        Game::logMsg("VRCompositor() returned null");
        m_IsVREnabled = false;
        return false;
    }

    m_Input = vr::VRInput();
    m_Overlay = vr::VROverlay();
    if (m_Overlay && bmvr::TryHudOverlay())
    {
        const vr::EVROverlayError oe = m_Overlay->CreateOverlay("bmvr.hud", "Black Mesa VR HUD", &m_HUDTopHandle);
        if (oe == vr::VROverlayError_None)
        {
            m_Overlay->SetOverlayWidthInMeters(m_HUDTopHandle, bmvr::g_HudSize);
            m_Overlay->SetOverlayFlag(m_HUDTopHandle, vr::VROverlayFlags_IgnoreTextureAlpha, false);
            m_Overlay->SetOverlayInputMethod(m_HUDTopHandle, vr::VROverlayInputMethod_None);
            m_Overlay->HideOverlay(m_HUDTopHandle);
            Game::logMsg("HUD overlay created (distance=%.2f m width=%.2f m; hidden until VGUI paints)",
                bmvr::g_HudDistance, bmvr::g_HudSize);
        }
        else
        {
            Game::logMsg("HUD overlay CreateOverlay err=%d", (int)oe);
            m_HUDTopHandle = vr::k_ulOverlayHandleInvalid;
        }
    }

    uint32_t recW = bmvr::g_RecommendedEyeWidth;
    uint32_t recH = bmvr::g_RecommendedEyeHeight;
    if (recW < 640 || recH < 360)
        m_System->GetRecommendedRenderTargetSize(&recW, &recH);
    if (recW >= 640 && recH >= 360)
    {
        bmvr::g_RecommendedEyeWidth = recW;
        bmvr::g_RecommendedEyeHeight = recH;
        // Do not assign m_RenderWidth/Height to the HMD size unless the
        // swapchain retry is still enabled. Reset() forces the D3D9
        // backbuffer to these values; 3168x3100 produced a black desktop
        // and SteamVR waiting room on this DLL (2026-08-16).
        if (bmvr::TryHmdSwapchain())
        {
            m_RenderWidth = recW;
            m_RenderHeight = recH;
        }
    }
    Game::logMsg("OpenVR recommended RT %ux%u (swapchain force=%d, eye/swapchain size %ux%u)",
        recW, recH, bmvr::TryHmdSwapchain() ? 1 : 0, m_RenderWidth, m_RenderHeight);
    ApplyRenderTargetFramebufferOverride();
    RefreshIpdFromHmd();

    float l_left = 0, l_right = 0, l_top = 0, l_bottom = 0;
    m_System->GetProjectionRaw(vr::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);
    float r_left = 0, r_right = 0, r_top = 0, r_bottom = 0;
    m_System->GetProjectionRaw(vr::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    const float tanHalfFovX = (std::max)({ -l_left, l_right, -r_left, r_right });
    const float tanHalfFovY = (std::max)({ -l_top, l_bottom, -r_top, r_bottom });

    // Same projection crop as L4D2VR vr_lifecycle_init.inl. Keep OpenVR v
    // unflipped on this DXVK path (Vulkan v-swap inverted the fused image).
    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFovX;
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFovX;
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFovY;
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFovY;
    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFovX;
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFovX;
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFovY;
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFovY;
    Game::logMsg("OpenVR projection UV L=(%.3f,%.3f)-(%.3f,%.3f) R=(%.3f,%.3f)-(%.3f,%.3f)",
        m_TextureBounds[0].uMin, m_TextureBounds[0].vMin, m_TextureBounds[0].uMax, m_TextureBounds[0].vMax,
        m_TextureBounds[1].uMin, m_TextureBounds[1].vMin, m_TextureBounds[1].uMax, m_TextureBounds[1].vMax);

    m_Aspect = tanHalfFovX / tanHalfFovY;
    m_Fov = 2.0f * atanf(tanHalfFovX) * 180.0f / 3.14159265358979323846f;
    m_FovLeft = m_Fov;
    m_FovRight = m_Fov;
    ChooseEyeRenderSize();

    m_IsVREnabled = true;
    m_CompositorAppHandoff = bmvr::g_CompositorPostPresentHandoff;
    m_Compositor->SetExplicitTimingMode(m_CompositorAppHandoff
        ? vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff
        : vr::VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff);
    m_Compositor->CompositorBringToFront();
    SetActionManifest();
    Game::logMsg("OpenVR scene app ready compositor=%p canRender=%d fov=%.1f aspect=%.3f appHandoff=%d aa=%u",
        (void*)m_Compositor, m_Compositor->CanRenderScene() ? 1 : 0, m_Fov, m_Aspect,
        m_CompositorAppHandoff ? 1 : 0, m_AntiAliasing);
    return true;
}

void VR::BindOpenXrActionHandles()
{
    auto handle = [](L4D2VROpenXrActionId id) -> vr::VRActionHandle_t {
        return static_cast<vr::VRActionHandle_t>(static_cast<uint32_t>(id));
    };
    m_ActionJump = handle(L4D2VROpenXrActionId::Jump);
    m_ActionPrimaryAttack = handle(L4D2VROpenXrActionId::PrimaryAttack);
    m_ActionSecondaryAttack = handle(L4D2VROpenXrActionId::SecondaryAttack);
    m_ActionReload = handle(L4D2VROpenXrActionId::Reload);
    m_ActionUse = handle(L4D2VROpenXrActionId::Use);
    m_ActionWalk = handle(L4D2VROpenXrActionId::Walk);
    m_ActionTurn = handle(L4D2VROpenXrActionId::Turn);
    m_ActionNextItem = handle(L4D2VROpenXrActionId::NextItem);
    m_ActionPrevItem = handle(L4D2VROpenXrActionId::PrevItem);
    m_ActionResetPosition = handle(L4D2VROpenXrActionId::ResetPosition);
    m_ActionCrouch = handle(L4D2VROpenXrActionId::Crouch);
    m_ActionFlashlight = handle(L4D2VROpenXrActionId::Flashlight);
    m_ActionScoreboard = handle(L4D2VROpenXrActionId::Scoreboard);
    m_ActionPause = handle(L4D2VROpenXrActionId::Pause);
    m_ActionMenuSelect = handle(L4D2VROpenXrActionId::MenuSelect);
    m_ActionMenuBack = handle(L4D2VROpenXrActionId::MenuBack);
    m_ActionMenuUp = handle(L4D2VROpenXrActionId::MenuUp);
    m_ActionMenuDown = handle(L4D2VROpenXrActionId::MenuDown);
    m_ActionMenuLeft = handle(L4D2VROpenXrActionId::MenuLeft);
    m_ActionMenuRight = handle(L4D2VROpenXrActionId::MenuRight);
    m_ActionWeaponMenu = handle(L4D2VROpenXrActionId::InventoryQuickSwitch);
    m_ActionInventoryQuickSwitch = handle(L4D2VROpenXrActionId::InventoryQuickSwitch);
    // Helper has no Sprint action id. CustomAction1 is left-stick click (G2 sprint).
    m_ActionSprint = handle(L4D2VROpenXrActionId::CustomAction1);
    m_ActionCrouchToggle = handle(L4D2VROpenXrActionId::CustomAction2);
    m_ActionSkeletonLeft = handle(L4D2VROpenXrActionId::CustomAction3);
    m_ActionSkeletonRight = handle(L4D2VROpenXrActionId::CustomAction4);
    m_ActionsReady.store(true, std::memory_order_release);
}

bool VR::InitOpenXR()
{
    const OpenXrHelperLaunchConfig helperConfig = L4D2VR_ReadOpenXrHelperLaunchConfig();
    m_OpenXrSwapGameEyeOrigins = helperConfig.swapGameEyeOrigins;
    Game::logMsg("[VR][OpenXRHelper] enabled=%d submitTestFrames=%u waitReadySeconds=%u swapGameEyeOrigins=%d",
        helperConfig.enabled ? 1 : 0,
        helperConfig.submitTestFrames,
        helperConfig.waitReadySeconds,
        m_OpenXrSwapGameEyeOrigins ? 1 : 0);
    if (!helperConfig.enabled)
    {
        Game::logMsg("[VR][OpenXRHelper] OpenXR selected but OpenXRHelper=false in VR/config.txt");
        return false;
    }

    m_OpenXrHelperBridgeActive = L4D2VR_StartOpenXrHelper(helperConfig);
    bmvr::SetOpenXrHelperSession(m_OpenXrHelperBridgeActive);
    if (!m_OpenXrHelperBridgeActive)
    {
        Game::logMsg("[VR][OpenXRHelper] helper did not start");
        return false;
    }

    L4D2VROpenXrRuntimeViewConfigDesc runtimeViewConfig{};
    uint32_t runtimeViewConfigGeneration = 0;
    const ULONGLONG runtimeViewStartMs = GetTickCount64();
    const ULONGLONG runtimeViewTimeoutMs = (std::max)(1000u, helperConfig.waitReadySeconds * 1000u);
    while (true)
    {
        const bool haveConfig = L4D2VR_ReadOpenXrRuntimeViewConfig(runtimeViewConfig, &runtimeViewConfigGeneration);
        if (haveConfig && runtimeViewConfig.reserved0 == L4D2VR_OPENXR_RUNTIME_VIEW_FOV_LOCATED)
            break;
        if (!L4D2VR_OpenXrHelperBridgeIsStarted())
        {
            Game::logMsg("[VR][OpenXRHelper] helper exited before runtime views");
            m_OpenXrHelperBridgeActive = false;
            bmvr::SetOpenXrHelperSession(false);
            return false;
        }
        if (GetTickCount64() - runtimeViewStartMs > runtimeViewTimeoutMs)
        {
            Game::logMsg("[VR][OpenXRHelper] runtime view projection was not published in time");
            m_OpenXrHelperBridgeActive = false;
            bmvr::SetOpenXrHelperSession(false);
            return false;
        }
        Sleep(10);
    }

    auto getOpenXrRawProjection = [](const L4D2VROpenXrRuntimeViewDesc& view,
        float& left, float& right, float& top, float& bottom) -> bool
    {
        if (!view.valid || view.width == 0 || view.height == 0)
            return false;
        left = std::tan(view.angleLeft);
        right = std::tan(view.angleRight);
        top = std::tan(view.angleDown);
        bottom = std::tan(view.angleUp);
        return std::isfinite(left) && std::isfinite(right) &&
            std::isfinite(top) && std::isfinite(bottom) &&
            left < -0.001f && right > 0.001f &&
            top < -0.001f && bottom > 0.001f;
    };

    float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
    float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
    if (!getOpenXrRawProjection(runtimeViewConfig.views[L4D2VR_OPENXR_EYE_LEFT], l_left, l_right, l_top, l_bottom) ||
        !getOpenXrRawProjection(runtimeViewConfig.views[L4D2VR_OPENXR_EYE_RIGHT], r_left, r_right, r_top, r_bottom))
    {
        Game::logMsg("[VR][OpenXRHelper] runtime view projection is invalid");
        m_OpenXrHelperBridgeActive = false;
        bmvr::SetOpenXrHelperSession(false);
        return false;
    }

    const float tanHalfFovX = (std::max)({ -l_left, l_right, -r_left, r_right });
    const float tanHalfFovY = (std::max)({ -l_top, l_bottom, -r_top, r_bottom });
    if (!(std::isfinite(tanHalfFovX) && tanHalfFovX > 0.001f &&
        std::isfinite(tanHalfFovY) && tanHalfFovY > 0.001f))
    {
        Game::logMsg("[VR][OpenXRHelper] runtime view FOV is invalid");
        m_OpenXrHelperBridgeActive = false;
        bmvr::SetOpenXrHelperSession(false);
        return false;
    }

    m_RenderWidth = (std::max)(
        runtimeViewConfig.views[L4D2VR_OPENXR_EYE_LEFT].width,
        runtimeViewConfig.views[L4D2VR_OPENXR_EYE_RIGHT].width);
    m_RenderHeight = (std::max)(
        runtimeViewConfig.views[L4D2VR_OPENXR_EYE_LEFT].height,
        runtimeViewConfig.views[L4D2VR_OPENXR_EYE_RIGHT].height);
    bmvr::g_RecommendedEyeWidth = m_RenderWidth;
    bmvr::g_RecommendedEyeHeight = m_RenderHeight;

    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFovX;
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFovX;
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFovY;
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFovY;
    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFovX;
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFovX;
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFovY;
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFovY;

    auto sanitizeTextureBounds = [](vr::VRTextureBounds_t& bounds)
    {
        bounds.uMin = std::clamp(bounds.uMin, 0.0f, 1.0f);
        bounds.uMax = std::clamp(bounds.uMax, 0.0f, 1.0f);
        bounds.vMin = std::clamp(bounds.vMin, 0.0f, 1.0f);
        bounds.vMax = std::clamp(bounds.vMax, 0.0f, 1.0f);
        if (bounds.uMax <= bounds.uMin || bounds.vMax <= bounds.vMin)
            bounds = vr::VRTextureBounds_t{ 0.0f, 0.0f, 1.0f, 1.0f };
    };
    sanitizeTextureBounds(m_TextureBounds[0]);
    sanitizeTextureBounds(m_TextureBounds[1]);

    m_Aspect = tanHalfFovX / tanHalfFovY;
    m_Fov = 2.0f * atanf(tanHalfFovX) * 180.0f / 3.14159265358979323846f;
    m_FovLeft = m_Fov;
    m_FovRight = m_Fov;
    ChooseEyeRenderSize();
    BindOpenXrActionHandles();

    m_IsVREnabled = true;
    Game::logMsg(
        "[VR][OpenXR] helper scene backend ready runtimeViewGen=%u recommendedRT=%ux%u finalRT=%ux%u fov=%.3f aspect=%.6f boundsL=(%.4f %.4f %.4f %.4f) boundsR=(%.4f %.4f %.4f %.4f)",
        runtimeViewConfigGeneration,
        bmvr::g_RecommendedEyeWidth, bmvr::g_RecommendedEyeHeight,
        m_RenderWidth, m_RenderHeight, m_Fov, m_Aspect,
        m_TextureBounds[0].uMin, m_TextureBounds[0].vMin, m_TextureBounds[0].uMax, m_TextureBounds[0].vMax,
        m_TextureBounds[1].uMin, m_TextureBounds[1].vMin, m_TextureBounds[1].uMax, m_TextureBounds[1].vMax);
    Game::logMsg("[VR][OpenXR] SteamVR overlay SS is sampled at helper start (not live). "
        "World pixels need WorldRenderAtEyeSize=true (log worldMatch=1 / grow LITERAL). "
        "worldMatch=0 means the HWND is being upscaled into these eyes.");
    return true;
}

namespace
{
    vr::TrackedDevicePose_t OpenXrPoseToTracked(
        const float* position, const float* orientation, bool valid)
    {
        vr::TrackedDevicePose_t pose{};
        if (!valid)
            return pose;
        float x = orientation[0];
        float y = orientation[1];
        float z = orientation[2];
        float w = orientation[3];
        const float lenSq = x * x + y * y + z * z + w * w;
        if (lenSq > 0.000001f)
        {
            const float invLen = 1.0f / std::sqrt(lenSq);
            x *= invLen; y *= invLen; z *= invLen; w *= invLen;
        }
        else
        {
            x = 0.0f; y = 0.0f; z = 0.0f; w = 1.0f;
        }
        pose.bDeviceIsConnected = true;
        pose.bPoseIsValid = true;
        pose.eTrackingResult = vr::TrackingResult_Running_OK;
        vr::HmdMatrix34_t& mat = pose.mDeviceToAbsoluteTracking;
        mat.m[0][0] = 1.0f - 2.0f * y * y - 2.0f * z * z;
        mat.m[0][1] = 2.0f * x * y - 2.0f * z * w;
        mat.m[0][2] = 2.0f * x * z + 2.0f * y * w;
        mat.m[1][0] = 2.0f * x * y + 2.0f * z * w;
        mat.m[1][1] = 1.0f - 2.0f * x * x - 2.0f * z * z;
        mat.m[1][2] = 2.0f * y * z - 2.0f * x * w;
        mat.m[2][0] = 2.0f * x * z - 2.0f * y * w;
        mat.m[2][1] = 2.0f * y * z + 2.0f * x * w;
        mat.m[2][2] = 1.0f - 2.0f * x * x - 2.0f * y * y;
        mat.m[0][3] = position[0];
        mat.m[1][3] = position[1];
        mat.m[2][3] = position[2];
        return pose;
    }

    // Differences a position sample into a cached velocity. Leaves the previous
    // value untouched when the sample is unusable, so a bad dt cannot blank a
    // good velocity.
    void DiffLinearVelocity(
        const float* position,
        const float* prevPosition,
        double dtSeconds,
        float* velocity)
    {
        if (!position || !prevPosition || !velocity || dtSeconds < 0.0005 || dtSeconds > 0.25)
            return;
        const float dt = static_cast<float>(dtSeconds);
        velocity[0] = (position[0] - prevPosition[0]) / dt;
        velocity[1] = (position[1] - prevPosition[1]) / dt;
        velocity[2] = (position[2] - prevPosition[2]) / dt;
    }

    void ApplyLinearVelocity(vr::TrackedDevicePose_t& pose, const float* velocity)
    {
        if (!pose.bPoseIsValid || !velocity)
            return;
        pose.vVelocity.v[0] = velocity[0];
        pose.vVelocity.v[1] = velocity[1];
        pose.vVelocity.v[2] = velocity[2];
    }

    const L4D2VROpenXrControllerPoseDesc& PickOpenXrHandPose(
        const L4D2VROpenXrControllerPoseDesc& grip,
        const L4D2VROpenXrControllerPoseDesc& aim)
    {
        if (grip.valid && grip.active)
            return grip;
        return aim;
    }

    uint32_t DetectOpenVrControllerFamily(
        vr::IVRSystem* system,
        vr::TrackedDeviceIndex_t left,
        vr::TrackedDeviceIndex_t right)
    {
        if (!system)
            return L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
        char buf[128]{};
        const vr::TrackedDeviceIndex_t indices[2] = { right, left };
        for (vr::TrackedDeviceIndex_t idx : indices)
        {
            if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount)
                continue;
            buf[0] = '\0';
            system->GetStringTrackedDeviceProperty(idx, vr::Prop_ControllerType_String, buf, sizeof(buf));
            const uint32_t family = L4D2VR_ClassifyOpenVrControllerType(buf);
            if (family != L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN)
                return family;
        }
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
    }

    bool FillOpenXrOverlayTextureFromShared(
        L4D2VROpenXrOverlayDesc& overlay,
        const SharedTextureHolder& texture)
    {
        const uint32_t width = texture.m_VulkanData.m_nWidth;
        const uint32_t height = texture.m_VulkanData.m_nHeight;
        const uint32_t sampleCount = texture.m_VulkanData.m_nSampleCount;
        if (!texture.m_SharedHandleValid || texture.m_SharedHandle == 0 ||
            width == 0 || height == 0 || sampleCount != 1)
        {
            return false;
        }

        overlay.texture.valid = 1;
        overlay.texture.width = width;
        overlay.texture.height = height;
        overlay.texture.format = texture.m_VulkanData.m_nFormat;
        overlay.texture.sampleCount = sampleCount;
        overlay.texture.handleType = texture.m_SharedHandleType;
        overlay.texture.queueFamilyIndex = texture.m_VulkanData.m_nQueueFamilyIndex;
        overlay.texture.kmtHandle = texture.m_SharedHandle;
        overlay.texture.image = static_cast<uint64_t>(texture.m_VulkanData.m_nImage);
        overlay.texture.uMin = 0.0f;
        overlay.texture.vMin = 0.0f;
        overlay.texture.uMax = 1.0f;
        overlay.texture.vMax = 1.0f;
        overlay.texture.renderFovXDeg = 90.0f;
        overlay.texture.renderAspect = static_cast<float>(width) / static_cast<float>(height);
        return true;
    }
}

bool VR::ConsumeOpenXrTracking()
{
    if (!m_OpenXrHelperBridgeActive)
        return false;

    L4D2VROpenXrPoseDesc openXrPose{};
    uint32_t generation = 0;
    if (!L4D2VR_ReadOpenXrHmdPose(openXrPose, &generation))
        return false;

    m_OpenXrLastHmdPose = openXrPose;
    m_OpenXrLastHmdPoseGeneration = generation;
    if (openXrPose.reserved0 != 0)
    {
        float ipd = 0.f;
        std::memcpy(&ipd, &openXrPose.reserved0, sizeof(ipd));
        if (ipd >= 0.04f && ipd <= 0.10f)
            m_Ipd = ipd;
    }
    L4D2VR_ReadOpenXrInputState(m_OpenXrLastInputState, &m_OpenXrLastInputStateGeneration);

    const L4D2VROpenXrControllerPoseDesc& physicalLeft = PickOpenXrHandPose(
        m_OpenXrLastInputState.controllerPoses[L4D2VR_OPENXR_HAND_LEFT],
        m_OpenXrLastInputState.controllerAimPoses[L4D2VR_OPENXR_HAND_LEFT]);
    const L4D2VROpenXrControllerPoseDesc& physicalRight = PickOpenXrHandPose(
        m_OpenXrLastInputState.controllerPoses[L4D2VR_OPENXR_HAND_RIGHT],
        m_OpenXrLastInputState.controllerAimPoses[L4D2VR_OPENXR_HAND_RIGHT]);
    const bool leftLive = bmvr::TryCtrlPose()
        && physicalLeft.valid != 0 && physicalLeft.active != 0;
    const bool rightLive = bmvr::TryCtrlPose()
        && physicalRight.valid != 0 && physicalRight.active != 0;
    if (leftLive || rightLive)
    {
        static int s_ctrlPoseArmed;
        if (s_ctrlPoseArmed == 0)
        {
            s_ctrlPoseArmed = 1;
            bmvr::BeginRisky(L"ctrl_pose");
            Game::logMsg("[VR][OpenXRHelper] first controller pose L=%u R=%u posL=(%.3f %.3f %.3f) posR=(%.3f %.3f %.3f)",
                leftLive ? 1u : 0u, rightLive ? 1u : 0u,
                physicalLeft.position[0], physicalLeft.position[1], physicalLeft.position[2],
                physicalRight.position[0], physicalRight.position[1], physicalRight.position[2]);
        }
    }
    vr::TrackedDevicePose_t hmdPose = OpenXrPoseToTracked(
        openXrPose.position, openXrPose.orientation, openXrPose.valid != 0);
    vr::TrackedDevicePose_t leftPose = OpenXrPoseToTracked(
        physicalLeft.position, physicalLeft.orientation, leftLive);
    vr::TrackedDevicePose_t rightPose = OpenXrPoseToTracked(
        physicalRight.position, physicalRight.orientation, rightLive);

    // OpenXR controller poses carry no velocity, so it is differenced from
    // position. This runs several times per rendered frame
    // (WaitPosesForStereoFrame, BeginStereoFramePose, Update); the repeat calls
    // read the same bridge sample, so differencing them produced dt ~= 0 and a
    // zero velocity that stuck for the rest of the frame. Crowbar melee reads
    // that velocity, so swinging silently stopped registering. Only
    // re-difference when the bridge has actually published a new sample, and
    // reuse the last result on the repeat calls.
    // Head and hand samples carry independent generations, so each is
    // differenced against its own previous sample and timestamp. Keying both
    // off one counter would re-difference unchanged hand positions whenever
    // only the head pose advanced, which is what zeroed the swing velocity.
    static struct
    {
        bool haveHmd = false;
        LARGE_INTEGER hmdQpc{};
        uint32_t hmdGeneration = 0;
        float hmd[3]{};
        float velHmd[3]{};

        bool haveLeft = false;
        bool haveRight = false;
        LARGE_INTEGER handQpc{};
        uint32_t inputGeneration = 0;
        float left[3]{};
        float right[3]{};
        float velLeft[3]{};
        float velRight[3]{};
    } s_openXrVel;
    LARGE_INTEGER nowQpc{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&nowQpc);
    QueryPerformanceFrequency(&freq);
    auto elapsedSeconds = [&freq, &nowQpc](const LARGE_INTEGER& then) -> double {
        if (freq.QuadPart <= 0)
            return 0.0;
        return static_cast<double>(nowQpc.QuadPart - then.QuadPart) /
            static_cast<double>(freq.QuadPart);
    };

    if (generation != s_openXrVel.hmdGeneration)
    {
        if (s_openXrVel.haveHmd)
            DiffLinearVelocity(openXrPose.position, s_openXrVel.hmd,
                elapsedSeconds(s_openXrVel.hmdQpc), s_openXrVel.velHmd);
        if (openXrPose.valid)
        {
            s_openXrVel.hmd[0] = openXrPose.position[0];
            s_openXrVel.hmd[1] = openXrPose.position[1];
            s_openXrVel.hmd[2] = openXrPose.position[2];
            s_openXrVel.haveHmd = true;
            s_openXrVel.hmdQpc = nowQpc;
        }
        s_openXrVel.hmdGeneration = generation;
    }

    if (m_OpenXrLastInputStateGeneration != s_openXrVel.inputGeneration)
    {
        const double dt = elapsedSeconds(s_openXrVel.handQpc);
        if (s_openXrVel.haveLeft)
            DiffLinearVelocity(physicalLeft.position, s_openXrVel.left, dt, s_openXrVel.velLeft);
        if (s_openXrVel.haveRight)
            DiffLinearVelocity(physicalRight.position, s_openXrVel.right, dt, s_openXrVel.velRight);
        s_openXrVel.haveLeft = physicalLeft.valid && physicalLeft.active;
        if (s_openXrVel.haveLeft)
        {
            s_openXrVel.left[0] = physicalLeft.position[0];
            s_openXrVel.left[1] = physicalLeft.position[1];
            s_openXrVel.left[2] = physicalLeft.position[2];
        }
        s_openXrVel.haveRight = physicalRight.valid && physicalRight.active;
        if (s_openXrVel.haveRight)
        {
            s_openXrVel.right[0] = physicalRight.position[0];
            s_openXrVel.right[1] = physicalRight.position[1];
            s_openXrVel.right[2] = physicalRight.position[2];
        }
        s_openXrVel.handQpc = nowQpc;
        s_openXrVel.inputGeneration = m_OpenXrLastInputStateGeneration;
    }
    ApplyLinearVelocity(hmdPose, s_openXrVel.velHmd);
    if (s_openXrVel.haveLeft)
        ApplyLinearVelocity(leftPose, s_openXrVel.velLeft);
    if (s_openXrVel.haveRight)
        ApplyLinearVelocity(rightPose, s_openXrVel.velRight);

    if (bmvr::g_LeftHanded)
        std::swap(leftPose, rightPose);

    {
        std::lock_guard<std::mutex> lock(m_PoseMutex);
        m_WaitedPoses[vr::k_unTrackedDeviceIndex_Hmd] = hmdPose;
        for (uint32_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i)
            m_WaitedPoses[i] = {};
        m_WaitedPoses[1] = leftPose;
        m_WaitedPoses[2] = rightPose;
    }
    m_WaitedPoseTick.store(GetTickCount(), std::memory_order_release);
    m_LastPoseWaitError.store(static_cast<int>(vr::VRCompositorError_None), std::memory_order_release);

    static bool s_logged;
    static bool s_loggedHands;
    if (!s_logged && openXrPose.valid)
    {
        s_logged = true;
        Game::logMsg("[VR][OpenXRHelper] consumed HMD pose gen=%u pos=(%.3f %.3f %.3f) L=%u R=%u",
            generation, openXrPose.position[0], openXrPose.position[1], openXrPose.position[2],
            physicalLeft.valid && physicalLeft.active ? 1u : 0u,
            physicalRight.valid && physicalRight.active ? 1u : 0u);
    }
    if (!s_loggedHands && (s_openXrVel.haveLeft || s_openXrVel.haveRight))
    {
        s_loggedHands = true;
        Game::logMsg("[VR][OpenXRHelper] controller poses L=%u R=%u velR=(%.2f %.2f %.2f)",
            s_openXrVel.haveLeft ? 1u : 0u,
            s_openXrVel.haveRight ? 1u : 0u,
            rightPose.vVelocity.v[0], rightPose.vVelocity.v[1], rightPose.vVelocity.v[2]);
    }
    return true;
}

bool VR::ShouldExportOpenXrEyeTexture(TextureID texID, uint32_t sampleCount) const
{
    if (!m_OpenXrHelperBridgeActive || !L4D2VR_OpenXrHelperBridgeIsStarted())
        return false;
    if (sampleCount != 1)
        return false;
    return texID == Texture_LeftEye ||
        texID == Texture_RightEye ||
        texID == Texture_LeftEyeSubmit ||
        texID == Texture_RightEyeSubmit ||
        texID == Texture_HUD ||
        texID == Texture_OpenXrPublish ||
        texID == Texture_McRing;
}

void VR::PublishOpenXrEyeTexture(TextureID texID, const D3D9_TEXTURE_VR_DESC& desc)
{
    if (!ShouldExportOpenXrEyeTexture(texID, desc.SampleCount))
        return;

    const bool isLeft = texID == Texture_LeftEye || texID == Texture_LeftEyeSubmit;
    const bool isRight = texID == Texture_RightEye || texID == Texture_RightEyeSubmit;
    if (!isLeft && !isRight)
        return;
    if (!desc.SharedHandleValid || desc.SharedHandle == 0)
    {
        Game::logMsg("[VR][OpenXRHelper] shared texture export missing texID=%d size=%ux%u",
            static_cast<int>(texID), desc.Width, desc.Height);
        return;
    }

    L4D2VROpenXrSharedTextureDesc shared{};
    shared.valid = 1;
    shared.width = desc.Width;
    shared.height = desc.Height;
    shared.format = static_cast<uint32_t>(desc.Format);
    shared.sampleCount = desc.SampleCount;
    shared.handleType = desc.SharedHandleType;
    shared.queueFamilyIndex = desc.QueueFamilyIndex;
    shared.kmtHandle = desc.SharedHandle;
    shared.image = desc.Image;
    const uint32_t eyeIndex = isLeft ? L4D2VR_OPENXR_EYE_LEFT : L4D2VR_OPENXR_EYE_RIGHT;
    const bool submitTexture = texID == Texture_LeftEyeSubmit || texID == Texture_RightEyeSubmit;
    // OpenXR helper contain-blits the full eye RT (hands/HUD/wheel are in-eye).
    // OpenVR-style UV bounds only crop overlay content away from the HMD image.
    const bool fullEyeBounds = submitTexture || m_OpenXrHelperBridgeActive;
    if (fullEyeBounds)
    {
        shared.uMin = 0.0f;
        shared.vMin = 0.0f;
        shared.uMax = 1.0f;
        shared.vMax = 1.0f;
        if (submitTexture)
        {
            shared.renderFovXDeg = 0.0f;
            shared.renderAspect = 0.0f;
        }
        else
        {
            shared.renderFovXDeg = (std::isfinite(m_Fov) && m_Fov > 1.0f && m_Fov < 179.0f) ? m_Fov : 90.0f;
            shared.renderAspect = (std::isfinite(m_Aspect) && m_Aspect > 0.1f && m_Aspect < 10.0f)
                ? m_Aspect
                : ((desc.Height > 0) ? (static_cast<float>(desc.Width) / static_cast<float>(desc.Height)) : 1.0f);
        }
    }
    else
    {
        shared.uMin = std::clamp(m_TextureBounds[eyeIndex].uMin, 0.0f, 1.0f);
        shared.vMin = std::clamp(m_TextureBounds[eyeIndex].vMin, 0.0f, 1.0f);
        shared.uMax = std::clamp(m_TextureBounds[eyeIndex].uMax, 0.0f, 1.0f);
        shared.vMax = std::clamp(m_TextureBounds[eyeIndex].vMax, 0.0f, 1.0f);
        shared.renderFovXDeg = (std::isfinite(m_Fov) && m_Fov > 1.0f && m_Fov < 179.0f) ? m_Fov : 90.0f;
        shared.renderAspect = (std::isfinite(m_Aspect) && m_Aspect > 0.1f && m_Aspect < 10.0f)
            ? m_Aspect
            : ((desc.Height > 0) ? (static_cast<float>(desc.Width) / static_cast<float>(desc.Height)) : 1.0f);
    }
    if (shared.uMax <= shared.uMin)
    {
        shared.uMin = 0.0f;
        shared.uMax = 1.0f;
    }
    if (shared.vMax <= shared.vMin)
    {
        shared.vMin = 0.0f;
        shared.vMax = 1.0f;
    }

    m_OpenXrSharedEyeTextures[eyeIndex] = shared;
    m_OpenXrSharedEyeTextureReadyMask.fetch_or(1u << eyeIndex, std::memory_order_acq_rel);
    Game::logMsg(
        "[VR][OpenXRHelper][GamePublishTexture] texID=%d eye=%s handle=0x%llX size=%ux%u fmt=%u bounds=(%.4f %.4f %.4f %.4f)",
        static_cast<int>(texID),
        eyeIndex == L4D2VR_OPENXR_EYE_LEFT ? "left" : "right",
        static_cast<unsigned long long>(shared.kmtHandle),
        shared.width, shared.height, shared.format,
        shared.uMin, shared.vMin, shared.uMax, shared.vMax);
}

void VR::PublishOpenXrResolvedEyeTextures(uint32_t frameId, bool panel2d)
{
    if (!m_OpenXrHelperBridgeActive || !L4D2VR_OpenXrHelperBridgeIsStarted() || frameId == 0)
        return;
    if (m_OpenXrLastPublishedSharedTextureFrameId.load(std::memory_order_acquire) == frameId)
        return;
    const uint32_t readyMask = m_OpenXrSharedEyeTextureReadyMask.load(std::memory_order_acquire);
    if ((readyMask & L4D2VR_OPENXR_EYES_READY_MASK) != L4D2VR_OPENXR_EYES_READY_MASK)
        return;
    const L4D2VROpenXrSharedTextureDesc left = m_OpenXrSharedEyeTextures[L4D2VR_OPENXR_EYE_LEFT];
    const L4D2VROpenXrSharedTextureDesc right = m_OpenXrSharedEyeTextures[L4D2VR_OPENXR_EYE_RIGHT];
    if (!left.valid || !right.valid)
        return;
    L4D2VROpenXrSharedTextureDesc publishLeft = left;
    L4D2VROpenXrSharedTextureDesc publishRight = right;
    // Hand the helper the rotating publish copy, not the live engine eye RT.
    // Keep the FOV/aspect the eye export carries: the helper builds the submit
    // projection from renderFovXDeg/renderAspect.
    if (m_OpenXrPublishActive && m_OpenXrPublishReady)
    {
        const L4D2VROpenXrSharedTextureDesc& slotLeft =
            m_OpenXrPublishDesc[L4D2VR_OPENXR_EYE_LEFT][m_OpenXrPublishSlot];
        const L4D2VROpenXrSharedTextureDesc& slotRight =
            m_OpenXrPublishDesc[L4D2VR_OPENXR_EYE_RIGHT][m_OpenXrPublishSlot];
        if (slotLeft.valid && slotRight.valid)
        {
            const float leftFovX = publishLeft.renderFovXDeg;
            const float leftAspect = publishLeft.renderAspect;
            const float rightFovX = publishRight.renderFovXDeg;
            const float rightAspect = publishRight.renderAspect;
            publishLeft = slotLeft;
            publishRight = slotRight;
            publishLeft.renderFovXDeg = leftFovX;
            publishLeft.renderAspect = leftAspect;
            publishRight.renderFovXDeg = rightFovX;
            publishRight.renderAspect = rightAspect;
        }
    }
    // The helper derives the submitted vertical FOV from renderFovXDeg and
    // renderAspect. NormalizeViewSetupForVREye renders each eye at the eye RT's
    // own aspect (logged 3664x3584 -> 1.022), but the export carried m_Aspect
    // latched at texture creation (logged 1.0972), so the submitted frustum was
    // ~7% short vertically and the compositor reprojected through the wrong
    // projection. Derive it from the image actually being submitted.
    auto useImageAspect = [](L4D2VROpenXrSharedTextureDesc& desc) {
        if (desc.height == 0)
            return;
        const float aspect = static_cast<float>(desc.width) / static_cast<float>(desc.height);
        if (std::isfinite(aspect) && aspect > 0.1f && aspect < 10.0f)
            desc.renderAspect = aspect;
    };
    useImageAspect(publishLeft);
    useImageAspect(publishRight);
    // 2D GameUI is a letterboxed blit, not a 90° world frustum. A non-zero
    // renderFovXDeg made the helper use "game-when-full-bounds" FOV on WMR.
    // panel2d is the state at copy time: a deferred slot may be published
    // after the menu opened or closed.
    if (panel2d)
    {
        publishLeft.renderFovXDeg = 0.f;
        publishRight.renderFovXDeg = 0.f;
    }
    // Hands/HUD/wheel are in-eye overlays; helper contain-blits the full RT.
    publishLeft.uMin = 0.0f;
    publishLeft.vMin = 0.0f;
    publishLeft.uMax = 1.0f;
    publishLeft.vMax = 1.0f;
    publishRight.uMin = 0.0f;
    publishRight.vMin = 0.0f;
    publishRight.uMax = 1.0f;
    publishRight.vMax = 1.0f;
    L4D2VR_PublishOpenXrSharedTexturePair(publishLeft, publishRight);
    L4D2VR_PublishOpenXrSharedTextureFrame(frameId);
    m_OpenXrLastPublishedSharedTextureFrameId.store(frameId, std::memory_order_release);
}

void VR::ReleaseOpenXrPublishTextures()
{
    ResetOpenXrDeferredPublish();
    for (uint32_t eye = 0; eye < L4D2VR_OPENXR_EYE_COUNT; ++eye)
    {
        for (uint32_t slot = 0; slot < kOpenXrPublishSlots; ++slot)
        {
            ReleaseT(m_D9OpenXrPublishSurface[eye][slot]);
            ReleaseT(m_D9OpenXrPublishTexture[eye][slot]);
            m_OpenXrPublishDesc[eye][slot] = L4D2VROpenXrSharedTextureDesc{};
        }
    }
    for (uint32_t slot = 0; slot < kOpenXrPublishSlots; ++slot)
        ReleaseT(m_OpenXrPublishQuery[slot]);
    m_OpenXrPublishReady = false;
    m_OpenXrPublishActive = false;
    m_OpenXrPublishWidth = 0;
    m_OpenXrPublishHeight = 0;
    m_OpenXrPublishSlot = 0;
}

void VR::ResetOpenXrDeferredPublish()
{
    // Pending copies are abandoned (their slots are being released or the
    // device is resetting); the helper keeps whatever it last imported.
    m_OpenXrPendingCount = 0;
    for (auto& p : m_OpenXrPending)
        p = OpenXrPendingPublish{};
    m_OpenXrVisibleSlot = kOpenXrNoSlot;
    for (double& t : m_OpenXrSlotFreedMs)
        t = 0.0;
    for (uint32_t& f : m_OpenXrSlotFrameId)
        f = 0;
}

void VR::RefreshOpenXrHelperConsumed(double nowMs)
{
    uint32_t consuming = 0, consumed = 0, count = 0;
    if (!L4D2VR_ReadOpenXrHelperConsumedFrame(consuming, consumed, count))
    {
        m_OpenXrHelperFeedbackLive = false;
        return;
    }
    if (count != m_OpenXrHelperConsumedCount)
    {
        m_OpenXrHelperConsumedCount = count;
        m_OpenXrHelperConsumedFrameId = consumed;
        m_OpenXrHelperConsumedChangedMs = nowMs;
    }
    // Live = the helper has finished at least one blit and did so recently.
    // A helper that stopped consuming (session lost focus, runtime paused)
    // must not pin every slot forever: fall back to the cooling timer so the
    // "latest" descriptor stays fresh for when it resumes.
    m_OpenXrHelperFeedbackLive = count != 0
        && (nowMs - m_OpenXrHelperConsumedChangedMs) < 500.0;
}

uint32_t VR::PickFreeOpenXrPublishSlot(double nowMs) const
{
    // Free = not the helper's visible slot, not pending on the GPU, and (when
    // the helper reports blit progress) not a slot whose frame the helper has
    // not finished blitting. Among free slots take the one that stopped
    // being visible longest ago, and never one still inside the cooling
    // margin.
    uint32_t best = kOpenXrNoSlot;
    double bestFreed = 0.0;
    for (uint32_t slot = 0; slot < kOpenXrPublishSlots; ++slot)
    {
        if (slot == m_OpenXrVisibleSlot)
            continue;
        bool pending = false;
        for (uint32_t i = 0; i < m_OpenXrPendingCount; ++i)
            if (m_OpenXrPending[i].slot == slot)
                pending = true;
        if (pending)
            continue;
        const uint32_t slotFrame = m_OpenXrSlotFrameId[slot];
        if (m_OpenXrHelperFeedbackLive && slotFrame != 0
            && m_OpenXrHelperConsumedFrameId < slotFrame)
            continue;
        const double freed = m_OpenXrSlotFreedMs[slot];
        if (freed != 0.0 && (nowMs - freed) < static_cast<double>(bmvr::g_OpenXrSlotCoolingMs))
            continue;
        if (best == kOpenXrNoSlot || freed < bestFreed)
        {
            best = slot;
            bestFreed = freed;
        }
    }
    return best;
}

void VR::WaitOldestOpenXrPending()
{
    if (m_OpenXrPendingCount == 0)
        return;
    IDirect3DQuery9* q = m_OpenXrPublishQuery[m_OpenXrPending[0].slot];
    const double t0 = QpcNowMs();
    bool done = !q;
    // FlushCommands already handed the copy + event to the CS thread, so a
    // plain poll suffices; no D3DGETDATA_FLUSH (DXVK's implicit-sync flush
    // heuristic). Yield between polls: the CS thread and the GPU need the
    // core more than a spinning render thread does.
    while (!done)
    {
        const HRESULT hr = q->GetData(nullptr, 0, 0);
        if (hr == S_OK || FAILED(hr))
        {
            done = true;
            break;
        }
        if ((QpcNowMs() - t0) > 250.0)
            break;
        SwitchToThread();
    }
    const double waited = QpcNowMs() - t0;
    ++m_OpenXrPaceWaits;
    m_OpenXrPaceWaitMs += waited;
    if (waited > m_OpenXrPaceWaitMaxMs)
        m_OpenXrPaceWaitMaxMs = waited;
    // Publish what completed (the oldest at least; newer completed ones win).
    PollOpenXrDeferredPublish();
    if (!done && m_OpenXrPendingCount != 0)
    {
        // After a timed poll everything may still be in flight. Publish the
        // newest anyway so a stuck EVENT query cannot pin the helper on one frame.
        const OpenXrPendingPublish newest = m_OpenXrPending[m_OpenXrPendingCount - 1];
        m_OpenXrDeferredDropped += m_OpenXrPendingCount - 1;
        m_OpenXrPendingCount = 0;
        PublishOpenXrPair(newest.slot, newest);
        ++m_OpenXrDeferredPublishes;
    }
}

void VR::PublishOpenXrPair(uint32_t slot, const OpenXrPendingPublish& pend)
{
    if (pend.havePose)
        L4D2VR_PublishOpenXrGameRenderPose(pend.pose);
    const uint32_t frameId = m_OpenXrSubmitFrameId.fetch_add(1, std::memory_order_acq_rel);
    m_OpenXrPublishActive = (slot != kOpenXrNoSlot) && m_OpenXrPublishReady;
    if (m_OpenXrPublishActive)
        m_OpenXrPublishSlot = slot;
    PublishOpenXrResolvedEyeTextures(frameId, pend.panel2d);
    ++m_OpenXrPublishes;
    if (slot != kOpenXrNoSlot)
    {
        if (m_OpenXrVisibleSlot != kOpenXrNoSlot && m_OpenXrVisibleSlot != slot)
            m_OpenXrSlotFreedMs[m_OpenXrVisibleSlot] = QpcNowMs();
        m_OpenXrVisibleSlot = slot;
        m_OpenXrSlotFrameId[slot] = frameId;
    }
    if (L4D2VR_OpenXrHelperHasSubmittedFrame() || frameId > 1)
    {
        m_HasSubmittedSceneFrame.store(true, std::memory_order_release);
        ++m_SubmitCount;
        if (!m_LoggedFirstSubmit)
        {
            m_LoggedFirstSubmit = true;
            Game::logMsg("[VR][OpenXRHelper] published shared eye frame %u gameRenderPose=%d panel2d=%d slot=%d deferred=%d",
                frameId, pend.havePose ? 1 : 0, pend.panel2d ? 1 : 0,
                slot == kOpenXrNoSlot ? -1 : static_cast<int>(slot),
                bmvr::g_OpenXrDeferredPublish ? 1 : 0);
        }
    }
}

void VR::PollOpenXrDeferredPublish()
{
    std::lock_guard<std::mutex> lock(m_OpenXrPublishMutex);
    if (m_OpenXrPendingCount == 0)
        return;
    // Same queue, in-order completion: if pending[i] is done, everything
    // older is too. Find the newest completed entry, publish it, and drop
    // the older ones (stale frames the helper never needs to see).
    int newestDone = -1;
    for (uint32_t i = 0; i < m_OpenXrPendingCount; ++i)
    {
        IDirect3DQuery9* q = m_OpenXrPublishQuery[m_OpenXrPending[i].slot];
        if (!q)
        {
            newestDone = static_cast<int>(i);
            continue;
        }
        // No D3DGETDATA_FLUSH: FlushCommands already handed the copy to the
        // CS thread and a flush here would be DXVK's implicit-sync heuristic.
        const HRESULT hr = q->GetData(nullptr, 0, 0);
        if (hr == S_OK || FAILED(hr))
            newestDone = static_cast<int>(i);
        else if (m_OpenXrPending[i].issuedMs > 0.0
            && (QpcNowMs() - m_OpenXrPending[i].issuedMs) > 250.0)
            newestDone = static_cast<int>(i);
        else
            break;
    }
    if (newestDone < 0)
        return;
    const OpenXrPendingPublish done = m_OpenXrPending[newestDone];
    m_OpenXrDeferredDropped += static_cast<uint32_t>(newestDone);
    const uint32_t remaining = m_OpenXrPendingCount - static_cast<uint32_t>(newestDone) - 1;
    for (uint32_t i = 0; i < remaining; ++i)
        m_OpenXrPending[i] = m_OpenXrPending[static_cast<uint32_t>(newestDone) + 1 + i];
    m_OpenXrPendingCount = remaining;
    PublishOpenXrPair(done.slot, done);
    ++m_OpenXrDeferredPublishes;
}

// Setup failure repeats every submit until the eye size changes, so an
// unthrottled log here writes to disk at the publish rate.
void VR::LogOpenXrPublishSetupFailure(const char* stage, uint32_t eye, uint32_t slot, unsigned hr)
{
    static uint32_t s_count = 0;
    static uint32_t s_lastTick = 0;
    const uint32_t now = GetTickCount();
    if (s_count >= 3 && (now - s_lastTick) < 10000)
    {
        ++s_count;
        return;
    }
    s_lastTick = now;
    ++s_count;
    Game::logMsg("OpenXR publish RT %s failed eye=%u slot=%u hr=0x%08X (occurrence %u; "
        "rotating publish disabled, helper reads the live eye RT)",
        stage, eye, slot, hr, s_count);
}

bool VR::EnsureOpenXrPublishTextures(IDirect3DDevice9* device, UINT w, UINT h)
{
    if (m_OpenXrPublishReady && m_OpenXrPublishWidth == w && m_OpenXrPublishHeight == h)
        return true;
    ReleaseOpenXrPublishTextures();
    if (!device || !g_D3DVR9 || w < 64 || h < 64)
        return false;

    for (uint32_t eye = 0; eye < L4D2VR_OPENXR_EYE_COUNT; ++eye)
    {
        for (uint32_t slot = 0; slot < kOpenXrPublishSlots; ++slot)
        {
            IDirect3DTexture9** tex = &m_D9OpenXrPublishTexture[eye][slot];
            IDirect3DSurface9** surf = &m_D9OpenXrPublishSurface[eye][slot];
            // The DXVK fork only requests an exportable shared handle when
            // ShouldExportOpenXrEyeTexture accepts m_CreatingTextureID. Without
            // this the texture is created unshared, GetVRDesc has no handle, and
            // the whole rotating-publish path silently falls back to letting the
            // helper read the live engine eye RT (2026-08-31).
            m_CreatingTextureID = Texture_OpenXrPublish;
            const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr);
            m_CreatingTextureID = Texture_None;
            if (FAILED(hr) || !*tex || FAILED((*tex)->GetSurfaceLevel(0, surf)) || !*surf)
            {
                LogOpenXrPublishSetupFailure("create", eye, slot, (unsigned)hr);
                ReleaseOpenXrPublishTextures();
                return false;
            }
            D3D9_TEXTURE_VR_DESC vrDesc{};
            const HRESULT descHr = g_D3DVR9->GetVRDesc(*surf, &vrDesc);
            if (FAILED(descHr) || !vrDesc.Image
                || !vrDesc.SharedHandleValid || vrDesc.SharedHandle == 0)
            {
                LogOpenXrPublishSetupFailure("GetVRDesc", eye, slot, (unsigned)descHr);
                ReleaseOpenXrPublishTextures();
                return false;
            }
            L4D2VROpenXrSharedTextureDesc& shared = m_OpenXrPublishDesc[eye][slot];
            shared = L4D2VROpenXrSharedTextureDesc{};
            shared.valid = 1;
            shared.width = vrDesc.Width;
            shared.height = vrDesc.Height;
            shared.format = static_cast<uint32_t>(vrDesc.Format);
            shared.sampleCount = vrDesc.SampleCount;
            shared.handleType = vrDesc.SharedHandleType;
            shared.queueFamilyIndex = vrDesc.QueueFamilyIndex;
            shared.kmtHandle = vrDesc.SharedHandle;
            shared.image = vrDesc.Image;
            shared.uMin = 0.0f;
            shared.vMin = 0.0f;
            shared.uMax = 1.0f;
            shared.vMax = 1.0f;
        }
    }

    m_OpenXrPublishWidth = w;
    m_OpenXrPublishHeight = h;
    m_OpenXrPublishSlot = 0;
    m_OpenXrPublishReady = true;
    Game::logMsg("OpenXR publish RTs ready %ux%u slots=%u", w, h, kOpenXrPublishSlots);
    return true;
}

bool VR::PrepareOpenXrEyeSurfacesForRead(const OpenXrPendingPublish& pend)
{
    if (!m_OpenXrHelperBridgeActive || !g_D3DVR9)
        return false;
    // Multicore: the material thread copies finished 1x eyes into publish
    // slots after the pair. Publishing the 2x3 ring at M-2 from Present
    // showed a late band (often still being written) — HMD stutter with
    // solid desktop FPS and world wobble on head motion.
    if (McPipelineEnabled())
        return true;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;
    ResolveMsaaEyesToSubmit(device);
    IDirect3DSurface9* left = m_D9LeftEyeSubmitSurface ? m_D9LeftEyeSubmitSurface : m_D9LeftEyeSurface;
    IDirect3DSurface9* right = m_D9RightEyeSubmitSurface ? m_D9RightEyeSubmitSurface : m_D9RightEyeSurface;
    device->Release();
    if (!left || !right)
        return false;
    return CopyEyesToOpenXrPublishSlot(left, right, pend);
}

bool VR::CopyEyesToOpenXrPublishSlot(IDirect3DSurface9* left, IDirect3DSurface9* right,
    const OpenXrPendingPublish& pend)
{
    if (!g_D3DVR9 || !left || !right)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;

    D3DSURFACE_DESC eyeDesc{};
    if (FAILED(left->GetDesc(&eyeDesc))
        || !EnsureOpenXrPublishTextures(device, eyeDesc.Width, eyeDesc.Height))
    {
        device->Release();
        return false;
    }

    std::lock_guard<std::mutex> lock(m_OpenXrPublishMutex);
    // OpenXrDeferredPublish=false (WMR): publish this pair as soon as *this*
    // copy's EVENT signals. Same cadence as WaitDeviceIdle without
    // vkDeviceWaitIdle (no timeout — Xen freeze). Forcing deferred on
    // multicore ran the CPU ahead of the GPU and made low-fps areas stutter
    // in the HMD (2026-09-08).
    const bool defer = bmvr::g_OpenXrDeferredPublish;
    uint32_t slot = kOpenXrNoSlot;
    if (defer)
    {
        const uint32_t maxPending = std::min<uint32_t>(
            std::max<uint32_t>(bmvr::g_OpenXrMaxPending, 1u), kOpenXrMaxPending);
        if (m_OpenXrPendingCount >= maxPending)
        {
            ++m_OpenXrDeferredThrottles;
            device->Release();
            return false;
        }
        const double nowMs = QpcNowMs();
        RefreshOpenXrHelperConsumed(nowMs);
        slot = PickFreeOpenXrPublishSlot(nowMs);
        if (slot == kOpenXrNoSlot)
        {
            ++m_OpenXrDeferredThrottles;
            if (m_OpenXrHelperFeedbackLive)
                ++m_OpenXrDeferredHelperHolds;
            device->Release();
            return false;
        }
    }
    else
        slot = (m_OpenXrPublishSlot + 1) % kOpenXrPublishSlots;

    IDirect3DSurface9* dstLeft = m_D9OpenXrPublishSurface[L4D2VR_OPENXR_EYE_LEFT][slot];
    IDirect3DSurface9* dstRight = m_D9OpenXrPublishSurface[L4D2VR_OPENXR_EYE_RIGHT][slot];
    auto stretch = [&](IDirect3DSurface9* src, IDirect3DSurface9* dst) {
        if (g_OrigStretchRect)
            return g_OrigStretchRect(device, src, nullptr, dst, nullptr, D3DTEXF_NONE);
        return device->StretchRect(src, nullptr, dst, nullptr, D3DTEXF_NONE);
    };
    auto finishCopy = [&](IDirect3DQuery9* q) {
        if (q)
            q->Issue(D3DISSUE_END);
        g_D3DVR9->FlushCommands();
        static int s_noIdle;
        if (s_noIdle < 4)
        {
            Game::logMsg("OpenXR copy: bounded EVENT wait, no WaitDeviceIdle");
            ++s_noIdle;
        }
    };
    auto pollEvent = [&](IDirect3DQuery9* q, double timeoutMs) {
        if (!q)
            return;
        const double t0 = QpcNowMs();
        while ((QpcNowMs() - t0) <= timeoutMs)
        {
            const HRESULT hr = q->GetData(nullptr, 0, 0);
            if (hr == S_OK || FAILED(hr))
                return;
            SwitchToThread();
        }
    };
    const bool prevReentry = m_CaptureReentry;
    m_CaptureReentry = true;
    const HRESULT hrL = (dstLeft && dstRight) ? stretch(left, dstLeft) : E_FAIL;
    const HRESULT hrR = SUCCEEDED(hrL) ? stretch(right, dstRight) : E_FAIL;
    m_CaptureReentry = prevReentry;
    if (dstLeft && dstRight && SUCCEEDED(hrL) && SUCCEEDED(hrR))
    {
        if (FAILED(g_D3DVR9->TransferSurface(dstLeft, FALSE)) ||
            FAILED(g_D3DVR9->TransferSurface(dstRight, FALSE)))
        {
            device->Release();
            return false;
        }
        IDirect3DQuery9*& q = m_OpenXrPublishQuery[slot];
        if (!q && FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &q)))
            q = nullptr;
        device->Release();
        finishCopy(q);
        if (defer && q && m_OpenXrPendingCount < kOpenXrMaxPending)
        {
            OpenXrPendingPublish& entry = m_OpenXrPending[m_OpenXrPendingCount++];
            entry = pend;
            entry.slot = slot;
            entry.issuedMs = QpcNowMs();
            if (m_OpenXrPendingCount > m_OpenXrDeferredMaxPending)
                m_OpenXrDeferredMaxPending = m_OpenXrPendingCount;
            return true;
        }
        // WaitDeviceIdle used to drain the whole device. Cap this copy's
        // event so a stuck GPU cannot hang the process; 250ms is still
        // several compositor frames in a low-fps Xen scene.
        pollEvent(q, 250.0);
        PublishOpenXrPair(slot, pend);
        return true;
    }

    device->Release();
    if (FAILED(g_D3DVR9->TransferSurface(left, FALSE)) ||
        FAILED(g_D3DVR9->TransferSurface(right, FALSE)))
        return false;
    g_D3DVR9->FlushCommands();
    PublishOpenXrPair(kOpenXrNoSlot, pend);
    return true;
}

bool VR::PublishOpenXrHudOverlay(uint32_t frameId)
{
    if (!m_OpenXrHelperBridgeActive || !L4D2VR_OpenXrHelperBridgeIsStarted())
        return false;
    if (frameId == 0)
        return false;

    SharedTextureHolder hud{};
    {
        std::lock_guard<TextureStateMutex> textureLock(m_TextureMutex);
        hud = m_VKHUD;
    }

    L4D2VROpenXrOverlayDesc overlay{};
    overlay.valid = 1;
    overlay.visible = 1;
    overlay.reserved0 = m_MenuOverlayEpoch;
    overlay.reserved1 = L4D2VR_OPENXR_OVERLAY_FLAG_BLEND_ALPHA;
    if (!FillOpenXrOverlayTextureFromShared(overlay, hud))
        return false;

    float dist = 0.f, width = 0.f, yOff = 0.f;
    GetMenuOverlayMetrics(dist, width, yOff);
    overlay.widthMeters = (std::max)(0.10f, width);
    overlay.heightMeters = overlay.widthMeters *
        (static_cast<float>(hud.m_VulkanData.m_nHeight) /
            static_cast<float>((std::max)(1u, hud.m_VulkanData.m_nWidth)));
    overlay.distanceMeters = (std::max)(0.10f, dist);
    overlay.curvature = 0.0f;
    overlay.offsetMeters[0] = 0.0f;
    overlay.offsetMeters[1] = yOff;
    overlay.offsetMeters[2] = 0.0f;

    L4D2VR_PublishOpenXrOverlay(L4D2VR_OPENXR_OVERLAY_HUD, overlay);
    L4D2VR_PublishOpenXrOverlayFrame(frameId);
    m_OpenXrHudOverlayPublished = true;
    return true;
}

void VR::HideOpenXrHudOverlay()
{
    if (!m_OpenXrHelperBridgeActive || !L4D2VR_OpenXrHelperBridgeIsStarted())
        return;
    if (!m_OpenXrHudOverlayPublished)
        return;
    L4D2VROpenXrOverlayDesc overlay{};
    overlay.valid = 1;
    overlay.visible = 0;
    L4D2VR_PublishOpenXrOverlay(L4D2VR_OPENXR_OVERLAY_HUD, overlay);
    m_OpenXrHudOverlayPublished = false;
    m_HudOverlayHasImage = false;
}

void VR::SetActionManifest()
{
    m_ActionsReady.store(false, std::memory_order_release);
    if (!m_Input)
    {
        Game::logMsg("VRInput() null; motion controllers disabled");
        return;
    }

    char cwd[MAX_PATH]{};
    GetCurrentDirectoryA(MAX_PATH, cwd);

    wchar_t wexe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, wexe, MAX_PATH);
    const std::string exeDir = DirFromModulePath(wexe);

    wchar_t wmod[MAX_PATH]{};
    HMODULE mod = bmvr::DllModule();
    if (mod)
        GetModuleFileNameW(mod, wmod, MAX_PATH);
    const std::string modDir = wmod[0] ? DirFromModulePath(wmod) : std::string();

    std::vector<std::string> dirs;
    dirs.push_back(cwd);
    if (!exeDir.empty())
        dirs.push_back(exeDir);
    if (!modDir.empty())
        dirs.push_back(modDir);

    char path[MAX_PATH]{};
    bool found = false;
    for (const std::string& dir : dirs)
    {
        snprintf(path, sizeof(path), "%s\\VR\\SteamVRActionManifest\\action_manifest.json", dir.c_str());
        if (FileExistsA(path))
        {
            found = true;
            break;
        }
        snprintf(path, sizeof(path), "%s\\..\\VR\\SteamVRActionManifest\\action_manifest.json", dir.c_str());
        if (FileExistsA(path))
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        Game::logMsg("SteamVR action manifest missing (install VR\\SteamVRActionManifest next to bms.exe)");
        return;
    }
    char full[MAX_PATH]{};
    if (GetFullPathNameA(path, MAX_PATH, full, nullptr) && full[0])
        snprintf(path, sizeof(path), "%s", full);

    const vr::EVRInputError err = m_Input->SetActionManifestPath(path);
    Game::logMsg("SetActionManifestPath %s err=%d", path, (int)err);
    if (err != vr::VRInputError_None)
        return;

    auto grab = [this](const char* name, vr::VRActionHandle_t* handle) {
        const vr::EVRInputError e = m_Input->GetActionHandle(name, handle);
        if (e != vr::VRInputError_None)
            Game::logMsg("GetActionHandle %s err=%d", name, (int)e);
    };
    grab("/actions/main/in/Jump", &m_ActionJump);
    grab("/actions/main/in/PrimaryAttack", &m_ActionPrimaryAttack);
    grab("/actions/main/in/SecondaryAttack", &m_ActionSecondaryAttack);
    grab("/actions/main/in/Reload", &m_ActionReload);
    grab("/actions/main/in/Use", &m_ActionUse);
    grab("/actions/main/in/Walk", &m_ActionWalk);
    grab("/actions/main/in/Turn", &m_ActionTurn);
    grab("/actions/main/in/boolean_turnleft", &m_ActionBooleanTurnLeft);
    grab("/actions/main/in/boolean_turnright", &m_ActionBooleanTurnRight);
    grab("/actions/main/in/NextItem", &m_ActionNextItem);
    grab("/actions/main/in/PrevItem", &m_ActionPrevItem);
    grab("/actions/main/in/ResetPosition", &m_ActionResetPosition);
    grab("/actions/main/in/Crouch", &m_ActionCrouch);
    grab("/actions/main/in/CrouchToggle", &m_ActionCrouchToggle);
    grab("/actions/main/in/Flashlight", &m_ActionFlashlight);
    grab("/actions/main/in/Scoreboard", &m_ActionScoreboard);
    grab("/actions/main/in/Pause", &m_ActionPause);
    grab("/actions/main/in/Sprint", &m_ActionSprint);
    grab("/actions/main/in/MenuSelect", &m_ActionMenuSelect);
    grab("/actions/main/in/MenuBack", &m_ActionMenuBack);
    grab("/actions/main/in/MenuUp", &m_ActionMenuUp);
    grab("/actions/main/in/MenuDown", &m_ActionMenuDown);
    grab("/actions/main/in/MenuLeft", &m_ActionMenuLeft);
    grab("/actions/main/in/MenuRight", &m_ActionMenuRight);
    grab("/actions/main/in/WeaponMenu", &m_ActionWeaponMenu);
    grab("/actions/main/in/InventoryQuickSwitch", &m_ActionInventoryQuickSwitch);
    grab("/actions/base/in/skeleton_lefthand", &m_ActionSkeletonLeft);
    grab("/actions/base/in/skeleton_righthand", &m_ActionSkeletonRight);

    m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
    m_Input->GetActionSetHandle("/actions/base", &m_BaseActionSet);
    m_ActiveActionSets[0] = {};
    m_ActiveActionSets[0].ulActionSet = m_ActionSet;
    m_ActiveActionSets[1] = {};
    m_ActiveActionSets[1].ulActionSet = m_BaseActionSet;
    m_ActionsReady.store(true, std::memory_order_release);
    Game::logMsg("SteamVR actions ready (Walk/Turn/Use/Attack). G2 type hpmotioncontroller");
}

bool VR::GetDigitalActionData(vr::VRActionHandle_t handle, vr::InputDigitalActionData_t& out) const
{
    out = {};
    if (m_RuntimeBackend == VrRuntimeBackend::OpenXR)
    {
        const uint32_t actionIndex = static_cast<uint32_t>(handle);
        if (actionIndex == 0 || actionIndex >= L4D2VR_OPENXR_ACTION_COUNT)
            return false;
        const L4D2VROpenXrDigitalActionDesc& action = m_OpenXrLastInputState.digitalActions[actionIndex];
        if (!action.active)
            return false;
        out.bActive = true;
        out.activeOrigin = vr::k_ulInvalidInputValueHandle;
        out.bState = action.state != 0;
        out.bChanged = action.changed != 0;
        out.fUpdateTime = 0.0f;
        return true;
    }
    if (!m_Input || handle == vr::k_ulInvalidActionHandle)
        return false;
    const vr::EVRInputError result = m_Input->GetDigitalActionData(
        handle, &out, sizeof(out), vr::k_ulInvalidInputValueHandle);
    return result == vr::VRInputError_None;
}

bool VR::GetAnalogActionData(vr::VRActionHandle_t handle, vr::InputAnalogActionData_t& out) const
{
    out = {};
    if (m_RuntimeBackend == VrRuntimeBackend::OpenXR)
    {
        const uint32_t actionIndex = static_cast<uint32_t>(handle);
        if (actionIndex == 0 || actionIndex >= L4D2VR_OPENXR_ACTION_COUNT)
            return false;
        const L4D2VROpenXrAnalogActionDesc& action = m_OpenXrLastInputState.analogActions[actionIndex];
        if (!action.active)
            return false;
        out.bActive = true;
        out.activeOrigin = vr::k_ulInvalidInputValueHandle;
        out.x = action.x;
        out.y = action.y;
        out.z = 0.0f;
        out.deltaX = 0.0f;
        out.deltaY = 0.0f;
        out.deltaZ = 0.0f;
        out.fUpdateTime = 0.0f;
        return true;
    }
    if (!m_Input || handle == vr::k_ulInvalidActionHandle)
        return false;
    const vr::EVRInputError result = m_Input->GetAnalogActionData(
        handle, &out, sizeof(out), vr::k_ulInvalidInputValueHandle);
    return result == vr::VRInputError_None;
}

bool VR::PressedDigitalAction(vr::VRActionHandle_t handle, bool onChanged) const
{
    vr::InputDigitalActionData_t data{};
    if (!GetDigitalActionData(handle, data))
        return false;
    if (onChanged)
        return data.bState && data.bChanged;
    return data.bState;
}

void VR::ApplyTurnStick(float stickX, float deltaMs)
{
    float offset = m_RotationOffsetY.load(std::memory_order_relaxed);
    if (bmvr::g_SnapTurning)
    {
        if (!m_PressedTurn && stickX > 0.5f)
        {
            offset -= bmvr::g_SnapTurnAngle;
            m_PressedTurn = true;
            PulseAimHaptic(1800);
        }
        else if (!m_PressedTurn && stickX < -0.5f)
        {
            offset += bmvr::g_SnapTurnAngle;
            m_PressedTurn = true;
            PulseAimHaptic(1800);
        }
        else if (stickX < 0.3f && stickX > -0.3f)
            m_PressedTurn = false;
    }
    else
    {
        const float deadzone = 0.2f;
        const float a = fabsf(stickX);
        if (a > deadzone)
        {
            const float xNormalized = (a - deadzone) / (1.f - deadzone);
            if (stickX > deadzone)
                offset -= bmvr::g_TurnSpeed * deltaMs * xNormalized;
            else
                offset += bmvr::g_TurnSpeed * deltaMs * xNormalized;
        }
        else
            m_PressedTurn = false;
    }
    offset -= 360.f * floorf(offset / 360.f);
    m_RotationOffsetY.store(offset, std::memory_order_release);
}

void VR::ProcessInput()
{
    if (!m_IsVREnabled || !m_ActionsReady.load(std::memory_order_acquire))
        return;
    if (m_RuntimeBackend != VrRuntimeBackend::OpenXR && !m_Input)
        return;

    static auto s_prev = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    float deltaMs = std::chrono::duration<float, std::milli>(now - s_prev).count();
    s_prev = now;
    if (!(deltaMs > 0.f) || deltaMs > 250.f)
        deltaMs = 16.f;

    static bool s_next, s_prevItem, s_pause, s_reset;
    static bool s_atk, s_atk2, s_reload, s_use;
    static bool s_crouchAxis;
    static bool s_fwdHeld;
    static DWORD s_fwdTapMs;
    static bool s_sprintDoubleTap;
    if (IsMenuUp())
        m_MenuLastVisibleMs = GetTickCount();
    if (!m_GameplayEligible)
    {
        m_ProcessInputEnabled = false;
        m_WalkForward.store(0.f, std::memory_order_release);
        m_WalkSide.store(0.f, std::memory_order_release);
        m_HeldButtons.store(0, std::memory_order_release);
        s_next = s_prevItem = s_pause = s_reset = false;
        s_atk = s_atk2 = s_reload = s_use = false;
        s_fwdHeld = false;
        s_fwdTapMs = 0;
        s_sprintDoubleTap = false;
        ClearUseGrab();
        m_CrouchToggled = false;
        m_WeaponMenuOpen = false;
        m_WeaponMenuClickHeld = false;
        m_WeaponMenuOpenedThisHold = false;
        if (m_Game)
        {
            m_Game->m_AnalogForward = 0.f;
            m_Game->m_AnalogSide = 0.f;
        }
        // GameUI / -oldgameui (no background map). Look and walk stay off.
        // Point+click / stick the captured menu in the HMD.
        ApplyMenuNavigation();
        ApplyMenuCursor();
        return;
    }

    m_ProcessInputEnabled = true;

    vr::InputAnalogActionData_t analog{};
    float walkX = 0.f, walkY = 0.f;
    if (GetAnalogActionData(m_ActionWalk, analog))
    {
        walkX = analog.x;
        walkY = analog.y;
    }
    auto dead = [](float v) {
        const float dz = 0.2f;
        const float a = fabsf(v);
        if (a <= dz)
            return 0.f;
        const float t = (a - dz) / (1.f - dz);
        return v < 0.f ? -t : t;
    };
    const float nx = dead(walkX);
    const float ny = dead(walkY);
    m_WalkSide.store(nx, std::memory_order_release);
    m_WalkForward.store(ny, std::memory_order_release);
    if (m_Game)
    {
        m_Game->m_AnalogSide = nx;
        m_Game->m_AnalogForward = ny;
    }

    const bool menuStick = WeaponMenuStickHeld();
    UpdateWeaponMenu(menuStick, deltaMs);

    bool usedAnalogTurn = false;
    float turnY = 0.f;
    bool haveTurnY = false;
    // Snap-turn is tracking-space. Freeze it while GameUI is a room-locked
    // overlay so the panel stays in front (HMD look still works).
    const bool menuUi = Want2dMenuPanel() || WantPauseWorldOverlay();
    if (!menuUi && GetAnalogActionData(m_ActionTurn, analog))
    {
        if (!menuStick)
            ApplyTurnStick(analog.x, deltaMs);
        else
            ApplyTurnStick(0.f, deltaMs);
        usedAnalogTurn = true;
        if (!menuStick)
        {
            turnY = analog.y;
            haveTurnY = true;
        }
    }
    if (!menuUi && !usedAnalogTurn)
    {
        if (!menuStick && PressedDigitalAction(m_ActionBooleanTurnLeft))
            ApplyTurnStick(-1.f, deltaMs);
        else if (!menuStick && PressedDigitalAction(m_ActionBooleanTurnRight))
            ApplyTurnStick(1.f, deltaMs);
        else
            ApplyTurnStick(0.f, deltaMs);
    }
    if (menuUi)
        ApplyTurnStick(0.f, deltaMs);

    const bool paused = SehIsPaused(m_Game->m_EngineClient);

    // Stick-click still sprints. Double-tap forward also latches IN_SPEED
    // until the stick drops back out of the forward keep zone.
    {
        constexpr float kTapOn = 0.65f;
        constexpr float kTapKeep = 0.35f;
        constexpr DWORD kTapMs = 400;
        if (paused || MenuGameplayBlocked())
        {
            s_fwdHeld = false;
            s_fwdTapMs = 0;
            s_sprintDoubleTap = false;
        }
        else
        {
            const bool fwdTap = ny > kTapOn;
            const bool fwdKeep = ny > kTapKeep;
            if (!fwdKeep)
                s_sprintDoubleTap = false;
            else if (fwdTap && !s_fwdHeld)
            {
                const DWORD nowMs = GetTickCount();
                if (s_fwdTapMs != 0 && (nowMs - s_fwdTapMs) <= kTapMs)
                {
                    s_sprintDoubleTap = true;
                    PulseAimHaptic(1000);
                }
                s_fwdTapMs = nowMs;
            }
            s_fwdHeld = fwdTap;
        }
    }

    RefreshActiveWeaponModel();
    if (!paused && !MenuGameplayBlocked() && !m_WeaponMenuOpen && !m_EmptyHands)
        UpdateCrowbarMelee();
    m_WearingHevSuit = m_Game->LocalPlayerHasSuit();
    m_HasHevSuit = !bmvr::g_HideHandsWithoutSuit || m_WearingHevSuit;
    const bool scopedWeapon = ViewmodelIsScoped();
    const bool rpgWeapon = ViewmodelIsRpg();
    if (!scopedWeapon)
        m_CrossbowZoomLatched = false;
    if (!paused && rpgWeapon && PressedDigitalAction(m_ActionSecondaryAttack, true))
        m_RpgLaserLatched = !m_RpgLaserLatched;
    if (!rpgWeapon)
        m_RpgLaserLatched = false;
    // One source of truth. OR-ing netvar, a software latch, and "secondary
    // held" desynced from the engine's own toggle: once any one stayed 1,
    // WorldRenderFov never dropped. Hold-to-zoom is wrong for HL2-style tap
    // zoom (the unscope press would keep FOV narrow while the button is down).
    const int zoomFlag = scopedWeapon ? m_Game->LocalPlayerZoomFlag() : 0;
    if (!paused && scopedWeapon && zoomFlag < 0
        && PressedDigitalAction(m_ActionSecondaryAttack, true))
        m_CrossbowZoomLatched = !m_CrossbowZoomLatched;
    if (zoomFlag >= 0)
        m_CrossbowZoomLatched = (zoomFlag == 1);
    m_ScopeZoomActive = bmvr::g_ScopeUsesHmdAim && scopedWeapon
        && (zoomFlag == 1 || (zoomFlag < 0 && m_CrossbowZoomLatched));
    {
        static int s_zoomLog;
        static bool s_lastZoom = false;
        if (m_ScopeZoomActive != s_lastZoom && s_zoomLog < 16)
        {
            Game::logMsg("Crossbow zoom %s netvar=%d latch=%d fov=%.1f->%.1f",
                m_ScopeZoomActive ? "on" : "off",
                zoomFlag,
                m_CrossbowZoomLatched ? 1 : 0,
                m_Fov, WorldRenderFov());
            s_lastZoom = m_ScopeZoomActive;
            ++s_zoomLog;
        }
    }
    if (paused)
    {
        m_AimCrosshairValid = false;
        m_RpgLaserPointValid = false;
    }
    else
    {
        UpdateAimCrosshair();
        UpdateRpgLaserPoint();
    }
    if (!paused)
    {
        UpdateWeaponFireHaptics();
        if (m_PendingFireHaptic.exchange(0, std::memory_order_acq_rel))
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 2500, 0.85f);
    }

    uint32_t buttons = 0;
    if (paused || MenuGameplayBlocked())
        s_sprintDoubleTap = false;
    if (!paused && !MenuGameplayBlocked())
    {
        const bool primaryHeld = PressedDigitalAction(m_ActionPrimaryAttack);
        const bool emptyHands = m_EmptyHands;
        const bool leftUse = PressedDigitalAction(m_ActionUse);
        if (!m_WeaponMenuOpen && primaryHeld && !emptyHands && !IsCrowbarEquipped())
            buttons |= IN_ATTACK;
        if (!m_WeaponMenuOpen && !emptyHands && PressedDigitalAction(m_ActionSecondaryAttack))
            buttons |= IN_ATTACK2;
        if (PressedDigitalAction(m_ActionJump) || (haveTurnY && turnY > 0.65f))
            buttons |= IN_JUMP;
        // Left trigger is Use. Right trigger is Use only while holstered.
        // Hold to hold a pickup; release pulses a second Use to drop.
        UpdateUseGrab(buttons, leftUse, !m_WeaponMenuOpen && emptyHands && primaryHeld);
        if (PressedDigitalAction(m_ActionReload))
            buttons |= IN_RELOAD;
        if (PressedDigitalAction(m_ActionCrouch))
            buttons |= IN_DUCK;
        if (!haveTurnY || turnY > -0.4f)
        {
            if (PressedDigitalAction(m_ActionCrouchToggle, true))
                m_CrouchToggled = !m_CrouchToggled;
        }
        if (haveTurnY)
        {
            if (turnY < -0.65f)
            {
                if (!s_crouchAxis)
                    m_CrouchToggled = !m_CrouchToggled;
                s_crouchAxis = true;
            }
            else
                s_crouchAxis = false;
        }
        if (m_CrouchToggled)
            buttons |= IN_DUCK;
        if (PressedDigitalAction(m_ActionSprint) || s_sprintDoubleTap)
            buttons |= IN_SPEED;
        if (GetTickCount() < m_MeleeAttackUntilMs)
            buttons |= IN_ATTACK;
        if (ny > 0.5f)
            buttons |= IN_FORWARD;
        else if (ny < -0.5f)
            buttons |= IN_BACK;
        if (nx > 0.5f)
            buttons |= IN_MOVERIGHT;
        else if (nx < -0.5f)
            buttons |= IN_MOVELEFT;
    }
    m_HeldButtons.store(buttons, std::memory_order_release);

    if ((buttons & IN_ATTACK) && !m_FirstAttackLogged)
    {
        m_FirstAttackLogged = true;
        m_FirstAttackPresentTick = m_PresentTick;
        m_FirstAttackSpikeLogs = 0;
        const char* wpn = m_LastViewmodelModel.empty() ? "?" : m_LastViewmodelModel.c_str();
        Game::logMsg("First IN_ATTACK this level weapon=%s bake=%d ox=%.2f oy=%.2f oz=%.2f scale=%.3f presentTick=%u",
            wpn, m_HasViewmodelBake ? 1 : 0, m_ViewmodelBakeOx, m_ViewmodelBakeOy, m_ViewmodelBakeOz,
            bmvr::g_ViewmodelScale, m_FirstAttackPresentTick);
    }

    const bool nextHeld = PressedDigitalAction(m_ActionNextItem);
    const bool prevHeld = PressedDigitalAction(m_ActionPrevItem);
    const bool pauseHeld = PressedDigitalAction(m_ActionPause);
    const bool resetHeld = PressedDigitalAction(m_ActionResetPosition);
    vr::InputDigitalActionData_t flash{};
    static bool s_flashLatched = false;
    static DWORD s_flashImpulseMs = 0;
    static int s_flashReleasePolls = 0;
    if (GetDigitalActionData(m_ActionFlashlight, flash))
    {
        const DWORD nowTick = GetTickCount();
        if (flash.bState)
        {
            s_flashReleasePolls = 0;
            if (!paused && !m_GameUiVisible && !s_flashLatched && (nowTick - s_flashImpulseMs) > 300)
            {
                m_PendingImpulse.store(100, std::memory_order_release);
                s_flashImpulseMs = nowTick;
                s_flashLatched = true;
                Game::logMsg("Flashlight queued impulse 100 (CreateMove only; vanilla ImpulseCommands) handle=%llu",
                    static_cast<unsigned long long>(m_ActionFlashlight));
            }
        }
        else if (++s_flashReleasePolls >= 3)
            s_flashLatched = false;
    }
    if (!paused && !MenuGameplayBlocked() && !menuStick && nextHeld && !s_next)
    {
        m_PendingInvDelta.store(1, std::memory_order_release);
        m_EmptyHands = false;
        Game::logMsg("NextItem queued on CreateMove (weaponselect, not invnext)");
    }
    if (!paused && !MenuGameplayBlocked() && !menuStick && prevHeld && !s_prevItem)
    {
        m_PendingInvDelta.store(-1, std::memory_order_release);
        m_EmptyHands = false;
        Game::logMsg("PrevItem queued on CreateMove (weaponselect, not invprev)");
    }
    RefreshHeldWeaponState();
    if (m_GameplayEligible && pauseHeld && !s_pause)
    {
        Game::logMsg("Pause action edge paused=%d gameUi=%d handle=%llu",
            paused ? 1 : 0, IsMenuUp() ? 1 : 0,
            static_cast<unsigned long long>(m_ActionPause));
        QueueGameUiToggle(paused);
    }
    if (Want2dMenuPanel() || WantPauseWorldOverlay())
    {
        ApplyMenuNavigation();
        ApplyMenuCursor();
    }
    else
        m_MenuTriggerWasDown = false;
    // Recenter is a short tap of the same right-stick click that opens the
    // weapon menu. Do not recenter on press-edge while that click is held.
    if (resetHeld && !s_reset && !menuStick && !m_WeaponMenuOpenedThisHold)
    {
        m_HmdOriginLatched = false;
        m_MenuPanelPoseValid = false;
        ++m_MenuOverlayEpoch;
        if (bmvr::g_RecenterResetsYaw)
            m_RotationOffsetY.store(0.f, std::memory_order_release);
        Game::logMsg("ResetPosition: cleared HMD origin latch%s",
            bmvr::g_RecenterResetsYaw ? " + yaw" : "");
    }

    const bool atk = PressedDigitalAction(m_ActionPrimaryAttack);
    const bool atk2 = PressedDigitalAction(m_ActionSecondaryAttack);
    const bool reload = PressedDigitalAction(m_ActionReload);
    const bool use = PressedDigitalAction(m_ActionUse)
        || (m_EmptyHands && !m_WeaponMenuOpen && atk);
    bool melee = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        melee = m_LastViewmodelModel.find("crowbar") != std::string::npos
            || m_LastViewmodelModel.find("wrench") != std::string::npos;
    }
    if (atk && !s_atk)
    {
        if (!melee && !m_EmptyHands)
            m_WeaponActionAnimUntilMs = GetTickCount() + 450;
    }
    if (atk2 && !s_atk2)
    {
        if (!melee)
            PulseAimHaptic(1600);
        m_WeaponActionAnimUntilMs = GetTickCount() + 450;
    }
    if (reload && !s_reload)
    {
        PulseAimHaptic(1200);
        m_WeaponActionAnimUntilMs = GetTickCount() + 2800;
    }
    if (use && !s_use)
        PulseAimHaptic(900);
    if (nextHeld && !s_next)
        PulseAimHaptic(800);
    if (prevHeld && !s_prevItem)
        PulseAimHaptic(800);
    s_atk = atk;
    s_atk2 = atk2;
    s_reload = reload;
    s_use = use;
    s_next = nextHeld;
    s_prevItem = prevHeld;
    s_pause = pauseHeld;
    s_reset = resetHeld;

    static int s_inLog;
    if (s_inLog < 8 && (fabsf(nx) > 0.1f || fabsf(ny) > 0.1f || buttons != 0))
    {
        Game::logMsg("VR input walk=(%.2f,%.2f) buttons=0x%x turnOff=%.1f",
            ny, nx, buttons, m_RotationOffsetY.load(std::memory_order_relaxed));
        ++s_inLog;
    }

    UpdateViewmodelNumpadAdjust(paused || IsMenuUp());
}

void VR::ApplyVulkanYFlip(vr::VRTextureBounds_t& bounds)
{
    const float tmp = bounds.vMin;
    bounds.vMin = bounds.vMax;
    bounds.vMax = tmp;
}

void VR::RefreshIpdFromHmd()
{
    if (!m_System)
        return;
    const vr::HmdMatrix34_t right = m_System->GetEyeToHeadTransform(vr::Eye_Right);
    const float ipd = fabsf(right.m[0][3]) * 2.0f;
    if (ipd >= 0.04f && ipd <= 0.10f)
        m_Ipd = ipd;
    m_EyeZ = right.m[2][3];
}

bool VR::ResolveSurfaceSize(IDirect3DSurface9* surf, UINT& w, UINT& h, D3DSURFACE_DESC* outDesc)
{
    w = 0;
    h = 0;
    if (!surf)
        return false;
    D3DSURFACE_DESC desc{};
    if (FAILED(surf->GetDesc(&desc)))
        return false;
    if (outDesc)
        *outDesc = desc;
    w = desc.Width;
    h = desc.Height;
    return w >= 2 && h >= 2;
}

UINT VR::KnownWindowWidth() const
{
    UINT w = 0, h = 0;
    if (QueryGameClientSize(w, h) && w >= 640 && w <= 4096)
        return w;
    if (m_VKBackBuffer.m_VulkanData.m_nWidth >= 640 && m_VKBackBuffer.m_VulkanData.m_nWidth <= 4096)
        return m_VKBackBuffer.m_VulkanData.m_nWidth;
    if (m_RenderWidth >= 640 && m_RenderWidth <= 4096)
        return m_RenderWidth;
    return 1920;
}

UINT VR::KnownWindowHeight() const
{
    UINT w = 0, h = 0;
    if (QueryGameClientSize(w, h) && h >= 360 && h <= 4096)
        return h;
    if (m_VKBackBuffer.m_VulkanData.m_nHeight >= 360 && m_VKBackBuffer.m_VulkanData.m_nHeight <= 4096)
        return m_VKBackBuffer.m_VulkanData.m_nHeight;
    if (m_RenderHeight >= 360 && m_RenderHeight <= 4096)
        return m_RenderHeight;
    return 1080;
}

void VR::ChooseEyeRenderSize()
{
    PollSteamVrRecommendedSize();

    if (m_CreatedVRTextures.load(std::memory_order_acquire)
        && m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_VKLeftEye.m_VulkanData.m_nWidth >= 640
        && m_VKLeftEye.m_VulkanData.m_nHeight >= 360)
    {
        const uint32_t recW = bmvr::g_RecommendedEyeWidth;
        const uint32_t recH = bmvr::g_RecommendedEyeHeight;
        const UINT haveW = m_VKLeftEye.m_VulkanData.m_nWidth;
        const UINT haveH = m_VKLeftEye.m_VulkanData.m_nHeight;
        const bool wantSteamVr = bmvr::TrySteamVrEyeRt() && recW >= 640 && recH >= 360
            && (haveW + 32 < recW || haveH + 32 < recH);
        uint32_t offW = 0, offH = 0;
        const bool haveOff = bmvr::ComputeOffscreenEyeSize(offW, offH);
        const bool wantOffscreenResize = haveOff
            && (haveW + 32 < offW || haveH + 32 < offH || offW + 32 < haveW || offH + 32 < haveH);
        if (wantOffscreenResize && m_EyeResizeSettleMs != 0
            && (GetTickCount() - m_EyeResizeSettleMs) < 300)
        {
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            return;
        }
        uint32_t ffW = 0, ffH = 0;
        const bool wantFullFrame = bmvr::TryFullFrameStereo()
            && bmvr::g_FullFrameActualWidth >= 640 && bmvr::g_FullFrameActualHeight >= 360;
        if (wantFullFrame)
        {
            bmvr::FitHmdAspectInWindow(
                bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight,
                m_Aspect, ffW, ffH);
            if (ffW + 32 > haveW || ffH + 32 > haveH)
            {
                // Fall through and recreate eyes at FullFrame-fitted size.
            }
            else if (!wantSteamVr && !wantOffscreenResize)
            {
                m_RenderWidth = haveW;
                m_RenderHeight = haveH;
                return;
            }
        }
        else if (!wantSteamVr && !wantOffscreenResize)
        {
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            return;
        }
    }

    const uint32_t recW = bmvr::g_RecommendedEyeWidth;
    const uint32_t recH = bmvr::g_RecommendedEyeHeight;
    const UINT winWKnown = KnownWindowWidth();
    const UINT winHKnown = KnownWindowHeight();

    uint32_t offW = 0, offH = 0;
    if (bmvr::ComputeOffscreenEyeSize(offW, offH))
    {
        if (m_RenderWidth != offW || m_RenderHeight != offH)
        {
            Game::logMsg(
                "Eye RT %ux%u (offscreen rec=%ux%u RenderScale=%.2f window %ux%u)",
                offW, offH, recW, recH, bmvr::g_RenderScale, winWKnown, winHKnown);
        }
        m_RenderWidth = offW;
        m_RenderHeight = offH;
        return;
    }

    // steamvr_rt: offscreen eyes at GetRecommendedRenderTargetSize(). Verified
    // 2026-08-18: 3296x3216 > 2560x1440 G-buffer, SetRT redirect, skipped BB
    // blit, world black on desktop and HMD (audio + Escape menu still worked).
    // Only retry if the recommended size actually fits the window.
    if (bmvr::TrySteamVrEyeRt() && recW >= 640 && recH >= 360
        && recW <= winWKnown + 32 && recH <= winHKnown + 32)
    {
        float s = bmvr::g_RenderScale;
        if (!(s > 0.24f && s < 4.f))
            s = 1.f;
        if (s > 1.f)
            s = 1.f;
        uint32_t eyeW = (static_cast<uint32_t>(static_cast<float>(recW) * s + 0.5f) + 15u) & ~15u;
        uint32_t eyeH = (static_cast<uint32_t>(static_cast<float>(recH) * s + 0.5f) + 15u) & ~15u;
        if (eyeW < 640)
            eyeW = recW;
        if (eyeH < 360)
            eyeH = recH;
        m_RenderWidth = eyeW;
        m_RenderHeight = eyeH;
        Game::logMsg("Eye RT %ux%u (SteamVR recommended %ux%u RenderScale=%.2f, window %ux%u)",
            eyeW, eyeH, recW, recH, bmvr::g_RenderScale, winWKnown, winHKnown);
        return;
    }

    if (bmvr::TryFullFrameStereo()
        && bmvr::g_FullFrameActualWidth >= 640 && bmvr::g_FullFrameActualHeight >= 360)
    {
        float aspect = m_Aspect;
        if (!(aspect > 0.5f && aspect < 3.f) && recW >= 640 && recH >= 360)
            aspect = static_cast<float>(recW) / static_cast<float>(recH);
        uint32_t eyeW = 0, eyeH = 0;
        const uint32_t recAlignW = (recW + 15u) & ~15u;
        const uint32_t recAlignH = (recH + 15u) & ~15u;
        if (recAlignW >= 640 && recAlignH >= 360
            && recAlignW <= bmvr::g_FullFrameActualWidth
            && recAlignH <= bmvr::g_FullFrameActualHeight)
        {
            eyeW = recAlignW;
            eyeH = recAlignH;
        }
        else
        {
            bmvr::FitHmdAspectInWindow(
                bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight,
                aspect, eyeW, eyeH);
        }
        float s = bmvr::g_RenderScale;
        if (!(s > 0.24f && s < 4.f))
            s = 1.f;
        if (s > 1.001f || s < 0.999f)
        {
            uint32_t scaledW = (static_cast<uint32_t>(static_cast<float>(eyeW) * s + 0.5f) + 15u) & ~15u;
            uint32_t scaledH = (static_cast<uint32_t>(static_cast<float>(eyeH) * s + 0.5f) + 15u) & ~15u;
            if (scaledW > bmvr::g_FullFrameActualWidth || scaledH > bmvr::g_FullFrameActualHeight)
                bmvr::FitHmdAspectInWindow(
                    bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight,
                    aspect, scaledW, scaledH);
            if (scaledW >= 640 && scaledH >= 360)
            {
                eyeW = scaledW;
                eyeH = scaledH;
            }
        }
        if (eyeW >= 640 && eyeH >= 360)
        {
            if (m_RenderWidth != eyeW || m_RenderHeight != eyeH)
            {
                Game::logMsg(
                    "Resolution rec=%ux%u fullframe=%ux%u window=%ux%u eye=%ux%u RenderScale=%.2f ff_stereo=1",
                    recW, recH,
                    bmvr::g_FullFrameActualWidth, bmvr::g_FullFrameActualHeight,
                    winWKnown, winHKnown, eyeW, eyeH, bmvr::g_RenderScale);
                if (eyeW > winWKnown + 32 || eyeH > winHKnown + 32)
                    bmvr::BeginRisky(L"ff_stereo");
            }
            m_RenderWidth = eyeW;
            m_RenderHeight = eyeH;
            return;
        }
    }

    uint32_t fbW = 0, fbH = 0;
    if (bmvr::HaveHmdFramebufferSize(fbW, fbH))
    {
        if (m_RenderWidth != fbW || m_RenderHeight != fbH)
            Game::logMsg("Eye/G-buffer size %ux%u (CreateDevice HMD-aspect, window %ux%u recommended %ux%u aspect=%.3f)",
                fbW, fbH, KnownWindowWidth(), KnownWindowHeight(),
                recW, recH, m_Aspect);
        m_RenderWidth = fbW;
        m_RenderHeight = fbH;
        return;
    }

    uint32_t winW = KnownWindowWidth();
    uint32_t winH = KnownWindowHeight();
    if (winW < 640)
        winW = 1280;
    if (winH < 360)
        winH = 720;
    winW = (winW + 1u) & ~1u;
    winH = (winH + 1u) & ~1u;

    if (bmvr::TryHmdFramebuffer() && recW >= 640 && recH >= 360)
    {
        bmvr::ComputeHmdFramebufferSize(recW, recH, winW, winH, m_Aspect);
        if (bmvr::HaveHmdFramebufferSize(fbW, fbH))
        {
            m_RenderWidth = fbW;
            m_RenderHeight = fbH;
            Game::logMsg("Eye/G-buffer size %ux%u (L4D2VR recommended %ux%u, window %ux%u native=%d aspect=%.3f)",
                fbW, fbH, recW, recH, winW, winH, bmvr::TryHmdNative() ? 1 : 0, m_Aspect);
            return;
        }
    }

    if (m_RenderWidth != winW || m_RenderHeight != winH)
        Game::logMsg("Eye RT size %ux%u (window, recommended %ux%u hmdAspect=%.3f)",
            winW, winH, recW, recH, m_Aspect);
    m_RenderWidth = winW;
    m_RenderHeight = winH;
}

Vector VR::GetViewAngle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    const QAngle& ang = m_StereoFramePoseActive ? m_StereoFrameAngles : m_HmdAngAbs;
    const float offset = m_RotationOffsetY.load(std::memory_order_acquire);
    float pitch = ang.x;
    float yaw = ang.y;
    float roll = ang.z;
    if (m_ScopeZoomActive && m_ZoomSmoothValid)
    {
        pitch = m_ZoomSmoothPitch;
        yaw = m_ZoomSmoothYaw;
        roll = m_ZoomSmoothRoll;
    }
    yaw += offset;
    yaw -= 360.f * floorf((yaw + 180.f) / 360.f);
    // L4D2VR GetViewAngle keeps HMD roll. Zeroing it here while OpenVR
    // Submit uses the full pose makes a head tilt rotate the world.
    return Vector(pitch, yaw, roll);
}

void VR::UpdateScopeZoomSmooth()
{
    QAngle ang{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        ang = m_StereoFramePoseActive ? m_StereoFrameAngles : m_HmdAngAbs;
    }
    const double nowMs = static_cast<double>(GetTickCount64());
    if (!m_ScopeZoomActive)
    {
        m_ZoomSmoothPitch = ang.x;
        m_ZoomSmoothYaw = ang.y;
        m_ZoomSmoothRoll = ang.z;
        m_ZoomSmoothValid = true;
        m_ZoomSmoothMs = nowMs;
        return;
    }
    float dt = 0.f;
    if (m_ZoomSmoothMs > 0.0)
        dt = static_cast<float>((nowMs - m_ZoomSmoothMs) * 0.001);
    m_ZoomSmoothMs = nowMs;
    if (!m_ZoomSmoothValid)
    {
        m_ZoomSmoothPitch = ang.x;
        m_ZoomSmoothYaw = ang.y;
        m_ZoomSmoothRoll = ang.z;
        m_ZoomSmoothValid = true;
        return;
    }
    float tau = bmvr::g_ScopeZoomSmoothSec;
    if (!(tau >= 0.02f && tau <= 0.6f))
        tau = 0.16f;
    if (dt <= 0.f)
        dt = 0.001f;
    if (dt > 0.25f)
        dt = 0.25f;
    const float a = 1.f - expf(-dt / tau);
    auto wrapDelta = [](float from, float to) {
        float d = to - from;
        d -= 360.f * floorf((d + 180.f) / 360.f);
        return d;
    };
    m_ZoomSmoothPitch += a * wrapDelta(m_ZoomSmoothPitch, ang.x);
    m_ZoomSmoothYaw += a * wrapDelta(m_ZoomSmoothYaw, ang.y);
    m_ZoomSmoothRoll += a * wrapDelta(m_ZoomSmoothRoll, ang.z);
    m_ZoomSmoothYaw -= 360.f * floorf((m_ZoomSmoothYaw + 180.f) / 360.f);
}

void VR::GetViewBasis(Vector* forward, Vector* right, Vector* up) const
{
    const Vector va = GetViewAngle();
    QAngle ang(va.x, va.y, va.z);
    QAngle::AngleVectors(ang, forward, right, up);
}

Vector VR::GetViewOriginUnshifted(const Vector& setupOrigin) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    Vector center = setupOrigin;
    if (m_HmdOriginLatched
        && (m_OpenXrHelperBridgeActive || m_HmdPosAbsZero.z > 24.f))
    {
        const Vector& hmdPos = m_StereoFramePoseActive ? m_StereoFrameHmdPosAbs : m_HmdPosAbs;
        Vector delta = hmdPos - m_HmdPosAbsZero;
        const float yaw = m_RotationOffsetY.load(std::memory_order_acquire);
        PivotYaw(delta, yaw);
        center += delta;
    }
    center.z += bmvr::g_HeightOffset;
    return center;
}

Vector VR::GetViewOrigin(const Vector& setupOrigin) const
{
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return GetViewOriginUnshifted(setupOrigin) + (fwd * (-(m_EyeZ * m_VRScale)));
}

Vector VR::GetViewOriginLeft(const Vector& setupOrigin) const
{
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return GetViewOrigin(setupOrigin) - (right * ((m_Ipd * m_IpdScale * m_VRScale) * 0.5f));
}

Vector VR::GetViewOriginRight(const Vector& setupOrigin) const
{
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return GetViewOrigin(setupOrigin) + (right * ((m_Ipd * m_IpdScale * m_VRScale) * 0.5f));
}

Vector VR::ControllerTrackingToWorld(const Vector& setupOrigin, const Vector& trackingPos) const
{
    // Baked viewmodel ox/oy/oz are authored on engine eye + (controller -
    // recenter), not on GetViewOrigin. GetViewOrigin also adds EyeZ along
    // look-forward; parenting the gun to that pushed every weapon backward.
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    const Vector& hmdPos = m_StereoFramePoseActive ? m_StereoFrameHmdPosAbs : m_HmdPosAbs;
    const float yaw = m_RotationOffsetY.load(std::memory_order_acquire);
    if (!m_ControllerPoseValid && !m_StereoFramePoseActive)
        return GetViewOrigin(setupOrigin);

    Vector arm = trackingPos - hmdPos;
    PivotYaw(arm, yaw);
    // Do not snap to the camera when the arm looks long. After a playspace
    // step a stale controller sample vs the current HMD is often >80 HU;
    // parenting to GetViewOrigin made the gloves float onto the face until
    // tracking recovered. Stick turn is independent of this path.
    if (!std::isfinite(arm.x) || !std::isfinite(arm.y) || !std::isfinite(arm.z))
        return GetViewOrigin(setupOrigin);

    if (m_HmdOriginLatched
        && (m_OpenXrHelperBridgeActive || m_HmdPosAbsZero.z > 24.f))
    {
        Vector world = setupOrigin;
        Vector delta = trackingPos - m_HmdPosAbsZero;
        PivotYaw(delta, yaw);
        if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
            return GetViewOrigin(setupOrigin);
        world += delta;
        world.z += bmvr::g_HeightOffset;
        return world;
    }
    return GetViewOrigin(setupOrigin) + arm;
}

Vector VR::GetRightControllerAbsPos(const Vector& eyePosition) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    return ControllerTrackingToWorld(eyePosition, m_RightControllerPosAbs);
}

Vector VR::GetLeftControllerAbsPos(const Vector& eyePosition) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    return ControllerTrackingToWorld(eyePosition, m_LeftControllerPosAbs);
}

bool VR::IsScopedWeaponModel(const char* model)
{
    if (!model || !model[0])
        return false;
    // Requiring the crossbow as well as m_bZooming means a scan that landed on
    // the wrong byte cannot hijack the aim on every other weapon.
    char m[128]{};
    size_t n = 0;
    for (; n + 1 < sizeof(m) && model[n]; ++n)
        m[n] = static_cast<char>(tolower(static_cast<unsigned char>(model[n])));
    m[n] = 0;
    return std::strstr(m, "crossbow") != nullptr;
}

bool VR::IsRpgWeaponModel(const char* model)
{
    if (!model || !model[0])
        return false;
    char m[128]{};
    size_t n = 0;
    for (; n + 1 < sizeof(m) && model[n]; ++n)
        m[n] = static_cast<char>(tolower(static_cast<unsigned char>(model[n])));
    m[n] = 0;
    return std::strstr(m, "rpg") != nullptr || std::strstr(m, "rocket") != nullptr;
}

bool VR::IsGluonWeaponModel(const char* model)
{
    if (!model || !model[0])
        return false;
    char m[128]{};
    size_t n = 0;
    for (; n + 1 < sizeof(m) && model[n]; ++n)
        m[n] = static_cast<char>(tolower(static_cast<unsigned char>(model[n])));
    m[n] = 0;
    return std::strstr(m, "egon") != nullptr || std::strstr(m, "gluon") != nullptr;
}

void VR::RememberFireAim(const QAngle& aim)
{
    m_LastFireAim = aim;
    m_HasLastFireAim = true;
}

bool VR::TryGetFireAim(QAngle& out) const
{
    if (m_HasLastFireAim)
    {
        out = m_LastFireAim;
        return true;
    }
    if (m_ControllerPoseValid)
    {
        out = GetAimAngles();
        return true;
    }
    return false;
}

QAngle VR::GetAimAngles() const
{
    // The one place the firing direction is defined. Both cmd->viewangles and
    // the shot origin projection read this, so they cannot drift apart.
    QAngle aim = GetRightControllerAbsAngle();
    // Trim the shot direction only. The viewmodel keeps its tuned pose, so a
    // grip that consistently lands low is corrected without rotating models.
    aim.x -= bmvr::g_AimPitchOffset;
    if (aim.x > 180.f) aim.x -= 360.f;
    if (aim.x < -180.f) aim.x += 360.f;
    if (aim.x > 89.f) aim.x = 89.f;
    if (aim.x < -89.f) aim.x = -89.f;
    aim.z = 0.f;
    return aim;
}

QAngle VR::GetUseAimAngles() const
{
    QAngle aim = UseFollowsLeftHand() ? GetLeftControllerAbsAngle() : GetRightControllerAbsAngle();
    if (aim.x > 180.f) aim.x -= 360.f;
    if (aim.x < -180.f) aim.x += 360.f;
    if (aim.x > 89.f) aim.x = 89.f;
    if (aim.x < -89.f) aim.x = -89.f;
    aim.z = 0.f;
    return aim;
}

Vector VR::GetRecommendedViewmodelAbsPos(const Vector& eyePosition) const
{
    Vector base = eyePosition;
    Vector aimTracking{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (m_HasStereoBodyOrigin)
            base = m_StereoBodyOrigin;
        else if (m_SetupOrigin.LengthSqr() > 1.f)
            base = m_SetupOrigin;
        aimTracking = m_RightControllerPosAbs;
    }
    Vector p0 = ControllerTrackingToWorld(base, aimTracking);
    float ox = 0.f, oy = 0.f, oz = 0.f, ax = 0.f, ay = 0.f, az = 0.f;
    ResolveWeaponViewmodelPose(ox, oy, oz, ax, ay, az);
    static int s_poseLog;
    if (s_poseLog < 8)
    {
        Game::logMsg("Viewmodel pose p0=(%.1f,%.1f,%.1f) base=(%.1f,%.1f,%.1f) ox,oy,oz=(%.1f,%.1f,%.1f) bake=%d",
            p0.x, p0.y, p0.z, base.x, base.y, base.z, ox, oy, oz, m_HasViewmodelBake ? 1 : 0);
        ++s_poseLog;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    p0 -= m_ViewmodelForward * ox;
    p0 -= m_ViewmodelRight * oy;
    p0 -= m_ViewmodelUp * oz;
    return p0;
}

float VR::ViewmodelVisualScale() const
{
    std::string model;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        model = m_LastViewmodelModel;
    }
    const char* modelName = model.c_str();
    float s = bmvr::g_ViewmodelScale;
    if (std::strstr(modelName, "crowbar") || std::strstr(modelName, "wrench")
        || std::strstr(modelName, "Crowbar") || std::strstr(modelName, "Wrench"))
        s = 1.f;
    if (std::strstr(modelName, "shotgun") || std::strstr(modelName, "spas")
        || std::strstr(modelName, "pump"))
        s = 0.64f;
    if (std::strstr(modelName, "grenade") || std::strstr(modelName, "Grenade")
        || std::strstr(modelName, "frag") || std::strstr(modelName, "Frag"))
        s *= 1.25f;
    if (std::strstr(modelName, "357") || std::strstr(modelName, "python")
        || std::strstr(modelName, "revolver") || std::strstr(modelName, "Revolver"))
        s *= 1.15f;
    if (s < 0.2f)
        s = 0.2f;
    if (s > 1.5f)
        s = 1.5f;
    return s;
}

void VR::ApplyViewmodelVisualScale(Vector& world) const
{
    const float scale = ViewmodelVisualScale();
    if (fabsf(scale - 1.f) <= 0.001f)
        return;
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    const Vector pivot = GetRightControllerAbsPos(body);
    world.x = pivot.x + (world.x - pivot.x) * scale;
    world.y = pivot.y + (world.y - pivot.y) * scale;
    world.z = pivot.z + (world.z - pivot.z) * scale;
}

bool VR::ScaleViewmodelRenderableAttachment(void* renderable, Vector& origin) const
{
    if (!renderable || !m_IsVREnabled || !m_ControllerPoseValid || !IsGameplayEligible())
        return false;
    if (!m_Game)
        return false;
    C_BaseEntity* vm = m_Game->GetViewModelEntity();
    if (!vm)
        return false;
    if (renderable != reinterpret_cast<unsigned char*>(vm) + 4)
        return false;
    ApplyViewmodelVisualScale(origin);
    return true;
}

bool VR::TryGetVrMuzzleWorld(Vector& origin) const
{
    if (!m_IsVREnabled || !m_ControllerPoseValid || !m_Game)
        return false;
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        return false;

    C_BaseEntity* vm = m_Game->GetViewModelEntity();
    QAngle ang{};
    static const char* kNames[] = { "muzzle", "Fire01", "Fire02" };
    if (vm)
    {
        for (const char* name : kNames)
        {
            if (m_Game->GetEntityAttachment(vm, name, origin, ang) && origin.LengthSqr() > 1.f)
                return true;
        }
    }

    origin = GetRecommendedViewmodelAbsPos(body);
    ApplyViewmodelVisualScale(origin);
    const QAngle aim = GetRecommendedViewmodelAbsAngle();
    Vector fwd, right, up;
    QAngle::AngleVectors(aim, &fwd, &right, &up);
    origin += fwd * (8.f * ViewmodelVisualScale());
    return origin.LengthSqr() > 1.f;
}

bool VR::TryGetVrShootOrigin(Vector& origin) const
{
    Vector muzzle{};
    if (!TryGetVrMuzzleWorld(muzzle))
        return false;

    // Bullets fly along cmd->viewangles, which is the controller aim, but they
    // start at the muzzle. The muzzle sits off that axis by the per-weapon
    // viewmodel offset and visual scale, so every shot lands a fixed distance
    // off target. It is worst on the revolver, whose 1.15x scale pushes the
    // muzzle furthest from the controller pivot. Slide the origin onto the aim
    // ray, keeping its distance along the barrel, so the shot goes where the
    // controller points regardless of how the model is posed.
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    const Vector aimOrigin = GetRightControllerAbsPos(body);
    if (aimOrigin.LengthSqr() <= 1.f)
    {
        origin = muzzle;
        return true;
    }
    const QAngle aim = GetAimAngles();
    Vector fwd, right, up;
    QAngle::AngleVectors(aim, &fwd, &right, &up);
    if (VectorNormalize(fwd) <= 0.01f)
    {
        origin = muzzle;
        return true;
    }
    float along = (muzzle - aimOrigin).Dot(fwd);
    if (along < 0.f)
        along = 0.f;
    origin = aimOrigin + fwd * along;
    return true;
}

bool VR::TryGetVrUseOrigin(Vector& origin) const
{
    const bool left = UseFollowsLeftHand();
    if (left && !m_LeftControllerTrackingValid)
        return false;
    if (!left && !m_RightControllerTrackingValid)
        return false;
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    origin = left ? GetLeftControllerAbsPos(body) : GetRightControllerAbsPos(body);
    return origin.LengthSqr() > 1.f;
}

bool VR::TryGetVrGrabTarget(Vector& origin) const
{
    if (!TryGetVrUseOrigin(origin))
        return false;
    Vector fwd{};
    QAngle::AngleVectors(GetUseAimAngles(), &fwd, nullptr, nullptr);
    if (VectorNormalize(fwd) > 0.01f)
        origin = origin + fwd * 18.f;

    // Palm+6 sat inside the player hull and blocked walking. Keep the hold
    // point near the hand but outside a standing hull (~16) plus a small crate.
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() > 1.f)
    {
        Vector d = origin - body;
        const float dist2d = sqrtf(d.x * d.x + d.y * d.y);
        constexpr float kMinBodyXy = 40.f;
        if (dist2d > 0.01f && dist2d < kMinBodyXy)
        {
            const float s = kMinBodyXy / dist2d;
            origin.x = body.x + d.x * s;
            origin.y = body.y + d.y * s;
        }
    }
    return true;
}

static bool UseNameLooksLikeWorldToggle(const char* n)
{
    if (!n || !n[0])
        return false;
    char b[96]{};
    size_t i = 0;
    for (; i + 1 < sizeof(b) && n[i]; ++i)
        b[i] = static_cast<char>(tolower(static_cast<unsigned char>(n[i])));
    b[i] = 0;
    static const char* const kKeys[] = {
        "door", "button", "charger", "recharge", "lever", "valve",
        "func_", "trigger", "momentary"
    };
    for (const char* k : kKeys)
    {
        if (std::strstr(b, k))
            return true;
    }
    return false;
}

static bool UseEntityIsWorldToggle(Game* game)
{
    if (!game)
        return false;
    C_BaseEntity* ent = game->LocalPlayerUseEntity();
    if (!ent)
        return false;
    if (UseNameLooksLikeWorldToggle(game->GetEntityClientClassName(ent)))
        return true;
    if (UseNameLooksLikeWorldToggle(game->GetEntityModelName(ent)))
        return true;
    return false;
}

static bool UseEntityIsHeldProp(Game* game)
{
    if (!game)
        return false;
    C_BaseEntity* ent = game->LocalPlayerUseEntity();
    if (!ent)
        return false;
    return !UseEntityIsWorldToggle(game);
}

void VR::ClearUseGrab()
{
    m_GrabLatched = false;
    m_GrabHandLeft = false;
    m_GrabHoldingPhysics = false;
    m_GrabUseEntityHeld = false;
    m_UseWasHeld = false;
    m_UseDropGapUntilMs = 0.0;
    m_UseDropPulseUntilMs = 0.0;
    m_UseHeldSinceMs = 0.0;
    m_UseDropClassifyUntilMs = 0.0;
    m_UseDropSettleUntilMs = 0.0;
    m_UseDropRetries = 0;
}

void VR::UpdateUseGrab(uint32_t& buttons, bool leftUse, bool rightUse)
{
    const bool useHeld = leftUse || rightUse;
    const double now = QpcNowMs();
    const bool worldToggle = UseEntityIsWorldToggle(m_Game);
    const bool heldProp = UseEntityIsHeldProp(m_Game);
    m_GrabUseEntityHeld = heldProp;
    if (heldProp)
        m_GrabHoldingPhysics = true;

    auto startDropPulse = [&]() {
        if (m_UseDropRetries >= 3)
            return;
        ++m_UseDropRetries;
        m_UseDropClassifyUntilMs = 0.0;
        m_UseDropSettleUntilMs = 0.0;
        m_UseDropGapUntilMs = now + 40.0;
        m_UseDropPulseUntilMs = now + 180.0;
    };

    if (useHeld)
    {
        m_UseDropGapUntilMs = 0.0;
        m_UseDropPulseUntilMs = 0.0;
        m_UseDropClassifyUntilMs = 0.0;
        m_UseDropSettleUntilMs = 0.0;
        if (!m_GrabLatched)
        {
            m_GrabLatched = true;
            m_GrabHandLeft = leftUse;
            m_LastGrabHandLeft = m_GrabHandLeft;
            m_UseHeldSinceMs = now;
            m_UseDropRetries = 0;
            static int s_latchLog;
            if (s_latchLog < 12)
            {
                Game::logMsg("Use grab latched %s", m_GrabHandLeft ? "left" : "right");
                ++s_latchLog;
            }
        }
        else if (m_UseHeldSinceMs > 0.0 && (now - m_UseHeldSinceMs) >= 100.0)
            m_GrabHoldingPhysics = true;
        buttons |= IN_USE;
        m_UseWasHeld = true;
        return;
    }

    if (heldProp && !m_GrabLatched)
    {
        m_GrabLatched = true;
        m_GrabHandLeft = m_LastGrabHandLeft;
        m_GrabHoldingPhysics = true;
        startDropPulse();
    }

    if (m_UseWasHeld && m_GrabLatched)
    {
        m_UseWasHeld = false;
        if (worldToggle)
        {
            ClearUseGrab();
            return;
        }
        m_UseDropClassifyUntilMs = now + 200.0;
    }
    m_UseWasHeld = false;

    if (m_UseDropClassifyUntilMs > 0.0)
    {
        if (worldToggle)
        {
            ClearUseGrab();
            return;
        }
        if (heldProp || m_GrabHoldingPhysics)
            startDropPulse();
        else if (now >= m_UseDropClassifyUntilMs)
        {
            m_UseDropClassifyUntilMs = 0.0;
            if (!heldProp && !m_GrabHoldingPhysics)
                ClearUseGrab();
        }
        else
            return;
    }

    if (m_UseDropPulseUntilMs > 0.0)
    {
        if (now >= m_UseDropGapUntilMs && now < m_UseDropPulseUntilMs)
            buttons |= IN_USE;
        if (now >= m_UseDropPulseUntilMs)
        {
            m_UseDropGapUntilMs = 0.0;
            m_UseDropPulseUntilMs = 0.0;
            m_UseDropSettleUntilMs = now + 220.0;
        }
        return;
    }

    if (m_UseDropSettleUntilMs > 0.0)
    {
        if (heldProp)
        {
            startDropPulse();
            if (m_UseDropPulseUntilMs <= 0.0)
                m_UseDropSettleUntilMs = 0.0;
            return;
        }
        if (now >= m_UseDropSettleUntilMs)
        {
            m_UseDropSettleUntilMs = 0.0;
            if (!heldProp)
                ClearUseGrab();
        }
        return;
    }

    if (m_GrabLatched && (heldProp || m_GrabHoldingPhysics))
        return;
    if (!heldProp && !m_GrabHoldingPhysics)
        ClearUseGrab();
}

bool VR::TryGetVrBeamSegment(Vector& start, Vector& end, Vector* outNormal) const
{
    // Visual effects that still trace from EyePosition/EyeAngles (TAU beam,
    // gluon glow, RPG laser) have to be retargeted onto the same ray the gun
    // fires along, or the glow leaves the eyes and the impact disagrees.
    // Cache per engine frame so every hook in the same shot gets identical
    // start/end/normal.
    const int tick = static_cast<int>(GetTickCount64() & 0x7FFFFFFF);
    if (m_CachedBeamFrame >= 0 && (tick - m_CachedBeamFrame) >= 0 && (tick - m_CachedBeamFrame) < 3)
    {
        start = m_CachedBeamStart;
        end = m_CachedBeamEnd;
        if (outNormal)
            *outNormal = m_CachedBeamNormal;
        return true;
    }

    if (!TryGetVrShootOrigin(start) && !TryGetVrMuzzleWorld(start))
        return false;
    Vector dir{};
    QAngle::AngleVectors(GetAimAngles(), &dir, nullptr, nullptr);
    if (VectorNormalize(dir) <= 0.01f)
        return false;
    // Start a little past the muzzle so the first hit is the world, not the
    // viewmodel the muzzle sits inside.
    start = start + dir * 8.f;
    constexpr float kMaxRangeHu = 16384.f;
    end = start + dir * kMaxRangeHu;
    Vector normal(0.f, 0.f, 0.f);
    if (!m_Game || !m_Game->m_EngineTrace)
    {
        m_CachedBeamStart = start;
        m_CachedBeamEnd = end;
        m_CachedBeamNormal = normal;
        m_CachedBeamFrame = tick;
        if (outNormal)
            *outNormal = normal;
        return true;
    }
    C_BaseEntity* player = m_Game->GetLocalPlayerEntity();
    C_BaseEntity* vm = m_Game->GetViewModelEntity();
    CTraceFilterSkipTwoEntities filter(player, vm, 0);
    Ray_t ray;
    ray.Init(start, end);
    CGameTrace tr{};
    m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
    if (tr.startsolid || tr.fraction <= 0.02f)
    {
        start = start + dir * 24.f;
        end = start + dir * kMaxRangeHu;
        ray.Init(start, end);
        m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
    }
    if (tr.fraction < 1.f && !tr.startsolid)
    {
        end = tr.endpos;
        normal = tr.plane.normal;
    }
    m_CachedBeamStart = start;
    m_CachedBeamEnd = end;
    m_CachedBeamNormal = normal;
    m_CachedBeamFrame = tick;
    if (outNormal)
        *outNormal = normal;
    return true;
}

void VR::QueueWeaponMenuSound(uint32_t bit, int kind, int entityIndex)
{
    if (!bit)
        return;
    m_PendingWeaponSounds.fetch_or(bit, std::memory_order_acq_rel);
    if (bit & kWeaponSoundSelect)
    {
        m_PendingWeaponSoundKind.store(kind, std::memory_order_release);
        m_PendingWeaponSoundEntity.store(entityIndex, std::memory_order_release);
    }
}

void VR::FlushPendingWeaponSounds()
{
    const uint32_t bits = m_PendingWeaponSounds.exchange(0, std::memory_order_acq_rel);
    if (!bits || !m_Game)
        return;
    if (bits & kWeaponSoundHover)
        m_Game->PlayUiSound("common/wpn_moveselect.wav");
    if (bits & kWeaponSoundSelect)
        m_Game->PlayUiSound("common/wpn_hudoff.wav");
}

QAngle VR::GetRightControllerAbsAngle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    return m_RightControllerAngAbs;
}

QAngle VR::GetLeftControllerAbsAngle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    return m_LeftControllerAngAbs;
}

QAngle VR::GetRecommendedViewmodelAbsAngle() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    QAngle result{};
    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);
    return result;
}

bool VR::ComputeHudInset(int fbW, int fbH, int& x, int& y, int& w, int& h) const
{
    // Overlay capture uses this to lay HUD out in the center of bmvrHUD
    // instead of the 2560x1440 texture edges (those were the "screen-edge"
    // elements the overlay transform could not move).
    if (fbW < 640 || fbH < 360)
        return false;
    float hmdFov = m_Fov;
    if (!(hmdFov > 20.f && hmdFov < 170.f))
        hmdFov = 98.5f;
    float hudFov = bmvr::g_HudMaxFov;
    if (!(hudFov >= 30.f && hudFov <= 90.f))
        hudFov = 60.f;
    float ratio = bmvr::g_HudDisplayRatio;
    if (!(ratio >= 0.4f && ratio <= 1.f))
        ratio = 0.82f;
    float s = (hudFov / hmdFov) * ratio;
    if (s > 0.9f)
        s = 0.9f;
    if (s < 0.4f)
        s = 0.4f;
    w = (static_cast<int>(static_cast<float>(fbW) * s) + 1) & ~1;
    h = (static_cast<int>(static_cast<float>(fbH) * s) + 1) & ~1;
    const int h16 = (w * 9) / 16;
    if (h16 >= 360 && h16 < h)
        h = h16;
    if (w < 640)
        w = 640;
    if (h < 360)
        h = 360;
    if (w > fbW)
        w = fbW;
    if (h > fbH)
        h = fbH;
    x = (fbW - w) / 2;
    y = (fbH - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    m_HudInsetX = x;
    m_HudInsetY = y;
    m_HudInsetW = w;
    m_HudInsetH = h;
    return true;
}

void VR::QueueEscapeKey()
{
    QueueVirtualKey(VK_ESCAPE);
    Game::logMsg("Pause queued as WM_ESCAPE (gameui_activate skipped or sticky)");
}

void VR::QueueVirtualKey(int vk)
{
    QueueKeyToHwnd(FindGameHwnd(), vk);
}

void VR::ApplyMenuNavigation()
{
    HWND hwnd = FindGameHwnd();
    if (!hwnd)
        return;

    vr::InputAnalogActionData_t analog{};
    float stickX = 0.f;
    float stickY = 0.f;
    if (GetAnalogActionData(m_ActionWalk, analog))
    {
        stickX = analog.x;
        stickY = analog.y;
    }
    constexpr float kStick = 0.55f;
    const bool analogUp = stickY > kStick;
    const bool analogDown = stickY < -kStick;
    const bool analogLeft = stickX < -kStick;
    const bool analogRight = stickX > kStick;

    const DWORD nowMs = GetTickCount();
    auto dpad = [&](bool down, int vk, bool& held, DWORD& lastMs, const char* name) {
        if (!down)
        {
            held = false;
            return;
        }
        if (!held || nowMs - lastMs >= 250u)
        {
            QueueKeyToHwnd(hwnd, vk);
            lastMs = nowMs;
            static int s_navLog;
            if (s_navLog < 12)
            {
                Game::logMsg("Menu nav %s vk=0x%X", name, vk);
                ++s_navLog;
            }
        }
        held = true;
    };

    static bool s_up, s_down, s_left, s_right, s_ok, s_back;
    static DWORD s_upMs, s_downMs, s_leftMs, s_rightMs;
    dpad(analogUp || PressedDigitalAction(m_ActionMenuUp), VK_UP, s_up, s_upMs, "up");
    dpad(analogDown || PressedDigitalAction(m_ActionMenuDown), VK_DOWN, s_down, s_downMs, "down");
    dpad(analogLeft || PressedDigitalAction(m_ActionMenuLeft), VK_LEFT, s_left, s_leftMs, "left");
    dpad(analogRight || PressedDigitalAction(m_ActionMenuRight), VK_RIGHT, s_right, s_rightMs, "right");

    float turnY = 0.f;
    vr::InputAnalogActionData_t turn{};
    if (GetAnalogActionData(m_ActionTurn, turn))
        turnY = turn.y;
    static DWORD s_wheelMs;
    if (fabsf(turnY) > 0.35f && nowMs - s_wheelMs >= 70u)
    {
        short wheel = (turnY > 0.f) ? WHEEL_DELTA : -WHEEL_DELTA;
        if (fabsf(turnY) > 0.85f)
            wheel = static_cast<short>(wheel * 2);
        const int cx = m_MenuCursorValid ? m_MenuCursorX : 640;
        const int cy = m_MenuCursorValid ? m_MenuCursorY : 360;
        PostMessageA(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, wheel), MAKELPARAM(cx, cy));
        s_wheelMs = nowMs;
        static int s_wheelLog;
        if (s_wheelLog < 8)
        {
            Game::logMsg("Menu wheel stickY=%.2f delta=%d", turnY, (int)wheel);
            ++s_wheelLog;
        }
    }

    const bool select = PressedDigitalAction(m_ActionMenuSelect);
    if (select && !s_ok)
    {
        QueueKeyToHwnd(hwnd, VK_RETURN);
        Game::logMsg("Menu confirm A/MenuSelect");
    }
    s_ok = select;

    const bool back = PressedDigitalAction(m_ActionMenuBack)
        || (!m_GameplayEligible && PressedDigitalAction(m_ActionPause));
    if (back && !s_back)
    {
        QueueKeyToHwnd(hwnd, VK_ESCAPE);
        Game::logMsg("Menu back %s",
            m_GameplayEligible ? "B/MenuBack" : "B/Pause");
    }
    s_back = back;
}

void VR::QueueGameUiToggle(bool currentlyPaused)
{
    (void)currentlyPaused;
    // HL2VR gameui_toggle: hide iff engine GameUI is visible, else activate.
    const bool hide = EngineGameUiVisible() || m_GameUiVisible;
    m_PendingGameUi.store(hide ? 2 : 1, std::memory_order_release);
    Game::logMsg("Pause queued %s for engine thread (gameUiVisible=%d engine=%d)",
        hide ? "gameui_hide" : "gameui_activate",
        m_GameUiVisible ? 1 : 0, EngineGameUiVisible() ? 1 : 0);
}

void VR::FlushPendingQuit()
{
    if (m_PendingQuit.exchange(0, std::memory_order_acq_rel) == 0 || !m_Game)
        return;
    Game::logMsg("SteamVR quit: ClientCmd_Unrestricted(quit)");
    if (!m_Game->ClientCmd_Unrestricted("quit"))
        m_Game->ClientCmd("quit");
}

void VR::FlushPendingGameUi()
{
    FlushPendingQuit();
    const int op = m_PendingGameUi.exchange(0, std::memory_order_acq_rel);
    if (op == 0 || !m_Game)
        return;
    const char* cmd = (op == 2) ? "gameui_hide" : "gameui_activate";
    bool ok = false;
    if (bmvr::TryGameUiActivate())
    {
        bmvr::BeginRisky(L"gameui");
        ok = m_Game->ClientCmd_Unrestricted(cmd);
        bmvr::EndRisky(L"gameui");
        Game::logMsg("Pause ClientCmd_Unrestricted(%s) engine-thread ok=%d (slot 108)",
            cmd, ok ? 1 : 0);
    }
    if (!ok)
    {
        ok = m_Game->ClientCmd(cmd);
        Game::logMsg("Pause ClientCmd(%s) slot 7 fallback ok=%d", cmd, ok ? 1 : 0);
    }
    if (ok)
    {
        m_GameUiVisible = (op == 1);
        if (op == 1)
            m_GameUiActivateMs = GetTickCount();
        else
            m_GameUiActivateMs = 0;
    }
}

void VR::PollVrMenuEvents()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    vr::IVRSystem* sys = m_System ? m_System : vr::VRSystem();
    if (!sys)
        return;
    vr::VREvent_t ev{};
    while (sys->PollNextEvent(&ev, sizeof(ev)))
    {
        if (ev.eventType == vr::VREvent_Quit)
        {
            Game::logMsg("SteamVR VREvent_Quit");
            sys->AcknowledgeQuit_Exiting();
            m_PendingQuit.store(1, std::memory_order_release);
        }
        else if (ev.eventType == vr::VREvent_DashboardActivated && !IsMenuUp())
        {
            // HL2VR: dashboard up while GameUI is down -> gameui_toggle (activate).
            m_PendingGameUi.store(1, std::memory_order_release);
            Game::logMsg("SteamVR dashboard opened GameUI");
        }
    }
}

void VR::NoteEngineVGui(void* engineVgui)
{
    if (engineVgui)
        m_EngineVGuiFromPaint = engineVgui;
}

bool VR::EngineGameUiVisible() const
{
    void* p = nullptr;
    if (m_Game && m_Game->m_EngineVGui)
        p = m_Game->m_EngineVGui;
    if (!p)
        p = m_EngineVGuiFromPaint;
    return SehIsGameUIVisible(p);
}

void VR::SyncGameUiFromEngine()
{
    void* p = nullptr;
    if (m_Game && m_Game->m_EngineVGui)
        p = m_Game->m_EngineVGui;
    if (!p)
        p = m_EngineVGuiFromPaint;
    if (!p)
        return;
    const bool vis = SehIsGameUIVisible(p);
    if (vis)
    {
        if (!m_GameUiVisible)
            Game::logMsg("GameUI visible (engine IsGameUIVisible)");
        m_GameUiVisible = true;
        m_MenuLastVisibleMs = GetTickCount();
        return;
    }
    if (!m_GameUiVisible)
        return;
    const DWORD now = GetTickCount();
    if (m_GameUiActivateMs != 0 && now - m_GameUiActivateMs < 750u)
        return;
    m_GameUiVisible = false;
    m_GameUiActivateMs = 0;
    Game::logMsg("GameUI dismissed (engine hide)");
}

bool VR::IsMenuUp() const
{
    if (m_GameUiVisible)
        return true;
    return EngineGameUiVisible();
}

bool VR::MenuGameplayBlocked() const
{
    if (IsMenuUp())
        return true;
    if (m_MenuLastVisibleMs == 0)
        return false;
    return GetTickCount() - m_MenuLastVisibleMs < 500u;
}

bool VR::PauseUiActive() const
{
    return IsMenuUp();
}

bool VR::WantPauseWorldOverlay() const
{
    if (!m_IsVREnabled || !m_GameplayEligible || !PauseUiActive())
        return false;
    if (!m_Game || !m_Game->m_EngineClient)
        return false;
    if (!m_Game->m_EngineClient->IsInGame())
        return false;
    // BM gameui_activate does not set IVEngineClient::IsPaused (log: Pause
    // action edge paused=0 gameUi=0/1, latch pause3d=0). In-game pause is
    // GameUI after the first gameplay CreateMove, not HL2VR's paused bit.
    return m_SeenGameplay;
}

bool VR::Want2dMenuPanel() const
{
    if (!m_IsVREnabled)
        return false;
    if (!m_GameplayEligible)
        return true;
    if (!PauseUiActive())
        return false;
    // Load / LevelInit GameUI before gameplay has run. In-game pause is overlay.
    return !m_SeenGameplay;
}

bool VR::WantMenuPanelLatch() const
{
    return Want2dMenuPanel() || WantPauseWorldOverlay();
}

void VR::GetMenuOverlayMetrics(float& distM, float& widthM, float& yOffM) const
{
    distM = bmvr::g_MenuForwardMeters;
    widthM = bmvr::g_MenuWidthMeters;
    yOffM = bmvr::g_MenuHeightOffsetMeters;
    if (!(distM > 0.2f && distM < 4.f))
        distM = 0.65f;
    if (!(widthM > 0.4f && widthM < 4.f))
        widthM = 1.05f;
    if (!(yOffM >= -0.5f && yOffM <= 0.5f))
        yOffM = -0.02f;
}

void VR::LatchMenuPanelIfNeeded()
{
    if (!WantMenuPanelLatch())
    {
        if (m_MenuPanelPoseValid)
        {
            m_MenuPanelPoseValid = false;
            Game::logMsg("Menu panel pose unlocked (back to live HMD)");
        }
        return;
    }
    if (m_MenuPanelPoseValid)
        return;
    if (!m_OpenXrLastHmdPose.valid || !m_HmdPoseValid)
        return;
    ++m_MenuOverlayEpoch;
    if (m_MenuOverlayEpoch == 0)
        m_MenuOverlayEpoch = 1;
    m_MenuPanelPose = m_OpenXrLastHmdPose;
    m_MenuPanelPose.reserved0 = m_MenuOverlayEpoch;
    if (Want2dMenuPanel())
        m_MenuPanelPose.reserved1 |= L4D2VR_OPENXR_POSE_FLAG_MONO;
    else
        m_MenuPanelPose.reserved1 &= ~L4D2VR_OPENXR_POSE_FLAG_MONO;
    LevelOpenXrPoseToYaw(m_MenuPanelPose);
    const Vector va = GetViewAngle();
    QAngle::AngleVectors(QAngle(0.f, va.y, 0.f), &m_MenuPanelFwd, &m_MenuPanelRight, &m_MenuPanelUp);
    m_MenuPanelPoseValid = true;
    float dist = 0.f, width = 0.f, yOff = 0.f;
    GetMenuOverlayMetrics(dist, width, yOff);
    Game::logMsg("Menu panel pose latched (level yaw, dist=%.2fm width=%.2fm pause3d=%d seen=%d paused=%d epoch=%u)",
        dist, width, WantPauseWorldOverlay() ? 1 : 0, m_SeenGameplay ? 1 : 0,
        (m_Game && SehIsPaused(m_Game->m_EngineClient)) ? 1 : 0, m_MenuOverlayEpoch);
}

bool VR::IntersectMenuPanelUv(float& u, float& v)
{
    if (!m_OpenXrHelperBridgeActive || !m_MenuPanelPoseValid)
        return false;

    float dist = 0.f, width = 0.f, yOff = 0.f;
    GetMenuOverlayMetrics(dist, width, yOff);
    const float height = width * 9.f / 16.f;

    float fwd[3], right[3], upv[3];
    RotateOpenXrVec(m_MenuPanelPose.orientation, 0.f, 0.f, -1.f, fwd);
    RotateOpenXrVec(m_MenuPanelPose.orientation, 1.f, 0.f, 0.f, right);
    upv[0] = 0.f;
    upv[1] = 1.f;
    upv[2] = 0.f;
    const float* p = m_MenuPanelPose.position;
    const float cx = p[0] + fwd[0] * dist + upv[0] * yOff;
    const float cy = p[1] + fwd[1] * dist + upv[1] * yOff;
    const float cz = p[2] + fwd[2] * dist + upv[2] * yOff;
    const float hw = width * 0.5f;
    const float hh = height * 0.5f;
    const float ul[3] = {
        cx - right[0] * hw + upv[0] * hh,
        cy - right[1] * hw + upv[1] * hh,
        cz - right[2] * hw + upv[2] * hh
    };
    const float ur[3] = {
        cx + right[0] * hw + upv[0] * hh,
        cy + right[1] * hw + upv[1] * hh,
        cz + right[2] * hw + upv[2] * hh
    };
    const float ll[3] = {
        cx - right[0] * hw - upv[0] * hh,
        cy - right[1] * hw - upv[1] * hh,
        cz - right[2] * hw - upv[2] * hh
    };

    auto tryHand = [&](uint32_t hand) -> bool {
        if (hand >= L4D2VR_OPENXR_HAND_COUNT)
            return false;
        const L4D2VROpenXrControllerPoseDesc& aim =
            m_OpenXrLastInputState.controllerAimPoses[hand];
        const L4D2VROpenXrControllerPoseDesc& grip =
            m_OpenXrLastInputState.controllerPoses[hand];
        const L4D2VROpenXrControllerPoseDesc* pose = nullptr;
        if (aim.valid && aim.active)
            pose = &aim;
        else if (grip.valid && grip.active)
            pose = &grip;
        if (!pose)
            return false;
        float dir[3];
        RotateOpenXrVec(pose->orientation, 0.f, 0.f, -1.f, dir);
        return RayHitQuadUv(pose->position, dir, ul, ur, ll, u, v);
    };

    const uint32_t first = (m_MenuPointerHand == 0)
        ? L4D2VR_OPENXR_HAND_LEFT : L4D2VR_OPENXR_HAND_RIGHT;
    const uint32_t second = (first == L4D2VR_OPENXR_HAND_LEFT)
        ? L4D2VR_OPENXR_HAND_RIGHT : L4D2VR_OPENXR_HAND_LEFT;
    if (tryHand(first))
        return true;
    if (tryHand(second))
    {
        m_MenuPointerHand = (second == L4D2VR_OPENXR_HAND_LEFT) ? 0 : 1;
        return true;
    }
    return false;
}

void VR::ApplyMenuCursor()
{
    if (!m_Game || !m_HmdPoseValid)
        return;
    const bool menuMap = !m_GameplayEligible;
    const bool pauseUi = PauseUiActive();
    if (!menuMap && !pauseUi && !m_GameUiVisible)
        return;

    int hudW = static_cast<int>(m_FrameCopyWidth ? m_FrameCopyWidth : KnownWindowWidth());
    int hudH = static_cast<int>(m_FrameCopyHeight ? m_FrameCopyHeight : KnownWindowHeight());
    {
        UINT ow = 0, oh = 0;
        if (HudOverlayPixelSize(ow, oh))
        {
            hudW = static_cast<int>(ow);
            hudH = static_cast<int>(oh);
        }
    }
    if (hudW < 320)
        hudW = 1280;
    if (hudH < 180)
        hudH = 720;

    float panelScale = bmvr::g_MenuPanelScale;
    if (!(panelScale > 0.2f && panelScale <= 1.f))
        panelScale = 0.70f;

    bool haveHit = false;
    float rawX = 0.f;
    float rawY = 0.f;

    // HL2VR HandleMenuInput: ray vs the room-fixed menu quad (same plane the
    // OpenXR helper uses for the both-eyes 2D layer). HWND messages stay on
    // the game thread — do not call VGUI IInput from Present.
    {
        float u = 0.f, v = 0.f;
        if (IntersectMenuPanelUv(u, v))
        {
            rawX = u * static_cast<float>(hudW);
            rawY = v * static_cast<float>(hudH);
            haveHit = true;
        }
    }

    if (!haveHit && m_HudOverlayReady && m_Overlay && m_HUDTopHandle != vr::k_ulOverlayHandleInvalid
        && m_Compositor && m_RightControllerTrackingValid)
    {
        vr::VROverlayIntersectionParams_t params{};
        vr::VROverlayIntersectionResults_t results{};
        params.eOrigin = m_Compositor->GetTrackingSpace();
        params.vSource = {
            m_RightControllerTracking.m[0][3],
            m_RightControllerTracking.m[1][3],
            m_RightControllerTracking.m[2][3]
        };
        params.vDirection = {
            -m_RightControllerTracking.m[0][2],
            -m_RightControllerTracking.m[1][2],
            -m_RightControllerTracking.m[2][2]
        };
        if (m_Overlay->ComputeOverlayIntersection(m_HUDTopHandle, &params, &results)
            == vr::VROverlayError_None)
        {
            hudW = m_HUDTexture ? m_HUDTexture->GetActualWidth() : hudW;
            hudH = m_HUDTexture ? m_HUDTexture->GetActualHeight() : hudH;
            if (hudW < 320)
                hudW = 1280;
            if (hudH < 180)
                hudH = 720;
            float u = results.vUVs.v[0];
            float v = results.vUVs.v[1];
            if (u < 0.f) u = 0.f;
            if (u > 1.f) u = 1.f;
            if (v < 0.f) v = 0.f;
            if (v > 1.f) v = 1.f;
            rawX = u * static_cast<float>(hudW);
            rawY = v * static_cast<float>(hudH);
            haveHit = true;
        }
    }

    // Map the right-controller aim onto the (possibly room-fixed) 2D panel.
    if (!haveHit && m_ControllerPoseValid && m_Fov > 10.f && m_Aspect > 0.2f)
    {
        Vector fwd, right, up;
        if (m_MenuPanelPoseValid)
        {
            fwd = m_MenuPanelFwd;
            right = m_MenuPanelRight;
            up = m_MenuPanelUp;
        }
        else
            GetViewBasis(&fwd, &right, &up);
        Vector aim{};
        {
            std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
            QAngle::AngleVectors(m_PhysicalRightAngAbs, &aim, nullptr, nullptr);
        }
        if (VectorNormalize(aim) > 0.01f)
        {
            const float z = aim.Dot(fwd);
            if (z > 0.12f)
            {
                const float tanHalfX = tanf(m_Fov * 0.5f * 3.14159265f / 180.f);
                const float tanHalfY = (m_Aspect > 0.2f) ? (tanHalfX / m_Aspect) : tanHalfX;
                const float nx = (aim.Dot(right) / z) / tanHalfX;
                const float ny = (aim.Dot(up) / z) / tanHalfY;
                if (fabsf(nx) <= panelScale * 1.04f && fabsf(ny) <= panelScale * 1.04f)
                {
                    rawX = (nx / panelScale * 0.5f + 0.5f) * static_cast<float>(hudW);
                    rawY = (-ny / panelScale * 0.5f + 0.5f) * static_cast<float>(hudH);
                    if (rawX < 0.f) rawX = 0.f;
                    if (rawY < 0.f) rawY = 0.f;
                    if (rawX > static_cast<float>(hudW - 1)) rawX = static_cast<float>(hudW - 1);
                    if (rawY > static_cast<float>(hudH - 1)) rawY = static_cast<float>(hudH - 1);
                    haveHit = true;
                }
            }
        }
    }

    const double nowMs = []() {
        static double s_toMs = 0.0;
        if (s_toMs == 0.0)
        {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            s_toMs = f.QuadPart ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
        }
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) * s_toMs;
    }();
    float dt = 0.016f;
    if (m_MenuCursorSmoothMs > 0.0)
        dt = static_cast<float>((nowMs - m_MenuCursorSmoothMs) * 0.001);
    if (dt < 0.001f)
        dt = 0.001f;
    if (dt > 0.08f)
        dt = 0.08f;
    m_MenuCursorSmoothMs = nowMs;

    if (haveHit)
    {
        if (!m_MenuCursorSmoothValid)
        {
            m_MenuCursorSmoothX = rawX;
            m_MenuCursorSmoothY = rawY;
            m_MenuCursorSmoothValid = true;
        }
        else
        {
            const float dx = rawX - m_MenuCursorSmoothX;
            const float dy = rawY - m_MenuCursorSmoothY;
            const float speed = sqrtf(dx * dx + dy * dy) / dt;
            float tau = bmvr::g_MenuCursorSmoothSec;
            if (!(tau >= 0.02f && tau <= 0.6f))
                tau = 0.18f;
            // Fast flicks cut lag; small tremor stays filtered.
            tau /= (1.f + speed / 2800.f);
            if (tau < 0.04f)
                tau = 0.04f;
            const float a = 1.f - expf(-dt / tau);
            m_MenuCursorSmoothX += a * dx;
            m_MenuCursorSmoothY += a * dy;
        }
        m_MenuCursorValid = true;
    }

    if (!m_MenuCursorValid)
        return;

    int wx = static_cast<int>(m_MenuCursorSmoothX + 0.5f);
    int wy = static_cast<int>(m_MenuCursorSmoothY + 0.5f);
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;
    if (wx >= hudW) wx = hudW - 1;
    if (wy >= hudH) wy = hudH - 1;
    m_MenuCursorX = wx;
    m_MenuCursorY = wy;

    // ProcessInput runs on the DXVK Present thread. VGUI IInput
    // (SetCursorPos / InternalCursorMoved / InternalMousePressed) is not
    // safe there: 2026-09-03 died on the first controller-tracking Update
    // after Menu compositor (~700 Presents, last line controller poses L=1).
    // HWND messages are processed on the game thread. Do not call IInput here.
    HWND hwnd = FindGameHwnd();
    if (hwnd && haveHit)
    {
        static DWORD s_lastMoveMs;
        const DWORD nowTick = GetTickCount();
        if (s_lastMoveMs == 0 || nowTick - s_lastMoveMs >= 8)
        {
            POINT pt{ wx, wy };
            ClientToScreen(hwnd, &pt);
            SetCursorPos(pt.x, pt.y);
            PostMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(wx, wy));
            s_lastMoveMs = nowTick;
            static int s_moveLog;
            if (s_moveLog < 3)
            {
                Game::logMsg("Menu cursor HWND %d,%d menuMap=%d pause=%d smooth=%.2f",
                    wx, wy, menuMap ? 1 : 0, pauseUi ? 1 : 0, bmvr::g_MenuCursorSmoothSec);
                ++s_moveLog;
            }
        }
    }

    const bool click = PressedDigitalAction(m_ActionPrimaryAttack);
    if (click && !m_MenuTriggerWasDown)
    {
        if (hwnd)
        {
            const DWORD nowClick = GetTickCount();
            const int dx = wx - m_MenuClickX;
            const int dy = wy - m_MenuClickY;
            const DWORD dblMs = GetDoubleClickTime();
            const bool dbl = m_MenuClickMs != 0
                && (nowClick - m_MenuClickMs) <= (dblMs ? dblMs : 500u)
                && (dx * dx + dy * dy) <= 36 * 36;
            if (dbl)
            {
                PostMessageA(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(wx, wy));
                PostMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(wx, wy));
                m_MenuClickMs = 0;
                Game::logMsg("VR menu double-click at %d,%d", wx, wy);
            }
            else
            {
                PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(wx, wy));
                PostMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(wx, wy));
                m_MenuClickMs = nowClick;
                m_MenuClickX = wx;
                m_MenuClickY = wy;
            }
        }
        static int s_clickLog;
        if (s_clickLog < 8)
        {
            Game::logMsg("VR menu click at %d,%d menuMap=%d pause=%d",
                wx, wy, menuMap ? 1 : 0, pauseUi ? 1 : 0);
            ++s_clickLog;
        }
    }
    m_MenuTriggerWasDown = click;
}

void VR::DrawMenuCursorOnSurface(IDirect3DDevice9* device, IDirect3DSurface9* surf)
{
    if (!device || !surf || !m_MenuCursorValid)
        return;
    D3DSURFACE_DESC desc{};
    if (FAILED(surf->GetDesc(&desc)) || desc.Width < 64 || desc.Height < 64)
        return;
    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    const int x = m_MenuCursorX;
    const int y = m_MenuCursorY;
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;

    auto fill = [&](int x0, int y0, int x1, int y1, D3DCOLOR c) {
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        if (x1 <= x0 || y1 <= y0)
            return;
        RECT r{ x0, y0, x1, y1 };
        device->ColorFill(surf, &r, c);
    };

    // Classic mouse arrow, hotspot at the tip. Sized for 1440p then scaled
    // down with the panel so it stays readable in the HMD.
    const int body = (std::max)(22, h / 28);
    const D3DCOLOR outline = D3DCOLOR_ARGB(255, 0, 0, 0);
    // HEV orange in Black Mesa; Calhoun blue in Blue Shift.
    const D3DCOLOR fillCol = bmvr::IsBlueShift()
        ? D3DCOLOR_ARGB(255, 64, 168, 255)
        : D3DCOLOR_ARGB(255, 255, 120, 16);
    for (int row = 0; row < body; ++row)
    {
        const int span = (row < body * 3 / 4) ? (row + 1) : (std::max)(3, body - row);
        fill(x - 2, y + row - 1, x + span + 2, y + row + 2, outline);
    }
    for (int row = 0; row < body; ++row)
    {
        const int span = (row < body * 3 / 4) ? (row + 1) : (std::max)(3, body - row);
        fill(x, y + row, x + span, y + row + 1, fillCol);
    }
}

bool VR::TryGetMeleeBladeViewAngles(QAngle& out) const
{
    if (!m_PerformingMelee || !m_MeleeBladeAnglesValid)
        return false;
    out = m_MeleeBladeAngles;
    return true;
}

bool VR::TryGetMeleeTraceOrigin(Vector& origin) const
{
    if (!m_PerformingMelee)
        return false;
    origin = m_MeleeTraceOrigin;
    return origin.LengthSqr() > 1.f;
}

bool VR::TryGetMeleeAim(Vector& origin, QAngle& angles) const
{
    // Visible crowbar strike point. Pitch follows the bar (down to the floor).
    if (!m_PerformingMelee || !m_MeleeBladeAnglesValid)
        return false;
    origin = m_MeleeTraceOrigin;
    if (origin.LengthSqr() <= 1.f)
        return false;
    angles = m_MeleeBladeAngles;
    if (angles.x > 180.f) angles.x -= 360.f;
    if (angles.x < -180.f) angles.x += 360.f;
    if (angles.x > 89.f) angles.x = 89.f;
    if (angles.x < -89.f) angles.x = -89.f;
    angles.z = 0.f;
    return true;
}

void VR::UpdateAimCrosshair()
{
    m_AimCrosshairValid = false;
    if (!bmvr::g_VrCrosshair || !m_GameplayEligible || m_EmptyHands || m_WeaponMenuOpen)
        return;
    // The scope draws its own centred crosshair, and while zoomed the shot no
    // longer follows the controller ray this would trace.
    if (m_ScopeZoomActive)
        return;
    if (!m_Game || !m_Game->m_EngineTrace || !m_Game->m_EngineClient)
        return;
    if (PauseUiActive())
        return;

    // Same origin and direction the bullet uses, so the reticle cannot disagree
    // with where the shot goes.
    Vector origin{};
    if (!TryGetVrShootOrigin(origin))
        return;
    Vector dir{};
    QAngle::AngleVectors(GetAimAngles(), &dir, nullptr, nullptr);
    if (VectorNormalize(dir) <= 0.01f)
        return;

    C_BaseEntity* player = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
    CTraceFilterSkipSelf filter(player, 0);
    constexpr float kMaxRangeHu = 8192.f;
    Ray_t ray;
    ray.Init(origin, origin + dir * kMaxRangeHu);
    CGameTrace tr{};
    m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
    if (tr.fraction >= 1.f)
        return;

    // Pull the reticle a hair off the surface it landed on. The overlay pass
    // draws without depth testing, so this is only to keep it from sinking into
    // geometry the glove pass does depth-test against.
    m_AimCrosshairWorld = tr.endpos - dir * 1.5f;
    m_AimCrosshairValid = true;
}

bool VR::TryGetAimCrosshairWorld(Vector& out) const
{
    if (!m_AimCrosshairValid)
        return false;
    out = m_AimCrosshairWorld;
    return true;
}

void VR::UpdateRpgLaserPoint()
{
    if (!m_GameplayEligible || m_EmptyHands || m_WeaponMenuOpen || !m_Game || PauseUiActive())
    {
        m_RpgLaserPointValid = false;
        return;
    }

    bool on = m_RpgLaserLatched;
    // RefreshActiveWeaponModel ran just before this in ProcessInput, so the
    // classification flag is current; skip the model-name lookup + strstr.
    C_BaseEntity* weap = ViewmodelIsRpg() ? m_Game->GetActiveWeaponEntity() : nullptr;
    if (weap)
    {
        unsigned char laserOn = 0;
        const int onOff = m_Game->RpgLaserOnOffset();
        __try
        {
            laserOn = *reinterpret_cast<unsigned char*>(
                reinterpret_cast<char*>(weap) + onOff);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            laserOn = 0;
        }
        if (laserOn)
            on = true;
        m_RpgLaserActive = on;
    }
    else if (!m_RpgLaserLatched)
        m_RpgLaserActive = false;
    if (!on)
    {
        m_RpgLaserPointValid = false;
        return;
    }

    Vector start{};
    Vector end{};
    if (!TryGetVrBeamSegment(start, end))
    {
        m_RpgLaserPointValid = false;
        return;
    }
    Vector dir = end - start;
    if (VectorNormalize(dir) > 0.01f)
        end = end - dir * 1.5f;
    NoteRpgLaserWorld(end);
}

void VR::NoteRpgLaserWorld(const Vector& world)
{
    m_RpgLaserWorld = world;
    m_RpgLaserPointValid = true;
}

bool VR::TryGetRpgLaserWorld(Vector& out) const
{
    if (m_RpgLaserPointValid)
    {
        out = m_RpgLaserWorld;
        return true;
    }
    if (RpgLaserActive() && m_CachedBeamFrame >= 0)
    {
        out = m_CachedBeamEnd;
        return out.LengthSqr() > 1.f;
    }
    return false;
}

// Wrist-HUD styling: amber ticks around an open centre, each backed by a dark
// outline so it stays readable against bright geometry. Screen-space size, so
// it reads the same whether the target is a wall or the far end of a hangar.
void VR::DrawAimCrosshair(IDirect3DDevice9* device, float sx, float sy, UINT h) const
{
    if (!device)
        return;
    struct Vert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };
    auto quad = [&](float x, float y, float w, float hh, D3DCOLOR c) {
        Vert v[4] = {
            { x, y, 0.f, 1.f, c },
            { x + w, y, 0.f, 1.f, c },
            { x, y + hh, 0.f, 1.f, c },
            { x + w, y + hh, 0.f, 1.f, c }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vert));
    };

    float scale = bmvr::g_VrCrosshairScale;
    if (!(scale > 0.05f) || scale > 8.f)
        scale = 1.f;
    const float px = ((h > 8) ? static_cast<float>(h) : 1440.f) / 1440.f * scale;
    const float thick = (std::max)(1.f, 2.f * px);
    const float len = 7.f * px;
    const float gap = 5.f * px;
    const D3DCOLOR amber = D3DCOLOR_ARGB(230, 255, 176, 0);
    const D3DCOLOR shade = D3DCOLOR_ARGB(150, 0, 0, 0);

    // Outline first, then the amber tick on top of it.
    for (int pass = 0; pass < 2; ++pass)
    {
        const float grow = (pass == 0) ? 1.f : 0.f;
        const D3DCOLOR c = (pass == 0) ? shade : amber;
        const float t = thick + grow * 2.f;
        const float l = len + grow * 2.f;
        quad(sx - gap - l, sy - t * 0.5f, l, t, c);          // left
        quad(sx + gap, sy - t * 0.5f, l, t, c);              // right
        quad(sx - t * 0.5f, sy - gap - l, t, l, c);          // up
        quad(sx - t * 0.5f, sy + gap, t, l, c);              // down
        quad(sx - t * 0.5f, sy - t * 0.5f, t, t, c);         // centre pip
    }
}

bool VR::WantsWeaponActionAnim() const
{
    // Manual VR crowbar swings must not play ACT_VM_HITCENTER on top of the
    // controller motion (implementation-plan §9).
    if (m_PerformingMelee)
        return false;
    if (GetTickCount() < m_WeaponActionAnimUntilMs)
        return true;
    const uint32_t buttons = m_HeldButtons.load(std::memory_order_acquire);
    return (buttons & (IN_ATTACK | IN_ATTACK2 | IN_RELOAD)) != 0
        && GetTickCount() >= m_MeleeAttackUntilMs;
}

void VR::UpdateControllerTracking(const vr::TrackedDevicePose_t& hmdPose)
{
    m_ControllerPoseValid = false;
    m_LeftControllerTrackingValid = false;
    m_PhysicalRightTrackingValid = false;
    m_RightControllerTrackingValid = false;
    if (!bmvr::TryCtrlPose())
        return;
    if (!m_OpenXrHelperBridgeActive && !m_System)
        return;

    struct Sample
    {
        bool valid = false;
        vr::TrackedDeviceIndex_t idx = vr::k_unTrackedDeviceIndexInvalid;
        vr::TrackedDevicePose_t pose{};
        Vector pos{};
        QAngle ang{};
        Vector fwd{};
        Vector right{};
        Vector up{};
    };
    uint32_t family = L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
    float tilt = bmvr::g_ControllerPitchTilt;
    auto sampleTrackedPose = [&](const vr::TrackedDevicePose_t& pose, vr::TrackedDeviceIndex_t idx, Sample& out) {
        out.valid = false;
        out.idx = vr::k_unTrackedDeviceIndexInvalid;
        if (!pose.bPoseIsValid || !pose.bDeviceIsConnected)
            return;
        Vector pos = HmdMatrixToSourcePos(pose.mDeviceToAbsoluteTracking, m_VRScale);
        QAngle ang = HmdMatrixToSourceAnglesWithRoll(pose.mDeviceToAbsoluteTracking);
        if (!std::isfinite(ang.x) || !std::isfinite(ang.y) || !std::isfinite(ang.z))
            return;
        ang.y = WrapYaw(ang.y + m_RotationOffsetY.load(std::memory_order_acquire));
        Vector fwd, right, up;
        QAngle::AngleVectors(ang, &fwd, &right, &up);
        if (tilt != 0.f)
        {
            fwd = VectorRotate(fwd, right, tilt);
            up = VectorRotate(up, right, tilt);
        }
        QAngle ctrlAng{};
        QAngle::VectorAngles(fwd, up, ctrlAng);
        ctrlAng.y = WrapYaw(ctrlAng.y);
        out.valid = true;
        out.idx = idx;
        out.pose = pose;
        out.pos = pos;
        out.ang = ctrlAng;
        out.fwd = fwd;
        out.right = right;
        out.up = up;
    };
    auto sampleIndex = [&](vr::TrackedDeviceIndex_t idx, Sample& out) {
        out.valid = false;
        out.idx = vr::k_unTrackedDeviceIndexInvalid;
        if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount)
            return;
        vr::TrackedDevicePose_t pose{};
        {
            std::lock_guard<std::mutex> lock(m_PoseMutex);
            pose = m_WaitedPoses[idx];
        }
        sampleTrackedPose(pose, idx, out);
    };
    auto sampleOpenXrDesc = [&](const L4D2VROpenXrControllerPoseDesc& desc, vr::TrackedDeviceIndex_t idx, Sample& out) {
        out.valid = false;
        out.idx = vr::k_unTrackedDeviceIndexInvalid;
        if (!(desc.valid && desc.active))
            return;
        sampleTrackedPose(
            OpenXrPoseToTracked(desc.position, desc.orientation, true),
            idx,
            out);
    };
    auto sampleRole = [&](vr::ETrackedControllerRole role, Sample& out) {
        sampleIndex(m_System->GetTrackedDeviceIndexForControllerRole(role), out);
    };
    auto copyOrientation = [](const Sample& src, Sample& dst) {
        if (!src.valid || !dst.valid)
            return;
        dst.ang = src.ang;
        dst.fwd = src.fwd;
        dst.right = src.right;
        dst.up = src.up;
    };
    auto rollAroundForward = [](Sample& s, float deg) {
        if (!s.valid || deg == 0.f)
            return;
        s.up = VectorRotate(s.up, s.fwd, deg);
        s.right = VectorRotate(s.right, s.fwd, deg);
        QAngle ctrlAng{};
        QAngle::VectorAngles(s.fwd, s.up, ctrlAng);
        ctrlAng.y = WrapYaw(ctrlAng.y);
        Vector fwd, right, up;
        QAngle::AngleVectors(ctrlAng, &fwd, &right, &up);
        s.ang = ctrlAng;
        s.fwd = fwd;
        s.right = right;
        s.up = up;
    };

    Sample physLeft{};
    Sample physRight{};
    Sample weaponLeft{};
    Sample weaponRight{};
    if (m_OpenXrHelperBridgeActive)
    {
        family = m_OpenXrLastInputState.reserved0;
        tilt = bmvr::EffectiveControllerPitchTilt(family);
        Sample gripLeft{};
        Sample gripRight{};
        Sample aimLeft{};
        Sample aimRight{};
        sampleOpenXrDesc(
            m_OpenXrLastInputState.controllerPoses[L4D2VR_OPENXR_HAND_LEFT], 1, gripLeft);
        sampleOpenXrDesc(
            m_OpenXrLastInputState.controllerPoses[L4D2VR_OPENXR_HAND_RIGHT], 2, gripRight);
        sampleOpenXrDesc(
            m_OpenXrLastInputState.controllerAimPoses[L4D2VR_OPENXR_HAND_LEFT], 1, aimLeft);
        sampleOpenXrDesc(
            m_OpenXrLastInputState.controllerAimPoses[L4D2VR_OPENXR_HAND_RIGHT], 2, aimRight);
        physLeft = gripLeft.valid ? gripLeft : aimLeft;
        physRight = gripRight.valid ? gripRight : aimRight;
        weaponLeft = physLeft;
        weaponRight = physRight;
        if (L4D2VR_ControllerFamilyPrefersAimPose(family))
        {
            // Touch / Index grip -Z follows the handle. Point with the aim
            // pose; keep the palm at the grip origin. Weapons use the full
            // aim pose (Index user-verified). Index aim +Y is the controller
            // face, ~90° from the palm — roll only the hands inward.
            if (aimLeft.valid)
            {
                weaponLeft = aimLeft;
                copyOrientation(aimLeft, physLeft);
            }
            if (aimRight.valid)
            {
                weaponRight = aimRight;
                copyOrientation(aimRight, physRight);
            }
            if (family == L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES)
            {
                const float roll = bmvr::g_VrHandsIndexRollDeg;
                rollAroundForward(physLeft, -roll);
                rollAroundForward(physRight, roll);
            }
        }
    }
    else
    {
        const vr::TrackedDeviceIndex_t leftIdx =
            m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        const vr::TrackedDeviceIndex_t rightIdx =
            m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        family = DetectOpenVrControllerFamily(m_System, leftIdx, rightIdx);
        tilt = bmvr::EffectiveControllerPitchTilt(family);
        sampleRole(vr::TrackedControllerRole_LeftHand, physLeft);
        sampleRole(vr::TrackedControllerRole_RightHand, physRight);
        if (!physLeft.valid || !physRight.valid)
        {
            for (uint32_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i)
            {
                if (m_System->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
                    continue;
                if (i == physLeft.idx || i == physRight.idx)
                    continue;
                Sample extra{};
                sampleIndex(i, extra);
                if (!extra.valid)
                    continue;
                const vr::ETrackedControllerRole role = m_System->GetControllerRoleForTrackedDeviceIndex(i);
                if (!physLeft.valid && (role == vr::TrackedControllerRole_LeftHand || role == vr::TrackedControllerRole_Invalid))
                    physLeft = extra;
                else if (!physRight.valid && (role == vr::TrackedControllerRole_RightHand || role == vr::TrackedControllerRole_Invalid))
                    physRight = extra;
                if (physLeft.valid && physRight.valid)
                    break;
            }
        }
        if (family == L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN)
        {
            family = DetectOpenVrControllerFamily(m_System, physLeft.idx, physRight.idx);
            const float retitled = bmvr::EffectiveControllerPitchTilt(family);
            if (retitled != tilt)
            {
                tilt = retitled;
                if (physLeft.valid)
                    sampleIndex(physLeft.idx, physLeft);
                if (physRight.valid)
                    sampleIndex(physRight.idx, physRight);
            }
        }
        weaponLeft = physLeft;
        weaponRight = physRight;
    }
    // OpenXR samples are rebuilt from position/orientation only. Linear
    // velocity was already differenced onto m_WaitedPoses[1]/[2]; without
    // copying it here m_RightControllerSpeedMs stays 0 and crowbar melee
    // never crosses the swing threshold.
    if (m_OpenXrHelperBridgeActive)
    {
        vr::TrackedDevicePose_t waitedLeft{};
        vr::TrackedDevicePose_t waitedRight{};
        {
            std::lock_guard<std::mutex> lock(m_PoseMutex);
            waitedLeft = m_WaitedPoses[1];
            waitedRight = m_WaitedPoses[2];
        }
        auto stampVel = [&](Sample& s) {
            if (!s.valid || (s.idx != 1 && s.idx != 2))
                return;
            const vr::TrackedDevicePose_t& src = (s.idx == 1) ? waitedLeft : waitedRight;
            s.pose.vVelocity = src.vVelocity;
            s.pose.vAngularVelocity = src.vAngularVelocity;
        };
        stampVel(physLeft);
        stampVel(physRight);
        stampVel(weaponLeft);
        stampVel(weaponRight);
    }
    m_ControllerFamily = family;
    const Sample& aim = (AimControllerRole() == vr::TrackedControllerRole_LeftHand) ? weaponLeft : weaponRight;
    if (!aim.valid)
        return;

    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    m_LeftControllerTrackingValid = physLeft.valid;
    m_LeftControllerDevice = physLeft.idx;
    if (physLeft.valid)
    {
        m_LeftControllerPosAbs = physLeft.pos;
        m_LeftControllerAngAbs = physLeft.ang;
        m_LeftControllerTracking = physLeft.pose.mDeviceToAbsoluteTracking;
    }
    m_PhysicalRightTrackingValid = physRight.valid;
    m_PhysicalRightDevice = physRight.idx;
    if (physRight.valid)
    {
        m_PhysicalRightPosAbs = physRight.pos;
        m_PhysicalRightAngAbs = physRight.ang;
        m_PhysicalRightTracking = physRight.pose.mDeviceToAbsoluteTracking;
    }

    m_RightControllerPosAbs = aim.pos;
    m_RightControllerAngAbs = aim.ang;
    m_RightControllerForward = aim.fwd;
    m_RightControllerRight = aim.right;
    m_RightControllerUp = aim.up;
    float vx = aim.pose.vVelocity.v[0];
    float vy = aim.pose.vVelocity.v[1];
    float vz = aim.pose.vVelocity.v[2];
    if (hmdPose.bPoseIsValid)
    {
        vx -= hmdPose.vVelocity.v[0];
        vy -= hmdPose.vVelocity.v[1];
        vz -= hmdPose.vVelocity.v[2];
    }
    m_RightControllerRelVel = Vector(vx, vy, vz);
    m_RightControllerSpeedMs = sqrtf(vx * vx + vy * vy + vz * vz);
    m_RightControllerTracking = aim.pose.mDeviceToAbsoluteTracking;
    m_RightControllerTrackingValid = true;
    if (hmdPose.bPoseIsValid)
    {
        const auto& hm = hmdPose.mDeviceToAbsoluteTracking.m;
        m_HmdTrackForward = Vector(-hm[0][2], -hm[1][2], -hm[2][2]);
    }

    m_ViewmodelForward = aim.fwd;
    m_ViewmodelRight = aim.right;
    m_ViewmodelUp = aim.up;
    ApplyViewmodelBasisOffsets();

    m_ControllerPoseValid = true;
    static int s_ctrlLog;
    if (s_ctrlLog < 4)
    {
        Game::logMsg("Controller tracking aim=(%.1f,%.1f,%.1f) left=%d right=%d tilt=%.1f family=%s aimPose=%d indexRoll=%.1f",
            m_RightControllerPosAbs.x, m_RightControllerPosAbs.y, m_RightControllerPosAbs.z,
            m_LeftControllerTrackingValid ? 1 : 0,
            m_PhysicalRightTrackingValid ? 1 : 0,
            tilt,
            L4D2VR_ControllerFamilyName(family),
            L4D2VR_ControllerFamilyPrefersAimPose(family) ? 1 : 0,
            (family == L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES) ? bmvr::g_VrHandsIndexRollDeg : 0.f);
        ++s_ctrlLog;
    }
    bmvr::EndRisky(L"ctrl_pose");
}

vr::ETrackedControllerRole VR::AimControllerRole() const
{
    return bmvr::g_LeftHanded ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
}

bool VR::GetFingerCurls(vr::VRActionHandle_t skeletonAction, float outCurls[5]) const
{
    if (!outCurls)
        return false;
    auto applyRightWeaponPose = [&]() -> bool {
        if (skeletonAction != m_ActionSkeletonRight)
            return false;
        bool any = false;
        float def[5]{};
        if (GetDefaultRightHandWeaponCurls(def))
        {
            for (int i = 0; i < 5; ++i)
                outCurls[i] = def[i];
            any = true;
        }
        if (!IsCrowbarEquipped())
            return any;
        float attach[5]{};
        if (GetRightHandAttachCurls(attach))
        {
            for (int i = 0; i < 5; ++i)
            {
                if (attach[i] >= 0.f)
                {
                    outCurls[i] = attach[i];
                    any = true;
                }
            }
        }
        return any;
    };
    auto finishOk = [&]() -> bool {
        applyRightWeaponPose();
        return true;
    };
    if (m_OpenXrHelperBridgeActive)
    {
        const bool rightHand = (skeletonAction == m_ActionSkeletonRight);
        const uint32_t hand = rightHand
            ? L4D2VR_OPENXR_HAND_RIGHT
            : L4D2VR_OPENXR_HAND_LEFT;
        const L4D2VROpenXrHandTrackingDesc& ht = m_OpenXrLastInputState.handTracking[hand];
        if (ht.valid && ht.active && ht.jointCount > 0)
        {
            for (int i = 0; i < 5; ++i)
                outCurls[i] = std::clamp(ht.fingerCurls[i], 0.f, 1.f);
            return finishOk();
        }
        const vr::VRActionHandle_t triggerAction = rightHand
            ? m_ActionPrimaryAttack : m_ActionUse;
        const vr::VRActionHandle_t gripAction = rightHand
            ? m_ActionFlashlight : m_ActionReload;
        vr::InputAnalogActionData_t analog{};
        float trigger = 0.f;
        float grip = 0.f;
        bool any = false;
        if (GetAnalogActionData(triggerAction, analog))
        {
            trigger = std::clamp(analog.x, 0.f, 1.f);
            any = true;
        }
        if (GetAnalogActionData(gripAction, analog))
        {
            grip = std::clamp(analog.x, 0.f, 1.f);
            any = true;
        }
        if (!any)
        {
            if (!ht.valid || !ht.active)
                return applyRightWeaponPose();
            for (int i = 0; i < 5; ++i)
                outCurls[i] = std::clamp(ht.fingerCurls[i], 0.f, 1.f);
            return finishOk();
        }
        constexpr float kRestCurl = 0.10f;
        outCurls[0] = (std::max)(kRestCurl, (std::max)(trigger * 0.15f, grip * 0.10f));
        outCurls[1] = (std::max)(kRestCurl, (std::max)(trigger, grip * 0.35f));
        for (int i = 2; i < 5; ++i)
            outCurls[i] = (std::max)(kRestCurl, grip);
        return finishOk();
    }
    if (!m_Input || skeletonAction == vr::k_ulInvalidActionHandle)
        return applyRightWeaponPose();
    vr::VRSkeletalSummaryData_t summary{};
    const vr::EVRInputError err = m_Input->GetSkeletalSummaryData(
        skeletonAction, vr::VRSummaryType_FromAnimation, &summary);
    if (err != vr::VRInputError_None)
        return applyRightWeaponPose();
    for (int i = 0; i < vr::VRFinger_Count; ++i)
        outCurls[i] = std::clamp(summary.flFingerCurl[i], 0.f, 1.f);
    return finishOk();
}

void VR::TryCompositorPostPresentHandoff(DWORD nowMs, DWORD poseAgeMs)
{
    (void)poseAgeMs;
    if (!m_CompositorAppHandoff || !ShouldCompositorSubmit() || !m_Compositor)
        return;

    // HL2VR: PostPresentHandoff is unconditional. Do not fall back to runtime
    // timing — that makes WaitGetPoses touch the GPU queue again.
    const DWORD t0 = GetTickCount();
    m_Compositor->PostPresentHandoff();
    const DWORD dt = GetTickCount() - t0;
    m_Hl2vrHandoffId.store(m_Hl2vrAwaitedId.load(std::memory_order_acquire), std::memory_order_release);
    m_Hl2vrFrameCv.notify_all();
    if (dt >= 40)
    {
        ++m_CompositorHandoffSlowCount;
        if (dt >= 50)
        {
            Game::logMsg("Compositor PostPresentHandoff slow dt=%ums poseAge=%ums",
                dt, nowMs - m_WaitedPoseTick.load(std::memory_order_acquire));
        }
    }
}

bool VR::DrawIndependentHandMarkers(IDirect3DSurface9* eyeSurf, int stereoEye, bool drawOverlays, bool drawGloves)
{
    if (!eyeSurf || !m_IsVREnabled || !IsGameplayEligible() || !m_HmdPoseValid || !g_D3DVR9)
        return false;
    if (IsMenuUp())
        return false;
    if (!(m_Fov > 10.f) || !m_HmdOriginLatched)
        return false;

    bool leftOk = false;
    bool rightOk = false;
    QAngle leftAng{};
    QAngle rightAng{};
    Vector leftPos{};
    Vector rightPos{};
    Vector body{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (m_StereoFramePoseActive)
        {
            leftOk = m_StereoFrameLeftCtrlValid;
            rightOk = m_StereoFrameRightCtrlValid;
            leftAng = m_StereoFrameLeftCtrlAng;
            rightAng = m_StereoFrameRightCtrlAng;
            leftPos = m_StereoFrameLeftCtrlPos;
            rightPos = m_StereoFrameRightCtrlPos;
        }
        else
        {
            leftOk = m_LeftControllerTrackingValid;
            rightOk = m_PhysicalRightTrackingValid;
            leftAng = m_LeftControllerAngAbs;
            rightAng = m_PhysicalRightAngAbs;
            leftPos = m_LeftControllerPosAbs;
            rightPos = m_PhysicalRightPosAbs;
        }
        body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    }
    if (!leftOk && !rightOk)
        return false;

    auto toWorld = [&](const Vector& tracking) {
        return ControllerTrackingToWorld(body, tracking);
    };

    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    const Vector eyeOrig = (stereoEye == 1)
        ? GetViewOriginLeft(body)
        : (stereoEye == 2) ? GetViewOriginRight(body) : GetViewOrigin(body);

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;

    if (bmvr::TryVrGloves() && bmvr::g_VrHandsGlovesEnabled)
        g_VrGloves.WarmupGpu(device);

    D3DSURFACE_DESC desc{};
    UINT w = 0, h = 0;
    if (FAILED(eyeSurf->GetDesc(&desc)) || desc.Width < 64 || desc.Height < 64)
    {
        device->Release();
        return false;
    }
    w = desc.Width;
    h = desc.Height;
    // gbmatch paints an HMD frustum into 16:9 pixels. Use the HMD aspect so
    // glove projection matches the anamorphic scene that squash-blits to the eye.
    const float pixelAspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float aspect = (bmvr::UseGbMatchViewLock() && m_Aspect > 0.1f) ? m_Aspect : pixelAspect;
    const float projFov = HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
    if (!(tanHalf > 0.01f))
    {
        device->Release();
        return false;
    }

    auto project = [&](const Vector& world, float& sx, float& sy) -> bool {
        const Vector delta = world - eyeOrig;
        const float z = delta.Dot(fwd);
        if (z < 4.f)
            return false;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x / z) / tanHalf;
        const float ndcY = ((y / z) * aspect) / tanHalf;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return sx > -40.f && sy > -40.f
            && sx < static_cast<float>(w) + 40.f && sy < static_cast<float>(h) + 40.f;
    };

    IDirect3DSurface9* oldRt = nullptr;
    IDirect3DSurface9* oldDepth = nullptr;
    D3DVIEWPORT9 oldVp{};
    const bool haveOldVp = SUCCEEDED(device->GetViewport(&oldVp));
    device->GetRenderTarget(0, &oldRt);
    device->GetDepthStencilSurface(&oldDepth);
    // Capture engine D3D state *before* HUD/gloves. Overlay turns Z off and
    // used to unbind the G-buffer depth; shaderapidx9 still thought Z/depth
    // were live, so the next eye's world pass depth-tested against nothing
    // (see-through walls, skybox, flashlight through geometry, one eye).
    // One persistent D3DSBT_ALL block per device (released on Reset).
    // CreateStateBlock already captures on creation, so the old per-eye
    // create + Capture + Apply + Release was two full state captures and an
    // allocation for every eye of every frame. Capture into the same block.
    IDirect3DStateBlock9* engineBlock = nullptr;
    if (!m_HandEngineStateBlock)
    {
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &m_HandEngineStateBlock)))
            m_HandEngineStateBlock = nullptr;
        else
            engineBlock = m_HandEngineStateBlock;
    }
    else if (SUCCEEDED(m_HandEngineStateBlock->Capture()))
        engineBlock = m_HandEngineStateBlock;
    if (FAILED(device->SetRenderTarget(0, eyeSurf)))
    {
        if (oldRt)
            oldRt->Release();
        if (oldDepth)
            oldDepth->Release();
        device->Release();
        return false;
    }

    auto depthMatchesColor = [&](IDirect3DSurface9* depth) -> bool {
        if (!depth)
            return false;
        D3DSURFACE_DESC depthDesc{};
        if (FAILED(depth->GetDesc(&depthDesc)))
            return false;
        return depthDesc.MultiSampleType == desc.MultiSampleType
            && depthDesc.Width == desc.Width && depthDesc.Height == desc.Height;
    };

    IDirect3DSurface9* gloveDepth = nullptr;
    if (stereoEye == 1 && depthMatchesColor(m_D9LeftEyeDepthSurface))
        gloveDepth = m_D9LeftEyeDepthSurface;
    else if (stereoEye == 2 && depthMatchesColor(m_D9RightEyeDepthSurface))
        gloveDepth = m_D9RightEyeDepthSurface;
    else if (depthMatchesColor(oldDepth))
        gloveDepth = oldDepth;

    D3DVIEWPORT9 eyeVp{};
    eyeVp.X = 0;
    eyeVp.Y = 0;
    eyeVp.Width = w;
    eyeVp.Height = h;
    eyeVp.MinZ = 0.f;
    eyeVp.MaxZ = 1.f;
    device->SetViewport(&eyeVp);

    // Snapshot engine draw state *before* overlay/gloves. Overlay turns Z off
    // for XYZRHW HUD; glove Draw() then CreateStateBlock/Apply around that, so
    // the next eye RenderView inherits ZENABLE=FALSE and world geometry fails
    // the depth test (see-through walls / shimmer once hands track). Do not
    // CreateStateBlock here after WorldDepth — that AVed on eye 2 (2026-09-04).
    DWORD capZEnable = TRUE, capZWrite = TRUE, capZFunc = D3DCMP_LESSEQUAL;
    DWORD capFog = FALSE, capLighting = FALSE, capBlend = FALSE, capATest = FALSE;
    DWORD capCull = D3DCULL_CCW, capColorWrite = 0xFu, capSrgb = FALSE, capStencil = FALSE;
    DWORD capColorVertex = TRUE, capDiffSrc = D3DMCS_COLOR1;
    DWORD capSrcBlend = D3DBLEND_SRCALPHA, capDestBlend = D3DBLEND_INVSRCALPHA;
    DWORD capClip = 0, capScissor = FALSE;
    DWORD capColorOp0 = D3DTOP_MODULATE, capColorArg1_0 = D3DTA_TEXTURE, capColorArg2_0 = D3DTA_CURRENT;
    DWORD capAlphaOp0 = D3DTOP_SELECTARG1, capAlphaArg1_0 = D3DTA_TEXTURE;
    DWORD capColorOp1 = D3DTOP_DISABLE;
    DWORD capFvf = 0;
    IDirect3DVertexShader9* capVs = nullptr;
    IDirect3DPixelShader9* capPs = nullptr;
    IDirect3DVertexDeclaration9* capDecl = nullptr;
    device->GetRenderState(D3DRS_ZENABLE, &capZEnable);
    device->GetRenderState(D3DRS_ZWRITEENABLE, &capZWrite);
    device->GetRenderState(D3DRS_ZFUNC, &capZFunc);
    device->GetRenderState(D3DRS_FOGENABLE, &capFog);
    device->GetRenderState(D3DRS_LIGHTING, &capLighting);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &capBlend);
    device->GetRenderState(D3DRS_ALPHATESTENABLE, &capATest);
    device->GetRenderState(D3DRS_CULLMODE, &capCull);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &capColorWrite);
    device->GetRenderState(D3DRS_SRGBWRITEENABLE, &capSrgb);
    device->GetRenderState(D3DRS_STENCILENABLE, &capStencil);
    device->GetFVF(&capFvf);
    device->GetVertexShader(&capVs);
    device->GetPixelShader(&capPs);
    device->GetVertexDeclaration(&capDecl);
    device->GetRenderState(D3DRS_COLORVERTEX, &capColorVertex);
    device->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, &capDiffSrc);
    device->GetRenderState(D3DRS_SRCBLEND, &capSrcBlend);
    device->GetRenderState(D3DRS_DESTBLEND, &capDestBlend);
    device->GetRenderState(D3DRS_CLIPPLANEENABLE, &capClip);
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &capScissor);
    device->GetTextureStageState(0, D3DTSS_COLOROP, &capColorOp0);
    device->GetTextureStageState(0, D3DTSS_COLORARG1, &capColorArg1_0);
    device->GetTextureStageState(0, D3DTSS_COLORARG2, &capColorArg2_0);
    device->GetTextureStageState(0, D3DTSS_ALPHAOP, &capAlphaOp0);
    device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &capAlphaArg1_0);
    device->GetTextureStageState(1, D3DTSS_COLOROP, &capColorOp1);

    auto restoreEngineDrawState = [&]() {
        device->SetRenderState(D3DRS_ZENABLE, capZEnable);
        device->SetRenderState(D3DRS_ZWRITEENABLE, capZWrite);
        device->SetRenderState(D3DRS_ZFUNC, capZFunc);
        device->SetRenderState(D3DRS_FOGENABLE, capFog);
        device->SetRenderState(D3DRS_LIGHTING, capLighting);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, capBlend);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, capATest);
        device->SetRenderState(D3DRS_CULLMODE, capCull);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, capColorWrite);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, capSrgb);
        device->SetRenderState(D3DRS_STENCILENABLE, capStencil);
        device->SetRenderState(D3DRS_COLORVERTEX, capColorVertex);
        device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, capDiffSrc);
        device->SetRenderState(D3DRS_SRCBLEND, capSrcBlend);
        device->SetRenderState(D3DRS_DESTBLEND, capDestBlend);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, capClip);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, capScissor);
        device->SetTextureStageState(0, D3DTSS_COLOROP, capColorOp0);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, capColorArg1_0);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, capColorArg2_0);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, capAlphaOp0);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, capAlphaArg1_0);
        device->SetTextureStageState(1, D3DTSS_COLOROP, capColorOp1);
        device->SetFVF(capFvf);
        if (capDecl)
            device->SetVertexDeclaration(capDecl);
        if (capVs)
            device->SetVertexShader(capVs);
        else
            device->SetVertexShader(nullptr);
        if (capPs)
            device->SetPixelShader(capPs);
        else
            device->SetPixelShader(nullptr);
    };
    auto releaseCaps = [&]() {
        if (capVs) { capVs->Release(); capVs = nullptr; }
        if (capPs) { capPs->Release(); capPs = nullptr; }
        if (capDecl) { capDecl->Release(); capDecl = nullptr; }
    };

    auto restoreTargets = [&]() {
        if (haveOldVp)
            device->SetViewport(&oldVp);
        if (oldRt)
        {
            device->SetRenderTarget(0, oldRt);
            oldRt->Release();
            oldRt = nullptr;
        }
        if (oldDepth)
        {
            device->SetDepthStencilSurface(oldDepth);
            oldDepth->Release();
            oldDepth = nullptr;
        }
        else
            device->SetDepthStencilSurface(nullptr);
    };

    auto applyHandOverlayState = [&]() {
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetTexture(0, nullptr);
        device->SetTexture(1, nullptr);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
        device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    };

    const bool drawBoxes = drawOverlays && bmvr::g_VrHandsDebugBoxes && m_HasHevSuit;
    bool wantHandHud = drawOverlays && bmvr::g_HandHud && m_Game && m_HasHevSuit;
    bool wantOverlay = drawOverlays && (drawBoxes
        || WeaponMenuOpen()
        || AimCrosshairVisible());
    if ((wantOverlay || wantHandHud) && !bmvr::TryHandOverlay())
    {
        static int s_skipOverlayLog;
        if (s_skipOverlayLog < 2)
        {
            Game::logMsg("Skip hand overlay (previous launch died during wrist HUD/menu)");
            ++s_skipOverlayLog;
        }
        wantOverlay = false;
        wantHandHud = false;
    }

    // Weapon menu / debug boxes on the post-RenderView device, then gloves.
    // Wrist HUD is drawn after gloves with Z off (HL2VR IgnoreZ) so the glove
    // mesh cannot cover it. GetVertexShader / SetFVF after WorldDepth AVed
    // when snapshotting *then*; this path restores the pre-glove snapshot
    // first, then sets FVF again under the same crash-sticky flag.
    if (wantOverlay)
    {
        static int s_overlayFlag;
        if (s_overlayFlag == 0)
        {
            s_overlayFlag = 1;
            bmvr::BeginRisky(L"hand_overlay");
        }
        static int s_overlayStartLog;
        if (s_overlayStartLog < 4)
        {
            Game::logMsg("Hand overlay start eye=%d boxes=%d hud=%d menu=%d",
                stereoEye, drawBoxes ? 1 : 0,
                wantHandHud ? 1 : 0,
                WeaponMenuOpen() ? 1 : 0);
            ++s_overlayStartLog;
        }

        applyHandOverlayState();

        struct Vert
        {
            float x, y, z, rhw;
            D3DCOLOR color;
        };
        auto drawBox = [&](float sx, float sy, D3DCOLOR color, float half) {
            Vert v[4] = {
                { sx - half, sy - half, 0.f, 1.f, color },
                { sx + half, sy - half, 0.f, 1.f, color },
                { sx - half, sy + half, 0.f, 1.f, color },
                { sx + half, sy + half, 0.f, 1.f, color },
            };
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vert));
        };

        auto drawHandProxy = [&](const Vector& origin, const QAngle& ang, D3DCOLOR color, bool mirror,
            vr::VRActionHandle_t skeletonAction) {
            Vector hf, hr, hu;
            QAngle::AngleVectors(ang, &hf, &hr, &hu);
            float curls[5]{ 0.15f, 0.15f, 0.15f, 0.15f, 0.15f };
            GetFingerCurls(skeletonAction, curls);
            auto drawPart = [&](float lx, float ly, float lz, float half) {
                if (mirror)
                    lx = -lx;
                const Vector w = origin + hr * lx + hu * ly - hf * lz;
                float px = 0.f, py = 0.f;
                if (project(w, px, py))
                    drawBox(px, py, color, half);
            };
            drawPart(0.f, 0.f, 0.f, 14.f);
            const float thumbSign = mirror ? -1.f : 1.f;
            drawPart(thumbSign * (-2.f + curls[0] * 1.5f), -1.f, 3.f + curls[0] * 2.f, 9.f);
            drawPart(thumbSign * (2.f + curls[1] * 2.f), 0.f, 4.f + curls[1] * 3.f, 8.f);
            drawPart(thumbSign * (4.f + curls[2] * 2.f), 0.f, 6.f + curls[2] * 3.f, 8.f);
            drawPart(thumbSign * (5.f + curls[3] * 1.5f), 0.f, 5.f + curls[3] * 2.5f, 7.f);
            drawPart(thumbSign * (6.f + curls[4] * 1.f), 0.f, 3.f + curls[4] * 2.f, 6.f);
        };

        if (drawBoxes)
        {
            if (leftOk)
                drawHandProxy(toWorld(leftPos), leftAng, D3DCOLOR_XRGB(0, 255, 255), true, m_ActionSkeletonLeft);
            if (rightOk && (bmvr::g_VrHandsRightEnabled
                || (g_Game && g_Game->m_VR && g_Game->m_VR->WantsRightGloveVisible())))
                drawHandProxy(toWorld(rightPos), rightAng, D3DCOLOR_XRGB(255, 0, 255), false, m_ActionSkeletonRight);
        }

        if (WeaponMenuOpen())
            DrawWeaponMenu(device, w, h, eyeOrig, fwd, right, up);
        else if (AimCrosshairVisible())
        {
            float cx = 0.f, cy = 0.f;
            if (project(m_AimCrosshairWorld, cx, cy))
            {
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                DrawAimCrosshair(device, cx, cy, h);
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            }
        }

        static int s_handLog;
        if (s_handLog < 4)
        {
            Game::logMsg("Hand overlay done eye=%d left=%d right=%d %ux%u fmt=%u",
                stereoEye, leftOk ? 1 : 0, rightOk ? 1 : 0, w, h, desc.Format);
            ++s_handLog;
        }

        device->SetFVF(0);
        restoreEngineDrawState();
        if (stereoEye == 2)
            bmvr::EndRisky(L"hand_overlay");
    }
    else if (stereoEye == 2)
        bmvr::EndRisky(L"hand_overlay");

    bool drewGloves = false;
    if (drawGloves && bmvr::TryVrGloves() && bmvr::g_VrHandsGlovesEnabled && (m_HasHevSuit || g_VrGloves.HasBareHands()))
    {
        if (gloveDepth)
        {
            device->SetDepthStencilSurface(gloveDepth);
            // Private eye depth is the stereo scene when HookedSetDepthStencil
            // redirects the eye RenderView into it (world + gun). Clearing it
            // made gloves an IgnoreZ overlay on top of the gun and through
            // walls; WorldDepth writes still self-occlude fingers vs palm.
            // Only clear a leftover private buffer that did not receive the
            // eye pass (desktop blit / size mismatch).
            const bool sceneEyeDepth =
                gloveDepth == oldDepth
                || (stereoEye == 1 && gloveDepth == m_D9LeftEyeDepthSurface)
                || (stereoEye == 2 && gloveDepth == m_D9RightEyeDepthSurface);
            if (!sceneEyeDepth)
            {
                if (g_OrigClear)
                    g_OrigClear(device, 0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.f, 0);
                else
                    device->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.f, 0);
            }
        }
        else if (stereoEye != 0 && !drawOverlays)
            device->SetDepthStencilSurface(nullptr);

        static int s_gloveFlag;
        if (s_gloveFlag == 0)
        {
            s_gloveFlag = 1;
            bmvr::BeginRisky(L"vr_gloves");
        }
        const Vector viewAngles = GetViewAngle();
        drewGloves = g_VrGloves.DrawForEye(
            device,
            stereoEye,
            eyeOrig,
            viewAngles,
            projFov,
            aspect,
            m_VRScale,
            bmvr::g_VrHandsModelScale,
            m_Input,
            m_ActionSkeletonLeft,
            m_ActionSkeletonRight,
            leftOk,
            toWorld(leftPos),
            leftAng,
            rightOk,
            toWorld(rightPos),
            rightAng,
            m_StereoZNear,
            m_StereoZFar,
            stereoEye == 0);
        static int s_gloveDepthLog;
        if (s_gloveDepthLog < 4)
        {
            Game::logMsg("VR gloves scene eye=%d %ux%u depth=%d sceneZ=%d light+zNear=%.2f zFar=%.0f",
                stereoEye, w, h, gloveDepth ? 1 : 0,
                (gloveDepth && (gloveDepth == oldDepth
                    || (stereoEye == 1 && gloveDepth == m_D9LeftEyeDepthSurface)
                    || (stereoEye == 2 && gloveDepth == m_D9RightEyeDepthSurface))) ? 1 : 0,
                m_StereoZNear, m_StereoZFar);
            ++s_gloveDepthLog;
        }
    }
    if (drawGloves && bmvr::TryVrGloves() && bmvr::g_VrHandsGlovesEnabled && (m_HasHevSuit || g_VrGloves.HasBareHands()) && !drewGloves)
    {
        static int s_gloveFallbackLog;
        if (s_gloveFallbackLog < 3)
        {
            Game::logMsg("VR gloves fallback to debug boxes: %s",
                g_VrGloves.FailureReason().empty() ? "no mesh this eye" : g_VrGloves.FailureReason().c_str());
            ++s_gloveFallbackLog;
        }
    }

    // HL2VR hand/weapon HUD: IgnoreZ (draw after the glove mesh, Z off) plus
    // IsBackfacing so the ammo plate is invisible from behind the gun.
    if (wantHandHud)
    {
        restoreEngineDrawState();
        device->SetViewport(&eyeVp);
        static int s_hudFlag;
        if (s_hudFlag == 0)
        {
            s_hudFlag = 1;
            bmvr::BeginRisky(L"hand_overlay");
        }
        applyHandOverlayState();
        DrawHandHud(device, stereoEye, w, h,
            leftOk, toWorld(leftPos), leftAng,
            rightOk, toWorld(rightPos), rightAng,
            eyeOrig, fwd, right, up);
        device->SetFVF(0);
        restoreEngineDrawState();
        if (stereoEye == 2)
            bmvr::EndRisky(L"hand_overlay");
    }

    if (engineBlock)
    {
        engineBlock->Apply();
        engineBlock = nullptr;
        if (oldRt)
        {
            oldRt->Release();
            oldRt = nullptr;
        }
        if (oldDepth)
        {
            oldDepth->Release();
            oldDepth = nullptr;
        }
    }
    else
        restoreTargets();
    restoreEngineDrawState();
    // Overlay SetRenderState bypasses shaderapidx9's cache. If we leave D3D
    // Z-off while the cache still says Z-on, the next eye never re-enables
    // depth and walls/flashlight fail on that eye only.
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetFVF(0);
    releaseCaps();
    device->Release();
    if (drewGloves && stereoEye == 2)
        bmvr::EndRisky(L"vr_gloves");
    static int s_doneLog;
    if (s_doneLog < 4)
    {
        Game::logMsg("Hand markers done eye=%d gloves=%d overlay=%d",
            stereoEye, drewGloves ? 1 : 0, wantOverlay ? 1 : 0);
        ++s_doneLog;
    }
    return drewGloves;
}

namespace
{
    // 5x7 glyphs, bit 4 = left column. 0-9 then A-Z. Digit 0 is slashed so
    // it does not read as O/8; 6/8/9 keep open counters.
    const uint8_t kHud5x7[36][7] = {
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
        { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
        { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E },
        { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
        { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
        { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
        { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
        { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
        { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
        { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0F },
        { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E },
        { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
        { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
        { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
        { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
        { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
        { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
        { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
        { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
        { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
        { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
        { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    };

    struct HudVert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };

    struct HudVertTex
    {
        float x, y, z, rhw;
        D3DCOLOR color;
        float u, v;
    };

    // World panel: lower-left origin, +xaxis = width, +left = height (up the face).
    struct HudPanel
    {
        Vector ul;
        Vector xaxis;
        Vector yDown;
        Vector face;
        float width = 0.f;
        float height = 0.f;
        bool valid = false;
    };

    constexpr float kHandHudSize = 2.5f;
    constexpr float kAmmoHudSize = 2.0f;

    void HudMakePanel(const Vector& lowerLeft, const Vector& xaxis, const Vector& left,
        const Vector& face, float size, HudPanel& out)
    {
        out.ul = lowerLeft + left * size;
        out.xaxis = xaxis;
        out.yDown = left * -1.f;
        out.face = face;
        out.width = size;
        out.height = size;
        out.valid = true;
    }

    // HL2VR C_VGuiScreen::IsBackfacing: skip the panel when the eye is behind it.
    bool HudIsBackfacing(const HudPanel& p, const Vector& eye)
    {
        const Vector origin = p.ul + p.xaxis * (p.width * 0.5f) + p.yDown * (p.height * 0.5f);
        return p.face.Dot(origin - eye) > 0.f;
    }

    Vector HudRotate(const Vector& v, const Vector& axis, float deg)
    {
        const float rad = deg * 0.01745329252f;
        const float c = cosf(rad);
        const float s = sinf(rad);
        return v * c + CrossProduct(axis, v) * s + axis * (axis.Dot(v) * (1.f - c));
    }

    // Glove mesh is pitched toward the knuckles and rolled vs the controller.
    // Runtime: controller-aligned panel sat flat while the HEV gauntlet sloped.
    void BuildHandHudPanel(const Vector& ctrlPos, const QAngle& ctrlAng, bool rightHand, HudPanel& out)
    {
        Vector fwd, right, up;
        QAngle::AngleVectors(ctrlAng, &fwd, &right, &up);
        Vector left = right * -1.f;
        constexpr float kPitch = -18.f;
        const float kRoll = rightHand ? 14.f : -14.f;
        fwd = HudRotate(fwd, left, kPitch);
        up = HudRotate(up, left, kPitch);
        left = HudRotate(left, fwd, kRoll);
        up = HudRotate(up, fwd, kRoll);
        constexpr float kLift = 1.05f;
        constexpr float kAlong = -0.55f;
        constexpr float kAcross = 0.3f;
        const Vector center = ctrlPos + up * kLift + fwd * kAlong + left * kAcross;
        const Vector origin = center - fwd * (kHandHudSize * 0.5f) - left * (kHandHudSize * 0.5f);
        HudMakePanel(origin, fwd, left, up, kHandHudSize, out);
    }

    // Beside the gun-hand controller, one-sided, inward. Same Cross order as
    // the readable plate, but "up" is the controller so the digits yaw/pitch/roll
    // with the gun instead of staying horizon-locked to the HMD.
    void BuildAmmoHudPanel(const Vector& ctrlPos, const QAngle& ctrlAng, bool rightHand, HudPanel& out)
    {
        Vector fwd, right, up;
        QAngle::AngleVectors(ctrlAng, &fwd, &right, &up);
        const float inward = rightHand ? -1.f : 1.f;
        constexpr float kBeside = 1.35f;
        constexpr float kBack = 0.2f;
        constexpr float kUp = 1.15f;
        const Vector center = ctrlPos + right * (inward * kBeside) - fwd * kBack + up * kUp;
        const Vector face = right * inward;
        Vector left = up - face * up.Dot(face);
        if (left.LengthSqr() < 0.01f)
            left = fwd - face * fwd.Dot(face);
        VectorNormalize(left);
        Vector xaxis = CrossProduct(left, face);
        if (xaxis.LengthSqr() < 0.01f)
            xaxis = fwd;
        VectorNormalize(xaxis);
        left = CrossProduct(face, xaxis);
        VectorNormalize(left);
        const Vector origin = center - xaxis * (kAmmoHudSize * 0.5f) - left * (kAmmoHudSize * 0.5f);
        HudMakePanel(origin, xaxis, left, face, kAmmoHudSize, out);
    }

    int HudGlyphIndex(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'Z')
            return 10 + (c - 'A');
        return -1;
    }
}

void VR::DrawHandHud(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
    bool leftOk, const Vector& leftOrigin, const QAngle& leftAng,
    bool rightOk, const Vector& rightOrigin, const QAngle& rightAng,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    (void)stereoEye;
    if (!m_Game || !device)
        return;
    int health = -1, armor = -1, clip = -1, reserve = -1, secondary = -1;
    if (!m_Game->ReadWristHudValues(health, armor, clip, reserve, secondary))
        return;
    const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float projFov = HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
    if (!(tanHalf > 0.01f))
        return;
    auto project = [&](const Vector& world, float& sx, float& sy) -> bool {
        const Vector delta = world - eyeOrig;
        const float z = delta.Dot(fwd);
        if (z < 4.f)
            return false;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x / z) / tanHalf;
        const float ndcY = ((y / z) * aspect) / tanHalf;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return sx > -80.f && sy > -80.f && sx < static_cast<float>(w) + 80.f && sy < static_cast<float>(h) + 80.f;
    };

    auto panelWorld = [](const HudPanel& p, float u, float v) {
        return p.ul + p.xaxis * u + p.yDown * v;
    };
    // Glyphs are 5x7 pixel blocks, i.e. up to 35 quads each and ~1000 quads
    // per eye across the health/armor/ammo fields. One DrawPrimitiveUP per
    // quad was ~1000 draw calls (each a DXVK UP-buffer upload + pipeline
    // lookup) per eye per frame. Batch the flat-colour quads into a triangle
    // list and flush once before every textured icon (order is preserved so
    // alpha blending is unchanged) and at the end. Present-thread only.
    static std::vector<HudVert> s_quadBatch;
    if (s_quadBatch.capacity() < 8192)
        s_quadBatch.reserve(8192);
    s_quadBatch.clear();
    auto flushQuads = [&]() {
        if (s_quadBatch.empty())
            return;
        const UINT tris = static_cast<UINT>(s_quadBatch.size() / 3);
        if (tris > 0)
            device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, tris, s_quadBatch.data(), sizeof(HudVert));
        s_quadBatch.clear();
    };
    auto panelQuad = [&](const HudPanel& p, float u, float v, float qw, float qh, D3DCOLOR color) {
        float x00 = 0.f, y00 = 0.f, x10 = 0.f, y10 = 0.f, x01 = 0.f, y01 = 0.f, x11 = 0.f, y11 = 0.f;
        if (!project(panelWorld(p, u, v), x00, y00)
            || !project(panelWorld(p, u + qw, v), x10, y10)
            || !project(panelWorld(p, u, v + qh), x01, y01)
            || !project(panelWorld(p, u + qw, v + qh), x11, y11))
            return;
        // Same two triangles the TRIANGLESTRIP (00,10,01,11) produced.
        const HudVert v00{ x00, y00, 0.f, 1.f, color };
        const HudVert v10{ x10, y10, 0.f, 1.f, color };
        const HudVert v01{ x01, y01, 0.f, 1.f, color };
        const HudVert v11{ x11, y11, 0.f, 1.f, color };
        s_quadBatch.push_back(v00);
        s_quadBatch.push_back(v10);
        s_quadBatch.push_back(v01);
        s_quadBatch.push_back(v10);
        s_quadBatch.push_back(v11);
        s_quadBatch.push_back(v01);
    };
    auto panelGlyph = [&](const HudPanel& p, float x, float y, float cell, char c, D3DCOLOR color) {
        const int idx = HudGlyphIndex(c);
        if (idx < 0)
            return;
        const float fill = cell * 0.84f;
        for (int row = 0; row < 7; ++row)
        {
            const uint8_t bits = kHud5x7[idx][row];
            for (int col = 0; col < 5; ++col)
            {
                if (bits & (0x10 >> col))
                    panelQuad(p, x + col * cell, y + row * cell, fill, fill, color);
            }
        }
    };
    auto panelNumber = [&](const HudPanel& p, float xRight, float y, float cell, int value, int minDigits, D3DCOLOR color) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", value < 0 ? 0 : value);
        const int len = static_cast<int>(strlen(buf));
        const int digits = len < minDigits ? minDigits : len;
        float x = xRight - digits * 6.f * cell;
        for (int pad = len; pad < digits; ++pad)
        {
            panelGlyph(p, x, y, cell, '0', color);
            x += 6.f * cell;
        }
        for (int i = 0; i < len; ++i)
        {
            panelGlyph(p, x, y, cell, buf[i], color);
            x += 6.f * cell;
        }
    };
    auto panelNumberField = [&](const HudPanel& p, float xRight, float y, float cell,
        int value, int fieldDigits, D3DCOLOR color, D3DCOLOR backing) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", value < 0 ? 0 : value);
        const int len = static_cast<int>(strlen(buf));
        const float advance = 6.f * cell;
        const float x = xRight - fieldDigits * advance;
        if ((backing >> 24) != 0)
        {
            for (int i = 0; i < fieldDigits - len; ++i)
                panelGlyph(p, x + i * advance, y, cell, '0', backing);
        }
        panelNumber(p, xRight, y, cell, value, 1, color);
    };
    auto panelIcon = [&](const HudPanel& p, IDirect3DTexture9* tex,
        float x, float y, float iw, float ih, D3DCOLOR tint) {
        if (!tex)
            return;
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(tex->GetLevelDesc(0, &desc)) && desc.Width > 0 && desc.Height > 0)
        {
            const float srcAspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
            float fitW = iw;
            float fitH = iw / srcAspect;
            if (fitH > ih)
            {
                fitH = ih;
                fitW = ih * srcAspect;
            }
            x += (iw - fitW) * 0.5f;
            y += (ih - fitH) * 0.5f;
            iw = fitW;
            ih = fitH;
        }
        float x00 = 0.f, y00 = 0.f, x10 = 0.f, y10 = 0.f, x01 = 0.f, y01 = 0.f, x11 = 0.f, y11 = 0.f;
        if (!project(panelWorld(p, x, y), x00, y00)
            || !project(panelWorld(p, x + iw, y), x10, y10)
            || !project(panelWorld(p, x, y + ih), x01, y01)
            || !project(panelWorld(p, x + iw, y + ih), x11, y11))
            return;
        flushQuads();
        device->SetTexture(0, tex);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        HudVertTex verts[4] = {
            { x00, y00, 0.f, 1.f, tint, 0.f, 0.f },
            { x10, y10, 0.f, 1.f, tint, 1.f, 0.f },
            { x01, y01, 0.f, 1.f, tint, 0.f, 1.f },
            { x11, y11, 0.f, 1.f, tint, 1.f, 1.f }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(HudVertTex));
        device->SetTexture(0, nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    };

    // The hand overlay draws opaque, but the wrist HUD needs real alpha: the
    // icons carry their shape in the alpha channel (white RGB), so without
    // blending they fill as solid blocks, and the dim backing digits would draw
    // at full strength. Restored before returning.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    const bool blueShift = bmvr::IsBlueShift();
    const D3DCOLOR theme = blueShift
        ? D3DCOLOR_RGBA(64, 168, 255, 235)
        : D3DCOLOR_RGBA(255, 176, 0, 235);
    const D3DCOLOR themeDim = blueShift
        ? D3DCOLOR_RGBA(64, 168, 255, 170)
        : D3DCOLOR_RGBA(255, 176, 0, 170);
    const D3DCOLOR themeBack = blueShift
        ? D3DCOLOR_RGBA(64, 168, 255, 55)
        : D3DCOLOR_RGBA(255, 176, 0, 55);
    const D3DCOLOR red = D3DCOLOR_RGBA(255, 48, 32, 240);
    const D3DCOLOR redBack = D3DCOLOR_RGBA(255, 48, 32, 60);
    constexpr int kLowHealth = 25;
    const bool rightDominant = !bmvr::g_LeftHanded;

    auto drawValueIcon = [&](const HudPanel& p, float x0, float yTop, float valueCell,
        float fieldWidth, float rowH, float iconSize, int value, D3DCOLOR color, D3DCOLOR backing,
        IDirect3DTexture9* icon) {
        const float gap = p.width * 0.04f;
        panelNumberField(p, x0 + fieldWidth, yTop, valueCell, value, 3, color, backing);
        panelIcon(p, icon, x0 + fieldWidth + gap, yTop + (rowH - iconSize) * 0.5f,
            iconSize, iconSize, color);
    };

    auto layoutRow = [&](const HudPanel& p, float pad, float yTop, float rowH, float cell,
        int value, D3DCOLOR color, D3DCOLOR backing, IDirect3DTexture9* icon) {
        const float gap = p.width * 0.04f;
        const float fieldW = 3.f * 6.f * cell;
        const float iconS = rowH;
        const float total = fieldW + gap + iconS;
        const float x0 = pad + (p.width - 2.f * pad - total) * 0.5f;
        drawValueIcon(p, x0, yTop, cell, fieldW, rowH, iconS, value, color, backing, icon);
    };

    if (health >= 0 && health <= 200)
    {
        const bool offOk = rightDominant ? leftOk : rightOk;
        if (offOk)
        {
            HudPanel handPanel{};
            BuildHandHudPanel(
                rightDominant ? leftOrigin : rightOrigin,
                rightDominant ? leftAng : rightAng,
                !rightDominant, handPanel);
            if (handPanel.valid && !HudIsBackfacing(handPanel, eyeOrig))
            {
                const float pad = handPanel.width * 0.08f;
                const float gap = handPanel.width * 0.06f;
                const float rowH = (handPanel.height - 2.f * pad - gap) * 0.5f;
                const float cell = rowH / 7.f;
                const bool low = health <= kLowHealth;
                layoutRow(handPanel, pad, pad, rowH, cell, health,
                    low ? red : theme, low ? redBack : themeBack,
                    bmvr::AcquireHudIcon(device, "hud_health_overlay.vtf"));
                if (armor >= 0 && armor <= 200)
                    layoutRow(handPanel, pad, pad + rowH + gap, rowH, cell, armor,
                        theme, themeBack,
                        bmvr::AcquireHudIcon(device, "hud_hev_overlay.vtf"));
            }
        }
    }

    const bool hasAmmoHud = !m_EmptyHands
        && ((clip >= 0 && clip <= 999) || (reserve >= 0 && reserve <= 999)
            || (secondary >= 0 && secondary <= 999));
    const bool gunOk = rightDominant ? rightOk : leftOk;
    if (hasAmmoHud && gunOk)
    {
        HudPanel weaponPanel{};
        BuildAmmoHudPanel(
            rightDominant ? rightOrigin : leftOrigin,
            rightDominant ? rightAng : leftAng,
            rightDominant, weaponPanel);
        if (weaponPanel.valid && !HudIsBackfacing(weaponPanel, eyeOrig))
        {
            std::string weaponModel;
            {
                std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
                weaponModel = m_LastViewmodelModel;
            }
            const char* model = weaponModel.c_str();
            IDirect3DTexture9* ammoIcon =
                bmvr::AcquireHudIcon(device, bmvr::PrimaryAmmoIconVtf(model, nullptr));

            const float pad = weaponPanel.width * 0.08f;
            const float gap = weaponPanel.width * 0.06f;
            const float primaryH = (weaponPanel.height - 2.f * pad - gap) * 0.62f;
            const float lowerH = (weaponPanel.height - 2.f * pad - gap) - primaryH;
            const float cell = primaryH / 7.f;
            const float smallCell = lowerH / 7.f;
            const bool showClip = clip >= 0 && clip <= 999;
            const int primary = showClip ? clip : reserve;
            if (primary >= 0 && primary <= 999)
                layoutRow(weaponPanel, pad, pad, primaryH, cell, primary,
                    theme, themeBack, ammoIcon);

            const bool showRes = showClip && reserve >= 0 && reserve <= 999;
            const bool showSec = secondary >= 0 && secondary <= 999;
            const float smallGap = weaponPanel.width * 0.04f;
            const float smallFieldW = 3.f * 6.f * smallCell;
            const float smallIconS = lowerH;
            const float cellW = smallFieldW + smallGap + smallIconS;
            float total = 0.f;
            if (showRes)
                total += cellW;
            if (showSec)
                total += (total > 0.f ? smallGap * 2.f : 0.f) + cellW;
            float cursor = pad + (weaponPanel.width - 2.f * pad - total) * 0.5f;
            const float y1 = pad + primaryH + gap;
            if (showRes)
            {
                drawValueIcon(weaponPanel, cursor, y1, smallCell, smallFieldW, lowerH, smallIconS,
                    reserve, themeDim, themeBack, ammoIcon);
                cursor += cellW + smallGap * 2.f;
            }
            if (showSec)
                drawValueIcon(weaponPanel, cursor, y1, smallCell, smallFieldW, lowerH, smallIconS,
                    secondary, themeDim, 0,
                    bmvr::AcquireHudIcon(device, bmvr::SecondaryAmmoIconVtf(model, nullptr)));
        }
    }

    flushQuads();
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void VR::DrawIndependentHandsOnDesktop()
{
    if (!(bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes))
        return;
    if (!g_D3DVR9)
        return;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return;
    }
    DrawIndependentHandMarkers(bb, 0);
    bb->Release();
    device->Release();
}

bool VR::DrawVrGlovesIntoBlitSource(int stereoEye)
{
    if (!bmvr::TryVrGloves() || !bmvr::g_VrHandsGlovesEnabled || !g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return false;
    }
    const bool drew = DrawIndependentHandMarkers(bb, stereoEye, false, true);
    bb->Release();
    device->Release();
    if (drew)
        m_VrGlovesDrawnIntoScene = true;
    return drew;
}

void VR::NoteSceneCubemap(IDirect3DBaseTexture9* texture)
{
    if (!texture || m_SceneCubemap == texture)
        return;
    IDirect3DBaseTexture9* previous = m_SceneCubemap;
    m_SceneCubemap = texture;
    m_SceneCubemap->AddRef();
    if (previous)
        previous->Release();
    static int s_cubeLog;
    if (s_cubeLog < 4)
    {
        Game::logMsg("Latched scene cubemap for VR gloves");
        ++s_cubeLog;
    }
}

void VR::NoteViewmodelModel(const char* modelName)
{
    if (!modelName || !modelName[0])
        return;
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (m_LastViewmodelModel != modelName)
    {
        m_LastViewmodelModel = modelName;
        ClassifyViewmodel(modelName, m_VmClass);
        uint32_t flags = 0;
        if (m_VmClass.shotgun) flags |= kVmFlagShotgun;
        if (m_VmClass.crowbar) flags |= kVmFlagCrowbar;
        if (m_VmClass.scoped) flags |= kVmFlagScoped;
        if (m_VmClass.rpg) flags |= kVmFlagRpg;
        if (m_VmClass.gluon) flags |= kVmFlagGluon;
        if (m_VmClass.mp5) flags |= kVmFlagMp5;
        m_VmClassFlags.store(flags, std::memory_order_relaxed);
        Game::logMsg("Viewmodel model %s", modelName);
    }
}

// Single lowercase pass over the model path; every consumer reads the result
// from m_VmClass. Tables are the ones ResolveWeaponViewmodelPose,
// ViewmodelVisualScale, ApplyTwoHandShotgunAim and the numpad offset key
// used to recompute per call.
void VR::ClassifyViewmodel(const char* modelName, ViewmodelClass& out)
{
    out = ViewmodelClass{};
    if (!modelName || !modelName[0])
        return;
    char m[256]{};
    size_t n = 0;
    for (; n + 1 < sizeof(m) && modelName[n]; ++n)
        m[n] = static_cast<char>(tolower(static_cast<unsigned char>(modelName[n])));
    m[n] = 0;
    auto has = [&](const char* s) { return std::strstr(m, s) != nullptr; };

    // Plan §7: L4D2 empirical position tables (HMD-tuned starting point).
    // DME bake rest is logged via NoteViewmodelWeaponBake for pivot diagnosis
    // only — not 1:1 with L4D2 offsets (implementation-plan §7 evidence).
    // 2026-08-28 HMD-saved numpad extras baked into this table. Live
    // extras in VR/viewmodel_offsets.txt still add on top.
    if (has("crowbar") || has("wrench"))
    { out.ox = 15.5f; out.oy = 8.5f; out.oz = -12.f; out.ax = -24.5f; out.ay = -6.5f; out.az = -6.f; out.numpadKey = "crowbar"; out.crowbar = true; }
    else if (has("glock") || has("pistol") || has("9mm") || has("beretta"))
    { out.ox = 16.5f; out.oy = 4.f; out.oz = -7.f; out.ax = -1.f; out.numpadKey = "glock"; }
    else if (has("357") || has("python") || has("revolver"))
    { out.ox = 10.f; out.oy = 3.5f; out.oz = -5.5f; out.ax = -0.5f; out.numpadKey = "357"; }
    else if (has("mp5") || has("smg") || has("mp5k"))
    { out.ox = 9.f; out.oy = 4.5f; out.oz = -9.f; out.ax = -0.5f; out.numpadKey = "mp5"; }
    else if (has("shotgun") || has("spas") || has("pump"))
    { out.ox = 9.f; out.oy = 8.f; out.oz = -9.5f; out.ax = -0.5f; out.ay = -4.f; out.numpadKey = "shotgun"; }
    else if (has("crossbow"))
    { out.ox = 12.5f; out.oy = 11.5f; out.oz = -13.f; out.ax = -4.5f; out.ay = -5.f; out.numpadKey = "crossbow"; }
    else if (has("rpg") || has("rocket"))
    { out.ox = 12.f; out.oy = 10.5f; out.oz = -6.5f; out.ax = -1.f; out.numpadKey = "rpg"; }
    else if (has("gauss") || has("tau"))
    { out.ox = 15.f; out.oy = 11.5f; out.oz = -16.5f; out.ax = -1.5f; out.ay = -8.f; out.numpadKey = "gauss"; }
    else if (has("egon") || has("gluon"))
    { out.ox = 19.f; out.oy = 8.f; out.oz = -2.f; out.ax = -1.5f; out.ay = -2.f; out.numpadKey = "gluon"; }
    else if (has("hgun") || has("hive") || has("hornet"))
    { out.ox = 19.5f; out.oy = 11.34f; out.oz = -17.69f; out.ax = -1.5f; out.ay = -2.f; out.numpadKey = "hgun"; }
    else if (has("grenade") || has("frag"))
    { out.ox = 15.f; out.oy = 6.5f; out.oz = -8.f; out.numpadKey = "grenade"; }
    else if (has("satchel"))
    { out.ox = 19.f; out.oy = -10.f; out.oz = -12.f; out.numpadKey = "satchel"; }
    else if (has("tripmine") || has("trip"))
    { out.ox = 12.5f; out.oy = 4.5f; out.oz = -9.5f; out.numpadKey = "tripmine"; }
    else if (has("squeak") || has("snark"))
    { out.ox = 15.f; out.oy = 7.5f; out.oz = -6.5f; out.numpadKey = "snark"; }
    else
        out.numpadKey = "default";

    // ViewmodelVisualScale table (order matters: later rules override).
    if (has("crowbar") || has("wrench"))
        out.fixedScale = 1.f;
    if (has("shotgun") || has("spas") || has("pump"))
        out.fixedScale = 0.64f;
    if (has("grenade") || has("frag"))
        out.scaleMul *= 1.25f;
    if (has("357") || has("python") || has("revolver"))
        out.scaleMul *= 1.15f;

    out.shotgun = has("shotgun") || has("spas") || has("pump");
    out.scoped = has("crossbow");
    out.rpg = has("rpg") || has("rocket");
    out.gluon = has("egon") || has("gluon");
    // Former hooks.cpp IsMp5Viewmodel (mp5 / MP5 / smg / mp5k) on the
    // lowercased path; dCalcViewModelView / SuppressViewmodelMovementAnims
    // now read kVmFlagMp5 / kVmFlagCrowbar instead.
    out.mp5 = has("mp5") || has("smg");
}

void VR::NoteViewmodelWeaponBake(const char* modelName, const char* boneName, float restX, float restY, float restZ)
{
    (void)modelName;
    // studiohdr model space: +X forward, +Y left, +Z up (Source viewmodel).
    // Entity origin + rest places the weapon root; p0 - ox*f - oy*r - oz*u
    // compensates so the root sits on the aim controller (l4d2vr-weapons §2.3).
    const float ox = -restX;
    const float oy = -restY;
    const float oz = restZ;
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    const bool changed = !m_HasViewmodelBake
        || m_ViewmodelBakeOx != ox || m_ViewmodelBakeOy != oy || m_ViewmodelBakeOz != oz;
    m_ViewmodelBakeOx = ox;
    m_ViewmodelBakeOy = oy;
    m_ViewmodelBakeOz = oz;
    m_HasViewmodelBake = true;
    if (changed)
    {
        Game::logMsg("Viewmodel bake bone=%s rest=(%.2f,%.2f,%.2f) ox,oy,oz=(%.2f,%.2f,%.2f)",
            boneName ? boneName : "?", restX, restY, restZ, ox, oy, oz);
    }
}

void VR::RefreshActiveWeaponModel()
{
    if (!m_Game)
        return;
    const char* name = m_Game->GetActiveWeaponModelName();
    if (name && name[0])
        NoteViewmodelModel(name);
}

void VR::RefreshHeldWeaponState()
{
    bool held = false;
    if (!m_EmptyHands && m_Game)
    {
        Game::InventoryWeapon first{};
        held = m_Game->CollectInventoryWeapons(&first, 1) > 0;
    }
    if (held != m_HasHeldWeapon)
        Game::logMsg("VR held weapon %s", held ? "yes" : "no");
    m_HasHeldWeapon = held;
}

void VR::ApplyViewmodelBasisOffsets()
{
    float ox = 0.f, oy = 0.f, oz = 0.f, ax = 0.f, ay = 0.f, az = 0.f;
    ResolveWeaponViewmodelPose(ox, oy, oz, ax, ay, az);
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelUp, ay);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelUp, ay);
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, ax);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, ax);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelForward, az);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelForward, az);
    ApplyTwoHandShotgunAim();
}

void VR::ApplyTwoHandShotgunAim()
{
    // L4D2 ResolvePavlovTwoHandedAimBasis, shotgun/spas/pump only. No
    // virtual stock, no pistols, no m_hViewModel[1]. Left GLB stays on
    // the left controller matrix.
    bool shotgun = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        shotgun = m_VmClass.shotgun;
    }
    if (!shotgun || !m_LeftControllerTrackingValid || !m_RightControllerTrackingValid)
    {
        if (m_TwoHandShotgunActive)
            Game::logMsg("Two-hand shotgun off (weapon/tracking)");
        m_TwoHandShotgunActive = false;
        return;
    }

    const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
    const Vector forend = m_RightControllerPosAbs + m_ViewmodelForward * (0.28f * scale);
    const float dist = (m_LeftControllerPosAbs - forend).Length();
    const float enterR = 0.06f * scale;
    const float stayR = 0.10f * scale;
    // Distance alone kept two-hand mode latched when the off-hand just dropped
    // to the player's side: the barrel then tracked the lowered hand and the
    // shotgun looked like it was lowering itself. Also require the off-hand to
    // stay ahead of the gun hand along the barrel.
    const float aheadHu = (m_LeftControllerPosAbs - m_RightControllerPosAbs).Dot(m_ViewmodelForward);
    const float enterAhead = 0.22f * scale;
    const float stayAhead = 0.14f * scale;
    const bool was = m_TwoHandShotgunActive;
    if (m_TwoHandShotgunActive)
        m_TwoHandShotgunActive = dist < stayR && aheadHu > stayAhead;
    else
        m_TwoHandShotgunActive = dist < enterR && aheadHu > enterAhead;
    if (m_TwoHandShotgunActive != was)
        Game::logMsg("Two-hand shotgun %s dist=%.1f hu ahead=%.1f enter=%.1f stay=%.1f",
            m_TwoHandShotgunActive ? "on" : "off", dist, aheadHu, enterR, stayR);
    if (!m_TwoHandShotgunActive)
        return;

    // L4D2: frontGrip = off-hand + 0.12 m along weapon forward.
    Vector front = m_LeftControllerPosAbs
        + m_ViewmodelForward * (0.12f * scale);
    Vector two = front - m_RightControllerPosAbs;
    if (VectorNormalize(two) <= 0.01f)
        return;
    const float strength = 0.55f;
    Vector blended = m_ViewmodelForward * (1.f - strength) + two * strength;
    if (VectorNormalize(blended) <= 0.01f)
        return;
    // A marginal off-hand position must not be able to swing the barrel far
    // from where the gun hand points. ~50 degrees.
    if (blended.Dot(m_ViewmodelForward) < 0.64f)
        return;
    m_ViewmodelForward = blended;
    m_ViewmodelRight = CrossProduct(m_ViewmodelForward, m_ViewmodelUp);
    if (VectorNormalize(m_ViewmodelRight) <= 0.01f)
        return;
    m_ViewmodelUp = CrossProduct(m_ViewmodelRight, m_ViewmodelForward);
    VectorNormalize(m_ViewmodelUp);
}

void VR::GetRightGlovePalmOffsetMeters(Vector& meters) const
{
    meters = Vector(0.f, 0.f, 0.f);
}

bool VR::WantsRightGloveVisible() const
{
    // Weapons (including crowbar): no right glove. Empty hands match the left.
    if (m_EmptyHands || !m_HasHeldWeapon)
        return true;
    return false;
}

bool VR::WantsRightGloveWeaponGripCurl() const
{
    return false;
}

namespace vm_numpad
{
    struct Offsets
    {
        float ox = 0.f;
        float oy = 0.f;
        float oz = 0.f;
        float ax = 0.f;
        float ay = 0.f;
        float az = 0.f;
    };

    std::map<std::string, Offsets> g_Off;
    bool g_Loaded = false;
    DWORD g_LastRepeatMs = 0;
    int g_LastVk = 0;

    std::wstring SavePath()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash);
        return dir + L"\\VR\\viewmodel_offsets.txt";
    }

    // Weapon key for viewmodel_offsets.txt: VR::ClassifyViewmodel fills
    // ViewmodelClass::numpadKey once per model change.

    void Load()
    {
        if (g_Loaded)
            return;
        g_Loaded = true;
        const std::wstring path = SavePath();
        std::ifstream in(path);
        if (!in)
            return;
        std::string line;
        int n = 0;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            char key[32]{};
            Offsets o{};
            const int nread = sscanf_s(line.c_str(), "%31s %f %f %f %f %f %f",
                key, static_cast<unsigned>(sizeof(key)),
                &o.ox, &o.oy, &o.oz, &o.ax, &o.ay, &o.az);
            if (nread >= 4)
            {
                g_Off[key] = o;
                ++n;
            }
        }
        Game::logMsg("Viewmodel numpad offsets loaded=%d %ls", n, path.c_str());
    }

    void Save()
    {
        const std::wstring path = SavePath();
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            Game::logMsg("Viewmodel numpad save failed %ls", path.c_str());
            return;
        }
        out << "# Numpad (gameplay): 4/6 Y  8/2 Z  7/9 X. Hold Ctrl for angles.\n";
        out << "# Numpad 5 save, 0 reset this weapon, Shift = fine step.\n";
        out << "# key ox oy oz ax ay az  (added on top of the built-in table)\n";
        for (const auto& kv : g_Off)
        {
            out << kv.first << " "
                << kv.second.ox << " " << kv.second.oy << " " << kv.second.oz << " "
                << kv.second.ax << " " << kv.second.ay << " " << kv.second.az << "\n";
        }
        Game::logMsg("Viewmodel numpad saved %d weapons %ls", static_cast<int>(g_Off.size()), path.c_str());
    }

    void ApplyKey(const std::string& key, float& ox, float& oy, float& oz, float& ax, float& ay, float& az)
    {
        Load();
        if (key.empty() || g_Off.empty())
            return;
        auto it = g_Off.find(key);
        if (it == g_Off.end())
            return;
        ox += it->second.ox;
        oy += it->second.oy;
        oz += it->second.oz;
        ax += it->second.ax;
        ay += it->second.ay;
        az += it->second.az;
    }

    bool KeyHeld(int vk)
    {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    bool RepeatEdge(int vk, DWORD now)
    {
        if (!KeyHeld(vk))
        {
            if (g_LastVk == vk)
                g_LastVk = 0;
            return false;
        }
        if (g_LastVk != vk)
        {
            g_LastVk = vk;
            g_LastRepeatMs = now;
            return true;
        }
        const DWORD delay = (now - g_LastRepeatMs > 400) ? 50u : 400u;
        if (now - g_LastRepeatMs >= delay)
        {
            g_LastRepeatMs = now;
            return true;
        }
        return false;
    }
}

void VR::UpdateViewmodelNumpadAdjust(bool paused)
{
    vm_numpad::Load();
    if (paused || !m_GameplayEligible)
        return;

    // ~30 GetAsyncKeyState syscalls per Present at 200+ Hz. Key edges only
    // need ~8 ms resolution, and the numpad is a keyboard tuning aid: poll it
    // only while the game window has keyboard focus.
    {
        static DWORD s_lastPollMs = 0;
        const DWORD pollNow = GetTickCount();
        if (s_lastPollMs != 0 && pollNow - s_lastPollMs < 8)
            return;
        s_lastPollMs = pollNow;
        HWND game = bmvr::GameWindow();
        if (game && GetForegroundWindow() != game)
            return;
    }

    // Aim pitch trim is global. Ctrl+Numpad+ shoots higher, Ctrl+Numpad-
    // lower; put the logged value in AimPitchOffset in VR/config.txt.
    {
        const DWORD nowMs = GetTickCount();
        const bool ctrlHeld = vm_numpad::KeyHeld(VK_CONTROL);
        const bool altHeld = vm_numpad::KeyHeld(VK_MENU);
        const float step = vm_numpad::KeyHeld(VK_SHIFT) ? 0.1f : 0.5f;
        float delta = 0.f;
        if (ctrlHeld && !altHeld && vm_numpad::RepeatEdge(VK_ADD, nowMs))
            delta = step;
        else if (ctrlHeld && !altHeld && vm_numpad::RepeatEdge(VK_SUBTRACT, nowMs))
            delta = -step;
        if (delta != 0.f)
        {
            bmvr::g_AimPitchOffset += delta;
            Game::logMsg("Aim pitch trim %+.2f deg (put AimPitchOffset=%.2f in VR/config.txt)",
                delta, bmvr::g_AimPitchOffset);
        }
    }

    std::string key;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        key = m_VmClass.numpadKey;
    }
    if (key.empty() || m_EmptyHands)
        return;

    const DWORD now = GetTickCount();
    const bool ctrl = vm_numpad::KeyHeld(VK_CONTROL);
    const bool alt = vm_numpad::KeyHeld(VK_MENU);
    if (alt)
        return;
    const bool fine = vm_numpad::KeyHeld(VK_SHIFT);
    const float pstep = fine ? 0.15f : 0.5f;
    const float astep = fine ? 0.5f : 2.0f;
    vm_numpad::Offsets& o = vm_numpad::g_Off[key];
    bool changed = false;
    auto bump = [&](int vk, float* dst, float delta) {
        if (!vm_numpad::RepeatEdge(vk, now))
            return;
        *dst += delta;
        changed = true;
    };
    if (ctrl)
    {
        bump(VK_NUMPAD8, &o.ax, astep);
        bump(VK_UP, &o.ax, astep);
        bump(VK_NUMPAD2, &o.ax, -astep);
        bump(VK_DOWN, &o.ax, -astep);
        bump(VK_NUMPAD6, &o.ay, astep);
        bump(VK_RIGHT, &o.ay, astep);
        bump(VK_NUMPAD4, &o.ay, -astep);
        bump(VK_LEFT, &o.ay, -astep);
        bump(VK_NUMPAD9, &o.az, astep);
        bump(VK_PRIOR, &o.az, astep);
        bump(VK_NUMPAD7, &o.az, -astep);
        bump(VK_HOME, &o.az, -astep);
    }
    else
    {
        bump(VK_NUMPAD9, &o.ox, pstep);
        bump(VK_PRIOR, &o.ox, pstep);
        bump(VK_NUMPAD7, &o.ox, -pstep);
        bump(VK_HOME, &o.ox, -pstep);
        bump(VK_NUMPAD6, &o.oy, pstep);
        bump(VK_RIGHT, &o.oy, pstep);
        bump(VK_NUMPAD4, &o.oy, -pstep);
        bump(VK_LEFT, &o.oy, -pstep);
        bump(VK_NUMPAD8, &o.oz, pstep);
        bump(VK_UP, &o.oz, pstep);
        bump(VK_NUMPAD2, &o.oz, -pstep);
        bump(VK_DOWN, &o.oz, -pstep);
    }
    if (vm_numpad::RepeatEdge(VK_NUMPAD0, now) || vm_numpad::RepeatEdge(VK_INSERT, now))
    {
        o = {};
        changed = true;
        Game::logMsg("Viewmodel numpad reset %s", key.c_str());
    }
    static bool s_saveDown = false;
    const bool saveHeld = vm_numpad::KeyHeld(VK_NUMPAD5) || vm_numpad::KeyHeld(VK_CLEAR);
    if (saveHeld && !s_saveDown)
    {
        vm_numpad::Save();
        Game::logMsg("Viewmodel numpad %s extra pos=(%.2f,%.2f,%.2f) ang=(%.2f,%.2f,%.2f) saved",
            key.c_str(), o.ox, o.oy, o.oz, o.ax, o.ay, o.az);
    }
    s_saveDown = saveHeld;
    if (changed)
    {
        Game::logMsg("Viewmodel numpad %s extra pos=(%.2f,%.2f,%.2f) ang=(%.2f,%.2f,%.2f)%s",
            key.c_str(), o.ox, o.oy, o.oz, o.ax, o.ay, o.az,
            ctrl ? " [ang]" : "");
    }
}

bool VR::GetDefaultRightHandWeaponCurls(float curls[5]) const
{
    if (!curls || !IsCrowbarEquipped())
        return false;
    // HL2VR closedPose fist, clenched tight on the crowbar handle.
    curls[0] = 0.88f;
    curls[1] = 0.96f;
    curls[2] = 0.94f;
    curls[3] = 0.93f;
    curls[4] = 0.92f;
    return true;
}

void VR::GetRightHandAttach(Vector& posMeters, Vector& rotDeg) const
{
    posMeters = Vector(0.f, 0.f, 0.f);
    rotDeg = Vector(0.f, 0.f, 0.f);
}

bool VR::GetRightHandAttachCurls(float curls[5]) const
{
    if (!curls)
        return false;
    for (int i = 0; i < 5; ++i)
        curls[i] = -1.f;
    return false;
}

bool VR::RightHandAttachActive() const
{
    return false;
}

void VR::ApplyViewmodelNumpadExtras(float& ox, float& oy, float& oz, float& ax, float& ay, float& az) const
{
    std::string key;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        key = m_VmClass.numpadKey;
    }
    vm_numpad::ApplyKey(key, ox, oy, oz, ax, ay, az);
}

void VR::ResolveWeaponViewmodelPose(float& ox, float& oy, float& oz, float& ax, float& ay, float& az) const
{
    uint32_t family = L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
    std::string key;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        // Built-in pose/angle table: see ClassifyViewmodel.
        ox = m_VmClass.ox;
        oy = m_VmClass.oy;
        oz = m_VmClass.oz;
        ax = m_VmClass.ax;
        ay = m_VmClass.ay;
        az = m_VmClass.az;
        family = m_ControllerFamily;
        key = m_VmClass.numpadKey;
    }
    ox += bmvr::g_ViewmodelPosOffsetX;
    oy += bmvr::g_ViewmodelPosOffsetY;
    oz += bmvr::g_ViewmodelPosOffsetZ;
    if (family == L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH)
    {
        ox += bmvr::g_ViewmodelPosOffsetXTouch;
        static bool s_loggedTouchOx;
        if (!s_loggedTouchOx)
        {
            Game::logMsg(
                "Touch viewmodel extra ox=%.1f (OpenXR aim origin is ahead of grip)",
                bmvr::g_ViewmodelPosOffsetXTouch);
            s_loggedTouchOx = true;
        }
    }
    ax += bmvr::g_ViewmodelAngOffsetX;
    ay += bmvr::g_ViewmodelAngOffsetY;
    az += bmvr::g_ViewmodelAngOffsetZ;
    vm_numpad::ApplyKey(key, ox, oy, oz, ax, ay, az);
}

void VR::PulseAimHaptic(unsigned short durationUs)
{
    PulseHandHaptic(AimControllerRole(), durationUs, 0.6f);
}

void VR::ApplyVrQualityOfLifeCvars()
{
    if (m_VrCvarsApplied || !m_Game)
        return;
    m_VrCvarsApplied = true;
    int n = 0;
    auto seti = [&](const char* name, int v) {
        if (m_Game->SetConVarInt(name, v))
            ++n;
    };
    auto setf = [&](const char* name, float v) {
        if (m_Game->SetConVarFloat(name, v))
            ++n;
    };
    if (bmvr::g_HideCrosshair)
        seti("crosshair", 0);
    setf("mat_grain_scale_override", 0.f);
    seti("engine_no_focus_sleep", 0);
    seti("mat_vsync", 0);
    seti("mat_triplebuffered", 0);
    seti("mat_motion_blur_enabled", 0);
    seti("mat_motion_blur_forward_enabled", 0);
    setf("mat_motion_blur_strength", 0.f);
    setf("mat_motion_blur_percent_of_screen_max", 0.f);
    setf("mat_motion_blur_rotation_intensity", 0.f);
    setf("mat_motion_blur_falling_intensity", 0.f);
    // fps_max 0: do not cap at HMD Hz (was 90 on G2). WaitGetPoses still
    // paces the compositor. MatchHmdHz no longer writes fps_max.
    if (m_Game->SetConVarInt("fps_max", 0) || m_Game->SetConVarFloat("fps_max", 0.f))
    {
        ++n;
        m_MenuFpsMaxLastHz = 0;
        Game::logMsg("fps_max 0 (uncapped; compositor syncs via WaitGetPoses)");
    }
    if (bmvr::g_DisableViewBob)
    {
        setf("cl_bob", 0.f);
        setf("cl_bobcycle", 0.f);
        setf("cl_bobup", 0.f);
        setf("cl_viewmodel_lag", 0.f);
        seti("r_jiggle_bones", 0);
    }
    // These gate only CBasePlayer::ViewPunch, the legacy m_vecPunchAngleVel
    // spring behind damage flinch and env_viewpunch. That angle is summed into
    // the server's shot direction, so suppressing it keeps a hit from throwing
    // aim off. It is not what makes MP5 fire climb: Black Mesa's weapon recoil
    // runs through AddRecoil, which reads no cvar at all. The climb is removed
    // in dGetShootAngles instead. Raw ConVar write; FCVAR_CHEAT does not block.
    seti("sv_suppress_viewpunch", 1);
    setf("sv_viewpunch_spring_constant", 0.f);
    // Two stereo copies can latch areaportals. Do not force r_portalsopenall —
    // that draws every leaf and tanks FPS. ForceOpenVis=true is the old
    // occlusion-off + portals-open pair. Do not zero CViewRender+0x744
    // (freeze-frame, not vis).
    if (bmvr::g_ForceOpenVis)
    {
        seti("r_occlusion", 0);
        seti("r_fastzreject", 0);
        seti("r_visocclusion", 0);
        seti("r_portalsopenall", 1);
    }
    else
    {
        seti("r_occlusion", 1);
        seti("r_fastzreject", 1);
        seti("r_visocclusion", 0);
        seti("r_portalsopenall", 0);
    }
    // Do not touch r_flashlightdepthtexture — that broke deferred flashlight.
    // Do not write video-quality ARCHIVE cvars (cl_csm_qualitymode,
    // nr_shadow_*, nr_lights_quality, r_shadowrendertotexture). The Surface
    // Tension potato set (quality 0 / 2048 / one pass) was a perf experiment
    // and it saved into config.cfg, so the video menu reset to Low every
    // launch. Leave those to the user's video options.
    // Verified unplayable on bm_c2a5a (2026-08-28): cl_csm_enabled 0,
    // nr_shadow_active 0, nr_dev_gb_debug_type 1, r_lod 2. Keep those off
    // so leftover ARCHIVE from that test cannot come back.
    seti("cl_csm_enabled", 1);
    seti("cl_csm_cascade_dynamic_ignore", 0);
    seti("nr_shadow_active", 1);
    seti("nr_dev_gb_debug_type", 2);
    seti("nr_lights_procedural_disable_all_lights", 0);
    seti("nr_dev_shoot_lights_enabled", 1);
    seti("gb_flashlight_shadow_enabled", 1);
    seti("cl_dlight_manager_enable", 1);
    seti("r_dynamic", 1);
    seti("r_maxdlights", 32);
    seti("r_lod", -1);
    seti("r_rootlod", 0);
    seti("r_eyes", 1);
    seti("r_teeth", 1);
    seti("flex_smooth", 1);
    seti("r_drawflecks", 1);
    seti("mat_reduceparticles", 0);
    seti("cl_new_impact_effects", 1);
    seti("nr_allow_hammer_nerfs", 0);
    seti("nr_allow_hammer_nerfs_4ways", 0);
    seti("nr_gbuffer_for_refraction_enabled", 0);
    seti("r_WaterDrawRefraction", 1);
    // Refraction-only (user 2026-09-06: no FPS cost, better surface). Full
    // water (reflection + entity reflect + expensive + gbuffer reflection)
    // was the 47→32 FPS regression vs GitHub. Planar water uses the HMD
    // camera inside each eye ViewDrawScene. Do not retry that full set.
    // Keep nr_gbuffer_for_refraction off (wall color-buffer stamps).
    seti("r_WaterDrawReflection", 0);
    seti("r_waterforcereflectentities", 0);
    seti("r_waterforceexpensive", 0);
    seti("nr_gbuffer_for_reflection_enabled", 0);
    // Undo the ghosting A/B that forced these to 0 (may have archived).
    seti("np_gr_quality", 1);
    setf("np_gr_weight", 1.f);
    setf("np_gr_exposure", 1.f);
    Game::logMsg("VR QoL cvars applied (%d ok) icvar=%p hideCrosshair=%d bobOff=%d forceOpenVis=%d blitFlush=%d waterrefract1 noVideoQualityOverride",
        n, m_Game->m_Cvar, bmvr::g_HideCrosshair ? 1 : 0, bmvr::g_DisableViewBob ? 1 : 0,
        bmvr::g_ForceOpenVis ? 1 : 0, bmvr::g_StereoBlitGpuFlush ? 1 : 0);
}

void VR::TickMatQueueFromRenderView()
{
    if (m_Game)
        m_Game->ProbeMatQueueModeFromRenderView();
    ApplyVrQualityOfLifeCvars();
    // QoL SetValue(mat_vsync) crashed the first RenderView (2026-08-26).
    // Crosshair stays in bmvr.cfg. Only mat_queue_mode is written after warmup.
    UpdateAutoMatQueueMode();
}

void VR::UpdateAutoMatQueueMode()
{
    if (!m_IsVREnabled || !m_Game)
        return;

    const int current = m_Game->GetMatQueueMode();
    static int s_qlog;
    if (s_qlog < 8)
    {
        Game::logMsg("GetMatQueueMode=%d vtableOk=%d slots=%d auto=%d multicore=%d ring=%d try=%d set=%d get=%d eq=%d",
            current, m_Game->MaterialVTableMatchesDump() ? 1 : 0,
            m_Game->MaterialThreadSlotsValid() ? 1 : 0,
            bmvr::g_AutoMatQueueMode ? 1 : 0, bmvr::g_MulticoreMode ? 1 : 0, m_McRingReady ? 1 : 0,
            bmvr::TryMatQueue() ? 1 : 0,
            m_Game->m_MatSetThreadSlot, m_Game->m_MatGetThreadSlot,
            m_Game->m_MatExecuteQueuedSlot);
        ++s_qlog;
    }

    if (current == 2 && m_GameplayEligible)
    {
        ++m_MatQueueOkPresents;
        if (m_MatQueueOkPresents == 120)
            bmvr::EndRisky(L"mat_queue");
    }

    const bool inGame = m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    if (m_MenuFpsMaxLastHz != 0)
    {
        if (m_Game->SetConVarInt("fps_max", 0) || m_Game->SetConVarFloat("fps_max", 0.f))
        {
            m_MenuFpsMaxLastHz = 0;
            Game::logMsg("fps_max 0 (uncapped inGame=%d)", inGame ? 1 : 0);
        }
    }

    if (!bmvr::TryMatQueue())
        return;

    const bool paused = SehIsPaused(m_Game->m_EngineClient);
    bool hasLocalPlayer = false;
    if (inGame && m_Game->m_EngineClient)
    {
        const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        hasLocalPlayer = playerIndex > 0 && m_Game->GetClientEntity(playerIndex) != nullptr;
    }
    const bool loadingMap = inGame && !hasLocalPlayer;

    int desired = 0;
    const bool autoQueue = bmvr::g_AutoMatQueueMode
        || (bmvr::g_MulticoreMode && m_McRingReady && m_OpenXrHelperBridgeActive);
    if (autoQueue)
    {
        const bool warmup = !PassThroughWarmupDone();
        desired = (!inGame || !m_GameplayEligible || paused || loadingMap || warmup) ? 0 : 2;
    }

    static bool s_forcedSingleThreadOnce;
    if (current == desired)
    {
        if (bmvr::g_AutoMatQueueMode || bmvr::g_MulticoreMode || s_forcedSingleThreadOnce)
        {
            m_AutoMatQueueModeLastRequested = desired;
            return;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const float since = (m_AutoMatQueueModeLastCmdTime.time_since_epoch().count() == 0)
        ? 9999.f
        : std::chrono::duration<float>(now - m_AutoMatQueueModeLastCmdTime).count();
    const float retrySec = (desired == 2) ? 5.f : 0.5f;
    if (m_AutoMatQueueModeLastRequested == desired && since < retrySec)
        return;

    if (desired == 2 && m_AutoMatQueueModeLastRequested != 2)
        bmvr::BeginRisky(L"mat_queue");

    if (desired != current)
        ResetMcPipeline();

    const bool ok = m_Game->SetMatQueueMode(desired);
    m_AutoMatQueueModeLastRequested = desired;
    m_AutoMatQueueModeLastCmdTime = now;
    if (desired == 0 && ok)
        s_forcedSingleThreadOnce = true;
    if (desired == 2 && !ok)
        bmvr::EndRisky(L"mat_queue");
    const char* reason = "in-game";
    if (!inGame) reason = "menu";
    else if (!m_GameplayEligible) reason = "loading";
    else if (loadingMap) reason = "no-local-player";
    else if (!PassThroughWarmupDone()) reason = "pass-through";
    else if (paused) reason = "paused";
    Game::logMsg("AutoMatQueueMode set %d (was %d) ok=%d reason=%s multicore=%d ring=%d",
        desired, current, ok ? 1 : 0, reason, bmvr::g_MulticoreMode ? 1 : 0, m_McRingReady ? 1 : 0);
}

void VR::TryApplySteamVrRecommendedEyeSize()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    vr::IVRSystem* sys = m_System ? m_System : vr::VRSystem();
    if (!sys)
        return;
    uint32_t recW = 0, recH = 0;
    sys->GetRecommendedRenderTargetSize(&recW, &recH);
    if (recW < 640 || recH < 360)
        return;
    if (recW > 4096)
        recW = 4096;
    if (recH > 4096)
        recH = 4096;
    if (recW == bmvr::g_RecommendedEyeWidth && recH == bmvr::g_RecommendedEyeHeight)
        return;
    const uint32_t prevW = bmvr::g_RecommendedEyeWidth;
    const uint32_t prevH = bmvr::g_RecommendedEyeHeight;
    bmvr::g_RecommendedEyeWidth = recW;
    bmvr::g_RecommendedEyeHeight = recH;
    Game::logMsg("SteamVR recommended RT %ux%u (was %ux%u)", recW, recH, prevW, prevH);
    if (prevW >= 640 && prevH >= 360
        && (recW + 32 < prevW || recH + 32 < prevH || prevW + 32 < recW || prevH + 32 < recH))
        m_EyeResizeSettleMs = GetTickCount();
}

void VR::PollSteamVrRecommendedSize()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    // Reached from EnsureStereoEyeSurfaces on every in-game Present.
    // GetRecommendedRenderTargetSize is a vrclient IPC round trip and the
    // SteamVR per-app resolution changes at human speed; 4 Hz is enough
    // (eye resize already waits a 300 ms settle window).
    static DWORD s_lastPollMs = 0;
    const DWORD nowMs = GetTickCount();
    if (bmvr::g_RecommendedEyeWidth >= 640 && s_lastPollMs != 0 && nowMs - s_lastPollMs < 250)
        return;
    s_lastPollMs = nowMs ? nowMs : 1;
    TryApplySteamVrRecommendedEyeSize();
}

void VR::ReclaimCompositorFocus(const char* reason)
{
    if (!m_Compositor)
        return;
    const DWORD now = GetTickCount();
    if (m_LastCompositorReclaimMs != 0 && (now - m_LastCompositorReclaimMs) < 1000)
        return;
    m_LastCompositorReclaimMs = now;
    m_Compositor->CompositorBringToFront();
    m_Compositor->SetExplicitTimingMode(m_CompositorAppHandoff
        ? vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff
        : vr::VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff);
    m_Compositor->ForceInterleavedReprojectionOn(false);
    Game::logMsg("Compositor reclaim (%s) canRender=%d poseErr=%d",
        reason ? reason : "?",
        m_Compositor->CanRenderScene() ? 1 : 0,
        m_LastPoseWaitError.load(std::memory_order_acquire));
}

void VR::TickCompositorFocus()
{
    if (!m_Compositor)
        return;
    // CanRenderScene + GetTrackedDeviceActivityLevel are two vrclient IPC
    // round trips. Focus changes are human-speed; 10 Hz is plenty and keeps
    // them off the per-Present critical path.
    static DWORD s_lastTickMs = 0;
    const DWORD nowMs = GetTickCount();
    if (s_lastTickMs != 0 && nowMs - s_lastTickMs < 100)
        return;
    s_lastTickMs = nowMs ? nowMs : 1;
    const bool can = m_Compositor->CanRenderScene();
    if (m_LastCanRenderScene && !can)
        Game::logMsg("Compositor lost scene focus (alt-tab / dashboard)");
    if (!m_LastCanRenderScene && can)
        ReclaimCompositorFocus("CanRenderScene restored");
    m_LastCanRenderScene = can;

    vr::EDeviceActivityLevel activity = vr::k_EDeviceActivityLevel_Unknown;
    if (m_System)
        activity = m_System->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
    const bool hmdActive = activity == vr::k_EDeviceActivityLevel_UserInteraction
        || activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;
    const int poseErr = m_LastPoseWaitError.load(std::memory_order_acquire);
    if (hmdActive && (!can || poseErr == static_cast<int>(vr::VRCompositorError_DoNotHaveFocus)))
        ReclaimCompositorFocus("HMD active without scene focus");
}

void VR::NoteEngineScopeFov(float engineFov)
{
    m_EngineViewFov = engineFov;
    if (!ViewmodelIsScoped() || !bmvr::g_ScopeUsesHmdAim)
        return;
    // Black Mesa writes the 2D zoom FOV (~20) into CViewSetup. Once we have
    // seen that, trust it to turn VR magnification off as well — a stuck
    // m_bZooming / latch cannot keep the HMD zoomed after unscope.
    if (engineFov > 10.f && engineFov < 45.f)
    {
        if (!m_SawEngineZoomFov)
            Game::logMsg("Engine scope FOV %.1f — VR zoom will follow CViewSetup", engineFov);
        m_SawEngineZoomFov = true;
        m_ScopeZoomActive = true;
        m_CrossbowZoomLatched = true;
        return;
    }
    if (m_SawEngineZoomFov && engineFov >= 55.f)
    {
        if (m_ScopeZoomActive)
            Game::logMsg("Engine unscoped FOV %.1f — dropping VR zoom", engineFov);
        m_ScopeZoomActive = false;
        m_CrossbowZoomLatched = false;
    }
}

void VR::NoteStereoClipPlanes(float zNear, float zFar)
{
    if (zNear > 0.01f && zFar > zNear + 1.f)
    {
        m_StereoZNear = zNear;
        m_StereoZFar = zFar;
    }
}

void VR::NoteFlashlightState(const Vector& origin, const Vector& forward)
{
    if (!std::isfinite(origin.x) || !std::isfinite(forward.x))
        return;
    if (origin.LengthSqr() < 1.f || forward.LengthSqr() < 0.01f)
        return;
    m_FlashlightOrigin = origin;
    m_FlashlightForward = forward;
    const float len = m_FlashlightForward.Length();
    if (len > 0.001f)
        m_FlashlightForward = m_FlashlightForward * (1.f / len);
    m_FlashlightLive = true;
}

bool VR::CopyFlashlightState(Vector& origin, Vector& forward) const
{
    if (!m_FlashlightLive)
        return false;
    origin = m_FlashlightOrigin;
    forward = m_FlashlightForward;
    return true;
}

float VR::WorldRenderFov() const
{
    if (!m_ScopeZoomActive)
        return m_Fov;
    float scale = bmvr::g_ScopeZoomFovScale;
    if (!(scale > 0.05f) || scale > 1.f)
        scale = 0.28f;
    float fov = m_Fov * scale;
    if (fov < 15.f)
        fov = 15.f;
    if (fov > m_Fov)
        fov = m_Fov;
    return fov;
}

float VR::WorldRenderFovForEye(bool left) const
{
    (void)left;
    return WorldRenderFov();
}

float VR::HorizontalFovForAspect(float targetAspect) const
{
    const float srcFov = WorldRenderFov();
    if (!(srcFov > 10.f) || !(m_Aspect > 0.1f) || !(targetAspect > 0.1f))
        return srcFov;
    const float halfRad = srcFov * 0.5f * (3.14159265358979323846f / 180.0f);
    const float tanHalfY = tanf(halfRad) / m_Aspect;
    return 2.0f * atanf(tanHalfY * targetAspect) * (180.0f / 3.14159265358979323846f);
}

void VR::BeginStereoFramePose()
{
    m_VrGlovesDrawnIntoScene = false;
    if (m_OpenXrHelperBridgeActive)
        ConsumeOpenXrTracking();
    UpdateTracking();
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        m_StereoFrameAngles = m_HmdAngAbs;
        m_StereoFrameHmdPosAbs = m_HmdPosAbs;
        m_StereoFrameTrackingPose = m_HmdTrackingPose;
        m_StereoFrameTrackingPoseValid = m_HmdPoseValid;
        m_StereoFrameLeftCtrlValid = m_LeftControllerTrackingValid;
        m_StereoFrameRightCtrlValid = m_PhysicalRightTrackingValid;
        m_StereoFrameLeftCtrlAng = m_LeftControllerAngAbs;
        m_StereoFrameRightCtrlAng = m_PhysicalRightAngAbs;
        m_StereoFrameLeftCtrlPos = m_LeftControllerPosAbs;
        m_StereoFrameRightCtrlPos = m_PhysicalRightPosAbs;
    }
    m_StereoFramePoseActive = true;
    UpdateScopeZoomSmooth();
    if (m_OpenXrHelperBridgeActive)
    {
        m_OpenXrStereoRenderPose = m_OpenXrLastHmdPose;
        m_OpenXrStereoRenderPoseValid = m_OpenXrLastHmdPose.valid != 0;
        const float renderIpd = m_Ipd * m_IpdScale;
        if (renderIpd >= 0.04f && renderIpd <= 0.10f)
            std::memcpy(&m_OpenXrStereoRenderPose.reserved0, &renderIpd, sizeof(renderIpd));
        static int s_stereoPoseLog;
        if (s_stereoPoseLog < 4)
        {
            Game::logMsg("Stereo frame pose latch openxr=%d pos=(%.3f %.3f %.3f) ang=(%.1f %.1f)",
                m_OpenXrStereoRenderPoseValid ? 1 : 0,
                m_OpenXrStereoRenderPose.position[0],
                m_OpenXrStereoRenderPose.position[1],
                m_OpenXrStereoRenderPose.position[2],
                m_StereoFrameAngles.x, m_StereoFrameAngles.y);
            ++s_stereoPoseLog;
        }
    }
}

void VR::EndStereoFramePose()
{
    m_StereoFramePoseActive = false;
}

void VR::LogOpenXrPublishRate()
{
    const uint32_t now = GetTickCount();
    if (m_OpenXrPublishRateTick == 0)
    {
        m_OpenXrPublishRateTick = now;
        return;
    }
    const uint32_t dt = now - m_OpenXrPublishRateTick;
    if (dt < 5000)
        return;
    const float scale = 1000.f / static_cast<float>(dt);
    const uint32_t latestFrame =
        m_OpenXrLastPublishedSharedTextureFrameId.load(std::memory_order_relaxed);
    Game::logMsg("OpenXR publish rate present=%.0f/s published=%.0f/s skipStale=%.0f/s skipRate=%.0f/s slots=%u copy=%d "
        "deferred=%.0f/s throttled=%.0f/s helperHold=%.0f/s dropped=%.0f/s pending=%u/%u maxPending=%u "
        "paceWait=%.0f/s avg=%.2fms max=%.2fms helperFeedback=%d consumedLag=%d",
        m_OpenXrSubmitAttempts * scale, m_OpenXrPublishes * scale,
        m_OpenXrSkippedNoNewFrame * scale, m_OpenXrSkippedHelperBusy * scale,
        kOpenXrPublishSlots, m_OpenXrPublishActive ? 1 : 0,
        m_OpenXrDeferredPublishes * scale, m_OpenXrDeferredThrottles * scale,
        m_OpenXrDeferredHelperHolds * scale,
        m_OpenXrDeferredDropped * scale, m_OpenXrPendingCount, bmvr::g_OpenXrMaxPending,
        m_OpenXrDeferredMaxPending,
        m_OpenXrPaceWaits * scale,
        m_OpenXrPaceWaits ? m_OpenXrPaceWaitMs / m_OpenXrPaceWaits : 0.0,
        m_OpenXrPaceWaitMaxMs,
        m_OpenXrHelperFeedbackLive ? 1 : 0,
        m_OpenXrHelperFeedbackLive
            ? static_cast<int>(latestFrame - 1 - m_OpenXrHelperConsumedFrameId) : -1);
    m_OpenXrPublishRateTick = now;
    m_OpenXrSubmitAttempts = 0;
    m_OpenXrPublishes = 0;
    m_OpenXrSkippedNoNewFrame = 0;
    m_OpenXrSkippedHelperBusy = 0;
    m_OpenXrDeferredPublishes = 0;
    m_OpenXrDeferredThrottles = 0;
    m_OpenXrDeferredHelperHolds = 0;
    m_OpenXrDeferredDropped = 0;
    m_OpenXrDeferredMaxPending = 0;
    m_OpenXrPaceWaits = 0;
    m_OpenXrPaceWaitMs = 0.0;
    m_OpenXrPaceWaitMaxMs = 0.0;
}

void VR::WaitPosesForStereoFrame(bool allowQueued)
{
    if (m_PosesWaitedThisFrame)
        return;
    if (m_OpenXrHelperBridgeActive)
    {
        ConsumeOpenXrTracking();
        UpdateTracking();
        m_PosesWaitedThisFrame = true;
        return;
    }
    if (!m_Compositor)
    {
        m_PosesWaitedThisFrame = true;
        return;
    }

    // HL2VR VRManager::OnFrameBegin (vrmanager.cpp): if queued, wait for the
    // previous WaitGetPoses, queue this frame's WGP, render with predicted.
    // If GetCallQueue is null, WaitGetPoses on this thread.
    const bool wantQueued = allowQueued
        && bmvr::TryHl2vrQueuedWgp()
        && m_Game && m_Game->GetMatQueueMode() != 0;
    if (wantQueued)
        SyncFrameGetPoses(m_Hl2vrFrameCounter);
    ++m_Hl2vrFrameCounter;
    bool queued = false;
    if (wantQueued)
        queued = QueueMatAwaitFrame(m_Hl2vrFrameCounter);
    if (!queued)
        MatAwaitFrame(m_Hl2vrFrameCounter);
    ApplyHl2vrWaitedPoses(queued);
    UpdateTracking();
    m_PosesWaitedThisFrame = true;
}

void VR::ApplyHl2vrWaitedPoses(bool usePredicted)
{
    {
        std::lock_guard<std::mutex> lock(m_PoseMutex);
        const vr::TrackedDevicePose_t* src = usePredicted ? m_PredictedPoses : m_RenderPoses;
        std::memcpy(m_WaitedPoses, src, sizeof(m_WaitedPoses));
        const vr::TrackedDevicePose_t& hmd = m_WaitedPoses[vr::k_unTrackedDeviceIndex_Hmd];
        m_HmdTrackingPose = hmd.mDeviceToAbsoluteTracking;
    }
}

void VR::SyncFrameGetPoses(uint64_t frameId)
{
    // HL2VR HL2VRInterop::SyncFrameGetPoses: frame 0 is a no-op.
    if (frameId == 0)
        return;
    std::unique_lock<std::mutex> lock(m_Hl2vrFrameMutex);
    if (m_Hl2vrAwaitedId.load(std::memory_order_acquire) < frameId)
    {
        if (!m_Hl2vrFrameCv.wait_for(lock, std::chrono::milliseconds(500), [this, frameId] {
                return m_Hl2vrAwaitedId.load(std::memory_order_acquire) >= frameId;
            }))
        {
            static int s_syncTimeout;
            if (s_syncTimeout < 8)
            {
                Game::logMsg("HL2VR SyncFrameGetPoses timeout frame=%llu awaited=%llu",
                    (unsigned long long)frameId,
                    (unsigned long long)m_Hl2vrAwaitedId.load(std::memory_order_relaxed));
                ++s_syncTimeout;
            }
        }
    }
}

void VR::MatAwaitFrame(uint64_t frameId)
{
    if (!m_Compositor)
        return;

    const uint64_t prev = (frameId > 0) ? (frameId - 1) : 0;
    if (prev > 0)
    {
        std::unique_lock<std::mutex> lock(m_Hl2vrFrameMutex);
        if (m_Hl2vrHandoffId.load(std::memory_order_acquire) < prev)
        {
            if (!m_Hl2vrFrameCv.wait_for(lock, std::chrono::milliseconds(500), [this, prev] {
                    return m_Hl2vrHandoffId.load(std::memory_order_acquire) >= prev;
                }))
            {
                static int s_handoffSkip;
                if (s_handoffSkip < 6)
                {
                    Game::logMsg("HL2VR MatAwaitFrame: previous handoff missing, WaitGetPoses anyway frame=%llu",
                        (unsigned long long)frameId);
                    ++s_handoffSkip;
                }
            }
        }
    }

    vr::TrackedDevicePose_t renderPoses[vr::k_unMaxTrackedDeviceCount]{};
    vr::TrackedDevicePose_t predictedPoses[vr::k_unMaxTrackedDeviceCount]{};
    const DWORD t0 = GetTickCount();
    const vr::EVRCompositorError err = m_Compositor->WaitGetPoses(
        renderPoses, vr::k_unMaxTrackedDeviceCount,
        predictedPoses, vr::k_unMaxTrackedDeviceCount);
    const DWORD dt = GetTickCount() - t0;
    {
        std::lock_guard<std::mutex> lock(m_PoseMutex);
        std::memcpy(m_RenderPoses, renderPoses, sizeof(renderPoses));
        std::memcpy(m_PredictedPoses, predictedPoses, sizeof(predictedPoses));
    }
    m_LastPoseWaitError.store(static_cast<int>(err), std::memory_order_release);
    m_WaitedPoseTick.store(GetTickCount(), std::memory_order_release);
    m_PoseWaitCount.fetch_add(1, std::memory_order_relaxed);
    if (dt > 50)
        m_PoseWaitOvershootCount.fetch_add(1, std::memory_order_relaxed);
    if (m_ActionsReady.load(std::memory_order_acquire) && m_Input)
        m_Input->UpdateActionState(m_ActiveActionSets, sizeof(vr::VRActiveActionSet_t), 2);

    {
        std::lock_guard<std::mutex> lock(m_Hl2vrFrameMutex);
        m_Hl2vrAwaitedId.store(frameId, std::memory_order_release);
    }
    m_Hl2vrFrameCv.notify_all();

    static int s_wgpLog;
    if (s_wgpLog < 6 || dt > 100)
    {
        Game::logMsg("HL2VR WaitGetPoses frame=%llu err=%d dt=%ums valid=%d queuedSlot=%d",
            (unsigned long long)frameId, (int)err, dt,
            renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid ? 1 : 0,
            m_Hl2vrCallQueueSlot);
        ++s_wgpLog;
    }
}

ICallQueue* VR::ProbeCallQueue()
{
    if (!m_Game || !m_Game->m_MaterialSystem)
        return nullptr;
    IMatRenderContext* ctx = SehGetRenderContext(m_Game->m_MaterialSystem);
    if (!ctx)
        return nullptr;

    ICallQueue* q = nullptr;
    if (m_Hl2vrCallQueueSlot >= 0)
        q = SehCallQueueFromSlot(ctx, m_Hl2vrCallQueueSlot);
    else
    {
        const int slots[] = {
            Offsets::kIMatRenderContext_GetCallQueueSlotBmShift,
            Offsets::kIMatRenderContext_GetCallQueueSlotSource,
        };
        for (int slot : slots)
        {
            q = SehCallQueueFromSlot(ctx, slot);
            if (q)
            {
                m_Hl2vrCallQueueSlot = slot;
                break;
            }
        }
        if (!m_Hl2vrCallQueueProbed)
        {
            m_Hl2vrCallQueueProbed = true;
            Game::logMsg("HL2VR GetCallQueue probe slot=%d q=%p (146=Source, 185=BM+39)",
                m_Hl2vrCallQueueSlot, (void*)q);
        }
    }
    SehReleaseMatContext(ctx);
    return q;
}

bool VR::QueueMatAwaitFrame(uint64_t frameId)
{
    ICallQueue* q = ProbeCallQueue();
    if (!q)
        return false;
    auto* functor = new (std::nothrow) Hl2vrAwaitFrameFunctor(this, frameId);
    if (!functor)
        return false;
    functor->AddRef();
    if (m_Hl2vrQueuedWgpFrames == 0)
        bmvr::BeginRisky(L"hl2vr_wgp");
    if (!SehQueueFunctorInternal(q, functor))
    {
        functor->Release();
        Game::logMsg("HL2VR QueueFunctorInternal failed frame=%llu — sync WaitGetPoses",
            (unsigned long long)frameId);
        return false;
    }
    ++m_Hl2vrQueuedWgpFrames;
    if (m_Hl2vrQueuedWgpFrames == 120)
        bmvr::EndRisky(L"hl2vr_wgp");
    static int s_qLog;
    if (s_qLog < 4)
    {
        Game::logMsg("HL2VR queued WaitGetPoses frame=%llu slot=%d",
            (unsigned long long)frameId, m_Hl2vrCallQueueSlot);
        ++s_qLog;
    }
    return true;
}

bool VR::QueueMcMatFunctor(CFunctor* functor, const char* failTag)
{
    if (!functor)
        return false;
    ICallQueue* q = ProbeCallQueue();
    if (q && SehQueueFunctorInternal(q, functor))
        return true;

    // Quest Horizon Link: slot 146 is not a working ICallQueue
    // (QueueFunctorInternal throws, then GetCallQueue returns null).
    // McPipelineEnabled skips Present publish, so dropping the functor
    // froze the HMD on the last 2D menu frame. Disable the MC publish
    // path; hooks fall back to the single-thread 1x-eye blit.
    m_McCallQueueDead = true;
    static int s_dead;
    if (s_dead < 8)
    {
        Game::logMsg("MC %s: call queue %s slot=%d — single-thread stereo publish",
            failTag ? failTag : "functor",
            q ? "failed" : "missing",
            m_Hl2vrCallQueueSlot);
        ++s_dead;
    }
    functor->Release();
    return false;
}

bool VR::HudPaintActive() const
{
    return g_HudPaintActive;
}

void VR::SetHudPaintActive(bool active)
{
    g_HudPaintActive = active;
}

IDirect3DSurface9* VR::McPlaybackEyeSurface() const
{
    if (g_McPlayEye == 2)
        return m_D9RightEyeSurface;
    if (g_McPlayEye == 1)
        return m_D9LeftEyeSurface;
    return nullptr;
}

IDirect3DSurface9* VR::McPlaybackEyeDepth() const
{
    if (g_McPlayEye == 2)
        return m_D9RightEyeDepthSurface;
    if (g_McPlayEye == 1)
        return m_D9LeftEyeDepthSurface;
    return nullptr;
}

void VR::McBindPlaybackEye(int stereoEye, bool flushGpu)
{
    g_McPlayEye = (stereoEye == 2) ? 2 : 1;
    IDirect3DSurface9* dest = McPlaybackEyeSurface();
    BeginStereoEyeBlit(dest);
    ClearStereoEyeSurfaces();
    if (flushGpu)
    {
        if (g_D3DVR9)
            g_D3DVR9->FlushCommands();
        FlushStereoBlitGpu();
    }
    static int s_run;
    if (s_run < 8)
    {
        Game::logMsg("MC mat-thread bind 1x eye=%d flush=%d dest=%p",
            g_McPlayEye, flushGpu ? 1 : 0, (void*)dest);
        ++s_run;
    }
}

void VR::McCompositeEyesToRing(uint32_t band, const L4D2VROpenXrPoseDesc* pose)
{
    IDirect3DSurface9* left = m_D9LeftEyeSurface;
    IDirect3DSurface9* right = m_D9RightEyeSurface;
    if (!IsMenuUp()
        && (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes || bmvr::g_HandHud
            || WeaponMenuOpen() || AimCrosshairVisible()))
    {
        DrawIndependentHandMarkers(left, 1, true, true);
        DrawIndependentHandMarkers(right, 2, true, true);
    }

    OpenXrPendingPublish pend{};
    if (pose && pose->valid)
    {
        pend.pose = *pose;
        pend.havePose = true;
    }
    else if (m_OpenXrStereoRenderPoseValid)
    {
        pend.pose = m_OpenXrStereoRenderPose;
        pend.havePose = true;
    }
    else if (m_OpenXrLastHmdPose.valid)
    {
        pend.pose = m_OpenXrLastHmdPose;
        pend.havePose = true;
    }

    // Drop TLS eye redirect before copies. StretchRect dest must not steal
    // HWND onto the right eye.
    EndStereoEyeBlit();
    g_McPlayEye = 0;

    const bool copied = CopyEyesToOpenXrPublishSlot(left, right, pend);
    static int s_comp;
    if (s_comp < 8)
    {
        Game::logMsg("MC 1x eyes -> OpenXR publish slot copied=%d pose=%d band=%u",
            copied ? 1 : 0, pend.havePose ? 1 : 0, band);
        ++s_comp;
    }

    // Spectator after HMD publish so the EVENT wait does not include this
    // blit. Offscreen dest — not the swapchain (that clobbered the eye
    // viewport / stalled Present).
    if (!Want2dMenuPanel() && !WantPauseWorldOverlay())
        MirrorStereoToDesktopWindow();
}

bool VR::QueueMcEyeBind(int stereoEye, bool flushGpu)
{
    auto* functor = new (std::nothrow) McMatFunctor(
        this, McMatFunctor::Kind::Bind, stereoEye, 0, flushGpu);
    if (!functor)
        return false;
    functor->AddRef();
    if (!QueueMcMatFunctor(functor, "QueueMcEyeBind"))
        return false;
    static int s_qLog;
    if (s_qLog < 8)
    {
        Game::logMsg("MC queued 1x eye bind eye=%d flush=%d slot=%d",
            stereoEye, flushGpu ? 1 : 0, m_Hl2vrCallQueueSlot);
        ++s_qLog;
    }
    return true;
}

bool VR::QueueMcCompositeToRing(uint32_t band)
{
    const L4D2VROpenXrPoseDesc* pose = m_OpenXrStereoRenderPoseValid
        ? &m_OpenXrStereoRenderPose : nullptr;
    auto* functor = new (std::nothrow) McMatFunctor(
        this, McMatFunctor::Kind::Composite, 0, band, false, pose);
    if (!functor)
        return false;
    functor->AddRef();
    if (!QueueMcMatFunctor(functor, "QueueMcCompositeToRing"))
        return false;
    static int s_qLog;
    if (s_qLog < 8)
    {
        Game::logMsg("MC queued 1x-eye OpenXR copy band=%u slot=%d",
            band, m_Hl2vrCallQueueSlot);
        ++s_qLog;
    }
    return true;
}

bool VR::QueueMcPauseHudBegin()
{
    auto* functor = new (std::nothrow) McPauseHudFunctor(
        this, McPauseHudFunctor::Kind::Begin);
    if (!functor)
        return false;
    functor->AddRef();
    if (!QueueMcMatFunctor(functor, "QueueMcPauseHudBegin"))
        return false;
    static int s_qLog;
    if (s_qLog < 8)
    {
        Game::logMsg("MC queued pause HUD bind slot=%d", m_Hl2vrCallQueueSlot);
        ++s_qLog;
    }
    return true;
}

bool VR::QueueMcPauseHudEnd()
{
    auto* functor = new (std::nothrow) McPauseHudFunctor(
        this, McPauseHudFunctor::Kind::End);
    if (!functor)
        return false;
    functor->AddRef();
    if (!QueueMcMatFunctor(functor, "QueueMcPauseHudEnd"))
        return false;
    return true;
}

bool VR::RefreshBackBufferTexture(bool forceRefresh)
{
    if (!g_D3DVR9)
        return false;
    if (!forceRefresh && m_BackBufferTextureValid)
        return true;
    const HRESULT hr = g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);
    const UINT w = m_VKBackBuffer.m_VulkanData.m_nWidth;
    const UINT h = m_VKBackBuffer.m_VulkanData.m_nHeight;
    m_BackBufferTextureValid = SUCCEEDED(hr) && m_VKBackBuffer.m_VulkanData.m_nImage && w >= 640 && h >= 360;
    if (m_BackBufferTextureValid && (m_RenderWidth == 0 || m_RenderHeight == 0))
    {
        m_RenderWidth = w;
        m_RenderHeight = h;
        Game::logMsg("Backbuffer VR desc %ux%u fmt=%u", w, h, m_VKBackBuffer.m_VulkanData.m_nFormat);
    }
    else if (!m_BackBufferTextureValid)
    {
        static int s_stubLog;
        if (s_stubLog < 3)
        {
            Game::logMsg("GetBackBufferData stub/unresolved hr=0x%08X %ux%u image=%llu",
                (unsigned)hr, w, h, (unsigned long long)m_VKBackBuffer.m_VulkanData.m_nImage);
            ++s_stubLog;
        }
    }
    return m_BackBufferTextureValid;
}

bool VR::FillSharedTexture(IDirect3DSurface9* surface, SharedTextureHolder& holder)
{
    if (!g_D3DVR9 || !surface)
        return false;
    D3D9_TEXTURE_VR_DESC desc{};
    if (FAILED(g_D3DVR9->GetVRDesc(surface, &desc)) || !desc.Image)
        return false;
    std::memcpy(&holder.m_VulkanData, &desc, sizeof(holder.m_VulkanData));
    holder.m_VRTexture.handle = &holder.m_VulkanData;
    holder.m_VRTexture.eType = vr::TextureType_Vulkan;
    holder.m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
    holder.m_SharedHandle = desc.SharedHandle;
    holder.m_SharedHandleType = desc.SharedHandleType;
    holder.m_SharedHandleValid = desc.SharedHandleValid;
    return holder.m_VulkanData.m_nImage != 0;
}

IDirect3DSurface9* VR::SubmitSurfaceForEye(IDirect3DSurface9* eye) const
{
    if (eye && eye == m_D9LeftEyeSurface)
        return m_D9LeftEyeSubmitSurface;
    if (eye && eye == m_D9RightEyeSurface)
        return m_D9RightEyeSubmitSurface;
    return nullptr;
}

bool VR::D3dRt0IsEyeSized() const
{
    if (m_RenderWidth < 640 || m_RenderHeight < 360 || !g_D3DVR9)
        return false;
    // Fast path: RT0 as tracked by HookedSetRenderTarget. Same answer as the
    // GetRenderTarget+GetDesc query below without three locked COM calls per
    // GetScreenSize / SetViewport / Clear.
    if (g_Rt0ActualKnown)
    {
        IDirect3DSurface9* rt = g_Rt0ActualPtr;
        if (!rt)
            return false;
        if (rt == m_StereoEyeBlitDest || rt == m_D9LeftEyeSurface || rt == m_D9RightEyeSurface
            || SurfaceIsMcRing(rt))
            return true;
        return g_Rt0ActualW == m_RenderWidth && g_Rt0ActualH == m_RenderHeight;
    }
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;
    IDirect3DSurface9* rt = nullptr;
    bool world = false;
    if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt)
    {
        D3DSURFACE_DESC desc{};
        const bool haveDesc = SUCCEEDED(rt->GetDesc(&desc));
        if (rt == m_StereoEyeBlitDest || rt == m_D9LeftEyeSurface || rt == m_D9RightEyeSurface
            || SurfaceIsMcRing(rt))
            world = true;
        else if (haveDesc)
            world = desc.Width == m_RenderWidth && desc.Height == m_RenderHeight;
        // Seed the tracker so later calls take the fast path.
        g_Rt0ActualPtr = rt;
        g_Rt0ActualW = haveDesc ? desc.Width : 0;
        g_Rt0ActualH = haveDesc ? desc.Height : 0;
        g_Rt0ActualKnown = true;
        rt->Release();
    }
    device->Release();
    return world;
}

bool VR::CachedRt0MatchesEyes() const
{
    return m_RenderWidth >= 640 && m_RenderHeight >= 360
        && g_Rt0W == m_RenderWidth && g_Rt0H == m_RenderHeight;
}

void VR::NoteCachedRt0Size(UINT w, UINT h)
{
    g_Rt0W = w;
    g_Rt0H = h;
}

bool VR::McPipelineEnabled() const
{
    if (!bmvr::g_MulticoreMode || !bmvr::TryMatQueue())
        return false;
    if (!m_McRingReady || !m_OpenXrHelperBridgeActive)
        return false;
    if (!m_IsVREnabled || !m_GameplayEligible)
        return false;
    if (!PassThroughWarmupDone())
        return false;
    if (!m_Game || m_Game->GetMatQueueMode() == 0)
        return false;
    if (Want2dMenuPanel())
        return false;
    if (m_McCallQueueDead)
        return false;
    return true;
}

bool VR::StereoWorldPassActive() const
{
    if (m_StereoEye != 0)
        return true;
    if (McPlaybackEyeSurface())
        return true;
    if (!McPipelineEnabled() && StereoEyeBlitActive())
        return true;
    return false;
}

bool VR::SurfaceIsMcRing(IDirect3DSurface9* s) const
{
    return s && m_D9McRingSurface && s == m_D9McRingSurface;
}

bool VR::D3dRt0IsMcRing() const
{
    return g_Rt0ActualKnown && SurfaceIsMcRing(g_Rt0ActualPtr);
}

bool VR::McOriginIsRingBand(int x, int y) const
{
    const int w = static_cast<int>(m_RenderWidth);
    const int h = static_cast<int>(m_RenderHeight);
    if (w < 640 || h < 360)
        return false;
    const int leftX = McLeftOriginX();
    const int rightX = McRightOriginX();
    if (!(std::abs(x - leftX) <= 8 || std::abs(x - rightX) <= 8))
        return false;
    for (int b = 0; b < 3; ++b)
    {
        if (std::abs(y - b * h) <= 8)
            return true;
    }
    return false;
}

bool VR::McClipAmbiguousWindowVp(int inX, int inY, int inW, int inH, int& x, int& y, int& w, int& h) const
{
    if (!McPipelineEnabled() || McOnRecordThread())
        return false;
    if (inX > 8 || inY > 8)
        return false;
    const int eyeW = static_cast<int>(m_RenderWidth);
    const int eyeH = static_cast<int>(m_RenderHeight);
    // Eye-sized @0,0 is deferred lighting / flashlight apply (log:
    // 3168x3104 @0,0). Clipping those to 1x1 left unlit world on the ring
    // and the lit frames z-fought through geometry. Only HWND 16:9 leftover.
    if (eyeW >= 640 && inW == eyeW && inH == eyeH)
        return false;
    if (inW < 1600 || inH < 900)
        return false;
    x = 0;
    y = 0;
    w = 1;
    h = 1;
    return true;
}

void VR::NoteMcRecordThread()
{
    m_McRecordThreadId.store(GetCurrentThreadId(), std::memory_order_release);
}

bool VR::McOnRecordThread() const
{
    const DWORD id = m_McRecordThreadId.load(std::memory_order_acquire);
    return id != 0 && id == GetCurrentThreadId();
}

bool VR::McResolveRingEyeOrigin(int inX, int inY, int inW, int inH, int& outX, int& outY) const
{
    const int eyeW = static_cast<int>(m_RenderWidth);
    const int eyeH = static_cast<int>(m_RenderHeight);
    if (eyeW < 640 || eyeH < 360)
        return false;
    // Only the game thread that recorded this pair may use m_StereoEye +
    // McWriteBand. D3D/IMat playback runs on the material thread while that
    // thread is already inside the *next* pair (bmvr_log: MC mat-thread origin
    // y=0 then D3D 2560x1440 @0,0 -> @0,3104 / @3168,3104). That stamped the
    // previous left eye onto the current right rect: left flicker, smeared
    // right 3D sky, tearing on lookaround.
    if (m_StereoEye != 0 && McOnRecordThread())
    {
        outX = (m_StereoEye == 2) ? McRightOriginX() : McLeftOriginX();
        outY = static_cast<int>(McWriteBand() * m_RenderHeight);
        return true;
    }
    const bool atBand = McOriginIsRingBand(inX, inY);
    const bool origin00 = inX <= 8 && inY <= 8;
    const bool fullEye = inW == eyeW && inH == eyeH;
    if (atBand && (fullEye || !origin00))
    {
        outX = inX;
        outY = inY;
        return true;
    }
    // Eye-sized @0,0 on the ring is lighting apply (G-buffer/FullFrame are
    // 1x at 0,0). Remap to the current eye. Window 16:9 @0,0 is leftover.
    if (origin00)
    {
        if (fullEye && g_McEyeKnown)
        {
            outX = g_McEyeX;
            outY = g_McEyeY;
            return true;
        }
        return false;
    }
    if (g_McEyeKnown)
    {
        outX = g_McEyeX;
        outY = g_McEyeY;
        return true;
    }
    outX = McLeftOriginX();
    outY = 0;
    return true;
}

void VR::McNoteRingEyeViewport(int x, int y, int w, int h)
{
    const int eyeW = static_cast<int>(m_RenderWidth);
    const int eyeH = static_cast<int>(m_RenderHeight);
    if (w != eyeW || h != eyeH || !McOriginIsRingBand(x, y))
        return;
    if (std::abs(g_McLastRingVpX - McLeftOriginX()) <= 8
        && std::abs(x - McRightOriginX()) <= 8 && g_D3DVR9)
        g_D3DVR9->FlushCommands();
    g_McLastRingVpX = x;
    g_McEyeX = x;
    g_McEyeY = y;
    g_McEyeKnown = true;
}

bool VR::McLastRingEyeOrigin(int& x, int& y) const
{
    if (!g_McEyeKnown)
        return false;
    x = g_McEyeX;
    y = g_McEyeY;
    return true;
}

void VR::ResetMcPipeline()
{
    m_McFrame = 0;
    m_McPoses[0] = L4D2VROpenXrPoseDesc{};
    m_McPoses[1] = L4D2VROpenXrPoseDesc{};
    m_McPoses[2] = L4D2VROpenXrPoseDesc{};
}

void VR::FinishMcStereoPair()
{
    const uint32_t band = McWriteBand();
    if (m_OpenXrStereoRenderPoseValid)
        m_McPoses[band] = m_OpenXrStereoRenderPose;
    else
        m_McPoses[band] = m_OpenXrLastHmdPose;
    ++m_McFrame;
    static int s_mcPair;
    if (s_mcPair < 8)
    {
        Game::logMsg("MC stereo pair write band=%u frame=%u", band, m_McFrame);
        ++s_mcPair;
    }
}

void VR::ReleaseMcRing()
{
    m_McRingReady = false;
    m_OpenXrMcRingDesc = L4D2VROpenXrSharedTextureDesc{};
    m_VKMcRing = SharedTextureHolder{};
    ReleaseT(m_D9McRingDepth);
    ReleaseT(m_D9McRingSurface);
    ReleaseT(m_D9McRingTexture);
    ResetMcPipeline();
}

bool VR::EnsureMcRing(IDirect3DDevice9* device)
{
    if (!device || !g_D3DVR9)
        return false;
    if (!bmvr::g_MulticoreMode || !m_OpenXrHelperBridgeActive)
    {
        if (m_McRingReady)
            ReleaseMcRing();
        return false;
    }
    const UINT w = m_RenderWidth;
    const UINT h = m_RenderHeight;
    if (w < 640 || h < 360)
        return false;
    const UINT ringW = McRingWidth();
    const UINT ringH = McRingHeight();
    if (m_McRingReady && m_D9McRingSurface)
    {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(m_D9McRingSurface->GetDesc(&desc))
            && desc.Width == ringW && desc.Height == ringH)
            return true;
    }
    ReleaseMcRing();

    D3DCAPS9 caps{};
    if (SUCCEEDED(device->GetDeviceCaps(&caps)))
    {
        if (ringW > caps.MaxTextureWidth || ringH > caps.MaxTextureHeight)
        {
            Game::logMsg("MC ring %ux%u exceeds D3D caps %ux%u — stay mat_queue_mode 0",
                ringW, ringH, caps.MaxTextureWidth, caps.MaxTextureHeight);
            return false;
        }
    }

    m_CreatingTextureID = Texture_McRing;
    const HRESULT hr = device->CreateTexture(
        ringW, ringH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &m_D9McRingTexture, nullptr);
    m_CreatingTextureID = Texture_None;
    if (FAILED(hr) || !m_D9McRingTexture)
    {
        Game::logMsg("MC ring CreateTexture %ux%u hr=0x%08X", ringW, ringH, (unsigned)hr);
        ReleaseMcRing();
        return false;
    }
    if (FAILED(m_D9McRingTexture->GetSurfaceLevel(0, &m_D9McRingSurface)) || !m_D9McRingSurface)
    {
        Game::logMsg("MC ring GetSurfaceLevel failed");
        ReleaseMcRing();
        return false;
    }
    const HRESULT depthHr = device->CreateDepthStencilSurface(
        ringW, ringH, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &m_D9McRingDepth, nullptr);
    if (FAILED(depthHr) || !m_D9McRingDepth)
    {
        Game::logMsg("MC ring depth %ux%u hr=0x%08X", ringW, ringH, (unsigned)depthHr);
        ReleaseMcRing();
        return false;
    }
    if (!FillSharedTexture(m_D9McRingSurface, m_VKMcRing)
        || !m_VKMcRing.m_SharedHandleValid || m_VKMcRing.m_SharedHandle == 0)
    {
        Game::logMsg("MC ring GetVRDesc/export failed surf=%p", (void*)m_D9McRingSurface);
        ReleaseMcRing();
        return false;
    }

    m_OpenXrMcRingDesc = L4D2VROpenXrSharedTextureDesc{};
    m_OpenXrMcRingDesc.valid = 1;
    m_OpenXrMcRingDesc.width = m_VKMcRing.m_VulkanData.m_nWidth;
    m_OpenXrMcRingDesc.height = m_VKMcRing.m_VulkanData.m_nHeight;
    m_OpenXrMcRingDesc.format = m_VKMcRing.m_VulkanData.m_nFormat;
    m_OpenXrMcRingDesc.sampleCount = m_VKMcRing.m_VulkanData.m_nSampleCount;
    m_OpenXrMcRingDesc.handleType = m_VKMcRing.m_SharedHandleType;
    m_OpenXrMcRingDesc.queueFamilyIndex = m_VKMcRing.m_VulkanData.m_nQueueFamilyIndex;
    m_OpenXrMcRingDesc.kmtHandle = m_VKMcRing.m_SharedHandle;
    m_OpenXrMcRingDesc.image = static_cast<uint64_t>(m_VKMcRing.m_VulkanData.m_nImage);
    m_OpenXrMcRingDesc.reserved0 = L4D2VR_OPENXR_SHARED_UV_EXPLICIT;
    m_McRingReady = true;
    Game::logMsg("MC ring ready %ux%u (2x3 +%d gutter) img=%llu handle=0x%llX",
        ringW, ringH, kMcGutter,
        (unsigned long long)m_OpenXrMcRingDesc.image,
        (unsigned long long)m_OpenXrMcRingDesc.kmtHandle);
    return true;
}

bool VR::PublishMcRingBand(const OpenXrPendingPublish& pend)
{
    if (!m_McRingReady || m_McFrame < 2 || !m_OpenXrMcRingDesc.valid)
        return false;
    const uint32_t band = (m_McFrame - 2) % 3u;
    const float v0 = static_cast<float>(band) / 3.0f;
    const float v1 = static_cast<float>(band + 1u) / 3.0f;
    const float eyeAspect = (m_RenderHeight > 0)
        ? (static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight))
        : 1.0f;
    const float fovX = (std::isfinite(m_Fov) && m_Fov > 1.0f && m_Fov < 179.0f) ? m_Fov : 90.0f;

    auto eyeDesc = [&](uint32_t eyeIndex) {
        L4D2VROpenXrSharedTextureDesc d = m_OpenXrMcRingDesc;
        const float invW = 1.0f / static_cast<float>(McRingWidth());
        const float u0 = (eyeIndex == L4D2VR_OPENXR_EYE_LEFT)
            ? static_cast<float>(McLeftOriginX()) * invW
            : static_cast<float>(McRightOriginX()) * invW;
        const float u1 = u0 + static_cast<float>(m_RenderWidth) * invW;
        const vr::VRTextureBounds_t& tb = m_TextureBounds[eyeIndex];
        const float tu0 = std::clamp(tb.uMin, 0.0f, 1.0f);
        const float tu1 = std::clamp(tb.uMax, 0.0f, 1.0f);
        const float tv0 = std::clamp(tb.vMin, 0.0f, 1.0f);
        const float tv1 = std::clamp(tb.vMax, 0.0f, 1.0f);
        d.uMin = u0 + (u1 - u0) * tu0;
        d.uMax = u0 + (u1 - u0) * tu1;
        d.vMin = v0 + (v1 - v0) * tv0;
        d.vMax = v0 + (v1 - v0) * tv1;
        if (d.uMax <= d.uMin)
        {
            d.uMin = u0;
            d.uMax = u1;
        }
        if (d.vMax <= d.vMin)
        {
            d.vMin = v0;
            d.vMax = v1;
        }
        d.renderFovXDeg = fovX;
        d.renderAspect = eyeAspect;
        d.reserved0 = L4D2VR_OPENXR_SHARED_UV_EXPLICIT;
        return d;
    };

    L4D2VROpenXrPoseDesc pose = m_McPoses[band];
    if (!pose.valid && pend.havePose)
        pose = pend.pose;
    if (pose.valid)
        L4D2VR_PublishOpenXrGameRenderPose(pose);

    const L4D2VROpenXrSharedTextureDesc left = eyeDesc(L4D2VR_OPENXR_EYE_LEFT);
    const L4D2VROpenXrSharedTextureDesc right = eyeDesc(L4D2VR_OPENXR_EYE_RIGHT);
    L4D2VR_PublishOpenXrSharedTexturePair(left, right);
    const uint32_t frameId = m_OpenXrSubmitFrameId.fetch_add(1, std::memory_order_acq_rel);
    L4D2VR_PublishOpenXrSharedTextureFrame(frameId);
    m_OpenXrLastPublishedSharedTextureFrameId.store(frameId, std::memory_order_release);
    ++m_OpenXrPublishes;
    m_HasSubmittedSceneFrame.store(true, std::memory_order_release);
    ++m_SubmitCount;
    static int s_mcSubmit;
    if (s_mcSubmit < 8)
    {
        Game::logMsg("MC submit band=%u frame=%u uvL=(%.3f %.3f)-(%.3f %.3f) pose=%d",
            band, m_McFrame, left.uMin, left.vMin, left.uMax, left.vMax, pose.valid ? 1 : 0);
        ++s_mcSubmit;
    }
    return true;
}

void VR::NoteMsaaEyeScene(IDirect3DSurface9* dst, bool copied)
{
    if (!copied || !dst)
        return;
    if (dst == m_D9LeftEyeSurface && m_D9LeftEyeSubmitSurface)
        m_LeftEyeMsaaHasScene = true;
    else if (dst == m_D9RightEyeSurface && m_D9RightEyeSubmitSurface)
        m_RightEyeMsaaHasScene = true;
}

IDirect3DSurface9* VR::ColorTargetForStereoEye(int stereoEye) const
{
    if (stereoEye == 1)
    {
        if (m_LeftEyeMsaaHasScene || !m_D9LeftEyeSubmitSurface)
            return m_D9LeftEyeSurface;
        return m_D9LeftEyeSubmitSurface;
    }
    if (stereoEye == 2)
    {
        if (m_RightEyeMsaaHasScene || !m_D9RightEyeSubmitSurface)
            return m_D9RightEyeSurface;
        return m_D9RightEyeSubmitSurface;
    }
    return nullptr;
}

void VR::ResolveMsaaEyesToSubmit(IDirect3DDevice9* device)
{
    if (!device)
        return;
    m_CaptureReentry = true;
    if (m_LeftEyeMsaaHasScene && m_D9LeftEyeSurface && m_D9LeftEyeSubmitSurface
        && m_D9LeftEyeSurface != m_D9LeftEyeSubmitSurface)
    {
        const HRESULT hr = device->StretchRect(
            m_D9LeftEyeSurface, nullptr, m_D9LeftEyeSubmitSurface, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
            Game::logMsg("MSAA left eye resolve hr=0x%08X", (unsigned)hr);
    }
    if (m_RightEyeMsaaHasScene && m_D9RightEyeSurface && m_D9RightEyeSubmitSurface
        && m_D9RightEyeSurface != m_D9RightEyeSubmitSurface)
    {
        const HRESULT hr = device->StretchRect(
            m_D9RightEyeSurface, nullptr, m_D9RightEyeSubmitSurface, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
            Game::logMsg("MSAA right eye resolve hr=0x%08X", (unsigned)hr);
    }
    m_CaptureReentry = false;
}

void VR::ReleaseVRRenderTargetsForDeviceReset()
{
    std::lock_guard<TextureStateMutex> lock(m_TextureMutex);
    // Reset rebinds RT0 / recreates the backbuffer behind the hooks.
    g_Rt0ActualPtr = nullptr;
    g_Rt0ActualW = 0;
    g_Rt0ActualH = 0;
    g_Rt0ActualKnown = false;
    g_CachedBackBuffer = nullptr;
    g_CachedBackBufferValid = false;
    ReleaseT(m_HandEngineStateBlock);
    ReleaseT(m_D9LeftEyeSurface);
    ReleaseT(m_D9RightEyeSurface);
    ReleaseT(m_D9LeftEyeSubmitSurface);
    ReleaseT(m_D9RightEyeSubmitSurface);
    g_VrGloves.OnDeviceLost();
    if (m_SceneCubemap)
    {
        m_SceneCubemap->Release();
        m_SceneCubemap = nullptr;
    }
    ReleaseT(m_D9LeftEyeDepthSurface);
    ReleaseT(m_D9RightEyeDepthSurface);
    ReleaseT(m_D9LeftEyeTexture);
    ReleaseT(m_D9RightEyeTexture);
    ReleaseT(m_D9LeftEyeSubmitTexture);
    ReleaseT(m_D9RightEyeSubmitTexture);
    ReleaseMcRing();
    ReleaseOpenXrPublishTextures();
    m_LeftEyeMsaaHasScene = false;
    m_RightEyeMsaaHasScene = false;
    ReleaseT(m_D9FrameColorSurface);
    ReleaseT(m_BlitEventQuery);
    ReleaseT(m_D9HUDSurface);
    ReleaseT(m_D9HUDTexture);
    ReleaseT(m_D9DesktopMirrorSurface);
    ReleaseT(m_D9DesktopMirrorTexture);
    m_DesktopMirrorHasImage.store(false, std::memory_order_release);
    m_VKHUD = SharedTextureHolder{};
    m_HudOverlayReady = false;
    if (m_PauseHudPrevRt)
    {
        m_PauseHudPrevRt->Release();
        m_PauseHudPrevRt = nullptr;
    }
    if (m_PauseHudPrevDs)
    {
        m_PauseHudPrevDs->Release();
        m_PauseHudPrevDs = nullptr;
    }
    m_PauseHudRtSaved = false;
    m_LeftEyeTexture = nullptr;
    m_RightEyeTexture = nullptr;
    m_UsedNamedRenderTargets = false;
    m_DirectEyeSubmit = false;
    m_StereoRenderViewActive = false;
    m_FrameCopyWidth = 0;
    m_FrameCopyHeight = 0;
    m_CreatedVRTextures.store(false, std::memory_order_release);
    m_BackBufferTextureValid = false;
    m_FrameCopyLatched = false;
    m_RenderedNewFrame.store(false, std::memory_order_release);
    m_SkipBlockingPoseWait = true;
    m_HasSubmittedSceneFrame.store(false, std::memory_order_release);
    Game::logMsg("Released VR render targets for device reset");
}

void VR::InstallDeviceHooks(IDirect3DDevice9* device)
{
    if (m_D3DHooksInstalled || !device)
        return;

    void** vtbl = *reinterpret_cast<void***>(device);
    if (!vtbl)
        return;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        Game::logMsg("MinHook init for D3D hooks failed %d", (int)st);
        return;
    }

    if (MH_CreateHook(vtbl[kIDirect3DDevice9_Present], reinterpret_cast<LPVOID>(&HookedPresent),
            reinterpret_cast<LPVOID*>(&g_OrigPresent)) != MH_OK)
    {
        Game::logMsg("MH_CreateHook Present failed");
        return;
    }
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_SetRenderTarget], reinterpret_cast<LPVOID>(&HookedSetRenderTarget),
            reinterpret_cast<LPVOID*>(&g_OrigSetRenderTarget)) != MH_OK)
    {
        Game::logMsg("MH_CreateHook SetRenderTarget failed");
        return;
    }
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_SetDepthStencilSurface], reinterpret_cast<LPVOID>(&HookedSetDepthStencil),
            reinterpret_cast<LPVOID*>(&g_OrigSetDepthStencil)) != MH_OK)
    {
        Game::logMsg("MH_CreateHook SetDepthStencilSurface failed");
        g_OrigSetDepthStencil = nullptr;
    }
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_SetViewport], reinterpret_cast<LPVOID>(&HookedSetViewport),
            reinterpret_cast<LPVOID*>(&g_OrigSetViewport)) != MH_OK)
    {
        Game::logMsg("MH_CreateHook SetViewport failed");
        g_OrigSetViewport = nullptr;
    }
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_SetScissorRect], reinterpret_cast<LPVOID>(&HookedSetScissorRect),
            reinterpret_cast<LPVOID*>(&g_OrigSetScissorRect)) != MH_OK)
    {
        Game::logMsg("MH_CreateHook SetScissorRect failed");
        g_OrigSetScissorRect = nullptr;
    }
    if (MH_EnableHook(vtbl[kIDirect3DDevice9_Present]) != MH_OK ||
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetRenderTarget]) != MH_OK)
    {
        Game::logMsg("MH_EnableHook D3D present/RT failed");
        return;
    }
    if (g_OrigSetDepthStencil)
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetDepthStencilSurface]);
    if (g_OrigSetViewport)
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetViewport]);
    if (g_OrigSetScissorRect)
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetScissorRect]);
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_Clear], reinterpret_cast<LPVOID>(&HookedClear),
            reinterpret_cast<LPVOID*>(&g_OrigClear)) == MH_OK)
        MH_EnableHook(vtbl[kIDirect3DDevice9_Clear]);
    else
        g_OrigClear = nullptr;
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_StretchRect],
            reinterpret_cast<LPVOID>(&HookedStretchRect),
            reinterpret_cast<LPVOID*>(&g_OrigStretchRect)) == MH_OK)
        MH_EnableHook(vtbl[kIDirect3DDevice9_StretchRect]);
    else
        g_OrigStretchRect = nullptr;
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_SetVertexShaderConstantF],
            reinterpret_cast<LPVOID>(&HookedSetVertexShaderConstantF),
            reinterpret_cast<LPVOID*>(&g_OrigSetVertexShaderConstantF)) == MH_OK)
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetVertexShaderConstantF]);
    else
        g_OrigSetVertexShaderConstantF = nullptr;
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_SetPixelShaderConstantF],
            reinterpret_cast<LPVOID>(&HookedSetPixelShaderConstantF),
            reinterpret_cast<LPVOID*>(&g_OrigSetPixelShaderConstantF)) == MH_OK)
        MH_EnableHook(vtbl[kIDirect3DDevice9_SetPixelShaderConstantF]);
    else
        g_OrigSetPixelShaderConstantF = nullptr;

    m_D3DHooksInstalled = true;
    g_DeviceHooksEnabled = true;
    Game::logMsg("D3D9 Present + SetRenderTarget + SetDepthStencil + SetViewport + SetScissorRect + Clear + StretchRect + shaderConst hooks installed");
}

bool VR::EnsureFrameCopySurface(IDirect3DDevice9* device, uint32_t width, uint32_t height)
{
    if (!device || width < 640 || height < 360)
        return false;
    if (m_D9FrameColorSurface && m_FrameCopyWidth == width && m_FrameCopyHeight == height)
        return true;

    IDirect3DSurface9* surf = nullptr;
    const HRESULT hr = device->CreateRenderTarget(
        width, height, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &surf, nullptr);
    if (FAILED(hr) || !surf)
    {
        Game::logMsg("Frame copy RT create failed hr=0x%08X %ux%u", (unsigned)hr, width, height);
        return false;
    }
    ReleaseT(m_D9FrameColorSurface);
    m_D9FrameColorSurface = surf;
    m_FrameCopyWidth = width;
    m_FrameCopyHeight = height;
    m_FrameCopyLatched = false;
    Game::logMsg("Frame copy RT ready %ux%u", width, height);
    return true;
}

bool VR::EnsureNamedEyeTextures()
{
    return false;
}

bool VR::NamedStereoReady() const
{
    return false;
}

void VR::PrepareNamedStereoFromPresent()
{
    if (!bmvr::TryStereoRenderView() || !m_GameplayEligible)
        return;
    // Do not ClientCmd cvars from Present during load. The 1576 matching-
    // swapchain death logged "queued cl_csm_enabled 0" then died before any
    // RenderView; CSM disable was a named-RT wrap leftover and is not
    // required for G-buffer-sized blit stereo (stereo_copy survived with CSM).
    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
        return;
    EnsureStereoEyeSurfaces();
}

bool VR::EnsurePrivateEyeSurfaces(IDirect3DDevice9* device)
{
    if (!device)
        return false;

    m_AntiAliasing = bmvr::g_AntiAliasing;
    ChooseEyeRenderSize();
    UINT w = m_RenderWidth;
    UINT h = m_RenderHeight;
    if (w < 640 || h < 360)
    {
        w = KnownWindowWidth();
        h = KnownWindowHeight();
    }
    if (w < 640 || h < 360)
        return false;

    if (m_CreatedVRTextures.load(std::memory_order_acquire) && m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_VKLeftEye.m_VulkanData.m_nWidth >= 640 && m_VKLeftEye.m_VulkanData.m_nHeight >= 360)
    {
        const UINT haveW = m_VKLeftEye.m_VulkanData.m_nWidth;
        const UINT haveH = m_VKLeftEye.m_VulkanData.m_nHeight;
        if (haveW == w && haveH == h)
        {
            if (bmvr::g_MulticoreMode)
                EnsureMcRing(device);
            return true;
        }
        const bool growForFullFrame = bmvr::TryFullFrameStereo()
            && w > haveW + 32
            && !m_StereoEyeBlitActive;
        const bool liveOffscreenResize = bmvr::TryOffscreenHmd()
            && !m_StereoEyeBlitActive
            && (w + 32 < haveW || h + 32 < haveH || haveW + 32 < w || haveH + 32 < h);
        if (liveOffscreenResize && m_EyeResizeSettleMs != 0
            && (GetTickCount() - m_EyeResizeSettleMs) < 300)
        {
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            return true;
        }
        if ((!bmvr::TrySteamVrEyeRt() && !growForFullFrame && !liveOffscreenResize)
            || (!liveOffscreenResize && haveW + 32 >= w))
        {
            Game::logMsg("Keep existing D3D eyes %ux%u (requested %ux%u; HWND resize must not recreate)",
                haveW, haveH, w, h);
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            if (bmvr::g_MulticoreMode)
                EnsureMcRing(device);
            return true;
        }
        Game::logMsg("Recreating D3D eyes %ux%u -> %ux%u (%s)",
            haveW, haveH, w, h,
            liveOffscreenResize ? "SteamVR live SS" : (growForFullFrame ? "FullFrame stereo" : "SteamVR recommended"));
        // Do not BeginRisky on live SS. Stereo already settled hmd_offscreen
        // after 120 frames; a recreate+quit false-banned the path.
        if (growForFullFrame && !liveOffscreenResize)
            bmvr::BeginRisky(L"ff_stereo");
        m_CreatedVRTextures.store(false, std::memory_order_release);
    }

    std::lock_guard<TextureStateMutex> lock(m_TextureMutex);

    auto createEye = [&](TextureID id, IDirect3DTexture9** tex, IDirect3DSurface9** surf, SharedTextureHolder& vk) -> bool {
        ReleaseT(*surf);
        ReleaseT(*tex);
        m_CreatingTextureID = id;
        const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr);
        m_CreatingTextureID = Texture_None;
        if (FAILED(hr) || !*tex)
        {
            Game::logMsg("CreateTexture eye id=%d hr=0x%08X", (int)id, (unsigned)hr);
            return false;
        }
        if (!*surf)
            (*tex)->GetSurfaceLevel(0, surf);
        // MSAA eyes cannot be OpenVR submit targets (GetVRDesc is skipped).
        if (!UseVrMsaa())
        {
            if (!FillSharedTexture(*surf, vk))
            {
                Game::logMsg("GetVRDesc failed for eye id=%d surf=%p", (int)id, (void*)*surf);
                return false;
            }
            Game::logMsg("Eye RT id=%d %ux%u img=%llu", (int)id, vk.m_VulkanData.m_nWidth, vk.m_VulkanData.m_nHeight,
                (unsigned long long)vk.m_VulkanData.m_nImage);
        }
        else
        {
            Game::logMsg("Eye RT id=%d %ux%u MSAA=%u (submit texture holds Vulkan desc)",
                (int)id, w, h, m_AntiAliasing);
        }
        return true;
    };

    auto createDepth = [&](TextureID id, IDirect3DSurface9** depth) -> bool {
        ReleaseT(*depth);
        m_CreatingTextureID = id;
        const HRESULT hr = device->CreateDepthStencilSurface(
            w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, depth, nullptr);
        m_CreatingTextureID = Texture_None;
        if (FAILED(hr) || !*depth)
        {
            Game::logMsg("CreateDepthStencil eye id=%d hr=0x%08X", (int)id, (unsigned)hr);
            return false;
        }
        return true;
    };

    bool leftOk = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
    bool rightOk = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
    const bool depthOk = createDepth(Texture_LeftEye, &m_D9LeftEyeDepthSurface)
        && createDepth(Texture_RightEye, &m_D9RightEyeDepthSurface);
    // Capture stays at HWND size. Growing this to the eye wiped the 2560x1440
    // GameUI copy and left OpenXR publishing an empty 3168 RT (HMD black).
    UINT copyW = KnownWindowWidth();
    UINT copyH = KnownWindowHeight();
    if (copyW < 640 || copyH < 360)
    {
        copyW = w;
        copyH = h;
    }
    if (!m_D9FrameColorSurface)
        EnsureFrameCopySurface(device, copyW, copyH);

    bool submitOk = !UseVrMsaa();
    if (UseVrMsaa())
    {
        auto createSubmit = [&](TextureID id, IDirect3DTexture9** tex, IDirect3DSurface9** surf, SharedTextureHolder& vk) -> bool {
            ReleaseT(*surf);
            ReleaseT(*tex);
            m_CreatingTextureID = id;
            const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr);
            m_CreatingTextureID = Texture_None;
            if (FAILED(hr) || !*tex)
            {
                Game::logMsg("CreateTexture submit id=%d hr=0x%08X", (int)id, (unsigned)hr);
                return false;
            }
            if (!*surf)
                (*tex)->GetSurfaceLevel(0, surf);
            if (!FillSharedTexture(*surf, vk))
            {
                Game::logMsg("GetVRDesc failed for submit id=%d surf=%p", (int)id, (void*)*surf);
                return false;
            }
            Game::logMsg("Eye submit RT id=%d %ux%u img=%llu", (int)id, vk.m_VulkanData.m_nWidth, vk.m_VulkanData.m_nHeight,
                (unsigned long long)vk.m_VulkanData.m_nImage);
            return true;
        };
        submitOk = createSubmit(Texture_LeftEyeSubmit, &m_D9LeftEyeSubmitTexture, &m_D9LeftEyeSubmitSurface, m_VKLeftEye)
            && createSubmit(Texture_RightEyeSubmit, &m_D9RightEyeSubmitTexture, &m_D9RightEyeSubmitSurface, m_VKRightEye);
        if (!submitOk)
        {
            Game::logMsg("MSAA submit RTs failed; recreating eyes without MSAA");
            m_AntiAliasing = 0;
            bmvr::g_AntiAliasing = 0;
            ReleaseT(m_D9LeftEyeSubmitSurface);
            ReleaseT(m_D9RightEyeSubmitSurface);
            ReleaseT(m_D9LeftEyeSubmitTexture);
            ReleaseT(m_D9RightEyeSubmitTexture);
            const bool leftRetry = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
            const bool rightRetry = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
            leftOk = leftRetry;
            rightOk = rightRetry;
            submitOk = leftRetry && rightRetry
                && m_VKLeftEye.m_VulkanData.m_nImage && m_VKRightEye.m_VulkanData.m_nImage;
        }
    }

    m_CreatedVRTextures.store(leftOk && rightOk && submitOk
        && m_VKLeftEye.m_VulkanData.m_nImage && m_VKRightEye.m_VulkanData.m_nImage,
        std::memory_order_release);
    if (m_CreatedVRTextures.load(std::memory_order_acquire))
        m_EyeResizeSettleMs = 0;
    Game::logMsg("VR D3D eye RTs ready=%d depth=%d msaa=%u submit=%d L=%p R=%p copy=%p %ux%u",
        m_CreatedVRTextures.load() ? 1 : 0, depthOk ? 1 : 0, m_AntiAliasing, submitOk ? 1 : 0,
        (void*)m_D9LeftEyeSurface, (void*)m_D9RightEyeSurface, (void*)m_D9FrameColorSurface, w, h);
    if (m_CreatedVRTextures.load(std::memory_order_acquire) && bmvr::g_MulticoreMode)
        EnsureMcRing(device);
    return m_CreatedVRTextures.load();
}

bool VR::EnsureStereoEyeSurfaces()
{
    if (!g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;
    const bool ok = EnsurePrivateEyeSurfaces(device);
    static int s_readyLog;
    if (ok && s_readyLog < 2)
    {
        Game::logMsg("Stereo D3D eyes ready=%d %ux%u L=%p R=%p",
            1, m_RenderWidth, m_RenderHeight,
            (void*)m_D9LeftEyeSurface, (void*)m_D9RightEyeSurface);
        ++s_readyLog;
    }
    device->Release();
    return ok;
}

bool VR::StereoEyesReady() const
{
    return m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_RenderWidth >= 640 && m_RenderHeight >= 360
        && m_VKLeftEye.m_VulkanData.m_nWidth == m_RenderWidth
        && m_VKLeftEye.m_VulkanData.m_nHeight == m_RenderHeight;
}

void VR::ClearUnusedDesktopBackbuffer()
{
    if (!g_D3DVR9)
        return;
    uint32_t fbW = 0, fbH = 0;
    if (!bmvr::HaveHmdFramebufferSize(fbW, fbH))
        return;
    // ColorFill the unused 16:9 window region. Do not use SteamVR eye size
    // here — 3168x3100 would black the whole desktop backbuffer.

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return;
    }
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(bb->GetDesc(&desc)))
    {
        if (desc.Width > fbW)
        {
            const RECT right = {
                static_cast<LONG>(fbW), 0,
                static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)
            };
            device->ColorFill(bb, &right, D3DCOLOR_XRGB(0, 0, 0));
        }
        if (desc.Height > fbH)
        {
            const LONG fillW = static_cast<LONG>((std::min)(fbW, desc.Width));
            const RECT bottom = {
                0, static_cast<LONG>(fbH),
                fillW, static_cast<LONG>(desc.Height)
            };
            device->ColorFill(bb, &bottom, D3DCOLOR_XRGB(0, 0, 0));
        }
    }
    bb->Release();
    device->Release();
}

bool VR::EnsureDesktopMirrorSurface(IDirect3DDevice9* device, UINT w, UINT h)
{
    if (!device || w < 640 || h < 360)
        return false;
    if (m_D9DesktopMirrorSurface)
    {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(m_D9DesktopMirrorSurface->GetDesc(&desc))
            && desc.Width == w && desc.Height == h)
            return true;
    }
    ReleaseT(m_D9DesktopMirrorSurface);
    ReleaseT(m_D9DesktopMirrorTexture);
    m_DesktopMirrorHasImage.store(false, std::memory_order_release);
    const HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_D9DesktopMirrorTexture, nullptr);
    if (FAILED(hr) || !m_D9DesktopMirrorTexture)
    {
        static int s_fail;
        if (s_fail < 4)
        {
            Game::logMsg("Desktop mirror CreateTexture failed hr=0x%08X %ux%u",
                (unsigned)hr, w, h);
            ++s_fail;
        }
        return false;
    }
    if (FAILED(m_D9DesktopMirrorTexture->GetSurfaceLevel(0, &m_D9DesktopMirrorSurface))
        || !m_D9DesktopMirrorSurface)
    {
        ReleaseT(m_D9DesktopMirrorTexture);
        return false;
    }
    Game::logMsg("Desktop mirror RT %ux%u", w, h);
    return true;
}

void VR::BlitDesktopMirrorToBackbuffer()
{
    if (!m_DesktopMirrorHasImage.load(std::memory_order_acquire)
        || !m_D9DesktopMirrorSurface || !g_D3DVR9)
        return;
    if (Want2dMenuPanel() || WantPauseWorldOverlay())
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return;
    }
    m_CaptureReentry = true;
    HRESULT hr = StretchRectKeepDeviceState(
        device, m_D9DesktopMirrorSurface, nullptr, bb, nullptr, D3DTEXF_NONE);
    m_CaptureReentry = false;
    static int s_bb;
    if (s_bb < 4 || FAILED(hr))
    {
        Game::logMsg("Desktop mirror -> BB hr=0x%08X", (unsigned)hr);
        ++s_bb;
    }
    bb->Release();
    device->Release();
}

void VR::MirrorStereoToDesktopWindow()
{
    if (!m_D9LeftEyeSurface || !g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return;
    }

    D3DSURFACE_DESC bbDesc{};
    UINT eyeW = 0, eyeH = 0;
    if (FAILED(bb->GetDesc(&bbDesc)) || !ResolveSurfaceSize(m_D9LeftEyeSurface, eyeW, eyeH)
        || bbDesc.Width < 640 || bbDesc.Height < 360 || eyeW < 640 || eyeH < 360)
    {
        bb->Release();
        device->Release();
        return;
    }

    // Cover-crop: same one StretchRect as letterbox. The ~1:1 eye is cropped
    // to the HWND aspect (top/bottom on 16:9) and scaled to the full window.
    const float srcAspect = static_cast<float>(eyeW) / static_cast<float>(eyeH);
    const float dstAspect = static_cast<float>(bbDesc.Width) / static_cast<float>(bbDesc.Height);
    UINT cropW = eyeW;
    UINT cropH = eyeH;
    UINT sx = 0;
    UINT sy = 0;
    if (srcAspect > dstAspect)
    {
        cropW = static_cast<UINT>(static_cast<float>(eyeH) * dstAspect + 0.5f);
        if (cropW > eyeW)
            cropW = eyeW;
        sx = (eyeW - cropW) / 2;
    }
    else if (dstAspect > srcAspect)
    {
        cropH = static_cast<UINT>(static_cast<float>(eyeW) / dstAspect + 0.5f);
        if (cropH > eyeH)
            cropH = eyeH;
        sy = (eyeH - cropH) / 2;
    }
    if (sx + cropW > eyeW)
        cropW = eyeW - sx;
    if (sy + cropH > eyeH)
        cropH = eyeH - sy;
    if (cropW < 64 || cropH < 64)
    {
        bb->Release();
        device->Release();
        return;
    }
    RECT src = {
        static_cast<LONG>(sx), static_cast<LONG>(sy),
        static_cast<LONG>(sx + cropW), static_cast<LONG>(sy + cropH)
    };
    const RECT* srcPtr = (cropW != eyeW || cropH != eyeH) ? &src : nullptr;

    IDirect3DSurface9* dest = bb;
    const bool mcOffscreen = McPipelineEnabled();
    if (mcOffscreen)
    {
        if (!EnsureDesktopMirrorSurface(device, bbDesc.Width, bbDesc.Height))
        {
            bb->Release();
            device->Release();
            return;
        }
        dest = m_D9DesktopMirrorSurface;
    }

    m_CaptureReentry = true;
    HRESULT hr = StretchRectKeepDeviceState(
        device, m_D9LeftEyeSurface, srcPtr, dest, nullptr, D3DTEXF_LINEAR);
    if (FAILED(hr))
        hr = StretchRectKeepDeviceState(
            device, m_D9LeftEyeSurface, srcPtr, dest, nullptr, D3DTEXF_NONE);
    m_CaptureReentry = false;
    if (SUCCEEDED(hr) && dest == m_D9DesktopMirrorSurface)
        m_DesktopMirrorHasImage.store(true, std::memory_order_release);

    static int s_mirrorLog;
    if (s_mirrorLog < 4 || FAILED(hr))
    {
        Game::logMsg("Desktop cover-crop left eye %ux%u src=%d,%d %ux%u -> %s %ux%u hr=0x%08X",
            eyeW, eyeH, src.left, src.top, cropW, cropH,
            dest == bb ? "BB" : "offscreen", bbDesc.Width, bbDesc.Height, (unsigned)hr);
        ++s_mirrorLog;
    }

    bb->Release();
    device->Release();
}

bool VR::BlitHmdViewFromBackbuffer(IDirect3DSurface9* dst, bool flushGpu)
{
    // Fallback only: when FullFrame == eye size the unbind StretchRect is 1:1.
    // If that miss fires, copy the top-left HMD-fit rectangle of the 16:9 BB.
    if (!dst || !g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    {
        device->Release();
        return false;
    }

    UINT bbW = 0, bbH = 0;
    if (!ResolveSurfaceSize(bb, bbW, bbH) || bbW < 640 || bbH < 360)
    {
        bb->Release();
        device->Release();
        return false;
    }

    UINT cropW = m_RenderWidth >= 640 ? m_RenderWidth : bbW;
    UINT cropH = m_RenderHeight >= 360 ? m_RenderHeight : bbH;
    if (cropW > bbW)
        cropW = bbW;
    if (cropH > bbH)
        cropH = bbH;
    // GB-match: the 2560 BB holds the full HMD frustum (anamorphic in 16:9
    // pixels). Top-left 1584 crop would drop FOV. Squash the whole BB into
    // the 1.1 eye (cancels the 16:9 vs 1.1 pixel stretch).
    const bool squashFull = bmvr::UseGbMatchViewLock();
    if (squashFull)
    {
        cropW = bbW;
        cropH = bbH;
    }
    const UINT x0 = 0;
    const UINT y0 = 0;
    RECT srcRect = {
        0, 0,
        static_cast<LONG>(cropW), static_cast<LONG>(cropH)
    };
    const RECT* srcPtr = (!squashFull && (cropW != bbW || cropH != bbH)) ? &srcRect : nullptr;
    m_LastEyeBlitWasWindowCrop = false;

    m_CaptureReentry = true;
    const bool upscale = m_RenderWidth > bbW + 32 || m_RenderHeight > bbH + 32;
    const D3DTEXTUREFILTERTYPE filter = upscale ? D3DTEXF_LINEAR : D3DTEXF_NONE;
    HRESULT hr = device->StretchRect(bb, srcPtr, dst, nullptr, filter);
    if (FAILED(hr))
    {
        IDirect3DSurface9* submit = SubmitSurfaceForEye(dst);
        if (submit && submit != dst)
            hr = device->StretchRect(bb, srcPtr, submit, nullptr, filter);
    }
    else
        NoteMsaaEyeScene(dst, true);
    m_CaptureReentry = false;

    static int s_bbBlitLog;
    if (s_bbBlitLog < 8 || FAILED(hr))
    {
        Game::logMsg(
            "HMD BB blit %u,%u %ux%u of BB %ux%u -> eye %ux%u hr=0x%08X (crop=%d squash=%d upscale=%d flush=%d)",
            x0, y0, cropW, cropH, bbW, bbH, m_RenderWidth, m_RenderHeight, (unsigned)hr,
            (srcPtr != nullptr) ? 1 : 0, squashFull ? 1 : 0, upscale ? 1 : 0, flushGpu ? 1 : 0);
        if (upscale && squashFull)
            Game::logMsg("G-buffer did not grow; upscaling window %ux%u into offscreen eyes %ux%u",
                bbW, bbH, m_RenderWidth, m_RenderHeight);
        ++s_bbBlitLog;
    }

    if (SUCCEEDED(hr))
    {
        m_LastStereoBlitWidth = cropW;
        m_LastStereoBlitHeight = cropH;
        if (flushGpu)
            FlushStereoBlitGpu();
    }

    bb->Release();
    device->Release();
    return SUCCEEDED(hr);
}

bool VR::HudOverlayPixelSize(UINT& w, UINT& h) const
{
    if (m_D9HUDSurface)
    {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(m_D9HUDSurface->GetDesc(&d)) && d.Width >= 640 && d.Height >= 360)
        {
            w = d.Width;
            h = d.Height;
            return true;
        }
    }
    w = KnownWindowWidth();
    h = KnownWindowHeight();
    return w >= 640 && h >= 360;
}

bool VR::ForceHudOverlayViewport(int& x, int& y, int& w, int& h) const
{
    UINT tw = 0, th = 0;
    if (!HudOverlayPixelSize(tw, th))
        return false;
    x = 0;
    y = 0;
    w = static_cast<int>(tw);
    h = static_cast<int>(th);
    return true;
}

bool VR::BindPauseHudForExtraPaint()
{
    if (!m_D9HUDSurface || !g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;

    if (m_PauseHudPrevRt)
    {
        m_PauseHudPrevRt->Release();
        m_PauseHudPrevRt = nullptr;
    }
    if (m_PauseHudPrevDs)
    {
        m_PauseHudPrevDs->Release();
        m_PauseHudPrevDs = nullptr;
    }
    device->GetRenderTarget(0, &m_PauseHudPrevRt);
    device->GetDepthStencilSurface(&m_PauseHudPrevDs);
    device->GetViewport(&m_PauseHudPrevVp);
    m_PauseHudPrevColorWrite = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
        | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;
    m_PauseHudColorWriteSaved = SUCCEEDED(device->GetRenderState(
        D3DRS_COLORWRITEENABLE, &m_PauseHudPrevColorWrite));
    m_PauseHudRtSaved = true;

    device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
        | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    const HRESULT hr = device->SetRenderTarget(0, m_D9HUDSurface);
    device->SetDepthStencilSurface(nullptr);
    UINT tw = 0, th = 0;
    HudOverlayPixelSize(tw, th);
    D3DVIEWPORT9 vp{};
    vp.Width = tw;
    vp.Height = th;
    vp.MaxZ = 1.f;
    device->SetViewport(&vp);
    if (SUCCEEDED(hr))
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.f, 0);
    device->Release();
    if (FAILED(hr))
    {
        UnbindPauseHudAfterExtraPaint();
        return false;
    }
    return true;
}

void VR::UnbindPauseHudAfterExtraPaint()
{
    if (!m_PauseHudRtSaved)
        return;
    IDirect3DDevice9* device = nullptr;
    if (g_D3DVR9 && SUCCEEDED(g_D3DVR9->GetD3DDevice(&device)) && device)
    {
        if (m_PauseHudPrevRt)
            device->SetRenderTarget(0, m_PauseHudPrevRt);
        device->SetDepthStencilSurface(m_PauseHudPrevDs);
        device->SetViewport(&m_PauseHudPrevVp);
        if (m_PauseHudColorWriteSaved)
            device->SetRenderState(D3DRS_COLORWRITEENABLE, m_PauseHudPrevColorWrite);
        device->Release();
    }
    if (m_PauseHudPrevRt)
    {
        m_PauseHudPrevRt->Release();
        m_PauseHudPrevRt = nullptr;
    }
    if (m_PauseHudPrevDs)
    {
        m_PauseHudPrevDs->Release();
        m_PauseHudPrevDs = nullptr;
    }
    m_PauseHudColorWriteSaved = false;
    m_PauseHudRtSaved = false;
}

void VR::PreparePauseHudForVgui(IMatRenderContext* ctx)
{
    // HL2VR RenderHUD / DrawMainMenu: OverrideAlphaWriteEnable so VGUI
    // writes destination alpha. Empty pixels stay the D3D clear (0,0,0,0).
    if (m_D9HUDSurface && !m_VKHUD.m_VRTexture.handle)
        FillSharedTexture(m_D9HUDSurface, m_VKHUD);
    SehOverrideAlphaWrite(ctx, true, true);
    m_PauseHudAlphaOverridden = (ctx != nullptr);
}

void VR::FinishPauseHudExtraPaint(IMatRenderContext* ctx)
{
    // viewrender.cpp / vrviewrender.cpp restore this after HUD. Leaving it
    // on made every later stereo pass write alpha (50fps areas dropped to 35).
    if (!m_PauseHudAlphaOverridden && !ctx)
        return;
    SehOverrideAlphaWrite(ctx, false, true);
    m_PauseHudAlphaOverridden = false;
}

void VR::StampPauseOverlayCursor()
{
    if (!g_D3DVR9 || !m_D9HUDSurface)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    DrawMenuCursorOnSurface(device, m_D9HUDSurface);
    device->Release();
}

void VR::McPauseHudBeginOnMatThread()
{
    SetHudPaintActive(true);
    // Present must not TransferSurface this RT while it is cleared. That
    // published a transparent overlay for one frame (world flicker through
    // the pause menu).
    m_HudPaintedThisFrame.store(false, std::memory_order_release);
    if (!BindPauseHudForExtraPaint())
    {
        SetHudPaintActive(false);
        g_McPauseHudBound.store(false, std::memory_order_release);
        return;
    }
    IMatRenderContext* ctx = nullptr;
    if (m_Game && m_Game->m_MaterialSystem)
        ctx = SehGetRenderContext(m_Game->m_MaterialSystem);
    PreparePauseHudForVgui(ctx);
    if (ctx)
        SehReleaseMatContext(ctx);
    g_McPauseHudBound.store(true, std::memory_order_release);
}

void VR::McPauseHudEndOnMatThread()
{
    IMatRenderContext* ctx = nullptr;
    if (m_Game && m_Game->m_MaterialSystem)
        ctx = SehGetRenderContext(m_Game->m_MaterialSystem);
    FinishPauseHudExtraPaint(ctx);
    if (ctx)
        SehReleaseMatContext(ctx);
    if (g_McPauseHudBound.load(std::memory_order_acquire))
        StampPauseOverlayCursor();
    UnbindPauseHudAfterExtraPaint();
    SetHudPaintActive(false);
    if (g_McPauseHudBound.exchange(false, std::memory_order_acq_rel))
        NoteHudPainted();
}

void VR::EnsureHudOverlay()
{
    if (!bmvr::TryHudOverlay())
        return;
    if (!m_OpenXrHelperBridgeActive && m_HUDTopHandle == vr::k_ulOverlayHandleInvalid)
        return;

    const UINT wantW = KnownWindowWidth();
    const UINT wantH = KnownWindowHeight();
    if (wantW < 640 || wantH < 360 || wantW > 4096 || wantH > 4096)
        return;

    auto sizeOk = [&]() -> bool {
        if (!m_D9HUDSurface)
            return false;
        D3DSURFACE_DESC d{};
        if (FAILED(m_D9HUDSurface->GetDesc(&d)))
            return false;
        return d.Width >= 640 && d.Height >= 360
            && std::abs(static_cast<int>(d.Width) - static_cast<int>(wantW)) <= 32
            && std::abs(static_cast<int>(d.Height) - static_cast<int>(wantH)) <= 32;
    };

    if (sizeOk())
    {
        if (!m_VKHUD.m_VRTexture.handle && m_D9HUDSurface)
            FillSharedTexture(m_D9HUDSurface, m_VKHUD);
        if (m_VKHUD.m_VRTexture.handle)
        {
            if (!m_HudOverlayReady)
            {
                m_HudOverlayReady = true;
                ClearHudSurface(false);
                Game::logMsg("HUD overlay RT ready surf=%p (D3D A8R8G8B8, transparent)",
                    (void*)m_D9HUDSurface);
            }
            return;
        }
        static int s_waitVk;
        if (s_waitVk < 6)
        {
            Game::logMsg("HUD overlay D3D %ux%u surf=%p missing vk handle",
                wantW, wantH, (void*)m_D9HUDSurface);
            ++s_waitVk;
        }
        return;
    }

    if (!g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    {
        std::lock_guard<TextureStateMutex> lock(m_TextureMutex);
        ReleaseT(m_D9HUDSurface);
        ReleaseT(m_D9HUDTexture);
        m_VKHUD = SharedTextureHolder{};
        m_HudOverlayReady = false;
        m_CreatingTextureID = Texture_HUD;
        const HRESULT hr = device->CreateTexture(wantW, wantH, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_D9HUDTexture, nullptr);
        m_CreatingTextureID = Texture_None;
        if (FAILED(hr) || !m_D9HUDTexture)
        {
            Game::logMsg("HUD D3D CreateTexture failed hr=0x%08X %ux%u",
                (unsigned)hr, wantW, wantH);
            device->Release();
            return;
        }
        if (!m_D9HUDSurface)
            m_D9HUDTexture->GetSurfaceLevel(0, &m_D9HUDSurface);
        if (m_D9HUDSurface && !m_VKHUD.m_VRTexture.handle)
            FillSharedTexture(m_D9HUDSurface, m_VKHUD);
        Game::logMsg("HUD D3D RT %ux%u surf=%p vk=%p",
            wantW, wantH, (void*)m_D9HUDSurface, m_VKHUD.m_VRTexture.handle);
    }

    if (m_D9HUDSurface && m_VKHUD.m_VRTexture.handle)
    {
        m_HudOverlayReady = true;
        ClearHudSurface(false);
        Game::logMsg("HUD overlay RT ready surf=%p (D3D A8R8G8B8, transparent)",
            (void*)m_D9HUDSurface);
    }
    else
    {
        static int s_wait;
        if (s_wait < 6)
        {
            Game::logMsg("HUD overlay waiting D3D surf=%p vk=%p",
                (void*)m_D9HUDSurface, m_VKHUD.m_VRTexture.handle);
            ++s_wait;
        }
    }
    device->Release();
}

void VR::BlitPauseMenuToHudOverlay()
{
    // HWND StretchRect copies the stereo world plus GameUI. HL2VR paints
    // VGUI onto a cleared-alpha RT instead (PaintVguiToOverlay).
}

void VR::ClearHudSurface(bool opaque)
{
    if (!m_D9HUDSurface)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(m_D9HUDSurface->GetDevice(&device)) || !device)
        return;
    IDirect3DSurface9* prev = nullptr;
    device->GetRenderTarget(0, &prev);
    if (SUCCEEDED(device->SetRenderTarget(0, m_D9HUDSurface)))
    {
        const D3DCOLOR color = opaque ? D3DCOLOR_ARGB(255, 0, 0, 0) : D3DCOLOR_ARGB(0, 0, 0, 0);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, color, 1.f, 0);
        if (prev)
            device->SetRenderTarget(0, prev);
    }
    if (prev)
        prev->Release();
    device->Release();
}

void VR::NoteEngineHudRtPush(const char* name, int w, int h)
{
    if (!m_IsVREnabled || !IsGameplayEligible())
        return;
    if (!m_HudOverlayReady || !m_D9HUDSurface)
        return;
    m_EngineHudRtPushed = true;
    static int s_log;
    if (s_log < 8)
    {
        Game::logMsg("HUD engine PushRT %s %dx%d (copy to bmvrHUD on pop; engine dest kept)",
            name && name[0] ? name : "?", w, h);
        ++s_log;
    }
}

void VR::BlitEngineHudRtToOverlay()
{
    const bool pushed = m_EngineHudRtPushed;
    m_EngineHudRtPushed = false;
    if (!pushed || !m_D9HUDSurface || !g_D3DVR9)
        return;
    // Pause overlay is extra VGui_Paint into bmvrHUD, not a copy of _rt_gui.
    if (WantPauseWorldOverlay())
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* src = nullptr;
    if (FAILED(device->GetRenderTarget(0, &src)) || !src)
    {
        device->Release();
        return;
    }
    if (src == m_D9HUDSurface)
    {
        src->Release();
        device->Release();
        return;
    }
    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};
    src->GetDesc(&srcDesc);
    m_D9HUDSurface->GetDesc(&dstDesc);
    const HRESULT hr = device->StretchRect(src, nullptr, m_D9HUDSurface, nullptr, D3DTEXF_LINEAR);
    if (SUCCEEDED(hr))
        NoteHudPainted();
    static int s_blitLog;
    if (s_blitLog < 8)
    {
        Game::logMsg("HUD blit engineRT %ux%u fmt=0x%X -> bmvrHUD %ux%u hr=0x%08X painted=%d",
            srcDesc.Width, srcDesc.Height, srcDesc.Format,
            dstDesc.Width, dstDesc.Height, hr, SUCCEEDED(hr) ? 1 : 0);
        ++s_blitLog;
    }
    src->Release();
    device->Release();
}

void VR::SubmitHudOverlay()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    if (!m_HudOverlayReady || !m_Overlay || m_HUDTopHandle == vr::k_ulOverlayHandleInvalid)
        return;
    if (!m_VKHUD.m_VRTexture.handle)
        return;

    // After UnlockSubmissionQueue only. Nested LockSubmissionQueue deadlocks
    // DXVK m_mutexQueue. Do not ShowOverlay a cleared-black RT: VGui_Paint
    // was skipped last launch and this was the opaque black square in the HMD.
    if (!m_HudPaintedThisFrame.load(std::memory_order_acquire))
    {
        m_Overlay->HideOverlay(m_HUDTopHandle);
        static int s_hideLog;
        if (s_hideLog < 4)
        {
            Game::logMsg("HUD overlay hidden (VGUI has not painted bmvrHUD this frame)");
            ++s_hideLog;
        }
        return;
    }

    const bool pauseUi = WantPauseWorldOverlay();
    // Gameplay extra-paint is CEngineVGui PAINT_UIPANELS (GameUI / pause
    // chrome), not client DRAWHUD / _rt_Hud. Showing that on a transparent
    // SteamVR overlay every frame is the floating pause-menu glass in the HMD
    // (log: extra-paint mode=0x7 pause=0, overlay shown). HEV HUD cannot come
    // from this path. Loading GameUI is not paused and must not stick here.
    if (!pauseUi)
    {
        m_Overlay->HideOverlay(m_HUDTopHandle);
        static int s_hidePlay;
        if (s_hidePlay < 4)
        {
            Game::logMsg("HUD overlay hidden (gameplay; skip GameUI extra-paint)");
            ++s_hidePlay;
        }
        m_HudPaintedThisFrame.store(false, std::memory_order_release);
        return;
    }
    // Pause needs a readable quad. Do not enlarge until VGUI has painted.
    float widthM = 0.f, distM = 0.f, downM = 0.f;
    GetMenuOverlayMetrics(distM, widthM, downM);
    vr::HmdMatrix34_t rel{};
    rel.m[0][0] = 1.f;
    rel.m[1][1] = 1.f;
    rel.m[2][2] = 1.f;
    rel.m[1][3] = downM;
    rel.m[2][3] = -distM;
    m_Overlay->SetOverlayTransformTrackedDeviceRelative(
        m_HUDTopHandle, vr::k_unTrackedDeviceIndex_Hmd, &rel);
    m_Overlay->SetOverlayWidthInMeters(m_HUDTopHandle, widthM);
    m_Overlay->SetOverlayInputMethod(m_HUDTopHandle,
        pauseUi ? vr::VROverlayInputMethod_Mouse : vr::VROverlayInputMethod_None);
    m_Overlay->ShowOverlay(m_HUDTopHandle);
    static int s_showLog;
    if (s_showLog < 4)
    {
        Game::logMsg("HUD overlay shown distance=%.2f m width=%.2f m pause=%d",
            distM, widthM, pauseUi ? 1 : 0);
        ++s_showLog;
    }
    m_HudPaintedThisFrame.store(false, std::memory_order_release);
}

void VR::BindHudOverlayWhileQueueLocked()
{
    if (m_OpenXrHelperBridgeActive)
        return;
    if (!m_HudOverlayReady || !m_Overlay || m_HUDTopHandle == vr::k_ulOverlayHandleInvalid)
        return;
    if (!m_VKHUD.m_VRTexture.handle)
        return;
    if (!m_HudPaintedThisFrame.load(std::memory_order_acquire))
        return;
    if (!WantPauseWorldOverlay())
        return;
    m_Overlay->SetOverlayTexture(m_HUDTopHandle, &m_VKHUD.m_VRTexture);
    static int s_hudBindLog;
    if (s_hudBindLog < 2)
    {
        Game::logMsg("HUD overlay SetOverlayTexture while queue locked");
        ++s_hudBindLog;
    }
}

void VR::CreateVRTextures()
{
    Game::logMsg("CreateVRTextures begin presents=%u", m_EligiblePresents);
    if (!g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    RefreshBackBufferTexture(true);
    EnsurePrivateEyeSurfaces(device);
    device->Release();
    // HUD RT is created on first in-game pause extra-paint, not every
    // gameplay eye recreate.
}

void VR::BeginStereoEyeBlit(IDirect3DSurface9* dst)
{
    m_StereoEyeBlitDest = dst;
    m_StereoEyeBlitActive = dst != nullptr;
    m_StereoEyeBlitOk = false;
    m_FlashlightLive = false;
    if (dst == m_D9LeftEyeSurface)
        m_LeftEyeMsaaHasScene = false;
    else if (dst == m_D9RightEyeSurface)
        m_RightEyeMsaaHasScene = false;
    m_StereoRedirectedToEye = false;
    m_StereoEyeBlitRank = 0;
}

void VR::ClearStereoEyeSurfaces()
{
    // Engine nClearFlags on stereo eyes is DEPTH|STENCIL only, and that clear
    // still uses a 16:9 viewport/rect. Color in the extra square-eye pixels
    // then stacks previous frusta as the HMD moves. Wipe the actual D3D eye
    // surfaces at full HMD size before RenderView. Do not guess matsys vtable
    // ClearBuffers (that produced hall-of-mirrors trails).
    if (!g_D3DVR9 || !m_StereoEyeBlitDest || m_RenderWidth < 640 || m_RenderHeight < 360)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    IDirect3DSurface9* color = m_StereoEyeBlitDest;
    IDirect3DSurface9* depth = nullptr;
    if (color == m_D9LeftEyeSurface)
        depth = m_D9LeftEyeDepthSurface;
    else if (color == m_D9RightEyeSurface)
        depth = m_D9RightEyeDepthSurface;

    IDirect3DSurface9* oldRt = nullptr;
    IDirect3DSurface9* oldDs = nullptr;
    D3DVIEWPORT9 oldVp{};
    device->GetRenderTarget(0, &oldRt);
    device->GetDepthStencilSurface(&oldDs);
    device->GetViewport(&oldVp);

    const bool prevReentry = m_CaptureReentry;
    m_CaptureReentry = true;
    if (g_OrigSetRenderTarget)
        g_OrigSetRenderTarget(device, 0, color);
    else
        device->SetRenderTarget(0, color);
    if (depth)
    {
        if (g_OrigSetDepthStencil)
            g_OrigSetDepthStencil(device, depth);
        else
            device->SetDepthStencilSurface(depth);
    }

    D3DVIEWPORT9 eyeVp{};
    eyeVp.X = 0;
    eyeVp.Y = 0;
    eyeVp.Width = m_RenderWidth;
    eyeVp.Height = m_RenderHeight;
    eyeVp.MinZ = 0.f;
    eyeVp.MaxZ = 1.f;
    if (g_OrigSetViewport)
        g_OrigSetViewport(device, &eyeVp);
    else
        device->SetViewport(&eyeVp);

    DWORD flags = D3DCLEAR_TARGET;
    if (depth)
        flags |= D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL;
    HRESULT hr = E_FAIL;
    if (g_OrigClear)
        hr = g_OrigClear(device, 0, nullptr, flags, D3DCOLOR_ARGB(0, 0, 0, 0), 1.f, 0);
    else
        hr = device->Clear(0, nullptr, flags, D3DCOLOR_ARGB(0, 0, 0, 0), 1.f, 0);

    if (oldRt)
    {
        if (g_OrigSetRenderTarget)
            g_OrigSetRenderTarget(device, 0, oldRt);
        else
            device->SetRenderTarget(0, oldRt);
    }
    if (oldDs)
    {
        if (g_OrigSetDepthStencil)
            g_OrigSetDepthStencil(device, oldDs);
        else
            device->SetDepthStencilSurface(oldDs);
    }
    if (g_OrigSetViewport)
        g_OrigSetViewport(device, &oldVp);
    else
        device->SetViewport(&oldVp);
    m_CaptureReentry = prevReentry;
    if (oldRt)
        oldRt->Release();
    if (oldDs)
        oldDs->Release();

    static int s_clr;
    if (s_clr < 4)
    {
        Game::logMsg("D3D Clear stereo eye %ux%u depth=%d hr=0x%08X",
            m_RenderWidth, m_RenderHeight, depth ? 1 : 0, (unsigned)hr);
        ++s_clr;
    }
    device->Release();
}

bool VR::EndStereoEyeBlit()
{
    const bool ok = m_StereoEyeBlitOk;
    m_StereoEyeBlitActive = false;
    m_StereoEyeBlitDest = nullptr;
    return ok;
}

bool VR::StereoUnbindMatchesEye() const
{
    if (bmvr::UseGbMatchViewLock())
        return false;
    const int dw = static_cast<int>(m_LastStereoBlitWidth) - static_cast<int>(m_RenderWidth);
    const int dh = static_cast<int>(m_LastStereoBlitHeight) - static_cast<int>(m_RenderHeight);
    return dw > -32 && dw < 32 && dh > -32 && dh < 32
        && m_LastStereoBlitWidth >= 640 && m_LastStereoBlitHeight >= 360;
}

void VR::FlushStereoBlitGpu()
{
    // Both eyes share _rt_FullFrameFB. StretchRect is queued; the right-eye
    // RenderView overwrites that RT before the left copy lands, so both eyes
    // submit the same image and near field cannot fuse. Event-query flush is
    // not WaitDeviceIdle (that crash-skipped during load).
    // One GetData(FLUSH) waits for the whole left eye. 64 FLUSH polls serialized
    // complex scenes (~2x GPU time). The HMD-fb blit path always flushes once
    // after the left copy; DXVK async will otherwise submit the same BB twice.
    if (!g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    if (!m_BlitEventQuery)
    {
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &m_BlitEventQuery)) || !m_BlitEventQuery)
        {
            static int s_qfail;
            if (s_qfail < 3)
            {
                Game::logMsg("Stereo blit GPU event-query create failed");
                ++s_qfail;
            }
            device->Release();
            return;
        }
    }
    m_BlitEventQuery->Issue(D3DISSUE_END);
    const HRESULT hr = m_BlitEventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    static int s_qok;
    if (s_qok < 2)
    {
        Game::logMsg("Stereo blit GPU event-query flush hr=0x%08X", (unsigned)hr);
        ++s_qok;
    }
    device->Release();
}

void VR::CaptureGameColorOnUnbind(IDirect3DSurface9* oldRt, uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH)
{
    (void)vpX;
    (void)vpY;
    (void)vpW;
    (void)vpH;
    // Menu/Present unbind StretchRect raced (2026-08-16). Only copy during an
    // eye RenderView, while FullFrameFB still holds the HMD-aspect scene.
    // client.dll RenderView (Ghidra 1020f5e4) restores the prologue RT — the
    // 16:9 D3D backbuffer — before our post-RenderView blit, so RT0 after
    // callOriginal is the wrong aspect (near fusion only at distance).
    if (!m_StereoEyeBlitActive || !m_StereoEyeBlitDest || !oldRt || m_CaptureReentry)
        return;
    // Native offscreen paints LDR onto the eyes via SetRT redirect. Copying
    // A2R10 FullFrame here is the ff_hmdfit white HMD.
    if (bmvr::OffscreenWorldMatchesEyes())
        return;
    if (oldRt == m_StereoEyeBlitDest || oldRt == m_D9LeftEyeSurface || oldRt == m_D9RightEyeSurface
        || oldRt == m_D9FrameColorSurface || oldRt == m_D9BlankSurface)
        return;

    UINT w = 0, h = 0;
    D3DSURFACE_DESC desc{};
    if (!ResolveSurfaceSize(oldRt, w, h, &desc) || w < 640 || h < 360)
        return;
    if (desc.Format == D3DFMT_D16 || desc.Format == D3DFMT_D24S8 || desc.Format == D3DFMT_D24X8
        || desc.Format == D3DFMT_D32 || desc.Format == D3DFMT_D24FS8)
        return;

    const int rank = SceneColorRank(desc.Format);
    if (rank <= 0)
        return;
    if (m_StereoEyeBlitOk && rank < m_StereoEyeBlitRank)
        return;

    const int eyeW = static_cast<int>(m_RenderWidth);
    const int eyeH = static_cast<int>(m_RenderHeight);
    if (eyeW < 640 || eyeH < 360)
        return;
    if (abs(static_cast<int>(w) - eyeW) > 32 || abs(static_cast<int>(h) - eyeH) > 32)
        return;

    IDirect3DDevice9* device = nullptr;
    if (!g_D3DVR9 || FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    m_CaptureReentry = true;
    HRESULT hr = device->StretchRect(oldRt, nullptr, m_StereoEyeBlitDest, nullptr, D3DTEXF_NONE);
    if (FAILED(hr))
    {
        IDirect3DSurface9* submit = SubmitSurfaceForEye(m_StereoEyeBlitDest);
        if (submit && submit != m_StereoEyeBlitDest)
            hr = device->StretchRect(oldRt, nullptr, submit, nullptr, D3DTEXF_NONE);
    }
    else
        NoteMsaaEyeScene(m_StereoEyeBlitDest, true);
    m_CaptureReentry = false;
    device->Release();

    if (SUCCEEDED(hr))
    {
        m_StereoEyeBlitOk = true;
        m_StereoEyeBlitRank = rank;
        m_LastStereoBlitWidth = w;
        m_LastStereoBlitHeight = h;
        static int s_unbindBlitLog;
        if (s_unbindBlitLog < 12)
        {
            Game::logMsg("Stereo unbind blit 1:1 eye=%d %ux%u fmt=%u rank=%d src=%p dest=%p",
                m_StereoEye, w, h, (unsigned)desc.Format, rank, (void*)oldRt, (void*)m_StereoEyeBlitDest);
            ++s_unbindBlitLog;
        }
    }
    else
    {
        static int s_unbindFailLog;
        if (s_unbindFailLog < 6)
        {
            Game::logMsg("Stereo unbind blit failed hr=0x%08X src=%ux%u fmt=%u",
                (unsigned)hr, w, h, (unsigned)desc.Format);
            ++s_unbindFailLog;
        }
    }
}

bool VR::BlitCurrentGameColorTo(IDirect3DSurface9* dst, bool flushGpu)
{
    if (!dst || !g_D3DVR9)
        return false;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;

    IDirect3DSurface9* rt0 = nullptr;
    device->GetRenderTarget(0, &rt0);
    IDirect3DSurface9* bb = nullptr;
    UINT w = 0, h = 0;
    IDirect3DSurface9* src = rt0;
    if (!ResolveSurfaceSize(rt0, w, h) || w < 640 || h < 360)
    {
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        src = bb;
        ResolveSurfaceSize(src, w, h);
    }

    bool ok = false;
    if (src && w >= 640 && h >= 360)
    {
        RECT srcRect{};
        const RECT* srcPtr = nullptr;
        D3DVIEWPORT9 vp{};
        UINT cropW = w, cropH = h;
        LONG x0 = 0, y0 = 0;
        if (SUCCEEDED(device->GetViewport(&vp)) && vp.Width >= 640 && vp.Height >= 360
            && vp.Width <= w && vp.Height <= h)
        {
            x0 = static_cast<LONG>(vp.X);
            y0 = static_cast<LONG>(vp.Y);
            cropW = vp.Width;
            cropH = vp.Height;
        }
        else if (m_RenderWidth >= 640 && m_RenderHeight >= 360
            && w >= m_RenderWidth && h >= m_RenderHeight)
        {
            cropW = m_RenderWidth;
            cropH = m_RenderHeight;
        }
        if (cropW != w || cropH != h || x0 != 0 || y0 != 0)
        {
            srcRect = { x0, y0, x0 + static_cast<LONG>(cropW), y0 + static_cast<LONG>(cropH) };
            srcPtr = &srcRect;
        }
        m_CaptureReentry = true;
        HRESULT hr = device->StretchRect(src, srcPtr, dst, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
        {
            IDirect3DSurface9* submit = SubmitSurfaceForEye(dst);
            if (submit && submit != dst)
                hr = device->StretchRect(src, srcPtr, submit, nullptr, D3DTEXF_NONE);
        }
        else
            NoteMsaaEyeScene(dst, true);
        m_CaptureReentry = false;
        ok = SUCCEEDED(hr);
        static int s_blitLog;
        if (s_blitLog < 8)
        {
            Game::logMsg("Stereo blit fallback RT0/bb %ux%u crop=%ux%u hr=0x%08X (want HMD %ux%u)",
                w, h, cropW, cropH, (unsigned)hr, m_RenderWidth, m_RenderHeight);
            ++s_blitLog;
        }
        if (ok)
        {
            if (flushGpu)
                FlushStereoBlitGpu();
        }
    }

    if (rt0)
        rt0->Release();
    if (bb)
        bb->Release();
    device->Release();
    return ok;
}

void VR::CaptureFrameBeforePresent()
{
    if (!m_IsVREnabled || !g_D3DVR9)
        return;
    if (McPipelineEnabled())
    {
        BlitDesktopMirrorToBackbuffer();
        return;
    }
    if (!ShouldCompositorSubmit())
        return;

    const bool panel2d = Want2dMenuPanel();
    // Stereo eyes skip this capture. Empty-menu / load 2D uses the HWND
    // backbuffer. In-game pause keeps stereo eyes; GameUI is extra-painted
    // onto bmvrHUD (HL2VR RenderHUD), not copied from the desktop window.
    if (m_DirectEyeSubmit && !panel2d)
        return;

    if (m_FrameCopyLatched && m_D9FrameColorSurface)
    {
        m_RenderedNewFrame.store(true, std::memory_order_release);
        return;
    }

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    IDirect3DSurface9* rt0 = nullptr;
    device->GetRenderTarget(0, &rt0);

    UINT probeW = 0, probeH = 0;
    const bool rtUsable = ResolveSurfaceSize(rt0, probeW, probeH) && probeW >= 640 && probeH >= 360;
    const bool rtIsEye = rt0 && (
        rt0 == m_D9LeftEyeSurface || rt0 == m_D9RightEyeSurface
        || rt0 == m_D9LeftEyeSubmitSurface || rt0 == m_D9RightEyeSubmitSurface);

    IDirect3DSurface9* bb = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);

    // GameUI lives on the HWND backbuffer. RT0 after CreateVRTextures can be
    // an empty HMD-sized eye — copying that published black even with Submit.
    const bool preferBb = panel2d || rtIsEye;
    IDirect3DSurface9* srcSurf = (preferBb && bb) ? bb : (rtUsable ? rt0 : bb);

    UINT rtW = 0, rtH = 0;
    D3DSURFACE_DESC rtDesc{};
    const bool rtOk = ResolveSurfaceSize(srcSurf, rtW, rtH, &rtDesc);
    const UINT winW = KnownWindowWidth();
    const UINT winH = KnownWindowHeight();

    D3DVIEWPORT9 vp{};
    const bool vpRawOk = SUCCEEDED(device->GetViewport(&vp)) && vp.Width >= 640 && vp.Height >= 360;
    const bool vpOk = vpRawOk && vp.Width <= winW + 16 && vp.Height <= winH + 16;

    LONG x0 = 0, y0 = 0;
    UINT cropW = 0, cropH = 0;
    const char* name = "none";
    if (rtOk && srcSurf && rtW >= 640 && rtH >= 360)
    {
        if (srcSurf == bb)
        {
            cropW = rtW;
            cropH = rtH;
            name = "bb";
        }
        else if (vpOk)
        {
            x0 = (LONG)vp.X;
            y0 = (LONG)vp.Y;
            cropW = (std::min)((UINT)vp.Width, winW);
            cropH = (std::min)((UINT)vp.Height, winH);
            name = "rt0-vp";
        }
        else if (rtW > winW + 16 || rtH > winH + 16)
        {
            cropW = (std::min)(rtW, winW);
            cropH = (std::min)(rtH, winH);
            x0 = 0;
            y0 = (rtH > cropH) ? (LONG)(rtH - cropH) : 0;
            name = "rt0-bl";
        }
        else
        {
            cropW = rtW;
            cropH = rtH;
            name = "rt0";
        }
    }

    HRESULT hr = E_FAIL;
    if (cropW >= 640 && cropH >= 360 && EnsureFrameCopySurface(device, cropW, cropH))
    {
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        cropW = (std::min)(cropW, rtW - (UINT)x0);
        cropH = (std::min)(cropH, rtH - (UINT)y0);
        RECT src{ x0, y0, x0 + (LONG)cropW, y0 + (LONG)cropH };
        const bool useRect = (x0 != 0 || y0 != 0 || rtW > winW + 16 || rtH > winH + 16);
        m_CaptureReentry = true;
        hr = device->StretchRect(srcSurf, useRect ? &src : nullptr, m_D9FrameColorSurface, nullptr, D3DTEXF_NONE);
        m_CaptureReentry = false;
        if (SUCCEEDED(hr))
        {
            m_RenderedNewFrame.store(true, std::memory_order_release);
            m_FrameCopyLatched = true;
            static int s_preLog;
            if (s_preLog < 6)
            {
                Game::logMsg("PrePresent capture %s %ux%u from src=%ux%u hr=0x%08X panel2d=%d",
                    name, cropW, cropH, rtW, rtH, (unsigned)hr, panel2d ? 1 : 0);
                ++s_preLog;
            }
            static int s_pause2dLog;
            if (panel2d && m_GameplayEligible && s_pause2dLog < 4)
            {
                Game::logMsg("Pause 2D panel capture %s %ux%u", name, cropW, cropH);
                ++s_pause2dLog;
            }
        }
        else
        {
            static int s_srLog;
            if (s_srLog < 6)
            {
                Game::logMsg("PrePresent StretchRect %s failed hr=0x%08X src=%ux%u crop=%ux%u",
                    name, (unsigned)hr, rtW, rtH, cropW, cropH);
                ++s_srLog;
            }
        }
    }
    else
    {
        static int s_noneLog;
        if (s_noneLog < 6)
        {
            UINT bbW = 0, bbH = 0;
            ResolveSurfaceSize(bb, bbW, bbH);
            Game::logMsg("PrePresent capture NONE (rt0=%dx%d bb=%ux%u win=%ux%u vp=%u,%u %ux%u)",
                rtUsable ? (int)probeW : -1, rtUsable ? (int)probeH : -1,
                bbW, bbH, winW, winH,
                vpRawOk ? vp.X : 0u, vpRawOk ? vp.Y : 0u,
                vpRawOk ? vp.Width : 0u, vpRawOk ? vp.Height : 0u);
            ++s_noneLog;
        }
    }

    if (rt0)
        rt0->Release();
    if (bb)
        bb->Release();
    device->Release();
}

void VR::SubmitVRTextures()
{
    if (!g_D3DVR9 || !m_IsVREnabled)
        return;
    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return;

    if (m_OpenXrHelperBridgeActive)
    {
        ++m_OpenXrSubmitAttempts;
        // Deferred publish: hand over any slot whose GPU copy has completed
        // since the last Present, before deciding whether to copy new eyes.
        PollOpenXrDeferredPublish();
        if (McPipelineEnabled())
        {
            LogOpenXrPublishRate();
            if (WantPauseWorldOverlay())
            {
                if (m_HudOverlayReady && m_D9HUDSurface)
                {
                    if (HudPaintedThisFrame())
                    {
                        g_D3DVR9->TransferSurface(m_D9HUDSurface, FALSE);
                        FillSharedTexture(m_D9HUDSurface, m_VKHUD);
                        m_HudPaintedThisFrame.store(false, std::memory_order_release);
                        m_HudOverlayHasImage = true;
                    }
                    if (m_HudOverlayHasImage)
                    {
                        const uint32_t overlayFrameId = m_OpenXrSubmitFrameId.fetch_add(1, std::memory_order_acq_rel);
                        PublishOpenXrHudOverlay(overlayFrameId);
                    }
                }
            }
            else if (m_OpenXrHudOverlayPublished)
                HideOpenXrHudOverlay();
            return;
        }
        const bool haveNewFrame = m_RenderedNewFrame.load(std::memory_order_acquire);
        // Present runs far faster than the compositor consumes (measured
        // 2026-08-30: ~212 publishes/s against 90Hz SteamVR, stereo pair only
        // 1.5-5ms). Every publish re-copies two full-res eye textures, and the
        // helper has no lock on the shared images: it blits a 3664x3584 eye
        // while the game overwrites that same texture 2-3 times, so its
        // swapchain copy mixes several game frames. That tears only when
        // consecutive frames differ, i.e. while the head turns. Publish one
        // pair per compositor frame: require freshly rendered eyes and cap the
        // copy rate.
        if (!haveNewFrame)
        {
            ++m_OpenXrSkippedNoNewFrame;
            LogOpenXrPublishRate();
            return;
        }
        // Waiting for the helper's submit counter published right after its
        // xrEndFrame, so the image then sat a whole compositor frame before it
        // was read and the ghost flipped to leading the turn. Rotating publish
        // slots make that handshake unnecessary: cap the copy cost instead and
        // let the helper pick up a much fresher image.
        //
        // Must be QPC, not GetTickCount: that counter only advances every
        // ~15.6ms, so a millisecond gate on it published ~64/s against a 90Hz
        // compositor and starved every third display frame.
        const double nowMs = QpcNowMs();
        // 7ms => ~143/s, so every 11.1ms compositor frame sees a fresh pair.
        if (m_OpenXrLastPublishMs != 0.0 && (nowMs - m_OpenXrLastPublishMs) < 7.0)
        {
            ++m_OpenXrSkippedHelperBusy;
            LogOpenXrPublishRate();
            return;
        }
        LogOpenXrPublishRate();
        const bool paused = m_Game && SehIsPaused(m_Game->m_EngineClient);
        const bool panel2d = Want2dMenuPanel();
        const bool stereoLayer = m_DirectEyeSubmit && !panel2d;
        const bool pauseKeepStereo = WantPauseWorldOverlay();
        // OpenVR copies PrePresent capture into both eyes here. The helper
        // only reads the private eye RTs, so menu / pass-through frames were
        // submitted empty (Link desktop in one layer, black stereo in another).
        // In-game pause must not overwrite stereo eyes with HWND GameUI.
        if (!stereoLayer && !pauseKeepStereo && m_D9FrameColorSurface && m_D9LeftEyeSurface)
        {
            IDirect3DDevice9* copyDev = nullptr;
            if (SUCCEEDED(g_D3DVR9->GetD3DDevice(&copyDev)) && copyDev)
            {
                m_CaptureReentry = true;
                DrawMenuCursorOnSurface(copyDev, m_D9FrameColorSurface);
                const bool leftOk = CopySurfaceLetterboxed(copyDev, m_D9FrameColorSurface, m_D9LeftEyeSurface,
                    bmvr::g_MenuPanelScale);
                bool rightOk = true;
                if (m_D9RightEyeSurface)
                    rightOk = CopySurfaceLetterboxed(copyDev, m_D9FrameColorSurface, m_D9RightEyeSurface,
                        bmvr::g_MenuPanelScale);
                m_CaptureReentry = false;
                copyDev->Release();
                static int s_monoCopyLog;
                if (s_monoCopyLog < 4)
                {
                    Game::logMsg("OpenXR mono capture letterboxed %ux%u -> eyes scale=%.2f pause2d=%d paused=%d ok=%d/%d",
                        m_FrameCopyWidth, m_FrameCopyHeight, bmvr::g_MenuPanelScale,
                        panel2d ? 1 : 0, paused ? 1 : 0,
                        leftOk ? 1 : 0, rightOk ? 1 : 0);
                    ++s_monoCopyLog;
                }
                static int s_pauseCopyLog;
                if (panel2d && m_GameplayEligible && s_pauseCopyLog < 4)
                {
                    Game::logMsg("Pause 2D letterboxed %ux%u scale=%.2f ok=%d/%d",
                        m_FrameCopyWidth, m_FrameCopyHeight, bmvr::g_MenuPanelScale,
                        leftOk ? 1 : 0, rightOk ? 1 : 0);
                    ++s_pauseCopyLog;
                }
                m_FrameCopyLatched = false;
            }
        }
        // The pose these eyes were drawn with travels with the copy; a
        // deferred slot is published one or more Presents later, when the
        // live pose already belongs to a newer frame.
        OpenXrPendingPublish pend{};
        pend.panel2d = panel2d;
        if (stereoLayer && m_OpenXrStereoRenderPoseValid)
        {
            pend.pose = m_OpenXrStereoRenderPose;
            pend.havePose = true;
        }
        else if (panel2d && m_MenuPanelPoseValid)
        {
            pend.pose = m_MenuPanelPose;
            pend.havePose = true;
        }
        else if (m_OpenXrLastHmdPose.valid)
        {
            pend.pose = m_OpenXrLastHmdPose;
            pend.pose.reserved0 = 0;
            pend.pose.reserved1 |= L4D2VR_OPENXR_POSE_FLAG_MONO;
            pend.havePose = true;
        }
        if (!PrepareOpenXrEyeSurfacesForRead(pend))
            return;
        m_OpenXrLastPublishMs = QpcNowMs();
        // Eyes are consumed (copied into a slot, or published live).
        m_RenderedNewFrame.store(false, std::memory_order_release);
        m_FrameCopyLatched = false;
        // The overlay has its own generation on the helper side; it does not
        // need to match the eye frame id.
        const uint32_t overlayFrameId = m_OpenXrSubmitFrameId.fetch_add(1, std::memory_order_acq_rel);
        if (WantPauseWorldOverlay() && !McPipelineEnabled())
        {
            static int s_pause3dLog;
            if (s_pause3dLog < 8)
            {
                Game::logMsg("Pause overlay on stereo painted=%d stereoLayer=%d epoch=%u ready=%d",
                    HudPaintedThisFrame() ? 1 : 0, stereoLayer ? 1 : 0,
                    m_MenuOverlayEpoch, m_HudOverlayReady ? 1 : 0);
                ++s_pause3dLog;
            }
            if (m_HudOverlayReady && m_D9HUDSurface)
            {
                if (HudPaintedThisFrame())
                {
                    g_D3DVR9->TransferSurface(m_D9HUDSurface, FALSE);
                    FillSharedTexture(m_D9HUDSurface, m_VKHUD);
                    m_HudPaintedThisFrame.store(false, std::memory_order_release);
                    m_HudOverlayHasImage = true;
                }
                if (m_HudOverlayHasImage)
                    PublishOpenXrHudOverlay(overlayFrameId);
            }
        }
        else if (m_OpenXrHudOverlayPublished)
            HideOpenXrHudOverlay();
        // CPU-bound frames: the GPU may already have finished the copy by now.
        PollOpenXrDeferredPublish();
        return;
    }

    if (!m_Compositor)
        return;
    if (!m_CreatedVRTextures.load(std::memory_order_acquire))
        return;
    const int poseErr = m_LastPoseWaitError.load(std::memory_order_acquire);
    if (poseErr == static_cast<int>(vr::VRCompositorError_DoNotHaveFocus))
    {
        if (m_Compositor)
            m_Compositor->CompositorBringToFront();
        static int s_focusSubmitLog;
        if (s_focusSubmitLog < 4)
        {
            Game::logMsg("Submit despite DoNotHaveFocus (bring compositor to front)");
            ++s_focusSubmitLog;
        }
    }

    const bool haveNewFrame = m_RenderedNewFrame.load(std::memory_order_acquire);
    const bool panel2d = Want2dMenuPanel();
    const bool pauseKeepEyes = WantPauseWorldOverlay()
        && m_D9LeftEyeSurface && m_D9RightEyeSurface;
    const bool directEyes = ((haveNewFrame && m_DirectEyeSubmit && !panel2d)
        || pauseKeepEyes)
        && m_D9LeftEyeSurface && m_D9RightEyeSurface;
    const bool haveFrame = haveNewFrame && m_D9FrameColorSurface;
    if (!directEyes && !haveFrame)
        return;

    const uint32_t poseToken = m_PoseWaitCount.load(std::memory_order_acquire);
    if (poseToken == 0)
        return;
    if (poseToken == m_LastSubmittedPoseToken.load(std::memory_order_acquire))
        return;

    static int s_submitEnter;
    if (s_submitEnter < 4)
    {
        Game::logMsg("Submit enter direct=%d frame=%d named=%d eye=%ux%u",
            directEyes ? 1 : 0, haveFrame ? 1 : 0, m_UsedNamedRenderTargets ? 1 : 0,
            m_RenderWidth, m_RenderHeight);
        ++s_submitEnter;
    }

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;

    HRESULT hrL = S_OK, hrR = S_OK;
    LONG usedOff = 0;
    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};

    if (!directEyes)
    {
        const bool haveSrc = SUCCEEDED(m_D9FrameColorSurface->GetDesc(&srcDesc)) && srcDesc.Width >= 64 && srcDesc.Height >= 64;
        const bool haveDst = m_D9LeftEyeSurface && SUCCEEDED(m_D9LeftEyeSurface->GetDesc(&dstDesc)) && dstDesc.Width >= 64;

        hrL = E_FAIL;
        hrR = E_FAIL;
        if (haveSrc && haveDst && m_D9LeftEyeSurface)
        {
            m_CaptureReentry = true;
            if (m_StereoCopyOffset && m_D9RightEyeSurface)
            {
                LONG useOff = m_StereoOffsetPx;
                if (useOff <= 0)
                    useOff = (std::max)(8L, (LONG)(srcDesc.Width / 64));
                const LONG maxOff = (LONG)(srcDesc.Width / 8);
                if (useOff > maxOff) useOff = maxOff;
                usedOff = useOff;
                RECT leftSrc{ useOff, 0, (LONG)srcDesc.Width, (LONG)srcDesc.Height };
                RECT rightSrc{ 0, 0, (LONG)srcDesc.Width - useOff, (LONG)srcDesc.Height };
                hrL = device->StretchRect(m_D9FrameColorSurface, &leftSrc, m_D9LeftEyeSurface, nullptr, D3DTEXF_LINEAR);
                hrR = device->StretchRect(m_D9FrameColorSurface, &rightSrc, m_D9RightEyeSurface, nullptr, D3DTEXF_LINEAR);
                if (SUCCEEDED(hrL))
                    NoteMsaaEyeScene(m_D9LeftEyeSurface, true);
                else if (m_D9LeftEyeSubmitSurface)
                    hrL = device->StretchRect(m_D9FrameColorSurface, &leftSrc, m_D9LeftEyeSubmitSurface, nullptr, D3DTEXF_LINEAR);
                if (SUCCEEDED(hrR))
                    NoteMsaaEyeScene(m_D9RightEyeSurface, true);
                else if (m_D9RightEyeSubmitSurface)
                    hrR = device->StretchRect(m_D9FrameColorSurface, &rightSrc, m_D9RightEyeSubmitSurface, nullptr, D3DTEXF_LINEAR);
            }
            else
            {
                DrawMenuCursorOnSurface(device, m_D9FrameColorSurface);
                hrL = CopySurfaceLetterboxed(device, m_D9FrameColorSurface, m_D9LeftEyeSurface,
                    bmvr::g_MenuPanelScale) ? S_OK : E_FAIL;
                hrR = m_D9RightEyeSurface
                    ? (CopySurfaceLetterboxed(device, m_D9FrameColorSurface, m_D9RightEyeSurface,
                        bmvr::g_MenuPanelScale) ? S_OK : E_FAIL)
                    : hrL;
                if (SUCCEEDED(hrL))
                    NoteMsaaEyeScene(m_D9LeftEyeSurface, true);
                if (m_D9RightEyeSurface && SUCCEEDED(hrR))
                    NoteMsaaEyeScene(m_D9RightEyeSurface, true);
            }
            m_CaptureReentry = false;
        }

        if (FAILED(hrL))
        {
            device->Release();
            m_RenderedNewFrame.store(false, std::memory_order_release);
            m_FrameCopyLatched = false;
            return;
        }
    }
    else
    {
        if (m_D9LeftEyeSurface)
            m_D9LeftEyeSurface->GetDesc(&dstDesc);
        srcDesc = dstDesc;
        hrR = S_OK;
    }

    g_D3DVR9->LockDevice();
    ResolveMsaaEyesToSubmit(device);
    IDirect3DSurface9* leftSubmit = m_D9LeftEyeSubmitSurface ? m_D9LeftEyeSubmitSurface : m_D9LeftEyeSurface;
    IDirect3DSurface9* rightSubmit = m_D9RightEyeSubmitSurface ? m_D9RightEyeSubmitSurface : m_D9RightEyeSurface;
    const BOOL waitGpu = bmvr::TryWaitDeviceIdle() ? TRUE : FALSE;
    if (bmvr::TryWaitDeviceIdle())
        bmvr::BeginRisky(L"wait_idle");
    const bool okL = leftSubmit && SUCCEEDED(g_D3DVR9->TransferSurface(leftSubmit, waitGpu))
        && FillSharedTexture(leftSubmit, m_VKLeftEye);
    bool okR = false;
    if (rightSubmit && SUCCEEDED(hrR))
        okR = SUCCEEDED(g_D3DVR9->TransferSurface(rightSubmit, waitGpu)) && FillSharedTexture(rightSubmit, m_VKRightEye);
    if (WantPauseWorldOverlay() && m_HudOverlayReady && m_D9HUDSurface
        && m_HudPaintedThisFrame.load(std::memory_order_acquire))
    {
        g_D3DVR9->TransferSurface(m_D9HUDSurface, FALSE);
        FillSharedTexture(m_D9HUDSurface, m_VKHUD);
        m_HudPaintedThisFrame.store(false, std::memory_order_release);
        m_HudOverlayHasImage = true;
    }
    if (bmvr::TryWaitDeviceIdle() && g_D3DVR9)
        g_D3DVR9->WaitDeviceIdle();
    if (bmvr::TryWaitDeviceIdle())
        bmvr::EndRisky(L"wait_idle");
    g_D3DVR9->UnlockDevice();

    if (!okL)
    {
        device->Release();
        return;
    }

    if (WantPauseWorldOverlay())
        EnsureHudOverlay();

    if (FAILED(g_D3DVR9->LockSubmissionQueue()))
    {
        device->Release();
        return;
    }

    vr::VRTextureBounds_t leftBounds{};
    vr::VRTextureBounds_t rightBounds{};
    const float imgW = srcDesc.Width > 0 ? (float)srcDesc.Width : (float)dstDesc.Width;
    const float imgH = srcDesc.Height > 0 ? (float)srcDesc.Height : (float)dstDesc.Height;
    const float imgAspect = (imgW >= 64.f && imgH >= 64.f) ? (imgW / imgH) : 0.f;

    if (directEyes)
    {
        // L4D2VR vr_lifecycle_init.inl: eye textures are the hidden-area HMD
        // frustum (fov=m_Fov, aspect=m_Aspect). Submit GetProjectionRaw UVs.
        leftBounds = m_TextureBounds[0];
        rightBounds = m_TextureBounds[1];
        static int s_directBoundsLog;
        if (s_directBoundsLog < 3)
        {
            Game::logMsg("Direct eye Submit UV L=(%.3f,%.3f)-(%.3f,%.3f) R=(%.3f,%.3f)-(%.3f,%.3f) img=%gx%g",
                leftBounds.uMin, leftBounds.vMin, leftBounds.uMax, leftBounds.vMax,
                rightBounds.uMin, rightBounds.vMin, rightBounds.uMax, rightBounds.vMax,
                imgW, imgH);
            ++s_directBoundsLog;
        }
    }
    else
    {
        // 16:9 game blit is not an HMD-projection eye RT. GetProjectionRaw UVs
        // showed the left strip of the pause menu, 180° rotated, rest black
        // (WMR portal 2026-08-16). Keep v unflipped. Crop U so 16:9 matches
        // HMD aspect (~1.1) instead of stretching vertically into the FOV.
        leftBounds = { 0.f, 0.f, 1.f, 1.f };
        const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
        // Cropping U to HMD aspect (u=0.191..0.809) ate the left-aligned old
        // game-UI buttons. Keep full 16:9 on the menu / !inGame capture path.
        if (inGame && imgAspect > m_Aspect * 1.02f && m_Aspect > 0.1f)
        {
            const float vis = m_Aspect / imgAspect;
            const float du = (1.f - vis) * 0.5f;
            leftBounds.uMin = du;
            leftBounds.uMax = 1.f - du;
        }
        rightBounds = leftBounds;
        static int s_boundsLog;
        if (s_boundsLog < 3)
        {
            Game::logMsg("Capture Submit UV u=%.3f..%.3f v=%.3f..%.3f img=%ux%u hmdAspect=%.3f",
                leftBounds.uMin, leftBounds.uMax, leftBounds.vMin, leftBounds.vMax,
                srcDesc.Width, srcDesc.Height, m_Aspect);
            ++s_boundsLog;
        }
    }

    vr::EVRCompositorError timingErr = vr::VRCompositorError_None;
    if (m_CompositorAppHandoff)
    {
        timingErr = m_Compositor->SubmitExplicitTimingData();
        static int s_timingLog;
        if (s_timingLog < 4 || timingErr != vr::VRCompositorError_None)
        {
            Game::logMsg("HL2VR SubmitExplicitTimingData err=%d", (int)timingErr);
            ++s_timingLog;
        }
    }

    vr::EVRSubmitFlags submitFlags = vr::Submit_Default;
    vr::VRTextureWithPose_t leftWithPose{};
    vr::VRTextureWithPose_t rightWithPose{};
    const vr::Texture_t* leftTex = &m_VKLeftEye.m_VRTexture;
    const vr::Texture_t* rightTex = okR ? &m_VKRightEye.m_VRTexture : &m_VKLeftEye.m_VRTexture;
    if (m_StereoFrameTrackingPoseValid || m_HmdPoseValid)
    {
        const vr::HmdMatrix34_t pose = m_StereoFrameTrackingPoseValid
            ? m_StereoFrameTrackingPose : m_HmdTrackingPose;
        leftWithPose.handle = m_VKLeftEye.m_VRTexture.handle;
        leftWithPose.eType = m_VKLeftEye.m_VRTexture.eType;
        leftWithPose.eColorSpace = m_VKLeftEye.m_VRTexture.eColorSpace;
        leftWithPose.mDeviceToAbsoluteTracking = pose;
        rightWithPose.handle = rightTex->handle;
        rightWithPose.eType = rightTex->eType;
        rightWithPose.eColorSpace = rightTex->eColorSpace;
        rightWithPose.mDeviceToAbsoluteTracking = pose;
        leftTex = &leftWithPose;
        rightTex = &rightWithPose;
        submitFlags = vr::Submit_TextureWithPose;
    }

    vr::EVRCompositorError eL = m_Compositor->Submit(vr::Eye_Left, leftTex, &leftBounds, submitFlags);
    vr::EVRCompositorError eR = m_Compositor->Submit(vr::Eye_Right, rightTex, &rightBounds, submitFlags);

    BindHudOverlayWhileQueueLocked();

    g_D3DVR9->UnlockSubmissionQueue();
    SubmitHudOverlay();

    m_ActualCompositorSubmitCount.fetch_add(1, std::memory_order_relaxed);
    ++m_SubmitCount;
    m_LastSubmittedPoseToken.store(poseToken, std::memory_order_release);
    TryCompositorPostPresentHandoff(GetTickCount(), 0);

    if (!m_LoggedFirstSubmit)
    {
        m_LoggedFirstSubmit = true;
        Game::logMsg("OpenVR Submit %s src=%ux%u eye=%ux%u stereoCopy=%d off=%ld namedRT=%d eL=%d eR=%d pose=%d",
            directEyes ? "direct-eyes" : "capture",
            srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height,
            m_StereoCopyOffset ? 1 : 0, usedOff, m_UsedNamedRenderTargets ? 1 : 0, (int)eL, (int)eR,
            submitFlags == vr::Submit_TextureWithPose ? 1 : 0);
    }
    else if ((m_SubmitCount % 120) == 0)
    {
        Game::logMsg("OpenVR submit #%d eL=%d eR=%d direct=%d blit=%ux%u eye=%ux%u captured=%ux%u",
            m_SubmitCount, (int)eL, (int)eR, directEyes ? 1 : 0,
            m_LastStereoBlitWidth, m_LastStereoBlitHeight,
            m_RenderWidth, m_RenderHeight, m_FrameCopyWidth, m_FrameCopyHeight);
    }

    m_RenderedNewFrame.store(false, std::memory_order_release);
    m_FrameCopyLatched = false;
    if (eL == vr::VRCompositorError_None || eL == vr::VRCompositorError_AlreadySubmitted)
        m_HasSubmittedSceneFrame.store(true, std::memory_order_release);
    device->Release();
}

void VR::UpdateTracking()
{
    m_HmdPoseValid = false;
    vr::TrackedDevicePose_t hmd{};
    const vr::EVRCompositorError err = static_cast<vr::EVRCompositorError>(
        m_LastPoseWaitError.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(m_PoseMutex);
        hmd = m_WaitedPoses[vr::k_unTrackedDeviceIndex_Hmd];
    }
    if (m_WaitedPoseTick.load(std::memory_order_acquire) == 0)
        return;
    if (err == vr::VRCompositorError_DoNotHaveFocus)
    {
        if (m_Compositor)
            m_Compositor->CompositorBringToFront();
        static int s_focusLog;
        if (s_focusLog < 6)
        {
            Game::logMsg("WaitGetPoses DoNotHaveFocus (SteamVR waiting room still owns the compositor)");
            ++s_focusLog;
        }
        // Still apply the pose OpenVR returned. Skipping here freezes the
        // game camera at 10Hz while the compositor reprojects — rubberband
        // after one alt-tab.
    }
    else if (err != vr::VRCompositorError_None)
        return;

    if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
        return;

    QAngle ang = HmdMatrixToSourceAnglesWithRoll(hmd.mDeviceToAbsoluteTracking);
    if (!std::isfinite(ang.x) || !std::isfinite(ang.y) || !std::isfinite(ang.z))
        return;

    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        m_HmdAngAbs = ang;
        m_HmdPosAbs = HmdMatrixToSourcePos(hmd.mDeviceToAbsoluteTracking, m_VRScale);
        m_HmdTrackingPose = hmd.mDeviceToAbsoluteTracking;
        QAngle::AngleVectors(m_HmdAngAbs, &m_HmdForward, &m_HmdRight, &m_HmdUp);
        m_HmdPoseValid = true;
    }
    m_IpdScale = bmvr::g_IPDScale;
    RefreshIpdFromHmd();
    UpdateControllerTracking(hmd);

    UpdateScopeZoomSmooth();

    if (m_GameplayEligible)
    {
        const bool openXrLatch = m_OpenXrHelperBridgeActive && m_HmdPoseValid;
        const bool openVrLatch = m_HmdPosAbs.z > 24.f;
        if (!m_HmdOriginLatched && (openXrLatch || openVrLatch))
        {
            m_HmdPosAbsZero = m_HmdPosAbs;
            m_HmdAngAbsZero = m_HmdAngAbs;
            m_HmdOriginLatched = true;
            m_PrevAppliedHmdYaw = m_HmdAngAbs.y;
            m_PrevAppliedHmdPitch = m_HmdAngAbs.x;
            Game::logMsg("ResetPosition latch hmd=(%.1f,%.1f,%.1f) openxr=%d (engine eye + 6DOF)",
                m_HmdPosAbs.x, m_HmdPosAbs.y, m_HmdPosAbs.z,
                openXrLatch ? 1 : 0);
        }
        ++m_GameplayFrames;
        if (m_LookApplyWanted)
        {
            m_SafeLookActive = true;
            m_LookApplyEnabled = true;
        }
    }
}

void VR::Update()
{
    if (!m_Game)
        return;

    if (!m_IsVREnabled && m_RequestedRuntimeBackend != VrRuntimeBackend::OpenXR && m_OpenVRInitAttempts < 8)
        m_IsInitialized = InitOpenVR();
    if (!m_IsVREnabled && m_RequestedRuntimeBackend == VrRuntimeBackend::OpenXR && m_RuntimeBackend == VrRuntimeBackend::OpenVR
        && m_OpenVRInitAttempts < 8)
        m_IsInitialized = InitOpenVR();
    if (!m_IsVREnabled)
        return;

    PollMapFromEngine();
    const bool inGame = m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    // LevelInit names the map before IsInGame. Do not run AutoMatQueueMode,
    // pose, or input during that window — Present is about to stop for
    // material precache, and overnight Update work is not what unblocks it.
    if (m_GameplayEligible && !inGame)
        return;

    if (m_OpenXrHelperBridgeActive && !m_PosesWaitedThisFrame)
        ConsumeOpenXrTracking();

    TickCompositorFocus();

    // QoL cvars go through ICvar from RenderView (game thread). FindVar/SetValue
    // from this Present/DXVK path crashed ~13s after init (2026-08-26).
    // Do not UpdateAutoMatQueueMode from Present. GetThreadMode nested
    // materialsystem after the first stereo frame (x32dbg 2026-08-18).

    ++m_PresentTick;
    m_StereoEyesDrawnThisFrame = false;
    static DWORD s_fpsLogMs;
    static uint32_t s_fpsLogTick;
    static DWORD s_lastPresentMs;
    static DWORD s_lastStallLogMs;
    const DWORD nowMs = GetTickCount();
    if (s_lastPresentMs != 0 && m_GameplayEligible && inGame)
    {
        const DWORD intervalMs = nowMs - s_lastPresentMs;
        if (intervalMs >= 50 && nowMs - s_lastStallLogMs >= 500)
        {
            s_lastStallLogMs = nowMs;
            Game::logMsg("Present stall interval=%ums tick=%u poseAge=%ums handoffSlow=%u poseOvershoot=%u",
                intervalMs, m_PresentTick,
                m_WaitedPoseTick.load(std::memory_order_acquire)
                    ? (nowMs - m_WaitedPoseTick.load(std::memory_order_acquire)) : 0xffffffffu,
                m_CompositorHandoffSlowCount,
                m_PoseWaitOvershootCount.load(std::memory_order_relaxed));
        }
    }
    if (s_lastPresentMs != 0 && m_FirstAttackPresentTick > 0 && m_FirstAttackSpikeLogs < 6)
    {
        const uint32_t sinceAttack = m_PresentTick - m_FirstAttackPresentTick;
        if (sinceAttack <= 90)
        {
            const DWORD intervalMs = nowMs - s_lastPresentMs;
            if (intervalMs >= 16)
            {
                ++m_FirstAttackSpikeLogs;
                Game::logMsg("First-shot present spike interval=%ums tick=%u sinceAttack=%u poseAge=%ums weapon=%s",
                    intervalMs, m_PresentTick, sinceAttack,
                    m_WaitedPoseTick.load(std::memory_order_acquire)
                        ? (nowMs - m_WaitedPoseTick.load(std::memory_order_acquire)) : 0xffffffffu,
                    m_LastViewmodelModel.empty() ? "?" : m_LastViewmodelModel.c_str());
            }
        }
    }
    s_lastPresentMs = nowMs;
    if (m_PresentTick == 1 || nowMs - s_fpsLogMs >= 1000)
    {
        const uint32_t dt = s_fpsLogMs ? (nowMs - s_fpsLogMs) : 0;
        const uint32_t dn = m_PresentTick - s_fpsLogTick;
        const unsigned fps = (dt > 0) ? (dn * 1000u / dt) : 0;
        Game::logMsg("present tick n=%u ~%ufps inGame=%d eligible=%d map=%s createdRT=%d menuVR=%d submit=%d namedRT=%d stereo=%d direct=%d blit=%ux%u poseWait=%u poseAge=%ums",
            m_PresentTick, fps,
            inGame ? 1 : 0, m_GameplayEligible ? 1 : 0,
            m_CurrentMapName.c_str(),
            m_CreatedVRTextures.load(std::memory_order_acquire) ? 1 : 0,
            bmvr::TryMenuCompositor() ? 1 : 0,
            ShouldCompositorSubmit() ? 1 : 0,
            m_UsedNamedRenderTargets ? 1 : 0,
            m_StereoRenderViewActive ? 1 : 0,
            m_DirectEyeSubmit ? 1 : 0,
            m_LastStereoBlitWidth, m_LastStereoBlitHeight,
            m_PoseWaitCount.load(std::memory_order_relaxed),
            m_WaitedPoseTick.load(std::memory_order_acquire)
                ? (nowMs - m_WaitedPoseTick.load(std::memory_order_acquire)) : 0xffffffffu);
        s_fpsLogMs = nowMs;
        s_fpsLogTick = m_PresentTick;
    }
    if (m_GameplayEligible && inGame && m_EligiblePresents < 100000)
        ++m_EligiblePresents;
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryHmdFramebuffer())
        bmvr::EndRisky(L"hmd_fb");
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryFramebufferOverride())
        bmvr::EndRisky(L"fb_override");
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryDrawHud())
        bmvr::EndRisky(L"drawhud");
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryHmdNative())
        bmvr::EndRisky(L"hmd_native");

    const DWORD poseTickEarly = m_WaitedPoseTick.load(std::memory_order_acquire);
    const DWORD poseAgeEarly = poseTickEarly ? (nowMs - poseTickEarly) : 0xffffffffu;
    if (m_Compositor && poseAgeEarly > 80 && poseAgeEarly <= 2000)
    {
        static DWORD s_lastBring = 0;
        if (nowMs - s_lastBring >= 1000)
        {
            m_Compositor->CompositorBringToFront();
            s_lastBring = nowMs;
        }
    }

    if (!m_PosesWaitedThisFrame)
    {
        if (m_OpenXrHelperBridgeActive)
            UpdateTracking();
        else
            WaitPosesForStereoFrame(false);
    }
    SyncGameUiFromEngine();
    PollVrMenuEvents();
    LatchMenuPanelIfNeeded();
    ProcessInput();
    m_PosesWaitedThisFrame = false;

    // Capture+Submit on background01 when menu_vr is still enabled. Look/stereo
    // stay gated on real maps. Named RTs / WaitDeviceIdle stay skipped.
    if (!ShouldCompositorSubmit())
        return;

    static int s_menuRisky;
    if (!m_GameplayEligible && s_menuRisky == 0)
    {
        Game::logMsg("Menu compositor begin map=%s presents=%u panelScale=%.2f cursorSmooth=%.2fs",
            m_CurrentMapName.empty() ? "(none)" : m_CurrentMapName.c_str(),
            m_PresentTick, bmvr::g_MenuPanelScale, bmvr::g_MenuCursorSmoothSec);
        s_menuRisky = -1;
    }

    if (g_D3DVR9 && !m_CreatedVRTextures.load(std::memory_order_acquire))
    {
        if (!m_GameplayEligible && s_menuRisky <= 0)
        {
            bmvr::BeginRisky(L"menu_vr");
            s_menuRisky = 1;
        }
        CreateVRTextures();
        if (m_CreatedVRTextures.load(std::memory_order_acquire) && s_menuRisky == 1)
        {
            bmvr::EndRisky(L"menu_vr");
            s_menuRisky = 2;
        }
    }
    else if (WantPauseWorldOverlay())
        EnsureHudOverlay();

    PrepareNamedStereoFromPresent();

    const DWORD poseTick = m_WaitedPoseTick.load(std::memory_order_acquire);
    const DWORD poseAge = poseTick ? (GetTickCount() - poseTick) : 0xffffffffu;
    if (poseTick == 0 || (!m_OpenXrHelperBridgeActive && poseAge > 500))
    {
        static int s_staleLog;
        if (s_staleLog < 6)
        {
            Game::logMsg("Skipping Submit, WaitGetPoses %s age=%ums",
                poseTick == 0 ? "not ready" : "stalled", poseAge);
            ++s_staleLog;
        }
        return;
    }
    SubmitVRTextures();
    static int s_updLog;
    if (s_updLog < 3)
    {
        Game::logMsg("Update done poses=%d created=%d", m_HmdPoseValid ? 1 : 0,
            m_CreatedVRTextures.load() ? 1 : 0);
        ++s_updLog;
    }
}

extern "C" void __cdecl L4D2VR_ShutdownSystemMouseInputSuppression()
{
}
