#include "vr.h"
#include "vr_ladder_rungs.h"
#include "vr_sound_cuts.h"
#include "vr_mag_reload.h"
#include "bmvr_prof.h"
#include "bmvr_sampler.h"
#include "game.h"
#include "sdk.h"
#include "offsets.h"
#include "MinHook.h"
#include "d3d9_vr.h"
#include "bmvr_glowtrace.h"
#include "bmvr_flags.h"
#include "bmvr_cvars.h"
#include "hooks.h"
#include "in_buttons.h"
#include "trace.h"
#include "texture.h"
#include "vr_hands.h"
#include "vr_hud_icons.h"
#include "openxr_helper_bridge.h"

#include <d3dcompiler.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
namespace ladder_tune { void NoteHeld(int model); }
#include <set>
#include <new>
#include <string>
#include <vector>
#include "bmvr_seh.h"

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
    constexpr UINT kIDirect3DDevice9_DrawPrimitive = 81;
    constexpr UINT kIDirect3DDevice9_DrawIndexedPrimitive = 82;
    constexpr UINT kIDirect3DDevice9_DrawPrimitiveUP = 83;
    constexpr UINT kIDirect3DDevice9_DrawIndexedPrimitiveUP = 84;
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

    // Yaw (radians, about OpenXR +Y) of a quaternion; 0 = facing -Z.
    float OpenXrQuatYaw(const float* q)
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        return atan2f(2.f * (w * y + x * z), 1.f - 2.f * (y * y + z * z));
    }

    float WrapRad(float a)
    {
        const float twoPi = 6.28318531f;
        return a - twoPi * floorf((a + 3.14159265f) / twoPi);
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

    // GPU pass timing (GpuPassTiming, also on while ReflectionRtScale > 0).
    // D3DQUERYTYPE_TIMESTAMP spans between RT0 changes, classified by the
    // bound RT0 (eye-sized, or the grown reflection set) and the eye being
    // drawn, plus refract copies (StretchRect into a reflection-sized RT).
    // Read back a few Presents later without D3DGETDATA_FLUSH, so nothing
    // stalls. D3D thread only (the render thread under MulticoreMode).
    // GpuPassDetail adds kSpanOther (every other RT0) and a label per span:
    // "<RT0 description> | <material group or copy/MSAA step>". Detail spans
    // share one timestamp per boundary (the end of one is the next's begin).
    enum GpuSpanKind { kSpanNone = -1, kSpanEye = 0, kSpanRefl = 1, kSpanCopy = 2, kSpanOther = 3, kSpanKinds = 4 };
    struct GpuSpan
    {
        IDirect3DQuery9* begin;
        IDirect3DQuery9* end;
        int kind;
        int eye;
        int label;          // GpuPassDetail label id, -1 = none
        bool beginShared;   // begin is the previous span's end (do not recycle it)
    };
    constexpr int kGpuFrames = 6;
    constexpr size_t kGpuMaxSpans = 96;
    constexpr size_t kGpuMaxDetailSpans = 8192;
    struct GpuDetailSample
    {
        int label;
        int eye;
        double ms;
    };
    struct GpuPassTimer
    {
        // GpuPassDetail state.
        int openLabel = -1;
        bool openBeginShared = false;
        IDirect3DDevice9* device = nullptr;
        std::string curRt = "none";
        std::string curMat = "-";
        std::string curOverride;
        std::vector<std::string> labelNames;
        std::unordered_map<std::string, int> labelIds;
        std::vector<std::array<double, 3>> labelMs;
        std::vector<GpuDetailSample> detailScratch;
        double detailSpans = 0.0;
        int detailFrames = 0;
        int detailDroppedFull = 0;
        DWORD lastDetailLogMs = 0;
        int detailReports = 0;
        // CPU side of the "after Present" span: Present call -> return, and
        // Present return -> first RT0 change of the next frame (QPC ms).
        double presentCallMs = 0.0;
        double presentReturnMs = 0.0;
        bool waitFirstRt = false;
        double cpuPresentSum = 0.0;
        double cpuGapSum = 0.0;
        int cpuGapN = 0;
        int cpuPresentN = 0;
        bool dumpNextFrame = false;   // log one resolved frame's spans in order
        std::vector<IDirect3DQuery9*> pool;
        std::vector<GpuSpan> frames[kGpuFrames];
        IDirect3DQuery9* frameEnd[kGpuFrames] = {};
        int write = 0;
        IDirect3DQuery9* openBegin = nullptr;
        int openKind = kSpanNone;
        int openEye = 0;
        IDirect3DQuery9* freqQuery = nullptr;
        UINT64 freq = 0;
        UINT64 lastFrameEndTs = 0;
        double sumMs[kSpanKinds][3] = {};
        double frameMs = 0.0;
        int frameIntervals = 0;
        int resolved = 0;
        int dropped = 0;
        DWORD lastLogMs = 0;
        DWORD threadId = 0;
        bool failed = false;
    };
    GpuPassTimer g_GpuTimer;

    bool GpuTimerEnabled()
    {
        return !g_GpuTimer.failed && (bmvr::g_GpuPassTiming || bmvr::g_PerfLog || bmvr::g_GpuPassDetail);
    }

    bool GpuTimerDetail()
    {
        return bmvr::g_GpuPassDetail && !g_GpuTimer.failed;
    }

    int GpuTimerLabelId(const std::string& name)
    {
        GpuPassTimer& t = g_GpuTimer;
        auto it = t.labelIds.find(name);
        if (it != t.labelIds.end())
            return it->second;
        const int id = static_cast<int>(t.labelNames.size());
        t.labelNames.push_back(name);
        t.labelMs.push_back({ 0.0, 0.0, 0.0 });
        t.labelIds.emplace(name, id);
        return id;
    }

    int GpuTimerCurrentLabel()
    {
        GpuPassTimer& t = g_GpuTimer;
        return GpuTimerLabelId(t.curRt + " | " + (t.curOverride.empty() ? t.curMat : t.curOverride));
    }

    bool GpuTimerThreadOk()
    {
        const DWORD tid = GetCurrentThreadId();
        if (g_GpuTimer.threadId == 0)
            g_GpuTimer.threadId = tid;
        return g_GpuTimer.threadId == tid;
    }

    IDirect3DQuery9* GpuTimerQuery(IDirect3DDevice9* device)
    {
        if (!g_GpuTimer.pool.empty())
        {
            IDirect3DQuery9* q = g_GpuTimer.pool.back();
            g_GpuTimer.pool.pop_back();
            return q;
        }
        IDirect3DQuery9* q = nullptr;
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q)) || !q)
        {
            g_GpuTimer.failed = true;
            Game::logMsg("GPU pass timing off: D3DQUERYTYPE_TIMESTAMP not available");
            return nullptr;
        }
        return q;
    }

    void GpuTimerRecycle(IDirect3DQuery9*& q)
    {
        if (q)
            g_GpuTimer.pool.push_back(q);
        q = nullptr;
    }

    // The open span's begin, unless it is a shared boundary (owned by the
    // previous span's end in the frame list).
    void GpuTimerDropOpenBegin()
    {
        GpuPassTimer& t = g_GpuTimer;
        if (t.openBeginShared)
            t.openBegin = nullptr;
        else
            GpuTimerRecycle(t.openBegin);
        t.openBeginShared = false;
    }

    void GpuTimerRecycleSpan(GpuSpan& sp)
    {
        if (sp.beginShared)
            sp.begin = nullptr;
        else
            GpuTimerRecycle(sp.begin);
        GpuTimerRecycle(sp.end);
    }

    IDirect3DQuery9* GpuTimerStamp(IDirect3DDevice9* device)
    {
        IDirect3DQuery9* q = GpuTimerQuery(device);
        if (q)
            q->Issue(D3DISSUE_END);
        return q;
    }

    // Close the open span and open one of `kind` (kSpanNone = just close).
    // `label` is the GpuPassDetail label (-1 outside detail mode).
    void GpuTimerSwitch(IDirect3DDevice9* device, int kind, int eye, int label = -1)
    {
        GpuPassTimer& t = g_GpuTimer;
        if (kind == kSpanNone)
            label = -1;
        if (kind == t.openKind && eye == t.openEye && label == t.openLabel && (kind == kSpanNone || t.openBegin))
            return;
        IDirect3DQuery9* reuse = nullptr;
        if (t.openBegin)
        {
            IDirect3DQuery9* end = GpuTimerStamp(device);
            std::vector<GpuSpan>& spans = t.frames[t.write];
            const size_t cap = bmvr::g_GpuPassDetail ? kGpuMaxDetailSpans : kGpuMaxSpans;
            if (end && spans.size() < cap)
            {
                spans.push_back(GpuSpan{ t.openBegin, end, t.openKind, t.openEye, t.openLabel, t.openBeginShared });
                if (bmvr::g_GpuPassDetail)
                    reuse = end;
                t.openBegin = nullptr;
                t.openBeginShared = false;
            }
            else
            {
                GpuTimerDropOpenBegin();
                GpuTimerRecycle(end);
                ++t.dropped;
                if (bmvr::g_GpuPassDetail)
                    ++t.detailDroppedFull;
            }
        }
        t.openKind = kind;
        t.openEye = eye;
        t.openLabel = label;
        if (kind != kSpanNone)
        {
            if (reuse)
            {
                t.openBegin = reuse;
                t.openBeginShared = true;
            }
            else
            {
                t.openBegin = GpuTimerStamp(device);
                t.openBeginShared = false;
            }
        }
    }

    std::atomic<bool> g_EyeRtReset{ false };   // SkyWaterPerEye: drop the per-eye RT copies (map change)

    int GpuTimerEye(VR* vr)
    {
        int eye = g_McPlayEye != 0 ? g_McPlayEye : (vr ? vr->m_StereoEye : 0);
        return (eye == 1 || eye == 2) ? eye : 0;
    }

    // Render-target audit (test aid, VR/audit.txt [frames]): for every draw,
    // copy and clear, which RT it writes (size, viewport) and which RT
    // textures it samples, with the eye and frame that last wrote each one.
    // Flags: OTHER-EYE (sampled RT last written by the other eye), PREV-FRAME,
    // UNSEEN (not written since the audit started: stale or static), VP-OUT
    // (viewport/scissor past the RT). The draw hooks are only enabled while an
    // audit runs. One line per change of (RT, viewport, sampled RTs, eye, HUD).
    using tDrawPrimitive = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using tDrawIndexedPrimitive = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    using tDrawPrimitiveUP = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using tDrawIndexedPrimitiveUP = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
        const void*, D3DFORMAT, const void*, UINT);
    tDrawPrimitive g_OrigDrawPrimitive = nullptr;
    tDrawIndexedPrimitive g_OrigDrawIndexedPrimitive = nullptr;
    tDrawPrimitiveUP g_OrigDrawPrimitiveUP = nullptr;
    tDrawIndexedPrimitiveUP g_OrigDrawIndexedPrimitiveUP = nullptr;
    void** g_AuditVtbl = nullptr;

    struct AuditTex
    {
        UINT w = 0, h = 0;
        int writerEye = -1;      // -1 = not written since the audit started
        bool writerHud = false;
        uint32_t writeFrame = 0;
        uint32_t writeSeq = 0;
    };
    std::recursive_mutex g_AuditMutex;
    std::map<const void*, AuditTex> g_AuditTex;
    std::atomic<int> g_AuditFramesLeft{ 0 };  // 1 while an audit runs (the first Present only warms up the writer map)
    std::atomic<bool> g_AuditHooksOn{ false };
    uint32_t g_AuditFrame = 0;
    uint32_t g_AuditSeq = 0;
    int g_AuditLines = 0;
    std::string g_AuditGroup;
    std::string g_AuditGroupLine;
    int g_AuditGroupDraws = 0;
    uint64_t g_AuditGroupPrims = 0;
    constexpr int kAuditMaxLines = 6000;
    // Material the render thread bound last (IMatRenderContext::Bind hook).
    thread_local char t_AuditMaterial[96];
    // Shadow copy of the shader constants (as the GPU gets them) while an
    // audit runs; draws of a material containing $BMVR_AUDIT_CONSTS (env)
    // log their non-zero VS c0-c47 / PS c0-c31.
    bool DumpSurfaceBmp(IDirect3DDevice9* dev, IDirect3DSurface9* src, const std::string& path, int step,
        bool alphaOnly);
    bool DumpSurfaceRaw(IDirect3DDevice9* dev, IDirect3DSurface9* src, const std::string& path);
    float g_AuditVsConst[256 * 4];
    float g_AuditPsConst[224 * 4];
    char g_AuditConstMat[64] = { 1 };  // { 1 } = env not read yet
    char g_AuditTexPrefix[64];          // BMVR_AUDIT_TEXPREFIX, read at each audit start
    std::set<std::pair<int, const void*>> g_AuditTexDumped;
    // The audit ends after N complete stereo pairs, not after N Presents: in
    // MulticoreMode the Present arrives on another thread, and a stall right
    // after the start (the panel capture's full-size eye dump) let a whole
    // frame draw during the warm-up, then two Presents came back to back and
    // the logged "frame" had no draws. A pair counts once a left-eye pass has
    // started after a right-eye draw (so it is logged from its start) and
    // right-eye draws followed it; the audit stops at the next Present.
    int g_AuditPairsWanted = 0;
    int g_AuditPairsDone = 0;
    int g_AuditPairPhase = 0;   // 0 = need a right-eye draw, 1 = need a left start, 2 = in left, need right
    int g_AuditLastEye = 0;     // last non-HUD eye 1/2 seen while logging
    int g_AuditPresentCap = 0;  // Presents after which the audit gives up

    bool AuditLogging()
    {
        return g_AuditFramesLeft.load(std::memory_order_acquire) > 0 && g_AuditFrame > 0;
    }

    void AuditTrackEye(int eye, bool hud)
    {
        if (hud || (eye != 1 && eye != 2) || eye == g_AuditLastEye)
            return;
        g_AuditLastEye = eye;
        if (eye == 2)
        {
            if (g_AuditPairPhase == 2)
                ++g_AuditPairsDone;
            g_AuditPairPhase = 1;
        }
        else if (g_AuditPairPhase == 1)
            g_AuditPairPhase = 2;
    }

    const char* AuditSizeLabel(VR* vr, UINT w, UINT h)
    {
        if (!vr)
            return "";
        const UINT ew = vr->m_RenderWidth, eh = vr->m_RenderHeight;
        if (w == ew && h == eh) return "eye";
        if (w == ew / 2 && h == eh / 2) return "eye/2";
        if (w == ew / 4 && h == eh / 4) return "eye/4";
        if (bmvr::g_ReflectionRtW > 0 && static_cast<int>(w) == bmvr::g_ReflectionRtW
            && static_cast<int>(h) == bmvr::g_ReflectionRtH) return "refl";
        if (bmvr::g_RefractCopyRtW > 0 && static_cast<int>(w) == bmvr::g_RefractCopyRtW
            && static_cast<int>(h) == bmvr::g_RefractCopyRtH) return "refrcopy";
        UINT winW = 0, winH = 0;
        if (QueryGameClientSize(winW, winH) && w == winW && h == winH) return "win";
        if (w == 1024 && h == 1024) return "1024sq";
        return "";
    }

    // Texture key for an RT surface: its container texture when it has one.
    const void* AuditSurfaceKey(IDirect3DSurface9* s, UINT& w, UINT& h)
    {
        w = h = 0;
        if (!s)
            return nullptr;
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(s->GetDesc(&d)))
        {
            w = d.Width;
            h = d.Height;
        }
        IDirect3DTexture9* t = nullptr;
        if (SUCCEEDED(s->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&t))) && t)
        {
            const void* key = t;
            t->Release();
            return key;
        }
        return s;
    }

    void AuditMarkWritten(VR* vr, const void* key, UINT w, UINT h, int eye)
    {
        if (!key)
            return;
        AuditTex& t = g_AuditTex[key];
        t.w = w;
        t.h = h;
        t.writerEye = eye;
        t.writerHud = vr && vr->HudPaintActive();
        t.writeFrame = g_AuditFrame;
        t.writeSeq = ++g_AuditSeq;
    }

    void AuditFlushGroup()
    {
        if (g_AuditGroupLine.empty())
            return;
        if (g_AuditLines < kAuditMaxLines)
        {
            Game::logMsg("%s | draws=%d prims=%llu", g_AuditGroupLine.c_str(), g_AuditGroupDraws,
                static_cast<unsigned long long>(g_AuditGroupPrims));
            ++g_AuditLines;
        }
        g_AuditGroupLine.clear();
        g_AuditGroup.clear();
        g_AuditGroupDraws = 0;
        g_AuditGroupPrims = 0;
    }

    // Appends " name WxH[label] w=eE fN flags" for a sampled or source texture.
    void AuditDescribeRead(VR* vr, std::string& out, const char* tag, const void* key, UINT w, UINT h, int eye)
    {
        char buf[200];
        auto it = g_AuditTex.find(key);
        const AuditTex* t = it != g_AuditTex.end() ? &it->second : nullptr;
        std::string flags;
        if (!t || t->writerEye < 0)
            flags += " UNSEEN";
        else
        {
            if ((eye == 1 || eye == 2) && (t->writerEye == 1 || t->writerEye == 2) && t->writerEye != eye)
                flags += " OTHER-EYE";
            if (t->writeFrame < g_AuditFrame)
                flags += " PREV-FRAME";
        }
        const char* label = AuditSizeLabel(vr, w, h);
        std::snprintf(buf, sizeof(buf), " %s %p %ux%u%s%s%s w=%s%d%s seq%u%s", tag, key, w, h,
            label[0] ? "[" : "", label, label[0] ? "]" : "",
            t ? "e" : "", t ? t->writerEye : -1, t && t->writerHud ? "/hud" : "",
            t ? t->writeSeq : 0, flags.c_str());
        out += buf;
    }

    // Called from every draw while an audit runs.
    void AuditDraw(IDirect3DDevice9* device, UINT prims)
    {
        if (g_AuditFramesLeft.load(std::memory_order_acquire) <= 0 || !device)
            return;
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        std::lock_guard<std::recursive_mutex> lock(g_AuditMutex);
        const int eye = GpuTimerEye(vr);
        const bool hud = vr && vr->HudPaintActive();
        IDirect3DSurface9* rt = nullptr;
        device->GetRenderTarget(0, &rt);
        UINT rw = 0, rh = 0;
        const void* rtKey = AuditSurfaceKey(rt, rw, rh);
        if (rt)
            rt->Release();
        if (AuditLogging())
        {
            AuditTrackEye(eye, hud);
            D3DVIEWPORT9 vp{};
            device->GetViewport(&vp);
            DWORD scissorOn = 0;
            device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorOn);
            RECT sc{};
            if (scissorOn)
                device->GetScissorRect(&sc);
            char head[320];
            const char* label = AuditSizeLabel(vr, rw, rh);
            const bool vpOut = rw && (vp.X + vp.Width > rw || vp.Y + vp.Height > rh);
            const bool scOut = scissorOn && rw && (sc.right > static_cast<LONG>(rw) || sc.bottom > static_cast<LONG>(rh));
            std::snprintf(head, sizeof(head), "audit f%u e%d%s: RT0 %p %ux%u%s%s%s vp %lu,%lu %lux%lu%s%s",
                g_AuditFrame, eye, hud ? " HUD" : "", rtKey, rw, rh,
                label[0] ? "[" : "", label, label[0] ? "]" : "",
                vp.X, vp.Y, vp.Width, vp.Height, vpOut ? " VP-OUT" : "", scOut ? " SC-OUT" : "");
            std::string line = head;
            if (t_AuditMaterial[0])
            {
                line += " mat=";
                line += t_AuditMaterial;
            }
            if (scissorOn)
            {
                char sb[80];
                std::snprintf(sb, sizeof(sb), " sc %ld,%ld-%ld,%ld", sc.left, sc.top, sc.right, sc.bottom);
                line += sb;
            }
            {
                DWORD zOn = 0, zWrite = 0, stencilOn = 0;
                device->GetRenderState(D3DRS_ZENABLE, &zOn);
                device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
                device->GetRenderState(D3DRS_STENCILENABLE, &stencilOn);
                IDirect3DSurface9* depthSurface = nullptr;
                D3DSURFACE_DESC depthDesc{};
                char msState[64] = {};
                if (SUCCEEDED(device->GetDepthStencilSurface(&depthSurface)) && depthSurface)
                {
                    depthSurface->GetDesc(&depthDesc);
                    bmvr::MsaaDepthStateText(depthSurface, msState, sizeof(msState));
                    depthSurface->Release();
                }
                char zb[128];
                std::snprintf(zb, sizeof(zb), " z=%lu zw=%lu st=%lu ds=%ux%u%s", zOn, zWrite, stencilOn, depthDesc.Width, depthDesc.Height, msState);
                line += zb;
            }
            std::string reads;
            for (DWORD stage = 0; stage < 16; ++stage)
            {
                IDirect3DBaseTexture9* bt = nullptr;
                if (FAILED(device->GetTexture(stage, &bt)) || !bt)
                    continue;
                if (bt->GetType() == D3DRTYPE_TEXTURE)
                {
                    auto* tex = static_cast<IDirect3DTexture9*>(bt);
                    D3DSURFACE_DESC d{};
                    if (SUCCEEDED(tex->GetLevelDesc(0, &d)) && (d.Usage & D3DUSAGE_RENDERTARGET))
                    {
                        char tag[8];
                        std::snprintf(tag, sizeof(tag), "s%lu", stage);
                        AuditDescribeRead(vr, reads, tag, bt, d.Width, d.Height, eye);
                    }
                }
                bt->Release();
            }
            if (!reads.empty())
                line += " |" + reads;
            // Group by everything but the writer sequence numbers.
            std::string key = line;
            for (size_t p = key.find(" seq"); p != std::string::npos; p = key.find(" seq", p + 1))
            {
                size_t e = key.find(' ', p + 1);
                key.erase(p, e == std::string::npos ? std::string::npos : e - p);
            }
            if (key != g_AuditGroup)
            {
                AuditFlushGroup();
                g_AuditGroup = key;
                g_AuditGroupLine = line;
            }
            ++g_AuditGroupDraws;
            g_AuditGroupPrims += prims;
        }
        if (AuditLogging() && g_AuditConstMat[0] == 1)
        {
            g_AuditConstMat[0] = 0;
            GetEnvironmentVariableA("BMVR_AUDIT_CONSTS", g_AuditConstMat, sizeof(g_AuditConstMat));
        }
        // '|' separates several material name parts.
        auto constMatch = [&]() {
            if (!g_AuditConstMat[0] || !t_AuditMaterial[0])
                return false;
            const char* p = g_AuditConstMat;
            while (*p)
            {
                const char* e = std::strchr(p, '|');
                const size_t n = e ? static_cast<size_t>(e - p) : std::strlen(p);
                if (n && n < 64)
                {
                    char tok[64];
                    std::memcpy(tok, p, n);
                    tok[n] = 0;
                    if (std::strstr(t_AuditMaterial, tok))
                        return true;
                }
                if (!e)
                    break;
                p = e + 1;
            }
            return false;
        };
        if (AuditLogging() && constMatch() && g_AuditLines < kAuditMaxLines)
        {
            // Non-zero constants, 8 registers per line (log lines are capped
            // at 1 KB; a single line cut off the higher registers).
            auto dumpConsts = [&](const char* tag, const float* c, int n) {
                for (int base = 0; base < n; base += 8)
                {
                    std::string out;
                    char b[112];
                    for (int i = base; i < base + 8 && i < n; ++i)
                    {
                        const float* v = c + i * 4;
                        if (v[0] == 0.f && v[1] == 0.f && v[2] == 0.f && v[3] == 0.f)
                            continue;
                        std::snprintf(b, sizeof(b), " c%d=(%.6g,%.6g,%.6g,%.6g)", i, v[0], v[1], v[2], v[3]);
                        out += b;
                    }
                    if (!out.empty())
                        Game::logMsg("audit consts e%d %s %s:%s", eye, t_AuditMaterial, tag, out.c_str());
                }
                ++g_AuditLines;
            };
            AuditFlushGroup();
            dumpConsts("VS", g_AuditVsConst, 48);
            dumpConsts("PS", g_AuditPsConst, 64);
            // The bound shaders' bytecode, once per shader, as
            // VR/audit-<vs|ps>-<ptr>.dxso (match against DXVK_SHADER_DUMP_PATH).
            {
                static std::set<const void*> s_savedShaders;
                IDirect3DVertexShader9* vsh = nullptr;
                IDirect3DPixelShader9* psh = nullptr;
                device->GetVertexShader(&vsh);
                device->GetPixelShader(&psh);
                Game::logMsg("audit shaders e%d %s vs=%p ps=%p", eye, t_AuditMaterial, (void*)vsh, (void*)psh);
                auto save = [&](const char* kind, const void* key, auto* sh) {
                    UINT size = 0;
                    if (!sh || s_savedShaders.count(key) || FAILED(sh->GetFunction(nullptr, &size)) || !size)
                        return;
                    std::vector<char> buf(size);
                    if (FAILED(sh->GetFunction(buf.data(), &size)))
                        return;
                    char name[64];
                    std::snprintf(name, sizeof(name), "audit-%s-%p.dxso", kind, key);
                    if (FILE* f = std::fopen(bmvr::VrFilePath(name).c_str(), "wb"))
                    {
                        std::fwrite(buf.data(), 1, size, f);
                        std::fclose(f);
                    }
                    s_savedShaders.insert(key);
                };
                save("vs", vsh, vsh);
                save("ps", psh, psh);
                if (vsh) vsh->Release();
                if (psh) psh->Release();
            }
            // Panel capture, first matching draw into each eye's own target:
            // its full render state and the eye colour it is drawn over
            // (handover 2026-11-13: these were the inputs no capture had).
            if (vr && (eye == 1 || eye == 2) && g_AuditTexPrefix[0] && rw == vr->m_RenderWidth
                && rh == vr->m_RenderHeight && g_AuditTexDumped.insert({ eye + 100, nullptr }).second)
            {
                auto rs = [&](D3DRENDERSTATETYPE st) { DWORD v = 0; device->GetRenderState(st, &v); return v; };
                Game::logMsg("audit drawstate e%d %s: blend=%lu src=%lu dst=%lu op=%lu sepA=%lu srcA=%lu dstA=%lu opA=%lu "
                    "atest=%lu aref=%lu afunc=%lu cwrite=0x%lx cwrite1=0x%lx cull=%lu z=%lu zw=%lu zfunc=%lu "
                    "srgbw=%lu msaa=%lu msmask=0x%lx fill=%lu dbias=0x%lx sdbias=0x%lx st=%lu stfunc=%lu stref=%lu "
                    "clip=0x%lx scissor=%lu",
                    eye, t_AuditMaterial, rs(D3DRS_ALPHABLENDENABLE), rs(D3DRS_SRCBLEND), rs(D3DRS_DESTBLEND),
                    rs(D3DRS_BLENDOP), rs(D3DRS_SEPARATEALPHABLENDENABLE), rs(D3DRS_SRCBLENDALPHA),
                    rs(D3DRS_DESTBLENDALPHA), rs(D3DRS_BLENDOPALPHA), rs(D3DRS_ALPHATESTENABLE), rs(D3DRS_ALPHAREF),
                    rs(D3DRS_ALPHAFUNC), rs(D3DRS_COLORWRITEENABLE), rs(D3DRS_COLORWRITEENABLE1), rs(D3DRS_CULLMODE),
                    rs(D3DRS_ZENABLE), rs(D3DRS_ZWRITEENABLE), rs(D3DRS_ZFUNC), rs(D3DRS_SRGBWRITEENABLE),
                    rs(D3DRS_MULTISAMPLEANTIALIAS), rs(D3DRS_MULTISAMPLEMASK), rs(D3DRS_FILLMODE), rs(D3DRS_DEPTHBIAS),
                    rs(D3DRS_SLOPESCALEDEPTHBIAS), rs(D3DRS_STENCILENABLE), rs(D3DRS_STENCILFUNC), rs(D3DRS_STENCILREF),
                    rs(D3DRS_CLIPPLANEENABLE), rs(D3DRS_SCISSORTESTENABLE));
                IDirect3DSurface9* ds = nullptr;
                D3DSURFACE_DESC dd{};
                if (SUCCEEDED(device->GetDepthStencilSurface(&ds)) && ds)
                {
                    ds->GetDesc(&dd);
                    ds->Release();
                }
                Game::logMsg("audit drawstate e%d %s: depth %p %ux%u fmt=%u ms=%u (not dumpable on this path; "
                    "colour before the draw below)", eye, t_AuditMaterial, (void*)ds, dd.Width, dd.Height, dd.Format,
                    dd.MultiSampleType);
                IDirect3DSurface9* rt0 = nullptr;
                if (SUCCEEDED(device->GetRenderTarget(0, &rt0)) && rt0)
                {
                    char name[160];
                    std::snprintf(name, sizeof(name), "%s-e%d-d%u-eye-before-water.bmp", g_AuditTexPrefix, eye, g_AuditSeq);
                    DumpSurfaceBmp(device, rt0, bmvr::VrFilePath(name), 2, false);
                    rt0->Release();
                }
            }
            // With BMVR_AUDIT_TEXPREFIX (panel capture): every render-target
            // texture each matching draw samples, any size, once per texture
            // and eye, as VR/<prefix>-e<eye>-<draw>-s<stage>-<tex>.bmp.
            if (vr && (eye == 1 || eye == 2) && g_AuditTexPrefix[0])
            {
                for (DWORD stage = 0; stage < 16; ++stage)
                {
                    IDirect3DBaseTexture9* bt = nullptr;
                    if (FAILED(device->GetTexture(stage, &bt)) || !bt)
                        continue;
                    if (bt->GetType() == D3DRTYPE_TEXTURE && g_AuditTexDumped.insert({ eye, bt }).second
                        && g_AuditTexDumped.size() < 64)
                    {
                        auto* tex = static_cast<IDirect3DTexture9*>(bt);
                        D3DSURFACE_DESC d{};
                        IDirect3DSurface9* surf = nullptr;
                        if (SUCCEEDED(tex->GetLevelDesc(0, &d)) && (d.Usage & D3DUSAGE_RENDERTARGET)
                            && SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf)
                        {
                            char name[160];
                            std::snprintf(name, sizeof(name), "%s-e%d-d%u-s%lu-%p.bmp", g_AuditTexPrefix, eye,
                                g_AuditSeq, stage, (void*)bt);
                            Game::logMsg("audit texdump e%d %s stage %lu %p %ux%u fmt=%u -> %s", eye,
                                t_AuditMaterial, stage, (void*)bt, d.Width, d.Height, d.Format, name);
                            const int texStep = d.Width > 2048 ? 2 : 1;
                            const bool floatFmt = d.Format == D3DFMT_R16F || d.Format == D3DFMT_G16R16F
                                || d.Format == D3DFMT_R32F || d.Format == D3DFMT_G32R32F
                                || d.Format == D3DFMT_A32B32G32R32F;
                            if (floatFmt)
                            {
                                // Raw rows as stored (depth etc. does not fit 8 bits); the
                                // log line above has width, height and format.
                                std::snprintf(name, sizeof(name), "%s-e%d-d%u-s%lu-%p-%ux%u-fmt%u.raw", g_AuditTexPrefix,
                                    eye, g_AuditSeq, stage, (void*)bt, d.Width, d.Height, d.Format);
                                DumpSurfaceRaw(device, surf, bmvr::VrFilePath(name));
                            }
                            else
                                DumpSurfaceBmp(device, surf, bmvr::VrFilePath(name), texStep, false);
                            if (d.Format == D3DFMT_A8R8G8B8)
                            {
                                std::snprintf(name, sizeof(name), "%s-e%d-d%u-s%lu-%p-alpha.bmp", g_AuditTexPrefix,
                                    eye, g_AuditSeq, stage, (void*)bt);
                                DumpSurfaceBmp(device, surf, bmvr::VrFilePath(name), texStep, true);
                            }
                            surf->Release();
                        }
                    }
                    bt->Release();
                }
            }
            // Once per eye: the eye-sized textures this draw samples (light
            // accumulation etc.) as VR/audit-<mat>-e<eye>-s<stage>.bmp.
            static int s_dumped[3];
            if (vr && (eye == 1 || eye == 2) && rw == vr->m_RenderWidth && !s_dumped[eye] && !g_AuditTexPrefix[0])
            {
                bool any = false;
                for (DWORD stage = 0; stage < 16; ++stage)
                {
                    IDirect3DBaseTexture9* bt = nullptr;
                    if (FAILED(device->GetTexture(stage, &bt)) || !bt)
                        continue;
                    if (bt->GetType() == D3DRTYPE_TEXTURE)
                    {
                        auto* tex = static_cast<IDirect3DTexture9*>(bt);
                        D3DSURFACE_DESC d{};
                        IDirect3DSurface9* surf = nullptr;
                        if (SUCCEEDED(tex->GetLevelDesc(0, &d)) && (d.Usage & D3DUSAGE_RENDERTARGET)
                            && d.Width == vr->m_RenderWidth && SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf)
                        {
                            char name[96];
                            std::snprintf(name, sizeof(name), "audit-e%d-s%lu.bmp", eye, stage);
                            DumpSurfaceBmp(device, surf, bmvr::VrFilePath(name), 2, false);
                            surf->Release();
                            any = true;
                        }
                    }
                    bt->Release();
                }
                if (any)
                    s_dumped[eye] = 1;
            }
        }
        AuditMarkWritten(vr, rtKey, rw, rh, eye);
    }

    void AuditStretchRect(IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst, const RECT* dstRect)
    {
        if (g_AuditFramesLeft.load(std::memory_order_acquire) <= 0)
            return;
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        std::lock_guard<std::recursive_mutex> lock(g_AuditMutex);
        const int eye = GpuTimerEye(vr);
        UINT sw = 0, sh = 0, dw = 0, dh = 0;
        const void* sk = AuditSurfaceKey(src, sw, sh);
        const void* dk = AuditSurfaceKey(dst, dw, dh);
        if (AuditLogging())
        {
            AuditFlushGroup();
            std::string line;
            char head[200];
            std::snprintf(head, sizeof(head), "audit f%u e%d%s: StretchRect", g_AuditFrame, eye,
                vr && vr->HudPaintActive() ? " HUD" : "");
            line = head;
            AuditDescribeRead(vr, line, "src", sk, sw, sh, eye);
            char rects[160];
            std::snprintf(rects, sizeof(rects), " rect %ld,%ld-%ld,%ld -> dst %p %ux%u[%s] rect %ld,%ld-%ld,%ld",
                srcRect ? srcRect->left : -1, srcRect ? srcRect->top : -1, srcRect ? srcRect->right : -1, srcRect ? srcRect->bottom : -1,
                dk, dw, dh, AuditSizeLabel(vr, dw, dh),
                dstRect ? dstRect->left : -1, dstRect ? dstRect->top : -1, dstRect ? dstRect->right : -1, dstRect ? dstRect->bottom : -1);
            line += rects;
            if (g_AuditLines < kAuditMaxLines)
            {
                Game::logMsg("%s", line.c_str());
                ++g_AuditLines;
            }
        }
        AuditMarkWritten(vr, dk, dw, dh, eye);
    }

    void AuditClear(IDirect3DDevice9* device, DWORD flags)
    {
        if (g_AuditFramesLeft.load(std::memory_order_acquire) <= 0 || !device)
            return;
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        std::lock_guard<std::recursive_mutex> lock(g_AuditMutex);
        IDirect3DSurface9* rt = nullptr;
        device->GetRenderTarget(0, &rt);
        UINT rw = 0, rh = 0;
        const void* key = AuditSurfaceKey(rt, rw, rh);
        if (rt)
            rt->Release();
        const int eye = GpuTimerEye(vr);
        if (AuditLogging() && g_AuditLines < kAuditMaxLines)
        {
            AuditFlushGroup();
            // Depth-only clears too (water passes): which depth, and its eye MSAA state.
            IDirect3DSurface9* ds = nullptr;
            D3DSURFACE_DESC dd{};
            char msState[64] = {};
            if (SUCCEEDED(device->GetDepthStencilSurface(&ds)) && ds)
            {
                ds->GetDesc(&dd);
                bmvr::MsaaDepthStateText(ds, msState, sizeof(msState));
                ds->Release();
            }
            Game::logMsg("audit f%u e%d%s: Clear%s%s%s RT0 %p %ux%u[%s] ds %p %ux%u%s", g_AuditFrame, eye,
                vr && vr->HudPaintActive() ? " HUD" : "",
                (flags & D3DCLEAR_TARGET) ? " C" : "", (flags & D3DCLEAR_ZBUFFER) ? " Z" : "", (flags & D3DCLEAR_STENCIL) ? " S" : "",
                key, rw, rh, AuditSizeLabel(vr, rw, rh), (void*)ds, dd.Width, dd.Height, msState);
            ++g_AuditLines;
        }
        if (flags & D3DCLEAR_TARGET)
            AuditMarkWritten(vr, key, rw, rh, eye);
    }

    void AuditSetDrawHooks(bool on)
    {
        if (!g_AuditVtbl || g_AuditHooksOn.load() == on)
            return;
        for (UINT i : { kIDirect3DDevice9_DrawPrimitive, kIDirect3DDevice9_DrawIndexedPrimitive,
                 kIDirect3DDevice9_DrawPrimitiveUP, kIDirect3DDevice9_DrawIndexedPrimitiveUP })
        {
            if (on)
                MH_EnableHook(g_AuditVtbl[i]);
            else
                MH_DisableHook(g_AuditVtbl[i]);
        }
        g_AuditHooksOn.store(on);
    }

    void AuditStart(int frames)
    {
        std::lock_guard<std::recursive_mutex> lock(g_AuditMutex);
        g_AuditTex.clear();
        g_AuditFrame = 0;
        g_AuditSeq = 0;
        g_AuditLines = 0;
        g_AuditGroup.clear();
        g_AuditGroupLine.clear();
        g_AuditPairsWanted = frames;
        g_AuditPairsDone = 0;
        g_AuditPairPhase = 0;
        g_AuditLastEye = 0;
        g_AuditPresentCap = 4 * frames + 60;
        g_AuditFramesLeft.store(1, std::memory_order_release);
        g_AuditConstMat[0] = 1;   // re-read the env (the panel capture sets it)
        g_AuditTexPrefix[0] = 0;
        GetEnvironmentVariableA("BMVR_AUDIT_TEXPREFIX", g_AuditTexPrefix, sizeof(g_AuditTexPrefix));
        g_AuditTexDumped.clear();
        AuditSetDrawHooks(true);
        Hooks::SetMatBindHooks(true);
        Game::logMsg("audit: %d stereo pair(s) after a warm-up Present, draw hooks %s", frames,
            g_AuditVtbl ? "on" : "missing");
    }

    // Draw + material hooks also run (outside audits) while the debug row
    // "Hide Xen fog" is on: the skip needs each draw's material name.
    void DebugSkipHooksUpdate()
    {
        static int s_on = -1;
        const bool want = bmvr::g_DebugHideXog >= 0.5f;
        if (static_cast<int>(want) == s_on)
            return;
        s_on = want ? 1 : 0;
        AuditSetDrawHooks(want);
        Hooks::SetMatBindHooks(want);
        Game::logMsg("Hide Xen fog: %d (draw hooks %s)", static_cast<int>(bmvr::g_DebugHideXog + 0.5f), want ? "on" : "off");
    }

    void AuditOnPresent()
    {
        if (g_AuditFramesLeft.load(std::memory_order_acquire) <= 0)
        {
            DebugSkipHooksUpdate();
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(g_AuditMutex);
        AuditFlushGroup();
        if (AuditLogging() && g_AuditLines < kAuditMaxLines)
        {
            Game::logMsg("audit f%u: Present", g_AuditFrame);
            ++g_AuditLines;
        }
        ++g_AuditFrame;
        const bool pairsDone = g_AuditPairsDone >= g_AuditPairsWanted;
        const bool gaveUp = !pairsDone && (g_AuditLines >= kAuditMaxLines
            || static_cast<int>(g_AuditFrame) > g_AuditPresentCap);
        if (pairsDone || gaveUp)
        {
            g_AuditFramesLeft.store(0, std::memory_order_release);
            VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
            Game::logMsg("audit: %s, %d/%d stereo pair(s) over %u Present(s), %d lines%s; RT textures written:",
                pairsDone ? "done" : "GAVE UP (no complete left+right pair)", g_AuditPairsDone,
                g_AuditPairsWanted, g_AuditFrame - 1, g_AuditLines,
                g_AuditLines >= kAuditMaxLines ? " (capped)" : "");
            for (const auto& [key, t] : g_AuditTex)
                Game::logMsg("audit tex %p %ux%u[%s] last e%d%s frame %u seq %u", key, t.w, t.h,
                    AuditSizeLabel(vr, t.w, t.h), t.writerEye, t.writerHud ? "/hud" : "", t.writeFrame, t.writeSeq);
            // BMVR_TEST_SKIP_MAT keeps the hooks on after the audit (debug).
            char skip[4] = {};
            if (!GetEnvironmentVariableA("BMVR_TEST_SKIP_MAT", skip, sizeof(skip)) && bmvr::g_DebugHideXog < 0.5f)
            {
                AuditSetDrawHooks(false);
                Hooks::SetMatBindHooks(false);
            }
        }
    }

    // Debug: BMVR_TEST_SKIP_MAT=<part>[|<part>...] drops the draws of matching
    // materials once an audit has turned the hooks on (they then stay on).
    bool AuditSkipDraw()
    {
        static char s_skip[64] = { 1 };
        if (s_skip[0] == 1)
        {
            s_skip[0] = 0;
            GetEnvironmentVariableA("BMVR_TEST_SKIP_MAT", s_skip, sizeof(s_skip));
        }
        if (!t_AuditMaterial[0])
            return false;
        const int hideXog = static_cast<int>(bmvr::g_DebugHideXog + 0.5f);
        if (hideXog && std::strncmp(t_AuditMaterial, "__gbxog", 7) == 0)
        {
            const bool global = t_AuditMaterial[7] == '0';
            if ((hideXog & (global ? 1 : 2)) != 0)
                return true;
        }
        if (!s_skip[0])
            return false;
        const char* p = s_skip;
        while (*p)
        {
            const char* e = std::strchr(p, '|');
            const size_t n = e ? static_cast<size_t>(e - p) : std::strlen(p);
            char tok[64];
            if (n && n < sizeof(tok))
            {
                std::memcpy(tok, p, n);
                tok[n] = 0;
                if (std::strstr(t_AuditMaterial, tok))
                    return true;
            }
            if (!e)
                break;
            p = e + 1;
        }
        return false;
    }

    HRESULT __stdcall HookedDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count)
    {
        AuditDraw(device, count);
        if (AuditSkipDraw())
            return S_OK;
        return g_OrigDrawPrimitive(device, type, start, count);
    }

    HRESULT __stdcall HookedDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base,
        UINT minIndex, UINT numVerts, UINT startIndex, UINT count)
    {
        AuditDraw(device, count);
        if (AuditSkipDraw())
            return S_OK;
        return g_OrigDrawIndexedPrimitive(device, type, base, minIndex, numVerts, startIndex, count);
    }

    HRESULT __stdcall HookedDrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count,
        const void* data, UINT stride)
    {
        AuditDraw(device, count);
        if (AuditSkipDraw())
            return S_OK;
        return g_OrigDrawPrimitiveUP(device, type, count, data, stride);
    }

    HRESULT __stdcall HookedDrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex,
        UINT numVerts, UINT count, const void* indices, D3DFORMAT fmt, const void* verts, UINT stride)
    {
        AuditDraw(device, count);
        if (AuditSkipDraw())
            return S_OK;
        return g_OrigDrawIndexedPrimitiveUP(device, type, minIndex, numVerts, count, indices, fmt, verts, stride);
    }

    int GpuTimerKindForRt0(VR* vr, UINT w, UINT h)
    {
        if (!vr || !vr->StereoWorldPassActive() || vr->HudPaintActive() || vr->m_CaptureReentry)
            return kSpanNone;
        if (bmvr::g_ReflectionRtW > 0 && static_cast<int>(w) == bmvr::g_ReflectionRtW
            && static_cast<int>(h) == bmvr::g_ReflectionRtH
            && !(w == vr->m_RenderWidth && h == vr->m_RenderHeight))
            return kSpanRefl;
        if (w == vr->m_RenderWidth && h == vr->m_RenderHeight)
            return kSpanEye;
        return kSpanNone;
    }

    // GpuPassDetail: "<size label> WxH f<format> [eyeL|eyeR] mrtN [ds ms=N ret=R]".
    std::string GpuTimerDescribeRt0(IDirect3DDevice9* device, VR* vr, IDirect3DSurface9* rt)
    {
        if (!rt)
            return "none";
        D3DSURFACE_DESC d{};
        if (FAILED(rt->GetDesc(&d)))
            return "?";
        char buf[192];
        const char* label = AuditSizeLabel(vr, d.Width, d.Height);
        const char* eyeTag = "";
        if (vr && rt == vr->m_D9LeftEyeSurface) eyeTag = " eyeL";
        else if (vr && rt == vr->m_D9RightEyeSurface) eyeTag = " eyeR";
        int mrt = 1;
        for (DWORD i = 1; i < 4; ++i)
        {
            IDirect3DSurface9* o = nullptr;
            if (SUCCEEDED(device->GetRenderTarget(i, &o)) && o)
            {
                ++mrt;
                o->Release();
            }
        }
        char ms[64] = {};
        IDirect3DSurface9* ds = nullptr;
        if (SUCCEEDED(device->GetDepthStencilSurface(&ds)) && ds)
        {
            bmvr::MsaaDepthStateText(ds, ms, sizeof(ms));
            if (char* cut = std::strstr(ms, " pend="))
                *cut = 0;
            ds->Release();
        }
        std::snprintf(buf, sizeof(buf), "%s%s%ux%u f%u%s rtms%u mrt%d%s%s", label, label[0] ? " " : "", d.Width,
            d.Height, static_cast<unsigned>(d.Format), eyeTag, static_cast<unsigned>(d.MultiSampleType), mrt,
            ms[0] ? " ds" : "", ms);
        return buf;
    }

    int GpuTimerDetailKind(int kind)
    {
        return kind == kSpanNone && GpuTimerDetail() ? kSpanOther : kind;
    }

    // Foveated rendering: after every RT0 change tell DXVK which eye's world
    // pass this is (rate image on eye-sized targets) or none (HUD paint,
    // captures, menus, everything outside the stereo world pass). DXVK only
    // attaches the map to render passes of exactly the eye size.
    // Foveated 2 = eye-tracked: the ring centre follows the gaze from the
    // OpenXR helper (XR_EXT_eye_gaze_interaction), read once per eye pass.
    // The centre only moves when the gaze moved more than FoveatedGazeStep
    // (tangent units, ~1 deg) so the maps are not re-uploaded every frame.
    // No usable gaze (not tracked, session not focused, sample older than
    // 150 ms) keeps the last centre for 300 ms (blinks), then falls back to
    // the lens centre.
    struct FoveationGazeState
    {
        int lastEye = -1;
        float cu[2] = { 0.5f, 0.5f }, cv[2] = { 0.5f, 0.5f };
        bool haveGaze = false;
        bool everGaze = false;         // eye tracking delivered a gaze at least once this launch
        float tx = 0.f, ty = 0.f;      // gaze tangents in use (x right, y up)
        uint64_t lastGoodMs = 0;
        uint32_t passes = 0, gazePasses = 0, moves = 0, fallbacks = 0;
        uint64_t statMs = 0;
    };
    FoveationGazeState g_FovGaze;

    void FoveationUpdateGaze(FoveationGazeState& g)
    {
        L4D2VROpenXrEyeGazeDesc gaze{};
        const uint64_t now = GetTickCount64();
        const bool read = L4D2VR_ReadOpenXrEyeGaze(gaze);
        const float fwd = -gaze.dir[2];
        const bool ok = read && gaze.valid && gaze.tracked && gaze.focused
            && now - gaze.publishTickMs <= 150 && fwd > 0.3f;
        if (ok)
        {
            const float tx = gaze.dir[0] / fwd, ty = gaze.dir[1] / fwd;
            const float step = bmvr::g_FoveatedGazeStep;
            if (!g.haveGaze || (tx - g.tx) * (tx - g.tx) + (ty - g.ty) * (ty - g.ty) > step * step)
            {
                g.tx = tx;
                g.ty = ty;
                ++g.moves;
            }
            g.haveGaze = true;
            g.everGaze = true;
            g.lastGoodMs = now;
        }
        else if (g.haveGaze && now - g.lastGoodMs > 300)
        {
            g.haveGaze = false;
            ++g.fallbacks;
        }
        ++g.passes;
        if (g.haveGaze)
            ++g.gazePasses;
        if (now - g.statMs >= 10000)
        {
            if (g.statMs)
                Game::logMsg("Foveation gaze: %u/%u eye passes on gaze, %u centre moves, %u fallbacks (last 10 s); "
                    "now %s tan %+.3f %+.3f (read=%d valid=%u tracked=%u focused=%u age=%llums)",
                    g.gazePasses, g.passes, g.moves, g.fallbacks, g.haveGaze ? "gaze" : "lens centre", g.tx, g.ty,
                    read ? 1 : 0, gaze.valid, gaze.tracked, gaze.focused,
                    static_cast<unsigned long long>(read ? now - gaze.publishTickMs : 0));
            g.statMs = now;
            g.passes = g.gazePasses = g.moves = g.fallbacks = 0;
        }
    }

    std::atomic<int> g_FovGpu{ -1 };   // variable-rate shading on this GPU: -1 not known yet, 0 no, 1 yes
    std::string FoveationStatusNow();

    void FoveationOnRt0(IDirect3DDevice9* device, VR* vr)
    {
        if (!bmvr::g_FoveatedAtStart || !device)
            return;
        if (g_FovGpu.load(std::memory_order_relaxed) < 0)
            g_FovGpu.store(bmvr::FoveationAvailable(device) ? 1 : 0, std::memory_order_relaxed);
        FoveationGazeState& g = g_FovGaze;
        int eye = 0;
        // Full rate while the VR settings panel, the VR weapon menu or the game's menus are up: the
        // panel floats in the scene and its text was the one place the coarse rings showed
        // (2026-11-16 headset). The weapon menu is opened looking ahead and then read with
        // glances, so its icons sat in the coarser rings (2026-11-18).
        if (vr && bmvr::g_Foveated > 0.f && vr->StereoWorldPassActive() && !vr->HudPaintActive()
            && !vr->m_CaptureReentry && !vr->SettingsPanelOpen() && !vr->WeaponMenuOpen()
            && !vr->m_GameUiVisible)
            eye = GpuTimerEye(vr);
        if (eye == 1 && eye != g.lastEye)
        {
            // Status changes (eye tracking found / paused / lost) go to the log once each.
            static std::string s_lastStatus;
            std::string status = FoveationStatusNow();
            if (status != s_lastStatus)
            {
                Game::logMsg("Foveation status: %s", status.c_str());
                s_lastStatus = std::move(status);
            }
        }
        if (eye && eye != g.lastEye)
        {
            // New eye pass: centre = straight ahead (lens) or the gaze, in this
            // eye's texture (0..1, v down) from its tangent rectangle.
            float cu = 0.5f, cv = 0.5f, tx = 0.f, ty = 0.f;
            if (bmvr::g_Foveated >= 1.5f)
            {
                FoveationUpdateGaze(g);
                if (g.haveGaze)
                {
                    tx = g.tx;
                    ty = g.ty;
                }
            }
            float r[4];
            if (vr->EyeRenderRect(eye, r) && r[1] > r[0] && r[3] > r[2])
            {
                cu = (tx - r[0]) / (r[1] - r[0]);
                cv = (r[3] - ty) / (r[3] - r[2]);
                // Ring radii are tuned as fractions of half the eye height on the PS VR2 (vertical span
                // 1.003 up + 1.142 down = 2.1451 tangent). Same angle elsewhere: scale by PS VR2 / this span.
                constexpr float kPsvr2TanSpan = 2.1451f;
                const float scale = kPsvr2TanSpan / (r[3] - r[2]);
                if (std::fabs(scale - bmvr::g_FoveatedRingScale) > 0.002f)
                {
                    bmvr::g_FoveatedRingScale = scale;
                    Game::logMsg("Foveation rings: eye vertical span %.4f tan -> ring scale %.3f (PS VR2 = 1)",
                        r[3] - r[2], scale);
                }
            }
            g.cu[eye - 1] = std::clamp(cu, 0.f, 1.f);
            g.cv[eye - 1] = std::clamp(cv, 0.f, 1.f);
        }
        g.lastEye = eye;
        bmvr::FoveationSelect(device, eye, vr ? vr->m_RenderWidth : 0, vr ? vr->m_RenderHeight : 0,
            eye ? g.cu[eye - 1] : 0.5f, eye ? g.cv[eye - 1] : 0.5f);
    }

    std::string FoveationStatusNow()
    {
        if (bmvr::g_Foveated < 0.5f)
            return "off";
        if (!bmvr::g_FoveatedAtStart)
            return "restart the game to turn on";
        const int gpu = g_FovGpu.load(std::memory_order_relaxed);
        if (gpu == 0)
            return "off (GPU has no variable-rate shading)";
        if (gpu < 0)
            return "starting";
        if (bmvr::g_Foveated < 1.5f)
            return "fixed (lens centre)";
        if (g_FovGaze.haveGaze)
            return "eye-tracked";
        return g_FovGaze.everGaze ? "fixed (eye tracking paused)" : "fixed (no eye tracking)";
    }

    // Called after RT0 changes (SetRenderTarget index 0).
    void GpuTimerOnRt0(IDirect3DDevice9* device, VR* vr, UINT w, UINT h)
    {
        if (!device || !GpuTimerEnabled() || !GpuTimerThreadOk())
            return;
        int label = -1;
        if (GpuTimerDetail())
        {
            if (g_GpuTimer.waitFirstRt)
            {
                g_GpuTimer.waitFirstRt = false;
                g_GpuTimer.cpuGapSum += QpcNowMs() - g_GpuTimer.presentReturnMs;
                ++g_GpuTimer.cpuGapN;
            }
            g_GpuTimer.device = device;
            g_GpuTimer.curRt = GpuTimerDescribeRt0(device, vr, g_Rt0ActualPtr);
            label = GpuTimerCurrentLabel();
        }
        GpuTimerSwitch(device, GpuTimerDetailKind(GpuTimerKindForRt0(vr, w, h)), GpuTimerEye(vr), label);
    }

    // GpuPassDetail: reopen the current span with a new label (material
    // group change, copy, MSAA switch), same kind and eye.
    void GpuTimerRelabel(IDirect3DDevice9* device)
    {
        GpuPassTimer& t = g_GpuTimer;
        if (!device || !GpuTimerDetail() || !GpuTimerThreadOk() || t.openKind == kSpanNone)
            return;
        GpuTimerSwitch(device, t.openKind, t.openEye, GpuTimerCurrentLabel());
    }

    // Material group: "__gbxog0" (global Xen fog), "__gbxog*" (fog volumes),
    // other "__" names without trailing digits, else the directory part.
    void GpuTimerMaterialGroup(const char* name, std::string& out)
    {
        out.clear();
        if (!name || !name[0])
        {
            out = "-";
            return;
        }
        if (std::strncmp(name, "__gbxog", 7) == 0)
        {
            out = name[7] == '0' ? "__gbxog0 (global Xen fog)" : "__gbxog* (fog volumes)";
            return;
        }
        const char* slash = std::strrchr(name, '/');
        const char* bslash = std::strrchr(name, '\\');
        if (bslash && (!slash || bslash > slash))
            slash = bslash;
        // Post / engine materials are few and each is its own pass: full name.
        const bool fullName = std::strncmp(name, "dev/", 4) == 0 || std::strncmp(name, "engine/", 7) == 0;
        if (slash && name[0] != '_' && !fullName)
        {
            out.assign(name, static_cast<size_t>(slash - name + 1));
            return;
        }
        size_t n = std::strlen(name);
        while (n > 1 && std::isdigit(static_cast<unsigned char>(name[n - 1])))
            --n;
        out.assign(name, n);
    }

    // GpuPassDetail: named scope (copy, MSAA retire/begin) inside the open span.
    struct GpuTimerScope
    {
        IDirect3DDevice9* device = nullptr;
        bool on = false;
        std::string saved;
        GpuTimerScope(IDirect3DDevice9* dev, const char* what)
        {
            if (!dev || !GpuTimerDetail() || !GpuTimerThreadOk() || g_GpuTimer.openKind == kSpanNone)
                return;
            device = dev;
            on = true;
            saved = g_GpuTimer.curOverride;
            g_GpuTimer.curOverride = what;
            GpuTimerRelabel(device);
        }
        ~GpuTimerScope()
        {
            if (!on || g_GpuTimer.failed)
                return;
            g_GpuTimer.curOverride = saved;
            GpuTimerRelabel(device);
        }
    };

    bool GpuTimerRead(IDirect3DQuery9* q, UINT64& out)
    {
        return q && q->GetData(&out, sizeof(out), 0) == S_OK;
    }

    // Called before Present: closes the frame and resolves the oldest one.
    // GpuPassDetail report: ms per frame per eye (L, R, outside eyes = O),
    // grouped by RT0, then the top labels (RT0 | material group / step).
    void GpuTimerLogDetail(VR* vr)
    {
        GpuPassTimer& t = g_GpuTimer;
        const double n = static_cast<double>(t.detailFrames);
        double tot[3] = {};
        std::map<std::string, std::array<double, 3>> byRt;
        std::map<std::string, std::array<double, 3>> byMat;
        for (size_t i = 0; i < t.labelNames.size(); ++i)
        {
            const std::string& name = t.labelNames[i];
            const size_t bar = name.find(" | ");
            std::array<double, 3>& r = byRt[bar == std::string::npos ? name : name.substr(0, bar)];
            std::array<double, 3>& m = byMat[bar == std::string::npos ? std::string("-") : name.substr(bar + 3)];
            for (int e = 0; e < 3; ++e)
            {
                r[e] += t.labelMs[i][e];
                m[e] += t.labelMs[i][e];
                tot[e] += t.labelMs[i][e];
            }
        }
        Game::logMsg("GPU pass detail: %d frames, %.0f spans/frame, %d frames over the span cap | total L=%.2f R=%.2f O=%.2f ms/frame (eye %ux%u, MSAA %s) | CPU: Present call %.2f ms, Present return -> first RT0 %.2f ms",
            t.detailFrames, t.detailSpans / n, t.detailDroppedFull, tot[1] / n, tot[2] / n, tot[0] / n,
            vr ? vr->m_RenderWidth : 0, vr ? vr->m_RenderHeight : 0, vr && vr->UseVrMsaa() ? "on" : "off",
            t.cpuPresentN ? t.cpuPresentSum / t.cpuPresentN : 0.0, t.cpuGapN ? t.cpuGapSum / t.cpuGapN : 0.0);
        t.cpuPresentSum = t.cpuGapSum = 0.0;
        t.cpuPresentN = t.cpuGapN = 0;
        auto dump = [&](const char* tag, const std::map<std::string, std::array<double, 3>>& m, size_t maxLines) {
            std::vector<std::pair<double, const std::string*>> order;
            for (const auto& [k, v] : m)
                order.emplace_back(v[0] + v[1] + v[2], &k);
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            size_t lines = 0;
            for (const auto& [sum, k] : order)
            {
                if (lines >= maxLines || sum / n < 0.002)
                    break;
                const std::array<double, 3>& v = m.at(*k);
                Game::logMsg("GPU pass %s: L %6.3f R %6.3f O %6.3f | %s", tag, v[1] / n, v[2] / n, v[0] / n, k->c_str());
                ++lines;
            }
        };
        dump("rt", byRt, 40);
        dump("mat", byMat, 40);
        std::map<std::string, std::array<double, 3>> byLabel;
        for (size_t i = 0; i < t.labelNames.size(); ++i)
            byLabel[t.labelNames[i]] = t.labelMs[i];
        dump("label", byLabel, 400);
        for (std::array<double, 3>& v : t.labelMs)
            v = { 0.0, 0.0, 0.0 };
        t.detailSpans = 0.0;
        t.detailFrames = 0;
        t.detailDroppedFull = 0;
        t.lastDetailLogMs = GetTickCount();
        if (++t.detailReports == 2)
            t.dumpNextFrame = true;
    }

    void GpuTimerOnPresent(IDirect3DDevice9* device, VR* vr)
    {
        if (!device || !GpuTimerEnabled())
            return;
        GpuPassTimer& t = g_GpuTimer;
        // The timer belongs to the thread that presents (the device thread). With
        // MulticoreMode that becomes the render thread after loading, so follow
        // it instead of staying on the first thread that drew (timing stopped).
        if (const DWORD tid = GetCurrentThreadId(); t.threadId != tid)
        {
            if (t.threadId != 0)
                Game::logMsg("GPU pass timing: following the presenting thread %lu (was %lu)",
                    static_cast<unsigned long>(tid), static_cast<unsigned long>(t.threadId));
            t.threadId = tid;
            GpuTimerDropOpenBegin();
            t.openKind = kSpanNone;
            t.openLabel = -1;
            for (int i = 0; i < kGpuFrames; ++i)
            {
                for (GpuSpan& sp : t.frames[i])
                    GpuTimerRecycleSpan(sp);
                t.frames[i].clear();
                GpuTimerRecycle(t.frameEnd[i]);
            }
            t.lastFrameEndTs = 0;
        }
        const int resumeKind = t.openKind;
        const int resumeEye = t.openEye;
        const int resumeLabel = t.openLabel;
        GpuTimerSwitch(device, kSpanNone, 0);
        if (GpuTimerDetail())
        {
            t.device = device;
            // Material names: the Bind hooks stay on while detail timing runs
            // (they are created lazily, so keep asking).
            Hooks::SetMatBindHooks(true);
        }
        if (!t.freqQuery)
        {
            if (SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &t.freqQuery)) && t.freqQuery)
                t.freqQuery->Issue(D3DISSUE_END);
            else
            {
                t.failed = true;
                Game::logMsg("GPU pass timing off: D3DQUERYTYPE_TIMESTAMPFREQ not available");
                return;
            }
        }
        if (t.freq == 0)
            GpuTimerRead(t.freqQuery, t.freq);
        t.frameEnd[t.write] = GpuTimerStamp(device);
        t.write = (t.write + 1) % kGpuFrames;
        // The slot about to be reused is kGpuFrames - 1 Presents old.
        std::vector<GpuSpan>& old = t.frames[t.write];
        IDirect3DQuery9*& oldEnd = t.frameEnd[t.write];
        bool ready = t.freq != 0;
        double frameSpan[kSpanKinds][3] = {};
        t.detailScratch.clear();
        const size_t spanCount = old.size();
        for (GpuSpan& sp : old)
        {
            UINT64 a = 0, b = 0;
            if (ready && GpuTimerRead(sp.begin, a) && GpuTimerRead(sp.end, b) && b >= a)
            {
                const double ms = static_cast<double>(b - a) * 1000.0 / static_cast<double>(t.freq);
                frameSpan[sp.kind][sp.eye] += ms;
                if (sp.label >= 0)
                    t.detailScratch.push_back(GpuDetailSample{ sp.label, sp.eye, ms });
            }
            else
                ready = false;
            GpuTimerRecycleSpan(sp);
        }
        old.clear();
        UINT64 endTs = 0;
        const bool haveEnd = ready && GpuTimerRead(oldEnd, endTs);
        GpuTimerRecycle(oldEnd);
        if (ready && haveEnd)
        {
            for (int k = 0; k < kSpanKinds; ++k)
                for (int e = 0; e < 3; ++e)
                    t.sumMs[k][e] += frameSpan[k][e];
            if (t.dumpNextFrame && !t.detailScratch.empty())
            {
                t.dumpNextFrame = false;
                Game::logMsg("GPU pass frame: %zu spans in GPU order (eye, ms, label)", t.detailScratch.size());
                double at = 0.0;
                for (const GpuDetailSample& d : t.detailScratch)
                {
                    Game::logMsg("GPU pass frame: @%7.3f e%d %6.3f | %s", at, d.eye, d.ms, t.labelNames[d.label].c_str());
                    at += d.ms;
                }
            }
            if (!t.detailScratch.empty())
            {
                for (const GpuDetailSample& d : t.detailScratch)
                    t.labelMs[d.label][d.eye] += d.ms;
                t.detailSpans += static_cast<double>(spanCount);
                ++t.detailFrames;
            }
            if (t.lastFrameEndTs != 0 && endTs > t.lastFrameEndTs)
            {
                t.frameMs += static_cast<double>(endTs - t.lastFrameEndTs) * 1000.0 / static_cast<double>(t.freq);
                ++t.frameIntervals;
            }
            t.lastFrameEndTs = endTs;
            ++t.resolved;
        }
        else
        {
            t.lastFrameEndTs = 0;
            ++t.dropped;
        }
        const DWORD now = GetTickCount();
        if (t.lastLogMs == 0)
            t.lastLogMs = now;
        if (now - t.lastLogMs >= 5000 && t.resolved > 0)
        {
            const double n = static_cast<double>(t.resolved);
            Game::logMsg("GPU ms/frame: reflection-set L=%.2f R=%.2f | refract copy %.2f | eye-size RTs L=%.2f R=%.2f | frame interval %.2f | frames=%d dropped=%d ReflectionRtScale %.2f (%dx%d) RefractCopyRtScale %.2f (%dx%d)",
                t.sumMs[kSpanRefl][1] / n, t.sumMs[kSpanRefl][2] / n,
                (t.sumMs[kSpanCopy][0] + t.sumMs[kSpanCopy][1] + t.sumMs[kSpanCopy][2]) / n,
                t.sumMs[kSpanEye][1] / n, t.sumMs[kSpanEye][2] / n,
                t.frameIntervals > 0 ? t.frameMs / t.frameIntervals : 0.0,
                t.resolved, t.dropped, bmvr::g_ReflectionRtScale,
                bmvr::g_ReflectionRtW, bmvr::g_ReflectionRtH, bmvr::RefractCopyRtScale(),
                bmvr::g_RefractCopyRtW, bmvr::g_RefractCopyRtH);
            for (int k = 0; k < kSpanKinds; ++k)
                for (int e = 0; e < 3; ++e)
                    t.sumMs[k][e] = 0.0;
            t.frameMs = 0.0;
            t.frameIntervals = 0;
            t.resolved = 0;
            t.dropped = 0;
            t.lastLogMs = now;
        }
        if (t.lastDetailLogMs == 0)
            t.lastDetailLogMs = now;
        if (now - t.lastDetailLogMs >= 10000 && t.detailFrames > 0)
            GpuTimerLogDetail(vr);
        // RT0 stays bound across Present; keep timing it in the next frame.
        // Detail: until the next RT0 change that span is labelled
        // "after Present" (GPU idle waiting for the next frame + its first work).
        if (GpuTimerDetail() && resumeKind != kSpanNone)
        {
            t.curOverride = "after Present";
            GpuTimerSwitch(device, resumeKind, resumeEye, GpuTimerCurrentLabel());
            t.curOverride.clear();
        }
        else
            GpuTimerSwitch(device, resumeKind, resumeEye, resumeLabel);
    }

    void GpuTimerReleaseAll()
    {
        GpuPassTimer& t = g_GpuTimer;
        auto rel = [](IDirect3DQuery9*& q) {
            if (q)
                q->Release();
            q = nullptr;
        };
        if (t.openBeginShared)
            t.openBegin = nullptr;
        rel(t.openBegin);
        t.openBeginShared = false;
        t.openKind = kSpanNone;
        t.openLabel = -1;
        for (int f = 0; f < kGpuFrames; ++f)
        {
            for (GpuSpan& sp : t.frames[f])
            {
                if (!sp.beginShared)
                    rel(sp.begin);
                sp.begin = nullptr;
                rel(sp.end);
            }
            t.frames[f].clear();
            rel(t.frameEnd[f]);
        }
        for (IDirect3DQuery9*& q : t.pool)
            rel(q);
        t.pool.clear();
        rel(t.freqQuery);
        t.freq = 0;
        t.lastFrameEndTs = 0;
    }

    HRESULT __stdcall HookedPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* dirty)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        // Which devices present (the new game UI has two, 2026-11-23): a
        // count per device every 300 Presents of the first one seen, 8 lines.
        {
            static IDirect3DDevice9* s_dev[2] = {};
            static unsigned s_n[2] = {};
            static int s_lines = 0;
            int slot = -1;
            for (int i = 0; i < 2; ++i)
            {
                if (s_dev[i] == device || !s_dev[i])
                {
                    s_dev[i] = device;
                    slot = i;
                    break;
                }
            }
            if (slot >= 0)
                ++s_n[slot];
            if (slot == 0 && s_n[0] % 300 == 1 && s_lines < 8)
            {
                ++s_lines;
                IDirect3DDevice9* vrDev = nullptr;
                if (g_D3DVR9)
                    g_D3DVR9->GetD3DDevice(&vrDev);
                Game::logMsg("Present per device: %p n=%u | %p n=%u | VR device %p",
                    static_cast<void*>(s_dev[0]), s_n[0], static_cast<void*>(s_dev[1]), s_n[1], static_cast<void*>(vrDev));
                if (vrDev)
                    vrDev->Release();
            }
        }
        if (vr && vr->m_DirectEyeSubmit && !vr->Want2dMenuPanel())
        {
            static int s_presentStereo;
            if (s_presentStereo < 4)
            {
                Game::logMsg("Present after stereo/blit submit=%u worldMatch=%d gbmatch=%d hwndBlit=%d",
                    vr->m_SubmitCount,
                    bmvr::OffscreenWorldMatchesEyes() ? 1 : 0,
                    bmvr::UseGbMatchViewLock() ? 1 : 0,
                    vr->ShouldHwndBlitStereo() ? 1 : 0);
                ++s_presentStereo;
            }
        }
        if (vr && vr->m_IsVREnabled && vr->IsGameplayEligible() && vr->m_DirectEyeSubmit
            && !vr->Want2dMenuPanel())
            vr->LogRenderPathIfChanged();
        if (vr)
            vr->CaptureFrameBeforePresent();
        GpuTimerOnPresent(device, vr);
        AuditOnPresent();
        if (!g_OrigPresent)
            return D3DERR_INVALIDCALL;
        if (!GpuTimerDetail())
            return g_OrigPresent(device, src, dst, hwnd, dirty);
        const double callMs = QpcNowMs();
        const HRESULT hr = g_OrigPresent(device, src, dst, hwnd, dirty);
        g_GpuTimer.presentReturnMs = QpcNowMs();
        g_GpuTimer.cpuPresentSum += g_GpuTimer.presentReturnMs - callMs;
        ++g_GpuTimer.cpuPresentN;
        g_GpuTimer.waitFirstRt = true;
        return hr;
    }

    HRESULT __stdcall HookedSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* rt)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        IDirect3DSurface9* const inRtForTrace = rt;
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
            if ((bmvr::OffscreenWorldMatchesEyes() || vr->McPipelineEnabled())
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
        IDirect3DSurface9* const requestedRt = inRtForTrace;
        const HRESULT hr = g_OrigSetRenderTarget(device, index, rt);
        // Pass trace (VR/trace.txt): every RT bind with the surface the game
        // asked for, what was bound, and who called. New game UI study: the
        // world's "back buffer" binds did not reach the eye (2026-11-23).
        if (bmvr::g_PassTraceFrames > 0 && index == 0)
        {
            D3DSURFACE_DESC inDesc{}, outDesc{};
            if (requestedRt)
                requestedRt->GetDesc(&inDesc);
            if (rt)
                rt->GetDesc(&outDesc);
            IDirect3DSurface9* bb = CachedBackBufferPtr(device);
            HMODULE mod = nullptr;
            char path[MAX_PATH] = "?";
            void* caller = _ReturnAddress();
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCSTR>(caller), &mod) && mod)
                GetModuleFileNameA(mod, path, MAX_PATH);
            const char* base = std::strrchr(path, '\\');
            base = base ? base + 1 : path;
            // Which device each surface belongs to (DXVK rejects a foreign one).
            int inDev = -1, outDev = -1;
            IDirect3DDevice9* d = nullptr;
            if (requestedRt && SUCCEEDED(requestedRt->GetDevice(&d)) && d)
            {
                inDev = (d == device) ? 1 : 0;
                d->Release();
            }
            d = nullptr;
            if (rt && SUCCEEDED(rt->GetDevice(&d)) && d)
            {
                outDev = (d == device) ? 1 : 0;
                d->Release();
            }
            Game::logMsg("trace: d3d SetRT0 dev=%p inDev=%d outDev=%d outUsage=0x%X", static_cast<void*>(device), inDev, outDev,
                static_cast<unsigned>(outDesc.Usage));
            Game::logMsg("trace: d3d SetRT0 in=%p %ux%u fmt=%u isBB=%d -> %p %ux%u redir=%d hud=%d dest=%p hr=0x%08X caller=%s+0x%X",
                static_cast<void*>(requestedRt), inDesc.Width, inDesc.Height, static_cast<unsigned>(inDesc.Format),
                (bb && requestedRt == bb) ? 1 : 0, static_cast<void*>(rt), outDesc.Width, outDesc.Height,
                redirectedToEye ? 1 : 0, (vr && vr->HudPaintActive()) ? 1 : 0, static_cast<void*>(dest),
                static_cast<unsigned>(hr), base,
                mod ? static_cast<unsigned>(reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(mod)) : 0u);
        }
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
            GpuTimerOnRt0(device, vr, g_Rt0ActualW, g_Rt0ActualH);
            FoveationOnRt0(device, vr);
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

    bool Rt0IsGrownReflection(VR* vr, UINT& w, UINT& h);

    HRESULT __stdcall HookedSetDepthStencil(IDirect3DDevice9* device, IDirect3DSurface9* depth)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        IDirect3DSurface9* eyeDepth = nullptr;
        if (vr)
            eyeDepth = vr->McPlaybackEyeDepth();
        if (!eyeDepth && vr && !vr->McPipelineEnabled() && vr->StereoEyeBlitActive()
            && bmvr::OffscreenWorldMatchesEyes())
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
        // Mono pass (main menu background map) on a grown reflection RT: the
        // engine binds the window depth (1920x1080), shorter than the RT
        // (1728x1760), so the remapped full-RT viewport would still only draw
        // where the depth exists. Lend it the left eye's depth (eye-sized, not
        // in use outside a stereo pass; the reflection view clears it first).
        // Shaderapi binds RT0 before the depth, so RT0 is already the new one.
        UINT reflW = 0, reflH = 0;
        if (vr && device && !eyeDepth && depth && vr->m_D9LeftEyeDepthSurface
            && depth != vr->m_D9LeftEyeDepthSurface
            && !vr->StereoWorldPassActive()
            && Rt0IsGrownReflection(vr, reflW, reflH)
            && SurfaceMatchesWindowOrBackbuffer(device, depth))
        {
            D3DSURFACE_DESC dd{}, ed{};
            if (SUCCEEDED(depth->GetDesc(&dd)) && (dd.Width < reflW || dd.Height < reflH)
                && SUCCEEDED(vr->m_D9LeftEyeDepthSurface->GetDesc(&ed))
                && ed.Width >= reflW && ed.Height >= reflH)
            {
                static int s_monoReflDsLog;
                if (s_monoReflDsLog < 4)
                {
                    Game::logMsg("D3D mono reflection RT %ux%u: depth %ux%u -> left eye depth %ux%u",
                        reflW, reflH, dd.Width, dd.Height, ed.Width, ed.Height);
                    ++s_monoReflDsLog;
                }
                depth = vr->m_D9LeftEyeDepthSurface;
            }
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

    // RT0 is one of the reflection RTs grown by ReflectionRtScale, in a
    // stereo eye pass or a mono pass (main menu background map). The
    // reflection/refraction views inherit the main view's window-sized
    // CViewSetup (1920x1080), so their window-sized viewports, scissors,
    // clears and texel-size constants must map to this RT, not to the eye.
    // Left alone, the lobby mirror floor drew only into the top-left
    // 1920x1080 of its 2000x2048 RTs: floor black below 52 % and past 96 % of
    // the eye (2026-10-30). Mono too: the menu's lobby background (background02)
    // filled only the top 1728x1080 of its 1728x1760 RT and the floor went
    // black (2026-11-24). At ReflectionRtScale 1 the RT is eye-sized and the
    // eye path below already handles it.
    bool Rt0IsGrownReflection(VR* vr, UINT& w, UINT& h)
    {
        if (!vr || (bmvr::g_ReflectionRtW <= 0 && bmvr::g_RefractCopyRtW <= 0)
            || !g_Rt0ActualKnown || !g_Rt0ActualPtr)
            return false;
        if (vr->m_CaptureReentry || vr->HudPaintActive())
            return false;
        // Exact size: +-16 px also matched the eye/2 bloom brightpass target
        // (2008x2048 next to 2000x2048) and remapped it every frame.
        if (!bmvr::GrownAuxRtSized(static_cast<int>(g_Rt0ActualW), static_cast<int>(g_Rt0ActualH)))
            return false;
        if (g_Rt0ActualW == vr->m_RenderWidth && g_Rt0ActualH == vr->m_RenderHeight)
            return false;
        w = g_Rt0ActualW;
        h = g_Rt0ActualH;
        return true;
    }

    // The window-sized reflection viewport/scissor/clear as the game issues
    // it on a grown reflection RT. When the RT is narrower or shorter than the
    // window (eye 3472 at ReflectionRtScale 0.5 = 1728 wide vs a 1920x1080
    // window) the size can arrive clamped to the RT per axis; that must remap
    // too, or the view only fills part of the RT and the rest keeps an older
    // frame (2026-11-03: the menu lobby showing in bm_c2a2a's water).
    bool MatchesWindowOnReflectionRt(UINT w, UINT h, UINT rw, UINT rh)
    {
        if (ViewportMatchesWindow(w, h))
            return true;
        UINT winW = 0, winH = 0;
        if (!QueryGameClientSize(winW, winH))
            return false;
        const int cw = static_cast<int>((std::min)(winW, rw));
        const int ch = static_cast<int>((std::min)(winH, rh));
        return (cw != static_cast<int>(winW) || ch != static_cast<int>(winH))
            && std::abs(static_cast<int>(w) - cw) <= 16 && std::abs(static_cast<int>(h) - ch) <= 16;
    }

    void LogReflectionRemap(const char* what, UINT fromW, UINT fromH, UINT toW, UINT toH)
    {
        static UINT s_lastFrom[3][2];
        static UINT s_lastTo[3][2];
        const int slot = what[0] == 'v' ? 0 : (what[0] == 's' ? 1 : 2);
        if (s_lastFrom[slot][0] == fromW && s_lastFrom[slot][1] == fromH
            && s_lastTo[slot][0] == toW && s_lastTo[slot][1] == toH)
            return;
        s_lastFrom[slot][0] = fromW;
        s_lastFrom[slot][1] = fromH;
        s_lastTo[slot][0] = toW;
        s_lastTo[slot][1] = toH;
        Game::logMsg("D3D reflection RT %s %ux%u -> %ux%u (ReflectionRtScale)", what, fromW, fromH, toW, toH);
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
        // Window texel sizes map to the bound RT: the eye, or a grown
        // reflection RT during the reflection/refraction views.
        UINT tw = vr->m_RenderWidth;
        UINT th = vr->m_RenderHeight;
        UINT rw = 0, rh = 0;
        if (Rt0IsGrownReflection(vr, rw, rh))
        {
            tw = rw;
            th = rh;
        }
        const float ew = static_cast<float>(tw);
        const float eh = static_cast<float>(th);
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
        if (use && g_AuditFramesLeft.load(std::memory_order_relaxed) > 0 && vec4Count <= 256)
        {
            const bool vs = tag && tag[0] == 'V';
            // Who writes VS c14-c17 (a view-projection the inside-light pass reads).
            if (vs && start <= 17 && start + vec4Count > 14 && AuditLogging() && g_AuditLines < kAuditMaxLines)
            {
                const float* r = use + (14 > start ? (14 - start) * 4 : 0);
                Game::logMsg("audit vsc14 e%d start=%u n=%u mat=%s c14=(%.4g,%.4g,%.4g,%.4g)",
                    GpuTimerEye(vr), start, vec4Count, t_AuditMaterial, r[0], r[1], r[2], r[3]);
                ++g_AuditLines;
            }
            const UINT cap = vs ? 256u : 224u;
            if (start < cap)
            {
                const UINT n = (std::min)(vec4Count, cap - start);
                std::memcpy((vs ? g_AuditVsConst : g_AuditPsConst) + start * 4, use, n * 4 * sizeof(float));
            }
        }
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
        {
            // Every distinct viewport size issued on a grown reflection RT
            // (first 16), to see what the reflection views ask for.
            static int s_reflVpLog;
            UINT rw = 0, rh = 0;
            if (pViewport && vr && s_reflVpLog < 16 && Rt0IsGrownReflection(vr, rw, rh))
            {
                static UINT s_last[2];
                if (s_last[0] != pViewport->Width || s_last[1] != pViewport->Height)
                {
                    s_last[0] = pViewport->Width;
                    s_last[1] = pViewport->Height;
                    UINT winW = 0, winH = 0;
                    QueryGameClientSize(winW, winH);
                    Game::logMsg("D3D reflection RT %ux%u: viewport %ux%u @%u,%u (window %ux%u, match %d)",
                        rw, rh, pViewport->Width, pViewport->Height, pViewport->X, pViewport->Y,
                        winW, winH, MatchesWindowOnReflectionRt(pViewport->Width, pViewport->Height, rw, rh) ? 1 : 0);
                    ++s_reflVpLog;
                }
            }
        }
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
        UINT reflW = 0, reflH = 0;
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
            && pViewport->X <= 16 && pViewport->Y <= 16
            && Rt0IsGrownReflection(vr, reflW, reflH)
            && MatchesWindowOnReflectionRt(pViewport->Width, pViewport->Height, reflW, reflH))
        {
            vp = *pViewport;
            vp.X = 0;
            vp.Y = 0;
            vp.Width = reflW;
            vp.Height = reflH;
            use = &vp;
            LogReflectionRemap("viewport", pViewport->Width, pViewport->Height, reflW, reflH);
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
        // Engine SetScissorRect often runs at HWND 2560x1440 *before* stereo
        // (ShouldExpandWindowVpOnWorldRt is false then). Expanding only the
        // viewport left a 1440-tall scissor on the 2560² eye.
        if (SUCCEEDED(hr) && use != pViewport && use && g_OrigSetScissorRect)
        {
            RECT sc{
                static_cast<LONG>(use->X),
                static_cast<LONG>(use->Y),
                static_cast<LONG>(use->X + use->Width),
                static_cast<LONG>(use->Y + use->Height)
            };
            g_OrigSetScissorRect(device, &sc);
        }
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
            UINT reflW = 0, reflH = 0;
            if (!vr->D3dRt0IsMcRing()
                && pRect->left <= 16 && pRect->top <= 16
                && Rt0IsGrownReflection(vr, reflW, reflH)
                && MatchesWindowOnReflectionRt(w, h, reflW, reflH))
            {
                expanded.left = 0;
                expanded.top = 0;
                expanded.right = static_cast<LONG>(reflW);
                expanded.bottom = static_cast<LONG>(reflH);
                use = &expanded;
                LogReflectionRemap("scissor", w, h, reflW, reflH);
            }
            else if (vr->D3dRt0IsMcRing()
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
        AuditClear(device, flags);
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        D3DRECT full{};
        const D3DRECT* useRects = rects;
        DWORD useCount = count;
        UINT reflW = 0, reflH = 0;
        if (device && vr && Rt0IsGrownReflection(vr, reflW, reflH))
        {
            // Rect-less clears cover the viewport, which SetViewport already
            // grew. A window-sized rect at the origin covers the whole RT.
            if (rects && count >= 1)
            {
                const LONG w = rects[0].x2 - rects[0].x1;
                const LONG h = rects[0].y2 - rects[0].y1;
                if (w > 0 && h > 0 && rects[0].x1 <= 16 && rects[0].y1 <= 16
                    && MatchesWindowOnReflectionRt(static_cast<UINT>(w), static_cast<UINT>(h), reflW, reflH))
                {
                    full.x1 = 0;
                    full.y1 = 0;
                    full.x2 = static_cast<LONG>(reflW);
                    full.y2 = static_cast<LONG>(reflH);
                    useRects = &full;
                    useCount = 1;
                    LogReflectionRemap("clear", static_cast<UINT>(w), static_cast<UINT>(h), reflW, reflH);
                }
            }
        }
        else if (device && vr && ShouldExpandWindowVpOnWorldRt(device, vr))
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

    HRESULT HookedStretchRectInner(IDirect3DDevice9* device, IDirect3DSurface9* pSource,
        const RECT* pSourceRect, IDirect3DSurface9* pDest, const RECT* pDestRect,
        D3DTEXTUREFILTERTYPE filter);

    // Refract copies (eye -> grown _rt_PowerOfTwoFB) get their own GPU span.
    HRESULT __stdcall HookedStretchRect(IDirect3DDevice9* device, IDirect3DSurface9* pSource,
        const RECT* pSourceRect, IDirect3DSurface9* pDest, const RECT* pDestRect,
        D3DTEXTUREFILTERTYPE filter)
    {
        VR* vr = (g_Game && g_Game->m_VR) ? g_Game->m_VR : nullptr;
        bool timed = false;
        int resumeKind = kSpanNone;
        int resumeEye = 0;
        int resumeLabel = -1;
        if (device && pDest && vr && bmvr::g_RefractCopyRtW > 0 && GpuTimerEnabled()
            && vr->StereoWorldPassActive() && GpuTimerThreadOk())
        {
            D3DSURFACE_DESC d{};
            if (SUCCEEDED(pDest->GetDesc(&d)) && static_cast<int>(d.Width) == bmvr::g_RefractCopyRtW
                && static_cast<int>(d.Height) == bmvr::g_RefractCopyRtH)
            {
                resumeKind = g_GpuTimer.openKind;
                resumeEye = g_GpuTimer.openEye;
                resumeLabel = g_GpuTimer.openLabel;
                GpuTimerSwitch(device, kSpanCopy, GpuTimerEye(vr), resumeLabel);
                timed = true;
            }
        }
        AuditStretchRect(pSource, pSourceRect, pDest, pDestRect);
        HRESULT hr;
        if (GpuTimerDetail() && pSource && pDest)
        {
            if (timed)
                GpuTimerSwitch(device, kSpanCopy, GpuTimerEye(vr), resumeLabel);
            D3DSURFACE_DESC sd{}, dd{};
            pSource->GetDesc(&sd);
            pDest->GetDesc(&dd);
            char what[160];
            std::snprintf(what, sizeof(what), "copy %s%s%ux%u f%u ms%u -> %s%s%ux%u f%u%s",
                AuditSizeLabel(vr, sd.Width, sd.Height), AuditSizeLabel(vr, sd.Width, sd.Height)[0] ? " " : "",
                sd.Width, sd.Height, static_cast<unsigned>(sd.Format), static_cast<unsigned>(sd.MultiSampleType),
                AuditSizeLabel(vr, dd.Width, dd.Height), AuditSizeLabel(vr, dd.Width, dd.Height)[0] ? " " : "",
                dd.Width, dd.Height, static_cast<unsigned>(dd.Format), timed ? " (refract)" : "");
            GpuTimerScope scope(device, what);
            hr = HookedStretchRectInner(device, pSource, pSourceRect, pDest, pDestRect, filter);
        }
        else
            hr = HookedStretchRectInner(device, pSource, pSourceRect, pDest, pDestRect, filter);
        if (timed && !g_GpuTimer.failed)
            GpuTimerSwitch(device, resumeKind, resumeEye, resumeLabel);
        return hr;
    }

    HRESULT HookedStretchRectInner(IDirect3DDevice9* device, IDirect3DSurface9* pSource,
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
        BMVR_TRY
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
        BMVR_TRY
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
        BMVR_TRY
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
        BMVR_TRY
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
        BMVR_TRY
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
        BMVR_TRY
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
        BMVR_TRY
        {
            vis = static_cast<IEngineVGui*>(engineVgui)->IsGameUIVisible();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vis = false;
        }
        return vis;
    }

    HRESULT SehTransferSurfaceNoWait(IDirect3DSurface9* surf)
    {
        if (!g_D3DVR9 || !surf)
            return E_POINTER;
        HRESULT hr = E_FAIL;
        BMVR_TRY
        {
            hr = g_D3DVR9->TransferSurface(surf, FALSE);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return E_FAIL;
        }
        return hr;
    }

    void** SehObjectVTable(void* obj)
    {
        void** vt = nullptr;
        if (!obj)
            return nullptr;
        BMVR_TRY
        {
            vt = *reinterpret_cast<void***>(obj);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vt = nullptr;
        }
        return vt;
    }

    int SehPlayerTeamNumber(void* player)
    {
        void** vt = SehObjectVTable(player);
        if (!vt)
            return 0;
        int team = 0;
        BMVR_TRY
        {
            using Fn = int(__thiscall*)(void*);
            Fn fn = reinterpret_cast<Fn>(vt[Offsets::kCBasePlayer_GetTeamNumberVt / 4]);
            if (fn)
                team = fn(player);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            team = 0;
        }
        return team;
    }

    bool SehPlayerEyePose(void* player, Vector& eye, QAngle& ang)
    {
        void** vt = SehObjectVTable(player);
        if (!vt)
            return false;
        bool ok = false;
        BMVR_TRY
        {
            using FnPos = Vector*(__thiscall*)(void*, Vector*);
            using FnAng = QAngle*(__thiscall*)(void*);
            FnPos pos = reinterpret_cast<FnPos>(vt[Offsets::kCBasePlayer_EyePositionVt / 4]);
            FnAng angles = reinterpret_cast<FnAng>(vt[Offsets::kCBasePlayer_EyeAnglesVt / 4]);
            if (pos)
            {
                Vector* ret = pos(player, &eye);
                if (ret && ret != &eye)
                    eye = *ret;
            }
            if (angles)
            {
                QAngle* ret = angles(player);
                if (ret)
                    ang = *ret;
            }
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    void* SehFindNearbyVguiScreen(void* fn, const Vector* eye, const QAngle* ang, int team)
    {
        if (!fn || !eye || !ang)
            return nullptr;
        void* screen = nullptr;
        BMVR_TRY
        {
            using Fn = void*(__cdecl*)(const Vector*, const QAngle*, int);
            screen = reinterpret_cast<Fn>(fn)(eye, ang, team);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            screen = nullptr;
        }
        return screen;
    }

    void SehSetVguiScreenButtons(void* fn, void* screen, int buttons)
    {
        if (!screen)
            return;
        if (fn)
        {
            BMVR_TRY
            {
                using Fn = void(__cdecl*)(void*, int);
                reinterpret_cast<Fn>(fn)(screen, buttons);
                return;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        BMVR_TRY
        {
            *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(screen)
                + Offsets::kC_VGuiScreen_ButtonState) = buttons;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    int SehReadIntOff(void* p, int off)
    {
        int v = 0;
        if (!p)
            return 0;
        BMVR_TRY
        {
            v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(p) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            v = 0;
        }
        return v;
    }

    void* SehReadPtrOff(void* p, int off)
    {
        void* v = nullptr;
        if (!p)
            return nullptr;
        BMVR_TRY
        {
            v = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(p) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            v = nullptr;
        }
        return v;
    }

    bool SehIntersectVguiRay(void* fn, void* screen, Ray_t* ray, float* u, float* v)
    {
        if (!fn || !screen || !ray || !u || !v)
            return false;
        bool hit = false;
        BMVR_TRY
        {
            using Fn = bool(__thiscall*)(void*, Ray_t*, float*, float*, float*);
            hit = reinterpret_cast<Fn>(fn)(screen, ray, u, v, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hit = false;
        }
        return hit;
    }

    // modeSite: the signature-resolved g_pClientMode (imm32 holding the global's address), so the global
    // follows game updates; kClientModeGlobalRva only when the signature did not resolve.
    void SehActivateInGameVgui(uintptr_t clientBase, uintptr_t modeSite, void* panel)
    {
        if (!clientBase || !panel)
            return;
        BMVR_TRY
        {
            const uintptr_t global = modeSite ? *reinterpret_cast<const uint32_t*>(modeSite)
                                              : clientBase + Offsets::kClientModeGlobalRva;
            void* mode = *reinterpret_cast<void**>(global);
            void** vt = SehObjectVTable(mode);
            if (!vt)
                return;
            using Fn = void(__thiscall*)(void*, void*);
            Fn fn = reinterpret_cast<Fn>(vt[Offsets::kIClientMode_ActivateInGameVGuiContext]);
            if (fn)
                fn(mode, panel);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void SehVguiCursorMoved(IInput* in, int x, int y)
    {
        void** vt = SehObjectVTable(in);
        if (!vt)
            return;
        BMVR_TRY
        {
            using Fn = void(__thiscall*)(void*, int, int);
            Fn fn = reinterpret_cast<Fn>(vt[Offsets::kVguiVtInternalCursorMoved]);
            if (fn)
                fn(in, x, y);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void SehVguiMouseButton(IInput* in, bool down)
    {
        void** vt = SehObjectVTable(in);
        if (!vt)
            return;
        const int code = Offsets::kVguiMouseLeftCode;
        BMVR_TRY
        {
            using FnState = void(__thiscall*)(void*, int, int);
            using FnBtn = void(__thiscall*)(void*, int);
            FnState set = reinterpret_cast<FnState>(vt[Offsets::kVguiVtSetMouseCodeState]);
            if (set)
                set(in, code, down ? 1 : 0);
            FnBtn btn = reinterpret_cast<FnBtn>(vt[down
                ? Offsets::kVguiVtInternalMousePressed
                : Offsets::kVguiVtInternalMouseReleased]);
            if (btn)
                btn(in, code);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    HWND FindGameHwnd()
    {
        return bmvr::GameWindow();
    }

    void QueueKeyToHwnd(HWND hwnd, int vk)
    {
        if (!hwnd)
            return;
        // Real scan code in lParam: the new game UI (Qt) maps keys by scan
        // code; with 0 it ignored our Escape (2026-11-23).
        const UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC) & 0xFF;
        PostMessageA(hwnd, WM_KEYDOWN, static_cast<WPARAM>(vk), static_cast<LPARAM>(1u | (scan << 16)));
        PostMessageA(hwnd, WM_KEYUP, static_cast<WPARAM>(vk),
            static_cast<LPARAM>(1u | (scan << 16) | (1u << 30) | (1u << 31)));
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

class McMatFunctor : public CFunctor
{
public:
    enum class Kind { Bind, Composite, RetireMsaa, RefractReset };

    McMatFunctor(VR* vr, Kind kind, int stereoEye, uint32_t band, bool flushGpu)
        : McMatFunctor(vr, kind, stereoEye, band, flushGpu, nullptr)
    {
    }

    McMatFunctor(VR* vr, Kind kind, int stereoEye, uint32_t band, bool flushGpu,
        const L4D2VROpenXrPoseDesc* pose, const VR::HandDrawSnap* hands = nullptr)
        : m_vr(vr)
        , m_kind(kind)
        , m_stereoEye(stereoEye)
        , m_band(band)
        , m_flushGpu(flushGpu)
        , m_havePose(pose != nullptr && pose->valid != 0)
    {
        if (m_havePose)
            m_pose = *pose;
        if (hands)
            m_hands = *hands;
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
            m_vr->McCompositeEyesToRing(m_band, m_havePose ? &m_pose : nullptr, m_hands.valid ? &m_hands : nullptr);
        else if (m_kind == Kind::RetireMsaa)
            m_vr->RetireMsaaEyeIfBound();
        else if (m_kind == Kind::RefractReset)
            m_vr->m_RefractEyeDone = nullptr;
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
    VR::HandDrawSnap m_hands{};
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
    BMVR_TRY
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
    BMVR_TRY
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
    BMVR_TRY
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
    BMVR_TRY
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
    Game::logMsg("VR ctor: OpenXR helper path");
    const VrRuntimeBackendConfig runtimeConfig = L4D2VR_ReadRuntimeBackendConfig();
    const VrRuntimeBackendSelection runtimeSelection = L4D2VR_SelectRuntimeBackend(runtimeConfig.requestedBackend);
    m_RequestedRuntimeBackend = runtimeSelection.requested;
    m_RuntimeBackend = runtimeSelection.active;
    m_OpenXrLoaderAvailable = runtimeSelection.openXrLoaderAvailable;
    Game::logMsg(
        "[VR][Runtime] requested=%s active=%s openxrLoader=%d openxrBackendImplemented=%d detail=%s",
        L4D2VR_RuntimeBackendName(runtimeSelection.requested),
        L4D2VR_RuntimeBackendName(runtimeSelection.active),
        runtimeSelection.openXrLoaderAvailable ? 1 : 0,
        runtimeSelection.openXrBackendImplemented ? 1 : 0,
        runtimeSelection.message.c_str());

    m_IsInitialized = InitOpenXR();
    if (!m_IsInitialized)
    {
        // The desktop game keeps running without VR.
        Game::logMsg("[VR][Runtime] OpenXR helper init failed; VR stays off");
        m_OpenXrHelperBridgeActive = false;
    }
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
    // Prefix (stock background01) or substring (Black Mesa Military
    // militarybackground01). Treating that menu map as gameplay armed stereo
    // and mat_queue before the first real level (bmvr_log 2026-09-13).
    if (name == "dedicated" || name.find("background") != std::string::npos)
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
    Hooks::ForgetViewmodelStudioHdr();
    g_EyeRtReset = true;   // RTs may be recreated with the map: drop the per-eye copies (SkyWaterPerEye)
    m_GameplayEligible = IsGameplayMapName(newmap);
    bmvr::NoteEngineMapName(newmap);
    bmvr::SetGameplayWorldRts(m_GameplayEligible);
    m_SeenGameplay = false;
    m_YawSyncTicks = 600;
    m_WelcomeTriedThisMap = false;
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
    if (m_VrPhase != VrPhase::Loading)   // loading panel keeps its place
        m_MenuPanelPoseValid = false;
    m_WeaponMenuPrevEntity = 0;
    m_WeaponMenuPrevKind = 0;
    m_GmanModelIndex[0] = m_GmanModelIndex[1] = -1;
    m_GmanMapState = 0;
    m_GmanMapChecks = 0;
    m_GmanMapCheckMs = 0;
    m_GmanScanMs = 0;
    m_GmanEntityCount = 0;
    m_GmanGestureActive.store(false, std::memory_order_relaxed);
    Game::logMsg("LevelInit map=%s eligible=%d", m_CurrentMapName.c_str(), m_GameplayEligible ? 1 : 0);
    m_DmgMapLoaded = true;
    ScanMapForGman();
    m_PhysicalCrowbarModel = nullptr;
    m_PhysicalCrowbarLoadTries = 0;
    m_CrowbarLastMotionCheckMs = 0;
    m_CrowbarNextAttackMs = 0;
    VectorClear(m_CrowbarPrevTipLocal);
    if (m_GameplayEligible)
    {
        bmvr::EndRisky(L"menu_vr");
        bmvr::KickHudIconPreload();
    }
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

const char* VR::VrPhaseName(VrPhase p)
{
    switch (p)
    {
    case VrPhase::Menu2D: return "menu2d";
    case VrPhase::Loading: return "loading";
    case VrPhase::Gameplay: return "gameplay";
    case VrPhase::Pause: return "pause";
    default: return "unknown";
    }
}

bool VR::EngineDrawingLoadingImage()
{
    if (!m_LoadingFlagProbed)
    {
        m_LoadingFlagProbed = true;
        if (m_Game && m_Game->m_EngineClient)
        {
            BMVR_TRY
            {
                void** vtbl = *reinterpret_cast<void***>(m_Game->m_EngineClient);
                const uint8_t* fn = static_cast<const uint8_t*>(vtbl[29]);
                // IsDrawingLoadingImage: A0 <abs32> C3 = mov al,[scr_drawloading]; ret
                if (fn && fn[0] == 0xA0 && fn[5] == 0xC3)
                    m_LoadingFlagAddr = *reinterpret_cast<const volatile uint8_t* const*>(fn + 1);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                m_LoadingFlagAddr = nullptr;
            }
        }
        Game::logMsg("VR phase: loading flag %s", m_LoadingFlagAddr ? "found (engine slot 29)" : "NOT found; loading phase from LevelInit/IsInGame only");
    }
    if (!m_LoadingFlagAddr)
        return false;
    bool on = false;
    BMVR_TRY
    {
        on = *m_LoadingFlagAddr != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        m_LoadingFlagAddr = nullptr;
    }
    return on;
}

void VR::UpdateVrPhase(bool inGame)
{
    const bool drawingLoading = EngineDrawingLoadingImage();
    // Want2dMenuPanel / WantPauseWorldOverlay depend on the current phase;
    // evaluate the non-loading phases as if no phase were set yet.
    const VrPhase prev = m_VrPhase;
    m_VrPhase = VrPhase::Unknown;
    VrPhase phase;
    if (drawingLoading || (m_GameplayEligible && (!inGame || !m_SeenGameplay) && !PauseUiActive()))
        phase = VrPhase::Loading;
    else if (Want2dMenuPanel())
        phase = VrPhase::Menu2D;
    else if (WantPauseWorldOverlay())
        phase = VrPhase::Pause;
    else if (m_GameplayEligible && inGame)
        phase = VrPhase::Gameplay;
    else
        phase = VrPhase::Menu2D;
    m_VrPhase = prev;
    if (phase == m_VrPhase)
        return;
    const DWORD now = GetTickCount();
    Game::logMsg("VR phase %s -> %s after %lu ms (loadingImage=%d inGame=%d eligible=%d seen=%d gameUi=%d map=%s)",
        VrPhaseName(m_VrPhase), VrPhaseName(phase),
        m_VrPhaseSinceMs ? static_cast<unsigned long>(now - m_VrPhaseSinceMs) : 0ul,
        drawingLoading ? 1 : 0, inGame ? 1 : 0, m_GameplayEligible ? 1 : 0, m_SeenGameplay ? 1 : 0,
        PauseUiActive() ? 1 : 0, m_CurrentMapName.c_str());
    if (m_VrPhase == VrPhase::Loading && m_LoadingCardShown)
    {
        m_LoadingCardShown = false;
        m_LoadingCardForcePublish = false;
        if (std::strcmp(m_ToastText, "LOADING") == 0)
            m_ToastUntilMs = 0;
    }
    if (phase == VrPhase::Loading && prev != VrPhase::Loading)
        ++m_LoadEpoch;
    if (prev == VrPhase::Loading && phase != VrPhase::Loading)
        m_PoseDiagUntilMs = now + 30000;   // pose diagnostic after every load
    m_PauseFromLoading = prev == VrPhase::Loading && phase == VrPhase::Pause;
    m_VrPhase = phase;
    m_VrPhaseSinceMs = now;
    // Test aid (BMVR_PHASE_DUMP=1, headless): dump what the next publish shows
    // after each change, plus the window backbuffer (dump verb, "-bb.bmp").
    static const bool s_phaseDump = [] { char v[8] = {}; return GetEnvironmentVariableA("BMVR_PHASE_DUMP", v, sizeof(v)) > 0 && v[0] == '1'; }();
    if (s_phaseDump)
        m_PhaseDumpAtMs = now + 150;   // after the switch has reached a publish
}

// One line per second for 30 s after a load (2026-11-11: head pose stuck
// after in-game loads in the headset, not reproduced headless). Every stage
// from the helper's HMD pose to the pose published with the eyes.
void VR::TickPoseDiag()
{
    if (!m_PoseDiagUntilMs)
        return;
    const DWORD now = GetTickCount();
    if (static_cast<int32_t>(now - m_PoseDiagUntilMs) > 0)
    {
        m_PoseDiagUntilMs = 0;
        return;
    }
    static DWORD s_last;
    if (now - s_last < 1000)
        return;
    s_last = now;
    L4D2VROpenXrPoseDesc raw{};
    uint32_t rawGen = 0;
    const bool rawOk = L4D2VR_ReadOpenXrHmdPose(raw, &rawGen);
    const uint32_t band = m_McFrame >= 2 ? (m_McFrame - 2) % 3u : 0u;
    const L4D2VROpenXrPoseDesc& mc = m_McPoses[band];
    Game::logMsg("pose diag: raw ok=%d gen=%u (%.4f %.4f %.4f) t=%lld | last gen=%u (%.4f %.4f %.4f) t=%lld latchedGen=%u"
        " | stereo valid=%d (%.4f %.4f %.4f) | mc frame=%u band=%u valid=%d (%.4f %.4f %.4f) | hmdAbs (%.2f %.2f %.2f) yaw %.2f"
        " | waitedTick age=%lu err=%d waited=%d phase=%s card=%d direct=%d",
        rawOk ? 1 : 0, rawGen, raw.position[0], raw.position[1], raw.position[2], static_cast<long long>(raw.displayTime),
        m_OpenXrLastHmdPoseGeneration, m_OpenXrLastHmdPose.position[0], m_OpenXrLastHmdPose.position[1],
        m_OpenXrLastHmdPose.position[2], static_cast<long long>(m_OpenXrLastHmdPose.displayTime), m_OpenXrLatchedPoseGeneration,
        m_OpenXrStereoRenderPoseValid ? 1 : 0, m_OpenXrStereoRenderPose.position[0], m_OpenXrStereoRenderPose.position[1],
        m_OpenXrStereoRenderPose.position[2],
        m_McFrame, band, mc.valid, mc.position[0], mc.position[1], mc.position[2],
        m_HmdPosAbs.x, m_HmdPosAbs.y, m_HmdPosAbs.z, m_HmdAngAbs.y,
        static_cast<unsigned long>(now - m_WaitedPoseTick.load(std::memory_order_acquire)),
        m_LastPoseWaitError.load(std::memory_order_acquire), m_PosesWaitedThisFrame ? 1 : 0,
        VrPhaseName(m_VrPhase), m_LoadingCardShown ? 1 : 0, m_DirectEyeSubmit ? 1 : 0);
}

void VR::TickPhaseDump()
{
    if (!m_PhaseDumpAtMs || static_cast<int32_t>(GetTickCount() - m_PhaseDumpAtMs) < 0)
        return;
    m_PhaseDumpAtMs = 0;
    static int s_phaseDumpN;
    if (FILE* df = std::fopen(bmvr::VrFilePath("dump.txt").c_str(), "wb"))
    {
        std::fprintf(df, "phase%02d-%s 2", ++s_phaseDumpN, VrPhaseName(m_VrPhase));
        std::fclose(df);
    }
    m_PhaseDumpBackbuffer = true;
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
    // The LevelInit / LevelShutdown hooks are skipped on this build: the map
    // poll lands here, so the cached gun header (previous map's model) is
    // dropped here (efb556f put it in the hooks only).
    Hooks::ForgetViewmodelStudioHdr();
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
    // A loading panel stays where it was latched for the whole load.
    if (m_VrPhase != VrPhase::Loading)
        m_MenuPanelPoseValid = false;
    Game::logMsg("LevelShutdown");
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
    BMVR_TRY
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
        BMVR_TRY
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
    m_ActionInventoryGripLeft = handle(L4D2VROpenXrActionId::InventoryGripLeft);
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
    {
        // top = tan(angleDown) < 0, bottom = tan(angleUp) > 0 (see above).
        const float le[4] = { l_left, l_right, l_top, l_bottom };
        const float re[4] = { r_left, r_right, r_top, r_bottom };
        SetupOffCenter(le, re);
    }

    // The engine renders at the eye RT's aspect, so overlays (VrCrosshair,
    // hand boxes) must use it too; tanX/tanY (PSVR2 1.386) put them 1.41x too
    // far off-centre. See the OpenVR init above.
    m_Aspect = (m_RenderWidth >= 640 && m_RenderHeight >= 360)
        ? static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight) : 1.0f;
    m_Fov = 2.0f * atanf(tanHalfFovX) * 180.0f / 3.14159265358979323846f;
    m_FovLeft = m_Fov;
    m_FovRight = m_Fov;
    if (LeftEyeCullPadTan() > 0.f)
        Game::logMsg("LeftEyeCullPad: left eye right edge +%.3f tan (%.0f%% of the half-FOV)",
            LeftEyeCullPadTan(), (std::min)(bmvr::g_LeftEyeCullPad, 0.25f) * 100.f);
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

    // Debug "Freeze head pose": the game renders from the pose of the moment
    // it was switched on. The frame still goes out with that pose and a live
    // display time, so SteamVR reprojects it to the real head (the scene stays
    // put in the room, edges go black on big turns). Controllers keep tracking.
    {
        static bool s_frozen = false;
        static float s_pos[3], s_rot[4];
        if (bmvr::g_FreezeHead && openXrPose.valid)
        {
            if (!s_frozen)
            {
                std::memcpy(s_pos, openXrPose.position, sizeof(s_pos));
                std::memcpy(s_rot, openXrPose.orientation, sizeof(s_rot));
                s_frozen = true;
                Game::logMsg("Freeze head pose: ON at (%.3f %.3f %.3f) rot (%.3f %.3f %.3f %.3f)", s_pos[0], s_pos[1],
                    s_pos[2], s_rot[0], s_rot[1], s_rot[2], s_rot[3]);
            }
            std::memcpy(openXrPose.position, s_pos, sizeof(s_pos));
            std::memcpy(openXrPose.orientation, s_rot, sizeof(s_rot));
        }
        else if (s_frozen && !bmvr::g_FreezeHead)
        {
            s_frozen = false;
            Game::logMsg("Freeze head pose: OFF");
        }
    }

    // Test "head pose hold" (PoseHoldDeg / PoseHoldMm): keep the previous
    // pose until the head turns or moves past the threshold. Render and
    // publish both use the held pose (like Freeze head pose), so SteamVR
    // reprojects the small difference; a still head renders identical frames.
    if ((bmvr::g_PoseHoldDeg > 0.f || bmvr::g_PoseHoldMm > 0.f) && openXrPose.valid && !bmvr::g_FreezeHead)
    {
        static bool s_have = false;
        static float s_pos[3], s_rot[4];
        static uint32_t s_holds, s_moves;
        static DWORD s_logMs;
        bool move = !s_have;
        if (s_have)
        {
            float dot = 0.f;
            for (int i = 0; i < 4; ++i)
                dot += s_rot[i] * openXrPose.orientation[i];
            const float angDeg = 2.f * acosf((std::min)(1.f, fabsf(dot))) * (180.f / 3.14159265f);
            const float dx = openXrPose.position[0] - s_pos[0], dy = openXrPose.position[1] - s_pos[1],
                        dz = openXrPose.position[2] - s_pos[2];
            const float distMm = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.f;
            // A threshold left at 0 while the other is set uses a small
            // default, so tracking noise in that axis cannot break every hold.
            const float degThr = bmvr::g_PoseHoldDeg > 0.f ? bmvr::g_PoseHoldDeg : 0.05f;
            const float mmThr = bmvr::g_PoseHoldMm > 0.f ? bmvr::g_PoseHoldMm : 1.f;
            move = angDeg > degThr || distMm > mmThr;
        }
        if (move)
        {
            std::memcpy(s_pos, openXrPose.position, sizeof(s_pos));
            std::memcpy(s_rot, openXrPose.orientation, sizeof(s_rot));
            s_have = true;
            ++s_moves;
        }
        else
        {
            std::memcpy(openXrPose.position, s_pos, sizeof(s_pos));
            std::memcpy(openXrPose.orientation, s_rot, sizeof(s_rot));
            ++s_holds;
        }
        const DWORD nowMs = GetTickCount();
        if (nowMs - s_logMs >= 10000)
        {
            s_logMs = nowMs;
            Game::logMsg("Head pose hold %.2f deg / %.1f mm: %u held, %u moved (last 10 s)",
                bmvr::g_PoseHoldDeg, bmvr::g_PoseHoldMm, s_holds, s_moves);
            s_holds = s_moves = 0;
        }
    }

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
        double hmdMs = 0.0;
        uint32_t hmdGeneration = 0;
        float hmd[3]{};
        float velHmd[3]{};

        bool haveLeft = false;
        bool haveRight = false;
        double handMs = 0.0;
        uint32_t inputGeneration = 0;
        float left[3]{};
        float right[3]{};
        float velLeft[3]{};
        float velRight[3]{};
    } s_openXrVel;
    // QpcNowMs caches QueryPerformanceFrequency. This runs inside the
    // compositor-pose wait, so a fresh QPF syscall per sample is wasted.
    const double nowMs = QpcNowMs();
    auto elapsedSeconds = [&nowMs](double thenMs) -> double {
        if (thenMs <= 0.0)
            return 0.0;
        return (nowMs - thenMs) / 1000.0;
    };

    if (generation != s_openXrVel.hmdGeneration)
    {
        if (s_openXrVel.haveHmd)
            DiffLinearVelocity(openXrPose.position, s_openXrVel.hmd,
                elapsedSeconds(s_openXrVel.hmdMs), s_openXrVel.velHmd);
        if (openXrPose.valid)
        {
            s_openXrVel.hmd[0] = openXrPose.position[0];
            s_openXrVel.hmd[1] = openXrPose.position[1];
            s_openXrVel.hmd[2] = openXrPose.position[2];
            s_openXrVel.haveHmd = true;
            s_openXrVel.hmdMs = nowMs;
        }
        s_openXrVel.hmdGeneration = generation;
    }

    if (m_OpenXrLastInputStateGeneration != s_openXrVel.inputGeneration)
    {
        const double dt = elapsedSeconds(s_openXrVel.handMs);
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
        s_openXrVel.handMs = nowMs;
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

bool VR::AwaitOpenXrCompositorFrame()
{
    // HL2VR Mat_AwaitFrame / WaitGetPoses: block until this compositor
    // pose exists, then render that pose. Do not wait for slot consume
    // (HMD 2026-09-20: that plus a 9 ms helper poll made EndFrame late).
    // Uncapped stereo (150-190 pairs/s of 3168x3088) saturated the GPU so
    // the helper blit fence missed vsync and helperHz fell to ~45.
    if (!m_OpenXrHelperBridgeActive)
        return false;

    const double t0 = QpcNowMs();
    ConsumeOpenXrTracking();
    const uint32_t prevGen = m_OpenXrLatchedPoseGeneration;
    const int64_t prevTime = m_OpenXrLatchedDisplayTime;
    bool gotNew = m_OpenXrLastHmdPoseGeneration != prevGen
        || m_OpenXrLastHmdPose.displayTime != prevTime;
    while (!gotNew && (QpcNowMs() - t0) < 22.0)
    {
        if (ConsumeOpenXrTracking()
            && (m_OpenXrLastHmdPoseGeneration != prevGen
                || m_OpenXrLastHmdPose.displayTime != prevTime))
        {
            gotNew = true;
            break;
        }
        SwitchToThread();
    }
    if (!gotNew)
        ConsumeOpenXrTracking();

    m_OpenXrLatchedPoseGeneration = m_OpenXrLastHmdPoseGeneration;
    m_OpenXrLatchedDisplayTime = m_OpenXrLastHmdPose.displayTime;
    const double waited = QpcNowMs() - t0;
    ++m_OpenXrAwaitFrames;
    m_OpenXrAwaitPoseMs += waited;
    if (waited > m_OpenXrAwaitPoseMaxMs)
        m_OpenXrAwaitPoseMaxMs = waited;
    if (!gotNew)
        ++m_OpenXrSkipStereoNoPose;
    return gotNew;
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
            // The eye was rendered at m_Fov (NormalizeViewSetupForVREye). With 0
            // the helper fell back to 90 deg and showed a 123 deg PSVR2 render
            // as 90 deg (world ~0.54x). Aspect 0 = helper uses the RT's w/h.
            shared.renderFovXDeg = (std::isfinite(m_Fov) && m_Fov > 1.0f && m_Fov < 179.0f) ? m_Fov : 0.0f;
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
    // Stereo CViewSetup uses WorldRenderFov() (narrow while scoped). Submit
    // must keep the real HMD FOV so that narrower render fills the lenses.
    // Publishing WorldRenderFov here made the helper build a ~27° projection
    // on a ~90° HMD: a small square of world on black, frozen until unscope.
    if (!panel2d)
    {
        const float hmdFov = m_Fov;
        if (hmdFov > 10.f && hmdFov < 179.f)
        {
            publishLeft.renderFovXDeg = hmdFov;
            publishRight.renderFovXDeg = hmdFov;
        }
        // An off-centre eye (OffCenterEyes, the left eye widened by
        // LeftEyeCullPad) hands the helper the exact rectangle it was drawn
        // with (unscoped, like renderFovXDeg); the helper submits that FOV
        // uncropped. Symmetric eyes keep the renderFovXDeg path.
        auto setExplicitFov = [&](L4D2VROpenXrSharedTextureDesc& desc, int stereoEye) {
            float rect[4];
            const float imageAspect = desc.height
                ? static_cast<float>(desc.width) / static_cast<float>(desc.height) : 0.f;
            if (EyeRenderIsCustom(stereoEye) && EyeRenderRect(stereoEye, rect, imageAspect))
            {
                desc.tanLeft = rect[0];
                desc.tanRight = rect[1];
                desc.tanDown = rect[2];
                desc.tanUp = rect[3];
            }
        };
        setExplicitFov(publishLeft, 1);
        setExplicitFov(publishRight, 2);
    }
    // 2D GameUI is a letterboxed blit, not a 90° world frustum. A non-zero
    // renderFovXDeg made the helper use "game-when-full-bounds" FOV on WMR.
    // panel2d is the state at copy time: a deferred slot may be published
    // after the menu opened or closed.
    if (panel2d)
    {
        publishLeft.renderFovXDeg = 0.f;
        publishRight.renderFovXDeg = 0.f;
        publishLeft.tanLeft = publishLeft.tanRight = publishLeft.tanDown = publishLeft.tanUp = 0.f;
        publishRight.tanLeft = publishRight.tanRight = publishRight.tanDown = publishRight.tanUp = 0.f;
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
    {
        // The helper draws the 2D menu quad where the pointer ray expects it
        // (IntersectMenuPanelUv uses the same metrics); only sent on change.
        L4D2VROpenXrMenuQuadDesc menu{};
        GetMenuOverlayMetrics(menu.distanceMeters, menu.widthMeters, menu.offsetYMeters);
        menu.contentScale = bmvr::g_MenuPanelScale;
        L4D2VR_PublishOpenXrMenuQuad(menu);
    }
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
    BMVR_PROF("wait GPU for oldest published frame (paceWait)");
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
    bmvr::sampler::NoteRenderPaceWait(waited);
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

namespace
{
    float HalfToFloat(uint16_t h)
    {
        const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        float v = (e == 0) ? std::ldexp(static_cast<float>(m), -24)
            : (e == 31) ? 65504.f : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
        return s ? -v : v;
    }

    // Read back a render-target surface and write its rows unchanged (no
    // header; the caller's file name carries width, height and format).
    bool DumpSurfaceRaw(IDirect3DDevice9* dev, IDirect3DSurface9* src, const std::string& path)
    {
        D3DSURFACE_DESC d{};
        if (!dev || !src || FAILED(src->GetDesc(&d)))
            return false;
        UINT bpp = 0;
        switch (d.Format)
        {
        case D3DFMT_R16F: bpp = 2; break;
        case D3DFMT_G16R16F: case D3DFMT_R32F: bpp = 4; break;
        case D3DFMT_G32R32F: bpp = 8; break;
        case D3DFMT_A32B32G32R32F: bpp = 16; break;
        default: return false;
        }
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys)
            return false;
        bool ok = false;
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(dev->GetRenderTargetData(src, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
        {
            if (FILE* f = std::fopen(path.c_str(), "wb"))
            {
                for (UINT y = 0; y < d.Height; ++y)
                    std::fwrite(static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, 1,
                        static_cast<size_t>(d.Width) * bpp, f);
                std::fclose(f);
                ok = true;
            }
            sys->UnlockRect();
        }
        sys->Release();
        Game::logMsg("debug dump %s: %s (%ux%u fmt=%u raw)", path.c_str(), ok ? "written" : "failed", d.Width, d.Height, d.Format);
        return ok;
    }

    // Read back a render-target surface, every `step`th pixel, as top-down
    // 24-bit RGB. 8888, 10-bit and 16F formats; others are only logged.
    // alphaOnly: the alpha channel as grey (layers blended by alpha).
    bool ReadSurfaceRgb(IDirect3DDevice9* dev, IDirect3DSurface9* src, const char* what, int step, bool alphaOnly,
        int& w, int& h, std::vector<uint8_t>& rgb, D3DSURFACE_DESC* descOut = nullptr)
    {
        w = h = 0;
        if (!dev || !src)
            return false;
        D3DSURFACE_DESC d{};
        if (FAILED(src->GetDesc(&d)))
            return false;
        if (descOut)
            *descOut = d;
        const bool f16 = d.Format == D3DFMT_A16B16G16R16F;
        const bool bgra = d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8;
        const bool rgba = d.Format == D3DFMT_A8B8G8R8 || d.Format == D3DFMT_X8B8G8R8;
        const bool a2r10 = d.Format == D3DFMT_A2R10G10B10;
        if (!f16 && !bgra && !rgba && !a2r10)
        {
            Game::logMsg("%s: format %u not handled (%ux%u)", what, d.Format, d.Width, d.Height);
            return false;
        }
        IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys)
            return false;
        bool ok = false;
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(dev->GetRenderTargetData(src, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
        {
            w = static_cast<int>(d.Width) / step;
            h = static_cast<int>(d.Height) / step;
            rgb.resize(static_cast<size_t>(w) * h * 3);
            for (int y = 0; y < h; ++y)
            {
                const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y * step) * lr.Pitch;
                uint8_t* out = &rgb[static_cast<size_t>(y) * w * 3];
                for (int x = 0; x < w; ++x)
                {
                    uint8_t r, g, b;
                    if (f16)
                    {
                        const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(x * step) * 4;
                        auto c = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
                        r = c(HalfToFloat(p[0])); g = c(HalfToFloat(p[1])); b = c(HalfToFloat(p[2]));
                        if (alphaOnly)
                            r = g = b = c(HalfToFloat(p[3]));
                    }
                    else if (a2r10)
                    {
                        const uint32_t v = *reinterpret_cast<const uint32_t*>(row + static_cast<size_t>(x * step) * 4);
                        b = static_cast<uint8_t>((v & 0x3FF) >> 2);
                        g = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
                        r = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
                        if (alphaOnly)
                            r = g = b = static_cast<uint8_t>((v >> 30) * 85);
                    }
                    else
                    {
                        const uint8_t* p = row + static_cast<size_t>(x * step) * 4;
                        if (bgra) { b = p[0]; g = p[1]; r = p[2]; }
                        else { r = p[0]; g = p[1]; b = p[2]; }
                        if (alphaOnly)
                            r = g = b = p[3];
                    }
                    out[x * 3 + 0] = r; out[x * 3 + 1] = g; out[x * 3 + 2] = b;
                }
            }
            sys->UnlockRect();
            ok = true;
        }
        sys->Release();
        return ok;
    }

    // Top-down RGB -> 24-bit BMP.
    bool WriteBmp(const std::string& path, int w, int h, const uint8_t* rgb)
    {
        const int rowBytes = (w * 3 + 3) & ~3;
        std::vector<uint8_t> img(static_cast<size_t>(rowBytes) * h);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* in = rgb + static_cast<size_t>(y) * w * 3;
            uint8_t* out = &img[static_cast<size_t>(h - 1 - y) * rowBytes];
            for (int x = 0; x < w; ++x)
            {
                out[x * 3 + 0] = in[x * 3 + 2];
                out[x * 3 + 1] = in[x * 3 + 1];
                out[x * 3 + 2] = in[x * 3 + 0];
            }
        }
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const uint32_t dataSize = static_cast<uint32_t>(img.size());
        uint8_t hdr[54]{};
        hdr[0] = 'B'; hdr[1] = 'M';
        *reinterpret_cast<uint32_t*>(hdr + 2) = 54 + dataSize;
        *reinterpret_cast<uint32_t*>(hdr + 10) = 54;
        *reinterpret_cast<uint32_t*>(hdr + 14) = 40;
        *reinterpret_cast<int32_t*>(hdr + 18) = w;
        *reinterpret_cast<int32_t*>(hdr + 22) = h;
        *reinterpret_cast<uint16_t*>(hdr + 26) = 1;
        *reinterpret_cast<uint16_t*>(hdr + 28) = 24;
        *reinterpret_cast<uint32_t*>(hdr + 34) = dataSize;
        std::fwrite(hdr, 1, sizeof(hdr), f);
        std::fwrite(img.data(), 1, img.size(), f);
        std::fclose(f);
        return true;
    }

    // Read back a render-target surface and write every `step`th pixel as a
    // 24-bit BMP. alphaOnly: write the alpha channel as grey.
    bool DumpSurfaceBmp(IDirect3DDevice9* dev, IDirect3DSurface9* src, const std::string& path, int step,
        bool alphaOnly = false)
    {
        int w = 0, h = 0;
        std::vector<uint8_t> rgb;
        D3DSURFACE_DESC d{};
        const std::string what = "debug dump " + path;
        const bool ok = ReadSurfaceRgb(dev, src, what.c_str(), step, alphaOnly, w, h, rgb, &d)
            && WriteBmp(path, w, h, rgb.data());
        Game::logMsg("debug dump %s: %s (%ux%u fmt=%u step=%d)", path.c_str(), ok ? "written" : "failed",
            d.Width, d.Height, d.Format, step);
        return ok;
    }

    // Steam screenshot library (ISteamScreenshots::WriteScreenshot through the
    // game's own steam_api.dll flat exports). nullptr = not available.
    using SteamScreenshotsFn = void* (__cdecl*)();
    using WriteScreenshotFn = uint32_t(__cdecl*)(void* self, void* rgb, uint32_t bytes, int w, int h);
    bool SteamWriteScreenshot(const std::vector<uint8_t>& rgb, int w, int h, const char*& why)
    {
        HMODULE api = GetModuleHandleA("steam_api.dll");
        if (!api)
        {
            why = "steam_api.dll not loaded";
            return false;
        }
        auto get = reinterpret_cast<SteamScreenshotsFn>(GetProcAddress(api, "SteamAPI_SteamScreenshots_v003"));
        auto write = reinterpret_cast<WriteScreenshotFn>(GetProcAddress(api, "SteamAPI_ISteamScreenshots_WriteScreenshot"));
        if (!get || !write)
        {
            why = "no ISteamScreenshots v003 exports";
            return false;
        }
        void* screenshots = get();
        if (!screenshots)
        {
            why = "ISteamScreenshots unavailable";
            return false;
        }
        const uint32_t handle = write(screenshots, const_cast<uint8_t*>(rgb.data()), static_cast<uint32_t>(rgb.size()), w, h);
        if (handle == 0)   // INVALID_SCREENSHOT_HANDLE
        {
            why = "WriteScreenshot failed";
            return false;
        }
        return true;
    }
}

namespace
{
    // Shared textures/surfaces on the VR (game) device, for `dump` (new game
    // UI study, 2026-11-23). Each keeps a reference; at most 16.
    struct SharedResEntry
    {
        IDirect3DSurface9* surface;
        IDirect3DTexture9* texture;   // not referenced separately (surface holds it)
        int index;
        uint64_t lastBind;            // g_SharedBindSeq at its last SetTexture
        uint32_t binds;               // since the last bind summary
    };
    std::mutex g_SharedResMutex;
    std::vector<SharedResEntry> g_SharedRes;
    int g_SharedResCount = 0;
    uint64_t g_SharedBindSeq = 0;
    void* g_NewUiHwnd = nullptr;
}

void bmvr::SetNewUiWindow(void* hwnd)
{
    g_NewUiHwnd = hwnd;
}

void* bmvr::NewUiWindow()
{
    return g_NewUiHwnd;
}

void bmvr::NoteSharedTextureBound(void* device, void* texture)
{
    std::lock_guard<std::mutex> lock(g_SharedResMutex);
    for (SharedResEntry& e : g_SharedRes)
    {
        if (e.texture == texture)
        {
            e.lastBind = ++g_SharedBindSeq;
            ++e.binds;
            return;
        }
    }
    (void)device;
}

// New game UI: the shared texture the game device bound last = the top layer
// it composites (the QML UI, alpha, vertically flipped). nullptr: none.
IDirect3DTexture9* VR::NewUiLayerTexture()
{
    std::lock_guard<std::mutex> lock(g_SharedResMutex);
    const SharedResEntry* best = nullptr;
    for (const SharedResEntry& e : g_SharedRes)
    {
        if (e.texture && e.lastBind && (!best || e.lastBind > best->lastBind))
            best = &e;
    }
    // Which layers the game draws, while it matters (12 lines).
    static int s_log;
    static ULONGLONG s_lastLog;
    const ULONGLONG now = GetTickCount64();
    if (s_log < 12 && now - s_lastLog > 2000)
    {
        s_lastLog = now;
        ++s_log;
        char line[256];
        int len = std::snprintf(line, sizeof(line), "New UI layer binds since last:");
        for (SharedResEntry& e : g_SharedRes)
        {
            if (len > 0 && len < static_cast<int>(sizeof(line)) - 40)
                len += std::snprintf(line + len, sizeof(line) - len, " #%d=%u", e.index, e.binds);
            e.binds = 0;
        }
        Game::logMsg("%s -> top #%d", line, best ? best->index : -1);
    }
    return best ? best->texture : nullptr;
}

// Copy the QML layer (flipped, cropped to the window size, alpha kept) onto
// the pause overlay. Its own state block: callers come from the VGUI paint
// path with the game's state bound.
bool VR::DrawNewUiLayerTo(IDirect3DDevice9* device, IDirect3DSurface9* dst)
{
    IDirect3DTexture9* tex = NewUiLayerTexture();
    if (!device || !dst || !tex)
        return false;
    D3DSURFACE_DESC td{}, dd{};
    if (FAILED(tex->GetLevelDesc(0, &td)) || FAILED(dst->GetDesc(&dd)) || td.Width < 64 || td.Height < 64)
        return false;
    // The UI is window-sized in the texture's bottom rows, upside down (GL
    // origin): its top row is the texture's last row.
    UINT uiW = KnownWindowWidth(), uiH = KnownWindowHeight();
    if (uiW < 64 || uiH < 64 || uiW > td.Width || uiH > td.Height)
    {
        uiW = dd.Width <= td.Width ? dd.Width : td.Width;
        uiH = dd.Height <= td.Height ? dd.Height : td.Height;
    }
    const float u1 = static_cast<float>(uiW) / static_cast<float>(td.Width);
    const float vTop = 1.f;
    const float vBottom = static_cast<float>(td.Height - uiH) / static_cast<float>(td.Height);
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved)
        return false;
    IDirect3DSurface9* prevRt = nullptr;
    IDirect3DSurface9* prevDs = nullptr;
    device->GetRenderTarget(0, &prevRt);
    device->GetDepthStencilSurface(&prevDs);
    bool ok = false;
    if (SUCCEEDED(device->SetRenderTarget(0, dst)))
    {
        device->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp{ 0, 0, dd.Width, dd.Height, 0.f, 1.f };
        device->SetViewport(&vp);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetTexture(0, tex);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        struct V { float x, y, z, rhw, u, v; };
        const float w = static_cast<float>(dd.Width) - 0.5f, h = static_cast<float>(dd.Height) - 0.5f;
        const V quad[4] = {
            { -0.5f, -0.5f, 0.f, 1.f, 0.f, vTop },
            { w, -0.5f, 0.f, 1.f, u1, vTop },
            { -0.5f, h, 0.f, 1.f, 0.f, vBottom },
            { w, h, 0.f, 1.f, u1, vBottom },
        };
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)));
        device->SetTexture(0, nullptr);
    }
    saved->Apply();
    saved->Release();
    if (prevRt)
    {
        device->SetRenderTarget(0, prevRt);
        prevRt->Release();
    }
    device->SetDepthStencilSurface(prevDs);
    if (prevDs)
        prevDs->Release();
    static int s_log;
    if (s_log < 6)
    {
        ++s_log;
        Game::logMsg("New UI layer -> pause overlay %ux%u from %ux%u (ui %ux%u, flipped) ok=%d",
            dd.Width, dd.Height, td.Width, td.Height, uiW, uiH, ok ? 1 : 0);
    }
    return ok;
}

void bmvr::NoteSharedResource(void* device, IDirect3DTexture9* texture, IDirect3DSurface9* surface, const char* kind,
    bool opened, void* handle)
{
    IDirect3DDevice9* vrDev = nullptr;
    if (g_D3DVR9)
        g_D3DVR9->GetD3DDevice(&vrDev);
    const bool onVr = vrDev && vrDev == device;
    if (vrDev)
        vrDev->Release();
    D3DSURFACE_DESC d{};
    if (surface)
        surface->GetDesc(&d);
    std::lock_guard<std::mutex> lock(g_SharedResMutex);
    const int index = g_SharedResCount++;
    if (index < 64)
        Game::logMsg("Shared %s #%d %s on %s device %p: tex=%p surf=%p %ux%u fmt=%u usage=0x%X pool=%u handle=%p",
            kind, index, opened ? "OPENED" : "created", onVr ? "VR(game)" : "other", device,
            static_cast<void*>(texture), static_cast<void*>(surface), d.Width, d.Height,
            static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Usage), static_cast<unsigned>(d.Pool), handle);
    if (onVr && surface && g_SharedRes.size() < 16)
    {
        surface->AddRef();
        g_SharedRes.push_back({ surface, texture, index, 0, 0 });
    }
}

// Grab gesture "Screenshot" (user 2026-11-23): the right eye with the
// desktop window's framing (MirrorStereoToDesktopWindow: the whole eye
// width, top and bottom cropped to 16:9, centred), at full eye resolution,
// into Steam's screenshot library like F12 (Steam shows its own notice).
// An earlier crop limited to what the right eye shows (+-43 deg on PSVR2)
// looked far too zoomed in (user). Fallback: a BMP in VR/. Headless test
// runs (BMVR_TESTDIR) never touch the library: VR/steamshot-test.bmp.
void VR::TakeSteamScreenshot(IDirect3DDevice9* dev, uint32_t slot)
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (slot >= kOpenXrPublishSlots
        || !ReadSurfaceRgb(dev, m_D9OpenXrPublishSurface[1][slot], "steam screenshot", 1, false, w, h, rgb))
    {
        Game::logMsg("Steam screenshot: no right eye to read (slot %u)", slot);
        ShowToast("Screenshot failed", 1500);
        return;
    }
    int cw = w & ~1;
    int ch = static_cast<int>(w * 9.f / 16.f) & ~1;
    if (ch > h)
    {
        ch = h & ~1;
        cw = static_cast<int>(h * 16.f / 9.f) & ~1;
    }
    const int x0 = (w - cw) / 2, y0 = (h - ch) / 2;
    std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch * 3);
    for (int y = 0; y < ch; ++y)
        std::memcpy(&crop[static_cast<size_t>(y) * cw * 3], &rgb[(static_cast<size_t>(y0 + y) * w + x0) * 3],
            static_cast<size_t>(cw) * 3);
    if (!bmvr::TestDir().empty())
    {
        const bool ok = WriteBmp(bmvr::VrFilePath("steamshot-test.bmp"), cw, ch, crop.data());
        Game::logMsg("Steam screenshot (test run, not sent): right eye %dx%d crop %dx%d at %d,%d -> VR/steamshot-test.bmp %s",
            w, h, cw, ch, x0, y0, ok ? "written" : "FAILED");
        ShowToast("Screenshot saved", 1500);
        return;
    }
    const char* why = "";
    if (SteamWriteScreenshot(crop, cw, ch, why))
    {
        Game::logMsg("Steam screenshot: right eye %dx%d crop %dx%d sent to the Steam library", w, h, cw, ch);
        return;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char name[64];
    std::snprintf(name, sizeof(name), "screenshot-%04u%02u%02u-%02u%02u%02u.bmp", st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    const bool ok = WriteBmp(bmvr::VrFilePath(name), cw, ch, crop.data());
    Game::logMsg("Steam screenshot: %s; saved VR/%s instead (%s)", why, name, ok ? "ok" : "FAILED");
    ShowToast(ok ? "Screenshot saved (VR folder)" : "Screenshot failed", 1500);
}

void VR::MaybeDebugDumpPublished(uint32_t slot, const L4D2VROpenXrPoseDesc* pose)
{
    // Test aid: the trigger file is polled on the publish path, so players'
    // builds skip it (headless tests set BMVR_TESTDIR; DebugDumps=true opts in).
    // The panel's "Capture debug frame" works in every build: it writes the
    // same trigger files a test would, then this keeps polling until they
    // are consumed.
    if (m_PauseCapEyes.load() > 0)
    {
        const int left = m_PauseCapEyes.fetch_sub(1) - 1;
        IDirect3DDevice9* dev = nullptr;
        if (slot < kOpenXrPublishSlots && g_D3DVR9 && SUCCEEDED(g_D3DVR9->GetD3DDevice(&dev)) && dev)
        {
            char nm[96];
            std::snprintf(nm, sizeof(nm), "pausecap-%05lu-eyes-%02d",
                static_cast<unsigned long>(GetTickCount() - m_PauseCapStartMs), 11 - left);
            const std::string base = bmvr::VrFilePath(nm);
            DumpSurfaceBmp(dev, m_D9OpenXrPublishSurface[0][slot], base + "-left.bmp", 3);
            DumpSurfaceBmp(dev, m_D9OpenXrPublishSurface[1][slot], base + "-right.bmp", 3);
            dev->Release();
            Game::logMsg("Pause capture: %s (phase %d, menu up %d)", nm, static_cast<int>(m_VrPhase), IsMenuUp() ? 1 : 0);
        }
        return;
    }
    if (bmvr::g_SteamScreenshotAt && static_cast<LONG>(GetTickCount() - bmvr::g_SteamScreenshotAt) >= 0)
    {
        bmvr::g_SteamScreenshotAt = 0;
        IDirect3DDevice9* dev = nullptr;
        if (g_D3DVR9 && SUCCEEDED(g_D3DVR9->GetD3DDevice(&dev)) && dev)
        {
            TakeSteamScreenshot(dev, slot);
            dev->Release();
        }
    }
    static int s_capturePolls = 0;
    if (bmvr::TestDir().empty() && !bmvr::g_FakeRuntime && !bmvr::g_DebugDumps
        && !bmvr::g_DebugCaptureAt && !bmvr::g_ScreenshotAt && s_capturePolls <= 0)
        return;
    static DWORD s_lastCheck = 0;
    const DWORD now = GetTickCount();
    if (now - s_lastCheck < 250 && !m_PhaseDumpBackbuffer)
        return;
    s_lastCheck = now;
    if (s_capturePolls > 0)
        --s_capturePolls;
    // VR/capture.txt: the panel's "Capture debug frame", for headless tests.
    const std::string captureTrigger = bmvr::VrFilePath("capture.txt");
    if (FILE* cf = std::fopen(captureTrigger.c_str(), "rb"))
    {
        std::fclose(cf);
        std::remove(captureTrigger.c_str());
        bmvr::g_DebugCaptureAt = now ? now : 1;
        Game::logMsg("debug capture: triggered by VR/capture.txt");
    }
    if (bmvr::g_DebugCaptureAt && static_cast<LONG>(now - bmvr::g_DebugCaptureAt) >= 0)
    {
        bmvr::g_DebugCaptureAt = 0;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char name[64];
        std::snprintf(name, sizeof(name), "capture-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
        // Constants + sampled textures of the water and fog draws.
        SetEnvironmentVariableA("BMVR_AUDIT_CONSTS", "liquids/|water|__gbxog");
        SetEnvironmentVariableA("BMVR_AUDIT_TEXPREFIX", name);
        if (FILE* af = std::fopen(bmvr::VrFilePath("audit.txt").c_str(), "wb"))
        {
            std::fputs("1", af);
            std::fclose(af);
        }
        if (FILE* df = std::fopen(bmvr::VrFilePath("dump.txt").c_str(), "wb"))
        {
            std::fprintf(df, "%s 1", name);
            std::fclose(df);
        }
        Game::logMsg("debug capture %s: eyes + audit of one frame, files VR/%s*", name, name);
        s_capturePolls = 40;
        return;
    }
    if (bmvr::g_ScreenshotAt && static_cast<LONG>(now - bmvr::g_ScreenshotAt) >= 0)
    {
        bmvr::g_ScreenshotAt = 0;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char name[64];
        std::snprintf(name, sizeof(name), "screenshot-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
        if (FILE* df = std::fopen(bmvr::VrFilePath("dump.txt").c_str(), "wb"))
        {
            std::fprintf(df, "%s 3", name);
            std::fclose(df);
        }
        Game::logMsg("screenshot %s: both eyes at 1/3 size, VR/%s-left/right.bmp", name, name);
        s_capturePolls = 40;
        return;
    }
    // VR/click.txt "x y" (test aid): a left click at those game-window
    // coordinates, as the laser and the new UI's Back do it.
    {
        const std::string clickTrigger = bmvr::VrFilePath("click.txt");
        if (FILE* cf = std::fopen(clickTrigger.c_str(), "rb"))
        {
            int x = -1, y = -1;
            const int got = std::fscanf(cf, "%d %d", &x, &y);
            std::fclose(cf);
            std::remove(clickTrigger.c_str());
            if (got == 2)
                StartSyntheticMenuClick(x, y);
        }
        TickSyntheticMenuClick();
    }
    // VR/trace.txt [frames]: log every RT push/pop, copy, viewport and
    // screen-space draw for that many frames (default 2).
    const std::string traceTrigger = bmvr::VrFilePath("trace.txt");
    if (FILE* tf = std::fopen(traceTrigger.c_str(), "rb"))
    {
        int frames = 2;
        if (std::fscanf(tf, "%d", &frames) < 1)
            frames = 2;
        std::fclose(tf);
        std::remove(traceTrigger.c_str());
        bmvr::g_PassTraceFrames = std::clamp(frames, 1, 8) + 1;
        Game::logMsg("trace: %d frames", bmvr::g_PassTraceFrames - 1);
    }
    // VR/audit.txt [frames]: render-target audit (sizes, viewports, which eye
    // wrote each sampled RT) for that many Presents, with the pass trace on too
    // so RT names line up with the audit lines.
    const std::string auditTrigger = bmvr::VrFilePath("audit.txt");
    if (FILE* af = std::fopen(auditTrigger.c_str(), "rb"))
    {
        int frames = 2;
        if (std::fscanf(af, "%d", &frames) < 1)
            frames = 2;
        std::fclose(af);
        std::remove(auditTrigger.c_str());
        frames = std::clamp(frames, 1, 8);
        bmvr::g_PassTraceFrames = frames + 1;
        AuditStart(frames);
    }
    const std::string trigger = bmvr::VrFilePath("dump.txt");
    FILE* f = std::fopen(trigger.c_str(), "rb");
    if (!f)
        return;
    char name[64] = "dump";
    int step = 4;
    if (std::fscanf(f, "%63s %d", name, &step) < 1)
        std::strcpy(name, "dump");
    step = std::clamp(step, 1, 8);
    std::fclose(f);
    std::remove(trigger.c_str());
    IDirect3DDevice9* dev = nullptr;
    if (!g_D3DVR9 || FAILED(g_D3DVR9->GetD3DDevice(&dev)) || !dev)
        return;
    const std::string base = bmvr::VrFilePath(name);
    // Pose of the pair being dumped, so a headless run can be put there.
    Game::logMsg("debug dump %s: setup origin %.2f %.2f %.2f, hmd abs %.2f %.2f %.2f ang %.2f %.2f %.2f, "
        "stereo frame hmd %.2f %.2f %.2f ang %.2f %.2f %.2f",
        name, m_SetupOrigin.x, m_SetupOrigin.y, m_SetupOrigin.z,
        m_HmdPosAbs.x, m_HmdPosAbs.y, m_HmdPosAbs.z, m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z,
        m_StereoFrameHmdPosAbs.x, m_StereoFrameHmdPosAbs.y, m_StereoFrameHmdPosAbs.z,
        m_StereoFrameAngles.x, m_StereoFrameAngles.y, m_StereoFrameAngles.z);
    if (pose && pose->valid)
        Game::logMsg("debug dump %s: tracking pos %.4f %.4f %.4f quat %.4f %.4f %.4f %.4f", name,
            pose->position[0], pose->position[1], pose->position[2],
            pose->orientation[0], pose->orientation[1], pose->orientation[2], pose->orientation[3]);
    if (std::strncmp(name, "capture-", 8) == 0)
    {
        // Debug capture: everything a headless run needs to show this exact
        // view again, from the frame dumped below.
        WriteCaptureState(name, pose);
        std::lock_guard<std::mutex> lock(m_CaptureMutex);
        m_CaptureEngineName = name;   // game thread: engine view, settings, save
    }
    if (slot < kOpenXrPublishSlots)
    {
        DumpSurfaceBmp(dev, m_D9OpenXrPublishSurface[0][slot], base + "-left.bmp", step);
        DumpSurfaceBmp(dev, m_D9OpenXrPublishSurface[1][slot], base + "-right.bmp", step);
    }
    else
        Game::logMsg("debug dump: no publish slot (%u)", slot);
    // HUD layer at the dump step capped at 2; step 1 keeps every row (the
    // title layer's scanlines are on odd rows, a step-2 dump drops them all).
    const int hudStep = step < 2 ? step : 2;
    DumpSurfaceBmp(dev, m_D9HUDPublishSurface, base + "-hud.bmp", hudStep);
    DumpSurfaceBmp(dev, m_D9HUDPublishSurface, base + "-hud-alpha.bmp", hudStep, true);
    {
        // Shared textures on the game device (new game UI: the Qt layer).
        std::lock_guard<std::mutex> lock(g_SharedResMutex);
        for (const SharedResEntry& e : g_SharedRes)
        {
            char suffix[48];
            std::snprintf(suffix, sizeof(suffix), "-shared%d.bmp", e.index);
            DumpSurfaceBmp(dev, e.surface, base + suffix, 1);
            std::snprintf(suffix, sizeof(suffix), "-shared%d-alpha.bmp", e.index);
            DumpSurfaceBmp(dev, e.surface, base + suffix, 1, true);
        }
    }
    // Panel "Screenshot": a short note once the eyes are on disk (user).
    if (std::strncmp(name, "screenshot-", 11) == 0)
        ShowToast("Screenshot saved", 1500);
    if (m_PhaseDumpBackbuffer)
    {
        m_PhaseDumpBackbuffer = false;
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
        {
            DumpSurfaceBmp(dev, bb, base + "-bb.bmp", 2);
            bb->Release();
        }
    }
    dev->Release();
}

namespace
{
    void FmtPose(std::string& out, const char* key, const float pos[3], const float q[4], bool valid)
    {
        char b[200];
        std::snprintf(b, sizeof(b), "%s %.6f %.6f %.6f %.7f %.7f %.7f %.7f%s\n", key, pos[0], pos[1], pos[2],
            q[0], q[1], q[2], q[3], valid ? "" : " # INVALID at capture");
        out += b;
    }
    void FmtVec(std::string& out, const char* key, const Vector& v)
    {
        char b[160];
        std::snprintf(b, sizeof(b), "%s %.4f %.4f %.4f\n", key, v.x, v.y, v.z);
        out += b;
    }
    bool WriteTextFile(const std::string& path, const std::string& text)
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        return true;
    }
}

// Render thread, at the capture: the raw OpenXR poses (exactly what the runtime
// gave; the fake runtime replays them with "raw" lines), the mod's view state
// (turn offset, recenter point) and, for checking the restore, the resulting
// view. VR/<name>-state.txt; tools/headless/bm-restore turns it into a run.
void VR::WriteCaptureState(const std::string& name, const L4D2VROpenXrPoseDesc* pose)
{
    std::string t = "# BMVR capture state v1 (tools/headless/bm-restore reads it)\n";
    char b[256];
    std::snprintf(b, sizeof(b), "name %s\nmap %s\n", name.c_str(),
        m_CurrentMapName.empty() ? "-" : m_CurrentMapName.c_str());
    t += b;
    // HMD: the pose this published frame was rendered for (falls back to the
    // newest runtime pose); hands: the newest input state.
    if (pose && pose->valid)
        FmtPose(t, "raw hmd", pose->position, pose->orientation, true);
    else
        FmtPose(t, "raw hmd", m_OpenXrLastHmdPose.position, m_OpenXrLastHmdPose.orientation,
            m_OpenXrLastHmdPose.valid != 0);
    const L4D2VROpenXrInputStateDesc& in = m_OpenXrLastInputState;
    const auto& gl = in.controllerPoses[L4D2VR_OPENXR_HAND_LEFT];
    const auto& gr = in.controllerPoses[L4D2VR_OPENXR_HAND_RIGHT];
    const auto& al = in.controllerAimPoses[L4D2VR_OPENXR_HAND_LEFT];
    const auto& ar = in.controllerAimPoses[L4D2VR_OPENXR_HAND_RIGHT];
    FmtPose(t, "raw gripl", gl.position, gl.orientation, gl.valid != 0);
    FmtPose(t, "raw gripr", gr.position, gr.orientation, gr.valid != 0);
    FmtPose(t, "raw aiml", al.position, al.orientation, al.valid != 0);
    FmtPose(t, "raw aimr", ar.position, ar.orientation, ar.valid != 0);
    std::snprintf(b, sizeof(b), "ipd %.5f\n", m_Ipd);
    t += b;
    L4D2VROpenXrRuntimeViewConfigDesc vc{};
    uint32_t vcGen = 0;
    if (L4D2VR_ReadOpenXrRuntimeViewConfig(vc, &vcGen) && vc.valid)
    {
        const auto& l = vc.views[L4D2VR_OPENXR_EYE_LEFT];
        std::snprintf(b, sizeof(b), "runtime_size %u %u\nruntime_fov %.5f %.5f %.5f %.5f\n", l.width, l.height,
            tanf(-l.angleLeft), tanf(l.angleRight), tanf(l.angleUp), tanf(-l.angleDown));
        t += b;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        std::snprintf(b, sizeof(b), "turn %.5f\nlatched %d\n", m_RotationOffsetY.load(std::memory_order_acquire),
            m_HmdOriginLatched ? 1 : 0);
        t += b;
        FmtVec(t, "zero_pos", m_HmdPosAbsZero);
        std::snprintf(b, sizeof(b), "zero_ang %.4f %.4f %.4f\n", m_HmdAngAbsZero.x, m_HmdAngAbsZero.y, m_HmdAngAbsZero.z);
        t += b;
        FmtVec(t, "setup", m_SetupOrigin);
        // Check values (bm-restore compares the restored run's log with these).
        FmtVec(t, "check_hmd_abs", m_StereoFramePoseActive ? m_StereoFrameHmdPosAbs : m_HmdPosAbs);
        const Vector va = GetViewAngle();
        FmtVec(t, "check_view_ang", va);
        FmtVec(t, "check_eye_left", GetViewOriginLeft(m_SetupOrigin));
        FmtVec(t, "check_eye_right", GetViewOriginRight(m_SetupOrigin));
        FmtVec(t, "check_right_hand", GetRightControllerAbsPos(m_SetupOrigin));
        std::snprintf(b, sizeof(b), "eye_size %u %u\nheight_offset_cm %.2f\neye_z %.4f\nvr_scale %.4f\nipd_scale %.4f\n",
            m_RenderWidth, m_RenderHeight, bmvr::g_HeightOffsetCm, m_EyeZ, m_VRScale, m_IpdScale);
        t += b;
    }
    const std::string base = bmvr::VrFilePath(name.c_str());
    const bool ok = WriteTextFile(base + "-state.txt", t);
    // The session's mod files (gun placement + settings), so a later edit of
    // the live ones does not change the restore.
    CopyFileA(bmvr::VrFilePath("config.txt").c_str(), (base + "-config.txt").c_str(), FALSE);
    CopyFileA(bmvr::VrFilePath("viewmodel_offsets.txt").c_str(), (base + "-viewmodel_offsets.txt").c_str(), FALSE);
    Game::logMsg("debug capture %s: view state %s (restore headless: tools/headless/bm-restore \"%s-state.txt\" <outdir>)",
        name.c_str(), ok ? "written" : "NOT written", base.c_str());
}

// Game thread: the engine side of a capture and the save that brings the
// world back (position, duck, trains, NPCs, flags). The view is not taken from
// the save (saves keep the aim, not the exact head pose): bm-restore sets it.
void VR::FlushCaptureEngineState()
{
    // Engine eye speed (moving trains/lifts: bm-restore slows time for those).
    {
        const DWORD now = GetTickCount();
        if (now - m_SetupVelPrevMs >= 100)
        {
            if (m_SetupVelPrevMs && now - m_SetupVelPrevMs < 1000)
                m_SetupVelocity = (m_SetupOrigin - m_SetupVelPrev) * (1000.f / static_cast<float>(now - m_SetupVelPrevMs));
            m_SetupVelPrev = m_SetupOrigin;
            m_SetupVelPrevMs = now;
        }
    }
    std::string name;
    {
        std::lock_guard<std::mutex> lock(m_CaptureMutex);
        name.swap(m_CaptureEngineName);
    }
    if (name.empty() || !m_Game)
        return;
    std::string t = "# BMVR capture engine state v1 (game thread)\n";
    char b[256];
    FmtVec(t, "engine_setup", m_SetupOrigin);
    FmtVec(t, "engine_velocity", m_SetupVelocity);
    QAngle ea{};
    if (m_Game->m_EngineClient)
    {
        m_Game->m_EngineClient->GetViewAngles(ea);
        std::snprintf(b, sizeof(b), "engine_angles %.4f %.4f %.4f\n", ea.x, ea.y, ea.z);
        t += b;
    }
    std::snprintf(b, sizeof(b), "save %s\n", name.c_str());
    t += b;
    const std::string base = bmvr::VrFilePath(name.c_str());
    WriteTextFile(base + "-engine.txt", t);
    WriteTextFile(base + "-settings.txt", bmvr::CaptureSettingsScenario(m_Game));
    std::snprintf(b, sizeof(b), "save %s", name.c_str());
    const bool ok = m_Game->ClientCmd_Unrestricted(b);
    Game::logMsg("debug capture %s: engine state + settings written, '%s' ok=%d (eye speed %.1f u/s)", name.c_str(), b,
        ok ? 1 : 0, m_SetupVelocity.Length());
}

// Game thread, fake runtime only (headless restore, from VR/cmd.txt):
//   bmvr_restore_view <turn> <zero x y z> <zero pitch yaw roll> <engine eye x y z>
// Sets the turn offset and the recenter point exactly as captured (the save
// cannot: loads re-latch the recenter point and sync the turn to the saved
// aim), then moves the player until the engine eye is the captured one.
bool VR::HandleRestoreCommand(const char* cmd)
{
    static const char kVerb[] = "bmvr_restore_view";
    if (!cmd || std::strncmp(cmd, kVerb, sizeof(kVerb) - 1) != 0)
        return false;
    float v[10] = {};
    const int n = std::sscanf(cmd + sizeof(kVerb) - 1, "%f %f %f %f %f %f %f %f %f %f",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n != 10)
    {
        Game::logMsg("restore view: needs 10 numbers, got %d: '%s'", n, cmd);
        return true;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        m_HmdPosAbsZero = Vector(v[1], v[2], v[3]);
        m_HmdAngAbsZero = QAngle(v[4], v[5], v[6]);
        m_HmdOriginLatched = true;
    }
    m_RotationOffsetY.store(v[0], std::memory_order_release);
    m_YawSyncTicks = 0;
    m_FirstCmdYawPending = false;
    m_RestoreTarget = Vector(v[7], v[8], v[9]);
    m_RestoreActive = true;
    m_RestoreHaveSent = false;
    m_RestoreSteps = 0;
    m_RestoreNextMs = GetTickCount() + 300;
    Game::logMsg("restore view: turn %.3f, recenter point (%.2f %.2f %.2f / %.2f %.2f %.2f), moving the engine eye to (%.2f %.2f %.2f)",
        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
    return true;
}

void VR::TickRestoreView()
{
    if (!m_RestoreActive || !m_Game)
        return;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - m_RestoreNextMs) < 0)
        return;
    const Vector diff = m_RestoreTarget - m_SetupOrigin;
    if (diff.Length() < 0.05f)
    {
        m_RestoreActive = false;
        Game::logMsg("restore view: engine eye at the captured spot (off by %.3f u after %d setpos); eyes L (%.2f %.2f %.2f) R (%.2f %.2f %.2f), view ang (%.2f %.2f %.2f)",
            diff.Length(), m_RestoreSteps, GetViewOriginLeft(m_SetupOrigin).x, GetViewOriginLeft(m_SetupOrigin).y,
            GetViewOriginLeft(m_SetupOrigin).z, GetViewOriginRight(m_SetupOrigin).x, GetViewOriginRight(m_SetupOrigin).y,
            GetViewOriginRight(m_SetupOrigin).z, GetViewAngle().x, GetViewAngle().y, GetViewAngle().z);
        return;
    }
    if (m_RestoreSteps >= 8)
    {
        m_RestoreActive = false;
        Game::logMsg("restore view: GAVE UP, engine eye still off by %.2f u (%.2f %.2f %.2f) after %d setpos",
            diff.Length(), diff.x, diff.y, diff.z, m_RestoreSteps);
        return;
    }
    // setpos takes the player origin; the eye sits a view offset above it
    // (duck dependent). First guess 64 u, then correct by what is left over.
    if (!m_RestoreHaveSent)
        m_RestoreSent = m_RestoreTarget - Vector(0.f, 0.f, 64.f);
    else
        m_RestoreSent = m_RestoreSent + diff;
    m_RestoreHaveSent = true;
    char b[160];
    std::snprintf(b, sizeof(b), "setpos %.4f %.4f %.4f", m_RestoreSent.x, m_RestoreSent.y, m_RestoreSent.z);
    const bool ok = m_Game->ClientCmd_Unrestricted(b);
    ++m_RestoreSteps;
    m_RestoreNextMs = now + 400;
    Game::logMsg("restore view: step %d, eye off by %.3f u -> '%s' ok=%d", m_RestoreSteps, diff.Length(), b, ok ? 1 : 0);
}

void VR::PublishOpenXrPair(uint32_t slot, const OpenXrPendingPublish& pend)
{
    MaybeDebugDumpPublished(slot, pend.havePose ? &pend.pose : nullptr);
    if (pend.havePose)
    {
        L4D2VR_PublishOpenXrGameRenderPose(pend.pose);
        m_OpenXrPublishedDisplayTime = pend.pose.displayTime;
        m_OpenXrPublishedPoseGeneration = m_OpenXrLastHmdPoseGeneration;
    }
    const uint32_t frameId = m_OpenXrSubmitFrameId.fetch_add(1, std::memory_order_acq_rel);
    m_OpenXrPublishActive = (slot != kOpenXrNoSlot) && m_OpenXrPublishReady;
    if (m_OpenXrPublishActive)
        m_OpenXrPublishSlot = slot;
    PublishOpenXrResolvedEyeTextures(frameId, pend.panel2d);
    ++m_OpenXrPublishes;
    NotePublishEvenness();
    if (bmvr::glowtrace::Active())
        bmvr::glowtrace::Event('U', frameId);
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
    // slots after the pair. Only skip the Present copy when that functor
    // actually queued. A true here with no copy left the HMD on the last
    // menu letterbox (Index SteamVR 2026-09-09: published=0/s, mc=1).
    if (McPipelineEnabled() && m_McEyeCopyQueued)
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

    // Deferred: the previous pair's copy is normally still on the GPU when the
    // next pair is ready (one frame of overlap). Dropping the new pair
    // (`throttled`) halved the headset to 45/s on bm_c1a0a (2026-10-14:
    // pairs=88/s published=44/s throttled=44/s). Wait for that copy, publish
    // it, then copy this pair: the GPU keeps working on this pair meanwhile
    // and nothing is dropped. Before the lock: Poll takes the same mutex.
    if (bmvr::g_OpenXrDeferredPublish)
    {
        const uint32_t maxPending = std::min<uint32_t>(
            std::max<uint32_t>(bmvr::g_OpenXrMaxPending, 1u), kOpenXrMaxPending);
        PollOpenXrDeferredPublish();
        if (m_OpenXrPendingCount >= maxPending)
            WaitOldestOpenXrPending();
    }

    std::lock_guard<std::mutex> lock(m_OpenXrPublishMutex);
    // Always use the consumed-id gate. OpenXrDeferredPublish=false used to
    // round-robin 4 slots at the Present rate (~200/s in-game) while the
    // helper's blit still held an older slot — that is the occasional
    // broken/old frame. HL2VR never rewrites a submitted image until
    // PostPresentHandoff. Same-displayTime skip starved the menu
    // (latched panel pose, published=0/s) and is not needed: no free slot
    // already drops the extra Present copy.
    const bool defer = bmvr::g_OpenXrDeferredPublish;
    const double nowMs = QpcNowMs();
    RefreshOpenXrHelperConsumed(nowMs);
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
    }
    uint32_t slot = PickFreeOpenXrPublishSlot(nowMs);
    if (slot == kOpenXrNoSlot)
    {
        ++m_OpenXrDeferredThrottles;
        if (m_OpenXrHelperFeedbackLive)
            ++m_OpenXrDeferredHelperHolds;
        device->Release();
        return false;
    }

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
        const double waitStart = QpcNowMs();
        pollEvent(q, 250.0);
        const double waited = QpcNowMs() - waitStart;
        ++m_OpenXrCopyWaits;
        m_OpenXrCopyWaitMs += waited;
        if (waited > m_OpenXrCopyWaitMaxMs)
            m_OpenXrCopyWaitMaxMs = waited;
        m_OpenXrCopyThread = GetCurrentThreadId();
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
    // Title texts stay in front of the eyes; the pause menu stays put in the room.
    // TitleLevel: titles take the pause-menu path too (the helper places the
    // layer level at the head's yaw once per epoch; UpdateTitleHud bumps it).
    // Suit boot-up sequences are always head-locked (HUD inside the helmet).
    // Captions too (head-locked at the bottom of the view, issue #18).
    if (!WantPauseWorldOverlay() && WantTitleHudOverlay() &&
        (!bmvr::g_TitleLevel || m_BootHudActive || m_CaptionHudActive))
        overlay.reserved1 |= L4D2VR_OPENXR_OVERLAY_FLAG_HEAD_LOCKED;
    // Title texts are published premultiplied (TitlePremultipliedShader).
    if (!WantPauseWorldOverlay() && WantTitleHudOverlay())
        overlay.reserved1 |= L4D2VR_OPENXR_OVERLAY_FLAG_PREMULTIPLIED;
    if (!FillOpenXrOverlayTextureFromShared(overlay, hud))
        return false;

    float dist = 0.f, width = 0.f, yOff = 0.f;
    GetMenuOverlayMetrics(dist, width, yOff);
    if (!WantPauseWorldOverlay() && m_BootHudActive)
        width *= std::clamp(bmvr::g_BootHudScale, 0.5f, 2.f);
    overlay.widthMeters = (std::max)(0.10f, width);
    overlay.heightMeters = overlay.widthMeters *
        (static_cast<float>(hud.m_VulkanData.m_nHeight) /
            static_cast<float>((std::max)(1u, hud.m_VulkanData.m_nWidth)));
    overlay.distanceMeters = (std::max)(0.10f, dist);
    // Subtitle height / size (GENERAL > VIEW): scale around the caption box
    // (it sits below the layer centre), then move by the angle.
    if (!WantPauseWorldOverlay() && m_CaptionOnly)
    {
        // The rows are relative to the user's headset-tested look (2026-11-25:
        // height +4 deg, size 1.1x = row values 0 / 1.00).
        constexpr float kBaseHeightDeg = 4.f, kBaseScale = 1.1f;
        const float s = kBaseScale * std::clamp(bmvr::g_CaptionScale, 0.5f, 2.f);
        const float deg = kBaseHeightDeg + std::clamp(bmvr::g_CaptionHeightDeg, -30.f, 30.f);
        const float h0 = overlay.heightMeters;
        overlay.widthMeters *= s;
        overlay.heightMeters *= s;
        yOff += (m_CaptionCentreY - 0.5f) * h0 * (s - 1.f);
        yOff += overlay.distanceMeters * std::tan(deg * 0.01745329f);
    }
    overlay.curvature = 0.0f;
    overlay.offsetMeters[0] = 0.0f;
    overlay.offsetMeters[1] = yOff;
    overlay.offsetMeters[2] = 0.0f;
    // Pause menu: place it from the latched panel pose (room front at eye
    // height), so it matches the pointer ray in IntersectMenuPanelUv.
    if (WantPauseWorldOverlay() && m_MenuPanelPoseValid
        && m_MenuPanelPose.reserved0 == m_MenuOverlayEpoch)
    {
        overlay.reserved1 |= L4D2VR_OPENXR_OVERLAY_FLAG_ANCHOR_POSE;
        std::memcpy(overlay.anchorPosition, m_MenuPanelPose.position, sizeof(overlay.anchorPosition));
        std::memcpy(overlay.orientation, m_MenuPanelPose.orientation, sizeof(overlay.orientation));
    }

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

bool VR::GetDigitalActionData(vr::VRActionHandle_t handle, vr::InputDigitalActionData_t& out) const
{
    out = {};
    if (m_RuntimeBackend == VrRuntimeBackend::OpenXR)
    {
        const uint32_t actionIndex = static_cast<uint32_t>(handle);
        if (actionIndex == 0 || actionIndex >= L4D2VR_OPENXR_ACTION_COUNT)
            return false;
        if (m_ButtonMapActive && m_MappedOverride[actionIndex])
        {
            const L4D2VROpenXrDigitalActionDesc& mapped = m_MappedDigital[actionIndex];
            out.bActive = true;
            out.activeOrigin = vr::k_ulInvalidInputValueHandle;
            out.bState = mapped.state != 0;
            out.bChanged = mapped.changed != 0;
            return true;
        }
        const L4D2VROpenXrDigitalActionDesc& action = m_OpenXrLastInputState.digitalActions[actionIndex];
        if (!action.active)
            return false;
        if (m_LegacyMask[actionIndex])
        {
            // Held across the switch from mapped buttons to the fixed menu
            // layout: released until let go.
            if (action.state)
            {
                out.bActive = true;
                out.activeOrigin = vr::k_ulInvalidInputValueHandle;
                return true;
            }
            m_LegacyMask[actionIndex] = false;
        }
        out.bActive = true;
        out.activeOrigin = vr::k_ulInvalidInputValueHandle;
        out.bState = action.state != 0;
        out.bChanged = action.changed != 0;
        out.fUpdateTime = 0.0f;
        return true;
    }
    return false;
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
    return false;
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

void VR::CaptureSaveThumbnail()
{
    {
        std::lock_guard<std::mutex> lock(m_SaveThumbMutex);
        if (m_SaveThumbReady)
            return;
    }
    if (!g_D3DVR9 || !m_D9LeftEyeSurface)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    D3DSURFACE_DESC eye{};
    int cw = 0, ch = 0;
    if (SUCCEEDED(m_D9LeftEyeSurface->GetDesc(&eye)) && eye.Width > 0 && eye.Height > 0)
    {
        cw = (std::min)(kSaveThumbCaptureWidth, static_cast<int>(eye.Width));
        ch = (std::max)(1, static_cast<int>(static_cast<float>(cw) * eye.Height / eye.Width + 0.5f));
    }
    IDirect3DSurface9* scaled = nullptr;
    IDirect3DSurface9* sys = nullptr;
    std::vector<unsigned char> bgr;
    if (cw > 0
        && SUCCEEDED(device->CreateRenderTarget(cw, ch, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &scaled, nullptr))
        && SUCCEEDED(device->CreateOffscreenPlainSurface(cw, ch, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr))
        && SUCCEEDED(device->StretchRect(m_D9LeftEyeSurface, nullptr, scaled, nullptr, D3DTEXF_LINEAR))
        && SUCCEEDED(device->GetRenderTargetData(scaled, sys)))
    {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
        {
            bgr.resize(static_cast<size_t>(cw) * ch * 3);
            for (int y = 0; y < ch; ++y)
            {
                const auto* src = static_cast<const unsigned char*>(lr.pBits) + y * lr.Pitch;
                unsigned char* dst = bgr.data() + static_cast<size_t>(y) * cw * 3;
                for (int x = 0; x < cw; ++x)
                {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                }
            }
            sys->UnlockRect();
        }
    }
    if (sys)
        sys->Release();
    if (scaled)
        scaled->Release();
    device->Release();
    std::lock_guard<std::mutex> lock(m_SaveThumbMutex);
    m_SaveThumbBgr.swap(bgr);
    m_SaveThumbW = cw;
    m_SaveThumbH = ch;
    // Ready even when the copy failed: one try per pause or save, never per frame.
    m_SaveThumbReady = true;
    Game::logMsg("Save thumbnail: left eye scaled to %dx%d (%s)", cw, ch,
        m_SaveThumbBgr.empty() ? "failed, the game's own grab stays" : "ok");
}

void VR::UpdateSaveThumbnailCapture()
{
    const bool paused = WantPauseWorldOverlay();
    bool wanted = false;
    {
        std::lock_guard<std::mutex> lock(m_SaveThumbMutex);
        if (!paused && !m_SaveThumbPending)
            m_SaveThumbReady = false;
        wanted = !m_SaveThumbReady && (paused || m_SaveThumbPending);
    }
    // Copying the eyes while the window is deactivated crashed with the overlay
    // being torn down (see SubmitVRTextures); no capture then.
    if (wanted && m_AppActivated.load(std::memory_order_relaxed))
        CaptureSaveThumbnail();
}

bool VR::WantsSaveThumbnail() const
{
    return m_IsVREnabled && m_GameplayEligible && m_SeenGameplay
        && m_CreatedVRTextures.load(std::memory_order_acquire);
}

VR::SaveThumbState VR::PollSaveThumbnail()
{
    const bool paused = WantPauseWorldOverlay();
    std::lock_guard<std::mutex> lock(m_SaveThumbMutex);
    if (m_SaveThumbReady && (paused || m_SaveThumbPending))
    {
        if (!m_SaveThumbBgr.empty())
            return SaveThumbState::Ready;
        m_SaveThumbPending = false;
        return SaveThumbState::Failed;
    }
    const ULONGLONG now = GetTickCount64();
    if (!m_SaveThumbPending)
    {
        m_SaveThumbPending = true;
        m_SaveThumbGiveUpMs = now + kSaveThumbWaitMs;
        return SaveThumbState::Waiting;
    }
    if (now < m_SaveThumbGiveUpMs)
        return SaveThumbState::Waiting;
    m_SaveThumbPending = false;
    return SaveThumbState::Failed;
}

void VR::EndSaveThumbnail()
{
    std::lock_guard<std::mutex> lock(m_SaveThumbMutex);
    m_SaveThumbPending = false;
}

bool VR::FillSaveThumbnail(unsigned char* rgb, int w, int h)
{
    if (!rgb || w <= 0 || h <= 0)
        return false;
    // The part of the eye to show, in 0..1 of the capture: the widest rectangle
    // of the grab's shape centred on straight ahead (eyes can be off-centre).
    float u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
    float rect[4]{};
    if (EyeRenderRect(1, rect) && rect[0] < 0.f && rect[1] > 0.f && rect[2] < 0.f && rect[3] > 0.f)
    {
        const float aspect = static_cast<float>(w) / static_cast<float>(h);
        float halfX = (std::min)(-rect[0], rect[1]);
        float halfY = (std::min)(-rect[2], rect[3]);
        if (halfX / halfY > aspect)
            halfX = halfY * aspect;
        else
            halfY = halfX / aspect;
        const float spanX = rect[1] - rect[0];
        const float spanY = rect[3] - rect[2];
        u0 = (-halfX - rect[0]) / spanX;
        u1 = (halfX - rect[0]) / spanX;
        v0 = (rect[3] - halfY) / spanY;
        v1 = (rect[3] + halfY) / spanY;
    }
    std::lock_guard<std::mutex> lock(m_SaveThumbMutex);
    if (!m_SaveThumbReady || m_SaveThumbBgr.empty())
        return false;
    const int cw = m_SaveThumbW;
    const int ch = m_SaveThumbH;
    for (int y = 0; y < h; ++y)
    {
        const float v = v0 + (v1 - v0) * (y + 0.5f) / h;
        const int sy = (std::min)(ch - 1, (std::max)(0, static_cast<int>(v * ch)));
        const unsigned char* row = m_SaveThumbBgr.data() + static_cast<size_t>(sy) * cw * 3;
        unsigned char* dst = rgb + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x)
        {
            const float u = u0 + (u1 - u0) * (x + 0.5f) / w;
            const int sx = (std::min)(cw - 1, (std::max)(0, static_cast<int>(u * cw)));
            dst[x * 3 + 0] = row[sx * 3 + 2];
            dst[x * 3 + 1] = row[sx * 3 + 1];
            dst[x * 3 + 2] = row[sx * 3 + 0];
        }
    }
    return true;
}

bool VR::ScriptedCameraActive() const
{
    return m_ScriptedCamActive && m_ScriptedCamDiffersNow && bmvr::g_FollowScriptedCamera;
}

void VR::UpdateScriptedCamera(const Vector& engineAngles)
{
    BMVR_PROF("vr UpdateScriptedCamera");
    if (!m_LastCmdYawValid || !std::isfinite(engineAngles.x) || !std::isfinite(engineAngles.y))
        return;
    // Only a cutscene camera (point_viewcontrol) sets the player's view
    // entity. Death (the view rolled onto the floor; followed, it turned the
    // whole view over), view punch, shakes and grabs never do.
    bool viewEntitySet = true;
    if (m_Game && m_Game->ReadLocalPlayerViewEntitySet(viewEntitySet) && !viewEntitySet)
    {
        if (m_ScriptedCamActive)
        {
            // Keep the heading the camera ended on: the normal view is turn offset + head yaw, which
            // still pointed where the player faced before the scene, so after the Xen arrival
            // stand-up (bm_c3a2i -> Xen) the view swung back to the wall instead of the cave exit
            // (2026-11-16 headset). Turn so the current view yaw equals the camera view's yaw.
            Vector camAngles, camRight;
            if (bmvr::g_FollowScriptedCamera && m_ScriptedCamDiffersNow && m_HmdPoseValid
                && ScriptedCameraView(camAngles, camRight))
            {
                const float viewYaw = GetViewAngle().y;
                const float before = m_RotationOffsetY.load(std::memory_order_acquire);
                float offset = before + WrapYaw(camAngles.y - viewYaw);
                offset -= 360.f * floorf(offset / 360.f);
                m_RotationOffsetY.store(offset, std::memory_order_release);
                Game::logMsg("Scripted camera end: turn kept on the camera heading %.1f (view %.1f), turn offset %.1f -> %.1f",
                    camAngles.y, viewYaw, before, offset);
            }
            Game::logMsg("Scripted camera off: view entity cleared");
        }
        m_ScriptedCamActive = false;
        m_ScriptedCamDiffersNow = false;
        m_ScriptedCamFrames = 0;
        return;
    }
    // Clear of view punch and screen shake (a few degrees, mostly roll).
    const bool differs = fabsf(WrapYaw(engineAngles.y - m_LastCmdYaw)) > 8.f
        || fabsf(engineAngles.x - m_LastCmdPitch) > 8.f || fabsf(engineAngles.z) > 8.f;
    QAngle hmd{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        hmd = m_StereoFramePoseActive ? m_StereoFrameAngles : m_HmdAngAbs;
    }
    // Enter after 10 frames that differ, leave after 45 that match (the
    // engine angles follow the usercmd again). In between, matching frames
    // render the normal view and a differing one picks the camera up again
    // with the same head reference.
    m_ScriptedCamDiffersNow = differs;
    if (differs)
        m_ScriptedCamFrames = m_ScriptedCamActive ? 0 : m_ScriptedCamFrames + 1;
    else
        m_ScriptedCamFrames = m_ScriptedCamActive ? m_ScriptedCamFrames + 1 : 0;
    if (!m_ScriptedCamActive && m_ScriptedCamFrames >= 10)
    {
        m_ScriptedCamActive = true;
        m_ScriptedCamFrames = 0;
        m_ScriptedCamHmdYaw0 = hmd.y;
        Game::logMsg("Scripted camera on: engine angles (%.1f %.1f %.1f), last sent (%.1f %.1f)",
            engineAngles.x, engineAngles.y, engineAngles.z, m_LastCmdPitch, m_LastCmdYaw);
    }
    else if (m_ScriptedCamActive && m_ScriptedCamFrames >= 45)
    {
        m_ScriptedCamActive = false;
        m_ScriptedCamFrames = 0;
        Game::logMsg("Scripted camera off: engine angles (%.1f %.1f %.1f)", engineAngles.x, engineAngles.y, engineAngles.z);
    }
    m_ScriptedCamAngles = QAngle(engineAngles.x, engineAngles.y, engineAngles.z);
}

bool VR::ScriptedCameraView(Vector& angles, Vector& right) const
{
    QAngle hmd{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        hmd = m_StereoFramePoseActive ? m_StereoFrameAngles : m_HmdAngAbs;
    }
    // Head orientation relative to where it pointed when the camera took over,
    // expressed in the camera's frame: world = camera * head.
    Vector cf, cr, cu, hf, hr, hu;
    QAngle::AngleVectors(m_ScriptedCamAngles, &cf, &cr, &cu);
    QAngle::AngleVectors(QAngle(hmd.x, WrapYaw(hmd.y - m_ScriptedCamHmdYaw0), hmd.z), &hf, &hr, &hu);
    const Vector cl = cr * -1.f;
    auto toWorld = [&](const Vector& v) { return cf * v.x + cl * v.y + cu * v.z; };
    const Vector f = toWorld(hf);
    const Vector l = toWorld(hr * -1.f);
    const Vector u = toWorld(hu);
    const float xy = sqrtf(f.x * f.x + f.y * f.y);
    float pitch, yaw, roll;
    if (xy > 0.001f)
    {
        yaw = atan2f(f.y, f.x);
        pitch = atan2f(-f.z, xy);
        roll = atan2f(l.z, u.z);
    }
    else
    {
        yaw = atan2f(-l.x, l.y);
        pitch = atan2f(-f.z, xy);
        roll = 0.f;
    }
    constexpr float kDeg = 180.f / 3.14159265f;
    angles = Vector(pitch * kDeg, yaw * kDeg, roll * kDeg);
    right = l * -1.f;
    return std::isfinite(angles.x) && std::isfinite(angles.y) && std::isfinite(angles.z);
}

void VR::SyncTurnToRestoredYaw(float engineYaw)
{
    const ULONGLONG now = GetTickCount64();
    // Diagnostics (new game UI loads did not sync, 2026-11-23): arming,
    // the first eligible command after it, and why a window ran out.
    static int s_diag;
    static bool s_firstEligibleLogged;
    if (m_LastCmdMs && now - m_LastCmdMs > 400)
    {
        if (s_diag < 40)
        {
            ++s_diag;
            Game::logMsg("Yaw sync armed after a %llu ms command gap (engine yaw %.1f, last sent %.1f valid=%d, eligible=%d pose=%d)",
                static_cast<unsigned long long>(now - m_LastCmdMs), engineYaw, m_LastCmdYaw, m_LastCmdYawValid ? 1 : 0,
                m_GameplayEligible ? 1 : 0, m_HmdPoseValid ? 1 : 0);
        }
        m_YawSyncTicks = 600;
        s_firstEligibleLogged = false;
        // A load, not the session start: the start-view path (fake runtime
        // only) must not take this window.
        m_FirstCmdYawPending = false;
    }
    m_LastCmdMs = now;
    if (m_YawSyncTicks <= 0)
        return;
    if (!s_firstEligibleLogged && m_GameplayEligible && m_HmdPoseValid && s_diag < 40)
    {
        s_firstEligibleLogged = true;
        ++s_diag;
        Game::logMsg("Yaw sync first eligible command: engine yaw %.1f, last sent %.1f valid=%d, last sent in gameplay %.1f valid=%d, ticks left %d",
            engineYaw, m_LastCmdYaw, m_LastCmdYawValid ? 1 : 0, m_LastGameplayCmdYaw, m_LastGameplayCmdYawValid ? 1 : 0, m_YawSyncTicks);
    }
    if (m_YawSyncTicks == 1 && s_diag < 40)
    {
        ++s_diag;
        Game::logMsg("Yaw sync window ran out: engine yaw %.1f, last sent in gameplay %.1f (no change >= 1 deg seen)",
            engineYaw, m_LastGameplayCmdYaw);
    }
    if (!m_LastCmdYawValid && !m_FirstCmdYawPending && std::isfinite(engineYaw) && L4D2VR_FakeHelperActive())
    {
        m_FirstCmdYawPending = true;
        m_FirstCmdYaw = engineYaw;
    }
    if (m_FirstCmdYawPending && m_HmdPoseValid && m_GameplayEligible)
    {
        m_FirstCmdYawPending = false;
        m_YawSyncTicks = 0;
        const float viewYaw = GetViewAngle().y;
        const float before = m_RotationOffsetY.load(std::memory_order_acquire);
        float offset = before + WrapYaw(m_FirstCmdYaw - viewYaw);
        offset -= 360.f * floorf(offset / 360.f);
        m_RotationOffsetY.store(offset, std::memory_order_release);
        Game::logMsg("Turn synced to the start view (no earlier command): engine yaw %.1f, view %.1f, turn offset %.1f -> %.1f",
            m_FirstCmdYaw, viewYaw, before, offset);
        return;
    }
    if (!m_HmdPoseValid || !m_GameplayEligible || !std::isfinite(engineYaw))
        return;
    --m_YawSyncTicks;
    // Reference: the last yaw sent in gameplay (commands outside gameplay
    // pass the restored angles through; comparing with those, the new game
    // UI's menu loads never synced, 2026-11-23). No gameplay yet this
    // session: the engine's yaw (spawn or save) is the one to take.
    if (m_LastGameplayCmdYawValid)
    {
        const float changed = WrapYaw(engineYaw - m_LastGameplayCmdYaw);
        if (fabsf(changed) < 1.f)
            return;
    }
    m_YawSyncTicks = 0;
    const float viewYaw = GetViewAngle().y;
    const float before = m_RotationOffsetY.load(std::memory_order_acquire);
    float offset = before + WrapYaw(engineYaw - viewYaw);
    offset -= 360.f * floorf(offset / 360.f);
    m_RotationOffsetY.store(offset, std::memory_order_release);
    Game::logMsg("Turn synced to the restored view: engine yaw %.1f (last sent in gameplay %.1f valid=%d), view %.1f, turn offset %.1f -> %.1f",
        engineYaw, m_LastGameplayCmdYaw, m_LastGameplayCmdYawValid ? 1 : 0, viewYaw, before, offset);
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

void VR::UpdateVrRecoil()
{
    const DWORD now = GetTickCount();
    const float dt = m_RecoilLastMs ? (std::min)(static_cast<float>(now - m_RecoilLastMs) * 0.001f, 0.1f) : 0.f;
    m_RecoilLastMs = now;
    // Back toward the hand's own aim with a ~0.12 s time constant (95 % in
    // ~0.35 s), so a burst climbs and a pause settles.
    float kick = m_RecoilKickDeg.load(std::memory_order_relaxed) * expf(-dt / 0.12f);
    if (kick < 0.01f)
        kick = 0.f;
    if (!bmvr::PhysicalWeapon(bmvr::g_VrRecoil) || !m_Game || m_EmptyHands)
    {
        m_RecoilKickDeg.store(0.f, std::memory_order_relaxed);
        m_RecoilWeapon = nullptr;
        m_RecoilClip = -1;
        return;
    }
    if (PressedDigitalAction(m_ActionPrimaryAttack) || PressedDigitalAction(m_ActionSecondaryAttack))
        m_RecoilAttackMs = now;
    C_BaseEntity* weapon = m_Game->GetActiveWeaponEntity();
    const int clip = weapon ? m_Game->ReadWeaponClip(weapon) : -1;
    // A shot = the held gun's clip went down while a fire button was down
    // (reloads only raise it; a manual-magazine eject happens without one).
    if (weapon && weapon == m_RecoilWeapon && clip >= 0 && m_RecoilClip > clip
        && now - m_RecoilAttackMs < 400)
    {
        float perShot = 0.f;
        if (m_VmClass.shotgun) perShot = 7.f;
        else if (m_VmClass.revolver) perShot = 7.f;
        else if (m_VmClass.mp5) perShot = 1.2f;
        else if (m_VmClass.glock) perShot = 2.5f;
        else if (m_VmClass.crossbow) perShot = 1.5f;
        bool twoHand = false, pistolGrip = false;
        {
            std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
            twoHand = m_TwoHandActive;
            pistolGrip = m_VmClass.offhandPistol;
        }
        // Second hand on the gun: VrRecoilTwoHand of the kick (pistols x5/3).
        const float two = std::clamp(bmvr::g_VrRecoilTwoHand, 0.f, 1.f);
        const float hands = twoHand ? (pistolGrip ? (std::min)(1.f, two * 5.f / 3.f) : two) : 1.f;
        const int shots = (std::min)(m_RecoilClip - clip, 3);
        const float add = perShot * static_cast<float>(shots) * hands * bmvr::g_VrRecoilScale;
        kick = (std::min)(kick + add, 15.f);
        static int s_recoilLog;
        if (add > 0.f && s_recoilLog < 60)
        {
            ++s_recoilLog;
            // aim = the firing pitch before this shot's kick is stored
            // (Source: negative = up), for checking the kick direction.
            Game::logMsg("VR recoil: %d shot(s) +%.1f deg (%s, x%.2f) -> kick %.1f deg, aim pitch %.1f",
                shots, add, twoHand ? "two hands" : "one hand", hands, kick, GetAimAngles().x);
        }
    }
    m_RecoilWeapon = weapon;
    m_RecoilClip = clip;
    m_RecoilKickDeg.store(kick, std::memory_order_relaxed);
}

void VR::UpdateButtonMap()
{
    using bmvr::BindAction;
    using Id = L4D2VROpenXrActionId;
    // Mapped action -> the OpenXR action slot the input code already reads.
    // QuickSave / QuickLoad / VrSettings / Recenter have no slot: handled here.
    static const std::pair<BindAction, Id> kSlots[] =
    {
        { BindAction::Fire, Id::PrimaryAttack },
        { BindAction::AltFire, Id::SecondaryAttack },
        { BindAction::Reload, Id::Reload },
        { BindAction::Jump, Id::Jump },
        { BindAction::CrouchToggle, Id::CustomAction2 },
        { BindAction::CrouchHold, Id::Crouch },
        { BindAction::Use, Id::Use },
        { BindAction::Flashlight, Id::Flashlight },
        { BindAction::WeaponMenu, Id::InventoryQuickSwitch },
        { BindAction::NextWeapon, Id::NextItem },
        { BindAction::PrevWeapon, Id::PrevItem },
        { BindAction::Pause, Id::Pause },
    };
    // Fixed-layout slots that mapping replaces with nothing: ResetPosition
    // (right stick click also counted as the weapon menu), Scoreboard (L4D2
    // leftover on the Index left A).
    static const Id kCleared[] = { Id::ResetPosition, Id::Scoreboard };

    const L4D2VROpenXrInputStateDesc& in = m_OpenXrLastInputState;
    const bool menuMode = !m_GameplayEligible || m_SettingsOpen || IsMenuUp()
        || Want2dMenuPanel() || WantPauseWorldOverlay();
    const bool usable = m_RuntimeBackend == VrRuntimeBackend::OpenXR && bmvr::g_ButtonMapping
        && bmvr::ButtonMapper::RawAvailable(in);
    if (!usable || menuMode)
    {
        if (m_ButtonMapActive)
        {
            // Fixed layout from here: slots still held read released until let go
            // (the Y hold that opened the VR panel must not close it again).
            for (uint32_t i = 1; i < L4D2VR_OPENXR_ACTION_COUNT; ++i)
                m_LegacyMask[i] = in.digitalActions[i].active && in.digitalActions[i].state;
            m_ButtonMapper.Suspend();
            m_ButtonMapActive = false;
        }
        return;
    }

    bmvr::ButtonMapContext ctx;
    // On a ladder (hand climbing) both grips hold the ladder: no mapped
    // grip action (flashlight) even with the hand off to the side.
    const bool onLadderNow = m_LadderActive.load(std::memory_order_acquire);
    ctx.gripInGestureZone[0] = m_GestureZoneLeft || onLadderNow || m_LadderTouch[0].load(std::memory_order_acquire);
    ctx.gripInGestureZone[1] = m_GestureZoneRight || onLadderNow || m_LadderTouch[1].load(std::memory_order_acquire);
    ctx.stickBlocked = m_WeaponMenuOpen;
    m_ButtonMapper.Update(in, ctx, GetTickCount());

    for (const auto& [action, id] : kSlots)
    {
        const uint32_t i = static_cast<uint32_t>(id);
        L4D2VROpenXrDigitalActionDesc& d = m_MappedDigital[i];
        const uint32_t held = m_ButtonMapper.Held(action) ? 1u : 0u;
        d.active = 1;
        d.changed = held != d.state ? 1u : 0u;
        d.state = held;
        m_MappedOverride[i] = true;
    }
    // Sprint: hold and toggle share the sprint slot; the one pressed last
    // picks the mode (sprint latch in ProcessInput).
    {
        const bool hold = m_ButtonMapper.Held(BindAction::Sprint);
        const bool toggle = m_ButtonMapper.Held(BindAction::SprintToggle);
        if (toggle)
            m_SprintMapToggle = true;
        else if (hold)
            m_SprintMapToggle = false;
        const uint32_t i = static_cast<uint32_t>(Id::CustomAction1);
        L4D2VROpenXrDigitalActionDesc& d = m_MappedDigital[i];
        const uint32_t held = (hold || toggle) ? 1u : 0u;
        d.active = 1;
        d.changed = held != d.state ? 1u : 0u;
        d.state = held;
        m_MappedOverride[i] = true;
    }
    for (const Id id : kCleared)
    {
        const uint32_t i = static_cast<uint32_t>(id);
        m_MappedDigital[i] = {};
        m_MappedDigital[i].active = 1;
        m_MappedOverride[i] = true;
    }
    std::memset(m_LegacyMask, 0, sizeof(m_LegacyMask));
    m_ButtonMapActive = true;

    const bool paused = m_Game && SehIsPaused(m_Game->m_EngineClient);
    if (m_ButtonMapper.Pressed(BindAction::QuickSave) && !paused)
    {
        m_PendingQuickSave.store(true, std::memory_order_release);
        Game::logMsg("Button map: quick save queued");
    }
    if (m_ButtonMapper.Pressed(BindAction::QuickLoad) && !paused)
    {
        m_PendingQuickLoad.store(true, std::memory_order_release);
        Game::logMsg("Button map: quick load queued");
    }
    if (m_ButtonMapper.Pressed(BindAction::Recenter))
        Recenter("mapped button");
    // Bare hands toggle: its own action since 2026-11-25 (the fixed layout
    // still does it on a short stick click, UpdateWeaponMenu).
    if (m_ButtonMapper.Pressed(BindAction::HandsToggle) && m_Game && !m_WeaponMenuOpen && !PauseUiActive())
        ToggleEmptyHands();
    if (m_ButtonMapper.Pressed(BindAction::VrSettings) && (!paused || m_WelcomeActive) && !m_WeaponMenuOpen)
    {
        Game::logMsg("Button map: VR settings panel");
        OpenSettingsPanel();
    }
}

void VR::ProcessInput()
{
    BMVR_PROF("vr ProcessInput");
    if (!m_IsVREnabled || !m_ActionsReady.load(std::memory_order_acquire))
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
        ClearUseGrab();
        m_CrouchToggled = false;
        m_SprintLatched = false;
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

    UpdateButtonMap();
    UpdateVrRecoil();
    const bool menuStick = WeaponMenuStickHeld();
    UpdateWeaponMenu(menuStick, deltaMs);
    WelcomeStep(m_Game && SehIsPaused(m_Game->m_EngineClient));
    UpdateSettingsPanel();

    bool usedAnalogTurn = false;
    float turnX = 0.f, turnY = 0.f;
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
            turnX = analog.x;
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

    RefreshActiveWeaponModel();
    if (!paused && !MenuGameplayBlocked() && !m_WeaponMenuOpen && !m_EmptyHands)
        UpdateCrowbarMelee();
    if (!paused)
        SampleThrowHand();
    m_WearingHevSuit = m_Game->LocalPlayerHasSuit();
    m_HasHevSuit = !bmvr::g_HideHandsWithoutSuit || m_WearingHevSuit;
    const bool scopedWeapon = ViewmodelIsScoped();
    const bool rpgWeapon = ViewmodelIsRpg();
    if (!scopedWeapon)
        m_CrossbowZoomLatched = false;
    // Software toggle only when m_bLaserOn was not found. With the netvar the
    // game decides: both toggled on each press but drifted out of phase
    // (game laser off, latch on), so the dot stayed on with guidance off.
    if (!paused && rpgWeapon && !m_Game->RpgLaserOnResolved()
        && PressedDigitalAction(m_ActionSecondaryAttack, true))
        m_RpgLaserLatched = !m_RpgLaserLatched;
    if (!rpgWeapon || m_Game->RpgLaserOnResolved())
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
    // Once the engine's zoomed CViewSetup FOV has been seen it decides (the
    // m_bZooming read stays 0 on beta 26234270 while scoped, which cleared
    // the zoom NoteEngineScopeFov had just set, every frame).
    // NoteEngineScopeFov decides from the engine FOV ramp (see there).
    const bool engineZoom = m_SawEngineZoomFov && m_ScopeZoomActive;
    m_ScopeZoomActive = bmvr::g_ScopeUsesHmdAim && scopedWeapon
        && (m_SawEngineZoomFov ? engineZoom
            : (zoomFlag == 1 || (zoomFlag < 0 && m_CrossbowZoomLatched)));
    UpdateScopeAimOffset();
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
    // UpdateAimCrosshair / UpdateRpgLaserPoint run on the game thread
    // (TickMatQueueFromRenderView): this is the render thread, and the muzzle
    // lookup sets up viewmodel bones under the model-cache lock, which
    // deadlocked against the main thread's bone setup (2026-10-15).
    if (paused)
    {
        m_AimCrosshairValid = false;
        m_AimBeamValid = false;
        m_RpgLaserPointValid = false;
    }
    if (!paused)
    {
        UpdateWeaponFireHaptics();
        const uint32_t pendingFire = m_PendingFireHaptic.exchange(0, std::memory_order_acq_rel);
        if (pendingFire == 2)
            StartDryFireHaptic();
        else if (pendingFire)
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 2500, 0.85f);
    }

    {
        const int flags = (!paused && m_Game) ? m_Game->LocalPlayerFlags() : -1;
        const bool onTrain = flags >= 0 && (flags & (1 << 4)) != 0;
        if (onTrain != m_OnTrainControls)
        {
            static int s_trainLog;
            if (s_trainLog < 40)
            {
                const uint32_t held = m_HeldButtons.load(std::memory_order_acquire);
                Game::logMsg("Train controls %s: flags 0x%X (on ground %d), last buttons use %d fwd %d back %d left %d right %d jump %d",
                    onTrain ? "taken" : "released", flags < 0 ? 0 : flags, flags >= 0 ? (flags & 1) : -1,
                    (held & IN_USE) ? 1 : 0, (held & IN_FORWARD) ? 1 : 0, (held & IN_BACK) ? 1 : 0,
                    (held & IN_MOVELEFT) ? 1 : 0, (held & IN_MOVERIGHT) ? 1 : 0, (held & IN_JUMP) ? 1 : 0);
                ++s_trainLog;
            }
            m_OnTrainControls = onTrain;
        }
    }
    uint32_t buttons = 0;
    if (!paused && !MenuGameplayBlocked())
    {
        const bool primaryHeld = PressedDigitalAction(m_ActionPrimaryAttack);
        // No weapon at all (start of the game, drag scene) counts as bare
        // hands: the right-stick tap cannot holster then, so R2 stayed fire
        // with nothing to fire and only L2 could use.
        const bool emptyHands = m_EmptyHands || !m_HasHeldWeapon;
        bool leftUse = PressedDigitalAction(m_ActionUse);
        // Satchel (VrSatchelHands, user 2026-11-18): bag in the left hand,
        // detonator in the right. Left trigger = throw (IN_ATTACK, thrown
        // from the left hand on the release), right trigger = detonate
        // (alt-fire); the left grip is Use.
        bool satchelThrow = false;
        const bool satchelHands = SatchelHandsActive();
        if (satchelHands)
        {
            satchelThrow = leftUse;
            leftUse = LeftSupportGripValue() > 0.6f;
        }
        // The settings panel takes trigger, Cross and Circle (reload / alt-fire).
        // They stay blocked after it (or the weapon menu) closes until they
        // are released: the trigger/Circle press that closed the panel used
        // to fire or alt-fire on the next frame.
        static bool s_menuButtonsLatched;
        const bool menuButtonHeld = primaryHeld
            || PressedDigitalAction(m_ActionSecondaryAttack)
            || PressedDigitalAction(m_ActionReload)
            || PressedDigitalAction(m_ActionMenuSelect);
        if (m_WeaponMenuOpen || m_SettingsOpen)
            s_menuButtonsLatched = true;
        else if (!menuButtonHeld)
            s_menuButtonsLatched = false;
        const bool menuOwnsButtons = s_menuButtonsLatched;
        // On a mounted gun (or reaching for one with the right hand) R2 holds
        // the handles like L2, and secondary fire shoots the gun: the game
        // fires it on IN_ATTACK only.
        UpdateMountedGun();
        const bool gunGripRight = m_OnGun || m_GunRightNear;
        static bool s_detWas = false;
        if (!menuOwnsButtons && primaryHeld && !emptyHands && !IsCrowbarEquipped() && !gunGripRight)
        {
            buttons |= satchelHands ? IN_ATTACK2 : IN_ATTACK;
            if (satchelHands)
            {
                const DWORD nowMs = GetTickCount();
                MagReload::g_DetonateHeldMs.store(nowMs ? nowMs : 1, std::memory_order_relaxed);
                // Detonator click (user 2026-11-22): a buzz on the press.
                if (!s_detWas)
                    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 1500, 0.7f);
                s_detWas = true;
            }
            else
                s_detWas = true;
        }
        else
            s_detWas = false;
        // Carried prop (flare, can, crate; user 2026-11-24): alt-fire sends
        // the game's own flat throw, a fire press on the pickup controller
        // (fixed strength, along the usercmd angles, which follow the grab
        // hand while carrying). One short pulse per press, added to the final
        // usercmd in Hooks::dCreateMove (past the empty-hands fire strip and
        // the gun filters); never IN_ATTACK2 there, that drops the prop.
        const bool carrying = m_GrabLatched && GrabControllerRecentlyActive()
            && !m_OnGun && !m_OnTrainControls;
        static bool s_carryAltWas = false;
        const bool carryAlt = !menuOwnsButtons && carrying && PressedDigitalAction(m_ActionSecondaryAttack);
        if (carryAlt && !s_carryAltWas)
        {
            m_CarryThrowUntilMs.store(GetTickCount64() + kCarryThrowPulseMs, std::memory_order_release);
            static int s_carryThrowLogs;
            if (s_carryThrowLogs++ < 30)
                Game::logMsg("Carry throw: alt-fire while carrying (%s hand) -> fire pulse %u ms",
                    m_GrabHandLeft ? "left" : "right", static_cast<unsigned>(kCarryThrowPulseMs));
        }
        // Held on after the throw: no alt-fire for the gun until released.
        s_carryAltWas = carryAlt || (s_carryAltWas && PressedDigitalAction(m_ActionSecondaryAttack));
        if (!menuOwnsButtons && m_OnGun)
        {
            if (PressedDigitalAction(m_ActionSecondaryAttack))
                buttons |= IN_ATTACK;
        }
        else if (!menuOwnsButtons && !emptyHands && !s_carryAltWas && PressedDigitalAction(m_ActionSecondaryAttack))
            buttons |= IN_ATTACK2;
        if (UpdateScopeRaise(!menuOwnsButtons && !emptyHands && !m_OnGun && !paused))
            buttons |= IN_ATTACK2;
        // Right stick up = jump, down = crouch. Only past
        // StickJumpCrouchThreshold and within StickJumpCrouchAngle (default
        // 30) degrees of straight up or down, so the diagonal a turn passes through does not count. With
        // TurnBlocksJumpCrouch, a stick that went sideways (turn) does
        // neither until it is back near the centre. A started crouch lasts
        // until the stick is back near the centre (or pushed up), so sliding
        // it along the bottom edge to turn keeps the player crouched.
        static bool s_stickTurned;
        static bool s_stickCrouchHeld;
        bool stickJump = false, stickCrouch = false;
        // With button mapping the mapper owns stick up/down (Jump / Crouch
        // are then ordinary mapped actions).
        if (haveTurnY && !m_ButtonMapActive)
        {
            const float ax = fabsf(turnX), ay = fabsf(turnY);
            const bool centred = ax * ax + ay * ay < 0.25f * 0.25f;
            if (centred)
                s_stickTurned = false;
            else if (ax > 0.5f && ax > ay && !s_stickCrouchHeld)
                s_stickTurned = true;
            const float coneTan = tanf(bmvr::g_StickJumpCrouchAngle * 0.01745329f);
            const bool vertical = ay >= bmvr::g_StickJumpCrouchThreshold && ax <= ay * coneTan;
            if (vertical && !(bmvr::g_TurnBlocksJumpCrouch && s_stickTurned))
            {
                stickJump = turnY > 0.f;
                stickCrouch = turnY < 0.f;
            }
            if (stickCrouch)
                s_stickCrouchHeld = true;
            else if (centred || turnY >= 0.25f)
                s_stickCrouchHeld = false;
            if (s_stickCrouchHeld)
                stickCrouch = true;
        }
        else
        {
            s_stickTurned = false;
            s_stickCrouchHeld = false;
        }
        const bool actionJump = PressedDigitalAction(m_ActionJump);
        if (actionJump || stickJump)
            buttons |= IN_JUMP;
        {
            // One line per jump press: which path and the stick it saw.
            static bool s_jumpWas;
            static int s_jumpLogs;
            const bool jumpNow = actionJump || stickJump;
            if (jumpNow && !s_jumpWas && s_jumpLogs < 200)
            {
                ++s_jumpLogs;
                Game::logMsg("Jump press: %s stick=(%.2f,%.2f) %.0f deg off vertical (cone %.0f, distance %.2f, turned %d)",
                    actionJump ? "action" : "stick", turnX, turnY,
                    atan2f(fabsf(turnX), fabsf(turnY)) * 57.2958f,
                    bmvr::g_StickJumpCrouchAngle, bmvr::g_StickJumpCrouchThreshold, s_stickTurned ? 1 : 0);
            }
            s_jumpWas = jumpNow;
        }
        // Left trigger is Use. Right trigger is Use only while holstered or
        // without any weapon.
        // Hold to hold a pickup; release pulses a second Use to drop.
        m_UseFireHeld = (buttons & (IN_ATTACK | IN_ATTACK2)) != 0;
        bool rightUse = !menuOwnsButtons && (emptyHands || gunGripRight) && primaryHeld;
        // Hands-only ladders: on / climbing / mounting a ladder the triggers
        // are no Use at all. A whole-hand squeeze on a rung also pulls the
        // trigger; f05009d dropped that IN_USE for the game, but the Use grab
        // below still latched on it and turned the view to the grab hand
        // instead of the ladder's vertical view, and HL2's ladder move then
        // floated the player off toward a dismount (user 2026-11-20: pushed
        // off after a couple of rungs, the hand left its rung).
        if (bmvr::g_PhysicalLadders && !bmvr::g_LadderStickClimb
            && (m_LadderActive.load(std::memory_order_acquire)
                || (m_Game && m_Game->LocalPlayerMoveType() == 9)))
        {
            static bool s_ladderUseLogged = false;
            if ((leftUse || rightUse) && !s_ladderUseLogged)
                Game::logMsg("Ladder: trigger Use ignored while climbing (hands-only ladders)");
            s_ladderUseLogged = leftUse || rightUse;
            leftUse = rightUse = false;
        }
        m_GunHeldLeft = leftUse;
        m_GunHeldRight = rightUse;
        UpdateUseGrab(buttons, leftUse, rightUse);
        if (!menuOwnsButtons && PressedDigitalAction(m_ActionReload))
            buttons |= IN_RELOAD;
        {
            static bool s_throwWas = false;
            if (!menuOwnsButtons && satchelThrow)
            {
                buttons |= IN_ATTACK;
                if (!s_throwWas)
                    Game::logMsg("Satchel: left trigger held (throw on release)");
            }
            s_throwWas = satchelThrow;
        }
        if (PressedDigitalAction(m_ActionCrouch))
            buttons |= IN_DUCK;
        if (m_ButtonMapActive || !haveTurnY || turnY > -0.4f)
        {
            if (PressedDigitalAction(m_ActionCrouchToggle, true))
                m_CrouchToggled = !m_CrouchToggled;
        }
        if (stickCrouch)
        {
            if (bmvr::g_CrouchStickHold)
                buttons |= IN_DUCK;
            else if (!s_crouchAxis)
                m_CrouchToggled = !m_CrouchToggled;
            s_crouchAxis = true;
        }
        else
            s_crouchAxis = false;
        if (m_CrouchToggled)
            buttons |= IN_DUCK;
        UpdatePhysicalCrouch(buttons);
        if (m_ButtonMapActive ? m_SprintMapToggle : bmvr::g_SprintToggle)
        {
            // Click latches; letting the move stick back to centre (or a
            // second click) ends the sprint. The first 600 ms after the
            // click ignore the centred stick, so clicking before pushing
            // works too.
            static bool s_sprintHeld;
            static DWORD s_sprintSinceMs;
            const bool sprintHeld = PressedDigitalAction(m_ActionSprint);
            if (sprintHeld && !s_sprintHeld)
            {
                m_SprintLatched = !m_SprintLatched;
                s_sprintSinceMs = GetTickCount();
            }
            s_sprintHeld = sprintHeld;
            // Easing off the stick (below half deflection) also ends it, and
            // the trigger ends it at once: BM blocks firing while sprinting.
            if (m_SprintLatched && nx * nx + ny * ny < 0.5f * 0.5f
                && GetTickCount() - s_sprintSinceMs > 600)
                m_SprintLatched = false;
            // Only as fire: the right trigger as Use (carrying with the right
            // hand, empty hands) ended the sprint every frame (user 2026-11-24).
            if (primaryHeld && !rightUse)
                m_SprintLatched = false;
            if (m_SprintLatched)
                buttons |= IN_SPEED;
        }
        else if (PressedDigitalAction(m_ActionSprint))
            buttons |= IN_SPEED;
        if (GetTickCount() < m_MeleeAttackUntilMs)
            buttons |= IN_ATTACK;
        if (ny > 0.5f)
            buttons |= IN_FORWARD;
        else if (ny < -0.5f)
            buttons |= IN_BACK;
        // On train controls a sideways bit lets go of them: only a clear
        // sideways push counts there, not a stick held slightly off forward.
        const float sideThreshold = (m_OnTrainControls && (nx * nx < 0.85f * 0.85f || nx * nx < ny * ny)) ? 2.f : 0.5f;
        if (nx > sideThreshold)
            buttons |= IN_MOVERIGHT;
        else if (nx < -sideThreshold)
            buttons |= IN_MOVELEFT;
    }
    // Before the manual-magazine filter drops a pull on an empty gun: the
    // empty-trigger buzz (AfterCreateMoveFireHaptics) needs that pull.
    m_TriggerBeforeMagFilter.store((buttons & IN_ATTACK) != 0, std::memory_order_release);
    UpdateMagReload(buttons);
    UpdateGrenadePin(buttons);
    UpdateSnarkThrow(buttons);
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
    // FlashlightOnSquare: left Square (OpenXR left x/click = PrevItem) toggles
    // the flashlight instead of switching to the previous weapon.
    const bool prevRaw = PressedDigitalAction(m_ActionPrevItem);
    // With button mapping, FlashlightOnSquare only picks the default of that
    // button (Flashlight vs Previous weapon); the mapper already applied it.
    const bool squareSetting = bmvr::g_FlashlightOnSquare && !m_ButtonMapActive;
    const bool squareFlash = squareSetting && prevRaw;
    const bool prevHeld = squareSetting ? false : prevRaw;
    const bool pauseHeld = PressedDigitalAction(m_ActionPause);
    const bool resetHeld = PressedDigitalAction(m_ActionResetPosition);
    // Grab gestures (HL2VR style): shoulder, hip and forehead zones with a
    // slot per hand (left = action, right = weapon); UpdateShoulderGestures.
    UpdateBodyFrame();
    const bool rightShoulder = UpdateShoulderGestures(paused);
    m_GestureZoneRight = rightShoulder;
    UpdateLadderGrips();
    // Right grip on or near a ladder grabs it (PhysicalLadders): no
    // flashlight (left Square still toggles it).
    const bool rightGripGrab = rightShoulder
        || (!squareFlash && (m_LadderTouch[1].load(std::memory_order_acquire)
            || m_LadderActive.load(std::memory_order_acquire)));
    vr::InputDigitalActionData_t flash{};
    static bool s_flashLatched = false;
    static DWORD s_flashImpulseMs = 0;
    static int s_flashReleasePolls = 0;
    static DWORD s_flashWaitSince = 0;   // near-ladder press waiting (see below)
    const bool flashOk = GetDigitalActionData(m_ActionFlashlight, flash);
    if (flashOk || squareSetting)
    {
        const DWORD nowTick = GetTickCount();
        if ((flashOk && flash.bState) || squareFlash)
        {
            // Diagnostic (2026-11-16 headset: a press near a usable door sometimes did nothing):
            // a fresh press that does not queue the impulse says why.
            if (!s_flashLatched && (paused || m_GameUiVisible || rightShoulder || (nowTick - s_flashImpulseMs) <= 300))
            {
                static int s_flashBlockLog;
                if (s_flashBlockLog < 40)
                {
                    Game::logMsg("Flashlight press from=%s not queued: paused=%d gameUi=%d shoulder=%d sinceLast=%lums",
                        squareFlash ? "left square" : "right grip", paused ? 1 : 0, m_GameUiVisible ? 1 : 0,
                        rightShoulder ? 1 : 0, static_cast<unsigned long>(nowTick - s_flashImpulseMs));
                    ++s_flashBlockLog;
                }
            }
            s_flashReleasePolls = 0;
            // A grip squeezed for a shoulder/hip grab is still held when
            // the hand comes forward: that press must not toggle the light.
            if (rightGripGrab)
                s_flashLatched = true;
            // Near a ladder a right-grip press waits kLadderFlashWaitMs: a
            // ladder grab / mount starting meanwhile takes it (user
            // 2026-11-20: catching a rung while falling toggled it every
            // time; the touch flag alone missed those presses).
            constexpr DWORD kLadderFlashWaitMs = 100;
            const unsigned long long nearMs = m_LadderNearMs.load(std::memory_order_relaxed);
            const bool nearLadder = !squareFlash && bmvr::g_PhysicalLadders && nearMs != 0
                && GetTickCount64() - nearMs < 1500;
            bool flashGo = !paused && !m_GameUiVisible && !s_flashLatched && !rightGripGrab && (nowTick - s_flashImpulseMs) > 300;
            if (rightGripGrab)
            {
                if (s_flashWaitSince != 0)
                    Game::logMsg("Flashlight press near a ladder became a ladder grab after %lu ms: no flashlight",
                        static_cast<unsigned long>(nowTick - s_flashWaitSince));
                s_flashWaitSince = 0;
            }
            else if (flashGo && nearLadder)
            {
                if (s_flashWaitSince == 0)
                {
                    s_flashWaitSince = nowTick ? nowTick : 1;
                    // Why the touch flag did not catch it (diagnostic).
                    Game::logMsg("Flashlight press near a ladder: waiting %lu ms (touch %d active %d onLadder-move %d)",
                        static_cast<unsigned long>(kLadderFlashWaitMs), m_LadderTouch[1].load() ? 1 : 0,
                        m_LadderActive.load() ? 1 : 0, m_Game && m_Game->LocalPlayerMoveType() == 9 ? 1 : 0);
                }
                if (nowTick - s_flashWaitSince < kLadderFlashWaitMs)
                    flashGo = false;
            }
            if (flashGo)
                s_flashWaitSince = 0;
            if (flashGo)
            {
                m_PendingImpulse.store(100, std::memory_order_release);
                s_flashImpulseMs = nowTick;
                s_flashLatched = true;
                Game::logMsg("Flashlight queued impulse 100 (CreateMove only; vanilla ImpulseCommands) from=%s handle=%llu",
                    squareFlash ? "left square" : "right grip",
                    static_cast<unsigned long long>(m_ActionFlashlight));
            }
        }
        else
        {
            // A short tap near a ladder let go before the wait ran out, with
            // no grab: still a flashlight press.
            if (s_flashWaitSince != 0 && !rightGripGrab && !s_flashLatched && !paused && !m_GameUiVisible)
            {
                m_PendingImpulse.store(100, std::memory_order_release);
                s_flashImpulseMs = nowTick;
                s_flashLatched = true;
                Game::logMsg("Flashlight queued impulse 100 (tap near a ladder, no grab)");
            }
            s_flashWaitSince = 0;
            if (++s_flashReleasePolls >= 3)
                s_flashLatched = false;
        }
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
    // Triangle: tap = pause (on release), hold 0.6 s in gameplay = VR
    // settings panel. While a menu or the panel is up it toggles on press as
    // before, so leaving them stays immediate.
    static DWORD s_pauseDownMs;
    static bool s_pauseDeferred;
    static bool s_pauseHoldFired;
    constexpr DWORD kSettingsHoldMs = 600;
    // Mapped buttons resolve tap vs hold in the mapper (Pause on release,
    // VrSettings on hold), so the Pause action here is already a tap.
    const bool pauseHoldable = bmvr::g_SettingsTriangleHold && !m_ButtonMapActive
        && (!paused || m_WelcomeActive) && !IsMenuUp() && !m_SettingsOpen;
    if (m_GameplayEligible && pauseHeld && !s_pause)
    {
        s_pauseHoldFired = false;
        s_pauseDeferred = pauseHoldable;
        s_pauseDownMs = GetTickCount();
        if (m_SettingsOpen)
            CloseSettingsPanel("triangle");
        else if (!s_pauseDeferred)
        {
            Game::logMsg("Pause action edge paused=%d gameUi=%d handle=%llu",
                paused ? 1 : 0, IsMenuUp() ? 1 : 0,
                static_cast<unsigned long long>(m_ActionPause));
            if (bmvr::g_PauseCaptureArm && !IsMenuUp())
            {
                bmvr::g_PauseCaptureArm = false;
                m_PauseCapStartMs = GetTickCount();
                m_PauseCapEyes.store(12);
                m_PauseCapHud.store(12);
                Game::logMsg("Pause capture: next 12 eye pairs + 12 menu layers to VR/pausecap-*");
            }
            QueueGameUiToggle(paused);
        }
    }
    if (s_pauseDeferred && pauseHeld && !s_pauseHoldFired
        && GetTickCount() - s_pauseDownMs >= kSettingsHoldMs)
    {
        s_pauseHoldFired = true;
        s_pauseDeferred = false;
        if (m_GameplayEligible && pauseHoldable && !m_WeaponMenuOpen)
        {
            Game::logMsg("Triangle hold -> VR settings panel");
            OpenSettingsPanel();
        }
    }
    if (s_pauseDeferred && !pauseHeld && s_pause)
    {
        s_pauseDeferred = false;
        if (m_GameplayEligible)
        {
            Game::logMsg("Pause action tap paused=%d gameUi=%d handle=%llu",
                paused ? 1 : 0, IsMenuUp() ? 1 : 0,
                static_cast<unsigned long long>(m_ActionPause));
            QueueGameUiToggle(paused);
        }
    }
    const bool menuPanelUp = Want2dMenuPanel() || WantPauseWorldOverlay();
    m_MenuPanelUpForCursor.store(menuPanelUp, std::memory_order_relaxed);
    if (menuPanelUp)
    {
        ApplyMenuNavigation();
        ApplyMenuCursor();
    }
    else
        // Keep tracking the held trigger while no menu panel is up: resetting
        // to false made a trigger still held from gameplay (e.g. pausing
        // while firing) click as soon as the menu appeared. Same 0.9 / 0.5
        // latch as the menu click itself.
        m_MenuTriggerWasDown = MenuTriggerClickHeld(m_MenuTriggerLatch);
    // Right-stick tap is bare hands / last weapon in gameplay (weapon menu);
    // the weapon menu is off while a menu panel is up, so there the tap still
    // recenters (main menu and loading have no VR settings panel).
    if (menuPanelUp && resetHeld && !s_reset)
        Recenter("right-stick tap in a menu");
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
    {
        // The hand that pressed: Use is the off hand's trigger; bare hands also use
        // with the aim hand's trigger. (Always went to the aim hand before.)
        const bool offHandUse = PressedDigitalAction(m_ActionUse);
        const auto aimRole = AimControllerRole();
        const auto offRole = aimRole == vr::TrackedControllerRole_LeftHand
            ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand;
        PulseHandHaptic(offHandUse ? offRole : aimRole, 900, 0.6f);
    }
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
    if (m_CreatedVRTextures.load(std::memory_order_acquire)
        && m_D9LeftEyeSurface && m_D9RightEyeSurface
        && m_VKLeftEye.m_VulkanData.m_nWidth >= 640
        && m_VKLeftEye.m_VulkanData.m_nHeight >= 360)
    {
        const UINT haveW = m_VKLeftEye.m_VulkanData.m_nWidth;
        const UINT haveH = m_VKLeftEye.m_VulkanData.m_nHeight;
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
        if (!wantOffscreenResize)
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
    BMVR_PROF("vr GetViewAngle");
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    const QAngle& ang = m_StereoFramePoseActive ? m_StereoFrameAngles : m_HmdAngAbs;
    const float offset = m_RotationOffsetY.load(std::memory_order_acquire);
    float pitch = ang.x;
    float yaw = ang.y;
    float roll = ang.z;
    // OpenXR submit publishes the raw HMD pose. Smoothing only the Source
    // camera (not that pose) looks live for one frame, then the compositor
    // reprojects a mismatched image and the scope picture freezes until unscope.
    if (m_ScopeZoomActive && m_ZoomSmoothValid && !m_OpenXrHelperBridgeActive)
    {
        pitch = m_ZoomSmoothPitch;
        yaw = m_ZoomSmoothYaw;
        roll = m_ZoomSmoothRoll;
    }
    yaw += offset;
    yaw -= 360.f * floorf((yaw + 180.f) / 360.f);
    // ScopeFollowsGun: look along the (smoothed) gun aim. The head only adds
    // its roll, so the horizon still tilts with it. The image is rendered for
    // the current head pose like any other frame, so reprojection stays right.
    if (m_ScopeZoomActive && m_ScopeGunDirValid)
    {
        Vector f{}, r{}, u{};
        QAngle::AngleVectors(QAngle(pitch, yaw, roll), &f, &r, &u);
        // Keep "up" perpendicular to the view direction; when the aim is far
        // off the head's up axis this is the head's up, tilted along.
        Vector up = u - m_ScopeGunDir * DotProduct(u, m_ScopeGunDir);
        if (VectorNormalize(up) < 0.05f)
            up = u;
        QAngle out{};
        QAngle::VectorAngles(m_ScopeGunDir, up, out);
        if (std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z))
        {
            pitch = out.x;
            yaw = out.y;
            roll = out.z;
            if (pitch > 180.f) pitch -= 360.f;
            if (pitch < -180.f) pitch += 360.f;
            yaw -= 360.f * floorf((yaw + 180.f) / 360.f);
        }
        return Vector(pitch, yaw, roll);
    }
    // Zoomed: look along the laser direction captured at zoom start, kept
    // relative to the head. The offset is constant while zoomed, so the
    // compositor's reprojection (raw HMD pose) still sees only head motion.
    if (m_ScopeZoomActive && m_ScopeAimLocalValid)
    {
        Vector f{}, r{}, u{};
        QAngle::AngleVectors(QAngle(pitch, yaw, roll), &f, &r, &u);
        const Vector& d = m_ScopeAimLocal;
        const Vector dir = f * d.x + r * d.y + u * d.z;
        QAngle out{};
        QAngle::VectorAngles(dir, u, out);
        if (std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z))
        {
            pitch = out.x;
            yaw = out.y;
            roll = out.z;
            if (pitch > 180.f) pitch -= 360.f;
            if (pitch < -180.f) pitch += 360.f;
            yaw -= 360.f * floorf((yaw + 180.f) / 360.f);
        }
    }
    // L4D2VR GetViewAngle keeps HMD roll. Zeroing it here while OpenVR
    // Submit uses the full pose makes a head tilt rotate the world.
    return Vector(pitch, yaw, roll);
}

void VR::UpdateScopeAimOffset()
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (m_ScopeZoomActive && bmvr::g_ScopeFollowsGun)
    {
        m_ScopeAimLocalValid = false;
        Vector aim{};
        QAngle::AngleVectors(GetAimAngles(), &aim, nullptr, nullptr);
        if (VectorNormalize(aim) < 0.01f)
            return;
        const double nowMs = static_cast<double>(GetTickCount64());
        const float tau = bmvr::g_ScopeGunSmoothSec;
        if (!m_ScopeGunDirValid || !(tau > 0.005f))
        {
            if (!m_ScopeGunDirValid)
                Game::logMsg("Scope follows gun: on (smoothing %.2f s)", tau);
            m_ScopeGunDir = aim;
            m_ScopeGunDirValid = true;
            m_ScopeGunDirMs = nowMs;
            return;
        }
        // Called several times per frame: integrate over real time only.
        float dt = static_cast<float>((nowMs - m_ScopeGunDirMs) * 0.001);
        if (dt <= 0.f)
            return;
        m_ScopeGunDirMs = nowMs;
        if (dt > 0.25f)
            dt = 0.25f;
        const float a = 1.f - expf(-dt / tau);
        Vector d = m_ScopeGunDir + (aim - m_ScopeGunDir) * a;
        if (VectorNormalize(d) > 0.01f)
            m_ScopeGunDir = d;
        return;
    }
    if (m_ScopeGunDirValid)
        Game::logMsg("Scope follows gun: off");
    m_ScopeGunDirValid = false;
    if (!m_ScopeZoomActive || !bmvr::g_ScopeKeepsAimOffset)
    {
        m_ScopeAimLocalValid = false;
        return;
    }
    if (m_ScopeAimLocalValid)
        return;
    // Head basis without the offset (still invalid here) and the laser's
    // direction, which comes from the controller and the per-weapon sight.
    const Vector va = GetViewAngle();
    Vector f{}, r{}, u{};
    QAngle::AngleVectors(QAngle(va.x, va.y, va.z), &f, &r, &u);
    Vector aim{};
    QAngle::AngleVectors(GetAimAngles(), &aim, nullptr, nullptr);
    if (VectorNormalize(aim) < 0.01f)
        return;
    Vector local(DotProduct(aim, f), DotProduct(aim, r), DotProduct(aim, u));
    // A laser pointing behind or far off to the side (gun held down, hand
    // out of view) would swing the scope away from the head: stay centred.
    if (local.x < 0.5f)
        local = Vector(1.f, 0.f, 0.f);
    m_ScopeAimLocal = local;
    m_ScopeAimLocalValid = true;
    Game::logMsg("Scope aim offset: local (%.3f %.3f %.3f) = %.1f deg off head forward",
        local.x, local.y, local.z,
        acosf((std::min)(1.f, (std::max)(-1.f, local.x))) * (180.f / 3.14159265358979323846f));
}

bool VR::UpdateScopeRaise(bool allowed)
{
    // Black Mesa's crossbow zooms while alt-fire is held, so this holds it
    // for as long as the crossbow is at the face. Returns true while held.
    if (!bmvr::g_ScopeRaiseToZoom || !allowed || !ViewmodelIsScoped())
    {
        m_ScopeRaised = false;
        return false;
    }
    float dist = 1e9f;
    bool twoHand = false;
    QAngle head{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
        if (m_RightControllerTrackingValid)
            dist = (m_RightControllerPosAbs - m_HmdPosAbs).Length() / scale;
        twoHand = m_TwoHandActive && !m_VmClass.offhandPistol;
        head = m_HmdAngAbs;
    }
    // Gun aim vs where the head looks (raw HMD angles, not the zoomed view).
    head.y += m_RotationOffsetY.load(std::memory_order_acquire);
    Vector hf{}, af{};
    QAngle::AngleVectors(head, &hf, nullptr, nullptr);
    QAngle::AngleVectors(GetAimAngles(), &af, nullptr, nullptr);
    const float aimDot = DotProduct(hf, af);
    const float enter = std::clamp(bmvr::g_ScopeRaiseMeters, 0.1f, 0.6f);
    const float leave = enter + 0.10f;
    const bool was = m_ScopeRaised;
    if (!m_ScopeRaised)
        // Two hands on the crossbow, at the face, pointing within ~35 deg
        // of where the head looks.
        m_ScopeRaised = twoHand && dist < enter && aimDot > 0.82f;
    else
        // Out only by distance: letting go with the support hand or looking
        // around while scoped keeps the zoom.
        m_ScopeRaised = dist < leave;
    if (m_ScopeRaised != was)
        Game::logMsg("Scope raise: %s (hand %.2f m from the head, aim %.0f deg off the head, two-hand %d)",
            m_ScopeRaised ? "at the face, zoom held" : "away from the face, zoom released", dist,
            acosf(std::clamp(aimDot, -1.f, 1.f)) * (180.f / 3.14159265358979323846f),
            twoHand ? 1 : 0);
    return m_ScopeRaised;
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
    center.z += bmvr::g_HeightOffsetCm * 0.01f * m_VRScale;   // cm -> game units
    return center;
}

Vector VR::GetViewOrigin(const Vector& setupOrigin) const
{
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return GetViewOriginUnshifted(setupOrigin) + (fwd * (-(m_EyeZ * m_VRScale)));
}

float VR::RenderHalfIpd() const
{
    if (m_ScopeZoomActive && bmvr::g_ScopeMonoZoom)
        return 0.f;
    return m_Ipd * m_IpdScale * m_VRScale * 0.5f;
}

Vector VR::GetViewOriginLeft(const Vector& setupOrigin) const
{
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return GetViewOrigin(setupOrigin) - (right * RenderHalfIpd());
}

Vector VR::GetViewOriginRight(const Vector& setupOrigin) const
{
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    return GetViewOrigin(setupOrigin) + (right * RenderHalfIpd());
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
    {
        // Diag: in gameplay this fallback puts gun/laser/gloves at the eye.
        // Counted so a blinking "pose valid" flag shows in the log.
        static std::atomic<int> s_eyeFallbacks{ 0 };
        static std::atomic<DWORD> s_lastLogMs{ 0 };
        if (m_GameplayEligible && m_HmdOriginLatched)
        {
            const int n = ++s_eyeFallbacks;
            const DWORD now = GetTickCount();
            DWORD last = s_lastLogMs.load(std::memory_order_relaxed);
            if (now - last > 2000 && s_lastLogMs.compare_exchange_strong(last, now))
                Game::logMsg("Controller pose fallback to the eye: %d so far (thread %lu, noblink=%d)",
                    n, GetCurrentThreadId(), bmvr::g_PoseFlagsNoBlink ? 1 : 0);
        }
        return GetViewOrigin(setupOrigin);
    }

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
        world.z += bmvr::g_HeightOffsetCm * 0.01f * m_VRScale;
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
    QAngle aim{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (m_TwoHandActive && !m_VmClass.offhandPistol)
        {
            // m_Viewmodel* is the model's basis, which carries the per-weapon
            // pose angles (ApplyViewmodelBasisOffsets: yaw ay, pitch ax, roll
            // az) on top of the aim. Undo them in reverse order so two-handed
            // aim matches one-handed aim (shotgun ay=-4 put the dot right of
            // the barrel, 2026-10-15).
            float ox = 0.f, oy = 0.f, oz = 0.f, ax = 0.f, ay = 0.f, az = 0.f;
            ResolveWeaponViewmodelPose(ox, oy, oz, ax, ay, az);
            Vector f = m_ViewmodelForward, r = m_ViewmodelRight, u = m_ViewmodelUp;
            r = VectorRotate(r, f, -az);
            u = VectorRotate(u, f, -az);
            f = VectorRotate(f, r, -ax);
            u = VectorRotate(u, r, -ax);
            f = VectorRotate(f, u, -ay);
            QAngle::VectorAngles(f, u, aim);
            aim.x -= bmvr::g_AimPitchOffset;
            if (aim.x > 180.f) aim.x -= 360.f;
            if (aim.x < -180.f) aim.x += 360.f;
            if (aim.x > 89.f) aim.x = 89.f;
            if (aim.x < -89.f) aim.x = -89.f;
            aim.z = 0.f;
            return aim;
        }
    }
    aim = GetRightControllerAbsAngle();
    // Tripmine (bmvr_tripmine_face_aim): placed out of the mine's back. Its
    // sensor faces along the controller's up axis in the hand; held as it
    // goes on the wall (back to the wall, sensor toward the player) it was
    // not placed (user 2026-11-19: only a controller pointed at the wall,
    // sensor up, placed it).
    if (bmvr::g_TripmineFaceAim && m_VmClass.numpadKey == "tripmine")
    {
        Vector cf, cr, cu;
        QAngle::AngleVectors(aim, &cf, &cr, &cu);
        QAngle::VectorAngles(cu * -1.f, cf, aim);
        if (aim.x > 180.f) aim.x -= 360.f;
        if (aim.x > 89.f) aim.x = 89.f;
        if (aim.x < -89.f) aim.x = -89.f;
        aim.z = 0.f;
        return aim;
    }
    // Trim the shot direction only. The viewmodel keeps its tuned pose, so a
    // grip that consistently lands low is corrected without rotating models.
    aim.x -= bmvr::g_AimPitchOffset;
    // VR recoil: up = negative pitch (the viewmodel basis gets the same kick).
    aim.x -= m_RecoilKickDeg.load(std::memory_order_relaxed);
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

bool VR::LadderMode(int& moveBits, bool& onLadder, float* mountYaw) const
{
    moveBits = 0;
    onLadder = false;
    if (mountYaw)
        *mountYaw = std::numeric_limits<float>::quiet_NaN();
    if (!m_Game)
        return false;
    constexpr int kMoveTypeLadder = 9;
    constexpr float kStickDeflection = 0.2f;
    const float f = m_WalkForward.load(std::memory_order_acquire);
    const float s = m_WalkSide.load(std::memory_order_acquire);
    bool ladder = m_Game->LocalPlayerMoveType() == kMoveTypeLadder;
    onLadder = ladder;
    Vector origin{};
    if (!ladder && f * f + s * s > kStickDeflection * kStickDeflection
        && m_Game->LocalPlayerAbsOrigin(origin))
    {
        // BM's ladders are func_useableladders: no ladder brush, just a line
        // between two mount points. HL2 auto-mounts within 32 units of it
        // (CheckLadderAutoMountCone) when the view faces it within 15
        // degrees flat and the move has no sideways part (any sidemove
        // counts as strafing; then only looking steeply up or down the
        // ladder axis mounts). With the gun's angles that rarely held, so
        // the head's angles take over from 48 units.
        constexpr float kNearHu = 48.f;
        Vector bottoms[32], tops[32];
        const int n = m_Game->CollectLadders(bottoms, tops, 32);
        float bestDist2 = kNearHu * kNearHu;
        Vector bestClosest{};
        float bestT = 0.f;
        for (int i = 0; i < n; ++i)
        {
            const Vector axis = tops[i] - bottoms[i];
            const float len2 = axis.LengthSqr();
            float t = len2 > 1.f ? (origin - bottoms[i]).Dot(axis) / len2 : 0.f;
            t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
            const Vector closest = bottoms[i] + axis * t;
            const float d2 = (closest - origin).LengthSqr();
            if (d2 < bestDist2)
            {
                bestDist2 = d2;
                bestClosest = closest;
                bestT = t;
                ladder = true;
            }
        }
        static int s_nearLog;
        if (ladder && s_nearLog < 8)
        {
            Game::logMsg("Ladder near (%d ladders on the map, %.0f units)", n, sqrtf(bestDist2));
            ++s_nearLog;
        }
        // Mount assist: the head still had to face the ladder within 15
        // degrees and the stick had to be dead straight, which took a few
        // tries at the bottom and while swimming. Inside the engine's
        // 32-unit range, a stick pointing within kMountConeDeg of the ladder
        // (in the head's frame) sends the view straight at it with a pure
        // forward move, so that tick's auto-mount check passes.
        // Only on the lower half of the ladder (from the bottom, or swimming
        // up to it): at the top the closest point is the top end, so walking
        // anywhere near the hole within 50 degrees mounted the ladder; there
        // the engine's own check (face it within 15 degrees) applies.
        constexpr float kMountHu = 30.f;   // engine: dist < 32
        constexpr float kMountConeDeg = 50.f;
        constexpr float kMountMaxAlong = 0.5f;   // fraction of the ladder from the bottom
        const float flatX = bestClosest.x - origin.x;
        const float flatY = bestClosest.y - origin.y;
        const float flat2 = flatX * flatX + flatY * flatY;
        if (ladder && mountYaw && bestDist2 < kMountHu * kMountHu && flat2 > 1.f && bestT <= kMountMaxAlong)
        {
            const float toLadderYaw = atan2f(flatY, flatX) * 180.f / 3.14159265f;
            // Stick direction in world yaw: head yaw, stick right turns
            // clockwise (Source yaw grows to the left).
            const float stickYaw = GetViewAngle().y - atan2f(s, f) * 180.f / 3.14159265f;
            float delta = std::fmod(stickYaw - toLadderYaw, 360.f);
            if (delta > 180.f) delta -= 360.f;
            if (delta < -180.f) delta += 360.f;
            if (std::fabs(delta) < kMountConeDeg)
            {
                *mountYaw = toLadderYaw;
                moveBits = IN_FORWARD;
                static int s_assistLog;
                if (s_assistLog < 16)
                {
                    Game::logMsg("Ladder mount assist: %.0f units, stick %.0f deg off the ladder, %.0f%% up it",
                        sqrtf(bestDist2), delta, bestT * 100.f);
                    ++s_assistLog;
                }
                return true;
            }
        }
    }
    if (!ladder)
        return false;
    if (f > kStickDeflection)
        moveBits |= IN_FORWARD;
    else if (f < -kStickDeflection)
        moveBits |= IN_BACK;
    if (s > kStickDeflection)
        moveBits |= IN_MOVERIGHT;
    else if (s < -kStickDeflection)
        moveBits |= IN_MOVELEFT;
    return true;
}

void VR::UpdateLadderGrips()
{
    // Raw grip (protocol 18) when the helper publishes it: the action slots
    // may carry whatever the button mapping put there.
    auto gripValue = [&](int hand, vr::VRActionHandle_t h) {
        const L4D2VROpenXrRawHandDesc& r = m_OpenXrLastInputState.rawHands[hand];
        if (m_RuntimeBackend == VrRuntimeBackend::OpenXR && r.valid)
            return r.grip;
        vr::InputAnalogActionData_t a{};
        if (GetAnalogActionData(h, a))
            return a.x;
        return PressedDigitalAction(h) ? 1.f : 0.f;
    };
    const float g[2] = { gripValue(0, m_ActionInventoryGripLeft), gripValue(1, m_ActionFlashlight) };
    for (int h = 0; h < 2; ++h)
    {
        const bool was = m_LadderGrip[h].load(std::memory_order_relaxed);
        m_LadderGrip[h].store(g[h] >= (was ? 0.45f : 0.55f), std::memory_order_release);
    }
}

bool VR::PhysicalLadderStep(PhysicalLadderCmd& out)
{
    out = {};
    // Climb state. Climbing = the player moves along the ladder by what the
    // climb hand moved down in tracking space since it grabbed, so the hand
    // stays where it took hold. LadderMove (HL2 func_useableladder) only
    // knows a fixed 200 u/s up or down per tick, so each tick goes one step
    // up, one down or holds, whichever keeps the sent total closest to the
    // hand. Approach after HL2VR (hl_gamemovement.cpp LadderMove, roomscale
    // ladders: velocity from the climb hand's delta), done in the usercmd
    // because the mod does not run the movement code.
    static int s_climbHand = -1;
    static float s_anchorHandZ = 0.f;
    static float s_anchorOriginZ = 0.f;
    static float s_sent = 0.f;       // along the ladder, units
    static bool s_anchored = false;  // set once on the ladder
    static bool s_prevGrip[2] = {};
    static ULONGLONG s_mountUntil = 0;
    static bool s_mountUseSent = false;
    static ULONGLONG s_mountStart = 0;
    static int s_mountLadder = -1;
    // Walk-on mounting only while a hand mount runs (every exit path).
    if (s_mountUntil == 0)
        m_LadderMountActive.store(false, std::memory_order_relaxed);
    static float s_mountYaw = 0.f;
    static ULONGLONG s_dropAt = 0;   // both hands let go: fall off at this time
    static bool s_autoHolstered = false;
    static ULONGLONG s_offLadderSince = 0;
    constexpr float kRungSnapHu = 14.f;    // grab reach to a rung
    constexpr float kRungLeaveHu = 24.f;   // hand this far off its rung lets go of it

    // Smooth view (PhysicalLadderViewOffset): on only while a hand climbs.
    struct ViewOff
    {
        LadderViewState& v;
        bool climbing = false;
        ~ViewOff() { if (!climbing) v.active = false; }
    } viewOff{ m_LadderView };
    auto clearTouch = [&]() {
        m_LadderActive.store(false, std::memory_order_release);
        m_LadderTouch[0].store(false, std::memory_order_release);
        m_LadderTouch[1].store(false, std::memory_order_release);
    };
    if (!bmvr::g_PhysicalLadders || !m_Game)
    {
        s_climbHand = -1;
        s_mountUntil = 0;
        m_LadderPin[0].valid = m_LadderPin[1].valid = false;
        m_LadderActive.store(false, std::memory_order_release);
        clearTouch();
        return false;
    }
    Vector origin{};
    if (!m_Game->LocalPlayerAbsOrigin(origin))
    {
        clearTouch();
        return false;
    }
    constexpr int kMoveTypeLadder = 9;
    const bool onLadder = m_Game->LocalPlayerMoveType() == kMoveTypeLadder;
    out.onLadder = onLadder;

    Vector bottoms[32], tops[32];
    const int n = m_Game->CollectLadders(bottoms, tops, 32);
    if (n <= 0)
    {
        s_climbHand = -1;
        clearTouch();
        return false;
    }

    // Hands: world position (touch test) and tracking height (climb).
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    bool valid[2]{};
    Vector world[2]{};
    float trackZ[2]{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        valid[0] = m_LeftControllerTrackingValid;
        valid[1] = m_PhysicalRightTrackingValid;
        if (valid[0])
        {
            world[0] = ControllerTrackingToWorld(body, m_LeftControllerPosAbs);
            trackZ[0] = m_LeftControllerPosAbs.z;
        }
        if (valid[1])
        {
            world[1] = ControllerTrackingToWorld(body, m_PhysicalRightPosAbs);
            trackZ[1] = m_PhysicalRightPosAbs.z;
        }
    }

    // A ladder here is only the line between its two player mount points
    // (feet positions); the rungs are a little in front of it. A hand
    // within kTouchHu of that line sideways, from just under the bottom to
    // above the top (reaching over the top end), is on the ladder.
    constexpr float kTouchHu = 36.f;
    auto nearestLadder = [&](const Vector& p, float& flatDist, float& yawTo) {
        int best = -1;
        float bestD2 = 1e30f;
        for (int i = 0; i < n; ++i)
        {
            const Vector axis = tops[i] - bottoms[i];
            const float len2 = axis.LengthSqr();
            float t = len2 > 1.f ? (p - bottoms[i]).Dot(axis) / len2 : 0.f;
            t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
            const Vector c = bottoms[i] + axis * t;
            const float dx = c.x - p.x, dy = c.y - p.y;
            const float d2 = dx * dx + dy * dy;
            const float zLo = (bottoms[i].z < tops[i].z ? bottoms[i].z : tops[i].z) - 16.f;
            const float zHi = (bottoms[i].z > tops[i].z ? bottoms[i].z : tops[i].z) + 80.f;
            if (p.z < zLo || p.z > zHi || d2 >= bestD2)
                continue;
            bestD2 = d2;
            best = i;
            yawTo = atan2f(dy, dx) * 180.f / 3.14159265f;
        }
        flatDist = best >= 0 ? sqrtf(bestD2) : 1e15f;
        return best;
    };
    const ULONGLONG now = GetTickCount64();
    {
        float dn = 0.f, yn = 0.f;
        if (nearestLadder(origin, dn, yn) >= 0 && dn < 96.f)
            m_LadderNearMs.store(now, std::memory_order_relaxed);
    }
    bool grip[2];
    for (int h = 0; h < 2; ++h)
        grip[h] = m_LadderGrip[h].load(std::memory_order_acquire) && valid[h];
    // The gun hand climbs empty (LadderGunHand): 0 = a grab holsters the
    // weapon (back when off the ladder), 1 = only an empty hand grabs,
    // 2 = a grab holsters it and it stays holstered.
    const int gunHand = bmvr::g_LeftHanded ? 0 : 1;
    const bool gunOut = m_HasHeldWeapon && !m_EmptyHands;
    const int gunHandMode = static_cast<int>(bmvr::g_LadderGunHand + 0.5f);
    // Touch = the hand at a rung of the map's ladder props (user 2026-11-19:
    // a grab from a metre away while falling past a ladder caught it).
    // Ladders without rung props (a few, e.g. elevator shafts) keep a short
    // reach to the mount line. lineTouch (old 36 u reach) is only for a hand
    // that already holds on taking over the climb.
    constexpr float kRungTouchHu = 12.f;
    constexpr float kNoRungTouchHu = 20.f;
    constexpr float kRungPropNearHu = 48.f;
    // Hand this close to a rung: grip is no flashlight. 24 u still let
    // catches after a fall toggle it (the hand closes in several units per
    // tick); user 2026-11-20: fine to have no grip flashlight anywhere in
    // the ladder's grab zone.
    constexpr float kFlashGuardHu = 40.f;
    bool touch[2]{};
    bool lineTouch[2]{};
    for (int h = 0; h < 2; ++h)
    {
        float d = 0.f, yaw = 0.f;
        lineTouch[h] = valid[h] && nearestLadder(world[h], d, yaw) >= 0 && d < kTouchHu;
        if (lineTouch[h])
        {
            Vector onRung{};
            float rungDist = 1e9f;
            if (bmvr::ladders::NearestRung(m_CurrentMapName.c_str(), world[h], kRungPropNearHu, onRung, &rungDist))
                touch[h] = rungDist < kRungTouchHu;
            else
                touch[h] = d < kNoRungTouchHu;
            static int s_missLog;
            if (!touch[h] && grip[h] && !s_prevGrip[h] && s_missLog < 40)
            {
                ++s_missLog;
                Game::logMsg("Ladder grab: %s hand not at a rung (rung %.1f, line %.1f units; hand %.0f,%.0f,%.0f rung %.0f,%.0f,%.0f)",
                    h == 0 ? "left" : "right", rungDist, d, world[h].x, world[h].y, world[h].z, onRung.x, onRung.y, onRung.z);
            }
        }
        const bool holding = grip[h] && s_prevGrip[h] && h == s_climbHand;
        // The gun hand with "needs empty hand": its grip stays the mapped
        // action. The support hand at the gun (two-hand grab): the gun
        // takes the grip.
        const bool blocked = !holding && gunOut
            && ((h == gunHand && gunHandMode == 1)
                || (h != gunHand && m_SupportNearGun.load(std::memory_order_acquire)));
        if (blocked)
            touch[h] = lineTouch[h] = false;
        // Flashlight guard: a grip with the hand within kFlashGuardHu of a
        // rung is a ladder grab (or a near miss), never the flashlight.
        // Wider than the grab reach because the mapper judges the press
        // from the last tick's flag while the hand is still closing in
        // (user 2026-11-20, bm_c1a0b: grabs 8-11 u from the rung and misses
        // at 14 u toggled it). The old mount-line reach was dropped: on
        // that ladder the line is 24 u behind the rungs, so it added
        // nothing. Ladders without rung props keep a line reach. Not for
        // grabbing; a hand away from the rungs keeps the flashlight grip.
        bool nearRung = false;
        if (valid[h] && !blocked)
        {
            Vector r{};
            float rd = 1e9f;
            if (bmvr::ladders::NearestRung(m_CurrentMapName.c_str(), world[h], kFlashGuardHu, r, &rd))
                nearRung = true;
            else if (lineTouch[h])
            {
                float dl = 0.f, yl = 0.f;
                Vector r2{};
                nearRung = !bmvr::ladders::NearestRung(m_CurrentMapName.c_str(), world[h], kRungPropNearHu, r2)
                    && nearestLadder(world[h], dl, yl) >= 0 && dl < kNoRungTouchHu + 8.f;
            }
        }
        m_LadderTouch[h].store(touch[h] || nearRung, std::memory_order_release);
    }
    // Grips on a ladder are ladder grabs, not the mapped action
    // (flashlight): on it, climbing, mounting, and for a moment after
    // leaving it (letting go at the top). Standing in front of one is not
    // enough any more (user 2026-11-19: the flashlight grip did nothing
    // there); a hand at a rung is (m_LadderTouch).
    {
        static ULONGLONG s_activeUntil = 0;
        const ULONGLONG tNow = GetTickCount64();
        if (onLadder || s_climbHand >= 0 || s_mountUntil != 0)
        {
            s_activeUntil = tNow + 500;
        }
        m_LadderActive.store(tNow < s_activeUntil, std::memory_order_release);
    }

    auto role = [](int h) {
        return h == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
    };
    // Grip pressed on the ladder: that hand climbs (the newest grab wins,
    // like HL2VR's SetClimbHand).
    for (int h = 0; h < 2; ++h)
    {
        if (grip[h] && !s_prevGrip[h] && touch[h])
        {
            s_climbHand = h;
            s_anchored = false;
            PulseHandHaptic(role(h), 1500, 0.4f);
            if (h == gunHand && gunOut)
            {
                m_EmptyHands = true;
                s_autoHolstered = gunHandMode == 0;   // 0: drawn again off the ladder
                Game::logMsg("Ladder grab: weapon holstered for the climb%s", gunHandMode == 0 ? "" : " (stays holstered)");
            }
            // Put the hand on the nearest rung of the ladder props.
            Vector onRung{};
            float rungDist = 0.f;
            Vector rungAxis{};
            int rungModel = -1;
            m_LadderPin[h].valid = bmvr::ladders::NearestRung(m_CurrentMapName.c_str(), world[h], kRungSnapHu, onRung,
                &rungDist, &rungAxis, &rungModel);
            m_LadderPin[h].world = onRung;
            m_LadderPin[h].rung = onRung;
            m_LadderPin[h].hasAxis = false;
            m_LadderPin[h].model = rungModel;
            m_LadderPin[h].face = Vector(0.f, 0.f, 0.f);
            if (m_LadderPin[h].valid)
            {
                Vector fc = onRung - origin;
                fc.z = 0.f;
                if (VectorNormalize(fc) > 0.1f)
                    m_LadderPin[h].face = fc;
                ladder_tune::NoteHeld(rungModel);
            }
            if (m_LadderPin[h].valid)
            {
                // Overhand grip: the thumbs point inward, the left one to the
                // player's right as seen facing the ladder. Facing = player
                // to rung, flattened.
                Vector face = onRung - origin;
                face.z = 0.f;
                Vector ax = rungAxis;
                ax.z = 0.f;
                if (VectorNormalize(face) > 0.1f && VectorNormalize(ax) > 0.1f)
                {
                    const Vector rightOfFace(face.y, -face.x, 0.f);   // face x up
                    const float side = ax.Dot(rightOfFace);
                    m_LadderPin[h].axis = (h == 0) == (side > 0.f) ? ax : ax * -1.f;
                    m_LadderPin[h].hasAxis = true;
                }
            }
            if (m_LadderPin[h].valid)
            {
                // Glove a little in front of the rung (towards the player's
                // side of the ladder), so the fist closes round it rather
                // than through it.
                float d = 0.f, yawToLine = 0.f;
                if (nearestLadder(onRung, d, yawToLine) >= 0 && d > 0.5f)
                {
                    const float r = yawToLine * 3.14159265f / 180.f;
                    m_LadderPin[h].world += Vector(cosf(r), sinf(r), 0.f) * bmvr::g_LadderHandBack;
                }
            }
            if (m_LadderPin[h].valid)
                Game::logMsg("Ladder grab: %s hand on a rung %.1f units away (%.0f,%.0f,%.0f)",
                    h == 0 ? "left" : "right", rungDist, onRung.x, onRung.y, onRung.z);
            if (!onLadder && s_mountUntil == 0)
            {
                // Mount like a +use at the ladder: HL2's LadderMove mounts
                // on a fresh IN_USE when the view faces a ladder within 64.
                float d = 0.f;
                float yaw = 0.f;
                const int li = nearestLadder(origin, d, yaw);
                if (d < 64.f)
                {
                    s_mountLadder = li;
                    // 1.5 s of use presses (was one in 0.6 s): in moving
                    // water the current carried the player out of reach of
                    // the single press (user 2026-11-19, bm_c2a5a; stick
                    // mounting, which keeps walking in, worked there).
                    s_mountUntil = now + 1500;
                    s_mountStart = now;
                    s_mountUseSent = false;
                    s_mountYaw = yaw;
                }
            }
            Game::logMsg("Ladder grab: %s hand%s", h == 0 ? "left" : "right", onLadder ? " (on the ladder)" : ", mounting");
        }
    }
    for (int h = 0; h < 2; ++h)
    {
        s_prevGrip[h] = grip[h];
        // A hand lets go of its rung on release, or when it is pulled far
        // off it (the other hand climbing with this one still squeezed).
        if (m_LadderPin[h].valid && (!grip[h] || !valid[h]
            || (world[h] - m_LadderPin[h].world).LengthSqr() > kRungLeaveHu * kRungLeaveHu))
            m_LadderPin[h].valid = false;
    }
    // Holstered for the climb: the weapon comes back once off the ladder
    // (300 ms, so mounting does not flicker it), unless the player took
    // another weapon or empty hands meanwhile.
    if (s_autoHolstered)
    {
        if (!m_EmptyHands)
            s_autoHolstered = false;
        else if (!onLadder && s_climbHand < 0 && s_mountUntil == 0)
        {
            if (!s_offLadderSince)
                s_offLadderSince = now;
            else if (now - s_offLadderSince > 300)
            {
                m_EmptyHands = false;
                s_autoHolstered = false;
                Game::logMsg("Ladder: off the ladder, weapon back");
            }
        }
        else
            s_offLadderSince = 0;
    }
    if (s_climbHand >= 0 && !grip[s_climbHand])
    {
        // Let go: the other hand takes over if it still holds the ladder.
        const int other = 1 - s_climbHand;
        if (grip[other] && (touch[other] || lineTouch[other] || m_LadderPin[other].valid))
        {
            s_climbHand = other;
            s_anchored = false;
        }
        else
        {
            s_climbHand = -1;
            // Both hands off: fall off like HL2VR (any grip without a
            // ladder dismount there). A short grace lets a hand-over where
            // the new hand grabs a moment late keep the ladder (200 ms felt
            // slow to fall, user 2026-11-19).
            if (onLadder)
                s_dropAt = now + 100;
        }
    }
    if (s_climbHand >= 0 || !onLadder)
        s_dropAt = 0;
    // Top end (user, 2026-11-18: stick mode gets off fine, forwards or to
    // the side; hands-only did not, as it zeroes the stick on the ladder and
    // HL2's auto-dismount needs a forward or side move). With the feet
    // within kTopHu of the top mount point, a moved stick (not back) runs
    // stick ladder mode for that tick: it climbs the rest and LadderMove's
    // auto-dismount takes the info_ladder_dismount the head faces, as with
    // LadderStickClimb. Letting go up there steps off (one IN_USE with the
    // level head view: dismount faced, else the nearest) instead of
    // dropping.
    constexpr float kTopHu = 40.f;
    bool nearTop = false;
    if (onLadder)
    {
        float d = 0.f, yaw = 0.f;
        const int ti = nearestLadder(origin, d, yaw);
        if (ti >= 0)
        {
            const float topZ = tops[ti].z > bottoms[ti].z ? tops[ti].z : bottoms[ti].z;
            nearTop = topZ - origin.z < kTopHu;
        }
    }
    {
        const float f = m_WalkForward.load(std::memory_order_acquire);
        const float sd = m_WalkSide.load(std::memory_order_acquire);
        static bool s_stickTopLogged = false;
        if (nearTop && f > -0.2f && f * f + sd * sd > 0.2f * 0.2f)
        {
            s_dropAt = 0;
            s_anchored = false;   // hands re-anchor when the stick is let go
            out.stickTop = true;
            if (!s_stickTopLogged)
                Game::logMsg("Ladder: stick at the top -> stick ladder mode (auto-dismount)");
            s_stickTopLogged = true;
            return true;
        }
        s_stickTopLogged = false;
    }
    if (nearTop && s_dropAt && now >= s_dropAt)
    {
        s_dropAt = 0;
        m_LadderPin[0].valid = m_LadderPin[1].valid = false;
        out.topExit = true;
        Game::logMsg("Ladder: let go at the top -> step off");
        return true;
    }
    if (s_dropAt)
    {
        if (now < s_dropAt)
            return true;   // hold still meanwhile (no stick climbing)
        s_dropAt = 0;
        out.drop = true;
        Game::logMsg("Ladder grab: both hands let go -> off the ladder");
        return true;
    }

    if (s_mountUntil)
    {
        if (onLadder || now > s_mountUntil || s_climbHand < 0)
        {
            if (!onLadder && s_climbHand >= 0)
                Game::logMsg("Ladder grab: mount did not happen in %.1f s (no ladder within use range facing it?)",
                    (now - s_mountStart) * 0.001f);
            else if (onLadder && s_mountStart)
                Game::logMsg("Ladder grab: mounted after %.2f s", (now - s_mountStart) * 0.001f);
            m_LadderMountActive.store(false, std::memory_order_relaxed);
            s_mountUntil = 0;
        }
        else
        {
            out.yaw = s_mountYaw;
            // One use press (more each played the deny sound); then walk in
            // with the walk-on auto-mount allowed (SyncLadderUseOnly), as the
            // stick mounts. Below the ladder's bottom end (rungs of the prop
            // reach lower than the game's ladder, e.g. the bm_c2a5a pool
            // ladder) swim up into its range first.
            out.useEdge = !s_mountUseSent;
            s_mountUseSent = true;
            out.mountPush = true;
            m_LadderMountActive.store(true, std::memory_order_relaxed);
            if (s_mountLadder >= 0 && s_mountLadder < n)
            {
                const float lowZ = (std::min)(bottoms[s_mountLadder].z, tops[s_mountLadder].z);
                out.mountRise = origin.z < lowZ + 2.f;
            }
            return true;
        }
    }
    if (!onLadder || s_climbHand < 0)
        return false;

    int li = -1;
    {
        float d = 0.f, yaw = 0.f;
        li = nearestLadder(origin, d, yaw);
    }
    if (li < 0)
        li = 0;
    Vector axis = tops[li] - bottoms[li];
    const float len = axis.Length();
    const float k = len > 1.f ? axis.z / len : 1.f;   // height per unit along the ladder
    if (fabsf(k) < 0.3f)
        return false;   // horizontal ladder: stick only

    const float hz = trackZ[s_climbHand];
    if (!s_anchored)
    {
        s_anchored = true;
        s_anchorHandZ = hz;
        s_anchorOriginZ = origin.z;
        s_sent = 0.f;
        m_LadderView.hand = s_climbHand;
        m_LadderView.anchorHandZ = hz;
        m_LadderView.rebase = true;
    }
    const float step = 200.f * m_Game->TickInterval();   // MAX_CLIMB_SPEED per tick
    viewOff.climbing = true;
    m_LadderView.active = true;
    m_LadderView.limit = 2.5f * step * fabsf(k);
    const float want = (s_anchorHandZ - hz) / k;
    const float actual = (origin.z - s_anchorOriginZ) / k;
    // The ladder ends (or anything else) stopped the player: count from
    // where they are, so pulling further keeps asking (dismount at the top,
    // stepping off at the bottom) instead of running up a debt.
    if (fabsf(actual - s_sent) > 4.f * step)
        s_sent = actual;
    const float err = want - s_sent;
    if (err > 0.5f * step)
    {
        out.dir = 1;
        s_sent += step;
    }
    else if (err < -0.5f * step)
    {
        out.dir = -1;
        s_sent -= step;
    }
    static int s_climbLog;
    if (out.dir != 0 && s_climbLog < 40)
    {
        Game::logMsg("Ladder climb: %s hand, want %.1f sent %.1f actual %.1f step %.2f -> %s",
            s_climbHand == 0 ? "left" : "right", want, s_sent, actual, step, out.dir > 0 ? "up" : "down");
        ++s_climbLog;
    }
    return true;
}

void VR::UpdatePhysicalCrouch(uint32_t& buttons)
{
    CrouchViewState& c = m_CrouchView;
    c.otherDuck = (buttons & IN_DUCK) != 0;
    bool want = false;
    float dropCm = 0.f;
    if (bmvr::g_PhysicalCrouch && m_Game && m_VRScale > 0.f
        && !m_LadderView.active && !m_LadderActive.load(std::memory_order_acquire)
        && m_Game->LocalPlayerMoveType() != 9)
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (m_HmdOriginLatched && m_HmdPoseValid)
        {
            dropCm = (m_HmdPosAbsZero.z - m_HmdPosAbs.z) / m_VRScale * 100.f;
            const float enter = bmvr::g_PhysicalCrouchDropCm;
            want = c.active ? dropCm > enter - 6.f : dropCm >= enter;
        }
    }
    if (want != c.active)
        Game::logMsg("Physical crouch %s (head %.0f cm below the recenter height, threshold %.0f)",
            want ? "on" : "off", dropCm, bmvr::g_PhysicalCrouchDropCm);
    c.active = want;
    if (want)
    {
        buttons |= IN_DUCK;
        c.owned = true;
    }
}

float VR::PhysicalCrouchViewOffset(float eyeZ)
{
    CrouchViewState& c = m_CrouchView;
    const ULONGLONG now = GetTickCount64();
    float dt = c.lastMs ? (now - c.lastMs) * 0.001f : 0.f;
    c.lastMs = now;
    if (dt > 0.1f)
        dt = 0.1f;
    Vector origin{};
    if (!bmvr::g_PhysicalCrouch || !m_Game || !m_Game->LocalPlayerAbsOrigin(origin))
    {
        c.owned = false;
        c.offset = 0.f;
        return 0.f;
    }
    const int flags = m_Game->LocalPlayerFlags();
    const bool onGround = flags < 0 || (flags & 1);   // FL_ONGROUND
    const bool ducked = flags >= 0 && (flags & 2);    // FL_DUCKING
    const float eye = eyeZ - origin.z;
    // Standing eye height (BM: 64), learnt while nothing asks for a duck.
    if (onGround && !ducked && !c.active && !c.otherDuck && eye > 40.f && eye < 80.f)
        c.standEye = eye;
    float target = 0.f;
    if (c.owned)
    {
        target = c.standEye - eye;
        target = target < 0.f ? 0.f : (target > 40.f ? 40.f : target);
        // Stood up (or a stick/button crouch took over): not ours any more.
        if (!c.active && (target < 0.5f || c.otherDuck))
        {
            c.owned = false;
            target = 0.f;
        }
    }
    // On the ground the engine lerps its eye down; follow it exactly (the
    // limit is far above the duck speed). In the air the engine lifts the
    // origin instead of lowering the eye: keep the offset, ease on landing.
    if (onGround)
    {
        const float maxStep = 400.f * dt;
        const float d = target - c.offset;
        c.offset += d > maxStep ? maxStep : (d < -maxStep ? -maxStep : d);
    }
    else if (!c.owned)
        c.offset = 0.f;
    return c.offset;
}

float VR::PhysicalLadderViewOffset(float renderZ)
{
    LadderViewState& v = m_LadderView;
    const ULONGLONG now = GetTickCount64();
    float dt = v.lastMs ? (now - v.lastMs) * 0.001f : 0.f;
    v.lastMs = now;
    if (dt > 0.1f)
        dt = 0.1f;
    float hz = 0.f;
    bool valid = false;
    if (v.active && bmvr::g_PhysicalLadderSmooth && v.hand >= 0)
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (v.hand == 0 && m_LeftControllerTrackingValid)
        {
            hz = m_LeftControllerPosAbs.z;
            valid = true;
        }
        else if (v.hand == 1 && m_PhysicalRightTrackingValid)
        {
            hz = m_PhysicalRightPosAbs.z;
            valid = true;
        }
    }
    if (!valid)
    {
        // Not climbing: ease what is left back onto the body (~80 ms).
        v.offset *= expf(-dt / 0.08f);
        if (fabsf(v.offset) < 0.01f)
            v.offset = 0.f;
        return v.offset;
    }
    if (v.rebase)
    {
        // New climb hand / anchor: the view stays where it is.
        v.visAnchorZ = renderZ + v.offset - (v.anchorHandZ - hz);
        v.rebase = false;
    }
    // Where the view belongs so the climb hand stays where it took hold.
    float off = v.visAnchorZ + (v.anchorHandZ - hz) - renderZ;
    // The body is at most a couple of ticks behind; more = blocked (ladder
    // end, ceiling): move the anchor instead of drifting off the body.
    if (off > v.limit)
    {
        v.visAnchorZ -= off - v.limit;
        off = v.limit;
    }
    else if (off < -v.limit)
    {
        v.visAnchorZ -= off + v.limit;
        off = -v.limit;
    }
    v.offset = off;
    return off;
}

void VR::PhysicalLadderCentreHead(const Vector& body, ULONGLONG now, float dt)
{
    // LadderMove keeps the body on the ladder's middle, but the head is
    // wherever the player stands from their recenter spot (no room-scale
    // body follow): 15 cm off made climbing feel lopsided (user 2026-11-24,
    // capture-20261124-221718 bm_c3a2h: rungs held at y -2086 / -2074, body
    // -2080, eyes -2085.7). At the start of a climb the tracking zero slides
    // along the rung so the head sits over the middle, like a sideways-only
    // recenter. Only in a short window: leaning later is not undone. It
    // stays after the climb (the body is then simply under the head).
    LadderViewState& v = m_LadderView;
    if (!v.active)
        return;
    if (now - v.lastActiveMs > 1000)
    {
        v.centreStartMs = now;
        v.centreFrom = 0.f;
        v.centred = 0.f;
    }
    v.lastActiveMs = now;
    if (!v.centreStartMs || !bmvr::g_PhysicalLadders || !bmvr::g_LadderCentreHead || body.LengthSqr() <= 1.f)
        return;
    constexpr ULONGLONG kWindowMs = 700;
    const bool last = now - v.centreStartMs >= kWindowMs;
    Vector axis{};
    bool haveAxis = false;
    for (int h = 0; h < 2 && !haveAxis; ++h)
    {
        if (!m_LadderPin[h].valid || !m_LadderPin[h].hasAxis)
            continue;
        axis = m_LadderPin[h].axis;
        axis.z = 0.f;
        haveAxis = VectorNormalize(axis) > 0.1f;
    }
    float lateral = 0.f;
    if (haveAxis && !last)
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        const Vector eye = ControllerTrackingToWorld(body, m_StereoFramePoseActive ? m_StereoFrameHmdPosAbs : m_HmdPosAbs);
        lateral = (eye - body).Dot(axis);
        if (v.centred == 0.f)
        {
            v.centreFrom = lateral;
            // Already near the middle: leave the view alone.
            if (fabsf(lateral) < 2.f)
            {
                v.centreStartMs = 0;
                return;
            }
        }
        // ~120 ms ease; the eye moves by -d when the zero moves by R^-1(d).
        const float a = dt > 0.f ? 1.f - expf(-dt / 0.12f) : 0.f;
        const float step = lateral * a;
        Vector d = axis * step;
        PivotYaw(d, -m_RotationOffsetY.load(std::memory_order_acquire));
        m_HmdPosAbsZero.x += d.x;
        m_HmdPosAbsZero.y += d.y;
        v.centred += step;
    }
    if (last)
    {
        if (v.centred != 0.f)
            Game::logMsg("Ladder centre: head moved %.1f units sideways onto the ladder's middle (was %.1f off)",
                v.centred, v.centreFrom);
        v.centreStartMs = 0;
    }
}

Vector VR::PhysicalLadderViewShift(const Vector& body)
{
    // The body hangs at LadderMove's fixed distance from the ladder; the
    // gloves are drawn on the rungs they hold (TryGetLadderPinDrawPos). Move
    // the view along the horizontal line to the held rung by what the real
    // hand is short of / past that rung, so pulling the arms in brings the
    // player closer and stretching them pushes away (user idea, 2026-11-18).
    // Capped at 8 units (~20 cm) each way on normal ladders: LadderMove
    // keeps the hull ~16 units off the ladder, so the eye never reaches the
    // ladder or a shaft wall behind (more where the rungs stand further
    // out, see maxShift).
    LadderViewState& v = m_LadderView;
    static ULONGLONG s_lastMs = 0;
    const ULONGLONG now = GetTickCount64();
    float dt = s_lastMs ? (now - s_lastMs) * 0.001f : 0.f;
    s_lastMs = now;
    if (dt > 0.1f)
        dt = 0.1f;
    PhysicalLadderCentreHead(body, now, dt);
    Vector want(0.f, 0.f, 0.f);
    int n = 0;
    // Cap: 8 units for the usual ladders (rungs ~24 units in front of the
    // body). Ladders whose rungs stand further out (bm_c4a1a Xen scaffold:
    // ~33 units) get the extra on top, so the eye ends up as close to the
    // rungs as on a normal ladder (user 2026-11-21: hard to reach, pushed
    // away; log showed the shift pinned at 8).
    constexpr float kBaseMax = 8.f;
    constexpr float kStdRungHu = 24.f;
    float maxShift = kBaseMax;
    if (v.active && bmvr::g_PhysicalLadders && bmvr::g_LadderBodyFollowsArms && body.LengthSqr() > 1.f)
    {
        Vector eye{};
        {
            std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
            eye = ControllerTrackingToWorld(body, m_StereoFramePoseActive ? m_StereoFrameHmdPosAbs : m_HmdPosAbs);
        }
        for (int h = 0; h < 2; ++h)
        {
            if (!m_LadderPin[h].valid || !m_LadderGrip[h].load(std::memory_order_relaxed))
                continue;
            Vector tracking{};
            bool ok = false;
            {
                std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
                if (h == 0 && m_LeftControllerTrackingValid)
                {
                    tracking = m_LeftControllerPosAbs;
                    ok = true;
                }
                else if (h == 1 && m_PhysicalRightTrackingValid)
                {
                    tracking = m_PhysicalRightPosAbs;
                    ok = true;
                }
            }
            if (!ok)
                continue;
            const Vector hand = ControllerTrackingToWorld(body, tracking);
            // Only toward / away from the ladder: along the rung's normal
            // (horizontal, perpendicular to the rung). The eye-to-rung line
            // used before is diagonal for a hand out to the side, so each
            // hand-over swayed the head left / right (user 2026-11-20).
            Vector dir = m_LadderPin[h].world - eye;
            dir.z = 0.f;
            if (VectorNormalize(dir) < 1.f)
                continue;
            if (m_LadderPin[h].hasAxis)
            {
                Vector nrm(-m_LadderPin[h].axis.y, m_LadderPin[h].axis.x, 0.f);
                if (VectorNormalize(nrm) > 0.1f)
                    dir = nrm.Dot(dir) >= 0.f ? nrm : nrm * -1.f;
            }
            const float gap = (m_LadderPin[h].world - hand).Dot(dir);
            want += dir * gap;
            ++n;
            const float rungOut = (m_LadderPin[h].world - body).Dot(dir) + bmvr::g_LadderHandBack;
            const float cap = kBaseMax + (rungOut > kStdRungHu ? rungOut - kStdRungHu : 0.f);
            if (cap > maxShift)
                maxShift = cap < 24.f ? cap : 24.f;
        }
    }
    if (n > 0)
    {
        want = want * (1.f / static_cast<float>(n));
        want.z = 0.f;
        const float len = want.Length();
        if (len > maxShift)
            want = want * (maxShift / len);
    }
    // Follows in ~60 ms; eases back onto the body in ~150 ms when let go.
    const float tau = n > 0 ? 0.06f : 0.15f;
    const float a = dt > 0.f ? 1.f - expf(-dt / tau) : 0.f;
    v.shift = v.shift + (want - v.shift) * a;
    if (n == 0 && v.shift.LengthSqr() < 0.0001f)
        v.shift = Vector(0.f, 0.f, 0.f);
    static int s_log = 0;
    static ULONGLONG s_logMs = 0;
    if (n > 0 && s_log < 40 && now - s_logMs > 1000)
    {
        s_logMs = now;
        ++s_log;
        Game::logMsg("Ladder arms: view shift %.1f units (want %.1f, cap %.1f, %d hand%s)",
            v.shift.Length(), want.Length(), maxShift, n, n == 1 ? "" : "s");
    }
    return v.shift;
}

QAngle VR::GetUseAimAnglesFromEye() const
{
    const QAngle handAim = GetUseAimAngles();
    Vector hand{};
    if (!m_Game || !m_Game->m_EngineTrace || m_SetupOrigin.LengthSqr() <= 1.f
        || !TryGetVrUseOrigin(hand))
        return handAim;
    // A little beyond the game's use reach (~80 units from the eye).
    constexpr float kReach = 96.f;
    Vector dir{};
    QAngle::AngleVectors(handAim, &dir, nullptr, nullptr);
    if (VectorNormalize(dir) <= 0.01f)
        return handAim;
    Vector end = hand + dir * kReach;
    C_BaseEntity* player = m_Game->GetLocalPlayerEntity();
    C_BaseEntity* vm = m_Game->GetViewModelEntity();
    CTraceFilterSkipTwoEntities filter(player, vm, 0);
    Ray_t ray;
    ray.Init(hand, end);
    CGameTrace tr{};
    m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
    if (tr.fraction < 1.f && !tr.startsolid)
        end = tr.endpos;
    Vector toTarget = end - m_SetupOrigin;
    if (VectorNormalize(toTarget) <= 0.01f)
        return handAim;
    QAngle aim{};
    QAngle::VectorAngles(toTarget, aim);
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
    // The muzzle offset measured while the gun was near the view centre
    // (UpdateMuzzleLocal), placed on this frame's gun pose. The raw
    // attachment drifted toward the view edges: Source's viewmodel attachment
    // FOV correction (FormatViewModelAttachment) ran against a FOV that is
    // not our per-eye one, so shots and laser swung around the barrel. That
    // correction is now skipped in VR (Hooks::dFormatViewModelAttachment).
    if (bmvr::g_VrShootFromMuzzle && m_AimBeamLocalValid)
    {
        std::string key;
        {
            std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
            key = m_VmClass.numpadKey;
        }
        Vector vo{}, vf{}, vr{}, vu{};
        if (key == m_AimBeamLocalKey && GunFrameNow(vo, vf, vr, vu))
        {
            origin = vo + vf * m_AimBeamLocal.x + vr * m_AimBeamLocal.y + vu * m_AimBeamLocal.z;
            return true;
        }
    }
    return TryGetRawMuzzleWorld(origin);
}

void VR::UpdateMuzzleLocal(const std::string& weaponKey)
{
    Vector raw{}, vo{}, vf{}, vr{}, vu{};
    if (!TryGetRawMuzzleWorld(raw) || !GunFrameNow(vo, vf, vr, vu))
        return;
    const Vector d = raw - vo;
    const Vector local(d.Dot(vf), d.Dot(vr), d.Dot(vu));
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    Vector viewFwd, viewRight, viewUp;
    GetViewBasis(&viewFwd, &viewRight, &viewUp);
    Vector toMuzzle = raw - GetViewOrigin(body);
    float offCentreDeg = 90.f;
    if (VectorNormalize(toMuzzle) > 0.01f && VectorNormalize(viewFwd) > 0.01f)
        offCentreDeg = acosf(std::clamp(toMuzzle.Dot(viewFwd), -1.f, 1.f)) * 57.29578f;
    const bool fresh = !m_AimBeamLocalValid || m_AimBeamLocalKey != weaponKey;
    const float drift = fresh ? 0.f : (local - m_AimBeamLocal).Length();
    // Runs every frame by default (VrShootFromMuzzle): log a weapon change,
    // and the drift every 3 s only with AimDebug.
    static DWORD s_logMs;
    const DWORD now = GetTickCount();
    if (fresh || (bmvr::g_AimDebug && now - s_logMs > 3000))
    {
        s_logMs = now;
        Game::logMsg("Muzzle local %s offCentre=%.1fdeg rawDrift=%.2fHU local=(%.2f,%.2f,%.2f)",
            weaponKey.c_str(), offCentreDeg, drift, local.x, local.y, local.z);
    }
    // The raw attachment is right anywhere in view now (no FOV correction in
    // VR), so keep following it: the first sample is taken during the draw
    // animation, and the old "near the view centre only" refine left that
    // low sample in place while the gun was held off-centre.
    // Preferred: the muzzle as the viewmodel DME drew it, in that draw's own
    // gun frame. The raw attachment above comes from bones set up for the
    // previous pose, so against this frame's gun pose it trails a moving gun
    // (laser start a frame or two behind the barrel).
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (m_DrawnMuzzleLocalValid && m_DrawnMuzzleLocalKey == weaponKey)
        {
            m_AimBeamLocal = m_DrawnMuzzleLocal;
            m_AimBeamLocalKey = weaponKey;
            m_AimBeamLocalValid = true;
            return;
        }
    }
    if (fresh)
        m_AimBeamLocal = local;
    else
        m_AimBeamLocal = m_AimBeamLocal + (local - m_AimBeamLocal) * 0.3f;
    m_AimBeamLocalKey = weaponKey;
    m_AimBeamLocalValid = true;
}

void VR::NoteDrawnMuzzleLocal(const Vector& local)
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    m_DrawnMuzzleLocal = local;
    m_DrawnMuzzleLocalKey = m_VmClass.numpadKey;
    m_DrawnMuzzleLocalValid = true;
}

void VR::NoteViewmodelLowerState(int state, const void* studioHdr)
{
    const int was = m_VmLowerState.exchange(state, std::memory_order_relaxed);
    if (state == kVmLowerIdle && was != kVmLowerIdle)
        m_VmLowerIdleSinceMs.store(GetTickCount64(), std::memory_order_relaxed);
    m_VmLowerHdr.store(studioHdr, std::memory_order_relaxed);
}

bool VR::StockOffhandFingersIdleSettled() const
{
    // Idle blends in over ~0.2 s and
    // the render thread draws a frame behind.
    return m_VmLowerState.load(std::memory_order_relaxed) == kVmLowerIdle
        && GetTickCount64() - m_VmLowerIdleSinceMs.load(std::memory_order_relaxed) >= 250;
}

void VR::NoteStockOffhandFingers(const void* studioHdr, const float rel[15][12])
{
    if (!studioHdr || studioHdr != m_VmLowerHdr.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(m_StockFingersMutex);
    if (m_StockFingersHdr != studioHdr)
        Game::logMsg("Stock hand pose: idle fingers sampled for %p", studioHdr);
    std::memcpy(m_StockFingers, rel, sizeof(m_StockFingers));
    m_StockFingersHdr = studioHdr;
}

bool VR::GetStockOffhandFingers(float rel[15][12]) const
{
    std::lock_guard<std::mutex> lock(m_StockFingersMutex);
    if (!m_StockFingersHdr || m_StockFingersHdr != m_VmLowerHdr.load(std::memory_order_relaxed))
        return false;
    std::memcpy(rel, m_StockFingers, sizeof(m_StockFingers));
    return true;
}

bool VR::TryGetRawMuzzleWorld(Vector& origin) const
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
    // v_crossbow has no "muzzle": its bolt leaves from "bolt_spawn".
    static const char* kNames[] = { "muzzle", "Fire01", "Fire02", "bolt_spawn" };
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

// Scoped crossbow: the bolt flies along the head's view angles. Start it at
// the ScopeEye eye so it lands on that eye's reticle at any distance (the
// scoped eyes are centred on straight ahead, see EyeRenderRect).
bool VR::TryGetScopeShootOrigin(Vector& origin) const
{
    if (!m_ScopeZoomActive)
        return false;
    if (bmvr::g_ScopeMonoZoom)
    {
        // Both eyes sit at the head centre: start the bolt there (= on the reticle in both eyes).
        origin = GetViewOrigin(m_SetupOrigin);
        return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z);
    }
    if (bmvr::g_ScopeEye == 0)
        return false;
    origin = bmvr::g_ScopeEye == 1 ? GetViewOriginLeft(m_SetupOrigin) : GetViewOriginRight(m_SetupOrigin);
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z);
}

bool VR::TryGetVrShootOrigin(Vector& origin) const
{
    Vector muzzle{};
    if (!TryGetVrMuzzleWorld(muzzle))
        return false;
    // Default since 2026-10-15: shots leave the muzzle along the aim, like a
    // real gun. Up close they hit where the barrel points (user: 5-10 cm
    // under it at a wall), and the laser is a straight line out of the
    // barrel. The constant few-cm offset from the controller axis is
    // invisible at range. VrShootFromMuzzle=false = the projection below.
    if (bmvr::g_VrShootFromMuzzle)
    {
        origin = muzzle;
        return true;
    }

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

void VR::UpdateMountedGun()
{
    const bool wasOnGun = m_OnGun;
    m_OnGun = false;
    m_GunRightNear = false;
    if (!m_Game || !m_GameplayEligible)
    {
        m_GunAimBaseValid = false;
        return;
    }
    int props[32];
    const int n = m_Game->CollectMountedGunProps(props, 32);
    if (n <= 0)
    {
        m_GunAimBaseValid = false;
        return;
    }
    // The gun brush (func_50cal / func_tow) is parented to its prop: on a
    // gun, the use entity is a brush model next to one of the props.
    // The brush's own origin is not usable on the client (parented, not
    // kept up to date), so the prop has to be near the player instead.
    C_BaseEntity* useEnt = m_Game->LocalPlayerUseEntity();
    const char* useModel = useEnt ? m_Game->GetEntityModelName(useEnt) : nullptr;
    Vector useOrigin{};
    const bool useIsBrush = useModel && useModel[0] == '*' && m_Game->LocalPlayerAbsOrigin(useOrigin);
    Vector hand{};
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    const bool haveHand = m_RightControllerTrackingValid && body.LengthSqr() > 1.f
        && (hand = GetRightControllerAbsPos(body)).LengthSqr() > 1.f;
    // Mount to hand: the handles sit about 20 units behind it.
    constexpr float kUseNearProp = 128.f;
    constexpr float kHandReach = 30.f;
    for (int i = 0; i < n; ++i)
    {
        C_BaseEntity* prop = m_Game->GetClientEntity(props[i]);
        Vector pivot{};
        QAngle unused{};
        if (!prop || (!m_Game->GetEntityAttachment(prop, "aimrotation", pivot, unused)
            && !m_Game->GetEntityAbsOrigin(prop, pivot)))
            continue;
        if (useIsBrush && (useOrigin - pivot).LengthSqr() < kUseNearProp * kUseNearProp)
        {
            m_OnGun = true;
            m_GunPropIndex = props[i];
            break;
        }
        if (haveHand && (hand - pivot).LengthSqr() < kHandReach * kHandReach)
            m_GunRightNear = true;
    }
    if (m_OnGun)
        m_GunRightNear = false;
    if (m_OnGun != wasOnGun)
    {
        static int s_gunLog;
        if (s_gunLog < 24)
        {
            Game::logMsg("Mounted gun %s (prop %d)", m_OnGun ? "taken" : "left", m_GunPropIndex);
            ++s_gunLog;
        }
    }
    if (!m_OnGun)
        m_GunAimBaseValid = false;
}

bool VR::MountedGunAim(QAngle& out)
{
    if (!m_OnGun || !m_Game)
        return false;
    C_BaseEntity* prop = m_Game->GetClientEntity(m_GunPropIndex);
    Vector pivot{};
    QAngle pivotAngles{};
    const bool haveAttach = prop && m_Game->GetEntityAttachment(prop, "aimrotation", pivot, pivotAngles);
    if (!haveAttach && !(prop && m_Game->GetEntityAbsOrigin(prop, pivot)))
        return false;
    // The hands on the handles: those holding a trigger, both = their midpoint.
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        return false;
    const bool left = m_GunHeldLeft && m_LeftControllerTrackingValid;
    const bool right = m_GunHeldRight && m_RightControllerTrackingValid;
    if (!left && !right)
    {
        // Between letting go and the game taking the player off the gun.
        if (!m_GunAimBaseValid)
            return false;
        out = m_GunLastAim;
        return true;
    }
    Vector hands{};
    if (left)
        hands = GetLeftControllerAbsPos(body);
    if (right)
        hands = left ? (hands + GetRightControllerAbsPos(body)) * 0.5f : GetRightControllerAbsPos(body);
    Vector push = pivot - hands;
    if (VectorNormalize(push) <= 0.01f)
        return false;
    QAngle handAngles{};
    QAngle::VectorAngles(push, handAngles);
    const int handMask = (left ? 1 : 0) | (right ? 2 : 0);
    if (!m_GunAimBaseValid || handMask != m_GunAimHands)
    {
        // A new hold (or a hand added / let go): the barrel stays where it
        // points, only hand movement from here on turns it.
        Vector muzzle{};
        QAngle unused{};
        Vector barrel{};
        if (prop && m_Game->GetEntityAttachment(prop, "muzzle", muzzle, unused)
            && (barrel = muzzle - pivot, VectorNormalize(barrel) > 0.01f))
            QAngle::VectorAngles(barrel, m_GunBarrelBase);
        else if (m_GunAimBaseValid)
            m_GunBarrelBase = m_GunLastAim;
        else
        {
            // No muzzle attachment on the client (50cal prop, 2026-11-01):
            // the barrel points away from the hands, level. The hands are
            // above the mount, so their own pitch would tip it ~25 deg down.
            m_GunBarrelBase = handAngles;
            m_GunBarrelBase.x = 0.f;
        }
        m_GunHandBase = handAngles;
        m_GunAimHands = handMask;
        m_GunAimBaseValid = true;
        Game::logMsg("Mounted gun hold: hands %s, pivot (%.0f,%.0f,%.0f)%s, barrel yaw %.1f pitch %.1f",
            left && right ? "both" : (left ? "left" : "right"), pivot.x, pivot.y, pivot.z,
            haveAttach ? "" : " (prop origin)", m_GunBarrelBase.y, m_GunBarrelBase.x);
    }
    auto wrap = [](float a) {
        while (a > 180.f) a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    };
    // MountedGunGain > 1: the barrel turns further than the hands swing, so
    // aiming low/high or far sideways needs less reach (the handle leads the
    // hand instead of staying in it).
    const float gain = std::clamp(bmvr::g_MountedGunGain, 0.5f, 4.f);
    QAngle wanted{};
    wanted.x = wrap(m_GunBarrelBase.x) + gain * wrap(handAngles.x - m_GunHandBase.x);
    wanted.y = wrap(m_GunBarrelBase.y + gain * wrap(handAngles.y - m_GunHandBase.y));
    wanted.z = 0.f;
    if (wanted.x > 89.f) wanted.x = 89.f;
    if (wanted.x < -89.f) wanted.x = -89.f;
    Vector dir{};
    QAngle::AngleVectors(wanted, &dir, nullptr, nullptr);
    Vector target = pivot + dir * 8192.f;
    if (m_Game->m_EngineTrace)
    {
        C_BaseEntity* player = m_Game->GetLocalPlayerEntity();
        CTraceFilterSkipTwoEntities filter(player, prop, 0);
        Ray_t ray;
        ray.Init(pivot + dir * 16.f, target);
        CGameTrace tr{};
        m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
        if (tr.fraction < 1.f && !tr.startsolid)
            target = tr.endpos;
    }
    Vector toTarget = target - m_SetupOrigin;
    if (VectorNormalize(toTarget) <= 0.01f)
        return false;
    QAngle aim{};
    QAngle::VectorAngles(toTarget, aim);
    aim.x = wrap(aim.x);
    if (aim.x > 89.f) aim.x = 89.f;
    if (aim.x < -89.f) aim.x = -89.f;
    aim.z = 0.f;
    m_GunLastAim = aim;
    static DWORD s_aimLogMs;
    static int s_aimLog;
    const DWORD now = GetTickCount();
    if (bmvr::g_AimDebug || (now - s_aimLogMs > 1000 && s_aimLog < 120))
    {
        s_aimLogMs = now;
        ++s_aimLog;
        Vector muzzle{};
        QAngle unused{};
        float barrelYaw = 0.f, barrelPitch = 0.f;
        Vector barrel{};
        if (prop && m_Game->GetEntityAttachment(prop, "muzzle", muzzle, unused)
            && (barrel = muzzle - pivot, VectorNormalize(barrel) > 0.01f))
        {
            QAngle b{};
            QAngle::VectorAngles(barrel, b);
            barrelYaw = wrap(b.y);
            barrelPitch = wrap(b.x);
        }
        Game::logMsg("Mounted gun aim: hands yaw %.1f pitch %.1f (hold %.1f %.1f) -> wanted yaw %.1f pitch %.1f, sent yaw %.1f pitch %.1f, barrel now yaw %.1f pitch %.1f",
            wrap(handAngles.y), wrap(handAngles.x), wrap(m_GunHandBase.y), wrap(m_GunHandBase.x),
            wanted.y, wanted.x, wrap(aim.y), aim.x, barrelYaw, barrelPitch);
    }
    out = aim;
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

// Free-hand Use target (CreateMove, client): a short thin sweep along the
// free hand's aim from just behind the hand. Physics props are left out (their
// Use is a pickup). Hooks::dPlayerUse reads the index on the server tick.
void VR::UpdateFreeHandUseTarget()
{
    int index = 0;
    const bool held = m_FreeUseHeld.load(std::memory_order_acquire);
    const bool left = m_FreeUseLeft.load(std::memory_order_relaxed);
    if (held && m_Game && m_Game->m_EngineTrace && m_Game->m_EngineClient && m_Game->m_ClientEntityList
        && (left ? m_LeftControllerTrackingValid : m_RightControllerTrackingValid))
    {
        Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        const Vector hand = left ? GetLeftControllerAbsPos(body) : GetRightControllerAbsPos(body);
        Vector fwd{};
        QAngle::AngleVectors(left ? GetLeftControllerAbsAngle() : GetRightControllerAbsAngle(), &fwd, nullptr, nullptr);
        if (hand.LengthSqr() > 1.f && VectorNormalize(fwd) > 0.01f)
        {
            constexpr float kBehind = 8.f, kReach = 64.f, kHull = 3.f;
            C_BaseEntity* player = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
            CTraceFilterSkipSelf filter(player, 0);
            CGameTrace tr{};
            tr.fraction = 1.f;
            tr.m_pEnt = nullptr;
            Ray_t ray;
            ray.Init(hand - fwd * kBehind, hand + fwd * kReach, Vector(-kHull, -kHull, -kHull), Vector(kHull, kHull, kHull));
            m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT_HULL, &filter, &tr);
            C_BaseEntity* hit = (tr.fraction < 1.f && tr.m_pEnt && tr.m_pEnt != player) ? static_cast<C_BaseEntity*>(tr.m_pEnt) : nullptr;
            if (hit)
            {
                const int top = m_Game->m_ClientEntityList->GetHighestEntityIndex();
                for (int i = 1; i <= top && !index; ++i)
                {
                    if (m_Game->GetClientEntity(i) == hit)
                        index = i;
                }
                const char* cls = index ? m_Game->GetEntityClientClassName(hit) : nullptr;
                char lower[96]{};
                for (size_t k = 0; cls && cls[k] && k + 1 < sizeof(lower); ++k)
                    lower[k] = static_cast<char>(tolower(static_cast<unsigned char>(cls[k])));
                if (std::strstr(lower, "phys"))
                    index = 0;
                static int s_lastIndex = -1;
                static int s_log;
                if (index != s_lastIndex && s_log < 40)
                {
                    ++s_log;
                    const char* mdl = m_Game->GetEntityModelName(hit);
                    Game::logMsg("Free-hand Use target: %d %s model %s", index, cls ? cls : "?", mdl ? mdl : "-");
                }
                s_lastIndex = index;
            }
        }
    }
    m_FreeUseEntIndex.store(index, std::memory_order_release);
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
    m_UseReleaseTick = 0;
    m_UseDropPulseEndTick = 0;
}

void VR::UpdateUseGrab(uint32_t& buttons, bool leftUse, bool rightUse)
{
    // Free hand (user 2026-11-24): with Use latched on one hand, the other
    // hand's Use no longer joins the game's (one) Use. It goes to what that
    // hand points at through the server (Hooks::dPlayerUse): a door or button
    // while the first hand carries a prop (a game Use press would drop it),
    // a second charger next to the first. The hand stays out of the game's
    // Use until it is let go, so it cannot latch a new Use mid-press.
    {
        static bool s_freeLock[2];   // [0] left, [1] right
        if (!leftUse)
            s_freeLock[0] = false;
        if (!rightUse)
            s_freeLock[1] = false;
        if (m_GrabLatched && m_UseWasHeld && (m_GrabHandLeft ? rightUse : leftUse))
            s_freeLock[m_GrabHandLeft ? 1 : 0] = true;
        const bool freeLeft = s_freeLock[0] && leftUse, freeRight = s_freeLock[1] && rightUse;
        const bool wasFree = m_FreeUseHeld.load(std::memory_order_relaxed);
        if ((freeLeft || freeRight) && !wasFree)
        {
            m_FreeUseLeft.store(freeLeft, std::memory_order_relaxed);
            m_FreeUsePressSeq.fetch_add(1, std::memory_order_acq_rel);
            static int s_freeLog;
            if (s_freeLog++ < 20)
                Game::logMsg("Free-hand Use: %s hand pressed (Use latched on the %s hand)",
                    freeLeft ? "left" : "right", m_GrabHandLeft ? "left" : "right");
        }
        m_FreeUseHeld.store(freeLeft || freeRight, std::memory_order_release);
        if (s_freeLock[0])
            leftUse = false;
        if (s_freeLock[1])
            rightUse = false;
    }
    const bool useHeld = leftUse || rightUse;
    const double now = QpcNowMs();
    {
        static C_BaseEntity* s_lastUseEnt;
        static int s_useEntLog;
        C_BaseEntity* useEnt = m_Game->LocalPlayerUseEntity();
        if (useEnt != s_lastUseEnt && s_useEntLog < 40)
        {
            const char* cls = useEnt ? m_Game->GetEntityClientClassName(useEnt) : nullptr;
            const char* mdl = useEnt ? m_Game->GetEntityModelName(useEnt) : nullptr;
            Game::logMsg("Use entity: %s model %s", useEnt ? (cls ? cls : "?") : "none", mdl ? mdl : "-");
            ++s_useEntLog;
        }
        s_lastUseEnt = useEnt;
    }
    const bool worldToggle = UseEntityIsWorldToggle(m_Game);
    const bool heldProp = UseEntityIsHeldProp(m_Game);
    // Server usercmds lag the client by a tick or two, so controller calls
    // within this margin of a release can still be from before it.
    constexpr uint64_t kGrabLagMs = 50;
    m_GrabUseEntityHeld = heldProp;
    if (heldProp)
        m_GrabHoldingPhysics = true;

    auto startDropPulse = [&]() {
        if (m_UseDropRetries >= 3)
            return;
        // A second Use lets go of train controls; nothing is carried there.
        if (m_OnTrainControls)
        {
            static int s_trainPulseLog;
            if (s_trainPulseLog < 8)
            {
                Game::logMsg("Use release on train controls: no drop pulse");
                ++s_trainPulseLog;
            }
            m_UseDropClassifyUntilMs = 0.0;
            m_GrabHoldingPhysics = false;
            return;
        }
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
            m_UsePressTick = GetTickCount64();
            m_UseDropRetries = 0;
            static int s_latchLog;
            if (s_latchLog < 12)
            {
                Game::logMsg("Use grab latched %s", m_GrabHandLeft ? "left" : "right");
                ++s_latchLog;
            }
        }
        // Was: any hold over 100 ms counted as carrying, so letting go of
        // train controls held that long pulsed a second Use that let go of them.
        else if (GrabControllerRecentlyActive())
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
        m_UseReleaseTick = GetTickCount64();
        // Decided again from what the controller does after the release: a
        // second click whose press already dropped the prop must not pulse
        // (the pulse picked it up again), and a click released before the
        // controller started still has to drop it.
        m_GrabHoldingPhysics = false;
    }
    m_UseWasHeld = false;

    if (m_UseDropClassifyUntilMs > 0.0)
    {
        if (worldToggle)
        {
            ClearUseGrab();
            return;
        }
        if (!heldProp && GrabControllerActiveAfter(m_UseReleaseTick + kGrabLagMs))
            m_GrabHoldingPhysics = true;
        if (heldProp || m_GrabHoldingPhysics)
        {
            Game::logMsg("Use release: still carrying, drop pulse %d", m_UseDropRetries + 1);
            startDropPulse();
        }
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
            m_UseDropPulseEndTick = GetTickCount64();
        }
        return;
    }

    if (m_UseDropSettleUntilMs > 0.0)
    {
        if (heldProp || GrabControllerActiveAfter(m_UseDropPulseEndTick + kGrabLagMs))
        {
            Game::logMsg("Use drop pulse missed, retry %d", m_UseDropRetries + 1);
            startDropPulse();
            if (m_UseDropPulseUntilMs <= 0.0)
                m_UseDropSettleUntilMs = 0.0;
            return;
        }
        if (now >= m_UseDropSettleUntilMs)
        {
            m_UseDropSettleUntilMs = 0.0;
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
    // Scroll diagnosis (user 2026-11-17: held stick stops scrolling, a new
    // push restarts it): once a second while the stick is held, wheel posts
    // vs calls here and the stick value.
    static DWORD s_wheelStatMs;
    static int s_wheelPosted, s_wheelCalls;
    static float s_wheelMinY = 1.f, s_wheelMaxY = -1.f;
    if (fabsf(turnY) > 0.1f || s_wheelPosted)
    {
        ++s_wheelCalls;
        s_wheelMinY = (std::min)(s_wheelMinY, turnY);
        s_wheelMaxY = (std::max)(s_wheelMaxY, turnY);
        if (s_wheelStatMs == 0)
            s_wheelStatMs = nowMs;
        if (nowMs - s_wheelStatMs >= 1000u)
        {
            Game::logMsg("Menu wheel 1s: posted=%d calls=%d stickY %.2f..%.2f cursor %d,%d valid=%d live=%d",
                s_wheelPosted, s_wheelCalls, s_wheelMinY, s_wheelMaxY, m_MenuCursorX, m_MenuCursorY,
                m_MenuCursorValid ? 1 : 0, MenuPointerLive() ? 1 : 0);
            s_wheelStatMs = nowMs;
            s_wheelPosted = s_wheelCalls = 0;
            s_wheelMinY = 1.f;
            s_wheelMaxY = -1.f;
        }
    }
    else
        s_wheelStatMs = 0;
    // New game UI: some lists run sideways (Load/Save Game). Its forwarder
    // only passes the vertical wheel to Qt, which scrolls a sideways list
    // under the pointer too. So with the laser on the menu, the turn stick
    // held (mostly) sideways = the same wheel (right = down = next items):
    // same speed as up/down, no keyboard focus. Left/Right keys moved focus
    // that the laser's hover took back (user 2026-11-23: focus fight, and
    // much slower at 250 ms); they stay for when the laser is off the menu.
    float turnX = 0.f;
    if (bmvr::NewUiWindow() && GetAnalogActionData(m_ActionTurn, turn))
        turnX = turn.x;
    const bool sideways = fabsf(turnX) > 0.55f && fabsf(turnX) > fabsf(turnY) * 1.5f;
    const bool pointerLive = MenuPointerLive();
    const float scrollAxis = (sideways && pointerLive) ? -turnX : turnY;
    if (fabsf(scrollAxis) > 0.35f && nowMs - s_wheelMs >= 70u)
    {
        short wheel = (scrollAxis > 0.f) ? WHEEL_DELTA : -WHEEL_DELTA;
        if (fabsf(scrollAxis) > 0.85f)
            wheel = static_cast<short>(wheel * 2);
        // Fallback: window centre (was 640,360 = a 1280x720 window's centre).
        const int cx = m_MenuCursorValid ? m_MenuCursorX : static_cast<int>(KnownWindowWidth() / 2);
        const int cy = m_MenuCursorValid ? m_MenuCursorY : static_cast<int>(KnownWindowHeight() / 2);
        PostMessageA(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, wheel), MAKELPARAM(cx, cy));
        s_wheelMs = nowMs;
        ++s_wheelPosted;
        static int s_wheelLog;
        if (s_wheelLog < 8)
        {
            Game::logMsg("Menu wheel stick=%.2f%s delta=%d", scrollAxis,
                scrollAxis != turnY ? " (sideways)" : "", (int)wheel);
            ++s_wheelLog;
        }
    }
    // Laser off the menu: the turn stick sideways = Left/Right (repeat
    // 250 ms), which moves along sideways lists; vertical lists ignore it.
    static bool s_tl, s_tr;
    static DWORD s_tlMs, s_trMs;
    dpad(sideways && !pointerLive && turnX < 0.f, VK_LEFT, s_tl, s_tlMs, "turn-left");
    dpad(sideways && !pointerLive && turnX > 0.f, VK_RIGHT, s_tr, s_trMs, "turn-right");

    const bool select = PressedDigitalAction(m_ActionMenuSelect);
    // While the pointer is on the menu, menus click with the trigger only
    // (Enter would activate whatever has keyboard focus instead). Otherwise
    // Cross confirms. m_MenuCursorValid stays true once the pointer has hit
    // a menu, so it is the 250 ms liveness that decides, not that flag.
    if (select && !s_ok && !MenuPointerLive())
    {
        QueueKeyToHwnd(hwnd, VK_RETURN);
        Game::logMsg("Menu confirm A/MenuSelect");
    }
    s_ok = select;

    TickSyntheticMenuClick();
    const bool back = PressedDigitalAction(m_ActionMenuBack)
        || (!m_GameplayEligible && PressedDigitalAction(m_ActionPause));
    if (back && !s_back)
    {
        // New game UI (Qt): in game (pause menu and its pages) Backspace
        // closed the whole menu even from Load/Save/Options, whose own BACK
        // button returns to the pause menu; so Circle presses that button
        // (bottom-right, the last hint: "Back" on pages, "Back to game" on
        // the pause menu). In the main menu Backspace behaves (page back;
        // top level = the game's "Really quit?" dialog) and the corner
        // button there is QUIT, so the key stays. Old UI: Escape.
        const bool newUi = bmvr::NewUiWindow() != nullptr;
        // Gameplay map, not IsInGame: the main menu's background map is "in game".
        const bool inGame = m_GameplayEligible;
        if (newUi && inGame)
        {
            const UINT w = KnownWindowWidth(), h = KnownWindowHeight();
            StartSyntheticMenuClick(static_cast<int>(w * 0.985f), static_cast<int>(h * 0.94f));
        }
        else
            QueueKeyToHwnd(hwnd, newUi ? VK_BACK : VK_ESCAPE);
        Game::logMsg("Menu back %s (%s)", m_GameplayEligible ? "B/MenuBack" : "B/Pause",
            !newUi ? "Escape" : inGame ? "new UI: corner Back button" : "new UI: Backspace");
    }
    s_back = back;
}

// A left click at game-window x,y the way the laser clicks: the pointer
// hovers there first (GetCursorPos hook + WM_MOUSEMOVE), the press and
// release follow ~150 ms later (all at once, the new game UI only
// highlighted the item).
void VR::StartSyntheticMenuClick(int x, int y)
{
    m_SynthClickX = x;
    m_SynthClickY = y;
    m_SynthClickAtMs = GetTickCount() + 150;
}

void VR::TickSyntheticMenuClick()
{
    if (m_SynthClickX < 0)
        return;
    HWND hwnd = FindGameHwnd();
    if (!hwnd)
        return;
    POINT pt{ m_SynthClickX, m_SynthClickY };
    ClientToScreen(hwnd, &pt);
    m_MenuClickHoldScreen.store(PackScreenPoint(pt), std::memory_order_relaxed);
    m_MenuClickHoldUntilMs.store(GetTickCount() + kMenuClickHoldMs, std::memory_order_release);
    SetCursorPos(pt.x, pt.y);
    PostMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(m_SynthClickX, m_SynthClickY));
    if (static_cast<int>(GetTickCount() - m_SynthClickAtMs) < 0)
        return;
    PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(m_SynthClickX, m_SynthClickY));
    PostMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(m_SynthClickX, m_SynthClickY));
    Game::logMsg("Synthetic menu click at %d,%d", m_SynthClickX, m_SynthClickY);
    m_SynthClickX = m_SynthClickY = -1;
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

// bmvr.cfg used to rely on an "exec bmvr" line in bms\cfg\autoexec.cfg, which
// Black Mesa ships and replaces on game updates (2026-10-27): the VR settings
// silently stopped applying. Run the configs from here once per game start
// (after autoexec/config.cfg, so they win), and warn in the headset if the
// file is gone. bmvr_local.cfg is an optional per-player override file.
void VR::ExecVrConfigsOnce()
{
    if (m_VrConfigsExecuted || !m_Game || !m_Game->m_EngineClient)
        return;
    m_VrConfigsExecuted = true;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cfgDir(exe);
    const size_t slash = cfgDir.find_last_of(L"\\/");
    cfgDir = (slash == std::wstring::npos ? std::wstring(L".") : cfgDir.substr(0, slash)) + L"\\bms\\cfg\\";
    auto exists = [&](const wchar_t* name) {
        const DWORD attr = GetFileAttributesW((cfgDir + name).c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (exists(L"bmvr.cfg"))
    {
        const bool ok = m_Game->ClientCmd_Unrestricted("exec bmvr");
        Game::logMsg("VR config: exec bmvr ok=%d", ok ? 1 : 0);
    }
    else
    {
        Game::logMsg("VR config: bms\\cfg\\bmvr.cfg missing");
        ShowToast("VR CONFIG MISSING", 10000, "bms\\cfg\\bmvr.cfg - reinstall the mod");
    }
    if (exists(L"bmvr_local.cfg"))
    {
        const bool ok = m_Game->ClientCmd_Unrestricted("exec bmvr_local");
        Game::logMsg("VR config: exec bmvr_local ok=%d", ok ? 1 : 0);
    }
}

bool VR::HeadAimReadyForSave()
{
    const ULONGLONG now = GetTickCount64();
    if (!m_HeadAimWanted)
    {
        if (!m_GameplayEligible || !m_HmdPoseValid || PauseUiActive())
            return true;
        m_HeadAimWanted = true;
        m_HeadAimSent = 0;
        m_HeadAimGiveUpMs = now + kHeadAimWaitMs;
        return false;
    }
    if (m_HeadAimSent < kHeadAimCmds && now < m_HeadAimGiveUpMs)
        return false;
    Game::logMsg("Save angles: %d usercmds with the head's angles (yaw %.1f) before the save/pause%s",
        m_HeadAimSent, GetViewAngle().y, m_HeadAimSent < kHeadAimCmds ? " (timed out)" : "");
    m_HeadAimWanted = false;
    return true;
}

bool VR::TakeHeadAimCmd()
{
    if (!m_HeadAimWanted)
        return false;
    ++m_HeadAimSent;
    return true;
}

void VR::SyncLadderUseOnly()
{
    // Hands-only ladders: HL2's walk-on auto-mount off, a +use (what a hand
    // grab sends) still mounts. Server cvar, not archived: set once per map
    // and when the setting changes; set back to 0 only if we set it.
    static int s_sent = -1;
    static std::string s_map;
    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
        return;
    // During a hand mount: 0, so walking in (the mount push) mounts like the
    // stick does, with one use press instead of repeated ones (each press
    // that finds nothing plays the deny sound, user 2026-11-19).
    const int want = (bmvr::g_PhysicalLadders && !bmvr::g_LadderStickClimb
        && !m_LadderMountActive.load(std::memory_order_relaxed)) ? 1 : 0;
    if (want == s_sent && s_map == m_CurrentMapName)
        return;
    if (want == 0 && s_sent != 1)
    {
        s_sent = 0;
        s_map = m_CurrentMapName;
        return;
    }
    const bool ok = m_Game->ClientCmd_Unrestricted(want ? "sv_ladder_useonly 1" : "sv_ladder_useonly 0");
    Game::logMsg("Ladders: sv_ladder_useonly %d (%s) ok=%d", want,
        want ? "hands only, no walk-on mount" : "stick climbing", ok ? 1 : 0);
    s_sent = want;
    s_map = m_CurrentMapName;
}

void VR::FlushPendingGameUi()
{
    FlushPendingQuit();
    SyncLadderUseOnly();
    ExecVrConfigsOnce();
    TickWindowFocus();
    FlushPanelEngineCommands();
    FlushCaptureEngineState();
    TickRestoreView();
    if (bmvr::g_FakeRuntime && m_Game)
    {
        char cmd[512];
        while (L4D2VR_FakeHelperPopCommand(cmd, sizeof(cmd)))
        {
            if (HandleRestoreCommand(cmd))
                continue;
            const bool ok = m_Game->ClientCmd_Unrestricted(cmd);
            Game::logMsg("fakehelper cmd '%s' ok=%d", cmd, ok ? 1 : 0);
        }
    }
    if (const int pc = m_PendingPauseCmd.exchange(0, std::memory_order_acq_rel); pc && m_Game)
    {
        const char* cmd = (pc == 1) ? "setpause" : "unpause";
        const bool ok = m_Game->ClientCmd_Unrestricted(cmd);
        Game::logMsg("VR settings %s ok=%d", cmd, ok ? 1 : 0);
    }
    if (m_PendingQuickSave.load(std::memory_order_acquire) && m_Game && HeadAimReadyForSave()
        && m_PendingQuickSave.exchange(false, std::memory_order_acq_rel))
    {
        const bool ok = m_Game->ClientCmd_Unrestricted("save quick");
        Game::logMsg("Quick save ClientCmd_Unrestricted ok=%d", ok ? 1 : 0);
        // The game's own "Saved..." HUD message reaches the HUD layer; only a
        // command that never ran gets a message of ours.
        if (!ok)
            ShowToast("SAVE FAILED");
    }
    if (m_PendingQuickLoad.exchange(false, std::memory_order_acq_rel) && m_Game)
    {
        const bool ok = m_Game->ClientCmd_Unrestricted("load quick");
        Game::logMsg("Quick load ClientCmd_Unrestricted ok=%d", ok ? 1 : 0);
        if (!ok)
            ShowToast("LOAD FAILED");
    }
    // Opening the pause menu: hold it until the head's angles reached the
    // server, so a save from the menu faces where the player looked.
    if (m_PendingGameUi.load(std::memory_order_acquire) == 1 && m_Game && m_SeenGameplay
        && !HeadAimReadyForSave())
        return;
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
        const bool wasVisible = m_GameUiVisible;
        m_GameUiVisible = (op == 1);
        if (op == 1)
            m_GameUiActivateMs = GetTickCount();
        else
            m_GameUiActivateMs = 0;
        if (op == 2 && wasVisible)
            NoteGameUiDismissed();
    }
}

void VR::NoteEngineVGui(void* engineVgui)
{
    if (engineVgui)
        m_EngineVGuiFromPaint = engineVgui;
}

namespace
{
    // vgui2.dll VGUI_Panel009 (VPanelWrapper; slot 0 is the destructor).
    // Slots from the Steam BM vgui2.dll thunks (2026-11-01): 14 SetVisible,
    // 15 IsVisible, 17 GetChildCount, 18 GetChild, 20 GetParent, 37 GetName.
    template <typename R, typename... A>
    R PanelCall(void* ipanel, int slot, A... a)
    {
        using Fn = R(__thiscall*)(void*, A...);
        return (*reinterpret_cast<Fn**>(ipanel))[slot](ipanel, a...);
    }

    constexpr int kPanelGetPos = 3;
    constexpr int kPanelGetSize = 5;
    constexpr int kPanelSetVisible = 14;
    constexpr int kPanelIsVisible = 15;
    constexpr int kPanelGetChildCount = 17;
    constexpr int kPanelGetChild = 18;
    constexpr int kPanelGetParent = 20;
    constexpr int kPanelGetName = 37;

    // HudCredits = the "BL@CK MESA" intro logo, HudMessage = chapter titles,
    // CHudIntroCredits = the developer names (bottom left, env_introcredits
    // on the tram ride; BM's own HUD element, not the SDK's intro credits).
    // CHudBootUpSequence = the HEV suit boot-up (bm_c1a0a suit pickup: system
    // text left, suit figure right); CLJHudBootUpSequence = the long jump one.
    // HudCloseCaption = closed captions / subtitles (closecaption 1; cc_subtitles 1 = dialogue only);
    // the panel hides itself when nothing is said.
    const char* const kTitleHudNames[kTitleHudCount] = { "HudCredits", "HudMessage", "CHudIntroCredits",
        "CHudBootUpSequence", "CLJHudBootUpSequence", "HudCloseCaption" };

    // Depth-first from the vgui root; fills found[i] for kTitleHudNames[i].
    bool SehFindTitleHudPanels(void* ipanel, unsigned int root, unsigned int* found)
    {
        BMVR_TRY
        {
            unsigned int stack[64];
            int sp = 0;
            int visited = 0;
            stack[sp++] = root;
            while (sp > 0 && visited < 4096)
            {
                const unsigned int p = stack[--sp];
                ++visited;
                const char* name = PanelCall<const char*>(ipanel, kPanelGetName, p);
                if (name)
                {
                    for (int i = 0; i < kTitleHudCount; ++i)
                    {
                        if (!found[i] && std::strcmp(name, kTitleHudNames[i]) == 0)
                            found[i] = p;
                    }
                }
                bool all = true;
                for (int i = 0; i < kTitleHudCount && all; ++i)
                    all = found[i] != 0;
                if (all)
                    return true;
                const int n = PanelCall<int>(ipanel, kPanelGetChildCount, p);
                for (int c = 0; c < n && sp < 64; ++c)
                {
                    const unsigned int child = PanelCall<unsigned int>(ipanel, kPanelGetChild, p, c);
                    if (child)
                        stack[sp++] = child;
                }
            }
            return found[0] || found[1];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    constexpr int kPanelGetAbsPos = 10;
    constexpr int kPanelGetClipRect = 11;

    // One line per panel from p up to the root: what VGUI clips the title
    // paint against (abs pos + clip rect are only valid during a paint).
    void SehLogPanelChain(void* ipanel, unsigned int p, const char* tag)
    {
        BMVR_TRY
        {
            for (int depth = 0; p && depth < 12; ++depth)
            {
                int x = 0, y = 0, w = 0, h = 0, ax = 0, ay = 0, c0 = 0, c1 = 0, c2 = 0, c3 = 0;
                PanelCall<void, unsigned int, int*, int*>(ipanel, kPanelGetPos, p, &x, &y);
                PanelCall<void, unsigned int, int*, int*>(ipanel, kPanelGetSize, p, &w, &h);
                PanelCall<void, unsigned int, int*, int*>(ipanel, kPanelGetAbsPos, p, &ax, &ay);
                PanelCall<void, unsigned int, int*, int*, int*, int*>(ipanel, kPanelGetClipRect, p,
                    &c0, &c1, &c2, &c3);
                const char* name = PanelCall<const char*>(ipanel, kPanelGetName, p);
                Game::logMsg("Title HUD chain %s d%d %s vis=%d pos %d,%d size %dx%d abs %d,%d clip %d,%d-%d,%d",
                    tag, depth, name ? name : "?", PanelCall<bool>(ipanel, kPanelIsVisible, p) ? 1 : 0,
                    x, y, w, h, ax, ay, c0, c1, c2, c3);
                p = PanelCall<unsigned int>(ipanel, kPanelGetParent, p);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // x, y, wide, tall of a panel (relative to its parent); false on a fault.
    bool SehPanelBounds(void* ipanel, unsigned int p, int* out)
    {
        BMVR_TRY
        {
            PanelCall<void, unsigned int, int*, int*>(ipanel, kPanelGetPos, p, &out[0], &out[1]);
            PanelCall<void, unsigned int, int*, int*>(ipanel, kPanelGetSize, p, &out[2], &out[3]);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SehPanelVisible(void* ipanel, unsigned int p)
    {
        BMVR_TRY
        {
            return PanelCall<bool>(ipanel, kPanelIsVisible, p);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Children of p: total, visible; names of the first few into buf.
    int SehVisibleChildren(void* ipanel, unsigned int p, int* total, char* buf, size_t bufLen)
    {
        BMVR_TRY
        {
            const int n = PanelCall<int>(ipanel, kPanelGetChildCount, p);
            int vis = 0;
            size_t used = 0;
            if (buf && bufLen)
                buf[0] = 0;
            for (int c = 0; c < n; ++c)
            {
                const unsigned int child = PanelCall<unsigned int>(ipanel, kPanelGetChild, p, c);
                const bool v = child && PanelCall<bool>(ipanel, kPanelIsVisible, child);
                if (v)
                    ++vis;
                const char* name = child ? PanelCall<const char*>(ipanel, kPanelGetName, child) : nullptr;
                if (buf && used + 40 < bufLen && c < 8)
                    used += static_cast<size_t>(snprintf(buf + used, bufLen - used, "%s%s=%d", c ? " " : "", name ? name : "?", v ? 1 : 0));
            }
            if (total)
                *total = n;
            return vis;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    int SehIsolatePanels(void* ipanel, const unsigned int* targets, int targetCount,
        unsigned int* hidden, int cap)
    {
        int count = 0;
        BMVR_TRY
        {
            unsigned int keep[40];
            int keepCount = 0;
            unsigned int parents[32];
            int parentCount = 0;
            for (int t = 0; t < targetCount; ++t)
            {
                if (keepCount < 40)
                    keep[keepCount++] = targets[t];
                unsigned int p = PanelCall<unsigned int>(ipanel, kPanelGetParent, targets[t]);
                for (int depth = 0; p && depth < 16 && keepCount < 40 && parentCount < 32; ++depth)
                {
                    keep[keepCount++] = p;
                    parents[parentCount++] = p;
                    p = PanelCall<unsigned int>(ipanel, kPanelGetParent, p);
                }
            }
            for (int a = 0; a < parentCount; ++a)
            {
                const unsigned int parent = parents[a];
                const int n = PanelCall<int>(ipanel, kPanelGetChildCount, parent);
                for (int c = 0; c < n && count < cap; ++c)
                {
                    const unsigned int child = PanelCall<unsigned int>(ipanel, kPanelGetChild, parent, c);
                    if (!child)
                        continue;
                    bool kept = false;
                    for (int k = 0; k < keepCount && !kept; ++k)
                        kept = keep[k] == child;
                    for (int h = 0; h < count && !kept; ++h)
                        kept = hidden[h] == child;
                    if (kept || !PanelCall<bool>(ipanel, kPanelIsVisible, child))
                        continue;
                    PanelCall<void>(ipanel, kPanelSetVisible, child, false);
                    hidden[count++] = child;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return count;
    }

    constexpr int kPanelSetSize = 4;

    // Sizes p and its parents that are bigger than w x h down to w x h;
    // returns how many were changed (old sizes in sizes[]).
    int SehFitPanelChain(void* ipanel, unsigned int p, int w, int h,
        unsigned int* panels, int (*sizes)[2], int count, int cap)
    {
        BMVR_TRY
        {
            for (int depth = 0; p && depth < 12 && count < cap; ++depth)
            {
                int pw = 0, ph = 0;
                PanelCall<void, unsigned int, int*, int*>(ipanel, kPanelGetSize, p, &pw, &ph);
                bool seen = false;
                for (int i = 0; i < count && !seen; ++i)
                    seen = panels[i] == p;
                if (!seen && (pw > w || ph > h))
                {
                    panels[count] = p;
                    sizes[count][0] = pw;
                    sizes[count][1] = ph;
                    ++count;
                    PanelCall<void, unsigned int, int, int>(ipanel, kPanelSetSize, p, w, h);
                }
                p = PanelCall<unsigned int>(ipanel, kPanelGetParent, p);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return count;
    }

    // Boot-up sequence: the visible children's rects (x, y, w, h, dx) on the
    // full-screen boot panel. Children left of the centre band get dx = inset,
    // right of it -inset, the centred title/standby lines 0. Setting the
    // children's positions for the paint did not move them (they place
    // themselves while painting, 2026-11-24), so the layer texture is moved
    // per rect instead (TitleLayerPremultiplied).
    int SehBootChildRects(void* ipanel, unsigned int p, int screenW, int inset, int (*rects)[5], int cap)
    {
        int count = 0;
        BMVR_TRY
        {
            const int n = PanelCall<int>(ipanel, kPanelGetChildCount, p);
            const int mid = screenW / 2, band = screenW / 20;
            for (int c = 0; c < n && count < cap; ++c)
            {
                const unsigned int child = PanelCall<unsigned int>(ipanel, kPanelGetChild, p, c);
                if (!child || !PanelCall<bool>(ipanel, kPanelIsVisible, child))
                    continue;
                int x = 0, y = 0, w = 0, h = 0;
                PanelCall<void, unsigned int, int*, int*>(ipanel, kPanelGetPos, child, &x, &y);
                PanelCall<void, unsigned int, int*, int*>(ipanel, kPanelGetSize, child, &w, &h);
                if (w <= 0 || h <= 0)
                    continue;
                const int cx = x + w / 2;
                rects[count][0] = x;
                rects[count][1] = y;
                rects[count][2] = w;
                rects[count][3] = h;
                rects[count][4] = cx < mid - band ? inset : (cx > mid + band ? -inset : 0);
                ++count;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return count;
    }

    void SehRestorePanelSizes(void* ipanel, const unsigned int* panels, const int (*sizes)[2], int count)
    {
        BMVR_TRY
        {
            for (int i = count - 1; i >= 0; --i)
                PanelCall<void, unsigned int, int, int>(ipanel, kPanelSetSize, panels[i], sizes[i][0], sizes[i][1]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void SehShowPanels(void* ipanel, const unsigned int* panels, int count)
    {
        BMVR_TRY
        {
            for (int i = 0; i < count; ++i)
                PanelCall<void>(ipanel, kPanelSetVisible, panels[i], true);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

namespace
{
    // env_screeneffect overlays (episodic intro blur, stun, EP2 stun). Each
    // draws an earlier 1024x512 corner copy of the frame over the eye, which
    // in VR is the other eye's, then saves this eye's top-left corner: a flat,
    // mirrored second view over the stereo scene (c2a3c ambush drag). Black
    // Mesa draws them without DrawScreenSpaceRectangle, so the DSSR skip
    // missed them (audit, 2026-11-02).
    //
    // [3] Black Mesa's global Xen fog (Xog) pass: its scrolling noise is looked up by view direction only, so
    // in stereo it sits at infinite depth and moves with the player ("under the floor", 2026-11-14), and it
    // shows inside rooms where flat has none (Xen research silos). User (2026-11-16): hidden is far better.
    constexpr int kScreenFxCount = 4;
    const char* const kVrSkippedScreenEffects[kScreenFxCount] = { "__ep1introeffect", "__stuneffect", "__ep2stuneffect",
        "__gbxog0" };

    // Code-made materials are not in FindMaterial's lookup; walk the list.
    int SehFindScreenEffectMaterials(void* matsys, IMaterial** out)
    {
        int found = 0;
        BMVR_TRY
        {
            void** vt = *reinterpret_cast<void***>(matsys);
            using tFirst = unsigned short(__thiscall*)(void*);
            using tNext = unsigned short(__thiscall*)(void*, unsigned short);
            using tGet = IMaterial*(__thiscall*)(void*, unsigned short);
            const auto first = reinterpret_cast<tFirst>(vt[Offsets::kIMaterialSystem_FirstMaterialVt]);
            const auto next = reinterpret_cast<tNext>(vt[Offsets::kIMaterialSystem_NextMaterialVt]);
            const auto invalid = reinterpret_cast<tFirst>(vt[Offsets::kIMaterialSystem_InvalidMaterialVt]);
            const auto get = reinterpret_cast<tGet>(vt[Offsets::kIMaterialSystem_GetMaterialVt]);
            const unsigned short end = invalid(matsys);
            for (unsigned short h = first(matsys); h != end; h = next(matsys, h))
            {
                IMaterial* m = get(matsys, h);
                const char* name = m ? m->GetName() : nullptr;
                if (!name || name[0] != '_' || name[1] != '_')
                    continue;
                for (int i = 0; i < kScreenFxCount; ++i)
                {
                    if (!out[i] && std::strcmp(name, kVrSkippedScreenEffects[i]) == 0)
                    {
                        out[i] = m;
                        ++found;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return found;
    }

    int SehNumMaterials(void* matsys)
    {
        BMVR_TRY
        {
            using tNum = int(__thiscall*)(void*);
            return reinterpret_cast<tNum>((*reinterpret_cast<void***>(matsys))[Offsets::kIMaterialSystem_GetNumMaterialsVt])(matsys);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    void SehSetNoDraw(IMaterial* m, bool on)
    {
        BMVR_TRY
        {
            using tSetFlag = void(__thiscall*)(IMaterial*, int, bool);
            reinterpret_cast<tSetFlag>((*reinterpret_cast<void***>(m))[Offsets::kIMaterial_SetMaterialVarFlagVt])(
                m, MATERIAL_VAR_NO_DRAW, on);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

// The game creates the screen-effect materials when an effect first starts,
// not at map load. Per frame: the material count (one call); the list is
// only walked when it changed while one of the three is still missing. The
// NO_DRAW flag only changes with VR / bmvr_skip_screen_effects.
void VR::UpdateScreenEffectNoDraw()
{
    if (!m_Game || !m_Game->m_MaterialSystem)
        return;
    void* matsys = m_Game->m_MaterialSystem;
    if (m_ScreenFxMap != m_CurrentMapName)
    {
        m_ScreenFxMap = m_CurrentMapName;
        for (int i = 0; i < kScreenFxCount; ++i)
        {
            m_ScreenFxMaterials[i] = nullptr;
            m_ScreenFxNoDraw[i] = false;
        }
        m_ScreenFxMaterialCount = -1;
    }
    bool missing = false;
    for (IMaterial* m : m_ScreenFxMaterials)
        missing |= !m;
    if (missing)
    {
        const int count = SehNumMaterials(matsys);
        if (count != m_ScreenFxMaterialCount)
        {
            m_ScreenFxMaterialCount = count;
            IMaterial* before[kScreenFxCount];
            for (int i = 0; i < kScreenFxCount; ++i)
                before[i] = m_ScreenFxMaterials[i];
            LARGE_INTEGER f{}, t0{}, t1{};
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            SehFindScreenEffectMaterials(matsys, m_ScreenFxMaterials);
            QueryPerformanceCounter(&t1);
            const double ms = f.QuadPart ? 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart : 0.0;
            for (int i = 0; i < kScreenFxCount; ++i)
            {
                if (m_ScreenFxMaterials[i] && !before[i])
                {
                    Game::logMsg("Screen effect %s found (%d materials, scan %.2f ms)",
                        kVrSkippedScreenEffects[i], count, ms);
                    m_ScreenFxNoDraw[i] = false;   // applied below
                }
            }
        }
    }
    for (int i = 0; i < kScreenFxCount; ++i)
    {
        IMaterial* m = m_ScreenFxMaterials[i];
        if (!m)
            continue;
        const bool want = m_IsVREnabled && (i == 3 ? bmvr::g_XenGlobalFogOff : bmvr::g_VrSkipScreenEffects);
        if (want == m_ScreenFxNoDraw[i])
            continue;
        m_ScreenFxNoDraw[i] = want;
        SehSetNoDraw(m, want);
        Game::logMsg("%s %s in VR", i == 3 ? "Xen global fog (__gbxog0)" : kVrSkippedScreenEffects[i], want ? "off" : "on");
    }
}

namespace
{
    // 0 = leave alone (own weapons/hands, effects, UI), 1 = world, 2 = models.
    // Cubemap-patched map materials are named maps/<map>/<original name>.
    int EnvmapClass(const char* name)
    {
        if (_strnicmp(name, "maps/", 5) == 0)
        {
            const char* slash = std::strchr(name + 5, '/');
            if (!slash)
                return 0;
            name = slash + 1;
        }
        if (_strnicmp(name, "models/weapons/", 15) == 0)
            return 0;
        if (_strnicmp(name, "models/", 7) == 0)
            return 2;
        static const char* const kSkip[] = { "_", "sprites/", "effects/", "particle/", "particles/", "vgui/",
            "engine/", "debug/", "dev/", "tools/", "console/", "hud/", "overlays/", "decals/" };
        for (const char* p : kSkip)
        {
            if (_strnicmp(name, p, std::strlen(p)) == 0)
                return 0;
        }
        return 1;
    }

    // $envmaptint as a vector var, or null.
    void* SehFindEnvmapTint(IMaterial* m, float* rgb)
    {
        BMVR_TRY
        {
            using tFindVar = void*(__thiscall*)(IMaterial*, const char*, bool*, bool);
            bool found = false;
            void* var = reinterpret_cast<tFindVar>((*reinterpret_cast<void***>(m))[Offsets::kIMaterial_FindVarVt])(
                m, "$envmaptint", &found, false);
            if (!found || !var)
                return nullptr;
            const auto* bytes = reinterpret_cast<const unsigned char*>(var);
            if ((bytes[Offsets::kIMaterialVar_TypeOffset] & 0xf) != 2)
                return nullptr;
            std::memcpy(rgb, bytes + Offsets::kIMaterialVar_VecValOffset, 3 * sizeof(float));
            return var;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void SehSetVec3(void* var, const float* rgb)
    {
        BMVR_TRY
        {
            using tSet = void(__thiscall*)(void*, float, float, float);
            reinterpret_cast<tSet>((*reinterpret_cast<void***>(var))[Offsets::kIMaterialVar_SetVecValue3Vt])(
                var, rgb[0], rgb[1], rgb[2]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    struct EnvmapWalkStats
    {
        int materials = 0;
        int tinted[3] = {};
        int changed = 0;
    };

    using EnvmapVisit = void (*)(void* ctx, IMaterial* m, const char* name);

    bool SehWalkMaterials(void* matsys, EnvmapVisit visit, void* ctx, int* count)
    {
        BMVR_TRY
        {
            void** vt = *reinterpret_cast<void***>(matsys);
            using tFirst = unsigned short(__thiscall*)(void*);
            using tNext = unsigned short(__thiscall*)(void*, unsigned short);
            using tGet = IMaterial*(__thiscall*)(void*, unsigned short);
            const auto first = reinterpret_cast<tFirst>(vt[Offsets::kIMaterialSystem_FirstMaterialVt]);
            const auto next = reinterpret_cast<tNext>(vt[Offsets::kIMaterialSystem_NextMaterialVt]);
            const auto invalid = reinterpret_cast<tFirst>(vt[Offsets::kIMaterialSystem_InvalidMaterialVt]);
            const auto get = reinterpret_cast<tGet>(vt[Offsets::kIMaterialSystem_GetMaterialVt]);
            const unsigned short end = invalid(matsys);
            for (unsigned short h = first(matsys); h != end; h = next(matsys, h))
            {
                IMaterial* m = get(matsys, h);
                const char* name = m ? m->GetName() : nullptr;
                ++*count;
                if (name)
                    visit(ctx, m, name);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

// Cubemap reflection strength per material class, as one-time material
// state: $envmaptint = its own value x the class scale (the shaders read it
// per draw). The whole list is walked when the map or a scale changes, or
// when one scaled material reads back unscaled (reloads such as
// mat_specular). A changed material count (checked every 2 s) only visits
// materials not seen before: firing and breaking props load materials
// mid-map, and a full walk of ~7000 cost 0.2-0.5 s on the game thread each
// time (stutter, 2026-11-03). Other frames pay a few compares. Each material
// keeps its original tint and the value written; a tint that no longer
// matches what was written was reloaded by the game and becomes the new
// original.
void VR::UpdateEnvmapScale()
{
    if (!m_Game || !m_Game->m_MaterialSystem)
        return;
    const float wantWorld = m_IsVREnabled ? bmvr::g_EnvmapScaleWorld : 1.f;
    const float wantModels = m_IsVREnabled ? bmvr::g_EnvmapScaleModels : 1.f;
    const bool scalesChanged = wantWorld != m_EnvmapAppliedWorld || wantModels != m_EnvmapAppliedModels;
    const bool mapChanged = m_EnvmapMap != m_CurrentMapName;
    const bool idle = wantWorld == 1.f && wantModels == 1.f && m_EnvmapTints.empty();
    if (idle && !scalesChanged)
    {
        m_EnvmapMap = m_CurrentMapName;
        return;
    }
    void* matsys = m_Game->m_MaterialSystem;
    const unsigned long long now = GetTickCount64();
    bool newOnly = false;
    if (!scalesChanged && !mapChanged)
    {
        if (now < m_EnvmapNextWalkMs)
            return;
        m_EnvmapNextWalkMs = now + 2000;
        const int count = SehNumMaterials(matsys);
        // mat_specular and other reloads keep the count but rebuild the
        // vars: read one scaled material back.
        bool reloaded = false;
        if (!m_EnvmapTints.empty())
        {
            const auto& probe = *m_EnvmapTints.begin();
            float cur[3];
            reloaded = !SehFindEnvmapTint(probe.first, cur) || std::memcmp(cur, probe.second.written, sizeof(cur)) != 0;
        }
        if (!reloaded)
        {
            if (count == m_EnvmapMaterialCount)
                return;
            newOnly = count > m_EnvmapMaterialCount && !m_EnvmapKnown.empty();
        }
    }
    m_EnvmapNextWalkMs = now + 2000;
    m_EnvmapMap = m_CurrentMapName;
    m_EnvmapAppliedWorld = wantWorld;
    m_EnvmapAppliedModels = wantModels;

    struct Ctx
    {
        VR* vr;
        float scale[3];
        EnvmapWalkStats stats;
        std::unordered_map<IMaterial*, EnvmapTintEntry> seen;
        std::unordered_set<IMaterial*> known;
        bool newOnly;
    } ctx{ this, { 1.f, wantWorld, wantModels }, {}, {}, {}, newOnly };
    if (newOnly)
        ctx.seen = m_EnvmapTints;
    else
        ctx.seen.reserve(m_EnvmapTints.size() + 256);
    ctx.known.reserve(m_EnvmapKnown.size() + 256);

    LARGE_INTEGER f{}, t0{}, t1{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int count = 0;
    const bool ok = SehWalkMaterials(matsys, [](void* p, IMaterial* m, const char* name) {
        auto& c = *static_cast<Ctx*>(p);
        c.known.insert(m);
        if (c.newOnly && c.vr->m_EnvmapKnown.count(m))
            return;
        const int cls = EnvmapClass(name);
        auto it = c.vr->m_EnvmapTints.find(m);
        // Unscaled class and nothing to restore: do not touch (FindVar
        // precaches the material's vars).
        if (cls == 0 || (c.scale[cls] == 1.f && it == c.vr->m_EnvmapTints.end()))
            return;
        float cur[3];
        void* var = SehFindEnvmapTint(m, cur);
        if (!var)
            return;
        ++c.stats.tinted[cls];
        EnvmapTintEntry e{};
        if (it != c.vr->m_EnvmapTints.end() && std::memcmp(cur, it->second.written, sizeof(cur)) == 0)
            std::memcpy(e.orig, it->second.orig, sizeof(e.orig));
        else
            std::memcpy(e.orig, cur, sizeof(e.orig));
        for (int i = 0; i < 3; ++i)
            e.written[i] = e.orig[i] * c.scale[cls];
        if (std::memcmp(cur, e.written, sizeof(cur)) != 0)
        {
            SehSetVec3(var, e.written);
            ++c.stats.changed;
        }
        if (c.scale[cls] != 1.f)
            c.seen[m] = e;
    }, &ctx, &count);
    QueryPerformanceCounter(&t1);
    m_EnvmapTints.swap(ctx.seen);
    m_EnvmapKnown.swap(ctx.known);
    m_EnvmapMaterialCount = ok ? SehNumMaterials(matsys) : -1;
    const double ms = f.QuadPart ? 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart : 0.0;
    static int s_newOnlyLog;
    if ((ctx.stats.changed || scalesChanged || mapChanged || ms > 20.0) && (!newOnly || s_newOnlyLog < 20))
    {
        if (newOnly)
            ++s_newOnlyLog;
        Game::logMsg("Envmap scale world %.2f models %.2f: %d materials%s, tinted world %d models %d, %d changed (%.2f ms)%s",
            wantWorld, wantModels, count, newOnly ? " (new only)" : "", ctx.stats.tinted[1], ctx.stats.tinted[2],
            ctx.stats.changed, ms, ok ? "" : " WALK FAULTED");
    }
}

void VR::UpdateTitleHud()
{
    const bool was = m_TitleHudActive;
    m_TitleHudActive = false;
    if (!m_IsVREnabled || !m_GameplayEligible || !m_SeenGameplay || PauseUiActive())
        return;
    if (!m_VguiPanel && m_Game)
        m_VguiPanel = m_Game->GetInterface("vgui2.dll", "VGUI_Panel009");
    if (!m_VguiPanel || !m_Game->m_EngineVGui)
        return;
    bool lateMissing = false;
    for (int i = 2; i < kTitleHudCount; ++i)
        lateMissing = lateMissing || !m_TitleHudPanels[i];
    if (!m_TitleHudPanels[0] || !m_TitleHudPanels[1] || (lateMissing && m_TitleHudSearches < 30))
    {
        const unsigned long long now = GetTickCount64();
        if (now >= m_TitleHudNextSearchMs)
        {
            m_TitleHudNextSearchMs = now + 2000;
            ++m_TitleHudSearches;
            // PANEL_ROOT = 0.
            const unsigned int root = static_cast<unsigned int>(
                reinterpret_cast<uintptr_t>(m_Game->m_EngineVGui->GetPanel(0)));
            if (root && SehFindTitleHudPanels(m_VguiPanel, root, m_TitleHudPanels))
            {
                Game::logMsg("Title HUD panels: HudCredits=%08X HudMessage=%08X CHudIntroCredits=%08X"
                    " CHudBootUpSequence=%08X CLJHudBootUpSequence=%08X HudCloseCaption=%08X",
                    m_TitleHudPanels[0], m_TitleHudPanels[1], m_TitleHudPanels[2], m_TitleHudPanels[3],
                    m_TitleHudPanels[4], m_TitleHudPanels[5]);
            }
        }
    }
    const bool bootWas = m_BootHudActive;
    const bool captionWas = m_CaptionHudActive;
    m_BootHudActive = false;
    m_CaptionHudActive = false;
    for (int i = 0; i < kTitleHudCount; ++i)
    {
        if (TitleHudPanelShowing(i))
        {
            m_TitleHudActive = true;
            if (i >= kBootHudFirst && i < kBootHudEnd)
                m_BootHudActive = true;
            if (i == kCaptionHud)
                m_CaptionHudActive = true;
        }
    }
    m_CaptionOnly = m_CaptionHudActive;
    for (int i = 0; i < kTitleHudCount && m_CaptionOnly; ++i)
        if (i != kCaptionHud && TitleHudPanelShowing(i))
            m_CaptionOnly = false;
    if (m_CaptionHudActive && !captionWas)
    {
        int b[4] = {};
        if (SehPanelBounds(m_VguiPanel, m_TitleHudPanels[kCaptionHud], b) && b[3] > 0 && b[1] + b[3] <= 1080)
            m_CaptionCentreY = (b[1] + b[3] * 0.5f) / 1080.f;
    }
    if (m_CaptionHudActive != captionWas)
    {
        static int s_capLogs;
        if (s_capLogs < 60)
        {
            ++s_capLogs;
            int b[4] = {};
            SehPanelBounds(m_VguiPanel, m_TitleHudPanels[kCaptionHud], b);
            Game::logMsg("Caption HUD %s bounds %d,%d %dx%d", m_CaptionHudActive ? "shown (head-locked title layer)" :
                "hidden", b[0], b[1], b[2], b[3]);
        }
    }
    LogBootHudChildren();
    if (m_BootHudActive != bootWas)
        Game::logMsg("Boot-up HUD %s", m_BootHudActive ? "shown (head-locked title layer)" : "hidden");
    if (m_TitleHudActive != was)
    {
        // New epoch: with TitleLevel the helper places the HUD layer level at
        // the head's yaw once per epoch (otherwise the layer is head-locked).
        if (m_TitleHudActive && ++m_MenuOverlayEpoch == 0)
            m_MenuOverlayEpoch = 1;
        Game::logMsg("Title HUD %s (credits=%d message=%d names=%d boot=%d ljboot=%d)",
            m_TitleHudActive ? "shown" : "hidden", TitleHudPanelShowing(0) ? 1 : 0, TitleHudPanelShowing(1) ? 1 : 0,
            TitleHudPanelShowing(2) ? 1 : 0, TitleHudPanelShowing(3) ? 1 : 0, TitleHudPanelShowing(4) ? 1 : 0);
        for (int i = 0; m_TitleHudActive && i < kTitleHudCount; ++i)
        {
            int b[4] = {};
            if (m_TitleHudPanels[i] && SehPanelBounds(m_VguiPanel, m_TitleHudPanels[i], b))
                Game::logMsg("Title HUD %s bounds %d,%d %dx%d", kTitleHudNames[i], b[0], b[1], b[2], b[3]);
        }
    }
}

// CHudIntroCredits (developer names) is visible all game; it has one
// label child per name on screen (0 before env_introcredits Activate and
// after the last name; log 2026-11-23), so it counts only with children.
bool VR::TitleHudPanelShowing(int i) const
{
    const unsigned int p = (i >= 0 && i < kTitleHudCount) ? m_TitleHudPanels[i] : 0;
    if (!p || !m_VguiPanel || !SehPanelVisible(m_VguiPanel, p))
        return false;
    if ((i != 2 && i < kBootHudFirst) || i == kCaptionHud)
        return true;
    int total = 0;
    return SehVisibleChildren(m_VguiPanel, p, &total, nullptr, 0) > 0;
}

bool VR::WantTitleHudOverlay() const
{
    return m_TitleHudActive && !WantPauseWorldOverlay() && !Want2dMenuPanel();
}

int VR::IsolateTitleHudPanels(unsigned int* hidden, int cap)
{
    if (!m_VguiPanel)
        return 0;
    unsigned int targets[kTitleHudCount];
    int n = 0;
    for (int i = 0; i < kTitleHudCount; ++i)
    {
        if (TitleHudPanelShowing(i))
        {
            targets[n++] = m_TitleHudPanels[i];
            static int s_chainLog[kTitleHudCount];
            if (s_chainLog[i] < 2)
            {
                SehLogPanelChain(m_VguiPanel, m_TitleHudPanels[i], kTitleHudNames[i]);
                ++s_chainLog[i];
            }
        }
    }
    // The client root follows the eye-sized screen (3472x3536 at the user's
    // RenderScale), and HudCredits fills it. Painted that way into the
    // 1280x640 HUD RT the title paint issued no draw at all (audit,
    // 2026-11-01); at eyes up to ~1300 px the root stays window-sized and the
    // logo shows. Shrinking the chain to the RT for this paint brings it back.
    m_TitleFitCount = 0;
    UINT tw = 0, th = 0;
    if (n && HudOverlayPixelSize(tw, th))
    {
        for (int i = 0; i < n; ++i)
            m_TitleFitCount = SehFitPanelChain(m_VguiPanel, targets[i], static_cast<int>(tw),
                static_cast<int>(th), m_TitleFitPanels, m_TitleFitSizes, m_TitleFitCount, 16);
        static int s_fitLog;
        if (m_TitleFitCount && s_fitLog < 4)
        {
            Game::logMsg("Title HUD fitted %d panels to %ux%u for the paint", m_TitleFitCount, tw, th);
            ++s_fitLog;
        }
    }
    // Boot-up: the parts' rects for the texture-space move toward the centre.
    {
        BootHudRects r{};
        const int inset = static_cast<int>(bmvr::g_BootHudInset + 0.5f);
        for (int i = kBootHudFirst; i < kBootHudEnd && !r.boot; ++i)
            r.boot = TitleHudPanelShowing(i);
        // Captions read better without the glow too (user 2026-11-25), but the
        // intro logo / names / messages keep it when they share the layer.
        r.noGlow = r.boot || (TitleHudPanelShowing(kCaptionHud) && !TitleHudPanelShowing(0)
            && !TitleHudPanelShowing(1) && !TitleHudPanelShowing(2));
        // Captions sharing the layer with a chapter title / logo / names:
        // keep the glow for those, but not over the caption box (user
        // 2026-11-25: chapter title glow also hit the subtitles).
        if (!r.noGlow && TitleHudPanelShowing(kCaptionHud) && tw && th)
        {
            int b[4] = {};
            if (SehPanelBounds(m_VguiPanel, m_TitleHudPanels[kCaptionHud], b) && b[2] > 0 && b[3] > 0)
            {
                const float pad = 8.f;
                r.capMask = true;
                r.capRect[0] = (b[0] - pad) / tw;
                r.capRect[1] = (b[1] - pad) / th;
                r.capRect[2] = (b[0] + b[2] + pad) / tw;
                r.capRect[3] = (b[1] + b[3] + pad) / th;
            }
        }
        for (int i = kBootHudFirst; i < kBootHudEnd && inset > 0 && !r.count; ++i)
        {
            int b[4] = {};
            if (!TitleHudPanelShowing(i) || !SehPanelBounds(m_VguiPanel, m_TitleHudPanels[i], b) || b[2] < 640)
                continue;
            r.panelW = b[2];
            r.panelH = b[3];
            r.count = SehBootChildRects(m_VguiPanel, m_TitleHudPanels[i], b[2], inset * b[2] / 1920, r.rects,
                kBootHudMaxRects);
        }
        PublishBootHudRects(r);
    }
    return n ? SehIsolatePanels(m_VguiPanel, targets, n, hidden, cap) : 0;
}

// Boot-up sequences: log the panel and its children (name, visible, bounds)
// whenever the visible set changes, to place the parts in VR (left text,
// suit figure, centre notices). Bounded output.
void VR::LogBootHudChildren()
{
    static unsigned int s_sig[kTitleHudCount];
    static int s_logs;
    if (!m_VguiPanel || s_logs >= 80)
        return;
    for (int i = kBootHudFirst; i < kBootHudEnd; ++i)
    {
        const unsigned int p = m_TitleHudPanels[i];
        if (!p)
            continue;
        const bool vis = SehPanelVisible(m_VguiPanel, p);
        int total = 0;
        char names[400];
        const int visKids = vis ? SehVisibleChildren(m_VguiPanel, p, &total, names, sizeof(names)) : 0;
        unsigned int sig = (vis ? 1u : 0u) | (static_cast<unsigned int>(visKids) << 1) |
            (static_cast<unsigned int>(total) << 12);
        if (sig == s_sig[i])
            continue;
        s_sig[i] = sig;
        ++s_logs;
        int b[4] = {};
        SehPanelBounds(m_VguiPanel, p, b);
        Game::logMsg("Boot HUD %s vis=%d bounds %d,%d %dx%d children %d/%d: %s", kTitleHudNames[i], vis ? 1 : 0,
            b[0], b[1], b[2], b[3], visKids, total, vis ? names : "");
        if (!vis)
            continue;
        BMVR_TRY
        {
            for (int c = 0; c < total && c < 24; ++c)
            {
                const unsigned int child = PanelCall<unsigned int>(m_VguiPanel, kPanelGetChild, p, c);
                if (!child)
                    continue;
                int cb[4] = {};
                SehPanelBounds(m_VguiPanel, child, cb);
                const char* name = PanelCall<const char*>(m_VguiPanel, kPanelGetName, child);
                Game::logMsg("Boot HUD   child %d %s vis=%d bounds %d,%d %dx%d", c, name ? name : "?",
                    SehPanelVisible(m_VguiPanel, child) ? 1 : 0, cb[0], cb[1], cb[2], cb[3]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

void VR::RestoreTitleHudPanels(const unsigned int* hidden, int count)
{
    if (m_VguiPanel && count > 0)
        SehShowPanels(m_VguiPanel, hidden, count);
    if (m_VguiPanel && m_TitleFitCount > 0)
        SehRestorePanelSizes(m_VguiPanel, m_TitleFitPanels, m_TitleFitSizes, m_TitleFitCount);
    m_TitleFitCount = 0;
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
    Game::logMsg("GameUI dismissed follow-up overlay=%d pause=%d",
        Want2dMenuPanel() ? 1 : 0, WantPauseWorldOverlay() ? 1 : 0);
    NoteGameUiDismissed();
}

void VR::NoteGameUiDismissed()
{
    // In a map only: the main menu and load screens have no head origin to
    // fix, and the latch itself waits for gameplay.
    if (!bmvr::g_AutoRecenterOnResume || !m_GameplayEligible)
        return;
    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
        return;
    RecenterPosition("pause menu closed");
}

void VR::NoteAppActivated(bool activated)
{
    const bool was = m_AppActivated.exchange(activated, std::memory_order_relaxed);
    if (was == activated)
        return;
    // Snapshot audio before the engine's wndproc runs S_Block / focus-mute.
    NoteAudioFocusTransition(activated);
    Game::logMsg("App %s (WM_ACTIVATEAPP); %s OpenXR copies while GameUI is up",
        activated ? "activated" : "deactivated",
        activated ? "resume" : "skip");
}

void VR::TickWindowFocus()
{
    if (!m_Game || !m_IsVREnabled)
        return;
    static DWORD s_lastPollMs;
    static int s_focused = -1;          // -1 unknown, 0 other app, 1 game
    static bool s_releasePending;
    static DWORD s_releaseAtMs;
    const DWORD now = GetTickCount();
    if (s_lastPollMs != 0 && now - s_lastPollMs < 50)
        return;
    s_lastPollMs = now;
    HWND game = bmvr::GameWindow();
    if (!game)
        return;
    HWND fg = GetForegroundWindow();
    const int focused = fg == game ? 1 : 0;
    if (focused != s_focused)
    {
        if (s_focused != -1)
            Game::logMsg("Game window %s (foreground=%p game=%p)",
                focused ? "back in the foreground" : "lost the foreground", (void*)fg, (void*)game);
        s_focused = focused;
        // Release on the way out too, then again once the refocusing click
        // has been delivered (its press arrives just after the switch).
        s_releasePending = true;
        s_releaseAtMs = focused ? now + 300 : now;
    }
    // Desktop mouse buttons while the game is in front (user 2026-11-19:
    // after alt-tabbing back the gun kept firing and pause-menu items were
    // clicked over and over on hover, not by the VR pointer). In VR the
    // desktop mouse is not used: log every change of the left/right button
    // as Windows reports it, and release one reported held > 0.5 s with a
    // real button-up (bmvr_desktop_mouse_guard), which also clears a press
    // whose release went to another window.
    if (focused && bmvr::g_DesktopMouseGuard)
    {
        static bool s_down[2];
        static DWORD s_downSince[2];
        static int s_logs;
        const int vk[2] = { VK_LBUTTON, VK_RBUTTON };
        for (int b = 0; b < 2; ++b)
        {
            const bool down = (GetAsyncKeyState(vk[b]) & 0x8000) != 0;
            if (down != s_down[b])
            {
                s_down[b] = down;
                s_downSince[b] = now;
                if (s_logs++ < 60)
                    Game::logMsg("Desktop mouse: %s button %s", b ? "right" : "left", down ? "down" : "up");
            }
            else if (down && now - s_downSince[b] > 500)
            {
                INPUT in{};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = b ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
                const UINT sent = SendInput(1, &in, sizeof(in));
                s_downSince[b] = now;
                static int s_relLogs;
                if (s_relLogs++ < 20)
                    Game::logMsg("Desktop mouse: %s button held %s 0.5 s in VR, sent a release (ok=%u)",
                        b ? "right" : "left", "over", sent);
                if (b == 0)
                    m_Game->ClientCmd_Unrestricted("-attack");
                else
                    m_Game->ClientCmd_Unrestricted("-attack2");
            }
        }
    }
    if (s_releasePending && (int)(now - s_releaseAtMs) >= 0)
    {
        s_releasePending = false;
        // No key number: the engine treats it like the console command and
        // clears both held keys of the button. VR fire bits are set per
        // usercmd from the controllers and are not affected.
        const bool ok = m_Game->ClientCmd_Unrestricted("-attack; -attack2");
        Game::logMsg("Focus change: released +attack/+attack2 ok=%d", ok ? 1 : 0);
    }
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
    if (m_VrPhase == VrPhase::Loading)
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
    // Loading phase with the game's loading dialog up (changelevel, load from
    // the pause menu): the window shows Black Mesa's loading screen; show it
    // as the 2D panel (helper drops the stereo layer) instead of the frozen
    // world. Without the dialog (in-game load) the window is only the world
    // mirror, so the stereo frame stays.
    if (m_VrPhase == VrPhase::Loading)
        return true;
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
        distM = 1.4f;
    if (!(widthM > 0.4f && widthM < 4.f))
        widthM = 1.6f;
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
        m_MenuPointerLastValid = false;
        m_MenuPanelMovedSinceMs = 0;
        m_MenuPanelTurnedSinceMs = 0;
        return;
    }
    const char* relatchWhy = nullptr;
    if (m_MenuPanelPoseValid)
    {
        // A pause overlay latched just before loading began (or the reverse):
        // keep the place, switch between mono panel and stereo world.
        const bool mono = Want2dMenuPanel();
        if (((m_MenuPanelPose.reserved1 & L4D2VR_OPENXR_POSE_FLAG_MONO) != 0) != mono)
        {
            if (mono)
                m_MenuPanelPose.reserved1 |= L4D2VR_OPENXR_POSE_FLAG_MONO;
            else
                m_MenuPanelPose.reserved1 &= ~L4D2VR_OPENXR_POSE_FLAG_MONO;
            Game::logMsg("Menu panel %s (phase %s)", mono ? "-> mono 2D" : "-> world overlay", VrPhaseName(m_VrPhase));
        }
        // Re-anchor an open menu when the head is clearly somewhere else:
        // headset put on after the startup menu latched, stood up / walked
        // (> 0.5 m sideways or 0.3 m up/down for 0.5 s), or turned > 90 deg
        // off the panel for 1 s. A short lean or glance keeps it static.
        if (!bmvr::g_MenuRoomFront || !m_OpenXrLastHmdPose.valid || !m_HmdPoseValid)
            return;
        const DWORD nowMs = GetTickCount();
        const float* hp = m_OpenXrLastHmdPose.position;
        const float* pp = m_MenuPanelPose.position;
        const float dx = hp[0] - pp[0];
        const float dz = hp[2] - pp[2];
        const float dy = hp[1] - pp[1];
        const bool moved = (dx * dx + dz * dz) > 0.25f || fabsf(dy) > 0.3f;
        const float off = fabsf(WrapRad(OpenXrQuatYaw(m_OpenXrLastHmdPose.orientation)
            - OpenXrQuatYaw(m_MenuPanelPose.orientation)));
        const bool turned = off > 1.5707964f;
        if (!moved)
            m_MenuPanelMovedSinceMs = 0;
        else if (m_MenuPanelMovedSinceMs == 0)
            m_MenuPanelMovedSinceMs = nowMs ? nowMs : 1;
        if (!turned)
            m_MenuPanelTurnedSinceMs = 0;
        else if (m_MenuPanelTurnedSinceMs == 0)
            m_MenuPanelTurnedSinceMs = nowMs ? nowMs : 1;
        if (m_MenuPanelMovedSinceMs != 0 && nowMs - m_MenuPanelMovedSinceMs >= 500)
            relatchWhy = "head moved";
        else if (m_MenuPanelTurnedSinceMs != 0 && nowMs - m_MenuPanelTurnedSinceMs >= 1000)
            relatchWhy = "turned > 90 deg";
        if (!relatchWhy)
            return;
        Game::logMsg("Menu panel re-anchor: %s (moved %.2fm sideways, %.2fm up, %.0f deg off)",
            relatchWhy, sqrtf(dx * dx + dz * dz), dy, off * 57.2957795f);
    }
    if (!m_OpenXrLastHmdPose.valid || !m_HmdPoseValid)
        return;
    m_MenuPanelMovedSinceMs = 0;
    m_MenuPanelTurnedSinceMs = 0;
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
    // Room front: face the play area's forward (STAGE -Z) at the current
    // eye position and height, so menus always open in the same direction
    // in the room. Turned > 90 deg away from it -> face the head instead.
    const float headYaw = OpenXrQuatYaw(m_MenuPanelPose.orientation);
    float panelYaw = headYaw;
    m_MenuPanelRoomFront = false;
    L4D2VROpenXrPoseDesc stage{};
    const bool haveStage = bmvr::g_MenuRoomFront && L4D2VR_ReadOpenXrStagePose(stage);
    float roomYaw = 0.f;
    if (haveStage)
    {
        roomYaw = OpenXrQuatYaw(stage.orientation);
        if (fabsf(WrapRad(headYaw - roomYaw)) <= 1.5707964f)
        {
            panelYaw = roomYaw;
            m_MenuPanelRoomFront = true;
        }
    }
    m_MenuPanelPose.orientation[0] = 0.f;
    m_MenuPanelPose.orientation[1] = sinf(panelYaw * 0.5f);
    m_MenuPanelPose.orientation[2] = 0.f;
    m_MenuPanelPose.orientation[3] = cosf(panelYaw * 0.5f);
    // Pointer fallback basis (no helper): game view yaw plus the panel's
    // offset from the head.
    const Vector va = GetViewAngle();
    const float yawOffDeg = WrapRad(panelYaw - headYaw) * 57.2957795f;
    QAngle::AngleVectors(QAngle(0.f, va.y + yawOffDeg, 0.f), &m_MenuPanelFwd, &m_MenuPanelRight, &m_MenuPanelUp);
    m_MenuPanelPoseValid = true;
    float dist = 0.f, width = 0.f, yOff = 0.f;
    GetMenuOverlayMetrics(dist, width, yOff);
    Game::logMsg("Menu panel pose latched (%s, head %.0f deg off room front%s, eye y=%.2fm, dist=%.2fm width=%.2fm pause3d=%d seen=%d paused=%d epoch=%u)",
        m_MenuPanelRoomFront ? "room front" : "head yaw",
        haveStage ? WrapRad(headYaw - roomYaw) * 57.2957795f : 0.f,
        haveStage ? "" : " [no STAGE]",
        m_MenuPanelPose.position[1], dist, width, WantPauseWorldOverlay() ? 1 : 0, m_SeenGameplay ? 1 : 0,
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

    // Right hand first. With both hands resting in front, both rays can hit
    // the panel; the old "keep the last hand, switch on a miss" rule let them
    // take the pointer from each other on every brief miss (user,
    // 2026-11-17). The left hand points only while the right has been off
    // the panel for kLeftAfterRightMs.
    constexpr DWORD kLeftAfterRightMs = 400;
    const DWORD now = GetTickCount();
    const int was = m_MenuPointerHand;
    if (tryHand(L4D2VR_OPENXR_HAND_RIGHT))
    {
        m_MenuRightHitMs = now;
        m_MenuPointerHand = 1;
    }
    else if (m_MenuRightHitMs != 0 && now - m_MenuRightHitMs < kLeftAfterRightMs)
        return false;
    else if (tryHand(L4D2VR_OPENXR_HAND_LEFT))
        m_MenuPointerHand = 0;
    else
        return false;
    if (m_MenuPointerHand != was)
    {
        static int s_log;
        if (s_log++ < 20)
            Game::logMsg("Menu pointer hand -> %s", m_MenuPointerHand ? "right" : "left");
    }
    return true;
}

void VR::ApplyWorldVguiTriggerClick(CUserCmd* cmd)
{
    m_WorldVguiSuppressAttack = false;
    static void* s_lastScreen = nullptr;
    auto releaseMouse = [&]() {
        if (m_WorldVguiTriggerWasDown && m_Game)
            SehVguiMouseButton(m_Game->m_VguiInput, false);
        m_WorldVguiTriggerWasDown = false;
    };
    auto clearLast = [&]() {
        if (s_lastScreen && m_Game && m_Game->m_Offsets)
            SehSetVguiScreenButtons(
                reinterpret_cast<void*>(m_Game->m_Offsets->SetVGuiScreenButtonState.address),
                s_lastScreen, 0);
        s_lastScreen = nullptr;
        releaseMouse();
    };

    if (!m_Game || !cmd)
        return;
    const bool allow = m_GameplayEligible && m_IsVREnabled && !IsMenuUp()
        && !MenuGameplayBlocked() && !m_WeaponMenuOpen;
    if (!allow)
    {
        clearLast();
        return;
    }

    Offsets* o = m_Game->m_Offsets;
    if (!o || !o->FindNearbyVguiScreen.valid)
        return;

    void* player = m_Game->GetLocalPlayerEntity();
    const int team = SehPlayerTeamNumber(player);
    const bool trigger = PressedDigitalAction(m_ActionPrimaryAttack);

    Vector hmdOrigin{};
    QAngle hmdAng{};
    Vector hmdFwd{};
    if (m_HmdPoseValid)
    {
        const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        hmdOrigin = GetViewOrigin(body.LengthSqr() > 1.f ? body : m_SetupOrigin);
        const Vector va = GetViewAngle();
        hmdAng.Init(va.x, va.y, 0.f);
        GetViewBasis(&hmdFwd, nullptr, nullptr);
        if (VectorNormalize(hmdFwd) < 0.01f)
            QAngle::AngleVectors(hmdAng, &hmdFwd, nullptr, nullptr);
    }

    void* findFn = reinterpret_cast<void*>(o->FindNearbyVguiScreen.address);
    void* setFn = o->SetVGuiScreenButtonState.valid
        ? reinterpret_cast<void*>(o->SetVGuiScreenButtonState.address) : nullptr;
    void* isectFn = o->IntersectVguiScreenRay.valid
        ? reinterpret_cast<void*>(o->IntersectVguiScreenRay.address) : nullptr;

    void* screen = nullptr;
    const char* src = "none";
    if (hmdOrigin.LengthSqr() > 1.f)
    {
        screen = SehFindNearbyVguiScreen(findFn, &hmdOrigin, &hmdAng, team);
        if (screen)
            src = "hmd";
    }
    Vector eye{};
    QAngle eyeAng{};
    if (!screen && player && SehPlayerEyePose(player, eye, eyeAng))
    {
        screen = SehFindNearbyVguiScreen(findFn, &eye, &eyeAng, team);
        if (screen)
            src = "eye";
    }

    if (s_lastScreen && s_lastScreen != screen)
    {
        SehSetVguiScreenButtons(setFn, s_lastScreen, 0);
        releaseMouse();
    }
    s_lastScreen = screen;

    if (!screen)
        return;

    const int buttons = trigger ? IN_ATTACK : 0;
    SehSetVguiScreenButtons(setFn, screen, buttons);

    float u = 0.f, v = 0.f;
    bool hit = false;
    if (isectFn && hmdOrigin.LengthSqr() > 1.f)
    {
        alignas(16) Ray_t ray;
        const Vector end = hmdOrigin + hmdFwd * 1000.f;
        ray.Init(hmdOrigin, end);
        hit = SehIntersectVguiRay(isectFn, screen, &ray, &u, &v);
    }
    int pw = SehReadIntOff(screen, Offsets::kC_VGuiScreen_PixelWidth);
    int ph = SehReadIntOff(screen, Offsets::kC_VGuiScreen_PixelHeight);
    if (pw < 32 || pw > 8192 || ph < 32 || ph > 8192)
    {
        pw = SehReadIntOff(screen, 0x694);
        ph = SehReadIntOff(screen, 0x698);
    }
    if (pw < 32 || pw > 8192)
        pw = 640;
    if (ph < 32 || ph > 8192)
        ph = 480;
    const int px = hit ? static_cast<int>(u * static_cast<float>(pw) + 0.5f) : pw / 2;
    const int py = hit ? static_cast<int>(v * static_cast<float>(ph) + 0.5f) : ph / 2;

    void* panel = SehReadPtrOff(screen, Offsets::kC_VGuiScreen_PanelPtr);
    if (!panel)
        panel = SehReadPtrOff(screen, 0x720);
    const uintptr_t modeSite = (m_Game->m_Offsets && m_Game->m_Offsets->g_pClientMode.valid)
        ? static_cast<uintptr_t>(m_Game->m_Offsets->g_pClientMode.address) : 0;
    SehActivateInGameVgui(m_Game->m_BaseClient, modeSite, panel);
    SehVguiCursorMoved(m_Game->m_VguiInput, px, py);

    if (trigger && !m_WorldVguiTriggerWasDown)
        SehVguiMouseButton(m_Game->m_VguiInput, true);
    else if (!trigger && m_WorldVguiTriggerWasDown)
        SehVguiMouseButton(m_Game->m_VguiInput, false);
    m_WorldVguiTriggerWasDown = trigger;

    if (trigger)
        m_WorldVguiSuppressAttack = true;

    static int s_log;
    if (s_log < 12)
    {
        Game::logMsg("World VGUI screen=%p src=%s hit=%d uv=%.3f,%.3f px=%d,%d %dx%d panel=%p trig=%d",
            screen, src, hit ? 1 : 0, u, v, px, py, pw, ph, panel, trigger ? 1 : 0);
        ++s_log;
    }

    if (m_WorldVguiSuppressAttack)
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
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


    // Screenspace FOV fallback maps idle / look-forward aim to the panel
    // center. Skip it once a laser hit exists, and never use it on the
    // OpenXR overlay ray (that miss means "not pointing at the menu").
    if (!haveHit && !m_MenuCursorValid && !m_OpenXrHelperBridgeActive
        && m_ControllerPoseValid && m_Fov > 10.f && m_Aspect > 0.2f)
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

    float pointerPos[3]{};
    float pointerDir[3]{};
    bool havePointerPose = false;
    if (m_OpenXrHelperBridgeActive)
    {
        const uint32_t hand = (m_MenuPointerHand == 0)
            ? L4D2VR_OPENXR_HAND_LEFT : L4D2VR_OPENXR_HAND_RIGHT;
        const L4D2VROpenXrControllerPoseDesc& aim =
            m_OpenXrLastInputState.controllerAimPoses[hand];
        const L4D2VROpenXrControllerPoseDesc& grip =
            m_OpenXrLastInputState.controllerPoses[hand];
        const L4D2VROpenXrControllerPoseDesc* pose = nullptr;
        if (aim.valid && aim.active)
            pose = &aim;
        else if (grip.valid && grip.active)
            pose = &grip;
        if (pose)
        {
            pointerPos[0] = pose->position[0];
            pointerPos[1] = pose->position[1];
            pointerPos[2] = pose->position[2];
            RotateOpenXrVec(pose->orientation, 0.f, 0.f, -1.f, pointerDir);
            havePointerPose = true;
        }
    }
    else if (m_RightControllerTrackingValid)
    {
        pointerPos[0] = m_RightControllerTracking.m[0][3];
        pointerPos[1] = m_RightControllerTracking.m[1][3];
        pointerPos[2] = m_RightControllerTracking.m[2][3];
        pointerDir[0] = -m_RightControllerTracking.m[0][2];
        pointerDir[1] = -m_RightControllerTracking.m[1][2];
        pointerDir[2] = -m_RightControllerTracking.m[2][2];
        havePointerPose = true;
    }

    // On the panel this frame, before the idle dead zone below drops haveHit
    // for a still pointer. GetCursorPos (Hooks::dGetCursorPos) must keep
    // reporting the pointer while it rests on a button, or a careful aim then
    // a still trigger pull clicks at the real desktop mouse after 250 ms.
    const bool onPanel = haveHit;
    bool pointerPastDeadZone = false;
    if (haveHit && havePointerPose && m_MenuPointerLastValid)
    {
        const float dx = pointerPos[0] - m_MenuPointerLastPos[0];
        const float dy = pointerPos[1] - m_MenuPointerLastPos[1];
        const float dz = pointerPos[2] - m_MenuPointerLastPos[2];
        const float dPos = sqrtf(dx * dx + dy * dy + dz * dz);
        float d0 = pointerDir[0], d1 = pointerDir[1], d2 = pointerDir[2];
        float n = sqrtf(d0 * d0 + d1 * d1 + d2 * d2);
        if (n > 0.01f)
        {
            d0 /= n;
            d1 /= n;
            d2 /= n;
        }
        float l0 = m_MenuPointerLastDir[0], l1 = m_MenuPointerLastDir[1], l2 = m_MenuPointerLastDir[2];
        n = sqrtf(l0 * l0 + l1 * l1 + l2 * l2);
        if (n > 0.01f)
        {
            l0 /= n;
            l1 /= n;
            l2 /= n;
        }
        float c = d0 * l0 + d1 * l1 + d2 * l2;
        if (c > 1.f)
            c = 1.f;
        if (c < -1.f)
            c = -1.f;
        const float dAng = acosf(c) * (180.f / 3.14159265f);
        // Idle tracking noise, not a real aim change: leave a resting cursor
        // put. Only at rest: the menu runs at ~280 fps, so a per-frame test
        // also swallowed slow sweeps until 0.45 deg had piled up, and the
        // cursor crossed the buttons in ~13 px steps (the laggy hover).
        // The reference pose only moves when the threshold is crossed, so a
        // slow sweep crosses it every 0.45 deg and keeps the 0.2 s window
        // open; a per-frame reference never crossed it at 280 fps.
        constexpr double kMoveHoldMs = 200.0;
        const double nowMoveMs = QpcNowMs();
        if (dPos >= 0.004f || dAng >= 0.45f)
        {
            m_MenuPointerMoveMs = nowMoveMs;
            pointerPastDeadZone = true;
        }
        else if (nowMoveMs - m_MenuPointerMoveMs > kMoveHoldMs)
            haveHit = false;
    }
    if (haveHit && havePointerPose && (pointerPastDeadZone || !m_MenuPointerLastValid))
    {
        m_MenuPointerLastPos[0] = pointerPos[0];
        m_MenuPointerLastPos[1] = pointerPos[1];
        m_MenuPointerLastPos[2] = pointerPos[2];
        m_MenuPointerLastDir[0] = pointerDir[0];
        m_MenuPointerLastDir[1] = pointerDir[1];
        m_MenuPointerLastDir[2] = pointerDir[2];
        m_MenuPointerLastValid = true;
    }

    const double nowMs = QpcNowMs();
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
                tau = 0.05f;
            // Fast flicks cut lag; small tremor stays filtered (the idle
            // dead zone above already holds a resting cursor still).
            tau /= (1.f + speed / 2800.f);
            if (tau < 0.015f)
                tau = 0.015f;
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
    if (onPanel)
    {
        m_MenuCursorHwnd = hwnd;
        // Screen position for the GetCursorPos hook, computed here once per
        // Present: VGUI can call GetCursorPos many times a frame, and the
        // hook must not make a window-manager call (ClientToScreen) each time.
        POINT sp{ wx, wy };
        if (hwnd && ClientToScreen(hwnd, &sp))
        {
            m_MenuCursorScreen.store(PackScreenPoint(sp), std::memory_order_relaxed);
            m_MenuCursorLiveMs = GetTickCount();
        }
    }
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

    // Where the pointer was ~100 ms ago. Pulling the trigger tips the
    // controller; since the menu pointer follows the hand closely (smoothing
    // 0.05 s, 2026-10-14) the click landed on the neighbouring entry. Click
    // where the user aimed before the pull.
    struct CursorSample { DWORD ms; int x; int y; };
    static CursorSample s_hist[128];
    static int s_histN;
    const DWORD histNow = GetTickCount();
    if (haveHit)
    {
        s_hist[s_histN % 128] = { histNow, wx, wy };
        ++s_histN;
    }
    // Trigger with hysteresis: press past 0.9, re-arm only below 0.5. A
    // finger resting on the PS VR2 Sense trigger reads 0.6-0.7 (log
    // 2026-10-15) and its small movements clicked menu items on their own.
    // Same latch as outside menus (ProcessInput), so a menu that opens with
    // the trigger resting between the thresholds starts from its real state.
    bool click = m_MenuTriggerLatch;
    float triggerValue = -1.f;
    vr::InputAnalogActionData_t trig{};
    static float s_trigMinSinceClick = 1.f;
    if (GetAnalogActionData(m_ActionPrimaryAttack, trig))
    {
        triggerValue = trig.x;
        s_trigMinSinceClick = (std::min)(s_trigMinSinceClick, trig.x);
        if (trig.x >= kMenuTriggerPress)
            click = true;
        else if (trig.x <= kMenuTriggerRearm)
            click = false;
    }
    else
        click = PressedDigitalAction(m_ActionPrimaryAttack);
    m_MenuTriggerLatch = click;
    // Trigger only. Cross (a/click) read as pressed while the user only
    // pointed (PS VR2 Sense via Touch emulation, 2026-10-15) and clicked
    // whatever was under the pointer; Cross still confirms (Enter) when the
    // pointer is not on the menu (ApplyMenuNavigation). Its edges are logged.
    const bool selectHeld = PressedDigitalAction(m_ActionMenuSelect);
    static bool s_selectWas;
    static int s_selectLog;
    if (selectHeld != s_selectWas && s_selectLog < 40)
    {
        Game::logMsg("Menu Cross %s t=%lu", selectHeld ? "down" : "up",
            static_cast<unsigned long>(GetTickCount()));
        ++s_selectLog;
    }
    s_selectWas = selectHeld;
    if (click && !m_MenuTriggerWasDown)
    {
        for (int i = 1; i <= 128 && i <= s_histN; ++i)
        {
            const CursorSample& smp = s_hist[(s_histN - i) % 128];
            if (histNow - smp.ms > 250)
                break;
            if (histNow - smp.ms >= 100)
            {
                // Only undo the small jerk of the pull itself. A larger
                // move means the user was still travelling to the item;
                // rewinding then clicked the one just passed (main menu,
                // 2026-10-15: things opened that were not aimed at).
                const int dx = smp.x - wx, dy = smp.y - wy;
                if (dx * dx + dy * dy <= 24 * 24)
                {
                    wx = smp.x;
                    wy = smp.y;
                }
                break;
            }
        }
        // VGUI reads the click position back through GetCursorPos (the hook
        // answers with the live pointer), and SetCursorPos does not move the
        // cursor while the window is unfocused under Wine. Report the
        // rewound click point for a moment so press and release both land
        // on the item the user aimed at.
        {
            POINT hp{ wx, wy };
            if (hwnd && ClientToScreen(hwnd, &hp))
            {
                m_MenuClickHoldScreen.store(PackScreenPoint(hp), std::memory_order_relaxed);
                m_MenuClickHoldUntilMs.store(GetTickCount() + kMenuClickHoldMs, std::memory_order_release);
            }
        }
        if (hwnd)
        {
            POINT pt{ wx, wy };
            ClientToScreen(hwnd, &pt);
            SetCursorPos(pt.x, pt.y);
            PostMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(wx, wy));
        }
        // Single clicks only. Two pulls within the double-click time used to
        // become WM_LBUTTONDBLCLK, which opens/loads the item in BM's lists
        // and menus when the user merely clicked again.
        if (hwnd)
        {
            PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(wx, wy));
            PostMessageA(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(wx, wy));
        }
        static int s_clickLog;
        if (s_clickLog < 40)
        {
            // cursorHook: GetCursorPos calls since the last click answered
            // with the VR pointer / passed to the real cursor. Pause (in-game
            // GameUI) vs main menu differ here if VGUI reads the cursor
            // another way in game.
            POINT real{};
            const BOOL haveReal = Hooks::hkGetCursorPos.fOriginal
                ? Hooks::hkGetCursorPos.fOriginal(&real) : ::GetCursorPos(&real);
            POINT ptr{};
            const bool live = TryGetMenuCursorScreen(ptr);
            Game::logMsg("VR menu click at %d,%d menuMap=%d pause=%d trigger=%.2f minSince=%.2f cross=%d t=%lu overlay=%d cursorHook=%ld/%ld live=%d ptrScreen=%ld,%ld real=%ld,%ld focus=%d fg=%d hud=%dx%d",
                wx, wy, menuMap ? 1 : 0, pauseUi ? 1 : 0, triggerValue, s_trigMinSinceClick,
                selectHeld ? 1 : 0, static_cast<unsigned long>(histNow), WantPauseWorldOverlay() ? 1 : 0,
                static_cast<long>(m_CursorHookHits.exchange(0)), static_cast<long>(m_CursorHookPass.exchange(0)),
                live ? 1 : 0, ptr.x, ptr.y, haveReal ? real.x : -1L, haveReal ? real.y : -1L,
                (hwnd && GetFocus() == hwnd) ? 1 : 0, (hwnd && GetForegroundWindow() == hwnd) ? 1 : 0,
                hudW, hudH);
            ++s_clickLog;
        }
        s_trigMinSinceClick = 1.f;
    }
    m_MenuTriggerWasDown = click;
}

bool VR::MenuTriggerClickHeld(bool& latch) const
{
    vr::InputAnalogActionData_t trig{};
    if (GetAnalogActionData(m_ActionPrimaryAttack, trig))
    {
        if (trig.x >= kMenuTriggerPress)
            latch = true;
        else if (trig.x <= kMenuTriggerRearm)
            latch = false;
        return latch;
    }
    latch = PressedDigitalAction(m_ActionPrimaryAttack);
    return latch;
}

bool VR::MenuPointerLive() const
{
    return m_MenuCursorValid && m_MenuCursorLiveMs != 0 && GetTickCount() - m_MenuCursorLiveMs <= 250u;
}

bool VR::TryGetMenuCursorScreen(POINT& out) const
{
    // Only while a VR menu panel is up (flag from ProcessInput: this runs on
    // whatever thread calls GetCursorPos, so no engine calls here). Right
    // after the menu closes the game's own readers (mouse look) get the real
    // cursor again.
    // No system calls beyond GetTickCount: positions are converted to screen
    // coordinates in ApplyMenuCursor.
    if (!bmvr::g_MenuCursorHook || !m_MenuCursorValid || !m_MenuCursorHwnd || m_MenuCursorLiveMs == 0
        || !m_MenuPanelUpForCursor.load(std::memory_order_relaxed))
        return false;
    const DWORD now = GetTickCount();
    if (now - m_MenuCursorLiveMs > 250u)
        return false;
    // Just after a click: the (possibly rewound) click point, see
    // ApplyMenuCursor.
    const DWORD holdUntil = m_MenuClickHoldUntilMs.load(std::memory_order_acquire);
    const bool hold = holdUntil != 0 && static_cast<int32_t>(holdUntil - now) > 0;
    out = UnpackScreenPoint((hold ? m_MenuClickHoldScreen : m_MenuCursorScreen).load(std::memory_order_relaxed));
    return true;
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
    // HEV orange in Black Mesa; Calhoun blue in Blue Shift / Azure Sheep.
    const D3DCOLOR fillCol = bmvr::UseBlueHud()
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
    BMVR_PROF("vr UpdateAimCrosshair");
    // The two valid flags are written once, when this returns. Clearing them
    // here and setting them again after the trace left them false for the
    // whole update; the render thread's overlay gate read them in that window
    // and dropped beam and dot for a frame (heavy scenes, 2026-11-14).
    struct AimCommit
    {
        VR* vr;
        bool cross = false, beam = false;
        int stage = 0;   // how far the update got (laser dropout log)
        ~AimCommit() { vr->m_AimCrosshairValid = cross; vr->m_AimBeamValid = beam; vr->m_AimUpdateStage = stage; }
    } commit{ this };
    {
        // Test only (BMVR_TEST_AIM_BLINK=1): the old behaviour, to count the race headless.
        static const bool s_blink = [] { char v[4] = {}; return GetEnvironmentVariableA("BMVR_TEST_AIM_BLINK", v, sizeof(v)) && v[0] == '1'; }();
        if (s_blink)
        {
            m_AimCrosshairValid = false;
            m_AimBeamValid = false;
        }
    }
    // VrShootFromMuzzle needs the muzzle offset (UpdateMuzzleLocal) even with
    // the marker and beam off; without it shots used the raw attachment,
    // which drifts toward the view edges.
    const bool wantAimAid = bmvr::g_VrCrosshair || bmvr::g_VrLaserBeam;
    // No weapon at all (new game, stripped on death): m_VmClass still names
    // the last gun, so without this the laser stayed on bare hands.
    if (!(wantAimAid || bmvr::g_VrShootFromMuzzle) || !m_GameplayEligible || m_EmptyHands || m_WeaponMenuOpen
        || m_SettingsOpen || !m_HasHeldWeapon || !(m_Game && m_Game->GetActiveWeaponEntity()))
        return;
    commit.stage = 1;
    // Guns only: no laser on the crowbar, thrown weapons, snarks or bare hands.
    // The RPG keeps its own guidance dot and gets no beam; the hivehand gets
    // neither (below).
    std::string weaponKey;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        weaponKey = m_VmClass.numpadKey;
    }
    static const char* kAimAidWeapons[] = { "glock", "357", "mp5", "shotgun", "crossbow", "rpg", "gauss", "gluon", "hgun" };
    bool gun = false;
    for (const char* k : kAimAidWeapons)
        gun = gun || weaponKey == k;
    if (!gun)
        return;
    commit.stage = 2;
    const bool wantBeam = bmvr::g_VrLaserBeam && weaponKey != "rpg";
    // The scope draws its own centred crosshair, and while zoomed the shot no
    // longer follows the controller ray this would trace.
    if (m_ScopeZoomActive)
        return;
    commit.stage = 3;
    if (!m_Game || !m_Game->m_EngineTrace || !m_Game->m_EngineClient)
        return;
    if (PauseUiActive())
        return;

    commit.stage = 5;
    UpdateMuzzleLocal(weaponKey);
    // The hivehand's hornets home in on their own (primary fire): no laser
    // and no dot on the organic Xen weapon. The muzzle above is still kept
    // up to date, the hornets spawn there.
    if (!wantAimAid || weaponKey == "hgun")
        return;
    commit.stage = 6;
    // Same origin and direction the bullet uses, so the reticle cannot disagree
    // with where the shot goes.
    Vector origin{};
    if (!TryGetVrShootOrigin(origin))
        return;
    commit.stage = 7;
    Vector dir{};
    QAngle::AngleVectors(GetAimAngles(), &dir, nullptr, nullptr);
    if (VectorNormalize(dir) <= 0.01f)
        return;

    commit.stage = 8;
    C_BaseEntity* player = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
    CTraceFilterSkipSelf filter(player, 0);
    constexpr float kMaxRangeHu = 8192.f;
    Ray_t ray;
    ray.Init(origin, origin + dir * kMaxRangeHu);
    CGameTrace tr{};
    m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
    const bool hit = tr.fraction < 1.f;

    // Everything is kept relative to the aim frame (controller position +
    // aim angles) and rebuilt from the current pose when drawn, so beam and
    // dot stay locked to the gun instead of trailing it by a tick.
    Vector pivot{}, f{}, r{}, u{};
    if (!AimFrameNow(pivot, f, r, u))
        return;
    commit.stage = 9;
    m_AimShootAlong = (origin - pivot).Dot(f);
    m_AimHitDist = hit ? (tr.endpos - origin).Length() - 1.5f : 0.f;

    if (wantBeam || bmvr::g_VrShootFromMuzzle)
    {
        // Beam from the muzzle (UpdateMuzzleLocal, gun frame) to the dot.
        if (m_AimBeamLocalValid)
        {
            m_AimBeamHit = hit;
            commit.beam = wantBeam;
        }
    }
    commit.stage = 10;
    if (!bmvr::g_VrCrosshair || !hit)
        return;

    // Pull the reticle a hair off the surface it landed on. The overlay pass
    // draws without depth testing, so this is only to keep it from sinking into
    // geometry the glove pass does depth-test against.
    m_AimCrosshairWorld = tr.endpos - dir * 1.5f;
    commit.cross = true;
}

bool VR::AimFrameNow(Vector& pivot, Vector& f, Vector& r, Vector& u) const
{
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    pivot = GetRightControllerAbsPos(body);
    if (pivot.LengthSqr() <= 1.f)
        return false;
    QAngle::AngleVectors(GetAimAngles(), &f, &r, &u);
    return VectorNormalize(f) > 0.01f;
}

bool VR::GunFrameNow(Vector& origin, Vector& f, Vector& r, Vector& u) const
{
    // Where the viewmodel is placed this frame, roll included.
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        return false;
    origin = GetRecommendedViewmodelAbsPos(body);
    ApplyViewmodelVisualScale(origin);
    QAngle::AngleVectors(GetRecommendedViewmodelAbsAngle(), &f, &r, &u);
    return VectorNormalize(f) > 0.01f;
}

bool VR::AimOverlayPointsNow(Vector& beamStart, Vector& beamEnd, Vector& dot) const
{
    Vector pivot{}, f{}, r{}, u{};
    if (!AimFrameNow(pivot, f, r, u))
        return false;
    Vector vo{}, vf{}, vr{}, vu{};
    if (!GunFrameNow(vo, vf, vr, vu))
        return false;
    beamStart = vo + vf * m_AimBeamLocal.x + vr * m_AimBeamLocal.y + vu * m_AimBeamLocal.z;
    if (bmvr::g_VrShootFromMuzzle && m_AimBeamLocalValid)
    {
        // Shot = muzzle along the aim: dot and beam on that one line.
        dot = beamStart + f * m_AimHitDist;
        beamEnd = m_AimBeamHit ? dot : beamStart + f * 1024.f;
    }
    else
    {
        dot = pivot + f * (m_AimShootAlong + m_AimHitDist);
        beamEnd = m_AimBeamHit ? dot : pivot + f * (m_AimShootAlong + 1024.f);
    }
    Vector along = beamEnd - beamStart;
    const float len = VectorNormalize(along);
    beamStart = beamStart + along * (std::min)(len, bmvr::g_VrLaserStartForward);
    return true;
}

bool VR::TryGetAimCrosshairWorld(Vector& out) const
{
    if (!m_AimCrosshairValid)
        return false;
    out = m_AimCrosshairWorld;
    return true;
}

namespace
{
    // AimDebug: last shoot origin per side, so the angle hook (called right
    // after Weapon_ShootPosition by FireBullets) can trace the actual shot.
    struct AimDebugOrigin
    {
        Vector origin{};
        Vector before{};
        void* player = nullptr;
        bool rewrote = false;
        DWORD ms = 0;
        bool valid = false;
    };
    AimDebugOrigin g_AimDebugOrigin[2];
    int g_AimDebugLines = 0;
    DWORD g_AimDebugLastMs = 0;
    constexpr int kAimDebugMaxLines = 400;
}

void VR::AimDebugShootOrigin(bool server, void* player, bool rewrote, const Vector& before, const Vector& after)
{
    if (!bmvr::g_AimDebug)
        return;
    AimDebugOrigin& o = g_AimDebugOrigin[server ? 1 : 0];
    o.origin = after;
    o.before = before;
    o.player = player;
    o.rewrote = rewrote;
    o.ms = GetTickCount();
    o.valid = true;
}

void VR::AimDebugShootAngles(bool server, void* player, bool rewrote, const QAngle& before, const QAngle& after)
{
    if (!bmvr::g_AimDebug || g_AimDebugLines >= kAimDebugMaxLines)
        return;
    // Only while the trigger is held: these hooks also run for NPCs and
    // non-firing queries.
    if ((m_HeldButtons.load(std::memory_order_acquire) & IN_ATTACK) == 0)
        return;
    const DWORD now = GetTickCount();
    if (now - g_AimDebugLastMs < 40)
        return;
    if (!m_Game || !m_Game->m_EngineTrace || !m_Game->m_EngineClient)
        return;
    g_AimDebugLastMs = now;
    ++g_AimDebugLines;

    C_BaseEntity* local = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
    auto trace = [&](const Vector& from, const QAngle& ang, Vector& hit) -> bool {
        Vector dir{};
        QAngle::AngleVectors(ang, &dir, nullptr, nullptr);
        if (VectorNormalize(dir) <= 0.01f)
            return false;
        CTraceFilterSkipSelf filter(local, 0);
        Ray_t ray;
        ray.Init(from, from + dir * 8192.f);
        CGameTrace tr{};
        m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
        hit = tr.endpos;
        return tr.fraction < 1.f;
    };

    const AimDebugOrigin& o = g_AimDebugOrigin[server ? 1 : 0];
    const bool haveOrigin = o.valid && o.player == player && now - o.ms < 250;
    Vector shotHit{};
    const bool shotHitOk = haveOrigin && trace(o.origin, after, shotHit);

    Vector retOrigin{};
    const bool haveRet = TryGetVrShootOrigin(retOrigin);
    const QAngle retAng = GetAimAngles();
    Vector retHit{};
    const bool retHitOk = haveRet && trace(retOrigin, retAng, retHit);

    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    const Vector eye = GetViewOrigin(body);
    const Vector hand = GetRightControllerAbsPos(body);
    Vector fwd, right, up;
    QAngle::AngleVectors(retAng, &fwd, &right, &up);
    const Vector dHit = shotHit - retHit;
    const Vector dOrig = o.origin - retOrigin;

    Game::logMsg("AimDebug %s player=%p origin rewrote=%d (%.1f,%.1f,%.1f) was (%.1f,%.1f,%.1f) age=%lums | "
        "angles rewrote=%d (%.2f,%.2f) was (%.2f,%.2f) | reticle o=(%.1f,%.1f,%.1f) a=(%.2f,%.2f) crosshair=%d | "
        "shotHit=%d (%.1f,%.1f,%.1f) retHit=%d (%.1f,%.1f,%.1f) dHit right=%.1f up=%.1f fwd=%.1f | "
        "dOrigin right=%.1f up=%.1f fwd=%.1f | eye=(%.1f,%.1f,%.1f) hand=(%.1f,%.1f,%.1f) body=(%.1f,%.1f,%.1f)",
        server ? "server" : "client", player,
        o.rewrote ? 1 : 0, o.origin.x, o.origin.y, o.origin.z, o.before.x, o.before.y, o.before.z,
        haveOrigin ? static_cast<unsigned long>(now - o.ms) : 9999ul,
        rewrote ? 1 : 0, after.x, after.y, before.x, before.y,
        retOrigin.x, retOrigin.y, retOrigin.z, retAng.x, retAng.y, m_AimCrosshairValid ? 1 : 0,
        shotHitOk ? 1 : 0, shotHit.x, shotHit.y, shotHit.z, retHitOk ? 1 : 0, retHit.x, retHit.y, retHit.z,
        dHit.Dot(right), dHit.Dot(up), dHit.Dot(fwd),
        dOrig.Dot(right), dOrig.Dot(up), dOrig.Dot(fwd),
        eye.x, eye.y, eye.z, hand.x, hand.y, hand.z, body.x, body.y, body.z);
}

void VR::UpdateRpgLaserPoint()
{
    m_RpgLockRing = m_RpgLockRing && ViewmodelIsRpg();
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
        BMVR_TRY
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
        if ((laserOn != 0) != m_RpgLockRing)
            Game::logMsg("RPG laser %s (game m_bLaserOn)", laserOn ? "on" : "off");
        m_RpgLockRing = laserOn != 0;
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

// Laser dot: a red disc with a dark rim. The caller sizes it like a real
// laser spot (fixed world size, so it shrinks with distance) with a floor
// that keeps it visible across a hangar.
void VR::DrawAimDot(IDirect3DDevice9* device, float sx, float sy, float radius, float depth) const
{
    if (!device)
        return;
    struct Vert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };
    constexpr int kSegs = 16;
    auto disc = [&](float r, D3DCOLOR centre, D3DCOLOR rim) {
        Vert v[kSegs + 2];
        v[0] = { sx, sy, depth, 1.f, centre };
        for (int i = 0; i <= kSegs; ++i)
        {
            const float a = static_cast<float>(i) * (6.2831853f / kSegs);
            v[i + 1] = { sx + cosf(a) * r, sy + sinf(a) * r, depth, 1.f, rim };
        }
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kSegs, v, sizeof(Vert));
    };
    disc(radius + (std::max)(0.75f, radius * 0.3f), D3DCOLOR_ARGB(120, 0, 0, 0), D3DCOLOR_ARGB(120, 0, 0, 0));
    disc(radius, D3DCOLOR_ARGB(255, 255, 90, 70), D3DCOLOR_ARGB(235, 230, 20, 10));
}

// Four short red lines rotating around the laser point, about the size of
// the flat game's: screen-space, so readable on a helicopter far away.
void VR::DrawRpgLockRing(IDirect3DDevice9* device, float sx, float sy, UINT h) const
{
    if (!device)
        return;
    struct Vert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };
    // Own size (Debug tab, "RPG laser ring size"), not the aim marker's: the
    // marker size floored at 1 here made the ring flat-HUD sized, which reads
    // large in the headset, most of all on far targets (user 2026-11-23).
    float scale = bmvr::g_VrRpgRingScale;
    if (!(scale > 0.05f) || scale > 8.f)
        scale = 0.25f;
    const float px = ((h > 8) ? static_cast<float>(h) : 1440.f) / 1440.f * scale;
    const float inner = 10.f * px;
    const float outer = 18.f * px;
    const float halfW = (std::max)(1.f, 1.5f * px);
    const float spin = static_cast<float>(GetTickCount() % 2000u) * (6.2831853f / 2000.f);
    for (int pass = 0; pass < 2; ++pass)
    {
        const float grow = (pass == 0) ? 1.f : 0.f;
        const D3DCOLOR c = (pass == 0) ? D3DCOLOR_ARGB(150, 0, 0, 0) : D3DCOLOR_ARGB(240, 255, 30, 20);
        for (int i = 0; i < 4; ++i)
        {
            const float a = spin + static_cast<float>(i) * 1.5707963f;
            const float dx = cosf(a), dy = sinf(a);
            const float nx = -dy * (halfW + grow), ny = dx * (halfW + grow);
            const float r0 = inner - grow, r1 = outer + grow;
            const Vert v[4] = {
                { sx + dx * r0 + nx, sy + dy * r0 + ny, 0.f, 1.f, c },
                { sx + dx * r0 - nx, sy + dy * r0 - ny, 0.f, 1.f, c },
                { sx + dx * r1 + nx, sy + dy * r1 + ny, 0.f, 1.f, c },
                { sx + dx * r1 - nx, sy + dy * r1 - ny, 0.f, 1.f, c },
            };
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vert));
        }
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
    BMVR_PROF("vr UpdateControllerTracking");
    // UpdateTracking runs on the game thread (pose wait) and, in
    // MulticoreMode, on the render thread too (VR::Update at Present), while
    // the other thread places the gun, laser and gloves. Clearing the valid
    // flags up front and setting them again ~300 lines later let that thread
    // see "no controller pose" for a moment: ControllerTrackingToWorld then
    // returned the eye position, so the laser started at the forehead (or a
    // glove vanished) for a frame, mostly in heavy scenes (2026-11-13). Keep
    // the last published pose until this update either publishes a new one
    // (under m_ControllerMutex, below) or fails (flags cleared on the way out).
    struct ClearIfNotPublished
    {
        VR* vr;
        bool published = false;
        ~ClearIfNotPublished()
        {
            if (published || !bmvr::g_PoseFlagsNoBlink)
                return;
            std::lock_guard<std::recursive_mutex> lock(vr->m_ControllerMutex);
            vr->m_ControllerPoseValid = false;
            vr->m_LeftControllerTrackingValid = false;
            vr->m_PhysicalRightTrackingValid = false;
            vr->m_RightControllerTrackingValid = false;
        }
    } clearGuard{ this };
    if (!bmvr::g_PoseFlagsNoBlink)
    {
        m_ControllerPoseValid = false;
        m_LeftControllerTrackingValid = false;
        m_PhysicalRightTrackingValid = false;
        m_RightControllerTrackingValid = false;
    }
    if (!bmvr::TryCtrlPose())
        return;
    if (!m_OpenXrHelperBridgeActive)
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
    {
        // Throw samples: the weapon hand's grip (where the grenade sits),
        // stamped with the runtime's pose time.
        // Satchel (VrSatchelHands): thrown from the left hand.
        const bool aimLeftHand = AimControllerRole() == vr::TrackedControllerRole_LeftHand
            || m_ThrowFromLeft.load(std::memory_order_relaxed);
        const Sample& throwHand = aimLeftHand ? physLeft : physRight;
        if (throwHand.valid)
        {
            int64_t poseNs = 0;
            if (m_OpenXrHelperBridgeActive)
                poseNs = m_OpenXrLastInputState.controllerPoses[
                    aimLeftHand ? L4D2VR_OPENXR_HAND_LEFT : L4D2VR_OPENXR_HAND_RIGHT].displayTime;
            RecordThrowPose(throwHand.pos, poseNs);
        }
    }
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
    clearGuard.published = true;
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
            ? m_ActionFlashlight : m_ActionInventoryGripLeft;
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
    return applyRightWeaponPose();
}

namespace
{
    // Set by McCompositeEyesToRing (render thread) while it draws both eyes.
    thread_local const VR::HandDrawSnap* t_HandSnap = nullptr;
}

void VR::CaptureHandDrawSnap(HandDrawSnap& s)
{
    s = HandDrawSnap{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (m_StereoFramePoseActive)
        {
            s.leftOk = m_StereoFrameLeftCtrlValid;
            s.rightOk = m_StereoFrameRightCtrlValid;
            s.leftAng = m_StereoFrameLeftCtrlAng;
            s.rightAng = m_StereoFrameRightCtrlAng;
            s.leftPos = m_StereoFrameLeftCtrlPos;
            s.rightPos = m_StereoFrameRightCtrlPos;
        }
        else
        {
            s.leftOk = m_LeftControllerTrackingValid;
            s.rightOk = m_PhysicalRightTrackingValid;
            s.leftAng = m_LeftControllerAngAbs;
            s.rightAng = m_PhysicalRightAngAbs;
            s.leftPos = m_LeftControllerPosAbs;
            s.rightPos = m_PhysicalRightPosAbs;
        }
        s.body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        Vector snapPos{};
        QAngle snapAng{};
        if (TryGetTwoHandOffhandDrawPose(true, snapPos, snapAng))
        {
            s.leftPos = snapPos;
            s.leftAng = snapAng;
        }
        if (TryGetTwoHandOffhandDrawPose(false, snapPos, snapAng))
        {
            s.rightPos = snapPos;
            s.rightAng = snapAng;
        }
        TryGetLadderPinDrawPos(true, s.body, s.leftPos);
        TryGetLadderPinDrawPos(false, s.body, s.rightPos);
        s.gunHudTwoHand = TwoHandGunHudAngles(
            bmvr::g_LeftHanded ? s.leftAng : s.rightAng, s.gunHudAng, s.gunHudShift);
    }
    if (m_Game)
        s.hudOk = m_Game->ReadWristHudValues(s.hud[0], s.hud[1], s.hud[2], s.hud[3], s.hud[4], s.hud[5]);
    GetViewBasis(&s.fwd, &s.right, &s.up);
    s.eyeOrig[0] = GetViewOrigin(s.body);
    s.eyeOrig[1] = GetViewOriginLeft(s.body);
    s.eyeOrig[2] = GetViewOriginRight(s.body);
    s.viewAngles = GetViewAngle();
    s.havePoints = AimOverlayPointsNow(s.beamA, s.beamB, s.dot);
    s.aimBeamValid = m_AimBeamValid;
    s.aimStage = m_AimUpdateStage;
    s.aimBeamHit = m_AimBeamHit;
    s.aimCrosshairValid = m_AimCrosshairValid;
    s.aimCrosshairWorld = m_AimCrosshairWorld;
    s.rotationOffsetY = m_RotationOffsetY.load(std::memory_order_acquire);
    {
        const unsigned ms = MagReload::g_GunWristMs[1].load(std::memory_order_acquire);
        const DWORD now = GetTickCount();
        s.gunWristOk = ms != 0 && now - ms <= 100;
        if (s.gunWristOk)
            for (int k = 0; k < 12; ++k)
                s.gunWrist[k] = MagReload::g_GunWrist[1][k].load(std::memory_order_relaxed);
    }
    s.valid = true;
}

void VR::MenuDrawBody(Vector& body, float& rotationOffsetY) const
{
    // MC: the render thread draws frame N while the game thread has moved the
    // body to frame N+1; eyes come from the snapshot, so the latched weapon
    // menu must too (user 2026-11-20: menu trailed/jittered while walking).
    if (const HandDrawSnap* snap = t_HandSnap)
    {
        body = snap->body;
        rotationOffsetY = snap->rotationOffsetY;
        return;
    }
    body = (m_HasStereoBodyOrigin && m_StereoBodyOrigin.LengthSqr() > 1.f) ? m_StereoBodyOrigin : m_SetupOrigin;
    rotationOffsetY = m_RotationOffsetY.load(std::memory_order_acquire);
}

bool VR::HudValuesForDraw(int& health, int& armor, int& clip, int& reserve, int& secondary, int& air) const
{
    if (const HandDrawSnap* snap = t_HandSnap)
    {
        health = snap->hud[0];
        armor = snap->hud[1];
        clip = snap->hud[2];
        reserve = snap->hud[3];
        secondary = snap->hud[4];
        air = snap->hud[5];
        return snap->hudOk;
    }
    health = armor = clip = reserve = secondary = air = -1;
    return m_Game && m_Game->ReadWristHudValues(health, armor, clip, reserve, secondary, air);
}

bool VR::GunCapWristForDraw(float out[12]) const
{
    // MC: the render thread draws frame N while the game thread already
    // publishes frame N+1's bones; the cap must use the wrist of the gun it
    // closes (user 2026-11-20: the plate lagged the hand on heavy frames).
    if (const HandDrawSnap* snap = t_HandSnap)
    {
        if (!snap->gunWristOk)
            return false;
        std::memcpy(out, snap->gunWrist, sizeof(snap->gunWrist));
        return true;
    }
    const unsigned ms = MagReload::g_GunWristMs[1].load(std::memory_order_acquire);
    if (ms == 0 || GetTickCount() - ms > 100)
        return false;
    for (int k = 0; k < 12; ++k)
        out[k] = MagReload::g_GunWrist[1][k].load(std::memory_order_relaxed);
    return true;
}

bool VR::DrawIndependentHandMarkers(IDirect3DSurface9* eyeSurf, int stereoEye, bool drawOverlays, bool drawGloves)
{
    BMVR_PROF("vr DrawIndependentHandMarkers");
    if (!eyeSurf || !m_IsVREnabled || !IsGameplayEligible() || !m_HmdPoseValid || !g_D3DVR9)
        return false;
    char gpuLabel[48];
    std::snprintf(gpuLabel, sizeof(gpuLabel), "mod hands/HUD overlay e%d%s", stereoEye, drawOverlays ? " +overlays" : "");
    GpuTimerScope gpuScope(g_GpuTimer.device, gpuLabel);
    if (IsMenuUp())
        return false;
    if (!(m_Fov > 10.f) || !m_HmdOriginLatched)
        return false;
    // Scoped: the view is the magnified scope picture, so gloves and the hand
    // HUD would be drawn magnified across it (the two-hand support glove sat
    // over the lower half of the lens).
    if (m_ScopeZoomActive)
        drawGloves = false;

    // Offsets that no longer match the game (a Black Mesa update) only showed in bmvr_log.txt, so
    // players ran for a session with fixes silently off (2026-10-27). Warn in the headset once in a
    // map, and again if more go missing later (server.dll hooks resolve after the first map load).
    // Only gameplay offsets count here: debug-only ones (PerfLog stats, profiling) cost players
    // nothing and show on the VR panel's Debug tab ("Missing hooks") instead.
    const int missingOffsets = Game::missingOffsetCount();
    if (missingOffsets > m_MissingOffsetsWarned && m_Game && m_Game->m_EngineClient
        && m_Game->m_EngineClient->IsInGame())
    {
        m_MissingOffsetsWarned = missingOffsets;
        char detail[64];
        sprintf_s(detail, "%d gameplay hook%s missing - Black Mesa updated?", missingOffsets,
            missingOffsets == 1 ? "" : "s");
        Game::logMsg("Out-of-date warning shown: %s", detail);
        ShowToast("VR MOD OUT OF DATE", 10000, detail);
    }

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
        Vector snapPos{};
        QAngle snapAng{};
        if (TryGetTwoHandOffhandDrawPose(true, snapPos, snapAng))
        {
            leftPos = snapPos;
            leftAng = snapAng;
        }
        if (TryGetTwoHandOffhandDrawPose(false, snapPos, snapAng))
        {
            rightPos = snapPos;
            rightAng = snapAng;
        }
        TryGetLadderPinDrawPos(true, body, leftPos);
        TryGetLadderPinDrawPos(false, body, rightPos);
    }
    const HandDrawSnap* snap = t_HandSnap;
    if (snap)
    {
        leftOk = snap->leftOk;
        rightOk = snap->rightOk;
        leftAng = snap->leftAng;
        rightAng = snap->rightAng;
        leftPos = snap->leftPos;
        rightPos = snap->rightPos;
        body = snap->body;
    }
    if (!leftOk && !rightOk)
        return false;

    auto toWorld = [&](const Vector& tracking) {
        return ControllerTrackingToWorld(body, tracking);
    };

    Vector fwd, right, up;
    Vector eyeOrig{};
    if (snap)
    {
        fwd = snap->fwd;
        right = snap->right;
        up = snap->up;
        eyeOrig = snap->eyeOrig[(stereoEye == 1 || stereoEye == 2) ? stereoEye : 0];
    }
    else
    {
        GetViewBasis(&fwd, &right, &up);
        eyeOrig = (stereoEye == 1)
            ? GetViewOriginLeft(body)
            : (stereoEye == 2) ? GetViewOriginRight(body) : GetViewOrigin(body);
    }
    const bool aimBeamValid = snap ? snap->aimBeamValid : m_AimBeamValid;
    const bool aimBeamHit = snap ? snap->aimBeamHit : m_AimBeamHit;
    const bool aimCrosshairValid = snap ? snap->aimCrosshairValid : m_AimCrosshairValid;
    const Vector aimCrosshairWorld = snap ? snap->aimCrosshairWorld : m_AimCrosshairWorld;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return false;

    // One upload per stereo pair. The right eye (2) must not upload again.
    {
        BMVR_PROF("hands: icon uploads + glove warmup");
        if (stereoEye != 2)
            bmvr::PumpHudIconUploads(device);

        if (bmvr::TryVrGloves() && bmvr::g_VrHandsGlovesEnabled)
            g_VrGloves.WarmupGpu(device);
    }

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
    // OffCenterEyes: project into the union frustum (WorldRenderFov,
    // m_OffCenterAspect), then remap to this eye's sub-rectangle.
    const float aspect = m_OffCenterActive ? m_OffCenterAspect
        : (bmvr::UseGbMatchViewLock() && m_Aspect > 0.1f) ? m_Aspect : pixelAspect;
    const float projFov = m_OffCenterActive ? WorldRenderFov() : HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
    if (!(tanHalf > 0.01f))
    {
        device->Release();
        return false;
    }
    float ocSx = 1.f, ocOx = 0.f, ocSy = 1.f, ocOy = 0.f;
    OffCenterNdc(stereoEye, ocSx, ocOx, ocSy, ocOy);

    auto project = [&](const Vector& world, float& sx, float& sy) -> bool {
        const Vector delta = world - eyeOrig;
        const float z = delta.Dot(fwd);
        if (z < 4.f)
            return false;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x / z) / tanHalf * ocSx + ocOx;
        const float ndcY = ((y / z) * aspect) / tanHalf * ocSy + ocOy;
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
    {
        BMVR_PROF("hands: state block capture");
        if (!m_HandEngineStateBlock)
        {
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &m_HandEngineStateBlock)))
                m_HandEngineStateBlock = nullptr;
            else
                engineBlock = m_HandEngineStateBlock;
        }
        else if (SUCCEEDED(m_HandEngineStateBlock->Capture()))
            engineBlock = m_HandEngineStateBlock;
    }
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
    bool wantHandHud = drawOverlays && bmvr::g_HandHud && m_Game && m_HasHevSuit && !m_ScopeZoomActive;
    bool wantOverlay = drawOverlays && (drawBoxes
        || WeaponMenuOpen()
        || SettingsPanelOpen()
        || AimCrosshairVisible()
        || ToastVisible()
        || bmvr::g_DebugBlankEye >= 0.5f); // keep the blanked eye after the panel closes
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
        BMVR_PROF("hands: overlay (menu/boxes)");
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

        // Debug (VR panel): black out one eye, so "which eye shows it?" needs
        // no winking. Stays on with the panel closed (not saved; off at restart).
        // Before the panel, which stays visible to switch it off.
        if (stereoEye > 0 && static_cast<int>(bmvr::g_DebugBlankEye + 0.5f) == stereoEye)
        {
            DWORD zOld = 0, blendOld = 0;
            device->GetRenderState(D3DRS_ZENABLE, &zOld);
            device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blendOld);
            device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            const float fw = static_cast<float>(w), fh = static_cast<float>(h);
            const Vert black[4] = { { -0.5f, -0.5f, 0.f, 1.f, D3DCOLOR_XRGB(0, 0, 0) },
                { fw, -0.5f, 0.f, 1.f, D3DCOLOR_XRGB(0, 0, 0) },
                { -0.5f, fh, 0.f, 1.f, D3DCOLOR_XRGB(0, 0, 0) },
                { fw, fh, 0.f, 1.f, D3DCOLOR_XRGB(0, 0, 0) } };
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, black, sizeof(Vert));
            device->SetRenderState(D3DRS_ZENABLE, zOld);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, blendOld);
        }

        if (SettingsPanelOpen())
            DrawSettingsPanel(device, stereoEye, w, h, eyeOrig, fwd, right, up);
        else if (WeaponMenuOpen())
            DrawWeaponMenu(device, stereoEye, w, h, eyeOrig, fwd, right, up);
        // Laser dropout log (below): keep entering for 300 ms after a beam.
        static DWORD s_lastDrawn[3];
        static int s_lines;
        const bool beamRecent = bmvr::g_VrLaserBeam && (stereoEye == 1 || stereoEye == 2) && s_lastDrawn[stereoEye]
            && GetTickCount() - s_lastDrawn[stereoEye] < 300;
        if (SettingsPanelOpen() || WeaponMenuOpen())
            s_lastDrawn[1] = s_lastDrawn[2] = 0;
        else if ((snap ? (aimBeamValid || aimCrosshairValid) : AimCrosshairVisible()) || RpgLockRingVisible() || beamRecent)
        {
            if (snap && bmvr::g_PerfLog && !AimCrosshairVisible())
            {
                // The frame's snapshot has the aim overlay, the live flags do not.
                static std::atomic<uint32_t> s_n{ 0 };
                static std::atomic<DWORD> s_t{ 0 };
                const uint32_t n = ++s_n;
                const DWORD now = GetTickCount();
                if (now - s_t.load() >= 5000)
                {
                    s_t = now;
                    Game::logMsg("aim overlay: snapshot valid while the live flags were off, %u eye draws so far", n);
                }
            }
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            Vector beamA{}, beamB{}, dotNow{};
            bool havePoints = false;
            if (snap)
            {
                havePoints = snap->havePoints;
                beamA = snap->beamA;
                beamB = snap->beamB;
                dotNow = snap->dot;
            }
            else
                havePoints = AimOverlayPointsNow(beamA, beamB, dotNow);
            const bool beamHidden = m_ViewmodelBeamHidden.load(std::memory_order_relaxed);
            // Beam and dot are depth-tested against the eye's scene depth
            // (world + gun, as for the gloves), so the gun hides them. Depth
            // as the eye projection writes it: f/(f-n) - n*f/((f-n)*z).
            const float zn = (m_StereoZNear > 0.01f) ? m_StereoZNear : 7.f;
            const float zf = (m_StereoZFar > zn + 1.f) ? m_StereoZFar : 28377.f;
            auto depthAt = [&](float z) { return zf / (zf - zn) - (zn * zf) / ((zf - zn) * z); };
            const bool sceneDepth = gloveDepth
                && (gloveDepth == oldDepth
                    || (stereoEye == 1 && gloveDepth == m_D9LeftEyeDepthSurface)
                    || (stereoEye == 2 && gloveDepth == m_D9RightEyeDepthSurface));
            IDirect3DSurface9* depthBefore = nullptr;
            auto beginSceneDepth = [&]() {
                if (!sceneDepth)
                    return;
                device->GetDepthStencilSurface(&depthBefore);
                device->SetDepthStencilSurface(gloveDepth);
                device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
                device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
                device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            };
            auto endSceneDepth = [&]() {
                if (!sceneDepth)
                    return;
                device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
                device->SetDepthStencilSurface(depthBefore);
                if (depthBefore)
                    depthBefore->Release();
                depthBefore = nullptr;
            };
            bool beamDrawn = false;
            float dropZa = 0.f, dropZb = 0.f, dropLen = -1.f;
            if (aimBeamValid && !beamHidden && havePoints && (beamB - beamA).LengthSqr() > 1.f)
            {
                // Clip to just in front of the eye, then a quad whose width is
                // a fixed world size at each end (perspective taper), never
                // thinner than a pixel so it does not shimmer far away.
                Vector a = beamA;
                Vector b = beamB;
                constexpr float kNear = 4.f;
                const float za = (a - eyeOrig).Dot(fwd);
                const float zb = (b - eyeOrig).Dot(fwd);
                dropZa = za;
                dropZb = zb;
                if (za >= kNear || zb >= kNear)
                {
                    if (za < kNear)
                        a = a + (b - a) * ((kNear - za) / (zb - za));
                    else if (zb < kNear)
                        b = a + (b - a) * ((kNear - za) / (zb - za));
                    auto toScreen = [&](const Vector& p, float& sx, float& sy, float& z) {
                        const Vector d = p - eyeOrig;
                        z = (std::max)(kNear, d.Dot(fwd));
                        const float ndcX = (d.Dot(right) / z) / tanHalf * ocSx + ocOx;
                        const float ndcY = ((d.Dot(up) / z) * aspect) / tanHalf * ocSy + ocOy;
                        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
                        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
                    };
                    float ax, ay, az, bx, by, bz;
                    toScreen(a, ax, ay, az);
                    toScreen(b, bx, by, bz);
                    float nx = -(by - ay);
                    float ny = bx - ax;
                    const float len = sqrtf(nx * nx + ny * ny);
                    dropLen = len;
                    if (len > 0.5f)
                    {
                        beamDrawn = true;
                        nx /= len;
                        ny /= len;
                        float beamWidthHu = bmvr::g_VrLaserWidth; // half width; 0.06 = 3 mm beam
                        if (!(beamWidthHu > 0.005f) || beamWidthHu > 1.f)
                            beamWidthHu = 0.06f;
                        const float pxPerHu = static_cast<float>(w) * 0.5f * ocSx / tanHalf;
                        const float wa = (std::max)(0.5f, beamWidthHu * pxPerHu / az);
                        const float wb = (std::max)(0.5f, beamWidthHu * pxPerHu / bz);
                        const D3DCOLOR ca = D3DCOLOR_ARGB(170, 255, 40, 25);
                        const D3DCOLOR cb = aimBeamHit
                            ? D3DCOLOR_ARGB(110, 255, 40, 25) : D3DCOLOR_ARGB(0, 255, 40, 25);
                        beginSceneDepth();
                        const float da = sceneDepth ? depthAt(az) : 0.f;
                        const float db = sceneDepth ? depthAt(bz) : 0.f;
                        Vert v[4] = {
                            { ax + nx * wa, ay + ny * wa, da, 1.f / az, ca },
                            { ax - nx * wa, ay - ny * wa, da, 1.f / az, ca },
                            { bx + nx * wb, by + ny * wb, db, 1.f / bz, cb },
                            { bx - nx * wb, by - ny * wb, db, 1.f / bz, cb },
                        };
                        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vert));
                        endSceneDepth();
                    }
                }
            }
            if (bmvr::g_AimDebug && bmvr::g_VrLaserBeam && (stereoEye == 1 || stereoEye == 2))
            {
                // Laser dropout log (AimDebug only: in normal play these are
                // intended hides, reloads and beams shorter than 1 unit): a
                // frame without the beam within 300 ms of one with it, and why
                // (at most 300 lines per session).
                const DWORD nowMs = GetTickCount();
                if (beamDrawn)
                    s_lastDrawn[stereoEye] = nowMs;
                else if (s_lines < 300 && s_lastDrawn[stereoEye] && nowMs - s_lastDrawn[stereoEye] < 300)
                {
                    ++s_lines;
                    Game::logMsg("laser dropout e%d %lu ms after the last beam: snap=%d valid=%d (update stage %d) hidden=%d points=%d lenSq=%.1f za=%.1f zb=%.1f screenLen=%.2f liveValid=%d",
                        stereoEye, static_cast<unsigned long>(nowMs - s_lastDrawn[stereoEye]), snap ? 1 : 0, aimBeamValid ? 1 : 0,
                        snap ? snap->aimStage : m_AimUpdateStage, beamHidden ? 1 : 0, havePoints ? 1 : 0,
                        (beamB - beamA).LengthSqr(), dropZa, dropZb, dropLen, m_AimBeamValid ? 1 : 0);
                }
            }
            float cx = 0.f, cy = 0.f;
            if (aimCrosshairValid && project(havePoints ? dotNow : aimCrosshairWorld, cx, cy))
            {
                if (bmvr::g_VrCrosshairDot)
                {
                    float scale = bmvr::g_VrCrosshairScale;
                    if (!(scale > 0.05f) || scale > 8.f)
                        scale = 1.f;
                    // World radius 0.25 u (~6 mm) x scale; floor 1.25 px per
                    // 1440 px of eye height x scale.
                    const Vector dotWorld = havePoints ? dotNow : aimCrosshairWorld;
                    const float z = (std::max)(4.f, (dotWorld - eyeOrig).Dot(fwd));
                    const float pxPerHu = static_cast<float>(w) * 0.5f * ocSx / tanHalf;
                    const float floorPx = 1.25f * static_cast<float>(h) / 1440.f * scale;
                    // Pulled a little toward the eye so the surface it lies
                    // on does not cover it; the gun in front still does.
                    const float pulled = (std::max)(zn + 0.5f, z - (1.5f + 0.01f * z));
                    beginSceneDepth();
                    DrawAimDot(device, cx, cy, (std::max)(floorPx, 0.25f * scale * pxPerHu / z),
                        sceneDepth ? depthAt(pulled) : 0.f);
                    endSceneDepth();
                }
                else
                    DrawAimCrosshair(device, cx, cy, h);
            }
            // The flat RPG crosshair spins red lines around the centre while
            // the laser guides; here they spin around the laser point, on top
            // of everything like the flat HUD.
            Vector laserWorld{};
            float lx = 0.f, ly = 0.f;
            if (RpgLockRingVisible() && TryGetRpgLaserWorld(laserWorld) && project(laserWorld, lx, ly))
                DrawRpgLockRing(device, lx, ly, h);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        }
        DrawToast(device, stereoEye, w, h, eyeOrig, fwd, right, up); // returns when no toast

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
        BMVR_PROF("hands: gloves");
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
        const Vector viewAngles = snap ? snap->viewAngles : GetViewAngle();
        drewGloves = g_VrGloves.DrawForEye(
            device,
            stereoEye,
            eyeOrig,
            viewAngles,
            projFov,
            aspect,
            m_VRScale,
            bmvr::g_VrHandsModelScale,
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

    // Damage flash: a short red tint over the view, like the battery pickup's
    // blue screen fade. The flat HUD's red damage edges are a 2D HUD element
    // that never reaches the eyes.
    UpdateDamageFlash(stereoEye);
    if (drawOverlays && bmvr::g_DamageFlash && m_DmgFlashLevel > 0.f)
    {
        restoreEngineDrawState();
        device->SetViewport(&eyeVp);
        applyHandOverlayState();
        DrawDamageFlash(device, stereoEye, w, h);
        device->SetFVF(0);
        restoreEngineDrawState();
    }

    // HL2VR hand/weapon HUD: IgnoreZ (draw after the glove mesh, Z off) plus
    // IsBackfacing so the ammo plate is invisible from behind the gun.
    if (wantHandHud)
    {
        BMVR_PROF("hands: hand HUD");
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
        BMVR_PROF("hands: state block apply");
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
        const float kPitch = bmvr::g_HandHudTilt;
        const float kRoll = rightHand ? bmvr::g_HandHudRoll : -bmvr::g_HandHudRoll;
        fwd = HudRotate(fwd, left, kPitch);
        up = HudRotate(up, left, kPitch);
        left = HudRotate(left, fwd, kRoll);
        up = HudRotate(up, fwd, kRoll);
        // Size/offsets/angles from the panel (Hands & Weapons -> HUD POSITION) / bmvr_hand_hud_*.
        const float size = bmvr::g_HandHudSize;
        const float lift = bmvr::g_HandHudUp;
        Vector offset = up * lift + fwd * bmvr::g_HandHudForward + left * bmvr::g_HandHudSide;
        {
            // The HUD rows place the HUD on the hand as is; the glove roll then turns the
            // whole panel with the hand (same z rule as the glove in vr_hands.cpp), so roll
            // and HUD rows do not fight. Reference roll: the rows were tuned with the
            // default 90 deg glove roll (unchanged look at 90).
            const float refRoll = 90.f;
            const float dRoll = bmvr::g_VrHandsRollDeg - refRoll;
            QAngle rolled = ctrlAng;
            rolled.z += rightHand ? dRoll : -dRoll;
            Vector cf, cr, cu, rf, rr, ru;
            QAngle::AngleVectors(ctrlAng, &cf, &cr, &cu);
            QAngle::AngleVectors(rolled, &rf, &rr, &ru);
            const auto remap = [&](const Vector& v) {
                return rf * v.Dot(cf) + rr * v.Dot(cr) + ru * v.Dot(cu);
            };
            fwd = remap(fwd);
            left = remap(left);
            up = remap(up);
            offset = remap(offset);
        }
        const Vector center = ctrlPos + offset;
        const Vector origin = center - fwd * (size * 0.5f) - left * (size * 0.5f);
        HudMakePanel(origin, fwd, left, up, size, out);
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
        // Per-weapon placement on top (VR panel DEBUG "AMMO HUD", VR/ammo_hud.txt;
        // VR::SyncAmmoHudRows keeps the globals on the held weapon's values).
        // Sideways + = away from the gun.
        const float size = (std::max)(0.25f, bmvr::g_AmmoHudSize);
        const Vector center = ctrlPos + right * (inward * (kBeside + bmvr::g_AmmoHudSide))
            + fwd * (bmvr::g_AmmoHudForward - kBack) + up * (kUp + bmvr::g_AmmoHudUp);
        Vector face = right * inward;
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
        // Turn: about the panel's vertical axis (+ = the gun-front edge toward the
        // viewer, who sits on the face side). Tilt: then about the turned panel's
        // horizontal axis (+ = top toward the viewer). Roll: last, in the panel
        // plane. Each step uses the axes the previous one left, so tilt stays a
        // tilt at any turn (tilt first locked tilt onto roll at turn 90, RPG
        // screen, user 2026-11-25). Turn and roll are mirrored for the left gun
        // hand so one file value looks the same.
        const float mirror = rightHand ? 1.f : -1.f;
        if (bmvr::g_AmmoHudTurn != 0.f)
        {
            face = HudRotate(face, left, bmvr::g_AmmoHudTurn * mirror);
            xaxis = HudRotate(xaxis, left, bmvr::g_AmmoHudTurn * mirror);
        }
        if (bmvr::g_AmmoHudTilt != 0.f)
        {
            face = HudRotate(face, xaxis, -bmvr::g_AmmoHudTilt);
            left = HudRotate(left, xaxis, -bmvr::g_AmmoHudTilt);
        }
        if (bmvr::g_AmmoHudRoll != 0.f)
        {
            xaxis = HudRotate(xaxis, face, bmvr::g_AmmoHudRoll * mirror);
            left = HudRotate(left, face, bmvr::g_AmmoHudRoll * mirror);
        }
        const Vector origin = center - xaxis * (size * 0.5f) - left * (size * 0.5f);
        HudMakePanel(origin, xaxis, left, face, size, out);
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

void VR::UpdateDamageFlash(int stereoEye)
{
    // Once per frame (eye 1); eye 2 draws the same level.
    if (stereoEye != 1)
        return;
    const double now = QpcNowMs();
    constexpr double kFadeMs = 500.0;
    if (m_DmgFlashLevel > 0.f && now - m_DmgFlashStartMs >= kFadeMs)
        m_DmgFlashLevel = 0.f;
    int health = -1, armor = -1, clip = -1, reserve = -1, secondary = -1, air = -1;
    // Loads/menus change health without damage (a save with less health).
    if (!bmvr::g_DamageFlash || m_VrPhase != VrPhase::Gameplay || PauseUiActive() || !m_Game
        || !HudValuesForDraw(health, armor, clip, reserve, secondary, air)
        || health < 0 || health > 1000)
    {
        m_DmgLastHealth = m_DmgLastArmor = -1;
        m_DmgSettleUntilMs = -1.0;   // armed: set on the first gameplay sample
        return;
    }
    // Just (back) in gameplay: the first values after a load or a menu are stale for a few frames (after loading
    // bm_c4a1a: "armor 90 -> 0" flashed at full strength), and "new game" from the pause menu runs a moment of
    // gameplay while the old map shuts down (red fade right before the new map, user 2026-11-16). Let the
    // baseline settle for 1 s before a drop counts as damage.
    // After a map load the HUD values stay the previous map's for over a second (2026-11-17: "new game" armor
    // 92 -> 0 and a load health 100 -> 55 both flashed after the 1 s settle), so a load settles 4 s.
    // The map start's own short pause re-arms the 1 s settle, so the load window is kept separately.
    if (m_DmgSettleUntilMs < 0.0)
        m_DmgSettleUntilMs = now + 1000.0;
    if (m_DmgMapLoaded)
    {
        m_DmgLoadSettleUntilMs = now + 4000.0;
        m_DmgMapLoaded = false;
    }
    if (armor < 0 || armor > 1000)
        armor = 0;
    if (m_DmgLastHealth >= 0)
    {
        const int lost = std::max(0, m_DmgLastHealth - health) + std::max(0, m_DmgLastArmor - armor);
        // Health up in the same sample = a reset (new game, load, respawn), never damage.
        if (lost > 0 && (now < m_DmgSettleUntilMs || now < m_DmgLoadSettleUntilMs || health > m_DmgLastHealth))
        {
            static int s_settleLog;
            if (s_settleLog < 8)
            {
                Game::logMsg("Damage flash suppressed (settling after load/menu): health %d -> %d, armor %d -> %d",
                    m_DmgLastHealth, health, m_DmgLastArmor, armor);
                ++s_settleLog;
            }
        }
        else if (lost > 0)
        {
            const float t = static_cast<float>((now - m_DmgFlashStartMs) / kFadeMs);
            const float current = m_DmgFlashLevel * std::max(0.f, 1.f - t);
            m_DmgFlashLevel = std::min(1.f, current + 0.4f + static_cast<float>(lost) / 40.f);
            m_DmgFlashStartMs = now;
            static int s_log;
            if (s_log < 8)
            {
                Game::logMsg("Damage flash: health %d -> %d, armor %d -> %d, level %.2f",
                    m_DmgLastHealth, health, m_DmgLastArmor, armor, m_DmgFlashLevel);
                ++s_log;
            }
        }
    }
    m_DmgLastHealth = health;
    m_DmgLastArmor = armor;
}

void VR::DrawDamageFlash(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h)
{
    if (!device || !(m_DmgFlashLevel > 0.f) || w == 0 || h == 0)
        return;
    constexpr double kFadeMs = 500.0;
    const float t = static_cast<float>((QpcNowMs() - m_DmgFlashStartMs) / kFadeMs);
    if (t >= 1.f)
        return;
    // Fast rise (first 10 %), then ease out.
    const float rise = std::min(1.f, t / 0.1f);
    const float fall = (1.f - t) * (1.f - t);
    const float edgeA = std::clamp(bmvr::g_DamageFlashStrength * m_DmgFlashLevel * rise * fall, 0.f, 1.f);
    if (edgeA < 0.004f)
        return;
    const float aspect = m_OffCenterActive ? m_OffCenterAspect
        : static_cast<float>(w) / static_cast<float>(h);
    const float projFov = m_OffCenterActive ? WorldRenderFov() : HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
    if (!(tanHalf > 0.01f))
        return;
    float ocSx = 1.f, ocOx = 0.f, ocSy = 1.f, ocOy = 0.f;
    OffCenterNdc(stereoEye, ocSx, ocOx, ocSy, ocOy);

    // Rings in view-tangent space (head-fixed, matches the off-centre eye
    // projection): centre at half strength, full strength from tan 1.0 out
    // (edge of what the lenses show), outer ring well past the image corners.
    constexpr int kSeg = 40;
    const float radius[4] = { 0.f, 0.5f, 1.0f, 4.f };
    const float alphaMul[4] = { 0.45f, 0.55f, 1.f, 1.f };
    struct Vert { float x, y, z, rhw; D3DCOLOR color; };
    auto vtx = [&](int ring, int seg) {
        const float ang = static_cast<float>(seg) * (6.2831853f / kSeg);
        const float tx = cosf(ang) * radius[ring];
        const float ty = sinf(ang) * radius[ring];
        const float ndcX = tx / tanHalf * ocSx + ocOx;
        const float ndcY = ty * aspect / tanHalf * ocSy + ocOy;
        const int a = static_cast<int>(edgeA * alphaMul[ring] * 255.f + 0.5f);
        return Vert{ (ndcX * 0.5f + 0.5f) * static_cast<float>(w),
            (-ndcY * 0.5f + 0.5f) * static_cast<float>(h), 0.f, 1.f,
            D3DCOLOR_ARGB(std::clamp(a, 0, 255), 200, 16, 12) };
    };
    static Vert verts[kSeg * 3 * 6];
    int n = 0;
    for (int seg = 0; seg < kSeg; ++seg)
    {
        verts[n++] = vtx(0, 0);
        verts[n++] = vtx(1, seg);
        verts[n++] = vtx(1, seg + 1);
        for (int ring = 1; ring < 3; ++ring)
        {
            const Vert a = vtx(ring, seg), b = vtx(ring, seg + 1);
            const Vert c = vtx(ring + 1, seg), d = vtx(ring + 1, seg + 1);
            verts[n++] = a; verts[n++] = c; verts[n++] = b;
            verts[n++] = b; verts[n++] = c; verts[n++] = d;
        }
    }
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, verts, sizeof(Vert));
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
}

void VR::DrawHandHud(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
    bool leftOk, const Vector& leftOrigin, const QAngle& leftAng,
    bool rightOk, const Vector& rightOrigin, const QAngle& rightAng,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    if (!m_Game || !device)
        return;
    int health = -1, armor = -1, clip = -1, reserve = -1, secondary = -1, air = -1;
    if (!HudValuesForDraw(health, armor, clip, reserve, secondary, air))
        return;
    {
        // Diagnostic (capped): how often a live read on this (render) thread
        // would have shown other ammo numbers than the frame's snapshot.
        static int s_lines, s_frames, s_diff;
        static DWORD s_next;
        if (t_HandSnap && stereoEye != 2 && s_lines < 12)
        {
            int h2, a2, c2, r2, s2, air2;
            if (m_Game->ReadWristHudValues(h2, a2, c2, r2, s2, air2))
            {
                ++s_frames;
                if (c2 != clip || r2 != reserve || s2 != secondary)
                {
                    if (s_diff++ < 3)
                        Game::logMsg("Hand HUD: live read clip %d reserve %d, frame snapshot clip %d reserve %d",
                            c2, r2, clip, reserve);
                }
            }
            const DWORD now = GetTickCount();
            if (now >= s_next)
            {
                if (s_next != 0 && s_diff > 0)
                {
                    Game::logMsg("Hand HUD: live read differed from the frame snapshot on %d of %d frames (10 s)",
                        s_diff, s_frames);
                    ++s_lines;
                }
                s_frames = s_diff = 0;
                s_next = now + 10000;
            }
        }
    }
    const float aspect = m_OffCenterActive ? m_OffCenterAspect
        : (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float projFov = m_OffCenterActive ? WorldRenderFov() : HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
    if (!(tanHalf > 0.01f))
        return;
    float ocSx = 1.f, ocOx = 0.f, ocSy = 1.f, ocOy = 0.f;
    OffCenterNdc(stereoEye, ocSx, ocOx, ocSy, ocOy);
    auto project = [&](const Vector& world, float& sx, float& sy) -> bool {
        const Vector delta = world - eyeOrig;
        const float z = delta.Dot(fwd);
        if (z < 4.f)
            return false;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x / z) / tanHalf * ocSx + ocOx;
        const float ndcY = ((y / z) * aspect) / tanHalf * ocSy + ocOy;
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
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
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

    const bool blueHud = bmvr::UseBlueHud();
    const D3DCOLOR theme = blueHud
        ? D3DCOLOR_RGBA(64, 168, 255, 235)
        : D3DCOLOR_RGBA(255, 176, 0, 235);
    const D3DCOLOR themeDim = blueHud
        ? D3DCOLOR_RGBA(64, 168, 255, 170)
        : D3DCOLOR_RGBA(255, 176, 0, 170);
    const D3DCOLOR themeBack = blueHud
        ? D3DCOLOR_RGBA(64, 168, 255, 55)
        : D3DCOLOR_RGBA(255, 176, 0, 55);
    const D3DCOLOR red = D3DCOLOR_RGBA(255, 48, 32, 240);
    const D3DCOLOR redBack = D3DCOLOR_RGBA(255, 48, 32, 60);
    constexpr int kLowHealth = 25;
    const bool rightDominant = !bmvr::g_LeftHanded;
    // Ammo panel orientation: the gun hand, or the two-hand aim (long guns).
    QAngle gunHudAng = rightDominant ? rightAng : leftAng;
    Vector gunHudPos = rightDominant ? rightOrigin : leftOrigin;
    if (t_HandSnap)
    {
        if (t_HandSnap->gunHudTwoHand)
        {
            gunHudAng = t_HandSnap->gunHudAng;
            gunHudPos += t_HandSnap->gunHudShift;
        }
    }
    else
    {
        QAngle twoHandAng{};
        Vector shift{};
        if (TwoHandGunHudAngles(gunHudAng, twoHandAng, shift))
        {
            gunHudAng = twoHandAng;
            gunHudPos += shift;
        }
    }

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
            // On the gun the glove sits at its tuned spot: the panel goes with it.
            Vector handHudOrigin = rightDominant ? leftOrigin : rightOrigin;
            const bool supportOnGun = rightDominant ? WantsOffhandGloveWeaponGripCurl() : WantsRightGloveWeaponGripCurl();
            const bool gunHandOk = rightDominant ? rightOk : leftOk;
            if (supportOnGun && bmvr::g_HudHealthOnGun && gunHandOk && !m_EmptyHands)
            {
                // Holding the gun with both hands (user, 2026-11-18): the
                // health panel sits on the ammo panel's plane, right above
                // it, so both read at once while aiming (on the support
                // wrist it turned away from the eye).
                HudPanel gunPanel{};
                BuildAmmoHudPanel(gunHudPos, gunHudAng, rightDominant, gunPanel);
                if (gunPanel.valid)
                {
                    handPanel = gunPanel;
                    handPanel.ul = gunPanel.ul - gunPanel.yDown * (gunPanel.height * 1.08f);
                }
            }
            else
            {
                if (supportOnGun)
                    handHudOrigin = handHudOrigin + GunGripHandOffset(!rightDominant, rightDominant ? rightAng : leftAng);
                BuildHandHudPanel(
                    handHudOrigin,
                    rightDominant ? leftAng : rightAng,
                    !rightDominant, handPanel);
            }
            if (handPanel.valid && !HudIsBackfacing(handPanel, eyeOrig))
            {
                const bool showArmor = armor >= 0 && armor <= 200;
                const bool showAir = air >= 0 && air <= 100;
                const int rows = 1 + (showArmor ? 1 : 0) + (showAir ? 1 : 0);
                const float pad = handPanel.width * 0.08f;
                const float gap = handPanel.width * 0.06f;
                const float rowH = (handPanel.height - 2.f * pad - gap * static_cast<float>(rows - 1))
                    / static_cast<float>(rows);
                const float cell = rowH / 7.f;
                float y = pad;
                const bool low = health <= kLowHealth;
                layoutRow(handPanel, pad, y, rowH, cell, health,
                    low ? red : theme, low ? redBack : themeBack,
                    bmvr::AcquireHudIcon(device, "hud_health_overlay.vtf"));
                y += rowH + gap;
                if (showArmor)
                {
                    layoutRow(handPanel, pad, y, rowH, cell, armor,
                        theme, themeBack,
                        bmvr::AcquireHudIcon(device, "hud_hev_overlay.vtf"));
                    y += rowH + gap;
                }
                if (showAir)
                {
                    const bool lowAir = air <= 25;
                    layoutRow(handPanel, pad, y, rowH, cell, air,
                        lowAir ? red : theme, lowAir ? redBack : themeBack,
                        bmvr::AcquireHudIcon(device, "damage_drown.vtf"));
                }
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
        BuildAmmoHudPanel(gunHudPos, gunHudAng, rightDominant, weaponPanel);
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
            // Nothing left in the gun: the count goes red, as low health does.
            if (primary >= 0 && primary <= 999)
                layoutRow(weaponPanel, pad, pad, primaryH, cell, primary,
                    primary == 0 ? red : theme, primary == 0 ? redBack : themeBack, ammoIcon);

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
    BMVR_PROF("vr DrawIndependentHandsOnDesktop");
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
    BMVR_PROF("vr DrawVrGlovesIntoBlitSource");
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

namespace
{
    // Per-eye copies of the reflection/refraction RTs (SkyWaterPerEye).
    // Water in the 3D skybox is drawn before the eye renders its own reflection and refraction, so it samples what
    // those RTs hold then: the other eye's (right eye: the left eye's of this frame; left eye: the right eye's of
    // the previous frame), rendered with the other eye's projection. That far water differs per eye and slides with
    // the head (bm_c2a5a, beyond the dam). Flat gets its own previous frame there. Here: when an eye starts writing
    // such an RT that the other eye wrote last, the other eye's image is saved first; an eye sampling an RT it did
    // not write last gets its own saved image. Only RTs that were actually sampled that way get copies.
    struct EyeRt
    {
        IDirect3DTexture9* tex = nullptr;   // not owned
        UINT w = 0, h = 0;
        D3DFORMAT fmt = D3DFMT_UNKNOWN;
        int writer = 0;                     // eye that wrote it last (0 = not an eye pass)
        bool tracked = false;
        IDirect3DTexture9* copy[3] = {};
        bool valid[3] = {};
    };
    std::vector<EyeRt> g_EyeRts;            // d3d9 (playback) thread only
    thread_local bool t_EyeRtCopying = false; // our own StretchRect must not register its copy as a write

    bool EyeRtSizeMatches(UINT w, UINT h)
    {
        if (bmvr::g_ReflectionRtW > 0)
            return static_cast<int>(w) == bmvr::g_ReflectionRtW && static_cast<int>(h) == bmvr::g_ReflectionRtH;
        return w == 1024 && h == 1024;
    }

    void EyeRtApplyReset()
    {
        if (!g_EyeRtReset.exchange(false))
            return;
        for (EyeRt& r : g_EyeRts)
            for (IDirect3DTexture9*& c : r.copy)
                if (c)
                {
                    c->Release();
                    c = nullptr;
                }
        g_EyeRts.clear();
    }
}

void VR::EyeRtNoteWrite(IDirect3DDevice9* device, IDirect3DSurface9* target)
{
    if (!bmvr::g_SkyWaterPerEye || !device || !target || t_EyeRtCopying)
        return;
    EyeRtApplyReset();
    D3DSURFACE_DESC d{};
    if (FAILED(target->GetDesc(&d)) || !EyeRtSizeMatches(d.Width, d.Height))
        return;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(target->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) || !tex)
        return;
    tex->Release();   // the game owns it; only the pointer is kept
    EyeRt* r = nullptr;
    for (EyeRt& e : g_EyeRts)
        if (e.tex == tex)
            r = &e;
    if (!r)
    {
        if (g_EyeRts.size() >= 16)
            return;
        g_EyeRts.push_back({});
        r = &g_EyeRts.back();
        r->tex = tex;
        r->w = d.Width;
        r->h = d.Height;
        r->fmt = d.Format;
    }
    const int eye = HudPaintActive() ? 0 : GpuTimerEye(this);
    if (r->tracked && (r->writer == 1 || r->writer == 2) && eye != 0 && r->writer != eye)
    {
        // About to be overwritten by this eye: keep the other eye's image.
        const int o = r->writer;
        if (!r->copy[o] && FAILED(device->CreateTexture(r->w, r->h, 1, D3DUSAGE_RENDERTARGET, r->fmt, D3DPOOL_DEFAULT,
            &r->copy[o], nullptr)))
            r->copy[o] = nullptr;
        IDirect3DSurface9* dst = nullptr;
        if (r->copy[o] && SUCCEEDED(r->copy[o]->GetSurfaceLevel(0, &dst)) && dst)
        {
            t_EyeRtCopying = true;
            r->valid[o] = SUCCEEDED(device->StretchRect(target, nullptr, dst, nullptr, D3DTEXF_NONE));
            t_EyeRtCopying = false;
            dst->Release();
        }
    }
    r->writer = eye;
}

IDirect3DBaseTexture9* VR::EyeRtSubstitute(IDirect3DBaseTexture9* texture)
{
    if (!bmvr::g_SkyWaterPerEye)
        return texture;
    EyeRtApplyReset();
    if (g_EyeRts.empty())
        return texture;
    for (EyeRt& r : g_EyeRts)
    {
        if (static_cast<IDirect3DBaseTexture9*>(r.tex) != texture)
            continue;
        const int eye = HudPaintActive() ? 0 : GpuTimerEye(this);
        if (AuditLogging() && g_AuditTexPrefix[0])
        {
            // Panel capture: every per-eye reflection/refraction decision of
            // the audited frame (doorway reflection lead, 2026-11-13).
            const bool sub = eye != 0 && r.writer != eye && (r.writer == 1 || r.writer == 2) && r.tracked
                && r.valid[eye] && r.copy[eye];
            Game::logMsg("audit eyert e%d mat=%s tex %p %ux%u writer e%d tracked=%d copies %p/%p valid %d/%d -> %s",
                eye, t_AuditMaterial, (void*)r.tex, r.w, r.h, r.writer, r.tracked ? 1 : 0, (void*)r.copy[1],
                (void*)r.copy[2], r.valid[1] ? 1 : 0, r.valid[2] ? 1 : 0,
                sub ? "OWN PREVIOUS COPY" : (eye != 0 && r.writer != eye && (r.writer == 1 || r.writer == 2))
                    ? "other eye's image (no copy yet)" : "as is");
        }
        if (eye == 0 || r.writer == eye || (r.writer != 1 && r.writer != 2))
            return texture;
        if (!r.tracked)
        {
            r.tracked = true;
            Game::logMsg("SkyWaterPerEye: eye %d samples a %ux%u RT the other eye wrote last: per-eye copies on for it", eye, r.w, r.h);
        }
        if (r.valid[eye] && r.copy[eye])
        {
            static int s_log;
            if (s_log < 4)
            {
                ++s_log;
                Game::logMsg("SkyWaterPerEye: eye %d gets its own previous %ux%u image", eye, r.w, r.h);
            }
            return r.copy[eye];
        }
        return texture;
    }
    return texture;
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
        m_OffhandGripBoneValid = false;
        if (m_TwoHandActive)
            Game::logMsg("Two-hand off (weapon changed)");
        m_TwoHandActive = false;
        m_TwoHandDrawValid = false;
        m_TwoHandSmoothValid = false;
        uint32_t flags = 0;
        if (m_VmClass.shotgun) flags |= kVmFlagShotgun;
        if (m_VmClass.crowbar) flags |= kVmFlagCrowbar;
        if (m_VmClass.scoped) flags |= kVmFlagScoped;
        if (m_VmClass.rpg) flags |= kVmFlagRpg;
        if (m_VmClass.gluon) flags |= kVmFlagGluon;
        if (m_VmClass.mp5) flags |= kVmFlagMp5;
        if (m_VmClass.glock) flags |= kVmFlagGlock;
        if (m_VmClass.crossbow) flags |= kVmFlagCrossbow;
        if (m_VmClass.revolver) flags |= kVmFlag357;
        if (m_VmClass.numpadKey == "snark") flags |= kVmFlagSnark;
        if (m_VmClass.numpadKey == "satchel") flags |= kVmFlagSatchel;
        m_VmClassFlags.store(flags, std::memory_order_relaxed);
        Game::logMsg("Viewmodel model %s", modelName);
    }
}

// Single lowercase pass over the model path; every consumer reads the result
// from m_VmClass. Tables are the ones ResolveWeaponViewmodelPose,
// ViewmodelVisualScale, ApplyTwoHandWeaponAim and the numpad offset key
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
    // 2026-11-03: glock/357/MP5/shotgun/crossbow angles re-tuned in the HMD
    // so the sights line up with the aim dot (laser beam on, right eye).
    // 2026-11-08: tau ax -1.5 -> -5.5 (tuned in the HMD).
    // 2026-08-28 HMD-saved numpad extras baked into this table. Live
    // extras in VR/viewmodel_offsets.txt still add on top.
    if (has("crowbar") || has("wrench"))
    {
        // ox 12.5 is half an inch back from the 12 / Touch 17.5 pose.
        // oy 8 is half an inch left of 7.5; 9 was too far left.
        // Positive ox pulls back. Positive oy subtracts along controller right.
        // oz -12 -> -8.5 (2026-11-18): with the crowbar's own right hand the
        // fist sat ~8 cm above the controller, the gun fists ~2 cm below
        // (log "fist vs controller", same pose: glock fwd -9.9 up -0.3, MP5
        // -10.5/-2.2, crowbar was -6.1/+5.9, now -9.9/-2.2).
        out.ox = 12.5f; out.oy = 8.f; out.oz = -8.5f; out.ax = -24.5f; out.ay = -6.5f; out.az = -6.f; out.numpadKey = "crowbar"; out.crowbar = true;
    }
    else if (has("glock") || has("pistol") || has("9mm") || has("beretta"))
    { out.ox = 16.5f; out.oy = 4.f; out.oz = -7.f; out.ax = 0.f; out.numpadKey = "glock"; out.glock = true; }
    else if (has("357") || has("python") || has("revolver"))
    { out.ox = 10.f; out.oy = 3.5f; out.oz = -5.5f; out.ax = 0.5f; out.numpadKey = "357"; out.revolver = true; }
    else if (has("mp5") || has("smg") || has("mp5k"))
    { out.ox = 9.f; out.oy = 4.5f; out.oz = -9.f; out.ax = 2.f; out.ay = -1.5f; out.numpadKey = "mp5"; }
    else if (has("shotgun") || has("spas") || has("pump"))
    { out.ox = 9.f; out.oy = 8.f; out.oz = -9.5f; out.ax = -1.5f; out.ay = -4.5f; out.numpadKey = "shotgun"; }
    else if (has("crossbow"))
    { out.ox = 12.5f; out.oy = 11.35f; out.oz = -13.f; out.ax = 1.5f; out.ay = -3.5f; out.az = -3.f; out.numpadKey = "crossbow"; out.crossbow = true; }
    else if (has("rpg") || has("rocket"))
    { out.ox = 12.f; out.oy = 10.5f; out.oz = -6.5f; out.ax = -1.f; out.numpadKey = "rpg"; }
    else if (has("gauss") || has("tau"))
    { out.ox = 15.f; out.oy = 11.5f; out.oz = -16.5f; out.ax = -5.5f; out.ay = -8.f; out.numpadKey = "gauss"; }
    else if (has("egon") || has("gluon"))
    { out.ox = 19.f; out.oy = 8.f; out.oz = -2.f; out.ax = -1.5f; out.ay = -2.f; out.numpadKey = "gluon"; }
    else if (has("hgun") || has("hive") || has("hornet"))
    { out.ox = 19.5f; out.oy = 11.34f; out.oz = -17.69f; out.ax = -1.5f; out.ay = -2.f; out.numpadKey = "hgun"; }
    else if (has("grenade") || has("frag"))
        // Turned by AlignWristToReference (hooks.cpp) when its own hand shows.
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

    auto setTwoHand = [&](float gx, float gy, float gz, bool useBone = true, bool pistol = false, float rail = 0.f,
        float enter = 0.05f) {
        out.twoHand = true;
        out.offhandEnterMeters = enter;
        out.offhandUseBone = useBone;
        out.offhandPistol = pistol;
        out.offhandMetersX = gx;
        out.offhandMetersY = gy;
        out.offhandMetersZ = gz;
        out.offhandRailMeters = rail;
    };
    if (out.shotgun)
        // L_Wrist is the rear of the pump; rail reaches toward the barrel.
        setTwoHand(0.26f, 0.02f, 0.015f, true, false, 0.20f);
    else if (out.mp5)
        // Past the magwell, on the left of the handguard. +Y snapped the gun
        // into the support hand. L_Wrist on this model sits on the magazine.
        setTwoHand(0.24f, -0.07f, 0.02f, false, false, 0.10f);
    else if (out.scoped)
        // Rail along the stock + wider radius: a single point never latched
        // in play (2026-11-09 log: 0 of 23 squeezes, hand 1-4 cm off it or
        // further along the stock).
        setTwoHand(0.24f, 0.00f, 0.02f, true, false, 0.25f, 0.08f);
    else if (out.rpg)
        // Fallback point; the stock left hand's L_Wrist (just ahead of the
        // right hand, flat bottom) is used once sampled. Crossbow-like radius
        // + a rail forward from it (user: hard to grab, not repeatable).
        setTwoHand(0.22f, 0.00f, 0.025f, true, false, 0.10f, 0.08f);
    // Tau, gluon and hivehand: one hand only. Their fire / charge clips
    // shake the whole gun away from a support glove on the controller grip,
    // and holding them in idle lost the tau's cylinder spin and the
    // hivehand's muzzle flash (user 2026-11-18). The handle point was
    // setTwoHand(0.20f, 0.03f, 0.02f, true, true, 0.f, 0.07f) for tau/gluon
    // (snap mode, SampleViewmodelOffhandGrip), (0.16f, 0.02f, 0.02f) hivehand.
    else if (has("glock") || has("pistol") || has("9mm") || has("beretta")
        || has("357") || has("python") || has("revolver"))
        // Beside the gun hand, on the grip — not the monitor two-hand pose.
        setTwoHand(0.02f, -0.065f, 0.0f, false, true);
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
    const char* why = "";
    if (!m_EmptyHands && m_Game)
    {
        Game::InventoryWeapon first{};
        held = m_Game->CollectInventoryWeapons(&first, 1) > 0;
        // Weapons in the inventory but none in hand: after the intro tram
        // (bm_c1a2a, save loads) the game shows its own HEV hands viewmodel
        // (models/weapons/v_hands.mdl, 'admire1') until a weapon is picked.
        // Counted as held, the mod hid those arms as gun arms and drew no
        // glove: no right hand until R3 (capture-20261125-232648).
        if (held && !m_Game->GetActiveWeaponEntity())
        {
            held = false;
            why = " (no active weapon)";
        }
        if (held)
        {
            const char* vm = m_Game->GetEntityModelName(m_Game->GetViewModelEntity());
            if (vm && std::strstr(vm, "weapons/v_hands.mdl"))
            {
                held = false;
                why = " (hands viewmodel)";
            }
        }
    }
    if (held != m_HasHeldWeapon)
        Game::logMsg("VR held weapon %s%s", held ? "yes" : "no", why);
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
    ApplyTwoHandWeaponAim();
    // VR recoil: muzzle up around the gun hand (after the two-hand aim, so it
    // shows with both hands too; GetAimAngles' two-hand path reads this basis).
    if (const float kick = m_RecoilKickDeg.load(std::memory_order_relaxed); kick > 0.f)
    {
        // VectorRotate about Right with a positive angle = muzzle up
        // (headless: -kick pitched the two-hand aim down).
        m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, kick);
        m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, kick);
    }
}

void VR::ApplyTwoHandWeaponAim()
{
    // Caller holds m_ControllerMutex. Grab is left-grip only while the support
    // hand is on the model point — no proximity auto-latch. Long guns rotate
    // so that point stays under the support hand. Pistols snap the glove to
    // the left of the gun hand and keep right-controller aim.
    auto clearTwoHand = [&](const char* reason) {
        if (m_TwoHandActive)
            Game::logMsg("Two-hand off (%s)", reason ? reason : "?");
        m_TwoHandActive = false;
        m_TwoHandDrawValid = false;
        m_TwoHandSmoothValid = false;
        m_TwoHandHudValid = false;
    };

    // Support-grip press edge for TwoHandGripToggle (down at 0.35, re-armed
    // below 0.20). Tracked before the early returns below, or a release
    // during a menu / weapon switch / tracking loss went unseen and the next
    // squeeze did not count.
    const float gripVal = LeftSupportGripValue();
    const bool squeezing = gripVal >= 0.35f;
    const bool holdingSqueeze = gripVal >= 0.20f;
    const bool squeezeEdge = squeezing && !m_TwoHandGripWasDown;
    if (squeezing)
        m_TwoHandGripWasDown = true;
    else if (!holdingSqueeze)
        m_TwoHandGripWasDown = false;
    // Re-set below once the support hand's distance to the gun is known
    // (PhysicalLadderStep: a support grip on the gun is not a ladder grab).
    m_SupportNearGun.store(false, std::memory_order_release);

    if (!m_VmClass.twoHand || m_EmptyHands || !m_HasHeldWeapon || IsMenuUp() || WeaponMenuOpen())
    {
        clearTwoHand("weapon/menu");
        return;
    }

    const bool leftHanded = bmvr::g_LeftHanded;
    const bool offhandOk = leftHanded ? m_PhysicalRightTrackingValid : m_LeftControllerTrackingValid;
    if (!m_RightControllerTrackingValid || !offhandOk)
    {
        // A tracking blip while the grip is held (or latched) keeps the hold (last pose).
        if (m_TwoHandActive && (bmvr::g_TwoHandGripToggle
            || (bmvr::g_TwoHandHoldWhileGrip && holdingSqueeze)))
            return;
        clearTwoHand("tracking");
        return;
    }

    const Vector gunTracking = m_RightControllerPosAbs;
    const Vector offTracking = leftHanded ? m_PhysicalRightPosAbs : m_LeftControllerPosAbs;
    Vector d = offTracking - gunTracking;
    PivotYaw(d, m_RotationOffsetY.load(std::memory_order_acquire));

    float gx = 0.f, gy = 0.f, gz = 0.f;
    if (!ResolveOffhandGripLocal(gx, gy, gz))
    {
        clearTwoHand("no grip");
        return;
    }
    // Pistols latch beside the grip or under it (cupping, user 2026-11-18);
    // while held, distances are to the point that was taken.
    if (m_VmClass.offhandPistol && m_TwoHandActive)
    {
        gx = m_TwoHandGrabGx;
        gy = m_TwoHandGrabGy;
        gz = m_TwoHandGrabGz;
    }

    const Vector F0 = m_ViewmodelForward;
    const Vector R0 = m_ViewmodelRight;
    const Vector U0 = m_ViewmodelUp;
    const Vector grip = F0 * gx + R0 * gy + U0 * gz;
    const float distToGrip = (d - grip).Length();
    const float along = d.Dot(F0);
    const float latR = d.Dot(R0) - gy;
    const float latU = d.Dot(U0) - gz;
    const float lat = std::sqrt(latR * latR + latU * latU);
    const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
    const bool pistol = m_VmClass.offhandPistol;
    // TwoHandGripScale (panel "Grip area scale"): wider and longer toward the
    // muzzle; the rear end (alongMin, toward the gun hand) stays where it is,
    // so a bigger zone does not reach back over the magwell / gun hand.
    const float gripScale = std::clamp(bmvr::g_TwoHandGripScale, 0.5f, 2.f);
    const float enterR = (pistol ? 0.07f : m_VmClass.offhandEnterMeters) * gripScale * scale;
    // Hold zone stays a bit larger than the grab zone, or a big grab zone
    // would let go right after the squeeze.
    const float stayR = (std::max)((pistol ? 0.14f : 0.12f) * scale, enterR + 0.03f * scale);
    const float slop = 0.04f * scale;
    const float railHu = m_VmClass.offhandRailMeters * scale;
    const float alongMin = gx - slop;
    const float alongMax = gx + (std::max)(railHu, 0.04f * scale) * gripScale;
    // Support hand at the gun (a little wider than the grab zone): its grip
    // takes the gun, not a ladder rung behind it.
    m_SupportNearGun.store(m_TwoHandActive || distToGrip < stayR + slop
        || (!pistol && lat < stayR && along >= alongMin - slop && along <= alongMax + slop),
        std::memory_order_release);

    // Pistol: second grab spot under the grip (cupping hand). From the live
    // miss log (user 2026-11-18, "a tad too far forward"): the left
    // controller sat at ~(-3.6, -0.4, -5.4) units (fwd, right, up) from the
    // gun hand.
    const float underX = -0.071f * scale + (gx - m_VmClass.offhandMetersX * scale);
    const float underY = -0.01f * scale, underZ = -0.127f * scale;
    const float distUnder = pistol
        ? (d - (F0 * underX + R0 * underY + U0 * underZ)).Length() : 1e9f;
    const float underR = 0.08f * gripScale * scale;

    // One light buzz in the support hand when it reaches the spot where a
    // squeeze takes the gun (user 2026-11-22; like the MP5 bolt buzz).
    // Pistols include the under-grip spot (user 2026-11-23: no buzz there).
    {
        static bool s_wasInZone = false;
        const bool inZone = !m_TwoHandActive
            && (pistol ? (distToGrip < enterR || distUnder < underR)
                       : (lat < enterR && along >= alongMin && along <= alongMax));
        if (inZone && !s_wasInZone)
            PulseHandHaptic(leftHanded ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand, 600, 0.3f);
        s_wasInZone = inZone;
    }

    const bool was = m_TwoHandActive;
    if (m_TwoHandActive && bmvr::g_TwoHandGripToggle)
    {
        // Latched: a new squeeze lets go, and so does a support hand taken
        // well off the gun (else the aim would follow a hand that left it;
        // TwoHandToggleAutoRelease off = squeeze only, developer row).
        // Distance from the grip point held for 0.25 s: the old lateral test
        // against the posed model axis drifted to its 0.30 m limit while the
        // hand still held the shotgun's pump (2026-10-26 log: lat 11.8-12.9).
        static DWORD s_leaveSinceMs;
        const float leaveR = (pistol ? 0.20f : 0.45f) * scale;
        if (squeezeEdge)
        {
            m_TwoHandActive = false;
            s_leaveSinceMs = 0;
        }
        else if (bmvr::g_TwoHandToggleAutoRelease && distToGrip > leaveR)
        {
            const DWORD now = GetTickCount();
            if (s_leaveSinceMs == 0)
                s_leaveSinceMs = now;
            else if (now - s_leaveSinceMs >= 250)
            {
                m_TwoHandActive = false;
                s_leaveSinceMs = 0;
            }
        }
        else
            s_leaveSinceMs = 0;
    }
    else if (m_TwoHandActive)
    {
        if (pistol)
            m_TwoHandActive = holdingSqueeze && distToGrip <= stayR;
        else
            // TwoHandHoldWhileGrip: held for as long as the grip is (a
            // distance cap let go on tracking jumps with the grip held).
            m_TwoHandActive = bmvr::g_TwoHandHoldWhileGrip ? holdingSqueeze
                : (holdingSqueeze && lat <= stayR
                    && along >= (alongMin - slop) && along <= (alongMax + slop));
    }
    // Press edge in both modes: a grip held since a mag/shell feed (or any
    // squeeze made away from the gun) must not snap onto the gun when the
    // hand drifts into the zone (user 2026-11-22).
    else if (squeezeEdge
        && MagReload::g_State.load(std::memory_order_relaxed) != MagReload::kInHand
        // Shotgun: the attached support hand works the pump
        // (UpdateMagReload), so a squeeze on a pump that needs racking
        // attaches as usual instead of being a separate pump grab.
        && (MagReload::PumpFollowsSupportHand()
            || (!MagReload::g_BoltGrab.load(std::memory_order_relaxed)
                && !(MagReload::g_NeedRack.load(std::memory_order_relaxed) && MagReload::g_NearBolt.load(std::memory_order_relaxed)))))
    {
        bool onPoint = pistol
            ? (distToGrip < enterR)
            : (lat < enterR && along >= alongMin && along <= alongMax);
        // A squeeze where the old held pump grab caught the pump (its
        // lenient grab radius) attaches too.
        if (!onPoint && MagReload::PumpFollowsSupportHand()
            && MagReload::g_NeedRack.load(std::memory_order_relaxed)
            && MagReload::g_NearBolt.load(std::memory_order_relaxed))
        {
            onPoint = true;
            Game::logMsg("Two-hand shotgun: attached at the pump (lat %.1f)", lat);
        }
        if (pistol)
        {
            // Under the grip (cupping hand), see underX above.
            if (distUnder < underR && (!onPoint || distUnder < distToGrip))
            {
                onPoint = true;
                gx = underX;
                gy = underY;
                gz = underZ;
                Game::logMsg("Two-hand pistol: under the grip (dist %.1f)", distUnder);
            }
        }
        if (onPoint)
        {
            m_TwoHandActive = true;
            m_TwoHandGrabGx = gx;
            m_TwoHandGrabGy = gy;
            m_TwoHandGrabGz = gz;
            m_TwoHandSmoothValid = false;
            PulseHandHaptic(leftHanded
                ? vr::TrackedControllerRole_RightHand
                : vr::TrackedControllerRole_LeftHand, 2200, 0.55f);
        }
        else if (squeezeEdge)
        {
            static int s_missLog;
            if (s_missLog < 200)
            {
                Game::logMsg("Two-hand miss (%s) local=%.1f,%.1f,%.1f dist=%.1f lat=%.1f (enter %.1f) along=%.1f (%.1f..%.1f) d=%.1f,%.1f,%.1f grip=%.1f,%.1f,%.1f",
                    m_VmClass.numpadKey.c_str(), d.Dot(F0), d.Dot(R0), d.Dot(U0),
                    distToGrip, lat, enterR, along, alongMin, alongMax, d.x, d.y, d.z, grip.x, grip.y, grip.z);
                ++s_missLog;
            }
        }
    }
    if (m_TwoHandActive != was)
        Game::logMsg("Two-hand %s dist=%.1f lat=%.1f along=%.1f rail=%.1f grip=%.2f bone=%d gx=%.1f,%.1f,%.1f",
            m_TwoHandActive ? "on" : "off", distToGrip, lat, along, railHu, gripVal,
            m_OffhandGripBoneValid ? 1 : 0, gx, gy, gz);
    if (!m_TwoHandActive)
    {
        m_TwoHandDrawValid = false;
        m_TwoHandSmoothValid = false;
        m_TwoHandHudValid = false;
        return;
    }

    gx = m_TwoHandGrabGx;
    gy = m_TwoHandGrabGy;
    gz = m_TwoHandGrabGz;

    auto finishDraw = [&](const Vector& f, const Vector& r, const Vector& u, float gxSlide) {
        const float rotYaw = m_RotationOffsetY.load(std::memory_order_acquire);
        Vector local(gxSlide, gy, gz);
        Vector gripYawed = f * local.x + r * local.y + u * local.z;
        PivotYaw(gripYawed, -rotYaw);
        m_TwoHandDrawPos = gunTracking + gripYawed;
        m_TwoHandDrawAng = leftHanded ? m_PhysicalRightAngAbs : m_LeftControllerAngAbs;
        m_TwoHandDrawValid = true;
        m_TwoHandDrawPhysicalLeft = !leftHanded;
    };

    if (pistol)
    {
        // Pistols keep the gun-hand aim: the ammo panel follows that hand.
        m_TwoHandHudValid = false;
        finishDraw(F0, R0, U0, gx);
        return;
    }

    Vector F = d;
    if (VectorNormalize(F) <= 0.01f)
        return;
    auto rebuild = [&](Vector& f, Vector& r, Vector& u) -> bool {
        r = CrossProduct(f, U0);
        if (VectorNormalize(r) <= 0.01f)
            r = CrossProduct(f, Vector(0.f, 0.f, 1.f));
        if (VectorNormalize(r) <= 0.01f)
            return false;
        u = CrossProduct(r, f);
        VectorNormalize(u);
        return true;
    };
    Vector R, U;
    if (!rebuild(F, R, U))
        return;

    float gxSlide = d.Dot(F);
    const float gxMin = gx - slop;
    const float gxMax = gx + (std::max)(railHu, 0.04f * scale);
    if (gxSlide < gxMin)
        gxSlide = gxMin;
    if (gxSlide > gxMax)
        gxSlide = gxMax;

    Vector corrected = d - (R * gy + U * gz);
    if (VectorNormalize(corrected) > 0.01f)
        F = corrected;
    if (!rebuild(F, R, U))
        return;

    if (m_TwoHandSmoothValid)
    {
        const float alpha = 0.40f;
        F = m_TwoHandSmoothF * (1.f - alpha) + F * alpha;
        U = m_TwoHandSmoothU * (1.f - alpha) + U * alpha;
        if (VectorNormalize(F) <= 0.01f)
            return;
        if (!rebuild(F, R, U))
            return;
    }
    m_TwoHandSmoothF = F;
    m_TwoHandSmoothU = U;
    m_TwoHandSmoothValid = true;

    m_ViewmodelForward = F;
    m_ViewmodelRight = R;
    m_ViewmodelUp = U;
    m_TwoHandHudFrom[0] = F0;
    m_TwoHandHudFrom[1] = R0;
    m_TwoHandHudFrom[2] = U0;
    m_TwoHandHudTo[0] = F;
    m_TwoHandHudTo[1] = R;
    m_TwoHandHudTo[2] = U;
    {
        // The gun turns about its aim-pose point (gunTracking); the ammo
        // panel is placed from the gun hand's grip pose. Carry that lever
        // with the turn too, or the panel slid along the gun as the
        // two-hand aim swung left/right (user 2026-11-25). World = tracking
        // turned by the turn yaw, as `d` above.
        const Vector gunGrip = leftHanded ? m_LeftControllerPosAbs : m_PhysicalRightPosAbs;
        Vector lever = gunTracking - gunGrip;
        PivotYaw(lever, m_RotationOffsetY.load(std::memory_order_acquire));
        const Vector turned = F * lever.Dot(F0) + R * lever.Dot(R0) + U * lever.Dot(U0);
        m_TwoHandHudShift = lever - turned;
    }
    m_TwoHandHudValid = true;

    finishDraw(F, R, U, gxSlide);
}

// Grab zones (shoulders, hips, ammo pouch) in a body frame instead of the
// headset (user 2026-11-23: looking left/right moved the holsters). Body yaw:
// a head turn within the dead zone leaves it alone, a turn held past it for
// the catch-up delay turns it (until fully aligned, else it stays ~35 deg off),
// past the hard limit it is dragged along at once; walking catches up at once.
// While a hand reaches into a zone the yaw is frozen (except the hard limit)
// so the slot does not move away from the hand. Everything stays in tracking
// space like the poses, so snap/smooth turning needs nothing.
void VR::UpdateBodyFrame()
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_last{};
    static float s_offsetSec = 0.f;
    static bool s_catchingUp = false;
    static bool s_wasOn = false;
    static int s_logLines = 0;
    struct Reach { Clock::time_point entered{}; Clock::time_point left{}; bool in = false; };
    static Reach s_reachLeft, s_reachRight;

    const Clock::time_point now = Clock::now();
    float dt = s_last == Clock::time_point{} ? 0.f : std::chrono::duration<float>(now - s_last).count();
    s_last = now;
    dt = std::clamp(dt, 0.f, 0.1f);

    if (!m_HmdPoseValid)
    {
        m_BodyFrameValid = false;
        return;
    }
    const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
    const Vector up(0.f, 0.f, 1.f);
    auto setFrame = [&](float yawDeg, const Vector& origin) {
        const float r = DEG2RAD(yawDeg);
        m_ZoneFwd = Vector(std::cos(r), std::sin(r), 0.f);
        m_ZoneRight = CrossProduct(m_ZoneFwd, up);   // Source: x fwd, y left, z up
        m_ZoneOrigin = origin;
        m_BodyFrameValid = true;
    };

    if (!bmvr::g_GestureBodyFrame)
    {
        // Old head-locked zones: the headset and its flattened forward.
        Vector fwd = m_HmdForward;
        fwd.z = 0.f;
        if (VectorNormalize(fwd) <= 0.01f)
        {
            m_BodyFrameValid = false;
            return;
        }
        m_BodyYawDeg = m_BodyHeadYawDeg = RAD2DEG(std::atan2(fwd.y, fwd.x));
        setFrame(m_BodyYawDeg, m_HmdPosAbs);
        if (s_wasOn)
            Game::logMsg("Body frame: off, grab zones follow the headset");
        s_wasOn = false;
        s_catchingUp = false;
        return;
    }

    // Head yaw that still works looking down/up: looking down, the top of the
    // head points forward; looking up, the back of it does.
    Vector d = m_HmdForward - m_HmdUp * m_HmdForward.z;
    d.z = 0.f;
    if (d.Length2D() > 1e-3f)
        m_BodyHeadYawDeg = RAD2DEG(std::atan2(d.y, d.x));
    const float headYaw = m_BodyHeadYawDeg;
    if (!s_wasOn || !m_BodyFrameValid)
    {
        m_BodyYawDeg = headYaw;
        s_offsetSec = 0.f;
        s_catchingUp = false;
        Game::logMsg("Body frame: on (dead zone %.0f, hard limit %.0f, catch-up %.1f s at %.0f deg/s)",
            bmvr::g_BodyYawDeadzoneDeg, bmvr::g_BodyYawHardLimitDeg, bmvr::g_BodyYawCatchUpSec,
            bmvr::g_BodyYawCatchUpDegPerSec);
    }
    s_wasOn = true;

    // Holster lock, from last frame's zones: a hand that moved into a zone
    // (8 cm margin) freezes the yaw, and for 250 ms after it leaves. A hand
    // resting there (> 1.5 s, e.g. hanging at the hip) no longer counts, or
    // empty hands at the sides would pin the body for good.
    auto reach = [&](Reach& r, bool in) {
        if (in && !r.in)
            r.entered = now;
        if (!in && r.in)
            r.left = now;
        r.in = in;
        if (in)
            return now - r.entered < std::chrono::milliseconds(1500);
        return r.left != Clock::time_point{} && now - r.left < std::chrono::milliseconds(250)
            && r.left - r.entered < std::chrono::milliseconds(1500);
    };
    // Every shoulder/hip zone of that hand, set or not (the forehead is head-framed).
    auto nearZone = [&](bool valid, bool right) {
        if (!valid || !m_BodyFrameValid)
            return false;
        for (int z = 0; z < bmvr::kGestureWeaponZoneCount; ++z)
            if (right ? RightHandInSlotZone(z, 0.08f) : LeftHandInSlotZone(z, 0.08f))
                return true;
        return false;
    };
    const bool locked = reach(s_reachLeft, nearZone(m_LeftControllerTrackingValid, false))
        | reach(s_reachRight, nearZone(m_PhysicalRightTrackingValid && !m_TwoHandActive, true));

    auto deltaAngle = [](float from, float to) { return std::remainder(to - from, 360.f); };
    float delta = deltaAngle(m_BodyYawDeg, headYaw);
    const float hardLimit = bmvr::g_BodyYawHardLimitDeg;
    if (std::fabs(delta) > hardLimit)   // also while locked: turning around still works
    {
        m_BodyYawDeg += (delta > 0.f ? 1.f : -1.f) * (std::fabs(delta) - hardLimit);
        delta = deltaAngle(m_BodyYawDeg, headYaw);
    }
    if (!locked)
    {
        const bool walking = std::fabs(m_WalkForward.load(std::memory_order_acquire)) > 0.2f
            || std::fabs(m_WalkSide.load(std::memory_order_acquire)) > 0.2f;
        const float delay = walking ? 0.f : bmvr::g_BodyYawCatchUpSec;
        const float absDelta = std::fabs(delta);
        s_offsetSec = absDelta > bmvr::g_BodyYawDeadzoneDeg ? s_offsetSec + dt : 0.f;
        if (!s_catchingUp && absDelta > bmvr::g_BodyYawDeadzoneDeg && s_offsetSec >= delay)
        {
            s_catchingUp = true;
            if (s_logLines < 40)
            {
                ++s_logLines;
                Game::logMsg("Body frame: catching up %+.0f deg%s", delta, walking ? " (walking)" : "");
            }
        }
        if (s_catchingUp)
        {
            const float step = bmvr::g_BodyYawCatchUpDegPerSec * dt;
            m_BodyYawDeg += std::clamp(delta, -step, step);
            if (std::fabs(deltaAngle(m_BodyYawDeg, headYaw)) < 5.f)
                s_catchingUp = false;   // keep going until aligned, not just back in the dead zone
        }
    }
    m_BodyYawDeg = std::remainder(m_BodyYawDeg, 360.f);

    // Neck pivot ~10 cm below and 9 cm behind the eyes (head frame), so head
    // pitch/roll no longer swings the zones. The zone limits were tuned from
    // the eyes looking straight ahead, so the origin is where the eyes would be
    // with the head straight over the neck, facing the body yaw.
    const Vector neck = m_HmdPosAbs - m_HmdForward * (0.09f * scale) - m_HmdUp * (0.10f * scale);
    const float r = DEG2RAD(m_BodyYawDeg);
    const Vector bodyFwd(std::cos(r), std::sin(r), 0.f);
    setFrame(m_BodyYawDeg, neck + bodyFwd * (0.09f * scale) + up * (0.10f * scale));
}

void VR::ZoneCoords(const Vector& hand, float& along, float& lateral, float& height) const
{
    const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
    const Vector d = hand - m_ZoneOrigin;
    along = d.Dot(m_ZoneFwd) / scale;
    lateral = d.Dot(m_ZoneRight) / scale;   // + = right
    height = d.z / scale;
}

// Zone boxes in metres from the (body-frame) eye point: along = ahead,
// lateral = toward that zone's side, height = up. growM widens every side
// (holster lock margin).
bool VR::HandInGestureZone(const Vector& hand, GestureZone zone, float growM) const
{
    if (!m_BodyFrameValid)
        return false;
    float along, lateral, height;
    ZoneCoords(hand, along, lateral, height);
    struct Box { float side, back, front, lo, hi, latLo, latHi; };
    Box b{};
    switch (zone)
    {
    // Behind the head (the HMD sits ~10 cm ahead of the neck), above chest
    // height, on that hand's side. Closer than the first try (2026-10-15:
    // crowbar a reach too far): level with the eyes counts. Not below the
    // shoulder (~20 cm under the eyes): down to -0.38 a shotgun stock held in
    // close at chest height took the crowbar on the grip squeeze (2026-10-26).
    case GestureZone::ShoulderLeft:  b = { -1.f, -10.f, 0.04f, -0.20f, 0.30f, 0.05f, 0.50f }; break;
    case GestureZone::ShoulderRight: b = { 1.f, -10.f, 0.04f, -0.20f, 0.30f, 0.05f, 0.50f }; break;
    // Pistol zone 18 cm further back and 8 cm lower than the first try
    // (2026-11-14: 5-10 cm too far forward and too high; 2026-11-16 headset:
    // still 10 cm too far forward).
    case GestureZone::HipRight:      b = { 1.f, -0.43f, 0.02f, -1.03f, -0.53f, 0.10f, 0.45f }; break;
    // Right hand, a bit above the left hip (cross draw).
    case GestureZone::HipCrossLeft:  b = { -1.f, -0.25f, 0.20f, -0.80f, -0.35f, 0.00f, 0.40f }; break;
    // Left hand: mirror of the pistol zone (manual-reload ammo).
    case GestureZone::HipAmmo:       b = { -1.f, -0.43f, 0.10f, -1.03f, -0.45f, 0.05f, 0.45f }; break;
    // Left hand, ammo grab at either hip (belt front to hip side; user
    // 2026-11-24: the HipAmmo box ended 10 cm ahead of the eyes, a belt-front
    // grab at ~0.35 m ahead, 0.35-0.4 m down missed; 0.45 / -0.25 was far too
    // big, a grab from the whole belly: 0.25 / -0.40 still 5-10 cm too far forward and high: now 0.16 ahead, down from -0.48).
    // Gun holsters unchanged.
    case GestureZone::AmmoWideLeft:  b = { -1.f, -0.43f, 0.16f, -1.03f, -0.48f, 0.10f, 0.45f }; break;
    case GestureZone::AmmoWideRight: b = { 1.f, -0.43f, 0.16f, -1.03f, -0.48f, 0.10f, 0.45f }; break;
    // Left hand, a bit above the right hip: mirror of the cross draw.
    case GestureZone::HipCrossRight: b = { 1.f, -0.25f, 0.20f, -0.80f, -0.35f, 0.00f, 0.40f }; break;
    }
    lateral *= b.side;
    return along > b.back - growM && along < b.front + growM && height > b.lo - growM && height < b.hi + growM
        && lateral > b.latLo - growM && lateral < b.latHi + growM;
}

bool VR::LeftHandInSlotZone(int zone, float growM) const
{
    switch (zone)
    {
    case bmvr::kGestureShoulderLeft:  return HandInGestureZone(m_LeftControllerPosAbs, GestureZone::ShoulderLeft, growM);
    case bmvr::kGestureShoulderRight: return HandInGestureZone(m_LeftControllerPosAbs, GestureZone::ShoulderRight, growM);
    // A slot set to Ammo gets the wide ammo box; other actions keep theirs.
    case bmvr::kGestureHipLeft:
        return HandInGestureZone(m_LeftControllerPosAbs, static_cast<int>(std::lround(bmvr::g_GestureLeft[zone])) == bmvr::kGestureAmmo
            ? GestureZone::AmmoWideLeft : GestureZone::HipAmmo, growM);
    case bmvr::kGestureHipRight:
        return HandInGestureZone(m_LeftControllerPosAbs, static_cast<int>(std::lround(bmvr::g_GestureLeft[zone])) == bmvr::kGestureAmmo
            ? GestureZone::AmmoWideRight : GestureZone::HipCrossRight, growM);
    case bmvr::kGestureHead:
    {
        // Forehead: just ahead of the eyes (the headset front is ~8 cm ahead
        // of them), from just below eye level up, within 25 cm of the middle.
        // Reaches 40 cm ahead and 15 cm down so the hand need not hit the
        // headset (user 2026-11-22). Starts where the shoulder zones end
        // (along 0.04), so they never overlap. Head frame (the headset's
        // flattened forward), not the body frame.
        if (!m_HmdPoseValid)
            return false;
        Vector fwd = m_HmdForward;
        fwd.z = 0.f;
        if (VectorNormalize(fwd) <= 0.01f)
            return false;
        const Vector right = CrossProduct(fwd, Vector(0.f, 0.f, 1.f));   // Source: x fwd, y left, z up
        const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
        const Vector d = m_LeftControllerPosAbs - m_HmdPosAbs;
        const float along = d.Dot(fwd) / scale;
        const float lateral = d.Dot(right) / scale;
        const float height = d.z / scale;
        return along > 0.04f - growM && along < 0.40f + growM && std::fabs(lateral) < 0.25f + growM
            && height > -0.15f - growM && height < 0.25f + growM;
    }
    }
    return false;
}

bool VR::RightHandInSlotZone(int zone, float growM) const
{
    switch (zone)
    {
    case bmvr::kGestureShoulderLeft:  return HandInGestureZone(m_PhysicalRightPosAbs, GestureZone::ShoulderLeft, growM);
    case bmvr::kGestureShoulderRight: return HandInGestureZone(m_PhysicalRightPosAbs, GestureZone::ShoulderRight, growM);
    case bmvr::kGestureHipLeft:       return HandInGestureZone(m_PhysicalRightPosAbs, GestureZone::HipCrossLeft, growM);
    case bmvr::kGestureHipRight:      return HandInGestureZone(m_PhysicalRightPosAbs, GestureZone::HipRight, growM);
    }
    return false;
}

bool VR::LeftHandInAmmoZone() const
{
    for (int z = 0; z < bmvr::kGestureZoneCount; ++z)
        if (static_cast<int>(std::lround(bmvr::g_GestureLeft[z])) == bmvr::kGestureAmmo && LeftHandInSlotZone(z))
            return true;
    return false;
}

bool VR::UpdateShoulderGestures(bool paused)
{
    // Grab zones, a slot per hand (user 2026-11-23; bmvr_flags.h
    // GestureZoneId). The left hand does an action, the right hand takes a
    // weapon. Returns true while the right hand is in one of its zones, so
    // the right grip does not also toggle the flashlight there.
    struct Zone { DWORD enteredMs = 0; DWORD holdStartMs = 0; DWORD pulseMs = 0; bool armed = false; bool gripDown = true; };
    static Zone s_left[bmvr::kGestureZoneCount], s_right[bmvr::kGestureWeaponZoneCount];
    static DWORD s_rightLeftMs;
    auto leftAction = [](int z) { return static_cast<int>(std::lround(bmvr::g_GestureLeft[z])); };
    auto rightSetting = [](int z) { return static_cast<int>(std::lround(bmvr::g_GestureRight[z])); };
    // Ammo counts only while manual reload is on: the left hip defaults to
    // Ammo, and must not buzz or swallow the grip for everyone else.
    const bool ammoOn = bmvr::PhysicalWeapon(bmvr::g_VrMagReload);
    auto leftOn = [&](int z) {
        const int a = leftAction(z);
        return a >= 0 && (a != bmvr::kGestureAmmo || ammoOn);
    };
    bool anyOn = false;
    for (int z = 0; z < bmvr::kGestureZoneCount; ++z)
        anyOn = anyOn || leftOn(z);
    for (int z = 0; z < bmvr::kGestureWeaponZoneCount; ++z)
        anyOn = anyOn || rightSetting(z) >= 0;
    const bool active = anyOn && m_GameplayEligible && !paused && !MenuGameplayBlocked()
        && !m_WeaponMenuOpen && !m_SettingsOpen && m_HmdPoseValid;
    if (!active)
    {
        for (Zone& z : s_left)
            z = Zone{};
        for (Zone& z : s_right)
            z = Zone{};
        m_GestureZoneLeft = false;
        return false;
    }
    auto gripValue = [&](vr::VRActionHandle_t h) {
        vr::InputAnalogActionData_t a{};
        if (GetAnalogActionData(h, a))
            return a.x;
        return PressedDigitalAction(h) ? 1.f : 0.f;
    };
    // Returns true on the grip's press edge while armed. holdMs > 0: the
    // grip must then stay down that long (rising buzz); let go = cancelled.
    auto step = [&](Zone& z, bool in, float grip, vr::ETrackedControllerRole role, DWORD dwellMs, DWORD holdMs) {
        const DWORD now = GetTickCount();
        if (!in)
        {
            z = Zone{};
            z.gripDown = grip >= 0.3f;
            return false;
        }
        if (z.enteredMs == 0)
            z.enteredMs = now;
        if (!z.armed && now - z.enteredMs >= dwellMs && !z.gripDown)
        {
            z.armed = true;
            PulseHandHaptic(role, 1200, 0.3f);
        }
        if (grip >= 0.6f && !z.gripDown)
        {
            z.gripDown = true;
            if (!z.armed)
                return false;
            if (holdMs == 0)
                return true;
            z.holdStartMs = now ? now : 1;
            z.pulseMs = now;
            return false;
        }
        if (grip <= 0.3f)
        {
            z.gripDown = false;
            z.holdStartMs = 0;
            return false;
        }
        if (z.holdStartMs)
        {
            const DWORD held = now - z.holdStartMs;
            if (held >= holdMs)
            {
                z.holdStartMs = 0;
                return true;
            }
            if (now - z.pulseMs >= 100)
            {
                z.pulseMs = now;
                const float t = static_cast<float>(held) / static_cast<float>(holdMs);
                PulseHandHaptic(role, static_cast<unsigned short>(500 + 2500 * t), 0.15f + 0.5f * t);
            }
        }
        return false;
    };

    // ---- Left hand: actions ----
    // Not while both hands hold the gun: that grip squeeze is the gun's.
    // Every slot arms after resting there ~0.25 s (a light buzz), so a fast
    // swing past does nothing. Quick load also needs a ~1 s grip hold.
    const float leftGrip = gripValue(m_ActionInventoryGripLeft);
    bool leftAny = false;
    for (int z = 0; z < bmvr::kGestureZoneCount; ++z)
    {
        const int a = leftAction(z);
        // Forehead: not on / at a ladder. Hand over hand, the left hand
        // passes the forehead on every reach (user 2026-11-24: screenshots
        // while climbing), and its grip there is a rung grab.
        const bool ladderBlocked = z == bmvr::kGestureHead
            && (m_LadderActive.load(std::memory_order_relaxed) || m_LadderView.active
                || m_LadderPin[0].valid || m_LadderPin[1].valid);
        const bool in = leftOn(z) && m_LeftControllerTrackingValid && !m_TwoHandActive && !ladderBlocked && LeftHandInSlotZone(z);
        leftAny = leftAny || in;
        // Ammo: no gesture, the manual-reload grab takes ammo there
        // (LeftHandInAmmoZone); the zone keeps the grip out of its mapped
        // action (m_GestureZoneLeft) and gives the same arming buzz as the
        // other left zones (user 2026-11-24: none at the ammo zone).
        if (a == bmvr::kGestureAmmo)
        {
            // Only while a grip there would take ammo (user 2026-11-24:
            // it buzzed with nothing to grab).
            step(s_left[z], in && m_AmmoGrabReady, leftGrip, vr::TrackedControllerRole_LeftHand, 250, 0);
            continue;
        }
        if (!step(s_left[z], in, leftGrip, vr::TrackedControllerRole_LeftHand, 250, a == bmvr::kGestureQuickLoad ? 1000 : 0))
            continue;
        PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 3000, 0.8f);
        switch (a)
        {
        case bmvr::kGestureQuickSave:
            m_PendingQuickSave.store(true, std::memory_order_release);
            break;
        case bmvr::kGestureFlashlight:
            m_PendingImpulse.store(100, std::memory_order_release);
            break;
        case bmvr::kGestureQuickLoad:
            m_PendingQuickLoad.store(true, std::memory_order_release);
            break;
        case bmvr::kGestureScreenshot:
        {
            // A moment later, so the hand is out of view (the forehead!).
            const unsigned long at = GetTickCount() + 800;
            bmvr::g_SteamScreenshotAt = at ? at : 1;
            break;
        }
        case bmvr::kGestureRecenter:
            Recenter("gesture");
            break;
        }
        Game::logMsg("Gesture: left hand at the %s -> %s", bmvr::GestureZoneName(z), bmvr::GestureActionName(a));
    }
    m_GestureZoneLeft = leftAny;

    // ---- Right hand: weapons ----
    // A weapon zone counts only while that weapon is carried (user
    // 2026-11-19: the pistol hip buzzed in the intro, before the pistol):
    // otherwise no buzz, and the grip keeps its own action there. The
    // inventory decides: m_LastViewmodelModel keeps the last gun's name after
    // a new game or a load without weapons (user 2026-11-23: zones buzzed
    // with 0 weapons), so it only counts while something is carried.
    Game::InventoryWeapon inv[32]{};
    const int invN = m_Game ? m_Game->CollectInventoryWeapons(inv, 32) : 0;
    auto matchesParts = [](const char* nm, const char* const* parts) {
        if (!nm)
            return false;
        for (const char* const* part = parts; *part; ++part)
            if (strstr(nm, *part))
                return true;
        return false;
    };
    auto carries = [&](int w) {
        if (w < 0 || invN <= 0)
            return false;
        const char* const* parts = bmvr::GestureWeaponParts(w);
        if (!m_LastViewmodelModel.empty() && matchesParts(m_LastViewmodelModel.c_str(), parts))
            return true;
        for (int i = 0; i < invN; ++i)
            if (inv[i].entityIndex > 0 && (matchesParts(inv[i].networkName, parts) || matchesParts(inv[i].modelName, parts)))
                return true;
        return false;
    };
    // Dynamic zones (user 2026-11-23, a choice per zone): the grip there
    // while holding another weapon puts that one away in it. heldW = the
    // weapon in the hand as a zone weapon index (-1 = empty hands or one no
    // zone can take). A weapon that some fixed zone is set to is never
    // stored (option B): that zone then just draws its own weapon.
    auto isDynamic = [&](int z) { return rightSetting(z) == bmvr::kGestureDynamic; };
    bool anyDynamic = false;
    for (int z = 0; z < bmvr::kGestureWeaponZoneCount; ++z)
        anyDynamic = anyDynamic || isDynamic(z);
    int heldW = -1;
    if (anyDynamic && !m_EmptyHands && invN > 0 && !m_LastViewmodelModel.empty())
        for (int w = 0; w < bmvr::kGestureWeaponCount && heldW < 0; ++w)
            if (matchesParts(m_LastViewmodelModel.c_str(), bmvr::GestureWeaponParts(w)))
                heldW = w;
    for (int z = 0; z < bmvr::kGestureWeaponZoneCount && heldW >= 0; ++z)
        if (rightSetting(z) == heldW)
            heldW = -1;   // fixed home elsewhere
    auto storesHere = [&](int z) { return heldW >= 0 && isDynamic(z) && bmvr::GestureWeaponAt(z) != heldW; };
    auto zoneLive = [&](int z) { return carries(bmvr::GestureWeaponAt(z)) || storesHere(z); };
    // The zone's stored weapon becomes the held one, saved to config.txt; a
    // Dynamic zone that held it before gets this zone's old one (swap), so
    // every weapon keeps one place. Then empty hands, as the holster in its
    // own zone.
    auto storeHeld = [&](int z) {
        const int oldW = bmvr::GestureWeaponAt(z);
        std::vector<std::pair<std::string, std::string>> kv;
        int swapped = -1;
        for (int o = 0; o < bmvr::kGestureWeaponZoneCount; ++o)
        {
            if (o == z || !isDynamic(o) || bmvr::GestureWeaponAt(o) != heldW)
                continue;
            bmvr::g_GestureDyn[o] = static_cast<float>(oldW);
            kv.emplace_back(bmvr::GestureDynKey(o), std::to_string(oldW));
            swapped = o;
        }
        bmvr::g_GestureDyn[z] = static_cast<float>(heldW);
        kv.emplace_back(bmvr::GestureDynKey(z), std::to_string(heldW));
        const bool ok = bmvr::WriteVrConfigValues(kv, {});
        ToggleEmptyHands();
        PulseHandHaptic(vr::TrackedControllerRole_RightHand, 3000, 0.8f);
        Game::logMsg("Dynamic holster: %s put away at the %s (was %s%s%s), config %s",
            bmvr::GestureWeaponName(heldW), bmvr::GestureZoneName(z), oldW >= 0 ? bmvr::GestureWeaponName(oldW) : "empty",
            swapped >= 0 ? ", that weapon moves to the " : "", swapped >= 0 ? bmvr::GestureZoneName(swapped) : "",
            ok ? "saved" : "SAVE FAILED");
    };
    // Selects the first inventory weapon whose network or model name has one
    // of the given parts. Already held = nothing to do (still counts).
    auto grabWeapon = [&](const char* label, const char* const* parts) {
        if (!m_Game)
            return;
        if (!m_LastViewmodelModel.empty() && matchesParts(m_LastViewmodelModel.c_str(), parts))
        {
            // Empty hands keep the game's active weapon (holstering never
            // switches it), so a grab of that same weapon must just show it
            // again: before, the pistol grab did nothing with empty hands.
            if (m_EmptyHands)
            {
                m_EmptyHands = false;
                PulseHandHaptic(vr::TrackedControllerRole_RightHand, 3000, 0.8f);
                Game::logMsg("Grab gesture: %s already active, hands -> weapon", label);
                return;
            }
            // The grip in the zone of the weapon in the hand puts it back
            // there, like the right-stick tap (that tap then draws it
            // again). Always on since 2026-11-24 (was the VrHolsterBack
            // toggle, off by default; user: the toggle bought nothing and
            // made Dynamic zones awkward).
            ToggleEmptyHands();
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 3000, 0.8f);
            Game::logMsg("Holster gesture: %s put back, empty hands", label);
            return;
        }
        for (int i = 0; i < invN; ++i)
        {
            if (inv[i].entityIndex <= 0 || !(matchesParts(inv[i].networkName, parts) || matchesParts(inv[i].modelName, parts)))
                continue;
            m_EmptyHands = false;
            m_PendingWeaponSelect.store(inv[i].entityIndex, std::memory_order_release);
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 3000, 0.8f);
            float along, lateral, height;
            ZoneCoords(m_PhysicalRightPosAbs, along, lateral, height);
            Game::logMsg("Grab gesture: %s entity=%d (hand %.2f m ahead, %.2f m up, %.2f m right of the %s; head %+.0f deg off the body)",
                label, inv[i].entityIndex, along, height, lateral, bmvr::g_GestureBodyFrame ? "body" : "head",
                std::remainder(m_BodyHeadYawDeg - m_BodyYawDeg, 360.f));
            return;
        }
        Game::logMsg("Grab gesture: %s not in the inventory", label);
    };
    // Not while both hands hold the gun. Weapon zones arm at once (a buzz on
    // entry): the grip squeeze is needed anyway, and a wait lost hurried grabs.
    const bool rightOk = m_PhysicalRightTrackingValid && !m_TwoHandActive;
    const float rightGrip = gripValue(m_ActionFlashlight);
    bool anyRightIn = false;
    for (int z = 0; z < bmvr::kGestureWeaponZoneCount; ++z)
    {
        const bool in = rightOk && rightSetting(z) >= 0 && RightHandInSlotZone(z) && zoneLive(z);
        anyRightIn = anyRightIn || in;
        if (!step(s_right[z], in, rightGrip, vr::TrackedControllerRole_RightHand, 0, 0))
            continue;
        const int w = bmvr::GestureWeaponAt(z);
        if (storesHere(z))
            storeHeld(z);
        else if (w >= 0)
            grabWeapon(bmvr::GestureWeaponName(w), bmvr::GestureWeaponParts(w));
    }

    // Also for 1 s after leaving a zone, so the grip squeeze that took the
    // weapon cannot toggle the flashlight on the way forward.
    if (anyRightIn)
        s_rightLeftMs = GetTickCount();
    return anyRightIn || (s_rightLeftMs && GetTickCount() - s_rightLeftMs < 1000);
}

float VR::LeftSupportGripValue() const
{
    if (bmvr::g_LeftHanded)
    {
        float curls[5]{};
        if (GetFingerCurls(m_ActionSkeletonRight, curls))
            return (std::max)(curls[2], (std::max)(curls[3], curls[4]));
        return 0.f;
    }
    vr::InputAnalogActionData_t analog{};
    if (GetAnalogActionData(m_ActionInventoryGripLeft, analog))
        return std::clamp(analog.x, 0.f, 1.f);
    float curls[5]{};
    if (GetFingerCurls(m_ActionSkeletonLeft, curls))
        return (std::max)(curls[2], (std::max)(curls[3], curls[4]));
    return 0.f;
}

namespace vm_numpad { float SupportForwardCm(const std::string& key); }

bool VR::ResolveOffhandGripLocal(float& gx, float& gy, float& gz) const
{
    // Per-weapon support-hand trim along the gun (VR panel DEBUG row,
    // VR/viewmodel_offsets.txt); moves the grab zone and the glove with it.
    const float scaleU = m_VRScale > 1.f ? m_VRScale : 39.37f;
    const float trim = vm_numpad::SupportForwardCm(m_VmClass.numpadKey) * 0.01f * scaleU;
    if (m_OffhandGripBoneValid && m_VmClass.offhandUseBone)
    {
        gx = m_OffhandGripGx + trim;
        gy = m_OffhandGripGy;
        gz = m_OffhandGripGz;
        return true;
    }
    if (!m_VmClass.twoHand)
        return false;
    const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
    float vis = 1.f;
    if (!m_VmClass.offhandPistol)
    {
        vis = (m_VmClass.fixedScale > 0.f) ? m_VmClass.fixedScale : bmvr::g_ViewmodelScale;
        vis *= m_VmClass.scaleMul;
        if (vis < 0.2f)
            vis = 0.2f;
        if (vis > 1.5f)
            vis = 1.5f;
    }
    gx = m_VmClass.offhandMetersX * scale * vis + trim;
    gy = m_VmClass.offhandMetersY * scale * vis;
    gz = m_VmClass.offhandMetersZ * scale * vis;
    if (m_VmClass.offhandPistol)
        return std::fabs(gy) > 0.3f || gx > 0.3f;
    return gx > 0.5f;
}

void VR::NoteViewmodelOffhandWorld(const Vector& worldOrigin, bool sideHandle)
{
    if (!m_ControllerPoseValid || !m_VmClass.twoHand || !m_VmClass.offhandUseBone)
        return;
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    const Vector gunW = GetRightControllerAbsPos(body);
    const Vector rel = worldOrigin - gunW;
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (m_TwoHandActive)
        return;
    const float gx = rel.Dot(m_ViewmodelForward);
    const float gy = rel.Dot(m_ViewmodelRight);
    const float gz = rel.Dot(m_ViewmodelUp);
    const float scale = m_VRScale > 1.f ? m_VRScale : 39.37f;
    // A side handle (gluon) sits beside the gun hand, not ahead of it: only
    // a sanity distance applies.
    // RPG (user 2026-11-18): its stock left hand holds the flat bottom just
    // in front of the right hand, closer than 8 cm, so it was always
    // rejected and the latch used the fixed point 15 cm out on the tube.
    const float minFwd = (m_VmClass.rpg ? 0.02f : 0.08f) * scale;
    const bool outside = sideHandle
        ? (gx * gx + gy * gy + gz * gz > (0.45f * scale) * (0.45f * scale))
        : (gx < minFwd || gx > 0.55f * scale
            || std::fabs(gy) > 0.18f * scale || std::fabs(gz) > 0.18f * scale);
    if (outside)
    {
        // Rate-limited, not capped: a cap of 8 was used up by one weapon.
        static DWORD s_rejectLogMs;
        const DWORD nowMs = GetTickCount();
        if (nowMs - s_rejectLogMs > 5000)
        {
            s_rejectLogMs = nowMs;
            Game::logMsg("Off-hand grip bone rejected (%s) gx,gy,gz=(%.2f,%.2f,%.2f) m (fwd %.2f..0.55, side/up within 0.18)",
                m_VmClass.numpadKey.c_str(), gx / scale, gy / scale, gz / scale, minFwd / scale);
        }
        return;
    }
    const bool changed = !m_OffhandGripBoneValid
        || std::fabs(m_OffhandGripGx - gx) > 0.4f
        || std::fabs(m_OffhandGripGy - gy) > 0.4f
        || std::fabs(m_OffhandGripGz - gz) > 0.4f;
    m_OffhandGripGx = gx;
    m_OffhandGripGy = gy;
    m_OffhandGripGz = gz;
    m_OffhandGripBoneValid = true;
    static DWORD s_offhandLogMs;
    if (changed && GetTickCount() - s_offhandLogMs > 2000)
    {
        s_offhandLogMs = GetTickCount();
        Game::logMsg("Off-hand grip from %s (%s) gx,gy,gz=(%.2f,%.2f,%.2f) hu", sideHandle ? "side handle" : "L_Wrist",
            m_VmClass.numpadKey.c_str(), gx, gy, gz);
    }
}

bool VR::TryGetTwoHandOffhandDrawPose(bool physicalLeft, Vector& trackingPos, QAngle& trackingAng) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (!m_TwoHandActive || !m_TwoHandDrawValid)
        return false;
    if (physicalLeft != m_TwoHandDrawPhysicalLeft)
        return false;
    trackingPos = m_TwoHandDrawPos;
    trackingAng = m_TwoHandDrawAng;
    return true;
}

bool VR::TwoHandGunHudAngles(const QAngle& gunCtrlAng, QAngle& out, Vector& shift) const
{
    // Ammo panel while a long gun is held with both hands (user 2026-11-25):
    // locked to the gun's aim, not the gun-hand controller. The controller
    // axes are expressed in the one-hand gun basis and rebuilt in the
    // two-hand one, so the per-weapon placement (VR/ammo_hud.txt) keeps its
    // meaning and nothing moves at the moment the support hand grabs.
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (!m_TwoHandActive || !m_TwoHandHudValid || bmvr::g_TwoHandAmmoHudFollowsHand)
        return false;
    Vector f, r, u;
    QAngle::AngleVectors(gunCtrlAng, &f, &r, &u);
    auto remap = [&](const Vector& v) {
        return m_TwoHandHudTo[0] * v.Dot(m_TwoHandHudFrom[0])
            + m_TwoHandHudTo[1] * v.Dot(m_TwoHandHudFrom[1])
            + m_TwoHandHudTo[2] * v.Dot(m_TwoHandHudFrom[2]);
    };
    Vector nf = remap(f), nu = remap(u);
    if (VectorNormalize(nf) <= 0.01f || VectorNormalize(nu) <= 0.01f)
        return false;
    QAngle::VectorAngles(nf, nu, out);
    out.y = WrapYaw(out.y);
    shift = m_TwoHandHudShift;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

bool VR::TryGetLadderPinDrawPos(bool physicalLeft, const Vector& body, Vector& trackingPos) const
{
    // Hand on a rung: draw the glove there. The world shift from the
    // controller to the rung, turned back into tracking space (tracking
    // deltas map 1:1 onto the world, rotated by the turn yaw).
    const LadderPin& pin = m_LadderPin[physicalLeft ? 0 : 1];
    if (!pin.valid || !bmvr::g_PhysicalLadders)
        return false;
    const Vector handWorld = ControllerTrackingToWorld(body, trackingPos);
    Vector d = pin.world - handWorld;
    if (d.LengthSqr() > 32.f * 32.f)
        return false;
    PivotYaw(d, -m_RotationOffsetY.load(std::memory_order_acquire));
    trackingPos += d;
    return true;
}

namespace ladder_tune
{
    // Per ladder model: the wrapped glove's grip point moved out (toward the
    // player) / up, cm, and its finger curl. VR/ladder_grip.txt
    // ("model out_cm up_cm curl"); a model not in it = 0 0 g_LadderGripCurl.
    struct Grip
    {
        float out = 0.f, up = 0.f, curl = -1.f;   // curl < 0 = g_LadderGripCurl
    };
    std::mutex g_Lock;
    std::map<std::string, Grip> g_Grip;
    bool g_Loaded = false;
    std::atomic<int> g_HeldModel{ -1 };

    std::string Path() { return bmvr::VrFilePath("ladder_grip.txt"); }

    void Load()
    {
        if (g_Loaded)
            return;
        g_Loaded = true;
        // Built-in tuning (the user's headset values); the file overrides.
        g_Grip["props_c17/metalladder001.mdl"] = Grip{ 1.f, -2.f, 0.15f };   // bm_c1a0b test chamber, 2026-11-20
        std::ifstream in(Path());
        std::string line;
        int n = 0;
        while (in && std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            char key[128]{};
            Grip g;
            if (sscanf_s(line.c_str(), "%127s %f %f %f", key, static_cast<unsigned>(sizeof(key)), &g.out, &g.up, &g.curl) >= 3)
            {
                g_Grip[key] = g;
                ++n;
            }
        }
        Game::logMsg("Ladder grip tuning: %d ladder models from %s", n, Path().c_str());
    }

    void Save()
    {
        std::ofstream out(Path(), std::ios::trunc);
        if (!out)
            return;
        out << "# Ladder grip tuning (VR panel DEBUG rows), per ladder model below models/:\n";
        out << "# model out_cm(toward the player) up_cm curl(0..1, -1 = default)\n";
        for (const auto& kv : g_Grip)
            out << kv.first << " " << kv.second.out << " " << kv.second.up << " " << kv.second.curl << "\n";
    }

    void NoteHeld(int model)
    {
        if (model >= 0)
            g_HeldModel.store(model, std::memory_order_relaxed);
    }

    Grip For(int model)
    {
        const char* name = bmvr::ladders::ModelName(model);
        std::lock_guard<std::mutex> lock(g_Lock);
        Load();
        auto it = name ? g_Grip.find(name) : g_Grip.end();
        return it != g_Grip.end() ? it->second : Grip{};
    }
}

bool VR::LadderRungGrip(bool physicalLeft, Vector& point, Vector& axis, float* curl) const
{
    const LadderPin& pin = m_LadderPin[physicalLeft ? 0 : 1];
    if (!pin.valid || !pin.hasAxis || !bmvr::g_PhysicalLadders || !bmvr::g_LadderHandWrap)
        return false;
    const ladder_tune::Grip g = ladder_tune::For(pin.model);
    const float u = (m_VRScale > 1.f ? m_VRScale : 39.37f) * 0.01f;   // units per cm
    point = pin.rung - pin.face * (g.out * u) + Vector(0.f, 0.f, g.up * u);
    axis = pin.axis;
    if (curl)
        *curl = g.curl >= 0.f ? g.curl : bmvr::g_LadderGripCurl;
    return true;
}

void VR::SyncLadderGripRows()
{
    const int model = ladder_tune::g_HeldModel.load(std::memory_order_relaxed);
    const char* name = bmvr::ladders::ModelName(model);
    static int s_model = -2;
    static float s_o = 0.f, s_u = 0.f, s_c = 0.f;
    if (model != s_model)
    {
        // New ladder model held: the rows show its values.
        s_model = model;
        const ladder_tune::Grip g = ladder_tune::For(model);
        s_o = bmvr::g_LadderTuneOutCm = g.out;
        s_u = bmvr::g_LadderTuneUpCm = g.up;
        s_c = bmvr::g_LadderTuneCurl = g.curl >= 0.f ? g.curl : bmvr::g_LadderGripCurl;
        return;
    }
    if (!name)
        return;
    if (bmvr::g_LadderTuneOutCm != s_o || bmvr::g_LadderTuneUpCm != s_u || bmvr::g_LadderTuneCurl != s_c)
    {
        s_o = bmvr::g_LadderTuneOutCm;
        s_u = bmvr::g_LadderTuneUpCm;
        s_c = bmvr::g_LadderTuneCurl;
        std::lock_guard<std::mutex> lock(ladder_tune::g_Lock);
        ladder_tune::Load();
        ladder_tune::g_Grip[name] = ladder_tune::Grip{ s_o, s_u, s_c };
        ladder_tune::Save();
        Game::logMsg("Ladder grip tune %s: out %+.1f cm up %+.1f cm curl %.2f", name, s_o, s_u, s_c);
    }
}

namespace ammo_hud
{
    // Per weapon (ViewmodelClass::numpadKey): the ammo panel's placement on top
    // of BuildAmmoHudPanel's base spot. VR/ammo_hud.txt
    // ("key forward side up tilt turn roll size"); the file overrides Builtin().
    struct Place
    {
        float fwd = 0.f, side = 0.f, up = 0.f, tilt = 0.f, turn = 0.f, roll = 0.f, size = 2.f;
    };
    std::map<std::string, Place> g_Place;
    std::string g_HeldKey;   // weapon the panel rows show (render thread)
    bool g_Loaded = false;

    std::string Path() { return bmvr::VrFilePath("ammo_hud.txt"); }

    // Built-in per-weapon values = the user's headset tuning (2026-11-25,
    // VR/ammo_hud.txt then); the file still overrides per weapon.
    Place Builtin(const std::string& key)
    {
        struct Row { const char* key; Place p; };
        static const Row kRows[] = {
            { "357", { 0.f, 0.25f, 0.f, 0.f, 0.f, 0.f, 2.f } },
            { "crossbow", { 3.25f, 0.75f, 2.5f, 0.f, 0.f, 0.f, 1.5f } },
            { "gauss", { 6.f, 1.75f, 6.5f, 0.f, 90.f, 60.f, 1.25f } },
            { "glock", { 0.f, 0.25f, 0.f, 0.f, 0.f, 0.f, 2.f } },
            { "gluon", { 2.5f, -4.5f, -4.5f, 55.f, 90.f, 0.f, 2.f } },
            { "grenade", { -1.25f, -0.5f, 0.f, -5.f, 5.f, 0.f, 1.5f } },
            { "hgun", { 0.25f, 2.25f, 3.f, 10.f, 35.f, 20.f, 2.f } },
            { "mp5", { 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 2.f } },
            { "rpg", { 4.75f, 0.75f, 0.25f, 15.f, 90.f, 0.f, 2.f } },
            { "satchel", { -2.f, -0.25f, -0.25f, 0.f, 30.f, 0.f, 1.75f } },
            { "shotgun", { 0.f, 0.5f, 0.f, 0.f, 0.f, 0.f, 2.f } },
            { "snark", { 1.25f, -0.75f, 1.25f, 0.f, 60.f, 0.f, 2.f } },
            { "tripmine", { 1.f, -0.75f, 2.25f, 0.f, 65.f, -10.f, 2.f } },
        };
        for (const Row& r : kRows)
            if (key == r.key)
                return r.p;
        return Place{};
    }

    void Load()
    {
        if (g_Loaded)
            return;
        g_Loaded = true;
        std::ifstream in(Path());
        std::string line;
        int n = 0;
        while (in && std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            char key[32]{};
            Place p;
            if (sscanf_s(line.c_str(), "%31s %f %f %f %f %f %f %f", key, static_cast<unsigned>(sizeof(key)),
                    &p.fwd, &p.side, &p.up, &p.tilt, &p.turn, &p.roll, &p.size) >= 8)
            {
                g_Place[key] = p;
                ++n;
            }
        }
        Game::logMsg("Ammo HUD tuning: %d weapons from %s", n, Path().c_str());
    }

    void Save()
    {
        std::ofstream out(Path(), std::ios::trunc);
        if (!out)
        {
            Game::logMsg("Ammo HUD tuning: save failed %s", Path().c_str());
            return;
        }
        out << "# Ammo HUD placement per weapon (VR panel DEBUG rows \"AMMO HUD\"), on top of the built-in spot.\n";
        out << "# key forward side(+ = away from the gun) up tilt turn roll size   (units; degrees; size 2 = default)\n";
        for (const auto& kv : g_Place)
        {
            const Place& p = kv.second;
            out << kv.first << " " << p.fwd << " " << p.side << " " << p.up << " " << p.tilt << " "
                << p.turn << " " << p.roll << " " << p.size << "\n";
        }
    }

    Place For(const std::string& key)
    {
        Load();
        auto it = g_Place.find(key);
        return it != g_Place.end() ? it->second : Builtin(key);
    }

    void ToGlobals(const Place& p)
    {
        bmvr::g_AmmoHudForward = p.fwd;
        bmvr::g_AmmoHudSide = p.side;
        bmvr::g_AmmoHudUp = p.up;
        bmvr::g_AmmoHudTilt = p.tilt;
        bmvr::g_AmmoHudTurn = p.turn;
        bmvr::g_AmmoHudRoll = p.roll;
        bmvr::g_AmmoHudSize = p.size;
    }

    Place FromGlobals()
    {
        return Place{ bmvr::g_AmmoHudForward, bmvr::g_AmmoHudSide, bmvr::g_AmmoHudUp, bmvr::g_AmmoHudTilt,
            bmvr::g_AmmoHudTurn, bmvr::g_AmmoHudRoll, bmvr::g_AmmoHudSize };
    }

    bool Same(const Place& a, const Place& b)
    {
        return a.fwd == b.fwd && a.side == b.side && a.up == b.up && a.tilt == b.tilt
            && a.turn == b.turn && a.roll == b.roll && a.size == b.size;
    }
}

bool bmvr::AmmoHudRowDefault(const float* f, float& def)
{
    if (!f)
        return false;
    const ammo_hud::Place b = ammo_hud::Builtin(ammo_hud::g_HeldKey);
    if (f == &g_AmmoHudForward) def = b.fwd;
    else if (f == &g_AmmoHudSide) def = b.side;
    else if (f == &g_AmmoHudUp) def = b.up;
    else if (f == &g_AmmoHudTilt) def = b.tilt;
    else if (f == &g_AmmoHudTurn) def = b.turn;
    else if (f == &g_AmmoHudRoll) def = b.roll;
    else if (f == &g_AmmoHudSize) def = b.size;
    else return false;
    return true;
}

// VR panel DEBUG rows "AMMO HUD": the rows (and the panel placement) follow the
// weapon in the hand; a row change is saved for that weapon right away.
void VR::SyncAmmoHudRows()
{
    std::string key;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        key = m_VmClass.numpadKey;
    }
    static ammo_hud::Place s_last;
    static bool s_init = false;
    if (!s_init || key != ammo_hud::g_HeldKey)
    {
        s_init = true;
        ammo_hud::g_HeldKey = key;
        s_last = key.empty() ? ammo_hud::Place{} : ammo_hud::For(key);
        ammo_hud::ToGlobals(s_last);
        return;
    }
    const ammo_hud::Place now = ammo_hud::FromGlobals();
    if (key.empty() || ammo_hud::Same(now, s_last))
        return;
    s_last = now;
    if (ammo_hud::Same(now, ammo_hud::Builtin(key)))
        ammo_hud::g_Place.erase(key);   // back at the built-in values: drop the line
    else
        ammo_hud::g_Place[key] = now;
    ammo_hud::Save();
    Game::logMsg("Ammo HUD tune %s: fwd %+.2f side %+.2f up %+.2f tilt %+.0f turn %+.0f roll %+.0f size %.2f",
        key.c_str(), now.fwd, now.side, now.up, now.tilt, now.turn, now.roll, now.size);
}

void VR::GetRightGlovePalmOffsetMeters(Vector& meters) const
{
    meters = Vector(0.f, 0.f, 0.f);
}

bool VR::WantsRightGloveVisible() const
{
    // Weapons (including crowbar): no gun-hand glove. Empty hands match the left.
    if (m_EmptyHands || !m_HasHeldWeapon)
        return true;
    if (bmvr::g_LeftHanded && m_TwoHandActive)
        return true;
    // Satchel before its first charge: no detonator viewmodel yet, so the
    // right hand would be missing (user 2026-11-18).
    if (SatchelHandsActive())
    {
        const unsigned radio = MagReload::g_SatchelRadioMs.load(std::memory_order_relaxed);
        return radio == 0 || GetTickCount() - radio > 300;
    }
    return false;
}

bool VR::WantsRightGloveWeaponGripCurl() const
{
    return bmvr::g_LeftHanded && m_TwoHandActive;
}

bool VR::WantsOffhandGloveWeaponGripCurl() const
{
    return m_TwoHandActive && !bmvr::g_LeftHanded;
}

Vector VR::GunGripHandOffset(bool rightHand, const QAngle& gunAngles) const
{
    Vector fwd, right, up;
    QAngle::AngleVectors(gunAngles, &fwd, &right, &up);
    return right * (rightHand ? -bmvr::g_VrHandsGunGripRight : bmvr::g_VrHandsGunGripRight)
        + up * bmvr::g_VrHandsGunGripUp + fwd * bmvr::g_VrHandsGunGripForward;
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
        float sf = 0.f;   // support (left) hand point along the gun, cm (+ = forward)
        float seatCm = -1.f;  // magazine seat distance, cm (< 0 = built-in default)
        float seatDeg = -1.f; // magazine seat angle, deg (< 0 = built-in default)
    };

    // Built-in magazine seat tolerances per weapon key (VR panel DEBUG rows
    // "Mag seat distance/angle" override them per weapon). Was one global
    // 9 units x viewmodel scale (~15 cm on the MP5) and no angle: with the
    // magazine drawn in Black Mesa's own reload grip it snapped in from far
    // too far (user, 2026-11-19). Shotgun shell and .357 loader keep their
    // old reach (~15/18 cm) and no angle check for now.
    void SeatDefaults(const std::string& key, float& cm, float& deg)
    {
        cm = 6.f;
        deg = 45.f;
        if (key == "mp5" || key == "glock")
            cm = 5.f;
        else if (key == "shotgun")
        {
            // Was 15 cm from the shell's centre: too eager (user 2026-11-19).
            // Then the shell's leading tip within 5 cm of the port, any angle
            // (it goes in upright from below; along the tube was wrong).
            // 7 cm (user 2026-11-20: misses at 6-7.5 cm it should take; the
            // port also moved 2 cm left, hooks.cpp kShellPortLeftCm).
            cm = 7.f;
            deg = 180.f;
        }
        else if (key == "357")
        {
            cm = 18.f;
            deg = 180.f;
        }
    }

    std::map<std::string, Offsets> g_Off;
    std::string g_HeldKey; // weapon key the DEBUG tuning rows show (game thread)
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
            const int nread = sscanf_s(line.c_str(), "%31s %f %f %f %f %f %f %f %f %f",
                key, static_cast<unsigned>(sizeof(key)),
                &o.ox, &o.oy, &o.oz, &o.ax, &o.ay, &o.az, &o.sf, &o.seatCm, &o.seatDeg);
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
        out << "# key ox oy oz ax ay az [support_cm [seat_cm seat_deg]]  (added on top of the built-in table;\n";
        out << "# support_cm moves the support-hand point along the gun, + = forward;\n";
        out << "# seat_cm / seat_deg: magazine seat distance and angle, -1 = built-in default)\n";
        for (const auto& kv : g_Off)
        {
            out << kv.first << " "
                << kv.second.ox << " " << kv.second.oy << " " << kv.second.oz << " "
                << kv.second.ax << " " << kv.second.ay << " " << kv.second.az << " " << kv.second.sf
                << " " << kv.second.seatCm << " " << kv.second.seatDeg << "\n";
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

    float SupportForwardCm(const std::string& key)
    {
        Load();
        auto it = key.empty() ? g_Off.end() : g_Off.find(key);
        return it != g_Off.end() ? it->second.sf : 0.f;
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

void bmvr::HeldWeaponSeatDefaults(float& cm, float& deg)
{
    vm_numpad::SeatDefaults(vm_numpad::g_HeldKey, cm, deg);
}

// VR panel DEBUG rows "HELD WEAPON ANGLE": show the held weapon's saved
// angle extras, write row changes back to them (and the file). Also picks up
// numpad edits so the rows never write a stale value over them.
void VR::SyncHeldWeaponAngleRows()
{
    std::string key;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        key = m_VmClass.numpadKey;
    }
    static std::string& s_key = vm_numpad::g_HeldKey;
    static float s_p = 0.f, s_y = 0.f, s_r = 0.f, s_s = 0.f, s_sc = 0.f, s_sd = 0.f;
    auto it = key.empty() ? vm_numpad::g_Off.end() : vm_numpad::g_Off.find(key);
    const bool have = it != vm_numpad::g_Off.end();
    const float fp = have ? it->second.ax : 0.f;
    const float fy = have ? it->second.ay : 0.f;
    const float fr = have ? it->second.az : 0.f;
    const float fs = have ? it->second.sf : 0.f;
    // Seat rows show the effective value: the file's, else the built-in one.
    float fsc = 0.f, fsd = 0.f;
    vm_numpad::SeatDefaults(key, fsc, fsd);
    if (have && it->second.seatCm > 0.f)
        fsc = it->second.seatCm;
    if (have && it->second.seatDeg > 0.f)
        fsd = it->second.seatDeg;
    if (key != s_key || fp != s_p || fy != s_y || fr != s_r || fs != s_s || fsc != s_sc || fsd != s_sd)
    {
        // New weapon, or the numpad changed it: the rows show the file.
        s_key = key;
        s_p = bmvr::g_VmTunePitch = fp;
        s_y = bmvr::g_VmTuneYaw = fy;
        s_r = bmvr::g_VmTuneRoll = fr;
        s_s = bmvr::g_VmTuneSupportCm = fs;
        s_sc = bmvr::g_VmTuneSeatCm = fsc;
        s_sd = bmvr::g_VmTuneSeatDeg = fsd;
        return;
    }
    if (key.empty())
        return;
    if (bmvr::g_VmTunePitch != s_p || bmvr::g_VmTuneYaw != s_y || bmvr::g_VmTuneRoll != s_r
        || bmvr::g_VmTuneSupportCm != s_s || bmvr::g_VmTuneSeatCm != s_sc || bmvr::g_VmTuneSeatDeg != s_sd)
    {
        auto& o = vm_numpad::g_Off[key];
        o.ax = s_p = bmvr::g_VmTunePitch;
        o.ay = s_y = bmvr::g_VmTuneYaw;
        o.az = s_r = bmvr::g_VmTuneRoll;
        o.sf = s_s = bmvr::g_VmTuneSupportCm;
        // Seat values are only written once they differ from the built-in one.
        float dc = 0.f, dd = 0.f;
        vm_numpad::SeatDefaults(key, dc, dd);
        s_sc = bmvr::g_VmTuneSeatCm;
        s_sd = bmvr::g_VmTuneSeatDeg;
        o.seatCm = s_sc != dc ? s_sc : -1.f;
        o.seatDeg = s_sd != dd ? s_sd : -1.f;
        vm_numpad::Save();
        Game::logMsg("Held weapon tune %s: pitch %.1f yaw %.1f roll %.1f support %+.1f cm, mag seat %.1f cm %.0f deg (extras on top of the table)",
            key.c_str(), o.ax, o.ay, o.az, o.sf, s_sc, s_sd);
    }
}

void VR::UpdateViewmodelNumpadAdjust(bool paused)
{
    vm_numpad::Load();
    SyncHeldWeaponAngleRows();
    SyncLadderGripRows();
    SyncAmmoHudRows();
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
    if (L4D2VR_ControllerFamilyPoseTuning(family) == L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH)
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
    // These values archive into config.cfg. Keep the list to VR-only fixes.
    // Do not write Portal 2 launch-option cvars (mat_motion_blur_*,
    // mat_grain_scale_override, fps_max, mat_antialias) or video-menu
    // ARCHIVE cvars (cl_csm_qualitymode, nr_shadow_*, nr_lights_quality,
    // r_shadowrendertotexture, water, lod, particles). That set saved the
    // video preset to Low while the menu still said Ultra.
    // Do not touch r_flashlightdepthtexture — that broke deferred flashlight.
    // Do not set cl_csm_enabled 0, nr_shadow_active 0, nr_dev_gb_debug_type 1,
    // or r_lod 2 (unplayable on bm_c2a5a, 2026-08-28).
    if (bmvr::g_HideCrosshair)
        seti("crosshair", 0);
    seti("engine_no_focus_sleep", 0);
    seti("mat_vsync", 0);
    if (bmvr::g_DisableViewBob)
    {
        setf("cl_bob", 0.f);
        setf("cl_bobcycle", 0.f);
        setf("cl_bobup", 0.f);
        setf("cl_viewmodel_lag", 0.f);
        seti("r_jiggle_bones", 0);
    }
    // ViewPunch is summed into the server shot direction. Suppressing it
    // keeps a hit from throwing aim off. Weapon recoil is removed in
    // dGetShootAngles; AddRecoil reads no cvar.
    seti("sv_suppress_viewpunch", 1);
    // ForceOpenVis=true is the old whole-map vis pair. Default leaves PVS alone.
    if (bmvr::g_ForceOpenVis)
    {
        seti("r_occlusion", 0);
        seti("r_fastzreject", 0);
        seti("r_visocclusion", 0);
        seti("r_portalsopenall", 1);
    }
    // Deferred impact lights sit on VR bullet-hole decals as blooms.
    seti("nr_dev_shoot_lights_enabled", 0);
    Game::logMsg("VR QoL cvars applied (%d ok) icvar=%p hideCrosshair=%d bobOff=%d forceOpenVis=%d",
        n, m_Game->m_Cvar, bmvr::g_HideCrosshair ? 1 : 0, bmvr::g_DisableViewBob ? 1 : 0,
        bmvr::g_ForceOpenVis ? 1 : 0);
}

void VR::UpdateHl2vrMsaa()
{
    if (!bmvr::g_MsaaAllowed)
    {
        bmvr::g_Msaa = 0;
        return;
    }
    if (!m_Game || m_Hl2vrMsaaCvarReady)
        return;
    m_Hl2vrMsaaCvarReady = true;
    // FXAA (engine_post on the eyes) stays on: eye MSAA only smooths triangle
    // edges, FXAA still helps alpha-tested and shading edges.
    Game::logMsg("Eye MSAA %u from VR/config.txt. Restart after editing Msaa.",
        bmvr::g_Msaa);
}

void VR::TickMatQueueFromRenderView()
{
    BMVR_PROF("vr TickMatQueueFromRenderView");
    UpdateHl2vrMsaa();
    bmvr::TickVrCvars(m_Game);
    // Entity attachments (muzzle) and traces belong on this thread.
    if (!PauseUiActive())
    {
        UpdateAimCrosshair();
        UpdateRpgLaserPoint();
    }
    UpdateGmanGesture();
    // Glove lighting samples GetLightForPoint here, not in the draw path.
    if (m_GameplayEligible && m_HmdPoseValid)
    {
        Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        if (body.LengthSqr() > 1.f)
        {
            if (m_LeftControllerTrackingValid)
                VrHands_UpdatePalmLight(0, GetLeftControllerAbsPos(body));
            if (m_RightControllerTrackingValid)
                VrHands_UpdatePalmLight(1, GetRightControllerAbsPos(body));
        }
    }
    if (m_Game)
        m_Game->ProbeMatQueueModeFromRenderView();
    ApplyVrQualityOfLifeCvars();
    // QoL SetValue(mat_vsync) crashed the first RenderView (2026-08-26).
    // Crosshair stays in bmvr.cfg. Only mat_queue_mode is written after warmup.
    UpdateAutoMatQueueMode();
}

namespace
{
    // World only (no entities, so the G-Man himself never blocks the ray).
    class CTraceFilterWorldOnlyBmvr : public CTraceFilter
    {
    public:
        CTraceFilterWorldOnlyBmvr() : CTraceFilter(nullptr, 0) {}
        bool ShouldHitEntity(IHandleEntity*, int) override { return false; }
        TraceType GetTraceType() const override { return TraceType::TRACE_WORLD_ONLY; }
    };
}

// Once per map load: does the entity lump list a G-Man? He is not always a
// placed entity: in bm_c3a2g he is an npc_template_maker template spawned by
// a trigger, and his model is only precached then. Maps that list him keep
// retrying the precache lookup once a second (the cost of a map that has him
// from the start); maps that do not, stop at once.
void VR::ScanMapForGman()
{
    m_GmanMapListed = -1;
    if (!bmvr::g_VrGmanGesture || !m_Game || !m_GameplayEligible || m_CurrentMapName.empty())
        return;
    std::string lump;
    const int rc = m_Game->ReadMapEntityLump(m_CurrentMapName.c_str(), lump);
    if (rc < 0)
    {
        Game::logMsg("G-Man gesture: entity lump of %s not read (code %d), precache check only",
            m_CurrentMapName.c_str(), rc);
        return;
    }
    for (char& c : lump)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    const bool listed = lump.find("npc_gman") != std::string::npos
        || lump.find("models/gman") != std::string::npos;
    m_GmanMapListed = listed ? 1 : 0;
    Game::logMsg("G-Man gesture: entity lump of %s (%d bytes) %s",
        m_CurrentMapName.c_str(), rc, listed ? "lists a G-Man" : "has no G-Man");
}

void VR::UpdateGmanGesture()
{
    auto setActive = [&](bool on) {
        if (m_GmanGestureActive.load(std::memory_order_relaxed) != on)
        {
            m_GmanGestureActive.store(on, std::memory_order_relaxed);
            static int s_log;
            if (s_log < 16)
            {
                ++s_log;
                Game::logMsg("G-Man gesture %s", on ? "armed (G-Man in view)" : "off");
            }
        }
    };
    if (!bmvr::g_VrGmanGesture || !m_Game || !m_GameplayEligible || !m_HmdPoseValid || PauseUiActive())
    {
        setActive(false);
        return;
    }
    // A save load on the same map does not change the map name, so LevelInit
    // does not reset us, but the precache table is rebuilt and his model
    // index can change (2026-11-21, bm_c1a3a: armed, quick load, never again).
    // Wait for the load to finish, then look the index up again; the entity
    // lump result (m_GmanMapListed) still holds for the same map.
    if (m_VrPhase == VrPhase::Loading)
    {
        setActive(false);
        return;
    }
    if (m_GmanLoadEpoch != m_LoadEpoch)
    {
        m_GmanLoadEpoch = m_LoadEpoch;
        m_GmanModelIndex[0] = m_GmanModelIndex[1] = -1;
        m_GmanMapState = 0;
        m_GmanMapChecks = 0;
        m_GmanMapCheckMs = 0;
        m_GmanScanMs = 0;
        m_GmanEntityCount = 0;
    }
    const DWORD now = GetTickCount();

    // Is a G-Man model precached? Maps whose entity lump lists him ask once
    // a second until he spawns (ScanMapForGman); maps that do not list him
    // are skipped. Lump not read: the precache table can still be filling
    // during the first frames, so ask up to 10 times, once a second.
    if (m_GmanMapState == 0)
    {
        if (m_GmanMapListed == 0)
        {
            m_GmanMapState = 2;
            return;
        }
        if (m_GmanMapCheckMs != 0 && now - m_GmanMapCheckMs < 1000)
            return;
        m_GmanMapCheckMs = now;
        ++m_GmanMapChecks;
        m_GmanModelIndex[0] = m_Game->ModelPrecacheIndex("models/gman.mdl");
        m_GmanModelIndex[1] = m_Game->ModelPrecacheIndex("models/gman_high.mdl");
        if (m_GmanModelIndex[0] > 0 || m_GmanModelIndex[1] > 0)
        {
            m_GmanMapState = 1;
            Game::logMsg("G-Man gesture: map %s uses the G-Man (model index %d/%d, check %d)",
                m_CurrentMapName.c_str(), m_GmanModelIndex[0], m_GmanModelIndex[1], m_GmanMapChecks);
        }
        else if (m_GmanMapListed != 1 && m_GmanMapChecks >= 10)
        {
            m_GmanMapState = 2;
        }
        return;
    }
    if (m_GmanMapState != 1)
        return;

    // He can enter the PVS (be created on the client) at any time.
    if (m_GmanScanMs == 0 || now - m_GmanScanMs >= 1000)
    {
        m_GmanScanMs = now;
        m_GmanEntityCount = m_Game->CollectEntitiesWithModelIndex(m_GmanModelIndex, 2, m_GmanEntities, 4);
    }

    const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
    {
        setActive(false);
        return;
    }
    const Vector eye = GetViewOrigin(body);
    Vector fwd{}, right{}, up{};
    GetViewBasis(&fwd, &right, &up);
    if (VectorNormalize(fwd) <= 0.01f)
    {
        setActive(false);
        return;
    }
    // ~40 m covers him behind the far windows; the cone is about the
    // headset's view (cos 50 deg).
    constexpr float kMaxDist = 1600.f;
    constexpr float kMinCos = 0.64f;
    bool seen = false;
    for (int i = 0; i < m_GmanEntityCount && !seen; ++i)
    {
        Vector origin{};
        const int mi = m_Game->VisibleEntityModelIndex(m_GmanEntities[i], origin);
        if (mi <= 0 || (mi != m_GmanModelIndex[0] && mi != m_GmanModelIndex[1]))
            continue;
        const Vector head = origin + Vector(0.f, 0.f, 60.f);
        Vector to = head - eye;
        const float dist = VectorNormalize(to);
        if (dist < 1.f || dist > kMaxDist || to.Dot(fwd) < kMinCos)
            continue;
        // Line of sight against solid world only: glass and grates let it
        // through, walls do not (also covers a stale origin out of the PVS).
        if (m_Game->m_EngineTrace)
        {
            CTraceFilterWorldOnlyBmvr filter;
            Ray_t ray;
            ray.Init(eye, head);
            CGameTrace tr{};
            m_Game->m_EngineTrace->TraceRay(ray, CONTENTS_SOLID, &filter, &tr);
            if (tr.fraction < 0.98f && !tr.startsolid)
                continue;
        }
        seen = true;
    }
    setActive(seen);
}

void VR::UpdateAutoMatQueueMode()
{
    if (!m_IsVREnabled || !m_Game)
        return;

    const int current = m_Game->GetMatQueueMode();
    static int s_qlog;
    if (s_qlog < 8)
    {
        // Mode 0 (single-threaded) is expected in menus, loading and warmup;
        // multicore switches to 2 after that ("AutoMatQueueMode set 2").
        Game::logMsg("Material queue mode=%d%s slots=%d auto=%d multicore=%d ring=%d try=%d set=%d get=%d eq=%d failedSlot=%d",
            current, (current == 0 && bmvr::g_MulticoreMode) ? " (single-threaded until warmup ends, then 2)" : "",
            m_Game->MaterialThreadSlotsValid() ? 1 : 0,
            bmvr::g_AutoMatQueueMode ? 1 : 0, bmvr::g_MulticoreMode ? 1 : 0, m_McRingReady ? 1 : 0,
            bmvr::TryMatQueue() ? 1 : 0,
            m_Game->m_MatSetThreadSlot, m_Game->m_MatGetThreadSlot,
            m_Game->m_MatExecuteQueuedSlot, m_McCallQueueFailedSlot);
        ++s_qlog;
    }

    if (current == 2 && m_GameplayEligible)
    {
        ++m_MatQueueOkPresents;
        if (m_MatQueueOkPresents == 120)
            bmvr::EndRisky(L"mat_queue");
    }

    const bool inGame = m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();

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

void VR::NoteEngineScopeFov(float engineFov)
{
    if (bmvr::g_AimDebug && ViewmodelIsScoped() && fabsf(engineFov - m_EngineViewFov) > 0.05f)
        Game::logMsg("AimDebug engine fov %.2f tick=%llu", engineFov, GetTickCount64());
    const float prevFov = m_EngineViewFov;
    m_EngineViewFov = engineFov;
    // The engine's unzoomed FOV (72.7 at 1920x1080): a value held for
    // 150 ms outside the zoom (a zoom ramp changes it every frame).
    {
        const ULONGLONG now = GetTickCount64();
        if (fabsf(engineFov - m_EngineFovStableValue) > 0.01f)
        {
            m_EngineFovStableValue = engineFov;
            m_EngineFovStableSinceMs = now;
        }
        else if (now - m_EngineFovStableSinceMs >= 150 && engineFov >= 55.f && engineFov < 179.f
            && !m_ScopeZoomActive)
            m_EngineRestFov = engineFov;
    }
    if (!ViewmodelIsScoped() || !bmvr::g_ScopeUsesHmdAim)
    {
        m_EngineZoomOutRamp = false;
        return;
    }
    // Black Mesa writes the 2D zoom FOV into CViewSetup (7.4 when fully
    // zoomed on beta 26234270). Once we have seen that, trust it to turn VR
    // magnification on and off — a stuck m_bZooming / latch cannot keep the
    // HMD zoomed after unscope.
    // The game draws its scope overlay (lens + reticle) for as long as its
    // FOV falls (zoom-in ramp, ~100 ms from rest) or sits zoomed, and drops
    // it the moment the FOV starts rising again (zoom-out ramp, ~160 ms).
    // VR magnification follows that: on at the first falling step, off at
    // the second rising one. Fixed thresholds (on below 45, off above 55)
    // showed the overlay over the unmagnified eye view (each eye's lens at
    // its own off-centre image centre) on the way in, and a magnified view
    // with the gun and no lens on the way out.
    const bool restKnown = m_EngineRestFov > 55.f;
    const bool falling = engineFov < prevFov - 0.01f;
    // Rising two frames in a row: a single-frame spike while fully zoomed
    // (7.4 -> 13.3 -> 7.4, seen headless) is not the zoom-out ramp.
    if (engineFov > prevFov + 0.02f)
        ++m_EngineFovRiseFrames;
    else
        m_EngineFovRiseFrames = 0;
    const bool rising = m_EngineFovRiseFrames >= 2;
    const bool atRest = restKnown ? engineFov >= m_EngineRestFov - 0.05f : engineFov >= 55.f;
    if (m_ScopeZoomActive)
    {
        // Before the first engine zoom the netvar/latch path decides.
        if (m_SawEngineZoomFov && (rising || atRest))
        {
            Game::logMsg("Engine unscoped FOV %.1f (%s) — dropping VR zoom", engineFov,
                rising ? "zoom-out ramp" : "at rest");
            m_ScopeZoomActive = false;
            m_CrossbowZoomLatched = false;
            m_EngineZoomOutRamp = rising && !atRest;
        }
        return;
    }
    if (m_EngineZoomOutRamp && (falling || atRest))
        m_EngineZoomOutRamp = false;
    if (m_EngineZoomOutRamp || !(engineFov > 1.f))
        return;
    if (engineFov < 45.f || (restKnown && falling && engineFov < m_EngineRestFov - 0.1f))
    {
        if (!m_SawEngineZoomFov)
            Game::logMsg("Engine scope FOV %.1f — VR zoom will follow CViewSetup", engineFov);
        m_SawEngineZoomFov = true;
        m_ScopeZoomActive = true;
        m_CrossbowZoomLatched = true;
        UpdateScopeAimOffset();
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

float VR::LeftEyeCullPadTan() const
{
    // Fraction of the union half-FOV tangent (tan(m_Fov / 2)) added to the
    // left eye's right edge.
    const float pad = (std::min)(bmvr::g_LeftEyeCullPad, 0.25f);
    if (!(pad > 0.f) || !(m_Fov > 10.f && m_Fov < 179.f))
        return 0.f;
    return pad * tanf(m_Fov * 0.5f * (3.14159265358979323846f / 180.f));
}

bool VR::EyeRenderIsCustom(int stereoEye) const
{
    if (stereoEye != 1 && stereoEye != 2)
        return false;
    return m_OffCenterActive || (stereoEye == 1 && LeftEyeCullPadTan() > 0.f);
}

float VR::EyeRtAspect() const
{
    if (m_RenderWidth >= 640 && m_RenderHeight >= 360)
        return static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight);
    return (m_Aspect > 0.1f) ? m_Aspect : 1.f;
}

bool VR::EyeRenderRect(int stereoEye, float rect[4], float rtAspect) const
{
    if (stereoEye != 1 && stereoEye != 2)
        return false;
    if (m_OffCenterActive && m_EyeTanValid)
    {
        for (int i = 0; i < 4; ++i)
            rect[i] = m_EyeTan[stereoEye - 1][i];
    }
    else
    {
        const float tX = tanf(m_Fov * 0.5f * (3.14159265358979323846f / 180.f));
        const float a = (rtAspect > 0.1f) ? rtAspect : EyeRtAspect();
        rect[0] = -tX;
        rect[1] = tX;
        rect[2] = -tX / a;
        rect[3] = tX / a;
    }
    if (stereoEye == 1)
        rect[1] += LeftEyeCullPadTan();
    // Scoped: Black Mesa draws the crossbow eyepiece, lens and reticle at the
    // image centre. With off-centre eyes that centre is a different direction
    // in each eye (±0.317 NDC on the PS VR2), so each eye showed its own scope
    // aimed elsewhere. Same size, centred on straight ahead: both copies land
    // on the head-forward ray the scoped bolt follows. Render and submit both
    // use this rectangle (OpenXR helper only: the OpenVR backend sets its
    // texture bounds once at start).
    if (m_ScopeZoomActive && m_OpenXrHelperBridgeActive)
    {
        const float hx = 0.5f * (rect[1] - rect[0]);
        const float hy = 0.5f * (rect[3] - rect[2]);
        rect[0] = -hx;
        rect[1] = hx;
        rect[2] = -hy;
        rect[3] = hy;
    }
    return true;
}

bool VR::EyeOffCenterView(int stereoEye, float& fovDeg, float& aspect, float frac[4]) const
{
    if (!EyeRenderIsCustom(stereoEye))
        return false;
    float r[4];
    if (!EyeRenderRect(stereoEye, r))
        return false;
    // The engine builds an off-centre view as fractions of a centred
    // fov/aspect frustum: use the smallest one holding this eye's rectangle.
    // Scoped (WorldRenderFov narrower than m_Fov) scales every tangent alike.
    const float ux = (std::max)(-r[0], r[1]);
    const float uy = (std::max)(-r[2], r[3]);
    if (!(ux > 0.001f && uy > 0.001f))
        return false;
    constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
    const float k = tanf(WorldRenderFov() * 0.5f * kDegToRad) / tanf(m_Fov * 0.5f * kDegToRad);
    fovDeg = 2.f * atanf(ux * k) / kDegToRad;
    aspect = ux / uy;
    frac[0] = 0.5f + 0.5f * r[0] / ux;   // left
    frac[1] = 0.5f + 0.5f * r[1] / ux;   // right
    frac[2] = 0.5f + 0.5f * r[2] / uy;   // bottom
    frac[3] = 0.5f + 0.5f * r[3] / uy;   // top
    return true;
}

float VR::EyeRenderFov(int stereoEye) const
{
    float fov = 0.f, aspect = 0.f, frac[4];
    if (EyeOffCenterView(stereoEye, fov, aspect, frac))
        return fov;
    return WorldRenderFov();
}

bool VR::EyeRenderAspect(int stereoEye, float& aspect) const
{
    float fov = 0.f, frac[4];
    return EyeOffCenterView(stereoEye, fov, aspect, frac);
}

bool VR::OffCenterNdc(int stereoEye, float& sx, float& ox, float& sy, float& oy) const
{
    if (!EyeRenderIsCustom(stereoEye))
        return false;
    float r[4];
    if (!EyeRenderRect(stereoEye, r))
        return false;
    // Overlays project into a reference frustum: the union (m_Fov,
    // m_OffCenterAspect) with OffCenterEyes, else the symmetric eye frustum.
    // Map its NDC onto this eye's rendered rectangle [L, R] x [D, U]. Scope
    // scales reference and rectangle alike, so unscoped values serve.
    const float refX = tanf(m_Fov * 0.5f * (3.14159265358979323846f / 180.f));
    const float refY = m_OffCenterActive ? refX / m_OffCenterAspect : refX / EyeRtAspect();
    if (!(r[1] - r[0] > 0.001f && r[3] - r[2] > 0.001f))
        return false;
    sx = 2.f * refX / (r[1] - r[0]);
    ox = -(r[1] + r[0]) / (r[1] - r[0]);
    sy = 2.f * refY / (r[3] - r[2]);
    oy = -(r[3] + r[2]) / (r[3] - r[2]);
    return true;
}

void VR::SetupOffCenter(const float leftEye[4], const float rightEye[4])
{
    m_OffCenterActive = false;
    const float* eyes[2] = { leftEye, rightEye };
    m_EyeTanValid = true;
    for (int i = 0; i < 2; ++i)
    {
        for (int j = 0; j < 4; ++j)
            m_EyeTan[i][j] = eyes[i][j];
        m_EyeTanValid = m_EyeTanValid && eyes[i][0] < -0.001f && eyes[i][1] > 0.001f
            && eyes[i][2] < -0.001f && eyes[i][3] > 0.001f;
    }
    if (!bmvr::g_OffCenterEyes)
        return;
    float tanX = 0.f, tanY = 0.f;
    for (const float* e : eyes)
    {
        if (!(e[0] < -0.001f && e[1] > 0.001f && e[2] < -0.001f && e[3] > 0.001f))
        {
            Game::logMsg("OffCenterEyes: invalid eye tangents l=%.4f r=%.4f d=%.4f u=%.4f, staying symmetric",
                e[0], e[1], e[2], e[3]);
            return;
        }
        tanX = (std::max)({ tanX, -e[0], e[1] });
        tanY = (std::max)({ tanY, -e[2], e[3] });
    }
    for (int i = 0; i < 2; ++i)
    {
        m_OffCenterFrac[i][0] = 0.5f + 0.5f * eyes[i][0] / tanX;
        m_OffCenterFrac[i][1] = 0.5f + 0.5f * eyes[i][1] / tanX;
        m_OffCenterFrac[i][2] = 0.5f + 0.5f * eyes[i][2] / tanY;
        m_OffCenterFrac[i][3] = 0.5f + 0.5f * eyes[i][3] / tanY;
    }
    m_OffCenterAspect = tanX / tanY;
    m_OffCenterActive = true;
    const float used = ((eyes[0][1] - eyes[0][0]) * (eyes[0][3] - eyes[0][2])) / (4.f * tanX * tanY);
    Game::logMsg("OffCenterEyes on: union fov=%.2f aspect=%.4f L=(l %.4f r %.4f b %.4f t %.4f) R=(l %.4f r %.4f b %.4f t %.4f) left eye uses %.0f%% of the union",
        2.f * atanf(tanX) * 180.f / 3.14159265f, m_OffCenterAspect,
        m_OffCenterFrac[0][0], m_OffCenterFrac[0][1], m_OffCenterFrac[0][2], m_OffCenterFrac[0][3],
        m_OffCenterFrac[1][0], m_OffCenterFrac[1][1], m_OffCenterFrac[1][2], m_OffCenterFrac[1][3],
        used * 100.f);
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
    BMVR_PROF("vr BeginStereoFramePose");
    m_McEyeCopyQueued = false;
    m_McEyeCopyDone = false;
    m_VrGlovesDrawnIntoScene = false;
    if (m_OpenXrHelperBridgeActive)
    {
        if (!m_PosesWaitedThisFrame)
            AwaitOpenXrCompositorFrame();
        // Keep the WaitPoses latch. Consuming a newer WaitFrame here would
        // draw a pose the helper may already have EndFrame'd.
        ++m_OpenXrStereoPairs;
        m_OpenXrPairThread = GetCurrentThreadId();
    }
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
    UpdateScopeAimOffset();
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
    BMVR_PROF("vr EndStereoFramePose");
    m_StereoFramePoseActive = false;
}

void VR::TickOpenXrHelperWatchdog()
{
    if (!m_OpenXrHelperBridgeActive)
        return;
    if (L4D2VR_OpenXrHelperBridgeIsStarted())
        return;

    const DWORD now = GetTickCount();
    if (m_OpenXrHelperRestartCount >= 8)
        return;
    if (m_OpenXrHelperRestartMs != 0 && (now - m_OpenXrHelperRestartMs) < 2500)
        return;

    m_OpenXrHelperRestartMs = now;
    ++m_OpenXrHelperRestartCount;
    Game::logMsg("[VR][OpenXRHelper] helper gone; restarting SteamVR session attempt=%d",
        m_OpenXrHelperRestartCount);

    const OpenXrHelperLaunchConfig cfg = L4D2VR_ReadOpenXrHelperLaunchConfig();
    if (!L4D2VR_StartOpenXrHelper(cfg))
    {
        Game::logMsg("[VR][OpenXRHelper] restart failed");
        return;
    }

    m_OpenXrLastPublishedSharedTextureFrameId.store(0, std::memory_order_release);
    bmvr::SetOpenXrHelperSession(true);
}

// Uncapped (fps_max 300) the game rendered ~272 stereo pairs/s for the 90
// the headset shows: GPU 99 % busy vs 44 % at fps_max 100 (2026-10-27).
// FrameCap 1 (default since 2026-11-17, user) = rate + 10: the least extra
// GPU work above the headset rate (the newest frame's age cycles 0..10 ms ten
// times a second; on 10-27 the user found 2 smoother, later chose 1). 2 =
// twice the headset rate: always a frame <= half a refresh old, at a steady
// age. 0 = leave the game's value (restores 300 if we changed it). The live value is read back so a cfg/console change is undone and
// logged. Runs on the Present path (VR::Update), where the OpenXR cap has
// set fps_max since 2026-10-27.
void VR::ApplyFrameCap(int nominalHz, float sourceHz, const char* source)
{
    bmvr::sampler::NoteHeadsetHz(nominalHz);
    if (!m_Game)
        return;
    const int mode = static_cast<int>(bmvr::g_FrameCapMode + 0.5f);
    int want = 0;
    if (mode == 1 && nominalHz)
        want = nominalHz + 10;
    else if (mode == 2 && nominalHz)
        want = nominalHz * 2;
    else if (mode == 0 && m_FrameCapApplied)
        want = 300;
    float live = 0.f;
    m_Game->GetConVarFloat("fps_max", live);
    if (want && static_cast<int>(live + 0.5f) != want)
    {
        m_Game->SetConVarFloat("fps_max", static_cast<float>(want));
        Game::logMsg("fps_max %.0f -> %d (FrameCap %d, headset %.1f Hz %s, %d nominal)",
            live, want, mode, sourceHz, source ? source : "?", nominalHz);
        m_FrameCapApplied = mode == 0 ? 0 : want;
    }
}

// OpenVR backend: the HMD reports its refresh rate (Prop_DisplayFrequency_Float,
// e.g. 89.99 / 119.88 on the PS VR2), so no measuring as on OpenXR. Checked
// every 5 s: a FrameCap change from the panel applies without a restart.
void VR::NotePublishEvenness()
{
    PublishEvenness& e = m_PublishEven;
    const double now = QpcNowMs();
    if (e.lastMs > 0)
    {
        const double dt = now - e.lastMs;
        if (dt < 250.0)  // longer = load / pause, not pacing
        {
            const double tick = 1000.0 / 90.0;
            int b = static_cast<int>(dt / tick + 0.5) - 1;
            if (b < 0) b = 0;
            if (b > 4) b = 4;
            ++e.ticks[b];
            if (e.lastBucket >= 0 && b != e.lastBucket)
                ++e.changes;
            e.lastBucket = b;
            ++e.n;
            e.sum += dt;
            e.sumSq += dt * dt;
            if (dt < e.minMs) e.minMs = dt;
            if (dt > e.maxMs) e.maxMs = dt;
        }
        else
            e.lastBucket = -1;
    }
    e.lastMs = now;
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
    uint32_t helperSubmitted = 0;
    const bool haveHelperSubmitted = L4D2VR_ReadOpenXrHelperSubmittedFrames(helperSubmitted);
    const uint32_t helperDelta = (haveHelperSubmitted && helperSubmitted >= m_OpenXrHelperSubmittedLast)
        ? (helperSubmitted - m_OpenXrHelperSubmittedLast) : 0;
    RefreshOpenXrHelperConsumed(QpcNowMs());
    // The rate is the helper's measured submit rate rounded up to a standard
    // rate, only ever raised, so a stretch where the helper falls behind
    // cannot pull it down.
    if (m_Game && haveHelperSubmitted)
    {
        const float hz = helperDelta * scale;
        static int s_nominal;
        const int kRates[] = { 72, 90, 120, 144 };
        for (int r : kRates)
        {
            if (hz >= 60.f && hz <= r * 1.05f)
            {
                if (r > s_nominal)
                    s_nominal = r;
                break;
            }
        }
        ApplyFrameCap(s_nominal, hz, "measured");
    }
    Game::logMsg("OpenXR publish rate present=%.0f/s published=%.0f/s skipStale=%.0f/s skipVsync=%.0f/s skipNoPose=%.0f/s slots=%u copy=%d "
        "deferred=%.0f/s throttled=%.0f/s helperHold=%.0f/s dropped=%.0f/s pending=%u/%u maxPending=%u "
        "paceWait=%.0f/s avg=%.2fms max=%.2fms await=%.0f/s avg=%.2fms max=%.2fms handoffWait=%.0f/s "
        "helperHz=%.0f helperFeedback=%d consumedLag=%d pairs=%.0f/s "
        "copyWait=%.0f/s avg=%.2fms max=%.2fms pairTid=%u copyTid=%u presentTid=%u "
        "mcQueued=%.0f/s mcQueueFail=%.0f/s mcMatCopied=%.0f/s mcPresentFallback=%.0f/s",
        m_OpenXrSubmitAttempts * scale, m_OpenXrPublishes * scale,
        m_OpenXrSkippedNoNewFrame * scale, m_OpenXrSkipSameVsync * scale,
        m_OpenXrSkipStereoNoPose * scale,
        kOpenXrPublishSlots, m_OpenXrPublishActive ? 1 : 0,
        m_OpenXrDeferredPublishes * scale, m_OpenXrDeferredThrottles * scale,
        m_OpenXrDeferredHelperHolds * scale,
        m_OpenXrDeferredDropped * scale, m_OpenXrPendingCount, bmvr::g_OpenXrMaxPending,
        m_OpenXrDeferredMaxPending,
        m_OpenXrPaceWaits * scale,
        m_OpenXrPaceWaits ? m_OpenXrPaceWaitMs / m_OpenXrPaceWaits : 0.0,
        m_OpenXrPaceWaitMaxMs,
        m_OpenXrAwaitFrames * scale,
        m_OpenXrAwaitFrames ? m_OpenXrAwaitPoseMs / m_OpenXrAwaitFrames : 0.0,
        m_OpenXrAwaitPoseMaxMs,
        m_OpenXrAwaitHandoffs * scale,
        helperDelta * scale,
        m_OpenXrHelperFeedbackLive ? 1 : 0,
        m_OpenXrHelperFeedbackLive
            ? static_cast<int>(latestFrame - m_OpenXrHelperConsumedFrameId) : -1,
        m_OpenXrStereoPairs * scale,
        m_OpenXrCopyWaits * scale,
        m_OpenXrCopyWaits ? m_OpenXrCopyWaitMs / m_OpenXrCopyWaits : 0.0,
        m_OpenXrCopyWaitMaxMs,
        m_OpenXrPairThread, m_OpenXrCopyThread, static_cast<unsigned>(GetCurrentThreadId()),
        m_McCopyQueuedCount * scale, m_McCopyQueueFailCount * scale,
        m_McCopyMatDoneCount * scale, m_McPresentFallbackCount * scale);
    m_McCopyQueuedCount = 0;
    m_McCopyQueueFailCount = 0;
    m_McCopyMatDoneCount = 0;
    m_McPresentFallbackCount = 0;
    m_OpenXrCopyWaits = 0;
    m_OpenXrCopyWaitMs = 0.0;
    m_OpenXrCopyWaitMaxMs = 0.0;
    m_OpenXrStereoPairs = 0;
    {
        PublishEvenness& e = m_PublishEven;
        if (e.n > 0)
        {
            const double avg = e.sum / e.n;
            const double var = e.sumSq / e.n - avg * avg;
            Game::logMsg("publish evenness: %u frames, interval avg %.2f ms sd %.2f min %.2f max %.2f; "
                "ticks 1/2/3/4/5+ = %u/%u/%u/%u/%u, tick changes %.0f%% of frames",
                e.n, avg, var > 0 ? std::sqrt(var) : 0.0, e.minMs, e.maxMs,
                e.ticks[0], e.ticks[1], e.ticks[2], e.ticks[3], e.ticks[4], 100.0 * e.changes / e.n);
        }
        const double last = e.lastMs;
        const int lastBucket = e.lastBucket;
        e = PublishEvenness{};
        e.lastMs = last;
        e.lastBucket = lastBucket;
    }
    m_OpenXrPublishRateTick = now;
    m_OpenXrSubmitAttempts = 0;
    m_OpenXrPublishes = 0;
    m_OpenXrSkippedNoNewFrame = 0;
    m_OpenXrSkippedHelperBusy = 0;
    m_OpenXrSkipSameVsync = 0;
    m_OpenXrSkipStereoNoPose = 0;
    m_OpenXrDeferredPublishes = 0;
    m_OpenXrDeferredThrottles = 0;
    m_OpenXrDeferredHelperHolds = 0;
    m_OpenXrDeferredDropped = 0;
    m_OpenXrDeferredMaxPending = 0;
    m_OpenXrPaceWaits = 0;
    m_OpenXrPaceWaitMs = 0.0;
    m_OpenXrPaceWaitMaxMs = 0.0;
    m_OpenXrAwaitFrames = 0;
    m_OpenXrAwaitHandoffs = 0;
    m_OpenXrAwaitPoseMs = 0.0;
    m_OpenXrAwaitPoseMaxMs = 0.0;
    if (haveHelperSubmitted)
        m_OpenXrHelperSubmittedLast = helperSubmitted;
}

void VR::WaitPosesForStereoFrame(bool allowQueued)
{
    BMVR_PROF("vr WaitPosesForStereoFrame");
    if (m_PosesWaitedThisFrame)
        return;
    // Frame budget line: wall time this frame's pose wait took (main thread).
    struct PoseWaitTimer
    {
        double t0 = QpcNowMs();
        ~PoseWaitTimer() { bmvr::sampler::NoteMainPoseWait(QpcNowMs() - t0); }
    } poseWaitTimer;
    if (m_OpenXrHelperBridgeActive)
    {
        // In-game pause / HWND deactivated with GameUI: do not block 22ms
        // for a WaitFrame pose. Do not skip the wait on focus-loss without
        // GameUI (SteamVR overlay). Do not set drawn here — leftover HWND
        // under GameUI must still paint the desktop pause screen.
        if (WantPauseWorldOverlay() || (!AppActivated() && IsMenuUp()))
        {
            ConsumeOpenXrTracking();
            UpdateTracking();
            m_PosesWaitedThisFrame = true;
            return;
        }
        const bool gotNew = AwaitOpenXrCompositorFrame();
        UpdateTracking();
        m_PosesWaitedThisFrame = true;
        // No new WaitFrame pose: do not draw another 3k pair this engine
        // frame. Last eyes stay in the helper swapchain (runtime ATW).
        if (!gotNew
            && m_OpenXrLastPublishedSharedTextureFrameId.load(std::memory_order_acquire) != 0)
            m_StereoEyesDrawnThisFrame = true;
        return;
    }
    m_PosesWaitedThisFrame = true;
}

ICallQueue* VR::ProbeCallQueue()
{
    if (!m_Game || !m_Game->m_MaterialSystem)
        return nullptr;
    IMatRenderContext* ctx = SehGetRenderContext(m_Game->m_MaterialSystem);
    if (!ctx)
        return nullptr;

    ICallQueue* q = nullptr;
    if (m_Hl2vrCallQueueSlot >= 0 && m_Hl2vrCallQueueSlot != m_McCallQueueFailedSlot)
        q = SehCallQueueFromSlot(ctx, m_Hl2vrCallQueueSlot);
    else
    {
        // BM first. Source slot 146 on a shifted IMatRenderContext can return
        // a non-null object that is not ICallQueue (Index SteamVR 2026-09-09:
        // QueueFunctorInternal SEH-failed at 146, then published=0/s).
        const int slots[] = {
            Offsets::kIMatRenderContext_GetCallQueueSlotBmShift,
            Offsets::kIMatRenderContext_GetCallQueueSlotSource,
        };
        for (int slot : slots)
        {
            if (slot == m_McCallQueueFailedSlot)
                continue;
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
            Game::logMsg("HL2VR GetCallQueue probe slot=%d q=%p (%d=BM, 146=Source)",
                m_Hl2vrCallQueueSlot, (void*)q, Offsets::kIMatRenderContext_GetCallQueueSlotBmShift);
        }
    }
    SehReleaseMatContext(ctx);
    return q;
}

bool VR::QueueMcMatFunctor(CFunctor* functor, const char* failTag)
{
    if (!functor)
        return false;
    ICallQueue* q = ProbeCallQueue();
    if (q && SehQueueFunctorInternal(q, functor))
        return true;

    // A non-null GetCallQueue at Source slot 146 is often not ICallQueue on BM.
    // Do not probe the other slot in the same call — vt[0] on that object can
    // corrupt the material context and kill bms.exe at first stereo (2026-09-09).
    if (q && m_Hl2vrCallQueueSlot >= 0)
    {
        m_McCallQueueFailedSlot = m_Hl2vrCallQueueSlot;
        m_Hl2vrCallQueueSlot = -1;
        static int s_fail;
        if (s_fail < 4)
        {
            Game::logMsg("MC %s: QueueFunctorInternal failed slot=%d",
                failTag ? failTag : "functor", m_McCallQueueFailedSlot);
            ++s_fail;
        }
    }
    else
    {
        static int s_noQ;
        if (s_noQ < 4)
        {
            Game::logMsg("MC %s: no ICallQueue", failTag ? failTag : "functor");
            ++s_noQ;
        }
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

void VR::McCompositeEyesToRing(uint32_t band, const L4D2VROpenXrPoseDesc* pose, const HandDrawSnap* hands)
{
    IDirect3DSurface9* left = m_D9LeftEyeSurface;
    IDirect3DSurface9* right = m_D9RightEyeSurface;
    if (!IsMenuUp()
        && (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes || bmvr::g_HandHud || bmvr::g_DamageFlash
            || WeaponMenuOpen() || SettingsPanelOpen() || AimCrosshairVisible()
            || (hands && hands->valid && (hands->aimBeamValid || hands->aimCrosshairValid))
            || ToastVisible() || bmvr::g_DebugBlankEye >= 0.5f))
    {
        t_HandSnap = (hands && hands->valid && bmvr::g_McHandSnapshot) ? hands : nullptr;
        DrawIndependentHandMarkers(left, 1, true, true);
        DrawIndependentHandMarkers(right, 2, true, true);
        t_HandSnap = nullptr;
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

    bool copied = false;
    {
        GpuTimerScope gpuScope(g_GpuTimer.device, "mod eyes -> OpenXR publish copy");
        copied = CopyEyesToOpenXrPublishSlot(left, right, pend);
    }
    if (copied)
    {
        m_McEyeCopyDone = true;
        ++m_McCopyMatDoneCount;
    }
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
    {
        GpuTimerScope gpuScope(g_GpuTimer.device, "mod desktop mirror");
        MirrorStereoToDesktopWindow();
    }
}

bool VR::QueueMcEyeBind(int stereoEye, bool flushGpu)
{
    BMVR_PROF("vr QueueMcEyeBind");
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
    BMVR_PROF("vr QueueMcCompositeToRing");
    const L4D2VROpenXrPoseDesc* pose = m_OpenXrStereoRenderPoseValid
        ? &m_OpenXrStereoRenderPose : nullptr;
    HandDrawSnap hands{};
    CaptureHandDrawSnap(hands);
    auto* functor = new (std::nothrow) McMatFunctor(
        this, McMatFunctor::Kind::Composite, 0, band, false, pose, &hands);
    if (!functor)
        return false;
    functor->AddRef();
    if (!QueueMcMatFunctor(functor, "QueueMcCompositeToRing"))
    {
        ++m_McCopyQueueFailCount;
        return false;
    }
    m_McEyeCopyQueued = true;
    ++m_McCopyQueuedCount;
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
    if (surface == m_D9HUDPublishSurface && m_PauseCapHud.load() > 0)
    {
        const int left = m_PauseCapHud.fetch_sub(1) - 1;
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(g_D3DVR9->GetD3DDevice(&dev)) && dev)
        {
            char nm[96];
            std::snprintf(nm, sizeof(nm), "pausecap-%05lu-layer-%02d",
                static_cast<unsigned long>(GetTickCount() - m_PauseCapStartMs), 11 - left);
            const std::string base = bmvr::VrFilePath(nm);
            DumpSurfaceBmp(dev, surface, base + ".bmp", 2);
            DumpSurfaceBmp(dev, surface, base + "-alpha.bmp", 2, true);
            dev->Release();
            Game::logMsg("Pause capture: %s (phase %d, menu up %d)", nm, static_cast<int>(m_VrPhase), IsMenuUp() ? 1 : 0);
        }
    }
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

bool VR::McEyesSmallerThanWindow() const
{
    UINT clientW = 0, clientH = 0;
    if (!QueryGameClientSize(clientW, clientH) || clientW < 640 || clientH < 360)
        return false;
    if (m_RenderWidth < 640 || m_RenderHeight < 360)
        return false;
    // OpenXR Simulator 1280x1408 vs HWND 2560x1440: height is only 32px
    // shorter, so "+32 <" never fired and MC still armed (2026-09-09).
    // Both dims <= window: 1x blit. WMR 3168x3100 is larger, MC stays.
    // Index native is taller than 1440, MC stays. mat_queue_mode unchanged.
    return m_RenderWidth <= clientW && m_RenderHeight <= clientH;
}

bool VR::ShouldHwndBlitStereo() const
{
    // OpenXR Simulator 1280x1408 inside HWND 2560x1440: grow-only leaves
    // FullFrame at the window (worldMatch=0). Double RenderView at 1280 into
    // that 2560 G-buffer, then a third DRAWHUD pass, exited bms.exe
    // (2026-09-09, heartbeat in_drawhud). WMR 3168x3100 is larger than the
    // HWND, world RTs grow, worldMatch=1, this stays false.
    if (bmvr::OffscreenWorldMatchesEyes())
        return false;
    if (bmvr::UseGbMatchViewLock())
        return true;
    return McEyesSmallerThanWindow();
}

void VR::LogRenderPathIfChanged()
{
    const bool worldMatch = bmvr::OffscreenWorldMatchesEyes();
    const bool hwndBlit = ShouldHwndBlitStereo();
    const bool mc = McPipelineEnabled();
    const int path = hwndBlit ? 3 : (worldMatch ? (mc ? 1 : 2) : 4);
    static int s_lastPath;
    static int s_logs;
    if (path == s_lastPath || s_logs >= 16)
        return;
    s_lastPath = path;
    ++s_logs;
    char why[256];
    bmvr::DescribeWorldRtMatch(why, sizeof(why));
    switch (path)
    {
    case 1:
        Game::logMsg("Render path: per-eye stereo at eye size, multicore (best path) eye=%ux%u", m_RenderWidth, m_RenderHeight);
        break;
    case 2:
        Game::logMsg("Render path: per-eye stereo at eye size, single-threaded (MulticoreMode=%d) eye=%ux%u",
            bmvr::g_MulticoreMode ? 1 : 0, m_RenderWidth, m_RenderHeight);
        break;
    case 3:
        Game::logMsg("Render path: FALLBACK window view blitted into the eyes (window resolution); %s%s%s",
            why, McEyesSmallerThanWindow() ? "; eyes fit inside the game window" : "",
            bmvr::g_MulticoreMode
                ? "; MulticoreMode=true has no effect on this path (the game still switches its material "
                  "queue on, but no eye copies are queued, so mcPresentFallback/s equals the frame rate)"
                : "");
        // Players rarely read the log: say it in the headset too, and ask for
        // a report so we can find out why this setup missed the per-eye path.
        Game::logMsg("Render path: this is the low-resolution FALLBACK, not the intended per-eye path. "
            "Please open an issue with this bmvr_log.txt at "
            "https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr/issues so we can get your setup "
            "onto the per-eye path.");
        ShowToast("LOW-RES FALLBACK", 20000, "Please report it on GitHub with bmvr_log.txt");
        break;
    default:
        Game::logMsg("Render path: per-eye stereo without eye-sized world RTs; %s", why);
        break;
    }
}

bool VR::McPipelineEnabled() const
{
    if (!bmvr::g_MulticoreMode || !bmvr::TryMatQueue())
        return false;
    if (McEyesSmallerThanWindow())
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
    return true;
}

bool VR::StereoWorldPassActive() const
{
    // Pass-through still draws one HWND-sized (or eye-matched) view. Treating
    // that as stereo armed GetScreenSize/viewmodel bone rewrite before the
    // pair exists (2026-09-09: AV in SehCopyAndFixViewmodelMatrices on
    // bm_c4a2a after GameUI hide, heartbeat in_rel_look, no stereo enter).
    if (!PassThroughWarmupDone())
        return false;
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
    m_McEyeCopyQueued = false;
    m_McEyeCopyDone = false;
}

void VR::FinishMcStereoPair()
{
    BMVR_PROF("vr FinishMcStereoPair");
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
    if (McEyesSmallerThanWindow())
    {
        if (m_McRingReady)
            ReleaseMcRing();
        static int s_small;
        if (s_small < 4)
        {
            UINT cw = 0, ch = 0;
            QueryGameClientSize(cw, ch);
            Game::logMsg("MC ring skipped: eyes %ux%u not larger than HWND %ux%u (1x blit; mat_queue_mode unchanged)",
                m_RenderWidth, m_RenderHeight, cw, ch);
            ++s_small;
        }
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
    NotePublishEvenness();
    if (bmvr::glowtrace::Active())
        bmvr::glowtrace::Event('U', frameId);
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

void VR::MsaaEyesTo1x(IDirect3DDevice9* device)
{
    // 2D menu / loading frames are copied or painted straight into the eye
    // surfaces: do that on their 1x images (no scaled copy into 4 samples).
    if (!UseVrMsaa() || !device)
        return;
    bmvr::MsaaEyeRetire(device, m_D9LeftEyeSurface, m_D9LeftEyeDepthSurface);
    bmvr::MsaaEyeRetire(device, m_D9RightEyeSurface, m_D9RightEyeDepthSurface);
}

IDirect3DSurface9* VR::ThreadEyeSurface() const
{
    IDirect3DSurface9* eye = McPlaybackEyeSurface();
    if (!eye && !McPipelineEnabled() && StereoEyeBlitActive())
        eye = StereoEyeBlitDest();
    return eye;
}

void VR::NoteTranslucentPassStart()
{
    if (!UseVrMsaa())
        return;
    if (McPipelineEnabled())
    {
        // Queued rendering: the switch has to sit in the draw stream, right
        // before this view's translucent draws.
        auto* functor = new (std::nothrow) McMatFunctor(this, McMatFunctor::Kind::RetireMsaa, 0, 0, false);
        if (!functor)
            return;
        functor->AddRef();
        QueueMcMatFunctor(functor, "RetireMsaaForTranslucents");
        return;
    }
    RetireMsaaEyeIfBound();
}

// RefractOncePerEye: the main view's translucent pass (world glass) is done.
// Copies after it (env_screenoverlay refract, underwater warp) must see the
// finished frame: the end-of-Lambda-Core overlay (bm_c3a2i) otherwise pasted
// the first glass copy, taken before the orb particle and its lens flare, over
// the whole eye. One more copy per eye at most.
void VR::NoteWorldTranslucentsDone()
{
    if (McPipelineEnabled())
    {
        auto* functor = new (std::nothrow) McMatFunctor(this, McMatFunctor::Kind::RefractReset, 0, 0, false);
        if (!functor)
            return;
        functor->AddRef();
        QueueMcMatFunctor(functor, "RefractResetAfterTranslucents");
        return;
    }
    m_RefractEyeDone = nullptr;
}

void VR::RetireMsaaEyeIfBound()
{
    // Only when the eye itself is the render target: the caller filters on the
    // main view, this also keeps reflection / refraction / monitor views out.
    IDirect3DSurface9* eye = ThreadEyeSurface();
    if (!eye || !g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DSurface9* rt0 = nullptr;
    const bool bound = SUCCEEDED(device->GetRenderTarget(0, &rt0)) && rt0 == eye;
    if (rt0)
        rt0->Release();
    device->Release();
    if (bound)
        RetireMsaaEyeForPost();
}

void VR::RetireMsaaEyeForPost()
{
    // Only the thread drawing this eye knows it: multicore playback TLS, or
    // the single-threaded stereo blit. Anything else (main thread) does nothing.
    IDirect3DSurface9* color = McPlaybackEyeSurface();
    IDirect3DSurface9* depth = McPlaybackEyeDepth();
    if (!color && !McPipelineEnabled() && StereoEyeBlitActive())
    {
        color = StereoEyeBlitDest();
        depth = color == m_D9LeftEyeSurface ? m_D9LeftEyeDepthSurface
            : color == m_D9RightEyeSurface ? m_D9RightEyeDepthSurface : nullptr;
    }
    if (!color || !g_D3DVR9)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    {
        GpuTimerScope scope(device, "msaa retire (colour resolve)");
        bmvr::MsaaEyeRetire(device, color, depth);
    }
    static int s_log;
    if (s_log < 4)
    {
        Game::logMsg("Eye MSAA: post chain switched to 1x (eye surface %p)", (void*)color);
        ++s_log;
    }
    device->Release();
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
    // Eye MSAA: always refresh the 1x submit copies. Multicore gameplay never
    // sets the HasScene flags (it publishes from the ring), so menus, loading
    // screens and the loading card were published from never-written submit
    // surfaces = black. DXVK serves this copy from the eye's 1x resolve.
    if (m_D9LeftEyeSurface && m_D9LeftEyeSubmitSurface
        && m_D9LeftEyeSurface != m_D9LeftEyeSubmitSurface)
    {
        const HRESULT hr = device->StretchRect(
            m_D9LeftEyeSurface, nullptr, m_D9LeftEyeSubmitSurface, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
            Game::logMsg("MSAA left eye resolve hr=0x%08X", (unsigned)hr);
    }
    if (m_D9RightEyeSurface && m_D9RightEyeSubmitSurface
        && m_D9RightEyeSurface != m_D9RightEyeSubmitSurface)
    {
        const HRESULT hr = device->StretchRect(
            m_D9RightEyeSurface, nullptr, m_D9RightEyeSubmitSurface, nullptr, D3DTEXF_NONE);
        if (FAILED(hr))
            Game::logMsg("MSAA right eye resolve hr=0x%08X", (unsigned)hr);
    }
    m_CaptureReentry = false;
}

// Pause-menu layer copies (FixPauseLayerAlpha / PauseLayerColourFromBackbuffer).
// Kept between frames (created per frame they made the menu flicker) and
// dropped here before a device Reset, with the other VR render targets.
static IDirect3DTexture9* g_PauseLayerCopyTex;
static IDirect3DTexture9* g_PauseFrameCopyTex;
static IDirect3DTexture9* g_TitleBloomTex[2];   // title layer glow (TitleLayerPremultiplied)
static void ReleasePauseLayerCopies()
{
    for (IDirect3DTexture9*& t : g_TitleBloomTex)
    {
        if (t)
        {
            t->Release();
            t = nullptr;
        }
    }
    if (g_PauseLayerCopyTex)
    {
        g_PauseLayerCopyTex->Release();
        g_PauseLayerCopyTex = nullptr;
    }
    if (g_PauseFrameCopyTex)
    {
        g_PauseFrameCopyTex->Release();
        g_PauseFrameCopyTex = nullptr;
    }
}

// Cached render-target texture of the given size/format (recreated on change).
static IDirect3DTexture9* CachedPauseCopy(IDirect3DTexture9*& slot, IDirect3DDevice9* device, UINT w, UINT h, D3DFORMAT fmt)
{
    if (slot)
    {
        D3DSURFACE_DESC d{};
        IDirect3DDevice9* owner = nullptr;
        bool same = SUCCEEDED(slot->GetLevelDesc(0, &d)) && d.Width == w && d.Height == h && d.Format == fmt
            && SUCCEEDED(slot->GetDevice(&owner)) && owner == device;
        if (owner)
            owner->Release();
        if (same)
            return slot;
        slot->Release();
        slot = nullptr;
    }
    if (FAILED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &slot, nullptr)))
        slot = nullptr;
    return slot;
}

void VR::ReleaseVRRenderTargetsForDeviceReset()
{
    bmvr::InvalidateClientSizeCache();
    ReleasePauseLayerCopies();
    GpuTimerReleaseAll();
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
    ReleaseT(m_D9HUDPublishSurface);
    ReleaseT(m_D9HUDPublishTexture);
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

    // Render-target audit draw hooks: created now, enabled only while an
    // audit runs (VR/audit.txt).
    if (MH_CreateHook(vtbl[kIDirect3DDevice9_DrawPrimitive], reinterpret_cast<LPVOID>(&HookedDrawPrimitive),
            reinterpret_cast<LPVOID*>(&g_OrigDrawPrimitive)) == MH_OK
        && MH_CreateHook(vtbl[kIDirect3DDevice9_DrawIndexedPrimitive], reinterpret_cast<LPVOID>(&HookedDrawIndexedPrimitive),
            reinterpret_cast<LPVOID*>(&g_OrigDrawIndexedPrimitive)) == MH_OK
        && MH_CreateHook(vtbl[kIDirect3DDevice9_DrawPrimitiveUP], reinterpret_cast<LPVOID>(&HookedDrawPrimitiveUP),
            reinterpret_cast<LPVOID*>(&g_OrigDrawPrimitiveUP)) == MH_OK
        && MH_CreateHook(vtbl[kIDirect3DDevice9_DrawIndexedPrimitiveUP], reinterpret_cast<LPVOID>(&HookedDrawIndexedPrimitiveUP),
            reinterpret_cast<LPVOID*>(&g_OrigDrawIndexedPrimitiveUP)) == MH_OK)
        g_AuditVtbl = vtbl;
    else
        Game::logMsg("MH_CreateHook draw (audit) failed");

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

    // Eye MSAA: only the private eye colour + depth surfaces get samples; the
    // engine's final material pass re-rasterises scene geometry into them
    // (single-threaded via StereoEyeBlitDest, multicore via McPlaybackEyeSurface).
    // Idea from a Discord user's AA4-GEOMETRY4 Pimax build.
    // The VR panel row changes g_MsaaF; a new value recreates the eyes like a
    // live SteamVR resolution change.
    bmvr::g_Msaa = bmvr::NormalizeMsaa(static_cast<int>(bmvr::g_MsaaF + 0.5f));
    uint32_t wantAa = (bmvr::g_MsaaAllowed && !m_EyeMsaaAllocFailed) ? bmvr::g_Msaa : 0;
    if (wantAa < 2)
        wantAa = 0;
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
        const bool aaChange = wantAa != m_AntiAliasing;
        if (haveW == w && haveH == h && !aaChange)
        {
            if (bmvr::g_MulticoreMode)
                EnsureMcRing(device);
            return true;
        }
        if (haveW == w && haveH == h && aaChange)
        {
            if (m_StereoEyeBlitActive)
                return true;   // mid-eye: next call
            Game::logMsg("Recreating D3D eyes %ux%u for MSAA %u -> %u (VR panel)", haveW, haveH, m_AntiAliasing, wantAa);
            m_CreatedVRTextures.store(false, std::memory_order_release);
        }
        else
        {
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
        if (!liveOffscreenResize)
        {
            Game::logMsg("Keep existing D3D eyes %ux%u (requested %ux%u; HWND resize must not recreate)",
                haveW, haveH, w, h);
            m_RenderWidth = haveW;
            m_RenderHeight = haveH;
            if (bmvr::g_MulticoreMode)
                EnsureMcRing(device);
            return true;
        }
        // Do not BeginRisky on live SS. Stereo already settled hmd_offscreen
        // after 120 frames; a recreate+quit false-banned the path.
        Game::logMsg("Recreating D3D eyes %ux%u -> %ux%u (SteamVR live SS)", haveW, haveH, w, h);
        m_CreatedVRTextures.store(false, std::memory_order_release);
        }
    }

    std::lock_guard<TextureStateMutex> lock(m_TextureMutex);
    m_AntiAliasing = wantAa;

    auto createEye = [&](TextureID id, IDirect3DTexture9** tex, IDirect3DSurface9** surf, SharedTextureHolder& vk) -> bool {
        ReleaseT(*surf);
        ReleaseT(*tex);
        m_CreatingTextureID = id;
        HRESULT hr;
        {
            bmvr::Hl2vrMsaaScopeGuard msaa(UseVrMsaa(), w, h);
            hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, nullptr);
        }
        m_CreatingTextureID = Texture_None;
        if (FAILED(hr) || !*tex)
        {
            Game::logMsg("CreateTexture eye id=%d hr=0x%08X", (int)id, (unsigned)hr);
            return false;
        }
        if (!*surf)
            (*tex)->GetSurfaceLevel(0, surf);
        if (UseVrMsaa())
        {
            D3DSURFACE_DESC actual{};
            if (!*surf || FAILED((*surf)->GetDesc(&actual))
                || static_cast<uint32_t>(actual.MultiSampleType) != m_AntiAliasing)
            {
                Game::logMsg("Eye MSAA colour id=%d got samples=%d (wanted %u)",
                    (int)id, *surf ? (int)actual.MultiSampleType : -1, m_AntiAliasing);
                return false;
            }
        }
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
        HRESULT hr;
        {
            bmvr::Hl2vrMsaaScopeGuard msaa(UseVrMsaa(), w, h);
            hr = device->CreateDepthStencilSurface(
                w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, depth, nullptr);
        }
        m_CreatingTextureID = Texture_None;
        if (FAILED(hr) || !*depth)
        {
            Game::logMsg("CreateDepthStencil eye id=%d hr=0x%08X", (int)id, (unsigned)hr);
            return false;
        }
        if (UseVrMsaa())
        {
            D3DSURFACE_DESC actual{};
            if (FAILED((*depth)->GetDesc(&actual))
                || static_cast<uint32_t>(actual.MultiSampleType) != m_AntiAliasing)
            {
                Game::logMsg("Eye MSAA depth id=%d got samples=%d (wanted %u)",
                    (int)id, (int)actual.MultiSampleType, m_AntiAliasing);
                return false;
            }
        }
        return true;
    };

    bool leftOk = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
    bool rightOk = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
    bool depthOk = createDepth(Texture_LeftEye, &m_D9LeftEyeDepthSurface)
        && createDepth(Texture_RightEye, &m_D9RightEyeDepthSurface);
    if (UseVrMsaa() && !(leftOk && rightOk && depthOk))
    {
        // Never mix sample counts: all four surfaces go back to 1x.
        Game::logMsg("Eye MSAA %u allocation failed; eyes stay 1x until restart", m_AntiAliasing);
        m_EyeMsaaAllocFailed = true;
        m_AntiAliasing = 0;
        leftOk = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
        rightOk = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
        depthOk = createDepth(Texture_LeftEye, &m_D9LeftEyeDepthSurface)
            && createDepth(Texture_RightEye, &m_D9RightEyeDepthSurface);
    }
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
    if (!UseVrMsaa())
    {
        // MSAA switched off in the panel: the 1x submit copies are not used.
        ReleaseT(m_D9LeftEyeSubmitSurface);
        ReleaseT(m_D9RightEyeSubmitSurface);
        ReleaseT(m_D9LeftEyeSubmitTexture);
        ReleaseT(m_D9RightEyeSubmitTexture);
    }
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
            ReleaseT(m_D9LeftEyeSubmitSurface);
            ReleaseT(m_D9RightEyeSubmitSurface);
            ReleaseT(m_D9LeftEyeSubmitTexture);
            ReleaseT(m_D9RightEyeSubmitTexture);
            const bool leftRetry = createEye(Texture_LeftEye, &m_D9LeftEyeTexture, &m_D9LeftEyeSurface, m_VKLeftEye);
            const bool rightRetry = createEye(Texture_RightEye, &m_D9RightEyeTexture, &m_D9RightEyeSurface, m_VKRightEye);
            leftOk = leftRetry;
            rightOk = rightRetry;
            m_EyeMsaaAllocFailed = true;
            depthOk = createDepth(Texture_LeftEye, &m_D9LeftEyeDepthSurface)
                && createDepth(Texture_RightEye, &m_D9RightEyeDepthSurface);
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
    BMVR_PROF("vr MirrorStereoToDesktopWindow");
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

    // DesktopEye (config, default right): the eye the window shows. Left
    // eye if the right surface is missing.
    IDirect3DSurface9* const mirrorEye = bmvr::g_DesktopEyeRight && m_D9RightEyeSurface
        ? m_D9RightEyeSurface : m_D9LeftEyeSurface;
    D3DSURFACE_DESC bbDesc{};
    UINT eyeW = 0, eyeH = 0;
    if (FAILED(bb->GetDesc(&bbDesc)) || !ResolveSurfaceSize(mirrorEye, eyeW, eyeH)
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

    if (bmvr::g_MirrorBothEyes && m_D9RightEyeSurface)
    {
        // Tests: both whole eye textures side by side (left half / right half),
        // the images the helper is handed, unscaled in content.
        const RECT halves[2] = {
            { 0, 0, static_cast<LONG>(bbDesc.Width / 2), static_cast<LONG>(bbDesc.Height) },
            { static_cast<LONG>(bbDesc.Width / 2), 0, static_cast<LONG>(bbDesc.Width), static_cast<LONG>(bbDesc.Height) } };
        IDirect3DSurface9* eyes[2] = { m_D9LeftEyeSurface, m_D9RightEyeSurface };
        HRESULT hrs[2] = { E_FAIL, E_FAIL };
        m_CaptureReentry = true;
        for (int e = 0; e < 2; ++e)
            hrs[e] = StretchRectKeepDeviceState(device, eyes[e], nullptr, dest, &halves[e], D3DTEXF_LINEAR);
        m_CaptureReentry = false;
        if (dest == m_D9DesktopMirrorSurface && SUCCEEDED(hrs[0]))
            m_DesktopMirrorHasImage.store(true, std::memory_order_release);
        static int s_bothLog;
        if (s_bothLog < 4 || FAILED(hrs[0]) || FAILED(hrs[1]))
        {
            Game::logMsg("Desktop mirror both eyes %ux%u -> %ux%u hr=0x%08X/0x%08X",
                eyeW, eyeH, bbDesc.Width, bbDesc.Height, (unsigned)hrs[0], (unsigned)hrs[1]);
            ++s_bothLog;
        }
        bb->Release();
        device->Release();
        return;
    }

    m_CaptureReentry = true;
    HRESULT hr = StretchRectKeepDeviceState(
        device, mirrorEye, srcPtr, dest, nullptr, D3DTEXF_LINEAR);
    if (FAILED(hr))
        hr = StretchRectKeepDeviceState(
            device, mirrorEye, srcPtr, dest, nullptr, D3DTEXF_NONE);
    m_CaptureReentry = false;
    if (SUCCEEDED(hr) && dest == m_D9DesktopMirrorSurface)
        m_DesktopMirrorHasImage.store(true, std::memory_order_release);

    static int s_mirrorLog;
    if (s_mirrorLog < 4 || FAILED(hr))
    {
        Game::logMsg("Desktop cover-crop %s eye %ux%u src=%d,%d %ux%u -> %s %ux%u hr=0x%08X",
            mirrorEye == m_D9LeftEyeSurface ? "left" : "right",
            eyeW, eyeH, src.left, src.top, cropW, cropH,
            dest == bb ? "BB" : "offscreen", bbDesc.Width, bbDesc.Height, (unsigned)hr);
        ++s_mirrorLog;
    }

    bb->Release();
    device->Release();
}

void VR::DrawToastOnEyeSurface(IDirect3DSurface9* eyeSurf, int stereoEye)
{
    BMVR_PROF("vr DrawToastOnEyeSurface");
    if (!eyeSurf || !g_D3DVR9 || !ToastVisible())
        return;
    GpuTimerScope gpuScope(g_GpuTimer.device, "mod toast");
    D3DSURFACE_DESC desc{};
    if (FAILED(eyeSurf->GetDesc(&desc)) || desc.Width < 64 || desc.Height < 64)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved)
    {
        device->Release();
        return;
    }
    IDirect3DSurface9* prevRt = nullptr;
    IDirect3DSurface9* prevDs = nullptr;
    device->GetRenderTarget(0, &prevRt);
    device->GetDepthStencilSurface(&prevDs);
    if (SUCCEEDED(device->SetRenderTarget(0, eyeSurf)))
    {
        device->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.f, 1.f };
        device->SetViewport(&vp);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        Vector fwd, right, up;
        GetViewBasis(&fwd, &right, &up);
        const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        const Vector eyeOrig = (stereoEye == 1) ? GetViewOriginLeft(body)
            : (stereoEye == 2) ? GetViewOriginRight(body) : GetViewOrigin(body);
        DrawToast(device, stereoEye, desc.Width, desc.Height, eyeOrig, fwd, right, up);
    }
    saved->Apply();
    saved->Release();
    if (prevRt)
    {
        device->SetRenderTarget(0, prevRt);
        prevRt->Release();
    }
    device->SetDepthStencilSurface(prevDs);
    if (prevDs)
        prevDs->Release();
    device->Release();
}

bool VR::BlitHmdViewFromBackbuffer(IDirect3DSurface9* dst, bool flushGpu)
{
    BMVR_PROF("vr BlitHmdViewFromBackbuffer");
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
    const bool squashFull = ShouldHwndBlitStereo();
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
    if (m_PauseHudSkipVgui.load(std::memory_order_acquire))
    {
        if (m_D9HUDPublishSurface && !m_VKHUD.m_VRTexture.handle)
            FillSharedTexture(m_D9HUDPublishSurface, m_VKHUD);
        m_PauseHudAlphaOverridden = false;
        return;
    }
    // HL2VR RenderHUD / DrawMainMenu: OverrideAlphaWriteEnable so VGUI
    // writes destination alpha. Empty pixels stay the D3D clear (0,0,0,0).
    if (m_D9HUDPublishSurface && !m_VKHUD.m_VRTexture.handle)
        FillSharedTexture(m_D9HUDPublishSurface, m_VKHUD);
    SehOverrideAlphaWrite(ctx, true, true);
    m_PauseHudAlphaOverridden = (ctx != nullptr);
}

void VR::FinishPauseHudExtraPaint(IMatRenderContext* ctx)
{
    // viewrender.cpp / vrviewrender.cpp restore this after HUD. Leaving it
    // on made every later stereo pass write alpha (50fps areas dropped to 35).
    if (!m_PauseHudAlphaOverridden && (!ctx || m_PauseHudSkipVgui.load(std::memory_order_acquire)))
        return;
    SehOverrideAlphaWrite(ctx, false, true);
    m_PauseHudAlphaOverridden = false;
}

// VGUI paints the pause layer with SRCALPHA/INVSRCALPHA on all four
// channels, so the layer's alpha ends up a*a (+ the layers below scaled by
// 1-a): a 0.8 dialog lands at 0.64 and SteamVR shows the world through it,
// worst for the stacked Video -> Advanced modal (2026-10-27). One quad over
// the alpha channel only, blended DESTALPHA/INVDESTALPHA with source alpha
// 1, gives a + a(1-a) = 1-(1-a)^2: 0 and 1 stay, 0.64 -> 0.87. One pass
// was still too see-through (user), so it repeats: 3 passes = 1-(1-a)^8.
//
// Stacked dialogs (Video -> Advanced over the options window) paint colour
// but leave the alpha of the window below them (user: the passes changed the
// options window, not the Advanced one). So first alpha = max(a, 8 * max(rgb))
// from a copy of the layer: anything a dialog painted, even dark grey, turns
// opaque; untouched pixels (cleared 0,0,0,0) stay transparent.
struct LayerShaderCache
{
    IDirect3DDevice9* device = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    bool failed = false;
};

static IDirect3DPixelShader9* CompileLayerShader(IDirect3DDevice9* device, LayerShaderCache& cache,
    const char* src, size_t srcLen, const char* name)
{
    IDirect3DPixelShader9*& s_ps = cache.ps;
    if (cache.device == device && (s_ps || cache.failed))
        return s_ps;
    cache.device = device;
    s_ps = nullptr;
    cache.failed = true;
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    if (!dll)
        dll = LoadLibraryA("d3dcompiler_43.dll");
    auto compile = dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    if (!compile)
    {
        Game::logMsg("%s: D3DCompile missing", name);
        return nullptr;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (SUCCEEDED(compile(src, srcLen, name, nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &err)) && code)
    {
        if (SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &s_ps)))
            cache.failed = false;
    }
    else
        Game::logMsg("%s: shader compile failed: %s", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    if (code) code->Release();
    if (err) err->Release();
    Game::logMsg("%s: shader %s", name, s_ps ? "ready" : "unavailable");
    return s_ps;
}

static IDirect3DPixelShader9* PauseAlphaFromColourShader(IDirect3DDevice9* device)
{
    static LayerShaderCache s_cache;
    static const char kSrc[] =
        "sampler2D s : register(s0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
        "  float4 c = tex2D(s, uv);\n"
        "  float m = max(c.r, max(c.g, c.b));\n"
        "  return float4(0, 0, 0, saturate(max(c.a, m * 8.0)));\n"
        "}\n";
    return CompileLayerShader(device, s_cache, kSrc, sizeof(kSrc) - 1, "pause_alpha.hlsl");
}

// Title layer (intro credits, chapter/location text): VGUI paints onto the
// cleared (0,0,0,0) RT with SRCALPHA (normal fonts) or SRCALPHA/ONE (Black
// Mesa's credit fonts are additive), so colour = c*a (premultiplied) and
// alpha = a*a. The pause menu's opaque boost made faint and fading credits
// opaque c*a: dark grey-brown instead of orange (user, 2026-11-17). Dividing
// the colour back to straight alpha left near-white rims on letter edges
// (edge alpha comes out lower than a*a), which the helper's and runtime's
// filtering spread (white speckle, user). So the colour stays premultiplied
// as VGUI wrote it, alpha = sqrt(a*a), and the layer is published with
// L4D2VR_OPENXR_OVERLAY_FLAG_PREMULTIPLIED: world*(1-a) + c*a, as flat.
static IDirect3DPixelShader9* TitlePremultipliedShader(IDirect3DDevice9* device)
{
    static LayerShaderCache s_cache;
    static const char kSrc[] =
        "sampler2D s : register(s0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
        "  float4 c = tex2D(s, uv);\n"
        "  return float4(c.rgb, sqrt(c.a));\n"
        "}\n";
    return CompileLayerShader(device, s_cache, kSrc, sizeof(kSrc) - 1, "title_alpha.hlsl");
}

// Flat Black Mesa does not show the HUD as VGUI painted it. With hud_post 1
// (default) client 0x267c40 paints the HUD a second time into _rt_Hud and
// adds a "retro" pass over the frame: dev/hud_downsample (downsample_nohdr,
// 4 bilinear taps, pow(c, $BLOOMEXP 6) * luma) to _rt_SmallFB0 at 1/4 size,
// dev/hud_blur_x / dev/hud_blur_y (13-tap Gaussian, Y pass scaled by
// hud_post_bloom 4), then bms_retroscanlines_ps20b, additive:
//   hud * scan * hud_post_scanline_influence (0.5)
//   + bloom * hud_post_bloom_influence (8) * scan   (scanline bloom influence 1)
// with scan = dev/hud_scanline (rows alternate 0 / 202/255), tiled once per
// screen row ($C0_Y = height / hud_scanline_size 128). That is the glow and
// the interlaced look of the intro logo, credit names and chapter titles. The
// title layer took only the VGUI paint (user, 2026-11-23: logo without the
// effect, names grey). These passes rebuild it on the title layer, with the
// blur radius scaled from the 1080-row screen to the layer height (the title
// panels are fitted to the layer for the paint).
static IDirect3DPixelShader9* TitleBloomDownsampleShader(IDirect3DDevice9* device)
{
    static LayerShaderCache s_cache;
    static const char kSrc[] =
        "sampler2D s : register(s0);\n"
        "float4 k : register(c0);\n"
        "float4 m : register(c2);\n"
        "float3 f(float3 c) { return pow(max(c, 1e-5), 6.0) * dot(c, float3(0.3, 0.59, 0.11)); }\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
        "  float3 r = f(tex2D(s, uv + float2(-k.x, -k.y)).rgb) + f(tex2D(s, uv + float2(k.x, -k.y)).rgb)\n"
        "    + f(tex2D(s, uv + float2(-k.x, k.y)).rgb) + f(tex2D(s, uv + float2(k.x, k.y)).rgb);\n"
        "  if (uv.x >= m.x && uv.x <= m.z && uv.y >= m.y && uv.y <= m.w) r = 0;\n"
        "  return float4(r * 0.25, 1);\n"
        "}\n";
    return CompileLayerShader(device, s_cache, kSrc, sizeof(kSrc) - 1, "title_bloom_down.hlsl");
}

static IDirect3DPixelShader9* TitleBloomBlurShader(IDirect3DDevice9* device)
{
    static LayerShaderCache s_cache;
    // Source blurfilter ($KERNEL 1): tap offsets in texels and weights.
    static const char kSrc[] =
        "sampler2D s : register(s0);\n"
        "float4 k : register(c0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
        "  float2 d = k.xy;\n"
        "  float3 r = tex2D(s, uv).rgb * 0.2013;\n"
        "  r += (tex2D(s, uv + d * 1.3366).rgb + tex2D(s, uv - d * 1.3366).rgb) * 0.2185;\n"
        "  r += (tex2D(s, uv + d * 3.4295).rgb + tex2D(s, uv - d * 3.4295).rgb) * 0.0821;\n"
        "  r += (tex2D(s, uv + d * 5.4264).rgb + tex2D(s, uv - d * 5.4264).rgb) * 0.0461;\n"
        "  r += (tex2D(s, uv + d * 7.4359).rgb + tex2D(s, uv - d * 7.4359).rgb) * 0.0262;\n"
        "  r += (tex2D(s, uv + d * 9.4436).rgb + tex2D(s, uv - d * 9.4436).rgb) * 0.0162;\n"
        "  r += (tex2D(s, uv + d * 11.4401).rgb + tex2D(s, uv - d * 11.4401).rgb) * 0.0102;\n"
        "  return float4(r * k.z, 1);\n"
        "}\n";
    return CompileLayerShader(device, s_cache, kSrc, sizeof(kSrc) - 1, "title_bloom_blur.hlsl");
}

// c0 = (layer height / 2, glow on, 0, 0); c1 = (scanline influence,
// bloom influence, scanline value, 0); c2 = uv rect without glow (the
// caption box; empty = none): there the output equals the no-glow shader. Colour stays premultiplied; alpha =
// max(text alpha, brightest channel) so the glow (no alpha of its own) is
// kept. Flat is additive (alpha 0 here), but SteamVR showed nothing at all
// for an alpha-0 premultiplied quad (user, 2026-11-23: logo and names gone).
static IDirect3DPixelShader9* TitleCombineShader(IDirect3DDevice9* device)
{
    static LayerShaderCache s_cache;
    static const char kSrc[] =
        "sampler2D s : register(s0);\n"
        "sampler2D b : register(s1);\n"
        "float4 k : register(c0);\n"
        "float4 g : register(c1);\n"
        "float4 m : register(c2);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
        "  float4 c = tex2D(s, uv);\n"
        "  float on = (uv.x >= m.x && uv.x <= m.z && uv.y >= m.y && uv.y <= m.w) ? 0.0 : k.y;\n"
        "  float scan = frac(uv.y * k.x) >= 0.5 ? g.z : 0.0;\n"
        "  float3 bl = tex2D(b, uv).rgb;\n"
        "  float3 rgb = c.rgb + on * scan * (c.rgb * g.x + bl * g.y);\n"
        "  rgb = saturate(rgb);\n"
        "  float a = max(sqrt(c.a), on * max(rgb.r, max(rgb.g, rgb.b)));\n"
        "  return float4(rgb, a);\n"
        "}\n";
    return CompileLayerShader(device, s_cache, kSrc, sizeof(kSrc) - 1, "title_combine.hlsl");
}

static IDirect3DTexture9* PauseLayerCopy(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc)
{
    return CachedPauseCopy(g_PauseLayerCopyTex, device, desc.Width, desc.Height, desc.Format);
}

// The VR layer is a second VGUI paint, and in it the windows come out in
// the wrong order: the Options window lands over Video -> Advanced, so both
// windows' contents show mixed ("see-through"; VR View capture 2026-10-27).
// The desktop frame the game is presenting has the right order and look,
// with nothing but black around the menu while paused. Take the colour from
// it and keep the layer's alpha (where any window is).
static void PauseLayerColourFromBackbuffer(IDirect3DDevice9* device, IDirect3DSurface9* surf)
{
    D3DSURFACE_DESC desc{};
    if (!device || !surf || FAILED(surf->GetDesc(&desc)))
        return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
    D3DSURFACE_DESC bbDesc{};
    IDirect3DTexture9* copy = nullptr;
    IDirect3DSurface9* copySurf = nullptr;
    if (SUCCEEDED(bb->GetDesc(&bbDesc)))
        copy = CachedPauseCopy(g_PauseFrameCopyTex, device, bbDesc.Width, bbDesc.Height, D3DFMT_A8R8G8B8);
    if (copy && SUCCEEDED(copy->GetSurfaceLevel(0, &copySurf))
        && FAILED(device->StretchRect(bb, nullptr, copySurf, nullptr, D3DTEXF_POINT)))
    {
        copySurf->Release();
        copySurf = nullptr;
    }
    bb->Release();
    if (!copySurf)
    {
        static int s_fail;
        if (s_fail++ < 3)
            Game::logMsg("Pause layer: backbuffer copy failed (bb %ux%u, layer %ux%u)", bbDesc.Width, bbDesc.Height, desc.Width, desc.Height);
        return;
    }
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved)
    {
        copySurf->Release();
        return;
    }
    IDirect3DSurface9* prevRt = nullptr;
    IDirect3DSurface9* prevDs = nullptr;
    device->GetRenderTarget(0, &prevRt);
    device->GetDepthStencilSurface(&prevDs);
    if (SUCCEEDED(device->SetRenderTarget(0, surf)))
    {
        device->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.f, 1.f };
        device->SetViewport(&vp);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        device->SetTexture(0, copy);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        struct VT { float x, y, z, rhw; float u, v; };
        const float w = static_cast<float>(desc.Width) - 0.5f, h = static_cast<float>(desc.Height) - 0.5f;
        const VT tq[4] = { { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f }, { w, -0.5f, 0.f, 1.f, 1.f, 0.f },
            { -0.5f, h, 0.f, 1.f, 0.f, 1.f }, { w, h, 0.f, 1.f, 1.f, 1.f } };
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(VT));
    }
    saved->Apply();
    saved->Release();
    if (prevRt)
    {
        device->SetRenderTarget(0, prevRt);
        prevRt->Release();
    }
    device->SetDepthStencilSurface(prevDs);
    if (prevDs)
        prevDs->Release();
    copySurf->Release();
    static int s_ok;
    if (s_ok++ < 2)
        Game::logMsg("Pause layer: colour from the presented frame %ux%u -> layer %ux%u", bbDesc.Width, bbDesc.Height, desc.Width, desc.Height);
}

static void FixPauseLayerAlpha(IDirect3DDevice9* device, IDirect3DSurface9* surf, int passes)
{
    D3DSURFACE_DESC desc{};
    if (!device || !surf || FAILED(surf->GetDesc(&desc)))
        return;
    IDirect3DPixelShader9* fromColour = PauseAlphaFromColourShader(device);
    IDirect3DTexture9* copy = fromColour ? PauseLayerCopy(device, desc) : nullptr;
    IDirect3DSurface9* copySurf = nullptr;
    if (copy && FAILED(copy->GetSurfaceLevel(0, &copySurf)))
        copySurf = nullptr;
    if (copySurf && FAILED(device->StretchRect(surf, nullptr, copySurf, nullptr, D3DTEXF_POINT)))
    {
        copySurf->Release();
        copySurf = nullptr;
    }
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved)
        return;
    IDirect3DSurface9* prevRt = nullptr;
    IDirect3DSurface9* prevDs = nullptr;
    device->GetRenderTarget(0, &prevRt);
    device->GetDepthStencilSurface(&prevDs);
    if (SUCCEEDED(device->SetRenderTarget(0, surf)))
    {
        device->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.f, 1.f };
        device->SetViewport(&vp);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVDESTALPHA);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        struct V { float x, y, z, rhw; D3DCOLOR c; };
        const float w = static_cast<float>(desc.Width) - 0.5f, h = static_cast<float>(desc.Height) - 0.5f;
        const D3DCOLOR white = D3DCOLOR_ARGB(255, 255, 255, 255);
        const V quad[4] = { { -0.5f, -0.5f, 0.f, 1.f, white }, { w, -0.5f, 0.f, 1.f, white },
            { -0.5f, h, 0.f, 1.f, white }, { w, h, 0.f, 1.f, white } };
        if (copySurf)
        {
            struct VT { float x, y, z, rhw; float u, v; };
            const VT tq[4] = { { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f }, { w, -0.5f, 0.f, 1.f, 1.f, 0.f },
                { -0.5f, h, 0.f, 1.f, 0.f, 1.f }, { w, h, 0.f, 1.f, 1.f, 1.f } };
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetTexture(0, copy);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            device->SetPixelShader(fromColour);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(VT));
            device->SetPixelShader(nullptr);
            device->SetTexture(0, nullptr);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            copySurf->Release();
        }
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        for (int i = 0; i < passes; ++i)
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    }
    else if (copySurf)
        copySurf->Release();
    saved->Apply();
    saved->Release();
    if (prevRt)
    {
        device->SetRenderTarget(0, prevRt);
        prevRt->Release();
    }
    device->SetDepthStencilSurface(prevDs);
    if (prevDs)
        prevDs->Release();
}

// One full-target quad with the bound pixel shader (state saved by the caller).
// Boot-up part rects, written on the game thread (title paint), read where the
// layer is finished (material thread with multicore): two slots + index.
static BootHudRects g_BootHudRects[2];
static std::atomic<int> g_BootHudRectsSlot{ 0 };

void PublishBootHudRects(const BootHudRects& r)
{
    const int next = 1 - g_BootHudRectsSlot.load(std::memory_order_relaxed);
    g_BootHudRects[next] = r;
    g_BootHudRectsSlot.store(next, std::memory_order_release);
}

// Copies the finished layer (surf) into copySurf with the boot-up sides moved
// toward the centre: the full-height strip left of the centred parts moves
// right, the strip right of them moves left. Strips, not part rects, because
// the boot panel draws the suit figure itself (not a child; part rects alone
// lost it, 2026-11-24). Order: centre band, side strips, then the centred
// parts again (a moved strip reaches into the band). False = nothing to move.
static bool BootHudMoveParts(IDirect3DDevice9* device, IDirect3DSurface9* surf, IDirect3DSurface9* copySurf,
    const D3DSURFACE_DESC& desc)
{
    const BootHudRects& r = g_BootHudRects[g_BootHudRectsSlot.load(std::memory_order_acquire)];
    if (r.count <= 0 || r.panelW <= 0 || r.panelH <= 0)
        return false;
    int leftEnd = -1, rightStart = r.panelW + 1, inset = 0;
    for (int i = 0; i < r.count; ++i)
    {
        const int* q = r.rects[i];
        if (q[4] > 0)
            leftEnd = (std::max)(leftEnd, q[0] + q[2]);
        else if (q[4] < 0)
            rightStart = (std::min)(rightStart, q[0]);
        if (q[4] != 0)
            inset = q[4] > 0 ? q[4] : -q[4];
    }
    if (!inset || FAILED(device->ColorFill(copySurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0))))
        return false;
    const float sx = static_cast<float>(desc.Width) / r.panelW, sy = static_cast<float>(desc.Height) / r.panelH;
    const LONG W = static_cast<LONG>(desc.Width), H = static_cast<LONG>(desc.Height);
    const LONG d = static_cast<LONG>(inset * sx);
    const LONG l1 = leftEnd >= 0 ? (std::min)(W - d, static_cast<LONG>(leftEnd * sx)) : 0;
    const LONG r0 = rightStart <= r.panelW ? (std::max)(d, static_cast<LONG>(rightStart * sx)) : W;
    int copies = 0;
    auto blit = [&](RECT src, LONG dx) {
        if (src.right <= src.left || src.bottom <= src.top)
            return;
        RECT dst{ src.left + dx, src.top, src.right + dx, src.bottom };
        if (SUCCEEDED(device->StretchRect(surf, &src, copySurf, &dst, D3DTEXF_POINT)))
            ++copies;
    };
    blit(RECT{ l1, 0, r0, H }, 0);
    if (l1 > 0)
        blit(RECT{ 0, 0, l1, H }, d);
    if (r0 < W)
        blit(RECT{ r0, 0, W, H }, -d);
    for (int i = 0; i < r.count; ++i)
    {
        const int* q = r.rects[i];
        if (q[4] != 0)
            continue;
        RECT src{ (std::max)(0L, static_cast<LONG>(q[0] * sx)), (std::max)(0L, static_cast<LONG>(q[1] * sy)),
            (std::min)(W, static_cast<LONG>((q[0] + q[2]) * sx)), (std::min)(H, static_cast<LONG>((q[1] + q[3]) * sy)) };
        blit(src, 0);
    }
    static int s_log;
    if (s_log < 3)
    {
        Game::logMsg("Boot HUD: layer sides moved %ld px (left strip 0-%ld, right strip %ld-%ld), %d copies",
            static_cast<long>(d), static_cast<long>(l1), static_cast<long>(r0), static_cast<long>(W), copies);
        ++s_log;
    }
    return true;
}

static void TitleLayerQuad(IDirect3DDevice9* device, IDirect3DSurface9* target, UINT w, UINT h)
{
    if (FAILED(device->SetRenderTarget(0, target)))
        return;
    D3DVIEWPORT9 vp{ 0, 0, w, h, 0.f, 1.f };
    device->SetViewport(&vp);
    struct VT { float x, y, z, rhw; float u, v; };
    const float fw = static_cast<float>(w) - 0.5f, fh = static_cast<float>(h) - 0.5f;
    const VT tq[4] = { { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f }, { fw, -0.5f, 0.f, 1.f, 1.f, 0.f },
        { -0.5f, fh, 0.f, 1.f, 0.f, 1.f }, { fw, fh, 0.f, 1.f, 1.f, 1.f } };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(VT));
}

static void TitleLayerSampler(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* tex, bool linear)
{
    device->SetTexture(stage, tex);
    const DWORD f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    device->SetSamplerState(stage, D3DSAMP_MINFILTER, f);
    device->SetSamplerState(stage, D3DSAMP_MAGFILTER, f);
    device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

static void TitleLayerPremultiplied(IDirect3DDevice9* device, IDirect3DSurface9* surf)
{
    D3DSURFACE_DESC desc{};
    if (!device || !surf || FAILED(surf->GetDesc(&desc)))
        return;
    // No glow on the HEV / long jump boot-up text: brighter and harder to read
    // than flat with it (user headset test 2026-11-24); same for captions on
    // their own (2026-11-25). The intro logo keeps it.
    const bool glow = bmvr::g_TitleGlow
        && !g_BootHudRects[g_BootHudRectsSlot.load(std::memory_order_acquire)].noGlow;
    IDirect3DPixelShader9* ps = glow ? TitleCombineShader(device) : TitlePremultipliedShader(device);
    IDirect3DPixelShader9* psDown = glow ? TitleBloomDownsampleShader(device) : nullptr;
    IDirect3DPixelShader9* psBlur = glow ? TitleBloomBlurShader(device) : nullptr;
    IDirect3DTexture9* copy = ps ? PauseLayerCopy(device, desc) : nullptr;
    IDirect3DSurface9* copySurf = nullptr;
    if (!copy || FAILED(copy->GetSurfaceLevel(0, &copySurf)) || !copySurf)
        return;
    if (!BootHudMoveParts(device, surf, copySurf, desc)
        && FAILED(device->StretchRect(surf, nullptr, copySurf, nullptr, D3DTEXF_POINT)))
    {
        copySurf->Release();
        return;
    }
    copySurf->Release();
    const UINT bw = desc.Width / 4 > 0 ? desc.Width / 4 : 1, bh = desc.Height / 4 > 0 ? desc.Height / 4 : 1;
    IDirect3DTexture9* bloom0 = (psDown && psBlur) ? CachedPauseCopy(g_TitleBloomTex[0], device, bw, bh, D3DFMT_A8R8G8B8) : nullptr;
    IDirect3DTexture9* bloom1 = bloom0 ? CachedPauseCopy(g_TitleBloomTex[1], device, bw, bh, D3DFMT_A8R8G8B8) : nullptr;
    IDirect3DSurface9* b0 = nullptr;
    IDirect3DSurface9* b1 = nullptr;
    if (bloom1 && (FAILED(bloom0->GetSurfaceLevel(0, &b0)) || FAILED(bloom1->GetSurfaceLevel(0, &b1))))
        bloom1 = nullptr;
    const bool doGlow = glow && bloom1 && b0 && b1;
    // Caption box sharing the layer with a glowing title: uv rect kept out of
    // the bloom source and the combine (empty rect = none).
    const BootHudRects& capRects = g_BootHudRects[g_BootHudRectsSlot.load(std::memory_order_acquire)];
    const float noRect[4] = { 2.f, 2.f, -1.f, -1.f };
    const float* capMask = capRects.capMask ? capRects.capRect : noRect;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved)
    {
        if (b0) b0->Release();
        if (b1) b1->Release();
        return;
    }
    IDirect3DSurface9* prevRt = nullptr;
    IDirect3DSurface9* prevDs = nullptr;
    device->GetRenderTarget(0, &prevRt);
    device->GetDepthStencilSurface(&prevDs);
    device->SetDepthStencilSurface(nullptr);
    device->SetVertexShader(nullptr);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
        | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    if (doGlow)
    {
        // Downsample: 4 bilinear taps 1 layer texel off the 4x4 block centre.
        const float down[4] = { 1.f / desc.Width, 1.f / desc.Height, 0.f, 0.f };
        device->SetPixelShader(psDown);
        device->SetPixelShaderConstantF(0, down, 1);
        device->SetPixelShaderConstantF(2, capMask, 1);   // caption box: no bloom source
        TitleLayerSampler(device, 0, copy, true);
        TitleLayerQuad(device, b0, bw, bh);
        // Blur radius: BM's offsets are 1/4-screen texels at 1080 rows.
        const float scale = static_cast<float>(desc.Height) / 1080.f;
        const float bx[4] = { scale / bw, 0.f, 1.f, 0.f };
        device->SetPixelShader(psBlur);
        device->SetPixelShaderConstantF(0, bx, 1);
        TitleLayerSampler(device, 0, bloom0, true);
        TitleLayerQuad(device, b1, bw, bh);
        const float by[4] = { 0.f, scale / bh, 4.f, 0.f };   // hud_post_bloom
        device->SetPixelShaderConstantF(0, by, 1);
        TitleLayerSampler(device, 0, bloom1, true);
        TitleLayerQuad(device, b0, bw, bh);
    }
    device->SetPixelShader(ps);
    if (ps != TitlePremultipliedShader(device))
    {
        const float k[4] = { desc.Height * 0.5f, doGlow ? 1.f : 0.f, 0.f, 0.f };
        const float g[4] = { 0.5f, 8.f, 202.f / 255.f, 0.f };
        device->SetPixelShaderConstantF(0, k, 1);
        device->SetPixelShaderConstantF(1, g, 1);
        device->SetPixelShaderConstantF(2, capMask, 1);
    }
    TitleLayerSampler(device, 0, copy, false);
    TitleLayerSampler(device, 1, doGlow ? static_cast<IDirect3DBaseTexture9*>(bloom0) : copy, true);
    TitleLayerQuad(device, surf, desc.Width, desc.Height);
    saved->Apply();
    saved->Release();
    if (b0) b0->Release();
    if (b1) b1->Release();
    if (prevRt)
    {
        device->SetRenderTarget(0, prevRt);
        prevRt->Release();
    }
    device->SetDepthStencilSurface(prevDs);
    if (prevDs)
        prevDs->Release();
    static int s_log;
    static int s_lastMode = -1;
    const int mode = doGlow ? 1 : 0;
    if (s_log < 8 && mode != s_lastMode)
    {
        ++s_log;
        s_lastMode = mode;
        Game::logMsg("Title layer: %ux%u glow=%d (bloom %ux%u)", desc.Width, desc.Height,
            doGlow ? 1 : 0, bw, bh);
    }
}

void VR::NoteHudPainted()
{
    // Copy the finished layer to what the helper reads (see m_D9HUDPublishSurface).
    if (m_D9HUDSurface && m_D9HUDPublishSurface)
    {
        IDirect3DDevice9* device = nullptr;
        if (SUCCEEDED(m_D9HUDSurface->GetDevice(&device)) && device)
        {
            const HRESULT hr = device->StretchRect(m_D9HUDSurface, nullptr, m_D9HUDPublishSurface, nullptr, D3DTEXF_POINT);
            static int s_fail;
            if (FAILED(hr) && s_fail++ < 4)
                Game::logMsg("HUD publish copy failed hr=0x%08X", (unsigned)hr);
            device->Release();
        }
    }
    m_HudPaintedKind.store(m_HudPaintTitle.load(std::memory_order_acquire) ? 1 : 0, std::memory_order_relaxed);
    m_HudPaintedThisFrame.store(true, std::memory_order_release);
}

bool VR::TakeHudPaintFor(int kind)
{
    if (m_HudPublishedKind != kind)
        m_HudOverlayHasImage = false;
    // Load end (menu / pause-menu load): the loading phase ends in a pause
    // phase of 5-40 ms while GameUI closes; its pause-menu paint flashed the
    // menu over the loading frame. In-game loads (LOADING card) go straight
    // to gameplay. Hold the menu layer for the first 300 ms of such a pause.
    if (kind == 0 && m_PauseFromLoading && m_VrPhase == VrPhase::Pause
        && GetTickCount() - m_VrPhaseSinceMs < 300u)
    {
        m_HudOverlayHasImage = false;
        if (m_HudPaintedThisFrame.exchange(false, std::memory_order_acq_rel))
        {
            static int s_log;
            if (s_log++ < 8)
                Game::logMsg("HUD layer: held a pause-menu paint at the load end (%lu ms into the pause)",
                    static_cast<unsigned long>(GetTickCount() - m_VrPhaseSinceMs));
        }
        return false;
    }
    if (!m_HudPaintedThisFrame.load(std::memory_order_acquire))
        return false;
    // Fresh pause: the first pause-menu paint(s) still carry the flat
    // menu's desktop-sized background (user 2026-11-19: one frame of it
    // centred at window size on every pause). Drop the first
    // bmvr_pause_skip_paints of each pause; the menu shows a frame later.
    // A menu that starts after 300 ms without a pause-menu paint counts as
    // fresh, in any phase: its first paints often land before the pause
    // phase is set (log 2026-11-19: only the first pause dropped any).
    if (kind == 0)
    {
        static DWORD s_lastMenuPaint = 0;
        static int s_dropped = 0;
        const DWORD nowMs = GetTickCount();
        if (s_lastMenuPaint == 0 || nowMs - s_lastMenuPaint > 300)
            s_dropped = 0;
        s_lastMenuPaint = nowMs;
        if (s_dropped < static_cast<int>(bmvr::g_PauseSkipPaints + 0.5f))
        {
            ++s_dropped;
            m_HudPaintedThisFrame.store(false, std::memory_order_release);
            m_HudOverlayHasImage = false;
            static int s_log;
            if (s_log++ < 8)
                Game::logMsg("HUD layer: dropped menu paint %d of a fresh menu (phase %d)", s_dropped,
                    static_cast<int>(m_VrPhase));
            return false;
        }
    }
    m_HudPaintedThisFrame.store(false, std::memory_order_release);
    const int painted = m_HudPaintedKind.load(std::memory_order_relaxed);
    if (painted != kind)
    {
        static int s_log;
        if (s_log++ < 8)
            Game::logMsg("HUD layer: dropped a %s paint for the %s layer",
                painted ? "title" : "pause-menu", kind ? "title" : "pause-menu");
        return false;
    }
    m_HudPublishedKind = kind;
    return true;
}

void VR::StampPauseOverlayCursor()
{
    if (!g_D3DVR9 || !m_D9HUDSurface)
        return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
        return;
    // Title layer (intro logo / chapter text): no frame colour, no pointer.
    const bool title = m_HudPaintTitle.load(std::memory_order_acquire);
    // Alpha first, from the menu's own paint; then the colour from the
    // presented frame (which keeps that alpha). The other way round the
    // alpha boost (8 x brightness) ran on the frame's colour, and for the
    // ~0.7 s Black Mesa's pause backdrop takes to fade in that frame is the
    // bright world: a window-sized opaque picture of it fading out over the
    // view (pause capture 2026-11-19: layer alpha mean 0.98 -> 0.02 over
    // 0.7 s; the eye images were clean).
    const int alphaPasses = static_cast<int>(bmvr::g_PauseMenuAlphaPasses + 0.5f);
    // New game UI: the menu is the QML layer, not VGUI (that paint is empty,
    // so the old alpha-from-paint path published a transparent overlay).
    const bool qml = !title && bmvr::NewUiWindow() && DrawNewUiLayerTo(device, m_D9HUDSurface);
    if (title)
        TitleLayerPremultiplied(device, m_D9HUDSurface);
    else if (alphaPasses > 0)
        FixPauseLayerAlpha(device, m_D9HUDSurface, alphaPasses > 6 ? 6 : alphaPasses);
    if (bmvr::g_PauseMenuColourFromFrame && !title && !qml)
        PauseLayerColourFromBackbuffer(device, m_D9HUDSurface);
    if (!title)
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
    // Colour/alpha fix + pointer on the material thread, in order with the
    // paint. Done at Present instead (other thread) it raced the next
    // frame's clear and repaint: flicker (2026-10-27).
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
    if (!m_OpenXrHelperBridgeActive)
        return;

    const UINT wantW = KnownWindowWidth();
    const UINT wantH = KnownWindowHeight();
    if (wantW < 640 || wantH < 360 || wantW > 4096 || wantH > 4096)
        return;

    auto sizeOk = [&]() -> bool {
        if (!m_D9HUDSurface || !m_D9HUDPublishSurface)
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
        if (!m_VKHUD.m_VRTexture.handle && m_D9HUDPublishSurface)
            FillSharedTexture(m_D9HUDPublishSurface, m_VKHUD);
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
        ReleaseT(m_D9HUDPublishSurface);
        ReleaseT(m_D9HUDPublishTexture);
        m_VKHUD = SharedTextureHolder{};
        m_HudOverlayReady = false;
        // Paint target: not shared. Publish copy: shared with the helper.
        HRESULT hr = device->CreateTexture(wantW, wantH, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_D9HUDTexture, nullptr);
        if (SUCCEEDED(hr) && m_D9HUDTexture)
        {
            m_CreatingTextureID = Texture_HUD;
            hr = device->CreateTexture(wantW, wantH, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_D9HUDPublishTexture, nullptr);
            m_CreatingTextureID = Texture_None;
        }
        if (FAILED(hr) || !m_D9HUDTexture || !m_D9HUDPublishTexture)
        {
            Game::logMsg("HUD D3D CreateTexture failed hr=0x%08X %ux%u",
                (unsigned)hr, wantW, wantH);
            ReleaseT(m_D9HUDTexture);
            ReleaseT(m_D9HUDPublishTexture);
            device->Release();
            return;
        }
        m_D9HUDTexture->GetSurfaceLevel(0, &m_D9HUDSurface);
        m_D9HUDPublishTexture->GetSurfaceLevel(0, &m_D9HUDPublishSurface);
        if (m_D9HUDPublishSurface && !m_VKHUD.m_VRTexture.handle)
            FillSharedTexture(m_D9HUDPublishSurface, m_VKHUD);
        Game::logMsg("HUD D3D RT %ux%u surf=%p publish=%p vk=%p",
            wantW, wantH, (void*)m_D9HUDSurface, (void*)m_D9HUDPublishSurface, m_VKHUD.m_VRTexture.handle);
    }

    if (m_D9HUDSurface && m_D9HUDPublishSurface && m_VKHUD.m_VRTexture.handle)
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
        if (m_D9HUDPublishSurface && SUCCEEDED(device->SetRenderTarget(0, m_D9HUDPublishSurface)))
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
    BMVR_PROF("vr BeginStereoEyeBlit");
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
    BMVR_PROF("vr ClearStereoEyeSurfaces");
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

    m_RefractEyeDone = nullptr;   // RefractOncePerEye: new eye frame
    // Eye MSAA: back to the multisampled surfaces for this eye's scene.
    if (UseVrMsaa())
    {
        GpuTimerScope scope(device, "msaa begin");
        bmvr::MsaaEyeBegin(device, color, depth);
    }

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
    BMVR_PROF("vr EndStereoEyeBlit");
    const bool ok = m_StereoEyeBlitOk;
    m_StereoEyeBlitActive = false;
    m_StereoEyeBlitDest = nullptr;
    return ok;
}

bool VR::StereoUnbindMatchesEye() const
{
    if (ShouldHwndBlitStereo())
        return false;
    const int dw = static_cast<int>(m_LastStereoBlitWidth) - static_cast<int>(m_RenderWidth);
    const int dh = static_cast<int>(m_LastStereoBlitHeight) - static_cast<int>(m_RenderHeight);
    return dw > -32 && dw < 32 && dh > -32 && dh < 32
        && m_LastStereoBlitWidth >= 640 && m_LastStereoBlitHeight >= 360;
}

void VR::FlushStereoBlitGpu()
{
    BMVR_PROF("vr FlushStereoBlitGpu");
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
    BMVR_PROF("vr BlitCurrentGameColorTo");
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

    UpdateSaveThumbnailCapture();
    if (m_OpenXrHelperBridgeActive)
    {
        TickOpenXrHelperWatchdog();
        ++m_OpenXrSubmitAttempts;
        if (bmvr::glowtrace::WantsStart())
        {
            char header[256];
            std::snprintf(header, sizeof(header),
                "glow trace: FrameCap mode %.0f applied fps_max %d, MulticoreMode %d, GlowHold %.2f, GlowSmooth %.2f, GlowHideMs %.0f, PixelVisPerEye %d",
                bmvr::g_FrameCapMode, m_FrameCapApplied, bmvr::g_MulticoreMode ? 1 : 0, bmvr::g_GlowHold,
                bmvr::g_GlowSmooth, bmvr::g_GlowHideMs, bmvr::g_PixelVisPerEye ? 1 : 0);
            bmvr::glowtrace::Start(header);
        }
        if (bmvr::glowtrace::Active())
        {
            bmvr::glowtrace::Event('P', 0);
            bmvr::glowtrace::PollFinish();
        }
        // Deferred publish: hand over any slot whose GPU copy has completed
        // since the last Present, before deciding whether to copy new eyes.
        PollOpenXrDeferredPublish();
        // Pause + alt-tab: Steam/NVIDIA overlay GL tears down while we still
        // StretchRect 1x eyes and TransferSurface the pause HUD. That closed
        // bms.exe with exitCode=1 and no bmvr_crash.txt (2026-09-13 and
        // 2026-09-20). Keep last compositor eyes for in-game pause only
        // (`WantPauseWorldOverlay`). Do not use IsMenuUp() here — that is
        // also true on the 2D main-menu panel. ICallQueue is already dead
        // (`MC Present fallback`) and the StretchRect is the crash.
        if (WantPauseWorldOverlay())
        {
            if (!m_AppActivated.load(std::memory_order_relaxed))
            {
                static int s_deactSkip;
                if (s_deactSkip < 4)
                {
                    Game::logMsg("OpenXR publish skipped (pause + HWND deactivated)");
                    ++s_deactSkip;
                }
                LogOpenXrPublishRate();
                return;
            }
            static int s_pauseHold;
            if (s_pauseHold < 4)
            {
                Game::logMsg("OpenXR pause: hold last eyes, HUD overlay only");
                ++s_pauseHold;
            }
            if (WantPauseWorldOverlay() && m_HudOverlayReady && m_D9HUDSurface)
            {
                if (TakeHudPaintFor(0))
                {
                    if (SUCCEEDED(SehTransferSurfaceNoWait(m_D9HUDPublishSurface)))
                    {
                        FillSharedTexture(m_D9HUDPublishSurface, m_VKHUD);
                        m_HudOverlayHasImage = true;
                    }
                    else
                    {
                        static int s_hudXfer;
                        if (s_hudXfer < 4)
                        {
                            Game::logMsg("OpenXR pause HUD TransferSurface failed");
                            ++s_hudXfer;
                        }
                    }
                    m_HudPaintedThisFrame.store(false, std::memory_order_release);
                }
                if (m_HudOverlayHasImage)
                {
                    const uint32_t overlayFrameId = m_OpenXrSubmitFrameId.fetch_add(1, std::memory_order_acq_rel);
                    PublishOpenXrHudOverlay(overlayFrameId);
                }
            }
            LogOpenXrPublishRate();
            return;
        }
        if (!m_AppActivated.load(std::memory_order_relaxed) && IsMenuUp())
        {
            static int s_menuDeactSkip;
            if (s_menuDeactSkip < 4)
            {
                Game::logMsg("OpenXR publish skipped (GameUI + HWND deactivated)");
                ++s_menuDeactSkip;
            }
            LogOpenXrPublishRate();
            return;
        }
        {
            const bool inGame = m_Game && m_Game->m_EngineClient
                && m_Game->m_EngineClient->IsInGame();
            if (inGame && m_GameplayEligible && !PassThroughWarmupDone() && !Want2dMenuPanel())
            {
                static int s_warmSkip;
                if (s_warmSkip < 4)
                {
                    Game::logMsg("OpenXR publish skipped during pass-through warmup (keep last compositor frame)");
                    ++s_warmSkip;
                }
                LogOpenXrPublishRate();
                return;
            }
        }
        // Skip Present copy only when the mat-thread functor actually queued
        // (or already copied). Returning here with no copy froze the HMD on
        // the last menu letterbox: Index SteamVR 2026-09-09 published=0/s
        // after bm_c1a0a while stereo still ran (mc=1, ICallQueue dead).
        if (McPipelineEnabled() && (m_McEyeCopyQueued || m_McEyeCopyDone))
        {
            LogOpenXrPublishRate();
            if (WantHudLayer())
            {
                if (m_HudOverlayReady && m_D9HUDSurface)
                {
                    if (TakeHudPaintFor(WantPauseWorldOverlay() ? 0 : 1))
                    {
                        g_D3DVR9->TransferSurface(m_D9HUDPublishSurface, FALSE);
                        FillSharedTexture(m_D9HUDPublishSurface, m_VKHUD);
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
        if (McPipelineEnabled())
        {
            ++m_McPresentFallbackCount;
            static int s_mcPresentFb;
            if (s_mcPresentFb < 8)
            {
                // On the per-eye path a Present now and then finds no eye copy
                // for this frame (Present runs faster than the game); the rate
                // is mcPresentFallback in "OpenXR publish rate", ~0/s is normal.
                // On the window-blit fallback nothing is ever queued.
                if (ShouldHwndBlitStereo())
                {
                    if (s_mcPresentFb == 0)
                        Game::logMsg("MC: no eye copies queued: the window-blit fallback renders the eyes "
                            "single-threaded, MulticoreMode has no effect (see 'Render path: FALLBACK')");
                    s_mcPresentFb = 8;
                }
                else
                {
                    Game::logMsg("MC Present without a queued eye copy (occasional is normal; see mcPresentFallback/s) queued=%d done=%d",
                        m_McEyeCopyQueued ? 1 : 0, m_McEyeCopyDone ? 1 : 0);
                    ++s_mcPresentFb;
                }
            }
        }
        // In-game load (no loading dialog): the game freezes for seconds after
        // two forced Presents; paint LOADING onto the eyes published now so the
        // helper holds the card instead of a silent frozen world. The first
        // such Present republishes the last pair even if nothing new was drawn.
        const bool loadingCard = bmvr::g_LoadingCard && m_VrPhase == VrPhase::Loading && m_DirectEyeSubmit && !PauseUiActive()
            && !Want2dMenuPanel() && m_D9LeftEyeSurface && m_D9RightEyeSurface;
        if (loadingCard)
        {
            if (!m_LoadingCardShown)
            {
                m_LoadingCardShown = true;
                m_LoadingCardForcePublish = true;
                ShowToast("LOADING", 120000);
                Game::logMsg("Loading card: in-game load, LOADING painted on the held eye pair");
            }
            if (UseVrMsaa() && g_D3DVR9)
            {
                IDirect3DDevice9* cardDev = nullptr;
                if (SUCCEEDED(g_D3DVR9->GetD3DDevice(&cardDev)) && cardDev)
                {
                    MsaaEyesTo1x(cardDev);
                    cardDev->Release();
                }
            }
            DrawToastOnEyeSurface(m_D9LeftEyeSurface, 1);
            DrawToastOnEyeSurface(m_D9RightEyeSurface, 2);
            if (m_LoadingCardForcePublish)
            {
                m_LoadingCardForcePublish = false;
                m_RenderedNewFrame.store(true, std::memory_order_release);
            }
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
        LogOpenXrPublishRate();
        const bool paused = m_Game && SehIsPaused(m_Game->m_EngineClient);
        const bool panel2d = Want2dMenuPanel();
        const bool stereoLayer = m_DirectEyeSubmit && !panel2d;
        static int s_stereoPresent;
        if (stereoLayer && s_stereoPresent < 4)
        {
            Game::logMsg("OpenXR stereo Present publish begin worldMatch=%d gbmatch=%d mc=%d",
                bmvr::OffscreenWorldMatchesEyes() ? 1 : 0,
                bmvr::UseGbMatchViewLock() ? 1 : 0,
                McPipelineEnabled() ? 1 : 0);
            ++s_stereoPresent;
        }
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
                MsaaEyesTo1x(copyDev);
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
        if (WantHudLayer() && !McPipelineEnabled())
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
                if (TakeHudPaintFor(0))
                {
                    g_D3DVR9->TransferSurface(m_D9HUDPublishSurface, FALSE);
                    FillSharedTexture(m_D9HUDPublishSurface, m_VKHUD);
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

}

void VR::UpdateTracking()
{
    BMVR_PROF("vr UpdateTracking");
    // Same as the controller flags (UpdateControllerTracking): no transient
    // "HMD pose invalid" that the other thread can see mid-update (the
    // hand/laser overlay skips its whole draw on it = hands gone a frame).
    struct ClearHmdIfNotPublished
    {
        VR* vr;
        bool published = false;
        ~ClearHmdIfNotPublished()
        {
            if (!published && bmvr::g_PoseFlagsNoBlink)
                vr->m_HmdPoseValid = false;
        }
    } hmdGuard{ this };
    if (!bmvr::g_PoseFlagsNoBlink)
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
        hmdGuard.published = true;
    }
    m_IpdScale = bmvr::g_IPDScale;
    UpdateControllerTracking(hmd);

    UpdateScopeZoomSmooth();
    UpdateScopeAimOffset();

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
    BMVR_PROF("vr Update");
    bmvr::sampler::CpuNote(1);
    bmvr::sampler::BudgetTick();
    if (!m_Game)
        return;

    if (!m_IsVREnabled)
        return;

    if (m_OpenXrHelperBridgeActive)
        TickOpenXrHelperWatchdog();

    PollMapFromEngine();
    const bool inGame = m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    UpdateVrPhase(inGame);
    TickPhaseDump();
    TickPoseDiag();
    // LevelInit names the map before IsInGame. Do not run AutoMatQueueMode,
    // pose, or input during that window — Present is about to stop for
    // material precache, and overnight Update work is not what unblocks it.
    if (m_GameplayEligible && !inGame)
        return;

    if (m_OpenXrHelperBridgeActive && !m_PosesWaitedThisFrame)
        ConsumeOpenXrTracking();


    // QoL cvars go through ICvar from RenderView (game thread). FindVar/SetValue
    // from this Present/DXVK path crashed ~13s after init (2026-08-26).
    // Do not UpdateAutoMatQueueMode from Present. GetThreadMode nested
    // materialsystem after the first stereo frame (x32dbg 2026-08-18).

    ++m_PresentTick;
    // MulticoreMode: Present runs on the render thread at the end of ITS
    // frame, which lags the main thread by a frame. Resetting here let the
    // main thread's next frame see "eyes drawn" + "poses waited" still set and
    // skip its whole view as a leftover (every other engine frame outdoors,
    // 2026-11-11). With bmvr_mc_frame_reset the main thread resets both at
    // its own frame end (Hooks::dClientPvsNotifierCheck).
    if (!(bmvr::g_McFrameReset && McPipelineEnabled()))
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
        bmvr::LogFaultStats();
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
        if (m_MenuPanelUpForCursor.load(std::memory_order_relaxed))
        {
            // Menu lag diagnosis: GetCursorPos calls/s answered with the VR
            // pointer vs passed through (main menu vs pause, hover vs not).
            const long hits = m_CursorHookHitsSec.exchange(0);
            const long pass = m_CursorHookPassSec.exchange(0);
            Game::logMsg("menu cursor hook %ld/%ld per %ums (pointer/real) live=%d hook=%d",
                hits, pass, dt, MenuPointerLive() ? 1 : 0, bmvr::g_MenuCursorHook ? 1 : 0);
        }
        s_fpsLogMs = nowMs;
        s_fpsLogTick = m_PresentTick;
    }
    if (m_GameplayEligible && inGame && m_EligiblePresents < 100000)
        ++m_EligiblePresents;
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryHmdFramebuffer())
        bmvr::EndRisky(L"hmd_fb");
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryDrawHud())
        bmvr::EndRisky(L"drawhud");
    if (m_GameplayEligible && inGame && m_EligiblePresents == 120 && bmvr::TryHmdNative())
        bmvr::EndRisky(L"hmd_native");

    if (!m_PosesWaitedThisFrame)
    {
        if (m_OpenXrHelperBridgeActive)
            UpdateTracking();
        else
            WaitPosesForStereoFrame(false);
    }
    SyncGameUiFromEngine();
    LatchMenuPanelIfNeeded();
    ProcessInput();
    if (!(bmvr::g_McFrameReset && McPipelineEnabled()))
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

void BmvrAuditNoteMaterial(const char* name)
{
    if (!name)
        name = "";
    if (GpuTimerDetail() && g_GpuTimer.device && GpuTimerThreadOk()
        && std::strncmp(name, t_AuditMaterial, sizeof(t_AuditMaterial) - 1) != 0)
    {
        static thread_local std::string s_group;
        GpuTimerMaterialGroup(name, s_group);
        if (s_group != g_GpuTimer.curMat)
        {
            g_GpuTimer.curMat = s_group;
            if (g_GpuTimer.curOverride.empty())
                GpuTimerRelabel(g_GpuTimer.device);
        }
    }
    std::strncpy(t_AuditMaterial, name, sizeof(t_AuditMaterial) - 1);
    t_AuditMaterial[sizeof(t_AuditMaterial) - 1] = 0;
}

std::string bmvr::FoveationStatusText()
{
    return FoveationStatusNow();
}

// ---- Manual magazines (VrMagReload): MP5 + glock ----
std::atomic<int> MagReload::g_Weapon{ MagReload::kWeaponNone };
std::atomic<int> MagReload::g_State{ MagReload::kLoaded };
std::atomic<bool> MagReload::g_SlideLocked{ false };
std::atomic<bool> MagReload::g_BoltGrab{ false };
std::atomic<bool> MagReload::g_NeedRack{ false };
std::atomic<unsigned> MagReload::g_DropTick{ 0 };
std::atomic<int> MagReload::g_DropFrom{ 0 };
std::atomic<int> MagReload::g_DisplayClip{ -1 };
std::atomic<int> MagReload::g_DisplayReserveExtra{ 0 };
std::atomic<const char*> MagReload::g_ScrubSeq{ nullptr };
std::atomic<float> MagReload::g_ScrubCycle{ 0.f };
std::atomic<bool> MagReload::g_ScrubActive{ false };
std::atomic<bool> MagReload::g_NearWell{ false };
std::atomic<float> MagReload::g_SeatDistCm{ 0.f };
std::atomic<float> MagReload::g_SeatAngleDeg{ 0.f };
std::atomic<bool> MagReload::g_NearBolt{ false };
std::atomic<float> MagReload::g_BoltPull{ 0.f };
std::atomic<float> MagReload::g_RpgInsert{ -1.f };
std::atomic<float> MagReload::g_LeftPalmLocal[3]{};
std::atomic<unsigned> MagReload::g_LeftPalmMs{ 0 };
std::atomic<float> MagReload::g_LeftPinchLocal[3]{};
std::atomic<unsigned> MagReload::g_LeftPinchMs{ 0 };
std::atomic<int> MagReload::g_LeftClipPose{ -1 };
std::atomic<unsigned> MagReload::g_LeftClipMs{ 0 };
std::atomic<float> MagReload::g_LeftClipWrist[12]{};
std::atomic<float> MagReload::g_LeftClipUnit{ 0.f };
std::atomic<unsigned> MagReload::g_SupportHandFromGunMs{ 0 };
std::atomic<unsigned> MagReload::g_SatchelRadioMs{ 0 };
std::atomic<unsigned> MagReload::g_SatchelBagMs{ 0 };
std::atomic<float> MagReload::g_GunWrist[2][12]{};
std::atomic<unsigned> MagReload::g_GunWristMs[2]{};
std::atomic<float> MagReload::g_LeftTunnelLocal[6]{};
std::atomic<unsigned> MagReload::g_LeftTunnelMs{ 0 };
std::atomic<unsigned> MagReload::g_DetonateHeldMs{ 0 };
std::atomic<unsigned> MagReload::g_EjectTick{ 0 };
std::atomic<unsigned> MagReload::g_357EjectTick{ 0 };
std::atomic<bool> MagReload::g_EjectedEmpty{ false };
std::atomic<bool> MagReload::g_PinWeapon{ false };
std::atomic<int> MagReload::g_PinState{ MagReload::kPinIn };
std::atomic<unsigned> MagReload::g_PinDropTick{ 0 };
std::atomic<bool> MagReload::g_PinNear{ false };
std::atomic<float> MagReload::g_PinPull{ 0.f };
std::atomic<bool> MagReload::g_PinThrown{ false };

// Frag grenade pin (VrGrenadePin, with VrMagReload): the trigger stays
// blocked until the left hand pulls the pin (grip at the ring, pull ~6 cm).
// Pulled, the mod holds IN_ATTACK so the game's pin-pull clip runs while the
// arm winds up; press and let go of the trigger to throw (hand velocity as
// before). Alt-fire (roll) is left to the game. A thrown grenade's successor
// comes with its pin (dGrenadeCreate sets g_PinThrown).
namespace
{
    struct QueuedHaptic { DWORD at; vr::ETrackedControllerRole hand; unsigned short us; float amp; };
    QueuedHaptic g_HapticQueue[8]{};
}

void VR::PumpHapticQueue()
{
    const DWORD now = GetTickCount();
    for (auto& q : g_HapticQueue)
    {
        if (q.at != 0 && static_cast<int>(now - q.at) >= 0)
        {
            PulseHandHaptic(q.hand, q.us, q.amp);
            q.at = 0;
            static int s_qLog;
            if (s_qLog++ < 20)
                Game::logMsg("Reload haptic: second tick (%s hand)", q.hand == vr::TrackedControllerRole_LeftHand ? "left" : "right");
        }
    }
}

void VR::ReloadHaptic(ReloadFeel feel)
{
    const auto L = vr::TrackedControllerRole_LeftHand;
    const auto R = vr::TrackedControllerRole_RightHand;
    auto later = [&](DWORD ms, vr::ETrackedControllerRole hand, unsigned short us, float amp) {
        for (auto& q : g_HapticQueue)
        {
            if (q.at == 0)
            {
                const DWORD at = GetTickCount() + ms;
                q = { at ? at : 1, hand, us, amp };
                return;
            }
        }
    };
    static const char* kFeel[] = { "seat", "shell in", "chamber", "pin taken", "pin pulled", "grab" };
    Game::logMsg("Reload haptic: %s", kFeel[static_cast<int>(feel)]);
    switch (feel)
    {
    case ReloadFeel::Seat:
        // Click-clack: the hand pushing it home, the gun taking it, then the
        // catch.
        PulseHandHaptic(L, 3999, 1.f);
        PulseHandHaptic(R, 2500, 0.65f);
        later(70, L, 1200, 0.55f);
        later(70, R, 800, 0.35f);
        break;
    case ReloadFeel::ShellIn:
        PulseHandHaptic(L, 2800, 0.85f);
        PulseHandHaptic(R, 1000, 0.35f);
        later(55, L, 600, 0.3f);
        break;
    case ReloadFeel::Chamber:
        PulseHandHaptic(L, 2500, 0.8f);
        PulseHandHaptic(R, 2500, 0.75f);
        later(60, R, 900, 0.4f);
        break;
    case ReloadFeel::PinTaken:
        PulseHandHaptic(L, 1800, 0.6f);
        break;
    case ReloadFeel::PinPulled:
        PulseHandHaptic(L, 3999, 1.f);
        PulseHandHaptic(R, 900, 0.3f);
        later(80, L, 900, 0.45f);
        break;
    case ReloadFeel::Grab:
        PulseHandHaptic(L, 2500, 0.7f);
        break;
    }
}

bool VR::SatchelHandsActive() const
{
    // Not with empty hands: the viewmodel class stays "satchel" after
    // holstering it, which kept L2 as throw (haptic) and the left grip as
    // Use ("cannot use" sound) on bare hands (user, 2026-11-21).
    return bmvr::g_VrSatchelHands && !bmvr::g_LeftHanded && ViewmodelIsSatchel()
        && !m_EmptyHands && m_HasHeldWeapon;
}

void VR::UpdateSnarkThrow(uint32_t& buttons)
{
    // User, 2026-11-18: snarks left from the centre of the view. Black Mesa
    // throws on the trigger press (anim event); here holding the trigger
    // does nothing and letting go throws, from the hand at its speed like a
    // frag (Hooks::dSnarkThrow / dSnarkPlace). Alt-fire (lob) stays stock.
    static bool s_held = false;
    static DWORD s_pulseUntil = 0;
    const DWORD now = GetTickCount();
    m_ThrowFromLeft.store(SatchelHandsActive(), std::memory_order_relaxed);
    // Satchel too (VrSatchelHands): thrown from the left hand on the release.
    // Snarks from the hand only with "Physical weapons (beta)" on.
    const bool snark = bmvr::g_VrHandThrow && !bmvr::g_LeftHanded && m_GameplayEligible
        && ((bmvr::g_PhysicalWeapons && ViewmodelIsSnark()) || SatchelHandsActive());
    if (!snark)
    {
        s_held = false;
        s_pulseUntil = 0;
        return;
    }
    const bool attack = (buttons & IN_ATTACK) != 0;
    if (attack && now >= s_pulseUntil)
    {
        s_held = true;
        buttons &= ~IN_ATTACK;
    }
    else if (!attack && s_held)
    {
        s_held = false;
        s_pulseUntil = now + 120;
        m_SnarkHandThrowUntil.store(now + 1500, std::memory_order_relaxed);
        // The release the throw speed is measured around: now, not when the
        // pulse below ends.
        Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        if (m_ControllerPoseValid && body.LengthSqr() > 1.f)
        {
            const Vector hand = SatchelHandsActive() ? GetLeftControllerAbsPos(body) : GetRightControllerAbsPos(body);
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
            m_ThrowReleaseT = t;
            m_ThrowReleaseOffset = hand - body;
            m_ThrowReleaseLockUntil = t + 1.0;
        }
        Game::logMsg("%s throw: trigger let go, throwing from the hand", ViewmodelIsSnark() ? "Snark" : "Satchel");
    }
    if (now < s_pulseUntil)
        buttons |= IN_ATTACK;
}

void VR::UpdateGrenadePin(uint32_t& buttons)
{
    using namespace MagReload;
    bool isGrenade = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        isGrenade = m_VmClass.numpadKey == "grenade";
    }
    const bool on = bmvr::g_VrGrenadePin && bmvr::PhysicalWeapon(bmvr::g_VrMagReload) && !bmvr::g_LeftHanded
        && isGrenade && m_HasHeldWeapon && !m_EmptyHands && m_GameplayEligible;
    static int s_state = kPinIn;
    static bool s_gripWas = false, s_trigSeen = false;
    const float grip = LeftSupportGripValue();
    const bool gripDown = grip > 0.6f;
    const bool gripEdge = gripDown && !s_gripWas;
    s_gripWas = gripDown;
    if (g_PinThrown.exchange(false) || !on)
    {
        if (s_state != kPinIn)
            Game::logMsg("Grenade pin: back in (%s)", on ? "grenade thrown" : "weapon changed");
        s_state = kPinIn;
        s_trigSeen = false;
    }
    g_PinWeapon.store(on);
    if (!on)
    {
        g_PinState.store(kPinIn);
        return;
    }
    const bool trigger = (buttons & IN_ATTACK) != 0;
    const DWORD now = GetTickCount();
    switch (s_state)
    {
    case kPinIn:
        // One light buzz when the hand reaches the ring (user 2026-11-18: a
        // buzz every 150 ms while there was too much). Re-armed only after
        // the hand has been away from the ring for 0.4 s.
        {
            static bool s_buzzed = false;
            static DWORD s_awayMs = 0;
            if (g_PinNear.load())
            {
                s_awayMs = 0;
                if (!s_buzzed && !gripDown)
                {
                    s_buzzed = true;
                    PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 400, 0.25f);
                }
            }
            else if (s_buzzed)
            {
                if (s_awayMs == 0)
                    s_awayMs = now;
                else if (now - s_awayMs > 400)
                    s_buzzed = false;
            }
        }
        if (gripEdge && g_PinNear.load())
        {
            s_state = kPinHeld;
            ReloadHaptic(ReloadFeel::PinTaken);
            Game::logMsg("Grenade pin: ring taken");
        }
        break;
    case kPinHeld:
        if (!gripDown)
            s_state = kPinIn;
        else if (g_PinPull.load() >= 2.5f)
        {
            s_state = kPinPulledInHand;
            s_trigSeen = false;
            if (m_Game)
                m_Game->PlayUiSound("weapons/grenade/special1.wav");
            ReloadHaptic(ReloadFeel::PinPulled);
            Game::logMsg("Grenade pin: pulled");
        }
        break;
    case kPinPulledInHand:
        if (!gripDown)
        {
            s_state = kPinDropped;
            g_PinDropTick.store(now ? now : 1);
        }
        break;
    default:
        break;
    }
    if (s_state == kPinIn || s_state == kPinHeld)
        buttons &= ~IN_ATTACK;
    else
    {
        // Armed: held for the game until the trigger has been pressed and let go.
        if (trigger)
            s_trigSeen = true;
        if (!s_trigSeen || trigger)
            buttons |= IN_ATTACK;
    }
    g_PinState.store(s_state);
}

bool MagReload::PumpFollowsSupportHand()
{
    // Both grip modes (user 2026-11-20): the hold-mode grab without the
    // two-hand attach left the glove off the pump and dropped the grab when
    // the hand left the pump's grab radius mid-stroke (back sound, no
    // forward stroke, gun still needing a pump).
    return g_Weapon.load(std::memory_order_relaxed) == kWeaponShotgun;
}

const MagReload::WeaponDef& MagReload::Def(int weapon)
{
    // Sounds: Black Mesa has one wav per whole reload, so the handling sounds
    // come from the Half-Life 2 sounds the game mounts itself (hl2/).
    static const WeaponDef kDefs[kWeaponCount] = {
        { "none", nullptr, nullptr, 0, 0.f, false, nullptr, nullptr, { 0, 0, 0 }, { 0, 0, 0 }, false, nullptr, { 0, 0, 0 }, nullptr },
        // Centres measured from v_mp5 / v_glock / v_shotgun (bind pose, 2026-11-17).
        { "mp5", "Clip_New", "Bolt_New", 30, 4.0f, false,
          "weapons/mp5/empty.wav", "weapons/smg1/switch_single.wav",
          { 0.00f, -8.93f, -8.32f }, { 1.04f, -14.23f, 1.49f }, false, nullptr, { 0, 0, 0 }, nullptr },
        // Slide locks back when fired dry (user 2026-11-24: realistic, and
        // shows the gun is empty); a short rack with the new magazine seated
        // lets it forward.
        { "glock", "bone_clip", "Bone01", 17, 1.8f, true,
          "weapons/pistol/pistol_empty.wav", "weapons/glock/reload_roundchambered.wav",
          { 3.90f, -0.12f, 0.04f }, { 2.56f, -0.09f, 0.04f }, false, nullptr, { 0, 0, 0 }, nullptr },
        { "shotgun", "bone_shell", "bone_pump", 8, 4.5f, false,
          "weapons/shotgun/empty.wav", "weapons/shotgun/pump.wav",
          { 2.53f, -0.02f, 0.06f }, { 8.60f, 0.30f, -0.21f }, true, "bone_gun", { 7.20f, -1.22f, 0.68f }, nullptr },
        // Crossbow: box magazine only. Black Mesa cocks it by sliding the
        // whole rear stock back with the gun hand, which has no natural VR
        // motion, so the stock cocking clip after each shot stays.
        { "crossbow", "clip_export_bone", nullptr, 5, 0.f, false,
          "weapons/crossbow/empty.wav", nullptr,
          { 0.01f, -0.33f, 0.20f }, { 1.88f, -0.77f, 0.06f }, false, nullptr, { 0, 0, 0 }, "body_export_bone" },
        // .357: speed loader into the swung-out cylinder (Chamber); the
        // cylinder scrubs the stock gun_reload1 clip.
        { "357", "speed_loader", nullptr, 6, 0.f, false,
          "weapons/pistol/pistol_empty.wav", nullptr,
          { 0.00f, -0.01f, -0.15f }, { 0, 0, 0 }, false, "Chamber", { 0.03f, 0.01f, -0.51f }, "357" },
        // RPG: the rocket (bodygroup "rocket", rigid on RPG_Projectile; long
        // axis = bone x, -15.6..12.7, radius 2.1, measured from v_rpg) goes
        // into the tube from the front. "Clip" 1 = one rocket in the tube.
        { "rpg", "RPG_Projectile", nullptr, 1, 0.f, false,
          "weapons/ar2/ar2_empty.wav", nullptr,
          { -1.46f, 0.f, 0.f }, { 0, 0, 0 }, false, nullptr, { 0, 0, 0 }, nullptr },
    };
    return kDefs[(weapon >= 0 && weapon < kWeaponCount) ? weapon : 0];
}

bool MagReload::RocketInFist()
{
    return bmvr::g_VrClipHandRpg && bmvr::g_PhysicalWeapons && !bmvr::g_LeftHanded
        && g_Weapon.load(std::memory_order_relaxed) == kWeaponRpg
        && g_State.load(std::memory_order_relaxed) == kInHand;
}

int MagReload::InHandClipPose(int weapon)
{
    switch (weapon)
    {
    case kWeaponMp5: return ClipHandPose::kMp5Mag;
    case kWeaponGlock: return ClipHandPose::kGlockMag;
    case kWeaponCrossbow: return ClipHandPose::kXbowMag;
    case kWeapon357: return ClipHandPose::k357Loader;
    case kWeaponShotgun: return ClipHandPose::kShell;
    // RPG: no clip pose. Black Mesa's gun_reload1 hand is an open palm
    // pushing the rocket, which did not read as holding it (user
    // 2026-11-19); with VrClipHandRpg the glove closes into a fist and the
    // rocket runs through it instead (RocketInFist, ApplyMagReloadBones).
    default: return ClipHandPose::kNone;
    }
}

int MagReload::WantedClipHandPose()
{
    if (!bmvr::g_VrClipHandPose || !bmvr::g_VrStockHandPose || !bmvr::g_PhysicalWeapons || bmvr::g_LeftHanded)
        return ClipHandPose::kNone;
    // Grenade pin by hand: Black Mesa's pinpullthrow fingers on the ring.
    if (g_PinWeapon.load(std::memory_order_relaxed))
    {
        const int s = g_PinState.load(std::memory_order_relaxed);
        if (s == kPinHeld)
            return ClipHandPose::kPinHold;
        if (s == kPinPulledInHand)
            return ClipHandPose::kPinPulled;
    }
    const int w = g_Weapon.load(std::memory_order_relaxed);
    if (g_BoltGrab.load(std::memory_order_relaxed))
    {
        // Shotgun pump: the gun's own left hand rides it already.
        if (w == kWeaponMp5)
            return ClipHandPose::kMp5Bolt;
        if (w == kWeaponGlock)
            return ClipHandPose::kGlockSlide;
        return ClipHandPose::kNone;
    }
    if (g_State.load(std::memory_order_relaxed) == kInHand)
        return InHandClipPose(w);
    return ClipHandPose::kNone;
}

bool VR::OffhandWorldPoseForDraw(Vector& pos, QAngle& ang) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    bool ok;
    Vector tracking;
    if (m_StereoFramePoseActive)
    {
        ok = m_StereoFrameLeftCtrlValid;
        tracking = m_StereoFrameLeftCtrlPos;
        ang = m_StereoFrameLeftCtrlAng;
    }
    else
    {
        ok = m_LeftControllerTrackingValid;
        tracking = m_LeftControllerPosAbs;
        ang = m_LeftControllerAngAbs;
    }
    if (!ok)
        return false;
    const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    pos = ControllerTrackingToWorld(body, tracking);
    return true;
}

int VR::MagReloadWeaponNow() const
{
    if (!bmvr::PhysicalWeapon(bmvr::g_VrMagReload) || bmvr::g_LeftHanded || !m_HasHeldWeapon || m_EmptyHands || !m_GameplayEligible)
        return MagReload::kWeaponNone;
    if (ViewmodelIsMp5())
        return MagReload::kWeaponMp5;
    if (ViewmodelIsGlock())
        return MagReload::kWeaponGlock;
    if (ViewmodelIsShotgun())
        return MagReload::kWeaponShotgun;
    if (ViewmodelIsCrossbow())
        return MagReload::kWeaponCrossbow;
    if (ViewmodelIs357())
        return MagReload::kWeapon357;
    if (ViewmodelIsRpg())
        return MagReload::kWeaponRpg;
    return MagReload::kWeaponNone;
}

void VR::UpdateMagReload(uint32_t& buttons)
{
    PumpHapticQueue();
    // Whether a grip at the hip would take ammo now; the gesture zone buzzes
    // only then (user 2026-11-24). Every early return leaves it false.
    m_AmmoGrabReady = false;
    using namespace MagReload;
    // Per weapon, so a gun put away with its magazine out is still empty
    // when it comes back. Reset on a new map / load.
    struct WeaponState
    {
        int mag = kLoaded;
        bool chambered = true;
        int lastClip = -1;
        int shells = -1;      // shotgun: shells the hands put in the tube
        bool needPump = false;
        bool spentHull = false; // shotgun: a fired hull waits for the back stroke
        bool dryRefill = false; // fired dry: the game's own reload filled its count from the reserve
        int lastReserve = -1;
    };
    static WeaponState s_w[kWeaponCount];
    // Map change: the hands' state from the previous map, taken over per gun
    // when it is first drawn with the same clip + reserve (a level transition
    // keeps the weapons). Else an empty gun came back loaded: the game puts
    // picked-up ammo straight into an empty gun, and the reset trusted it
    // (user 2026-11-23: RPG loaded itself after an ammo pickup).
    static WeaponState s_carry[kWeaponCount];
    static bool s_carryPending[kWeaponCount] = {};
    static bool s_reloadWas = false, s_gripWas = false, s_attackWas = false;
    static bool s_boltFull = false;
    static DWORD s_pulseUntil = 0;
    static bool s_pulseReload = false;
    static int s_lastWeapon = kWeaponNone;
    static int s_lastWeaponForRpg = kWeaponNone;
    static std::string s_level;
    const DWORD now = GetTickCount();
    const bool reload = (buttons & IN_RELOAD) != 0;
    const bool attack = (buttons & IN_ATTACK) != 0;
    // Glock alt-fire uses the same magazine (user 2026-11-20: with the gun
    // dry, alt-fire still fired off the game's background refill).
    const bool attack2 = (buttons & IN_ATTACK2) != 0;
    static bool s_attack2Was = false;
    // Also on every load of the same map (save load, reload after death):
    // the hands' counts follow the restored game, else a shotgun that died
    // with 2 shells kept 2 after a load of a full gun, and one that died
    // empty stayed blocked (tester log 2026-11-20).
    static unsigned s_loadEpoch = 0;
    // Nothing during a load: a level transition starts loading under the old
    // map name, which looked like a same-map load (state reset to the game's).
    if (m_VrPhase == VrPhase::Loading)
        return;
    if (s_level != m_CurrentMapName || s_loadEpoch != m_LoadEpoch)
    {
        const bool sameMap = s_level == m_CurrentMapName;
        if (s_loadEpoch != m_LoadEpoch && sameMap)
            Game::logMsg("Mag reload: load, hands' weapon state reset to the game's");
        for (int k = 0; k < kWeaponCount; ++k)
        {
            // Only guns that were not plainly loaded need carrying over.
            // A pending carry survives an empty map name in between.
            const WeaponState& o = s_w[k];
            const bool carry = !sameMap && !s_level.empty() && o.lastReserve >= 0
                && (o.mag != kLoaded || !o.chambered || o.needPump || (o.shells >= 0 && o.shells != o.lastClip));
            if (carry)
                s_carry[k] = o;
            s_carryPending[k] = carry || (!sameMap && s_carryPending[k]);
        }
        s_level = m_CurrentMapName;
        s_loadEpoch = m_LoadEpoch;
        for (auto& w : s_w)
            w = WeaponState{};
        s_boltFull = false;
        s_pulseReload = false;
        g_BoltGrab.store(false);
    }
    const int weapon = MagReloadWeaponNow();
    if (weapon != s_lastWeapon)
    {
        s_lastWeapon = weapon;
        s_lastWeaponForRpg = kWeaponNone;
        s_boltFull = false;
        g_BoltGrab.store(false);
        // A magazine in the hand is dropped by a weapon switch.
        if (weapon != kWeaponNone && s_w[weapon].mag == kInHand)
            s_w[weapon].mag = kEmpty;
        g_ScrubSeq.store(nullptr);
        Game::logMsg("Mag reload: weapon %s (mag %d, chambered %d)", Def(weapon).name,
            weapon ? s_w[weapon].mag : 0, weapon ? (s_w[weapon].chambered ? 1 : 0) : 1);
    }
    g_Weapon.store(weapon);
    if (!Def(weapon).tube)
        g_DisplayReserveExtra.store(0);
    if (weapon == kWeaponNone)
    {
        g_State.store(kLoaded);
        g_SlideLocked.store(false);
        g_NeedRack.store(false);
        g_DisplayClip.store(-1);
        g_ScrubSeq.store(nullptr);
        s_reloadWas = reload;
        s_pulseReload = false;
        return;
    }
    const WeaponDef& def = Def(weapon);
    WeaponState& w = s_w[weapon];
    // A locked-back slide can only be worked once the new magazine is in
    // (user 2026-11-24: arcade, no racking it before the swap). No grab, no
    // haptic buzz, no hand pose until then.
    const auto slideReadyNow = [&]() { return !def.slideLocksBack || w.mag == kLoaded; };
    // The reload button only ejects; the game's reload runs when a magazine
    // is seated (it keeps the ammo count and the reserve the game's own).
    buttons &= ~IN_RELOAD;
    // Mid weapon switch the viewmodel can name this gun while the active
    // weapon (whose clip is read) is still the previous one; a lower clip was
    // then counted as shots (user 2026-11-18: shotgun tube showed 0 after a
    // switch). Skip the frame until both agree.
    if (m_Game)
    {
        const char* activeModel = m_Game->GetActiveWeaponModelName();
        if (!activeModel || !std::strstr(activeModel, Def(weapon).name))
        {
            static DWORD s_mismatchLogMs = 0;
            if (now - s_mismatchLogMs > 2000)
            {
                s_mismatchLogMs = now;
                Game::logMsg("Mag reload: active weapon '%s' is not the %s yet, frame skipped",
                    activeModel ? activeModel : "?", Def(weapon).name);
            }
            return;
        }
    }
    int clip = m_Game ? m_Game->ReadWeaponClip(m_Game->GetActiveWeaponEntity()) : -1;
    const int reserve = m_Game ? m_Game->ReadWeaponReserve(m_Game->GetActiveWeaponEntity()) : -1;
    if (s_carryPending[weapon] && reserve >= 0)
    {
        s_carryPending[weapon] = false;
        const WeaponState& c = s_carry[weapon];
        if (c.lastClip == clip && c.lastReserve == reserve)
        {
            w = c;
            if (w.mag == kInHand)
                w.mag = kEmpty;
            Game::logMsg("Mag reload: %s state kept over the map change (mag %d, chambered %d, clip %d, reserve %d)",
                def.name, w.mag, w.chambered ? 1 : 0, clip, reserve);
        }
        else
            Game::logMsg("Mag reload: %s ammo changed over the map change (clip %d/%d, reserve %d/%d), game's state",
                def.name, c.lastClip, clip, c.lastReserve, reserve);
    }
    w.lastReserve = reserve;
    // Rounds the game still holds for this gun (its own magazine count, kept
    // through the hands' swaps, plus the reserve): a new magazine/shell only
    // comes off the hip while there are any (user: manual reloads used no ammo).
    const int gameRounds = (clip >= 0 && reserve >= 0) ? clip + reserve : -1;
    // Magazine guns; the RPG, shotgun and .357 branches refine it.
    m_AmmoGrabReady = w.mag == kEmpty && gameRounds != 0 && !def.tube
        && weapon != kWeaponRpg && weapon != kWeapon357;
    auto outOfAmmo = [&](const char* what) {
        static DWORD s_buzzMs = 0;
        if (now - s_buzzMs > 400)
        {
            s_buzzMs = now;
            PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 800, 0.4f);
            Game::logMsg("Mag reload: no %s at the hip (clip %d, reserve %d)", what, clip, reserve);
        }
    };
    auto sound = [&](const char* wav) {
        if (wav && m_Game)
            m_Game->PlayUiSound(wav);
    };
    // Fired dry: the game reloads by itself once the trigger is let go
    // (checked headless 2026-11-17). The magazine stays in, the chamber is
    // empty and the trigger blocked until a swap; the game's refill is only
    // bookkeeping.
    if (weapon != kWeaponRpg && w.mag == kLoaded && clip == 0 && w.lastClip > 0)
    {
        w.mag = kDry;
        w.chambered = def.boltBone == nullptr; // no bolt/slide to work: nothing to rack
        w.dryRefill = true;
        // Locking slide (glock): goes back on its own with the slide's
        // pull-back sound and stays (almost fully) back; a short rack then
        // lets it go forward (user 2026-11-24: racking a closed slide on an
        // empty gun was not realistic).
        if (def.slideLocksBack)
            sound(bmvr::soundcuts::Sample(bmvr::soundcuts::kGlockSlideBack, nullptr));
        Game::logMsg("Mag reload: %s fired dry, swap the magazine%s", def.name, def.slideLocksBack ? " (slide held back)" : "");
    }
    const float grip = LeftSupportGripValue();
    const bool gripDown = grip > 0.6f;
    const bool gripEdge = gripDown && !s_gripWas;
    const bool released = grip < 0.35f;
    bool boltGrab = g_BoltGrab.load();

    // Left hip: mirror of the right-hand pistol holster zone.
    auto atLeftHip = [&]() -> bool {
        if (!m_HmdPoseValid || !m_LeftControllerTrackingValid)
            return false;
        if (!m_BodyFrameValid)
            return false;
        // Every left-hand slot set to Ammo (default: the left hip).
        const bool in = LeftHandInAmmoZone();
        float along, lateral, height;
        ZoneCoords(m_LeftControllerPosAbs, along, lateral, height);
        lateral = -lateral;
        static int s_missLog;
        if (!in && s_missLog < 20)
        {
            ++s_missLog;
            Game::logMsg("Mag reload: grip outside an ammo zone (%.2f m ahead, %.2f m up, %.2f m left of the body)",
                along, height, lateral);
        }
        return in;
    };

    if (weapon == kWeaponRpg)
    {
        // RPG: the game's own (hidden) reload keeps the rocket count; the
        // hands take a rocket from the hip and push it into the tube from the
        // front (the draw code puts it on the tube axis and reports how far
        // it is in). The trigger is blocked until it is seated.
        static int s_lastNeed = -1, s_lastRounds = -1;
        static bool s_wasEngaged = false;
        if (s_lastWeaponForRpg != weapon)
        {
            s_lastNeed = -1;
            s_lastRounds = -1;
        }
        int need = -1, inRel = -1;
        if (m_Game)
            m_Game->ReadWeaponReloadFlags(m_Game->GetActiveWeaponEntity(), need, inRel);
        const int rounds = reserve < 0 ? -1 : reserve + (clip > 0 ? clip : 0);
        {
            static int s_logKey = -9999, s_logs = 0;
            const int key = ((need + 1) * 1000 + (clip + 1)) * 1000 + (reserve + 1) + w.mag * 7919;
            if (key != s_logKey && s_logs < 200)
            {
                ++s_logs;
                s_logKey = key;
                Game::logMsg("Mag reload: rpg game needReload %d inReload %d clip %d reserve %d (tube %d)",
                    need, inRel, clip, reserve, w.mag);
            }
        }
        // Fired: the game flags a reload (rocket left the tube) or its rocket
        // count dropped. A load / first draw with an empty tube starts empty.
        const bool fired = (need == 1 && s_lastNeed == 0)
            || (rounds >= 0 && s_lastRounds >= 0 && rounds < s_lastRounds);
        if (w.mag == kLoaded && (fired || (s_lastNeed < 0 && need == 1)))
        {
            w.mag = kEmpty;
            Game::logMsg("Mag reload: rpg tube empty (%s)", fired ? "fired" : "empty at draw");
        }
        s_lastNeed = need;
        s_lastRounds = rounds;
        m_AmmoGrabReady = w.mag == kEmpty && rounds != 0;
        if (w.mag == kEmpty && gripEdge && atLeftHip() && rounds == 0)
            outOfAmmo("rocket");
        else if (w.mag == kEmpty && gripEdge && atLeftHip())
        {
            w.mag = kInHand;
            g_RpgInsert.store(-1.f);
            g_NearWell.store(false);
            s_wasEngaged = false;
            PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 2500, 0.7f);
            Game::logMsg("Mag reload: rpg rocket taken from the hip (%d rockets)", rounds);
        }
        else if (w.mag == kInHand)
        {
            const float in = g_RpgInsert.load();
            const bool engaged = in >= 0.f;
            if (engaged && !s_wasEngaged)
            {
                sound("weapons/ar2/ar2_reload_rotate.wav");
                PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 1500, 0.5f);
                Game::logMsg("Mag reload: rpg rocket at the muzzle");
            }
            else if (engaged)
            {
                static DWORD s_slideMs = 0;
                if (now - s_slideMs > 120)
                {
                    s_slideMs = now;
                    PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 300 + static_cast<int>(in * 900.f), 0.2f);
                }
            }
            s_wasEngaged = engaged;
            if (g_NearWell.load())
            {
                w.mag = kLoaded;
                s_wasEngaged = false;
                // The game's reload, if it has not run yet (needs a rocket in reserve).
                s_pulseReload = need != 0;
                s_pulseUntil = now + 200;
                sound("weapons/ar2/ar2_reload_push.wav");
                ReloadHaptic(ReloadFeel::Seat);
                Game::logMsg("Mag reload: rpg rocket seated (needReload %d, game reload %d)", need, s_pulseReload ? 1 : 0);
            }
            else if (released && in >= 0.35f)
            {
                // Let go at least a third in: it slides home by itself (user:
                // fun beats finicky; a half-pushed rocket must not leave the
                // gun empty in a fight).
                w.mag = kLoaded;
                s_wasEngaged = false;
                s_pulseReload = need != 0;
                s_pulseUntil = now + 200;
                sound("weapons/ar2/ar2_reload_push.wav");
                ReloadHaptic(ReloadFeel::Seat);
                Game::logMsg("Mag reload: rpg rocket let go %.0f%% in, seated (needReload %d)", in * 100.f, need);
            }
            else if (released)
            {
                w.mag = kEmpty;
                s_wasEngaged = false;
                g_DropFrom.store(1);
                g_DropTick.store(now ? now : 1);
                Game::logMsg("Mag reload: rpg rocket dropped (%s)", engaged ? "less than a third in" : "not at the tube");
            }
        }
        else
            s_wasEngaged = false;
        s_lastWeaponForRpg = weapon;
        const bool canFire = w.mag == kLoaded;
        if (!canFire)
        {
            if (attack && !s_attackWas)
            {
                sound(def.emptyClick);
                Game::logMsg("Mag reload: rpg fire blocked (rocket state %d, game clip %d)", w.mag, clip);
            }
            buttons &= ~IN_ATTACK;
        }
        if (s_pulseReload)
        {
            if (now < s_pulseUntil)
                buttons |= IN_RELOAD;
            else
                s_pulseReload = false;
        }
        g_ScrubSeq.store(nullptr);
        g_SlideLocked.store(false);
        g_NeedRack.store(false);
        g_DisplayClip.store(clip >= 0 ? (w.mag == kLoaded ? clip : 0) : -1);
        g_State.store(w.mag);
        w.lastClip = clip;
        s_reloadWas = reload;
        s_gripWas = gripDown;
        s_attackWas = attack;
        return;
    }
    s_lastWeaponForRpg = weapon;

    if (def.tube)
    {
        // Shotgun: the mod counts the shells the hands put in; the game's
        // own reload fills the tube in the background (it cannot stop after
        // one shell without firing), so its count is only an upper bound.
        if (w.shells < 0)
            w.shells = (std::max)(0, clip);
        if (clip >= 0 && w.lastClip >= 0 && clip < w.lastClip)
        {
            w.shells = (std::max)(0, w.shells - (w.lastClip - clip));
            w.needPump = true;
            w.spentHull = true;
        }
        // The pump only moves when it needs working (after a shot); else it
        // stays locked forward, so a hand resting on it never pulls it back,
        // plays the pump sound or blocks the trigger (user, 2026-11-17).
        // The attached support hand works it by moving: toggle hold without
        // a held squeeze, hold mode while the grip (and so the attach) is held.
        const bool handOnPump = PumpFollowsSupportHand() && TwoHandGripActive();
        m_AmmoGrabReady = !boltGrab && w.mag != kInHand && w.shells < def.maxClip
            && !(gameRounds >= 0 && w.shells >= gameRounds);
        if (!boltGrab && w.needPump && handOnPump && w.mag != kInHand)
        {
            boltGrab = true;
            s_boltFull = false;
            Game::logMsg("Mag reload: shotgun pump grabbed (support hand, %s)", bmvr::g_TwoHandGripToggle ? "toggle" : "hold");
        }
        else if (!boltGrab && gripEdge && w.mag != kInHand && w.shells < def.maxClip && atLeftHip()
            && gameRounds >= 0 && w.shells >= gameRounds)
            outOfAmmo("shell");
        else if (!boltGrab && gripEdge && w.mag != kInHand && w.shells < def.maxClip && atLeftHip())
        {
            w.mag = kInHand;
            PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 2500, 0.7f);
            Game::logMsg("Mag reload: shotgun shell taken from the hip (%d in the tube)", w.shells);
        }
        if (w.mag == kInHand)
        {
            if (g_NearWell.load())
            {
                w.mag = kLoaded;
                ++w.shells;
                static const char* kInsert[3] = { "weapons/shotgun/reload1.wav", "weapons/shotgun/reload2.wav", "weapons/shotgun/reload3.wav" };
                sound(kInsert[now % 3]);
                ReloadHaptic(ReloadFeel::ShellIn);
                // The game tops the tube up when it is behind the hands.
                if (clip >= 0 && clip < w.shells)
                {
                    s_pulseReload = true;
                    s_pulseUntil = now + 200;
                }
                Game::logMsg("Mag reload: shotgun shell in (%.1f cm, %.0f deg; %d in the tube, game %d)",
                    g_SeatDistCm.load(), g_SeatAngleDeg.load(), w.shells, clip);
            }
            else if (released)
            {
                w.mag = kLoaded;
                g_DropFrom.store(1);
                g_DropTick.store(now ? now : 1);
                Game::logMsg("Mag reload: shotgun shell dropped");
            }
        }
        float pull = 0.f;
        if (boltGrab)
        {
            pull = g_BoltPull.load();
            // Back stroke at 3/4 of the hand stroke (was 0.85, user
            // 2026-11-21: more lenient).
            if (pull >= (bmvr::g_PumpStrokeAtController ? 0.75f : 0.85f) && !s_boltFull)
            {
                s_boltFull = true;
                // Back stroke: first half of pump.wav (whole wav if the split
                // is not available).
                const char* back = m_Game ? m_Game->PumpHalfSound(false) : nullptr;
                sound(back ? back : def.boltSound);
                PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 2000, 0.7f);
                // Back stroke: the spent hull flies out of the port, once
                // per shot (pumping an empty gun ejected a hull each time,
                // user 2026-11-18).
                if (w.spentHull)
                {
                    w.spentHull = false;
                    g_EjectTick.store(now ? now : 1);
                }
            }
            // Forward stroke counts from halfway (user: a pump that did not
            // come all the way forward was still "needs pump"); the pump
            // then snaps forward.
            if (s_boltFull && pull <= 0.5f)
            {
                s_boltFull = false;
                // Forward stroke: second half + a firm pulse, so the player
                // knows the pump landed (user, 2026-11-18).
                if (const char* fwd = m_Game ? m_Game->PumpHalfSound(true) : nullptr)
                    sound(fwd);
                ReloadHaptic(ReloadFeel::Chamber);
                // An empty tube chambers nothing: still needs a pump once
                // shells are in (user).
                w.needPump = w.shells <= 0;
                Game::logMsg("Mag reload: shotgun pumped (%d in the tube, chambered %d)", w.shells, w.needPump ? 0 : 1);
                // Pumped: the pump locks forward again.
                if (!w.needPump)
                    boltGrab = false;
            }
            if (!handOnPump)
            {
                boltGrab = false;
                s_boltFull = false;
            }
        }
        g_BoltGrab.store(boltGrab);
        const bool canFire = w.shells > 0 && !w.needPump && pull < 0.1f;
        const bool attack2 = (buttons & IN_ATTACK2) != 0;
        if (!canFire)
        {
            if (attack && !s_attackWas)
            {
                sound(def.emptyClick);
                if (w.shells > 0 && w.needPump)
                    ShowReloadHint(2, "PUMP IT");
                Game::logMsg("Mag reload: shotgun fire blocked (tube %d, game %d, needPump %d, pull %.2f, pump grabbed %d)",
                    w.shells, clip, w.needPump ? 1 : 0, pull, boltGrab ? 1 : 0);
            }
            buttons &= ~(IN_ATTACK | IN_ATTACK2);
        }
        else if (w.shells < 2)
            buttons &= ~IN_ATTACK2;
        (void)attack2;
        if (s_pulseReload)
        {
            if (now < s_pulseUntil)
                buttons |= IN_RELOAD;
            else
                s_pulseReload = false;
        }
        g_SlideLocked.store(false);
        g_NeedRack.store(w.needPump);
        g_DisplayClip.store(w.shells);
        g_DisplayReserveExtra.store(clip > w.shells ? clip - w.shells : 0);
        g_State.store(w.mag);
        if (clip != w.lastClip && w.lastClip >= 0)
            Game::logMsg("Mag reload: shotgun game clip %d -> %d (tube %d, pump %d)", w.lastClip, clip, w.shells, w.needPump ? 1 : 0);
        w.lastClip = clip;
        s_reloadWas = reload;
        s_gripWas = gripDown;
        s_attackWas = attack;
        return;
    }

    if (weapon == kWeapon357)
    {
        // .357: the cylinder is the stock gun_reload1 clip, scrubbed in
        // timed parts: open + eject (cycle 0.02 -> 0.5), then insert
        // (0.62 -> 0.76, new rounds in the open cylinder) once the speed
        // loader reaches it, then close (0.76 -> 0.97). With bmvr_357_flick
        // (user, 2026-11-18) the cylinder stays open after the insert until
        // a flick of the gun wrist (raw tracking rotation faster than
        // bmvr_357_flick_speed deg/s for two updates) or the reload button
        // snaps it shut; without it the close follows the insert.
        // The clip's whole-reload sound sits at cycle 0, so it starts after.
        static int s_phase = 0; // 0 none, 1 opening, 2 open, 3 inserting, 4 waiting for the flick, 5 closing
        static DWORD s_phaseMs = 0;
        auto scrub = [&](float a, float b, float secs) {
            const float t = std::clamp((now - s_phaseMs) * 0.001f / secs, 0.f, 1.f);
            g_ScrubSeq.store("gun_reload1");
            g_ScrubCycle.store(a + (b - a) * t);
            return t >= 1.f;
        };
        if (w.mag != kInHand && w.mag != kEmpty)
            s_phase = 0;
        else if (s_phase == 0)
            s_phase = 2; // back from a weapon switch with the cylinder open
        if ((w.mag == kLoaded || w.mag == kDry) && reload && !s_reloadWas)
        {
            // Opening the cylinder dumps every round (VrMagDiscard): the
            // loader then brings six from the reserve.
            if (bmvr::g_VrMagDiscard && m_Game && clip > 0 && reserve > 0
                && m_Game->SetServerActiveWeaponClip(clip, 0))
            {
                Game::logMsg("Mag reload: 357 cylinder dumped %d live rounds, lost", clip);
                clip = 0;
            }
            w.mag = kEmpty;
            s_phase = 1;
            s_phaseMs = now;
            // Empties stay in the open cylinder until they fall out (tilt or at once).
            g_357EjectTick.store(0);
            // The game's reload runs while the hands work (ammo stays the
            // game's); its own clip is not shown.
            s_pulseReload = clip >= 0 && clip < def.maxClip;
            s_pulseUntil = now + 200;
            sound("weapons/357/357_reload1.wav");
            Game::logMsg("Mag reload: 357 cylinder open (clip %d, game reload %d)", clip, s_pulseReload ? 1 : 0);
        }
        // With bmvr_357_tilt (user, 2026-11-18) the empties stay in the open
        // cylinder until the muzzle points up by bmvr_357_tilt_pitch degrees
        // (gun tipped back towards the player, raw tracking); the speed
        // loader cannot be taken from the hip before that. Back from a
        // weapon switch with the cylinder open, the old tick stays (empties gone).
        const bool emptiesIn = s_phase == 2 && g_357EjectTick.load() == 0;
        if (s_phase == 1 && scrub(0.02f, 0.5f, 0.7f))
        {
            s_phase = 2;
            if (!bmvr::g_Vr357Tilt)
                g_357EjectTick.store(now ? now : 1);
            else
                ShowReloadHint(4, "TILT UP TO DUMP");
        }
        else if (s_phase == 2)
            scrub(0.5f, 0.5f, 1.f);
        if (emptiesIn)
        {
            // Muzzle direction = controller -Z; tracking space up = +Y.
            float pitch = 0.f;
            if (m_RightControllerTrackingValid)
                pitch = std::asin(std::clamp(-m_RightControllerTracking.m[1][2], -1.f, 1.f)) * 57.2958f;
            static float s_peak = -90.f;
            static DWORD s_peakLogMs = 0;
            s_peak = (std::max)(s_peak, pitch);
            if (now - s_peakLogMs > 1000)
            {
                s_peakLogMs = now;
                Game::logMsg("Mag reload: 357 empties in, highest muzzle pitch %.0f deg (need %.0f)",
                    s_peak, bmvr::g_Vr357TiltPitch);
                s_peak = -90.f;
            }
            if (m_RightControllerTrackingValid && pitch >= bmvr::g_Vr357TiltPitch)
            {
                g_357EjectTick.store(now ? now : 1);
                PulseHandHaptic(vr::TrackedControllerRole_RightHand, 1500, 0.5f);
                Game::logMsg("Mag reload: 357 tilted %.0f deg, empties out", pitch);
            }
        }
        m_AmmoGrabReady = !emptiesIn && w.mag == kEmpty && s_phase == 2 && gameRounds != 0;
        if (emptiesIn && gripEdge && atLeftHip())
        {
            static int s_log;
            if (s_log++ < 10)
                Game::logMsg("Mag reload: 357 speed loader not taken, empties still in the cylinder");
        }
        else if (w.mag == kEmpty && s_phase == 2 && gripEdge && atLeftHip() && gameRounds == 0)
            outOfAmmo("speed loader");
        else if (w.mag == kEmpty && s_phase == 2 && gripEdge && atLeftHip())
        {
            w.mag = kInHand;
            PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 2500, 0.7f);
            Game::logMsg("Mag reload: 357 speed loader taken from the hip");
        }
        else if (w.mag == kInHand && s_phase == 2)
        {
            if (g_NearWell.load())
            {
                s_phase = 3;
                s_phaseMs = now;
                sound("weapons/357/357_reload3.wav");
                ReloadHaptic(ReloadFeel::Seat);
                Game::logMsg("Mag reload: 357 speed loader in");
            }
            else if (released)
            {
                w.mag = kEmpty;
                g_DropFrom.store(1);
                g_DropTick.store(now ? now : 1);
                Game::logMsg("Mag reload: 357 speed loader dropped");
            }
        }
        // Gun wrist rotation speed from the raw tracking pose (smooth/snap
        // turning does not count).
        float flickRate = 0.f;
        {
            static float s_prevR[3][3]{};
            static DWORD s_prevMs = 0;
            static bool s_prevValid = false;
            if (m_RightControllerTrackingValid)
            {
                float R[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        R[i][j] = m_RightControllerTracking.m[i][j];
                const DWORD dtMs = now - s_prevMs;
                if (s_prevValid && dtMs >= 4 && dtMs < 100)
                {
                    float tr = 0.f;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            tr += s_prevR[i][j] * R[i][j];
                    const float ang = std::acos(std::clamp((tr - 1.f) * 0.5f, -1.f, 1.f));
                    flickRate = ang * 57.2958f * 1000.f / static_cast<float>(dtMs);
                }
                if (!s_prevValid || dtMs >= 4)
                {
                    std::memcpy(s_prevR, R, sizeof(R));
                    s_prevMs = now;
                    s_prevValid = true;
                }
            }
            else
                s_prevValid = false;
        }
        if (s_phase == 3 && !bmvr::g_Vr357Flick && scrub(0.62f, 0.97f, 0.8f))
        {
            s_phase = 0;
            w.mag = kLoaded;
            w.chambered = true;
            sound("weapons/357/357_reload4.wav");
            Game::logMsg("Mag reload: 357 cylinder closed (clip %d)", clip);
        }
        else if (s_phase == 3 && bmvr::g_Vr357Flick && scrub(0.62f, 0.76f, 0.45f))
        {
            s_phase = 4;
            s_phaseMs = now;
            ShowReloadHint(3, "FLICK TO CLOSE");
            Game::logMsg("Mag reload: 357 rounds in, cylinder open until a flick");
        }
        else if (s_phase == 4)
        {
            scrub(0.76f, 0.76f, 1.f);
            static int s_fastUpdates = 0;
            static float s_peak = 0.f;
            static DWORD s_peakLogMs = 0;
            s_fastUpdates = flickRate > bmvr::g_Vr357FlickSpeed ? s_fastUpdates + 1 : 0;
            s_peak = (std::max)(s_peak, flickRate);
            if (now - s_peakLogMs > 1000)
            {
                s_peakLogMs = now;
                Game::logMsg("Mag reload: 357 waiting for the flick, fastest wrist turn %.0f deg/s (need %.0f)",
                    s_peak, bmvr::g_Vr357FlickSpeed);
                s_peak = 0.f;
            }
            const bool byButton = reload && !s_reloadWas;
            if (s_fastUpdates >= 2 || byButton)
            {
                s_phase = 5;
                s_phaseMs = now;
                s_fastUpdates = 0;
                sound("weapons/357/357_reload4.wav");
                PulseHandHaptic(vr::TrackedControllerRole_RightHand, 2500, 0.8f);
                Game::logMsg("Mag reload: 357 cylinder flicked shut (%s, %.0f deg/s)",
                    byButton ? "reload button" : "wrist", flickRate);
            }
        }
        else if (s_phase == 5 && scrub(0.76f, 0.97f, 0.22f))
        {
            s_phase = 0;
            w.mag = kLoaded;
            w.chambered = true;
            Game::logMsg("Mag reload: 357 cylinder closed (clip %d)", clip);
        }
        if (s_phase == 0)
            g_ScrubSeq.store(nullptr);
        // While the loader goes in, it is the clip's own loader, not the hand's.
        g_State.store(s_phase >= 3 ? kEmpty : w.mag);
        const bool canFire = w.mag == kLoaded && s_phase == 0;
        if (!canFire)
        {
            if (attack && !s_attackWas)
            {
                sound(def.emptyClick);
                Game::logMsg("Mag reload: 357 fire blocked (cylinder state %d, phase %d, game clip %d)", w.mag, s_phase, clip);
            }
            buttons &= ~IN_ATTACK;
        }
        if (s_pulseReload)
        {
            if (now < s_pulseUntil)
                buttons |= IN_RELOAD;
            else
                s_pulseReload = false;
        }
        g_SlideLocked.store(false);
        g_NeedRack.store(false);
        g_DisplayClip.store(w.mag == kLoaded ? clip : 0);
        if (clip > w.lastClip && w.lastClip >= 0)
            Game::logMsg("Mag reload: 357 clip %d -> %d (mag %d)", w.lastClip, clip, w.mag);
        w.lastClip = clip;
        s_reloadWas = reload;
        s_gripWas = gripDown;
        s_attackWas = attack;
        return;
    }

    if ((w.mag == kLoaded || w.mag == kDry) && reload && !s_reloadWas)
    {
        g_EjectedEmpty.store(w.mag == kDry || clip <= 0);
        // Rounds left in the magazine go with it (VrMagDiscard); a round in
        // the MP5/glock chamber stays. The game's reload below then takes a
        // full magazine from the reserve.
        // Not after a dry fire: the game's own reload has already filled its
        // count from the reserve; those rounds are the next magazine (user
        // 2026-11-19: every magazine cost double).
        if (bmvr::g_VrMagDiscard && m_Game && clip > 0 && reserve > 0 && w.mag == kLoaded)
        {
            const int keep = (weapon != kWeaponCrossbow && w.mag == kLoaded && w.chambered) ? 1 : 0;
            if (clip > keep && m_Game->SetServerActiveWeaponClip(clip, keep))
            {
                Game::logMsg("Mag reload: %s magazine ejected with %d rounds, lost (%d stays in the chamber, reserve %d)",
                    def.name, clip - keep, keep, reserve);
                clip = keep;
            }
        }
        w.mag = kEmpty;
        g_DropFrom.store(0);
        g_DropTick.store(now ? now : 1);
        // The game's own (hidden) reload starts now and runs while the hands
        // swap (the trigger is blocked meanwhile). Started at the seat, a
        // shot right after cancelled it and the old count stayed (user).
        if (clip >= 0 && clip < def.maxClip && reserve != 0)
        {
            s_pulseReload = true;
            s_pulseUntil = now + 200;
        }
        // The gun's own reload sound, only its magazine-out start (was
        // HL2's AR2 rotate: user 2026-11-20, an odd electric sound).
        // (Cut from the wav at start: StopSound on the full one did not
        // stop it.)
        {
            using namespace bmvr::soundcuts;
            const Clip c = weapon == kWeaponGlock ? kGlockMagOut : (weapon == kWeaponCrossbow ? kCrossbowMagOut : kMp5MagOut);
            sound(Sample(c, "weapons/ar2/ar2_reload_rotate.wav"));
        }
        PulseHandHaptic(vr::TrackedControllerRole_RightHand, 1500, 0.5f);
        Game::logMsg("Mag reload: %s ejected (clip %d, chambered %d)", def.name, clip, w.chambered ? 1 : 0);
    }
    // Bolt / slide: left grip held at it, pull back; letting go of the grip
    // lets go of the handle (also with the two-hand grip toggle: user
    // 2026-11-19, the toggle kept the MP5 handle held after the grip was
    // released and the hand moving back still finished the pull).
    else if (!boltGrab && gripEdge && w.mag != kInHand && !w.chambered && slideReadyNow() && g_NearBolt.load())
    {
        boltGrab = true;
        s_boltFull = false;
        PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 1500, 0.5f);
        Game::logMsg("Mag reload: %s bolt grabbed", def.name);
    }
    else if (w.mag == kEmpty && gripEdge && atLeftHip() && gameRounds == 0)
        outOfAmmo("magazine");
    else if (w.mag == kEmpty && gripEdge && atLeftHip())
    {
        w.mag = kInHand;
        PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 2500, 0.7f);
        Game::logMsg("Mag reload: %s magazine taken from the hip", def.name);
    }
    else if (w.mag == kInHand)
    {
        if (g_NearWell.load())
        {
            w.mag = kLoaded;
            w.dryRefill = false;
            s_pulseReload = clip >= 0 && clip < def.maxClip;
            s_pulseUntil = now + 200;
            {
                // The gun's own magazine-in (was HL2's AR2 push, user
                // 2026-11-20: electric).
                using namespace bmvr::soundcuts;
                const Clip c = weapon == kWeaponGlock ? kGlockMagIn : (weapon == kWeaponCrossbow ? kCrossbowMagIn : kMp5MagIn);
                sound(Sample(c, "weapons/ar2/ar2_reload_push.wav"));
            }
            ReloadHaptic(ReloadFeel::Seat);
            Game::logMsg("Mag reload: %s seated (%.1f cm, %.0f deg; clip %d, game reload %d, chambered %d)",
                def.name, g_SeatDistCm.load(), g_SeatAngleDeg.load(), clip, s_pulseReload ? 1 : 0, w.chambered ? 1 : 0);
        }
        else if (released)
        {
            w.mag = kEmpty;
            g_DropFrom.store(1);
            g_DropTick.store(now ? now : 1);
            Game::logMsg("Mag reload: %s magazine dropped", def.name);
        }
    }

    // One light buzz when the hand reaches a bolt/slide that needs working
    // (was every 150 ms while there: user 2026-11-19, kept vibrating).
    {
        static bool s_wasNear = false;
        const bool atBolt = !boltGrab && !w.chambered && w.mag != kInHand && slideReadyNow() && g_NearBolt.load();
        if (atBolt && !s_wasNear)
            PulseHandHaptic(vr::TrackedControllerRole_LeftHand, 600, 0.3f);
        s_wasNear = atBolt;
    }
    if (boltGrab && def.slideLocksBack)
    {
        // Slide held back (fired dry): pulled a little further
        // (~1 cm of hand travel) and let go, or pulled fully back, it snaps
        // forward with the second half of the slide sound.
        static bool s_slideArmed = false;
        const float pull = g_BoltPull.load();
        if (pull >= 0.2f)
            s_slideArmed = true;
        if (pull >= 0.85f || (released && s_slideArmed))
        {
            boltGrab = false;
            s_boltFull = false;
            s_slideArmed = false;
            w.chambered = (w.mag == kLoaded) && clip > 0;
            sound(bmvr::soundcuts::Sample(bmvr::soundcuts::kGlockSlideForward, def.boltSound));
            ReloadHaptic(ReloadFeel::Chamber);
            Game::logMsg("Mag reload: %s slide released (pull %.2f), chambered %d", def.name, pull, w.chambered ? 1 : 0);
        }
        else if (released)
        {
            boltGrab = false;
            s_slideArmed = false;
            Game::logMsg("Mag reload: %s slide let go without a rack (pull %.2f), stays back", def.name, pull);
        }
    }
    else if (boltGrab)
    {
        const float pull = g_BoltPull.load();
        if (pull >= 0.85f)
        {
            // Pulled fully back: it springs forward on its own (no need to
            // let go, so toggle hold works). A loaded magazine chambers a
            // round; without one it only cycles (and closes a locked slide).
            boltGrab = false;
            s_boltFull = false;
            w.chambered = (w.mag == kLoaded) && clip > 0;
            // MP5: Black Mesa's own charging handle (HL2's smg1 switch was
            // louder on the left).
            if (weapon == kWeaponMp5)
                sound(bmvr::soundcuts::Sample(bmvr::soundcuts::kMp5Bolt, def.boltSound));
            else
                sound(def.boltSound);
            ReloadHaptic(ReloadFeel::Chamber);
            Game::logMsg("Mag reload: %s bolt pulled back, chambered %d", def.name, w.chambered ? 1 : 0);
        }
        else if (released)
        {
            boltGrab = false;
            s_boltFull = false;
            Game::logMsg("Mag reload: %s bolt let go (pull %.2f)", def.name, pull);
        }
    }
    g_BoltGrab.store(boltGrab);

    const bool canFire = w.mag == kLoaded && w.chambered && !boltGrab;
    if (!canFire)
    {
        if (attack && !s_attackWas)
        {
            sound(def.emptyClick);
            Game::logMsg("Mag reload: %s fire blocked (mag state %d, chambered %d, bolt grabbed %d, game clip %d)",
                def.name, w.mag, w.chambered ? 1 : 0, boltGrab ? 1 : 0, clip);
            // Loaded but nothing chambered: the step people miss.
            if (w.mag == kLoaded && !w.chambered && clip > 0)
                ShowReloadHint(weapon == kWeaponGlock ? 1 : 0,
                    weapon == kWeaponGlock ? "RACK THE SLIDE" : "PULL THE CHARGING HANDLE");
        }
        buttons &= ~IN_ATTACK;
        if (weapon == kWeaponGlock)
        {
            if (attack2 && !s_attack2Was && !(attack && !s_attackWas))
                sound(def.emptyClick);
            buttons &= ~IN_ATTACK2;
        }
    }
    s_attack2Was = attack2;
    if (s_pulseReload)
    {
        if (now < s_pulseUntil)
            buttons |= IN_RELOAD;
        else
            s_pulseReload = false;
    }
    // Open (locked back) slide: glock with an empty chamber.
    g_SlideLocked.store(def.slideLocksBack && !w.chambered);
    g_NeedRack.store(!w.chambered && slideReadyNow());
    g_DisplayClip.store((w.mag == kLoaded) ? clip : 0);
    // After a dry fire the game's own reload already moved a magazine from
    // the reserve into its count; until the new magazine is seated the HUD
    // shows those rounds in the reserve (user 2026-11-19: the reserve dropped
    // the moment the gun ran dry).
    g_DisplayReserveExtra.store(w.dryRefill && w.mag != kLoaded && clip > 0 ? clip : 0);
    g_State.store(w.mag);
    if (clip > w.lastClip && w.lastClip >= 0)
        Game::logMsg("Mag reload: %s clip %d -> %d (mag %d, reserve %d)", def.name, w.lastClip, clip, w.mag, reserve);
    w.lastClip = clip;
    s_reloadWas = reload;
    s_gripWas = gripDown;
    s_attackWas = attack;
}
