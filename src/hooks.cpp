#include "hooks.h"
#include "vr_mag_reload.h"
#include "offset_checks.h"
#include "game.h"
#include "sdk.h"
#include "bmvr_seh.h"
#include "vr.h"
#include "offsets.h"

// Diagnostic: DXVK logs every draw while this is set (BM deferred-light pass in a pass trace).
extern "C" volatile int g_BmvrD3dDrawTrace = 0;
// d3d9 device: rebuild the flashlight shadow matrix from the current light pose (VR/config.txt FlashlightShadowMatrixFix).
extern "C" volatile int g_BmvrFlashlightMatrixFix = 1;
#include "bmvr_flags.h"
#include "bmvr_glowtrace.h"
#include "bmvr_prof.h"
#include "bmvr_sampler.h"
#include <intrin.h>
#include <map>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <set>

// The areaportal setup's call into ComputeViewMatrices (E8 rel32 just before
// kAreaPortalComputeViewReturnRva) must target `fn`; otherwise this is another
// engine build and the return-address filter would never match.
static bool AreaPortalComputeViewCallSiteOk(int fn)
{
    const uintptr_t engine = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine.dll"));
    if (!engine || !fn)
        return false;
    const uintptr_t ret = engine + Offsets::kAreaPortalComputeViewReturnRva;
    const auto* call = reinterpret_cast<const unsigned char*>(ret - 5);
    bool ok = false;
    BMVR_TRY
    {
        ok = call[0] == 0xE8
            && ret + *reinterpret_cast<const int32_t*>(call + 1) == static_cast<uintptr_t>(fn);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    Game::logMsg("Areaportal ComputeViewMatrices call site %s", ok ? "ok" : "MISMATCH (hook skipped)");
    if (!ok)
        Game::noteMissingOffset("Areaportal ComputeViewMatrices call site");
    return ok;
}

// Pixel visibility (glow occlusion queries) is keyed by CurrentViewID().
// The redirected calls give the right eye's views their own id (id + 0x100,
// as dOcclusionBeginView does for the occlusion tracker). Unlike there, the
// reflection / refraction views (3 / 4) are included: each eye renders its own
// and shared queries made glows in floor reflections flicker (headless at
// reflref: right-eye drops 20-36 per 5 s in view 3 shared, 0 per eye).
using ClientCurrentViewIdFn = int(__cdecl*)();
static ClientCurrentViewIdFn g_ClientCurrentViewId = nullptr;

static int __cdecl PixelVisCurrentViewId()
{
    const int id = g_ClientCurrentViewId();
    if (bmvr::g_PixelVisPerEye && Hooks::m_VR && Hooks::m_VR->m_StereoEye == 2 && id >= 0 && id < 0x100)
        return id + 0x100;
    return id;
}

// Glow trace: the query-level value (before GlowHold) of the current
// GetFractionVisible call, filled by the hook below on the same thread.
static thread_local float t_GlowTraceRaw = 0.f;
static thread_local float t_GlowTraceFadeInv = 0.f;
static thread_local bool t_GlowTraceHaveRaw = false;

// Glow hold (GlowHold / bmvr_glow_hold): per query (= per glow, eye and view)
// keep the brightest recent value while the game's value dips, then fade.
using PixelVisQueryFractionFn = float(__thiscall*)(void* query, float fadeTimeInv);
static PixelVisQueryFractionFn g_OrigPixelVisQueryFraction = nullptr;

static float __fastcall PixelVisQueryFractionHold(void* ecx, void* edx, float fadeTimeInv)
{
    (void)edx;
    const float raw = g_OrigPixelVisQueryFraction(ecx, fadeTimeInv);
    if (bmvr::glowtrace::Active())
    {
        t_GlowTraceRaw = raw;
        t_GlowTraceFadeInv = fadeTimeInv;
        t_GlowTraceHaveRaw = true;
    }
    const float hold = bmvr::g_GlowHold;
    if (!(hold > 0.f) || !std::isfinite(raw))
        return raw;
    struct Held { float shown; ULONGLONG seenMs; ULONGLONG holdUntilMs; };
    static std::unordered_map<const void*, Held> s_held;
    static ULONGLONG s_nextPurge;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG holdMs = static_cast<ULONGLONG>(hold * 1000.f);
    auto [it, fresh] = s_held.try_emplace(ecx, Held{ raw, now, now + holdMs });
    Held& h = it->second;
    // Not asked for a while: the query was idle or reused for another glow.
    if (fresh || now - h.seenMs > 500)
        h = Held{ raw, now, now + holdMs };
    else if (raw >= h.shown)
    {
        h.shown = raw;
        h.holdUntilMs = now + holdMs;
    }
    else if (now >= h.holdUntilMs)
    {
        const float fade = static_cast<float>(now - h.seenMs) / (hold * 1000.f);
        h.shown = (std::max)(raw, h.shown - fade);
    }
    h.seenMs = now;
    if (now >= s_nextPurge)
    {
        s_nextPurge = now + 10000;
        for (auto p = s_held.begin(); p != s_held.end();)
            p = (now - p->second.seenMs > 2000) ? s_held.erase(p) : std::next(p);
    }
    return h.shown;
}

// Save thumbnails. The game's writer grabs the bound render target with
// ReadPixels, scales it and writes the file itself. In VR that target is not a
// picture of the game: the desktop frame is black behind the pause menu, and in
// gameplay it can be the eye, of which a window-sized corner gets read. So for
// the length of the writer call the context's ReadPixels returns the left eye.
// Sizes come from the game's call, so any window resolution works.
using SaveShotWriterFn = void(__thiscall*)(void* view, const char* name, int w, int h, bool pow2, bool vtf);
static SaveShotWriterFn g_OrigSaveShotWriter = nullptr;
static volatile unsigned char* g_SaveShotNow = nullptr;

using SaveShotReadPixelsFn = void(__thiscall*)(void* ctx, int x, int y, int w, int h, unsigned char* data, int format);
static SaveShotReadPixelsFn g_RealSaveShotReadPixels = nullptr;
static DWORD g_SaveShotThread = 0;
static const char* g_SaveShotName = nullptr;
static constexpr int kImageFormatRgb888 = 2;

static void __fastcall SaveShotReadPixels(void* ecx, void* edx, int x, int y, int w, int h, unsigned char* data, int format)
{
    (void)edx;
    if (GetCurrentThreadId() == g_SaveShotThread && format == kImageFormatRgb888 && x == 0 && y == 0
        && Hooks::m_VR && Hooks::m_VR->FillSaveThumbnail(data, w, h))
    {
        Game::logMsg("Save thumbnail: %s <- left eye (grab %dx%d)", g_SaveShotName ? g_SaveShotName : "?", w, h);
        return;
    }
    g_RealSaveShotReadPixels(ecx, x, y, w, h, data, format);
}

static bool PatchSaveShotReadPixels(void** slot, void* fn)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *slot = fn;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

static void __fastcall SaveShotWriter(void* ecx, void* edx, const char* name, int w, int h, bool pow2, bool vtf)
{
    (void)edx;
    const char* shown = name ? name : "?";
    void* materials = (Hooks::m_VR && Hooks::m_Game && g_SaveShotNow && *g_SaveShotNow != 0
        && Hooks::m_VR->WantsSaveThumbnail()) ? Hooks::m_Game->m_MaterialSystem : nullptr;
    if (!materials)
    {
        g_OrigSaveShotWriter(ecx, name, w, h, pow2, vtf);
        return;
    }
    const VR::SaveThumbState state = Hooks::m_VR->PollSaveThumbnail();
    if (state == VR::SaveThumbState::Waiting)
    {
        // Not captured yet: with the flag clear the writer only queues the
        // request again, and the game calls it back at the end of the next frame.
        *g_SaveShotNow = 0;
        g_OrigSaveShotWriter(ecx, name, w, h, pow2, vtf);
        *g_SaveShotNow = 1;
        return;
    }
    if (state == VR::SaveThumbState::Failed)
    {
        Game::logMsg("Save thumbnail: %s: no left eye captured, the game's picture stays", shown);
        g_OrigSaveShotWriter(ecx, name, w, h, pow2, vtf);
        return;
    }
    using GetContextFn = void*(__thiscall*)(void*);
    using ReleaseFn = int(__thiscall*)(void*);
    void* ctx = (*reinterpret_cast<GetContextFn**>(materials))[Offsets::kIMaterialSystem_GetRenderContext / 4](materials);
    void** slot = ctx ? *reinterpret_cast<void***>(ctx) + Offsets::kIMatRenderContext_ReadPixels / 4 : nullptr;
    void* real = slot ? *slot : nullptr;
    g_RealSaveShotReadPixels = reinterpret_cast<SaveShotReadPixelsFn>(real);
    g_SaveShotThread = GetCurrentThreadId();
    g_SaveShotName = name;
    const bool patched = slot && PatchSaveShotReadPixels(slot, reinterpret_cast<void*>(&SaveShotReadPixels));
    if (!patched)
        Game::logMsg("Save thumbnail: %s: no ReadPixels hook, the game's picture stays", shown);
    g_OrigSaveShotWriter(ecx, name, w, h, pow2, vtf);
    if (patched)
        PatchSaveShotReadPixels(slot, real);
    g_SaveShotThread = 0;
    g_SaveShotName = nullptr;
    if (ctx)
        reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(ctx))[Offsets::kIMatRenderContext_Release / 4])(ctx);
    Hooks::m_VR->EndSaveThumbnail();
}

// Headless diagnostic (env BMVR_PIXELVIS_STATS=1): every 5 s, per eye, how
// often a glow's visible fraction fell from lit (>= 0.5) to near zero (< 0.05)
// since that eye's previous call for the same glow. Hooks
// CPixelVisibilitySystem::GetFractionVisible (thiscall, ret 8).
using PixelVisFractionFn = float(__thiscall*)(void* sys, const void* params, void* handle);
static PixelVisFractionFn g_OrigPixelVisFraction = nullptr;
static int g_PixelVisCreates[2];

static bool g_PixelVisStatsOn = false;

// pixelvis_queryparams_t starts with the glow's world position.
static bool ReadGlowPos(const void* params, float out[3])
{
    BMVR_TRY
    {
        const float* p = static_cast<const float*>(params);
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Glow smoothing (GlowSmooth / bmvr_glow_smooth): per glow and view (the
// eye offset of the view id left out), the latest fraction of each eye is
// averaged and low-passed, so both eyes show the same, steady brightness.
// An eye whose query has read 0 for GlowHideMs gets 0 at once and leaves
// the average: smoothing never keeps a glow lit behind a wall for longer
// than that (the engine's own fade is ~60 ms).
static double GlowNowMs()
{
    static LARGE_INTEGER s_freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(s_freq.QuadPart);
}

static float GlowStereoSmooth(const void* handle, int viewId, int eye, float raw)
{
    struct State
    {
        float raw[2] = {};
        double seenMs[2] = { -1e9, -1e9 };
        double zeroSinceMs[2] = { -1.0, -1.0 };   // -1 = not reading 0
        float ema = 0.f;
        double emaMs = 0.0;
    };
    static SRWLOCK s_lock = SRWLOCK_INIT;
    static std::unordered_map<uint64_t, State> s_glows;
    static double s_nextPurge;
    const double now = GlowNowMs();
    const double tauMs = bmvr::g_GlowSmooth * 1000.0;
    const double hideMs = bmvr::g_GlowHideMs;
    const int e = eye - 1, o = 1 - e;
    const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle)) << 16) | static_cast<uint16_t>(viewId);
    AcquireSRWLockExclusive(&s_lock);
    auto [it, fresh] = s_glows.try_emplace(key);
    State& st = it->second;
    // New, or not asked for a while (off screen, reused handle): start from now.
    if (fresh || now - (std::max)(st.seenMs[0], st.seenMs[1]) > 500.0)
    {
        st = State{};
        st.ema = raw;
        st.emaMs = now;
    }
    st.raw[e] = raw;
    st.seenMs[e] = now;
    if (raw > 0.f)
        st.zeroSinceMs[e] = -1.0;
    else if (st.zeroSinceMs[e] < 0.0)
        st.zeroSinceMs[e] = now;
    auto hidden = [&](int i) { return st.zeroSinceMs[i] >= 0.0 && now - st.zeroSinceMs[i] >= hideMs; };
    float out;
    if (hidden(e))
    {
        // Really hidden in this eye: dark now; if the other eye agrees (or is
        // gone), the glow restarts from 0 when it shows again.
        if (hidden(o) || now - st.seenMs[o] > 100.0)
            st.ema = 0.f;
        out = 0.f;
    }
    else
    {
        float sum = raw;
        int n = 1;
        if (now - st.seenMs[o] <= 100.0 && !hidden(o))
        {
            sum += st.raw[o];
            ++n;
        }
        const float target = sum / static_cast<float>(n);
        const double dt = now - st.emaMs;
        const float a = tauMs > 0.0 ? static_cast<float>(1.0 - std::exp(-(std::max)(dt, 0.0) / tauMs)) : 1.f;
        st.ema += a * (target - st.ema);
        out = st.ema;
    }
    st.emaMs = now;
    if (now >= s_nextPurge)
    {
        s_nextPurge = now + 10000.0;
        for (auto p = s_glows.begin(); p != s_glows.end();)
            p = (now - (std::max)(p->second.seenMs[0], p->second.seenMs[1]) > 2000.0) ? s_glows.erase(p) : std::next(p);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return out;
}

// GlowProxyScale: the query's test area (pixelvis_queryparams_t::proxySize,
// after the Vector position) widened on a copy of the params; the engine
// keeps one query set per proxy size, so a change applies from the next query.
struct PixelVisParamsCopy
{
    float position[3];
    float proxySize;
    float proxyAspect;
    float fadeTime;
    bool bSetup;
    bool bSizeInScreenspace;
};
static_assert(sizeof(PixelVisParamsCopy) == 28, "pixelvis_queryparams_t layout");

static const void* ScaledPixelVisParams(const void* params, PixelVisParamsCopy& copy)
{
    const float scale = bmvr::g_GlowProxyScale;
    if (!params || !(scale > 1.001f))
        return params;
    BMVR_TRY
    {
        std::memcpy(&copy, params, sizeof(copy));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return params; }
    if (!copy.bSetup || !std::isfinite(copy.proxySize) || copy.proxySize <= 0.f)
        return params;
    copy.proxySize *= scale;
    static int s_log;
    if (s_log < 6)
    {
        ++s_log;
        Game::logMsg("Glow proxy scale %.2f: proxySize %.4f -> %.4f aspect %.2f screenspace=%d",
            scale, copy.proxySize / scale, copy.proxySize, copy.proxyAspect, copy.bSizeInScreenspace ? 1 : 0);
    }
    return &copy;
}

static float __fastcall PixelVisFractionStats(void* ecx, void* edx, const void* params, void* handle)
{
    (void)edx;
    PixelVisParamsCopy scaled;
    params = ScaledPixelVisParams(params, scaled);
    const bool smooth = bmvr::g_GlowSmooth > 0.f;
    if (!smooth && !g_PixelVisStatsOn && !bmvr::glowtrace::Active())
        return g_OrigPixelVisFraction(ecx, params, handle);
    t_GlowTraceHaveRaw = false;
    float f = g_OrigPixelVisFraction(ecx, params, handle);
    const int eye = Hooks::m_VR ? Hooks::m_VR->m_StereoEye : 0;
    if (smooth && (eye == 1 || eye == 2) && std::isfinite(f) && g_ClientCurrentViewId)
        f = GlowStereoSmooth(handle, g_ClientCurrentViewId(), eye, f);
    if (!g_PixelVisStatsOn && !bmvr::glowtrace::Active())
        return f;
    if (bmvr::glowtrace::Active())
    {
        float pos[3];
        const bool havePos = params && ReadGlowPos(params, pos);
        const int vid = g_ClientCurrentViewId ? g_ClientCurrentViewId() : -1;
        const QAngle a = Hooks::m_VR ? Hooks::m_VR->m_HmdAngAbs : QAngle{};
        bmvr::glowtrace::Glow(eye, vid & 0xFFFF, reinterpret_cast<uintptr_t>(handle), havePos ? pos : nullptr,
            t_GlowTraceHaveRaw ? t_GlowTraceFadeInv : -1.f, t_GlowTraceHaveRaw ? t_GlowTraceRaw : -1.f, f,
            a.x, a.y, a.z);
    }
    if (!g_PixelVisStatsOn || (eye != 1 && eye != 2))
        return f;
    struct Stats { int samples = 0, lit = 0, drops = 0; };
    static std::unordered_map<uintptr_t, float> s_last[2];
    static Stats s_stats[2];
    static ULONGLONG s_next;
    const int e = eye - 1;
    // Per glow and view (eye views, monitor cameras, reflection/refraction).
    const int viewId = g_ClientCurrentViewId ? g_ClientCurrentViewId() : 0;
    const uintptr_t key = reinterpret_cast<uintptr_t>(handle) ^ (static_cast<uintptr_t>(viewId & 0xFF) << 24);
    static int s_viewDrops[16][2];
    const auto it = s_last[e].find(key);
    if (it != s_last[e].end() && it->second >= 0.5f && f < 0.05f)
    {
        ++s_stats[e].drops;
        ++s_viewDrops[viewId & 0xF][e];
    }
    // Pulsing: the fraction jumps by more than 0.2 between this glow's
    // consecutive samples, split by head roll (upright < 15 deg, tilted >= 30).
    static int s_swing[2][2], s_rollSamples[2][2];
    static double s_rollSum;
    static int s_rollN;
    float roll = Hooks::m_VR ? std::fabs(Hooks::m_VR->m_HmdAngAbs.z) : 0.f;
    if (roll > 180.f)
        roll = 360.f - roll;
    const int rb = roll < 15.f ? 0 : (roll >= 30.f ? 1 : -1);
    if (rb >= 0)
    {
        ++s_rollSamples[e][rb];
        if (it != s_last[e].end() && std::fabs(it->second - f) > 0.2f)
            ++s_swing[e][rb];
    }
    s_rollSum += roll;
    ++s_rollN;
    s_last[e][key] = f;
    ++s_stats[e].samples;
    if (f >= 0.5f)
        ++s_stats[e].lit;
    const ULONGLONG now = GetTickCount64();
    if (!s_next)
        s_next = now + 5000;
    if (now >= s_next)
    {
        Game::logMsg("pixelvis stats (per_eye=%d): creates id0=%d id0x100=%d glows L=%u R=%u samples L=%d R=%d lit L=%d R=%d drops L=%d R=%d",
            bmvr::g_PixelVisPerEye ? 1 : 0, g_PixelVisCreates[0], g_PixelVisCreates[1],
            static_cast<unsigned>(s_last[0].size()), static_cast<unsigned>(s_last[1].size()),
            s_stats[0].samples, s_stats[1].samples, s_stats[0].lit, s_stats[1].lit,
            s_stats[0].drops, s_stats[1].drops);
        char views[160] = {};
        int len = 0;
        for (int v = 0; v < 16; ++v)
        {
            if (s_viewDrops[v][0] || s_viewDrops[v][1])
                len += _snprintf(views + len, sizeof(views) - len, " view%d L=%d R=%d", v, s_viewDrops[v][0], s_viewDrops[v][1]);
            s_viewDrops[v][0] = s_viewDrops[v][1] = 0;
        }
        Game::logMsg("pixelvis drops by view:%s", len ? views : " none");
        Game::logMsg("pixelvis swings (>0.2 between samples) upright(<15deg) L=%d/%d R=%d/%d tilted(>=30deg) L=%d/%d R=%d/%d, mean |roll| %.1f",
            s_swing[0][0], s_rollSamples[0][0], s_swing[1][0], s_rollSamples[1][0],
            s_swing[0][1], s_rollSamples[0][1], s_swing[1][1], s_rollSamples[1][1],
            s_rollN ? s_rollSum / s_rollN : 0.0);
        std::memset(s_swing, 0, sizeof(s_swing));
        std::memset(s_rollSamples, 0, sizeof(s_rollSamples));
        s_rollSum = 0.0;
        s_rollN = 0;
        g_PixelVisCreates[0] = g_PixelVisCreates[1] = 0;
        s_stats[0] = Stats();
        s_stats[1] = Stats();
        s_next = now + 5000;
    }
    return f;
}

// Diagnostic: CPixelVisibilitySystem's find-or-create query for (set, view id)
// (thiscall, ret 8); counts the creations per view id (a glow's query being
// thrown away and restarted at brightness 0).
using PixelVisQueryForViewFn = unsigned(__thiscall*)(void* sys, void* set, unsigned viewId);
static PixelVisQueryForViewFn g_OrigPixelVisQueryForView = nullptr;

static unsigned __fastcall PixelVisQueryForViewStats(void* ecx, void* edx, void* set, unsigned viewId)
{
    (void)edx;
    // Same list walk as the original: the set's query list holds one query per view id.
    const auto* sys = static_cast<const unsigned char*>(ecx);
    const auto* heads = *reinterpret_cast<unsigned char* const*>(sys + 0x50);
    const auto* queries = *reinterpret_cast<unsigned char* const*>(sys + 0x44);
    unsigned short q = *reinterpret_cast<const unsigned short*>(heads + *reinterpret_cast<const unsigned short*>(static_cast<unsigned char*>(set) + 0xC) * 10);
    bool found = false;
    for (int guard = 0; q != 0xFFFF && guard < 64; ++guard)
    {
        if (*reinterpret_cast<const unsigned short*>(queries + 0x22 + q * 0x28) == (viewId & 0xFFFF))
        {
            found = true;
            break;
        }
        q = *reinterpret_cast<const unsigned short*>(queries + 0x26 + q * 0x28);
    }
    if (!found)
        ++g_PixelVisCreates[viewId >= 0x100 ? 1 : 0];
    return g_OrigPixelVisQueryForView(ecx, set, viewId);
}

// Lens flares lay out their ghosts and fade them by the distance from the middle
// of the render target. On an off-centre eye that is not where the eye looks, so
// the same light got different ghosts and brightness in each eye. The position
// helper hands the layout coordinates relative to the eye's view axis; the quad
// draw shifts them back.
using FlareWorldToScreenFn = void(__cdecl*)(const float* world, const float* size, float* out);
static FlareWorldToScreenFn g_OrigFlareWorldToScreen = nullptr;
using FlareDrawQuadFn = void(__thiscall*)(void* flare, void* material, int x, int y, int w, int h, void* color, float angle);
static FlareDrawQuadFn g_OrigFlareDrawQuad = nullptr;
static void* const* g_FlareMaterials = nullptr;

// Screen-space quads (DrawScreenSpaceRectangle: underwater warp + post chain; the lens flare quad
// draw) push identity view + projection and draw in clip space, but keep the current MODEL matrix
// (materialsystem DSSR body and client flare quad, disassembled 2026-11-17: MatrixMode 0/1 only,
// both end on MatrixMode(1) + PopMatrix). Flat Black Mesa happens to have an identity model there.
// A leftover translation in it drew the underwater warp only in the top-right part of the eye and
// the flares away from their light, for a whole launch (handover 2026-11-17). Every check logs a
// non-identity model (on change); with ScreenSpaceModelFix the draw gets identity, and the mode is
// left as the stock functions leave it (PROJECTION).
namespace ScreenModel
{
    // IMatRenderContext slots (offsets.h notes): 0x04 Release, 0x08 BeginRender, 0x0C EndRender,
    // 0x50 MatrixMode, 0x54 PushMatrix, 0x58 PopMatrix, 0x78 GetMatrix(mode, VMatrix*), 0x7C LoadIdentity.
    constexpr int kModeProjection = 1;
    constexpr int kModeModel = 10;   // MATERIAL_MODEL (client.dll pushes 0xA before MatrixMode)
    using VoidFn = void(__thiscall*)(void*);
    using IntFn = void(__thiscall*)(void*, int);
    using GetMatrixFn = void(__thiscall*)(void*, int, float*);

    inline void** Vt(void* ctx) { return *reinterpret_cast<void***>(ctx); }

    inline void GetModel(void* ctx, float* m)
    {
        reinterpret_cast<GetMatrixFn>(Vt(ctx)[0x78 / 4])(ctx, kModeModel, m);
    }

    inline bool IsIdentity(const float* m)
    {
        for (int i = 0; i < 16; ++i)
            if (std::fabs(m[i] - ((i % 5) == 0 ? 1.f : 0.f)) > 1e-4f)
                return false;
        return true;
    }

    struct SiteStats
    {
        const char* name;
        unsigned draws = 0, bad = 0, logged = 0;
        float last[16] = {};
        ULONGLONG nextSummaryMs = 0;
        unsigned summaryDraws = 0, summaryBad = 0;
    };

    // Checks the model matrix before a screen-space draw. Returns true when the draw should be
    // wrapped (fix on). One site per calling thread (DSSR: drawing thread; flares: client thread).
    inline bool Check(SiteStats& st, void* ctx, const char* mat)
    {
        if (!ctx || !Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
            return false;
        float m[16];
        GetModel(ctx, m);
        ++st.draws;
        if (!IsIdentity(m))
        {
            ++st.bad;
            if (st.logged < 200 && std::memcmp(m, st.last, sizeof(m)) != 0)
            {
                ++st.logged;
                std::memcpy(st.last, m, sizeof(m));
                Game::logMsg("Screen model: %s %s eye=%d tid=%lu model NOT identity: r0=(%.4g %.4g %.4g %.4g) r1=(%.4g %.4g %.4g %.4g) r2=(%.4g %.4g %.4g %.4g) r3=(%.4g %.4g %.4g %.4g) -> %s",
                    st.name, mat ? mat : "?", Hooks::m_VR->m_StereoEye, GetCurrentThreadId(),
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15],
                    bmvr::g_ScreenSpaceModelFix ? "identity for this draw" : "kept (fix off)");
            }
        }
        // Summary every 10 s when something was drawn: a good launch logs "0 not identity".
        const ULONGLONG now = GetTickCount64();
        if (now >= st.nextSummaryMs)
        {
            if (st.draws != st.summaryDraws)
                Game::logMsg("Screen model: %s %u draws, %u not identity (fix %s)", st.name, st.draws, st.bad,
                    bmvr::g_ScreenSpaceModelFix ? "on" : "off");
            st.summaryDraws = st.draws;
            st.summaryBad = st.bad;
            st.nextSummaryMs = now + 10000;
        }
        return bmvr::g_ScreenSpaceModelFix;
    }

    inline void Begin(void* ctx)
    {
        void** vt = Vt(ctx);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, kModeModel);
        reinterpret_cast<VoidFn>(vt[0x54 / 4])(ctx);
        reinterpret_cast<VoidFn>(vt[0x7C / 4])(ctx);
    }

    inline void End(void* ctx)
    {
        void** vt = Vt(ctx);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, kModeModel);
        reinterpret_cast<VoidFn>(vt[0x58 / 4])(ctx);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, kModeProjection);
    }

    // Diagnostic around code that may leave the model matrix changed (held-particle replay,
    // particle-level hooks): logs when the model differs after the call. Logs only.
    struct Probe
    {
        void* ctx = nullptr;
        float before[16] = {};
        explicit Probe(void* c) : ctx(c)
        {
            if (ctx)
                GetModel(ctx, before);
        }
        void After(const char* where)
        {
            if (!ctx)
                return;
            float after[16];
            GetModel(ctx, after);
            if (std::memcmp(before, after, sizeof(after)) == 0)
                return;
            static std::atomic<int> s_lines{ 0 };
            if (s_lines.fetch_add(1) >= 200)
                return;
            Game::logMsg("Model matrix changed by %s eye=%d tid=%lu: before t=(%.4g %.4g %.4g)%s after r0=(%.4g %.4g %.4g %.4g) r1=(%.4g %.4g %.4g %.4g) r2=(%.4g %.4g %.4g %.4g)%s",
                where, Hooks::m_VR ? Hooks::m_VR->m_StereoEye : -1, GetCurrentThreadId(),
                before[3], before[7], before[11], IsIdentity(before) ? " (identity)" : "",
                after[0], after[1], after[2], after[3], after[4], after[5], after[6], after[7],
                after[8], after[9], after[10], after[11], IsIdentity(after) ? " (identity)" : "");
        }
    };

    // The client's render context (AddRef'd + BeginRender), for code that has no context at hand.
    struct ClientCtx
    {
        void* ctx = nullptr;
        ClientCtx()
        {
            if (!g_FlareMaterials || !*g_FlareMaterials)
                return;
            void* materials = *g_FlareMaterials;
            using GetContextFn = void*(__thiscall*)(void*);
            ctx = (*reinterpret_cast<GetContextFn**>(materials))[0x19C / 4](materials);
            if (ctx)
                reinterpret_cast<VoidFn>(Vt(ctx)[0x08 / 4])(ctx);
        }
        ~ClientCtx()
        {
            if (!ctx)
                return;
            reinterpret_cast<VoidFn>(Vt(ctx)[0x0C / 4])(ctx);
            reinterpret_cast<VoidFn>(Vt(ctx)[0x04 / 4])(ctx);
        }
        ClientCtx(const ClientCtx&) = delete;
        ClientCtx& operator=(const ClientCtx&) = delete;
    };
}
// Flare log (bmvr::g_FlareLog): every switch-on is a new session, so each
// logger starts with fresh line caps and forgets what it has seen.
static unsigned g_FlareLogSession = 0;
static bool FlareLogActive()
{
    static bool s_wasOn = false;
    const bool on = bmvr::g_FlareLog;
    if (on != s_wasOn)
    {
        s_wasOn = on;
        if (on)
            ++g_FlareLogSession;
        Game::logMsg("flare log %s (session %u)", on ? "on" : "off", g_FlareLogSession);
    }
    return on;
}
static float g_FlareAxisShiftX = 0.f;
static float g_FlareAxisShiftY = 0.f;
static bool g_FlareElementOnLight = true;
static float g_FlareAngleFade = 1.f;
static float g_FlareRoll = 0.f;
// client CurrentViewOrigin() global: the view of the pass being drawn.
static const float* g_ClientViewOrigin = nullptr;

// Diagnostic, BMVR_FLARE_LOG=1: what a lens flare's position is computed from.
// One line per flare and eye when the projection or on-screen state changes,
// otherwise once a second.
static void FlareLogPosition(const float* world, const float* size, const float* out, const float* view, const float* proj)
{
    static int s_lines;
    struct Seen { int sig; ULONGLONG nextMs; };
    static std::unordered_map<uint64_t, Seen> s_seen;
    static unsigned s_session;
    if (s_session != g_FlareLogSession)
    {
        s_session = g_FlareLogSession;
        s_lines = 0;
        s_seen.clear();
    }
    if (s_lines >= 20000)
        return;
    const int eye = Hooks::m_VR ? Hooks::m_VR->m_StereoEye : 0;
    const bool onScreen = out[0] >= 0.f && out[1] >= 0.f && out[0] <= size[0] && out[1] <= size[1];
    const int sig = static_cast<int>(proj[0] * 100.f) * 31 + static_cast<int>(proj[2] * 100.f) * 17
        + static_cast<int>(proj[5] * 100.f) * 7 + static_cast<int>(proj[6] * 100.f) + (onScreen ? 100000 : 0);
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int>(world[0]))) << 32)
        ^ (static_cast<uint32_t>(static_cast<int>(world[1])) << 2) ^ static_cast<uint32_t>(eye);
    const ULONGLONG now = GetTickCount64();
    auto [it, fresh] = s_seen.try_emplace(key, Seen{ sig, 0 });
    if (!fresh && it->second.sig == sig && now < it->second.nextMs)
        return;
    const bool changed = !fresh && it->second.sig != sig;
    it->second = Seen{ sig, now + 1000 };
    ++s_lines;
    // Camera position implied by the view matrix.
    const float cam[3] = {
        -(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]),
        -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]),
        -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11]) };
    Game::logMsg("flare %s t=%llu eye=%d view=%d stereo=%d world=(%.0f,%.0f,%.0f) size=%.0fx%.0f out=(%.0f,%.0f)%s"
        " proj=(%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f | %.3f %.3f)"
        " viewFwd=(%.3f %.3f %.3f) cam=(%.1f,%.1f,%.1f) tid=%lu",
        changed ? "CHANGE" : "state", now, eye, g_ClientCurrentViewId ? g_ClientCurrentViewId() : -1,
        Hooks::m_VR && Hooks::m_VR->StereoWorldPassActive() ? 1 : 0,
        world[0], world[1], world[2], size[0], size[1], out[0], out[1], onScreen ? "" : " OFFSCREEN",
        proj[0], proj[1], proj[2], proj[3], proj[4], proj[5], proj[6], proj[7],
        proj[10], proj[11], proj[14], proj[15],
        view[8], view[9], view[10], cam[0], cam[1], cam[2], GetCurrentThreadId());
}

// Screen angle of the world horizontal through the light, perpendicular to the
// line of sight. Rows 0 and 1 of the view matrix are the camera's right and up.
// Straight above or below, the horizon has no direction; the turn fades out there.
static float FlareLevelRoll(const float* world, const float* view)
{
    const float cam[3] = {
        -(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]),
        -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]),
        -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11]) };
    const float d[3] = { world[0] - cam[0], world[1] - cam[1], world[2] - cam[2] };
    const float dLen = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float h[3] = { -d[1], d[0], 0.f }; // world up x d
    const float hLen = std::sqrt(h[0] * h[0] + h[1] * h[1]);
    if (dLen < 1.f || hLen < 1e-3f)
        return 0.f;
    const float hx = view[0] * h[0] + view[1] * h[1] + view[2] * h[2];
    const float hy = view[4] * h[0] + view[5] * h[1] + view[6] * h[2];
    // Counter-clockwise on screen, as the quad draw turns a positive angle.
    float a = std::atan2(hy, hx);
    // A streak looks the same turned half way round.
    const float pi = 3.14159265f;
    while (a > pi * 0.5f) a -= pi;
    while (a <= -pi * 0.5f) a += pi;
    const float level = (std::min)(1.f, hLen / dLen / 0.2f);
    return a * level;
}

static void __cdecl FlareWorldToScreenHook(const float* world, const float* size, float* out)
{
    g_OrigFlareWorldToScreen(world, size, out);
    g_FlareAxisShiftX = 0.f;
    g_FlareAxisShiftY = 0.f;
    if (!g_FlareMaterials || !*g_FlareMaterials)
        return;
    void* materials = *g_FlareMaterials;
    using GetContextFn = void*(__thiscall*)(void*);
    void* ctx = (*reinterpret_cast<GetContextFn**>(materials))[0x19C / 4](materials);
    if (!ctx)
        return;
    using VoidFn = void(__thiscall*)(void*);
    using GetMatrixFn = void(__thiscall*)(void*, int, float*);
    void** vt = *reinterpret_cast<void***>(ctx);
    reinterpret_cast<VoidFn>(vt[0x08 / 4])(ctx);
    float view[16] = {}, proj[16] = {};
    reinterpret_cast<GetMatrixFn>(vt[0x78 / 4])(ctx, 0, view);
    reinterpret_cast<GetMatrixFn>(vt[0x78 / 4])(ctx, 1, proj);
    reinterpret_cast<VoidFn>(vt[0x0C / 4])(ctx);
    reinterpret_cast<VoidFn>(vt[0x04 / 4])(ctx);
    if (FlareLogActive())
        FlareLogPosition(world, size, out, view, proj);
    // Where the view axis lands: NDC (-m02, -m12), screen y pointing down.
    if (bmvr::g_FlareEyeAxis && g_OrigFlareDrawQuad)
    {
        g_FlareAxisShiftX = -proj[2] * size[0] * 0.5f;
        g_FlareAxisShiftY = proj[6] * size[1] * 0.5f;
        // One layout for both eyes, from the point between them: the angle to
        // the axis decides how the rays turn, and near the axis it differs a
        // lot per eye. Each eye then gets that layout at its own view of the light.
        const int eye = Hooks::m_VR ? Hooks::m_VR->m_StereoEye : 0;
        if (bmvr::g_FlareSharedLayout && (eye == 1 || eye == 2))
        {
            const float depth = -(view[8] * world[0] + view[9] * world[1] + view[10] * world[2] + view[11]);
            if (depth > 1.f)
            {
                const float halfIpd = Hooks::m_VR->RenderHalfIpd();
                const float parallax = proj[0] * halfIpd / depth * size[0] * 0.5f;
                g_FlareAxisShiftX += eye == 1 ? parallax : -parallax;
            }
        }
        out[0] -= g_FlareAxisShiftX;
        out[1] -= g_FlareAxisShiftY;
    }
    // Turn the layers so the streaks lie along the world horizon at the light:
    // the eye's image rolls with the head, a quad drawn level in it does too.
    g_FlareRoll = 0.f;
    if (bmvr::g_FlareLevel && Hooks::m_VR && Hooks::m_VR->m_StereoEye != 0)
        g_FlareRoll = FlareLevelRoll(world, view);
    // Fade by the angle off the view axis, seen from between the eyes.
    g_FlareAngleFade = 1.f;
    if (bmvr::g_FlareFadeAngle > 0.f && proj[0] > 0.01f && proj[5] > 0.01f && size[0] > 0.f && size[1] > 0.f)
    {
        const float tanX = (out[0] - size[0] * 0.5f) / (size[0] * 0.5f) / proj[0];
        const float tanY = (out[1] - size[1] * 0.5f) / (size[1] * 0.5f) / proj[5];
        const float deg = std::atan(std::sqrt(tanX * tanX + tanY * tanY)) * 57.29578f;
        g_FlareAngleFade = (std::max)(0.f, 1.f - deg / bmvr::g_FlareFadeAngle);
    }
}

// Element types 1 and 4 sit on the line from the light through the middle, at a
// set fraction: 0 is on the light. The other types are always on the light.
using FlareElementFn = void(__thiscall*)(void* flare, float scale, float x, float y, float z, const unsigned char* element, float* size);
static FlareElementFn g_OrigFlareElement = nullptr;

static void __fastcall FlareElementHook(void* ecx, void* edx, float scale, float x, float y, float z, const unsigned char* element, float* size)
{
    (void)edx;
    const int type = *reinterpret_cast<const int*>(element);
    const float along = *reinterpret_cast<const float*>(element + 0x30);
    g_FlareElementOnLight = !(type == 1 || type == 4) || std::abs(along) < 0.01f;
    g_OrigFlareElement(ecx, scale, x, y, z, element, size);
    g_FlareElementOnLight = true;
}

static void __fastcall FlareDrawQuadHook(void* ecx, void* edx, void* material, int x, int y, int w, int h, void* color, float angle)
{
    (void)edx;
    // Layers placed away from the light (see FlareElementHook) wash out the view
    // in a headset: the large square ones most, the rest as moving ghosts.
    const int eyeH = Hooks::m_VR ? static_cast<int>(Hooks::m_VR->m_RenderHeight) : 0;
    const bool onLight = g_FlareElementOnLight;
    const bool square = std::abs(w - h) * 8 < (std::max)(w, h);
    const bool haze = !onLight && square && w > bmvr::g_FlareHazeMinSize * eyeH;
    const float scale = (onLight ? bmvr::g_FlareLightScale : (haze ? bmvr::g_FlareHazeScale : bmvr::g_FlareGhostScale)) * g_FlareAngleFade;
    // Smaller as well as dimmer, down to half size.
    const float shrink = 0.5f + 0.5f * g_FlareAngleFade;
    const int drawW = static_cast<int>(w * shrink);
    const int drawH = static_cast<int>(h * shrink);
    x += (w - drawW) / 2;
    y += (h - drawH) / 2;
    if (bmvr::g_FlareLog && Hooks::m_VR && Hooks::m_VR->m_StereoEye != 0)
    {
        static int s_quads;
        static unsigned s_session;
        if (s_session != g_FlareLogSession)
        {
            s_session = g_FlareLogSession;
            s_quads = 0;
        }
        if (s_quads < 120)
        {
            ++s_quads;
            Game::logMsg("flarequad eye=%d mat=%p %dx%d at %d,%d %s angle=%.3f roll=%.3f", Hooks::m_VR->m_StereoEye, material, w, h, x, y,
                onLight ? "light" : (haze ? "haze" : "ghost"), angle, g_FlareRoll);
        }
    }
    unsigned char dim[4];
    if (color && scale < 1.f)
    {
        if (scale <= 0.f)
            return;
        std::memcpy(dim, color, 4);
        for (int i = 0; i < 3; ++i)
            dim[i] = static_cast<unsigned char>(dim[i] * scale);
        color = dim;
    }
    x += static_cast<int>(g_FlareAxisShiftX);
    y += static_cast<int>(g_FlareAxisShiftY);
    if (g_FlareRoll != 0.f && Hooks::m_VR && Hooks::m_VR->m_RenderWidth > 0 && Hooks::m_VR->m_RenderHeight > 0)
    {
        // The draw turns the quad about the middle of its bottom edge, in NDC.
        // Move it so the extra turn happens about the quad's own centre, which
        // for the layers on the light is the light.
        const float ndcX = 2.f / static_cast<float>(Hooks::m_VR->m_RenderWidth);
        const float ndcY = 2.f / static_cast<float>(Hooks::m_VR->m_RenderHeight);
        // Pixel offset (y down) of the centre from the pivot after turning by t.
        auto centreOffset = [&](float t, float& ox, float& oy) {
            const float uy = drawH * 0.5f * ndcY; // straight up in NDC
            ox = -std::sin(t) * uy / ndcX;
            oy = -std::cos(t) * uy / ndcY;
        };
        float ax, ay, bx, by;
        centreOffset(angle, ax, ay);
        centreOffset(angle + g_FlareRoll, bx, by);
        x += static_cast<int>(std::lround(ax - bx));
        y += static_cast<int>(std::lround(ay - by));
        angle += g_FlareRoll;
    }
    // The quad draw uses the client's render context; identity model around it (ScreenModel above).
    static ScreenModel::SiteStats s_flareModel{ "flare quad" };
    ScreenModel::ClientCtx cc;
    const bool wrap = ScreenModel::Check(s_flareModel, cc.ctx, "lensflare");
    if (wrap)
        ScreenModel::Begin(cc.ctx);
    g_OrigFlareDrawQuad(ecx, material, x, y, drawW, drawH, color, angle);
    if (wrap)
        ScreenModel::End(cc.ctx);
}

// Same switch: how visible the flare is for each eye. Below the draw threshold
// the position above is never computed, so this also covers hidden flares.
using FlareFractionFn = float(__thiscall*)(void* flare, void* leaf, const float* pos);
static FlareFractionFn g_OrigFlareFraction = nullptr;

static float __fastcall FlareFractionLog(void* ecx, void* edx, void* leaf, const float* pos)
{
    (void)edx;
    const float f = g_OrigFlareFraction(ecx, leaf, pos);
    if (!FlareLogActive())
        return f;
    static int s_lines;
    struct Seen { int bucket; ULONGLONG nextMs; };
    static std::unordered_map<uint64_t, Seen> s_seen;
    static unsigned s_session;
    if (s_session != g_FlareLogSession)
    {
        s_session = g_FlareLogSession;
        s_lines = 0;
        s_seen.clear();
    }
    if (s_lines >= 30000)
        return f;
    const int eye = Hooks::m_VR ? Hooks::m_VR->m_StereoEye : 0;
    const int bucket = f <= 0.02f ? 0 : (f < 0.98f ? 1 : 2);
    const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ecx)) << 2) | static_cast<uint32_t>(eye);
    const ULONGLONG now = GetTickCount64();
    auto [it, fresh] = s_seen.try_emplace(key, Seen{ bucket, 0 });
    if (!fresh && it->second.bucket == bucket && now < it->second.nextMs)
        return f;
    const bool changed = !fresh && it->second.bucket != bucket;
    it->second = Seen{ bucket, now + 500 };
    ++s_lines;
    float view[16] = {};
    if (g_FlareMaterials && *g_FlareMaterials)
    {
        void* materials = *g_FlareMaterials;
        using GetContextFn = void*(__thiscall*)(void*);
        void* ctx = (*reinterpret_cast<GetContextFn**>(materials))[0x19C / 4](materials);
        if (ctx)
        {
            using VoidFn = void(__thiscall*)(void*);
            using GetMatrixFn = void(__thiscall*)(void*, int, float*);
            void** vt = *reinterpret_cast<void***>(ctx);
            reinterpret_cast<VoidFn>(vt[0x08 / 4])(ctx);
            reinterpret_cast<GetMatrixFn>(vt[0x78 / 4])(ctx, 0, view);
            reinterpret_cast<VoidFn>(vt[0x0C / 4])(ctx);
            reinterpret_cast<VoidFn>(vt[0x04 / 4])(ctx);
        }
    }
    Game::logMsg("flarefrac %s t=%llu eye=%d view=%d f=%.3f world=(%.0f,%.0f,%.0f) cam=(%.1f,%.1f,%.1f)",
        changed ? "CHANGE" : "state", now, eye, g_ClientCurrentViewId ? g_ClientCurrentViewId() : -1, f,
        pos[0], pos[1], pos[2],
        -(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]),
        -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]),
        -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11]));
    return f;
}

static void InstallFlareHooks(uintptr_t client)
{
    static const unsigned char kPosPrologue[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xE4, 0x00, 0x00, 0x00 };
    static const unsigned char kQuadPrologue[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x20, 0x02, 0x00, 0x00 };
    auto* pos = reinterpret_cast<unsigned char*>(client + Offsets::kLensflareWorldToScreenRva);
    auto* quad = reinterpret_cast<unsigned char*>(client + Offsets::kLensflareDrawQuadRva);
    if (std::memcmp(pos, kPosPrologue, sizeof(kPosPrologue)) != 0
        || std::memcmp(quad, kQuadPrologue, sizeof(kQuadPrologue)) != 0)
    {
        Game::logMsg("Lens flare eye axis: client+0x%X / +0x%X MISMATCH (off)",
            Offsets::kLensflareWorldToScreenRva, Offsets::kLensflareDrawQuadRva);
        Game::noteMissingOffset("Lens flare position + quad draw (per-eye flare layout)");
        return;
    }
    g_FlareMaterials = reinterpret_cast<void* const*>(client + Offsets::kClientMaterialsPtrRva);
    if (MH_CreateHook(quad, reinterpret_cast<LPVOID>(&FlareDrawQuadHook),
            reinterpret_cast<LPVOID*>(&g_OrigFlareDrawQuad)) != MH_OK
        || MH_EnableHook(quad) != MH_OK)
    {
        g_OrigFlareDrawQuad = nullptr;
        Game::logMsg("Lens flare eye axis: quad hook failed (off)");
        return;
    }
    if (MH_CreateHook(pos, reinterpret_cast<LPVOID>(&FlareWorldToScreenHook),
            reinterpret_cast<LPVOID*>(&g_OrigFlareWorldToScreen)) != MH_OK
        || MH_EnableHook(pos) != MH_OK)
    {
        Game::logMsg("Lens flare eye axis: position hook failed (off)");
        return;
    }
    static const unsigned char kElementPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0x56, 0x57 };
    auto* element = reinterpret_cast<unsigned char*>(client + Offsets::kLensflareElementRva);
    if (std::memcmp(element, kElementPrologue, sizeof(kElementPrologue)) != 0
        || MH_CreateHook(element, reinterpret_cast<LPVOID>(&FlareElementHook),
            reinterpret_cast<LPVOID*>(&g_OrigFlareElement)) != MH_OK
        || MH_EnableHook(element) != MH_OK)
        Game::logMsg("Lens flare layers: element hook failed, all layers stay on");
    Game::logMsg("Lens flare eye axis hooks on (FlareEyeAxis=%d)", bmvr::g_FlareEyeAxis ? 1 : 0);

    // The log itself is switched at runtime (panel DEBUG / bmvr_flare_log); the
    // visibility hook only adds a flag check while it is off.
    char v[8] = {};
    if (GetEnvironmentVariableA("BMVR_FLARE_LOG", v, sizeof(v)) && v[0] == '1')
        bmvr::g_FlareLog = true;
    static const unsigned char kFractionPrologue[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x83, 0xEC, 0x1C };
    auto* frac = reinterpret_cast<unsigned char*>(client + Offsets::kLensflareFractionRva);
    if (std::memcmp(frac, kFractionPrologue, sizeof(kFractionPrologue)) != 0)
        Game::logMsg("flare log: fraction client+0x%X MISMATCH (off)", Offsets::kLensflareFractionRva);
    else if (MH_CreateHook(frac, reinterpret_cast<LPVOID>(&FlareFractionLog),
            reinterpret_cast<LPVOID*>(&g_OrigFlareFraction)) == MH_OK)
        MH_EnableHook(frac);
}

// Performance log (PerfLog=true in VR/config.txt, or env BMVR_WORLDBATCH_STATS=1):
// per engine world batcher call (eye, client view id, mode, sort group) the
// number of surface groups, triangles and world static mesh changes, i.e. the
// engine's batch count with its 127 limit ("Too many vertex format changes in
// frame, whole world not rendered": the rest of that group is skipped). Every
// 5 s one line per key that drew anything, plus the player's view position;
// a call at the limit is logged at once (at most once per 2 s).
using EngineWorldBatchFn = void(__cdecl*)(void* ctx, void* list, int group, int mode);
static EngineWorldBatchFn g_OrigEngineWorldBatch = nullptr;
static uintptr_t g_EngineBase = 0;

struct WorldBatchCount { int groups = 0, batches = 0, dyn = 0, distinct = 0; unsigned tris = 0; };

static bool CountWorldBatches(void* list, int group, WorldBatchCount& c)
{
    BMVR_TRY
    {
        auto* base = static_cast<unsigned char*>(list);
        auto* vec = base + (group + 3) * 20;
        auto** elems = *reinterpret_cast<unsigned char***>(vec);
        const int count = *reinterpret_cast<int*>(vec + 0xC);
        auto* surfList = *reinterpret_cast<unsigned char**>(base);
        auto** meshes = *reinterpret_cast<void***>(g_EngineBase + Offsets::kEngineWorldStaticMeshesRva);
        void* last = nullptr;
        void* seen[64];
        for (int i = 0; i < count && elems && surfList && meshes; ++i)
        {
            const unsigned char* g = elems[i];
            const short h = *reinterpret_cast<const short*>(g);
            if (h == -1)
                continue;
            const unsigned char* surf = *reinterpret_cast<unsigned char* const*>(surfList + h * 64 + 4);
            if (!surf)
                continue;
            ++c.groups;
            if (surf[0] & 0x80)
            {
                ++c.dyn;
                continue;
            }
            c.tris += *reinterpret_cast<const unsigned short*>(g + 0xC);
            void* mesh = meshes[*reinterpret_cast<const short*>(surf + 0x12)];
            if (mesh != last)
            {
                ++c.batches;
                last = mesh;
                bool known = false;
                for (int k = 0; k < c.distinct; ++k)
                    if (seen[k] == mesh) { known = true; break; }
                if (!known && c.distinct < 64)
                    seen[c.distinct++] = mesh;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

static void __cdecl EngineWorldBatchStats(void* ctx, void* list, int group, int mode)
{
    WorldBatchCount c;
    const bool ok = CountWorldBatches(list, group, c);
    struct Agg { int calls = 0, over = 0, maxGroups = 0, maxBatches = 0, maxDistinct = 0, maxDyn = 0; long long groups = 0, batches = 0, tris = 0; };
    static std::map<unsigned, Agg> s_agg;
    static ULONGLONG s_next, s_nextOver;
    static int s_overSince;
    const int eye = Hooks::m_VR ? Hooks::m_VR->m_StereoEye : 0;
    const int viewId = g_ClientCurrentViewId ? g_ClientCurrentViewId() : -1;
    const Vector pos = Hooks::m_VR ? Hooks::m_VR->m_SetupOrigin : Vector{};
    const unsigned key = (static_cast<unsigned>(eye & 0xF) << 24) | (static_cast<unsigned>(viewId & 0xFFF) << 12)
        | (static_cast<unsigned>(mode & 0xF) << 4) | static_cast<unsigned>(group & 0xF);
    const ULONGLONG now = GetTickCount64();
    if (ok)
    {
        Agg& a = s_agg[key];
        ++a.calls;
        a.groups += c.groups;
        a.batches += c.batches;
        a.tris += c.tris;
        a.maxGroups = (std::max)(a.maxGroups, c.groups);
        a.maxBatches = (std::max)(a.maxBatches, c.batches);
        a.maxDistinct = (std::max)(a.maxDistinct, c.distinct);
        a.maxDyn = (std::max)(a.maxDyn, c.dyn);
        if (c.batches >= 127)
        {
            ++a.over;
            ++s_overSince;
            if (now >= s_nextOver)
            {
                Game::logMsg("worldbatch LIMIT eye=%d view=%d mode=%d group=%d: groups=%d (dynamic %d) batches=%d distinct meshes=%d tris=%u "
                    "pos=(%.0f,%.0f,%.0f) calls at limit since last=%d",
                    eye, viewId, mode, group, c.groups, c.dyn, c.batches, c.distinct, c.tris, pos.x, pos.y, pos.z, s_overSince);
                s_overSince = 0;
                s_nextOver = now + 2000;
            }
        }
    }
    if (!s_next)
        s_next = now + 5000;
    if (now >= s_next)
    {
        Game::logMsg("worldbatch 5s: pos=(%.0f,%.0f,%.0f) keys=%u", pos.x, pos.y, pos.z, static_cast<unsigned>(s_agg.size()));
        for (const auto& [k, a] : s_agg)
            if (a.maxGroups > 0)
                Game::logMsg("worldbatch eye=%u view=%d mode=%u group=%u: calls=%d groups avg %lld max %d (dyn max %d), "
                    "batches avg %lld max %d (distinct meshes max %d), tris avg %lld, at limit %d",
                    k >> 24, static_cast<int>((k >> 12) & 0xFFF), (k >> 4) & 0xF, k & 0xF, a.calls,
                    a.groups / a.calls, a.maxGroups, a.maxDyn, a.batches / a.calls, a.maxBatches, a.maxDistinct,
                    a.tris / a.calls, a.over);
        s_agg.clear();
        s_next = now + 5000;
    }
    g_OrigEngineWorldBatch(ctx, list, group, mode);
}

static void InstallEngineWorldBatchStats()
{
    char v[8] = {};
    const bool env = GetEnvironmentVariableA("BMVR_WORLDBATCH_STATS", v, sizeof(v)) && v[0] == '1';
    if ((!env && !bmvr::g_PerfLog) || g_OrigEngineWorldBatch)
        return;
    g_EngineBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine.dll"));
    if (!g_EngineBase)
        return;
    auto* fn = reinterpret_cast<unsigned char*>(g_EngineBase + Offsets::kEngineWorldBatchRva);
    static const unsigned char kPrologue[] = { 0x55, 0x8B, 0xEC, 0xB8, 0x94, 0x18, 0x00, 0x00 };
    if (std::memcmp(fn, kPrologue, sizeof(kPrologue)) != 0)
    {
        Game::logMsg("worldbatch stats: prologue MISMATCH (not hooked)");
        return;
    }
    if (MH_CreateHook(fn, reinterpret_cast<LPVOID>(&EngineWorldBatchStats),
            reinterpret_cast<LPVOID*>(&g_OrigEngineWorldBatch)) == MH_OK
        && MH_EnableHook(fn) == MH_OK)
        Game::logMsg("Perf log: world batch counters on (%s), GPU pass timing %s",
            env ? "BMVR_WORLDBATCH_STATS" : "PerfLog", (bmvr::g_GpuPassTiming || bmvr::g_PerfLog) ? "on" : "off");
}

// PerfLog bone/particle diagnostics: is bone setup or particle simulation
// redone per eye? Each object (entity / particle collection) is tracked per
// client frame (frame-start hook); a second recompute of the same object in
// the same frame is counted, by the eye being drawn when it happened.
namespace
{
    std::atomic<unsigned> g_ClientFrameSeq{ 0 };
    std::mutex g_RepeatMutex;

    // Returns true when obj was already seen in this client frame (by kind).
    bool SeenThisFrame(int kind, const void* obj)
    {
        static std::unordered_map<const void*, unsigned> s_seen[4];
        const unsigned frame = g_ClientFrameSeq.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_RepeatMutex);
        auto& m = s_seen[kind & 3];
        if (m.size() > 8192)
            m.clear();
        auto [it, fresh] = m.try_emplace(obj, frame);
        if (fresh)
            return false;
        const bool again = it->second == frame;
        it->second = frame;
        return again;
    }

    using ClientFrameStartFn = void(__cdecl*)();
    ClientFrameStartFn g_OrigClientFrameStart = nullptr;
    void __cdecl ClientFrameStartProf()
    {
        g_ClientFrameSeq.fetch_add(1, std::memory_order_relaxed);
        BMVR_PROF("client frame start (bones+particles)");
        g_OrigClientFrameStart();
    }

    using SetupBonesFn = bool(__thiscall*)(void* self, void* out, int maxBones, int mask, float time);
    SetupBonesFn g_OrigSetupBones = nullptr;
    bool __fastcall SetupBonesProf(void* ecx, void* edx, void* out, int maxBones, int mask, float time)
    {
        (void)edx;
        BMVR_PROF("client SetupBones (incl. cached)");
        return g_OrigSetupBones(ecx, out, maxBones, mask, time);
    }

    using BuildTransformationsFn = void(__thiscall*)(void* self, void* hdr, void* pos, void* q, const void* cam, int mask, void* computed);
    BuildTransformationsFn g_OrigBuildTransformations = nullptr;
    void __fastcall BuildTransformationsProf(void* ecx, void* edx, void* hdr, void* pos, void* q, const void* cam, int mask, void* computed)
    {
        (void)edx;
        if (SeenThisFrame(0, ecx))
            BMVR_PROF_COUNT("bones rebuilt again same frame");
        BMVR_PROF("client BuildTransformations (bone recompute)");
        g_OrigBuildTransformations(ecx, hdr, pos, q, cam, mask, computed);
    }

    using ParticleCollectionSimulateFn = void(__thiscall*)(void* self, float dt, bool flag);
    ParticleCollectionSimulateFn g_OrigParticleCollectionSimulate = nullptr;
    void __fastcall ParticleCollectionSimulateProf(void* ecx, void* edx, float dt, bool flag)
    {
        (void)edx;
        if (SeenThisFrame(1, ecx))
            BMVR_PROF_COUNT("particles simulated again same frame");
        BMVR_PROF("client CParticleCollection::Simulate");
        g_OrigParticleCollectionSimulate(ecx, dt, flag);
    }

    using ParticleMgrSimulateFn = void(__thiscall*)(void* self, float dt);
    ParticleMgrSimulateFn g_OrigParticleMgrSimulate = nullptr;
    void __fastcall ParticleMgrSimulateProf(void* ecx, void* edx, float dt)
    {
        (void)edx;
        BMVR_PROF("client ParticleMgr()->Simulate");
        g_OrigParticleMgrSimulate(ecx, dt);
    }
}

static void InstallBoneParticleProf(uintptr_t client)
{
    if (!bmvr::g_PerfLog || g_OrigClientFrameStart)
        return;
    struct H { uint32_t rva; const unsigned char* bytes; size_t n; void* detour; void** orig; const char* name; };
    static const unsigned char kFrame[] = { 0x55, 0x8B, 0xEC, 0x51 };
    static const unsigned char kBones[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x68, 0x0E, 0x00, 0x00 };
    static const unsigned char kBuild[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00 };
    static const unsigned char kPcSim[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30 };
    static const unsigned char kPmSim[] = { 0x55, 0x8B, 0xEC, 0x83, 0x79, 0x64, 0x00 };
    const H hooks[] = {
        { Offsets::kClientFrameStartRva, kFrame, sizeof(kFrame), reinterpret_cast<void*>(&ClientFrameStartProf), reinterpret_cast<void**>(&g_OrigClientFrameStart), "frame start" },
        { Offsets::kClientSetupBonesRva, kBones, sizeof(kBones), reinterpret_cast<void*>(&SetupBonesProf), reinterpret_cast<void**>(&g_OrigSetupBones), "SetupBones" },
        { Offsets::kClientBuildTransformationsRva, kBuild, sizeof(kBuild), reinterpret_cast<void*>(&BuildTransformationsProf), reinterpret_cast<void**>(&g_OrigBuildTransformations), "BuildTransformations" },
        { Offsets::kClientParticleCollectionSimulateRva, kPcSim, sizeof(kPcSim), reinterpret_cast<void*>(&ParticleCollectionSimulateProf), reinterpret_cast<void**>(&g_OrigParticleCollectionSimulate), "CParticleCollection::Simulate" },
        { Offsets::kClientParticleMgrSimulateRva, kPmSim, sizeof(kPmSim), reinterpret_cast<void*>(&ParticleMgrSimulateProf), reinterpret_cast<void**>(&g_OrigParticleMgrSimulate), "ParticleMgr::Simulate" },
    };
    for (const H& h : hooks)
    {
        auto* fn = reinterpret_cast<unsigned char*>(client + h.rva);
        if (std::memcmp(fn, h.bytes, h.n) != 0)
        {
            Game::logMsg("Perf log: %s prologue MISMATCH (not hooked)", h.name);
            continue;
        }
        const bool ok = MH_CreateHook(fn, h.detour, h.orig) == MH_OK && MH_EnableHook(fn) == MH_OK;
        Game::logMsg("Perf log: %s hook %s", h.name, ok ? "on" : "FAILED");
    }
}

// Old-style particles (CParticleMgr: smoke stacks, smoke trails, dust; material
// e.g. particle/particle_smokegrenade1) are built on the CPU in camera space:
// RenderStart stores the view as their model-view and loads an identity view,
// and each particle's quad corners are added in camera space. In a headset the
// camera rolls with the head, so the smoke rolled with it. In a VR eye pass the
// view given to RenderStart is turned about its forward axis until world up is
// straight up in it (the "levelled" view the particles are built in), and the
// view loaded for the draw turns it back: particle centres land where they did,
// the quads keep world up. They still turn with head yaw. bmvr_particle_level.
namespace
{
    using ParticleRenderStartFn = void(__thiscall*)(void* self, float* tempModel, float* tempView);
    ParticleRenderStartFn g_OrigParticleRenderStart = nullptr;

    // Angle that turns the view's up onto world up as seen in it (fades out
    // looking straight up/down); 0 = nothing to do. view: VMatrix, row major.
    float ParticleLevelAngle(const float* view)
    {
        const float ux = view[2], uy = view[6], uz = view[10];
        const float n = std::sqrt(ux * ux + uy * uy);
        const float len = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (n < 1e-4f || len < 1e-6f)
            return 0.f;
        float phi = std::atan2(-ux, uy);
        if (n / len < 0.2f)
            phi *= (n / len) / 0.2f;
        return phi;
    }

    void __fastcall ParticleRenderStartLevel(void* self, void* edx, float* tempModel, float* tempView)
    {
        (void)edx;
        BMVR_PROF("hook ParticleRenderStart");
        const bool cameraSpace = self && (*(reinterpret_cast<unsigned char*>(self) + 0x94) & 0x80) != 0;
        if (!bmvr::g_ParticleLevel || !cameraSpace || !Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled
            || Hooks::m_VR->m_StereoEye == 0 || !tempView)
        {
            g_OrigParticleRenderStart(self, tempModel, tempView);
            return;
        }
        void* mgr = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x90);
        void* materials = mgr ? *reinterpret_cast<void**>(reinterpret_cast<char*>(mgr) + 0x64) : nullptr;
        using GetContextFn = void*(__thiscall*)(void*);
        void* ctx = materials ? (*reinterpret_cast<GetContextFn**>(materials))[0x19C / 4](materials) : nullptr;
        if (!ctx)
        {
            g_OrigParticleRenderStart(self, tempModel, tempView);
            return;
        }
        using VoidFn = void(__thiscall*)(void*);
        using IntFn = void(__thiscall*)(void*, int);
        using GetMatrixFn = void(__thiscall*)(void*, int, float*);
        using MatFn = void(__thiscall*)(void*, const float*);
        void** vt = *reinterpret_cast<void***>(ctx);
        reinterpret_cast<VoidFn>(vt[0x08 / 4])(ctx);
        float view[16] = {};
        reinterpret_cast<GetMatrixFn>(vt[0x78 / 4])(ctx, 0 /* MATERIAL_VIEW */, view);
        const float phi = ParticleLevelAngle(view);
        if (std::fabs(phi) < 1e-4f)
        {
            reinterpret_cast<VoidFn>(vt[0x0C / 4])(ctx);
            reinterpret_cast<VoidFn>(vt[0x04 / 4])(ctx);
            g_OrigParticleRenderStart(self, tempModel, tempView);
            return;
        }
        const float c = std::cos(phi), s = std::sin(phi);
        // Levelled view: Rz(-phi) * view (rows 0 and 1 mixed).
        float level[16];
        std::memcpy(level, view, sizeof(level));
        for (int k = 0; k < 4; ++k)
        {
            level[k] = c * view[k] + s * view[4 + k];
            level[4 + k] = -s * view[k] + c * view[4 + k];
        }
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 0 /* MATERIAL_VIEW */);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, level);
        // Stores the levelled view as tempView + the particles' model-view, sets
        // the user clip transform, loads identity model and view.
        g_OrigParticleRenderStart(self, tempModel, tempView);
        // Draw view: levelled camera space -> real camera space.
        const float back[16] = { c, -s, 0.f, 0.f, s, c, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 0 /* MATERIAL_VIEW */);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, back);
        // World -> clip is the real view again for the user clip planes, and
        // RenderEnd (inlined in DrawModel) restores the real view from tempView.
        reinterpret_cast<MatFn>(vt[0x1F8 / 4])(ctx, view);
        std::memcpy(tempView, view, sizeof(view));
        reinterpret_cast<VoidFn>(vt[0x0C / 4])(ctx);
        reinterpret_cast<VoidFn>(vt[0x04 / 4])(ctx);
        static int s_log;
        if (s_log < 4)
        {
            ++s_log;
            Game::logMsg("particle level: camera-space effect turned %.1f deg (eye %d)", phi * 57.2958f,
                Hooks::m_VR->m_StereoEye);
        }
    }
}

// Black Mesa's precipitation renderer (rain, and the smoke/fog volumes made with
// it) builds its quads in camera space the same way, per entity inside one
// function. Around the whole call: the view becomes the levelled view (the quads
// are built in it) and the projection gets the turn back (P * Rz(phi)), so
// world -> clip is unchanged for the quad centres and the user clip planes;
// both are restored afterwards.
namespace
{
    using PrecipRenderFn = void(__cdecl*)();
    PrecipRenderFn g_OrigPrecipRender = nullptr;

    void __cdecl PrecipRenderLevel()
    {
        BMVR_PROF("hook PrecipRender");
        void* materials = (g_FlareMaterials && *g_FlareMaterials) ? *g_FlareMaterials : nullptr;
        if (!bmvr::g_ParticleLevel || !materials || !Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled
            || Hooks::m_VR->m_StereoEye == 0)
        {
            g_OrigPrecipRender();
            return;
        }
        using GetContextFn = void*(__thiscall*)(void*);
        void* ctx = (*reinterpret_cast<GetContextFn**>(materials))[0x19C / 4](materials);
        if (!ctx)
        {
            g_OrigPrecipRender();
            return;
        }
        using VoidFn = void(__thiscall*)(void*);
        using IntFn = void(__thiscall*)(void*, int);
        using GetMatrixFn = void(__thiscall*)(void*, int, float*);
        using MatFn = void(__thiscall*)(void*, const float*);
        void** vt = *reinterpret_cast<void***>(ctx);
        reinterpret_cast<VoidFn>(vt[0x08 / 4])(ctx);
        float view[16] = {}, proj[16] = {};
        reinterpret_cast<GetMatrixFn>(vt[0x78 / 4])(ctx, 0 /* MATERIAL_VIEW */, view);
        reinterpret_cast<GetMatrixFn>(vt[0x78 / 4])(ctx, 1 /* MATERIAL_PROJECTION */, proj);
        const float phi = ParticleLevelAngle(view);
        if (std::fabs(phi) < 1e-4f)
        {
            reinterpret_cast<VoidFn>(vt[0x0C / 4])(ctx);
            reinterpret_cast<VoidFn>(vt[0x04 / 4])(ctx);
            g_OrigPrecipRender();
            return;
        }
        const float c = std::cos(phi), s = std::sin(phi);
        float level[16], turned[16];
        std::memcpy(level, view, sizeof(level));
        std::memcpy(turned, proj, sizeof(turned));
        for (int k = 0; k < 4; ++k)
        {
            level[k] = c * view[k] + s * view[4 + k];
            level[4 + k] = -s * view[k] + c * view[4 + k];
        }
        for (int r = 0; r < 4; ++r)
        {
            turned[r * 4 + 0] = c * proj[r * 4 + 0] + s * proj[r * 4 + 1];
            turned[r * 4 + 1] = -s * proj[r * 4 + 0] + c * proj[r * 4 + 1];
        }
        ScreenModel::Probe probe(ctx);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 1);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, turned);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 0);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, level);
        g_OrigPrecipRender();
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 1);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, proj);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 0);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, view);
        probe.After("precipitation level hook");
        reinterpret_cast<VoidFn>(vt[0x0C / 4])(ctx);
        reinterpret_cast<VoidFn>(vt[0x04 / 4])(ctx);
        static int s_log;
        if (s_log < 4)
        {
            ++s_log;
            Game::logMsg("particle level: precipitation/smoke volumes turned %.1f deg (eye %d)", phi * 57.2958f,
                Hooks::m_VR->m_StereoEye);
        }
    }
}

// .pcf sprites with a non-spritecard material (most room haze/smoke) are built
// on the CPU in camera space from the current view (the SDK's
// RenderNonSpriteCardCameraFacing). Same turn as the precipitation renderer,
// on the render context the renderer is given.
namespace
{
    using PcfCameraFacingFn = void(__thiscall*)(void* self, void* particles, void* context, void* ctx, void* material);
    PcfCameraFacingFn g_OrigPcfCameraFacing = nullptr;

    void __fastcall PcfCameraFacingLevel(void* self, void* edx, void* particles, void* context, void* ctx, void* material)
    {
        (void)edx;
        BMVR_PROF("hook PcfCameraFacing");
        if (!bmvr::g_ParticleLevel || !ctx || !Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled || Hooks::m_VR->m_StereoEye == 0)
        {
            g_OrigPcfCameraFacing(self, particles, context, ctx, material);
            return;
        }
        using IntFn = void(__thiscall*)(void*, int);
        using GetMatrixFn = void(__thiscall*)(void*, int, float*);
        using MatFn = void(__thiscall*)(void*, const float*);
        void** vt = *reinterpret_cast<void***>(ctx);
        float view[16] = {}, proj[16] = {};
        reinterpret_cast<GetMatrixFn>(vt[0x78 / 4])(ctx, 0 /* MATERIAL_VIEW */, view);
        reinterpret_cast<GetMatrixFn>(vt[0x78 / 4])(ctx, 1 /* MATERIAL_PROJECTION */, proj);
        const float phi = ParticleLevelAngle(view);
        if (std::fabs(phi) < 1e-4f)
        {
            g_OrigPcfCameraFacing(self, particles, context, ctx, material);
            return;
        }
        const float c = std::cos(phi), s = std::sin(phi);
        float level[16], turned[16];
        std::memcpy(level, view, sizeof(level));
        std::memcpy(turned, proj, sizeof(turned));
        for (int k = 0; k < 4; ++k)
        {
            level[k] = c * view[k] + s * view[4 + k];
            level[4 + k] = -s * view[k] + c * view[4 + k];
        }
        for (int r = 0; r < 4; ++r)
        {
            turned[r * 4 + 0] = c * proj[r * 4 + 0] + s * proj[r * 4 + 1];
            turned[r * 4 + 1] = -s * proj[r * 4 + 0] + c * proj[r * 4 + 1];
        }
        ScreenModel::Probe probe(ctx);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 1);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, turned);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 0);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, level);
        g_OrigPcfCameraFacing(self, particles, context, ctx, material);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 1);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, proj);
        reinterpret_cast<IntFn>(vt[0x50 / 4])(ctx, 0);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ctx, view);
        probe.After("pcf level hook");
        static int s_log;
        if (s_log < 4)
        {
            ++s_log;
            Game::logMsg("particle level: pcf camera-facing sprites turned %.1f deg (eye %d)", phi * 57.2958f,
                Hooks::m_VR->m_StereoEye);
        }
    }
}

// "MSAA skips particles" (VR panel): in the main view's translucent pass the
// particle systems are held back; when the pass is done the multisampled eye
// switches to its 1x resolve and they are drawn there (blended overdraw on 4
// samples was ~40 % of the 4x cost in particle-heavy rooms). Models, ropes
// and glass of that pass stay multisampled. Without particles in view nothing
// switches. View 0 = VIEW_MAIN: the 3D skybox (1) and the water views (3 / 4)
// are left alone.
using DrawTranslucentAFn = void(__fastcall*)(void* ecx, void* edx, int arg);
using DrawTranslucentBFn = void(__fastcall*)(void* ecx, void* edx);
using DrawTranslucentRenderableFn = void(__cdecl*)(void* ent, int twoPass, int a3, int a4, void* viewData);
static DrawTranslucentAFn g_OrigDrawTranslucentA = nullptr;
static DrawTranslucentBFn g_OrigDrawTranslucentB = nullptr;
static DrawTranslucentRenderableFn g_OrigDrawTranslucentRenderable = nullptr;

namespace
{
    struct HeldParticle { void* ent; int twoPass, a3, a4; void* viewData; };
    // Game thread only.
    int g_TranslucentDepth = 0;
    bool g_TranslucentHold = false;
    bool g_TranslucentHoldTo1x = false;   // "MSAA skips particles": held particles go to the 1x resolve
    bool g_TranslucentMainView = false;   // the outermost pass is VIEW_MAIN's
    std::vector<HeldParticle> g_HeldParticles;

    // Particle system renderable? By the MSVC RTTI class name behind the
    // renderable's vtable (CNewParticleEffect, CParticleEffectBinding, ...),
    // looked up once per vtable.
    bool IsParticleRenderable(void* ent)
    {
        static std::unordered_map<const void*, bool> s_byVtable;
        const void* vt = nullptr;
        BMVR_TRY { vt = *reinterpret_cast<const void* const*>(ent); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        const auto it = s_byVtable.find(vt);
        if (it != s_byVtable.end())
            return it->second;
        bool particle = false;
        char name[96] = {};
        BMVR_TRY
        {
            const auto* col = reinterpret_cast<const uint32_t* const*>(vt)[-1];   // RTTICompleteObjectLocator
            const char* raw = reinterpret_cast<const char*>(col[3]) + 8;         // TypeDescriptor name
            strncpy_s(name, raw, _TRUNCATE);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { name[0] = 0; }
        particle = std::strstr(name, "Particle") != nullptr;
        s_byVtable.emplace(vt, particle);
        if (particle)
            Game::logMsg("MSAA skips particles: renderable class %s is drawn on the 1x resolve", name);
        return particle;
    }

    void TranslucentPassEnter()
    {
        if (g_TranslucentDepth++ != 0)
            return;
        const bool mainView = Hooks::m_VR && g_ClientCurrentViewId && g_ClientCurrentViewId() == 0;
        g_TranslucentMainView = mainView;
        g_TranslucentHoldTo1x = mainView && bmvr::g_MsaaParticles1x && Hooks::m_VR->UseVrMsaa();
        // ParticlesLast: same hold as a pure draw-order change (no 1x switch).
        g_TranslucentHold = g_TranslucentHoldTo1x || (mainView && bmvr::g_ParticlesLast);
    }

    void TranslucentPassLeave()
    {
        if (--g_TranslucentDepth != 0)
            return;
        g_TranslucentHold = false;
        if (!g_HeldParticles.empty())
        {
            if (g_TranslucentHoldTo1x && Hooks::m_VR)
                Hooks::m_VR->NoteTranslucentPassStart();
            // Diagnostic: does the replay (after the engine's own loop) leave the model matrix changed?
            ScreenModel::ClientCtx cc;
            ScreenModel::Probe probe(cc.ctx);
            for (const HeldParticle& h : g_HeldParticles)
                g_OrigDrawTranslucentRenderable(h.ent, h.twoPass, h.a3, h.a4, h.viewData);
            probe.After("held-particle replay");
            g_HeldParticles.clear();
        }
        if (g_TranslucentMainView && bmvr::g_RefractOncePerEye && Hooks::m_VR && Hooks::m_VR->m_IsVREnabled)
            Hooks::m_VR->NoteWorldTranslucentsDone();
    }
}

static void __cdecl DrawTranslucentRenderableHook(void* ent, int twoPass, int a3, int a4, void* viewData)
{
    if (g_TranslucentHold && ent && IsParticleRenderable(ent))
    {
        g_HeldParticles.push_back({ ent, twoPass, a3, a4, viewData });
        return;
    }
    g_OrigDrawTranslucentRenderable(ent, twoPass, a3, a4, viewData);
}

static void __fastcall DrawTranslucentAHook(void* ecx, void* edx, int arg)
{
    TranslucentPassEnter();
    g_OrigDrawTranslucentA(ecx, edx, arg);
    TranslucentPassLeave();
}

static void __fastcall DrawTranslucentBHook(void* ecx, void* edx)
{
    TranslucentPassEnter();
    g_OrigDrawTranslucentB(ecx, edx);
    TranslucentPassLeave();
}

// ShadowsOncePerFrame: the deferred lights' shadow atlas is view independent but the engine
// renders it in every view, so once per eye. The first eye of a stereo pair renders as usual
// and records each call; the second eye skips a call that matches one of them (same light,
// type, atlas slot and flags = the same tile with the same content, still in the atlas since
// nothing else writes it in between). Anything else (a light only the second eye queues, a
// static light's first render whose flags change between the eyes) renders normally.
using RenderLightShadowFn = void(__fastcall*)(void* ecx, void* edx, void* light, int isStatic, int arg3,
    int copyStatic, int arg5, int arg6);
static RenderLightShadowFn g_OrigRenderLightShadow = nullptr;
namespace
{
    struct ShadowCallKey
    {
        void* light;
        int type, slot;
        uint32_t flags;  // the five bool/int args packed (low bytes)
        int arg3;
        uint32_t pose[6];  // light +0x20..+0x34 (the shadow view's origin and angles, raw bits)
        bool operator==(const ShadowCallKey& o) const
        {
            return light == o.light && type == o.type && slot == o.slot && flags == o.flags && arg3 == o.arg3
                && std::memcmp(pose, o.pose, sizeof(pose)) == 0;
        }
    };
    struct ShadowOnceState
    {
        int pairSeq = -1;
        int firstEye = 0;
        int n = 0;
        ShadowCallKey keys[64];
        // stats (logged every 2000 second-eye calls)
        int skipped = 0, rendered = 0;
    };
    ShadowOnceState g_ShadowOnce;
}

static void __fastcall RenderLightShadowHook(void* ecx, void* edx, void* light, int isStatic, int arg3,
    int copyStatic, int arg5, int arg6)
{
    VR* vr = Hooks::m_VR;
    const int eye = vr ? vr->m_StereoEye : 0;
    if (!light || (eye != 1 && eye != 2) || !vr->m_IsVREnabled)
    {
        g_OrigRenderLightShadow(ecx, edx, light, isStatic, arg3, copyStatic, arg5, arg6);
        return;
    }
    const auto* l = static_cast<const unsigned char*>(light);
    ShadowCallKey key{ light, *reinterpret_cast<const int*>(l + 4), *reinterpret_cast<const int*>(l + 0x48),
        (static_cast<uint32_t>(isStatic & 0xFF)) | (static_cast<uint32_t>(copyStatic & 0xFF) << 8)
            | (static_cast<uint32_t>(arg5 & 0xFF) << 16) | (static_cast<uint32_t>(arg6 & 0xFF) << 24),
        arg3, {} };
    std::memcpy(key.pose, l + 0x20, sizeof(key.pose));
    ShadowOnceState& st = g_ShadowOnce;
    if (st.pairSeq != bmvr::g_StereoPairSeq)
    {
        st.pairSeq = bmvr::g_StereoPairSeq;
        st.firstEye = eye;
        st.n = 0;
    }
    if (eye == st.firstEye)
    {
        g_OrigRenderLightShadow(ecx, edx, light, isStatic, arg3, copyStatic, arg5, arg6);
        if (st.n < static_cast<int>(sizeof(st.keys) / sizeof(st.keys[0])))
            st.keys[st.n++] = key;
        return;
    }
    bool match = bmvr::g_ShadowsOnceTestSkipAll;  // test: negative control (skips mismatches too)
    if (!match && bmvr::g_ShadowsOncePerFrame)
    {
        for (int i = 0; i < st.n; ++i)
        {
            if (st.keys[i] == key)
            {
                match = true;
                break;
            }
        }
    }
    if (match)
        ++st.skipped;
    else
    {
        ++st.rendered;
        static int s_missLog;
        if (s_missLog < 12 && bmvr::g_ShadowsOncePerFrame)
        {
            ++s_missLog;
            const ShadowCallKey* same = nullptr;
            for (int i = 0; i < st.n; ++i)
                if (st.keys[i].light == light)
                    same = &st.keys[i];
            Game::logMsg("Shadows once: second eye renders light %p type %d slot %d flags %08X arg3 %d; first eye %s",
                light, key.type, key.slot, key.flags, key.arg3,
                !same ? "did not render it"
                    : same->slot != key.slot ? "used another slot"
                    : same->flags != key.flags || same->arg3 != key.arg3 ? "had other flags"
                    : "had another pose");
        }
        g_OrigRenderLightShadow(ecx, edx, light, isStatic, arg3, copyStatic, arg5, arg6);
    }
    if (st.skipped + st.rendered >= 2000)
    {
        Game::logMsg("Shadows once per frame: second eye skipped %d, rendered %d light shadows (%s)",
            st.skipped, st.rendered, bmvr::g_ShadowsOncePerFrame ? "on" : "off");
        st.skipped = st.rendered = 0;
    }
}

static void InstallRenderLightShadowHook(uintptr_t client)
{
    static const unsigned char kSig[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x34, 0x02, 0x00, 0x00, 0xA1 };
    auto* fn = reinterpret_cast<unsigned char*>(client + Offsets::kClientRenderLightShadowRva);
    if (std::memcmp(fn, kSig, sizeof(kSig)) != 0)
    {
        Game::logMsg("light shadows once per frame: MISMATCH (not hooked; shadows render per eye)");
        return;
    }
    const bool ok = MH_CreateHook(fn, reinterpret_cast<LPVOID>(&RenderLightShadowHook),
                        reinterpret_cast<LPVOID*>(&g_OrigRenderLightShadow)) == MH_OK
        && MH_EnableHook(fn) == MH_OK;
    Game::logMsg("light shadows once per frame: hook %s", ok ? "on" : "FAILED");
}

static void InstallTranslucentPassHooks(uintptr_t client)
{
    static const unsigned char kA[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x54, 0xA1 };
    static const unsigned char kB[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0xA1 };
    static const unsigned char kR[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0xA1 };
    auto* a = reinterpret_cast<unsigned char*>(client + Offsets::kClientDrawTranslucentARva);
    auto* b = reinterpret_cast<unsigned char*>(client + Offsets::kClientDrawTranslucentBRva);
    auto* r = reinterpret_cast<unsigned char*>(client + Offsets::kClientDrawTranslucentRenderableRva);
    if (std::memcmp(a, kA, sizeof(kA)) != 0 || std::memcmp(b, kB, sizeof(kB)) != 0 || std::memcmp(r, kR, sizeof(kR)) != 0)
    {
        Game::logMsg("translucent pass: MISMATCH (not hooked; MSAA skips particles has no effect)");
        return;
    }
    const bool ok = MH_CreateHook(r, reinterpret_cast<LPVOID>(&DrawTranslucentRenderableHook), reinterpret_cast<LPVOID*>(&g_OrigDrawTranslucentRenderable)) == MH_OK
        && MH_EnableHook(r) == MH_OK
        && MH_CreateHook(a, reinterpret_cast<LPVOID>(&DrawTranslucentAHook), reinterpret_cast<LPVOID*>(&g_OrigDrawTranslucentA)) == MH_OK
        && MH_EnableHook(a) == MH_OK
        && MH_CreateHook(b, reinterpret_cast<LPVOID>(&DrawTranslucentBHook), reinterpret_cast<LPVOID*>(&g_OrigDrawTranslucentB)) == MH_OK
        && MH_EnableHook(b) == MH_OK;
    Game::logMsg("translucent pass: hooks %s", ok ? "on" : "FAILED");
}

// Sound listener. CViewRender::SetUpView hands the engine the player view as
// the listener (engine->SetAudioState). That view carries the engine view
// angles, which CreateMove sets to the aim hand, so sounds panned with the
// right controller (a waterfall on the right came from the left ear when
// pointing at it). In VR gameplay the listener gets the head pose instead:
// HMD angles (scripted camera + head inside point_viewcontrol) and the
// centre-eye origin, as the mono path of applyL4d2VrHead in RenderView.
namespace
{
    struct AudioStateBm
    {
        Vector origin;
        Vector angles;
        bool underwater;
    };
    static_assert(sizeof(AudioStateBm) == 28, "Host_SetAudioState copies 7 dwords");
    using SetAudioStateFn = void(__thiscall*)(void* self, const AudioStateBm& state);
    SetAudioStateFn g_OrigSetAudioState = nullptr;

    void __fastcall SetAudioStateHmd(void* ecx, void* edx, const AudioStateBm& in)
    {
        (void)edx;
        VR* vr = Hooks::m_VR;
        if (!vr || !vr->m_IsVREnabled || !vr->m_HmdPoseValid || !vr->IsGameplayEligible()
            || !std::isfinite(in.origin.x) || !std::isfinite(in.origin.y) || !std::isfinite(in.origin.z))
        {
            g_OrigSetAudioState(ecx, in);
            return;
        }
        AudioStateBm st = in;
        Vector camRight;
        if (!(vr->ScriptedCameraActive() && vr->ScriptedCameraView(st.angles, camRight)))
        {
            st.angles = vr->GetViewAngle();
            st.origin = vr->GetViewOrigin(in.origin);
        }
        static int s_logged;
        if (s_logged < 3 && fabsf(in.angles.y - st.angles.y) > 5.f)
        {
            ++s_logged;
            Game::logMsg("Sound listener: view yaw %.1f -> head yaw %.1f", in.angles.y, st.angles.y);
        }
        g_OrigSetAudioState(ecx, st);
    }
}

static void InstallSoundListenerHmd()
{
    const auto engine = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine.dll"));
    if (!engine)
        return;
    // push ebp; mov ebp,esp; push [ebp+8]; call Host_SetAudioState, whose body is
    // push ebp; mov ebp,esp; push esi; mov esi,[ebp+8]; mov ecx,7
    auto* fn = reinterpret_cast<unsigned char*>(engine + Offsets::kEngineSetAudioStateRva);
    static const unsigned char kWrap[] = { 0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x08, 0xE8 };
    static const unsigned char kHost[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0xB9, 0x07, 0x00, 0x00, 0x00 };
    bool match = std::memcmp(fn, kWrap, sizeof(kWrap)) == 0;
    if (match)
    {
        int32_t rel;
        std::memcpy(&rel, fn + 7, 4);
        match = std::memcmp(fn + 11 + rel, kHost, sizeof(kHost)) == 0;
    }
    if (!match)
    {
        Game::logMsg("Sound listener: SetAudioState MISMATCH (not hooked; sounds pan with the aim hand)");
        Game::noteMissingOffset("SetAudioState (sound listener follows the head)");
        return;
    }
    const bool ok = MH_CreateHook(fn, reinterpret_cast<LPVOID>(&SetAudioStateHmd),
                        reinterpret_cast<LPVOID*>(&g_OrigSetAudioState)) == MH_OK
        && MH_EnableHook(fn) == MH_OK;
    Game::logMsg("Sound listener: SetAudioState hook %s", ok ? "on" : "FAILED");
}

static void InstallParticleLevel(uintptr_t client)
{
    {
        static const unsigned char kPcf[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xC8, 0x02, 0x00, 0x00, 0xA1 };
        auto* cfn = reinterpret_cast<unsigned char*>(client + Offsets::kClientPcfCameraFacingRva);
        if (std::memcmp(cfn, kPcf, sizeof(kPcf)) != 0)
            Game::logMsg("particle level: pcf camera-facing renderer MISMATCH (not hooked)");
        else
        {
            const bool cok = MH_CreateHook(cfn, reinterpret_cast<LPVOID>(&PcfCameraFacingLevel),
                                 reinterpret_cast<LPVOID*>(&g_OrigPcfCameraFacing)) == MH_OK
                && MH_EnableHook(cfn) == MH_OK;
            Game::logMsg("particle level: pcf camera-facing renderer hook %s", cok ? "on" : "FAILED");
        }
    }
    {
        static const unsigned char kPrecip[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x38, 0x02, 0x00, 0x00, 0xA1 };
        auto* pfn = reinterpret_cast<unsigned char*>(client + Offsets::kClientPrecipRenderRva);
        if (std::memcmp(pfn, kPrecip, sizeof(kPrecip)) != 0)
            Game::logMsg("particle level: precipitation render MISMATCH (not hooked)");
        else
        {
            const bool pok = MH_CreateHook(pfn, reinterpret_cast<LPVOID>(&PrecipRenderLevel),
                                 reinterpret_cast<LPVOID*>(&g_OrigPrecipRender)) == MH_OK
                && MH_EnableHook(pfn) == MH_OK;
            Game::logMsg("particle level: precipitation render hook %s", pok ? "on" : "FAILED");
        }
    }
    static const unsigned char kStart[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48, 0xA1 };
    auto* fn = reinterpret_cast<unsigned char*>(client + Offsets::kClientParticleRenderStartRva);
    if (std::memcmp(fn, kStart, sizeof(kStart)) != 0 || fn[0x1D] != 0xF6 || fn[0x1F] != 0x94)
    {
        Game::logMsg("particle level: RenderStart MISMATCH (not hooked)");
        return;
    }
    const bool ok = MH_CreateHook(fn, reinterpret_cast<LPVOID>(&ParticleRenderStartLevel),
                        reinterpret_cast<LPVOID*>(&g_OrigParticleRenderStart)) == MH_OK
        && MH_EnableHook(fn) == MH_OK;
    Game::logMsg("particle level: RenderStart hook %s", ok ? "on" : "FAILED");
}

static uintptr_t g_PerfLogClient;
static bool g_PerfLogApplied;
static void InstallEngineWorldBatchStats();
static void InstallBoneParticleProf(uintptr_t client);

// VR panel Debug -> PerfLog switched while playing: the stats that are only
// hooked when PerfLog is on at startup are hooked now; the rest are flag checks
// (the wait profiler starts on the next frame by itself).
static void ApplyPerfLogLive()
{
    if (bmvr::g_PerfLog == g_PerfLogApplied || !g_PerfLogClient)
        return;
    g_PerfLogApplied = bmvr::g_PerfLog;
    char v[8] = {};
    g_PixelVisStatsOn = (GetEnvironmentVariableA("BMVR_PIXELVIS_STATS", v, sizeof(v)) && v[0] == '1') || bmvr::g_PerfLog;
    if (bmvr::g_PerfLog)
    {
        InstallEngineWorldBatchStats();
        InstallBoneParticleProf(g_PerfLogClient);
    }
    Game::logMsg("PerfLog %s (VR panel)", bmvr::g_PerfLog ? "on" : "off");
}

static void InstallPixelVisStats(uintptr_t client)
{
    // Always hooked (one flag test per glow when idle) so bmvr_glow_trace
    // works in every session; the stats need PerfLog or BMVR_PIXELVIS_STATS.
    char v[8] = {};
    g_PixelVisStatsOn = (GetEnvironmentVariableA("BMVR_PIXELVIS_STATS", v, sizeof(v)) && v[0] == '1') || bmvr::g_PerfLog;
    auto* fn = reinterpret_cast<LPVOID>(client + Offsets::kPixelVisGetFractionRva);
    if (MH_CreateHook(fn, reinterpret_cast<LPVOID>(&PixelVisFractionStats),
            reinterpret_cast<LPVOID*>(&g_OrigPixelVisFraction)) == MH_OK
        && MH_EnableHook(fn) == MH_OK)
        Game::logMsg("pixelvis hook on (glow trace%s; GlowSmooth %.2f s, GlowHideMs %.0f)", g_PixelVisStatsOn ? " + stats" : "", bmvr::g_GlowSmooth, bmvr::g_GlowHideMs);
    else
        return;
    if (!g_PixelVisStatsOn)
        return;
    auto* qfn = reinterpret_cast<LPVOID>(client + Offsets::kPixelVisQueryForViewRva);
    if (MH_CreateHook(qfn, reinterpret_cast<LPVOID>(&PixelVisQueryForViewStats),
            reinterpret_cast<LPVOID*>(&g_OrigPixelVisQueryForView)) == MH_OK)
        MH_EnableHook(qfn);
}

static void InstallPixelVisPerEye()
{
    const uintptr_t client = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
    if (!client || g_ClientCurrentViewId)
        return;
    const uintptr_t target = client + Offsets::kClientCurrentViewIdRva;
    for (const uint32_t rva : Offsets::kPixelVisViewIdCallRvas)
    {
        const auto* call = reinterpret_cast<const unsigned char*>(client + rva);
        bool ok = false;
        BMVR_TRY
        {
            ok = call[0] == 0xE8
                && client + rva + 5 + *reinterpret_cast<const int32_t*>(call + 1) == target;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok)
        {
            Game::logMsg("Pixel visibility per eye: call site client+0x%X MISMATCH (not patched)", rva);
            Game::noteMissingOffset("Pixel visibility CurrentViewID call sites (per-eye glow fix)");
            return;
        }
    }
    // A query (or a glow's whole set of queries) not issued in the last frame
    // is deleted, and a new one starts at brightness 0 and fades in. That
    // cleanup also runs between the two eyes, so after a game frame without
    // rendering the right eye's queries were two frames old and deleted every
    // time: right-eye glows pulsed. Allow kPixelVisQueryGraceFrames instead of 1.
    for (const uint32_t rva : { Offsets::kPixelVisQueryAgeCmpRva, Offsets::kPixelVisSetAgeCmpRva })
    {
        const auto* cmp = reinterpret_cast<const unsigned char*>(client + rva);
        if (cmp[0] != 0x83 || cmp[1] != 0xF8 || cmp[2] != 0x01)
        {
            Game::logMsg("Pixel visibility per eye: age check client+0x%X MISMATCH (not patched)", rva);
            Game::noteMissingOffset("Pixel visibility query age checks (per-eye glow fix)");
            return;
        }
    }
    g_ClientCurrentViewId = reinterpret_cast<ClientCurrentViewIdFn>(target);
    g_PerfLogClient = client;
    g_PerfLogApplied = bmvr::g_PerfLog;
    InstallPixelVisStats(client);
    InstallParticleLevel(client);
    InstallSoundListenerHmd();
    InstallTranslucentPassHooks(client);
    InstallRenderLightShadowHook(client);
    InstallEngineWorldBatchStats();
    InstallBoneParticleProf(client);
    InstallFlareHooks(client);
    // Headless A/B aid: BMVR_PIXELVIS_NOGRACE=1 keeps the stock 1-frame age.
    char noGrace[8] = {};
    const bool grace = !(GetEnvironmentVariableA("BMVR_PIXELVIS_NOGRACE", noGrace, sizeof(noGrace)) && noGrace[0] == '1');
    for (const uint32_t rva : { Offsets::kPixelVisQueryAgeCmpRva, Offsets::kPixelVisSetAgeCmpRva })
    {
        if (!grace)
            break;
        auto* imm = reinterpret_cast<unsigned char*>(client + rva + 2);
        DWORD old = 0;
        if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old))
            continue;
        *imm = Offsets::kPixelVisQueryGraceFrames;
        VirtualProtect(imm, 1, old, &old);
    }
    for (const uint32_t rva : Offsets::kPixelVisViewIdCallRvas)
    {
        auto* rel = reinterpret_cast<int32_t*>(client + rva + 1);
        DWORD old = 0;
        if (!VirtualProtect(rel, 4, PAGE_EXECUTE_READWRITE, &old))
            continue;
        *rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&PixelVisCurrentViewId) - (client + rva + 5));
        VirtualProtect(rel, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    auto* qf = reinterpret_cast<unsigned char*>(client + Offsets::kPixelVisQueryFractionRva);
    static const unsigned char kQfPrologue[] = { 0x55, 0x8B, 0xEC, 0x51, 0x57, 0x8B, 0xF9, 0x83, 0x7F, 0x18, 0x00 };
    if (std::memcmp(qf, kQfPrologue, sizeof(kQfPrologue)) == 0
        && MH_CreateHook(qf, reinterpret_cast<LPVOID>(&PixelVisQueryFractionHold),
            reinterpret_cast<LPVOID*>(&g_OrigPixelVisQueryFraction)) == MH_OK
        && MH_EnableHook(qf) == MH_OK)
        Game::logMsg("Glow hold hook on (GlowHold %.2f s, cvar bmvr_glow_hold)", bmvr::g_GlowHold);
    else
        Game::noteMissingOffset("Pixel visibility query fraction (glow hold)");
    Game::logMsg("Pixel visibility per eye: %u call sites patched (%s), unused queries kept %d frames",
        static_cast<unsigned>(sizeof(Offsets::kPixelVisViewIdCallRvas) / sizeof(Offsets::kPixelVisViewIdCallRvas[0])),
        bmvr::g_PixelVisPerEye ? "on" : "off, bmvr_pixelvis_per_eye 1 turns it on",
        grace ? static_cast<int>(Offsets::kPixelVisQueryGraceFrames) : 1);
}

// Offset of addr inside server.dll (0 if outside/unknown). AimDebug and the
// shot-origin caller whitelist identify call sites by return address.
// dServerBmShootSource calls this on every player +0x474 call, so the module
// base is looked up once (GetModuleHandleA takes the loader lock; costly
// under Wine). server.dll stays loaded for the whole map, and is looked up
// again if it was not loaded yet.
static uint32_t ServerRva(uintptr_t addr)
{
    static uintptr_t s_base = 0;
    if (!s_base)
        s_base = reinterpret_cast<uintptr_t>(GetModuleHandleA("server.dll"));
    return s_base && addr > s_base ? static_cast<uint32_t>(addr - s_base) : 0;
}
#include "sigscanner.h"
#include "in_buttons.h"
#include "trace.h"
#include "texture.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <cstring>
#include <mutex>

void BmvrAuditNoteMaterial(const char* name);

namespace
{
    void** SehMaterialSystemVTable(IMaterialSystem* mat)
    {
        void** vt = nullptr;
        if (!mat)
            return nullptr;
        BMVR_TRY
        {
            vt = *reinterpret_cast<void***>(mat);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vt = nullptr;
        }
        return vt;
    }

    bool EngineInGame()
    {
        return Hooks::m_Game && Hooks::m_Game->m_EngineClient
            && Hooks::m_Game->m_EngineClient->IsInGame();
    }

    const char* PeekLevelName()
    {
        if (!Hooks::m_Game || !Hooks::m_Game->m_EngineClient)
            return "";
        IEngineClient* eng = Hooks::m_Game->m_EngineClient;
        const char* map = nullptr;
        BMVR_TRY
        {
            map = eng->GetLevelNameShort();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            map = nullptr;
        }
        if (map && map[0])
            return map;
        BMVR_TRY
        {
            map = eng->GetLevelName();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            map = nullptr;
        }
        return map ? map : "";
    }

    // Source viewrender.h: RENDERVIEW_DRAWVIEWMODEL=1, RENDERVIEW_DRAWHUD=2.
    constexpr int kRenderViewDrawHud = 0x2;

    bool TextureNameIsHudRt(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strstr(name, "_rt_gui") != nullptr
            || std::strstr(name, "_rt_Hud") != nullptr
            || std::strstr(name, "_rt_HUD") != nullptr;
    }
}

namespace
{
    constexpr uint16_t kLcSentinel = 0x200;
    constexpr int kLcMax = 512;
    constexpr int kLcStride = 0x1E0;
    constexpr int kLcNext = 0x1D0;
    constexpr int kLcGX = 0x1D4;
    constexpr int kLcFlags = 0x18C;
    uint16_t* g_LcHead = nullptr;
    uint8_t* g_LcBase = nullptr;
    CRITICAL_SECTION g_LcLock{};
    bool g_LcLockReady = false;

    int LcAbs(int v) { return v < 0 ? -v : v; }

    void* LightcacheFindNearestImpl(int x, int y, int z, int flags)
    {
        if (!g_LcHead || !g_LcBase)
            return nullptr;
        uint16_t idx = *g_LcHead;
        if (idx == kLcSentinel)
            return nullptr;

        uint8_t seen[(kLcMax + 7) / 8]{};
        auto marked = [&](unsigned i) -> bool {
            return (seen[i >> 3] & static_cast<uint8_t>(1u << (i & 7))) != 0;
        };
        auto mark = [&](unsigned i) {
            seen[i >> 3] = static_cast<uint8_t>(seen[i >> 3] | (1u << (i & 7)));
        };

        void* best = nullptr;
        int bestDist = 0x7FFFFFFF;
        uint16_t prev = kLcSentinel;
        for (int step = 0; step < kLcMax; ++step)
        {
            if (idx == kLcSentinel)
                break;
            if (idx >= static_cast<uint16_t>(kLcMax) || marked(idx))
            {
                if (prev < static_cast<uint16_t>(kLcMax))
                    *reinterpret_cast<uint16_t*>(g_LcBase + prev * kLcStride + kLcNext) = kLcSentinel;
                static int s_log;
                if (s_log < 8)
                {
                    Game::logMsg("Lightcache LRU cycle/bad next idx=%u; broke link", idx);
                    ++s_log;
                }
                break;
            }
            mark(idx);
            uint8_t* node = g_LcBase + idx * kLcStride;
            const int dx = LcAbs(*reinterpret_cast<int*>(node + kLcGX) - x);
            const int dy = LcAbs(*reinterpret_cast<int*>(node + kLcGX + 4) - y);
            const int dz = LcAbs(*reinterpret_cast<int*>(node + kLcGX + 8) - z);
            const int fl = *reinterpret_cast<int*>(node + kLcFlags);
            int dist = dx;
            if (dy > dist)
                dist = dy;
            if (dz > dist)
                dist = dz;
            const int pen = (flags == fl) ? 0 : 2;
            if (pen > dist)
                dist = pen;
            if (dist < bestDist)
            {
                bestDist = dist;
                best = node;
                if (dist <= 1)
                    break;
            }
            prev = idx;
            idx = *reinterpret_cast<uint16_t*>(node + kLcNext);
        }
        return best;
    }

    bool TryParseLightcacheFindNearest(void* addr)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(addr);
        if (!bytes || bytes[0] != 0x55 || bytes[6] != 0x0F || bytes[7] != 0xB7 || bytes[8] != 0x0D
            || bytes[0x39] != 0x81 || bytes[0x3A] != 0xC1)
            return false;
        g_LcHead = *reinterpret_cast<uint16_t* const*>(bytes + 9);
        g_LcBase = *reinterpret_cast<uint8_t* const*>(bytes + 0x3B);
        return g_LcHead && g_LcBase;
    }
}

Hooks::Hooks(Game* game)
{
    const MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
        Game::errorMsg("Failed to init MinHook");

    m_Game = game;
    m_VR = m_Game->m_VR;
    initSourceHooks();

    auto enableIfReady = [](auto& hk, const char* name) {
        if (hk.pTarget && hk.fOriginal)
        {
            if (hk.enableHook() == 0)
                Game::logMsg("Hook enabled: %s", name);
            else
                Game::logMsg("Hook enable failed: %s", name);
        }
        else
            Game::logMsg("Hook skipped: %s", name);
    };

    enableIfReady(hkGetRenderTarget, "GetRenderTarget");
    enableIfReady(hkPushRenderTargetAndViewport, "PushRT");
    enableIfReady(hkPopRenderTargetAndViewport, "PopRT");
    enableIfReady(hkDrawScreenSpaceRectangle, "DrawScreenSpaceRectangle");
    enableIfReady(hkCopyRenderTargetToTextureEx, "CopyRenderTargetToTextureEx");
    enableIfReady(hkViewport, "Viewport");
    enableIfReady(hkGetViewport, "GetViewport");
    enableIfReady(hkGetViewportQueued, "GetViewportQueued");
    enableIfReady(hkGetRenderTargetDimensions, "GetRenderTargetDimensions");
    enableIfReady(hkGetRenderTargetDimensionsBase, "GetRenderTargetDimensionsBase");
    enableIfReady(hkGetRenderTargetDimensionsQueued, "GetRenderTargetDimensionsQueued");
    enableIfReady(hkDepthRange, "DepthRange");
    enableIfReady(hkDepthRangeQueued, "DepthRangeQueued");
    enableIfReady(hkGetBackBufferDimensions, "GetBackBufferDimensions");
    enableIfReady(hkGetScreenSize, "GetScreenSize");
    enableIfReady(hkGetScreenAspectRatio, "GetScreenAspectRatio");
    enableIfReady(hkComputeViewMatrices, "ComputeViewMatrices");
    enableIfReady(hkNewRendererUpdatePerCamera, "NewRendererUpdatePerCamera");
    enableIfReady(hkClientUpdatePvsNotifiers, "ClientUpdatePvsNotifiers");
    enableIfReady(hkClientPvsNotifierCheck, "ClientPvsNotifierCheck");
    enableIfReady(hkOcclusionBeginView, "OcclusionTrackerBeginView");
    enableIfReady(hkOccluderSetView, "OccluderSetView");
    enableIfReady(hkOccluderIsOccluded, "OccluderIsOccluded");
    enableIfReady(hkDrawWorldLists, "DrawWorldLists");
    enableIfReady(hkCreateNamedRTEx, "CreateNamedRTEx");
    enableIfReady(hkEndRTAlloc, "EndRTAlloc");
    enableIfReady(hkDrawFilledRect, "DrawFilledRect");
    enableIfReady(hkDrawTexturedRect, "DrawTexturedRect");
    enableIfReady(hkPanelSetSize, "PanelSetSize");
    enableIfReady(hkAdjustEngineViewport, "AdjustEngineViewport");
    enableIfReady(hkDrawModelExecute, "DrawModelExecute");
    enableIfReady(hkLightcacheFindNearest, "LightcacheFindNearest");
    enableIfReady(hkVgui_Paint, "VGui_Paint");
    enableIfReady(hkRenderView, "RenderView");
    enableIfReady(hkCreateMove, "CreateMove");
    enableIfReady(hkLevelInit, "LevelInit");
    enableIfReady(hkLevelShutdown, "LevelShutdown");
    enableIfReady(hkCalcViewModelView, "CalcViewModelView");
    enableIfReady(hkGetViewModelFOV, "GetViewModelFOV");
    enableIfReady(hkGetCursorPos, "GetCursorPos");
    enableIfReady(hkSetCursorPos, "SetCursorPos");
    enableIfReady(hkEndFrame, "EndFrame");
    enableIfReady(hkTraceRay, "TraceRay");
}

Hooks::~Hooks()
{
    MH_Uninitialize();
}

int Hooks::initSourceHooks()
{
    auto& o = *m_Game->m_Offsets;

    if (o.GetRenderTarget.valid)
        hkGetRenderTarget.createHook((LPVOID)o.GetRenderTarget.address, &dGetRenderTarget);
    if (o.RenderView.valid)
        hkRenderView.createHook((LPVOID)o.RenderView.address, &dRenderView);
    // Portal 2 VR: CreateMove writes viewangles. Camera stays HMD on the
    // RenderView copies. Only mutate cmds on gameplay maps — a previous
    // trampoline on background01 coincided with a 17s crash after the first
    // menu Submit.
    if (o.CreateMove.valid)
        hkCreateMove.createHook((LPVOID)o.CreateMove.address, &dCreateMove);
    if (o.CalcViewModelView.valid)
        hkCalcViewModelView.createHook((LPVOID)o.CalcViewModelView.address, &dCalcViewModelView);
    if (o.GetViewModelFOV.valid)
        hkGetViewModelFOV.createHook((LPVOID)o.GetViewModelFOV.address, &dGetViewModelFOV);
    // VGUI reads the menu cursor back with GetCursorPos (vgui::Button fires on
    // release only if the cursor is still over it). Under Wine our
    // SetCursorPos only moves the pointer while the game window has focus,
    // so unfocused the menu hovered/clicked at the real desktop mouse.
    // Re-enabled 2026-10-17: the phantom clicks that got it switched off in
    // 4d77b7f came from Steam Input, not from this hook.
    if (HMODULE user32 = GetModuleHandleA("user32.dll"))
    {
        hkGetCursorPos.createHook((LPVOID)GetProcAddress(user32, "GetCursorPos"), &dGetCursorPos);
        // The engine's mouse look re-centres the cursor every frame. Under
        // Wine that warp lands while the pointer is over the game window even
        // when another app has focus: hovering the window traps the cursor.
        hkSetCursorPos.createHook((LPVOID)GetProcAddress(user32, "SetCursorPos"), &dSetCursorPos);
    }
    if (o.AdjustEngineViewport.valid)
        hkAdjustEngineViewport.createHook((LPVOID)o.AdjustEngineViewport.address, &dAdjustEngineViewport);
    if (o.Viewport.valid)
        hkViewport.createHook((LPVOID)o.Viewport.address, &dViewport);
    if (o.GetViewport.valid)
        hkGetViewport.createHook((LPVOID)o.GetViewport.address, &dGetViewport);
    if (o.GetViewportQueued.valid)
        hkGetViewportQueued.createHook((LPVOID)o.GetViewportQueued.address, &dGetViewport);
    if (o.GetRenderTargetDimensions.valid)
        hkGetRenderTargetDimensions.createHook((LPVOID)o.GetRenderTargetDimensions.address, &dGetRenderTargetDimensions);
    if (o.GetRenderTargetDimensionsBase.valid)
        hkGetRenderTargetDimensionsBase.createHook((LPVOID)o.GetRenderTargetDimensionsBase.address, &dGetRenderTargetDimensions);
    if (o.GetRenderTargetDimensionsQueued.valid)
        hkGetRenderTargetDimensionsQueued.createHook((LPVOID)o.GetRenderTargetDimensionsQueued.address, &dGetRenderTargetDimensions);
    if (o.PushRenderTargetAndViewport.valid)
        hkPushRenderTargetAndViewport.createHook((LPVOID)o.PushRenderTargetAndViewport.address, &dPushRenderTargetAndViewport);
    if (o.PopRenderTargetAndViewport.valid)
        hkPopRenderTargetAndViewport.createHook((LPVOID)o.PopRenderTargetAndViewport.address, &dPopRenderTargetAndViewport);
    if (o.DrawScreenSpaceRectangle.valid)
        hkDrawScreenSpaceRectangle.createHook((LPVOID)o.DrawScreenSpaceRectangle.address, &dDrawScreenSpaceRectangle);
    if (o.CopyRenderTargetToTextureEx.valid)
        hkCopyRenderTargetToTextureEx.createHook((LPVOID)o.CopyRenderTargetToTextureEx.address, &dCopyRenderTargetToTextureEx);
    if (o.GetBackBufferDimensions.valid && !hkGetBackBufferDimensions.pTarget)
        hkGetBackBufferDimensions.createHook((LPVOID)o.GetBackBufferDimensions.address, &dGetBackBufferDimensions);
    if (o.GetScreenSize.valid && !hkGetScreenSize.pTarget)
        hkGetScreenSize.createHook((LPVOID)o.GetScreenSize.address, &dGetScreenSize);
    if (o.GetScreenAspectRatio.valid && !hkGetScreenAspectRatio.pTarget)
        hkGetScreenAspectRatio.createHook((LPVOID)o.GetScreenAspectRatio.address, &dGetScreenAspectRatio);
    // Off-centre eyes only (OffCenterEyes, LeftEyeCullPad): the areaportal
    // pass must see the centred union view.
    if ((bmvr::g_OffCenterEyes || bmvr::g_LeftEyeCullPad > 0.f) && o.ComputeViewMatrices.valid && !hkComputeViewMatrices.pTarget
        && AreaPortalComputeViewCallSiteOk(o.ComputeViewMatrices.address))
        hkComputeViewMatrices.createHook((LPVOID)o.ComputeViewMatrices.address, &dComputeViewMatrices);
    if (o.NewRendererUpdatePerCamera.valid && !hkNewRendererUpdatePerCamera.pTarget)
        hkNewRendererUpdatePerCamera.createHook((LPVOID)o.NewRendererUpdatePerCamera.address, &dNewRendererUpdatePerCamera);
    if (o.ClientUpdatePvsNotifiers.valid && o.ClientPvsNotifierCheck.valid && !hkClientPvsNotifierCheck.pTarget)
    {
        hkClientUpdatePvsNotifiers.createHook((LPVOID)o.ClientUpdatePvsNotifiers.address, &dClientUpdatePvsNotifiers);
        hkClientPvsNotifierCheck.createHook((LPVOID)o.ClientPvsNotifierCheck.address, &dClientPvsNotifierCheck);
    }
    else if (!o.ClientUpdatePvsNotifiers.valid || !o.ClientPvsNotifierCheck.valid)
        Game::noteMissingOffset("Client PVS notifier check (MulticoreMode shadow flicker fix)");
    if (bmvr::g_OcclusionTrackerPerEye && o.OcclusionTrackerBeginView.valid && !hkOcclusionBeginView.pTarget)
        hkOcclusionBeginView.createHook((LPVOID)o.OcclusionTrackerBeginView.address, &dOcclusionBeginView);
    else if (bmvr::g_OcclusionTrackerPerEye && !o.OcclusionTrackerBeginView.valid)
        Game::noteMissingOffset("Occlusion tracker BeginView (per-eye occlusion culling)");
    InstallPixelVisPerEye();
    if (const uintptr_t client = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll")); client && !g_OrigSaveShotWriter)
    {
        // Prologue, then cmp byte ptr [kSaveShotNowRva], 0 at +0x13.
        const auto* fn = reinterpret_cast<const unsigned char*>(client + Offsets::kSaveShotWriterRva);
        const uintptr_t flag = client + Offsets::kSaveShotNowRva;
        if (fn[0] == 0x55 && fn[1] == 0x8B && fn[2] == 0xEC && fn[0x13] == 0x80 && fn[0x14] == 0x3D
            && *reinterpret_cast<const uint32_t*>(fn + 0x15) == static_cast<uint32_t>(flag)
            && MH_CreateHook(const_cast<unsigned char*>(fn), reinterpret_cast<LPVOID>(&SaveShotWriter),
                reinterpret_cast<LPVOID*>(&g_OrigSaveShotWriter)) == MH_OK
            && MH_EnableHook(const_cast<unsigned char*>(fn)) == MH_OK)
        {
            g_SaveShotNow = reinterpret_cast<volatile unsigned char*>(flag);
            Game::logMsg("Save thumbnail hook on (saves get the left eye)");
        }
        else
            Game::noteMissingOffset("Save thumbnail writer (save pictures)");
    }
    if ((bmvr::g_OccluderViewPerEye || bmvr::g_OffCenterEyes) && o.OccluderSetView.valid && !hkOccluderSetView.pTarget)
        hkOccluderSetView.createHook((LPVOID)o.OccluderSetView.address, &dOccluderSetView);
    else if ((bmvr::g_OccluderViewPerEye || bmvr::g_OffCenterEyes) && !o.OccluderSetView.valid)
        Game::noteMissingOffset("Occluder SetView (per-eye occluder culling)");
    if (o.DrawWorldLists.valid && !hkDrawWorldLists.pTarget)
        hkDrawWorldLists.createHook((LPVOID)o.DrawWorldLists.address, &dDrawWorldLists);
    else if (!o.DrawWorldLists.valid)
        Game::noteMissingOffset("DrawWorldLists (underwater world in the eyes)");
    if (o.OccluderIsOccluded.valid && !hkOccluderIsOccluded.pTarget)
        hkOccluderIsOccluded.createHook((LPVOID)o.OccluderIsOccluded.address, &dOccluderIsOccluded);
    if (o.CreateNamedRTEx.valid)
        hkCreateNamedRTEx.createHook((LPVOID)o.CreateNamedRTEx.address, &dCreateNamedRTEx);
    if (o.EndRTAlloc.valid)
        hkEndRTAlloc.createHook((LPVOID)o.EndRTAlloc.address, &dEndRTAlloc);
    // vguimatsurface.dll VGUI_Surface030: CMatSystemSurface vtable slot 15
    // DrawFilledRect / 37 DrawTexturedRect (both ret 0x10). File offsets
    // 0x3D880 / 0x40D20 in the Steam BM module.
    if (m_Game)
    {
        void* surf = m_Game->GetInterface("vguimatsurface.dll", "VGUI_Surface030");
        if (!surf)
            surf = m_Game->GetInterface("vguimatsurface.dll", "VGUI_Surface031");
        if (surf)
        {
            void** vt = nullptr;
            BMVR_TRY
            {
                vt = *reinterpret_cast<void***>(surf);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                vt = nullptr;
            }
            if (vt)
            {
                if (hkDrawFilledRect.createHook(vt[15], &dDrawFilledRect) != 0)
                    Game::logMsg("DrawFilledRect createHook failed");
                if (hkDrawTexturedRect.createHook(vt[37], &dDrawTexturedRect) != 0)
                    Game::logMsg("DrawTexturedRect createHook failed");
            }
        }
        else
            Game::logMsg("VGUI_Surface030/031 missing; pause dimmer skip unavailable");
        // vgui2.dll VGUI_Panel009 slot 4 SetSize (same table as the panel
        // calls in vr.cpp: 3 GetPos, 5 GetSize).
        void* ipanel = m_Game->GetInterface("vgui2.dll", "VGUI_Panel009");
        void** pvt = nullptr;
        BMVR_TRY
        {
            pvt = ipanel ? *reinterpret_cast<void***>(ipanel) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            pvt = nullptr;
        }
        if (!pvt)
            Game::noteMissingOffset("VGUI_Panel009 (client UI root at window size)");
        else if (hkPanelSetSize.createHook(pvt[4], &dPanelSetSize) != 0)
            Game::logMsg("Panel SetSize createHook failed");
    }
    if (o.VGui_Paint.valid && bmvr::TryVguiPaint())
        hkVgui_Paint.createHook((LPVOID)o.VGui_Paint.address, &dVGui_Paint);
    else if (o.VGui_Paint.valid)
        Game::logMsg("VGui_Paint createHook skipped (sticky vgui_paint)");
    // Real CModelRender::DrawModelExecute is engine.dll 0x113E80 (vtable +0x4C),
    // thiscall 3-arg, ret 0xC. 0xF6A20 is a displacement loader — do not hook it.
    if (o.DrawModelExecute.valid && bmvr::TryDrawModelExecute())
    {
        // Hold the sticky only around createHook. The old window lasted until
        // the first gameplay DME, so a later crash (ff_hmdfit) false-banned
        // yFix / scale / HideViewmodelArms.
        bmvr::BeginRisky(L"dme");
        if (hkDrawModelExecute.createHook((LPVOID)o.DrawModelExecute.address, &dDrawModelExecute) != 0)
        {
            bmvr::EndRisky(L"dme");
            Game::logMsg("DrawModelExecute createHook failed");
        }
        else
        {
            bmvr::EndRisky(L"dme");
            Game::logMsg("DrawModelExecute createHook CModelRender+0x4C rva=0x%X", o.DrawModelExecute.offset);
        }
    }
    else if (o.DrawModelExecute.valid)
        Game::logMsg("DrawModelExecute createHook skipped (sticky dme)");
    if (o.LightcacheFindNearest.valid)
    {
        if (TryParseLightcacheFindNearest(reinterpret_cast<void*>(o.LightcacheFindNearest.address)))
        {
            InitializeCriticalSection(&g_LcLock);
            g_LcLockReady = true;
            if (hkLightcacheFindNearest.createHook((LPVOID)o.LightcacheFindNearest.address, &dLightcacheFindNearest) != 0)
                Game::logMsg("LightcacheFindNearest createHook failed");
            else
                Game::logMsg("LightcacheFindNearest createHook rva=0x%X (LRU cycle guard)", o.LightcacheFindNearest.offset);
        }
        else
            Game::logMsg("LightcacheFindNearest skipped (could not parse LRU head/base)");
    }
    // LevelInit stays unhooked: MinHook on it crashed inside the original on
    // background01 (bring-up, 2026-08). Map names come from GetLevelNameShort.
    if (m_Game->MaterialVTableMatchesDump() && m_Game->m_MaterialSystem)
    {
        void** vt = SehMaterialSystemVTable(m_Game->m_MaterialSystem);
        if (vt && vt[37])
            hkEndFrame.createHook(vt[37], &dEndFrame);
        else
            Game::logMsg("EndFrame hook skipped (no vtbl[37])");
    }
    else
        Game::logMsg("EndFrame hook skipped (IMaterialSystem dump slot 37 unverified on BM)");

    // IEngineTrace::TraceRay is not hooked. dTraceRay has been a pure
    // pass-through since the melee rewrite moved to UpdateCrowbarMelee
    // (47777b5); the detour only added a trampoline to every client trace
    // (AI, physics, bullets — thousands per frame). Melee traces call
    // m_EngineTrace->TraceRay directly and never needed hkTraceRay.fOriginal.
    (void)&dTraceRay;

    EnsureClientFlashlightHook();
    EnsureFogColorPatch();
    EnsureWeaponShootOriginHooks();

    return 1;
}

namespace
{
    thread_local IMatRenderContext* g_MatCtx = nullptr;
    thread_local ITexture* g_StereoRedirect = nullptr;
    thread_local int g_VguiOverlayReentry = 0;
    thread_local int g_RenderViewNest = 0;
    thread_local int g_GluonFx = 0;
    DWORD g_GluonFxUntilMs = 0;
    // Size of the reflection/refraction RTs grown by ReflectionRtScale
    // (bmvr::g_ReflectionRtW/H, 0 = not grown). Reflection views use it as
    // their viewport; it must not be mistaken for a world pass and expanded
    // to the eye.
    // Exact size only: a +-16 px tolerance also matched the eye/2 bloom
    // brightpass target (2008x2048 next to 2000x2048, 2026-10-30), and the
    // grow below never picks eye/2 or eye/4.
    bool ReflectionRtSized(int w, int h)
    {
        return bmvr::GrownAuxRtSized(w, h);
    }
    thread_local Vector g_CurViewOrigin{};
    thread_local Vector g_CurViewForward{};
    thread_local bool g_HaveCurView = false;
    constexpr int kRtStackMax = 32;
    struct RtStackEntry
    {
        char name[64];
        int w;
        int h;
        bool aux;
        // RtStackTopIsWorldScene() result, fixed at push time. It used to
        // re-run ~15 strstr on the stack-top name from every GetScreenSize /
        // GetBackBufferDimensions call.
        bool world;
    };
    bool IsOffscreenWorldRtName(const char* name);
    thread_local RtStackEntry g_RtStack[kRtStackMax]{};
    thread_local int g_RtStackDepth = 0;
    // Pushes past kRtStackMax used to return without a slot, then NotePopRt
    // still popped a real entry. Aux/world class then tracked the wrong RT
    // for the rest of the frame.
    thread_local int g_RtStackOverflow = 0;
    thread_local int g_AuxRtDepth = 0;
    bool g_CostActive = false;
    int g_ShadowPush = 0;
    int g_GbDepthPush = 0;
    int g_GbLightPush = 0;
    int g_LsMaskPush = 0;

    bool NestedRenderView()
    {
        return g_RenderViewNest > 1;
    }

    bool TextureNameIsAuxSceneRt(const char* name)
    {
        if (!name || !name[0] || name[0] == '?')
            return false;
        if (bmvr::EyeScaledRtClass(name) == bmvr::EyeRtClass::Scene)
            return false;
        if (std::strstr(name, "backbuffer"))
            return false;
        if (std::strstr(name, "_rt_FullFrameFB") || std::strstr(name, "_rt_ResolvedFullFrame"))
            return false;
        // Gameplay G-buffers are the stereo scene. `_rt_gbXog` contains "xog"
        // and was treated as aux, so every later PushRT/Viewport/GetScreenSize
        // stayed 2560x1440 on 2544x2480 RTs (warp + bottom garbage, 2026-08-26).
        if (std::strstr(name, "_rt_gb")
            && !std::strstr(name, "shadow") && !std::strstr(name, "Shadow")
            && !std::strstr(name, "flashlight") && !std::strstr(name, "Flashlight")
            && !std::strstr(name, "csm") && !std::strstr(name, "CSM"))
            return false;
        if (std::strstr(name, "Water") || std::strstr(name, "water"))
            return true;
        if (std::strstr(name, "Reflect") || std::strstr(name, "Refract"))
            return true;
        if (std::strstr(name, "PowerOfTwo") || std::strstr(name, "_rt_Camera"))
            return true;
        if (std::strstr(name, "SmallFB") || std::strstr(name, "Small2FB") || std::strstr(name, "Small8FB")
            || std::strstr(name, "Small16FB") || std::strstr(name, "Small32FB"))
            return true;
        if (std::strstr(name, "shadow") || std::strstr(name, "Shadow") || std::strstr(name, "CSM")
            || std::strstr(name, "csm") || std::strstr(name, "flashlight") || std::strstr(name, "Flashlight"))
            return true;
        if (std::strstr(name, "Dof") || std::strstr(name, "xbow")
            || std::strstr(name, "gbShadow"))
            return true;
        return false;
    }

    bool AuxSceneRtBound()
    {
        return g_AuxRtDepth > 0;
    }

    // Name-derived RT classes, memoized per name pointer. ITexture::GetName
    // returns the texture's own name buffer, so the pointer is stable for the
    // texture's lifetime; the stored copy is compared too so a recycled
    // allocation after a level change cannot return a stale class. Without
    // this every PushRenderTargetAndViewport (dozens per eye) ran ~50 strstr.
    struct RtNameClass
    {
        bool aux = false;
        bool world = false;
        bool hud = false;
    };

    RtNameClass ClassifyRtName(const char* name)
    {
        RtNameClass out{};
        if (!name || !name[0] || name[0] == '?')
            return out;
        struct Entry { const char* ptr; char name[64]; RtNameClass cls; };
        thread_local Entry cache[128]{};
        const uintptr_t p = reinterpret_cast<uintptr_t>(name);
        Entry& e = cache[static_cast<unsigned>((p >> 3) ^ (p >> 11)) & 127u];
        if (e.ptr == name && std::strncmp(e.name, name, sizeof(e.name) - 1) == 0)
            return e.cls;
        out.aux = TextureNameIsAuxSceneRt(name);
        out.world = IsOffscreenWorldRtName(name);
        out.hud = TextureNameIsHudRt(name);
        e.ptr = name;
        strncpy_s(e.name, name, _TRUNCATE);
        e.cls = out;
        return out;
    }

    void NotePushRt(const char* name, int w, int h, const RtNameClass* cls = nullptr)
    {
        if (g_RtStackDepth >= kRtStackMax)
        {
            ++g_RtStackOverflow;
            static int s_overflowLog;
            if (s_overflowLog < 3)
            {
                Game::logMsg("RT stack overflow at %d (nested push not tracked)", kRtStackMax);
                ++s_overflowLog;
            }
            return;
        }
        RtStackEntry& e = g_RtStack[g_RtStackDepth++];
        e.w = w;
        e.h = h;
        e.name[0] = 0;
        const bool named = name && name[0] && name[0] != '?';
        if (named)
            strncpy_s(e.name, name, _TRUNCATE);
        else
            strncpy_s(e.name, "backbuffer", _TRUNCATE);
        const bool smallVp = (w > 0 && h > 0 && (w < 640 || h < 360));
        RtNameClass local{};
        if (named && !cls)
        {
            local = ClassifyRtName(name);
            cls = &local;
        }
        e.aux = smallVp || (named && cls->aux);
        // "backbuffer"/"null" count as the world scene (RtStackTopIsWorldScene).
        e.world = !named || (std::strcmp(e.name, "null") == 0) || cls->world;
        if (e.aux)
            ++g_AuxRtDepth;
        if (g_CostActive && name)
        {
            if (std::strstr(name, "shadow") || std::strstr(name, "Shadow")
                || std::strstr(name, "CSM") || std::strstr(name, "csm"))
                ++g_ShadowPush;
            if (std::strstr(name, "gbdepth") || std::strstr(name, "gbDepth") || std::strstr(name, "GBDepth"))
                ++g_GbDepthPush;
            if (std::strstr(name, "gblight") || std::strstr(name, "gbLight") || std::strstr(name, "GBLight"))
                ++g_GbLightPush;
            if (std::strstr(name, "lsmask") || std::strstr(name, "lsMask") || std::strstr(name, "LSMask"))
                ++g_LsMaskPush;
        }
    }

    void NotePopRt()
    {
        if (g_RtStackOverflow > 0)
        {
            --g_RtStackOverflow;
            return;
        }
        if (g_RtStackDepth <= 0)
            return;
        const RtStackEntry e = g_RtStack[--g_RtStackDepth];
        if (e.aux && g_AuxRtDepth > 0)
            --g_AuxRtDepth;
    }

    struct RenderViewNestScope
    {
        RenderViewNestScope() { ++g_RenderViewNest; }
        ~RenderViewNestScope() { --g_RenderViewNest; }
    };

    struct CurrentViewScope
    {
        Vector oldOrigin{};
        Vector oldForward{};
        bool oldHave = false;
        CurrentViewScope(const CViewSetup& setup)
        {
            oldOrigin = g_CurViewOrigin;
            oldForward = g_CurViewForward;
            oldHave = g_HaveCurView;
            g_CurViewOrigin = setup.origin;
            const QAngle ang(setup.angles.x, setup.angles.y, setup.angles.z);
            QAngle::AngleVectors(ang, &g_CurViewForward, nullptr, nullptr);
            g_HaveCurView = VectorNormalize(g_CurViewForward) > 0.01f;
        }
        ~CurrentViewScope()
        {
            g_CurViewOrigin = oldOrigin;
            g_CurViewForward = oldForward;
            g_HaveCurView = oldHave;
        }
    };

    void TryHookMatContextQueries(void* ctx);

    void* SehContextVtableSlot(void* ctx, int byteOffset)
    {
        void* fn = nullptr;
        if (!ctx || byteOffset < 0)
            return nullptr;
        BMVR_TRY
        {
            void** vt = *reinterpret_cast<void***>(ctx);
            if (vt)
                fn = vt[byteOffset / 4];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            fn = nullptr;
        }
        return fn;
    }

    void NoteMatContext(void* ecx)
    {
        if (!ecx)
            return;
        g_MatCtx = reinterpret_cast<IMatRenderContext*>(ecx);
        TryHookMatContextQueries(ecx);
    }

    const char* SafeTextureName(ITexture* texture)
    {
        if (!texture)
            return "null";
        const char* name = "?";
        BMVR_TRY
        {
            name = texture->GetName();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name = "?";
        }
        return name ? name : "?";
    }

    const char* SafeModelName(IModelInfo* info, void* model)
    {
        if (!info || !model)
            return nullptr;
        const char* name = nullptr;
        BMVR_TRY
        {
            name = info->GetModelName(model);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name = nullptr;
        }
        return name;
    }

    int SafeLocalPlayerIndex()
    {
        if (!Hooks::m_Game || !Hooks::m_Game->m_EngineClient)
            return 0;
        int local = 0;
        BMVR_TRY
        {
            local = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            local = 0;
        }
        return local;
    }

    bool ModelNameIsViewmodel(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strstr(name, "/v_") != nullptr
            || std::strstr(name, "\\v_") != nullptr
            || std::strstr(name, "/V_") != nullptr
            || std::strstr(name, "\\V_") != nullptr;
    }

    bool CachedModelIsViewmodel(void* pModel)
    {
        struct Entry { void* model; bool isVm; bool filled; };
        static Entry cache[256]{};
        if (!pModel)
            return false;
        const unsigned i = (static_cast<unsigned>(reinterpret_cast<uintptr_t>(pModel) >> 4)) & 255u;
        if (cache[i].filled && cache[i].model == pModel)
            return cache[i].isVm;
        const char* name = nullptr;
        if (Hooks::m_Game && Hooks::m_Game->m_ModelInfo)
            name = SafeModelName(Hooks::m_Game->m_ModelInfo, pModel);
        const bool vm = ModelNameIsViewmodel(name);
        cache[i] = { pModel, vm, true };
        return vm;
    }

    int g_CachedLocalPlayer = 0;
    bool g_HaveCachedLocalPlayer = false;

    int CachedLocalPlayerIndex()
    {
        if (g_HaveCachedLocalPlayer)
            return g_CachedLocalPlayer;
        g_CachedLocalPlayer = SafeLocalPlayerIndex();
        g_HaveCachedLocalPlayer = true;
        return g_CachedLocalPlayer;
    }

    long long QpcNow()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    double QpcMs(long long ticks)
    {
        static double s_ms = 0.0;
        if (s_ms == 0.0)
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            s_ms = 1000.0 / static_cast<double>(f.QuadPart);
        }
        return static_cast<double>(ticks) * s_ms;
    }

    struct StereoCost
    {
        long long leftTicks = 0;
        long long rightTicks = 0;
        long long blitTicks = 0;
        long long handsTicks = 0;
        long long dmeOrigTicks = 0;
        long long dmeHookTicks = 0;
        int dme = 0;
        int dmeL = 0;
        int dmeR = 0;
        int dmeEnt = 0;
        int dmeVm = 0;
        int dmeShadow = 0;
        int dmeBones32 = 0;
        int uniqEnt = 0;
        uint32_t seenEnt[128]{};
        void* lastCharModel = nullptr;
        int eye = 0;
        bool active = false;
    };
    StereoCost g_Cost{};

    void CostMarkEnt(int idx)
    {
        if (idx <= 1 || idx >= 4096)
            return;
        const unsigned w = static_cast<unsigned>(idx) >> 5;
        const unsigned b = static_cast<unsigned>(idx) & 31u;
        const uint32_t bit = 1u << b;
        if (g_Cost.seenEnt[w] & bit)
            return;
        g_Cost.seenEnt[w] |= bit;
        ++g_Cost.uniqEnt;
    }

    void StereoCostBegin()
    {
        g_Cost = StereoCost{};
        g_Cost.active = true;
        g_CostActive = true;
        g_ShadowPush = 0;
        g_GbDepthPush = 0;
        g_GbLightPush = 0;
        g_LsMaskPush = 0;
        g_HaveCachedLocalPlayer = false;
    }

    void StereoCostLog()
    {
        if (!g_Cost.active)
            return;
        g_Cost.active = false;
        g_CostActive = false;
        const double leftMs = QpcMs(g_Cost.leftTicks);
        const double rightMs = QpcMs(g_Cost.rightTicks);
        const double pairMs = leftMs + rightMs;
        const double blitMs = QpcMs(g_Cost.blitTicks);
        const double handsMs = QpcMs(g_Cost.handsTicks);
        const double origMs = QpcMs(g_Cost.dmeOrigTicks);
        const double hookMs = QpcMs(g_Cost.dmeHookTicks);
        static DWORD s_lastLogMs;
        const DWORD now = GetTickCount();
        const bool slow = pairMs >= 20.0;
        if (!slow && (now - s_lastLogMs) < 1000)
            return;
        if (slow && (now - s_lastLogMs) < 200)
            return;
        s_lastLogMs = now;
        const char* charName = "?";
        if (g_Cost.lastCharModel && Hooks::m_Game && Hooks::m_Game->m_ModelInfo)
        {
            const char* n = SafeModelName(Hooks::m_Game->m_ModelInfo, g_Cost.lastCharModel);
            if (n && n[0])
                charName = n;
        }
        Game::logMsg(
            "Stereo cost pair=%.1fms L=%.1f R=%.1f blit=%.1f hands=%.1f dme=%d (L=%d R=%d) orig=%.1fms hook=%.1fms ents=%d uniq=%d bones32+=%d shadowDme=%d shadowPush=%d vm=%d gbDepth=%d gbLight=%d lsMask=%d char=%s",
            pairMs, leftMs, rightMs, blitMs, handsMs,
            g_Cost.dme, g_Cost.dmeL, g_Cost.dmeR, origMs, hookMs,
            g_Cost.dmeEnt, g_Cost.uniqEnt, g_Cost.dmeBones32,
            g_Cost.dmeShadow, g_ShadowPush, g_Cost.dmeVm,
            g_GbDepthPush, g_GbLightPush, g_LsMaskPush, charName);
    }

    // Source DrawModelState_t (x86). CModelRender::DrawModelExecute
    // (engine 0x113E80) reads studiohdr flags at state[0]+0x98 and
    // drawFlags at state+0x14 — first pointer is studiohdr_t*, not CStudioHdr.
    struct DrawModelStateLite
    {
        unsigned char* studioHdr;
        void* studioHWData;
        void* renderable;
        const void* modelToWorld;
        int decals;
        int drawFlags;
        int lod;
    };

    constexpr int kStudioIdst = 0x54534449; // 'IDST'
    constexpr int kStudioHdrLength = 76;
    constexpr int kMaxStudioBones = 256;
    constexpr int kStudioHdrNumBones = 156;
    constexpr int kStudioHdrBoneIndex = 160;
    constexpr int kStudioHdrNumBodyparts = 232;
    constexpr int kStudioHdrBodypartIndex = 236;
    constexpr int kStudioBoneSize = 216;
    constexpr int kStudioBodypartSize = 16;
    constexpr int kStudioModelSize = 148;
    constexpr int kStudioModelNumMeshes = 72;

    struct StudioHdrPatch
    {
        int* p = nullptr;
        int original = 0;
        int patched = 0;           // what the mod wrote there
        unsigned char* hdr = nullptr;
        int checksum = 0;          // studiohdr_t checksum (+8) when patched
        char name[64]{};           // studiohdr_t name (+12) when patched
    };
    constexpr int kMaxStudioHdrPatches = 96;
    StudioHdrPatch g_ArmHdrPatches[kMaxStudioHdrPatches]{};
    int g_ArmHdrPatchCount = 0;
    constexpr int kVmDrawSlots = 4;
    thread_local int g_VmDrawSlot = 0;
    thread_local float g_ScaledViewmodelBones[kVmDrawSlots][kMaxStudioBones][3][4];
    thread_local float g_ScaledViewmodelModelToWorld[kVmDrawSlots][3][4];
    thread_local unsigned char g_ScaledViewmodelInfo[kVmDrawSlots][sizeof(ModelRenderInfo_t)];

    // World position of the viewmodel's muzzle attachment from bone matrices
    // (the ones handed to DrawModelExecute). Same names as VR::TryGetRawMuzzleWorld.
    bool DrawnAttachmentPoint(unsigned char* hdr, const float* bones, int nBones, Vector& out)
    {
        static const char* kNames[] = { "muzzle", "Fire01", "Fire02", "bolt_spawn" };
        if (!hdr || !bones)
            return false;
        BMVR_TRY
        {
            const int n = *reinterpret_cast<const int*>(hdr + 0xF0);           // numlocalattachments
            const int idx = *reinterpret_cast<const int*>(hdr + 0xF4);         // localattachmentindex
            for (const char* want : kNames)
            {
                for (int i = 0; i < n && i < 64; ++i)
                {
                    const unsigned char* att = hdr + idx + i * 0x5C;           // mstudioattachment_t
                    const char* name = reinterpret_cast<const char*>(att + *reinterpret_cast<const int*>(att));
                    if (_stricmp(name, want) != 0)
                        continue;
                    const int bone = *reinterpret_cast<const int*>(att + 8);
                    if (bone < 0 || bone >= nBones)
                        return false;
                    const float* m = bones + bone * 12;
                    const float* l = reinterpret_cast<const float*>(att + 12);
                    out.x = m[0] * l[3] + m[1] * l[7] + m[2] * l[11] + m[3];
                    out.y = m[4] * l[3] + m[5] * l[7] + m[6] * l[11] + m[7];
                    out.z = m[8] * l[3] + m[9] * l[7] + m[10] * l[11] + m[11];
                    return true;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return false;
    }
    // DT_LocalPlayerExclusive RecvTable (Ghidra FUN_100b7240): m_vecVelocity[0]
    // at +0xF8, not the L4D2 C_BasePlayer +0x100 (that is only .z here).
    constexpr int kLocalPlayerVecVelocity = 0xF8;
    // DT_BaseAnimating / DT_ServerAnimationData (Ghidra FUN_10090ac0 / FUN_10090f90).
    constexpr int kViewmodelSequence = 0x960;
    constexpr int kViewmodelCycle = 0x968;
    constexpr int kViewmodelPlaybackRate = 0x6E8;
    constexpr int kStudioHdrNumLocalSeq = 188;
    constexpr int kStudioHdrLocalSeqIndex = 192;
    constexpr int kStudioSeqDescSize = 212;
    constexpr int kStudioSeqLabelIndex = 4;
    constexpr int kStudioSeqFlags = 12;      // mstudioseqdesc_t::flags
    constexpr int kStudioSeqFlagDelta = 0x4; // STUDIO_DELTA: additive layer, no pose of its own

    unsigned char* g_LastViewmodelStudioHdr = nullptr;
    int g_LastViewmodelIdleSeq = -1;
    const char* StudioSequenceLabel(unsigned char* hdr, int seq);

    bool SequenceLabelLooksLikeWeaponAction(const char* label)
    {
        if (!label || !label[0])
            return false;
        char buf[96]{};
        for (int i = 0; i < 95 && label[i]; ++i)
            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
        auto has = [&](const char* s) { return std::strstr(buf, s) != nullptr; };
        if (has("reload") || has("fire") || has("shoot") || has("attack")
            || has("draw") || has("holster") || has("deploy") || has("pump")
            || has("bolt") || has("eject") || has("inspect") || has("admire")
            || has("pickup") || has("hit") || has("miss") || has("primary")
            || has("secondary") || has("ironsight") || has("_is"))
            return true;
        return false;
    }

    // Only explicit movement/idle names. Unknown/null labels must NOT suppress —
    // that froze fire/reload/draw (equip delayed several seconds).
    bool SequenceLabelLooksLikeLocomotionOnly(const char* label)
    {
        if (!label || !label[0])
            return false;
        char buf[96]{};
        for (int i = 0; i < 95 && label[i]; ++i)
            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
        auto has = [&](const char* s) { return std::strstr(buf, s) != nullptr; };
        // Keep draw/fire/reload even if the name also contains "idle" (rare).
        if (SequenceLabelLooksLikeWeaponAction(label))
            return false;
        if (has("sprint") || has("swim") || has("walk") || has("run") || has("bob"))
            return true;
        // Plain idle / fidget only — not idletosprint transitions that may share
        // bones with equip; freeze cycle, do not rewrite sequence index.
        if (has("fidget"))
            return true;
        if (has("idle") && !has("to") && !has("from"))
            return true;
        return false;
    }

    bool SequenceLabelLooksLikeEquipOnly(const char* label)
    {
        if (!label || !label[0])
            return false;
        char buf[96]{};
        for (int i = 0; i < 95 && label[i]; ++i)
            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
        auto has = [&](const char* s) { return std::strstr(buf, s) != nullptr; };
        return has("draw") || has("holster") || has("deploy") || has("pickup") || has("admire");
    }

    unsigned char* g_LastViewmodelIdleHdr = nullptr;

    int FindIdleSequence(unsigned char* hdr)
    {
        if (!hdr)
            return -1;
        // Per studiohdr result. The scan lowercases and strstr-classifies
        // every sequence label (up to 64) and ran twice per CalcViewModelView
        // while the crowbar was held. Reset on viewmodel change in
        // ApplyViewmodelStudioWork.
        if (hdr == g_LastViewmodelIdleHdr)
            return g_LastViewmodelIdleSeq;
        int numSeq = 0;
        BMVR_TRY
        {
            numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        if (numSeq <= 0 || numSeq > 64)
            return -1;
        int best = -1;
        int bestScore = -1;
        for (int i = 0; i < numSeq; ++i)
        {
            const char* label = StudioSequenceLabel(hdr, i);
            if (!label || !label[0])
                continue;
            char buf[96]{};
            for (int n = 0; n < 95 && label[n]; ++n)
                buf[n] = static_cast<char>(tolower(static_cast<unsigned char>(label[n])));
            auto has = [&](const char* s) { return std::strstr(buf, s) != nullptr; };
            if (has("draw") || has("holster") || has("deploy") || has("reload")
                || has("fire") || has("shoot") || has("attack") || has("hit")
                || has("miss") || has("sprint") || has("low") || has("to") || has("from"))
                continue;
            int score = 0;
            if (std::strcmp(buf, "idle1") == 0)
                score = 4;
            else if (std::strcmp(buf, "idle") == 0)
                score = 3;
            else if (has("idle") && !has("fidget"))
                score = 2;
            else if (has("idle"))
                score = 1;
            if (score > bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        // Seq before hdr: a reader that sees the matching hdr then also sees
        // the seq that belongs to it (main thread vs render-thread reset).
        g_LastViewmodelIdleSeq = best;
        g_LastViewmodelIdleHdr = hdr;
        return best;
    }

    int FindSequenceByLabel(unsigned char* hdr, const char* name)
    {
        if (!hdr || !name)
            return -1;
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local const char* s_name = nullptr;
        static thread_local int s_seq = -1;
        if (hdr == s_hdr && name == s_name)
            return s_seq;
        int numSeq = 0;
        BMVR_TRY { numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        int found = -1;
        for (int i = 0; i < numSeq && i < 128; ++i)
        {
            const char* label = StudioSequenceLabel(hdr, i);
            if (label && _stricmp(label, name) == 0)
            {
                found = i;
                break;
            }
        }
        s_hdr = hdr;
        s_name = name;
        s_seq = found;
        if (found < 0)
            Game::logMsg("Mag reload: sequence '%s' not in this viewmodel", name);
        return found;
    }

    const char* StudioRelString(const unsigned char* base, int rel);
    bool StudioHdrMatchesModel(unsigned char* hdr, const char* model);

    // Base and model count of a body part ("rocket", "arms"...); false if absent.
    bool FindBodypartBase(unsigned char* hdr, const char* name, int& base, int& models)
    {
        if (!hdr)
            return false;
        int numbody = 0, bodyindex = 0;
        BMVR_TRY
        {
            numbody = *reinterpret_cast<int*>(hdr + kStudioHdrNumBodyparts);
            bodyindex = *reinterpret_cast<int*>(hdr + kStudioHdrBodypartIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        for (int bi = 0; bi < numbody && bi < 16; ++bi)
        {
            unsigned char* bp = hdr + bodyindex + bi * kStudioBodypartSize;
            int szname = 0, nummodels = 0, b = 0;
            BMVR_TRY
            {
                szname = *reinterpret_cast<int*>(bp);
                nummodels = *reinterpret_cast<int*>(bp + 4);
                b = *reinterpret_cast<int*>(bp + 8);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (_stricmp(StudioRelString(bp, szname), name) == 0)
            {
                base = b;
                models = nummodels;
                return b > 0 && nummodels > 0;
            }
        }
        return false;
    }

    // RPG rocket: Black Mesa shows the rocket in the tube only through client
    // animation events (reload shows it at 0.24, the first-pickup clip at
    // 0.73, firing hides it). Anything that resets or cuts those clips leaves
    // a loaded RPG with an empty tube until the next fire + reload: a save
    // load (headless: body 7 -> 0), and switching while a clip runs (the VR
    // weapon menu switches on hover). The weapon's m_bNeedReload says what is
    // true: 1 from the shot until the reload starts (rocket in flight), else
    // a rocket is loaded. Rocket off + not needing a reload + no fire/reload
    // clip + rockets in reserve = show it, as the reload would have.
    void FixRpgRocketShown(void* viewmodel, unsigned char* hdr, const char* label)
    {
        if (!Hooks::m_Game)
            return;
        if (!StudioHdrMatchesModel(hdr, "models/weapons/v_rpg.mdl")
            || !StudioHdrMatchesModel(hdr, Hooks::m_Game->GetEntityModelName(static_cast<C_BaseEntity*>(viewmodel))))
            return;
        int base = 0, models = 0;
        if (!FindBodypartBase(hdr, "rocket", base, models) || models < 2)
            return;
        const int body = Hooks::m_Game->ReadEntityBody(viewmodel);
        if (body < 0)
            return;
        const int rocket = (body / base) % models;
        // Manual RPG reload: the rocket bone shows/hides/moves the rocket
        // (tube, hand, gone after a shot), so the bodygroup stays on.
        if (MagReload::g_Weapon.load(std::memory_order_relaxed) == MagReload::kWeaponRpg)
        {
            if (rocket == 0)
                Hooks::m_Game->WriteEntityBody(viewmodel, body + base);
            return;
        }
        C_BaseEntity* weapon = Hooks::m_Game->GetActiveWeaponEntity();
        int need = -1, inRel = -1;
        Hooks::m_Game->ReadWeaponReloadFlags(weapon, need, inRel);
        char lb[32]{};
        for (int i = 0; label && i < 31 && label[i]; ++i)
            lb[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
        const bool clip = std::strstr(lb, "fire") || std::strstr(lb, "reload");
        {
            static int s_lastKey = -999, s_logs = 0;
            const int key = need * 100 + (clip ? 10 : 0) + rocket;
            if (key != s_lastKey && s_logs < 100)
            {
                ++s_logs;
                s_lastKey = key;
                Game::logMsg("RPG state: needReload=%d rocket=%d seq '%s'", need, rocket, label ? label : "?");
            }
        }
        static int s_offFrames = 0;
        if (rocket != 0 || need != 0 || clip)
        {
            s_offFrames = 0;
            return;
        }
        // A few frames, so a fire clip arriving right after the event is seen first.
        if (++s_offFrames < 5)
            return;
        s_offFrames = 0;
        if (Hooks::m_Game->WeaponHasNoAmmo(weapon))
            return;
        const int fixed = body + base;
        Hooks::m_Game->WriteEntityBody(viewmodel, fixed);
        static int s_fixLogs = 0;
        if (s_fixLogs++ < 50)
            Game::logMsg("RPG rocket: loaded but not shown (seq '%s'), body %d -> %d", label ? label : "?", body, fixed);
    }

    void ForceViewmodelIdlePose(void* viewmodel, unsigned char* hdr)
    {
        if (!viewmodel)
            return;
        const int idle = FindIdleSequence(hdr);
        BMVR_TRY
        {
            if (idle >= 0)
                *reinterpret_cast<int*>(static_cast<char*>(viewmodel) + kViewmodelSequence) = idle;
            *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelCycle) = 0.f;
            *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate) = 0.f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        static int s_idleLog;
        if (s_idleLog < 8)
        {
            const char* label = idle >= 0 ? StudioSequenceLabel(hdr, idle) : "?";
            Game::logMsg("Viewmodel force idle seq=%d '%s'", idle, label ? label : "?");
            ++s_idleLog;
        }
    }

    // Holds the named sequence at cycle 0 (ForceViewmodelIdlePose with a
    // chosen clip); falls back to the idle when the model has no such clip.
    void ForceViewmodelSequencePose(void* viewmodel, unsigned char* hdr, const char* seqName)
    {
        if (!viewmodel)
            return;
        static unsigned char* s_hdr = nullptr;
        static char s_name[32]{};
        static int s_seq = -1;
        // Re-found when the header changes, or when the cached index is not
        // that clip in this header any more (a freed header's address reused
        // by another model).
        const char* cachedLabel = s_seq >= 0 ? StudioSequenceLabel(hdr, s_seq) : nullptr;
        if (s_hdr != hdr || std::strcmp(s_name, seqName) != 0 || !cachedLabel || _stricmp(cachedLabel, seqName) != 0)
        {
            s_hdr = hdr;
            strncpy_s(s_name, seqName, _TRUNCATE);
            s_seq = -1;
            int numSeq = 0;
            BMVR_TRY { numSeq = hdr ? *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq) : 0; }
            __except (EXCEPTION_EXECUTE_HANDLER) { numSeq = 0; }
            for (int i = 0; i < numSeq && i < 256; ++i)
            {
                const char* l = StudioSequenceLabel(hdr, i);
                if (l && _stricmp(l, seqName) == 0)
                {
                    s_seq = i;
                    break;
                }
            }
        }
        if (s_seq < 0)
        {
            ForceViewmodelIdlePose(viewmodel, hdr);
            return;
        }
        BMVR_TRY
        {
            *reinterpret_cast<int*>(static_cast<char*>(viewmodel) + kViewmodelSequence) = s_seq;
            *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelCycle) = 0.f;
            *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate) = 0.f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Crowbar / MP5 viewmodel tests moved to VR::ClassifyViewmodel
    // (kVmFlagCrowbar / kVmFlagMp5); callers read the atomic flags.

    void ZeroPlayerViewRecoil(void* player)
    {
        if (!player)
            return;
        // DT_LocalPlayerExclusive m_Local +0x103C. DT_Local m_vecPunchAngle +0x74,
        // m_vecPunchAngleVel +0xB0 (Ghidra FUN_100b7240 / FUN_100b6d20).
        // DT_BlackMesaPlayer m_recoilPunchAngles +0x1934, m_recoilPositionOffset +0x1940
        // (FUN_1025cb60).
        BMVR_TRY
        {
            float* punch = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x103C + 0x74);
            float* punchVel = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x103C + 0xB0);
            float* recoil = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x1934);
            float* recoilPos = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x1940);
            float* recoilStart = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x194C);
            punch[0] = punch[1] = punch[2] = 0.f;
            punchVel[0] = punchVel[1] = punchVel[2] = 0.f;
            recoil[0] = recoil[1] = recoil[2] = 0.f;
            recoilPos[0] = recoilPos[1] = recoilPos[2] = 0.f;
            *recoilStart = 0.f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    const char* StudioSequenceLabel(unsigned char* hdr, int seq)
    {
        if (!hdr || seq < 0)
            return nullptr;
        int numSeq = 0;
        int seqIndex = 0;
        BMVR_TRY
        {
            numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq);
            seqIndex = *reinterpret_cast<int*>(hdr + kStudioHdrLocalSeqIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        if (numSeq <= 0 || seq >= numSeq || seqIndex <= 0)
            return nullptr;
        unsigned char* desc = hdr + seqIndex + seq * kStudioSeqDescSize;
        int labelOff = 0;
        BMVR_TRY
        {
            labelOff = *reinterpret_cast<int*>(desc + kStudioSeqLabelIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        if (labelOff <= 0)
            return nullptr;
        // Copied under SEH: the header can be freed under us (no viewmodel is
        // drawn without a weapon, so the cached header stays an old weapon's;
        // after saves/loads its model was unloaded and a strncmp on the label
        // crashed the game, 2026-11-02). Callers only ever get the copy.
        thread_local char s_copy[4][96];
        thread_local unsigned s_next;
        char* out = s_copy[s_next++ & 3];
        const char* src = reinterpret_cast<const char*>(desc + labelOff);
        BMVR_TRY
        {
            int n = 0;
            for (; n < 95 && src[n]; ++n)
                out[n] = src[n];
            out[n] = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        return out;
    }

    // True when sequence seq of hdr is a delta (additive) layer. Played as the
    // main sequence it shows the bare reference pose.
    bool StudioSequenceIsDelta(unsigned char* hdr, int seq)
    {
        if (!hdr || seq < 0)
            return false;
        BMVR_TRY
        {
            const int numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq);
            const int seqIndex = *reinterpret_cast<int*>(hdr + kStudioHdrLocalSeqIndex);
            if (numSeq <= 0 || seq >= numSeq || seqIndex <= 0)
                return false;
            const int flags = *reinterpret_cast<int*>(hdr + seqIndex + seq * kStudioSeqDescSize + kStudioSeqFlags);
            return (flags & kStudioSeqFlagDelta) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // studiohdr_t::name (+12, 64 chars) is the path below models/, e.g.
    // "weapons\\v_gauss.mdl"; model names carry the "models/" prefix.
    bool StudioHdrMatchesModel(unsigned char* hdr, const char* model)
    {
        if (!hdr || !model || !model[0])
            return false;
        char name[65]{};
        BMVR_TRY
        {
            std::memcpy(name, hdr + 12, 64);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        // The header spells the path with backslashes, entity models with '/'.
        auto norm = [](char c) {
            return c == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
        };
        const size_t ln = std::strlen(name), lm = std::strlen(model);
        if (ln == 0 || ln > lm)
            return false;
        const char* tail = model + (lm - ln);
        for (size_t i = 0; i < ln; ++i)
            if (norm(tail[i]) != norm(name[i]))
                return false;
        return tail == model || norm(tail[-1]) == '/';
    }

    void SuppressViewmodelMovementAnims(void* viewmodel)
    {
        if (!viewmodel || !Hooks::m_VR || !Hooks::m_VR->IsGameplayEligible())
            return;
        unsigned char* hdr = g_LastViewmodelStudioHdr;
        int seq = 0;
        BMVR_TRY
        {
            seq = *reinterpret_cast<int*>(static_cast<char*>(viewmodel) + kViewmodelSequence);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        // Stock clip held at one cycle: the hands scrub it (manual magazines,
        // MagReload::g_ScrubSeq), or the debug hold (bmvr_debug_vm_seq /
        // bmvr_debug_vm_cycle). The draw keeps the gun body at its idle pose.
        if (Hooks::m_Game && viewmodel == static_cast<void*>(Hooks::m_Game->GetViewModelEntity()))
        {
            int scrubSeq = -1;
            float scrubCycle = 0.f;
            if (bmvr::g_DebugVmSeq >= 0.f)
            {
                scrubSeq = static_cast<int>(bmvr::g_DebugVmSeq + 0.5f);
                scrubCycle = bmvr::g_DebugVmCycle;
            }
            else if (const char* name = MagReload::g_ScrubSeq.load(std::memory_order_relaxed))
            {
                scrubSeq = FindSequenceByLabel(hdr, name);
                scrubCycle = MagReload::g_ScrubCycle.load(std::memory_order_relaxed);
            }
            MagReload::g_ScrubActive.store(scrubSeq >= 0, std::memory_order_relaxed);
            if (scrubSeq >= 0)
            {
                BMVR_TRY
                {
                    *reinterpret_cast<int*>(static_cast<char*>(viewmodel) + kViewmodelSequence) = scrubSeq;
                    *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelCycle) = scrubCycle;
                    *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate) = 0.f;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                Hooks::m_VR->NoteViewmodelBeamHidden(false);
                return;
            }
        }
        const char* label = StudioSequenceLabel(hdr, seq);
        if ((bmvr::g_RpgRocketShown || MagReload::g_Weapon.load(std::memory_order_relaxed) == MagReload::kWeaponRpg)
            && Hooks::m_Game && viewmodel == static_cast<void*>(Hooks::m_Game->GetViewModelEntity()))
            FixRpgRocketShown(viewmodel, hdr, label);
        const bool melee = Hooks::m_VR->IsPerformingMelee();
        // Classify once per sequence change instead of three lowercase passes
        // + ~30 strstr on every CalcViewModelView. The label is a copy, so the
        // header + sequence index identify it.
        struct LabelClass { unsigned char* hdr; int seq; char text[16]; bool action; bool loco; bool equip; bool beamHide; bool lowered; bool delta; bool scope; bool fireKick; };
        static LabelClass s_lc{ nullptr, -1 };
        if (!label || hdr != s_lc.hdr || seq != s_lc.seq
            || std::strncmp(s_lc.text, label, sizeof(s_lc.text) - 1) != 0)
        {
            s_lc.hdr = hdr;
            s_lc.seq = seq;
            s_lc.text[0] = 0;
            if (label)
                strncpy_s(s_lc.text, label, _TRUNCATE);
            s_lc.action = SequenceLabelLooksLikeWeaponAction(label);
            s_lc.loco = SequenceLabelLooksLikeLocomotionOnly(label);
            s_lc.equip = SequenceLabelLooksLikeEquipOnly(label);
            s_lc.delta = StudioSequenceIsDelta(hdr, seq);
            // Laser beam off while the animation swings the gun off the aim
            // (reload, draw, holster...); kept while firing and for the
            // shotgun pump between shots (gun stays on the aim). The shotgun
            // reload starts/ends with idletoshell / shelltoidle, which the
            // action words miss.
            s_lc.beamHide = false;
            s_lc.lowered = false;
            s_lc.scope = false;
            s_lc.fireKick = false;
            if (label)
            {
                char lb[96]{};
                for (int i = 0; i < 95 && label[i]; ++i)
                    lb[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
                if (s_lc.action || std::strstr(lb, "shell"))
                    s_lc.beamHide = !(std::strstr(lb, "fire") || std::strstr(lb, "shoot") || std::strstr(lb, "attack")
                        || std::strstr(lb, "pump"));
                // .357 fire clip kicks the barrel far up (user 2026-11-18):
                // beam off for the first half of it (bmvr_357_fire_beam_hide).
                s_lc.fireKick = s_lc.action && !s_lc.beamHide
                    && StudioHdrMatchesModel(hdr, "models/weapons/v_357.mdl");
                // Weapon held down while aiming at a friendly: idletolow, lowidle,
                // lowtoidle. Tested as names since their activities differ per model.
                if (std::strstr(lb, "idletolow") || std::strstr(lb, "lowidle") || std::strstr(lb, "lowtoidle"))
                    s_lc.beamHide = s_lc.lowered = true;
                // Crossbow idletoscope (and any to/from-scope clip): brings the
                // gun up to the flat camera.
                s_lc.scope = std::strstr(lb, "scope") != nullptr;
            }
            if (bmvr::g_AimDebug)
                Game::logMsg("AimDebug viewmodel seq %d '%s' action=%d loco=%d beamHide=%d tick=%lu",
                    seq, label ? label : "?", s_lc.action ? 1 : 0, s_lc.loco ? 1 : 0, s_lc.beamHide ? 1 : 0, GetTickCount());
        }
        const bool isAction = s_lc.action;
        const bool isLoco = s_lc.loco;
        // Black Mesa runs a second viewmodel that idles all the time; only
        // the weapon's own viewmodel decides.
        // The idle after a raise fades in over 0.2 s from the raise's last
        // frame. On the glock, MP5 and 357 the two transition clips carry
        // swapped activities, so the raise plays the lowering clip and idle
        // fades in from a lowered gun: keep the beam off a little longer.
        // The hands place the gun, so Black Mesa's lowered clip (aimed at a
        // friendly) is not shown and the laser stays on, one- or two-handed
        // (user 2026-11-17: the clip swung the gun ~33 deg across, away from
        // the real hands; 2026-11-25: one-handed too, the gun must not move
        // without the player aiming).
        bool noLower = false;
        if (Hooks::m_Game && viewmodel == static_cast<void*>(Hooks::m_Game->GetViewModelEntity()))
        {
            noLower = s_lc.lowered && bmvr::g_NoLowerAtFriendly;
            const bool lowered = s_lc.lowered && !noLower;
            static ULONGLONG s_loweredUntil = 0;
            const ULONGLONG now = GetTickCount64();
            if (lowered)
                s_loweredUntil = now + 300;
            else if (noLower)
                s_loweredUntil = 0;
            bool kick = false;
            if (s_lc.fireKick && bmvr::g_Vr357FireBeamHide)
            {
                BMVR_TRY
                {
                    kick = *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelCycle) < 0.5f;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            const bool beamHide = noLower ? false : (s_lc.beamHide || kick);
            // Right after a switch the last drawn header is still the previous
            // gun's, so the draw-skip / delta-idle guards (which need a match)
            // cannot hold the new gun yet: a fast menu switch to the gluon
            // showed its draw1 pose with the laser far off the aim for the
            // first frame (also in stable-26713286). No laser until the drawn
            // model is the selected one, nor on a frame that still shows a
            // draw / holster clip (the draw skip turns it into idle for the
            // next one), plus 150 ms: the overlay draws a beam snapshot a frame
            // behind this flag, and the gun cannot fire for ~0.5 s anyway.
            // The name compare only runs when the header or model changes.
            static ULONGLONG s_switchUntil = 0;
            static unsigned char* s_cmpHdr = nullptr;
            static const char* s_cmpModel = nullptr;
            static bool s_cmpMatch = false;
            const char* vmModel = Hooks::m_Game->GetEntityModelName(static_cast<C_BaseEntity*>(viewmodel));
            if (hdr != s_cmpHdr || vmModel != s_cmpModel)
            {
                s_cmpHdr = hdr;
                s_cmpModel = vmModel;
                s_cmpMatch = StudioHdrMatchesModel(hdr, vmModel);
            }
            if (s_lc.equip || !s_cmpMatch)
                s_switchUntil = now + 150;
            Hooks::m_VR->NoteViewmodelBeamHidden(beamHide || now < s_loweredUntil || now < s_switchUntil);
            // Same 300 ms tail for the clip state: idle blends in from the
            // lowered pose.
            const int lowerState = (lowered || now < s_loweredUntil) ? VR::kVmLowerLowered
                : (!beamHide && !s_lc.action ? VR::kVmLowerIdle : VR::kVmLowerOther);
            Hooks::m_VR->NoteViewmodelLowerState(lowerState, hdr);
            // The idle written below stays until the server changes the
            // sequence again, so this fires once per lowering.
            if (noLower)
            {
                static int s_log;
                if (s_log++ < 20)
                    Game::logMsg("No lower: lowered clip '%s' skipped, gun stays in the hand", label ? label : "?");
            }
        }
        if (noLower)
        {
            ForceViewmodelIdlePose(viewmodel, hdr);
            return;
        }
        // MP5 grenade launch: the stock 'grenade' clip plays (the gun kicks,
        // user 2026-11-19); its left hand going to the stock is undone on the
        // drawn bones (HoldLeftHandOnMp5Body).
        // Manual magazines: the hands do the reload, so the stock reload clip
        // (also the game's own refill after firing dry) is not shown.
        if (label && Hooks::m_Game && viewmodel == static_cast<void*>(Hooks::m_Game->GetViewModelEntity())
            && MagReload::g_Weapon.load(std::memory_order_relaxed) != MagReload::kWeaponNone)
        {
            char lb[64]{};
            for (int i = 0; i < 63 && label[i]; ++i)
                lb[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
            const bool shotgunClip = MagReload::g_Weapon.load(std::memory_order_relaxed) == MagReload::kWeaponShotgun
                && (std::strstr(lb, "shell") || std::strstr(lb, "pump"));
            // Crossbow without a loaded magazine (fired dry, magazine out or
            // in the hand): the unloaded idle, string forward, instead of the
            // cocked idle the hidden refill leaves (user 2026-11-20: the
            // string went back before the new magazine was in).
            // Only on the crossbow's own header: around a load g_Weapon / the
            // cached header can still be the previous gun's, and a crossbow
            // sequence index written into another model crashed the game.
            const bool crossbowHdr = MagReload::g_Weapon.load(std::memory_order_relaxed) == MagReload::kWeaponCrossbow
                && StudioHdrMatchesModel(hdr, "models/weapons/v_crossbow.mdl");
            const bool crossbowUnloaded = crossbowHdr
                && MagReload::g_State.load(std::memory_order_relaxed) != MagReload::kLoaded;
            const bool idleClip = std::strncmp(lb, "idle", 4) == 0 && std::strcmp(lb, "idle_unloaded") != 0;
            if (crossbowUnloaded && (idleClip || std::strstr(lb, "reload")))
            {
                static int s_unlLog;
                if (s_unlLog++ < 10)
                    Game::logMsg("Mag reload: crossbow unloaded, '%s' shown as idle_unloaded", label);
                Hooks::m_VR->NoteViewmodelBeamHidden(false);
                ForceViewmodelSequencePose(viewmodel, hdr, "idle_unloaded");
                return;
            }
            // Magazine seated: the frozen idle_unloaded written above stays on
            // the client until the game next changes the sequence (user
            // 2026-11-20: the string went back only a while after the
            // magazine was in). Back to the running idle at once.
            if (!crossbowUnloaded && crossbowHdr && std::strcmp(lb, "idle_unloaded") == 0)
            {
                const int idle = FindIdleSequence(hdr);
                BMVR_TRY
                {
                    if (idle >= 0)
                        *reinterpret_cast<int*>(static_cast<char*>(viewmodel) + kViewmodelSequence) = idle;
                    *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelCycle) = 0.f;
                    *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate) = 1.f;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                Game::logMsg("Mag reload: crossbow loaded, idle_unloaded -> idle");
                return;
            }
            if (std::strstr(lb, "reload") || shotgunClip)
            {
                static int s_log;
                if (s_log++ < 20)
                    Game::logMsg("Mag reload: stock clip '%s' not shown", label);
                Hooks::m_VR->NoteViewmodelBeamHidden(false);
                ForceViewmodelIdlePose(viewmodel, hdr);
                return;
            }
        }
        // Lock-free class flag from the weapon model dCalcViewModelView just
        // classified; replaces a model-info lookup + four strstr per call (this
        // runs twice per CalcViewModelView) and an unlocked read of the
        // std::string m_LastViewmodelModel that ProcessInput rewrites.
        const bool crowbar = Hooks::m_VR->ViewmodelIsCrowbar();

        auto restoreRate = [&]() {
            BMVR_TRY
            {
                float* rate = reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate);
                if (*rate <= 0.01f)
                    *rate = 1.f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        };
        auto freeze = [&]() {
            BMVR_TRY
            {
                *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelCycle) = 0.f;
                *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate) = 0.f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        };

        // Crowbar: keep only equip/draw. Attack/idle/sprint would otherwise
        // snap to the first frame of HITCENTER. Force the idle sequence so
        // the controller is the swing (implementation-plan §9).
        // Never freeze MP5 (or any other gun) fire/reload/draw here.
        if (crowbar)
        {
            if (s_lc.equip)
                restoreRate();
            else
                ForceViewmodelIdlePose(viewmodel, hdr);
            return;
        }

        // Crowbar VR swing: pin cycle once without thrashing every frame
        // (that made HITCENTER look jerky). Prefer idle hold when possible.
        if (melee)
        {
            if (isAction && label
                && (std::strstr(label, "hit") || std::strstr(label, "HIT")
                    || std::strstr(label, "miss") || std::strstr(label, "MISS")
                    || std::strstr(label, "attack") || std::strstr(label, "Attack")))
            {
                ForceViewmodelIdlePose(viewmodel, hdr);
            }
            return;
        }

        // Draw / holster / first-pickup "admire": the gun is already in the
        // hand, so show the idle pose at once. The clips swing the gun off
        // the hand (the tau points up and back) and the idle fading in
        // afterwards brought the laser back on a tilted gun. Firing still
        // waits for the server's draw time, as in flat (~0.5 s).
        // The header is the one last drawn, so right after a switch it can
        // still be the previous gun's: only force when its name matches the
        // active weapon's viewmodel, or the old model's idle index would land
        // on the new model.
        // Switching again while the previous gun is still drawing (hovering
        // through the weapon menu ring) leaves the new viewmodel on sequence 0
        // for its whole draw time (~0.5 s; the game does this, also with the
        // draw skip off). On the tau and crossbow sequence 0 is a delta layer,
        // which alone is the reference pose: barrel sideways, laser off the
        // aim. A delta layer is never a real main sequence, so show idle. Other
        // guns' sequence 0 is not a delta layer: they are not touched.
        if (bmvr::g_DeltaSeqIdle && s_lc.delta && Hooks::m_Game
            && viewmodel == static_cast<void*>(Hooks::m_Game->GetViewModelEntity())
            && StudioHdrMatchesModel(hdr, Hooks::m_Game->GetEntityModelName(static_cast<C_BaseEntity*>(viewmodel))))
        {
            static unsigned char* s_loggedHdr;
            if (s_loggedHdr != hdr)
            {
                s_loggedHdr = hdr;
                Game::logMsg("Viewmodel delta layer seq %d '%s' as main sequence -> idle", seq, label ? label : "?");
            }
            ForceViewmodelIdlePose(viewmodel, hdr);
            return;
        }

        // Crossbow zoom: idletoscope moves the gun from the hand to the flat
        // camera (screen centre) over the engine's zoom ramp, so in VR the gun
        // slid sideways across the view before the scope opened. The gun is
        // already at the eye in VR: hold the idle pose.
        if (bmvr::g_ScopeClipIdle && s_lc.scope && Hooks::m_Game
            && viewmodel == static_cast<void*>(Hooks::m_Game->GetViewModelEntity())
            && StudioHdrMatchesModel(hdr, Hooks::m_Game->GetEntityModelName(static_cast<C_BaseEntity*>(viewmodel))))
        {
            static unsigned char* s_loggedScopeHdr;
            static int s_loggedScopeSeq = -1;
            if (s_loggedScopeHdr != hdr || s_loggedScopeSeq != seq)
            {
                s_loggedScopeHdr = hdr;
                s_loggedScopeSeq = seq;
                Game::logMsg("Viewmodel scope clip seq %d '%s' -> idle", seq, label ? label : "?");
            }
            ForceViewmodelIdlePose(viewmodel, hdr);
            return;
        }

        if (bmvr::g_SkipDrawAnim && s_lc.equip && Hooks::m_Game
            && viewmodel == static_cast<void*>(Hooks::m_Game->GetViewModelEntity())
            && StudioHdrMatchesModel(hdr, Hooks::m_Game->GetEntityModelName(static_cast<C_BaseEntity*>(viewmodel))))
        {
            ForceViewmodelIdlePose(viewmodel, hdr);
            return;
        }

        // Sprint loop and its in/out transitions: show the aimed idle pose.
        // Freezing them held the gun on the lowered first frame for as long
        // as BM's server-side sprint-out lasted (~1 s after stopping, user
        // 2026-10-14); in VR the gun is in the hand and should stay aimed.
        if (bmvr::g_SprintAimedPose && !isAction && label)
        {
            char low[32]{};
            for (int i = 0; i < 31 && label[i]; ++i)
                low[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
            if (std::strstr(low, "sprint") != nullptr)
            {
                ForceViewmodelIdlePose(viewmodel, hdr);
                return;
            }
        }

        // Never rewrite m_nSequence. Freeze cycle/rate only for explicit
        // sprint/swim/walk/run/bob/idle/fidget. Never freeze draw/holster/
        // reload/fire/attack. If idle zeroed playbackRate, restore 1 on action.
        if (isLoco && !isAction)
            freeze();
        else
            restoreRate();
    }

    unsigned char* AsStudioHdr(void* p)
    {
        if (!p)
            return nullptr;
        int id = 0;
        int ver = 0;
        BMVR_TRY
        {
            id = *reinterpret_cast<int*>(p);
            ver = *(reinterpret_cast<int*>(p) + 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        if (id != kStudioIdst || ver < 44 || ver > 49)
            return nullptr;
        return static_cast<unsigned char*>(p);
    }

    unsigned char* ResolveStudioHdr(void* state)
    {
        if (!state)
            return nullptr;
        DrawModelStateLite* st = nullptr;
        unsigned char* hdr = nullptr;
        void* inner = nullptr;
        BMVR_TRY
        {
            st = reinterpret_cast<DrawModelStateLite*>(state);
            hdr = AsStudioHdr(st->studioHdr);
            if (hdr)
                return hdr;
            inner = *reinterpret_cast<void**>(st->studioHdr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        return AsStudioHdr(inner);
    }

    void ScaleMatrix3x4AroundPivot(float m[3][4], const float pivot[3], float s)
    {
        for (int r = 0; r < 3; ++r)
        {
            m[r][0] *= s;
            m[r][1] *= s;
            m[r][2] *= s;
            m[r][3] = pivot[r] + (m[r][3] - pivot[r]) * s;
        }
    }

    // Anisotropic view-Y scale around the controller grip, not the HMD.
    // Scaling translations toward the camera parented the gun to nod
    // (lower head → weapon rose; live log yFix=0.577 pulled origin Z onto
    // the eye). Stereo already draws at the eye RT aspect (worldMatch /
    // NormalizeViewSetupForVREye), so this is only for a leftover viewport
    // mismatch and must not move the grip.
    void UnstretchMatrix3x4ViewY(float m[3][4], const float pivot[3],
        const float right[3], const float up[3], const float fwd[3], float yFix)
    {
        const float rel[3] = {
            m[0][3] - pivot[0],
            m[1][3] - pivot[1],
            m[2][3] - pivot[2]
        };
        const float vx = rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2];
        const float vy = (rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2]) * yFix;
        const float vz = rel[0] * fwd[0] + rel[1] * fwd[1] + rel[2] * fwd[2];
        m[0][3] = pivot[0] + right[0] * vx + up[0] * vy + fwd[0] * vz;
        m[1][3] = pivot[1] + right[1] * vx + up[1] * vy + fwd[1] * vz;
        m[2][3] = pivot[2] + right[2] * vx + up[2] * vy + fwd[2] * vz;
        const float k = yFix - 1.f;
        for (int c = 0; c < 3; ++c)
        {
            const float au = m[0][c] * up[0] + m[1][c] * up[1] + m[2][c] * up[2];
            m[0][c] += up[0] * au * k;
            m[1][c] += up[1] * au * k;
            m[2][c] += up[2] * au * k;
        }
    }

    void BuildMatrix3x4FromOrgAngles(float out[3][4], const Vector& origin, const QAngle& ang)
    {
        Vector f, r, u;
        QAngle::AngleVectors(ang, &f, &r, &u);
        out[0][0] = f.x; out[0][1] = r.x; out[0][2] = u.x; out[0][3] = origin.x;
        out[1][0] = f.y; out[1][1] = r.y; out[1][2] = u.y; out[1][3] = origin.y;
        out[2][0] = f.z; out[2][1] = r.z; out[2][2] = u.z; out[2][3] = origin.z;
    }

    void InvertMatrix3x4TR(const float in[3][4], float out[3][4])
    {
        out[0][0] = in[0][0]; out[0][1] = in[1][0]; out[0][2] = in[2][0];
        out[1][0] = in[0][1]; out[1][1] = in[1][1]; out[1][2] = in[2][1];
        out[2][0] = in[0][2]; out[2][1] = in[1][2]; out[2][2] = in[2][2];
        const float tx = -in[0][3];
        const float ty = -in[1][3];
        const float tz = -in[2][3];
        out[0][3] = tx * out[0][0] + ty * out[0][1] + tz * out[0][2];
        out[1][3] = tx * out[1][0] + ty * out[1][1] + tz * out[1][2];
        out[2][3] = tx * out[2][0] + ty * out[2][1] + tz * out[2][2];
    }

    void MulMatrix3x4(const float a[3][4], const float b[3][4], float out[3][4])
    {
        float tmp[3][4];
        for (int r = 0; r < 3; ++r)
        {
            tmp[r][0] = a[r][0] * b[0][0] + a[r][1] * b[1][0] + a[r][2] * b[2][0];
            tmp[r][1] = a[r][0] * b[0][1] + a[r][1] * b[1][1] + a[r][2] * b[2][1];
            tmp[r][2] = a[r][0] * b[0][2] + a[r][1] * b[1][2] + a[r][2] * b[2][2];
            tmp[r][3] = a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3];
        }
        std::memcpy(out, tmp, sizeof(tmp));
    }

    void ApplyMatrix3x4Delta(float m[3][4], const float delta[3][4])
    {
        float tmp[3][4];
        MulMatrix3x4(delta, m, tmp);
        std::memcpy(m, tmp, sizeof(tmp));
    }

    int StudioHdrNumBones(unsigned char* hdr)
    {
        if (!hdr)
            return 0;
        int n = 0;
        BMVR_TRY { n = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (n < 1 || n > kMaxStudioBones)
            return 0;
        return n;
    }

    const char* StudioRelString(const unsigned char* base, int rel);

    int FindStudioBoneIndex(unsigned char* hdr, const char* name)
    {
        if (!hdr || !name || !name[0])
            return -1;
        int length = 0;
        int numbones = 0;
        int boneindex = 0;
        BMVR_TRY
        {
            length = *reinterpret_cast<int*>(hdr + kStudioHdrLength);
            numbones = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones);
            boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        if (length < 256 || length > 4 * 1024 * 1024)
            return -1;
        if (numbones <= 0 || numbones > kMaxStudioBones)
            return -1;
        if (boneindex < 0 || boneindex + numbones * kStudioBoneSize > length)
            return -1;
        for (int i = 0; i < numbones; ++i)
        {
            unsigned char* bone = hdr + boneindex + i * kStudioBoneSize;
            int szname = 0;
            BMVR_TRY { szname = *reinterpret_cast<int*>(bone); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            const char* n = StudioRelString(bone, szname);
            if (n[0] && _stricmp(n, name) == 0)
                return i;
        }
        return -1;
    }

    void SampleViewmodelOffhandGrip(unsigned char* hdr, const float* bones12, int nBones)
    {
        if (!hdr || !bones12 || nBones <= 0 || !Hooks::m_VR)
            return;
        // Tau / gluon: the support grip is the handle on the gun's left side.
        // The stock left hand is posed for the flat camera (~0.4 m off the VR
        // gun), so the point comes from the gun's own body bone instead:
        // middle of the left rod, in that bone's frame, measured from the
        // models' vertices (v_egon.vvd: rod x 7.4..9.6, z -5.5..-15.7, where
        // the stock L_Wrist binds; v_gauss.vvd: rod x 7..12.9 sloping down).
        // Not Egon_handle: that is the gluon's right-hand pistol grip on top.
        struct SideHandle { const char* bone; float local[3]; };
        static const SideHandle kSideHandles[] =
        {
            { "Egon_Root", { 8.5f, -1.4f, -12.5f } },
            { "V_TauCannon_Ref", { 10.0f, -12.5f, -5.5f } },
        };
        float local[3] = { 0.f, 0.f, 0.f };
        bool sideHandle = false;
        int idx = -1;
        for (const SideHandle& h : kSideHandles)
        {
            idx = FindStudioBoneIndex(hdr, h.bone);
            if (idx >= 0)
            {
                sideHandle = true;
                local[0] = h.local[0];
                local[1] = h.local[1];
                local[2] = h.local[2];
                break;
            }
        }
        if (idx < 0)
            idx = FindStudioBoneIndex(hdr, "L_Wrist");
        if (idx < 0)
            idx = FindStudioBoneIndex(hdr, "L_Hand");
        if (idx < 0 || idx >= nBones)
            return;
        const float* m = bones12 + idx * 12;
        Vector world{};
        BMVR_TRY
        {
            world.x = m[0] * local[0] + m[1] * local[1] + m[2] * local[2] + m[3];
            world.y = m[4] * local[0] + m[5] * local[1] + m[6] * local[2] + m[7];
            world.z = m[8] * local[0] + m[9] * local[1] + m[10] * local[2] + m[11];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
            return;
        if (world.LengthSqr() <= 1.f)
            return;
        Hooks::m_VR->NoteViewmodelOffhandWorld(world, sideHandle);
    }

    int FindWeaponRootBone(unsigned char* hdr, float rest[3], char (&bestName)[64]);

    // Frag grenade: the grenade is its own root bone (GrenadeSpawn, pins are
    // its children) and the pin-pull/throw animations swing it across the
    // view while the hidden arms do the work. In VR the hand holds still, so
    // keep GrenadeSpawn where it sits in idle (entity-relative pose recorded
    // during idle1/idle2) and let only the pins move. Returns the extra delta
    // to apply to every bone before the rigid move, or false for none.
    // "Gun's own right hand": the viewmodel's arms body part stays drawn, but
    // every left-arm bone and the right upper/forearm collapse to a point at
    // the right wrist. What is left is the stock right hand on the grip (same
    // art and grip pose the gun was animated with); the left hand is the glove.
    // Left and right arm meshes share no vertex (checked on v_glock/v_mp5/
    // v_shotgun, all arm variants), so nothing of the left arm can stretch.
    // "Gun's own right hand" only on the guns (not the gluon): their rigs
    // have the R_Arm/L_* bones the collapse needs and a hand on a grip. The
    // grenade, satchel, tripmine, snark and Hivehand viewmodels are hands
    // holding or wearing the thing (the satchel kept a whole arm, the
    // Hivehand a hand under the model): those keep the hidden arms. The
    // crowbar is drawn at scale 1, not ViewmodelScale: ShrinkViewmodelRightHand
    // brings its hand to the gun hands' size.
    bool GunRightHandForModel(const char* modelName)
    {
        if (!bmvr::g_VrGunRightHand || bmvr::g_LeftHanded || !modelName)
            return false;
        // Not the gluon (v_egon): its idle pose holds the right hand a hand's
        // width beside the top grip, floating off the gun (headless dump
        // 2026-11-18); without it the gluon has no right hand, as the Hivehand.
        static const char* kGuns[] = { "v_glock", "v_357", "v_mp5", "v_shotgun", "v_crossbow", "v_rpg",
            "v_gauss", "v_tau", "v_crowbar", "v_grenade", "v_squeak", "v_tripmine" };
        char buf[128]{};
        strncpy_s(buf, modelName, _TRUNCATE);
        for (char* c = buf; *c; ++c)
            *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        // Satchel hands: the radio's own right hand is drawn
        // (SatchelRadioToRightHand); the bag's arm stays hidden.
        if (std::strstr(buf, "v_satchel_radio") && Hooks::m_VR && Hooks::m_VR->SatchelHandsActive())
            return true;
        for (const char* g : kGuns)
            if (std::strstr(buf, g))
                return true;
        return false;
    }

    // Crowbar with its own right hand: the crowbar is drawn at scale 1 while
    // the guns (and their hands) use ViewmodelScale, so its hand looked ~1.5x
    // too big. Scales R_Wrist and the finger bones by k around the fist
    // centre (wrist + middle finger joints as posed), so the smaller fist
    // still closes on the handle.
    void ShrinkViewmodelRightHand(unsigned char* hdr, float* bones, int nBones, float k)
    {
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_idx[24]{};
        static thread_local int s_n = 0, s_centre[4]{ -1, -1, -1, -1 };
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_n = 0;
            static const char* kHand[] = { "R_Wrist", "R_Thumb1", "R_Thumb2", "R_Thumb3", "R_Index1", "R_Index2",
                "R_Index3", "R_Middle1", "R_Middle2", "R_Middle3", "R_Ring1", "R_Ring2", "R_Ring3", "R_Pinky1",
                "R_Pinky2", "R_Pinky3", "R_Middlenub",
                // Grenade: the grenade and its pin shrink with the hand (user
                // 2026-11-19: the smaller fist cut through the x1.25 grenade).
                "GrenadeSpawn", "Grenade_Pin2", "Grenade_Pin1", "Grenad_Pin1" };
            for (const char* n : kHand)
            {
                const int i = FindStudioBoneIndex(hdr, n);
                if (i >= 0 && s_n < 24)
                    s_idx[s_n++] = i;
            }
            const char* kCentre[4] = { "R_Wrist", "R_Middle1", "R_Middle2", "R_Middle3" };
            for (int c = 0; c < 4; ++c)
                s_centre[c] = FindStudioBoneIndex(hdr, kCentre[c]);
            Game::logMsg("Gun right hand: hand shrink over %d bones, k=%.2f", s_n, k);
        }
        if (!bones || !(k > 0.2f && k < 1.5f) || std::fabs(k - 1.f) < 0.01f)
            return;
        float c[3] = { 0.f, 0.f, 0.f };
        for (int i = 0; i < 4; ++i)
        {
            if (s_centre[i] < 0 || s_centre[i] >= nBones)
                return;
            const float* m = bones + s_centre[i] * 12;
            c[0] += m[3] * 0.25f;
            c[1] += m[7] * 0.25f;
            c[2] += m[11] * 0.25f;
        }
        for (int j = 0; j < s_n; ++j)
        {
            if (s_idx[j] >= nBones)
                continue;
            float* m = bones + s_idx[j] * 12;
            for (int r = 0; r < 3; ++r)
            {
                m[r * 4 + 0] *= k;
                m[r * 4 + 1] *= k;
                m[r * 4 + 2] *= k;
                m[r * 4 + 3] = c[r] + (m[r * 4 + 3] - c[r]) * k;
            }
        }
    }

    // Own right hand turned like the gun hands (user 2026-11-18: the
    // grenade's stock hand held it like a torch, forearm down, pitched and
    // rolled ~90 deg from a hand on a controller). The pistol-grip guns
    // (glock, .357, MP5) look right, so their R_Wrist orientation in the
    // right controller's frame is the reference; the grenade model turns
    // about its fist so its R_Wrist matches. Reference sampled from the last
    // such gun drawn (logged for baking in). Baked from the user's
    // 2026-11-18 headset log (v_mp5) and fixed from now on.
    struct WristRef { float m[3][3]; bool valid; };
    WristRef g_WristRef{ { { 0.999f, 0.049f, -0.004f },
                           { 0.004f, 0.010f, 1.000f },
                           { -0.049f, 0.999f, -0.010f } }, true };

    bool WristAxesInController(VR* vr, unsigned char* hdr, const float* bones, int nBones, float out[3][3],
        float world[3][3], Vector* fist)
    {
        const int w = FindStudioBoneIndex(hdr, "R_Wrist");
        if (w < 0 || w >= nBones)
            return false;
        const float* m = bones + w * 12;
        Vector col[3];
        for (int c = 0; c < 3; ++c)
        {
            col[c] = Vector(m[c], m[4 + c], m[8 + c]);
            if (VectorNormalize(col[c]) < 1e-4f)
                return false;
        }
        Vector f, r, u;
        QAngle::AngleVectors(vr->GetRightControllerAbsAngle(), &f, &r, &u);
        for (int c = 0; c < 3; ++c)
        {
            out[0][c] = col[c].Dot(f);
            out[1][c] = col[c].Dot(r);
            out[2][c] = col[c].Dot(u);
            world[0][c] = col[c].x;
            world[1][c] = col[c].y;
            world[2][c] = col[c].z;
        }
        if (fist)
        {
            const char* kCentre[4] = { "R_Wrist", "R_Middle1", "R_Middle2", "R_Middle3" };
            Vector c(0.f, 0.f, 0.f);
            for (const char* n : kCentre)
            {
                const int b = FindStudioBoneIndex(hdr, n);
                if (b < 0 || b >= nBones)
                    return false;
                const float* bm = bones + b * 12;
                c += Vector(bm[3], bm[7], bm[11]) * 0.25f;
            }
            *fist = c;
        }
        return true;
    }

    void NoteGunWristReference(VR* vr, unsigned char* hdr, const float* bones, int nBones, const char* modelName)
    {
        float local[3][3], world[3][3];
        if (!vr || !WristAxesInController(vr, hdr, bones, nBones, local, world, nullptr))
            return;
        // Logged only: the baked reference (g_WristRef) stays. Live samples
        // differ a lot per gun and frame (headless glock 2026-11-19: x axis
        // (0.57,-0.60,-0.56) vs the headset MP5's (1.00,0.00,-0.05)), so the
        // grenade's pose depended on which gun was drawn last.
        static int s_log;
        if (s_log++ < 3)
            Game::logMsg("Wrist reference from %s: x=(%.3f,%.3f,%.3f) y=(%.3f,%.3f,%.3f) z=(%.3f,%.3f,%.3f) (controller fwd/right/up)",
                modelName ? modelName : "?", local[0][0], local[1][0], local[2][0], local[0][1], local[1][1], local[2][1],
                local[0][2], local[1][2], local[2][2]);
    }

    void AlignWristToReference(VR* vr, unsigned char* hdr, float* bones, int nBones)
    {
        if (!vr || !g_WristRef.valid || !bmvr::g_VrGrenadeWristAlign)
            return;
        float local[3][3], world[3][3];
        Vector c;
        if (!WristAxesInController(vr, hdr, bones, nBones, local, world, &c))
            return;
        // Wanted wrist axes in the world = controller axes * reference.
        Vector f, r, u;
        QAngle::AngleVectors(vr->GetRightControllerAbsAngle(), &f, &r, &u);
        float want[3][3];
        for (int c2 = 0; c2 < 3; ++c2)
        {
            const Vector v = f * g_WristRef.m[0][c2] + r * g_WristRef.m[1][c2] + u * g_WristRef.m[2][c2];
            want[0][c2] = v.x;
            want[1][c2] = v.y;
            want[2][c2] = v.z;
        }
        // Q = want * world^T (world is orthonormal).
        float Q[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                Q[i][j] = want[i][0] * world[j][0] + want[i][1] * world[j][1] + want[i][2] * world[j][2];
        static int s_log;
        if (s_log++ < 3)
            Game::logMsg("Grenade/snark wrist aligned to the gun hands (Q diag %.2f %.2f %.2f)", Q[0][0], Q[1][1], Q[2][2]);
        BMVR_TRY
        {
            for (int b = 0; b < nBones; ++b)
            {
                float* m = bones + b * 12;
                float out[12];
                for (int rr = 0; rr < 3; ++rr)
                    for (int col = 0; col < 3; ++col)
                        out[rr * 4 + col] = Q[rr][0] * m[col] + Q[rr][1] * m[4 + col] + Q[rr][2] * m[8 + col];
                const float p[3] = { m[3] - c.x, m[7] - c.y, m[11] - c.z };
                const float t[3] = { c.x, c.y, c.z };
                for (int rr = 0; rr < 3; ++rr)
                    out[rr * 4 + 3] = t[rr] + Q[rr][0] * p[0] + Q[rr][1] * p[1] + Q[rr][2] * p[2];
                std::memcpy(m, out, sizeof(out));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Satchel (VrSatchelHands, user 2026-11-18: bag left, detonator right).
    // Black Mesa's v_satchel is the flat pose: the left hand holding the
    // detonator (part of the arm mesh) with the bag (bodygroup on the root
    // bone SatchelSpawn) floating beside it, thrown by the right side;
    // v_satchel_radio (right arm + detonator) only exists once a charge is
    // out. Here: v_satchel's arms stay hidden and only the bag is drawn,
    // hung by the middle of its top from the left glove's fist and turned
    // from the right controller's frame to the left one's (upright like
    // the flat pose when the controller is held the same way). The radio's
    // own right hand is moved so its fist sits where a glove fist sits on
    // the right controller (bmvr_satchel_fist_fwd/right/up, cm).
    void SatchelRadioToRightHand(VR* vr, unsigned char* hdr, float* bones, int nBones, const Vector& rightCtrl)
    {
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_centre[4]{ -1, -1, -1, -1 };
        if (!vr || !hdr || !bones || nBones <= 0)
            return;
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            const char* kR[4] = { "R_Wrist", "R_Middle1", "R_Middle2", "R_Middle3" };
            for (int i = 0; i < 4; ++i)
                s_centre[i] = FindStudioBoneIndex(hdr, kR[i]);
            Game::logMsg("Satchel radio (right) hand: centre bones %d %d %d %d", s_centre[0], s_centre[1], s_centre[2], s_centre[3]);
        }
        for (int i = 0; i < 4; ++i)
            if (s_centre[i] < 0 || s_centre[i] >= nBones)
                return;
        Vector cf, cr, cu;
        QAngle::AngleVectors(vr->GetRightControllerAbsAngle(), &cf, &cr, &cu);
        const float u = (vr->m_VRScale > 1.f ? vr->m_VRScale : 39.37f) * 0.01f; // units per cm
        const Vector target = rightCtrl + cf * (bmvr::g_VrSatchelFist[0] * u) + cr * (bmvr::g_VrSatchelFist[1] * u)
            + cu * (bmvr::g_VrSatchelFist[2] * u);
        BMVR_TRY
        {
            Vector c(0.f, 0.f, 0.f);
            for (int i = 0; i < 4; ++i)
            {
                const float* m = bones + s_centre[i] * 12;
                c += Vector(m[3], m[7], m[11]) * 0.25f;
            }
            const Vector d = target - c;
            for (int b = 0; b < nBones; ++b)
            {
                float* m = bones + b * 12;
                m[3] += d.x;
                m[7] += d.y;
                m[11] += d.z;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        const DWORD nowMs = GetTickCount();
        MagReload::g_SatchelRadioMs.store(nowMs ? nowMs : 1, std::memory_order_relaxed);
    }

    // Detonator thumb (user 2026-11-19: the charge blew, then the finger
    // pressed). The game detonates on the trigger frame, but its
    // detonateDetonator clip (31 frames, thumb down on frames 4-9) reaches
    // the viewmodel a few frames later. While the trigger is held and that
    // clip is not past its own press yet, the radio hand's thumb is bent to
    // the clip's frame-4 press here (decoded from v_satchel_radio: R_Thumb2
    // -16.2 -> -26.9 deg, R_Thumb3 -30.3 -> -51.3 deg about their own z;
    // neither moves in idle).
    // Gun's own hands: wrist bones as drawn, for the glove renderer's cuff
    // caps (MagReload::g_GunWrist). The left one only while the gun's own
    // left hand is kept (support-hand lock / bolt snap).
    void PublishGunWrists(unsigned char* hdr, const float* bones, int nBones, bool left)
    {
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_w[2] = { -1, -1 };
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_w[0] = FindStudioBoneIndex(hdr, "L_Wrist");
            s_w[1] = FindStudioBoneIndex(hdr, "R_Wrist");
        }
        const DWORD now = GetTickCount();
        for (int h = 0; h < 2; ++h)
        {
            if (s_w[h] < 0 || s_w[h] >= nBones || (h == 0 && !left))
                continue;
            const float* m = bones + s_w[h] * 12;
            for (int k = 0; k < 12; ++k)
                MagReload::g_GunWrist[h][k].store(m[k], std::memory_order_relaxed);
            MagReload::g_GunWristMs[h].store(now ? now : 1, std::memory_order_release);
        }
    }

    bool InvertMatrix3x4General(const float in[3][4], float out[3][4]);
    void PressDetonatorThumb(unsigned char* hdr, float* bones, int nBones)
    {
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_t2 = -1, s_t3 = -1, s_detSeq = -1;
        if (!hdr || !bones || !Hooks::m_Game)
            return;
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_t2 = FindStudioBoneIndex(hdr, "R_Thumb2");
            s_t3 = FindStudioBoneIndex(hdr, "R_Thumb3");
            s_detSeq = FindSequenceByLabel(hdr, "detonateDetonator");
            Game::logMsg("Detonator thumb: bones %d %d, detonate clip %d", s_t2, s_t3, s_detSeq);
        }
        if (s_t2 < 0 || s_t3 < 0 || s_t2 >= nBones || s_t3 >= nBones)
            return;
        const unsigned held = MagReload::g_DetonateHeldMs.load(std::memory_order_relaxed);
        const DWORD now = GetTickCount();
        if (held == 0 || now - held > 60)
            return;
        void* vm = Hooks::m_Game->GetViewModelEntity();
        int seq = -1;
        float cycle = 0.f;
        BMVR_TRY
        {
            if (vm)
            {
                seq = *reinterpret_cast<int*>(static_cast<char*>(vm) + kViewmodelSequence);
                cycle = *reinterpret_cast<float*>(static_cast<char*>(vm) + kViewmodelCycle);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        // The clip's own press (frame 4 of 30) has started: it takes over.
        if (seq == s_detSeq && cycle >= 4.f / 30.f && cycle < 13.f / 30.f)
            return;
        // Clip frames 0-3 already bend part of the way: only the rest.
        float done = 0.f;
        if (seq == s_detSeq && cycle < 4.f / 30.f)
        {
            static const float kPart[5] = { 0.f, 0.16f, 0.50f, 0.84f, 1.f }; // frames 0..4, from the decoded angles
            const float fr = cycle * 30.f;
            const int i = static_cast<int>(fr);
            done = kPart[i] + (kPart[i + 1] - kPart[i]) * (fr - i);
        }
        auto bend = [&](int b, float deg, int child) {
            float* m = bones + b * 12;
            const float a = deg * (3.14159265f / 180.f);
            const float c = std::cos(a), s = std::sin(a);
            // M' = M * Rz(a): the bone turned about its own z axis.
            float before[12];
            std::memcpy(before, m, sizeof(before));
            for (int r = 0; r < 3; ++r)
            {
                const float x = before[r * 4 + 0], y = before[r * 4 + 1];
                m[r * 4 + 0] = c * x + s * y;
                m[r * 4 + 1] = -s * x + c * y;
            }
            if (child < 0)
                return;
            // The child rides along: child' = M' * inv(M) * child (rotation
            // about the bone's origin, which stays).
            float inv[3][4], rel[3][4], out[3][4];
            if (!InvertMatrix3x4General(reinterpret_cast<const float(*)[4]>(before), inv))
                return;
            float* ch = bones + child * 12;
            MulMatrix3x4(inv, reinterpret_cast<const float(*)[4]>(ch), rel);
            MulMatrix3x4(reinterpret_cast<const float(*)[4]>(m), rel, out);
            std::memcpy(ch, out, sizeof(out));
        };
        BMVR_TRY
        {
            bend(s_t2, -10.7f * (1.f - done), s_t3);
            bend(s_t3, -21.0f * (1.f - done), -1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        static int s_log;
        if (s_log++ < 3)
            Game::logMsg("Detonator thumb pressed with the trigger (clip seq %d cycle %.2f)", seq, cycle);
    }

    void SatchelBagToLeftHand(VR* vr, unsigned char* hdr, float* bones, int nBones)
    {
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_bag = -1;
        if (!vr || !hdr || !bones || nBones <= 0)
            return;
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_bag = FindStudioBoneIndex(hdr, "SatchelSpawn");
            Game::logMsg("Satchel bag bone %d", s_bag);
        }
        if (s_bag < 0 || s_bag >= nBones)
            return;
        Vector lp;
        QAngle la;
        if (!vr->OffhandWorldPoseForDraw(lp, la))
            return;
        Vector lf, lr, lu, rf, rr, ru;
        QAngle::AngleVectors(la, &lf, &lr, &lu);
        QAngle::AngleVectors(vr->GetRightControllerAbsAngle(), &rf, &rr, &ru);
        // Left glove fist (drawn this frame or recently), else the constant.
        Vector fist;
        const DWORD now = GetTickCount();
        if (const unsigned palmMs = MagReload::g_LeftPalmMs.load(std::memory_order_relaxed);
            palmMs != 0 && now - palmMs < 500)
            fist = lp + lf * MagReload::g_LeftPalmLocal[0].load(std::memory_order_relaxed)
                + lr * MagReload::g_LeftPalmLocal[1].load(std::memory_order_relaxed)
                + lu * MagReload::g_LeftPalmLocal[2].load(std::memory_order_relaxed);
        else
        {
            const float u = (vr->m_VRScale > 1.f ? vr->m_VRScale : 39.37f) * 0.01f;
            fist = lp + lf * (bmvr::g_VrSatchelFist[0] * u) - lr * (bmvr::g_VrSatchelFist[1] * u)
                + lu * (bmvr::g_VrSatchelFist[2] * u);
        }
        // Bag mesh in the SatchelSpawn frame (v_satchel.vvd through
        // poseToBone): x -3.97..9.75, y -3.15..4.69, z -6.76..6.20.
        static const float kLo[3] = { -3.97f, -3.15f, -6.76f };
        static const float kHi[3] = { 9.75f, 4.69f, 6.20f };
        BMVR_TRY
        {
            float* m = bones + s_bag * 12;
            const float L[3][3] = { { lf.x, lr.x, lu.x }, { lf.y, lr.y, lu.y }, { lf.z, lr.z, lu.z } };
            const float R[3][3] = { { rf.x, rr.x, ru.x }, { rf.y, rr.y, ru.y }, { rf.z, rr.z, ru.z } };
            float Q[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    Q[i][j] = L[i][0] * R[j][0] + L[i][1] * R[j][1] + L[i][2] * R[j][2];
            float rot[3][3];
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    rot[r][col] = Q[r][0] * m[col] + Q[r][1] * m[4 + col] + Q[r][2] * m[8 + col];
            // Top of the bag: the bone axis most along the left controller's up.
            int axis = 0;
            float best = 0.f;
            for (int k = 0; k < 3; ++k)
            {
                const float dot = rot[0][k] * lu.x + rot[1][k] * lu.y + rot[2][k] * lu.z;
                if (std::fabs(dot) > std::fabs(best))
                {
                    best = dot;
                    axis = k;
                }
            }
            float top[3];
            for (int k = 0; k < 3; ++k)
                top[k] = 0.5f * (kLo[k] + kHi[k]);
            top[axis] = best >= 0.f ? kHi[axis] : kLo[axis];
            // In the fist (user 2026-11-19: the closed fist sat above the
            // bag's corner): the top edge lies in the curl of the fingers
            // (glove fist tunnel), turned about the bag's up axis so its long
            // top edge runs along the knuckle line, a little into the fist.
            Vector hold = fist;
            if (const unsigned tunMs = MagReload::g_LeftTunnelMs.load(std::memory_order_relaxed);
                tunMs != 0 && now - tunMs < 500)
            {
                float T[6];
                for (int k = 0; k < 6; ++k)
                    T[k] = MagReload::g_LeftTunnelLocal[k].load(std::memory_order_relaxed);
                hold = lp + lf * T[0] + lr * T[1] + lu * T[2];
                Vector knuckle = lf * T[3] + lr * T[4] + lu * T[5];
                // Bag up (world) and its long top edge: the bigger of the two other axes.
                Vector up(rot[0][axis], rot[1][axis], rot[2][axis]);
                if (best < 0.f)
                    up = up * -1.f;
                const float upLen = VectorNormalize(up);
                int edge = -1;
                float edgeLen = 0.f;
                for (int k = 0; k < 3; ++k)
                    if (k != axis && kHi[k] - kLo[k] > edgeLen)
                    {
                        edgeLen = kHi[k] - kLo[k];
                        edge = k;
                    }
                knuckle = knuckle - up * knuckle.Dot(up);
                Vector e(rot[0][edge], rot[1][edge], rot[2][edge]);
                e = e - up * e.Dot(up);
                if (upLen > 1e-4f && edge >= 0 && VectorNormalize(knuckle) > 1e-3f && VectorNormalize(e) > 1e-3f)
                {
                    // Smallest turn about up taking the edge onto the knuckle line (either way round).
                    if (e.Dot(knuckle) < 0.f)
                        knuckle = knuckle * -1.f;
                    const float c = std::clamp(e.Dot(knuckle), -1.f, 1.f);
                    const float s = CrossProduct(e, knuckle).Dot(up);
                    const float ang = std::atan2(s, c);
                    const float ca = std::cos(ang), sa = std::sin(ang);
                    const float K[3] = { up.x, up.y, up.z };
                    float Rm[3][3];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                        {
                            const float cross = (i == 0 ? (j == 1 ? -K[2] : j == 2 ? K[1] : 0.f)
                                : i == 1 ? (j == 0 ? K[2] : j == 2 ? -K[0] : 0.f)
                                         : (j == 0 ? -K[1] : j == 1 ? K[0] : 0.f));
                            Rm[i][j] = (i == j ? ca : 0.f) + sa * cross + (1.f - ca) * K[i] * K[j];
                        }
                    float nr[3][3];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            nr[i][j] = Rm[i][0] * rot[0][j] + Rm[i][1] * rot[1][j] + Rm[i][2] * rot[2][j];
                    std::memcpy(rot, nr, sizeof(rot));
                }
                // The top is ~5 units thick at the viewmodel scale, more than a
                // fist closes round (user 2026-11-19: still hung below the
                // hand): grip the top edge on the back-of-hand side, so the
                // bag body hangs on the palm side (+right for the left hand).
                if (edge >= 0)
                {
                    const int thick = 3 - axis - edge;
                    const float side = rot[0][thick] * lr.x + rot[1][thick] * lr.y + rot[2][thick] * lr.z;
                    top[thick] = side > 0.f ? kLo[thick] : kHi[thick];
                }
                // Top edge at the fingers' curl, raised bmvr_satchel_bag_depth
                // (units at scale 1) further into the fist.
                const float sc = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
                hold = hold + up * (bmvr::g_SatchelBagDepth * (sc > 0.05f ? sc : 1.f));
            }
            // Hand-tuned on top (user 2026-11-19 screenshot: bag upright in
            // front of the knuckles): turned about the left controller's right
            // axis by bmvr_satchel_bag_turn (+90 = its top back toward the
            // fist, the bag along the knuckle direction), then moved in toward
            // the fist and up (cm).
            // Turn (about the left controller's right axis: tips toward / away
            // from the player), roll (about its forward axis) and spin (about
            // its up axis), in that order.
            auto rotateBag = [&](const Vector& axis, float deg) {
                if (deg == 0.f)
                    return;
                const float a = deg * (3.14159265f / 180.f);
                const float ca = std::cos(a), sa = std::sin(a);
                const float K[3] = { axis.x, axis.y, axis.z };
                float Rm[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                    {
                        const float cross = (i == 0 ? (j == 1 ? -K[2] : j == 2 ? K[1] : 0.f)
                            : i == 1 ? (j == 0 ? K[2] : j == 2 ? -K[0] : 0.f)
                                     : (j == 0 ? -K[1] : j == 1 ? K[0] : 0.f));
                        Rm[i][j] = (i == j ? ca : 0.f) + sa * cross + (1.f - ca) * K[i] * K[j];
                    }
                float nr[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        nr[i][j] = Rm[i][0] * rot[0][j] + Rm[i][1] * rot[1][j] + Rm[i][2] * rot[2][j];
                std::memcpy(rot, nr, sizeof(rot));
            };
            rotateBag(lr, bmvr::g_SatchelBagTurn);
            rotateBag(lf, bmvr::g_SatchelBagRoll);
            rotateBag(lu, bmvr::g_SatchelBagSpin);
            {
                const float ucm = (vr->m_VRScale > 1.f ? vr->m_VRScale : 39.37f) * 0.01f;
                hold = hold - lf * (bmvr::g_SatchelBagInCm * ucm) + lu * (bmvr::g_SatchelBagUpCm * ucm)
                    + lr * (bmvr::g_SatchelBagSideCm * ucm);
            }
            // Bone origin so that rot * top + origin = the hold point.
            const float t[3] = { hold.x, hold.y, hold.z };
            for (int r = 0; r < 3; ++r)
            {
                for (int col = 0; col < 3; ++col)
                    m[r * 4 + col] = rot[r][col];
                m[r * 4 + 3] = t[r] - (rot[r][0] * top[0] + rot[r][1] * top[1] + rot[r][2] * top[2]);
            }
            static int s_log;
            if (s_log++ < 2)
                Game::logMsg("Satchel bag: top along bone axis %d (%s), hung from the left fist", axis, best >= 0.f ? "+" : "-");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        MagReload::g_SatchelBagMs.store(now ? now : 1, std::memory_order_relaxed);
    }

    // Satchel bag viewmodel playing its stock 'throw' clip (VrSatchelHands:
    // the left hand throws, the clip is not shown).
    bool SatchelThrowClipShowing(void* state)
    {
        if (!Hooks::m_Game)
            return false;
        unsigned char* hdr = ResolveStudioHdr(state);
        void* vm = Hooks::m_Game->GetViewModelEntity();
        if (!hdr || !vm)
            return false;
        int seq = -1;
        BMVR_TRY { seq = *reinterpret_cast<int*>(static_cast<char*>(vm) + kViewmodelSequence); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        const char* label = seq >= 0 ? StudioSequenceLabel(hdr, seq) : nullptr;
        return label && _stricmp(label, "throw") == 0;
    }

    // Diagnostic: the drawn own right fist (R_Wrist + R_Middle1-3 average, as
    // ShrinkViewmodelRightHand) relative to the right controller, in the
    // controller's forward/right/up, cm. Once per viewmodel header, so the
    // guns and the crowbar can be compared at one controller pose.
    void LogGunFistVsController(VR* vr, unsigned char* hdr, const float* bones, int nBones, const Vector& ctrl,
        const char* modelName)
    {
        static unsigned char* s_hdrs[16]{};
        static int s_n = 0;
        static unsigned char* s_cur = nullptr;
        static int s_draws = 0;
        if (!vr || !hdr || !bones || s_n >= 16)
            return;
        for (int i = 0; i < s_n; ++i)
            if (s_hdrs[i] == hdr)
                return;
        // Settled pose only: skip the switch frames (old pose / draw clip).
        if (hdr != s_cur)
        {
            s_cur = hdr;
            s_draws = 0;
        }
        if (++s_draws < 120)
            return;
        const char* kCentre[4] = { "R_Wrist", "R_Middle1", "R_Middle2", "R_Middle3" };
        Vector c(0.f, 0.f, 0.f);
        for (const char* n : kCentre)
        {
            const int b = FindStudioBoneIndex(hdr, n);
            if (b < 0 || b >= nBones)
                return;
            const float* m = bones + b * 12;
            c += Vector(m[3], m[7], m[11]) * 0.25f;
        }
        s_hdrs[s_n++] = hdr;
        Vector f, r, u;
        QAngle::AngleVectors(vr->GetRightControllerAbsAngle(), &f, &r, &u);
        const float scale = vr->m_VRScale > 1.f ? vr->m_VRScale : 39.37f;
        const Vector d = (c - ctrl) * (100.f / scale);
        Game::logMsg("Gun right hand: fist vs controller fwd %.1f right %.1f up %.1f cm (%s)",
            d.Dot(f), d.Dot(r), d.Dot(u), modelName ? modelName : "?");
    }

    // The drawn crowbar bar (bone crowbar_new, one rigid 845-vertex mesh)
    // for VR::ComputePhysicalCrowbarPose: the shaft from the hand to the far
    // end, in the right controller's forward/right/up. Bar centreline in the
    // bone frame measured from v_crowbar (VVD through poseToBone): z -13.5 ..
    // 13.5 at y ~1.5.
    void NoteDrawnCrowbarBar(VR* vr, unsigned char* hdr, const float* bones, int nBones, const Vector& ctrl)
    {
        static unsigned char* s_hdr = nullptr;
        static int s_bar = -1;
        if (!vr || !hdr || !bones)
            return;
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_bar = FindStudioBoneIndex(hdr, "crowbar_new");
            Game::logMsg("Crowbar hit bar: bone crowbar_new %d", s_bar);
        }
        if (s_bar < 0 || s_bar >= nBones)
            return;
        const float* m = bones + s_bar * 12;
        auto pt = [&](float x, float y, float z) {
            return Vector(m[0] * x + m[1] * y + m[2] * z + m[3],
                m[4] * x + m[5] * y + m[6] * z + m[7],
                m[8] * x + m[9] * y + m[10] * z + m[11]);
        };
        const Vector a = pt(0.f, 1.5f, -13.5f);
        const Vector b = pt(0.f, 1.5f, 13.5f);
        // Hand end = the end nearer the controller; the sweep starts at the
        // controller projected on the bar so it does not run through the hand.
        const bool aNear = (a - ctrl).LengthSqr() < (b - ctrl).LengthSqr();
        const Vector nearEnd = aNear ? a : b;
        const Vector farEnd = aNear ? b : a;
        Vector along = farEnd - nearEnd;
        const float len = VectorNormalize(along);
        if (len < 1.f)
            return;
        const float t = std::clamp((ctrl - nearEnd).Dot(along), 0.f, len);
        const Vector start = nearEnd + along * t;
        Vector f, r, u;
        QAngle::AngleVectors(vr->GetRightControllerAbsAngle(), &f, &r, &u);
        const Vector ds = start - ctrl, dt = farEnd - ctrl;
        vr->NoteDrawnCrowbarBar(Vector(ds.Dot(f), ds.Dot(r), ds.Dot(u)), Vector(dt.Dot(f), dt.Dot(r), dt.Dot(u)));
        static int s_log = 0;
        if (s_log < 3 && ++s_log)
            Game::logMsg("Crowbar hit bar: start %.1f/%.1f/%.1f tip %.1f/%.1f/%.1f (controller fwd/right/up, units)",
                ds.Dot(f), ds.Dot(r), ds.Dot(u), dt.Dot(f), dt.Dot(r), dt.Dot(u));
    }

    // Support hand from the gun (VrSupportHandLock): set by the draw before
    // the arm collapse, read by the pump code in the same draw.
    thread_local bool t_KeepLeftHand = false;
    thread_local int t_LeftHandBones[24]{};
    thread_local int t_LeftHandCount = 0;
    // Left forearm/arm bones folded into the kept L_Wrist (keepLeftHand).
    thread_local int t_LeftArmBones[24]{};
    thread_local int t_LeftArmCount = 0;
    // VrClipHandSnap: reload-clip pose (ClipHandPose::Id) the gun's own left
    // hand takes on the bolt/slide in this draw, -1 = none.
    thread_local int t_SnapClipPose = -1;

    // Guns whose stock idle holds the left hand on the gun (headless dumps
    // 2026-11-18: MP5 magazine, shotgun pump, crossbow forestock, RPG front
    // grip, glock/.357 cupping the grip). The tau shows no stock left hand.
    bool SupportHandFromGunModel(const char* modelName)
    {
        if (!modelName)
            return false;
        static const char* kGuns[] = { "v_mp5", "v_shotgun", "v_crossbow", "v_rpg", "v_glock", "v_357" };
        char buf[128]{};
        strncpy_s(buf, modelName, _TRUNCATE);
        for (char* c = buf; *c; ++c)
            *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        for (const char* g : kGuns)
            if (std::strstr(buf, g))
                return true;
        return false;
    }

    bool CollapseViewmodelArmBones(unsigned char* hdr, float* bones, int nBones, bool keepLeftHand = false)
    {
        struct Cache
        {
            unsigned char* hdr = nullptr;
            int wrist = -1;
            int lwrist = -1;
            int rforearm = -1;
            int count = 0;
            int idx[48]{};
            bool leftHand[48]{}; // L_Wrist + fingers (kept with keepLeftHand)
            bool leftArm[48]{};  // other left bones (folded into L_Wrist then)
        };
        static thread_local Cache c;
        if (!hdr || !bones || nBones <= 0)
            return false;
        if (c.hdr != hdr)
        {
            c = Cache{};
            c.hdr = hdr;
            c.wrist = FindStudioBoneIndex(hdr, "R_Wrist");
            c.lwrist = FindStudioBoneIndex(hdr, "L_Wrist");
            int length = 0, numbones = 0, boneindex = 0;
            BMVR_TRY
            {
                length = *reinterpret_cast<int*>(hdr + kStudioHdrLength);
                numbones = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones);
                boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                numbones = 0;
            }
            if (c.wrist >= 0 && numbones > 0 && numbones <= kMaxStudioBones
                && boneindex >= 0 && boneindex + numbones * kStudioBoneSize <= length)
            {
                for (int i = 0; i < numbones && c.count < 48; ++i)
                {
                    unsigned char* bone = hdr + boneindex + i * kStudioBoneSize;
                    int szname = 0;
                    BMVR_TRY { szname = *reinterpret_cast<int*>(bone); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                    const char* n = StudioRelString(bone, szname);
                    const bool left = _strnicmp(n, "L_", 2) == 0
                        || _stricmp(n, "SCI_Hand_Forearm_L") == 0 || _stricmp(n, "gman_arms_l") == 0;
                    const bool rightArm = _stricmp(n, "R_Arm") == 0 || _stricmp(n, "R_Forearm") == 0
                        || _stricmp(n, "SCI_Hand_Forearm_R") == 0 || _stricmp(n, "gman_arms_r") == 0;
                    if (_stricmp(n, "R_Forearm") == 0)
                        c.rforearm = i;
                    if (left || rightArm)
                    {
                        const bool hand = _stricmp(n, "L_Wrist") == 0 || _strnicmp(n, "L_Thumb", 7) == 0
                            || _strnicmp(n, "L_Index", 7) == 0 || _strnicmp(n, "L_Middle", 8) == 0
                            || _strnicmp(n, "L_Ring", 6) == 0 || _strnicmp(n, "L_Pinky", 7) == 0;
                        c.leftHand[c.count] = left && hand;
                        c.leftArm[c.count] = left && !hand;
                        c.idx[c.count++] = i;
                    }
                }
            }
            static int s_log;
            if (s_log < 6)
            {
                Game::logMsg("Gun right hand: wrist=%d collapsed bones=%d", c.wrist, c.count);
                ++s_log;
            }
        }
        if (c.wrist < 0 || c.wrist >= nBones || c.count == 0)
            return false;
        const bool keepLeft = keepLeftHand && c.lwrist >= 0 && c.lwrist < nBones;
        t_LeftHandCount = 0;
        t_LeftArmCount = 0;
        // Right forearm/arm fold to a point a little behind the wrist (along
        // the forearm toward the elbow), not into the wrist origin inside the
        // hand: the sleeve then closes as a short cone over the cuff instead
        // of folding into the hand and leaving the wrist open from behind
        // (user screenshot 2026-11-19, glock). bmvr_gun_hand_cuff units.
        const int forearm = c.rforearm < nBones ? c.rforearm : -1;
        BMVR_TRY
        {
            const float* w = bones + c.wrist * 12;
            float t[3] = { w[3], w[7], w[11] };
            if (forearm >= 0 && bmvr::g_GunHandCuff > 0.f)
            {
                const float* fa = bones + forearm * 12;
                float d[3] = { fa[3] - t[0], fa[7] - t[1], fa[11] - t[2] };
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len > 1e-3f)
                {
                    // Bones are already scaled: the wrist matrix's column
                    // length is the draw scale.
                    const float sc = std::sqrt(w[0] * w[0] + w[4] * w[4] + w[8] * w[8]);
                    const float back = (std::min)(bmvr::g_GunHandCuff * (sc > 0.05f ? sc : 1.f), 0.5f * len);
                    for (int r = 0; r < 3; ++r)
                        t[r] += d[r] / len * back;
                }
            }
            const float* lw = keepLeft ? bones + c.lwrist * 12 : w;
            const float lt[3] = { lw[3], lw[7], lw[11] };
            for (int k = 0; k < c.count; ++k)
            {
                if (c.idx[k] >= nBones)
                    continue;
                if (keepLeft && c.leftHand[k])
                {
                    if (t_LeftHandCount < 24)
                        t_LeftHandBones[t_LeftHandCount++] = c.idx[k];
                    continue;
                }
                // Left forearm folds into the kept left wrist (as the right
                // forearm folds into the right one).
                if (keepLeft && c.leftArm[k] && t_LeftArmCount < 24)
                    t_LeftArmBones[t_LeftArmCount++] = c.idx[k];
                const float* to = (keepLeft && c.leftArm[k]) ? lt : t;
                float* m = bones + c.idx[k] * 12;
                for (int r = 0; r < 3; ++r)
                {
                    m[r * 4 + 0] = m[r * 4 + 1] = m[r * 4 + 2] = 0.f;
                    m[r * 4 + 3] = to[r];
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool InvertMatrix3x4General(const float in[3][4], float out[3][4])
    {
        const float a = in[0][0], b = in[0][1], c = in[0][2];
        const float d = in[1][0], e = in[1][1], f = in[1][2];
        const float g = in[2][0], h = in[2][1], i = in[2][2];
        const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        const float det = a * A + b * B + c * C;
        if (fabsf(det) < 1e-12f)
            return false;
        const float k = 1.f / det;
        out[0][0] = A * k; out[0][1] = -(b * i - c * h) * k; out[0][2] = (b * f - c * e) * k;
        out[1][0] = B * k; out[1][1] = (a * i - c * g) * k; out[1][2] = -(a * f - c * d) * k;
        out[2][0] = C * k; out[2][1] = -(a * h - b * g) * k; out[2][2] = (a * e - b * d) * k;
        for (int r = 0; r < 3; ++r)
            out[r][3] = -(out[r][0] * in[0][3] + out[r][1] * in[1][3] + out[r][2] * in[2][3]);
        return true;
    }

    // VrStockHandPose: the stock left hand's finger joints relative to
    // L_Wrist while the gun idles (settled, not scrubbed), for the support
    // glove (vr_hands.cpp maps them through the glove's source bone frames).
    // Read from the drawn copy before the arm collapse: viewmodel scale and
    // the Y fix act on every bone alike, so they cancel in wrist^-1 * bone.
    void SampleStockOffhandFingers(unsigned char* hdr, const float* bones12, int nBones)
    {
        if (!bmvr::g_VrStockHandPose || !bmvr::g_VrHandsBlackMesa || bmvr::g_LeftHanded || !Hooks::m_VR
            || !Hooks::m_VR->m_VmClass.twoHand || !hdr || !bones12)
            return;
        if (MagReload::g_ScrubActive.load(std::memory_order_relaxed)
            || !Hooks::m_VR->StockOffhandFingersIdleSettled())
            return;
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_idx[16]{};
        static thread_local bool s_ok = false;
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            static const char* kFinger[5] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
            s_idx[0] = FindStudioBoneIndex(hdr, "L_Wrist");
            s_ok = s_idx[0] >= 0 && s_idx[0] < nBones;
            for (int f = 0; f < 5 && s_ok; ++f)
                for (int seg = 0; seg < 3 && s_ok; ++seg)
                {
                    char name[24];
                    std::snprintf(name, sizeof(name), "L_%s%d", kFinger[f], seg + 1);
                    const int i = FindStudioBoneIndex(hdr, name);
                    s_idx[1 + f * 3 + seg] = i;
                    s_ok = i >= 0 && i < nBones;
                }
            if (!s_ok)
                Game::logMsg("Stock hand pose: left finger bones missing on this viewmodel");
        }
        if (!s_ok)
            return;
        const auto* all = reinterpret_cast<const float (*)[3][4]>(bones12);
        float wristInv[3][4];
        if (!InvertMatrix3x4General(all[s_idx[0]], wristInv))
            return;
        float rel[15][12];
        for (int j = 0; j < 15; ++j)
            MulMatrix3x4(wristInv, all[s_idx[1 + j]], reinterpret_cast<float (*)[4]>(rel[j]));
        Hooks::m_VR->NoteStockOffhandFingers(hdr, rel);
    }

    // Debug (bmvr_dump_hand_pose + bmvr_debug_vm_seq): the stock left hand
    // at the held clip frame, logged once per model/sequence/cycle as
    // inv(L_Wrist) * bone for the object bones (magazine, bolt, slide,
    // shell, loader) and the 15 finger joints. Raw game bones (before our
    // scale/Y fix); the rows are the 3x4 matrix, model units.
    void DumpClipHandPose(unsigned char* hdr, const void* raw, int nBones)
    {
        if (!bmvr::g_DebugDumpHandPose || bmvr::g_DebugVmSeq < 0.f || !hdr || !raw || nBones <= 0)
            return;
        static unsigned char* s_hdr = nullptr;
        static int s_seq = -1;
        static float s_cycle = -1.f;
        const int seq = static_cast<int>(bmvr::g_DebugVmSeq + 0.5f);
        const float cycle = bmvr::g_DebugVmCycle;
        if (hdr == s_hdr && seq == s_seq && cycle == s_cycle)
            return;
        s_hdr = hdr;
        s_seq = seq;
        s_cycle = cycle;
        const auto* all = reinterpret_cast<const float (*)[3][4]>(raw);
        const int lw = FindStudioBoneIndex(hdr, "L_Wrist");
        const char* label = StudioSequenceLabel(hdr, seq);
        if (lw < 0 || lw >= nBones)
        {
            Game::logMsg("Hand pose dump: seq %d (%s) cycle %.3f: no L_Wrist", seq, label ? label : "?", cycle);
            return;
        }
        float inv[3][4];
        if (!InvertMatrix3x4General(all[lw], inv))
            return;
        Game::logMsg("Hand pose dump: seq %d (%s) cycle %.3f", seq, label ? label : "?", cycle);
        auto logRel = [&](const char* name)
        {
            const int i = FindStudioBoneIndex(hdr, name);
            if (i < 0 || i >= nBones)
                return;
            float r[3][4];
            MulMatrix3x4(inv, all[i], r);
            Game::logMsg("Hand pose dump:   %-16s %8.4f %8.4f %8.4f %8.3f | %8.4f %8.4f %8.4f %8.3f | %8.4f %8.4f %8.4f %8.3f",
                name, r[0][0], r[0][1], r[0][2], r[0][3], r[1][0], r[1][1], r[1][2], r[1][3],
                r[2][0], r[2][1], r[2][2], r[2][3]);
        };
        static const char* kObjects[] = { "Clip_New", "Bolt_New", "bone_clip", "Bone01", "bone_shell", "bone_pump",
            "clip_export_bone", "speed_loader", "Chamber", "RPG_Projectile", "Grenade_Pin2", "Grenade_Pin1", "Grenad_Pin1",
            "R_Wrist" };
        for (const char* n : kObjects)
            logRel(n);
        static const char* kFinger[5] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
        for (int f = 0; f < 5; ++f)
            for (int seg = 1; seg <= 3; ++seg)
            {
                char name[24];
                std::snprintf(name, sizeof(name), "L_%s%d", kFinger[f], seg);
                logRel(name);
            }
    }

    // While a stock clip is scrubbed by the hands, the clip also swings the
    // whole gun. Hold the gun body bone at the pose it had in idle (relative
    // to the draw's gun frame) and move every bone by the same correction, so
    // only the parts that move relative to the body animate.
    void StabilizeScrubbedBody(unsigned char* hdr, float* bones, int nBones, const Vector& gunOrigin, const QAngle& gunAng)
    {
        const int weapon = MagReload::g_Weapon.load(std::memory_order_relaxed);
        const char* bodyName = MagReload::Def(weapon).bodyBone;
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_weapon = -1;
        static thread_local int s_body = -1;
        static thread_local float s_idleLocal[3][4]{};
        static thread_local bool s_idleValid = false;
        if (s_hdr != hdr || s_weapon != weapon)
        {
            s_hdr = hdr;
            s_weapon = weapon;
            s_body = bodyName ? FindStudioBoneIndex(hdr, bodyName) : -1;
            s_idleValid = false;
            Game::logMsg("Scrub: body bone '%s' = %d", bodyName ? bodyName : "-", s_body);
        }
        {
            static thread_local int s_lastState = -1;
            const int state = (MagReload::g_ScrubActive.load() ? 2 : 0) + (s_idleValid ? 1 : 0);
            static thread_local int s_stateLogs = 0;
            if (state != s_lastState && s_stateLogs < 40)
            {
                ++s_stateLogs;
                s_lastState = state;
                Game::logMsg("Scrub: active=%d idleCaptured=%d body=%d", state >> 1, state & 1, s_body);
            }
        }
        if (s_body < 0 || s_body >= nBones)
            return;
        float gf[3][4], gfInv[3][4];
        BuildMatrix3x4FromOrgAngles(gf, gunOrigin, gunAng);
        InvertMatrix3x4TR(gf, gfInv);
        float (*all)[3][4] = reinterpret_cast<float (*)[3][4]>(bones);
        float* body = &all[s_body][0][0];
        // Right-arm bones (the stock hand on the grip): held at their idle
        // pose relative to the body too; the clips move that hand off the
        // grip for the parts the left hand does in VR.
        static thread_local int s_armCount = 0;
        static thread_local int s_arm[40]{};
        static thread_local float s_armRel[40][3][4]{};
        static thread_local unsigned char* s_armHdr = nullptr;
        if (s_armHdr != hdr)
        {
            s_armHdr = hdr;
            s_armCount = 0;
            int numbones = 0, boneindex = 0;
            BMVR_TRY
            {
                numbones = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones);
                boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { numbones = 0; }
            for (int i = 0; i < numbones && i < nBones && s_armCount < 40; ++i)
            {
                unsigned char* bone = hdr + boneindex + i * kStudioBoneSize;
                int szname = 0;
                BMVR_TRY { szname = *reinterpret_cast<int*>(bone); }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                const char* n = StudioRelString(bone, szname);
                if (_strnicmp(n, "R_", 2) == 0 || _stricmp(n, "SCI_Hand_Forearm_R") == 0 || _stricmp(n, "gman_arms_r") == 0)
                    s_arm[s_armCount++] = i;
            }
        }
        if (!MagReload::g_ScrubActive.load(std::memory_order_relaxed))
        {
            // Idle pose only (not mid-fire / mid-draw).
            bool idle = false;
            if (Hooks::m_Game)
            {
                void* vm = Hooks::m_Game->GetViewModelEntity();
                int seq = -1;
                BMVR_TRY { if (vm) seq = *reinterpret_cast<int*>(static_cast<char*>(vm) + kViewmodelSequence); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                const char* label = seq >= 0 ? StudioSequenceLabel(hdr, seq) : nullptr;
                char lb[64]{};
                for (int i = 0; label && i < 63 && label[i]; ++i)
                    lb[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
                idle = std::strstr(lb, "idle") && !std::strstr(lb, "low") && !std::strstr(lb, "sprint");
            }
            if (idle)
            {
                MulMatrix3x4(gfInv, all[s_body], s_idleLocal);
                float bodyInv[3][4];
                if (InvertMatrix3x4General(all[s_body], bodyInv))
                {
                    for (int k = 0; k < s_armCount; ++k)
                        MulMatrix3x4(bodyInv, all[s_arm[k]], s_armRel[k]);
                    s_idleValid = true;
                }
            }
            return;
        }
        if (!s_idleValid)
            return;
        float target[3][4], cur[3][4], curInv[3][4], delta[3][4];
        MulMatrix3x4(gf, s_idleLocal, target);
        std::memcpy(cur, body, sizeof(cur));
        if (!InvertMatrix3x4General(cur, curInv))
            return;
        MulMatrix3x4(target, curInv, delta);
        for (int b = 0; b < nBones; ++b)
            MulMatrix3x4(delta, all[b], all[b]);
        for (int k = 0; k < s_armCount; ++k)
            MulMatrix3x4(all[s_body], s_armRel[k], all[s_arm[k]]);
    }

    // MP5 grenade launch (user 2026-11-19): the stock 'grenade' clip kicks the
    // gun (kept) and moves the gun's own left hand from the grip to the
    // trigger on the stock after the grenade has left. The left-arm bones are
    // held at their idle pose relative to the gun body (the root bone), so the
    // hand rides the kick on the grip instead. Idle frames refresh the
    // relative pose; only the MP5 pays for it (~25 matrix products a frame).
    void HoldLeftHandOnMp5Body(unsigned char* hdr, float* bones, int nBones)
    {
        if (!hdr || !bones || nBones <= 0 || !Hooks::m_Game || !Hooks::m_VR || !Hooks::m_VR->ViewmodelIsMp5())
            return;
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_root = -1;
        static thread_local int s_count = 0;
        static thread_local int s_arm[40]{};
        static thread_local float s_rel[40][3][4]{};
        static thread_local bool s_valid = false;
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_valid = false;
            s_count = 0;
            float rest[3]{};
            char rootName[64]{};
            s_root = FindWeaponRootBone(hdr, rest, rootName);
            int numbones = 0, boneindex = 0;
            BMVR_TRY
            {
                numbones = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones);
                boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { numbones = 0; }
            for (int i = 0; i < numbones && i < nBones && s_count < 40; ++i)
            {
                unsigned char* bone = hdr + boneindex + i * kStudioBoneSize;
                int szname = 0;
                BMVR_TRY { szname = *reinterpret_cast<int*>(bone); }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                const char* n = StudioRelString(bone, szname);
                if (_strnicmp(n, "L_", 2) == 0 || _stricmp(n, "SCI_Hand_Forearm_L") == 0 || _stricmp(n, "gman_arms_l") == 0)
                    s_arm[s_count++] = i;
            }
            Game::logMsg("MP5 grenade hold: root bone '%s' = %d, %d left-arm bones", rootName, s_root, s_count);
        }
        if (s_root < 0 || s_root >= nBones || s_count == 0)
            return;
        void* vm = Hooks::m_Game->GetViewModelEntity();
        int seq = -1;
        BMVR_TRY { if (vm) seq = *reinterpret_cast<int*>(static_cast<char*>(vm) + kViewmodelSequence); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        const char* label = seq >= 0 ? StudioSequenceLabel(hdr, seq) : nullptr;
        if (!label)
            return;
        float (*all)[3][4] = reinterpret_cast<float (*)[3][4]>(bones);
        if (_stricmp(label, "grenade") == 0)
        {
            if (!s_valid)
                return;
            for (int k = 0; k < s_count; ++k)
                MulMatrix3x4(all[s_root], s_rel[k], all[s_arm[k]]);
            static int s_log;
            if (s_log++ < 3)
                Game::logMsg("MP5 grenade hold: left hand kept on the grip during the clip");
            return;
        }
        // Plain idle only (the fidget moves the hands; lowered / sprint move the gun).
        char lb[32]{};
        for (int i = 0; i < 31 && label[i]; ++i)
            lb[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
        if (!std::strstr(lb, "idle") || std::strstr(lb, "fidget") || std::strstr(lb, "low") || std::strstr(lb, "sprint"))
            return;
        float rootInv[3][4];
        if (!InvertMatrix3x4General(all[s_root], rootInv))
            return;
        for (int k = 0; k < s_count; ++k)
            MulMatrix3x4(rootInv, all[s_arm[k]], s_rel[k]);
        s_valid = true;
    }

    // Manual magazines (VrMagReload, vr_mag_reload.h). Works on the copied
    // bone matrices of the gun's own viewmodel: the magazine bone is hidden,
    // dropped, or placed at the left hand (same orientation to the left
    // controller as it has to the gun); the bolt/slide bone slides back along
    // the gun while the left hand pulls it, or stays back when locked open.
    // Bones parented to the magazine (the crossbow's bolts in it) go where it
    // goes: they were left floating under the gun when it was ejected (user).
    int FindMagChildBones(unsigned char* hdr, int mag, int* out, int maxOut)
    {
        int n = 0, length = 0, numbones = 0, boneindex = 0;
        BMVR_TRY
        {
            length = *reinterpret_cast<int*>(hdr + kStudioHdrLength);
            numbones = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones);
            boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
            if (numbones <= 0 || numbones > kMaxStudioBones || boneindex < 0
                || boneindex + numbones * kStudioBoneSize > length)
                return 0;
            for (int i = 0; i < numbones && n < maxOut; ++i)
            {
                int p = i, guard = 0;
                while (p >= 0 && p != mag && guard++ < 64)
                    p = *reinterpret_cast<int*>(hdr + boneindex + p * kStudioBoneSize + 4);
                if (p == mag && i != mag)
                    out[n++] = i;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return n;
    }

    void ApplyMagReloadBonesInner(VR* vr, unsigned char* hdr, float* bones, int nBones,
        const Vector& gunOrigin, const QAngle& gunAng, float vmScale);

    // .357 extras after the loader is placed (user, 2026-11-18):
    // - the speed loader (bind pose ~11 units under the gun) is hidden unless
    //   it is in the hand, falling, or going in (stock clip, cycle >= 0.6):
    //   the idle pose left an empty loader hanging under the revolver;
    // - the bullets ride in the cylinder while it swings out, then fall out
    //   of it once it is open (the frozen stock clip had them where the
    //   stock left hand was), and are gone until the loader brings new ones;
    // - in the hand / falling, the loader carries bullets (their place in the
    //   loader is sampled from the stock clip the first time it goes in).
    void Apply357Extras(unsigned char* hdr, float* bones, int nBones, const QAngle& gunAng, float vmScale)
    {
        if (MagReload::g_Weapon.load(std::memory_order_relaxed) != MagReload::kWeapon357 || !hdr || !bones)
            return;
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_loader = -1, s_chamber = -1, s_bullet[6]{};
        static thread_local bool s_ok = false;
        static thread_local float s_inCyl[6][3][4]{};   // bullet relative to Chamber, idle
        static thread_local bool s_inCylValid = false;
        static thread_local float s_inLoader[6][3][4]{}; // bullet relative to speed_loader, stock insert
        static thread_local bool s_inLoaderValid = false;
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_loader = FindStudioBoneIndex(hdr, "speed_loader");
            s_chamber = FindStudioBoneIndex(hdr, "Chamber");
            s_ok = s_loader >= 0 && s_loader < nBones && s_chamber >= 0 && s_chamber < nBones;
            for (int i = 0; i < 6; ++i)
            {
                char n[16];
                std::snprintf(n, sizeof(n), "Bullet%d", i + 1);
                s_bullet[i] = FindStudioBoneIndex(hdr, n);
                s_ok = s_ok && s_bullet[i] >= 0 && s_bullet[i] < nBones;
            }
            s_inCylValid = false;
            Game::logMsg("Mag reload: 357 extras loader %d chamber %d bullets %d..%d ok=%d",
                s_loader, s_chamber, s_bullet[0], s_bullet[5], s_ok ? 1 : 0);
        }
        if (!s_ok)
            return;
        float (*all)[3][4] = reinterpret_cast<float (*)[3][4]>(bones);
        auto hide = [&](int b) {
            for (int r = 0; r < 3; ++r)
                all[b][r][0] = all[b][r][1] = all[b][r][2] = 0.f;
        };
        auto relTo = [&](int parent, int child, float out[3][4]) {
            float inv[3][4];
            if (!InvertMatrix3x4General(all[parent], inv))
                return false;
            MulMatrix3x4(inv, all[child], out);
            return true;
        };
        const DWORD now = GetTickCount();
        const int st = MagReload::g_State.load(std::memory_order_relaxed);
        const bool scrubbing = MagReload::g_ScrubSeq.load(std::memory_order_relaxed) != nullptr;
        const float cycle = MagReload::g_ScrubCycle.load(std::memory_order_relaxed);
        const bool inserting = scrubbing && cycle >= 0.6f;
        const unsigned dropTick = MagReload::g_DropTick.load(std::memory_order_relaxed);
        const bool loaderFalling = MagReload::g_DropFrom.load() == 1 && dropTick && now - dropTick < 1200;
        const bool loaderOurs = st == MagReload::kInHand || (st == MagReload::kEmpty && !inserting && loaderFalling);

        if (!scrubbing && (st == MagReload::kLoaded || st == MagReload::kDry))
        {
            // Idle: bullets in the cylinder as the gun shows them.
            bool ok = true;
            for (int i = 0; i < 6 && ok; ++i)
                ok = relTo(s_chamber, s_bullet[i], s_inCyl[i]);
            s_inCylValid = ok;
            hide(s_loader);
            return;
        }
        if (inserting)
        {
            // The stock clip's loader carries the new bullets in.
            if (!s_inLoaderValid && cycle < 0.66f)
            {
                bool ok = true;
                for (int i = 0; i < 6 && ok; ++i)
                    ok = relTo(s_loader, s_bullet[i], s_inLoader[i]);
                s_inLoaderValid = ok;
                Game::logMsg("Mag reload: 357 bullets in the loader sampled (%d)", ok ? 1 : 0);
            }
            // Rounds in by 0.68; the stock loader then leaves with the stock
            // left hand, which would hang there while the cylinder waits
            // open for the flick.
            if (cycle >= 0.70f)
                hide(s_loader);
            return;
        }
        if (!loaderOurs)
            hide(s_loader);

        // Bullets: in the loader while it is in the hand or falling.
        if (loaderOurs)
        {
            for (int i = 0; i < 6; ++i)
            {
                if (s_inLoaderValid)
                    MulMatrix3x4(all[s_loader], s_inLoader[i], all[s_bullet[i]]);
                else
                    hide(s_bullet[i]);
            }
            return;
        }
        // Cylinder opening / open: the old bullets.
        const unsigned eject = MagReload::g_357EjectTick.load(std::memory_order_relaxed);
        static thread_local unsigned s_ejectSeen = 0;
        static thread_local float s_ejectStart[6][3][4]{};
        if (!scrubbing || !s_inCylValid)
        {
            for (int i = 0; i < 6; ++i)
                hide(s_bullet[i]);
            return;
        }
        float seated[6][3][4];
        for (int i = 0; i < 6; ++i)
            MulMatrix3x4(all[s_chamber], s_inCyl[i], seated[i]);
        // Opening (cycle 0.02 -> 0.5): still in the cylinder. Open (held at
        // 0.5): the empties drop out of the gun to the floor and lie there,
        // gone 2.5 s after the eject (also when back from a weapon switch
        // with the cylinder open). Only six bullet bones: a loader taken
        // from the hip takes them over (it carries the new rounds).
        const float te = eject ? (now - eject) * 0.001f : 99.f;
        // eject == 0: the open cylinder still holds them (bmvr_357_tilt).
        if (cycle < 0.499f || eject == 0)
        {
            for (int i = 0; i < 6; ++i)
                std::memcpy(all[s_bullet[i]], seated[i], sizeof(seated[i]));
            return;
        }
        static thread_local float s_floorZ = 0.f;
        if (s_ejectSeen != eject)
        {
            s_ejectSeen = eject;
            std::memcpy(s_ejectStart, seated, sizeof(seated));
            // Floor = the player's feet (origin), else 48 units under the gun.
            Vector feet;
            const bool haveFeet = Hooks::m_Game && Hooks::m_Game->LocalPlayerAbsOrigin(feet)
                && feet.z < seated[0][2][3];
            s_floorZ = haveFeet ? feet.z + 0.6f : seated[0][2][3] - 48.f;
            Game::logMsg("Mag reload: 357 empties out, %.1f units above the floor%s",
                seated[0][2][3] - s_floorZ, haveFeet ? "" : " (floor guessed)");
        }
        if (te >= 2.5f)
        {
            for (int i = 0; i < 6; ++i)
                hide(s_bullet[i]);
            return;
        }
        // User, 2026-11-18: the empties fall out of the gun itself to the
        // floor. Pushed out of the back of the cylinder, then nearly straight
        // down with a slight drift to the gun's left (clear of the gun hand;
        // a strong push put them next to the left hand), tumbling until
        // they land.
        Vector f, r, u;
        QAngle::AngleVectors(gunAng, &f, &r, &u);
        constexpr float kGravity = 386.f; // units/s^2 (1 unit ~ 1 inch)
        for (int i = 0; i < 6; ++i)
        {
            const float push = (std::min)(te, 0.08f) * 60.f * vmScale;
            const float tf = (std::max)(0.f, te - 0.05f); // falling time after the push
            const float vary = 0.75f + 0.1f * static_cast<float>(i);
            Vector side = r * -1.f;
            side.z = 0.f;
            VectorNormalize(side);
            Vector back = f * -1.f;
            back.z = 0.f;
            VectorNormalize(back);
            const float startZ = s_ejectStart[i][2][3];
            float drop = 0.5f * kGravity * tf * tf;
            float tAir = tf;
            if (startZ - drop < s_floorZ)
            {
                drop = (std::max)(0.f, startZ - s_floorZ);
                tAir = std::sqrt(2.f * drop / kGravity);
            }
            const Vector d = f * -push + (side * 7.f + back * 3.f + r * ((i % 3) - 1.f) * 2.f) * (vary * tAir)
                - Vector(0.f, 0.f, drop);
            // Tumble about the gun's forward axis while in the air.
            const float ang = tAir * (9.f + 1.5f * static_cast<float>(i)) * (i % 2 ? 1.f : -1.f);
            const float c = std::cos(ang), sn = std::sin(ang), t1 = 1.f - c;
            const Vector a = f;
            const float R[3][3] = {
                { c + a.x * a.x * t1, a.x * a.y * t1 - a.z * sn, a.x * a.z * t1 + a.y * sn },
                { a.y * a.x * t1 + a.z * sn, c + a.y * a.y * t1, a.y * a.z * t1 - a.x * sn },
                { a.z * a.x * t1 - a.y * sn, a.z * a.y * t1 + a.x * sn, c + a.z * a.z * t1 } };
            float* out = &all[s_bullet[i]][0][0];
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    out[row * 4 + col] = R[row][0] * s_ejectStart[i][0][col] + R[row][1] * s_ejectStart[i][1][col]
                        + R[row][2] * s_ejectStart[i][2][col];
            all[s_bullet[i]][0][3] = s_ejectStart[i][0][3] + d.x;
            all[s_bullet[i]][1][3] = s_ejectStart[i][1][3] + d.y;
            all[s_bullet[i]][2][3] = s_ejectStart[i][2][3] + d.z;
        }
    }

    // Crossbow bolts with manual magazines (user, 2026-11-18): the four in
    // the magazine (Bolt02..05_Cntl, children of clip_export_bone) and the
    // one on the string (Bolt01_bone, bodygroup dart_live) are drawn from the
    // game's clip (a_bolts_advance layer + dart body), and the game refills
    // its clip by itself once the mod's magazine is dry, so they came back
    // in an empty gun. Draw them from the mod's state instead:
    // - on the string only while a magazine with rounds is seated (with the
    //   magazine out there is nothing to shoot, so none is shown);
    // - in the magazine: none when it is dry in the gun or fell out empty;
    //   all four for a fresh one in the hand (pose sampled from a full
    //   magazine in idle).
    void ApplyCrossbowBolts(unsigned char* hdr, float* bones, int nBones)
    {
        if (MagReload::g_Weapon.load(std::memory_order_relaxed) != MagReload::kWeaponCrossbow || !hdr || !bones)
            return;
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_clip = -1, s_dart = -1, s_bolt[4]{};
        static thread_local bool s_ok = false;
        static thread_local float s_full[4][3][4]{};
        static thread_local bool s_fullValid = false;
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_clip = FindStudioBoneIndex(hdr, "clip_export_bone");
            s_dart = FindStudioBoneIndex(hdr, "Bolt01_bone");
            s_ok = s_clip >= 0 && s_clip < nBones && s_dart >= 0 && s_dart < nBones;
            for (int i = 0; i < 4; ++i)
            {
                char n[20];
                std::snprintf(n, sizeof(n), "Bolt%02d_Cntl", i + 2);
                s_bolt[i] = FindStudioBoneIndex(hdr, n);
                s_ok = s_ok && s_bolt[i] >= 0 && s_bolt[i] < nBones;
            }
            s_fullValid = false;
            Game::logMsg("Mag reload: crossbow bolts clip %d dart %d magazine %d..%d ok=%d",
                s_clip, s_dart, s_bolt[0], s_bolt[3], s_ok ? 1 : 0);
        }
        if (!s_ok)
            return;
        float (*all)[3][4] = reinterpret_cast<float (*)[3][4]>(bones);
        auto hide = [&](int b) {
            for (int r = 0; r < 3; ++r)
                all[b][r][0] = all[b][r][1] = all[b][r][2] = 0.f;
        };
        const int st = MagReload::g_State.load(std::memory_order_relaxed);
        const int shown = MagReload::g_DisplayClip.load(std::memory_order_relaxed);
        const int maxClip = MagReload::Def(MagReload::kWeaponCrossbow).maxClip;
        const bool clipHidden = all[s_clip][0][0] == 0.f && all[s_clip][0][1] == 0.f && all[s_clip][0][2] == 0.f;
        if (st == MagReload::kLoaded && shown >= maxClip && !clipHidden)
        {
            float inv[3][4];
            if (InvertMatrix3x4General(all[s_clip], inv))
            {
                for (int i = 0; i < 4; ++i)
                    MulMatrix3x4(inv, all[s_bolt[i]], s_full[i]);
                if (!s_fullValid)
                    Game::logMsg("Mag reload: crossbow full magazine bolts sampled");
                s_fullValid = true;
            }
        }
        if (st != MagReload::kLoaded || shown <= 0)
            hide(s_dart);
        const bool fellEmpty = st == MagReload::kEmpty && MagReload::g_DropFrom.load() == 0
            && MagReload::g_EjectedEmpty.load();
        if (st == MagReload::kDry || (st == MagReload::kLoaded && shown <= 0) || fellEmpty)
        {
            for (int i = 0; i < 4; ++i)
                hide(s_bolt[i]);
        }
        else if (st == MagReload::kInHand && s_fullValid && !clipHidden)
        {
            for (int i = 0; i < 4; ++i)
                MulMatrix3x4(all[s_clip], s_full[i], all[s_bolt[i]]);
        }
    }

    void ApplyMagReloadBonesTracked(VR* vr, unsigned char* hdr, float* bones, int nBones,
        const Vector& gunOrigin, const QAngle& gunAng, float vmScale);

    // VrClipHandSnap: the gun's own (kept) left hand onto the held object
    // in Black Mesa's clip pose: L_Wrist = object bone * inv(baked wrist ->
    // object), fingers = L_Wrist * baked finger poses. The baked poses are
    // raw-bone relative and the drawn copy applies one affine map to every
    // bone, so the map carries over. Fingertip nubs follow their finger's
    // last joint, the folded forearm follows the wrist.
    void SnapLeftHandToClipObject(unsigned char* hdr, float* bones, int nBones, int pose)
    {
        if (pose < 0 || pose >= ClipHandPose::kCount || !hdr || !bones)
            return;
        const ClipHandPose::Pose& P = ClipHandPose::kPoses[pose];
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_pose = -1;
        static thread_local int s_obj = -1, s_wrist = -1, s_fing[15]{};
        static thread_local bool s_ok = false;
        static thread_local float s_objInv[3][4]{};
        if (s_hdr != hdr || s_pose != pose)
        {
            s_hdr = hdr;
            s_pose = pose;
            s_obj = FindStudioBoneIndex(hdr, P.objectBone);
            s_wrist = FindStudioBoneIndex(hdr, "L_Wrist");
            s_ok = s_obj >= 0 && s_obj < nBones && s_wrist >= 0 && s_wrist < nBones
                && InvertMatrix3x4General(reinterpret_cast<const float (*)[4]>(P.object), s_objInv);
            static const char* kFinger[5] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
            for (int f = 0; f < 5 && s_ok; ++f)
                for (int seg = 0; seg < 3 && s_ok; ++seg)
                {
                    char name[24];
                    std::snprintf(name, sizeof(name), "L_%s%d", kFinger[f], seg + 1);
                    s_fing[f * 3 + seg] = FindStudioBoneIndex(hdr, name);
                    s_ok = s_fing[f * 3 + seg] >= 0 && s_fing[f * 3 + seg] < nBones;
                }
            Game::logMsg("Clip hand pose: snap %s onto '%s' %s", P.name, P.objectBone, s_ok ? "ok" : "MISSING BONES");
        }
        if (!s_ok)
            return;
        auto* all = reinterpret_cast<float (*)[3][4]>(bones);
        // Old pose of the kept hand bones (nubs follow their finger's last joint).
        float oldDistal[5][3][4];
        for (int f = 0; f < 5; ++f)
            std::memcpy(oldDistal[f], all[s_fing[f * 3 + 2]], sizeof(oldDistal[f]));
        float wristNew[3][4];
        MulMatrix3x4(all[s_obj], s_objInv, wristNew);
        float fingNew[15][3][4];
        for (int j = 0; j < 15; ++j)
            MulMatrix3x4(wristNew, reinterpret_cast<const float (*)[4]>(P.fingers[j]), fingNew[j]);
        static const char* kPrefix[5] = { "L_Thumb", "L_Index", "L_Middle", "L_Ring", "L_Pinky" };
        for (int k = 0; k < t_LeftHandCount; ++k)
        {
            const int b = t_LeftHandBones[k];
            if (b < 0 || b >= nBones)
                continue;
            if (b == s_wrist)
                continue;
            bool done = false;
            for (int j = 0; j < 15 && !done; ++j)
                done = (b == s_fing[j]);
            if (done)
                continue;
            // Other kept bones (fingertip nubs): ride their finger's last joint.
            int bone = 0;
            BMVR_TRY
            {
                const int boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
                unsigned char* bp = hdr + boneindex + b * kStudioBoneSize;
                const char* n = StudioRelString(bp, *reinterpret_cast<int*>(bp));
                bone = -1;
                for (int f = 0; f < 5; ++f)
                    if (_strnicmp(n, kPrefix[f], std::strlen(kPrefix[f])) == 0)
                        bone = f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { bone = -1; }
            if (bone < 0)
                continue;
            float inv[3][4], d[3][4], out[3][4];
            if (!InvertMatrix3x4General(oldDistal[bone], inv))
                continue;
            MulMatrix3x4(fingNew[bone * 3 + 2], inv, d);
            MulMatrix3x4(d, all[b], out);
            std::memcpy(all[b], out, sizeof(out));
        }
        std::memcpy(all[s_wrist], wristNew, sizeof(wristNew));
        for (int j = 0; j < 15; ++j)
            std::memcpy(all[s_fing[j]], fingNew[j], sizeof(fingNew[j]));
        for (int k = 0; k < t_LeftArmCount; ++k)
        {
            const int b = t_LeftArmBones[k];
            if (b < 0 || b >= nBones)
                continue;
            all[b][0][3] = wristNew[0][3];
            all[b][1][3] = wristNew[1][3];
            all[b][2][3] = wristNew[2][3];
        }
    }

    void ApplyMagReloadBones(VR* vr, unsigned char* hdr, float* bones, int nBones,
        const Vector& gunOrigin, const QAngle& gunAng, float vmScale)
    {
        ApplyMagReloadBonesTracked(vr, hdr, bones, nBones, gunOrigin, gunAng, vmScale);
        ApplyCrossbowBolts(hdr, bones, nBones);
    }

    void ApplyMagReloadBonesTracked(VR* vr, unsigned char* hdr, float* bones, int nBones,
        const Vector& gunOrigin, const QAngle& gunAng, float vmScale)
    {
        const int weapon = MagReload::g_Weapon.load(std::memory_order_relaxed);
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_w = -1, s_mag = -1, s_kids[16]{}, s_kidCount = 0;
        if (weapon != MagReload::kWeaponNone && hdr && (s_hdr != hdr || s_w != weapon))
        {
            s_hdr = hdr;
            s_w = weapon;
            s_mag = FindStudioBoneIndex(hdr, MagReload::Def(weapon).magBone);
            s_kidCount = s_mag >= 0 ? FindMagChildBones(hdr, s_mag, s_kids, 16) : 0;
            Game::logMsg("Mag reload: %s magazine carries %d child bones", MagReload::Def(weapon).name, s_kidCount);
        }
        const bool track = weapon != MagReload::kWeaponNone && s_kidCount > 0 && s_mag >= 0 && s_mag < nBones && bones;
        float before[3][4]{};
        if (track)
            std::memcpy(before, bones + s_mag * 12, sizeof(before));
        ApplyMagReloadBonesInner(vr, hdr, bones, nBones, gunOrigin, gunAng, vmScale);
        Apply357Extras(hdr, bones, nBones, gunAng, vmScale);
        if (!track)
            return;
        float* m = bones + s_mag * 12;
        if (std::memcmp(before, m, sizeof(before)) == 0)
            return;
        float inv[3][4], delta[3][4];
        const bool ok = InvertMatrix3x4General(before, inv);
        MulMatrix3x4(reinterpret_cast<const float(*)[4]>(m), inv, delta);
        const bool hidden = m[0] == 0.f && m[1] == 0.f && m[2] == 0.f;
        for (int k = 0; k < s_kidCount; ++k)
        {
            if (s_kids[k] >= nBones)
                continue;
            float* c = bones + s_kids[k] * 12;
            if (hidden || !ok)
            {
                for (int r = 0; r < 3; ++r)
                    c[r * 4 + 0] = c[r * 4 + 1] = c[r * 4 + 2] = 0.f;
                continue;
            }
            float out[3][4];
            MulMatrix3x4(delta, reinterpret_cast<const float(*)[4]>(c), out);
            std::memcpy(c, out, sizeof(out));
        }
    }

    void ApplyMagReloadBonesInner(VR* vr, unsigned char* hdr, float* bones, int nBones,
        const Vector& gunOrigin, const QAngle& gunAng, float vmScale)
    {
        const int weapon = MagReload::g_Weapon.load(std::memory_order_relaxed);
        if (weapon == MagReload::kWeaponNone || !vr)
        {
            MagReload::g_NearWell.store(false);
            MagReload::g_NearBolt.store(false);
            return;
        }
        const MagReload::WeaponDef& def = MagReload::Def(weapon);
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_mag = -1, s_bolt = -1, s_wellBone = -1;
        static thread_local int s_weapon = -1;
        if (s_hdr != hdr || s_weapon != weapon)
        {
            s_hdr = hdr;
            s_weapon = weapon;
            s_mag = FindStudioBoneIndex(hdr, def.magBone);
            s_bolt = FindStudioBoneIndex(hdr, def.boltBone);
            s_wellBone = def.wellBone ? FindStudioBoneIndex(hdr, def.wellBone) : -1;
            Game::logMsg("Mag reload: %s magazine bone %d, bolt bone %d, port bone %d", def.name, s_mag, s_bolt, s_wellBone);
        }
        Vector f, r, u;
        QAngle::AngleVectors(gunAng, &f, &r, &u);
        Vector hand;
        QAngle handAng;
        const bool handOk = vr->OffhandWorldPoseForDraw(hand, handAng);
        Vector hf, hr, hu;
        QAngle::AngleVectors(handAng, &hf, &hr, &hu);
        // Bolt/pump grab point: the controller (tuned grab radius).
        const Vector anchor = hand + hf * -3.f + hu * 3.f;
        const DWORD now = GetTickCount();
        // Magazine/shell in the hand: the centre of the drawn left fist
        // (glove draw, VrHands pose offsets and size included) when the glove
        // drew recently, else the old controller-relative spot. Tuning
        // offsets on top (hand frame).
        Vector magAnchor = anchor;
        if (const unsigned palmMs = MagReload::g_LeftPalmMs.load(std::memory_order_relaxed);
            palmMs != 0 && now - palmMs < 500)
        {
            magAnchor = hand + hf * MagReload::g_LeftPalmLocal[0].load(std::memory_order_relaxed)
                + hr * MagReload::g_LeftPalmLocal[1].load(std::memory_order_relaxed)
                + hu * MagReload::g_LeftPalmLocal[2].load(std::memory_order_relaxed);
        }
        // Bottom of the magazine at the bottom of the fist, so the top sticks
        // out and is easy to seat (user, 2026-11-18). Magazine centre to its
        // bottom along the gun's up axis, measured from v_mp5 / v_glock /
        // v_crossbow (model units at scale 1); the fist is ~8 cm tall (index
        // to pinky) around its centre. Shell and speed loader: small, as is.
        static const float kMagBelowCentre[MagReload::kWeaponCount] = { 0.f, 5.26f, 3.93f, 0.f, 6.21f, 0.f, 0.f };
        const float fistHalf = 0.04f * (vr->m_VRScale > 1.f ? vr->m_VRScale : 39.37f);
        const float lift = (std::max)(0.f, kMagBelowCentre[weapon] * vmScale - fistHalf);
        const Vector fistCentre = magAnchor;
        magAnchor = magAnchor + hf * bmvr::g_VrMagHandForward + hu * (bmvr::g_VrMagHandUp + lift);
        auto local = [](const float* mm, const float* p) {
            return Vector(mm[0] * p[0] + mm[1] * p[1] + mm[2] * p[2] + mm[3],
                mm[4] * p[0] + mm[5] * p[1] + mm[6] * p[2] + mm[7],
                mm[8] * p[0] + mm[9] * p[1] + mm[10] * p[2] + mm[11]);
        };

        // ---- bolt / slide ----
        if (s_bolt >= 0 && s_bolt < nBones)
        {
            float* b = bones + s_bolt * 12;
            const Vector boltPos = local(b, def.boltCenter);
            // Lenient (user: the MP5 handle and glock slide were hard to
            // catch): the fist centre or the old controller spot, against a
            // short segment along the gun (the glock slide is long), 1.5x
            // the grab radius.
            auto segDist = [&](const Vector& p) {
                const float half = 3.f * vmScale;
                const float t = std::clamp((p - boltPos).Dot(f), -half, half);
                return (p - (boltPos + f * t)).Length();
            };
            const float boltDist = (std::min)(segDist(anchor), segDist(fistCentre));
            const bool atBolt = handOk && boltDist < 1.5f * bmvr::g_VrBoltGrabRadius * vmScale;
            MagReload::g_NearBolt.store(atBolt);
            static thread_local DWORD s_boltLogMs = 0;
            if (handOk && MagReload::g_NeedRack.load() && GetTickCount() - s_boltLogMs > 1000)
            {
                s_boltLogMs = GetTickCount();
                const Vector d = boltPos - anchor;
                Game::logMsg("Mag reload: %s bolt %.1f units from the hand (bolt - hand: %.1f fwd %.1f right %.1f up in the hand frame) grab=%d pull=%.2f",
                    def.name, d.Length(), d.Dot(hf), d.Dot(hr), d.Dot(hu),
                    MagReload::g_BoltGrab.load() ? 1 : 0, MagReload::g_BoltPull.load());
            }
            static thread_local bool s_grabbing = false;
            static thread_local float s_startAlong = 0.f;
            float pull = 0.f, handBack = 0.f;
            const float travel = def.boltTravel * vmScale;
            // The hand has to move at least ~6 cm (2.4 units) for a full
            // stroke: the glock slide travels 1.8 x vmScale (~3 cm), so a
            // touch plus a little drift racked it (user, 2026-11-18). The
            // slide follows the hand up to its own travel.
            const float handStroke = (std::max)(travel, 2.4f);
            if (MagReload::g_BoltGrab.load() && handOk)
            {
                // Stroke measured at the controller itself, not at the grab
                // anchor: the anchor sits ~4 units off the controller in its
                // own frame, so wrist tilt during the stroke (which differs
                // with how far forward the hand holds the pump) added to or
                // took from the pull (user 2026-11-21: pump sometimes harder,
                // depending on where the hand snapped on).
                // Dev A/B ("Pump stroke at the controller"): off = the old grab
                // anchor point.
                const float along = ((bmvr::g_PumpStrokeAtController ? hand : anchor) - gunOrigin).Dot(f);
                // Re-anchored forward: a hand that rests on the pump/handle
                // and drifts forward does not then read as pulled.
                if (!s_grabbing || along > s_startAlong)
                {
                    s_grabbing = true;
                    s_startAlong = along;
                }
                handBack = std::clamp(s_startAlong - along, 0.f, handStroke);
                pull = handBack / (handStroke > 0.01f ? handStroke : 1.f);
            }
            else
                s_grabbing = false;
            MagReload::g_BoltPull.store(pull);
            // Held back: almost fully, and the hand pulls it the rest of the way.
            const float back = MagReload::g_SlideLocked.load()
                ? (std::min)(travel * MagReload::kSlideHeldBack + handBack, travel)
                : (std::min)(handBack, travel);
            b[3] -= f.x * back;
            b[7] -= f.y * back;
            b[11] -= f.z * back;
            // The gun's own left hand on the pump rides it.
            if (def.tube && t_KeepLeftHand && back > 0.f)
            {
                for (int k = 0; k < t_LeftHandCount; ++k)
                {
                    if (t_LeftHandBones[k] >= nBones)
                        continue;
                    float* lh = bones + t_LeftHandBones[k] * 12;
                    lh[3] -= f.x * back;
                    lh[7] -= f.y * back;
                    lh[11] -= f.z * back;
                }
            }
        }
        else
            MagReload::g_NearBolt.store(false);

        // ---- magazine ----
        if (s_mag < 0 || s_mag >= nBones)
            return;
        float* m = bones + s_mag * 12;
        // Magazine guns: where the magazine sits in the gun. Shotgun: the
        // loading port on the gun body.
        Vector well = local(m, def.magCenter);
        if (def.wellBone)
        {
            if (s_wellBone < 0 || s_wellBone >= nBones)
                return;
            well = local(bones + s_wellBone * 12, def.wellCenter);
        }
        static thread_local float s_handMag[12]{};
        static thread_local bool s_handMagValid = false;
        static thread_local unsigned s_seenDrop = 0;
        static thread_local float s_drop[12]{};
        const unsigned dropTick = MagReload::g_DropTick.load();
        if (dropTick != s_seenDrop)
        {
            s_seenDrop = dropTick;
            if (MagReload::g_DropFrom.load() == 1 && s_handMagValid)
                std::memcpy(s_drop, s_handMag, sizeof(s_drop));
            else
                std::memcpy(s_drop, m, sizeof(s_drop));
        }
        const int st = MagReload::g_State.load(std::memory_order_relaxed);
        if (st == MagReload::kLoaded || st == MagReload::kDry)
        {
            MagReload::g_NearWell.store(false);
            s_handMagValid = false;
            // Shotgun pump back stroke: the gun's own shell flies out of the
            // ejection port (ShellEject bone) up and to the right, tumbling,
            // and falls; gone after ~1 s. Stands in for the stock pump clip's
            // AE_CLIENT_EJECT_CUSTOM, which goes with that clip.
            static thread_local unsigned char* s_ejectHdr = nullptr;
            static thread_local int s_ejectBone = -1;
            if (s_ejectHdr != hdr)
            {
                s_ejectHdr = hdr;
                s_ejectBone = def.tube ? FindStudioBoneIndex(hdr, "ShellEject") : -1;
            }
            const unsigned ejectTick = MagReload::g_EjectTick.load(std::memory_order_relaxed);
            const float te = ejectTick ? (now - ejectTick) * 0.001f : 99.f;
            if (def.tube && s_ejectBone >= 0 && s_ejectBone < nBones && te < 1.0f)
            {
                static thread_local unsigned s_ejectSeen = 0;
                static thread_local Vector s_port, s_vel, s_spinAxis;
                static thread_local float s_rot0[3][3];
                if (s_ejectSeen != ejectTick)
                {
                    s_ejectSeen = ejectTick;
                    const float* e = bones + s_ejectBone * 12;
                    s_port = Vector(e[3], e[7], e[11]);
                    // Units/s (before the viewmodel scale): right, up, a little back.
                    s_vel = (r * 70.f + u * 60.f - f * 15.f) * vmScale;
                    s_spinAxis = f;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            s_rot0[i][j] = m[i * 4 + j];
                    // A shell bone the idle pose hides (zero scale): lay it
                    // along the gun instead.
                    const float sx = s_rot0[0][0] * s_rot0[0][0] + s_rot0[1][0] * s_rot0[1][0] + s_rot0[2][0] * s_rot0[2][0];
                    if (sx < 0.25f)
                    {
                        const Vector col[3] = { f, r * -1.f, u };
                        for (int j = 0; j < 3; ++j)
                        {
                            s_rot0[0][j] = col[j].x;
                            s_rot0[1][j] = col[j].y;
                            s_rot0[2][j] = col[j].z;
                        }
                    }
                    Game::logMsg("Mag reload: shotgun hull ejected (shell bone scale %.2f)", std::sqrt(sx));
                }
                // Tumble: rotate the shell's frame about the gun's forward axis.
                const float a = te * 12.f; // ~2 turns/s
                const float c = std::cos(a), sn = std::sin(a);
                const Vector k = s_spinAxis;
                float Rk[3][3] = {
                    { c + k.x * k.x * (1 - c), k.x * k.y * (1 - c) - k.z * sn, k.x * k.z * (1 - c) + k.y * sn },
                    { k.y * k.x * (1 - c) + k.z * sn, c + k.y * k.y * (1 - c), k.y * k.z * (1 - c) - k.x * sn },
                    { k.z * k.x * (1 - c) - k.y * sn, k.z * k.y * (1 - c) + k.x * sn, c + k.z * k.z * (1 - c) } };
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        m[i * 4 + j] = Rk[i][0] * s_rot0[0][j] + Rk[i][1] * s_rot0[1][j] + Rk[i][2] * s_rot0[2][j];
                const Vector pos = s_port + s_vel * te - Vector(0.f, 0.f, 0.5f * 386.f * te * te);
                m[3] = m[7] = m[11] = 0.f;
                const Vector c0 = local(m, def.magCenter);
                m[3] = pos.x - c0.x;
                m[7] = pos.y - c0.y;
                m[11] = pos.z - c0.z;
                return;
            }
            // Shotgun: a shell let go of falls; the stock shell otherwise.
            const float t = dropTick ? (now - dropTick) * 0.001f : 99.f;
            if (def.wellBone && t < 1.2f && MagReload::g_DropFrom.load() == 1)
            {
                std::memcpy(m, s_drop, sizeof(s_drop));
                m[11] -= (std::min)(0.5f * 386.f * t * t, 48.f);
            }
            return;
        }
        // .357: the cylinder and loader follow the scrubbed stock clip; only
        // a loader let go of falls.
        if (st == MagReload::kEmpty && def.bodyBone && def.wellBone)
        {
            MagReload::g_NearWell.store(false);
            s_handMagValid = false;
            const float t = dropTick ? (now - dropTick) * 0.001f : 99.f;
            if (t < 1.2f && MagReload::g_DropFrom.load() == 1)
            {
                std::memcpy(m, s_drop, sizeof(s_drop));
                m[11] -= (std::min)(0.5f * 386.f * t * t, 48.f);
            }
            return;
        }
        if (st == MagReload::kEmpty)
        {
            MagReload::g_NearWell.store(false);
            s_handMagValid = false;
            const float t = dropTick ? (now - dropTick) * 0.001f : 99.f;
            if (t < 1.2f)
            {
                // Falls for ~0.5 s (gravity 386 in/s^2, at most 4 ft), then lies still.
                std::memcpy(m, s_drop, sizeof(s_drop));
                m[11] -= (std::min)(0.5f * 386.f * t * t, 48.f);
                return;
            }
            for (int i = 0; i < 3; ++i)
                m[i * 4 + 0] = m[i * 4 + 1] = m[i * 4 + 2] = 0.f;
            return;
        }
        // In the hand.
        if (!handOk)
            return;
        float seat[12];
        std::memcpy(seat, m, sizeof(seat)); // the rocket's seated pose (RPG)
        auto basis = [](const QAngle& a, float out[3][3]) {
            Vector fa, ra, ua;
            QAngle::AngleVectors(a, &fa, &ra, &ua);
            const Vector c[3] = { fa, ra * -1.f, ua };
            for (int i = 0; i < 3; ++i)
            {
                out[0][i] = c[i].x;
                out[1][i] = c[i].y;
                out[2][i] = c[i].z;
            }
        };
        float L[3][3], G[3][3], D[3][3], R[3][3];
        basis(handAng, L);
        basis(gunAng, G);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                D[i][j] = L[i][0] * G[j][0] + L[i][1] * G[j][1] + L[i][2] * G[j][2];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                R[i][j] = D[i][0] * m[0 * 4 + j] + D[i][1] * m[1 * 4 + j] + D[i][2] * m[2 * 4 + j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i * 4 + j] = R[i][j];
        // Shotgun shell: held by its base in the left glove's pinch (thumb
        // and index, VrStockHandPose), pointing out of the hand away from
        // the palm, about a fifth in the fingers (user, 2026-11-18: centred
        // in the fist along its grip tunnel it was "basically invisible").
        // bmvr_shell_out = shell centre beyond the pinch point, fraction of
        // its length (0.3 = a fifth held). The well check uses the moved
        // centre. Without a drawn glove pinch: slid out of the fist instead.
        Vector shellTip;
        bool haveShellTip = false;
        if (def.tube)
        {
            Vector axis(m[0], m[4], m[8]);
            const float axisLen = VectorNormalize(axis);
            const unsigned pinchMs = MagReload::g_LeftPinchMs.load(std::memory_order_relaxed);
            Vector out;
            Vector base = fistCentre;
            if (pinchMs != 0 && now - pinchMs < 500)
            {
                base = hand + hf * MagReload::g_LeftPinchLocal[0].load(std::memory_order_relaxed)
                    + hr * MagReload::g_LeftPinchLocal[1].load(std::memory_order_relaxed)
                    + hu * MagReload::g_LeftPinchLocal[2].load(std::memory_order_relaxed);
                out = base - fistCentre;
            }
            else
                out = hu - hf * 0.5f;
            if (axisLen > 0.01f && VectorNormalize(out) > 0.01f)
            {
                // Turn the shell so its axis (bone x, the sign that makes the
                // turn smallest) points along "out".
                Vector cur = axis.Dot(out) >= 0.f ? axis : axis * -1.f;
                if (bmvr::g_VrShellFlip)
                    cur = cur * -1.f;
                const Vector v = CrossProduct(cur, out);
                const float c = cur.Dot(out);
                if (c > -0.999f)
                {
                    const float k = 1.f / (1.f + c);
                    const float R[3][3] = {
                        { 1.f - (v.y * v.y + v.z * v.z) * k, -v.z + v.x * v.y * k, v.y + v.x * v.z * k },
                        { v.z + v.x * v.y * k, 1.f - (v.x * v.x + v.z * v.z) * k, -v.x + v.y * v.z * k },
                        { -v.y + v.x * v.z * k, v.x + v.y * v.z * k, 1.f - (v.x * v.x + v.y * v.y) * k } };
                    float rot[3][3];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            rot[i][j] = R[i][0] * m[0 * 4 + j] + R[i][1] * m[1 * 4 + j] + R[i][2] * m[2 * 4 + j];
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            m[i * 4 + j] = rot[i][j];
                }
                magAnchor = base + out * (bmvr::g_VrShellOut * 4.27f * vmScale);
                // Leading end (away from the fingers): what goes into the port.
                shellTip = magAnchor + out * (0.5f * 4.27f * vmScale);
                haveShellTip = true;
            }
        }
        // Reload-clip hand pose (VrClipHandPose): the magazine where Black
        // Mesa's own hand holds it in the reload clip, from the drawn
        // glove's wrist (MagReload::g_LeftClipWrist): wrist * the baked
        // wrist -> magazine pose. The palm point scales with the glove,
        // the magazine's own extent with the gun, so a glove size that
        // differs from the viewmodel scale keeps it in the fingers.
        if (const int want = MagReload::InHandClipPose(weapon); want >= 0)
        {
            const unsigned clipMs = MagReload::g_LeftClipMs.load(std::memory_order_relaxed);
            const float unit = MagReload::g_LeftClipUnit.load(std::memory_order_relaxed);
            if (MagReload::g_LeftClipPose.load(std::memory_order_relaxed) == want && clipMs != 0 && now - clipMs < 500
                && unit > 1e-4f)
            {
                float W[12];
                for (int k = 0; k < 12; ++k)
                    W[k] = MagReload::g_LeftClipWrist[k].load(std::memory_order_relaxed);
                Vector ax[3];
                for (int c = 0; c < 3; ++c)
                    ax[c] = hf * W[0 * 4 + c] + hr * W[1 * 4 + c] + hu * W[2 * 4 + c];
                const Vector wo = hand + hf * W[3] + hr * W[7] + hu * W[11];
                const float* O = ClipHandPose::kPoses[want].object;
                for (int j = 0; j < 3; ++j)
                {
                    const float len = std::sqrt(m[0 * 4 + j] * m[0 * 4 + j] + m[1 * 4 + j] * m[1 * 4 + j]
                        + m[2 * 4 + j] * m[2 * 4 + j]);
                    const Vector col = (ax[0] * O[0 * 4 + j] + ax[1] * O[1 * 4 + j] + ax[2] * O[2 * 4 + j]) * len;
                    m[0 * 4 + j] = col.x;
                    m[1 * 4 + j] = col.y;
                    m[2 * 4 + j] = col.z;
                }
                float centre[3], grasp[3];
                const float (*fing)[12] = ClipHandPose::kPoses[want].fingers;
                for (int i = 0; i < 3; ++i)
                {
                    centre[i] = O[i * 4 + 3] + O[i * 4 + 0] * def.magCenter[0] + O[i * 4 + 1] * def.magCenter[1]
                        + O[i * 4 + 2] * def.magCenter[2];
                    // Palm: the wrist and the middle finger's joints (6..8).
                    grasp[i] = (fing[6][i * 4 + 3] + fing[7][i * 4 + 3] + fing[8][i * 4 + 3]) * 0.25f;
                }
                Vector v;
                v.x = grasp[0] * unit + (centre[0] - grasp[0]) * vmScale;
                v.y = grasp[1] * unit + (centre[1] - grasp[1]) * vmScale;
                v.z = grasp[2] * unit + (centre[2] - grasp[2]) * vmScale;
                magAnchor = wo + ax[0] * v.x + ax[1] * v.y + ax[2] * v.z;
                // Glock (DEBUG "GLOCK MAGAZINE IN THE HAND"): BM's glock reload
                // shows little of its left hand, so the baked pose is tuned by
                // hand: turned about the left controller's right / forward / up
                // axes round the grip (palm), then moved forward / up / side (cm).
                if (weapon == MagReload::kWeaponGlock)
                {
                    const Vector grip = wo + ax[0] * (grasp[0] * unit) + ax[1] * (grasp[1] * unit) + ax[2] * (grasp[2] * unit);
                    auto rotateMag = [&](const Vector& axis, float deg) {
                        if (deg == 0.f)
                            return;
                        const float a = deg * (3.14159265f / 180.f);
                        const float ca = std::cos(a), sa = std::sin(a);
                        auto rv = [&](const Vector& q) {
                            return q * ca + CrossProduct(axis, q) * sa + axis * (axis.Dot(q) * (1.f - ca));
                        };
                        for (int j = 0; j < 3; ++j)
                        {
                            const Vector col = rv(Vector(m[0 * 4 + j], m[1 * 4 + j], m[2 * 4 + j]));
                            m[0 * 4 + j] = col.x;
                            m[1 * 4 + j] = col.y;
                            m[2 * 4 + j] = col.z;
                        }
                        magAnchor = grip + rv(magAnchor - grip);
                    };
                    rotateMag(hr, bmvr::g_GlockMagTurn);
                    rotateMag(hf, bmvr::g_GlockMagRoll);
                    rotateMag(hu, bmvr::g_GlockMagSpin);
                    const float ucm = (vr->m_VRScale > 1.f ? vr->m_VRScale : 39.37f) * 0.01f;
                    magAnchor = magAnchor + hf * (bmvr::g_GlockMagFwdCm * ucm) + hu * (bmvr::g_GlockMagUpCm * ucm)
                        + hr * (bmvr::g_GlockMagSideCm * ucm);
                }
                static thread_local DWORD s_clipLogMs = 0;
                if (now - s_clipLogMs > 2000)
                {
                    s_clipLogMs = now;
                    const Vector d = magAnchor - hand;
                    // Object's bone x vs the gun's forward (RPG: rocket vs tube axis).
                    Vector ox(m[0], m[4], m[8]);
                    const float oxLen = ox.Length();
                    const float alignF = oxLen > 1e-4f ? ox.Dot(f) / oxLen : 0.f;
                    Game::logMsg("Clip hand pose: %s magazine centre %.1f fwd %.1f right %.1f up of the controller (unit %.3f, vm %.3f, bone x . gun fwd %.2f; bone x in the controller frame %.2f fwd %.2f right %.2f up)",
                        def.name, d.Dot(hf), d.Dot(hr), d.Dot(hu), unit, vmScale, alignF,
                        oxLen > 1e-4f ? ox.Dot(hf) / oxLen : 0.f, oxLen > 1e-4f ? ox.Dot(hr) / oxLen : 0.f,
                        oxLen > 1e-4f ? ox.Dot(hu) / oxLen : 0.f);
                }
            }
        }
        // RPG rocket through the closed left fist (VrClipHandRpg; user
        // 2026-11-19: in BM's open pushing palm it did not look held): its
        // long axis along the glove's knuckle line, the fist round it a
        // quarter of its length from the warhead, the tail out of the pinky
        // side (VrClipHandRpgFlip, default) or the thumb side. The tail goes
        // into the tube first.
        if (weapon == MagReload::kWeaponRpg && bmvr::g_VrClipHandRpg)
        {
            const unsigned tunMs = MagReload::g_LeftTunnelMs.load(std::memory_order_relaxed);
            Vector seatX(seat[0], seat[4], seat[8]);
            Vector hx(m[0], m[4], m[8]);
            if (tunMs != 0 && now - tunMs < 500 && VectorNormalize(seatX) > 1e-4f && VectorNormalize(hx) > 1e-4f)
            {
                float T[6];
                for (int k = 0; k < 6; ++k)
                    T[k] = MagReload::g_LeftTunnelLocal[k].load(std::memory_order_relaxed);
                const Vector tunnel = hand + hf * T[0] + hr * T[1] + hu * T[2];
                Vector knuckle = hf * T[3] + hr * T[4] + hu * T[5]; // pinky -> index
                VectorNormalize(knuckle);
                // Warhead direction of the bone x axis: out of the muzzle when seated.
                const Vector warheadNow = seatX.Dot(f) < 0.f ? hx * -1.f : hx;
                const Vector tail = bmvr::g_VrClipHandRpgFlip ? knuckle * -1.f : knuckle;
                const Vector want = tail * -1.f;
                const Vector v = CrossProduct(warheadNow, want);
                const float c = warheadNow.Dot(want);
                float R[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                if (c > -0.999f)
                {
                    const float k = 1.f / (1.f + c);
                    const float Rv[3][3] = {
                        { 1.f - (v.y * v.y + v.z * v.z) * k, -v.z + v.x * v.y * k, v.y + v.x * v.z * k },
                        { v.z + v.x * v.y * k, 1.f - (v.x * v.x + v.z * v.z) * k, -v.x + v.y * v.z * k },
                        { -v.y + v.x * v.z * k, v.x + v.y * v.z * k, 1.f - (v.x * v.x + v.y * v.y) * k } };
                    std::memcpy(R, Rv, sizeof(R));
                }
                else
                {
                    // Opposite: half a turn about the controller's up axis.
                    const Vector ax = hu;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            R[i][j] = 2.f * (&ax.x)[i] * (&ax.x)[j] - (i == j ? 1.f : 0.f);
                }
                float rot[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        rot[i][j] = R[i][0] * m[0 * 4 + j] + R[i][1] * m[1 * 4 + j] + R[i][2] * m[2 * 4 + j];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        m[i * 4 + j] = rot[i][j];
                const float len = 28.3f * vmScale;
                magAnchor = tunnel + tail * (0.25f * len);
                // The rocket is thicker than a closed fist (user screenshot
                // 2026-11-19: fingers inside it): the hand opens partway
                // (bmvr_rpg_fist_curl) and the rocket sits out of the palm,
                // away from the palm centre past the curl (bmvr_rpg_fist_out, cm).
                Vector outOfPalm = tunnel - fistCentre;
                outOfPalm = outOfPalm - knuckle * outOfPalm.Dot(knuckle);
                const float ucm = (vr->m_VRScale > 1.f ? vr->m_VRScale : 39.37f) * 0.01f;
                if (VectorNormalize(outOfPalm) > 1e-3f)
                    magAnchor = magAnchor + outOfPalm * (bmvr::g_RpgFistOutCm * ucm);
                // Hand tuning (DEBUG "RPG ROCKET IN THE HAND"): turned about the
                // left controller's right / forward / up axes round the grip
                // (curl centre), then moved forward / up / side (cm).
                auto rotateRocket = [&](const Vector& axis, float deg) {
                    if (deg == 0.f)
                        return;
                    const float a = deg * (3.14159265f / 180.f);
                    const float ca = std::cos(a), sa = std::sin(a);
                    auto rv = [&](const Vector& v) {
                        return v * ca + CrossProduct(axis, v) * sa + axis * (axis.Dot(v) * (1.f - ca));
                    };
                    for (int j = 0; j < 3; ++j)
                    {
                        const Vector col = rv(Vector(m[0 * 4 + j], m[1 * 4 + j], m[2 * 4 + j]));
                        m[0 * 4 + j] = col.x;
                        m[1 * 4 + j] = col.y;
                        m[2 * 4 + j] = col.z;
                    }
                    magAnchor = tunnel + rv(magAnchor - tunnel);
                };
                rotateRocket(hr, bmvr::g_RpgHandTurn);
                rotateRocket(hf, bmvr::g_RpgHandRoll);
                rotateRocket(hu, bmvr::g_RpgHandSpin);
                magAnchor = magAnchor + hf * (bmvr::g_RpgHandFwdCm * ucm) + hu * (bmvr::g_RpgHandUpCm * ucm)
                    + hr * (bmvr::g_RpgHandSideCm * ucm);
                static thread_local DWORD s_fistLogMs = 0;
                if (now - s_fistLogMs > 2000)
                {
                    s_fistLogMs = now;
                    const Vector d = magAnchor - hand;
                    Game::logMsg("Mag reload: rpg rocket through the left fist (tail out of the %s side), centre %.1f fwd %.1f right %.1f up of the controller",
                        bmvr::g_VrClipHandRpgFlip ? "pinky" : "thumb", d.Dot(hf), d.Dot(hr), d.Dot(hu));
                }
            }
        }
        // Magazine centre in the palm.
        m[3] = m[7] = m[11] = 0.f;
        const Vector c0 = local(m, def.magCenter);
        m[3] = magAnchor.x - c0.x;
        m[7] = magAnchor.y - c0.y;
        m[11] = magAnchor.z - c0.z;
        if (weapon == MagReload::kWeaponRpg)
        {
            // RPG: front loading. The tube axis is the seated rocket's long
            // axis (bone x), pointing out of the muzzle. A rocket held in
            // front of the muzzle, lined up with the tube, with its tail near
            // the muzzle snaps onto the axis; pushing it back slides it in and
            // it seats at its stock position. Brought to the seat from the
            // side or the back it does not go in (user, 2026-11-18).
            static thread_local bool s_engaged = false;
            static thread_local DWORD s_inHandMs = 0;
            if (now - s_inHandMs > 250)
                s_engaged = false;
            s_inHandMs = now;
            Vector ax(seat[0], seat[4], seat[8]);
            const float axLen = ax.Length();
            Vector hx(m[0], m[4], m[8]);
            const float hxLen = hx.Length();
            float sign = 1.f;
            if (axLen > 1e-4f && hxLen > 1e-4f)
            {
                ax = ax * (1.f / axLen);
                hx = hx * (1.f / hxLen);
                if (ax.Dot(f) < 0.f)
                {
                    ax = ax * -1.f;
                    sign = -1.f;
                }
                hx = hx * sign;
                const Vector seatC = local(seat, def.magCenter);
                const Vector d = magAnchor - seatC;
                const float along = d.Dot(ax);
                const float radial = (d - ax * along).Length();
                const float align = hx.Dot(ax);
                // Rocket length 28.3 units; seated, ~5 stick out of the
                // muzzle, so its tail reaches the muzzle at along ~0.8 L.
                const float len = 28.3f * vmScale;
                // Was lenient (5 units off the axis, ~45 deg, kept on the axis up
                // to 10 units away): it snapped onto the muzzle too easily and
                // then sat there without the hand (user 2026-11-19). Now 2.5
                // units / ~25 deg to engage, off again past 4 units / ~40 deg.
                const float tol = 2.5f * vmScale;
                static thread_local float s_engageAlong = 0.f;
                if (!s_engaged && along > 0.4f * len && along < 0.95f * len && radial < tol && align > 0.9f)
                {
                    s_engaged = true;
                    s_engageAlong = along;
                    Game::logMsg("Mag reload: rpg rocket engaged at the muzzle (along %.1f radial %.1f align %.2f)",
                        along / vmScale, radial / vmScale, align);
                }
                else if (s_engaged && (radial > 1.6f * tol || along > 1.05f * len || align < 0.75f))
                {
                    s_engaged = false;
                    Game::logMsg("Mag reload: rpg rocket off the tube (along %.1f radial %.1f align %.2f)",
                        along / vmScale, radial / vmScale, align);
                }
                if (s_engaged)
                {
                    const float depth = std::clamp(along, 0.f, 0.95f * len);
                    std::memcpy(m, seat, sizeof(seat));
                    m[3] += ax.x * depth;
                    m[7] += ax.y * depth;
                    m[11] += ax.z * depth;
                }
                MagReload::g_RpgInsert.store(s_engaged ? std::clamp(1.f - along / (0.95f * len), 0.f, 1.f) : -1.f);
                // Seats by itself 70 % in (user, 2026-11-18: had to push
                // further than expected; was along <= 1.5 units = all the way),
                // or once pushed a tenth of its length past where it engaged
                // (user 2026-11-20: pushing on, the hand went through the gun;
                // take it a little after the buzz).
                MagReload::g_NearWell.store(s_engaged
                    && (along <= 0.3f * 0.95f * len || along <= s_engageAlong - 0.1f * len));
                static thread_local DWORD s_rpgLogMs = 0;
                if (now - s_rpgLogMs > 1000)
                {
                    s_rpgLogMs = now;
                    Game::logMsg("Mag reload: rpg rocket along %.1f radial %.1f align %.2f (units at scale 1), engaged %d",
                        along / vmScale, radial / vmScale, align, s_engaged ? 1 : 0);
                }
            }
            else
            {
                MagReload::g_RpgInsert.store(-1.f);
                MagReload::g_NearWell.store(false);
            }
            std::memcpy(s_handMag, m, sizeof(s_handMag));
            s_handMagValid = true;
            return;
        }
        MagReload::g_RpgInsert.store(-1.f);
        std::memcpy(s_handMag, m, sizeof(s_handMag));
        s_handMagValid = true;
        // Seats when the magazine centre is within the held weapon's seat
        // distance (cm) of the well and turned less than its seat angle away
        // from the seated pose (VR panel DEBUG rows, saved per weapon; user
        // 2026-11-19: with the magazine in BM's reload grip it snapped in
        // from far too far, so be strict). Angle = rotation between the
        // magazine's frame in the hand and in the gun (shotgun / .357: 180 =
        // not checked).
        const float unitsPerM = vr->m_VRScale > 1.f ? vr->m_VRScale : 39.37f;
        constexpr float kShellPortLeftCm = 2.f;
        // Shotgun: the shell's leading tip at the port (user 2026-11-19: held
        // upright under the port it did not load; its centre was measured).
        // Port centre 2 cm to the gun's left of the measured point (user
        // 2026-11-20: gun held level, the zone sat a little to the right;
        // height was right).
        if (def.tube)
            well = well - r * (kShellPortLeftCm * 0.01f * unitsPerM);
        float dist = ((haveShellTip ? shellTip : magAnchor) - well).Length();
        // Glock: the seated magazine's centre is up inside the grip, inside
        // the right fist, so the left hand stopped 7-9 cm short below it
        // (log 2026-11-19, angle 12-22 deg). Below the well along the gun's
        // up axis, the first kEntryCm count as at the well; sideways stays
        // strict.
        static const float kEntryCm[MagReload::kWeaponCount] = { 0.f, 0.f, 6.f, 0.f, 0.f, 0.f, 0.f };
        if (kEntryCm[weapon] > 0.f)
        {
            const Vector d = magAnchor - well;
            const float up = d.Dot(u);
            if (up < 0.f)
            {
                const float lateral = (d - u * up).Length();
                const float below = (std::max)(0.f, -up - kEntryCm[weapon] * 0.01f * unitsPerM);
                dist = std::sqrt(lateral * lateral + below * below);
            }
        }
        const float distCm = dist / unitsPerM * 100.f;
        float angleDeg = 0.f;
        if (def.tube)
        {
            // Shotgun shell: its long axis (bone x) along the tube (gun
            // forward), either end; the idle shell bone can be hidden, so the
            // seated pose is not used.
            Vector sx(m[0], m[4], m[8]);
            if (VectorNormalize(sx) > 1e-4f)
                angleDeg = std::acos(std::clamp(std::fabs(sx.Dot(f)), 0.f, 1.f)) * (180.f / 3.14159265f);
        }
        else
        {
            float tr = 0.f;
            for (int j = 0; j < 3; ++j)
            {
                const Vector a(m[j], m[4 + j], m[8 + j]);
                const Vector b(seat[j], seat[4 + j], seat[8 + j]);
                const float la = a.Length(), lb = b.Length();
                if (la > 1e-5f && lb > 1e-5f)
                    tr += a.Dot(b) / (la * lb);
            }
            angleDeg = std::acos(std::clamp((tr - 1.f) * 0.5f, -1.f, 1.f)) * (180.f / 3.14159265f);
        }
        const float maxCm = bmvr::g_VmTuneSeatCm;
        const float maxDeg = bmvr::g_VmTuneSeatDeg;
        const bool angleOk = maxDeg >= 179.5f || angleDeg <= maxDeg;
        const bool atWell = distCm <= maxCm && angleOk;
        MagReload::g_NearWell.store(atWell);
        MagReload::g_SeatDistCm.store(distCm, std::memory_order_relaxed);
        MagReload::g_SeatAngleDeg.store(angleDeg, std::memory_order_relaxed);
        static thread_local DWORD s_logMs = 0;
        // Once a second, and at once when only the angle keeps it out.
        static thread_local bool s_angleLogged = false;
        const bool angleOnly = distCm <= maxCm && !angleOk;
        if (now - s_logMs > 1000 || (angleOnly && !s_angleLogged))
        {
            s_logMs = now;
            s_angleLogged = angleOnly;
            const Vector d = well - magAnchor;
            if (def.tube)
            {
                // Shell tip relative to the port in the gun's frame, cm.
                const Vector t = (haveShellTip ? shellTip : magAnchor) - well;
                const float k = 100.f / unitsPerM;
                Game::logMsg("Mag reload: shell tip - port: %.1f fwd %.1f right %.1f up cm in the gun frame",
                    t.Dot(f) * k, t.Dot(r) * k, t.Dot(u) * k);
            }
            Game::logMsg("Mag reload: magazine %.1f cm from the well, %.0f deg off the seated pose (seats within %.1f cm, %.0f deg)%s (well - hand: %.1f fwd %.1f right %.1f up units in the hand frame)",
                distCm, angleDeg, maxCm, maxDeg, angleOnly ? " - angle keeps it out" : "", d.Dot(hf), d.Dot(hr), d.Dot(hu));
        }
    }

    // Frag grenade pin (VR::UpdateGrenadePin). The pin (rod Grenade_Pin2 ->
    // Grenade_Pin1 -> ring Grenad_Pin1, children of GrenadeSpawn) follows the
    // left fist while held/pulled, falls when let go and stays gone until
    // the next grenade. The ring centre is measured from v_grenade.
    void ApplyGrenadePinBones(VR* vr, unsigned char* hdr, float* bones, int nBones, float vmScale)
    {
        if (!MagReload::g_PinWeapon.load(std::memory_order_relaxed) || !vr)
        {
            MagReload::g_PinNear.store(false);
            return;
        }
        static thread_local unsigned char* s_hdr = nullptr;
        static thread_local int s_pin[3] = { -1, -1, -1 };
        if (s_hdr != hdr)
        {
            s_hdr = hdr;
            s_pin[0] = FindStudioBoneIndex(hdr, "Grenade_Pin2");
            s_pin[1] = FindStudioBoneIndex(hdr, "Grenade_Pin1");
            s_pin[2] = FindStudioBoneIndex(hdr, "Grenad_Pin1");
            Game::logMsg("Grenade pin: bones %d %d %d", s_pin[0], s_pin[1], s_pin[2]);
        }
        for (int i = 0; i < 3; ++i)
            if (s_pin[i] < 0 || s_pin[i] >= nBones)
                return;
        Vector hand;
        QAngle handAng;
        if (!vr->OffhandWorldPoseForDraw(hand, handAng))
            return;
        Vector hf, hr, hu;
        QAngle::AngleVectors(handAng, &hf, &hr, &hu);
        const Vector ctrlSpot = hand + hf * -3.f + hu * 3.f;
        Vector fist = ctrlSpot;
        const DWORD now = GetTickCount();
        if (const unsigned palmMs = MagReload::g_LeftPalmMs.load(std::memory_order_relaxed);
            palmMs != 0 && now - palmMs < 500)
            fist = hand + hf * MagReload::g_LeftPalmLocal[0].load(std::memory_order_relaxed)
                + hr * MagReload::g_LeftPalmLocal[1].load(std::memory_order_relaxed)
                + hu * MagReload::g_LeftPalmLocal[2].load(std::memory_order_relaxed);
        const float* ring = bones + s_pin[2] * 12;
        const float rc[3] = { 0.f, 0.05f, -0.74f };
        const Vector ringPos(ring[0] * rc[0] + ring[1] * rc[1] + ring[2] * rc[2] + ring[3],
            ring[4] * rc[0] + ring[5] * rc[1] + ring[6] * rc[2] + ring[7],
            ring[8] * rc[0] + ring[9] * rc[1] + ring[10] * rc[2] + ring[11]);
        const int st = MagReload::g_PinState.load(std::memory_order_relaxed);
        // Lenient like the bolt (user): fist centre or the controller spot,
        // 1.5x the grab radius.
        const float ringDist = (std::min)((fist - ringPos).Length(), (ctrlSpot - ringPos).Length());
        MagReload::g_PinNear.store(st == MagReload::kPinIn
            && ringDist < 1.5f * bmvr::g_VrBoltGrabRadius * vmScale);
        {
            // For harness hand placement; first 30 only.
            static thread_local DWORD s_ringLogMs = 0;
            static thread_local int s_ringLogs = 0;
            if (st == MagReload::kPinIn && now - s_ringLogMs > 1000 && s_ringLogs < 30)
            {
                ++s_ringLogs;
                s_ringLogMs = now;
                const Vector d = ringPos - ctrlSpot;
                Game::logMsg("Grenade pin: ring %.1f units from the hand (ring - hand: %.1f fwd %.1f right %.1f up in the hand frame)",
                    ringDist, d.Dot(hf), d.Dot(hr), d.Dot(hu));
            }
        }
        static thread_local bool s_holding = false;
        static thread_local Vector s_grabOffset; // ring - fist at the grab
        static thread_local Vector s_lastDelta;  // last pin move (for the drop)
        auto move = [&](const Vector& d) {
            for (int i = 0; i < 3; ++i)
            {
                float* m = bones + s_pin[i] * 12;
                m[3] += d.x;
                m[7] += d.y;
                m[11] += d.z;
            }
        };
        if (st == MagReload::kPinIn)
        {
            s_holding = false;
            MagReload::g_PinPull.store(0.f);
            return;
        }
        if (st == MagReload::kPinHeld || st == MagReload::kPinPulledInHand)
        {
            // Anchored to the controller, not the drawn fist: the fist centre
            // moves when the glove changes finger pose at the grab (pinch /
            // reload-clip pose), which would move the ring and count as pull.
            if (!s_holding)
            {
                s_holding = true;
                s_grabOffset = ringPos - hand;
            }
            // The pull is the hand's travel since the grab (ring kept where
            // the hand took it, against the ring's own spot on the grenade).
            const Vector held = hand + s_grabOffset;
            if (st == MagReload::kPinHeld)
                MagReload::g_PinPull.store((held - ringPos).Length() / (vmScale > 0.05f ? vmScale : 1.f));
            Vector target = held;
            // With the reload-clip pose on the glove the ring is drawn where
            // Black Mesa's pinpullthrow hand holds it, round the finger
            // (gliding there over ~0.15 s; the grab offset can be ~10 cm with
            // the lenient grab radius). Was only after the pull: held, the
            // ring floated beside the fingers (user 2026-11-19).
            static thread_local float s_toFingers = 0.f;
            static thread_local DWORD s_blendMs = 0;
            const unsigned clipMs = MagReload::g_LeftClipMs.load(std::memory_order_relaxed);
            const float unit = MagReload::g_LeftClipUnit.load(std::memory_order_relaxed);
            const int pinPose = st == MagReload::kPinHeld ? ClipHandPose::kPinHold : ClipHandPose::kPinPulled;
            if (MagReload::g_LeftClipPose.load(std::memory_order_relaxed) == pinPose
                && clipMs != 0 && now - clipMs < 500 && unit > 1e-4f)
            {
                const float dt = s_blendMs ? (now - s_blendMs) * 0.001f : 0.f;
                s_toFingers = (std::min)(1.f, s_toFingers + dt / 0.15f);
                float W[12];
                for (int k = 0; k < 12; ++k)
                    W[k] = MagReload::g_LeftClipWrist[k].load(std::memory_order_relaxed);
                const float* O = ClipHandPose::kPoses[pinPose].object;
                float p[3];
                for (int i = 0; i < 3; ++i)
                    p[i] = (O[i * 4 + 0] * rc[0] + O[i * 4 + 1] * rc[1] + O[i * 4 + 2] * rc[2] + O[i * 4 + 3]) * unit;
                Vector local(0.f, 0.f, 0.f);
                for (int c = 0; c < 3; ++c)
                    local = local + Vector(W[0 * 4 + c], W[1 * 4 + c], W[2 * 4 + c]) * p[c];
                local = local + Vector(W[3], W[7], W[11]);
                const Vector fingers = hand + hf * local.x + hr * local.y + hu * local.z;
                target = target + (fingers - target) * s_toFingers;
            }
            else
                s_toFingers = 0.f;
            s_blendMs = now;
            const Vector d = target - ringPos;
            move(d);
            s_lastDelta = d;
            return;
        }
        // Dropped: falls ~1 s from where the hand let go, then gone.
        s_holding = false;
        const unsigned drop = MagReload::g_PinDropTick.load(std::memory_order_relaxed);
        const float t = drop ? (now - drop) * 0.001f : 99.f;
        if (t < 1.f)
        {
            move(s_lastDelta - Vector(0.f, 0.f, (std::min)(0.5f * 386.f * t * t, 48.f)));
            return;
        }
        for (int i = 0; i < 3; ++i)
        {
            float* m = bones + s_pin[i] * 12;
            for (int r = 0; r < 3; ++r)
                m[r * 4 + 0] = m[r * 4 + 1] = m[r * 4 + 2] = 0.f;
        }
    }

    // Snark (user 2026-11-19): on the hand throw's release the game plays
    // snark_throw (0.4 s, spawn at cycle 0.67), which swung the snark sideways
    // in the hand before it left, so it did not look thrown. During the throw
    // clips the whole model (hand + snark) is held where SnarkROOT sits in
    // idle, like the grenade; other clips (draw, reload) play as they are.
    bool GrenadeHoldCorrection(unsigned char* hdr, const void* bones, int nBones,
        const float entity[3][4], float out[3][4], bool snark = false)
    {
        static unsigned char* s_hdrs[2] = { nullptr, nullptr };
        static int s_bones[2] = { -1, -1 };
        static bool s_haveAnchors[2] = { false, false };
        static float s_anchors[2][3][4];
        const int w = snark ? 1 : 0;
        unsigned char*& s_hdr = s_hdrs[w];
        int& s_bone = s_bones[w];
        bool& s_haveAnchor = s_haveAnchors[w];
        float (&s_anchor)[3][4] = s_anchors[w];
        if (hdr != s_hdr)
        {
            s_hdr = hdr;
            // Snark: the hand itself (user 2026-11-19: after the throw it was
            // pulled back and came back with a new snark).
            s_bone = FindStudioBoneIndex(hdr, snark ? "R_Wrist" : "GrenadeSpawn");
            s_haveAnchor = false;
        }
        if (s_bone < 0 || s_bone >= nBones || !Hooks::m_Game)
            return false;
        void* vm = Hooks::m_Game->GetViewModelEntity();
        int seq = -1;
        float cur[3][4];
        BMVR_TRY
        {
            if (vm)
                seq = *reinterpret_cast<int*>(static_cast<char*>(vm) + kViewmodelSequence);
            std::memcpy(cur, static_cast<const float*>(bones) + s_bone * 12, sizeof(cur));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        const char* label = seq >= 0 ? StudioSequenceLabel(hdr, seq) : nullptr;
        float entityInv[3][4];
        InvertMatrix3x4TR(entity, entityInv);
        if (label && (_stricmp(label, "idle1") == 0 || _stricmp(label, "idle2") == 0))
        {
            MulMatrix3x4(entityInv, cur, s_anchor);
            s_haveAnchor = true;
            return false;
        }
        if (!s_haveAnchor)
            return false;
        // Snark: held through the throw and the "reload" that brings the next
        // snark into the hand; draw / holster play as they are.
        if (snark && !(label && (_strnicmp(label, "snark_throw", 11) == 0 || _stricmp(label, "reload") == 0)))
            return false;
        // out = entity * anchor * cur^-1: moves GrenadeSpawn back onto its idle pose.
        float want[3][4], curInv[3][4];
        MulMatrix3x4(entity, s_anchor, want);
        InvertMatrix3x4TR(cur, curInv);
        MulMatrix3x4(want, curInv, out);
        return true;
    }

    bool SehCopyAndFixViewmodelMatrices(float* dst, const void* src, int count,
        const float pivot[3], float scale,
        const float yFixOrigin[3], const float right[3], const float up[3], const float fwd[3],
        float yFix, const float* rigidDelta)
    {
        if (!dst || !src || count < 1 || count > kMaxStudioBones)
            return false;
        BMVR_TRY
        {
            std::memcpy(dst, src, static_cast<size_t>(count) * 12 * sizeof(float));
            for (int i = 0; i < count; ++i)
            {
                float* m = dst + i * 12;
                auto matrix = reinterpret_cast<float(*)[4]>(m);
                if (rigidDelta)
                    ApplyMatrix3x4Delta(matrix, reinterpret_cast<const float(*)[4]>(rigidDelta));
                if (yFixOrigin && right && up && fwd
                    && yFix > 0.2f && yFix < 2.f && fabsf(yFix - 1.f) > 0.03f)
                    UnstretchMatrix3x4ViewY(matrix, yFixOrigin, right, up, fwd, yFix);
                if (pivot && scale > 0.2f && scale < 1.5f && fabsf(scale - 1.f) > 0.001f)
                    ScaleMatrix3x4AroundPivot(matrix, pivot, scale);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SehWriteModelScale(void* renderable, float scale)
    {
        if (!renderable)
            return false;
        BMVR_TRY
        {
            *reinterpret_cast<float*>(reinterpret_cast<char*>(renderable) + 0x7C0) = scale;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char* StudioRelString(const unsigned char* base, int rel)
    {
        if (!base || rel <= 0 || rel > 0x100000)
            return "";
        const char* s = reinterpret_cast<const char*>(base + rel);
        BMVR_TRY
        {
            if (!s[0])
                return "";
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return "";
        }
        return s;
    }

    bool PokeInt(int* p, int value)
    {
        if (!p)
            return false;
        DWORD oldProt = 0;
        if (!VirtualProtect(p, sizeof(int), PAGE_READWRITE, &oldProt))
            return false;
        bool ok = false;
        BMVR_TRY
        {
            *p = value;
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        DWORD tmp = 0;
        VirtualProtect(p, sizeof(int), oldProt, &tmp);
        return ok;
    }

    void RememberHdrPatch(int* p, int original, int patched, unsigned char* hdr)
    {
        if (!p || !hdr || g_ArmHdrPatchCount >= kMaxStudioHdrPatches)
            return;
        for (int i = 0; i < g_ArmHdrPatchCount; ++i)
        {
            if (g_ArmHdrPatches[i].p == p)
                return;
        }
        StudioHdrPatch& e = g_ArmHdrPatches[g_ArmHdrPatchCount];
        BMVR_TRY
        {
            e.checksum = *reinterpret_cast<int*>(hdr + 8);
            std::memcpy(e.name, hdr + 12, sizeof(e.name));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        e.p = p;
        e.original = original;
        e.patched = patched;
        e.hdr = hdr;
        ++g_ArmHdrPatchCount;
    }

    // A remembered patch may only be undone while its model header is still
    // there: headers from earlier maps get freed (a map transition unloads
    // models), and writing the old value back then corrupted the heap (user
    // 2026-11-20: crash a few frames into a save loaded from the pause menu
    // after a ladder map transition; LevelShutdown restores every patch).
    // Same header (readable, same checksum and name) and our own value still
    // in place, or nothing is written.
    bool HdrPatchStillOurs(const StudioHdrPatch& e)
    {
        if (!e.p || !e.hdr || !bmvr::seh::IsReadable(e.hdr, 76) || !bmvr::seh::IsReadable(e.p, sizeof(int)))
            return false;
        bool same = false;
        BMVR_TRY
        {
            same = *reinterpret_cast<int*>(e.hdr + 8) == e.checksum
                && std::memcmp(e.hdr + 12, e.name, sizeof(e.name)) == 0
                && *e.p == e.patched;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            same = false;
        }
        return same;
    }

    // Fidget sounds: the fidget clips (tau coil spin, MP5 check...) play
    // their sound from a client AE_CL_PLAYSOUND event at the clip start. In
    // VR the fidget pose is frozen (the gun stays in the hand), so the sound
    // came without the motion it belongs to. Moving those events past the end
    // of the clip (cycle 2) means they never fire; other events in the clip
    // stay. One-time write per studio header, restored with the arm patches.
    // Toggling off restores the header being drawn.
    constexpr int kStudioSeqNumEvents = 24;
    constexpr int kStudioSeqEventIndex = 28;
    constexpr int kStudioEventSize = 80;      // cycle, event, type, options[64], szeventindex
    constexpr int kStudioEventOptions = 12;
    constexpr int kStudioEventNameIndex = 76;
    constexpr int kStudioEventNewStyle = 0x400;
    constexpr int kClEventSound = 5004;       // old-style CL_EVENT_SOUND
    constexpr float kMutedEventCycle = 2.f;

    // Headers already handled (gun + the always-idle hands viewmodel take
    // turns here, so one slot would re-walk both every frame).
    struct FidgetHdrState { unsigned char* hdr; bool muted; };
    FidgetHdrState g_FidgetHdrs[8]{};
    unsigned g_FidgetHdrNext = 0;

    int FloatBits(float f)
    {
        int i;
        std::memcpy(&i, &f, sizeof(i));
        return i;
    }

    void ApplyFidgetSoundMute(unsigned char* hdr)
    {
        const bool mute = bmvr::g_MuteFidgetSounds;
        if (!hdr)
            return;
        FidgetHdrState* slot = nullptr;
        for (auto& st : g_FidgetHdrs)
            if (st.hdr == hdr)
                slot = &st;
        if (slot && slot->muted == mute)
            return;
        if (!slot)
            slot = &g_FidgetHdrs[g_FidgetHdrNext++ & 7];
        slot->hdr = hdr;
        slot->muted = mute;
        int numSeq = 0, seqIndex = 0;
        BMVR_TRY
        {
            numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq);
            seqIndex = *reinterpret_cast<int*>(hdr + kStudioHdrLocalSeqIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (numSeq <= 0 || numSeq > 256 || seqIndex <= 0)
            return;
        int changed = 0;
        for (int s = 0; s < numSeq; ++s)
        {
            const char* label = StudioSequenceLabel(hdr, s);
            if (!label)
                continue;
            char low[96]{};
            for (int i = 0; i < 95 && label[i]; ++i)
                low[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
            if (!std::strstr(low, "fidget"))
                continue;
            unsigned char* desc = hdr + seqIndex + s * kStudioSeqDescSize;
            int numEv = 0, evIndex = 0;
            BMVR_TRY
            {
                numEv = *reinterpret_cast<int*>(desc + kStudioSeqNumEvents);
                evIndex = *reinterpret_cast<int*>(desc + kStudioSeqEventIndex);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            if (numEv <= 0 || numEv > 64 || evIndex <= 0)
                continue;
            for (int e = 0; e < numEv; ++e)
            {
                unsigned char* ev = desc + evIndex + e * kStudioEventSize;
                float cycle = 0.f;
                bool sound = false;
                BMVR_TRY
                {
                    cycle = *reinterpret_cast<float*>(ev);
                    const int id = *reinterpret_cast<int*>(ev + 4);
                    const int type = *reinterpret_cast<int*>(ev + 8);
                    const int nameOff = *reinterpret_cast<int*>(ev + kStudioEventNameIndex);
                    if (type & kStudioEventNewStyle)
                        sound = nameOff > 0
                            && std::strncmp(reinterpret_cast<const char*>(ev + nameOff), "AE_CL_PLAYSOUND", 16) == 0;
                    else
                        sound = id == kClEventSound;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    continue;
                }
                if (!sound)
                    continue;
                int* p = reinterpret_cast<int*>(ev);
                if (mute && cycle <= 1.f)
                {
                    RememberHdrPatch(p, FloatBits(cycle), FloatBits(kMutedEventCycle), hdr);
                    if (PokeInt(p, FloatBits(kMutedEventCycle)))
                        ++changed;
                }
                else if (!mute && cycle == kMutedEventCycle)
                {
                    for (int i = 0; i < g_ArmHdrPatchCount; ++i)
                    {
                        if (g_ArmHdrPatches[i].p == p)
                        {
                            if (HdrPatchStillOurs(g_ArmHdrPatches[i]) && PokeInt(p, g_ArmHdrPatches[i].original))
                                ++changed;
                            g_ArmHdrPatches[i].p = nullptr;
                            break;
                        }
                    }
                }
            }
        }
        if (changed)
        {
            char name[65]{};
            BMVR_TRY { std::memcpy(name, hdr + 12, 64); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            Game::logMsg("Fidget sounds %s on %s: %d event(s)", mute ? "muted" : "restored", name, changed);
        }
    }

    // .357 brass with manual reloads (user, 2026-11-18: "ammo dropping to the
    // ground from the left hand, not from the actual gun"): gun_reload1 has
    // AE_CLIENT_EJECT_BRASS '6' at cycle 0.376. The hands scrub that clip to
    // open the cylinder, so the event fired and the game spawned six casings
    // from its own (un-held) pose of the clip, where the stock left hand
    // dumps them. The drawn bullets already fall out of the cylinder
    // (Apply357Extras), so the event moves past the clip end (same one-time
    // header patch as the fidget sounds) while VrMagReload is on.
    // Same for its AE_CL_PLAYSOUND weapon_357.Reload at cycle 0 (user,
    // 2026-11-18): reload.wav is the whole stock reload ending in the
    // cylinder spin; it refired (CHAN_ITEM, restarting) while the cylinder
    // waited open for the flick, and the last one played the spin after the
    // close. The hands' own open/insert/close sounds come from UpdateMagReload.
    struct BrassHdrState { unsigned char* hdr; bool muted; };
    BrassHdrState g_BrassHdrs[4]{};
    unsigned g_BrassHdrNext = 0;

    void Apply357BrassMute(unsigned char* hdr)
    {
        const bool mute = bmvr::PhysicalWeapon(bmvr::g_VrMagReload);
        if (!hdr)
            return;
        BrassHdrState* slot = nullptr;
        for (auto& st : g_BrassHdrs)
            if (st.hdr == hdr)
                slot = &st;
        if (slot && slot->muted == mute)
            return;
        if (!slot)
            slot = &g_BrassHdrs[g_BrassHdrNext++ & 3];
        slot->hdr = hdr;
        slot->muted = mute;
        // Only the .357 (checked once per header and switch, not per draw).
        if (!StudioHdrMatchesModel(hdr, "models/weapons/v_357.mdl"))
            return;
        int numSeq = 0, seqIndex = 0;
        BMVR_TRY
        {
            numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq);
            seqIndex = *reinterpret_cast<int*>(hdr + kStudioHdrLocalSeqIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (numSeq <= 0 || numSeq > 256 || seqIndex <= 0)
            return;
        int changed = 0;
        for (int s = 0; s < numSeq; ++s)
        {
            const char* label = StudioSequenceLabel(hdr, s);
            if (!label || _stricmp(label, "gun_reload1") != 0)
                continue;
            unsigned char* desc = hdr + seqIndex + s * kStudioSeqDescSize;
            int numEv = 0, evIndex = 0;
            BMVR_TRY
            {
                numEv = *reinterpret_cast<int*>(desc + kStudioSeqNumEvents);
                evIndex = *reinterpret_cast<int*>(desc + kStudioSeqEventIndex);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            if (numEv <= 0 || numEv > 64 || evIndex <= 0)
                continue;
            for (int e = 0; e < numEv; ++e)
            {
                unsigned char* ev = desc + evIndex + e * kStudioEventSize;
                float cycle = 0.f;
                bool brass = false;
                BMVR_TRY
                {
                    cycle = *reinterpret_cast<float*>(ev);
                    const int type = *reinterpret_cast<int*>(ev + 8);
                    const int nameOff = *reinterpret_cast<int*>(ev + kStudioEventNameIndex);
                    const char* evName = reinterpret_cast<const char*>(ev + nameOff);
                    brass = (type & kStudioEventNewStyle) && nameOff > 0
                        && (std::strncmp(evName, "AE_CLIENT_EJECT_BRASS", 22) == 0
                            || std::strncmp(evName, "AE_CL_PLAYSOUND", 16) == 0);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    continue;
                }
                if (!brass)
                    continue;
                int* p = reinterpret_cast<int*>(ev);
                if (mute && cycle <= 1.f)
                {
                    RememberHdrPatch(p, FloatBits(cycle), FloatBits(kMutedEventCycle), hdr);
                    if (PokeInt(p, FloatBits(kMutedEventCycle)))
                        ++changed;
                }
                else if (!mute && cycle == kMutedEventCycle)
                {
                    for (int i = 0; i < g_ArmHdrPatchCount; ++i)
                    {
                        if (g_ArmHdrPatches[i].p == p)
                        {
                            if (HdrPatchStillOurs(g_ArmHdrPatches[i]) && PokeInt(p, g_ArmHdrPatches[i].original))
                                ++changed;
                            g_ArmHdrPatches[i].p = nullptr;
                            break;
                        }
                    }
                }
            }
        }
        Game::logMsg("Mag reload: 357 gun_reload1 brass + reload sound events %s (%d)", mute ? "off (bullets drawn from the cylinder)" : "restored", changed);
    }

    // Manual magazines: the game's own reload clips (hidden; they still run
    // for the refill after firing dry) played their whole stock reload
    // sound from an AE_CL_PLAYSOUND at cycle 0 (user 2026-11-20: the
    // crossbow sounded its reload when emptied, before any hand reload).
    // The hands' own clips come from UpdateMagReload. Same one-time header
    // patch as above; only the sound events move.
    struct ReloadHdrState { unsigned char* hdr; bool muted; };
    ReloadHdrState g_ReloadHdrs[6]{};
    unsigned g_ReloadHdrNext = 0;

    void ApplyStockReloadSoundMute(unsigned char* hdr)
    {
        const bool mute = bmvr::PhysicalWeapon(bmvr::g_VrMagReload);
        if (!hdr)
            return;
        ReloadHdrState* slot = nullptr;
        for (auto& st : g_ReloadHdrs)
            if (st.hdr == hdr)
                slot = &st;
        if (slot && slot->muted == mute)
            return;
        if (!slot)
            slot = &g_ReloadHdrs[g_ReloadHdrNext++ % 6];
        slot->hdr = hdr;
        slot->muted = mute;
        // Sequences per model (v_*.mdl events, checked 2026-11-20).
        static const struct { const char* model; const char* seqs[3]; } kGuns[] = {
            { "models/weapons/v_crossbow.mdl", { "reload", "reload_long", nullptr } },
            { "models/weapons/v_glock.mdl", { "gun_reload", "gun_reload_empty", nullptr } },
            { "models/weapons/v_mp5.mdl", { "reload", "reload_long", nullptr } },
        };
        const char* const* seqs = nullptr;
        for (const auto& g : kGuns)
            if (StudioHdrMatchesModel(hdr, g.model))
                seqs = g.seqs;
        if (!seqs)
            return;
        int numSeq = 0, seqIndex = 0;
        BMVR_TRY
        {
            numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq);
            seqIndex = *reinterpret_cast<int*>(hdr + kStudioHdrLocalSeqIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (numSeq <= 0 || numSeq > 256 || seqIndex <= 0)
            return;
        int changed = 0;
        for (int s = 0; s < numSeq; ++s)
        {
            const char* label = StudioSequenceLabel(hdr, s);
            bool want = false;
            for (int k = 0; label && k < 3 && seqs[k]; ++k)
                want = want || _stricmp(label, seqs[k]) == 0;
            if (!want)
                continue;
            unsigned char* desc = hdr + seqIndex + s * kStudioSeqDescSize;
            int numEv = 0, evIndex = 0;
            BMVR_TRY
            {
                numEv = *reinterpret_cast<int*>(desc + kStudioSeqNumEvents);
                evIndex = *reinterpret_cast<int*>(desc + kStudioSeqEventIndex);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            if (numEv <= 0 || numEv > 64 || evIndex <= 0)
                continue;
            for (int e = 0; e < numEv; ++e)
            {
                unsigned char* ev = desc + evIndex + e * kStudioEventSize;
                float cycle = 0.f;
                bool sound = false;
                BMVR_TRY
                {
                    cycle = *reinterpret_cast<float*>(ev);
                    const int type = *reinterpret_cast<int*>(ev + 8);
                    const int nameOff = *reinterpret_cast<int*>(ev + kStudioEventNameIndex);
                    sound = (type & kStudioEventNewStyle) && nameOff > 0
                        && std::strncmp(reinterpret_cast<const char*>(ev + nameOff), "AE_CL_PLAYSOUND", 16) == 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    continue;
                }
                if (!sound)
                    continue;
                int* p = reinterpret_cast<int*>(ev);
                if (mute && cycle <= 1.f)
                {
                    RememberHdrPatch(p, FloatBits(cycle), FloatBits(kMutedEventCycle), hdr);
                    if (PokeInt(p, FloatBits(kMutedEventCycle)))
                        ++changed;
                }
                else if (!mute && cycle == kMutedEventCycle)
                {
                    for (int i = 0; i < g_ArmHdrPatchCount; ++i)
                    {
                        if (g_ArmHdrPatches[i].p == p)
                        {
                            if (HdrPatchStillOurs(g_ArmHdrPatches[i]) && PokeInt(p, g_ArmHdrPatches[i].original))
                                ++changed;
                            g_ArmHdrPatches[i].p = nullptr;
                            break;
                        }
                    }
                }
            }
        }
        char name[65]{};
        BMVR_TRY { std::memcpy(name, hdr + 12, 64); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        Game::logMsg("Mag reload: stock reload sound events %s on %s (%d)", mute ? "muted" : "restored", name, changed);
    }

    int WeaponRootScore(const char* n)
    {
        if (!n || !n[0])
            return -1;
        if (_stricmp(n, "R_Arm") == 0 || _stricmp(n, "L_Arm") == 0)
            return -1;
        if (std::strstr(n, "SCI_Hand") || std::strstr(n, "gman_arms"))
            return -1;
        char buf[64]{};
        strncpy_s(buf, n, _TRUNCATE);
        for (char* c = buf; *c; ++c)
            *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        if (std::strstr(buf, "projectile") || std::strstr(buf, "bullet")
            || std::strstr(buf, "clip") || std::strstr(buf, "trigger")
            || std::strstr(buf, "shell") || std::strstr(buf, "chamber")
            || std::strstr(buf, "loader") || std::strstr(buf, "eject")
            || std::strstr(buf, "muzzle") || std::strstr(buf, "bolt")
            || std::strstr(buf, "pump") || std::strstr(buf, "screen"))
            return 0;
        if (std::strstr(buf, "crowbar") || std::strstr(buf, "bone_gun")
            || std::strcmp(buf, "357") == 0 || std::strstr(buf, "mp5")
            || std::strstr(buf, "v_rpg") || std::strstr(buf, "tau")
            || std::strstr(buf, "egon") || std::strstr(buf, "gauss")
            || std::strstr(buf, "glock") || std::strstr(buf, "shotgun")
            || std::strstr(buf, "wrench") || std::strstr(buf, "hgun")
            || std::strstr(buf, "hive") || std::strcmp(buf, "mainbody") == 0)
            return 10;
        return 1;
    }

    // The weapon is its own parentless root bone (not attached to the arms);
    // picked by name (WeaponRootScore). Returns its index or -1.
    int FindWeaponRootBone(unsigned char* hdr, float rest[3], char (&bestName)[64])
    {
        if (!hdr)
            return -1;
        int length = 0;
        int numbones = 0;
        int boneindex = 0;
        BMVR_TRY
        {
            length = *reinterpret_cast<int*>(hdr + kStudioHdrLength);
            numbones = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones);
            boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        if (length < 256 || length > 4 * 1024 * 1024)
            return -1;
        if (numbones <= 0 || numbones > 256)
            return -1;
        if (boneindex < 0 || boneindex + numbones * kStudioBoneSize > length)
            return -1;

        int bestScore = -1;
        int bestIndex = -1;
        for (int i = 0; i < numbones; ++i)
        {
            unsigned char* bone = hdr + boneindex + i * kStudioBoneSize;
            int parent = -2;
            int szname = 0;
            float pos[3]{};
            BMVR_TRY
            {
                szname = *reinterpret_cast<int*>(bone);
                parent = *reinterpret_cast<int*>(bone + 4);
                pos[0] = *reinterpret_cast<float*>(bone + 32);
                pos[1] = *reinterpret_cast<float*>(bone + 36);
                pos[2] = *reinterpret_cast<float*>(bone + 40);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            if (parent != -1)
                continue;
            const char* n = StudioRelString(bone, szname);
            const int score = WeaponRootScore(n);
            if (score > bestScore)
            {
                bestScore = score;
                bestIndex = i;
                rest[0] = pos[0];
                rest[1] = pos[1];
                rest[2] = pos[2];
                strncpy_s(bestName, n, _TRUNCATE);
            }
        }
        if (bestScore < 1 || bestIndex < 0)
            return -1;
        return bestIndex;
    }

    void NoteWeaponRootFromHdr(unsigned char* hdr, const char* modelName)
    {
        if (!hdr || !Hooks::m_VR)
            return;
        float rest[3]{};
        char name[64]{};
        if (FindWeaponRootBone(hdr, rest, name) < 0)
            return;
        Hooks::m_VR->NoteViewmodelWeaponBake(modelName, name, rest[0], rest[1], rest[2]);
    }

    struct HideArmsResult
    {
        int bodyparts = 0;
        int armsPart = -1;
        int armsModels = 0;
        int selected = -1;
        int meshesZeroed = 0;
        int alreadyZero = 0;
        char armsName[32]{};
        char gunName[64]{};
        int hwLods = -1;
        int hwMats = -1;
    };

    HideArmsResult InspectAndHideArmBodypart(void* state, int body, bool hideMeshes)
    {
        HideArmsResult r{};
        unsigned char* hdr = ResolveStudioHdr(state);
        if (!hdr)
            return r;

        if (state)
        {
            BMVR_TRY
            {
                auto* st = reinterpret_cast<DrawModelStateLite*>(state);
                int* hw = reinterpret_cast<int*>(st->studioHWData);
                if (hw)
                {
                    r.hwLods = hw[1];
                    if (r.hwLods > 0 && r.hwLods <= 8 && hw[2])
                    {
                        unsigned char* lod0 = reinterpret_cast<unsigned char*>(
                            static_cast<uintptr_t>(hw[2]));
                        r.hwMats = *reinterpret_cast<int*>(lod0 + 8);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        int length = 0;
        int numbody = 0;
        int bodyindex = 0;
        BMVR_TRY
        {
            length = *reinterpret_cast<int*>(hdr + kStudioHdrLength);
            numbody = *reinterpret_cast<int*>(hdr + kStudioHdrNumBodyparts);
            bodyindex = *reinterpret_cast<int*>(hdr + kStudioHdrBodypartIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return r;
        }
        if (length < 256 || length > 4 * 1024 * 1024)
            return r;
        if (numbody <= 0 || numbody > 16)
            return r;
        if (bodyindex < 0 || bodyindex + numbody * kStudioBodypartSize > length)
            return r;
        r.bodyparts = numbody;

        for (int bi = 0; bi < numbody; ++bi)
        {
            unsigned char* bp = hdr + bodyindex + bi * kStudioBodypartSize;
            int szname = 0;
            int nummodels = 0;
            int base = 1;
            int modelindex = 0;
            BMVR_TRY
            {
                szname = *reinterpret_cast<int*>(bp);
                nummodels = *reinterpret_cast<int*>(bp + 4);
                base = *reinterpret_cast<int*>(bp + 8);
                modelindex = *reinterpret_cast<int*>(bp + 12);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            const char* bpName = StudioRelString(bp, szname);
            if (bi == 0)
                strncpy_s(r.gunName, bpName, _TRUNCATE);
            // Black Mesa names the arms part "arms". Black Mesa: Blue Shift's satchel
            // splits it into "l_arm" (the hand holding the bag) + "r_arm" (detonator
            // hand), and its grenade / RPG add "r_arm_clavicle" / "l_arm_clavicle";
            // left drawn, the satchel showed a second left hand on the right
            // controller (headless bs_c3m1b, 2026-11-18). No Black Mesa viewmodel
            // uses these names.
            if (_stricmp(bpName, "arms") != 0 && _stricmp(bpName, "l_arm") != 0 && _stricmp(bpName, "r_arm") != 0
                && _stricmp(bpName, "l_arm_clavicle") != 0 && _stricmp(bpName, "r_arm_clavicle") != 0)
                continue;
            r.armsPart = bi;
            r.armsModels = nummodels;
            strncpy_s(r.armsName, bpName, _TRUNCATE);
            if (nummodels <= 0 || nummodels > 16 || base <= 0)
                continue;
            r.selected = (body / base) % nummodels;
            if (modelindex < 0 || bp + modelindex + nummodels * kStudioModelSize > hdr + length)
                continue;
            for (int mi = 0; mi < nummodels; ++mi)
            {
                unsigned char* model = bp + modelindex + mi * kStudioModelSize;
                int* pMeshes = reinterpret_cast<int*>(model + kStudioModelNumMeshes);
                int nMesh = 0;
                BMVR_TRY { nMesh = *pMeshes; }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (nMesh < 0 || nMesh > 64)
                    continue;
                if (nMesh == 0)
                {
                    ++r.alreadyZero;
                    continue;
                }
                if (!hideMeshes)
                    continue;
                RememberHdrPatch(pMeshes, nMesh, 0, hdr);
                if (PokeInt(pMeshes, 0))
                    ++r.meshesZeroed;
            }
        }
        return r;
    }

    void ApplyViewmodelStudioWork(void* state, const char* modelName, int body, int skin,
        bool hideArms, void* pCustomBoneToWorld)
    {
        unsigned char* hdr = ResolveStudioHdr(state);
        if (!hdr)
        {
            static int s_noHdr;
            if (s_noHdr < 4)
            {
                Game::logMsg("HideViewmodelArms no studiohdr %s", modelName ? modelName : "?");
                ++s_noHdr;
            }
        }
        NoteWeaponRootFromHdr(hdr, modelName);
        if (hdr)
        {
            g_LastViewmodelStudioHdr = hdr;
            ApplyFidgetSoundMute(hdr);
            Apply357BrassMute(hdr);
            ApplyStockReloadSoundMute(hdr);
        }
        static char s_lastVm[260]{};
        if (modelName && modelName[0] && _stricmp(s_lastVm, modelName) != 0)
        {
            strncpy_s(s_lastVm, modelName, _TRUNCATE);
            // hdr first so no reader matches a header whose seq is being reset.
            g_LastViewmodelIdleHdr = nullptr;
            g_LastViewmodelIdleSeq = -1;
        }
        const HideArmsResult hide = InspectAndHideArmBodypart(state, body, hideArms);
        static int s_armLog;
        if (s_armLog < 8 && (hideArms || hide.armsPart >= 0))
        {
            Game::logMsg(
                "HideViewmodelArms %s body=%d skin=%d parts=%d arms='%s' idx=%d/%d "
                "zeroed=%d already=%d hwLod=%d hwMat=%d hide=%d bones=%d",
                modelName ? modelName : "?", body, skin, hide.bodyparts,
                hide.armsName[0] ? hide.armsName : "?",
                hide.selected, hide.armsModels, hide.meshesZeroed, hide.alreadyZero,
                hide.hwLods, hide.hwMats, hideArms ? 1 : 0,
                pCustomBoneToWorld ? 1 : 0);
            ++s_armLog;
        }
    }

    void RestoreArmBodypartMeshes()
    {
        int n = 0, stale = 0;
        for (int i = 0; i < g_ArmHdrPatchCount; ++i)
        {
            StudioHdrPatch& e = g_ArmHdrPatches[i];
            if (e.p)
            {
                if (HdrPatchStillOurs(e))
                {
                    PokeInt(e.p, e.original);
                    ++n;
                }
                else
                    ++stale;
            }
            e = {};
        }
        if (g_ArmHdrPatchCount)
            Game::logMsg("HideViewmodelArms restored %d studiohdr patches (%d left alone: header gone or changed)", n, stale);
        g_ArmHdrPatchCount = 0;
        // The event moves above were undone too: walk those headers again.
        std::memset(g_FidgetHdrs, 0, sizeof(g_FidgetHdrs));
        std::memset(g_BrassHdrs, 0, sizeof(g_BrassHdrs));
        std::memset(g_ReloadHdrs, 0, sizeof(g_ReloadHdrs));
    }

    struct SourceRenderQueueBuildScope
    {
        VR* vr = nullptr;
        bool active = false;
        SourceRenderQueueBuildScope(VR* owner, bool enabled)
            : vr(owner), active(enabled && owner != nullptr)
        {
            if (active)
            {
                std::lock_guard<std::recursive_mutex> consumerGate(vr->m_SourceRenderConsumerGate);
                vr->m_SourceRenderQueueBuildCount.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        ~SourceRenderQueueBuildScope()
        {
            if (active)
            {
                std::lock_guard<std::recursive_mutex> consumerGate(vr->m_SourceRenderConsumerGate);
                vr->m_SourceRenderQueueBuildCount.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        SourceRenderQueueBuildScope(const SourceRenderQueueBuildScope&) = delete;
        SourceRenderQueueBuildScope& operator=(const SourceRenderQueueBuildScope&) = delete;
    };

    // Only the HDR scene color buffer. Redirecting 8192 CSM/flashlight
    // targets into the eye RT (and keeping their 8K depth) died on the first
    // named-RT stereo frame (2026-08-17, `_rt_gbshadowmaprt`).
    bool IsStereoSceneColorTarget(ITexture* texture)
    {
        const char* name = SafeTextureName(texture);
        if (!name || !name[0] || name[0] == '?')
            return false;
        if (std::strstr(name, "shadow") || std::strstr(name, "Shadow"))
            return false;
        if (std::strstr(name, "flashlight") || std::strstr(name, "Flashlight"))
            return false;
        if (std::strstr(name, "csm") || std::strstr(name, "CSM"))
            return false;
        if (std::strstr(name, "water") || std::strstr(name, "Water"))
            return false;
        if (std::strstr(name, "depth") || std::strstr(name, "Depth"))
            return false;
        // HUD PushRT at RenderView 1020fa90 uses FindTexture "_rt_gui"
        // (IMaterialSystem +0x150) then context PushRT +0x23C. Leave it.
        if (std::strstr(name, "_rt_gui"))
            return false;
        if (std::strstr(name, "bmvrHUD") || std::strstr(name, "vrHUD"))
            return false;
        return std::strstr(name, "_rt_FullFrameFB") != nullptr;
    }

    // Isolated for MSVC: __try cannot live in a function with C++ unwind objects.
    IMatRenderContext* GetMatRenderContextFromMatsys()
    {
        if (!Hooks::m_Game || !Hooks::m_Game->m_MaterialSystem)
            return nullptr;
        void* mat = Hooks::m_Game->m_MaterialSystem;
        IMatRenderContext* ctx = nullptr;
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

    void ContextRelease(IMatRenderContext* ctx)
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

    // client.dll is loaded once by the engine and stays until shutdown.
    // GetModuleHandleA takes the loader lock and walks the module list;
    // resolve it once for the per-frame callers below.
    HMODULE ClientModule()
    {
        static HMODULE s_client = nullptr;
        if (!s_client)
            s_client = GetModuleHandleA("client.dll");
        return s_client;
    }

    void CallSetAbsOriginAngles(void* ent, const float* origin, const float* angles)
    {
        if (!ent || !origin || !angles)
            return;
        HMODULE client = ClientModule();
        if (!client)
            return;
        using SetVecFn = void(__thiscall*)(void*, const float*);
        auto* originCode = reinterpret_cast<const unsigned char*>(client) + Offsets::kCBaseEntity_SetAbsOrigin;
        auto* anglesCode = reinterpret_cast<const unsigned char*>(client) + Offsets::kCBaseEntity_SetAbsAngles;
        // Both start by comparing the new vector against the abs field
        // (ucomiss [esi+0x294] / [esi+0x2D0]). A stale RVA after a client
        // rebuild lands mid-function; do not call it.
        static int s_verified = -1;
        if (s_verified < 0)
        {
            static const unsigned char kOrigin[] = { 0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08, 0x8B, 0xF1,
                0xF3, 0x0F, 0x10, 0x07, 0x0F, 0x2E, 0x86, 0x94, 0x02, 0x00, 0x00 };
            static const unsigned char kAngles[] = { 0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08, 0x8B, 0xF1,
                0xF3, 0x0F, 0x10, 0x07, 0x0F, 0x2E, 0x86, 0xD0, 0x02, 0x00, 0x00 };
            s_verified = (memcmp(originCode, kOrigin, sizeof(kOrigin)) == 0
                && memcmp(anglesCode, kAngles, sizeof(kAngles)) == 0) ? 1 : 0;
            Game::logMsg("SetAbsOrigin/SetAbsAngles rva=0x%X/0x%X %s (origin %02X %02X %02X %02X %02X, angles %02X %02X %02X %02X %02X)",
                Offsets::kCBaseEntity_SetAbsOrigin, Offsets::kCBaseEntity_SetAbsAngles,
                s_verified ? "verified" : "bytes mismatch; viewmodel stays on the engine pose",
                originCode[0], originCode[1], originCode[2], originCode[3], originCode[4],
                anglesCode[0], anglesCode[1], anglesCode[2], anglesCode[3], anglesCode[4]);
            if (!s_verified)
                Game::noteMissingOffset("SetAbsOrigin/SetAbsAngles");
        }
        if (!s_verified)
            return;
        auto setOrigin = reinterpret_cast<SetVecFn>(const_cast<unsigned char*>(originCode));
        auto setAngles = reinterpret_cast<SetVecFn>(const_cast<unsigned char*>(anglesCode));
        BMVR_TRY
        {
            setOrigin(ent, origin);
            setAngles(ent, angles);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool EntityLooksLikeLaserDot(C_BaseEntity* ent)
    {
        if (!ent || !Hooks::m_Game)
            return false;
        const char* cls = Hooks::m_Game->GetEntityClientClassName(ent);
        if (cls && cls[0] && (std::strstr(cls, "EnvLaserDot") || std::strstr(cls, "LaserDot")))
            return true;
        const char* model = Hooks::m_Game->GetEntityModelName(ent);
        if (model && model[0] && (std::strstr(model, "laserdot") || std::strstr(model, "laser_dot")))
            return true;
        return false;
    }

    bool LaserDotMaterialName(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strstr(name, "laserdot") != nullptr
            || std::strstr(name, "laser_dot") != nullptr;
    }

    bool ExactGlowDotMaterial(const char* name)
    {
        if (!name || !name[0])
            return false;
        const char* slash = std::strrchr(name, '/');
        const char* base = slash ? slash + 1 : name;
        return std::strcmp(base, "glow.vmt") == 0 || std::strcmp(base, "glow.spr") == 0;
    }

    bool RpgLaserWorldSpriteMaterial(const char* name)
    {
        if (LaserDotMaterialName(name) || ExactGlowDotMaterial(name))
            return true;
        if (!name || !name[0])
            return false;
        // bmvr_log: look-ray sprites/glow01.spr stay centred after glow.vmt
        // is moved. They hide during RPG reload with the weapon laser.
        // glow06 is map lights — never match that.
        return std::strstr(name, "glow01") != nullptr;
    }

    bool GlowSpriteMaterialName(const char* name)
    {
        if (!name || !name[0])
            return false;
        if (std::strstr(name, "glow06"))
            return true;
        return ExactGlowDotMaterial(name);
    }

    bool SameClientEntity(const void* a, const void* b)
    {
        if (!a || !b)
            return false;
        if (a == b)
            return true;
        const char* pa = static_cast<const char*>(a);
        const char* pb = static_cast<const char*>(b);
        return pa + 4 == pb || pa - 4 == pb;
    }

    bool SpriteEyeAndForward(Vector& eye, Vector& fwd)
    {
        if (!Hooks::m_VR)
            return false;
        Vector setup = Hooks::m_VR->m_HasStereoBodyOrigin
            ? Hooks::m_VR->m_StereoBodyOrigin
            : Hooks::m_VR->m_SetupOrigin;
        if (setup.LengthSqr() < 1.f)
            setup = Hooks::m_VR->m_SetupOrigin;
        if (setup.LengthSqr() < 1.f)
            return false;
        eye = Hooks::m_VR->GetViewOrigin(setup);
        Hooks::m_VR->GetViewBasis(&fwd, nullptr, nullptr);
        return VectorNormalize(fwd) > 0.01f;
    }

    float AimOffLookDegrees()
    {
        if (!Hooks::m_VR)
            return 0.f;
        Vector look{};
        Hooks::m_VR->GetViewBasis(&look, nullptr, nullptr);
        if (VectorNormalize(look) <= 0.01f)
            return 0.f;
        Vector aim{};
        QAngle::AngleVectors(Hooks::m_VR->GetAimAngles(), &aim, nullptr, nullptr);
        if (VectorNormalize(aim) <= 0.01f)
            return 0.f;
        float c = aim.Dot(look);
        if (c > 1.f) c = 1.f;
        if (c < -1.f) c = -1.f;
        return acosf(c) * (180.f / 3.14159265f);
    }

    float PointOffCurrentViewDegrees(const float* origin)
    {
        if (!origin || !g_HaveCurView)
            return 180.f;
        Vector dir(origin[0] - g_CurViewOrigin.x,
            origin[1] - g_CurViewOrigin.y,
            origin[2] - g_CurViewOrigin.z);
        if (VectorNormalize(dir) <= 0.01f)
            return 180.f;
        float c = dir.Dot(g_CurViewForward);
        if (c > 1.f) c = 1.f;
        if (c < -1.f) c = -1.f;
        return acosf(c) * (180.f / 3.14159265f);
    }

    float PointOffLookDegrees(const float* origin)
    {
        if (!origin)
            return 180.f;
        Vector pos(origin[0], origin[1], origin[2]);
        Vector eye{};
        Vector fwd{};
        if (!SpriteEyeAndForward(eye, fwd))
            return 180.f;
        Vector dir = pos - eye;
        if (VectorNormalize(dir) <= 0.01f)
            return 180.f;
        float c = dir.Dot(fwd);
        if (c > 1.f) c = 1.f;
        if (c < -1.f) c = -1.f;
        return acosf(c) * (180.f / 3.14159265f);
    }

    bool SpriteOriginOnLookRay(const float* origin)
    {
        if (!origin)
            return false;
        Vector pos(origin[0], origin[1], origin[2]);
        if (pos.LengthSqr() < 1.f)
            return false;
        Vector eye{};
        Vector fwd{};
        if (!SpriteEyeAndForward(eye, fwd))
            return false;
        const Vector delta = pos - eye;
        const float along = delta.Dot(fwd);
        if (along < 48.f)
            return false;
        const Vector closest = eye + fwd * along;
        const Vector off = pos - closest;
        return off.LengthSqr() < (72.f * 72.f);
    }

    // Camera-locked glow sits a few dozen hu in front of the HMD. Look-ray
    // required along>=48, so those sprites logged look=0 and were left stuck
    // on centre while unrelated world glow06 lights were rewritten into the
    // giant blue blob.
    bool SpriteOriginNearEye(const float* origin)
    {
        if (!origin)
            return false;
        Vector pos(origin[0], origin[1], origin[2]);
        Vector eye{};
        Vector fwd{};
        if (!SpriteEyeAndForward(eye, fwd))
            return false;
        const Vector delta = pos - eye;
        const float dist = delta.Length();
        if (dist < 1.f || dist > 120.f)
            return false;
        const float along = delta.Dot(fwd);
        if (along < 0.5f || along > 120.f)
            return false;
        const Vector closest = eye + fwd * along;
        return (pos - closest).LengthSqr() < (40.f * 40.f);
    }

    // GetActiveWeaponModelName is local player -> weapon handle -> entity list
    // -> model info per call, and these predicates run per sprite quad / beam
    // segment (GluonFxLive from dSpriteQuad). Resolve once per Present.
    struct HeldWeaponCache
    {
        uint32_t tick = 0xffffffffu;
        bool rpg = false;
        bool gluon = false;
    };
    // thread_local: sprite/beam hooks run on the render thread while
    // CreateMove-side callers run on the main thread; one cheap resolve per
    // thread per Present beats a lock here.
    thread_local HeldWeaponCache g_HeldWeapon;

    const HeldWeaponCache& RefreshHeldWeaponCache()
    {
        const uint32_t tick = Hooks::m_VR ? Hooks::m_VR->m_PresentTick : 0u;
        if (g_HeldWeapon.tick != tick)
        {
            g_HeldWeapon.tick = tick;
            const char* active = Hooks::m_Game ? Hooks::m_Game->GetActiveWeaponModelName() : nullptr;
            g_HeldWeapon.rpg = VR::IsRpgWeaponModel(active);
            g_HeldWeapon.gluon = VR::IsGluonWeaponModel(active);
        }
        return g_HeldWeapon;
    }

    bool RpgWeaponHeld()
    {
        if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
            return false;
        if (Hooks::m_VR->RpgLaserActive() || Hooks::m_VR->RpgLaserLatched())
            return true;
        if (Hooks::m_VR->ViewmodelIsRpg())
            return true;
        return RefreshHeldWeaponCache().rpg;
    }

    bool GluonWeaponHeld()
    {
        if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
            return false;
        if (Hooks::m_VR->ViewmodelIsGluon())
            return true;
        return RefreshHeldWeaponCache().gluon;
    }

    void NoteGluonFx()
    {
        ++g_GluonFx;
        g_GluonFxUntilMs = GetTickCount() + 250;
    }

    void EndGluonFx()
    {
        if (g_GluonFx > 0)
            --g_GluonFx;
    }

    bool GluonFxLive()
    {
        if (g_GluonFx > 0)
            return true;
        if (GetTickCount() < g_GluonFxUntilMs)
            return true;
        return GluonWeaponHeld();
    }

    bool GluonGlowMaterialName(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strstr(name, "gluon_glow") != nullptr
            || std::strstr(name, "gluon_beam_end") != nullptr
            || std::strstr(name, "gluon_beam_burst") != nullptr;
    }

    void ApplyVrTraceHit(CGameTrace* trace, const Vector& start, const Vector& end, const Vector& n)
    {
        if (!trace)
            return;
        trace->startpos = start;
        trace->endpos = end;
        trace->startsolid = false;
        trace->allsolid = false;
        if (n.LengthSqr() > 0.01f)
        {
            trace->plane.normal = n;
            trace->plane.dist = n.x * end.x + n.y * end.y + n.z * end.z;
        }
        Vector delta = end - start;
        const float dist = delta.Length();
        constexpr float kMax = 16384.f;
        float frac = (dist > 1.f) ? (dist / kMax) : 1.f;
        if (frac > 1.f)
            frac = 1.f;
        if (frac < 0.f)
            frac = 0.f;
        trace->fraction = frac;
    }

    bool TryParkedFxWorld(Vector& parked)
    {
        if (!Hooks::m_VR)
            return false;
        if (Hooks::m_VR->TryGetRpgLaserWorld(parked))
            return true;
        Vector start{};
        return Hooks::m_VR->TryGetVrBeamSegment(start, parked);
    }

    bool RewriteToVrBeamEnd(Vector* p)
    {
        if (!p || !Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
            return false;
        Vector start{};
        Vector end{};
        Vector n{};
        if (!Hooks::m_VR->TryGetVrBeamSegment(start, end, &n))
            return false;
        p->x = end.x;
        p->y = end.y;
        p->z = end.z;
        return true;
    }

    const char* ParticleDefName(void* def)
    {
        if (!def)
            return nullptr;
        const char* name = nullptr;
        BMVR_TRY
        {
            name = *reinterpret_cast<const char**>(
                static_cast<char*>(def) + Offsets::kCParticleDef_Name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name = nullptr;
        }
        return name;
    }

    bool IsGluonImpactParticleName(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strncmp(name, "gluon_beam_end", 14) == 0;
    }

    void* ParticleEffectDef(void* effect)
    {
        if (!effect)
            return nullptr;
        void* def = nullptr;
        BMVR_TRY
        {
            def = *reinterpret_cast<void**>(
                static_cast<char*>(effect) + Offsets::kCNewParticleEffect_Def);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            def = nullptr;
        }
        return def;
    }

    bool ClearParticleViewModelEffect(void* def)
    {
        if (!def)
            return false;
        bool changed = false;
        BMVR_TRY
        {
            unsigned char* flag = reinterpret_cast<unsigned char*>(def)
                + Offsets::kCParticleDef_ViewModelEffect;
            if (*flag)
            {
                *flag = 0;
                changed = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return changed;
    }

    bool EffectIsViewModel(void* effect)
    {
        if (!effect)
            return false;
        BMVR_TRY
        {
            return *(static_cast<unsigned char*>(effect) + Offsets::kCNewParticleEffect_ViewModel) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ClearEffectViewModelFlag(void* effect)
    {
        if (!effect)
            return;
        BMVR_TRY
        {
            *(static_cast<unsigned char*>(effect) + Offsets::kCNewParticleEffect_ViewModel) = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool CodeBytesMatch(const unsigned char* p, const char* hex);   // defined with the weapon hooks below

    void PatchGluonImpactParticleDefs()
    {
        if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
            return;
        if (!Offsets::kParticleSystemMgrVerified)
            return;
        HMODULE client = ClientModule();
        if (!client)
            return;
        static const char* const kNames[] = {
            "gluon_beam_end",
            "gluon_beam_end_small_particles",
            "gluon_beam_end_supercharge",
            "gluon_beam_end_small_particles_supercharge",
            "gluon_beam_end_small_particles_a_supercharge",
            "gluon_beam_end_thirdperson",
            "gluon_beam_end_small_particles_thirdperson",
        };
        using tFind = void*(__thiscall*)(void* thisptr, const char* name);
        auto* findCode = reinterpret_cast<unsigned char*>(client) + Offsets::kParticleSystemMgr_Find;
        // FindParticleSystem: push ebp; mov ebp,esp; push ecx; push esi; mov esi,[ecx+0x8C]. Both RVAs were
        // stale for several builds and this called into the middle of another function.
        static bool s_findBad;
        if (s_findBad)
            return;
        if (!CodeBytesMatch(findCode, "558BEC51568BB18C000000"))
        {
            Game::logMsg("particle FindParticleSystem skipped (bytes/rva 0x%X)", Offsets::kParticleSystemMgr_Find);
            Game::noteMissingOffset("particle FindParticleSystem");
            s_findBad = true;
            return;
        }
        auto* find = reinterpret_cast<tFind>(findCode);
        void* mgr = nullptr;
        BMVR_TRY
        {
            mgr = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(client) + Offsets::kParticleSystemMgr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            mgr = nullptr;
        }
        if (!mgr)
            return;
        static int s_patched;
        for (const char* name : kNames)
        {
            void* def = nullptr;
            BMVR_TRY
            {
                def = find(mgr, name);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                def = nullptr;
            }
            if (ClearParticleViewModelEffect(def) && s_patched < 12)
            {
                Game::logMsg("gluon particle world-pass %s", name);
                ++s_patched;
            }
        }
    }

    void ClearEntityNodraw(C_BaseEntity* ent)
    {
        if (!ent)
            return;
        BMVR_TRY
        {
            int* fx = reinterpret_cast<int*>(
                reinterpret_cast<char*>(ent) + Offsets::kCBaseEntity_fEffects);
            *fx &= ~Offsets::kEF_NODRAW;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    C_BaseEntity* FindRpgLaserDot();

    void DumpRpgLaserEntitiesOnce()
    {
        static bool s_wasOn;
        static int s_dumps;
        const bool on = Hooks::m_VR && Hooks::m_VR->RpgLaserActive();
        const bool rising = on && !s_wasOn;
        s_wasOn = on;
        if (!rising || s_dumps >= 4 || !Hooks::m_Game || !Hooks::m_Game->m_ClientEntityList)
            return;
        ++s_dumps;
        C_BaseEntity* laserDot = FindRpgLaserDot();
        if (laserDot)
            Game::logMsg("RPG EnvLaserDot ptr=%p class=%s", laserDot,
                Hooks::m_Game->GetEntityClientClassName(laserDot)
                    ? Hooks::m_Game->GetEntityClientClassName(laserDot) : "?");
        else
            Game::logMsg("RPG EnvLaserDot not in client list");
        const int hi = Hooks::m_Game->m_ClientEntityList->GetHighestEntityIndex();
        int n = 0;
        int glow06 = 0;
        for (int e = 1; e <= hi && n < 40; ++e)
        {
            C_BaseEntity* ent = Hooks::m_Game->GetClientEntity(e);
            if (!ent)
                continue;
            const char* cls = Hooks::m_Game->GetEntityClientClassName(ent);
            const char* net = Hooks::m_Game->GetEntityNetworkName(e);
            const char* model = Hooks::m_Game->GetEntityModelName(ent);
            const bool priority = (cls && (std::strstr(cls, "Laser") || std::strstr(cls, "Beam")
                    || std::strstr(cls, "Particle")))
                || (net && (std::strstr(net, "Laser") || std::strstr(net, "Beam")))
                || (model && (std::strstr(model, "laser") || std::strstr(model, "glow.vmt")
                    || std::strstr(model, "glow01") || std::strstr(model, "laserdot")));
            const bool glow06Hit = model && std::strstr(model, "glow06");
            const bool hit = priority
                || (cls && std::strstr(cls, "Sprite"))
                || (net && std::strstr(net, "Sprite"))
                || (model && std::strstr(model, "glow"));
            if (!hit)
                continue;
            if (glow06Hit)
            {
                if (glow06 >= 6)
                    continue;
                ++glow06;
            }
            Game::logMsg("RPG ent[%d] class=%s net=%s model=%s", e,
                cls ? cls : "?", net ? net : "?", model ? model : "?");
            ++n;
        }
        if (n == 0)
            Game::logMsg("RPG laser: no Sprite/Laser/Beam entities in client list");
    }

    bool QueryContextColorRtSize(void* ctx, int& w, int& h, const char** nameOut);

    C_BaseEntity* g_rpgLaserDotEnt = nullptr;
    thread_local bool g_EnvLaserDotQuad = false;

    bool RpgLaserSpriteCandidate(C_BaseEntity* entity, const char* name, const float* origin)
    {
        if (!Hooks::m_VR || !Hooks::m_VR->RpgLaserActive())
            return false;
        const bool classDot = EntityLooksLikeLaserDot(entity);
        const bool materialDot = LaserDotMaterialName(name);
        const bool glowDot = RpgLaserWorldSpriteMaterial(name);
        const bool cachedDot = SameClientEntity(entity, g_rpgLaserDotEnt);
        const bool look = SpriteOriginOnLookRay(origin);
        return classDot || materialDot || cachedDot || (glowDot && look);
    }

    void LogRpgLaserSpriteSkip(const char* path, const char* name, const char* cls, const float* origin)
    {
        static int s_skip;
        if (s_skip >= 12)
            return;
        const char* rtName = "";
        int rtW = 0, rtH = 0;
        QueryContextColorRtSize(g_MatCtx, rtW, rtH, &rtName);
        Vector parked{};
        const int haveParked = (Hooks::m_VR && Hooks::m_VR->TryGetRpgLaserWorld(parked)) ? 1 : 0;
        Game::logMsg(
            "RPG laser HIDE %s model=%s class=%s at=(%.0f,%.0f,%.0f) offLook=%.1fdeg aimOff=%.1fdeg parked=%d (%.0f,%.0f,%.0f) rt=%s",
            path, name ? name : "?", cls ? cls : "?",
            origin ? origin[0] : 0.f, origin ? origin[1] : 0.f, origin ? origin[2] : 0.f,
            PointOffLookDegrees(origin), AimOffLookDegrees(),
            haveParked, parked.x, parked.y, parked.z,
            rtName && rtName[0] ? rtName : "?");
        ++s_skip;
    }

    // Entity index the cached dot was found at, and the last full scan tick.
    int g_rpgLaserDotIndex = 0;
    DWORD g_rpgLaserDotScanMs = 0;

    C_BaseEntity* FindRpgLaserDot()
    {
        if (!Hooks::m_Game || !Hooks::m_Game->m_ClientEntityList)
            return nullptr;
        // Called from applyL4d2VrHead, i.e. per eye per frame while the laser
        // is on. The old path was three full entity-list scans (lowercasing
        // every network name) plus a fourth by class/model, every call.
        // Re-validate the cached entity at its index first; rescan at most
        // every 250 ms when it is gone.
        if (g_rpgLaserDotEnt && g_rpgLaserDotIndex > 0)
        {
            C_BaseEntity* at = Hooks::m_Game->GetClientEntity(g_rpgLaserDotIndex);
            if (at && SameClientEntity(at, g_rpgLaserDotEnt) && EntityLooksLikeLaserDot(at))
                return g_rpgLaserDotEnt;
            g_rpgLaserDotEnt = nullptr;
            g_rpgLaserDotIndex = 0;
        }
        const DWORD nowMs = GetTickCount();
        if (g_rpgLaserDotScanMs != 0 && nowMs - g_rpgLaserDotScanMs < 250)
            return nullptr;
        g_rpgLaserDotScanMs = nowMs ? nowMs : 1;

        // One pass: network name (EnvLaserDot / laserdot / laser_dot), then
        // class / model as the old fallback scan did.
        const int hi = Hooks::m_Game->m_ClientEntityList->GetHighestEntityIndex();
        C_BaseEntity* fallback = nullptr;
        int fallbackIdx = 0;
        for (int e = 1; e <= hi; ++e)
        {
            const char* net = Hooks::m_Game->GetEntityNetworkName(e);
            if (net && net[0])
            {
                char lower[80]{};
                size_t i = 0;
                for (; i + 1 < sizeof(lower) && net[i]; ++i)
                    lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(net[i])));
                lower[i] = 0;
                if (std::strstr(lower, "envlaserdot") || std::strstr(lower, "laserdot")
                    || std::strstr(lower, "laser_dot"))
                {
                    C_BaseEntity* dot = Hooks::m_Game->GetClientEntity(e);
                    if (dot)
                    {
                        g_rpgLaserDotEnt = dot;
                        g_rpgLaserDotIndex = e;
                        return dot;
                    }
                }
            }
            if (!fallback)
            {
                C_BaseEntity* ent = Hooks::m_Game->GetClientEntity(e);
                if (EntityLooksLikeLaserDot(ent))
                {
                    fallback = ent;
                    fallbackIdx = e;
                }
            }
        }
        g_rpgLaserDotEnt = fallback;
        g_rpgLaserDotIndex = fallbackIdx;
        return fallback;
    }

    void SnapRpgLaserDot()
    {
        if (!Hooks::m_VR || !Hooks::m_Game || !Hooks::m_VR->m_IsVREnabled)
            return;
        if (!Hooks::m_VR->IsGameplayEligible() || !Hooks::m_VR->RpgLaserActive())
            return;
        Vector end{};
        if (!Hooks::m_VR->TryGetRpgLaserWorld(end))
            return;
        DumpRpgLaserEntitiesOnce();
        C_BaseEntity* dot = FindRpgLaserDot();
        if (!dot)
            return;
        ClearEntityNodraw(dot);
        const float origin3[3] = { end.x, end.y, end.z };
        const float angles3[3] = { 0.f, 0.f, 0.f };
        CallSetAbsOriginAngles(dot, origin3, angles3);
    }

    bool BeamAlongLook(const Vector& start, const Vector& delta)
    {
        Vector eye{};
        Vector fwd{};
        if (!SpriteEyeAndForward(eye, fwd))
            return false;
        const float dLen = delta.Length();
        if (dLen < 8.f)
            return false;
        Vector dN = delta;
        VectorNormalize(dN);
        if (dN.Dot(fwd) < 0.92f)
            return false;
        const Vector fromEye = start - eye;
        const float along = fromEye.Dot(fwd);
        const Vector closest = eye + fwd * along;
        const float off = (start - closest).Length();
        // Start at the eye, or at the look-hit (short end-cap beam).
        return off < 80.f && along > -48.f;
    }

    void RetargetRpgLaserBeam(void* beam)
    {
        if (!beam || !Hooks::m_VR || !Hooks::m_VR->RpgLaserActive())
            return;
        Vector muzzle{};
        Vector hit{};
        if (!Hooks::m_VR->TryGetVrBeamSegment(muzzle, hit))
            return;
        float* start = nullptr;
        float* delta = nullptr;
        int type = -1;
        float r = 0.f, g = 0.f, b = 0.f;
        BMVR_TRY
        {
            type = *reinterpret_cast<int*>(static_cast<char*>(beam) + Offsets::kBeam_t_type);
            start = reinterpret_cast<float*>(static_cast<char*>(beam) + Offsets::kBeam_t_start);
            delta = reinterpret_cast<float*>(static_cast<char*>(beam) + Offsets::kBeam_t_delta);
            r = *reinterpret_cast<float*>(static_cast<char*>(beam) + Offsets::kBeam_t_r);
            g = *reinterpret_cast<float*>(static_cast<char*>(beam) + Offsets::kBeam_t_r + 4);
            b = *reinterpret_cast<float*>(static_cast<char*>(beam) + Offsets::kBeam_t_r + 8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (!start || !delta)
            return;
        Vector st(start[0], start[1], start[2]);
        Vector d(delta[0], delta[1], delta[2]);
        static int s_beamLog;
        if (s_beamLog < 16)
        {
            Game::logMsg("DrawBeam type=%d rgb=(%.2f,%.2f,%.2f) start=(%.0f,%.0f,%.0f) dlen=%.0f look=%d",
                type, r, g, b, st.x, st.y, st.z, d.Length(),
                BeamAlongLook(st, d) ? 1 : 0);
            ++s_beamLog;
        }
        if (!BeamAlongLook(st, d))
            return;
        Vector nd = hit - muzzle;
        start[0] = muzzle.x;
        start[1] = muzzle.y;
        start[2] = muzzle.z;
        delta[0] = nd.x;
        delta[1] = nd.y;
        delta[2] = nd.z;
        static int s_retarget;
        if (s_retarget < 8)
        {
            Game::logMsg("RPG laser beam -> muzzle=(%.0f,%.0f,%.0f) hit=(%.0f,%.0f,%.0f)",
                muzzle.x, muzzle.y, muzzle.z, hit.x, hit.y, hit.z);
            ++s_retarget;
        }
    }

    void SnapLocalViewmodelForFire()
    {
        if (!Hooks::m_VR || !Hooks::m_Game || !Hooks::m_VR->m_IsVREnabled)
            return;
        if (!Hooks::m_VR->m_ControllerPoseValid || !Hooks::m_VR->IsGameplayEligible())
            return;
        C_BaseEntity* vm = Hooks::m_Game->GetViewModelEntity();
        if (!vm)
            return;
        Vector body = Hooks::m_VR->m_HasStereoBodyOrigin ? Hooks::m_VR->m_StereoBodyOrigin : Hooks::m_VR->m_SetupOrigin;
        if (body.LengthSqr() <= 1.f)
            body = Hooks::m_VR->m_SetupOrigin;
        const Vector targetOrigin = Hooks::m_VR->GetRecommendedViewmodelAbsPos(body);
        const QAngle targetAng = Hooks::m_VR->GetRecommendedViewmodelAbsAngle();
        const float origin3[3] = { targetOrigin.x, targetOrigin.y, targetOrigin.z };
        const float angles3[3] = { targetAng.x, targetAng.y, targetAng.z };
        CallSetAbsOriginAngles(vm, origin3, angles3);
    }

    void CallCalcViewModelViewOriginal(void* ecx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
    {
        if (!Hooks::hkCalcViewModelView.fOriginal)
            return;
        Vector savedVel{};
        bool zeroVel = bmvr::g_DisableViewBob && owner;
        if (zeroVel)
        {
            BMVR_TRY
            {
                savedVel = *reinterpret_cast<Vector*>(reinterpret_cast<char*>(owner) + kLocalPlayerVecVelocity);
                *reinterpret_cast<Vector*>(reinterpret_cast<char*>(owner) + kLocalPlayerVecVelocity) = Vector(0.f, 0.f, 0.f);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                zeroVel = false;
            }
        }
        Hooks::hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
        if (zeroVel)
        {
            BMVR_TRY
            {
                *reinterpret_cast<Vector*>(reinterpret_cast<char*>(owner) + kLocalPlayerVecVelocity) = savedVel;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    void ContextSetRenderTarget(IMatRenderContext* ctx, ITexture* texture)
    {
        if (!ctx)
            return;
        BMVR_TRY
        {
            void** vt = *reinterpret_cast<void***>(ctx);
            if (!vt)
                return;
            using Fn = void(__thiscall*)(void*, ITexture*);
            auto fn = reinterpret_cast<Fn>(vt[Offsets::kIMatRenderContext_SetRenderTarget / 4]);
            if (fn)
                fn(ctx, texture);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // L4D2VR CRefPtr: GetRenderContext already AddRefs; we own that one ref.
    struct MatCtxScope
    {
        IMatRenderContext* ctx = nullptr;
        bool owned = false;

        MatCtxScope()
        {
            ctx = GetMatRenderContextFromMatsys();
            if (ctx)
                owned = true;
            else
                ctx = g_MatCtx;
        }

        ~MatCtxScope()
        {
            if (owned && ctx)
                ContextRelease(ctx);
        }

        MatCtxScope(const MatCtxScope&) = delete;
        MatCtxScope& operator=(const MatCtxScope&) = delete;
    };

    // L4D2VR EyeRenderTargetScope: original PushRT(eye)/PopRT around RenderView.
    // Fallback is BM-verified SetRT at context +0x18 (RenderView 1020f5e4).
    struct EyeRtPush
    {
        IMatRenderContext* ctx = nullptr;
        ITexture* oldRT = nullptr;
        int oldX = 0;
        int oldY = 0;
        int oldW = 0;
        int oldH = 0;
        bool hasViewport = false;
        bool pushed = false;

        EyeRtPush(IMatRenderContext* renderContext, ITexture* target, int width, int height)
            : ctx(renderContext)
        {
            if (!ctx || !target)
                return;
            if (Hooks::hkPushRenderTargetAndViewport.fOriginal
                && Hooks::hkPopRenderTargetAndViewport.fOriginal)
            {
                Hooks::hkPushRenderTargetAndViewport.fOriginal(
                    ctx, target, nullptr, 0, 0, width, height);
                pushed = true;
                return;
            }
            if (Hooks::hkGetRenderTarget.fOriginal)
                oldRT = Hooks::hkGetRenderTarget.fOriginal(ctx);
            if (Hooks::hkGetViewport.fOriginal && Hooks::hkViewport.fOriginal)
            {
                Hooks::hkGetViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);
                hasViewport = true;
            }
            ContextSetRenderTarget(ctx, target);
            if (Hooks::hkViewport.fOriginal)
                Hooks::hkViewport.fOriginal(ctx, 0, 0, width, height);
        }

        ~EyeRtPush()
        {
            if (!ctx)
                return;
            if (pushed)
            {
                if (Hooks::hkPopRenderTargetAndViewport.fOriginal)
                    Hooks::hkPopRenderTargetAndViewport.fOriginal(ctx);
                return;
            }
            ContextSetRenderTarget(ctx, oldRT);
            if (hasViewport && Hooks::hkViewport.fOriginal)
                Hooks::hkViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);
        }

        EyeRtPush(const EyeRtPush&) = delete;
        EyeRtPush& operator=(const EyeRtPush&) = delete;
    };

    void NormalizeViewSetupForVREye(CViewSetup& view, const VR* vr, int stereoEye)
    {
        // L4D2VR hooks_render.inl: eye copies are HMD size + HMD FOV + HMD
        // aspect. 16:9 engine FOV + center-crop (2026-08-18) zoomed the world
        // (objects huge), broke distance fusion (IPD still ~2.5in), and sat
        // the camera above NPC eye level. Do not write height into 0x1C
        // (that is m_eStereoEye on BM). Do not force zNear=6.
        const int eyeWidth = static_cast<int>(vr->m_RenderWidth);
        const int eyeHeight = static_cast<int>(vr->m_RenderHeight);
        if (eyeWidth < 640 || eyeHeight < 360)
            return;
        view.x = 0;
        view.y = 0;
        view.m_nUnscaledX = 0;
        view.m_nUnscaledY = 0;
        // HWND blit / gbmatch: keep engine 2560 width/height so deferred
        // flashlight apply matches the G-buffer. Still write HMD fov/aspect.
        if (!vr->ShouldHwndBlitStereo())
        {
            view.width = eyeWidth;
            view.height = eyeHeight;
            view.m_nUnscaledWidth = eyeWidth;
            view.m_nUnscaledHeight = eyeHeight;
        }
        // DrawViewModels (client FUN_1020a8f0) copies this setup, then
        // overwrites m_flAspectRatio with engine GetScreenAspectRatio()
        // (window 16:9). dGetScreenAspectRatio returns the eye aspect during
        // the stereo pass so the gun frustum matches world/gloves.
        const float rtAspect = static_cast<float>(eyeWidth) / static_cast<float>(eyeHeight);
        view.m_flAspectRatio = rtAspect;
        const float eyeFov = vr->WorldRenderFov();
        view.fov = eyeFov;
        view.fovViewmodel = eyeFov;
        // Off-centre eye (OffCenterEyes, and/or the left eye widened to the
        // right by LeftEyeCullPad): render exactly this eye's rectangle. The
        // engine builds it (ComputeViewMatrices, MatrixBuildPerspective-
        // OffCenterX) from fov + aspect + the 0..1 sub-rectangle; culling and
        // deferred lights follow the matrices (flat test 2026-10-11; widened
        // left eye fixed the far-right prop pop-in, HMD 2026-10-17). The
        // copy in DrawViewModels keeps the sub-rectangle.
        float ocFov = 0.f, ocAspect = 0.f, frac[4];
        if (vr->EyeOffCenterView(stereoEye, ocFov, ocAspect, frac))
        {
            view.fov = ocFov;
            view.fovViewmodel = ocFov;
            view.m_flAspectRatio = ocAspect;
            view.m_bOffCenter = true;
            view.m_flOffCenterLeft = frac[0];
            view.m_flOffCenterRight = frac[1];
            view.m_flOffCenterBottom = frac[2];
            view.m_flOffCenterTop = frac[3];
        }
        // L4D2VR: native Source viewmodels use a separate projection and a
        // compressed 0..0.1 depth range so they draw over the world. In VR the
        // gun is a world-space mesh; keep Z (and therefore size vs world
        // geometry) on the same projection as the eye scene.
        // Near clip (VrNearZ): BM's 7 units cut a gun held at the face for the
        // sights (user). Only lowers it; the gun shares this projection.
        if (bmvr::g_VrNearZ >= 1.f && bmvr::g_VrNearZ < view.zNear)
            view.zNear = bmvr::g_VrNearZ;
        if (view.zNear > 0.01f)
            view.zNearViewmodel = view.zNear;
        if (view.zFar > view.zNear)
            view.zFarViewmodel = view.zFar;
    }

    bool IsOffscreenWorldRtName(const char* name)
    {
        if (!name || !name[0] || name[0] == '?')
            return false;
        if (bmvr::EyeScaledRtClass(name) == bmvr::EyeRtClass::Scene)
            return true;
        if (std::strstr(name, "shadow") || std::strstr(name, "Shadow")
            || std::strstr(name, "flashlight") || std::strstr(name, "Flashlight")
            || std::strstr(name, "csm") || std::strstr(name, "CSM")
            || std::strstr(name, "Hud") || std::strstr(name, "gui")
            || std::strstr(name, "Dof") || std::strstr(name, "Water")
            || std::strstr(name, "Camera"))
            return false;
        return std::strstr(name, "_rt_FullFrame") != nullptr
            || std::strstr(name, "_rt_ResolvedFullFrame") != nullptr
            || std::strstr(name, "_rt_gb") != nullptr
            || std::strstr(name, "_rt_ls") != nullptr;
    }

    bool RtStackTopIsWorldScene()
    {
        if (g_RtStackDepth <= 0)
            return false;
        return g_RtStack[g_RtStackDepth - 1].world;
    }

    bool McViewportDestIsRing()
    {
        if (g_RtStackDepth <= 0)
            return true;
        const RtStackEntry& e = g_RtStack[g_RtStackDepth - 1];
        return e.name[0] == 0
            || std::strcmp(e.name, "backbuffer") == 0
            || std::strcmp(e.name, "null") == 0;
    }

    bool McLeftoverRecording()
    {
        return Hooks::m_VR
            && Hooks::m_VR->McPipelineEnabled()
            && Hooks::m_VR->m_StereoEye == 0
            && Hooks::m_VR->m_SourceRenderQueueBuildCount.load(std::memory_order_acquire) != 0;
    }

    bool WorldMatchEyeSizeLieAllowed()
    {
        if (!bmvr::OffscreenWorldMatchesEyes() || !Hooks::m_VR)
            return false;
        if (Hooks::m_VR->HudPaintActive() || Hooks::m_VR->Want2dMenuPanel())
            return false;
        if (Hooks::m_VR->m_CaptureReentry)
            return false;
        // Flashlight/CSM stay aux so their 8192 views keep atlas size. The
        // deferred apply Viewport/GetScreenSize(2560) while that shadow is
        // still parent, but D3D RT0 is already the 3168 G-buffer.
        if (AuxSceneRtBound() && !RtStackTopIsWorldScene()
            && !Hooks::m_VR->D3dRt0IsEyeSized())
            return false;
        if (McLeftoverRecording())
            return false;
        // FullFrame is already eye-sized during HWND pass-through. Using RT0
        // size alone to lie GetScreenSize made water/refract CopyRT use a
        // 1280x1408 srcRect into _rt_poweroftwofb 1024 (2026-09-09 bm_c4a2a).
        if (!Hooks::m_VR->PassThroughWarmupDone())
            return false;
        if (!Hooks::m_VR->IsGameplayEligible() || !EngineInGame())
            return false;
        return Hooks::m_VR->m_RenderWidth >= 640 && Hooks::m_VR->m_RenderHeight >= 360;
    }

    bool OffscreenStereoSizeLie(int& width, int& height)
    {
        // GetBackBufferDimensions only: the swapchain is still the HWND.
        // Lying BB size for the whole frame made Present StretchRect 2560²
        // onto a 2560x1440 backbuffer.
        if (!bmvr::OffscreenWorldMatchesEyes() || !Hooks::m_VR)
            return false;
        if (Hooks::m_VR->HudPaintActive())
            return false;
        if (AuxSceneRtBound() && !RtStackTopIsWorldScene()
            && !Hooks::m_VR->D3dRt0IsEyeSized())
            return false;
        if (McLeftoverRecording())
            return false;
        if (!Hooks::m_VR->StereoWorldPassActive())
            return false;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return false;
        width = eyeW;
        height = eyeH;
        return true;
    }

    bool OffscreenWorldViewSizeLie(int& width, int& height)
    {
        // GetScreenSize builds CViewSetup *before* m_StereoEye is set.
        // Lying only during StereoWorldPassActive left setup=2560x1440 on a
        // 2560x2560 eye; D3D then stretched that 16:9 viewport into the
        // square RT (soft / letterboxed HMD, 2026-09-13 simulator).
        if (!WorldMatchEyeSizeLieAllowed())
            return false;
        width = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        height = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        return true;
    }

    bool LooksLikeAuxSceneView(const CViewSetup& setup)
    {
        if (!Hooks::m_VR)
            return false;
        const Vector body = Hooks::m_VR->m_HasStereoBodyOrigin
            ? Hooks::m_VR->m_StereoBodyOrigin
            : Hooks::m_VR->m_SetupOrigin;
        if (body.LengthSqr() > 1.f)
        {
            const float dx = setup.origin.x - body.x;
            const float dy = setup.origin.y - body.y;
            const float dz = setup.origin.z - body.z;
            if ((dx * dx + dy * dy + dz * dz) > 25.f)
                return true;
        }
        const Vector va = Hooks::m_VR->GetViewAngle();
        if (fabsf(setup.angles.x + va.x) < 12.f && fabsf(va.x) > 1.f)
            return true;
        float dyaw = setup.angles.y - va.y;
        dyaw -= 360.f * floorf((dyaw + 180.f) / 360.f);
        if (fabsf(dyaw) > 90.f)
            return true;
        return false;
    }

    // EyeBloom: the bloom pyramid binds its window-proportional targets
    // (_rt_Small2FB0 960x540 ...) with SetRenderTarget and a viewport that
    // covers exactly that target. ClampStereoViewport grew such a viewport to
    // the eye, so DrawScreenSpaceRectangle (dest relative to the viewport)
    // drew the bright-pass into the top-left 960/eyeW x 540/eyeH of its
    // target and engine_post stretched it into a scene stamp in the eye's
    // top-left corner (bm_c2a5a, 2026-11-04). A viewport that is the whole
    // bound target, when that target is neither the eye nor the window, stays
    // as is.
    bool LooksLikeWindowExtent(int w, int h);
    bool EyeBloomOwnRtViewport(void* ctx, int x, int y, int width, int height)
    {
        if (!bmvr::g_EyeBloom || !Hooks::m_VR || x != 0 || y != 0 || width <= 0 || height <= 0)
            return false;
        int rtW = 0, rtH = 0;
        if (!QueryContextColorRtSize(ctx, rtW, rtH, nullptr) || rtW != width || rtH != height)
            return false;
        if (rtW == static_cast<int>(Hooks::m_VR->m_RenderWidth) && rtH == static_cast<int>(Hooks::m_VR->m_RenderHeight))
            return false;
        // The pyramid is sized from the eye (_rt_Small2FB0 1736x1768 for a
        // 3472x3536 eye), so the test is "not the eye and not the window": a
        // 1736x1768 viewport grown to the eye put the bright-pass in the
        // top-left quarter (headset, On A Rail / c2a5a, 2026-11-04).
        if (LooksLikeWindowExtent(rtW, rtH))
            return false;
        static int s_keepLog;
        if (s_keepLog < 6)
        {
            ++s_keepLog;
            Game::logMsg("EyeBloom: viewport kept at its own target %dx%d", rtW, rtH);
        }
        return true;
    }

    void ClampStereoViewport(int& x, int& y, int& width, int& height)
    {
        if (!Hooks::m_VR)
            return;
        if (AuxSceneRtBound() && !RtStackTopIsWorldScene()
            && !Hooks::m_VR->D3dRt0IsEyeSized())
            return;
        if (NestedRenderView() && !Hooks::m_VR->StereoWorldPassActive())
            return;
        if (Hooks::m_VR->HudPaintActive())
        {
            // Pause GameUI is laid out at HWND size, same as the laser UV map.
            // HEV inset was for in-eye HUD, not this overlay.
            int hx = 0, hy = 0, hw = 0, hh = 0;
            if (Hooks::m_VR->ForceHudOverlayViewport(hx, hy, hw, hh))
            {
                x = hx;
                y = hy;
                width = hw;
                height = hh;
            }
            return;
        }
        // Water / PowerOfTwo / cubemap DrawSetup never calls RenderView, so
        // nest stays 1. Do not expand a 512 reflection viewport to the HMD
        // eye — that stamps a view-locked world into the reflection RT.
        if (width > 0 && height > 0 && (width < 640 || height < 360))
            return;
        if (!g_StereoRedirect && !Hooks::m_VR->StereoWorldPassActive())
            return;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return;
        // Square cubemap / 1024 reflection. Do not also skip 16:9 HWND
        // (2560x1440): that left a warped slice in the 3168 G-buffer which
        // gbuffer-refraction then pasted onto world geometry only (skybox and
        // the forward viewmodel never sample it).
        if (width > 0 && height > 0 && std::abs(width - height) < 32
            && width + 32 < eyeW)
            return;
        // Grown reflection RT (ReflectionRtScale < 1): its 2000x2048 view is
        // not square, so the test above let it through and the reflection
        // rendered at eye size into the top-left of its RT (2026-10-30).
        if (ReflectionRtSized(width, height) && width + 32 < eyeW)
            return;
        // Flashlight apply PushRT/Viewport is 2560 (G-buffer actual). Forcing
        // 1584 here is why the beam never landed in fused eyes. Keep engine
        // size; squash-blit after RenderView. Same lock when gbmatch was
        // crash-stickied but eyes still fit in the HWND (simulator).
        if (Hooks::m_VR->ShouldHwndBlitStereo())
            return;
        const int inX = x;
        const int inY = y;
        const int inW = width;
        const int inH = height;
        width = eyeW;
        height = eyeH;
        // Multicore world dest is a 1x eye (same as queue 0), not the ring.
        // Baking ring origins here recorded Viewport(3184,bandY) which missed
        // the 1x RT and left fog/lighting on the last-known ring rect.
        if (Hooks::m_VR->D3dRt0IsMcRing() && McViewportDestIsRing())
        {
            int ox = 0, oy = 0;
            if (Hooks::m_VR->McResolveRingEyeOrigin(inX, inY, inW, inH, ox, oy))
            {
                x = ox;
                y = oy;
                return;
            }
            int cx = 0, cy = 0, cw = 0, ch = 0;
            if (Hooks::m_VR->McClipAmbiguousWindowVp(inX, inY, inW, inH, cx, cy, cw, ch))
            {
                x = cx;
                y = cy;
                width = cw;
                height = ch;
                return;
            }
        }
        x = 0;
        y = 0;
    }

    const char* SafeMaterialName(IMaterial* material)
    {
        if (!material)
            return "null";
        const char* name = "?";
        BMVR_TRY
        {
            name = material->GetName();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name = "?";
        }
        return name ? name : "?";
    }

    bool MatchesWindowClientSize(int w, int h)
    {
        uint32_t winW = 0, winH = 0;
        if (!bmvr::QueryWindowClientSize(winW, winH))
            return false;
        return std::abs(w - static_cast<int>(winW)) <= 32
            && std::abs(h - static_cast<int>(winH)) <= 32;
    }

    // GameUI pause dimmer is a fullscreen DrawFilledRect / DrawTexturedRect
    // (vguimatsurface CMatSystemSurface slots 15 / 37). Skip it during extra
    // paint so the overlay is menu chrome only.
    bool SkipPauseOverlayFullscreenRect(int x0, int y0, int x1, int y1)
    {
        if (!Hooks::m_VR || !Hooks::m_VR->HudPaintActive())
            return false;
        UINT hw = 0, hh = 0;
        if (!Hooks::m_VR->HudOverlayPixelSize(hw, hh) || hw < 640 || hh < 360)
            return false;
        int rw = x1 - x0;
        int rh = y1 - y0;
        if (rw < 0)
            rw = -rw;
        if (rh < 0)
            rh = -rh;
        if (rw * 10 < static_cast<int>(hw) * 8 || rh * 10 < static_cast<int>(hh) * 8)
            return false;
        static int s_skip;
        if (s_skip < 8)
        {
            Game::logMsg("Pause overlay skip fullscreen VGUI %d,%d %dx%d hud=%ux%u",
                x0, y0, rw, rh, hw, hh);
            ++s_skip;
        }
        return true;
    }

    bool StereoEyeWorldActive()
    {
        if (!Hooks::m_VR || !bmvr::OffscreenWorldMatchesEyes())
            return false;
        if (Hooks::m_VR->HudPaintActive() || Hooks::m_VR->m_CaptureReentry)
            return false;
        return Hooks::m_VR->StereoWorldPassActive();
    }

    bool OffscreenEyePassActive()
    {
        if (!Hooks::m_VR || !bmvr::OffscreenWorldMatchesEyes())
            return false;
        if (Hooks::m_VR->HudPaintActive() || Hooks::m_VR->m_CaptureReentry)
            return false;
        // Same as PushRT: unpaused pass-through is still a 16:9 HWND view.
        // Treating it as an eye pass rewrote DSSR/CopyRT into 1280 RTs.
        return Hooks::m_VR->StereoWorldPassActive();
    }

    bool LooksLikeWindowExtent(int w, int h)
    {
        return w >= 640 && h >= 360 && MatchesWindowClientSize(w, h);
    }

    // CLensflare WorldToScreen (client 0x22FC00) takes pixel size from
    // GetRenderTargetDimensions (+0x20). Mesh 0x22E470 divides by GetViewport
    // (+0x9C). Stereo already forces the viewport to the eye; HWND 16:9 here
    // parks sprites off the lights. Never remap SmallFB 1232x1200.
    //
    // mat_queue_mode 2: CMatQueuedRenderContext +0x20 is `ret 8` (writes
    // nothing). Hardware MinHook never saw those calls. Fill eye size when
    // the outs are HWND, zero, or otherwise not the eye.
    bool ApplyHwndStereoRtDimLie(int& width, int& height, bool originalWroteNothing)
    {
        if (!Hooks::m_VR || Hooks::m_VR->HudPaintActive())
            return false;
        // HWND blit / gbmatch keeps CViewSetup / G-buffer at HWND size and
        // squash-blits after RenderView. Lying 2560x1440 -> 1280x1408 here
        // made PushRT and shaders allocate/draw as if RT0 were the eye
        // (2026-09-09 simulator: first pair done worldMatch=0 mc=0, then
        // exitCode=1). Gate on eyes-vs-window, not only gbmatch — skip-file
        // leftover disabled UseGbMatchViewLock and this lie still fired.
        if (Hooks::m_VR->ShouldHwndBlitStereo())
            return false;
        if (McLeftoverRecording())
            return false;
        if (NestedRenderView())
            return false;
        if (!Hooks::m_VR->StereoWorldPassActive())
            return false;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return false;
        if (width == eyeW && height == eyeH)
            return false;
        const bool hwnd = LooksLikeWindowExtent(width, height);
        const bool empty = width == 0 && height == 0;
        if (width > 0 && height > 0 && (width < 640 || height < 360))
            return false;
        if (!hwnd && !empty && !originalWroteNothing)
            return false;
        static int s_rtDimLog;
        if (s_rtDimLog < 8)
        {
            const char* why = hwnd ? "HWND" : (empty ? "queued empty" : "queued stub");
            Game::logMsg("GetRenderTargetDimensions %dx%d -> %dx%d (%s stereo)",
                width, height, eyeW, eyeH, why);
            ++s_rtDimLog;
        }
        width = eyeW;
        height = eyeH;
        return true;
    }

    template <typename Hk>
    bool HookAlreadyHasTarget(Hk& hk, void* fn)
    {
        return hk.pTarget && fn && hk.pTarget == fn;
    }

    void TryHookMatContextQueries(void* ctx)
    {
        void* dimFn = SehContextVtableSlot(ctx, Offsets::kIMatRenderContext_GetRenderTargetDimensions);
        void* vpFn = SehContextVtableSlot(ctx, Offsets::kIMatRenderContext_GetViewport);
        void* depthRangeFn = SehContextVtableSlot(ctx, Offsets::kIMatRenderContext_DepthRange);
        if (depthRangeFn
            && !HookAlreadyHasTarget(Hooks::hkDepthRange, depthRangeFn)
            && !HookAlreadyHasTarget(Hooks::hkDepthRangeQueued, depthRangeFn))
        {
            static std::mutex s_drMu;
            std::lock_guard<std::mutex> lock(s_drMu);
            auto tryDr = [&](Hook<tDepthRange>& hk, const char* tag) {
                if (hk.pTarget)
                    return false;
                if (hk.createHook(depthRangeFn, &Hooks::dDepthRange) != 0
                    || hk.enableHook() != 0)
                    return false;
                Game::logMsg("Hook enabled: DepthRange %s fn=%p", tag, depthRangeFn);
                return true;
            };
            if (!tryDr(Hooks::hkDepthRange, "ctx")
                && !tryDr(Hooks::hkDepthRangeQueued, "ctx-queued"))
            {
                static int s_drFail;
                if (s_drFail < 4)
                {
                    Game::logMsg("DepthRange extra ctx fn=%p unused", depthRangeFn);
                    ++s_drFail;
                }
            }
        }
        void* clipFn = SehContextVtableSlot(ctx, Offsets::kIMatRenderContext_SetHeightClipMode);
        if (clipFn
            && !HookAlreadyHasTarget(Hooks::hkSetHeightClipMode, clipFn)
            && !HookAlreadyHasTarget(Hooks::hkSetHeightClipModeQueued, clipFn))
        {
            static std::mutex s_clipMu;
            std::lock_guard<std::mutex> lock(s_clipMu);
            // One detour per hook, each calling its own trampoline: the queued context's
            // SetHeightClipMode calls the base one directly with the queued `this`, so picking
            // the original by `this`'s vtable recursed forever (hang at the first frame).
            auto tryClip = [&](Hook<tSetHeightClipMode>& hk, tSetHeightClipMode detour, const char* tag) {
                if (hk.pTarget)
                    return false;
                if (hk.createHook(clipFn, detour) != 0
                    || hk.enableHook() != 0)
                    return false;
                Game::logMsg("Hook enabled: SetHeightClipMode %s fn=%p", tag, clipFn);
                return true;
            };
            if (!tryClip(Hooks::hkSetHeightClipMode, reinterpret_cast<tSetHeightClipMode>(&Hooks::dSetHeightClipMode), "ctx"))
                tryClip(Hooks::hkSetHeightClipModeQueued, reinterpret_cast<tSetHeightClipMode>(&Hooks::dSetHeightClipModeQueued), "ctx-queued");
        }
        void* bindFn = SehContextVtableSlot(ctx, Offsets::kIMatRenderContext_Bind);
        if (bindFn
            && !HookAlreadyHasTarget(Hooks::hkMatBind, bindFn)
            && !HookAlreadyHasTarget(Hooks::hkMatBindQueued, bindFn))
        {
            static std::mutex s_bindMu;
            std::lock_guard<std::mutex> lock(s_bindMu);
            // Created disabled; SetMatBindHooks turns them on for an audit.
            auto tryBind = [&](Hook<tMatBind>& hk, const char* tag) {
                if (hk.pTarget)
                    return false;
                if (hk.createHook(bindFn, &Hooks::dMatBind) != 0)
                    return false;
                Game::logMsg("Hook created: Bind %s fn=%p", tag, bindFn);
                return true;
            };
            if (!tryBind(Hooks::hkMatBind, "ctx"))
                tryBind(Hooks::hkMatBindQueued, "ctx-queued");
        }
        if (dimFn
            && !HookAlreadyHasTarget(Hooks::hkGetRenderTargetDimensions, dimFn)
            && !HookAlreadyHasTarget(Hooks::hkGetRenderTargetDimensionsBase, dimFn)
            && !HookAlreadyHasTarget(Hooks::hkGetRenderTargetDimensionsQueued, dimFn))
        {
            static std::mutex s_dimMu;
            std::lock_guard<std::mutex> lock(s_dimMu);
            auto tryDim = [&](Hook<tGetRenderTargetDimensions>& hk, const char* tag) {
                if (hk.pTarget)
                    return false;
                if (hk.createHook(dimFn, &Hooks::dGetRenderTargetDimensions) != 0
                    || hk.enableHook() != 0)
                    return false;
                Game::logMsg("Hook enabled: GetRenderTargetDimensions %s fn=%p", tag, dimFn);
                return true;
            };
            if (!tryDim(Hooks::hkGetRenderTargetDimensions, "ctx")
                && !tryDim(Hooks::hkGetRenderTargetDimensionsBase, "ctx-base")
                && !tryDim(Hooks::hkGetRenderTargetDimensionsQueued, "ctx-queued"))
            {
                static int s_dimFail;
                if (s_dimFail < 4)
                {
                    Game::logMsg("GetRenderTargetDimensions extra ctx fn=%p unused", dimFn);
                    ++s_dimFail;
                }
            }
        }
        if (vpFn
            && !HookAlreadyHasTarget(Hooks::hkGetViewport, vpFn)
            && !HookAlreadyHasTarget(Hooks::hkGetViewportQueued, vpFn)
            && !Hooks::hkGetViewportQueued.pTarget)
        {
            static std::mutex s_vpMu;
            std::lock_guard<std::mutex> lock(s_vpMu);
            if (!Hooks::hkGetViewportQueued.pTarget
                && Hooks::hkGetViewportQueued.createHook(vpFn, &Hooks::dGetViewport) == 0
                && Hooks::hkGetViewportQueued.enableHook() == 0)
            {
                Game::logMsg("Hook enabled: GetViewport queued-ctx fn=%p", vpFn);
            }
        }
    }

    bool CallOriginalGetRenderTargetDimensions(void* ecx, int& width, int& height)
    {
        void* slot = SehContextVtableSlot(ecx, Offsets::kIMatRenderContext_GetRenderTargetDimensions);
        auto tryCall = [&](Hook<tGetRenderTargetDimensions>& hk) {
            if (!hk.fOriginal || slot != hk.pTarget)
                return false;
            hk.fOriginal(ecx, width, height);
            return true;
        };
        if (tryCall(Hooks::hkGetRenderTargetDimensionsQueued))
            return true;
        if (tryCall(Hooks::hkGetRenderTargetDimensions))
            return false;
        if (tryCall(Hooks::hkGetRenderTargetDimensionsBase))
            return false;
        if (Hooks::hkGetRenderTargetDimensions.fOriginal)
            Hooks::hkGetRenderTargetDimensions.fOriginal(ecx, width, height);
        else if (Hooks::hkGetRenderTargetDimensionsQueued.fOriginal)
        {
            Hooks::hkGetRenderTargetDimensionsQueued.fOriginal(ecx, width, height);
            return true;
        }
        else if (Hooks::hkGetRenderTargetDimensionsBase.fOriginal)
            Hooks::hkGetRenderTargetDimensionsBase.fOriginal(ecx, width, height);
        return slot && slot == Hooks::hkGetRenderTargetDimensionsQueued.pTarget;
    }

    void CallOriginalGetViewport(void* ecx, int& x, int& y, int& width, int& height)
    {
        void* slot = SehContextVtableSlot(ecx, Offsets::kIMatRenderContext_GetViewport);
        if (Hooks::hkGetViewportQueued.fOriginal && slot == Hooks::hkGetViewportQueued.pTarget)
        {
            Hooks::hkGetViewportQueued.fOriginal(ecx, x, y, width, height);
            return;
        }
        if (Hooks::hkGetViewport.fOriginal)
            Hooks::hkGetViewport.fOriginal(ecx, x, y, width, height);
    }

    bool StereoViewmodelUsesWorldDepth()
    {
        return Hooks::m_VR
            && Hooks::m_VR->m_IsVREnabled
            && Hooks::m_VR->IsGameplayEligible()
            && Hooks::m_VR->StereoWorldPassActive()
            && EngineInGame();
    }

    bool IsCompressedViewmodelDepthRange(float zNear, float zFar)
    {
        return zNear >= 0.f && zNear <= 0.01f && zFar > 0.f && zFar < 0.5f;
    }

    void CallOriginalDepthRange(void* ecx, float zNear, float zFar)
    {
        void* slot = SehContextVtableSlot(ecx, Offsets::kIMatRenderContext_DepthRange);
        if (Hooks::hkDepthRangeQueued.fOriginal && slot == Hooks::hkDepthRangeQueued.pTarget)
        {
            Hooks::hkDepthRangeQueued.fOriginal(ecx, zNear, zFar);
            return;
        }
        if (Hooks::hkDepthRange.fOriginal && slot == Hooks::hkDepthRange.pTarget)
        {
            Hooks::hkDepthRange.fOriginal(ecx, zNear, zFar);
            return;
        }
        if (Hooks::hkDepthRange.fOriginal)
        {
            Hooks::hkDepthRange.fOriginal(ecx, zNear, zFar);
            return;
        }
        if (Hooks::hkDepthRangeQueued.fOriginal)
        {
            Hooks::hkDepthRangeQueued.fOriginal(ecx, zNear, zFar);
            return;
        }
        if (slot)
        {
            auto fn = reinterpret_cast<tDepthRange>(slot);
            fn(ecx, zNear, zFar);
        }
    }

    void ForceStereoViewmodelWorldDepth(void* ctx)
    {
        if (!ctx || !StereoViewmodelUsesWorldDepth())
            return;
        CallOriginalDepthRange(ctx, 0.f, 1.f);
    }

    bool LooksLikeWindowUv(float x0, float y0, float x1, float y1)
    {
        const int uw = static_cast<int>(std::fabs(x1 - x0) + 0.5f);
        const int uh = static_cast<int>(std::fabs(y1 - y0) + 0.5f);
        return LooksLikeWindowExtent(uw, uh)
            || LooksLikeWindowExtent(uw + 1, uh + 1);
    }

    bool HudScreenspaceMaterial(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strstr(name, "hud") != nullptr
            || std::strstr(name, "Hud") != nullptr
            || std::strstr(name, "HUD") != nullptr;
    }

    bool DestLooksLikeRefractCopy(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strstr(name, "poweroftwo") != nullptr
            || std::strstr(name, "PowerOfTwo") != nullptr
            || std::strstr(name, "SmallFB") != nullptr
            || std::strstr(name, "smallfb") != nullptr
            || std::strstr(name, "_rt_Power") != nullptr
            || std::strstr(name, "_rt_Small") != nullptr;
    }

    bool SrcLooksLikePowerOfTwoFb(int srcW, int srcH, int eyeW, int eyeH)
    {
        if (ReflectionRtSized(srcW, srcH) && srcW + 32 < eyeW)
            return true;
        if (srcW < 256 || srcH < 256 || srcW > 2048 || srcH > 2048)
            return false;
        if (srcW != srcH)
            return false;
        if (eyeW >= 640 && srcW + 32 >= eyeW)
            return false;
        return true;
    }

    // Stereo-eye bloom (engine_post + downsample/blur pyramid). Verified
    // 2026-09-03: cafeteria fluorescent copies + table stamp. Dest=eye,
    // Viewport rewrite, DoEnginePost eye w/h, and growing _rt_Small*FB* to
    // eye/N all failed (ghosts stayed, or the whole image zoomed/warped with
    // head movement). Keep the rest of post (bms_postprocess / xog / DOF /
    // god rays). dev/bloomadd is the bloom add the engine draws instead of
    // engine_post when mat_colorcorrection is 0: it sampled the stale
    // window-sized 480x270 bloom buffer (BM's HUD glow) with eye/4 texel
    // coordinates and smeared the HUD numbers over the top-left of each eye
    // (2026-11-03).
    bool StereoBloomMaterial(const char* name)
    {
        if (!name || !name[0])
            return false;
        if (std::strstr(name, "hud") || std::strstr(name, "Hud") || std::strstr(name, "HUD"))
            return false;
        return std::strstr(name, "engine_post") != nullptr
            || std::strstr(name, "lumcompare") != nullptr
            || std::strstr(name, "downsample") != nullptr
            || std::strstr(name, "blurfilter") != nullptr
            || std::strstr(name, "blur_combine") != nullptr
            || std::strstr(name, "bloomadd") != nullptr;
    }

    // engine_post* is the final LDR composite (bloom add, tonemap, colour
    // correction, FXAA). With g_StereoEnginePost it runs on the eyes with its
    // bloom var zeroed; the rest of the bloom chain stays skipped.
    bool EnginePostMaterial(const char* name)
    {
        if (!name || !name[0])
            return false;
        if (std::strstr(name, "hud") || std::strstr(name, "Hud") || std::strstr(name, "HUD"))
            return false;
        return std::strstr(name, "engine_post") != nullptr;
    }

    // IMaterialVar vtable differs by branch. Source 2013: 1 GetName,
    // 2 GetNameAsSymbol, 3 SetFloatValue, 4 SetIntValue. CS:GO inserts
    // IsTextureValueInternalEnvCubemap at 1, so GetName is 2 and the setters
    // 4 / 5. Steam BM: FindVar finds $bloomenable / $aaenable but slot 1
    // does not return the name (headless bm_c1a0a, 2026-10-30). The name slot
    // is detected on a var FindVar found, and the setters follow it. Data
    // after the vptr is the same in both: m_pStringVal +0x4, m_intVal +0x8,
    // m_VecVal +0xC (what the inline GetIntValue / GetFloatValue return).
    using tMatVarGetName = const char*(__thiscall*)(void*);
    using tMatVarSetFloat = void(__thiscall*)(void*, float);
    using tMatVarSetInt = void(__thiscall*)(void*, int);

    int g_MatVarNameSlot = 0; // 0 = not detected yet

    char LowerAscii(char c)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    bool NameEqualsI(const char* a, const char* b)
    {
        for (; *a && *b; ++a, ++b)
        {
            if (LowerAscii(*a) != LowerAscii(*b))
                return false;
        }
        return *a == *b;
    }

    const char* SafeMatVarNameAt(void* var, int slot)
    {
        const char* name = nullptr;
        BMVR_TRY
        {
            name = reinterpret_cast<tMatVarGetName>((*reinterpret_cast<void***>(var))[slot])(var);
            if (name)
                (void)*reinterpret_cast<const volatile char*>(name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name = nullptr;
        }
        return name ? name : "";
    }

    const char* SafeMatVarName(void* var)
    {
        return g_MatVarNameSlot ? SafeMatVarNameAt(var, g_MatVarNameSlot) : "?";
    }

    // Both candidate slots are const getters in either layout, so probing
    // them has no side effects.
    void DetectMatVarLayout(void* var, const char* expected)
    {
        if (g_MatVarNameSlot || !var)
            return;
        for (int slot = 1; slot <= 2; ++slot)
        {
            if (NameEqualsI(SafeMatVarNameAt(var, slot), expected))
            {
                g_MatVarNameSlot = slot;
                Game::logMsg("IMaterialVar layout: GetName slot %d (%s), SetFloatValue %d, SetIntValue %d",
                    slot, slot == 1 ? "Source 2013" : "CS:GO", slot + 2, slot + 3);
                return;
            }
        }
    }

    bool SafeMatVarGetFloat(void* var, float& out)
    {
        BMVR_TRY
        {
            out = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(var) + 0xC);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return std::isfinite(out) && out >= 0.f && out <= 1000.f;
    }

    bool SafeMatVarGetInt(void* var, int& out)
    {
        BMVR_TRY
        {
            out = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(var) + 0x8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return out == 0 || out == 1;
    }

    // Without a detected layout, write m_intVal and m_VecVal[0] directly
    // (both inline getters read those) instead of guessing a setter slot.
    bool SafeMatVarSetFloat(void* var, float value)
    {
        BMVR_TRY
        {
            if (g_MatVarNameSlot)
            {
                reinterpret_cast<tMatVarSetFloat>((*reinterpret_cast<void***>(var))[g_MatVarNameSlot + 2])(var, value);
            }
            else
            {
                uint8_t* p = reinterpret_cast<uint8_t*>(var);
                *reinterpret_cast<float*>(p + 0xC) = value;
                *reinterpret_cast<int*>(p + 0x8) = static_cast<int>(value);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool SafeMatVarSetInt(void* var, int value)
    {
        BMVR_TRY
        {
            if (g_MatVarNameSlot)
            {
                reinterpret_cast<tMatVarSetInt>((*reinterpret_cast<void***>(var))[g_MatVarNameSlot + 3])(var, value);
            }
            else
            {
                uint8_t* p = reinterpret_cast<uint8_t*>(var);
                *reinterpret_cast<int*>(p + 0x8) = value;
                *reinterpret_cast<float*>(p + 0xC) = static_cast<float>(value);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    // FindVar (IMaterial slot 11). Not found still returns the shared dummy
    // var, which carries the CMaterialVar vtable the param scan needs.
    void* SafeFindMaterialVar(IMaterial* material, const char* name, bool& found)
    {
        void* var = nullptr;
        found = false;
        BMVR_TRY
        {
            var = material->FindVar(name, &found, false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            var = nullptr;
            found = false;
        }
        return var;
    }

    void* SafeReadPtr(const void* at)
    {
        void* p = nullptr;
        BMVR_TRY
        {
            p = *reinterpret_cast<void* const*>(at);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            p = nullptr;
        }
        return p;
    }

    // ShaderParamCount / GetShaderParams (sdk.h slots) returned 0 / nothing
    // on Steam BM (headless bm_c1a0a, 2026-10-29). Find the param array from
    // data instead: a pointer in the material object to consecutive pointers
    // whose objects carry the CMaterialVar vtable. Reads only.
    int ScanMaterialParams(IMaterial* material, void* varVtable, void**& paramsOut)
    {
        paramsOut = nullptr;
        if (!material || !varVtable)
            return 0;
        int best = 0;
        const uint8_t* base = reinterpret_cast<const uint8_t*>(material);
        // Most words in the material are not pointers: check each address is
        // readable first, so the scan does not lean on caught faults.
        for (int off = 4; off < 0x200; off += 4)
        {
            if (!bmvr::seh::IsReadable(base + off, sizeof(void*)))
                break;
            void** arr = reinterpret_cast<void**>(SafeReadPtr(base + off));
            if (!arr || (reinterpret_cast<uintptr_t>(arr) & 3) != 0)
                continue;
            int n = 0;
            while (n < 256)
            {
                if (!bmvr::seh::IsReadable(arr + n, sizeof(void*)))
                    break;
                void* v = SafeReadPtr(arr + n);
                if (!v || (reinterpret_cast<uintptr_t>(v) & 3) != 0 || !bmvr::seh::IsReadable(v, sizeof(void*))
                    || SafeReadPtr(v) != varVtable)
                    break;
                ++n;
            }
            if (n > best)
            {
                best = n;
                paramsOut = arr;
            }
        }
        return best;
    }

    bool LooksLikeBloomAmountVar(const char* name)
    {
        if (!name || !name[0])
            return false;
        char lower[64];
        size_t i = 0;
        for (; name[i] && i + 1 < sizeof(lower); ++i)
            lower[i] = LowerAscii(name[i]);
        lower[i] = 0;
        if (!std::strstr(lower, "bloom"))
            return false;
        // $bloomtexture / $bloomenable are not the strength.
        return !std::strstr(lower, "tex") && !std::strstr(lower, "enable")
            && !std::strstr(lower, "sampler");
    }

    struct EnginePostBloomVar
    {
        void* var = nullptr;
        const char* name = "none";
        bool isInt = false; // $bloomenable-style bool: zero it with SetIntValue
    };

    // Bloom var of an engine_post material, best first: a known float name,
    // a scanned float param named *bloom*, then a $bloomenable bool. FindVar
    // hits are only trusted when the var reports the name asked for. Logs the
    // FindVar results and every scanned param name so the pick can be checked.
    EnginePostBloomVar FindEnginePostBloomVar(IMaterial* material, const char* matName)
    {
        static const char* const kFloatNames[] = {
            "$bloomamount", "$bloomscale", "$bloom_amount", "$bloomstrength", "$bloomamt", "$bloom" };
        static const char* const kProbeNames[] = { "$basetexture", "$fbtexture", "$aaenable" };
        static const char* const kEnableNames[] = { "$bloomenable", "$bloom_enable" };
        EnginePostBloomVar out;
        void* varVtable = nullptr;
        char found[512];
        size_t used = 0;
        found[0] = 0;
        auto probe = [&](const char* name) -> void* {
            bool hit = false;
            void* v = SafeFindMaterialVar(material, name, hit);
            if (v && !varVtable)
                varVtable = SafeReadPtr(v);
            // FindVar's found flag is trusted; the name check only reports
            // whether the IMaterialVar layout was detected.
            if (v && hit)
                DetectMatVarLayout(v, name);
            const bool ok = v && hit;
            const bool named = ok && NameEqualsI(SafeMatVarName(v), name);
            const int n = std::snprintf(found + used, sizeof(found) - used, "%s%s=%d",
                used ? " " : "", name, named ? 1 : (ok ? 2 : 0));
            if (n > 0 && used + static_cast<size_t>(n) < sizeof(found))
                used += static_cast<size_t>(n);
            return ok ? v : nullptr;
        };
        for (const char* name : kProbeNames)
            probe(name);
        for (const char* name : kFloatNames)
        {
            void* v = probe(name);
            if (v && !out.var)
            {
                out.var = v;
                out.name = name;
            }
        }
        void* enableVar = nullptr;
        const char* enableName = "none";
        for (const char* name : kEnableNames)
        {
            void* v = probe(name);
            if (v && !enableVar)
            {
                enableVar = v;
                enableName = name;
            }
        }
        void** params = nullptr;
        const int count = ScanMaterialParams(material, varVtable, params);
        char list[1024];
        size_t listUsed = 0;
        list[0] = 0;
        for (int i = 0; i < count; ++i)
        {
            void* v = SafeReadPtr(params + i);
            const char* name = SafeMatVarName(v);
            if (!out.var && LooksLikeBloomAmountVar(name))
            {
                out.var = v;
                out.name = name;
            }
            if (!enableVar && std::strstr(name, "bloom") && std::strstr(name, "enable"))
            {
                enableVar = v;
                enableName = name;
            }
            const int n = std::snprintf(list + listUsed, sizeof(list) - listUsed, "%s%s",
                listUsed ? " " : "", name);
            if (n > 0 && listUsed + static_cast<size_t>(n) < sizeof(list))
                listUsed += static_cast<size_t>(n);
        }
        if (!out.var && enableVar)
        {
            out.var = enableVar;
            out.name = enableName;
            out.isInt = true;
        }
        static int s_paramLog;
        if (s_paramLog < 4)
        {
            // FindVar: 1 = found and GetName (detected slot) agrees, 2 = found
            // but no GetName slot returned the name, 0 = not found.
            Game::logMsg("DSSR engine_post vars %s varVt=%p FindVar: %s", matName, varVtable, found);
            Game::logMsg("DSSR engine_post params %s (%d scanned): %s", matName, count, list);
            ++s_paramLog;
        }
        return out;
    }

    // Eye-pass bloom var zeroed and not yet restored. The DSSR quad can sit in
    // the buffered dynamic mesh after the call returns, and the shader reads
    // the var when that batch flushes. Restore only after a render target
    // change or EndFrame, which flush it (Source 2013 shaderapidx8
    // SetRenderTargetEx). thread_local: the restore must run on the thread
    // that drew (render thread under mat_queue_mode 2).
    thread_local void* g_EnginePostBloomVar = nullptr;
    thread_local bool g_EnginePostBloomIsInt = false;
    thread_local float g_EnginePostBloomValue = 0.f;
    thread_local int g_EnginePostBloomIntValue = 0;

    void RestoreEnginePostBloom()
    {
        if (!g_EnginePostBloomVar)
            return;
        if (g_EnginePostBloomIsInt)
            SafeMatVarSetInt(g_EnginePostBloomVar, g_EnginePostBloomIntValue);
        else
            SafeMatVarSetFloat(g_EnginePostBloomVar, g_EnginePostBloomValue);
        g_EnginePostBloomVar = nullptr;
    }

    // Returns whether the bloom var is now 0. varName / origOut feed the
    // caller's log line.
    bool ZeroEnginePostBloomForEye(IMaterial* material, const char* matName, const char*& varName, float& origOut)
    {
        static IMaterial* s_mat;
        static EnginePostBloomVar s_var;
        if (material != s_mat)
        {
            s_mat = material;
            s_var = material ? FindEnginePostBloomVar(material, matName) : EnginePostBloomVar{};
        }
        varName = s_var.name;
        origOut = 0.f;
        void* var = s_var.var;
        if (!var)
            return false;
        if (g_EnginePostBloomVar == var)
        {
            // Second eye before a flush point: keep the real value.
            origOut = g_EnginePostBloomIsInt ? static_cast<float>(g_EnginePostBloomIntValue)
                                             : g_EnginePostBloomValue;
            return s_var.isInt ? SafeMatVarSetInt(var, 0) : SafeMatVarSetFloat(var, 0.f);
        }
        RestoreEnginePostBloom();
        if (s_var.isInt)
        {
            int orig = 0;
            if (!SafeMatVarGetInt(var, orig) || !SafeMatVarSetInt(var, 0))
                return false;
            g_EnginePostBloomIntValue = orig;
            origOut = static_cast<float>(orig);
        }
        else
        {
            float orig = 0.f;
            if (!SafeMatVarGetFloat(var, orig) || !SafeMatVarSetFloat(var, 0.f))
                return false;
            g_EnginePostBloomValue = orig;
            origOut = orig;
        }
        g_EnginePostBloomVar = var;
        g_EnginePostBloomIsInt = s_var.isInt;
        return true;
    }

    bool QueryTextureSize(ITexture* texture, int& w, int& h)
    {
        w = 0;
        h = 0;
        if (!texture)
            return false;
        BMVR_TRY
        {
            w = texture->GetActualWidth();
            h = texture->GetActualHeight();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            w = h = 0;
        }
        return w > 0 && h > 0;
    }

    bool QueryContextColorRtSize(void* ctx, int& w, int& h, const char** nameOut)
    {
        w = 0;
        h = 0;
        if (nameOut)
            *nameOut = "";
        if (!ctx || !Hooks::hkGetRenderTarget.fOriginal)
            return false;
        ITexture* rt = nullptr;
        BMVR_TRY
        {
            rt = Hooks::hkGetRenderTarget.fOriginal(ctx);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            rt = nullptr;
        }
        if (nameOut)
            *nameOut = SafeTextureName(rt);
        return QueryTextureSize(rt, w, h);
    }

    void BindStereoPushToEye(ITexture*& pTexture, ITexture*& pDepthTexture, int& nViewX, int& nViewY, int& nViewW, int& nViewH, bool& redirected)
    {
        redirected = false;
        if (!g_StereoRedirect || !Hooks::m_VR)
            return;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return;
        if (pTexture == g_StereoRedirect)
            return;
        // Shadows / flashlight / water stay on their own RTs.
        if (pTexture && !IsStereoSceneColorTarget(pTexture) && nViewW >= 640 && nViewH >= 360)
            return;
        // NULL = backbuffer. ViewDrawScene / CSimpleWorldView PushRT(NULL,0x0)
        // three times. Binding the LDR swapchain on top of the HDR eye killed
        // named_push. Rewrite while g_StereoRedirect is set.
        pTexture = g_StereoRedirect;
        pDepthTexture = nullptr;
        nViewX = 0;
        nViewY = 0;
        nViewW = eyeW;
        nViewH = eyeH;
        redirected = true;
    }

    void SehViewport(IMatRenderContext* ctx, int w, int h)
    {
        BMVR_TRY
        {
            Hooks::hkViewport.fOriginal(ctx, 0, 0, w, h);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool SehVguiPaintOriginal(void* vgui, int paintMode)
    {
        if (!Hooks::hkVgui_Paint.fOriginal)
            return false;
        BMVR_TRY
        {
            Hooks::hkVgui_Paint.fOriginal(vgui, paintMode);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // L4D2VR PaintToHudOnce: extra VGui_Paint into vrHUD. Engine dest stays
    // untouched so desktop VGUI survives. BM never PushRT `_rt_gui` during
    // stereo gameplay (log: only gbuffer/null), so the PopRT blit never ran
    // and the overlay stayed hidden — headset only saw in-eye HUD at the
    // 16:9 edges (2026-08-18).
    void PaintVguiToOverlay(void* vgui, int mode)
    {
        (void)mode;
        VR* vr = Hooks::m_VR;
        if (!vr || !Hooks::hkVgui_Paint.fOriginal)
            return;
        // Extra paint is engine VGUI only. Forcing UIPANELS|CURSOR during
        // in-game GameUI copies menu chrome onto a cleared-transparent overlay.
        // HEV HUD is client DRAWHUD, not this path.
        const bool title = !vr->WantPauseWorldOverlay() && vr->WantTitleHudOverlay();
        if (!vr->WantPauseWorldOverlay() && !title)
            return;
        if (!vr->AppActivated())
            return;
        if (vr->HudPaintedThisFrame())
            return;

        vr->EnsureHudOverlay();
        if (!vr->HudOverlayReady() || !vr->m_D9HUDSurface)
            return;

        UINT tw = 0, th = 0;
        if (!vr->HudOverlayPixelSize(tw, th))
            return;

        MatCtxScope scope;
        // New game UI pause: the menu is the QML layer (drawn in
        // StampPauseOverlayCursor); the VGUI paint is empty there and its
        // OverrideAlphaWriteEnable faulted in materialsystem at load ends
        // (2026-11-23). Title layer (intro credits) still paints VGUI.
        const bool skipVgui = !title && bmvr::NewUiWindow() != nullptr;
        vr->SetPauseHudSkipVgui(skipVgui);
        vr->SetHudPaintActive(true);
        vr->SetHudPaintTitle(title);
        const bool mc = vr->McPipelineEnabled();
        bool queued = false;
        if (mc)
            queued = vr->QueueMcPauseHudBegin();
        if (!queued)
        {
            if (!vr->BindPauseHudForExtraPaint())
            {
                vr->SetHudPaintActive(false);
                return;
            }
        }
        // Queued (multicore): the material thread binds the HUD RT, which
        // resets the viewport to its size. Setting it here from the main
        // thread raced the material thread (AV in shaderapidx9 opening the
        // pause menu / applying video settings, 2026-10-15, twice).
        if (!queued && scope.ctx && Hooks::hkViewport.fOriginal)
            SehViewport(scope.ctx, static_cast<int>(tw), static_cast<int>(th));
        // HL2VR RenderHUD: full dest, alpha write, clear 0,0,0,0,
        // then PAINT_UIPANELS. Software CURSOR fights the ColorFill arrow
        // and strobes. D3D bind replaces named PushRT.
        if (!queued)
            vr->PreparePauseHudForVgui(scope.ctx);
        // Title layer: the client HUD tree with only the logo / message
        // panels left visible for this one paint.
        const int paintMode = title ? PAINT_INGAMEPANELS : PAINT_UIPANELS;
        unsigned int hiddenPanels[256];
        const int hiddenCount = title ? vr->IsolateTitleHudPanels(hiddenPanels, 256) : 0;
        ++g_VguiOverlayReentry;
        const bool painted = skipVgui ? true : SehVguiPaintOriginal(vgui, paintMode);
        --g_VguiOverlayReentry;
        vr->RestoreTitleHudPanels(hiddenPanels, hiddenCount);
        if (!painted)
        {
            if (!queued)
                vr->UnbindPauseHudAfterExtraPaint();
            vr->SetHudPaintActive(false);
            static int s_paintSeh;
            if (s_paintSeh < 4)
            {
                Game::logMsg("VGui extra-paint overlay SEH");
                ++s_paintSeh;
            }
            return;
        }
        if (queued)
        {
            if (!vr->QueueMcPauseHudEnd())
            {
                vr->FinishPauseHudExtraPaint(scope.ctx);
                vr->StampPauseOverlayCursor();
                vr->UnbindPauseHudAfterExtraPaint();
                vr->NoteHudPainted();
            }
        }
        else
        {
            vr->FinishPauseHudExtraPaint(scope.ctx);
            vr->UnbindPauseHudAfterExtraPaint();
            vr->StampPauseOverlayCursor();
            vr->NoteHudPainted();
        }
        vr->SetHudPaintActive(false);
        static int s_ov;
        if (s_ov < 8)
        {
            Game::logMsg("VGui extra-paint overlay %ux%u mode=0x%X pause=%d ready=%d queued=%d",
                tw, th, paintMode, vr->PauseUiActive() ? 1 : 0,
                vr->HudOverlayReady() ? 1 : 0, queued ? 1 : 0);
            ++s_ov;
        }
    }
}

ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
    BMVR_PROF("hook GetRenderTarget");
    (void)edx;
    NoteMatContext(ecx);
    return hkGetRenderTarget.fOriginal(ecx);
}

void __fastcall Hooks::dRenderView(void* ecx, void* edx, CViewSetup& setup, int nClearFlags, int whatToDraw)
{
    ApplyPerfLogLive();
    bmvr::sampler::CpuNote(0);
    bmvr::NoteClientSizeRefreshThread();
    if (bmvr::g_CpuSampleSeconds > 0.f)
    {
        if (bmvr::sampler::CpuSampleStart(bmvr::g_CpuSampleSeconds))
            Game::logMsg("cpu sample: started for %.1f s", bmvr::g_CpuSampleSeconds);
        bmvr::g_CpuSampleSeconds = 0.f;
    }
    (void)edx;
    struct ProfFrame
    {
        ProfFrame() { bmvr::prof::FrameRenderViewEnter(); }
        ~ProfFrame() { bmvr::prof::FrameRenderViewLeave(); }
    } profFrame;
    BMVR_PROF("hook RenderView");
    RenderViewNestScope nest;
    CurrentViewScope curView(setup);
    if (!hkRenderView.fOriginal)
        return;
    // Trace frames count top-level RenderViews (BM skips the EndFrame hook);
    // the trigger sets frames + 1 so the frame it lands in is not counted.
    if (bmvr::g_PassTraceFrames > 0 && g_RenderViewNest == 1)
        --bmvr::g_PassTraceFrames;
    if (bmvr::g_PassTraceFrames > 0)
        Game::logMsg("trace: RenderView nest=%d eye=%d %dx%d draw=0x%x clear=0x%x", g_RenderViewNest,
            m_VR ? m_VR->m_StereoEye : -1, setup.width, setup.height, whatToDraw, nClearFlags);

    if (g_RenderViewNest > 1)
    {
        hkRenderView.fOriginal(ecx, setup, nClearFlags, whatToDraw);
        return;
    }

    auto callOriginal = [&](CViewSetup& view, int clearFlags, int drawFlags) {
        BMVR_PROF("RenderView eye pass (game)");
        if (m_VR)
            ReplayOccluderView(m_VR->m_StereoEye);
        hkRenderView.fOriginal(ecx, view, clearFlags, drawFlags);
    };

    if (setup.width >= 640 && setup.height >= 360 && !(m_VR && m_VR->m_IsVREnabled)
        && bmvr::OffCenterTestActive())
    {
        CViewSetup view = setup;
        view.m_bOffCenter = true;
        view.m_flOffCenterLeft = 0.0f;
        view.m_flOffCenterRight = 0.758f;
        view.m_flOffCenterBottom = 0.0f;
        view.m_flOffCenterTop = 1.0f;
        callOriginal(view, nClearFlags, whatToDraw);
        return;
    }

    if (m_VR)
    {
        m_VR->FlushPendingGameUi();
        m_VR->TickAudioFocusRecover();
    }

    EnsureServerFlashlightHook();
    if (m_VR)
        m_VR->LogFullFrameSizeIfReady();
    ApplyFxaaParams();

    const bool queued = m_Game && m_Game->GetMatQueueMode() != 0;
    SourceRenderQueueBuildScope sourceRenderQueueBuildScope(m_VR, queued);

    const bool mainView = setup.width >= 640 && setup.height >= 360;
    const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();

    const bool originOk = std::isfinite(setup.origin.x) && std::isfinite(setup.origin.y)
        && std::isfinite(setup.origin.z)
        && fabsf(setup.origin.x) < 100000.f && fabsf(setup.origin.y) < 100000.f;
    const bool fovOk = std::isfinite(setup.fov) && setup.fov > 10.f && setup.fov < 170.f;
    // Crossbow zoom lerps CViewSetup.fov through ~43 then down toward the
    // 2D scope value. fov>10 skipped stereo after the first live frame and
    // left the last eye texture frozen in the HMD until unscope.
    const bool scopeFov = m_VR && std::isfinite(setup.fov) && setup.fov > 1.f && setup.fov < 50.f
        && (m_VR->ScopeZoomActive() || m_VR->ViewmodelIsScoped());
    const bool fovForStereo = fovOk || scopeFov;
    if (scopeFov && !fovOk)
    {
        static int s_zoomFovGate;
        if (s_zoomFovGate < 8)
        {
            Game::logMsg("Stereo zoom FOV gate engine=%.1f (would have skipped at fov<=10)", setup.fov);
            ++s_zoomFovGate;
        }
    }

    // Hand climbing: the view follows the climb hand between LadderMove's
    // per-tick bursts (VR::PhysicalLadderViewOffset). Before anything reads
    // setup.origin, so eyes, hands and the body origin agree.
    if (m_VR && m_VR->m_IsVREnabled && mainView && inGame && originOk)
    {
        const float off = m_VR->PhysicalLadderViewOffset(setup.origin.z);
        const Vector shift = m_VR->PhysicalLadderViewShift(setup.origin);
        if (off != 0.f)
            setup.origin.z += off;
        // Physical crouching: the view stays at the real head.
        setup.origin.z += m_VR->PhysicalCrouchViewOffset(setup.origin.z - off);
        setup.origin.x += shift.x;
        setup.origin.y += shift.y;
    }
    if (m_VR && mainView)
        m_VR->m_SetupOrigin = setup.origin;
    {
        // Headless diagnostic (BMVR_VIEW_TRACE=1): the engine's view for the
        // main pass ~4x per second, to compare scripted cameras with ours.
        static int s_viewTrace = -1;
        if (s_viewTrace < 0)
        {
            char v[8] = {};
            s_viewTrace = GetEnvironmentVariableA("BMVR_VIEW_TRACE", v, sizeof(v)) && v[0] == '1';
        }
        static ULONGLONG s_next;
        if (s_viewTrace && mainView && m_Game && GetTickCount64() >= s_next)
        {
            s_next = GetTickCount64() + 250;
            const Vector va = m_VR ? m_VR->GetViewAngle() : Vector();
            Game::logMsg("view trace: engine origin (%.0f %.0f %.0f) angles (%.1f %.1f %.1f) fov %.1f; ours (%.1f %.1f %.1f) scripted=%d",
                setup.origin.x, setup.origin.y, setup.origin.z, setup.angles.x, setup.angles.y, setup.angles.z,
                setup.fov, va.x, va.y, va.z, m_VR && m_VR->ScriptedCameraActive() ? 1 : 0);
        }
    }
    if (m_VR && mainView && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && inGame)
        m_VR->UpdateScriptedCamera(setup.angles);

    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && inGame)
        m_VR->WaitPosesForStereoFrame();

    if (m_VR && m_Game && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
        && inGame && mainView && !m_VR->m_StereoEyesDrawnThisFrame)
        m_VR->TickMatQueueFromRenderView();

    // L4D2VR/Portal2: HMD angles + origin on a COPY. Writing the same values
    // onto the live CViewSetup is abs_view (tram camera blacked).
    // Do not SetViewAngles from RenderView — L4D2VR only does that in
    // mat_queue_mode 0 and restores. Permanent overwrite + CreateMove HMD
    // angles fights interpolation (rubberband). CreateMove already writes
    // cmd->viewangles.
    auto applyL4d2VrHead = [&](CViewSetup& view, bool stereo, bool leftEye) {
        if (!m_VR || !m_VR->m_HmdPoseValid)
            return;
        // A scripted camera (point_viewcontrol, cutscene) drives the view:
        // its orientation, with the head turning relative to it.
        Vector camRight;
        if (m_VR->ScriptedCameraActive() && m_VR->ScriptedCameraView(view.angles, camRight))
        {
            const float halfIpd = m_VR->RenderHalfIpd();
            view.origin = setup.origin;
            if (stereo)
                view.origin += camRight * (leftEye ? -halfIpd : halfIpd);
            return;
        }
        view.angles = m_VR->GetViewAngle();
        SnapRpgLaserDot();
        if (stereo)
            view.origin = leftEye ? m_VR->GetViewOriginLeft(setup.origin)
                                  : m_VR->GetViewOriginRight(setup.origin);
        else
            view.origin = m_VR->GetViewOrigin(setup.origin);
    };

    auto passThrough = [&]() {
        if (m_VR && !m_VR->m_StereoEyesDrawnThisFrame)
        {
            m_VR->m_DirectEyeSubmit = false;
            m_VR->m_StereoRenderViewActive = false;
        }
        if (m_VR && m_VR->IsGameplayEligible() && inGame
            && mainView && m_VR->m_HmdPoseValid)
        {
            // HWND blit (eyes smaller than the window, or gbmatch lock): do
            // not rewrite look or shrink the view. Look copy + gluon CopyRT
            // died in pass-through on OpenXR Simulator (heartbeat in_rel_look).
            if (m_VR->ShouldHwndBlitStereo())
            {
                static int s_ptHwnd;
                if (s_ptHwnd < 4)
                {
                    Game::logMsg("HMD-fb pass-through HWND %dx%d (no look copy; eyes %ux%u)",
                        setup.width, setup.height, m_VR->m_RenderWidth, m_VR->m_RenderHeight);
                    ++s_ptHwnd;
                }
                callOriginal(setup, nClearFlags, whatToDraw);
                return;
            }
            CViewSetup vrView = setup;
            applyL4d2VrHead(vrView, false, true);
            // WorldRenderAtEyeSize is grow-only. When FullFrame already matches
            // eyes that are at least as large as this HWND view, keep the
            // pass-through on the RT size (CopyRT 2560 into 1280 FullFrame
            // 2026-09-09). Do not shrink a 2560 view down to a leftover 1280
            // eye — that is the simulator gluon crash path.
            if (bmvr::OffscreenWorldMatchesEyes()
                && m_VR->m_RenderWidth + 32 >= static_cast<UINT>(setup.width)
                && m_VR->m_RenderHeight + 32 >= static_cast<UINT>(setup.height))
            {
                NormalizeViewSetupForVREye(vrView, m_VR, 0);
                static int s_ptEye;
                if (s_ptEye < 4)
                {
                    Game::logMsg("HMD-fb pass-through view %dx%d -> eye %dx%d (world RTs match)",
                        setup.width, setup.height, vrView.width, vrView.height);
                    ++s_ptEye;
                }
            }

            static int s_relLog;
            if (s_relLog < 4)
            {
                Game::logMsg("L4D2VR look copy engine=(%.1f,%.1f) hmd=(%.1f,%.1f) out=(%.1f,%.1f)",
                    setup.angles.x, setup.angles.y,
                    m_VR->m_HmdAngAbs.x, m_VR->m_HmdAngAbs.y,
                    vrView.angles.x, vrView.angles.y);
                ++s_relLog;
            }

            static int s_relFrames;
            if (s_relFrames == 0)
                bmvr::BeginRisky(L"rel_look");
            callOriginal(vrView, nClearFlags, whatToDraw);
            ++s_relFrames;
            if (s_relFrames == 8)
                bmvr::EndRisky(L"rel_look");
            return;
        }
        callOriginal(setup, nClearFlags, whatToDraw);
    };

    if (!m_VR || !m_VR->m_IsVREnabled)
    {
        callOriginal(setup, nClearFlags, whatToDraw);
        return;
    }
    if (mainView && m_VR->IsGameplayEligible())
    {
        static int s_setupLog;
        if (s_setupLog < 8)
        {
            Game::logMsg("RenderView setup=%dx%d fov=%.1f aspect=%.3f origin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f) whatToDraw=0x%X submits=%d inGame=%d pass=%u",
                setup.width, setup.height, setup.fov, setup.m_flAspectRatio,
                setup.origin.x, setup.origin.y, setup.origin.z,
                setup.angles.x, setup.angles.y, whatToDraw, m_VR->m_SubmitCount, inGame ? 1 : 0,
                m_VR->m_PassThroughMainViews);
            ++s_setupLog;
        }
    }
    else if (m_VR->IsGameplayEligible())
    {
        static int s_smallLog;
        if (s_smallLog < 8)
        {
            Game::logMsg("RenderView small setup=%dx%d inGame=%d", setup.width, setup.height, inGame ? 1 : 0);
            ++s_smallLog;
        }
    }

    // 2026-08-17: GetScreenSize made the first gameplay view 1584x1440, then
    // this hook immediately ran two RenderViews with HMD FOV/zNear/IPD during
    // spawn. Died inside the left callOriginal. Menu 1584 and that first
    // setup= line both succeeded. Wait for pass-through world frames first.
    constexpr uint32_t kStereoAfterPassThrough = VR::kPassThroughViewsBeforeQueued;
    if (bmvr::TryStereoRenderView()
        && m_VR->IsGameplayEligible()
        && inGame
        && mainView
        && originOk
        && fovForStereo
        && m_VR->StereoEyesReady()
        && m_VR->m_PassThroughMainViews < kStereoAfterPassThrough)
    {
        ++m_VR->m_PassThroughMainViews;
        if (m_VR->m_PassThroughMainViews == 1)
            bmvr::BeginRisky(L"hmd_fb");
        static int s_ptLog;
        if (s_ptLog < 10)
        {
            Game::logMsg("HMD-fb pass-through %u/%u inGame=%d setup=%dx%d fov=%.1f zNear=%.1f",
                m_VR->m_PassThroughMainViews, kStereoAfterPassThrough, inGame ? 1 : 0,
                setup.width, setup.height, setup.fov, setup.zNear);
            ++s_ptLog;
        }
        passThrough();
        return;
    }

    // Named PushRT wrap onto HDR leftEye0 dies in CSimpleWorldView after three
    // rewritten PushRT(NULL) — even when the named RT is 2560x1440, matching
    // the G-buffer (2026-08-17). L4D2's real fix is game render size = HMD
    // size. On BM that is GetBackBufferDimensions / FullFrameFB at HMD aspect
    // with the 16:9 window left alone, then two RenderViews + blit (the path
    // that survived as stereo_copy) instead of a second HDR RT.
    if (bmvr::TryStereoRenderView()
        && m_VR->IsGameplayEligible()
        && inGame
        && mainView
        && originOk
        && fovForStereo
        && m_VR->StereoEyesReady())
    {
        if (m_VR->m_StereoEyesDrawnThisFrame)
        {
            // 47777b5: skip only same-size duplicate player views after the
            // stereo pair. d24bc07 also skipped leftover 16:9 mains
            // (windowed169) — that dropped the deferred flashlight apply that
            // still painted the desktop G-buffer. Nested water/cubemap already
            // returned at nest>1. Reflections at 1024² must still run.
            const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
            const int eyeH = static_cast<int>(m_VR->m_RenderHeight);
            const bool sameAsStereo = eyeW >= 640 && std::abs(setup.width - eyeW) < 32
                && std::abs(setup.height - eyeH) < 32;
            const bool windowed169 = setup.m_flAspectRatio > 1.45f && setup.width >= 1600;
            // GetScreenSize/aspect still lie after the pair, so leftover 2560x1440
            // often has eye aspect (~1.02) and misses windowed169. Pixel size is
            // the HWND pass. In-eye 3D sky is inside callOriginal; this leftover
            // is what painted sky into the wrong ring rect (sky "moving around").
            const bool leftoverWindow = MatchesWindowClientSize(setup.width, setup.height)
                || (setup.width >= 1600 && setup.height >= 900);
            if (m_VR->McPipelineEnabled() && (leftoverWindow || sameAsStereo))
            {
                static int s_mcSkipDup;
                if (s_mcSkipDup < 8)
                {
                    Game::logMsg("Skip leftover after MC stereo %dx%d aspect=%.3f",
                        setup.width, setup.height, setup.m_flAspectRatio);
                    ++s_mcSkipDup;
                }
                m_SkippedViewSinceCheck = true;
                return;
            }
            // Leftover policy: gbmatch + gb_leftskip skips the 16:9 desktop
            // main (2 scene renders). gbmatch without the skip renders it.
            // No-gbmatch uses DesktopLeftoverRender (default off).
            const bool gbSkipLeftover = bmvr::TryFlashlightGbMatch() && bmvr::TryGbLeftSkip();
            const bool skipLeftover = bmvr::OffscreenWorldMatchesEyes()
                || m_VR->ShouldHwndBlitStereo()
                || (bmvr::TryFlashlightGbMatch()
                    ? gbSkipLeftover
                    : !bmvr::g_DesktopLeftoverRender);
            if ((sameAsStereo || windowed169)
                && !LooksLikeAuxSceneView(setup)
                && skipLeftover)
            {
                static int s_gbLeftSkipArmed;
                if (gbSkipLeftover && !s_gbLeftSkipArmed)
                {
                    s_gbLeftSkipArmed = 1;
                    bmvr::BeginRisky(L"gb_leftskip");
                }
                static int s_skipDup;
                if (s_skipDup < 8)
                {
                    Game::logMsg("Skip leftover main RenderView after stereo %dx%d",
                        setup.width, setup.height);
                    ++s_skipDup;
                }
                m_SkippedViewSinceCheck = true;
                return;
            }
            static int s_auxLog;
            if (s_auxLog < 8)
            {
                Game::logMsg("Allow aux RenderView after stereo %dx%d origin=(%.1f,%.1f,%.1f) pitch=%.1f",
                    setup.width, setup.height, setup.origin.x, setup.origin.y, setup.origin.z,
                    setup.angles.x);
                ++s_auxLog;
            }
            callOriginal(setup, nClearFlags, whatToDraw);
            return;
        }
        m_VR->m_StereoEyesDrawnThisFrame = true;
        m_VR->BeginStereoFramePose();
        if (m_VR->ShouldHwndBlitStereo())
        {
            // Simulator 1280 eyes / 2560 G-buffer: do not shrink CViewSetup to
            // 1280 (that plus DRAWHUD exited 1). One HWND view blit to both
            // eyes was mono and ALTERNATING_VISIBLE_FRAMES — 16:9 fov 106
            // squashed into a ~square eye the compositor treated as 90°.
            // Two HWND-sized RVs with IPD + HMD fov/aspect, squash-blit each
            // after a GPU flush. Do not bind the 1280 eye RTs, gloves overlay,
            // ColorFill, or a third DRAWHUD pass (those killed gbmatch).
            static int s_hwndBlit;
            if (s_hwndBlit == 0)
                bmvr::BeginRisky(L"hmd_fb");
            m_VR->NoteEngineScopeFov(setup.fov);
            CViewSetup leftEyeView = setup;
            CViewSetup rightEyeView = setup;
            NormalizeViewSetupForVREye(leftEyeView, m_VR, 1);
            NormalizeViewSetupForVREye(rightEyeView, m_VR, 2);
            m_VR->NoteStereoClipPlanes(leftEyeView.zNear, leftEyeView.zFar);
            applyL4d2VrHead(leftEyeView, true, true);
            applyL4d2VrHead(rightEyeView, true, false);
            if (s_hwndBlit < 8)
            {
                const float ipd = m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale;
                Game::logMsg("HWND gbmatch stereo pair setup=%dx%d -> %dx%d fov=%.1f->%.1f aspect=%.3f->%.3f eyes=%ux%u ipd=%.3f",
                    setup.width, setup.height, leftEyeView.width, leftEyeView.height,
                    setup.fov, leftEyeView.fov, setup.m_flAspectRatio, leftEyeView.m_flAspectRatio,
                    m_VR->m_RenderWidth, m_VR->m_RenderHeight, ipd);
            }
            m_VR->m_StereoRenderViewActive = false;
            m_VR->m_StereoBodyOrigin = setup.origin;
            m_VR->m_HasStereoBodyOrigin = true;
            ++bmvr::g_StereoPairSeq;
            m_VR->m_StereoEye = 1;
            callOriginal(leftEyeView, nClearFlags, whatToDraw);
            const bool leftOk = m_VR->BlitHmdViewFromBackbuffer(m_VR->m_D9LeftEyeSurface, true);
            m_VR->m_StereoEye = 2;
            callOriginal(rightEyeView, nClearFlags, whatToDraw);
            const bool rightOk = m_VR->BlitHmdViewFromBackbuffer(m_VR->m_D9RightEyeSurface, true);
            if (leftOk && rightOk)
            {
                // "LOW-RES FALLBACK" toast (set in LogRenderPathIfChanged).
                m_VR->DrawToastOnEyeSurface(m_VR->m_D9LeftEyeSurface, 1);
                m_VR->DrawToastOnEyeSurface(m_VR->m_D9RightEyeSurface, 2);
            }
            m_VR->m_StereoEye = 0;
            m_VR->m_DirectEyeSubmit = true;
            m_VR->FlushStereoBlitGpu();
            m_VR->m_RenderedNewFrame.store(true, std::memory_order_release);
            m_VR->m_HasStereoBodyOrigin = false;
            m_VR->EndStereoFramePose();
            if (s_hwndBlit < 8)
            {
                Game::logMsg("HWND gbmatch stereo done left=%d right=%d", leftOk ? 1 : 0, rightOk ? 1 : 0);
                ++s_hwndBlit;
            }
            else
                ++s_hwndBlit;
            if (s_hwndBlit == 120)
                bmvr::EndRisky(L"hmd_fb");
            return;
        }
        static int s_enterLog;
        if (s_enterLog < 4)
        {
            Game::logMsg("Stereo HMD-fb enter setup=%dx%d fov=%.1f zNear=%.1f",
                setup.width, setup.height, setup.fov, setup.zNear);
            ++s_enterLog;
        }
        m_VR->NoteEngineScopeFov(setup.fov);
        CViewSetup leftEyeView = setup;
        CViewSetup rightEyeView = setup;
        NormalizeViewSetupForVREye(leftEyeView, m_VR, 1);
        NormalizeViewSetupForVREye(rightEyeView, m_VR, 2);
        m_VR->NoteStereoClipPlanes(leftEyeView.zNear, leftEyeView.zFar);
        applyL4d2VrHead(leftEyeView, true, true);
        applyL4d2VrHead(rightEyeView, true, false);
        const float ipd = m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale;
        const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(m_VR->m_RenderHeight);
        const bool mc = m_VR->McPipelineEnabled();
        const uint32_t mcBand = mc ? m_VR->McWriteBand() : 0;
        if (mc)
        {
            m_VR->NoteMcRecordThread();
            // 1x eyes at 0,0 — same WorldRenderAtEyeSize path as queue 0.
            // Ring band origins made lighting/flashlight sample the atlas.
            leftEyeView.x = 0;
            leftEyeView.y = 0;
            leftEyeView.m_nUnscaledX = 0;
            leftEyeView.m_nUnscaledY = 0;
            rightEyeView.x = 0;
            rightEyeView.y = 0;
            rightEyeView.m_nUnscaledX = 0;
            rightEyeView.m_nUnscaledY = 0;
            static int s_mcView;
            if (s_mcView < 8)
            {
                Game::logMsg("MC bake 1x view 0,0 %dx%d band=%u",
                    leftEyeView.width, leftEyeView.height, mcBand);
                ++s_mcView;
            }
        }

        if (bmvr::g_FakeRuntime)
        {
            // Headless tests: eye origins/angles in world units, ~1/s (world scale check).
            static DWORD s_fakeEyeLogMs;
            const DWORD nowMs = GetTickCount();
            if (nowMs - s_fakeEyeLogMs >= 1000)
            {
                s_fakeEyeLogMs = nowMs;
                Game::logMsg("fake eyes setup=(%.2f,%.2f,%.2f) L=(%.2f,%.2f,%.2f) R=(%.2f,%.2f,%.2f) ang=(%.2f,%.2f,%.2f) fov=%.3f aspect=%.4f ipdUnits=%.3f",
                    setup.origin.x, setup.origin.y, setup.origin.z,
                    leftEyeView.origin.x, leftEyeView.origin.y, leftEyeView.origin.z,
                    rightEyeView.origin.x, rightEyeView.origin.y, rightEyeView.origin.z,
                    leftEyeView.angles.x, leftEyeView.angles.y, leftEyeView.angles.z,
                    leftEyeView.fov, leftEyeView.m_flAspectRatio, ipd);
            }
        }

        static int s_hmdFbOnce;
        if (s_hmdFbOnce == 0)
        {
            s_hmdFbOnce = 1;
            bmvr::BeginRisky(L"hmd_fb");
            if (bmvr::TryOffscreenHmd())
                bmvr::BeginRisky(L"hmd_offscreen");
            if (bmvr::TryOffscreenWorldGrow())
                bmvr::BeginRisky(L"hmd_world");
            if (bmvr::UseGbMatchViewLock())
                bmvr::BeginRisky(L"fl_gbmatch");
            Game::logMsg("Stereo HMD-fb begin setup=%dx%d unscaled=%dx%d stereoEye=%d eye=%dx%d fov=%.1f->%.1f aspect=%.3f->%.3f zNear=%.1f ipd=%.2f L=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) offscreen=%d worldMatch=%d gbmatch=%d",
                setup.width, setup.height, setup.m_nUnscaledWidth, setup.m_nUnscaledHeight,
                setup.m_eStereoEye, eyeW, eyeH,
                setup.fov, leftEyeView.fov, setup.m_flAspectRatio, leftEyeView.m_flAspectRatio,
                leftEyeView.zNear, ipd,
                leftEyeView.origin.x, leftEyeView.origin.y, leftEyeView.origin.z,
                rightEyeView.origin.x, rightEyeView.origin.y, rightEyeView.origin.z,
                bmvr::TryOffscreenHmd() ? 1 : 0,
                bmvr::OffscreenWorldMatchesEyes() ? 1 : 0,
                bmvr::UseGbMatchViewLock() ? 1 : 0);
        }

        m_VR->m_DirectEyeSubmit = true;
        m_VR->m_StereoRenderViewActive = false;
        m_VR->m_DesktopMirrorEnabled = false;

        static int s_eyeRvLog;
        if (s_eyeRvLog < 4)
        {
            Game::logMsg("Stereo HMD-fb left RenderView %dx%d",
                leftEyeView.width, leftEyeView.height);
            ++s_eyeRvLog;
        }
        m_VR->m_StereoBodyOrigin = setup.origin;
        m_VR->m_HasStereoBodyOrigin = true;
        StereoCostBegin();
        // RightEyeFirst (test aid, MulticoreMode only): same two passes, right
        // eye recorded first, to see whether culling follows the first view.
        const bool rightFirst = mc && bmvr::g_RightEyeFirst;
        if (rightFirst)
        {
            static int s_rfLog;
            if (s_rfLog < 2)
            {
                Game::logMsg("RightEyeFirst: right eye RenderView before left");
                ++s_rfLog;
            }
            const int eyeDraw = whatToDraw & ~kRenderViewDrawHud;
            ++bmvr::g_StereoPairSeq;
            g_Cost.eye = 2;
            m_VR->m_StereoEye = 2;
            m_VR->QueueMcEyeBind(2, false);
            long long t0 = QpcNow();
            callOriginal(rightEyeView, nClearFlags, eyeDraw);
            g_Cost.rightTicks += QpcNow() - t0;
            g_Cost.eye = 1;
            m_VR->m_StereoEye = 1;
            m_VR->QueueMcEyeBind(1, true);
            t0 = QpcNow();
            callOriginal(leftEyeView, nClearFlags, eyeDraw);
            g_Cost.leftTicks += QpcNow() - t0;
        }
        else
        {
        ++bmvr::g_StereoPairSeq;
        g_Cost.eye = 1;
        m_VR->m_StereoEye = 1;
        if (!mc)
        {
            m_VR->BeginStereoEyeBlit(m_VR->m_D9LeftEyeSurface);
            m_VR->ClearStereoEyeSurfaces();
        }
        else
        {
            m_VR->QueueMcEyeBind(1, false);
        }
        {
            const int eyeDraw = whatToDraw & ~kRenderViewDrawHud;
            const long long t0 = QpcNow();
            callOriginal(leftEyeView, nClearFlags, eyeDraw);
            g_Cost.leftTicks += QpcNow() - t0;
        }
        if (!mc)
        {
        const bool leftUnbind = m_VR->EndStereoEyeBlit();
        {
            (void)leftUnbind;
            // Native: LDR backbuffer bind was redirected onto the eye (G-buffer
            // and FullFrame match). Do not StretchRect the 2560 window over it.
            // Unbind of A2R10 FullFrame still whites LDR eyes (ff_hmdfit).
            const bool keepNative = bmvr::OffscreenWorldMatchesEyes()
                && m_VR->StereoRedirectedToEye();
            bool glovesInBlit = false;
            if (!keepNative)
            {
                const long long tHands = QpcNow();
                if (!m_VR->IsMenuUp())
                    glovesInBlit = m_VR->DrawVrGlovesIntoBlitSource(1);
                g_Cost.handsTicks += QpcNow() - tHands;
                const long long t0 = QpcNow();
                // Always GPU-wait this copy. Both eyes share _rt_FullFrameFB /
                // the HWND backbuffer; without a flush the right-eye RenderView
                // overwrites the source before the left StretchRect lands, so
                // OpenXR/OpenVR submit the same picture twice (Quest 3 / Link
                // log: same image both eyes). Config StereoBlitGpuFlush cannot
                // turn this off — DXVK async will miss the hazard.
                const bool leftBb = m_VR->BlitHmdViewFromBackbuffer(
                    m_VR->m_D9LeftEyeSurface, true);
                if (!leftBb)
                {
                    glovesInBlit = false;
                    m_VR->BlitCurrentGameColorTo(
                        m_VR->m_D9LeftEyeSurface, true);
                }
                g_Cost.blitTicks += QpcNow() - t0;
            }
            else
            {
                // keepNative skips StretchRect, so the blit-path flush never
                // runs. Left still shares FullFrame with the coming right
                // RenderView on DXVK async; wait before the right eye starts.
                m_VR->FlushStereoBlitGpu();
            }
            if (!m_VR->IsMenuUp()
                && (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes || bmvr::g_HandHud || bmvr::g_DamageFlash
                || m_VR->WeaponMenuOpen() || m_VR->SettingsPanelOpen() || m_VR->AimCrosshairVisible() || m_VR->ToastVisible() || bmvr::g_DebugBlankEye >= 0.5f))
            {
                const long long t0 = QpcNow();
                m_VR->DrawIndependentHandMarkers(
                    m_VR->ColorTargetForStereoEye(1), 1, true, !glovesInBlit);
                g_Cost.handsTicks += QpcNow() - t0;
            }
        }
        } // !mc left post-RV
        if (s_eyeRvLog < 8)
        {
            Game::logMsg("Stereo HMD-fb right RenderView %dx%d",
                rightEyeView.width, rightEyeView.height);
            ++s_eyeRvLog;
        }
        g_Cost.eye = 2;
        g_Cost.eye = 2;
        m_VR->m_StereoEye = 2;
        if (!mc)
        {
            m_VR->BeginStereoEyeBlit(m_VR->m_D9RightEyeSurface);
            m_VR->ClearStereoEyeSurfaces();
        }
        else
        {
            // Flush so left's shared FullFrame copy can land on the 1x eye
            // before the right RenderView reuses it. Do not IMat Viewport
            // the HWND at eye size (nvogl crash).
            m_VR->QueueMcEyeBind(2, true);
        }
        {
            const int eyeDraw = whatToDraw & ~kRenderViewDrawHud;
            const long long t0 = QpcNow();
            callOriginal(rightEyeView, nClearFlags, eyeDraw);
            g_Cost.rightTicks += QpcNow() - t0;
        }
        if (!mc)
        {
        const bool rightUnbind = m_VR->EndStereoEyeBlit();
        {
            (void)rightUnbind;
            const bool keepNative = bmvr::OffscreenWorldMatchesEyes()
                && m_VR->StereoRedirectedToEye();
            bool glovesInBlit = false;
            if (!keepNative)
            {
                const long long tHands = QpcNow();
                if (!m_VR->IsMenuUp())
                    glovesInBlit = m_VR->DrawVrGlovesIntoBlitSource(2);
                g_Cost.handsTicks += QpcNow() - tHands;
                const long long t0 = QpcNow();
                const bool rightBb = m_VR->BlitHmdViewFromBackbuffer(
                    m_VR->m_D9RightEyeSurface, false);
                if (!rightBb)
                {
                    glovesInBlit = false;
                    m_VR->BlitCurrentGameColorTo(m_VR->m_D9RightEyeSurface, false);
                }
                g_Cost.blitTicks += QpcNow() - t0;
            }
            if (!m_VR->IsMenuUp()
                && (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes || bmvr::g_HandHud || bmvr::g_DamageFlash
                || m_VR->WeaponMenuOpen() || m_VR->SettingsPanelOpen() || m_VR->AimCrosshairVisible() || m_VR->ToastVisible() || bmvr::g_DebugBlankEye >= 0.5f))
            {
                const long long t0 = QpcNow();
                m_VR->DrawIndependentHandMarkers(
                    m_VR->ColorTargetForStereoEye(2), 2, true, !glovesInBlit);
                g_Cost.handsTicks += QpcNow() - t0;
            }
        }
        } // !mc right post-RV
        } // !rightFirst
        m_VR->m_StereoEye = 0;
        if (mc)
        {
            m_VR->QueueMcCompositeToRing(mcBand);
            m_VR->FinishMcStereoPair();
        }
        // Keep the 3D world on the HWND under GameUI. Skipping this left the
        // desktop pause menu on black (2026-09-06). ColorFill of unused 16:9
        // still fights VGUI chrome — skip only that.
        // Multicore: cover-crop goes to an offscreen RT on the material
        // thread; Present copies it onto the HWND. StretchRect of 1x eyes
        // from this thread races playback. Do not blit onto the swapchain
        // from the mat thread (viewport clobber / Present stall).
        if (!mc)
        {
        if (m_VR->ShouldHwndBlitStereo())
            m_VR->FlushStereoBlitGpu();
        if (bmvr::OffscreenWorldMatchesEyes() || bmvr::g_MirrorBothEyes)
            m_VR->MirrorStereoToDesktopWindow();
        else if (m_VR->m_IsVREnabled && !m_VR->WantPauseWorldOverlay()
            && !m_VR->ShouldHwndBlitStereo())
            m_VR->ClearUnusedDesktopBackbuffer();
        }
        // gbmatch already draws flashlight in the eye passes. A third window
        // DRAWHUD pass was a 15fps regression and, on OpenXR Simulator with
        // gbmatch skip-filed, the process exit (heartbeat in_drawhud).
        // Keep it only for fused fallback (eyes stripped HUD).
        const bool runDrawHudPass = !mc && !bmvr::TryFlashlightGbMatch()
            && !m_VR->ShouldHwndBlitStereo() && bmvr::TryDrawHud();
        if (runDrawHudPass)
        {
            static int s_drawHudFrames;
            if (s_drawHudFrames == 0)
                bmvr::BeginRisky(L"drawhud");
            // Eyes stripped DRAWHUD. One window-sized pass fills _rt_gui /
            // _rt_Hud (client RenderView only — engine VGui_Paint cannot).
            // Keep engine width/height; do not NormalizeViewSetupForVREye.
            CViewSetup hudView = setup;
            applyL4d2VrHead(hudView, false, true);
            callOriginal(hudView, nClearFlags, whatToDraw | kRenderViewDrawHud);
            ++s_drawHudFrames;
            if (s_drawHudFrames == 120)
                bmvr::EndRisky(L"drawhud");
        }
        // MirrorStereoToDesktopWindow already copied an eye that has the gloves
        // rendered into it with depth. Adding the no-depth desktop overlay on
        // top of that put a second, slightly offset copy of each hand on the
        // window, which is what read as the hands being see-through there.
        if (!mc
            && (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes)
            && !m_VR->IsMenuUp()
            && !bmvr::OffscreenWorldMatchesEyes()
            && !m_VR->VrGlovesDrawnIntoScene())
            m_VR->DrawIndependentHandsOnDesktop();
        m_VR->m_HasStereoBodyOrigin = false;
        m_VR->EndStereoFramePose();
        StereoCostLog();
        // Do not StretchRect A2R10 FullFrame onto the backbuffer. That
        // overwrote the engine's 1584 tram strip with a black HDR copy
        // (2026-08-18). LDR 1x-eye letterbox is MirrorStereoToDesktopWindow.

        m_VR->m_RenderedNewFrame.store(true, std::memory_order_release);

        static int s_hmdFbDone;
        if (s_hmdFbDone < 4)
        {
            Game::logMsg("Stereo HMD-fb pair done %dx%d redirected=%d worldMatch=%d mc=%d",
                eyeW, eyeH, m_VR->StereoRedirectedToEye() ? 1 : 0,
                bmvr::OffscreenWorldMatchesEyes() ? 1 : 0, mc ? 1 : 0);
            if (bmvr::OffscreenWorldMatchesEyes())
                Game::logMsg("offscreen native: FullFrame/G-buffer match eyes %dx%d", eyeW, eyeH);
            if (bmvr::UseGbMatchViewLock())
                Game::logMsg("HWND stereo post-pair: no RT dim lie, no BB ColorFill (gbmatch squash blit)");
            ++s_hmdFbDone;
        }

        static int s_hmdFbFrames;
        ++s_hmdFbFrames;
        if (s_hmdFbFrames >= 120)
        {
            // >= not == : live SS / later BeginRisky must not leave flags
            // after the first successful 120 frames (installer false-ban).
            bmvr::EndRisky(L"hmd_fb");
            if (bmvr::TryOffscreenHmd())
                bmvr::EndRisky(L"hmd_offscreen");
            if (bmvr::TryOffscreenWorldGrow())
                bmvr::EndRisky(L"hmd_world");
            if (bmvr::TryFlashlightGbMatch())
                bmvr::EndRisky(L"fl_gbmatch");
            if (bmvr::TryGbLeftSkip())
                bmvr::EndRisky(L"gb_leftskip");
        }
        return;
    }

    passThrough();
}

namespace
{
    // Thrown snark flight log (2.5 s after a hand throw): where it goes and
    // whether its velocity is still the one set (snarks hop and hunt on
    // their own). Server entity, same process (listen server); reads are
    // SEH-guarded since it may die meanwhile.
    void* g_SnarkTraceEnt = nullptr;
    int g_SnarkTraceOrgOff = -1;
    DWORD g_SnarkTraceUntil = 0;
    DWORD g_SnarkTraceStart = 0;
    void TraceThrownSnark()
    {
        if (!g_SnarkTraceEnt || g_SnarkTraceOrgOff < 0)
            return;
        const DWORD now = GetTickCount();
        if (now > g_SnarkTraceUntil)
        {
            g_SnarkTraceEnt = nullptr;
            return;
        }
        static DWORD s_last;
        if (now - s_last < 100)
            return;
        s_last = now;
        Vector org{}, vel{};
        BMVR_TRY
        {
            const char* e = static_cast<const char*>(g_SnarkTraceEnt);
            org = *reinterpret_cast<const Vector*>(e + g_SnarkTraceOrgOff);
            vel = *reinterpret_cast<const Vector*>(e + 0x218);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_SnarkTraceEnt = nullptr;
            return;
        }
        Game::logMsg("Snark flight +%lums: pos (%.0f,%.0f,%.0f) vel (%.0f,%.0f,%.0f) %.0f u/s",
            static_cast<unsigned long>(now - g_SnarkTraceStart), org.x, org.y, org.z, vel.x, vel.y, vel.z, vel.Length());
    }
}

namespace
{
    // RPG rocket flight log (diagnostic, user 2026-11-18: the rocket rises
    // after launch before flying forward): after a shot with the RPG out,
    // every client entity whose network class names a rocket/missile is
    // logged every ~100 ms for 2.5 s (first 4 shots per run).
    void TraceRpgRocket(Game* game, VR* vr, const CUserCmd* cmd)
    {
        static DWORD s_until = 0, s_start = 0, s_last = 0;
        static int s_shots = 0;
        static std::vector<const char*> s_before;   // network name per index at the shot
        static bool s_prevAttack = false;
        if (!game || !vr || !cmd || !game->m_ClientEntityList)
            return;
        const bool attack = (cmd->buttons & IN_ATTACK) != 0;
        const DWORD now = GetTickCount();
        if (attack && !s_prevAttack && s_shots < 4)
        {
            const char* wm = game->GetActiveWeaponModelName();
            if (wm && std::strstr(wm, "rpg"))
            {
                ++s_shots;
                s_start = now;
                const int top = game->m_ClientEntityList->GetHighestEntityIndex();
                s_before.assign(static_cast<size_t>(top + 1 > 0 ? top + 1 : 0), nullptr);
                for (int e = 1; e <= top; ++e)
                    s_before[static_cast<size_t>(e)] = game->GetEntityNetworkName(e);
                s_until = now + 2500;
                Game::logMsg("RPG flight log: shot %d, aim pitch %.1f yaw %.1f", s_shots, cmd->viewangles.x, cmd->viewangles.y);
            }
        }
        s_prevAttack = attack;
        if (!s_until || now > s_until || now - s_last < 100)
            return;
        s_last = now;
        const int hi = game->m_ClientEntityList->GetHighestEntityIndex();
        for (int e = 1; e <= hi; ++e)
        {
            const char* name = game->GetEntityNetworkName(e);
            // Entities that appeared since the shot (or rocket-named ones).
            const bool isNew = static_cast<size_t>(e) >= s_before.size() || s_before[static_cast<size_t>(e)] != name;
            if (!name || !(isNew || std::strstr(name, "issile") || std::strstr(name, "ocket")))
                continue;
            const char* ent = static_cast<const char*>(game->m_ClientEntityList->GetClientEntity(e));
            if (!ent)
                continue;
            Vector org{};
            BMVR_TRY { org = *reinterpret_cast<const Vector*>(ent + Offsets::kCBaseEntity_AbsOrigin); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            Game::logMsg("RPG flight +%lums: %s #%d at (%.0f,%.0f,%.0f)", static_cast<unsigned long>(now - s_start), name, e, org.x, org.y, org.z);
        }
    }
}

namespace
{
    // Pistol trigger (user 2026-11-20, a player asked for HL2VR's pistol
    // rapid fire). HL2VR lets a fresh trigger pull skip the pistol's fire-rate
    // wait (hlvr_weapon_pistol.cpp ItemPostFrame); Black Mesa's glock has that
    // trade-off as its alt-fire instead (scripts/gameplay/weapons/weapon_glock.dmx:
    // primary 0.32 s spread 1, secondary 0.18 s spread 6), so input only:
    //  - VrPistolSemiAuto (glock, .357): a pull sends its fire bit until the
    //    clip drops (the shot) or a timeout below the gun's cycle; holding on
    //    fires no more until the trigger is let go.
    //  - VrGlockRapidAlt (glock): a pull within kRapidWindow s of the previous
    //    pull sends IN_ATTACK2 instead of IN_ATTACK.
    // Runs on the final usercmd buttons (after the original CreateMove).
    void ApplyPistolTrigger(Game* game, VR* vr, CUserCmd* cmd)
    {
        constexpr double kRapidWindow = 0.40;   // s between pull starts
        static bool s_prevPull = false;
        static double s_pullT = 0.0, s_lastPullT = -10.0;
        static bool s_alt = false, s_shot = false;
        static int s_clipAtPull = -1;
        static int s_logs = 0;
        if (!game || !vr || !cmd)
            return;
        const bool semi = bmvr::g_VrPistolSemiAuto, rapid = bmvr::g_VrGlockRapidAlt;
        const char* wm = (semi || rapid) ? game->GetActiveWeaponModelName() : nullptr;
        const bool glock = wm && std::strstr(wm, "glock");
        const bool magnum = wm && std::strstr(wm, "357");
        if (!glock && !magnum)
        {
            s_prevPull = false;
            return;
        }
        const bool pull = (vr->HeldButtons() & IN_ATTACK) != 0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto readClip = [&]() {
            int hp, ar, clip, res, sec, air;
            return game->ReadWristHudValues(hp, ar, clip, res, sec, air) ? clip : -1;
        };
        if (pull && !s_prevPull)
        {
            s_alt = rapid && glock && (now - s_lastPullT) < kRapidWindow;
            s_lastPullT = s_pullT = now;
            s_shot = false;
            s_clipAtPull = readClip();
            if (s_logs++ < 30)
                Game::logMsg("Pistol trigger: %s pull -> %s (clip %d, semi=%d rapid=%d)",
                    glock ? "glock" : ".357", s_alt ? "alt-fire" : "primary", s_clipAtPull, semi ? 1 : 0, rapid ? 1 : 0);
        }
        s_prevPull = pull;
        if (!pull)
            return;
        if (semi && !s_shot)
        {
            // Timeouts: below each cycle (glock 0.32, .357 0.75), so a missed
            // clip read cannot let a held pull fire twice. Alt-fire waits
            // longer: the glock's primary shot may still hold it back.
            const double maxHold = s_alt ? 0.30 : (glock ? 0.29 : 0.70);
            const int clip = readClip();
            if (clip >= 0 && s_clipAtPull >= 0 && clip < s_clipAtPull)
            {
                s_shot = true;
                if (s_logs < 30)
                    Game::logMsg("Pistol trigger: shot after %.0f ms (clip %d -> %d)", (now - s_pullT) * 1000.0, s_clipAtPull, clip);
            }
            else if (now - s_pullT > maxHold)
            {
                s_shot = true;
                if (s_logs < 30)
                    Game::logMsg("Pistol trigger: no shot seen in %.0f ms (clip %d), fire bit released", maxHold * 1000.0, clip);
            }
        }
        cmd->buttons &= ~IN_ATTACK;
        if (!(semi && s_shot))
            cmd->buttons |= s_alt ? IN_ATTACK2 : IN_ATTACK;
    }

    // VrPistolSemiAuto for the glock's alt-fire button too (user 2026-11-20):
    // one shot per press, same rule as the trigger. Runs after
    // ApplyPistolTrigger (its rapid alt-fire bit is left alone).
    void ApplyPistolAltButton(Game* game, VR* vr, CUserCmd* cmd)
    {
        static bool s_prev = false, s_shot = false;
        static double s_pressT = 0.0;
        static int s_clipAtPress = -1;
        if (!game || !vr || !cmd || !bmvr::g_VrPistolSemiAuto)
        {
            s_prev = false;
            return;
        }
        const char* wm = game->GetActiveWeaponModelName();
        if (!wm || !std::strstr(wm, "glock"))
        {
            s_prev = false;
            return;
        }
        const bool press = (vr->HeldButtons() & IN_ATTACK2) != 0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto readClip = [&]() {
            int hp, ar, clip, res, sec, air;
            return game->ReadWristHudValues(hp, ar, clip, res, sec, air) ? clip : -1;
        };
        if (press && !s_prev)
        {
            s_pressT = now;
            s_shot = false;
            s_clipAtPress = readClip();
        }
        s_prev = press;
        if (!press || s_shot)
        {
            if (press)
                cmd->buttons &= ~IN_ATTACK2;
            return;
        }
        const int clip = readClip();
        if ((clip >= 0 && s_clipAtPress >= 0 && clip < s_clipAtPress) || now - s_pressT > 0.30)
        {
            // Shot seen, or none in 0.30 s (as the trigger's alt-fire: the
            // primary's cycle may still hold it back).
            s_shot = true;
            cmd->buttons &= ~IN_ATTACK2;
        }
    }
}

bool __fastcall Hooks::dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd)
{
    TraceThrownSnark();
    BMVR_PROF("hook CreateMove");
    (void)edx;
    if (!hkCreateMove.fOriginal)
        return false;
    if (!cmd)
        return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);
    // The caller stores the final cmd->viewangles into the engine after this
    // returns; note them so the next tick can tell a restored (loaded) angle.
    struct CmdYawNote
    {
        CUserCmd* cmd;
        ~CmdYawNote()
        {
            if (Hooks::m_VR && cmd->command_number && std::isfinite(cmd->viewangles.y))
                Hooks::m_VR->NoteCmdAngles(cmd->viewangles.x, cmd->viewangles.y);
        }
    } cmdYawNote{ cmd };
    if (m_VR && m_VR->m_IsVREnabled && cmd->command_number)
        m_VR->SyncTurnToRestoredYaw(cmd->viewangles.y);

    // Yaw actually written to cmd->viewangles this tick. Movement is expressed
    // relative to viewangles, so the stick-to-usercmd conversion below has to
    // use the same yaw or strafing skews whenever the aim source changes.
    // Black Mesa's ladders are HL2 func_useableladders (CHL2GameMovement::
    // LadderMove / FullLadderMove, SDK hl_gamemovement.cpp). There the view
    // direction decides everything: stick forward climbs up unless looking
    // > ~17 degrees down, stick back climbs the opposite way of the view,
    // the auto-dismount at either end needs an info_ladder_dismount within
    // 60 degrees of the full 3D view direction, and jump leaves along the
    // view. Ladder mode therefore sends the head's angles, except:
    //  - stick back = IN_FORWARD with the view turned round and 45 degrees
    //    down: always climbs down, and at the bottom the dismount behind the
    //    player is 'in front' of that view, so it steps off backwards;
    //  - jump = along the stick (head frame) or the view, stick zeroed so
    //    HL2 does not flip it; positive forwardmove is held off for 0.5 s so
    //    the auto-mount cone does not grab the ladder straight back;
    //  - mount assist (VR::LadderMode): close to a ladder with the stick
    //    roughly towards it, the view yaw points straight at the ladder and
    //    the move is pure forward, so the engine's 15-degree / no-strafe
    //    auto-mount check passes on the first try.
    // Firing keeps the gun's angles and the stock behaviour.
    static ULONGLONG s_ladderJumpUntil = 0;
    int ladderBits = 0;
    bool onLadder = false;
    float ladderMountYaw = std::numeric_limits<float>::quiet_NaN();
    const ULONGLONG ladderNow = GetTickCount64();
    const bool afterLadderJump = ladderNow < s_ladderJumpUntil;
    // Physical ladders (VR::PhysicalLadderStep): a hand gripping the ladder
    // drives it instead of the stick: mount = one IN_USE facing the ladder,
    // then each tick up (view pitched up, IN_FORWARD), down (same as stick
    // back) or hold (no bits).
    VR::PhysicalLadderCmd physLadder{};
    const bool ladderBase = m_VR && m_VR->m_IsVREnabled && cmd->command_number
        && m_VR->IsGameplayEligible() && !afterLadderJump
        && (m_VR->HeldButtons() & (IN_ATTACK | IN_ATTACK2)) == 0;
    // Jump latch: released once the stick lets go of jump.
    if (m_VR && m_VR->m_LadderJumpLatch.load(std::memory_order_relaxed)
        && (m_VR->RawHeldButtons() & IN_JUMP) == 0)
    {
        m_VR->m_LadderJumpLatch.store(false, std::memory_order_relaxed);
        Game::logMsg("Ladder grab: jump released, stick jump works again");
    }
    // Jump held only skips the hand ladder on a ladder (jump = jump off).
    // Not on one yet it still runs: under water stick up swims up, and a
    // grab made while swimming up never mounted (user 2026-11-19, the
    // bm_c2a5a water ladder; near the surface it worked).
    constexpr int kMoveTypeLadderHook = 9;
    const bool jumpHeldNow = m_VR && (m_VR->HeldButtons() & IN_JUMP) != 0;
    const bool onLadderNow = m_Game && m_Game->LocalPlayerMoveType() == kMoveTypeLadderHook;
    const bool physLadderOn = ladderBase && (!jumpHeldNow || !onLadderNow)
        && m_VR->PhysicalLadderStep(physLadder);
    // Hand mount with jump held: ignore jump from now until it is let go,
    // or the tick after the mount (on the ladder, jump still held) would
    // jump straight off.
    if (physLadderOn && !physLadder.onLadder && physLadder.mountPush && jumpHeldNow
        && !m_VR->m_LadderJumpLatch.load(std::memory_order_relaxed))
    {
        m_VR->m_LadderJumpLatch.store(true, std::memory_order_relaxed);
        Game::logMsg("Ladder grab: mounting with jump held, jump ignored until it is let go");
    }
    // Mount failed (not on a ladder, not mounting): swimming up works again.
    else if (m_VR && m_VR->m_LadderJumpLatch.load(std::memory_order_relaxed) && !onLadderNow
        && !(physLadderOn && (physLadder.onLadder || physLadder.mountPush)))
    {
        m_VR->m_LadderJumpLatch.store(false, std::memory_order_relaxed);
        Game::logMsg("Ladder grab: not on the ladder, jump latch cleared");
    }
    if (physLadderOn)
    {
        onLadder = physLadder.onLadder;
        ladderBits = physLadder.dir > 0 ? IN_FORWARD : (physLadder.dir < 0 ? IN_BACK : 0);
        if (physLadder.drop || physLadder.topExit)
            ladderBits = IN_USE;   // +use on a ladder = get off (HL2 LadderMove)
        if (physLadder.stickTop)
        {
            // Near the top end: the stick works as in stick ladder mode.
            bool stickOnLadder = false;
            m_VR->LadderMode(ladderBits, stickOnLadder, nullptr);
        }
        if (!onLadder)
        {
            ladderMountYaw = physLadder.yaw;
            if (physLadder.useEdge)
                ladderBits |= IN_USE;
        }
    }
    else if (m_VR && !(ladderBase && (m_VR->HeldButtons() & IN_JUMP) == 0))
    {
        // Keep the touch flags fresh-false when the step does not run.
        m_VR->m_LadderTouch[0].store(false, std::memory_order_release);
        m_VR->m_LadderTouch[1].store(false, std::memory_order_release);
    }
    // Hands only (PhysicalLadders without LadderStickClimb): no stick mount
    // assist and no stick climbing; walking into a ladder does not mount it
    // either (sv_ladder_useonly, VR::SyncLadderUseOnly).
    const bool ladderHandsOnly = bmvr::g_PhysicalLadders && !bmvr::g_LadderStickClimb;
    const bool ladderMode = ladderBase
        && (physLadderOn || (!ladderHandsOnly && m_VR->LadderMode(ladderBits, onLadder, &ladderMountYaw)));
    // Hands only: on a ladder the stick never climbs, also when the hand
    // step is not running this tick (no hand holding, mounted some other
    // way); user 2026-11-19: the left stick still moved them up/down.
    const bool ladderStickBlocked = ladderHandsOnly
        && (physLadderOn ? (physLadder.onLadder && !physLadder.stickTop) : onLadderNow);
    const bool ladderMountAssist = ladderMode && !onLadder && std::isfinite(ladderMountYaw);
    const bool ladderJump = ladderMode && onLadder && (m_VR->HeldButtons() & IN_JUMP) != 0;
    const bool ladderBack = ladderMode && onLadder && !ladderJump && (ladderBits & IN_BACK) != 0;
    const bool ladderPhysUp = physLadderOn && onLadder && physLadder.dir > 0;
    if (ladderJump)
        s_ladderJumpUntil = ladderNow + 500;
    if (ladderBack)
        ladderBits = IN_FORWARD;   // side bits would be mirrored in the turned view
    auto applyLadderButtons = [&]() {
        if (!ladderMode)
            return;
        if (ladderBack || ladderMountAssist)
            cmd->buttons &= ~(IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
        cmd->buttons |= ladderBits;
    };
    QAngle appliedAim{};
    bool haveAppliedAim = false;
    auto applyControllerAim = [&]() {
        if (!m_VR || !m_VR->m_IsVREnabled || !cmd->command_number)
            return;
        if (!(m_VR->IsGameplayEligible() && EngineInGame() && m_VR->m_HmdPoseValid))
            return;
        auto apply = [&](const QAngle& a) {
            cmd->viewangles.Init(a.x, a.y, 0.f);
            appliedAim = a;
            haveAppliedAim = true;
            m_VR->RememberFireAim(QAngle(a.x, a.y, 0.f));
        };
        // Before a quick save / pause: the head's angles, so the save faces
        // where the player looked (see VR::HeadAimReadyForSave).
        if (m_VR->TakeHeadAimCmd())
        {
            const Vector hmdVa = m_VR->GetViewAngle();
            apply(QAngle(hmdVa.x, hmdVa.y, 0.f));
            return;
        }
        // A live crowbar swing overrides the aim so the engine's melee trace
        // follows the blade instead of the controller's forward ray.
        Vector meleeOrigin{};
        QAngle meleeAim{};
        if (m_VR->TryGetMeleeAim(meleeOrigin, meleeAim))
        {
            apply(meleeAim);
            return;
        }
        QAngle gunAim{};
        if (m_VR->MountedGunAim(gunAim))
        {
            apply(gunAim);
            return;
        }
        // Pickup follow uses the grab hand. Weapon fire / flashlight must not:
        // leftover left-Use latch used to keep viewangles and shoot origin on
        // that hand after a door, button, or charger.
        if (m_VR->UseAimFollowsGrabHand() && m_VR->GrabHandTrackingValid())
        {
            apply(m_VR->UseGrabRedirectsFire() ? m_VR->GetUseAimAngles() : m_VR->GetUseAimAnglesFromEye());
            return;
        }
        // Ladder mode (see above where ladderMode is computed).
        if (ladderMode)
        {
            const Vector hmdVa = m_VR->GetViewAngle();
            float pitch = hmdVa.x;
            if (pitch > 180.f)
                pitch -= 360.f;
            float yaw = hmdVa.y;
            if (ladderJump)
            {
                // Along the stick (head frame), or the view with the stick
                // centred. The turned-round view jumped backwards for a
                // player looking at where they wanted to go.
                pitch = -10.f;   // facing up a little adds the jump's +50 z
                const float f = m_VR->m_WalkForward.load(std::memory_order_acquire);
                const float sd = m_VR->m_WalkSide.load(std::memory_order_acquire);
                if (f * f + sd * sd > 0.2f * 0.2f)
                    yaw -= atan2f(sd, f) * 180.f / 3.14159265f;   // Source yaw grows to the left
            }
            else if (physLadderOn && onLadder && !physLadder.drop && !physLadder.topExit && !physLadder.stickTop)
            {
                // Hand climbing: straight up (climb / hold) or straight
                // down (IN_FORWARD = down, see ladderBack). Fully underwater,
                // HL2's FullLadderMove floats the player off to any
                // info_ladder_dismount faced within 40 units (dot >= 0.7 of
                // the view's flat part), which the head's yaw did mid-span:
                // a grab pushed the player sideways (user 2026-11-19). A
                // vertical view has no flat part.
                pitch = ladderBack ? 89.f : -89.f;
            }
            else if (ladderBack)
            {
                pitch = 45.f;
                yaw += 180.f;
            }
            else if (ladderPhysUp)
                pitch = -30.f;   // looking down at the feet would climb down
            else if (physLadderOn && physLadder.drop)
                pitch = 89.f;    // the use-dismount pushes along the view: straight down = just fall
            else if (physLadderOn && physLadder.topExit)
                pitch = 0.f;     // level head view: the dismount faced, or forward over the edge
            else if (ladderMountAssist)
                yaw = ladderMountYaw;
            static int s_ladderLog;
            if (s_ladderLog < 24)
            {
                Game::logMsg("Ladder aim: %s%s head yaw %.0f pitch %.0f -> yaw %.0f pitch %.0f",
                    onLadder ? "on" : "near",
                    ladderJump ? " jump" : (ladderBack ? " back" : (ladderMountAssist ? " mount" : "")),
                    hmdVa.y, hmdVa.x, yaw, pitch);
                ++s_ladderLog;
            }
            apply(QAngle(pitch, yaw, 0.f));
            return;
        }
        // Looking down a scope, the crosshair is fixed in the middle of the
        // view, so motion aiming has nothing to point at. Aim from the headset
        // while zoomed so the bolt goes where the scope picture is centred.
        if (m_VR->ScopeZoomActive())
        {
            const Vector hmdVa = m_VR->GetViewAngle();
            apply(QAngle(hmdVa.x, hmdVa.y, 0.f));
            return;
        }
        // Falling back to the headset angle sends shots wherever the player is
        // looking, which is normally well above where the gun is pointed, and
        // it sticks for as long as the controller pose stays flagged invalid.
        // Hold the last good controller aim across dropouts instead, and only
        // use the headset before any controller aim has been seen.
        static bool s_haveLastAim = false;
        static QAngle s_lastAim{};
        if (m_VR->m_ControllerPoseValid)
        {
            const QAngle aim = m_VR->GetAimAngles();
            s_lastAim = aim;
            s_haveLastAim = true;
            apply(aim);
        }
        else if (s_haveLastAim)
            apply(s_lastAim);
        else
        {
            const Vector hmdVa = m_VR->GetViewAngle();
            apply(QAngle(hmdVa.x, hmdVa.y, 0.f));
        }
    };

    void* localPlayer = nullptr;
    if (m_Game && m_Game->m_EngineClient && m_Game->m_ClientEntityList)
    {
        const int local = m_Game->m_EngineClient->GetLocalPlayer();
        if (local > 0)
            localPlayer = m_Game->m_ClientEntityList->GetClientEntity(local);
    }
    if (m_VR && m_VR->EmptyHands() && !m_VR->OnMountedGun())
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
    // Quick draw: the deploy wait after a weapon switch (about 0.5 s) cut short.
    if (m_Game && bmvr::g_QuickDrawSec >= 0.f)
        m_Game->ClampServerWeaponDeploy(bmvr::g_QuickDrawSec);
    ApplyAutoSwitchPatches();
    // Predicted FireBullets add m_vecPunchAngle on the client. That punch
    // resets on map load and then walks the visible hit off the crosshair.
    // Zero it for every weapon, not only the MP5.
    if (m_VR)
        ZeroPlayerViewRecoil(localPlayer);
    // FireBullets / ItemPostFrame run inside original player CreateMove.
    // Aim, VR buttons, and weaponselect must be applied before that.
    applyControllerAim();

    auto applyVrUserCmd = [&]() {
        if (!m_VR || !m_VR->m_IsVREnabled)
            return;
        m_VR->FlushPendingWeaponSounds();
        m_VR->FlushPendingGameUi();
        EnsureServerFlashlightHook();
        EnsureWeaponShootOriginHooks();
        SnapLocalViewmodelForFire();

        if (!m_VR->m_ProcessInputEnabled)
            return;

        const uint32_t impulse = m_VR->m_PendingImpulse.load(std::memory_order_acquire);
        if (impulse)
        {
            cmd->impulse = static_cast<byte>(impulse);
            m_VR->m_PendingImpulse.store(0, std::memory_order_release);
            Game::logMsg("CreateMove applied impulse %u cmd=%d buttons=0x%x",
                impulse, cmd->command_number, cmd->buttons);
        }

        const float maxSpeed = 450.f;
        const float analogF = m_VR->m_WalkForward.load(std::memory_order_acquire) * maxSpeed;
        const float analogS = m_VR->m_WalkSide.load(std::memory_order_acquire) * maxSpeed;
        if (m_VR->m_ControllerPoseValid && m_VR->m_HmdPoseValid)
        {
            const Vector hmdVa = m_VR->GetViewAngle();
            const QAngle aim = haveAppliedAim
                ? appliedAim : m_VR->GetRightControllerAbsAngle();
            Vector hmdF, hmdR, hmdU, cF, cR, cU;
            QAngle::AngleVectors(QAngle(0.f, hmdVa.y, 0.f), &hmdF, &hmdR, &hmdU);
            QAngle::AngleVectors(QAngle(0.f, aim.y, 0.f), &cF, &cR, &cU);
            const Vector wish = hmdF * analogF + hmdR * analogS;
            cmd->forwardmove += DotProduct2D(wish, cF);
            cmd->sidemove += DotProduct2D(wish, cR);
            // Swimming: the engine's WaterMove goes along the full 3D view
            // (forward * fmove + right * smove, upmove added to z), and the
            // view is the weapon aim, so a two-handed gun aimed up/down made
            // the player swim up/down (user 2026-11-22). Send the stick as
            // the head's 3D direction expressed in the aim's basis instead.
            if (!ladderMode && m_Game && m_Game->LocalPlayerWaterLevel() >= 2)
            {
                Vector hF, hR, hU, aF, aR, aU;
                QAngle::AngleVectors(QAngle(hmdVa.x, hmdVa.y, 0.f), &hF, &hR, &hU);
                QAngle::AngleVectors(QAngle(aim.x, aim.y, 0.f), &aF, &aR, &aU);
                const Vector w = hF * analogF + hR * analogS;
                const float f = DotProduct(w, aF);
                const float s = DotProduct(w, aR);
                cmd->forwardmove += f - DotProduct2D(wish, cF);
                cmd->sidemove += s - DotProduct2D(wish, cR);
                // z the aim's forward/right do not give (+ stick-held jump/duck adds its own upmove)
                cmd->upmove += w.z - (aF.z * f + aR.z * s);
            }
        }
        else
        {
            cmd->forwardmove += analogF;
            cmd->sidemove += analogS;
        }
        cmd->buttons |= static_cast<int>(m_VR->HeldButtons());
        applyLadderButtons();
        if (ladderJump)
        {
            cmd->forwardmove = 0.f;
            cmd->sidemove = 0.f;
        }
        else if (ladderMountAssist)
        {
            // Whole stick deflection as forward: any sidemove counts as
            // strafing and blocks the engine's auto-mount. A hand mount
            // walks in by itself (at least 40 % of walking speed): in moving
            // water the current otherwise carried the player off the ladder
            // before the use press caught it (user 2026-11-19).
            cmd->forwardmove = sqrtf(analogF * analogF + analogS * analogS);
            if (physLadder.mountPush)
                cmd->forwardmove = (std::max)(cmd->forwardmove, 0.4f * 190.f);
            if (physLadder.mountRise)
                cmd->upmove = (std::max)(cmd->upmove, 190.f);   // swim up into the ladder's range
            cmd->sidemove = 0.f;
        }
        else if (afterLadderJump && cmd->forwardmove > 0.f)
            cmd->forwardmove = 0.f;
        if (ladderStickBlocked && !ladderJump)
        {
            // On a ladder the hands climb; the stick moves nothing.
            cmd->forwardmove = 0.f;
            cmd->sidemove = 0.f;
        }
        const int inv = m_VR->m_PendingInvDelta.exchange(0, std::memory_order_acq_rel);
        if (inv != 0 && m_Game)
        {
            const int weap = m_Game->CycleWeaponSelect(inv > 0 ? 1 : -1);
            if (weap > 0)
            {
                cmd->weaponselect = weap;
                Game::logMsg("Weapon cycle via CUserCmd weaponselect=%d dir=%d", weap, inv);
            }
            else
                Game::logMsg("Weapon cycle skipped (no other weapon) dir=%d", inv);
        }
        const int menuWeap = m_VR->m_PendingWeaponSelect.exchange(0, std::memory_order_acq_rel);
        if (menuWeap > 0)
        {
            cmd->weaponselect = menuWeap;
            Game::logMsg("Weapon menu via CUserCmd weaponselect=%d", menuWeap);
        }
    };

    if (m_VR && m_VR->m_IsVREnabled && cmd->command_number)
        applyVrUserCmd();

    // World vgui_screen (C_VGuiScreen / CMortarControlScreen): hover uses
    // EyeAngles (HMD). DetermineVguiInputMode uses cmd viewangles, which we
    // set to controller aim, so IN_ATTACK never becomes a VGUI click.
    const int buttonsBeforeOriginal = cmd->buttons;
    if (m_VR && m_VR->m_IsVREnabled && cmd->command_number)
    {
        if (m_VR->IsGameplayEligible() && EngineInGame()
            && m_VR->m_HmdPoseValid && !m_VR->IsMenuUp()
            && !m_VR->MenuGameplayBlocked())
        {
            const Vector hmdVa = m_VR->GetViewAngle();
            cmd->viewangles.Init(hmdVa.x, hmdVa.y, 0.f);
        }
    }

    bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);
    TraceRpgRocket(m_Game, m_VR, cmd);

    if (m_VR && cmd->command_number && m_VR->IsGameplayEligible())
        m_VR->m_SeenGameplay = true;

    if (!m_VR || !m_VR->m_IsVREnabled || !cmd->command_number)
        return result;

    // Camera stays HMD on RenderView copies. Shooting uses controller
    // viewangles. Do not hook EyePosition (that would move the camera).
    applyControllerAim();
    if (m_VR)
        ZeroPlayerViewRecoil(localPlayer);
    if (m_VR)
        m_VR->ApplyWorldVguiTriggerClick(cmd);
    if (m_VR)
        m_VR->UpdateFreeHandUseTarget();
    const bool vguiAteAttack = (buttonsBeforeOriginal & (IN_ATTACK | IN_ATTACK2)) != 0
        && (cmd->buttons & (IN_ATTACK | IN_ATTACK2)) == 0;
    if (m_VR && m_VR->EmptyHands() && !m_VR->OnMountedGun())
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
    else if (vguiAteAttack || (m_VR && m_VR->WorldVguiSuppressAttack()))
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
    else
    {
        // Desktop mouse fire is not used in VR (user 2026-11-19: after an
        // alt-tab the gun kept firing from a held desktop button): only the
        // controllers' fire bits (bmvr_desktop_mouse_guard).
        if (m_VR && m_VR->m_IsVREnabled && bmvr::g_DesktopMouseGuard)
        {
            static int s_dropLogs;
            if ((cmd->buttons & (IN_ATTACK | IN_ATTACK2)) && s_dropLogs++ < 20)
                Game::logMsg("Desktop mouse: dropped the game's own fire bits 0x%x from a usercmd",
                    cmd->buttons & (IN_ATTACK | IN_ATTACK2));
            cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
        }
        cmd->buttons |= static_cast<int>(m_VR->HeldButtons());
        if (!m_VR->OnMountedGun())
        {
            ApplyPistolTrigger(m_Game, m_VR, cmd);
            ApplyPistolAltButton(m_Game, m_VR, cmd);
        }
    }
    // Alt-fire while carrying a prop (VR input): the pickup controller's
    // throw is a fire press; IN_ATTACK2 would drop it instead.
    if (m_VR && m_VR->CarryThrowActive())
        cmd->buttons = (cmd->buttons & ~IN_ATTACK2) | IN_ATTACK;
    // Hands-only physical ladders: a Use press near a ladder (left trigger,
    // or the empty right hand) made HL2's LadderMove mount it and pull the
    // player on (user 2026-11-20). Not on a ladder and not during a hand
    // mount, Use within reach of a ladder is dropped unless the view aims
    // at an entity (a door, button or charger beside the ladder still works).
    // The hand mount's own IN_USE is added after this (applyLadderButtons).
    static bool s_ladderUseLogged = false;
    if (!(cmd->buttons & IN_USE))
        s_ladderUseLogged = false;
    // On a ladder, Use (a trigger) let go of it (HL2: +use = get off; user
    // 2026-11-20): hands-only climbing drops it there too; letting go with
    // both hands still gets off (the drop's IN_USE comes from
    // applyLadderButtons below).
    if (m_VR && m_Game && (cmd->buttons & IN_USE) && bmvr::g_PhysicalLadders && !bmvr::g_LadderStickClimb && onLadderNow)
    {
        if (!s_ladderUseLogged)
            Game::logMsg("Ladder: Use on the ladder dropped (hands-only ladders: let go with both hands to get off)");
        s_ladderUseLogged = true;
        cmd->buttons &= ~IN_USE;
    }
    if (m_VR && m_Game && (cmd->buttons & IN_USE) && bmvr::g_PhysicalLadders && !bmvr::g_LadderStickClimb
        && !onLadderNow && !m_VR->m_LadderMountActive.load(std::memory_order_relaxed)
        && m_Game->m_EngineTrace && m_VR->m_SetupOrigin.LengthSqr() > 1.f)
    {
        Vector origin{};
        Vector bottoms[32], tops[32];
        const int n = m_Game->LocalPlayerAbsOrigin(origin) ? m_Game->CollectLadders(bottoms, tops, 32) : 0;
        bool nearLadder = false;
        for (int i = 0; i < n && !nearLadder; ++i)
        {
            const Vector axis = tops[i] - bottoms[i];
            const float len2 = axis.LengthSqr();
            float t = len2 > 1.f ? (origin - bottoms[i]).Dot(axis) / len2 : 0.f;
            t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
            const Vector c = bottoms[i] + axis * t;
            const float dx = c.x - origin.x, dy = c.y - origin.y;
            const float zLo = (std::min)(bottoms[i].z, tops[i].z) - 72.f;
            const float zHi = (std::max)(bottoms[i].z, tops[i].z) + 72.f;
            nearLadder = dx * dx + dy * dy < 120.f * 120.f && origin.z > zLo && origin.z < zHi;
        }
        if (nearLadder)
        {
            Vector dir{};
            QAngle::AngleVectors(cmd->viewangles, &dir, nullptr, nullptr);
            C_BaseEntity* player = m_Game->GetLocalPlayerEntity();
            C_BaseEntity* vm = m_Game->GetViewModelEntity();
            CTraceFilterSkipTwoEntities filter(player, vm, 0);
            Ray_t ray;
            ray.Init(m_VR->m_SetupOrigin, m_VR->m_SetupOrigin + dir * 96.f);
            CGameTrace tr{};
            tr.m_pEnt = nullptr;
            m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
            C_BaseEntity* world = m_Game->GetClientEntity(0);
            const bool atEntity = tr.fraction < 1.f && tr.m_pEnt && tr.m_pEnt != world;
            if (!atEntity)
            {
                if (!s_ladderUseLogged)
                    Game::logMsg("Ladder: Use near a ladder dropped (hands-only ladders; aim hits %s)",
                        tr.fraction < 1.f ? "the world" : "nothing");
                cmd->buttons &= ~IN_USE;
            }
            s_ladderUseLogged = true;
        }
    }
    applyLadderButtons();

    EnsureServerFlashlightHook();
    EnsureWeaponShootOriginHooks();

    m_VR->AfterCreateMoveFireHaptics();
    return result;
}

void __fastcall Hooks::dLevelInit(void* ecx, void* edx, const char* newmap)
{
    (void)edx;
    // server.dll is loaded by now; check its offsets before the server hooks patch them.
    bmvr::VerifyGameOffsets();
    // The cached gun header belongs to the previous map's model, which a
    // load frees (user 2026-11-20: crash loading a save after the crossbow;
    // the last faults were reads of it). The next draw sets it again.
    g_LastViewmodelStudioHdr = nullptr;
    EnsureServerFlashlightHook();
    EnsureClientFlashlightHook();
    EnsureFogColorPatch();
    EnsureWeaponShootOriginHooks();
    if (m_VR)
    {
        m_VR->OnLevelInit(newmap);
    }
    if (hkLevelInit.fOriginal)
        hkLevelInit.fOriginal(ecx, newmap);
}

void __fastcall Hooks::dLevelShutdown(void* ecx, void* edx)
{
    (void)edx;
    g_LastViewmodelStudioHdr = nullptr;
    if (m_VR)
        m_VR->OnLevelShutdown();
    if (hkLevelShutdown.fOriginal)
        hkLevelShutdown.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
    BMVR_PROF("hook CalcViewModelView");
    (void)edx;
    if (!hkCalcViewModelView.fOriginal)
        return;
    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && EngineInGame() && m_VR->m_HmdPoseValid)
    {
        // Hand climbing lifts the view (PhysicalLadderViewOffset, last
        // frame's value): the gun hand moves with it.
        Vector ladderEye = eyePosition;
        ladderEye.z += m_VR->m_LadderView.offset + m_VR->m_CrouchView.offset;
        ladderEye.x += m_VR->m_LadderView.shift.x;
        ladderEye.y += m_VR->m_LadderView.shift.y;
        // Uncoupled viewmodel: same world pose both eyes (sd805 / Portal 2).
        // Do not IPD the gun — that drew two weapons (2026-08-17).
        if (m_VR->m_ControllerPoseValid)
        {
            // One active-weapon lookup per call (was three). The MP5 test on
            // the last classified viewmodel is the lock-free class flag rather
            // than an unlocked read of the std::string.
            const char* weaponModel = m_Game ? m_Game->GetActiveWeaponModelName() : nullptr;
            if (weaponModel)
                m_VR->NoteViewmodelModel(weaponModel);
            // NoteViewmodelModel just classified weaponModel (mp5/MP5/smg/mp5k
            // all match on the lowercased path), so the flag is authoritative.
            const bool mp5 = m_Game && m_VR->ViewmodelIsMp5();
            // L4D2VR: feed the aim-controller pose as CalcViewModelView eye so
            // lag/bob and $origin are in the same frame as the hard-lock.
            const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos(ladderEye);
            const QAngle targetAng = m_VR->GetRecommendedViewmodelAbsAngle();
            // Crowbar idle-force runs in SuppressViewmodelMovementAnims.
            // MP5 fire sequences must keep playing (never idle-force those).
            if (mp5)
                ZeroPlayerViewRecoil(owner);
            SuppressViewmodelMovementAnims(ecx);
            CallCalcViewModelViewOriginal(ecx, owner, targetOrigin, targetAng);
            const float origin3[3] = { targetOrigin.x, targetOrigin.y, targetOrigin.z };
            const float angles3[3] = { targetAng.x, targetAng.y, targetAng.z };
            CallSetAbsOriginAngles(ecx, origin3, angles3);
            if (mp5)
                ZeroPlayerViewRecoil(owner);
            SuppressViewmodelMovementAnims(ecx);
            return;
        }
        // Controller pose dropped: keep the gun on the last tracking pose.
        // Feeding HMD origin/angles here parented the weapon to nod.
        const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos(ladderEye);
        const QAngle targetAng = m_VR->GetRecommendedViewmodelAbsAngle();
        CallCalcViewModelViewOriginal(ecx, owner, targetOrigin, targetAng);
        const float origin3[3] = { targetOrigin.x, targetOrigin.y, targetOrigin.z };
        const float angles3[3] = { targetAng.x, targetAng.y, targetAng.z };
        CallSetAbsOriginAngles(ecx, origin3, angles3);
        return;
    }
    hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
}

BOOL WINAPI Hooks::dGetCursorPos(LPPOINT point)
{
    // While a VR menu pointer is live, report it; otherwise the real cursor.
    if (point && m_VR && m_VR->TryGetMenuCursorScreen(*point))
    {
        m_VR->m_CursorHookHits.fetch_add(1, std::memory_order_relaxed);
        m_VR->m_CursorHookHitsSec.fetch_add(1, std::memory_order_relaxed);
        return TRUE;
    }
    if (m_VR)
    {
        m_VR->m_CursorHookPass.fetch_add(1, std::memory_order_relaxed);
        m_VR->m_CursorHookPassSec.fetch_add(1, std::memory_order_relaxed);
    }
    return hkGetCursorPos.fOriginal ? hkGetCursorPos.fOriginal(point) : FALSE;
}

BOOL WINAPI Hooks::dSetCursorPos(int x, int y)
{
    // Never move the pointer while the game window is not in the foreground
    // (alt-tabbed): the VR menu pointer goes through the GetCursorPos hook
    // and needs no real warp then.
    HWND game = bmvr::GameWindow();
    if (m_VR && m_VR->m_IsVREnabled && game && GetForegroundWindow() != game)
    {
        static std::atomic<unsigned> s_blocked{ 0 };
        const unsigned n = s_blocked.fetch_add(1, std::memory_order_relaxed);
        if (n < 3 || (n & (n - 1)) == 0)
            Game::logMsg("SetCursorPos(%d,%d) skipped: game window not in the foreground (%u so far)", x, y, n + 1);
        return TRUE;
    }
    return hkSetCursorPos.fOriginal ? hkSetCursorPos.fOriginal(x, y) : FALSE;
}

float __fastcall Hooks::dGetViewModelFOV(void* ecx, void* edx)
{
    BMVR_PROF("hook GetViewModelFOV");
    (void)edx;
    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && EngineInGame() && m_VR->m_Fov > 10.f)
        return m_VR->EyeRenderFov(m_VR->m_StereoEye);
    if (hkGetViewModelFOV.fOriginal)
        return hkGetViewModelFOV.fOriginal(ecx);
    return 54.f;
}

void __fastcall Hooks::dAdjustEngineViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    BMVR_PROF("hook AdjustEngineViewport");
    if (hkAdjustEngineViewport.fOriginal)
        hkAdjustEngineViewport.fOriginal(ecx, x, y, width, height);
    ClampStereoViewport(x, y, width, height);
}

void __fastcall Hooks::dViewport(void* ecx, void* edx, int x, int y, int width, int height)
{
    BMVR_PROF("hook Viewport");
    NoteMatContext(ecx);
    const int trW = width, trH = height;
    if (!EyeBloomOwnRtViewport(ecx, x, y, width, height))
        ClampStereoViewport(x, y, width, height);
    if (bmvr::g_PassTraceFrames > 0)
        Game::logMsg("trace: Viewport %dx%d -> %d,%d %dx%d", trW, trH, x, y, width, height);
    if (hkViewport.fOriginal)
        hkViewport.fOriginal(ecx, x, y, width, height);
}

void __fastcall Hooks::dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    BMVR_PROF("hook GetViewport");
    (void)edx;
    NoteMatContext(ecx);
    CallOriginalGetViewport(ecx, x, y, width, height);
    if (!EyeBloomOwnRtViewport(ecx, x, y, width, height))
        ClampStereoViewport(x, y, width, height);
}

void __fastcall Hooks::dGetRenderTargetDimensions(void* ecx, void* edx, int& width, int& height)
{
    BMVR_PROF("hook GetRenderTargetDimensions");
    (void)edx;
    NoteMatContext(ecx);
    const bool queuedStub = CallOriginalGetRenderTargetDimensions(ecx, width, height);
    ApplyHwndStereoRtDimLie(width, height, queuedStub);
}

namespace
{
    const char* SehMaterialName(IMaterial* material)
    {
        BMVR_TRY
        {
            return material ? material->GetName() : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }
}

void Hooks::SetMatBindHooks(bool on)
{
    for (Hook<tMatBind>* hk : { &hkMatBind, &hkMatBindQueued })
    {
        if (!hk->pTarget || hk->isEnabled == on)
            continue;
        if ((on ? MH_EnableHook(hk->pTarget) : MH_DisableHook(hk->pTarget)) == MH_OK)
            hk->isEnabled = on;
    }
}

void __fastcall Hooks::dMatBind(void* ecx, void* edx, IMaterial* material, void* proxyData)
{
    BMVR_PROF("hook MatBind");
    (void)edx;
    // Enabled only while an audit runs: names the material of each draw.
    BmvrAuditNoteMaterial(SehMaterialName(material));
    void* slot = SehContextVtableSlot(ecx, Offsets::kIMatRenderContext_Bind);
    if (hkMatBindQueued.fOriginal && slot == hkMatBindQueued.pTarget)
        hkMatBindQueued.fOriginal(ecx, material, proxyData);
    else if (hkMatBind.fOriginal)
        hkMatBind.fOriginal(ecx, material, proxyData);
}

// Glow occlusion queries count the proxy's "possible" pixels under
// DepthRange(0, 0.01) (client CPixelVisibilitySystem, client+0xD42B2), which
// keeps them in front of everything. The viewmodel rewrite below (Source's
// viewmodel range is 0..0.1) turned that into 0..1 as well, so the count was
// depth-tested like the visible query, visible == possible, and a glow showed
// at full brightness as soon as any sliver of its proxy peeked out (police car
// rear light through the car, bm_c2a5a). Matched by value, because with
// MulticoreMode the call reaches this hook again from the queue's playback.
static bool IsGlowCountDepthRange(float zNear, float zFar)
{
    return zNear == 0.f && zFar > 0.0099f && zFar < 0.0101f;
}

void __fastcall Hooks::dDepthRange(void* ecx, void* edx, float zNear, float zFar)
{
    BMVR_PROF("hook DepthRange");
    (void)edx;
    NoteMatContext(ecx);
    if (StereoViewmodelUsesWorldDepth() && IsCompressedViewmodelDepthRange(zNear, zFar)
        && !(bmvr::g_GlowCountDepthFix && IsGlowCountDepthRange(zNear, zFar)))
    {
        static int s_drLog;
        if (s_drLog < 8)
        {
            Game::logMsg("DepthRange %.3f,%.3f -> 0,1 (viewmodel world Z)", zNear, zFar);
            ++s_drLog;
        }
        zNear = 0.f;
        zFar = 1.f;
    }
    CallOriginalDepthRange(ecx, zNear, zFar);
}

// Eye at the water line. When the near plane crosses the water surface, the engine draws the eye view in two parts
// (SDK CAboveWaterView, hardware clip planes): the main view clipped to "above the water" (PushView ->
// SetHeightClipMode(RENDER_ABOVE_HEIGHT)), then CIntersectionView for the part under the surface, clipped to
// "below". Black Mesa is deferred and the intersection view has no G-buffer pass of its own, so it shades with the
// main view's light buffer, which the above-water clip left empty under the surface: the lower part of the view went
// black / flat fog green, ending at a straight line (crouched or swimming in bm_c2a5a, 2026-11-16 capture 213843;
// mat_clipz 0, which skips the clip, fixed it). Drop that clip for the eye view only: water reflection/refraction
// views push their own RT (AuxSceneRtBound) and keep it, and the intersection view's "below" clip is untouched.
// Both threads see the same RT pushes in the same order (thread-local depth), so the queued and the hardware
// context decide alike.
static int WaterLineClipMode(int mode)
{
    constexpr int kRenderAboveHeight = 1, kDisable = 0;
    if (mode != kRenderAboveHeight || !bmvr::g_WaterLineClipFix || !Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled
        || !Hooks::m_VR->StereoWorldPassActive() || AuxSceneRtBound())
        return mode;
    static int s_log;
    if (s_log < 4 || bmvr::g_PassTraceFrames > 0)
    {
        Game::logMsg("Water line: eye %d view not clipped above the water (stock: under-water part unlit)",
            Hooks::m_VR->m_StereoEye);
        ++s_log;
    }
    return kDisable;
}

void __fastcall Hooks::dSetHeightClipMode(void* ecx, void* edx, int mode)
{
    (void)edx;
    // MulticoreMode: the queued context decided on the main thread (and calls this one inline with
    // the result); calls here on the render thread are its replay, so pass them through.
    hkSetHeightClipMode.fOriginal(ecx, bmvr::g_MulticoreMode ? mode : WaterLineClipMode(mode));
}

void __fastcall Hooks::dSetHeightClipModeQueued(void* ecx, void* edx, int mode)
{
    (void)edx;
    hkSetHeightClipModeQueued.fOriginal(ecx, WaterLineClipMode(mode));
}

void __fastcall Hooks::dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld)
{
    (void)edx;
    const long long dmeEnter = g_Cost.active ? QpcNow() : 0;
    char modelNameBuf[260]{};
    bool hideArms = false;
    bool skipLocal = false;
    bool isViewmodel = false;
    const char* modelName = nullptr;
    int body = 0;
    int skin = 0;
    int entityIndex = 0;
    void* pModel = nullptr;
    BMVR_TRY
    {
        pModel = info.pModel;
        entityIndex = info.entity_index;
        body = info.body;
        skin = info.skin;
        if (bmvr::g_HideLocalPlayerModel
            && m_VR
            && m_VR->IsGameplayEligible())
        {
            const int local = CachedLocalPlayerIndex();
            if (local > 0 && entityIndex == local)
                skipLocal = true;
        }
        // World props: skip GetModelName. Open maps call DME thousands of times
        // per eye; the name string was the hot cost next to the draw itself.
        isViewmodel = CachedModelIsViewmodel(pModel);
        if (isViewmodel && m_Game && m_Game->m_ModelInfo)
        {
            modelName = SafeModelName(m_Game->m_ModelInfo, pModel);
            if (modelName && modelName[0])
            {
                strncpy_s(modelNameBuf, modelName, _TRUNCATE);
                modelName = modelNameBuf;
            }
        }
        static int s_dmeEnter;
        if (s_dmeEnter < 6)
        {
            Game::logMsg("DME %s viewmodel=%d eligible=%d body=%d skin=%d ent=%d bones=%d",
                modelName ? modelName : "?", isViewmodel ? 1 : 0,
                (m_VR && m_VR->IsGameplayEligible()) ? 1 : 0,
                body, skin, entityIndex, pCustomBoneToWorld ? 1 : 0);
            ++s_dmeEnter;
        }
        // Body (bodygroup) changes of the drawn viewmodel, e.g. the RPG's
        // rocket, which client animation events switch on and off.
        if (isViewmodel && modelName)
        {
            static char s_bodyModel[96]{};
            static int s_lastBody = -1;
            static int s_bodyLogs = 0;
            if (std::strncmp(s_bodyModel, modelName, sizeof(s_bodyModel) - 1) != 0)
            {
                strncpy_s(s_bodyModel, modelName, _TRUNCATE);
                s_lastBody = body;
            }
            else if (body != s_lastBody && s_bodyLogs < 200)
            {
                ++s_bodyLogs;
                Game::logMsg("Viewmodel body %s %d -> %d", modelName, s_lastBody, body);
                s_lastBody = body;
            }
        }
        hideArms = bmvr::g_HideViewmodelArms
            && !GunRightHandForModel(modelName)
            && isViewmodel
            && m_VR
            && m_VR->m_IsVREnabled
            && m_VR->IsGameplayEligible()
            && EngineInGame();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        hideArms = false;
        skipLocal = false;
        isViewmodel = false;
        Game::logMsg("DME info SEH - passthrough original");
    }
    // Satchel (VrSatchelHands): the bag is thrown by the left hand, so the
    // stock throw clip (and the bag in it) is not drawn.
    if (isViewmodel && modelName && m_VR && m_VR->SatchelHandsActive() && std::strstr(modelName, "v_satchel")
        && !std::strstr(modelName, "v_satchel_radio") && SatchelThrowClipShowing(state))
        return;

    if (g_Cost.active)
    {
        ++g_Cost.dme;
        if (g_Cost.eye == 1)
            ++g_Cost.dmeL;
        else if (g_Cost.eye == 2)
            ++g_Cost.dmeR;
        if (AuxSceneRtBound())
            ++g_Cost.dmeShadow;
        if (isViewmodel)
            ++g_Cost.dmeVm;
        if (entityIndex > 1)
        {
            ++g_Cost.dmeEnt;
            CostMarkEnt(entityIndex);
        }
        if (state)
        {
            const auto* lite = reinterpret_cast<const DrawModelStateLite*>(state);
            const unsigned char* hdr = lite->studioHdr;
            if (hdr)
            {
                const int nb = *reinterpret_cast<const int*>(hdr + kStudioHdrNumBones);
                if (nb >= 32 && nb <= kMaxStudioBones)
                {
                    ++g_Cost.dmeBones32;
                    if (pModel)
                        g_Cost.lastCharModel = pModel;
                }
            }
        }
        g_Cost.dmeHookTicks += QpcNow() - dmeEnter;
    }

    if (skipLocal)
    {
        static int s_hideLog;
        if (s_hideLog < 4)
        {
            Game::logMsg("HideLocalPlayerModel skip %s", modelName ? modelName : "?");
            ++s_hideLog;
        }
        return;
    }

    if (isViewmodel && m_VR && m_VR->EmptyHands() && m_VR->IsGameplayEligible() && EngineInGame())
        return;

    // Pause / GameUI: keep controller poses updating, skip the gun mesh so it
    // does not track in front of the overlay. Next unpaused DME draws it again.
    if (isViewmodel && m_VR && m_VR->IsMenuUp() && m_VR->IsGameplayEligible() && EngineInGame())
        return;

    const bool vrViewmodelPass = m_VR && m_VR->PassThroughWarmupDone()
        && m_VR->StereoWorldPassActive();
    // Turning the gun's own right hand on gives back the arm meshes hidden
    // for this map; off zeroes them again on the next draw.
    // Keyed to the setting, not the model: the gun and BM's always-idle
    // hands viewmodel take turns here. Models outside GunRightHandForModel
    // get hideArms and are zeroed again on their next draw.
    static bool s_gunRightHand = false;
    const bool gunRightHandSetting = bmvr::g_VrGunRightHand && !bmvr::g_LeftHanded;
    if (isViewmodel && gunRightHandSetting != s_gunRightHand)
    {
        s_gunRightHand = gunRightHandSetting;
        if (gunRightHandSetting)
            RestoreArmBodypartMeshes();
    }
    const bool gunRightHand = isViewmodel && GunRightHandForModel(modelName);
    if (isViewmodel && vrViewmodelPass && m_VR->IsGameplayEligible() && EngineInGame())
        ApplyViewmodelStudioWork(state, modelName, body, skin, hideArms, pCustomBoneToWorld);

    void* bonesToDraw = pCustomBoneToWorld;
    const ModelRenderInfo_t* infoToDraw = &info;
    if (isViewmodel && vrViewmodelPass && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
        && EngineInGame() && m_VR->m_ControllerPoseValid)
    {
        const float scale = [&]() -> float {
            float s = bmvr::g_ViewmodelScale;
            if (modelName && (std::strstr(modelName, "crowbar") || std::strstr(modelName, "wrench")
                || std::strstr(modelName, "Crowbar") || std::strstr(modelName, "Wrench")))
                s = 1.f;
            if (modelName && (std::strstr(modelName, "shotgun") || std::strstr(modelName, "spas")
                || std::strstr(modelName, "pump")))
                s = 0.64f;
            if (modelName && (std::strstr(modelName, "grenade") || std::strstr(modelName, "Grenade")
                || std::strstr(modelName, "frag") || std::strstr(modelName, "Frag")))
                s *= 1.25f;
            if (modelName && (std::strstr(modelName, "357") || std::strstr(modelName, "python")
                || std::strstr(modelName, "revolver") || std::strstr(modelName, "Revolver")))
                s *= 1.15f;
            if (s < 0.2f)
                s = 0.2f;
            if (s > 1.5f)
                s = 1.5f;
            return s;
        }();
        float yFix = 1.f;
        bool eyePass = m_VR->m_StereoEye != 0 || m_VR->StereoEyeBlitActive();
        int vw = 0, vh = 0;
        if (g_MatCtx && hkGetViewport.fOriginal)
        {
            int vx = 0, vy = 0;
            hkGetViewport.fOriginal(g_MatCtx, vx, vy, vw, vh);
            ClampStereoViewport(vx, vy, vw, vh);
            if (!eyePass && vw >= 640 && vh >= 360 && m_VR->m_RenderWidth >= 640
                && m_VR->m_RenderHeight >= 360)
            {
                const float vpAspect = static_cast<float>(vw) / static_cast<float>(vh);
                const float eyeAspect = static_cast<float>(m_VR->m_RenderWidth)
                    / static_cast<float>(m_VR->m_RenderHeight);
                if (fabsf(vpAspect - eyeAspect) < 0.12f)
                    eyePass = true;
            }
        }
        // Stereo CViewSetup is already eye-sized (worldMatch) or uses HMD
        // m_flAspectRatio. DrawViewModels still forced window aspect via
        // GetScreenAspectRatio; that is hooked separately. Do not unstretch
        // around the HMD — that parented the grip to the look plane.
        if (eyePass && vw >= 640 && vh >= 360 && m_VR->m_RenderWidth >= 640
            && m_VR->m_RenderHeight >= 360)
        {
            const float vpAspect = static_cast<float>(vw) / static_cast<float>(vh);
            const float eyeAspect = static_cast<float>(m_VR->m_RenderWidth)
                / static_cast<float>(m_VR->m_RenderHeight);
            if (vpAspect > 0.5f && eyeAspect > 0.5f)
                yFix = eyeAspect / vpAspect;
        }
        const bool needScale = scale > 0.2f && scale < 1.5f && fabsf(scale - 1.f) > 0.001f;
        const bool needYFix = yFix > 0.2f && yFix < 2.f && fabsf(yFix - 1.f) > 0.03f;
        {
            const int slot = g_VmDrawSlot++ & (kVmDrawSlots - 1);
            Vector pivot = info.origin;
            const Vector body = m_VR->m_HasStereoBodyOrigin ? m_VR->m_StereoBodyOrigin : m_VR->m_SetupOrigin;
            if (body.LengthSqr() > 1.f)
                pivot = m_VR->GetRightControllerAbsPos(body);
            const float pivot3[3] = { pivot.x, pivot.y, pivot.z };

            const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos(
                body.LengthSqr() > 1.f ? body : info.origin);
            const QAngle targetAng = m_VR->GetRecommendedViewmodelAbsAngle();
            float rigidDelta[3][4]{};
            const float* rigidPtr = nullptr;
            if (info.origin.LengthSqr() > 1.f)
            {
                float origEntity[3][4]{};
                float origInv[3][4]{};
                float targetEntity[3][4]{};
                BuildMatrix3x4FromOrgAngles(origEntity, info.origin, info.angles);
                InvertMatrix3x4TR(origEntity, origInv);
                BuildMatrix3x4FromOrgAngles(targetEntity, targetOrigin, targetAng);
                MulMatrix3x4(targetEntity, origInv, rigidDelta);
                rigidPtr = &rigidDelta[0][0];
            }

            Vector fwd, right, up;
            float right3[3]{};
            float up3[3]{};
            float fwd3[3]{};
            if (needYFix)
            {
                m_VR->GetViewBasis(&fwd, &right, &up);
                right3[0] = right.x; right3[1] = right.y; right3[2] = right.z;
                up3[0] = up.x; up3[1] = up.y; up3[2] = up.z;
                fwd3[0] = fwd.x; fwd3[1] = fwd.y; fwd3[2] = fwd.z;
            }

            unsigned char* hdr = ResolveStudioHdr(state);
            const int nBones = StudioHdrNumBones(hdr);
            const float* boneDelta = rigidPtr;
            float grenadeDelta[3][4]{};
            const bool snarkModel = modelName && (std::strstr(modelName, "v_squeak") || std::strstr(modelName, "v_Squeak"));
            if (rigidPtr && pCustomBoneToWorld && nBones > 0 && modelName
                && (std::strstr(modelName, "v_grenade") || std::strstr(modelName, "v_Grenade") || snarkModel))
            {
                float entity[3][4]{}, hold[3][4]{};
                BuildMatrix3x4FromOrgAngles(entity, info.origin, info.angles);
                if (GrenadeHoldCorrection(hdr, pCustomBoneToWorld, nBones, entity, hold, snarkModel))
                {
                    MulMatrix3x4(rigidDelta, hold, grenadeDelta);
                    boneDelta = &grenadeDelta[0][0];
                }
            }
            bool usedBones = false;
            if (pCustomBoneToWorld && nBones > 0
                && SehCopyAndFixViewmodelMatrices(&g_ScaledViewmodelBones[slot][0][0][0], pCustomBoneToWorld,
                    nBones, pivot3, needScale ? scale : 1.f, pivot3, right3, up3, fwd3,
                    needYFix ? yFix : 1.f, boneDelta))
            {
                bonesToDraw = g_ScaledViewmodelBones[slot];
                usedBones = true;
                DumpClipHandPose(hdr, pCustomBoneToWorld, nBones);
                StabilizeScrubbedBody(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones,
                    needScale ? pivot + (targetOrigin - pivot) * scale : targetOrigin, targetAng);
                HoldLeftHandOnMp5Body(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones);
                // Gun frame of this draw: scaled origin + target angles
                // (VR::GunFrameNow at the same instant).
                Vector o = targetOrigin;
                if (needScale)
                    o = pivot + (targetOrigin - pivot) * scale;
                Vector gf, gr, gu;
                QAngle::AngleVectors(targetAng, &gf, &gr, &gu);
                Vector drawnMuzzle{};
                if (DrawnAttachmentPoint(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, drawnMuzzle))
                {
                    const Vector d = drawnMuzzle - o;
                    m_VR->NoteDrawnMuzzleLocal(Vector(d.Dot(gf), d.Dot(gr), d.Dot(gu)));
                }
                SampleViewmodelOffhandGrip(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones);
                SampleStockOffhandFingers(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones);
                if (gunRightHand)
                {
                {
                    // Support hand locks to the gun: the stock left hand stays
                    // while the left hand holds the gun (not mid-reload, not
                    // while a stock clip is scrubbed by the hands).
                    const int magSt = MagReload::g_State.load(std::memory_order_relaxed);
                    // The MP5 handle / glock slide take the left hand off
                    // the grip; the shotgun pump keeps it (it rides the pump).
                    const bool otherGrab = MagReload::g_BoltGrab.load(std::memory_order_relaxed)
                        && MagReload::g_Weapon.load(std::memory_order_relaxed) != MagReload::kWeaponShotgun;
                    // Debug hand pose dump: the clip's own left hand stays drawn.
                    const bool dumpHold = bmvr::g_DebugDumpHandPose && bmvr::g_DebugVmSeq >= 0.f;
                    // VrClipHandSnap: the gun's own left hand goes onto the
                    // held MP5 bolt / glock slide (posed after the bolt moves,
                    // SnapLeftHandToClipObject).
                    t_SnapClipPose = -1;
                    if (bmvr::g_VrClipHandSnap && !dumpHold)
                    {
                        const int want = MagReload::WantedClipHandPose();
                        if ((want == ClipHandPose::kMp5Bolt && std::strstr(modelName, "v_mp5"))
                            || (want == ClipHandPose::kGlockSlide && std::strstr(modelName, "v_glock")))
                            t_SnapClipPose = want;
                    }
                    t_KeepLeftHand = dumpHold || t_SnapClipPose >= 0 || (bmvr::g_VrSupportHandLock && !bmvr::g_LeftHanded
                        && SupportHandFromGunModel(modelName)
                        && m_VR->m_VmClass.twoHand && (m_VR->TwoHandGripActive() || bmvr::g_VrSupportHandForce)
                        && !MagReload::g_ScrubActive.load(std::memory_order_relaxed)
                        && magSt != MagReload::kInHand && !otherGrab);
                    CollapseViewmodelArmBones(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, t_KeepLeftHand);
                    if (t_KeepLeftHand && t_LeftHandCount > 0)
                    {
                        const DWORD nowMs = GetTickCount();
                        MagReload::g_SupportHandFromGunMs.store(nowMs ? nowMs : 1, std::memory_order_relaxed);
                    }
                    else if (!std::strstr(modelName, "v_satchel"))
                    {
                        // Let go: the glove comes back with the next glove
                        // draw, not 150 ms later (user 2026-11-19: the left
                        // hand vanished for a frame on release).
                        MagReload::g_SupportHandFromGunMs.store(0, std::memory_order_relaxed);
                    }
                }
                    // Crowbar (scale 1) and grenade (x1.25): hand back to the
                    // gun hands' size (user 2026-11-18: grenade hand too big).
                    if (m_VR->m_VmClass.crowbar || std::strstr(modelName, "v_grenade"))
                        ShrinkViewmodelRightHand(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones,
                            bmvr::g_ViewmodelScale / scale);
                    if (std::strstr(modelName, "v_glock") || std::strstr(modelName, "v_357") || std::strstr(modelName, "v_mp5"))
                        NoteGunWristReference(m_VR, hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, modelName);
                    else if (std::strstr(modelName, "v_grenade") || std::strstr(modelName, "v_squeak")
                        || std::strstr(modelName, "v_tripmine"))
                        AlignWristToReference(m_VR, hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones);
                    if (std::strstr(modelName, "v_satchel_radio") && m_VR->SatchelHandsActive())
                    {
                        PressDetonatorThumb(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones);
                        SatchelRadioToRightHand(m_VR, hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, pivot);
                    }
                    LogGunFistVsController(m_VR, hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, pivot,
                        modelName);
                }
                if (modelName && std::strstr(modelName, "v_satchel") && !std::strstr(modelName, "v_satchel_radio")
                    && m_VR->SatchelHandsActive())
                    SatchelBagToLeftHand(m_VR, hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones);
                if (m_VR->m_VmClass.crowbar)
                    NoteDrawnCrowbarBar(m_VR, hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, pivot);
                ApplyMagReloadBones(m_VR, hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, o, targetAng,
                    needScale ? scale : 1.f);
                if (gunRightHand && t_SnapClipPose >= 0 && t_KeepLeftHand)
                    SnapLeftHandToClipObject(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, t_SnapClipPose);
                ApplyGrenadePinBones(m_VR, hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones,
                    needScale ? scale : 1.f);
                if (gunRightHand)
                    PublishGunWrists(hdr, &g_ScaledViewmodelBones[slot][0][0][0], nBones, false);
                else if (modelName && !std::strstr(modelName, "v_satchel"))
                {
                    // A viewmodel without the gun's own hands: no cap, no
                    // hidden support glove from the last gun.
                    MagReload::g_GunWristMs[1].store(0, std::memory_order_release);
                    MagReload::g_SupportHandFromGunMs.store(0, std::memory_order_relaxed);
                }
            }
            else if (needScale)
                SehWriteModelScale(info.pRenderable, scale);

            std::memcpy(g_ScaledViewmodelInfo[slot], &info, sizeof(ModelRenderInfo_t));
            auto* scaledInfo = reinterpret_cast<ModelRenderInfo_t*>(g_ScaledViewmodelInfo[slot]);
            scaledInfo->origin = targetOrigin;
            scaledInfo->angles = targetAng;
            float origin3[3] = { targetOrigin.x, targetOrigin.y, targetOrigin.z };
            if (needScale)
            {
                origin3[0] = pivot.x + (origin3[0] - pivot.x) * scale;
                origin3[1] = pivot.y + (origin3[1] - pivot.y) * scale;
                origin3[2] = pivot.z + (origin3[2] - pivot.z) * scale;
            }
            scaledInfo->origin.x = origin3[0];
            scaledInfo->origin.y = origin3[1];
            scaledInfo->origin.z = origin3[2];
            if (info.pModelToWorld
                && SehCopyAndFixViewmodelMatrices(&g_ScaledViewmodelModelToWorld[slot][0][0], info.pModelToWorld,
                    1, pivot3, needScale ? scale : 1.f, pivot3, right3, up3, fwd3,
                    needYFix ? yFix : 1.f, rigidPtr))
            {
                scaledInfo->pModelToWorld = reinterpret_cast<const matrix3x4_t*>(g_ScaledViewmodelModelToWorld[slot]);
            }
            infoToDraw = scaledInfo;
            static int s_scaleLog;
            if (s_scaleLog < 8)
            {
                Game::logMsg("Viewmodel DME scale=%.2f yFix=%.3f eyePass=%d rigid=%d bones=%d usedBones=%d origin=(%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)",
                    scale, yFix, eyePass ? 1 : 0, rigidPtr ? 1 : 0, nBones, usedBones ? 1 : 0,
                    info.origin.x, info.origin.y, info.origin.z,
                    scaledInfo->origin.x, scaledInfo->origin.y, scaledInfo->origin.z);
                ++s_scaleLog;
            }
        }
    }

    if (hkDrawModelExecute.fOriginal)
    {
        BMVR_TRY
        {
            if (isViewmodel && m_VR && m_VR->StereoWorldPassActive())
                ForceStereoViewmodelWorldDepth(g_MatCtx);
            const long long t0 = g_Cost.active ? QpcNow() : 0;
            hkDrawModelExecute.fOriginal(ecx, state, *infoToDraw, bonesToDraw);
            if (g_Cost.active)
                g_Cost.dmeOrigTicks += QpcNow() - t0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Game::logMsg("DME original SEH %s", modelName ? modelName : "?");
        }
    }

    // Studio draw is queued after this returns (NO_DRAW restore-after-original
    // left HEV arms visible 2026-08-19). Keep arms nummeshes=0 until map end.
}

void* __cdecl Hooks::dLightcacheFindNearest(int x, int y, int z, int flags)
{
    BMVR_PROF("hook LightcacheFindNearest");
    // public-beta lightcache nodes are not stride 0x1E0. The walker treated
    // a real next index (45106) as a cycle, wrote the sentinel into the pool,
    // and engine.dll faulted on bm_c1a0a. Call the engine lookup.
    if (hkLightcacheFindNearest.fOriginal)
        return hkLightcacheFindNearest.fOriginal(x, y, z, flags);
    if (!g_LcLockReady)
        return LightcacheFindNearestImpl(x, y, z, flags);
    EnterCriticalSection(&g_LcLock);
    void* found = LightcacheFindNearestImpl(x, y, z, flags);
    LeaveCriticalSection(&g_LcLock);
    return found;
}

void Hooks::RestoreViewmodelArmHides()
{
    RestoreArmBodypartMeshes();
}

void Hooks::ForgetViewmodelStudioHdr()
{
    g_LastViewmodelStudioHdr = nullptr;
}

void __fastcall Hooks::dEndFrame(void* ecx, void* edx)
{
    BMVR_PROF("hook EndFrame");
    (void)edx;
    if (hkEndFrame.fOriginal)
        hkEndFrame.fOriginal(ecx);
    RestoreEnginePostBloom();
    if (!m_VR)
        return;
    m_VR->m_PresentSpikeEndFrameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (m_Game && m_Game->GetMatQueueMode() != 0)
    {
        const uint32_t queued = m_VR->m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
        m_VR->m_SourceRenderQueueMarkerCompletedId.store(queued, std::memory_order_release);
    }
}

void __fastcall Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
    BMVR_PROF("hook PushRenderTargetAndViewport");
    NoteMatContext(ecx);
    const char* pushName = SafeTextureName(pTexture);
    const RtNameClass nameClass = ClassifyRtName(pushName);
    const int trInW = nViewW, trInH = nViewH;
    // Grow world G-buffer viewports before NotePushRt so the stack records
    // 3168, and so a `_rt_gb*` push while flashlight is still parent is not
    // skipped by AuxSceneRtBound().
    // Only during an actual eye pass. GameUI-dismiss used to arm this during
    // the 2560 pass-through warmup (2026-09-09: world flashed, then
    // exitCode=1, heartbeat in_rel_look, no stereo enter). FullFrame/GB are
    // already 1280x1408; rewriting PushRT onto them while CViewSetup is still
    // HWND 16:9 kills the next RenderView.
    if (m_VR && bmvr::OffscreenWorldMatchesEyes()
        && !m_VR->HudPaintActive()
        && m_VR->StereoWorldPassActive())
    {
        const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(m_VR->m_RenderHeight);
        const bool namedWorld = nameClass.world;
        const bool backbuffer = (pTexture == nullptr)
            || (pushName && (std::strcmp(pushName, "null") == 0));
        const bool smallVp = nViewW > 0 && nViewH > 0 && (nViewW < 640 || nViewH < 360);
        const bool reflectionVp = ReflectionRtSized(nViewW, nViewH) && nViewW + 32 < eyeW;
        const bool squareAux = (nViewW > 0 && nViewH > 0
            && std::abs(nViewW - nViewH) < 32 && nViewW + 32 < eyeW) || reflectionVp;
        if (reflectionVp)
        {
            static int s_reflVp;
            if (s_reflVp < 12)
            {
                Game::logMsg("PushRT %s viewport %d,%d %dx%d kept (reflection RT size, not the eye)",
                    pushName, nViewX, nViewY, nViewW, nViewH);
                ++s_reflVp;
            }
        }
        if (eyeW >= 640 && eyeH >= 360 && (namedWorld || backbuffer)
            && !smallVp && !squareAux
            && (nViewW != eyeW || nViewH != eyeH || nViewW <= 0 || nViewH <= 0))
        {
            static int s_offVp;
            if (s_offVp < 24)
            {
                Game::logMsg("PushRT %s viewport %dx%d -> eye %dx%d (offscreen native) parentAux=%d",
                    pushName, nViewW, nViewH, eyeW, eyeH, AuxSceneRtBound() ? 1 : 0);
                ++s_offVp;
            }
            const int inX = nViewX;
            const int inY = nViewY;
            const int inW = nViewW;
            const int inH = nViewH;
            nViewW = eyeW;
            nViewH = eyeH;
            if (m_VR->D3dRt0IsMcRing() && backbuffer)
            {
                int ox = 0, oy = 0;
                if (m_VR->McResolveRingEyeOrigin(inX, inY, inW, inH, ox, oy))
                {
                    nViewX = ox;
                    nViewY = oy;
                }
                else
                {
                    int cx = 0, cy = 0, cw = 0, ch = 0;
                    if (m_VR->McClipAmbiguousWindowVp(inX, inY, inW, inH, cx, cy, cw, ch))
                    {
                        nViewX = cx;
                        nViewY = cy;
                        nViewW = cw;
                        nViewH = ch;
                    }
                }
            }
            else
            {
                nViewX = 0;
                nViewY = 0;
            }
        }
    }
    NotePushRt(pushName, nViewW, nViewH, &nameClass);
    if (m_VR && m_VR->IsGameplayEligible())
    {
        static int s_pushNames;
        if (s_pushNames < 24)
        {
            Game::logMsg("PushRT name=%s %dx%d aux=%d",
                pushName, nViewW, nViewH, AuxSceneRtBound() ? 1 : 0);
            ++s_pushNames;
        }
        static int s_flPush;
        if (s_flPush < 12 && pushName
            && (m_VR->StereoEyeBlitActive() || m_VR->m_StereoEye != 0)
            && (std::strstr(pushName, "flashlight") || std::strstr(pushName, "Flashlight")
                || std::strstr(pushName, "FlashLight")))
        {
            Game::logMsg("Flashlight PushRT inside eye RV %s %dx%d stereoEye=%d",
                pushName, nViewW, nViewH, m_VR->m_StereoEye);
            ++s_flPush;
        }
    }
    if (m_VR && nameClass.hud)
        m_VR->NoteEngineHudRtPush(pushName, nViewW, nViewH);
    if (g_StereoRedirect)
    {
        bool redir = false;
        const char* before = SafeTextureName(pTexture);
        const int beforeW = nViewW;
        const int beforeH = nViewH;
        BindStereoPushToEye(pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH, redir);
        static int s_pushLog;
        if (s_pushLog < 20)
        {
            Game::logMsg("Stereo PushRT %s %dx%d redirect=%d -> %s %dx%d",
                before, beforeW, beforeH, redir ? 1 : 0,
                SafeTextureName(pTexture), nViewW, nViewH);
            ++s_pushLog;
        }
    }
    if (bmvr::g_PassTraceFrames > 0)
    {
        int texW = 0, texH = 0;
        QueryTextureSize(pTexture, texW, texH);
        Game::logMsg("trace: PushRT %s tex=%dx%d depth=%s vp %dx%d -> %d,%d %dx%d world=%d aux=%d eye=%d",
            SafeTextureName(pTexture), texW, texH, SafeTextureName(pDepthTexture), trInW, trInH,
            nViewX, nViewY, nViewW, nViewH, nameClass.world ? 1 : 0, nameClass.aux ? 1 : 0,
            m_VR ? m_VR->m_StereoEye : -1);
        if (std::strcmp(SafeTextureName(pTexture), "_rt_gblightdiff") == 0)
            g_BmvrD3dDrawTrace = 1;
    }
    if (hkPushRenderTargetAndViewport.fOriginal)
        hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    RestoreEnginePostBloom();
    if (m_VR && !m_VR->HudPaintActive() && !m_VR->ShouldHwndBlitStereo()
        && nViewW > 0 && nViewH > 0
        && static_cast<UINT>(nViewW) == m_VR->m_RenderWidth
        && static_cast<UINT>(nViewH) == m_VR->m_RenderHeight)
        m_VR->NoteCachedRt0Size(static_cast<UINT>(nViewW), static_cast<UINT>(nViewH));
}

void __fastcall Hooks::dPopRenderTargetAndViewport(void* ecx, void* edx)
{
    BMVR_PROF("hook PopRenderTargetAndViewport");
    if (m_VR && m_VR->EngineHudRtPushed())
        m_VR->BlitEngineHudRtToOverlay();
    if (hkPopRenderTargetAndViewport.fOriginal)
        hkPopRenderTargetAndViewport.fOriginal(ecx);
    RestoreEnginePostBloom();
    g_BmvrD3dDrawTrace = 0;
    if (bmvr::g_PassTraceFrames > 0)
        Game::logMsg("trace: PopRT");
    NotePopRt();
}

void __fastcall Hooks::dDrawScreenSpaceRectangle(void* ecx, void* edx, IMaterial* material,
    int destX, int destY, int width, int height,
    float srcX0, float srcY0, float srcX1, float srcY1,
    int srcWidth, int srcHeight, void* clientRenderable, int xDice, int yDice)
{
    // Identity model around every screen-space rectangle (ScreenModel, top of this file). ecx is
    // the render context the rectangle is drawn on (immediate, or the replaying one with MulticoreMode).
    static ScreenModel::SiteStats s_dssrModel{ "screen rect" };
    const char* matName = SafeMaterialName(material);
    // Test switch (harness): BMVR_TEST_SCREEN_MODEL_SHIFT=1 gives the underwater warp a leftover model
    // translation of (+1, +1), as a broken launch would, to check what it looks like and that the fix
    // repairs it.
    static const bool s_testShift = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("BMVR_TEST_SCREEN_MODEL_SHIFT", v, sizeof(v)) && v[0] == '1';
    }();
    const bool testShift = s_testShift && ecx && matName && std::strstr(matName, "water_warp");
    if (testShift)
    {
        using MatFn = void(__thiscall*)(void*, const float*);
        void** vt = ScreenModel::Vt(ecx);
        const float shift[16] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1 };
        reinterpret_cast<ScreenModel::IntFn>(vt[0x50 / 4])(ecx, ScreenModel::kModeModel);
        reinterpret_cast<ScreenModel::VoidFn>(vt[0x54 / 4])(ecx);
        reinterpret_cast<MatFn>(vt[0x60 / 4])(ecx, shift);
    }
    const bool wrap = ScreenModel::Check(s_dssrModel, ecx, matName);
    if (wrap)
        ScreenModel::Begin(ecx);
    dDrawScreenSpaceRectangleBody(ecx, edx, material, destX, destY, width, height,
        srcX0, srcY0, srcX1, srcY1, srcWidth, srcHeight, clientRenderable, xDice, yDice);
    if (wrap)
        ScreenModel::End(ecx);
    if (testShift)
    {
        void** vt = ScreenModel::Vt(ecx);
        reinterpret_cast<ScreenModel::IntFn>(vt[0x50 / 4])(ecx, ScreenModel::kModeModel);
        reinterpret_cast<ScreenModel::VoidFn>(vt[0x58 / 4])(ecx);
        reinterpret_cast<ScreenModel::IntFn>(vt[0x50 / 4])(ecx, ScreenModel::kModeProjection);
    }
}

void __fastcall Hooks::dDrawScreenSpaceRectangleBody(void* ecx, void* edx, IMaterial* material,
    int destX, int destY, int width, int height,
    float srcX0, float srcY0, float srcX1, float srcY1,
    int srcWidth, int srcHeight, void* clientRenderable, int xDice, int yDice)
{
    BMVR_PROF("hook DrawScreenSpaceRectangle");
    (void)edx;
    const int origX = destX;
    const int origY = destY;
    const int origW = width;
    const int origH = height;
    const int origSrcW = srcWidth;
    const int origSrcH = srcHeight;
    const int eyeW = m_VR ? static_cast<int>(m_VR->m_RenderWidth) : 0;
    const int eyeH = m_VR ? static_cast<int>(m_VR->m_RenderHeight) : 0;
    const char* matName = SafeMaterialName(material);
    if (m_VR && m_VR->RpgLaserActive())
    {
        static int s_laserDssr;
        if (s_laserDssr < 16)
        {
            const bool glowish = std::strstr(matName, "glow") || std::strstr(matName, "laser")
                || std::strstr(matName, "dot") || std::strstr(matName, "flare");
            const bool smallQuad = width > 0 && height > 0 && width < 256 && height < 256;
            if (glowish || smallQuad)
            {
                Game::logMsg("DSSR laser-on %s dest=%d,%d %dx%d", matName, destX, destY, width, height);
                ++s_laserDssr;
            }
        }
    }
    const bool windowDest = LooksLikeWindowExtent(width, height)
        && ((destX <= 16 && destY <= 16)
            || (m_VR && m_VR->McOriginIsRingBand(destX, destY)));
    const bool windowUv = LooksLikeWindowUv(srcX0, srcY0, srcX1, srcY1);
    const bool potSrc = SrcLooksLikePowerOfTwoFb(srcWidth, srcHeight, eyeW, eyeH);
    const bool waterWarp = matName && std::strstr(matName, "water_warp") != nullptr;
    // No motion blur in VR. The beta re-enables mat_motion_blur_enabled after
    // our cfg (2026-10-15) and ran a full-eye blur pass per eye. The pass
    // draws over the scene still in the target, so skipping it is safe.
    if (bmvr::g_VrSkipMotionBlur && matName && std::strstr(matName, "motion_blur") != nullptr
        && m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible())
    {
        static int s_skipBlur;
        if (s_skipBlur < 2)
        {
            Game::logMsg("DSSR skip motion blur %s dest=%dx%d", matName, width, height);
            ++s_skipBlur;
        }
        return;
    }
    // No env_screeneffect overlays in VR (stun / intro blur, e.g. the c2a3c
    // ambush knockout). Each draws a 1024x512 copy of an earlier eye image
    // full-screen over the eye before saving the current one, so the eyes
    // get a stale, flat second view on top of the stereo scene.
    if (bmvr::g_VrSkipScreenEffects && matName && m_VR && m_VR->m_IsVREnabled
        && m_VR->IsGameplayEligible()
        && (std::strcmp(matName, "__ep1introeffect") == 0 || std::strcmp(matName, "__stuneffect") == 0
            || std::strcmp(matName, "__ep2stuneffect") == 0))
    {
        static int s_skipScreenFx;
        if (s_skipScreenFx < 4)
        {
            Game::logMsg("DSSR skip screen effect %s dest=%dx%d src=%dx%d", matName, width, height, srcWidth, srcHeight);
            ++s_skipScreenFx;
        }
        return;
    }
    const bool stereoEyePost = Hooks::m_VR && Hooks::m_VR->StereoWorldPassActive()
        && (bmvr::OffscreenWorldMatchesEyes() || Hooks::m_VR->ShouldHwndBlitStereo());
    // Gameplay only (non-background LevelInit and IsInGame, same gate as the
    // menu engine_post uv fix below). Load screens and menu maps keep the old
    // skip: the src-rect remap zoomed the load screen into its top-left
    // (user, 2026-10-30).
    const bool enginePostMat = stereoEyePost && bmvr::g_StereoEnginePost
        && EnginePostMaterial(matName);
    const bool eyeEnginePost = enginePostMat && Hooks::m_VR->IsGameplayEligible() && EngineInGame();
    if (enginePostMat && !eyeEnginePost)
    {
        static int s_gatedPost;
        if (s_gatedPost < 4)
        {
            Game::logMsg("DSSR engine_post not gameplay, skipped %s dest=%d,%d %dx%d src=%dx%d uv=(%.1f,%.1f)-(%.1f,%.1f) eligible=%d inGame=%d",
                matName, destX, destY, width, height, srcWidth, srcHeight, srcX0, srcY0, srcX1, srcY1,
                Hooks::m_VR->IsGameplayEligible() ? 1 : 0, EngineInGame() ? 1 : 0);
            ++s_gatedPost;
        }
    }
    // dev/bloomadd (drawn instead of engine_post when mat_colorcorrection is
    // 0) adds _rt_SmallFB0, not BM's bloom result _rt_SmallFB3 (what
    // engine_post_nxtgen reads). On the eyes SmallFB0 only holds BM's HUD glow
    // at window scale, so it smears blurred HUD digits over both eyes; skip it
    // also with EyeBloom on (regressed when b2fad11 made EyeBloom default,
    // 2026-11-16).
    const bool bloomAdd = matName && std::strstr(matName, "bloomadd") != nullptr;
    if ((!bmvr::g_EyeBloom || bloomAdd) && StereoBloomMaterial(matName) && stereoEyePost && !eyeEnginePost)
    {
        static int s_skipBloom;
        if (s_skipBloom < 12)
        {
            Game::logMsg("DSSR skip bloom %s dest=%dx%d src=%dx%d stereo=1",
                matName, width, height, srcWidth, srcHeight);
            ++s_skipBloom;
        }
        return;
    }
    int expanded = 0;
    // Underwater overlay is screenspace water_warp from _rt_poweroftwofb.
    // GetScreenSize eye-lie can leave dest at 1024 (or HWND 16:9) on the
    // square eye — a magnified inset in one corner that pops fullscreen
    // when dest happens to match. Planar water uses the Water shader, not
    // this DSSR path.
    if (waterWarp && OffscreenEyePassActive() && eyeW >= 640 && eyeH >= 360
        && width >= 256 && height >= 256
        && (width + 64 < eyeW || height + 64 < eyeH || windowDest))
    {
        int ox = 0;
        int oy = 0;
        bool rewrite = true;
        if (m_VR->D3dRt0IsMcRing())
        {
            if (!m_VR->McResolveRingEyeOrigin(origX, origY, origW, origH, ox, oy))
            {
                int cx = 0, cy = 0, cw = 0, ch = 0;
                if (m_VR->McClipAmbiguousWindowVp(origX, origY, origW, origH, cx, cy, cw, ch))
                {
                    destX = cx;
                    destY = cy;
                    width = cw;
                    height = ch;
                    rewrite = false;
                    expanded = 1;
                }
                else
                    rewrite = false;
            }
        }
        if (rewrite)
        {
            destX = ox;
            destY = oy;
            width = eyeW;
            height = eyeH;
            expanded = 1;
        }
        static int s_warp;
        if (s_warp < 8)
        {
            Game::logMsg("DSSR water_warp dest %d,%d %dx%d -> %d,%d %dx%d",
                origX, origY, origW, origH, destX, destY, width, height);
            ++s_warp;
        }
    }
    if (expanded == 0 && OffscreenEyePassActive() && (windowDest || windowUv)
        && !HudScreenspaceMaterial(matName) && !potSrc && eyeW >= 640)
    {
        const bool ringDest = m_VR->D3dRt0IsMcRing() && McViewportDestIsRing();
        int ox = 0;
        int oy = 0;
        bool rewrite = true;
        if (ringDest)
        {
            if (!m_VR->McResolveRingEyeOrigin(origX, origY, origW, origH, ox, oy))
            {
                int cx = 0, cy = 0, cw = 0, ch = 0;
                if (m_VR->McClipAmbiguousWindowVp(origX, origY, origW, origH, cx, cy, cw, ch))
                {
                    destX = cx;
                    destY = cy;
                    width = cw;
                    height = ch;
                    rewrite = false;
                }
                else
                    rewrite = false;
            }
        }
        if (rewrite)
        {
            destX = ox;
            destY = oy;
            width = eyeW;
            height = eyeH;
            expanded = 1;
            if (windowUv && origSrcW >= eyeW - 32 && origSrcH >= eyeH - 32)
            {
                srcX0 = 0.f;
                srcY0 = 0.f;
                srcX1 = static_cast<float>(origSrcW) - 1.f;
                srcY1 = static_cast<float>(origSrcH) - 1.f;
            }
            else if (LooksLikeWindowExtent(origSrcW, origSrcH) && origSrcW > 0 && origSrcH > 0)
            {
                const float sx = static_cast<float>(eyeW) / static_cast<float>(origSrcW);
                const float sy = static_cast<float>(eyeH) / static_cast<float>(origSrcH);
                srcX0 *= sx;
                srcY0 *= sy;
                srcX1 *= sx;
                srcY1 *= sy;
                srcWidth = eyeW;
                srcHeight = eyeH;
            }
        }
    }
    // Menu background maps / loading (not gameplay): WorldRenderAtEyeSize
    // grew _rt_FullFrameFB to the eye size, but these views still render at
    // the window size into its top-left. BM's final composite then showed
    // the background squeezed into the top-left with stale streaks below and
    // right (window and VR menu panel, 2026-10-14).
    // dev/engine_post_nxtgen samples the grown
    // _rt_FullFrameFB through uvs the engine stretched by FullFrame/window
    // (319x159 -> 379x387 on its 320x160 source), so everything past the
    // drawn part read clamped edge texels. Give it the uvs it gets without
    // grown RTs.
    if (m_VR && eyeW >= 640 && eyeH >= 360
        && !(m_VR->IsGameplayEligible() && EngineInGame())
        && !StereoEyeWorldActive() && !OffscreenEyePassActive()
        && std::strstr(matName, "engine_post") != nullptr
        && origSrcW > 0 && origSrcH > 0
        && (srcX1 > static_cast<float>(origSrcW) || srcY1 > static_cast<float>(origSrcH)))
    {
        static int s_postLog;
        if (s_postLog < 4)
        {
            Game::logMsg("DSSR menu %s: uv (%.1f,%.1f)-(%.1f,%.1f) -> full %dx%d",
                matName, srcX0, srcY0, srcX1, srcY1, origSrcW, origSrcH);
            ++s_postLog;
        }
        srcX0 = 0.f;
        srcY0 = 0.f;
        srcX1 = static_cast<float>(origSrcW) - 1.f;
        srcY1 = static_cast<float>(origSrcH) - 1.f;
    }
    static int s_dssrLog;
    static int s_stereoDssrLog;
    static char s_seen[24][80];
    static int s_seenN;
    bool seen = false;
    // Only matters while the seen-table still has room (first 24 materials).
    for (int i = 0; s_seenN < 24 && i < s_seenN; ++i)
    {
        if (std::strcmp(s_seen[i], matName) == 0)
        {
            seen = true;
            break;
        }
    }
    const bool logDssr = (!seen && s_seenN < 24) || s_dssrLog < 12
        || (StereoEyeWorldActive() && s_stereoDssrLog < 16)
        || (OffscreenEyePassActive() && (windowDest || windowUv || expanded) && s_stereoDssrLog < 16);
    if (logDssr)
    {
        // Diagnostic only: GetRenderTarget + texture size per DSSR call was
        // paid for every post-process quad long after the log caps filled.
        int rtW = 0;
        int rtH = 0;
        QueryContextColorRtSize(ecx, rtW, rtH, nullptr);
        Game::logMsg("DSSR %s dest=%d,%d %dx%d src=%dx%d uv=(%.1f,%.1f)-(%.1f,%.1f) expand=%d rt=%dx%d rt0eye=%d",
            matName, origX, origY, origW, origH, origSrcW, origSrcH,
            srcX0, srcY0, srcX1, srcY1, expanded, rtW, rtH,
            (m_VR && m_VR->CachedRt0MatchesEyes()) ? 1 : 0);
        ++s_dssrLog;
        if (StereoEyeWorldActive()
            || (OffscreenEyePassActive() && (windowDest || windowUv || expanded)))
            ++s_stereoDssrLog;
        if (!seen && s_seenN < 24)
        {
            std::strncpy(s_seen[s_seenN], matName, 79);
            s_seen[s_seenN][79] = 0;
            ++s_seenN;
        }
    }
    if (bmvr::g_PassTraceFrames > 0)
    {
        int rtW = 0, rtH = 0;
        const char* rtName = "";
        QueryContextColorRtSize(ecx, rtW, rtH, &rtName);
        Game::logMsg("trace: DSSR %s dest %d,%d %dx%d -> %d,%d %dx%d src=%dx%d uv=(%.1f,%.1f)-(%.1f,%.1f) rt=%s %dx%d",
            matName, origX, origY, origW, origH, destX, destY, width, height, srcWidth, srcHeight,
            srcX0, srcY0, srcX1, srcY1, rtName, rtW, rtH);
    }
    // engine_post on an eye. With the bloom chain sized from the eye the src
    // rect already matches and the composite is 1:1 (measured 2026-11-05); the
    // remap below is for the small-eye fallback, where the chain stays window
    // sized. It stretched the eye up to 10 px and softened it by about 20 %.
    // Its src texture is the window/4 small FB
    // (320x160 for 1280x640), but the engine sizes the src rect from the eye
    // (287x295 for 1152x1184), so v ran to 1.84 and the composite, which also
    // samples _rt_FullFrameFB through those texcoords, landed in the top 54 %
    // (headless bm_c1a0a, MulticoreMode, 2026-10-30). Map the rect onto the
    // whole src texture whenever dest covers the eye.
    // If the context viewport is not the eye on a 1x eye RT, pin it too.
    int vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    bool vpPinned = false;
    if (eyeEnginePost)
    {
        const char* varName = "none";
        float bloomOrig = 0.f;
        bool bloomSet = false;
        if (bmvr::g_EyeBloom)
        {
            RestoreEnginePostBloom();
            varName = "kept (EyeBloom)";
        }
        else
            bloomSet = ZeroEnginePostBloomForEye(material, matName, varName, bloomOrig);
        CallOriginalGetViewport(ecx, vpX, vpY, vpW, vpH);
        int rtW = 0, rtH = 0;
        QueryContextColorRtSize(ecx, rtW, rtH, nullptr);
        const bool eyeRt = rtW == eyeW && rtH == eyeH;
        const int inDestX = destX, inDestY = destY, inDestW = width, inDestH = height;
        const float inU0 = srcX0, inV0 = srcY0, inU1 = srcX1, inV1 = srcY1;
        if (eyeRt)
        {
            if ((vpX != 0 || vpY != 0 || vpW != eyeW || vpH != eyeH) && hkViewport.fOriginal)
            {
                hkViewport.fOriginal(ecx, 0, 0, eyeW, eyeH);
                vpPinned = true;
            }
            destX = 0;
            destY = 0;
            width = eyeW;
            height = eyeH;
        }
        // Keep the engine's half-texel origin (v0 = -0.5); only the extent
        // changes.
        const bool destIsEye = destX == 0 && destY == 0 && width == eyeW && height == eyeH;
        const float extU = srcX1 - srcX0 + 1.f;
        const float extV = srcY1 - srcY0 + 1.f;
        if (destIsEye && srcWidth > 0 && srcHeight > 0
            && (std::fabs(extU - static_cast<float>(srcWidth)) > 2.f
                || std::fabs(extV - static_cast<float>(srcHeight)) > 2.f))
        {
            srcX1 = srcX0 + static_cast<float>(srcWidth) - 1.f;
            srcY1 = srcY0 + static_cast<float>(srcHeight) - 1.f;
        }
        static int s_eyePostLog;
        if (s_eyePostLog < 8)
        {
            Game::logMsg("DSSR engine_post on eye %s bloomVar=%s set=%d orig=%.3f varSlot=%d rt=%dx%d vp=%d,%d %dx%d pinned=%d "
                "dest %d,%d %dx%d -> %d,%d %dx%d src=%dx%d uv=(%.1f,%.1f)-(%.1f,%.1f) -> (%.1f,%.1f)-(%.1f,%.1f)",
                matName, varName, bloomSet ? 1 : 0, bloomOrig, g_MatVarNameSlot, rtW, rtH, vpX, vpY, vpW, vpH, vpPinned ? 1 : 0,
                inDestX, inDestY, inDestW, inDestH, destX, destY, width, height, srcWidth, srcHeight,
                inU0, inV0, inU1, inV1, srcX0, srcY0, srcX1, srcY1);
            ++s_eyePostLog;
        }
    }
    // PostCopySwap: a quad over the whole eye may drop the eye's pending
    // copy (DXVK checks that the draw's state really writes every pixel).
    const bool armOverwrite = bmvr::g_PostCopySwap && eyeW >= 640
        && destX <= 0 && destY <= 0 && destX + width >= eyeW && destY + height >= eyeH;
    if (armOverwrite)
        bmvr::g_FullOverwriteArmThread = GetCurrentThreadId();
    if (hkDrawScreenSpaceRectangle.fOriginal)
        hkDrawScreenSpaceRectangle.fOriginal(ecx, material, destX, destY, width, height,
            srcX0, srcY0, srcX1, srcY1, srcWidth, srcHeight, clientRenderable, xDice, yDice);
    if (armOverwrite)
        bmvr::g_FullOverwriteArmThread = 0;
    if (vpPinned && hkViewport.fOriginal)
        hkViewport.fOriginal(ecx, vpX, vpY, vpW, vpH);
}

void __fastcall Hooks::dCopyRenderTargetToTextureEx(void* ecx, void* edx, ITexture* texture,
    int renderTargetId, SourceRect_t* srcRect, SourceRect_t* dstRect)
{
    BMVR_PROF("hook CopyRenderTargetToTextureEx");
    (void)edx;
    SourceRect_t expanded{};
    SourceRect_t expandedDst{};
    int destW = 0;
    int destH = 0;
    QueryTextureSize(texture, destW, destH);
    int srcW = 0;
    int srcH = 0;
    const char* srcName = "";
    QueryContextColorRtSize(ecx, srcW, srcH, &srcName);
    const int eyeW = m_VR ? static_cast<int>(m_VR->m_RenderWidth) : 0;
    const int eyeH = m_VR ? static_cast<int>(m_VR->m_RenderHeight) : 0;
    const bool destWindow = destW >= 640 && destH >= 360
        && MatchesWindowClientSize(destW, destH);
    const bool windowSrc = srcRect
        && srcRect->x <= 16 && srcRect->y <= 16
        && MatchesWindowClientSize(srcRect->width, srcRect->height);
    bool srcEye = eyeW >= 640 && srcW == eyeW && srcH == eyeH;
    if (!srcEye && m_VR && m_VR->CachedRt0MatchesEyes())
        srcEye = true;
    if (!srcEye && OffscreenEyePassActive() && m_VR->D3dRt0IsEyeSized())
        srcEye = true;
    const bool destEye = eyeW >= 640 && destW == eyeW && destH == eyeH;
    // GetRT is often null. A window srcRect into an eye-sized FullFrame is
    // the 16:9 stamp (log: destTex=2656x2592 srcRect=2560x1440).
    if (!srcEye && destEye && OffscreenEyePassActive())
        srcEye = true;
    int grew = 0;
    const bool refractDest = DestLooksLikeRefractCopy(SafeTextureName(texture))
        || (destW >= 256 && destH >= 256 && destW == destH && destW <= 2048
            && eyeW >= 640 && destW + 32 < eyeW)
        // Grown by ReflectionRtScale, so no longer square. Under water the engine copies the eye
        // into _rt_WaterRefraction for the surface seen from below: clipping that copy to the
        // texture size kept only the eye's top-left corner.
        || (ReflectionRtSized(destW, destH) && destW + 32 < eyeW);
    // Do not grow a HWND srcRect to eye size when the dest is the 1024
    // underwater / refract RT. That copy must downsample the whole view.
    if (srcRect && windowSrc && !destWindow && srcEye && OffscreenEyePassActive()
        && !refractDest)
    {
        int ox = 0;
        int oy = 0;
        bool rewrite = true;
        if (m_VR && m_VR->McPipelineEnabled() && m_VR->D3dRt0IsMcRing())
        {
            if (!m_VR->McResolveRingEyeOrigin(srcRect->x, srcRect->y,
                srcRect->width, srcRect->height, ox, oy))
            {
                int cx = 0, cy = 0, cw = 0, ch = 0;
                if (m_VR->McClipAmbiguousWindowVp(srcRect->x, srcRect->y,
                    srcRect->width, srcRect->height, cx, cy, cw, ch))
                {
                    expanded.x = cx;
                    expanded.y = cy;
                    expanded.width = cw;
                    expanded.height = ch;
                    srcRect = &expanded;
                    rewrite = false;
                }
                else
                    rewrite = false;
            }
        }
        if (rewrite)
        {
            expanded.x = ox;
            expanded.y = oy;
            expanded.width = eyeW;
            expanded.height = eyeH;
            srcRect = &expanded;
            grew = 1;
        }
    }
    if (dstRect && destEye && OffscreenEyePassActive()
        && dstRect->x <= 16 && dstRect->y <= 16
        && MatchesWindowClientSize(dstRect->width, dstRect->height))
    {
        int ox = 0;
        int oy = 0;
        bool rewrite = true;
        if (m_VR && m_VR->D3dRt0IsMcRing() && McViewportDestIsRing())
        {
            if (!m_VR->McResolveRingEyeOrigin(dstRect->x, dstRect->y,
                dstRect->width, dstRect->height, ox, oy))
            {
                int cx = 0, cy = 0, cw = 0, ch = 0;
                if (m_VR->McClipAmbiguousWindowVp(dstRect->x, dstRect->y,
                    dstRect->width, dstRect->height, cx, cy, cw, ch))
                {
                    expandedDst.x = cx;
                    expandedDst.y = cy;
                    expandedDst.width = cw;
                    expandedDst.height = ch;
                    dstRect = &expandedDst;
                    rewrite = false;
                }
                else
                    rewrite = false;
            }
        }
        if (rewrite)
        {
            expandedDst.x = ox;
            expandedDst.y = oy;
            expandedDst.width = eyeW;
            expandedDst.height = eyeH;
            dstRect = &expandedDst;
            grew = 1;
        }
    }
    static int s_copyLog;
    static int s_stereoCopyLog;
    const bool logCopy = s_copyLog < 16
        || (StereoEyeWorldActive() && s_stereoCopyLog < 12);
    if (logCopy)
    {
        Game::logMsg("CopyRTEx %s src=%s srcTex=%s %dx%d destTex=%dx%d grew=%d stereo=%d refract=%d",
            SafeTextureName(texture),
            srcRect ? "rect" : "null",
            srcName, srcW, srcH, destW, destH, grew,
            StereoEyeWorldActive() ? 1 : 0,
            DestLooksLikeRefractCopy(SafeTextureName(texture)) ? 1 : 0);
        if (srcRect)
        {
            Game::logMsg("CopyRTEx srcRect=%d,%d %dx%d",
                srcRect->x, srcRect->y, srcRect->width, srcRect->height);
        }
        ++s_copyLog;
        if (StereoEyeWorldActive())
            ++s_stereoCopyLog;
    }
    // Clip src to the source RT and dst to the dest texture. Clipping src to
    // dest turned a legal 1280→1024 stretch (_rt_poweroftwofb refract) into a
    // crop, and a 2560 srcRect into a 1280 FullFrame is illegal only when the
    // *source* is smaller than srcRect (2026-09-09).
    auto clipToBounds = [&](SourceRect_t*& rect, SourceRect_t& storage, int maxW, int maxH, const char* tag) {
        if (!rect || maxW < 1 || maxH < 1)
            return;
        if (rect->x >= 0 && rect->y >= 0 && rect->width > 0 && rect->height > 0
            && rect->x + rect->width <= maxW && rect->y + rect->height <= maxH)
            return;
        storage = *rect;
        if (storage.x < 0)
            storage.x = 0;
        if (storage.y < 0)
            storage.y = 0;
        if (storage.x >= maxW || storage.y >= maxH)
        {
            storage.x = 0;
            storage.y = 0;
            storage.width = maxW;
            storage.height = maxH;
        }
        else
        {
            if (storage.width > maxW - storage.x)
                storage.width = maxW - storage.x;
            if (storage.height > maxH - storage.y)
                storage.height = maxH - storage.y;
        }
        static int s_clip;
        if (s_clip < 8)
        {
            Game::logMsg("CopyRTEx clip %s %d,%d %dx%d -> %dx%d",
                tag, rect->x, rect->y, rect->width, rect->height, maxW, maxH);
            ++s_clip;
        }
        rect = &storage;
    };
    if (srcW >= 1 && srcH >= 1)
        clipToBounds(srcRect, expanded, srcW, srcH, "src");
    else if (refractDest && srcRect
        && (srcRect->width > destW || srcRect->height > destH))
    {
        // Source RT size unknown. Clipping srcRect to dest cropped the
        // underwater copy to the top-left 1024 of a 2544 eye after
        // GetScreenSize returned eye size (magnified corner overlay that
        // then water_warp stretched fullscreen — same look as scope zoom).
        static int s_refractStretch;
        if (s_refractStretch < 8)
        {
            Game::logMsg("CopyRTEx refract stretch drop srcRect %d,%d %dx%d into dest %dx%d",
                srcRect->x, srcRect->y, srcRect->width, srcRect->height, destW, destH);
            ++s_refractStretch;
        }
        srcRect = nullptr;
    }
    else
        clipToBounds(srcRect, expanded, destW, destH, "src-unk");
    clipToBounds(dstRect, expandedDst, destW, destH, "dst");
    if (bmvr::g_PassTraceFrames > 0)
        Game::logMsg("trace: CopyRT -> %s %dx%d from %s %dx%d src=%d,%d %dx%d dst=%d,%d %dx%d grew=%d",
            SafeTextureName(texture), destW, destH, srcName, srcW, srcH,
            srcRect ? srcRect->x : -1, srcRect ? srcRect->y : -1, srcRect ? srcRect->width : -1, srcRect ? srcRect->height : -1,
            dstRect ? dstRect->x : -1, dstRect ? dstRect->y : -1, dstRect ? dstRect->width : -1, dstRect ? dstRect->height : -1, grew);
    // Limit refractions per eye (VR panel, default on, key RefractOncePerEye): Source refreshes
    // _rt_PowerOfTwoFB for every refractive object it draws (up to ~50 per eye
    // per frame behind the train windows). With this on, only the first copy of
    // each eye's frame runs; later glass refracts that copy (glass behind glass
    // is then missing from the refraction). Any other writer resets it, and so
    // does the end of the main view's translucent pass (screen overlays).
    // Off while the player's eyes are under water: the underwater screen warp
    // copies after the gun is drawn, and skipping that copy hid the gun and
    // hand (2026-11-13). (The viewmodel pass is not visible on the drawing
    // thread with MulticoreMode, so the water level decides.)
    // The water level is only looked up when a copy is about to be skipped.
    if (bmvr::g_RefractOncePerEye && m_VR)
    {
        const char* rn = SafeTextureName(texture);
        if (rn && _stricmp(rn, "_rt_PowerOfTwoFB") == 0)
        {
            IDirect3DSurface9* eyeNow = m_VR->ThreadEyeSurface();
            if (eyeNow && srcEye)
            {
                // RefractCopiesPerEye (dev row, 1..20, default 6): this many copies per eye run.
                const int maxCopies = (std::max)(1, static_cast<int>(bmvr::g_RefractCopiesPerEye + 0.5f));
                if (m_VR->m_RefractEyeDone == eyeNow)
                {
                    if (m_VR->m_RefractEyeCopies >= maxCopies && !(m_Game && m_Game->LocalPlayerWaterLevel() == 3))
                    {
                        ++m_VR->m_RefractCopiesSkipped;
                        return;
                    }
                    ++m_VR->m_RefractEyeCopies;
                }
                else
                {
                    m_VR->m_RefractEyeDone = eyeNow;
                    m_VR->m_RefractEyeCopies = 1;
                }
            }
            else
                m_VR->m_RefractEyeDone = nullptr;
        }
    }
    // PostCopySwap: the post chain's full eye -> _rt_FullFrameFB copies swap
    // the images' storage in DXVK instead of copying (see d3d9_device.h).
    bool armSwap = false;
    if (bmvr::g_PostCopySwap && srcEye && m_VR && m_VR->m_IsVREnabled)
    {
        const char* dn = SafeTextureName(texture);
        armSwap = dn && _stricmp(dn, "_rt_FullFrameFB") == 0;
    }
    if (armSwap)
        bmvr::g_PostSwapArmThread = GetCurrentThreadId();
    if (hkCopyRenderTargetToTextureEx.fOriginal)
        hkCopyRenderTargetToTextureEx.fOriginal(ecx, texture, renderTargetId, srcRect, dstRect);
    if (armSwap)
        bmvr::g_PostSwapArmThread = 0;
    if (bmvr::g_PerfLog)
    {
        // Engine eye copies per second, logged when > 1500/s (refraction storms).
        static DWORD s_t; static int s_n; static char s_last[8][64]; static int s_cnt[8];
        const char* nm = SafeTextureName(texture);
        int k = 0;
        for (; k < 8 && s_last[k][0]; ++k)
            if (nm && std::strncmp(s_last[k], nm, 63) == 0) break;
        if (k < 8) { if (!s_last[k][0] && nm) strncpy_s(s_last[k], nm, 63); ++s_cnt[k]; }
        ++s_n;
        const DWORD now = GetTickCount();
        if (now - s_t >= 1000)
        {
            if (s_n > 1500)
                Game::logMsg("Engine copies/s: %d: %s=%d %s=%d %s=%d %s=%d, refraction copies skipped %u", s_n, s_last[0], s_cnt[0], s_last[1], s_cnt[1],
                    s_last[2], s_cnt[2], s_last[3], s_cnt[3], m_VR ? m_VR->m_RefractCopiesSkipped : 0u);
            if (m_VR)
                m_VR->m_RefractCopiesSkipped = 0;
            s_t = now; s_n = 0;
            for (int i = 0; i < 8; ++i) { s_last[i][0] = 0; s_cnt[i] = 0; }
        }
    }
    // Eye MSAA: the first copy into _rt_FullFrameFB starts the post chain
    // (bloom, engine_post, bms_postprocess, FXAA, HUD, hands). Run all of it on
    // the eye's 1x resolve instead of full-screen passes on 4 samples.
    if (m_VR && m_VR->UseVrMsaa() && bmvr::g_MsaaPostSwitch)
    {
        const char* dn = SafeTextureName(texture);
        // Material system names come back lower case.
        if (dn && _stricmp(dn, "_rt_FullFrameFB") == 0)
            m_VR->RetireMsaaEyeForPost();
    }
}

void __fastcall Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
    BMVR_PROF("hook VGui_Paint");
    (void)edx;
    if (g_VguiOverlayReentry)
    {
        if (hkVgui_Paint.fOriginal)
            hkVgui_Paint.fOriginal(ecx, mode);
        return;
    }

    if (m_VR)
        m_VR->NoteEngineVGui(ecx);

    const bool inGame = EngineInGame();
    const bool eligible = m_VR && m_VR->IsGameplayEligible();

    // Never steal the engine destination. Last steal emptied _rt_gui.
    // Menu/background stay original only. In-game: original (desktop) plus
    // one extra paint into bmvrHUD (headset overlay).
    static int s_paintLog;
    static int s_ingamePaintLog;
    if (s_paintLog < 8)
    {
        Game::logMsg("VGui_Paint original dest inGame=%d eligible=%d mode=0x%X overlay=%d",
            inGame ? 1 : 0, eligible ? 1 : 0, mode,
            (m_VR && m_VR->HudOverlayReady()) ? 1 : 0);
        ++s_paintLog;
    }
    else if (eligible && s_ingamePaintLog < 4)
    {
        Game::logMsg("VGui_Paint in-game original mode=0x%X overlay=%d",
            mode, (m_VR && m_VR->HudOverlayReady()) ? 1 : 0);
        ++s_ingamePaintLog;
    }
    if (hkVgui_Paint.fOriginal)
    {
        if (m_VR)
            m_VR->SetVguiPaintActive(true);
        hkVgui_Paint.fOriginal(ecx, mode);
        if (m_VR)
            m_VR->SetVguiPaintActive(false);
    }
    if (inGame && eligible && m_VR)
    {
        m_VR->UpdateTitleHud();
        m_VR->UpdateScreenEffectNoDraw();
        m_VR->UpdateEnvmapScale();
    }
    if (inGame && eligible && m_VR && m_VR->WantHudLayer()
        && !m_VR->Want2dMenuPanel())
        PaintVguiToOverlay(ecx, mode);
}

void __fastcall Hooks::dTraceRay(void* ecx, void* edx, const Ray_t& ray, unsigned int fMask, CTraceFilter* filter, CGameTrace* pTrace)
{
    BMVR_PROF("hook TraceRay");
    (void)edx;
    // 47777b5 / L4D2VR melee: do not rewrite TraceRay. Hit geometry is the
    // client Rodrigues fan from viewmodel abs origin in UpdateCrowbarMelee.
    // Invented pitch clamps + origin rewrite were the ceiling-hit path.
    if (hkTraceRay.fOriginal)
        hkTraceRay.fOriginal(ecx, ray, fMask, filter, pTrace);
}

void Hooks::EnsureServerFlashlightHook()
{
    if (hkImpulseCommands.pTarget)
        return;
    HMODULE server = GetModuleHandleA("server.dll");
    if (!server)
        return;
    auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kCBasePlayer_ImpulseCommands;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
    {
        static int s_nq;
        if (s_nq < 2)
        {
            Game::logMsg("ImpulseCommands RVA 0x%X not executable", Offsets::kCBasePlayer_ImpulseCommands);
            ++s_nq;
        }
        return;
    }
    bool hasImpulseField = false;
    BMVR_TRY
    {
        for (int i = 0; i < 120; ++i)
        {
            if (p[i] == 0x44 && p[i + 1] == 0x0E && p[i + 2] == 0x00 && p[i + 3] == 0x00)
            {
                hasImpulseField = true;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        hasImpulseField = false;
    }
    if (!hasImpulseField)
    {
        Game::logMsg("ImpulseCommands skipped (no m_nImpulse +0xE44 near 0x%X)",
            Offsets::kCBasePlayer_ImpulseCommands);
        Game::noteMissingOffset("ImpulseCommands");
        hkImpulseCommands.pTarget = p;
        return;
    }
    if (hkImpulseCommands.createHook(p, &dImpulseCommands) != 0
        || hkImpulseCommands.enableHook() != 0)
    {
        Game::logMsg("ImpulseCommands hook failed rva=0x%X", Offsets::kCBasePlayer_ImpulseCommands);
        return;
    }
    Game::logMsg("Hook enabled: ImpulseCommands rva=0x%X (passthrough; CreateMove impulse 100)",
        Offsets::kCBasePlayer_ImpulseCommands);
}

void __fastcall Hooks::dImpulseCommands(void* ecx, void* edx)
{
    (void)edx;
    // Working flashlight (47777b5) was CreateMove cmd->impulse=100 only.
    // Intercepting impulse 100 here cleared it and called EF_DIMLIGHT
    // virtuals that suit/gamerules may no-op — vanilla never saw the impulse.
    // Pass through; do not add flashlight cvars.
    // Diagnostic: the server saw the flashlight impulse (pairs with "CreateMove applied impulse 100";
    // a press that queued and applied but never shows here was lost between client and server).
    int impulse = 0;
    BMVR_TRY { impulse = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(ecx) + Offsets::kCBasePlayer_m_nImpulse); }
    __except (EXCEPTION_EXECUTE_HANDLER) { impulse = 0; }
    if (impulse == 100)
    {
        static int s_srvLog;
        if (s_srvLog < 200)
        {
            Game::logMsg("Server ImpulseCommands impulse 100 (flashlight toggle)");
            ++s_srvLog;
        }
    }
    if (hkImpulseCommands.fOriginal)
        hkImpulseCommands.fOriginal(ecx);
}

void Hooks::EnsureClientFlashlightHook()
{
    EnsureBmFlashlightHook();
    if (hkUpdateFlashlightState.pTarget)
        return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client)
        return;
    auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kUpdateFlashlightState;
    MEMORY_BASIC_INFORMATION mbi{};
    static const unsigned char kFlashPrologue[] = { 0x55, 0x8B, 0xEC, 0x51, 0x53, 0x8B, 0xD9 };
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        || memcmp(p, kFlashPrologue, sizeof(kFlashPrologue)) != 0)
    {
        Game::logMsg("UpdateFlashlightState skipped (bytes/rva 0x%X)", Offsets::kUpdateFlashlightState);
        Game::noteMissingOffset("UpdateFlashlightState");
        hkUpdateFlashlightState.pTarget = p;
        return;
    }
    if (hkUpdateFlashlightState.createHook(p, &dUpdateFlashlightState) != 0
        || hkUpdateFlashlightState.enableHook() != 0)
    {
        Game::logMsg("UpdateFlashlightState hook failed rva=0x%X", Offsets::kUpdateFlashlightState);
        return;
    }
    Game::logMsg("Hook enabled: UpdateFlashlightState rva=0x%X (HMD origin/forward for VR eyes)",
        Offsets::kUpdateFlashlightState);
}

// Redirects the two fog-colour EyeVectors calls to dFogEyeVectors, so the
// fogblend colour follows the headset like the picture does, not the gun
// (or, while L2 is held, the left hand): moving a hand up/down changed the
// brightness of the distant scene.
void Hooks::EnsureFogColorPatch()
{
    static bool s_done;
    if (s_done)
        return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client)
        return;
    s_done = true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(client);
    const uintptr_t target = base + Offsets::kEyeVectors;
    static const unsigned char kEyeVectorsPrologue[] = { 0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0xF9 };
    const int sites[] = { Offsets::kFogColorEyeVectorsCall, Offsets::kSkyFogColorEyeVectorsCall };
    int patched = 0;
    for (int rva : sites)
    {
        auto* call = reinterpret_cast<unsigned char*>(base + rva);
        bool ok = false;
        BMVR_TRY
        {
            ok = memcmp(reinterpret_cast<const void*>(target), kEyeVectorsPrologue, sizeof(kEyeVectorsPrologue)) == 0
                && call[-1] == 0x50 && call[0] == 0xE8
                && reinterpret_cast<uintptr_t>(call) + 5 + *reinterpret_cast<const int32_t*>(call + 1) == target;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok)
        {
            Game::logMsg("Fog colour EyeVectors call skipped (bytes/rva 0x%X)", rva);
            Game::noteMissingOffset("Fog colour EyeVectors call");
            continue;
        }
        DWORD oldProt = 0, tmp = 0;
        if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &oldProt))
            continue;
        *reinterpret_cast<int32_t*>(call + 1) = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&dFogEyeVectors) - (reinterpret_cast<uintptr_t>(call) + 5));
        VirtualProtect(call + 1, 4, oldProt, &tmp);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        ++patched;
    }
    Game::logMsg("Fog colour follows the headset: %d/2 call sites patched", patched);
}

void __fastcall Hooks::dFogEyeVectors(void* ecx, void* edx, Vector* fwd, Vector* right, Vector* up)
{
    BMVR_PROF("hook FogEyeVectors");
    (void)edx;
    if (fwd && !right && !up && m_VR && m_VR->m_IsVREnabled && m_VR->m_HmdPoseValid)
    {
        const Vector a = m_VR->GetViewAngle();
        const float p = a.x * 3.14159265f / 180.f;
        const float y = a.y * 3.14159265f / 180.f;
        fwd->x = cosf(p) * cosf(y);
        fwd->y = cosf(p) * sinf(y);
        fwd->z = -sinf(p);
        return;
    }
    using EyeVectorsFn = void(__thiscall*)(void*, Vector*, Vector*, Vector*);
    static const auto s_eyeVectors = reinterpret_cast<EyeVectorsFn>(
        reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll")) + Offsets::kEyeVectors);
    s_eyeVectors(ecx, fwd, right, up);
}

void Hooks::EnsureBmFlashlightHook()
{
    if (hkBmFlashlightUpdate.pTarget)
        return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client)
        return;
    MEMORY_BASIC_INFORMATION mbi{};
    auto* q = reinterpret_cast<unsigned char*>(client) + Offsets::kBmFlashlightUpdate;
    static const unsigned char kBmFlashPrologue[] = { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0,
        0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04 };
    hkBmFlashlightUpdate.pTarget = q;
    if (!VirtualQuery(q, &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        || memcmp(q, kBmFlashPrologue, sizeof(kBmFlashPrologue)) != 0)
    {
        Game::logMsg("BM flashlight update skipped (bytes/rva 0x%X)", Offsets::kBmFlashlightUpdate);
        Game::noteMissingOffset("BmFlashlightUpdate");
        return;
    }
    if (hkBmFlashlightUpdate.createHook(q, &dBmFlashlightUpdate) == 0
        && hkBmFlashlightUpdate.enableHook() == 0)
        Game::logMsg("Hook enabled: BM flashlight update rva=0x%X (light from the right hand)",
            Offsets::kBmFlashlightUpdate);
    else
        Game::logMsg("BM flashlight update hook failed rva=0x%X", Offsets::kBmFlashlightUpdate);
}

void __fastcall Hooks::dBmFlashlightUpdate(void* ecx, void* edx, Vector* pos, void* player, Vector* fwd, Vector* right, Vector* up)
{
    (void)edx;
    // The game lights from the body's eye point along the eye angles, which
    // in VR are the gun aim: the beam points with the hand but starts at the
    // body, so it bends round corners the hand reaches past. Start it at the
    // right hand instead. The game adds its shoulder offset (PosY, -28 from
    // skill.cfg) along its own time-smoothed basis, so subtracting it here
    // along the current basis left a wobble whenever the hand turned: zero
    // those cvars instead (not archived). PosX (skill.cfg -5) stays.
    Vector p{}, f{}, r{}, u{};
    bool vr = false;
    static Vector s_lastOffset;
    static bool s_haveOffset;
    if (pos && fwd && right && up && m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
        && (m_VR->m_RightControllerTrackingValid || m_VR->m_LeftControllerTrackingValid || s_haveOffset)
        && EngineInGame())
    {
        const Vector body = m_VR->m_HasStereoBodyOrigin ? m_VR->m_StereoBodyOrigin : m_VR->m_SetupOrigin;
        if (body.LengthSqr() > 1.f)
        {
            // skill.cfg sets them (PosY -28) and runs again on every map
            // load, so check once a second.
            static uint64_t s_offsetTick;
            const uint64_t tick = GetTickCount64();
            if (m_Game && tick - s_offsetTick > 1000)
            {
                s_offsetTick = tick;
                const char* kOffsets[] = { "gb_flashlight_PosY", "gb_flashlight_PosZ",
                    "gb_flashlight_PosY_Crouch", "gb_flashlight_PosZ_Crouch" };
                for (const char* name : kOffsets)
                {
                    float v = 0.f;
                    if (m_Game->GetConVarFloat(name, v) && v != 0.f)
                    {
                        m_Game->SetConVarFloat(name, 0.f);
                        Game::logMsg("BM flashlight: %s %.1f -> 0", name, v);
                    }
                }
            }
            // Head (FlashlightFrom 2): from the eyes along the view, as the
            // flat game does.
            const int from = static_cast<int>(bmvr::g_FlashlightFrom + 0.5f);
            if (from == 2)
            {
                const Vector eye = m_VR->GetViewOrigin(body);
                const Vector va = m_VR->GetViewAngle();
                QAngle::AngleVectors(QAngle(va.x, va.y, 0.f), &f, &r, &u);
                if (VectorNormalize(f) > 0.001f && eye.LengthSqr() > 1.f)
                {
                    p = eye;
                    vr = true;
                    static int s_headLog;
                    if (s_headLog++ < 2)
                        Game::logMsg("BM flashlight from the head (%.1f,%.1f,%.1f)", eye.x, eye.y, eye.z);
                }
                hkBmFlashlightUpdate.fOriginal(ecx, vr ? &p : pos, player, vr ? &f : fwd, vr ? &r : right, vr ? &u : up);
                return;
            }
            // Left hand (option): its own pointing direction, except while
            // both hands hold the gun, when it shines along the gun (as the
            // Lambda1VR torch does).
            const bool leftHand = from == 1;
            QAngle dir = m_VR->GetAimAngles();
            if (leftHand && !m_VR->TwoHandGripActive())
            {
                dir = m_VR->GetLeftControllerAbsAngle();
                dir.x -= bmvr::g_AimPitchOffset;
                dir.z = 0.f;
            }
            QAngle::AngleVectors(dir, &f, &r, &u);
            // This runs every game frame (~180/s). Between pose updates the
            // controller is not flagged valid and ControllerTrackingToWorld
            // falls back to the head, so the light jumped to the face a few
            // times a second (flicker). Hold the last good offset instead.
            const bool poseFresh = (leftHand ? m_VR->m_LeftControllerTrackingValid : m_VR->m_RightControllerTrackingValid)
                && (m_VR->m_ControllerPoseValid || m_VR->m_StereoFramePoseActive);
            static bool s_lastLeft;
            if (leftHand != s_lastLeft)
            {
                s_lastLeft = leftHand;
                s_haveOffset = false;
                s_lastOffset = Vector{};
            }
            Vector hand{};
            if (poseFresh)
                hand = leftHand ? m_VR->GetLeftControllerAbsPos(body) : m_VR->GetRightControllerAbsPos(body);
            // The flag can still flip between this check and the conversion:
            // a 'hand' on the head view point is that fallback, not a hand.
            if (poseFresh && (hand - m_VR->GetViewOrigin(body)).LengthSqr() > 1.5f * 1.5f)
            {
                s_lastOffset = hand - body;
                s_haveOffset = true;
            }
            else
                hand = body + s_lastOffset;
            if (VectorNormalize(f) > 0.001f && hand.LengthSqr() > 1.f)
            {
                p = hand;
                vr = true;
                static int s_log;
                if (s_log < 4)
                {
                    Game::logMsg("BM flashlight from hand (%.1f,%.1f,%.1f), game eye (%.1f,%.1f,%.1f)",
                        hand.x, hand.y, hand.z, pos->x, pos->y, pos->z);
                    ++s_log;
                }
            }
        }
    }
    if (vr)
        hkBmFlashlightUpdate.fOriginal(ecx, &p, player, &f, &r, &u);
    else
        hkBmFlashlightUpdate.fOriginal(ecx, pos, player, fwd, right, up);
}

void __fastcall Hooks::dUpdateFlashlightState(void* ecx, void* edx, void* flashlightState)
{
    (void)edx;
    // Desktop beam works; HMD missed it because FlashlightState_t is filled
    // from player EyePosition/EyeAngles (body) while stereo cameras use the
    // HMD. Retarget to HMD view.
            if (flashlightState && m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
        && m_VR->m_HmdPoseValid && EngineInGame())
    {
        BMVR_TRY
        {
            auto* f = reinterpret_cast<float*>(flashlightState);
            const Vector body = m_VR->m_HasStereoBodyOrigin ? m_VR->m_StereoBodyOrigin : m_VR->m_SetupOrigin;
            const Vector origin = m_VR->GetViewOrigin(body.LengthSqr() > 1.f ? body : m_VR->m_SetupOrigin);
            Vector fwd, right, up;
            m_VR->GetViewBasis(&fwd, &right, &up);
            if (VectorNormalize(fwd) > 0.001f && origin.LengthSqr() > 1.f)
            {
                f[0] = origin.x;
                f[1] = origin.y;
                f[2] = origin.z;
                f[3] = fwd.x;
                f[4] = fwd.y;
                f[5] = fwd.z;
                m_VR->NoteFlashlightState(origin, fwd);
                static int s_flLog;
                if (s_flLog < 6)
                {
                    Game::logMsg("FlashlightState -> HMD origin=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f)",
                        origin.x, origin.y, origin.z, fwd.x, fwd.y, fwd.z);
                    ++s_flLog;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    if (hkUpdateFlashlightState.fOriginal)
        hkUpdateFlashlightState.fOriginal(ecx, flashlightState);
}

namespace
{
    bool CodeBytesMatch(const unsigned char* p, const char* hex)
    {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int i = 0;
        while (hex[0] && hex[1])
        {
            if (hex[0] == ' ')
            {
                ++hex;
                continue;
            }
            if (hex[0] == '?' && hex[1] == '?')
            {
                ++i;
                hex += 2;
                continue;
            }
            const int hi = nibble(hex[0]);
            const int lo = nibble(hex[1]);
            if (hi < 0 || lo < 0)
                return false;
            if (p[i] != static_cast<unsigned char>((hi << 4) | lo))
                return false;
            ++i;
            hex += 2;
        }
        return i > 0;
    }

    // unique: nullptr unless the pattern occurs exactly once (ResolveCode's fallback after a game update).
    unsigned char* FindPatternInModule(HMODULE mod, const char* hex, bool unique = false)
    {
        unsigned char* found = nullptr;
        if (!mod || !hex)
            return nullptr;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const unsigned char*>(mod) + dos->e_lfanew);
        const size_t imgSize = nt->OptionalHeader.SizeOfImage;
        auto* base = reinterpret_cast<unsigned char*>(mod);
        size_t patLen = 0;
        for (const char* h = hex; h[0] && h[1]; )
        {
            if (h[0] == ' ')
            {
                ++h;
                continue;
            }
            ++patLen;
            h += 2;
        }
        if (patLen == 0 || imgSize <= patLen)
            return nullptr;
        for (size_t i = 0; i + patLen < imgSize; ++i)
        {
            if (CodeBytesMatch(base + i, hex))
            {
                if (!unique)
                    return base + i;
                if (found)
                    return nullptr;   // ambiguous: a short prologue that also starts other functions
                found = base + i;
            }
        }
        return found;
    }

    unsigned char* ResolveCode(HMODULE mod, int rva, const char* hex)
    {
        if (!mod || !hex)
            return nullptr;
        auto* p = reinterpret_cast<unsigned char*>(mod) + rva;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi))
            && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, hex))
            return p;
        return FindPatternInModule(mod, hex, true);
    }

    bool InServerDll(const void* p)
    {
        static uintptr_t s_base = 0, s_end = 0;
        if (!s_base)
        {
            HMODULE mod = GetModuleHandleA("server.dll");
            if (!mod)
                return false;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const unsigned char*>(mod) + dos->e_lfanew);
            s_base = reinterpret_cast<uintptr_t>(mod);
            s_end = s_base + nt->OptionalHeader.SizeOfImage;
        }
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        return a >= s_base && a < s_end;
    }

    int ServerEntityIndex(void* ent)
    {
        if (!ent)
            return 0;
        int idx = 0;
        // BM's server player has no IServerNetworkable vtable at +8 (null or a
        // non-code pointer). Relying on __except cost ~1000 faults/s under
        // Wine (8 fps, 2026-10-11) and called a garbage address half the time.
        // Only call through a vtable and function that live in server.dll.
        unsigned char* net = reinterpret_cast<unsigned char*>(ent) + 8;
        void** vt = nullptr;
        void* fn = nullptr;
        using Fn = int(__thiscall*)(void*);
        BMVR_TRY
        {
            vt = *reinterpret_cast<void***>(net);
            if (InServerDll(vt))
                fn = vt[0x24 / 4];
            if (InServerDll(fn))
                idx = reinterpret_cast<Fn>(fn)(net);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            idx = 0;
        }
        return idx;
    }

    bool ShouldRewriteShootOrigin(void* player, const Vector* current)
    {
        if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
            return false;
        if (!Hooks::m_VR->UseGrabActive() && !Hooks::m_VR->m_ControllerPoseValid)
            return false;
        if (!Hooks::m_VR->IsGameplayEligible() || !EngineInGame())
            return false;
        if (!Hooks::m_Game)
            return false;
        C_BaseEntity* local = Hooks::m_Game->GetLocalPlayerEntity();
        if (local && player == local)
            return true;
        if (Hooks::m_Game->m_EngineClient)
        {
            const int want = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
            const int have = ServerEntityIndex(player);
            if (want > 0 && have == want)
                return true;
        }
        if (current)
        {
            Vector body = Hooks::m_VR->m_HasStereoBodyOrigin ? Hooks::m_VR->m_StereoBodyOrigin : Hooks::m_VR->m_SetupOrigin;
            if (body.LengthSqr() > 1.f)
            {
                const Vector d = *current - body;
                if (d.LengthSqr() < 96.f * 96.f)
                    return true;
            }
        }
        return false;
    }

    bool ShouldRewriteShootAngles(void* player)
    {
        if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
            return false;
        if (!Hooks::m_VR->IsGameplayEligible() || !EngineInGame())
            return false;
        if (!player)
            return false;
        // Server GetShootAngles is called with the SERVER player. That pointer
        // is never the client GetLocalPlayerEntity(), and IServerNetworkable
        // slot 0x24 is not a reliable entindex, so requiring a match left the
        // original EyeAngles+punch in place. Punch resets on map load — the
        // "hits normalize then walk off" report. Only skip a *different*
        // identified player (listen-server MP). Unknown index = local SP.
        if (Hooks::m_Game && Hooks::m_Game->m_EngineClient)
        {
            const int want = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
            const int have = ServerEntityIndex(player);
            if (have > 0 && want > 0 && have != want)
                return false;
        }
        return true;
    }

    void ApplyVrShootAngles(void* player, QAngle* out)
    {
        if (!player || !out || !ShouldRewriteShootAngles(player) || !Hooks::m_VR)
            return;
        QAngle aim{};
        if (!Hooks::m_VR->TryGetFireAim(aim))
            return;
        out->x = aim.x;
        out->y = aim.y;
        out->z = 0.f;
        static int s_aimLog;
        if (s_aimLog < 8)
        {
            Game::logMsg("GetShootAngles VR (%.1f,%.1f)", aim.x, aim.y);
            ++s_aimLog;
        }
    }

    Vector* RewriteShootOriginImpl(void* player, Vector* out, tWeaponShootPosition original, bool grabHold);

    // A muzzle pushed through bulletproof glass, a wall or a door must not
    // fire from the other side (the player could shoot soldiers who could not
    // hit back). Trace from the game's own origin (the body's eye) to the
    // muzzle; anything bullet-solid in between keeps the game's origin, so the
    // shot hits that instead. (Server and client run on the game thread in
    // single player, so the client trace is safe here.)
    bool MuzzleBehindGeometry(const Vector& eye, const Vector& muzzle)
    {
        if (!Hooks::m_Game || !Hooks::m_Game->m_EngineTrace)
            return false;
        C_BaseEntity* local = Hooks::m_Game->GetLocalPlayerEntity();
        C_BaseEntity* vm = Hooks::m_Game->GetViewModelEntity();
        CTraceFilterSkipTwoEntities filter(local, vm, 0);
        Ray_t ray;
        ray.Init(eye, muzzle);
        CGameTrace tr{};
        Hooks::m_Game->m_EngineTrace->TraceRay(ray, CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW, &filter, &tr);
        if (!(tr.startsolid || tr.fraction < 1.f))
            return false;
        static int s_log;
        if (s_log < 16)
        {
            Game::logMsg("Shot origin: muzzle behind geometry (fraction %.2f), fired from the body", tr.fraction);
            ++s_log;
        }
        return true;
    }

    Vector* RewriteShootOrigin(void* player, Vector* out, tWeaponShootPosition original, bool grabHold, bool server)
    {
        Vector* r = RewriteShootOriginImpl(player, out, original, grabHold);
        if (bmvr::g_AimDebug && Hooks::m_VR && out && !grabHold)
        {
            Vector before{};
            if (original)
                original(player, &before);
            const bool rewrote = (before - *out).LengthSqr() > 0.01f;
            Hooks::m_VR->AimDebugShootOrigin(server, player, rewrote, before, *out);
        }
        return r;
    }

    Vector* RewriteShootOriginImpl(void* player, Vector* out, tWeaponShootPosition original, bool grabHold)
    {
        if (original && out)
            original(player, out);
        else if (out)
        {
            out->x = 0.f;
            out->y = 0.f;
            out->z = 0.f;
        }
        if (!out)
            return out;
        if (!ShouldRewriteShootOrigin(player, out))
            return out;
        Vector muzzle{};
        QAngle meleeAim{};
        if (!grabHold && Hooks::m_VR->TryGetMeleeAim(muzzle, meleeAim))
        {
            *out = muzzle;
            return out;
        }
        // Grab-controller hold origin and weapon fire share this hook family.
        // Only a live physics hold may move origin onto the Use hand. Leftover
        // left-Use latch used to keep gunshots and the flashlight on that hand.
        if (grabHold)
        {
            if (Hooks::m_VR->UseGrabRedirectsFire() && Hooks::m_VR->TryGetVrUseOrigin(muzzle))
            {
                static int s_grabLog;
                if (s_grabLog < 8)
                {
                    Game::logMsg("Grab hold origin VR (%.1f,%.1f,%.1f) was (%.1f,%.1f,%.1f)",
                        muzzle.x, muzzle.y, muzzle.z, out->x, out->y, out->z);
                    ++s_grabLog;
                }
                *out = muzzle;
            }
            return out;
        }
        if (Hooks::m_VR->UseGrabRedirectsFire() && Hooks::m_VR->TryGetVrUseOrigin(muzzle))
        {
            if (!MuzzleBehindGeometry(*out, muzzle))
                *out = muzzle;
            return out;
        }
        if (Hooks::m_VR->ScopeZoomActive())
        {
            if (Hooks::m_VR->TryGetScopeShootOrigin(muzzle))
            {
                static int s_scopeLog;
                if (s_scopeLog < 8)
                {
                    Game::logMsg("Weapon_ShootPosition scope eye (%.1f,%.1f,%.1f) was (%.1f,%.1f,%.1f)",
                        muzzle.x, muzzle.y, muzzle.z, out->x, out->y, out->z);
                    ++s_scopeLog;
                }
                if (!MuzzleBehindGeometry(*out, muzzle))
                    *out = muzzle;
            }
            return out;
        }
        if (!Hooks::m_VR->TryGetVrShootOrigin(muzzle))
            return out;
        static int s_log;
        if (s_log < 8)
        {
            Game::logMsg("Weapon_ShootPosition VR muzzle (%.1f,%.1f,%.1f) was (%.1f,%.1f,%.1f)",
                muzzle.x, muzzle.y, muzzle.z, out->x, out->y, out->z);
            ++s_log;
        }
        if (!MuzzleBehindGeometry(*out, muzzle))
            *out = muzzle;
        return out;
    }
}

void Hooks::EnsureWeaponShootOriginHooks()
{
    // Called 2-3 times per CreateMove tick and from RenderView. Once every
    // target is resolved (installed or marked skipped) the body is only
    // GetModuleHandleA calls under the loader lock; skip it.
    static bool s_done = false;
    if (s_done)
        return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (client && !hkClientWeaponShootPosition.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCBasePlayer_Weapon_ShootPosition;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC8B11FF7508FF9268020000"))
        {
            if (hkClientWeaponShootPosition.createHook(p, &dClientWeaponShootPosition) == 0
                && hkClientWeaponShootPosition.enableHook() == 0)
                Game::logMsg("Hook enabled: client Weapon_ShootPosition rva=0x%X", Offsets::kCBasePlayer_Weapon_ShootPosition);
            else
                Game::logMsg("client Weapon_ShootPosition hook failed");
        }
        else if (!hkClientWeaponShootPosition.pTarget)
        {
            Game::logMsg("client Weapon_ShootPosition skipped (bytes/rva 0x%X)", Offsets::kCBasePlayer_Weapon_ShootPosition);
            Game::noteMissingOffset("client Weapon_ShootPosition");
            hkClientWeaponShootPosition.pTarget = p;
        }
    }
    if (client && !hkGetAttachmentVec.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCBaseAnimating_GetAttachmentVec;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC56578B7D088BF183FF01"))
        {
            if (hkGetAttachmentVec.createHook(p, &dGetAttachmentVec) == 0
                && hkGetAttachmentVec.enableHook() == 0)
                Game::logMsg("Hook enabled: GetAttachment vec rva=0x%X", Offsets::kCBaseAnimating_GetAttachmentVec);
            else
                Game::logMsg("GetAttachment vec hook failed");
        }
        else
        {
            Game::logMsg("GetAttachment vec skipped (bytes/rva 0x%X)", Offsets::kCBaseAnimating_GetAttachmentVec);
            Game::noteMissingOffset("GetAttachment vec");
            hkGetAttachmentVec.pTarget = p;
        }
    }
    if (client && !hkFormatViewModelAttachment.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kFormatViewModelAttachment;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC83EC188B0D????????56578B01FF5030"))
        {
            if (hkFormatViewModelAttachment.createHook(p, &dFormatViewModelAttachment) == 0
                && hkFormatViewModelAttachment.enableHook() == 0)
                Game::logMsg("Hook enabled: FormatViewModelAttachment rva=0x%X (VR: viewmodel attachments stay where the gun is drawn)", Offsets::kFormatViewModelAttachment);
            else
                Game::logMsg("FormatViewModelAttachment hook failed");
        }
        else
        {
            Game::logMsg("FormatViewModelAttachment skipped (bytes/rva 0x%X)", Offsets::kFormatViewModelAttachment);
            Game::noteMissingOffset("FormatViewModelAttachment");
            hkFormatViewModelAttachment.pTarget = p;
        }
    }
    if (client && !hkGetAttachmentMatrix.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCBaseAnimating_GetAttachmentMatrix;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC568B7508578BF983FE01"))
        {
            if (hkGetAttachmentMatrix.createHook(p, &dGetAttachmentMatrix) == 0
                && hkGetAttachmentMatrix.enableHook() == 0)
                Game::logMsg("Hook enabled: GetAttachment matrix rva=0x%X", Offsets::kCBaseAnimating_GetAttachmentMatrix);
            else
                Game::logMsg("GetAttachment matrix hook failed");
        }
        else
        {
            Game::logMsg("GetAttachment matrix skipped (bytes/rva 0x%X)", Offsets::kCBaseAnimating_GetAttachmentMatrix);
            Game::noteMissingOffset("GetAttachment matrix");
            hkGetAttachmentMatrix.pTarget = p;
        }
    }

    HMODULE server = GetModuleHandleA("server.dll");
    if (server && !hkServerWeaponShootPosition.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kCBasePlayer_Weapon_ShootPosition_Server;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC8B11FF7508FF9230020000"))
        {
            if (hkServerWeaponShootPosition.createHook(p, &dServerWeaponShootPosition) == 0
                && hkServerWeaponShootPosition.enableHook() == 0)
                Game::logMsg("Hook enabled: server Weapon_ShootPosition rva=0x%X", Offsets::kCBasePlayer_Weapon_ShootPosition_Server);
            else
                Game::logMsg("server Weapon_ShootPosition hook failed");
        }
        else
        {
            Game::logMsg("server Weapon_ShootPosition skipped (bytes/rva 0x%X)", Offsets::kCBasePlayer_Weapon_ShootPosition_Server);
            Game::noteMissingOffset("server Weapon_ShootPosition");
            hkServerWeaponShootPosition.pTarget = p;
        }
    }
    if (server && !hkServerGrabHoldOrigin.pTarget)
    {
        // CGrabController::UpdateObject calls player vtable +0x474, not +0x23C.
        auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kCBasePlayer_GrabHoldOrigin_Server;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC83EC24578BF98D4DE86A00516A008B078BCFFF904C020000"))
        {
            if (hkServerGrabHoldOrigin.createHook(p, &dServerGrabHoldOrigin) == 0
                && hkServerGrabHoldOrigin.enableHook() == 0)
                Game::logMsg("Hook enabled: server grab hold origin rva=0x%X", Offsets::kCBasePlayer_GrabHoldOrigin_Server);
            else
                Game::logMsg("server grab hold origin hook failed");
        }
        else
        {
            Game::logMsg("server grab hold origin skipped (bytes/rva 0x%X)", Offsets::kCBasePlayer_GrabHoldOrigin_Server);
            Game::noteMissingOffset("server grab hold origin");
            hkServerGrabHoldOrigin.pTarget = p;
        }
    }
    if (server && !hkServerBmShootSource.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kBlackMesaPlayer_ShootSource_Server;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BECFF7508E8????????8B45085DC20400"))
        {
            if (hkServerBmShootSource.createHook(p, &dServerBmShootSource) == 0
                && hkServerBmShootSource.enableHook() == 0)
                Game::logMsg("Hook enabled: server BM player +0x474 shot origin rva=0x%X", Offsets::kBlackMesaPlayer_ShootSource_Server);
            else
                Game::logMsg("server BM player +0x474 shot origin hook failed");
        }
        else
        {
            Game::logMsg("server BM player +0x474 shot origin skipped (bytes/rva 0x%X)", Offsets::kBlackMesaPlayer_ShootSource_Server);
            Game::noteMissingOffset("server BM player +0x474 shot origin");
            hkServerBmShootSource.pTarget = p;
        }
    }
    if (server && !hkGrabSetTarget.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kCGrabController_SetTargetPosition;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC8B45088BD1D900D95A04"))
        {
            if (hkGrabSetTarget.createHook(p, &dGrabSetTarget) == 0
                && hkGrabSetTarget.enableHook() == 0)
                Game::logMsg("Hook enabled: server grab SetTargetPosition rva=0x%X", Offsets::kCGrabController_SetTargetPosition);
            else
                Game::logMsg("server grab SetTargetPosition hook failed");
        }
        else
        {
            Game::logMsg("server grab SetTargetPosition skipped (bytes/rva 0x%X)", Offsets::kCGrabController_SetTargetPosition);
            Game::noteMissingOffset("server grab SetTargetPosition");
            hkGrabSetTarget.pTarget = p;
        }
    }
    if (server && !hkPlayerUse.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kCBasePlayer_PlayerUse_Server;
        MEMORY_BASIC_INFORMATION mbi{};
        // aligned-frame prologue, sub esp 0xC8, cookie, mov edi,ecx; mov eax,[edi+0xAD8]
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "538BDC83EC0883E4F083C404558B6B04896C24048BEC81ECC8000000A1????????33C58945FC56578BF98B87D80A0000"))
        {
            if (hkPlayerUse.createHook(p, &dPlayerUse) == 0 && hkPlayerUse.enableHook() == 0)
                Game::logMsg("Hook enabled: server PlayerUse rva=0x%X", Offsets::kCBasePlayer_PlayerUse_Server);
            else
                Game::logMsg("server PlayerUse hook failed");
        }
        else
        {
            Game::logMsg("server PlayerUse skipped (bytes/rva 0x%X)", Offsets::kCBasePlayer_PlayerUse_Server);
            Game::noteMissingOffset("server PlayerUse");
        }
        hkPlayerUse.pTarget = p;
    }
    if (server && !hkGetShootAngles.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kBlackMesaPlayer_GetShootAngles_Server;
        MEMORY_BASIC_INFORMATION mbi{};
        // prologue, mov esi/ecx, call vtable+0x234 (EyeAngles), fetch the out arg
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC568BF18B06FF90340200008B5508"))
        {
            if (hkGetShootAngles.createHook(p, &dGetShootAngles) == 0
                && hkGetShootAngles.enableHook() == 0)
                Game::logMsg("Hook enabled: server GetShootAngles rva=0x%X", Offsets::kBlackMesaPlayer_GetShootAngles_Server);
            else
                Game::logMsg("server GetShootAngles hook failed");
        }
        else
        {
            Game::logMsg("server GetShootAngles skipped (bytes/rva 0x%X)", Offsets::kBlackMesaPlayer_GetShootAngles_Server);
            Game::noteMissingOffset("server GetShootAngles");
            hkGetShootAngles.pTarget = p;
        }
    }
    auto hookServerFn = [&](auto& hook, int rva, const char* bytes, auto detour, const char* what)
    {
        if (!server || hook.pTarget)
            return;
        auto* p = reinterpret_cast<unsigned char*>(server) + rva;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, bytes))
        {
            if (hook.createHook(p, detour) == 0 && hook.enableHook() == 0)
                Game::logMsg("Hook enabled: %s rva=0x%X", what, rva);
            else
                Game::logMsg("%s hook failed", what);
        }
        else
        {
            Game::logMsg("%s skipped (bytes/rva 0x%X)", what, rva);
            Game::noteMissingOffset(what);
            hook.pTarget = p;
        }
    };
    hookServerFn(hkGrenadeCreate, Offsets::kGrenadeCreate_Server, "558BEC8B450883EC3083F815",
        &dGrenadeCreate, "server grenade create");
    hookServerFn(hkGrenadeThrowPosition, Offsets::kGrenadeThrowPosition_Server, "558BEC81EC88000000",
        &dGrenadeThrowPosition, "server grenade throw position");
    hookServerFn(hkProjectileCreate, Offsets::kProjectileCreate_Server, "558BEC83EC3C538B5D0883FB15",
        &dProjectileCreate, "server projectile create");
    hookServerFn(hkSnarkThrow, Offsets::kSnarkThrow_Server, "558BEC83EC3C53578B7D088BD9",
        &dSnarkThrow, "server snark throw");
    hookServerFn(hkSnarkPlace, Offsets::kSnarkPlace_Server, "558BEC83EC18538B5D1485DB",
        &dSnarkPlace, "server snark placement");
    hookServerFn(hkShouldSwitchWeapon, Offsets::kSpRules_FShouldSwitchWeapon_Server, "558BEC56578B7D0C8BCF8B078B8018040000",
        &dShouldSwitchWeapon, "server pickup auto-switch (FShouldSwitchWeapon)");
    if (server && !s_AutoSwitchSitesTried)
    {
        s_AutoSwitchSitesTried = true;
        // Each site: one opcode byte the toggle flips, checked with the bytes around it first.
        struct Site { int rva; const char* ctx; int at; const char* what; unsigned char** slot; };
        const Site sites[] = {
            // call [eax+0x55C] (GetWeaponFlags); test al, 4 (A8 / or al, 4 = 0C); jne
            { Offsets::kReloadOrSwitchNoAutoSwitchInsn_Server, "FF905C050000??040F85", 6,
              "server empty-weapon auto-switch", &s_AutoSwitchEmptyInsn },
            // call eax (slot 0x558); test al, al; je +0D (74 / jmp = EB); ...; call [eax+0x470] (Weapon_Switch)
            { Offsets::kBmWeaponEquipSwitchJeInsn_Server, "FFD084C0??0D8B078BCF6A0056FF9070040000", 4,
              "server pickup auto-switch (weapon Equip)", &s_AutoSwitchEquipInsn },
        };
        for (const Site& st : sites)
        {
            auto* p = reinterpret_cast<unsigned char*>(server) + st.rva;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p - st.at, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
                && CodeBytesMatch(p - st.at, st.ctx))
            {
                *st.slot = p;
                Game::logMsg("%s patch site ok rva=0x%X", st.what, st.rva);
            }
            else
            {
                Game::logMsg("%s patch skipped (bytes/rva 0x%X)", st.what, st.rva);
                Game::noteMissingOffset(st.what);
            }
        }
        ApplyAutoSwitchPatches();
    }
    if (client && !hkClientGetShootAngles.pTarget)
    {
        // C_BlackMesaPlayer::GetShootAngles (vtable slot 0x9D): EyeAngles (client vtable +0x26C, not the
        // server's +0x234) plus punch. C_BasePlayer has an identical copy (0x6C660), so no pattern fallback.
        constexpr const char* kShootAngBytes = "558BEC568BF18B06FF906C0200008B5508";
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kC_BlackMesaPlayer_GetShootAngles;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))
            || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            || !CodeBytesMatch(p, kShootAngBytes))
            p = nullptr;
        if (p)
        {
            if (hkClientGetShootAngles.createHook(p, &dClientGetShootAngles) == 0
                && hkClientGetShootAngles.enableHook() == 0)
                Game::logMsg("Hook enabled: client GetShootAngles rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("client GetShootAngles hook failed");
        }
        else
        {
            Game::logMsg("client GetShootAngles skipped (bytes/rva 0x%X)", Offsets::kC_BlackMesaPlayer_GetShootAngles);
            Game::noteMissingOffset("client GetShootAngles");
            hkClientGetShootAngles.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCBasePlayer_Weapon_ShootPosition;
        }
    }
    const bool vfxDone = EnsureWeaponVfxHooks();
    s_done = vfxDone
        && hkClientWeaponShootPosition.pTarget && hkGetAttachmentVec.pTarget
        && hkGetAttachmentMatrix.pTarget && hkServerWeaponShootPosition.pTarget
        && hkServerGrabHoldOrigin.pTarget && hkGrabSetTarget.pTarget && hkPlayerUse.pTarget
        && hkGetShootAngles.pTarget && hkClientGetShootAngles.pTarget
        && hkGrenadeCreate.pTarget && hkGrenadeThrowPosition.pTarget;
}

void __fastcall Hooks::dGetShootAngles(void* ecx, void* edx, QAngle* out)
{
    BMVR_PROF("hook GetShootAngles");
    (void)edx;
    if (!hkGetShootAngles.fOriginal)
        return;
    hkGetShootAngles.fOriginal(ecx, out);
    const QAngle before = out ? *out : QAngle{};
    ApplyVrShootAngles(ecx, out);
    if (bmvr::g_AimDebug && m_VR && out)
    {
        if ((m_VR->m_HeldButtons.load(std::memory_order_acquire) & (IN_ATTACK | IN_ATTACK2)) != 0)
        {
            static int s_callerLog;
            if (s_callerLog < 60)
            {
                ++s_callerLog;
                Game::logMsg("AimDebug GetShootAngles caller rva=0x%X",
                    ServerRva(reinterpret_cast<uintptr_t>(_ReturnAddress())));
            }
        }
        m_VR->AimDebugShootAngles(true, ecx, (before.x != out->x || before.y != out->y), before, *out);
    }
}

void __fastcall Hooks::dClientGetShootAngles(void* ecx, void* edx, QAngle* out)
{
    BMVR_PROF("hook ClientGetShootAngles");
    (void)edx;
    if (hkClientGetShootAngles.fOriginal)
        hkClientGetShootAngles.fOriginal(ecx, out);
    const QAngle before = out ? *out : QAngle{};
    ApplyVrShootAngles(ecx, out);
    if (bmvr::g_AimDebug && m_VR && out)
        m_VR->AimDebugShootAngles(false, ecx, (before.x != out->x || before.y != out->y), before, *out);
}

Vector* __fastcall Hooks::dClientWeaponShootPosition(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    return RewriteShootOrigin(ecx, out, hkClientWeaponShootPosition.fOriginal, false, false);
}

Vector* __fastcall Hooks::dServerWeaponShootPosition(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    return RewriteShootOrigin(ecx, out, hkServerWeaponShootPosition.fOriginal, false, true);
}

Vector* __fastcall Hooks::dServerGrabHoldOrigin(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    return RewriteShootOrigin(ecx, out, hkServerGrabHoldOrigin.fOriginal, true, true);
}

namespace
{
    // Bullet origin. CBlackMesaPlayer overrides player vtable +0x474 with a
    // thunk (server RVA 0x47C810 -> FUN_1012F2C0, the eye position); 0x77240
    // above is only CBasePlayer's version, so the BM player's +0x474 calls never
    // reached a hook, and neither does any weapon go through Weapon_ShootPosition
    // (+0x23C). Bullets therefore left the head along the controller direction
    // while the reticle sits on the controller ray: a constant world offset of
    // (hand - head), "up-left" for a right hand held low. Measured headless with
    // the fake runtime 2026-10-12 (decals on the eye-origin ray to ~1 px; 357,
    // Glock, MP5). Only the weapon fire call sites below get the VR origin; every
    // other caller (grab controller, AI, ...) keeps the original eye position.
    // Return-address RVAs in server.dll (public-beta 25844216). 0x34E5D9: the
    // shared bullet routine reads the shooter's +0x474 as vecSrc (`mov eax,
    // [eax+0x474]; call eax`; seen with AimDebug for the 357, Glock and MP5 on
    // stable, where it was 0x34DEF9). The others are the +0x474 calls directly
    // followed by GetShootAngles (+0x240) in weapon code (stable 0x48EDA4,
    // 0x484C85, 0x484F0B, 0x48B435; all four moved +0x9D0). 0x48D265: the tau
    // cannon's fire routine (called only from CWeapon_Tau, vtable +0x49C),
    // `mov eax,[eax+0x474]; call eax` (AimDebug weaponFire=0 before; tau
    // shots started at the head on stable too).
    // RVAs: Offsets::kServerWeaponFireOriginReturnRvas.
}

Vector* __fastcall Hooks::dServerBmShootSource(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    const uint32_t callerRva = ServerRva(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    bool weaponFire = false;
    if (bmvr::g_VrShotOriginFix)
    {
        for (uint32_t rva : Offsets::kServerWeaponFireOriginReturnRvas)
            weaponFire = weaponFire || (rva != 0 && callerRva == rva);
    }
    if (bmvr::g_AimDebug && m_VR
        && (m_VR->m_HeldButtons.load(std::memory_order_acquire) & (IN_ATTACK | IN_ATTACK2)) != 0)
    {
        static int s_callerLog;
        if (s_callerLog < 80)
        {
            ++s_callerLog;
            Game::logMsg("AimDebug +0x474 origin caller rva=0x%X weaponFire=%d", callerRva, weaponFire ? 1 : 0);
        }
    }
    if (weaponFire)
        return RewriteShootOrigin(ecx, out, hkServerBmShootSource.fOriginal, false, true);
    if (hkServerBmShootSource.fOriginal)
        return hkServerBmShootSource.fOriginal(ecx, out);
    return out;
}

// Free-hand Use (VR::UpdateUseGrab / UpdateFreeHandUseTarget), on the
// server tick after the game's own Use: the same AcceptInput("Use") the
// game's PlayerUse sends, to the entity the free hand points at. Continuous
// entities (chargers) every tick while held, impulse ones (doors, buttons)
// once per press, on/off ones on at the press and off at the release.
void __fastcall Hooks::dPlayerUse(void* ecx, void* edx)
{
    (void)edx;
    if (hkPlayerUse.fOriginal)
        hkPlayerUse.fOriginal(ecx);
    if (!m_VR || !m_Game || !ecx)
        return;
    struct Variant { uint32_t v[3]; uint32_t ehandle; int fieldType; };
    using tObjectCaps = int(__thiscall*)(void*);
    using tAcceptInput = bool(__thiscall*)(void*, const char*, void*, void*, Variant, int);
    auto serverEntity = [](int index) { return m_Game->ServerEntityByIndex(index); };
    auto vfn = [](void* ent, int off) { return *reinterpret_cast<void**>(*reinterpret_cast<char**>(ent) + off); };
    auto use = [&](void* ent, int useType) {
        const Variant empty{ { 0, 0, 0 }, 0xFFFFFFFFu, 0 };
        reinterpret_cast<tAcceptInput>(vfn(ent, Offsets::kServerEntity_AcceptInputVtOff))(ent, "Use", ecx, ecx, empty, useType);
    };
    constexpr int kCapImpulse = 0x10, kCapContinuous = 0x20, kCapOnOff = 0x40;
    static uint32_t s_doneSeq = 0;
    static int s_onOffIndex = 0;
    static int s_logs = 0;
    const int index = m_VR->m_FreeUseHeld.load(std::memory_order_acquire)
        ? m_VR->m_FreeUseEntIndex.load(std::memory_order_acquire) : 0;
    const uint32_t seq = m_VR->m_FreeUsePressSeq.load(std::memory_order_acquire);
    BMVR_TRY
    {
        if (s_onOffIndex && s_onOffIndex != index)
        {
            if (void* ent = serverEntity(s_onOffIndex))
                use(ent, 0);
            s_onOffIndex = 0;
        }
        void* ent = serverEntity(index);
        if (ent && ent != ecx)
        {
            const int caps = reinterpret_cast<tObjectCaps>(vfn(ent, Offsets::kServerEntity_ObjectCapsVtOff))(ent);
            if (caps & kCapContinuous)
                use(ent, 3);
            else if ((caps & (kCapImpulse | kCapOnOff)) && seq != s_doneSeq)
            {
                use(ent, (caps & kCapOnOff) ? 1 : 3);
                if (caps & kCapOnOff)
                    s_onOffIndex = index;
            }
            if ((caps & (kCapImpulse | kCapContinuous | kCapOnOff)) && seq != s_doneSeq)
            {
                s_doneSeq = seq;
                if (s_logs++ < 30)
                    Game::logMsg("Free-hand Use: entity %d caps 0x%X -> %s", index, caps,
                        (caps & kCapContinuous) ? "continuous" : (caps & kCapOnOff) ? "on" : "toggle");
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (s_logs++ < 30)
            Game::logMsg("Free-hand Use: exception (entity %d)", index);
    }
}

void __fastcall Hooks::dGrabSetTarget(void* ecx, void* edx, Vector* pos, QAngle* ang)
{
    (void)edx;
    if (m_VR)
        m_VR->NoteGrabControllerActive();
    Vector p{};
    QAngle a{};
    if (pos)
        p = *pos;
    if (ang)
        a = *ang;
    if (m_VR && m_VR->UseGrabActive())
    {
        Vector hand{};
        if (m_VR->TryGetVrGrabTarget(hand))
        {
            static int s_tgtLog;
            if (s_tgtLog < 8)
            {
                Game::logMsg("Grab SetTarget palm (%.1f,%.1f,%.1f) was (%.1f,%.1f,%.1f)",
                    hand.x, hand.y, hand.z, p.x, p.y, p.z);
                ++s_tgtLog;
            }
            p = hand;
        }
    }
    if (hkGrabSetTarget.fOriginal)
        hkGrabSetTarget.fOriginal(ecx, pos ? &p : pos, ang ? &a : ang);
}

namespace
{
    // C_BaseAnimating::GetAttachment(int, Vector&, QAngle&) is built on the
    // matrix overload, which is hooked too. Scale in the outer call only, or
    // the point is pulled toward the controller twice (scale^2: ~6 cm short
    // of the muzzle at ViewmodelScale 0.68).
    thread_local int t_InAttachmentVec = 0;
}

// Source moves viewmodel attachments (muzzle, shell eject, ...) from the
// viewmodel's own projection into the world's: the offset from the player
// view axis is scaled by tan(fov/2)/tan(fovViewmodel/2). In VR the gun is a
// world-space mesh drawn with the eye's projection, so that correction only
// pulls muzzle flashes and other attachment effects toward the engine eye's
// axis: off the barrel, more the further the gun is from that axis (gun held
// to the side, playing seated). Leave the point where the bones put it.
void __cdecl Hooks::dFormatViewModelAttachment(Vector* origin, bool inverse)
{
    BMVR_PROF("hook FormatViewModelAttachment");
    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && EngineInGame())
        return;
    if (hkFormatViewModelAttachment.fOriginal)
        hkFormatViewModelAttachment.fOriginal(origin, inverse);
}

int __fastcall Hooks::dGetAttachmentVec(void* ecx, void* edx, int number, Vector* origin, QAngle* angles)
{
    BMVR_PROF("hook GetAttachmentVec");
    (void)edx;
    int result = 0;
    ++t_InAttachmentVec;
    if (hkGetAttachmentVec.fOriginal)
        result = hkGetAttachmentVec.fOriginal(ecx, number, origin, angles);
    --t_InAttachmentVec;
    if (result && origin && m_VR)
        m_VR->ScaleViewmodelRenderableAttachment(ecx, *origin);
    return result;
}

int __fastcall Hooks::dGetAttachmentMatrix(void* ecx, void* edx, int number, float* matrix)
{
    BMVR_PROF("hook GetAttachmentMatrix");
    (void)edx;
    int result = 0;
    if (hkGetAttachmentMatrix.fOriginal)
        result = hkGetAttachmentMatrix.fOriginal(ecx, number, matrix);
    if (result && matrix && m_VR && t_InAttachmentVec == 0)
    {
        Vector origin(matrix[3], matrix[7], matrix[11]);
        if (m_VR->ScaleViewmodelRenderableAttachment(ecx, origin))
        {
            matrix[3] = origin.x;
            matrix[7] = origin.y;
            matrix[11] = origin.z;
        }
    }
    return result;
}

bool Hooks::EnsureWeaponVfxHooks()
{
    static bool s_done = false;
    if (s_done)
        return true;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!hkTauBeamView.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCTauBeam_ViewMuzzle;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC83EC60807D14008B550856578B"))
        {
            if (hkTauBeamView.createHook(p, &dTauBeamView) == 0 && hkTauBeamView.enableHook() == 0)
                Game::logMsg("Hook enabled: TAU Fire02 beam rva=0x%X", Offsets::kCTauBeam_ViewMuzzle);
            else
                Game::logMsg("TAU Fire02 beam hook failed");
        }
        else
        {
            Game::logMsg("TAU Fire02 beam skipped (bytes/rva 0x%X)", Offsets::kCTauBeam_ViewMuzzle);
            Game::noteMissingOffset("TAU Fire02 beam");
            hkTauBeamView.pTarget = p;
        }
    }
    if (!hkTauBeamWorld.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCTauBeam_WorldBeam;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC83EC788B45080F57C956578B7D"))
        {
            if (hkTauBeamWorld.createHook(p, &dTauBeamWorld) == 0 && hkTauBeamWorld.enableHook() == 0)
                Game::logMsg("Hook enabled: TAU impact glow rva=0x%X", Offsets::kCTauBeam_WorldBeam);
            else
                Game::logMsg("TAU impact glow hook failed");
        }
        else
        {
            Game::logMsg("TAU impact glow skipped (bytes/rva 0x%X)", Offsets::kCTauBeam_WorldBeam);
            Game::noteMissingOffset("TAU impact glow");
            hkTauBeamWorld.pTarget = p;
        }
    }
    if (!hkTauBeamFireTrace.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCTauBeam_FireTrace;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "538BDC83EC0883E4F083C404558B6B04896C24048BEC81ECEC000000"))
        {
            if (hkTauBeamFireTrace.createHook(p, &dTauBeamFireTrace) == 0 && hkTauBeamFireTrace.enableHook() == 0)
                Game::logMsg("Hook enabled: TAU fire trace rva=0x%X", Offsets::kCTauBeam_FireTrace);
            else
                Game::logMsg("TAU fire trace hook failed");
        }
        else
        {
            Game::logMsg("TAU fire trace skipped (bytes/rva 0x%X)", Offsets::kCTauBeam_FireTrace);
            Game::noteMissingOffset("TAU fire trace");
            hkTauBeamFireTrace.pTarget = p;
        }
    }
    if (!hkGluonImpactTrace.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCWeaponGluon_ImpactTrace;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "538BDC83EC0883E4F083C404558B6B04896C24048BEC81ECD8000000"))
        {
            if (hkGluonImpactTrace.createHook(p, &dGluonImpactTrace) == 0 && hkGluonImpactTrace.enableHook() == 0)
                Game::logMsg("Hook enabled: gluon impact trace rva=0x%X", Offsets::kCWeaponGluon_ImpactTrace);
            else
                Game::logMsg("gluon impact trace hook failed");
        }
        else
        {
            Game::logMsg("gluon impact trace skipped (bytes/rva 0x%X)", Offsets::kCWeaponGluon_ImpactTrace);
            Game::noteMissingOffset("gluon impact trace");
            hkGluonImpactTrace.pTarget = p;
        }
    }
    if (!hkGluonBeamUpdate.pTarget)
    {
        constexpr const char* kBytes =
            "558BEC81ECD0000000A1????????33C58945FC56578BF9";
        auto* p = ResolveCode(client, Offsets::kCWeaponGluon_BeamUpdate, kBytes);
        if (p)
        {
            if (hkGluonBeamUpdate.createHook(p, &dGluonBeamUpdate) == 0
                && hkGluonBeamUpdate.enableHook() == 0)
                Game::logMsg("Hook enabled: gluon beam update rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("gluon beam update hook failed");
        }
        else
        {
            Game::logMsg("gluon beam update skipped (bytes/rva 0x%X)", Offsets::kCWeaponGluon_BeamUpdate);
            Game::noteMissingOffset("gluon beam update");
            hkGluonBeamUpdate.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCWeaponGluon_BeamUpdate;
        }
    }
    if (!hkGluonBeamFxSet.pTarget)
    {
        constexpr const char* kBytes = "558BEC8B450C568BF1C6460C01";
        auto* p = ResolveCode(client, Offsets::kCGluonBeamFx_SetBeam, kBytes);
        if (p)
        {
            if (hkGluonBeamFxSet.createHook(p, &dGluonBeamFxSet) == 0
                && hkGluonBeamFxSet.enableHook() == 0)
                Game::logMsg("Hook enabled: gluon beam set rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("gluon beam set hook failed");
        }
        else
        {
            Game::logMsg("gluon beam set skipped (bytes/rva 0x%X)", Offsets::kCGluonBeamFx_SetBeam);
            Game::noteMissingOffset("gluon beam set");
            hkGluonBeamFxSet.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCGluonBeamFx_SetBeam;
        }
    }
    if (!hkGluonBeamFxDraw.pTarget)
    {
        constexpr const char* kBytes =
            "538BDC83EC0883E4F083C404558B6B04896C24048BEC81EC68050000";
        auto* p = ResolveCode(client, Offsets::kCGluonBeamFx_Draw, kBytes);
        if (p)
        {
            if (hkGluonBeamFxDraw.createHook(p, &dGluonBeamFxDraw) == 0
                && hkGluonBeamFxDraw.enableHook() == 0)
                Game::logMsg("Hook enabled: gluon beam draw rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("gluon beam draw hook failed");
        }
        else
        {
            Game::logMsg("gluon beam draw skipped (bytes/rva 0x%X)", Offsets::kCGluonBeamFx_Draw);
            Game::noteMissingOffset("gluon beam draw");
            hkGluonBeamFxDraw.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCGluonBeamFx_Draw;
        }
    }
    if (!hkParticleSetControlPoint.pTarget)
    {
        constexpr const char* kBytes = "558BEC538B5D0C578BF983BF541B0000FF";
        auto* p = ResolveCode(client, Offsets::kParticleSetControlPoint, kBytes);
        if (p)
        {
            if (hkParticleSetControlPoint.createHook(p, &dParticleSetControlPoint) == 0
                && hkParticleSetControlPoint.enableHook() == 0)
                Game::logMsg("Hook enabled: particle SetControlPoint rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("particle SetControlPoint hook failed");
        }
        else
        {
            Game::logMsg("particle SetControlPoint skipped (bytes/rva 0x%X)",
                Offsets::kParticleSetControlPoint);
            Game::noteMissingOffset("particle SetControlPoint");
            hkParticleSetControlPoint.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kParticleSetControlPoint;
        }
    }
    if (!hkParticleMgrAddEffect.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kParticleMgr_AddEffect;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC8B414C53568B7508"))
        {
            if (hkParticleMgrAddEffect.createHook(p, &dParticleMgrAddEffect) == 0
                && hkParticleMgrAddEffect.enableHook() == 0)
                Game::logMsg("Hook enabled: particle AddEffect rva=0x%X",
                    Offsets::kParticleMgr_AddEffect);
            else
                Game::logMsg("particle AddEffect hook failed");
        }
        else
        {
            Game::logMsg("particle AddEffect skipped (bytes/rva 0x%X)",
                Offsets::kParticleMgr_AddEffect);
            Game::noteMissingOffset("particle AddEffect");
            hkParticleMgrAddEffect.pTarget = p;
        }
    }
    if (!hkRpgUpdateLaser.pTarget)
    {
        constexpr const char* kRpgLaserBytes =
            "558BEC83EC44A1????????33C58945FC8BC1568945D0";
        auto* p = ResolveCode(client, Offsets::kCWeaponRpg_UpdateLaser, kRpgLaserBytes);
        if (p)
        {
            if (hkRpgUpdateLaser.createHook(p, &dRpgUpdateLaser) == 0 && hkRpgUpdateLaser.enableHook() == 0)
                Game::logMsg("Hook enabled: RPG laser rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("RPG laser hook failed");
        }
        else
        {
            Game::logMsg("RPG laser skipped (bytes/rva 0x%X)", Offsets::kCWeaponRpg_UpdateLaser);
            Game::noteMissingOffset("RPG laser");
            hkRpgUpdateLaser.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCWeaponRpg_UpdateLaser;
        }
    }
    if (!hkHudCrosshairPaint.pTarget)
    {
        constexpr const char* kHudCrosshairBytes =
            "558BEC5153568B35????????8BD9578BCE8B06FF5070";
        auto* p = ResolveCode(client, Offsets::kCHudCrosshair_Paint, kHudCrosshairBytes);
        if (p)
        {
            if (hkHudCrosshairPaint.createHook(p, &dHudCrosshairPaint) == 0
                && hkHudCrosshairPaint.enableHook() == 0)
                Game::logMsg("Hook enabled: HUD crosshair rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("HUD crosshair hook failed");
        }
        else
        {
            Game::logMsg("HUD crosshair skipped (bytes/rva 0x%X)", Offsets::kCHudCrosshair_Paint);
            Game::noteMissingOffset("HUD crosshair");
            hkHudCrosshairPaint.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCHudCrosshair_Paint;
        }
    }
    if (!hkSpriteRendererDraw.pTarget)
    {
        constexpr const char* kDrawSpriteBytes =
            "558BEC83EC48894DF88B0D????????FF15????????8845FF";
        auto* p = ResolveCode(client, Offsets::kCSpriteRenderer_DrawSprite, kDrawSpriteBytes);
        if (p)
        {
            if (hkSpriteRendererDraw.createHook(p, &dSpriteRendererDraw) == 0
                && hkSpriteRendererDraw.enableHook() == 0)
                Game::logMsg("Hook enabled: DrawSprite rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("DrawSprite hook failed");
        }
        else
        {
            Game::logMsg("DrawSprite skipped (bytes/rva 0x%X)", Offsets::kCSpriteRenderer_DrawSprite);
            Game::noteMissingOffset("DrawSprite");
            hkSpriteRendererDraw.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCSpriteRenderer_DrawSprite;
        }
    }
    if (!hkViewRenderBeamsDraw.pTarget)
    {
        constexpr const char* kDrawBeamBytes =
            "558BEC83EC2CA1????????33C58945FC894DE88B0D????????568B7508";
        auto* p = ResolveCode(client, Offsets::kCViewRenderBeams_DrawBeam, kDrawBeamBytes);
        if (p)
        {
            if (hkViewRenderBeamsDraw.createHook(p, &dViewRenderBeamsDraw) == 0
                && hkViewRenderBeamsDraw.enableHook() == 0)
                Game::logMsg("Hook enabled: DrawBeam rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("DrawBeam hook failed");
        }
        else
        {
            Game::logMsg("DrawBeam skipped (bytes/rva 0x%X)", Offsets::kCViewRenderBeams_DrawBeam);
            Game::noteMissingOffset("DrawBeam");
            hkViewRenderBeamsDraw.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCViewRenderBeams_DrawBeam;
        }
    }
    if (!hkSpriteRenderableDraw.pTarget)
    {
        constexpr const char* kSprBytes =
            "558BEC83EC64538B5D088BCB568B03FF5004";
        auto* p = ResolveCode(client, Offsets::kCSpriteRenderable_Draw, kSprBytes);
        if (p)
        {
            if (hkSpriteRenderableDraw.createHook(p, &dSpriteRenderableDraw) == 0
                && hkSpriteRenderableDraw.enableHook() == 0)
                Game::logMsg("Hook enabled: SpriteRenderableDraw rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("SpriteRenderableDraw hook failed");
        }
        else
        {
            Game::logMsg("SpriteRenderableDraw skipped (bytes/rva 0x%X)",
                Offsets::kCSpriteRenderable_Draw);
            Game::noteMissingOffset("SpriteRenderableDraw");
            hkSpriteRenderableDraw.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCSpriteRenderable_Draw;
        }
    }
    if (!hkEnvLaserDotDraw.pTarget)
    {
        constexpr const char* kDotBytes =
            "558BEC81ECA0000000A1????????33C58945FCF745080000004E";
        auto* p = ResolveCode(client, Offsets::kCEnvLaserDot_Draw, kDotBytes);
        if (p)
        {
            if (hkEnvLaserDotDraw.createHook(p, &dEnvLaserDotDraw) == 0
                && hkEnvLaserDotDraw.enableHook() == 0)
                Game::logMsg("Hook enabled: EnvLaserDot draw rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("EnvLaserDot draw hook failed");
        }
        else
        {
            Game::logMsg("EnvLaserDot draw skipped (bytes/rva 0x%X)", Offsets::kCEnvLaserDot_Draw);
            Game::noteMissingOffset("EnvLaserDot draw");
            hkEnvLaserDotDraw.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCEnvLaserDot_Draw;
        }
    }
    if (!hkSpriteQuad.pTarget)
    {
        constexpr const char* kQuadBytes =
            "538BDC83EC0883E4F083C404558B6B04896C24048BEC81EC58020000";
        auto* p = ResolveCode(client, Offsets::kSpriteQuad, kQuadBytes);
        if (p)
        {
            if (hkSpriteQuad.createHook(p, &dSpriteQuad) == 0
                && hkSpriteQuad.enableHook() == 0)
                Game::logMsg("Hook enabled: sprite quad rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("sprite quad hook failed");
        }
        else
        {
            Game::logMsg("sprite quad skipped (bytes/rva 0x%X)", Offsets::kSpriteQuad);
            Game::noteMissingOffset("sprite quad");
            hkSpriteQuad.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kSpriteQuad;
        }
    }
    if (!hkSpriteAxes.pTarget)
    {
        constexpr const char* kAxesBytes =
            "538BDC83EC0883E4F083C404558B6B04896C24048BEC83EC388B4B10F30F100D";
        auto* p = ResolveCode(client, Offsets::kGetSpriteAxes, kAxesBytes);
        if (p)
        {
            g_ClientViewOrigin = reinterpret_cast<const float*>(
                reinterpret_cast<unsigned char*>(client) + Offsets::kCurrentViewOriginGlobal);
            if (hkSpriteAxes.createHook(p, &dSpriteAxes) == 0 && hkSpriteAxes.enableHook() == 0)
                Game::logMsg("Hook enabled: sprite axes rva=0x%X",
                    static_cast<unsigned>(p - reinterpret_cast<unsigned char*>(client)));
            else
                Game::logMsg("sprite axes hook failed");
        }
        else
        {
            Game::logMsg("sprite axes skipped (bytes/rva 0x%X)", Offsets::kGetSpriteAxes);
            Game::noteMissingOffset("sprite axes");
            hkSpriteAxes.pTarget = reinterpret_cast<unsigned char*>(client) + Offsets::kGetSpriteAxes;
        }
    }
    s_done = hkTauBeamView.pTarget && hkTauBeamWorld.pTarget && hkTauBeamFireTrace.pTarget
        && hkGluonImpactTrace.pTarget && hkGluonBeamUpdate.pTarget && hkGluonBeamFxSet.pTarget
        && hkGluonBeamFxDraw.pTarget && hkParticleSetControlPoint.pTarget
        && hkParticleMgrAddEffect.pTarget && hkRpgUpdateLaser.pTarget
        && hkHudCrosshairPaint.pTarget && hkSpriteRendererDraw.pTarget
        && hkViewRenderBeamsDraw.pTarget && hkSpriteRenderableDraw.pTarget
        && hkEnvLaserDotDraw.pTarget && hkSpriteQuad.pTarget && hkSpriteAxes.pTarget;
    return s_done;
}

void __fastcall Hooks::dTauBeamView(void* ecx, void* edx, Vector* startEnd, void* a2, float a3, int a4, int a5)
{
    BMVR_PROF("hook TauBeamView");
    (void)edx;
    Vector local[2]{};
    Vector* seg = startEnd;
    if (startEnd && m_VR && m_VR->TryGetVrBeamSegment(local[0], local[1]))
        seg = local;
    if (hkTauBeamView.fOriginal)
        hkTauBeamView.fOriginal(ecx, seg, a2, a3, a4, a5);
}

void __fastcall Hooks::dTauBeamWorld(void* ecx, void* edx, Vector* impact, Vector* normal, float width)
{
    (void)edx;
    // FUN_102346A0 is the impact glow: ProgressBeam passes (endpos, plane.normal).
    // Treating those as (muzzle, wall) spawned tau_beam_glow on the viewmodel.
    Vector start{};
    Vector end{};
    Vector n{};
    if (impact && m_VR && m_VR->TryGetVrBeamSegment(start, end, &n))
    {
        Vector* nPtr = (n.LengthSqr() > 0.01f) ? &n : normal;
        if (hkTauBeamWorld.fOriginal)
            hkTauBeamWorld.fOriginal(ecx, &end, nPtr, width);
        return;
    }
    if (hkTauBeamWorld.fOriginal)
        hkTauBeamWorld.fOriginal(ecx, impact, normal, width);
}

void __fastcall Hooks::dTauBeamFireTrace(void* ecx, void* edx)
{
    (void)edx;
    if (ecx && m_VR && m_VR->m_IsVREnabled && m_VR->m_ControllerPoseValid
        && m_VR->IsGameplayEligible())
    {
        Vector start{};
        Vector end{};
        Vector n{};
        if (m_VR->TryGetVrBeamSegment(start, end, &n))
        {
            Vector dir = end - start;
            if (VectorNormalize(dir) > 0.01f)
            {
                *reinterpret_cast<Vector*>(reinterpret_cast<char*>(ecx) + 0xC) = start;
                *reinterpret_cast<Vector*>(reinterpret_cast<char*>(ecx) + 0x18) = dir;
            }
        }
    }
    if (hkTauBeamFireTrace.fOriginal)
        hkTauBeamFireTrace.fOriginal(ecx);
}

void __fastcall Hooks::dGluonImpactTrace(void* ecx, void* edx, void* player, CGameTrace* trace)
{
    (void)edx;
    static int s_enter;
    if (s_enter < 8)
    {
        Game::logMsg("gluon impact enter trace=%p vr=%d",
            trace, (m_VR && m_VR->m_IsVREnabled) ? 1 : 0);
        ++s_enter;
    }
    ++g_GluonFx;
    g_GluonFxUntilMs = GetTickCount() + 250;
    if (hkGluonImpactTrace.fOriginal)
        hkGluonImpactTrace.fOriginal(ecx, player, trace);
    Vector start{};
    Vector end{};
    Vector n{};
    if (trace && m_VR && m_VR->m_IsVREnabled && m_VR->TryGetVrBeamSegment(start, end, &n))
    {
        static int s_gluonTr;
        if (s_gluonTr < 8)
        {
            Game::logMsg("gluon impact was=(%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f) aimOff=%.1fdeg",
                trace->endpos.x, trace->endpos.y, trace->endpos.z,
                end.x, end.y, end.z, AimOffLookDegrees());
            ++s_gluonTr;
        }
        ApplyVrTraceHit(trace, start, end, n);
        float* raw = reinterpret_cast<float*>(trace);
        raw[3] = end.x;
        raw[4] = end.y;
        raw[5] = end.z;
        raw[0] = start.x;
        raw[1] = start.y;
        raw[2] = start.z;
    }
    --g_GluonFx;
    if (g_GluonFx < 0)
        g_GluonFx = 0;
}

void __fastcall Hooks::dGluonBeamUpdate(void* ecx, void* edx)
{
    BMVR_PROF("hook GluonBeamUpdate");
    (void)edx;
    NoteGluonFx();
    PatchGluonImpactParticleDefs();
    if (hkGluonBeamUpdate.fOriginal)
        hkGluonBeamUpdate.fOriginal(ecx);
    EndGluonFx();
}

void __fastcall Hooks::dGluonBeamFxSet(void* ecx, void* edx, void* viewmodel, Vector* start, Vector* mid, Vector* end)
{
    (void)edx;
    if ((g_GluonFx > 0 || GluonFxLive()) && RewriteToVrBeamEnd(end))
    {
        static int s_set;
        if (s_set < 8)
        {
            Game::logMsg("gluon SetBeam end=(%.0f,%.0f,%.0f) aimOff=%.1fdeg",
                end->x, end->y, end->z, AimOffLookDegrees());
            ++s_set;
        }
    }
    if (hkGluonBeamFxSet.fOriginal)
        hkGluonBeamFxSet.fOriginal(ecx, viewmodel, start, mid, end);
}

int __fastcall Hooks::dGluonBeamFxDraw(void* ecx, void* edx, unsigned flags)
{
    (void)edx;
    if (ecx && m_VR && m_VR->m_IsVREnabled && GluonFxLive())
    {
        auto* beamEnd = reinterpret_cast<Vector*>(static_cast<char*>(ecx) + 0x24);
        if (RewriteToVrBeamEnd(beamEnd))
        {
            static int s_draw;
            if (s_draw < 8)
            {
                Game::logMsg("gluon BeamFx draw end=(%.0f,%.0f,%.0f) aimOff=%.1fdeg",
                    beamEnd->x, beamEnd->y, beamEnd->z, AimOffLookDegrees());
                ++s_draw;
            }
        }
    }
    if (!hkGluonBeamFxDraw.fOriginal)
        return 0;
    return hkGluonBeamFxDraw.fOriginal(ecx, flags);
}

void __fastcall Hooks::dParticleSetControlPoint(void* ecx, void* edx, int index, Vector* origin)
{
    BMVR_PROF("hook ParticleSetControlPoint");
    (void)edx;
    const bool gluonImpact = IsGluonImpactParticleName(ParticleDefName(ParticleEffectDef(ecx)));
    if (gluonImpact)
    {
        ClearParticleViewModelEffect(ParticleEffectDef(ecx));
        ClearEffectViewModelFlag(ecx);
    }
    if ((g_GluonFx > 0 || gluonImpact) && index == 0 && RewriteToVrBeamEnd(origin))
    {
        static int s_cp;
        if (s_cp < 8)
        {
            Game::logMsg("gluon particle CP0 -> (%.0f,%.0f,%.0f) aimOff=%.1fdeg vm=%s",
                origin->x, origin->y, origin->z, AimOffLookDegrees(),
                gluonImpact ? "world" : "?");
            ++s_cp;
        }
    }
    // Viewmodel effects (muzzle flashes) take their control point from the
    // gun's attachment while particles simulate, before this frame's view is
    // set up: the gun then still sits at last frame's view origin, which
    // trails the player by 3-5 units while moving. The gun is drawn from the
    // fresh origin, so the flash lagged behind it when strafing (user video
    // 2026-11-11). Shift the point by the player's move since then.
    Vector shifted{};
    if (origin && bmvr::g_ViewmodelFxCatchUp && m_VR && m_VR->m_IsVREnabled && m_VR->m_StereoEye == 0
        && !m_VR->m_HasStereoBodyOrigin && EffectIsViewModel(ecx) && hkClientWeaponShootPosition.fOriginal
        && m_Game && m_Game->m_EngineClient && m_VR->m_SetupOrigin.LengthSqr() > 1.f)
    {
        void* player = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
        Vector eyeNow{};
        if (player && hkClientWeaponShootPosition.fOriginal(player, &eyeNow))
        {
            const Vector delta = eyeNow - m_VR->m_SetupOrigin;
            if (delta.LengthSqr() > 1e-4f && delta.LengthSqr() < 32.f * 32.f)
            {
                shifted = *origin + delta;
                origin = &shifted;
                static int s_log;
                if (s_log < 6)
                {
                    ++s_log;
                    Game::logMsg("viewmodel fx catch-up: %s cp%d moved (%.2f,%.2f,%.2f)",
                        ParticleDefName(ParticleEffectDef(ecx)), index, delta.x, delta.y, delta.z);
                }
            }
        }
    }
    if (hkParticleSetControlPoint.fOriginal)
        hkParticleSetControlPoint.fOriginal(ecx, index, origin);
}

void __fastcall Hooks::dParticleMgrAddEffect(void* ecx, void* edx, void* effect)
{
    BMVR_PROF("hook ParticleMgrAddEffect");
    (void)edx;
    if (m_VR && m_VR->m_IsVREnabled && effect)
    {
        void* def = ParticleEffectDef(effect);
        if (IsGluonImpactParticleName(ParticleDefName(def)))
        {
            if (ClearParticleViewModelEffect(def))
            {
                static int s_add;
                if (s_add < 4)
                {
                    Game::logMsg("gluon AddEffect world-pass %s", ParticleDefName(def));
                    ++s_add;
                }
            }
            ClearEffectViewModelFlag(effect);
        }
    }
    if (hkParticleMgrAddEffect.fOriginal)
        hkParticleMgrAddEffect.fOriginal(ecx, effect);
}

void __fastcall Hooks::dRpgUpdateLaser(void* ecx, void* edx)
{
    (void)edx;
    // CHudCrosshair::Paint calls this at vtable +0x5C4. The body is screen-space
    // ISurface at ScreenWidth/2, so in VR it is glued to the look centre. Skip
    // whenever the VR DLL is running — eligibility gates here left the original
    // drawing, which is the mark the user still saw.
    unsigned char laserOn = 0;
    const int onOff = m_Game ? m_Game->RpgLaserOnOffset() : Offsets::kCWeaponRpg_bLaserOn;
    if (ecx)
    {
        BMVR_TRY
        {
            laserOn = *reinterpret_cast<unsigned char*>(static_cast<char*>(ecx) + onOff);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            laserOn = 0;
        }
    }
    static int s_entryLog;
    if (s_entryLog < 16)
    {
        Game::logMsg("UpdateLaser vr=%d on=%u latch=%d rpgHeld=%d",
            (m_VR && m_VR->m_IsVREnabled) ? 1 : 0, laserOn,
            (m_VR && m_VR->RpgLaserLatched()) ? 1 : 0,
            RpgWeaponHeld() ? 1 : 0);
        ++s_entryLog;
    }
    if (!(ecx && m_VR && m_VR->m_IsVREnabled))
    {
        if (hkRpgUpdateLaser.fOriginal)
            hkRpgUpdateLaser.fOriginal(ecx);
        return;
    }

    const bool wantLaser = laserOn != 0 || m_VR->RpgLaserLatched();
    m_VR->SetRpgLaserActive(wantLaser);
    if (!wantLaser)
        return;

    Vector start{};
    Vector end{};
    if (m_VR->TryGetVrBeamSegment(start, end))
    {
        Vector dir = end - start;
        if (VectorNormalize(dir) > 0.01f)
            end = end - dir * 1.5f;
        m_VR->NoteRpgLaserWorld(end);
        C_BaseEntity* dot = FindRpgLaserDot();
        if (dot)
        {
            ClearEntityNodraw(dot);
            const float origin3[3] = { end.x, end.y, end.z };
            const float angles3[3] = { 0.f, 0.f, 0.f };
            CallSetAbsOriginAngles(dot, origin3, angles3);
        }
        static int s_rpgLog;
        if (s_rpgLog < 8)
        {
            Game::logMsg("RPG laser skip HUD end=(%.1f,%.1f,%.1f) on=%u dot=%d",
                end.x, end.y, end.z, laserOn, dot ? 1 : 0);
            ++s_rpgLog;
        }
    }
}

void __fastcall Hooks::dHudCrosshairPaint(void* ecx, void* edx)
{
    BMVR_PROF("hook HudCrosshairPaint");
    (void)edx;
    const bool skip = m_VR && m_VR->m_IsVREnabled && RpgWeaponHeld();
    static int s_paint;
    if (s_paint < 8)
    {
        Game::logMsg("HUD crosshair paint skip=%d rpg=%d laser=%d vm=%s",
            skip ? 1 : 0, RpgWeaponHeld() ? 1 : 0,
            (m_VR && m_VR->RpgLaserActive()) ? 1 : 0,
            (m_VR && !m_VR->m_LastViewmodelModel.empty())
                ? m_VR->m_LastViewmodelModel.c_str() : "-");
        ++s_paint;
    }
    if (skip)
        return;
    if (hkHudCrosshairPaint.fOriginal)
        hkHudCrosshairPaint.fOriginal(ecx);
}

int __fastcall Hooks::dSpriteRendererDraw(void* ecx, void* edx, void* entity, void* model,
    float* origin, float* angles, float scale, void* attach, int a7, int a8, int a9,
    unsigned a10, unsigned a11, unsigned a12, unsigned a13, float frame, int a15)
{
    BMVR_PROF("hook SpriteRendererDraw");
    (void)edx;
    const bool rpgLaser = hkSpriteRendererDraw.fOriginal && m_VR && m_VR->m_IsVREnabled
        && m_Game && m_Game->m_ModelInfo && m_VR->RpgLaserActive();
    const bool gluonHeld = hkSpriteRendererDraw.fOriginal && m_VR && m_VR->m_IsVREnabled
        && m_Game && m_Game->m_ModelInfo && origin && GluonWeaponHeld();
    if (rpgLaser || gluonHeld)
    {
        // Model / class names only matter for the laser and gluon paths;
        // plain sprites skip both engine lookups.
        const char* name = SafeModelName(m_Game->m_ModelInfo, model);
        const char* cls = rpgLaser
            ? m_Game->GetEntityClientClassName(static_cast<C_BaseEntity*>(entity)) : nullptr;
        if (rpgLaser)
        {
            const bool look = SpriteOriginOnLookRay(origin);
            const bool nearEye = SpriteOriginNearEye(origin);
            static int s_sprNameLog;
            if (s_sprNameLog < 24 && (look || nearEye || EntityLooksLikeLaserDot(static_cast<C_BaseEntity*>(entity))
                    || LaserDotMaterialName(name) || RpgLaserWorldSpriteMaterial(name)))
            {
                Game::logMsg("DrawSprite model=%s class=%s look=%d near=%d attach=%d hide=%d at=(%.0f,%.0f,%.0f)",
                    name ? name : "?", cls ? cls : "?", look ? 1 : 0, nearEye ? 1 : 0,
                    attach ? 1 : 0,
                    RpgLaserSpriteCandidate(static_cast<C_BaseEntity*>(entity), name, origin) ? 1 : 0,
                    origin ? origin[0] : 0.f, origin ? origin[1] : 0.f, origin ? origin[2] : 0.f);
                ++s_sprNameLog;
            }
            if (RpgLaserSpriteCandidate(static_cast<C_BaseEntity*>(entity), name, origin)
                || (GlowSpriteMaterialName(name) && nearEye && !std::strstr(name ? name : "", "glow06")))
            {
                static int s_skipLog;
                if (s_skipLog < 4)
                {
                    LogRpgLaserSpriteSkip("DrawSprite", name, cls, origin);
                    ++s_skipLog;
                }
            }
        }
        if (gluonHeld && GluonGlowMaterialName(name)
            && !std::strstr(name ? name : "", "glow06"))
        {
            Vector parked{};
            if (TryParkedFxWorld(parked))
            {
                static int s_gluonSpr;
                if (s_gluonSpr < 8)
                {
                    Game::logMsg("gluon DrawSprite %s -> (%.0f,%.0f,%.0f) was (%.0f,%.0f,%.0f) aimOff=%.1fdeg",
                        name ? name : "?", parked.x, parked.y, parked.z,
                        origin[0], origin[1], origin[2], AimOffLookDegrees());
                    ++s_gluonSpr;
                }
                origin[0] = parked.x;
                origin[1] = parked.y;
                origin[2] = parked.z;
            }
        }
    }
    if (!hkSpriteRendererDraw.fOriginal)
        return 0;
    return hkSpriteRendererDraw.fOriginal(ecx, entity, model, origin, angles, scale, attach,
        a7, a8, a9, a10, a11, a12, a13, frame, a15);
}

void __fastcall Hooks::dViewRenderBeamsDraw(void* ecx, void* edx, void* beam)
{
    BMVR_PROF("hook ViewRenderBeamsDraw");
    (void)edx;
    static int s_enter;
    if (s_enter < 8 && m_VR && m_VR->RpgLaserActive())
    {
        Game::logMsg("DrawBeam enter beam=%p laser=1", beam);
        ++s_enter;
    }
    RetargetRpgLaserBeam(beam);
    if (hkViewRenderBeamsDraw.fOriginal)
        hkViewRenderBeamsDraw.fOriginal(ecx, beam);
}

int __stdcall Hooks::dSpriteRenderableDraw(void* renderable, float scale, float frame,
    int rendermode, int renderfx, unsigned char* color, float hdr, unsigned* unk)
{
    if (renderable && m_VR && m_VR->m_IsVREnabled && m_VR->RpgLaserActive()
        && m_Game && m_Game->m_ModelInfo)
    {
        void* model = nullptr;
        float* origin = nullptr;
        BMVR_TRY
        {
            void** vt = *reinterpret_cast<void***>(renderable);
            using GetVecFn = float*(__thiscall*)(void*);
            using GetModelFn = void*(__thiscall*)(void*);
            if (vt && vt[1])
                origin = GetVecFn(vt[1])(renderable);
            if (vt && vt[9])
                model = GetModelFn(vt[9])(renderable);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            origin = nullptr;
            model = nullptr;
        }
        auto* entity = reinterpret_cast<C_BaseEntity*>(static_cast<char*>(renderable) - 4);
        const char* name = SafeModelName(m_Game->m_ModelInfo, model);
        const char* cls = m_Game->GetEntityClientClassName(entity);
        static int s_alt;
        if (s_alt < 12)
        {
            Game::logMsg("SpriteRenderable model=%s class=%s hide=%d at=(%.0f,%.0f,%.0f)",
                name ? name : "?", cls ? cls : "?",
                RpgLaserSpriteCandidate(entity, name, origin) ? 1 : 0,
                origin ? origin[0] : 0.f, origin ? origin[1] : 0.f, origin ? origin[2] : 0.f);
            ++s_alt;
        }
        if (RpgLaserSpriteCandidate(entity, name, origin))
        {
            static int s_altSkip;
            if (s_altSkip < 4)
            {
                LogRpgLaserSpriteSkip("SpriteRenderable", name, cls, origin);
                ++s_altSkip;
            }
        }
    }
    if (!hkSpriteRenderableDraw.fOriginal)
        return 0;
    return hkSpriteRenderableDraw.fOriginal(renderable, scale, frame, rendermode, renderfx,
        color, hdr, unk);
}

int __fastcall Hooks::dEnvLaserDotDraw(void* ecx, void* edx, unsigned flags)
{
    BMVR_PROF("hook EnvLaserDotDraw");
    (void)edx;
    static int s_dot;
    if (s_dot < 12)
    {
        Game::logMsg("EnvLaserDot draw flags=0x%X vr=%d laser=%d",
            flags, (m_VR && m_VR->m_IsVREnabled) ? 1 : 0,
            (m_VR && m_VR->RpgLaserActive()) ? 1 : 0);
        ++s_dot;
    }
    const bool retarget = ecx && m_VR && m_VR->m_IsVREnabled
        && (flags & 0x4E000000u) == 0;
    // The local player's dot is not drawn at the entity origin: this draw
    // traces from the main view origin along the view forward, so in VR it
    // follows the head while the gun (aim marker, guidance lines) points
    // elsewhere. Moving the entity cannot change that; skip it while the
    // RPG is held. The aim marker and the guidance lines mark the gun aim.
    if (retarget && RpgWeaponHeld())
    {
        static int s_hideLog;
        if (s_hideLog < 2)
        {
            Game::logMsg("EnvLaserDot hidden in VR (view-traced dot)");
            ++s_hideLog;
        }
        return 0;
    }
    if (retarget)
    {
        m_VR->SetRpgLaserActive(true);
        g_EnvLaserDotQuad = true;
    }
    int result = 0;
    if (hkEnvLaserDotDraw.fOriginal)
        result = hkEnvLaserDotDraw.fOriginal(ecx, flags);
    g_EnvLaserDotQuad = false;
    return result;
}

// Sprites drawn "parallel to the view plane" (glows, most env_sprites) copy the
// camera's right/up, so in a headset they roll with the head and turn with it.
// In a VR eye pass they face that eye's position with up kept to world up
// instead, as the game's own upright sprites do (bmvr_sprite_level).
void __cdecl Hooks::dSpriteAxes(int type, const float* origin, const float* angles, float* fwd, float* right, float* up)
{
    hkSpriteAxes.fOriginal(type, origin, angles, fwd, right, up);
    // 0 = VP_PARALLEL_UPRIGHT (turns with head yaw), 2 = VP_PARALLEL (also
    // rolls); 2 with a roll angle is the game's own oriented case, left alone.
    if (!bmvr::g_SpriteLevel || !m_VR || m_VR->m_StereoEye == 0 || !g_ClientViewOrigin
        || !origin || !fwd || !right || !up)
        return;
    if (!(type == 0 || (type == 2 && !(angles && angles[2] != 0.f))))
        return;
    Vector f(origin[0] - g_ClientViewOrigin[0], origin[1] - g_ClientViewOrigin[1], origin[2] - g_ClientViewOrigin[2]);
    if (VectorNormalize(f) < 1.f)
        return;
    // Straight above or below: no horizon, keep the game's axes.
    Vector r(f.y, -f.x, 0.f); // forward x world up
    if (VectorNormalize(r) < 0.05f)
        return;
    const Vector u(r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x); // right x forward
    fwd[0] = f.x; fwd[1] = f.y; fwd[2] = f.z;
    right[0] = r.x; right[1] = r.y; right[2] = r.z;
    up[0] = u.x; up[1] = u.y; up[2] = u.z;
    static int s_log;
    if (s_log < 4)
    {
        ++s_log;
        Game::logMsg("sprite axes: type %d at (%.0f,%.0f,%.0f) eye %d levelled", type, origin[0], origin[1], origin[2],
            m_VR->m_StereoEye);
    }
}

void __cdecl Hooks::dSpriteQuad(float* origin, float width, float height, unsigned color)
{
    BMVR_PROF("hook SpriteQuad");
    Vector parked{};
    const bool rpg = m_VR && m_VR->m_IsVREnabled && m_VR->RpgLaserActive();
    const bool gluon = GluonFxLive();
    if (!rpg && !gluon)
    {
        // Nothing below applies without RPG laser / gluon FX; skip the two
        // pose-mutex look-angle computations per sprite quad.
        if (hkSpriteQuad.fOriginal)
            hkSpriteQuad.fOriginal(origin, width, height, color);
        return;
    }
    const float offHmd = origin ? PointOffLookDegrees(origin) : 180.f;
    const float offView = origin ? PointOffCurrentViewDegrees(origin) : 180.f;
    const bool onLook = origin && (offHmd < 4.f || offView < 4.f);
    const unsigned rgb = color & 0x00FFFFFFu;
    // Distant EnvLaserDot scales well past 80; skybox DrawModel is a nested
    // RenderView whose look-hit is not the HMD world ray.
    const bool rpgMark = rpg && onLook && width >= 16.f
        && (rgb == 0x00FFFFFFu || width >= 20.f);
    const bool gluonMark = gluon && onLook && width >= 6.f && width <= 256.f
        && rgb != 0x00FFFFFFu && !(width > 12.2f && width < 13.4f);
    bool haveParked = (rpgMark || gluonMark) && TryParkedFxWorld(parked);
    if (haveParked && gluonMark && !rpgMark && PointOffLookDegrees(&parked.x) < 5.f)
        haveParked = false;
    static int s_quad;
    if (s_quad < 16 && origin && (rpg || gluon))
    {
        Game::logMsg(
            "sprite quad at=(%.0f,%.0f,%.0f) offHmd=%.1fdeg offView=%.1fdeg aimOff=%.1fdeg nest=%d retarget=%d size=%.1f rgb=%06X rpg=%d gluon=%d",
            origin[0], origin[1], origin[2], offHmd, offView, AimOffLookDegrees(),
            g_RenderViewNest, haveParked ? 1 : 0, width, rgb, rpgMark ? 1 : 0, gluonMark ? 1 : 0);
        ++s_quad;
    }
    if (haveParked && rpgMark && NestedRenderView())
    {
        static int s_sky;
        if (s_sky < 8)
        {
            Game::logMsg("RPG laser skip nested/sky quad size=%.1f offView=%.1fdeg",
                width, offView);
            ++s_sky;
        }
        return;
    }
    if (haveParked)
    {
        static int s_move;
        if (s_move < 10)
        {
            Game::logMsg("FX quad -> (%.1f,%.1f,%.1f) was (%.1f,%.1f,%.1f) aimOff=%.1fdeg rpg=%d gluon=%d nest=%d size=%.1f",
                parked.x, parked.y, parked.z, origin[0], origin[1], origin[2],
                AimOffLookDegrees(), rpgMark ? 1 : 0, gluonMark ? 1 : 0,
                g_RenderViewNest, width);
            ++s_move;
        }
        origin[0] = parked.x;
        origin[1] = parked.y;
        origin[2] = parked.z;
    }
    if (hkSpriteQuad.fOriginal)
        hkSpriteQuad.fOriginal(origin, width, height, color);
}

namespace
{
    // Last back-buffer size the engine asked for, printed with each render target it creates. Bloom targets are
    // eye/2 in headset sessions and window/2 headless; this shows which size each creation saw, and who asked.
    struct BackBufferTrace
    {
        int origW, origH, w, h;
        void* caller;
        bool stereo;
    };
    BackBufferTrace g_LastBackBuffer{};

    struct BackBufferTraceScope
    {
        int& w;
        int& h;
        int origW;
        int origH;
        void* caller;
        ~BackBufferTraceScope()
        {
            g_LastBackBuffer = { origW, origH, w, h, caller,
                Hooks::m_VR && Hooks::m_VR->StereoWorldPassActive() };
        }
    };

    // "client.dll+0x1234" for a code address, for log lines.
    void DescribeCodeAddress(void* addr, char* out, size_t outSize)
    {
        HMODULE mod = nullptr;
        char path[MAX_PATH] = "?";
        if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(addr), &mod) && mod)
            GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        base = base ? base + 1 : path;
        sprintf_s(out, outSize, "%s+0x%X", base,
            mod ? static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)) : 0u);
    }

    void RtActualSize(ITexture* tex, int& w, int& h)
    {
        w = h = 0;
        if (!tex)
            return;
        BMVR_TRY
        {
            w = tex->GetActualWidth();
            h = tex->GetActualHeight();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            w = h = 0;
        }
    }

    // One line per render target the engine creates (first 256).
    void LogRtTrace(void* materialSystem, const char* name, int reqW, int reqH, int mode, ITexture* tex, void* caller)
    {
        static int s_count;
        if (s_count >= 256)
            return;
        ++s_count;
        int aw, ah;
        RtActualSize(tex, aw, ah);
        const BackBufferTrace bb = g_LastBackBuffer;
        char callerText[96];
        char bbCallerText[96];
        DescribeCodeAddress(caller, callerText, sizeof(callerText));
        DescribeCodeAddress(bb.caller, bbCallerText, sizeof(bbCallerText));
        int fbW = 0, fbH = 0;
        BMVR_TRY
        {
            using GetFn = void(__thiscall*)(void*, int&, int&);
            void** vt = *reinterpret_cast<void***>(materialSystem);
            reinterpret_cast<GetFn>(vt[Offsets::kIMaterialSystem_GetRTFBDimensionsVt])(materialSystem, fbW, fbH);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            fbW = fbH = 0;
        }
        Game::logMsg("RtTrace %s req=%dx%d mode=%d fb=%dx%d actual=%dx%d thread=%lu stereo=%d caller=%s lastBB=%dx%d(orig %dx%d) bbStereo=%d bbCaller=%s",
            name ? name : "null", reqW, reqH, mode, fbW, fbH, aw, ah, GetCurrentThreadId(),
            Hooks::m_VR && Hooks::m_VR->StereoWorldPassActive() ? 1 : 0, callerText,
            bb.w, bb.h, bb.origW, bb.origH, bb.stereo ? 1 : 0, bbCallerText);
    }
}

void Hooks::ApplyFxaaParams()
{
    static float* s_console = nullptr;
    static float* s_quality = nullptr;
    static bool s_resolved = false;
    if (!s_resolved)
    {
        s_resolved = true;
        if (m_Game && m_Game->m_Offsets && m_Game->m_Offsets->EnginePostProxyBind.valid)
        {
            const auto* bind = reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(m_Game->m_Offsets->EnginePostProxyBind.address));
            s_console = *reinterpret_cast<float* const*>(bind + Offsets::kEnginePostBindFxaaC);
            s_quality = *reinterpret_cast<float* const*>(bind + Offsets::kEnginePostBindFxaaQ);
            // The two blocks are neighbours. Anything else means this is not the
            // code we read, so leave the game's memory alone.
            if (s_quality != s_console + 4)
            {
                s_console = s_quality = nullptr;
                Game::noteMissingOffset("FXAA parameter blocks in the engine_post proxy");
            }
        }
        Game::logMsg("FXAA parameters %s (FxaaApply=%d)", s_console ? "available" : "NOT available on this game build",
            bmvr::g_FxaaApply ? 1 : 0);
    }
    if (!s_console || !s_quality)
        return;
    // The shader reads only this block: x sub-pixel, z edge, w dark edge. The
    // game's own filler puts the dark edge value in the console block instead,
    // which no shader register reads. Off restores the zeros the game runs with.
    const bool on = bmvr::g_FxaaApply;
    s_quality[0] = on ? bmvr::g_FxaaSubpixel : 0.f;
    s_quality[2] = on ? bmvr::g_FxaaEdgeThreshold : 0.f;
    s_quality[3] = on ? bmvr::g_FxaaEdgeThresholdMin : 0.f;
}

void __fastcall Hooks::dGetBackBufferDimensions(void* ecx, void* edx, int& width, int& height)
{
    BMVR_PROF("hook GetBackBufferDimensions");
    (void)edx;
    if (hkGetBackBufferDimensions.fOriginal)
        hkGetBackBufferDimensions.fOriginal(ecx, width, height);
    BackBufferTraceScope traceScope{ width, height, width, height, _ReturnAddress() };
    if (m_VR && m_VR->HudPaintActive())
    {
        int x = 0, y = 0, w = 0, h = 0;
        if (m_VR->ForceHudOverlayViewport(x, y, w, h))
        {
            width = w;
            height = h;
            return;
        }
    }
    if (OffscreenStereoSizeLie(width, height))
        return;
    if (McLeftoverRecording())
        return;
    uint32_t fbW = 0, fbH = 0;
    if (!bmvr::HaveHmdFramebufferSize(fbW, fbH))
        return;
    static int s_bbKeep;
    if (s_bbKeep < 8)
    {
        Game::logMsg("GetBackBufferDimensions %dx%d (HMD-fb %ux%u unused; keep window size)",
            width, height, fbW, fbH);
        ++s_bbKeep;
    }
}

void __fastcall Hooks::dGetScreenSize(void* ecx, void* edx, int& width, int& height)
{
    BMVR_PROF("hook GetScreenSize");
    (void)edx;
    if (hkGetScreenSize.fOriginal)
        hkGetScreenSize.fOriginal(ecx, width, height);
    if (m_VR && m_VR->HudPaintActive())
    {
        int x = 0, y = 0, w = 0, h = 0;
        if (m_VR->ForceHudOverlayViewport(x, y, w, h))
        {
            width = w;
            height = h;
            return;
        }
    }
    const int origW = width;
    const int origH = height;
    if (OffscreenWorldViewSizeLie(width, height))
    {
        static int s_ssLie;
        if (s_ssLie < 8 && (origW != width || origH != height))
        {
            Game::logMsg("GetScreenSize %dx%d -> %dx%d (eye RT)", origW, origH, width, height);
            ++s_ssLie;
        }
        return;
    }
    if (McLeftoverRecording())
        return;
    uint32_t fbW = 0, fbH = 0;
    if (!bmvr::HaveHmdFramebufferSize(fbW, fbH))
        return;
    // Permanent 1584x1440 videomode inside a 2560x1440 HWND pillarboxed the
    // desktop and offset VGUI mouse (2026-08-18) when ff_hmdfit is off.
    static int s_ssLog;
    if (s_ssLog < 8 && (width != static_cast<int>(fbW) || height != static_cast<int>(fbH)))
    {
        Game::logMsg("GetScreenSize %dx%d (HMD-fb %ux%u unused; keep window size)",
            width, height, fbW, fbH);
        ++s_ssLog;
    }
}

// Areaportal culling with off-centre eyes (OffCenterEyes, LeftEyeCullPad). The engine's areaportal setup
// (engine+0x138CF0) copies the current view, sets its screen window to the
// centred +-tan(fov/2) of that view (fov/aspect = the union frustum), then
// projects doorways with the copy's ComputeViewMatrices — which honours
// m_bOffCenter and so maps each eye's own sub-frustum onto -1..1. Doorway
// rects then land in the wrong part of the window and rooms behind them were
// culled while in view (2026-10-11 / 10-14). Clearing the flag on that
// private copy gives the union projection, which matches the window; the
// union contains both eyes, so culling stays conservative.
void __fastcall Hooks::dComputeViewMatrices(void* ecx, void* edx, void* worldToView, void* viewToProjection, void* worldToProjection)
{
    BMVR_PROF("hook ComputeViewMatrices");
    (void)edx;
    static const uintptr_t s_engine = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine.dll"));
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    // Pass trace: every view's projection inputs (mirror/water views vs the eye passes).
    if (bmvr::g_PassTraceFrames > 0 && ecx)
    {
        const auto* v = static_cast<const CViewSetup*>(ecx);
        static const uintptr_t s_client = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
        const bool inEngine = s_engine && ret >= s_engine && ret < s_engine + 0x800000;
        const bool inClient = s_client && ret >= s_client && ret < s_client + 0x800000;
        Game::logMsg("trace: ComputeViewMatrices ret=%s+0x%X eye=%d %dx%d fov=%.2f aspect=%.4f offc=%d L=%.4f R=%.4f B=%.4f T=%.4f "
            "origin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f) zNear=%.2f",
            inEngine ? "engine" : (inClient ? "client" : "?"),
            static_cast<unsigned>(ret - (inEngine ? s_engine : (inClient ? s_client : 0))),
            m_VR ? m_VR->m_StereoEye : -1, v->width, v->height, v->fov, v->m_flAspectRatio,
            v->m_bOffCenter ? 1 : 0, v->m_flOffCenterLeft, v->m_flOffCenterRight, v->m_flOffCenterBottom,
            v->m_flOffCenterTop, v->origin.x, v->origin.y, v->origin.z, v->angles.x, v->angles.y, v->angles.z,
            v->zNear);
    }
    if (ecx && s_engine && ret == s_engine + Offsets::kAreaPortalComputeViewReturnRva
        && m_VR && m_VR->AnyEyeOffCenter())
    {
        auto* view = static_cast<CViewSetup*>(ecx);
        if (view->m_bOffCenter)
        {
            view->m_bOffCenter = false;
            static int s_log;
            if (s_log < 2)
            {
                Game::logMsg("Areaportal view: off-centre cleared on the portal copy (fov=%.1f aspect=%.3f)",
                    view->fov, view->m_flAspectRatio);
                ++s_log;
            }
        }
    }
    // Mirror views (water and reflective floors, _rt_WaterReflection) copy the eye's view, put the
    // camera below the surface, negate the pitch and turn the roll by 180 degrees. A 180-degree roll
    // also turns the eye's off-centre sub-window to the opposite side, so the mirror rendered the
    // wrong part of the field and the floor read it back offset, moving with the head (lobby floor in
    // bm_c1a0a, right eye worst, 2026-10-29). Pitch negated + roll 180 makes the mirror image the eye
    // image flipped left-right only (checked numerically, also with head roll), so mirror the window
    // horizontally; top and bottom stay.
    if (ecx)
    {
        const auto* view = static_cast<const CViewSetup*>(ecx);
        float roll = std::fmod(view->angles.z, 360.f);
        if (roll > 180.f) roll -= 360.f;
        if (roll < -180.f) roll += 360.f;
        if (view->m_bOffCenter && std::fabs(roll) > 90.f)
        {
            CViewSetup mirrored = *view;
            mirrored.m_flOffCenterLeft = 1.f - view->m_flOffCenterRight;
            mirrored.m_flOffCenterRight = 1.f - view->m_flOffCenterLeft;
            static int s_log;
            if (s_log < 4)
            {
                Game::logMsg("Mirror view: off-centre window mirrored left-right L/R %.3f/%.3f -> %.3f/%.3f (%dx%d)",
                    view->m_flOffCenterLeft, view->m_flOffCenterRight, mirrored.m_flOffCenterLeft,
                    mirrored.m_flOffCenterRight, view->width, view->height);
                ++s_log;
            }
            hkComputeViewMatrices.fOriginal(&mirrored, worldToView, viewToProjection, worldToProjection);
            return;
        }
        // Water reflection views copy the eye's view with the camera below the surface and pitch and
        // roll negated (no 180-degree roll). That makes the mirror image the eye image flipped
        // top-bottom only, so the off-centre window has to flip the same way. Without it, a headset
        // with a different up and down FOV reads the reflection at a fixed screen offset, and the
        // reflection slides over the water as the head turns. Left and right stay.
        const int eye = m_VR ? m_VR->m_StereoEye : 0;
        if (view->m_bOffCenter && eye >= 0 && eye <= 2)
        {
            struct EyeView { bool valid; float x, y, z, pitch, yaw, roll; };
            static EyeView s_eyeView[3];
            const auto norm180 = [](float a) {
                a = std::fmod(a, 360.f);
                if (a > 180.f) a -= 360.f;
                if (a < -180.f) a += 360.f;
                return a;
            };
            const float pitch = norm180(view->angles.x);
            const float yaw = norm180(view->angles.y);
            EyeView& last = s_eyeView[eye];
            constexpr float kAngleEps = 0.01f;
            const bool sameXY = std::fabs(view->origin.x - last.x) < 0.01f && std::fabs(view->origin.y - last.y) < 0.01f;
            const bool mirrored = last.valid
                && std::fabs(pitch + last.pitch) < kAngleEps
                && std::fabs(norm180(yaw - last.yaw)) < kAngleEps
                && std::fabs(roll + last.roll) < kAngleEps
                && (std::fabs(pitch) > kAngleEps || (sameXY && std::fabs(view->origin.z - last.z) > 1.f));
            if (!mirrored)
            {
                last = { true, view->origin.x, view->origin.y, view->origin.z, pitch, yaw, roll };
            }
            else if (std::fabs(view->m_flOffCenterBottom + view->m_flOffCenterTop - 1.f) > 0.0005f)
            {
                CViewSetup flipped = *view;
                flipped.m_flOffCenterBottom = 1.f - view->m_flOffCenterTop;
                flipped.m_flOffCenterTop = 1.f - view->m_flOffCenterBottom;
                static int s_log;
                if (s_log < 4)
                {
                    Game::logMsg("Water mirror view: off-centre window flipped top-bottom B/T %.3f/%.3f -> %.3f/%.3f (%dx%d)",
                        view->m_flOffCenterBottom, view->m_flOffCenterTop, flipped.m_flOffCenterBottom,
                        flipped.m_flOffCenterTop, view->width, view->height);
                    ++s_log;
                }
                hkComputeViewMatrices.fOriginal(&flipped, worldToView, viewToProjection, worldToProjection);
                return;
            }
        }
    }
    hkComputeViewMatrices.fOriginal(ecx, worldToView, viewToProjection, worldToProjection);
}

// Diagnostic: which views BM's deferred-light update sees (light culling
// under off-centre eyes, 2026-10-29). Logs only while a pass trace runs.
// Black Mesa marks client PVS notifiers (client ragdolls, props) from each
// view's BuildWorldRenderLists and, at FRAME_RENDER_END, tells every notifier
// not marked since the last check that it left the PVS: its projected shadow
// is destroyed and rebuilt a frame later. With MulticoreMode the leftover main
// view after a stereo pair is skipped, so that frame has no view at all and
// every shadow flickered off. A frame without a view says nothing about
// visibility: keep the previous state.
void __cdecl Hooks::dClientUpdatePvsNotifiers()
{
    BMVR_PROF("hook ClientUpdatePvsNotifiers");
    m_PvsSeenSinceCheck = true;
    hkClientUpdatePvsNotifiers.fOriginal();
}

void __cdecl Hooks::dClientPvsNotifierCheck()
{
    BMVR_PROF("hook ClientPvsNotifierCheck");
    // Main-thread frame end (FRAME_RENDER_END): per-frame VR flags reset here
    // in MulticoreMode, see VR::Update.
    if (m_VR && bmvr::g_McFrameReset && m_VR->McPipelineEnabled())
    {
        static int s_log;
        if (s_log < 2)
        {
            Game::logMsg("MC per-frame flags reset at the main thread's frame end (bmvr_mc_frame_reset 1)");
            ++s_log;
        }
        m_VR->m_StereoEyesDrawnThisFrame = false;
        m_VR->m_PosesWaitedThisFrame = false;
    }
    const bool skip = m_SkippedViewSinceCheck && !m_PvsSeenSinceCheck;
    m_SkippedViewSinceCheck = false;
    m_PvsSeenSinceCheck = false;
    if (skip)
        return;
    hkClientPvsNotifierCheck.fOriginal();
}

// Black Mesa's occlusion tracker (r_occlusion_tracker_enabled) keys its GPU occlusion results
// by (view id, object). Both eyes render as the same main view, so the right eye used results
// queried against the left eye's depth: objects only the right eye could see (far right edge,
// through doorways at another angle) stayed culled until the left eye saw them (2026-10-29,
// headset: r_occlusion_tracker_enabled 0 removed all of it; the eye-order test moved it to the
// other side). The right eye's views get their own id, so EndView queries them against the
// right eye's depth. Shadow views (3, 4) do not depend on the eye and keep one shared id; 3 is
// also special-cased inside the tracker's visibility test.
void __fastcall Hooks::dOcclusionBeginView(void* ecx, void* edx, int viewId, int a2, const float* plane, bool a4)
{
    BMVR_PROF("hook OcclusionBeginView");
    (void)edx;
    constexpr int kRightEyeViewIdOffset = 0x100;
    const int id = viewId & 0xFFFF;
    if (m_VR && m_VR->m_StereoEye == 2 && id != 0xFFFF && id != 3 && id != 4 && id < kRightEyeViewIdOffset)
    {
        static int s_log;
        if (s_log < 4)
        {
            Game::logMsg("Occlusion tracker: right-eye view id %d -> %d", id, id + kRightEyeViewIdOffset);
            ++s_log;
        }
        viewId = (viewId & ~0xFFFF) | (id + kRightEyeViewIdOffset);
    }
    {
        // Pop-in diagnostic (2026-10-30): which tracker view ids run per eye
        // and whether a reflection-set RT is bound (water reflection /
        // refraction views). Logs each (id, eye, reflection) combination once.
        const int eye = m_VR ? m_VR->m_StereoEye : -1;
        const bool reflTop = g_RtStackDepth > 0
            && ReflectionRtSized(g_RtStack[g_RtStackDepth - 1].w, g_RtStack[g_RtStackDepth - 1].h);
        static int s_seen[48][3];
        static int s_seenN;
        const int finalId = viewId & 0xFFFF;
        bool seen = false;
        for (int i = 0; i < s_seenN; ++i)
        {
            if (s_seen[i][0] == finalId && s_seen[i][1] == eye && s_seen[i][2] == (reflTop ? 1 : 0))
            {
                seen = true;
                break;
            }
        }
        if (!seen && s_seenN < 48)
        {
            s_seen[s_seenN][0] = finalId;
            s_seen[s_seenN][1] = eye;
            s_seen[s_seenN][2] = reflTop ? 1 : 0;
            ++s_seenN;
            Game::logMsg("Occlusion tracker view id %d (in %d) eye=%d rtTop=%s reflectionRt=%d plane=%d",
                finalId, id, eye, g_RtStackDepth > 0 ? g_RtStack[g_RtStackDepth - 1].name : "-",
                reflTop ? 1 : 0, plane ? 1 : 0);
        }
    }
    hkOcclusionBeginView.fOriginal(ecx, viewId, a2, plane, a4);
}

// The engine's occluder system (r_occlusion, map func_occluder) takes its view in
// CRender::Push3DView, but the client tests renderables with IsOccluded while building the
// view's lists, before that push. Flat, the test uses the previous frame's view, which is
// close enough. In stereo the right eye was tested from where the left eye last pushed its
// view, so static props the left eye saw as hidden behind an occluder (cable runs past a
// wall corner, the soda machine, a doorway) were dropped from the right eye
// (2026-10-30, headless: r_occlusion 0 restored them). Record each eye's last SetView and
// replay it before that eye's RenderView, so each eye is tested from its own previous frame.
namespace
{
    struct OccluderView
    {
        bool valid = false;
        float origin[3];
        float fov;
        float worldToView[16];
        float viewToProjection[16];
        float nearPlane[4];
    };
    OccluderView g_OccluderViews[3];
    void* g_OccluderSystem = nullptr;
}

// G-buffer world pass and the lighting tier: at nr_lights_quality 3+ Black Mesa draws the main view's G-buffer world
// pass (DrawWorldLists flag 0x4000) with flags 0x4007, at 2 and below with 0x4005: without 0x2 (strictly under
// water). The underwater world is then only in the refraction view, and the water shader, which reads the eye's
// G-buffer depth, sees holes (nothing drawn) under the surface: hard-edged clear patches in the water (bm_c1a1e,
// both eyes). Adding 0x2 to that pass is what the higher tier does.
void __fastcall Hooks::dDrawWorldLists(void* ecx, void* edx, void* renderContext, void* list, unsigned long flags, float waterZAdjust)
{
    const unsigned long kUnder = 0x2, kGBufferPass = 0x4000;
    const unsigned long use = (bmvr::g_EyeUnderwaterWorld && (flags & kGBufferPass)) ? (flags | kUnder) : flags;
    if (use != flags || bmvr::g_PassTraceFrames > 0)
    {
        // one line per distinct (eye, flags) combination
        const int eye = m_VR ? m_VR->m_StereoEye : 0;
        static std::mutex s_mtx;
        static std::set<unsigned long long> s_seen;
        const unsigned long long key = (static_cast<unsigned long long>(eye) << 32) | flags | (use != flags ? 0x80000000ull : 0);
        std::lock_guard<std::mutex> lock(s_mtx);
        if (s_seen.size() < 64 && s_seen.insert(key).second)
            Game::logMsg("DrawWorldLists eye=%d flags=0x%lX%s", eye, flags,
                use != flags ? " -> 0x2 added: underwater world in the G-buffer (EyeUnderwaterWorld)" : "");
    }
    hkDrawWorldLists.fOriginal(ecx, renderContext, list, use, waterZAdjust);
}

void __fastcall Hooks::dOccluderSetView(void* ecx, void* edx, const float* origin, float fov, const float* worldToView,
    const float* viewToProjection, const float* nearPlane)
{
    BMVR_PROF("hook OccluderSetView");
    (void)edx;
    const int eye = m_VR ? m_VR->m_StereoEye : 0;
    // The occluder system projects its occluders with only the symmetric scales of this matrix
    // (m00, m11, m23) but tests occludees with the full projection, so an off-centre eye
    // (OffCenterEyes) shifts occludees sideways against the occluders: the right eye dropped a
    // wall cable run behind a corner the left eye kept (2026-10-30, headless). Remove the
    // off-centre shift so both sides use the same projection; a sideways shift applied to both
    // does not change what overlaps what.
    float symmetricProjection[16];
    if (bmvr::g_OffCenterEyes && (eye == 1 || eye == 2) && viewToProjection)
    {
        std::memcpy(symmetricProjection, viewToProjection, sizeof(symmetricProjection));
        static int s_log;
        if (s_log < 2 || bmvr::g_PassTraceFrames > 0)
        {
            Game::logMsg("Occluder view: eye %d projection off-centre m02=%.4f m12=%.4f m20=%.4f m21=%.4f removed", eye,
                symmetricProjection[2], symmetricProjection[6], symmetricProjection[8], symmetricProjection[9]);
            ++s_log;
        }
        symmetricProjection[2] = 0.f;
        symmetricProjection[6] = 0.f;
        viewToProjection = symmetricProjection;
    }
    if (bmvr::g_OccluderViewPerEye && (eye == 1 || eye == 2) && origin && worldToView && viewToProjection && nearPlane)
    {
        OccluderView& v = g_OccluderViews[eye];
        std::memcpy(v.origin, origin, sizeof(v.origin));
        v.fov = fov;
        std::memcpy(v.worldToView, worldToView, sizeof(v.worldToView));
        std::memcpy(v.viewToProjection, viewToProjection, sizeof(v.viewToProjection));
        std::memcpy(v.nearPlane, nearPlane, sizeof(v.nearPlane));
        v.valid = true;
        g_OccluderSystem = ecx;
    }
    if (bmvr::g_PassTraceFrames > 0 && origin)
        Game::logMsg("trace: OccluderSetView eye=%d origin=(%.1f,%.1f,%.1f) fov=%.2f", eye, origin[0], origin[1], origin[2], fov);
    hkOccluderSetView.fOriginal(ecx, origin, fov, worldToView, viewToProjection, nearPlane);
}

void Hooks::ReplayOccluderView(int eye)
{
    if ((eye != 1 && eye != 2) || !hkOccluderSetView.fOriginal || !g_OccluderSystem)
        return;
    const OccluderView& v = g_OccluderViews[eye];
    if (!v.valid)
        return;
    static int s_log;
    if (s_log < 2)
    {
        Game::logMsg("Occluder view: eye %d tested from its own last view (%.1f,%.1f,%.1f)", eye,
            v.origin[0], v.origin[1], v.origin[2]);
        ++s_log;
    }
    if (bmvr::g_PassTraceFrames > 0)
        Game::logMsg("trace: OccluderReplay eye=%d origin=(%.1f,%.1f,%.1f)", eye, v.origin[0], v.origin[1], v.origin[2]);
    hkOccluderSetView.fOriginal(g_OccluderSystem, v.origin, v.fov, v.worldToView, v.viewToProjection, v.nearPlane);
}

// Diagnostic (trace frames only): which eye/thread tests which box, against which occluder view
// origin (this+0x98), with the edge list still dirty (this+4) or not, and the result.
bool __fastcall Hooks::dOccluderIsOccluded(void* ecx, void* edx, const float* mins, const float* maxs)
{
    BMVR_PROF("hook OccluderIsOccluded");
    (void)edx;
    if (bmvr::g_PassTraceFrames <= 0 || !ecx || !mins || !maxs)
        return hkOccluderIsOccluded.fOriginal(ecx, mins, maxs);
    const unsigned char* self = static_cast<const unsigned char*>(ecx);
    const bool dirty = self[4] != 0;
    const float* o = reinterpret_cast<const float*>(self + 0x98);
    const bool r = hkOccluderIsOccluded.fOriginal(ecx, mins, maxs);
    Game::logMsg("trace: IsOccluded eye=%d tid=%lu view=(%.1f,%.1f,%.1f) dirty=%d box=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) -> %d",
        m_VR ? m_VR->m_StereoEye : -1, GetCurrentThreadId(), o[0], o[1], o[2], dirty ? 1 : 0,
        mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2], r ? 1 : 0);
    return r;
}

void __fastcall Hooks::dNewRendererUpdatePerCamera(void* ecx, void* edx, void* a1, void* view, bool secondary, void* a4)
{
    BMVR_PROF("hook NewRendererUpdatePerCamera");
    (void)edx;
    if (bmvr::g_PassTraceFrames > 0 && view)
    {
        const auto* v = static_cast<const CViewSetup*>(view);
        Game::logMsg("trace: DefLights UpdatePerCamera ret=client+0x%X sec=%d modEye=%d viewEye=%d %dx%d fov=%.2f aspect=%.4f "
            "offc=%d L=%.4f R=%.4f B=%.4f T=%.4f origin=(%.2f,%.2f,%.2f) ang=(%.2f,%.2f,%.2f) view=%p",
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(_ReturnAddress())
                - reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"))),
            secondary ? 1 : 0, m_VR ? m_VR->m_StereoEye : -1, v->m_eStereoEye, v->width, v->height, v->fov,
            v->m_flAspectRatio, v->m_bOffCenter ? 1 : 0, v->m_flOffCenterLeft, v->m_flOffCenterRight,
            v->m_flOffCenterBottom, v->m_flOffCenterTop, v->origin.x, v->origin.y, v->origin.z,
            v->angles.x, v->angles.y, v->angles.z, view);
    }
    // The light loop tests against the frame's frustum, built from the main view.
    // In VR that view follows the gun aim, so a light behind the hand went out
    // with the head facing it. Test against the view being rendered instead.
    if (bmvr::g_LightCullEyeView && view && m_VR && m_VR->m_StereoEye != 0)
    {
        using GenerateFrustumFn = void(__cdecl*)(const void* origin, const void* angles, float, float, float, float, void*);
        static GenerateFrustumFn s_generate = nullptr;
        static void* s_frustum = nullptr;
        static bool s_checked = false;
        if (!s_checked)
        {
            s_checked = true;
            static const unsigned char kPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28, 0x8D, 0x45, 0xF0 };
            const auto client = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
            const auto* fn = reinterpret_cast<const unsigned char*>(client + Offsets::kGeneratePerspectiveFrustumRva);
            if (client && std::memcmp(fn, kPrologue, sizeof(kPrologue)) == 0)
            {
                s_generate = reinterpret_cast<GenerateFrustumFn>(client + Offsets::kGeneratePerspectiveFrustumRva);
                s_frustum = reinterpret_cast<void*>(client + Offsets::kClientFrameFrustumRva);
                Game::logMsg("Light culling follows each eye's view (LightCullEyeView)");
            }
            else
            {
                Game::logMsg("Light culling: frustum builder client+0x%X MISMATCH (off)", Offsets::kGeneratePerspectiveFrustumRva);
                Game::noteMissingOffset("Frame frustum builder (per-eye light culling)");
            }
        }
        if (s_generate)
        {
            const auto* v = static_cast<const CViewSetup*>(view);
            s_generate(&v->origin, &v->angles, v->zNear, v->zFar, v->fov, v->m_flAspectRatio, s_frustum);
        }
    }
    hkNewRendererUpdatePerCamera.fOriginal(ecx, a1, view, secondary, a4);
}

float __fastcall Hooks::dGetScreenAspectRatio(void* ecx, void* edx)
{
    BMVR_PROF("hook GetScreenAspectRatio");
    (void)edx;
    float aspect = 0.f;
    if (hkGetScreenAspectRatio.fOriginal)
        aspect = hkGetScreenAspectRatio.fOriginal(ecx);
    if (m_VR && m_VR->HudPaintActive())
        return aspect > 0.1f ? aspect : (4.f / 3.f);
    if (McLeftoverRecording())
        return aspect > 0.1f ? aspect : (4.f / 3.f);
    // client.dll FUN_1020a8f0 DrawViewModels: viewModelSetup.m_flAspectRatio =
    // engine->GetScreenAspectRatio() (IVEngineClient slot 96). That is the
    // HWND 16:9 ratio, not GetScreenSize and not the stereo CViewSetup we
    // already set to the eye. World/gloves use the HMD frustum (~1.027);
    // the gun used 16:9 in a ~square eye RT (stretch along view-up, and the
    // grip rides the look plane when you nod).
    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && EngineInGame()
        && m_VR->m_RenderWidth >= 640 && m_VR->m_RenderHeight >= 360
        && (WorldMatchEyeSizeLieAllowed()
            || (m_VR->StereoWorldPassActive()
                && (bmvr::OffscreenWorldMatchesEyes() || bmvr::UseGbMatchViewLock()
                    || m_VR->CachedRt0MatchesEyes()
                    || m_VR->D3dRt0IsEyeSized()))))
    {
        // Off-centre eye: the centred union aspect its sub-rectangle is of.
        float eyeAspect = static_cast<float>(m_VR->m_RenderWidth) / static_cast<float>(m_VR->m_RenderHeight);
        m_VR->EyeRenderAspect(m_VR->m_StereoEye, eyeAspect);
        static int s_arLog;
        if (s_arLog < 8)
        {
            Game::logMsg("GetScreenAspectRatio %.3f -> %.3f (eye %ux%u stereoEye=%d)",
                aspect, eyeAspect, m_VR->m_RenderWidth, m_VR->m_RenderHeight,
                m_VR->m_StereoEye);
            ++s_arLog;
        }
        return eyeAspect;
    }
    return aspect > 0.1f ? aspect : (4.f / 3.f);
}

ITexture* __fastcall Hooks::dCreateNamedRTEx(void* ecx, void* edx, const char* name, int w, int h, int sizeMode, int format, int depth, unsigned textureFlags, unsigned renderTargetFlags)
{
    BMVR_PROF_COUNT("render target created");
    (void)edx;
    void* const rtCaller = _ReturnAddress();
    const int rtReqW = w, rtReqH = h, rtReqMode = sizeMode;
    {
        const char* map = PeekLevelName();
        if (map && map[0])
            bmvr::NoteEngineMapName(map);
    }

    uint32_t fbW = 0, fbH = 0;
    const bool haveFb = bmvr::TryHmdFramebuffer() && bmvr::HaveHmdFramebufferSize(fbW, fbH);
    const bool fullMode = sizeMode == RT_SIZE_FULL_FRAME_BUFFER
        || sizeMode == RT_SIZE_FULL_FRAME_BUFFER_ROUNDED_UP;
    // BM's light shafts (god rays, np_gr_quality) draw _rt_lsMask with the
    // G-buffer depth and copy the scene into _rt_lsScene; PushRT already
    // treats _rt_ls* as eye-sized world targets. Left at the window size the
    // mask pass ran with a depth buffer of another size and a viewport larger
    // than its target, and the composite replaced the eye with the mask:
    // world gone, black/white plus the source's glow, whenever the source
    // was in front (bm_c1a3c, 2026-10-25). Grow them with FullFrame. Only
    // the full-frame-sized ones (all four are RT_SIZE_FULL_FRAME_BUFFER on
    // this build); a smaller _rt_ls* would otherwise be blown up to the eye.
    const bool namedLs = name && fullMode && std::strstr(name, "_rt_ls") != nullptr;
    const bool namedFb = name && (std::strstr(name, "_rt_FullFrameFB")
        || std::strstr(name, "_rt_ResolvedFullFrame") || namedLs);
    const bool namedGb = name && std::strstr(name, "_rt_gb")
        && !std::strstr(name, "shadow") && !std::strstr(name, "Shadow")
        && !std::strstr(name, "flashlight") && !std::strstr(name, "Flashlight")
        && !std::strstr(name, "csm") && !std::strstr(name, "CSM");
    const bool skipGrow = name && (std::strstr(name, "_rt_Hud") || std::strstr(name, "_rt_gui")
        || std::strstr(name, "Dof") || std::strstr(name, "dof")
        || std::strstr(name, "Water") || std::strstr(name, "Reflect")
        || std::strstr(name, "Refract")
        || std::strstr(name, "PowerOfTwo") || std::strstr(name, "_rt_Camera"));

    uint32_t growW = 0, growH = 0;
    // hmd_world (WorldRenderAtEyeSize): FullFrame and G-buffer must grow on the
    // same gate. Growing only one leaves worldMatch=0, which keeps the HWND
    // view lock and stamps 2560x1440 into the top of an eye-sized RT.
    const bool looksLikeSceneBuffer = fullMode || w <= 0 || h <= 0
        || (w >= 1280 && h >= 720 && w != h);
    uint32_t offW = 0, offH = 0;
    const bool wantGrowFb = namedFb && !skipGrow && sizeMode != RT_SIZE_PICMIP
        && bmvr::ComputeGrownWorldFramebuffer(offW, offH);
    uint32_t gbW = 0, gbH = 0;
    const bool wantGrowGb = namedGb && looksLikeSceneBuffer && !skipGrow
        && sizeMode != RT_SIZE_PICMIP
        && bmvr::ComputeGrownWorldGbuffer(gbW, gbH);
    if (wantGrowFb)
    {
        growW = offW;
        growH = offH;
    }
    if (wantGrowGb)
    {
        growW = gbW;
        growH = gbH;
    }
    bool wantGrow = wantGrowFb || wantGrowGb;
    // ReflectionRtScale (default 0 = engine size): the water reflection /
    // refraction RTs and _rt_PowerOfTwoFB (refract materials, e.g. wired
    // glass) stay 1024x1024 while the eye is 3472-5056 px, so mirrored floors
    // and glass are a 3-5x blow-up of a small render: stair-stepped edges
    // that no RenderScale or FXAA changes (bm_c1a0a lobby floor, bm_c2a2b door
    // glass, 2026-10-30). Opt-in experiment: earlier reflection-viewport bugs
    // stamped a view-locked world into these RTs, and the reflection is a
    // second scene render per eye. _rt_Camera (monitor cameras) stays.
    // The reflection view's own deferred G-buffer (_rt_gbDepth2,
    // _rt_gbDepthRT2, _rt_gbNormal2, _rt_gbLightDiff2; 1024x1024 mode 2 like
    // the water RTs) must grow with them: left at 1024 under a 2000x2048
    // reflection viewport, only its top-left quarter was written and the
    // mirror floor showed that quarter magnified, black elsewhere
    // (2026-10-30). Growing these alone to the eye broke deferred albedo
    // before (2026-08-21); the point is one size for the whole reflection set.
    const size_t nameLen = name ? std::strlen(name) : 0;
    const bool reflectGb = name && nameLen > 7 && std::strncmp(name, "_rt_gb", 6) == 0
        && name[nameLen - 1] == '2'
        && !std::strstr(name, "shadow") && !std::strstr(name, "Shadow");
    // _rt_PowerOfTwoFB is only a copy of the eye, so it has its own scale
    // (RefractCopyRtScale): growing it alone fixes the stair-stepped floor /
    // wall junctions without the second scene render of the reflection set.
    const bool refractCopy = name && std::strstr(name, "PowerOfTwo") != nullptr;
    const float auxScale = refractCopy ? bmvr::RefractCopyRtScale() : bmvr::g_ReflectionRtScale;
    const bool reflectRt = name && auxScale > 0.f
        && (refractCopy || std::strstr(name, "Water") || std::strstr(name, "water")
            || std::strstr(name, "Reflect") || std::strstr(name, "Refract")
            || reflectGb);
    uint32_t reflEyeW = 0, reflEyeH = 0;
    if (reflectRt && bmvr::ComputeOffscreenEyeSize(reflEyeW, reflEyeH))
    {
        const float scale = std::min(1.f, auxScale);
        int rw = static_cast<int>(static_cast<float>(reflEyeW) * scale) & ~15;
        const int rh = static_cast<int>(static_cast<float>(reflEyeH) * scale) & ~15;
        // The D3D hooks recognise the reflection set by exact size, so it
        // must not equal the eye/2 or eye/4 engine targets (bloom/small FB).
        const int ew = static_cast<int>(reflEyeW);
        const int eh = static_cast<int>(reflEyeH);
        if ((rw == ew / 2 && rh == eh / 2) || (rw == ew / 4 && rh == eh / 4))
            rw -= 16;
        if (rw > w && rh > h && rw >= 256 && rh >= 256)
        {
            Game::logMsg("CreateNamedRT %s reflection grow %dx%d mode=%d -> %dx%d LITERAL (%s %.2f of eye %ux%u)",
                name, w, h, sizeMode, rw, rh, refractCopy ? "RefractCopyRtScale" : "ReflectionRtScale",
                scale, reflEyeW, reflEyeH);
            w = rw;
            h = rh;
            sizeMode = RT_SIZE_LITERAL;
            if (refractCopy)
            {
                bmvr::g_RefractCopyRtW = rw;
                bmvr::g_RefractCopyRtH = rh;
            }
            else
            {
                bmvr::g_ReflectionRtW = rw;
                bmvr::g_ReflectionRtH = rh;
            }
        }
    }
    uint32_t tableW = 0, tableH = 0;
    if (bmvr::EyeScaledRtSize(name, tableW, tableH))
    {
        static int s_tableLog;
        if (s_tableLog < 32)
        {
            ++s_tableLog;
            const bool scene = bmvr::EyeScaledRtClass(name) == bmvr::EyeRtClass::Scene;
            Game::logMsg("CreateNamedRT %s mode=%d depth=%d -> %ux%u LITERAL, %s (eye size table)",
                name, sizeMode, depth, tableW, tableH, scene ? "engine depth, scene" : "own depth");
        }
        w = static_cast<int>(tableW);
        h = static_cast<int>(tableH);
        sizeMode = RT_SIZE_LITERAL;
        // Bloom: own depth buffer of the same size. With the engine's shared
        // depth the viewport on a target taller than the window is cut to the
        // window's height: the bright pass filled only the top of the target and
        // left stale pixels below, which engine_post then showed as a ghost.
        // Scene targets keep the engine's choice: the CSM gather is pushed with
        // the eye-sized G-buffer depth and tests its stencil.
        if (bmvr::EyeScaledRtClass(name) != bmvr::EyeRtClass::Scene)
            depth = MATERIAL_RT_DEPTH_SEPARATE;
    }
    const int oldW = w;
    const int oldH = h;
    if (wantGrow && oldW >= 640 && oldH >= 360
        && growW <= static_cast<uint32_t>(oldW)
        && growH <= static_cast<uint32_t>(oldH))
    {
        static int s_shrinkLog;
        if (s_shrinkLog < 8)
        {
            Game::logMsg("CreateNamedRT %s skip shrink %dx%d -> %ux%u (keep engine size)",
                name ? name : "?", oldW, oldH, growW, growH);
            ++s_shrinkLog;
        }
        wantGrow = false;
    }
    if (wantGrow)
    {
        const int oldMode = sizeMode;
        w = static_cast<int>(growW);
        h = static_cast<int>(growH);
        sizeMode = RT_SIZE_LITERAL;
        static int s_growLog;
        if (s_growLog < 12)
        {
            Game::logMsg("CreateNamedRT %s grow %dx%d mode=%d -> %dx%d LITERAL (offscreen rec=%ux%u ff=%d gb=%d)",
                name ? name : "?", oldW, oldH, oldMode, w, h,
                bmvr::g_RecommendedEyeWidth, bmvr::g_RecommendedEyeHeight,
                wantGrowFb ? 1 : 0, wantGrowGb ? 1 : 0);
            ++s_growLog;
        }
        // Crash-sticky must cover the grow, not just stereo enter: ff_gbfit
        // died at map load before the first stereo frame. EndRisky(120) runs
        // every stereo frame past 120, so a later map-change grow re-arms and
        // is cleared again on the next frame.
        static bool s_worldGrowRisky;
        if (!s_worldGrowRisky)
        {
            s_worldGrowRisky = true;
            bmvr::BeginRisky(L"hmd_world");
        }
    }
    else if (haveFb && (fullMode || namedFb) && name
        && !std::strstr(name, "shadow") && !std::strstr(name, "Shadow")
        && !std::strstr(name, "csm") && !std::strstr(name, "CSM")
        && !std::strstr(name, "flashlight"))
    {
        static int s_log;
        if (s_log < 8)
        {
            Game::logMsg("CreateNamedRT %s %dx%d mode=%d (keep engine size, HMD-fb %ux%u is eyes only)",
                name, w, h, sizeMode, fbW, fbH);
            ++s_log;
        }
    }
    if (!hkCreateNamedRTEx.fOriginal)
        return nullptr;
    // Named engine RTs (G-buffer, light buffers, FullFrame) stay 1x. The old
    // name-based HL2VR MSAA scope made only the light buffers 4x; DXVK then
    // dropped their 1x depth attachment. MSAA now lives on the private eye
    // targets only (VR::EnsurePrivateEyeSurfaces). Idea from a Discord user's
    // AA4-GEOMETRY4 Pimax build.
    ITexture* tex = hkCreateNamedRTEx.fOriginal(ecx, name, w, h, sizeMode, format, depth, textureFlags, renderTargetFlags);
    LogRtTrace(ecx, name, rtReqW, rtReqH, rtReqMode, tex, rtCaller);
    if (tex && name && (fullMode || namedFb || namedGb || wantGrow))
    {
        int aw = 0, ah = 0;
        BMVR_TRY
        {
            aw = tex->GetActualWidth();
            ah = tex->GetActualHeight();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            aw = 0;
            ah = 0;
        }
        static int s_actualLog;
        if (s_actualLog < 16)
        {
            Game::logMsg("CreateNamedRT %s requested %dx%d mode=%d actual %dx%d",
                name, w, h, sizeMode, aw, ah);
            ++s_actualLog;
        }
        if (aw > 0 && name && std::strstr(name, "_rt_FullFrameFB") && !std::strstr(name, "FB1") && !std::strstr(name, "FB2"))
        {
            bmvr::g_FullFrameActualWidth = static_cast<uint32_t>(aw);
            bmvr::g_FullFrameActualHeight = static_cast<uint32_t>(ah);
        }
        else if (aw > 0 && namedFb && !namedLs && bmvr::g_FullFrameActualWidth < 640)
        {
            bmvr::g_FullFrameActualWidth = static_cast<uint32_t>(aw);
            bmvr::g_FullFrameActualHeight = static_cast<uint32_t>(ah);
        }
        if (aw >= 640 && ah >= 360 && namedGb)
        {
            if (static_cast<uint32_t>(aw) >= bmvr::g_GbActualWidth
                && static_cast<uint32_t>(ah) >= bmvr::g_GbActualHeight)
            {
                bmvr::g_GbActualWidth = static_cast<uint32_t>(aw);
                bmvr::g_GbActualHeight = static_cast<uint32_t>(ah);
            }
        }
    }
    return tex;
}

void __fastcall Hooks::dEndRTAlloc(void* ecx, void* edx)
{
    (void)edx;
    if (hkEndRTAlloc.fOriginal)
        hkEndRTAlloc.fOriginal(ecx);
}

void __fastcall Hooks::dDrawFilledRect(void* ecx, void* edx, int x0, int y0, int x1, int y1)
{
    (void)edx;
    if (SkipPauseOverlayFullscreenRect(x0, y0, x1, y1))
        return;
    if (hkDrawFilledRect.fOriginal)
        hkDrawFilledRect.fOriginal(ecx, x0, y0, x1, y1);
}

void __fastcall Hooks::dDrawTexturedRect(void* ecx, void* edx, int x0, int y0, int x1, int y1)
{
    (void)edx;
    if (SkipPauseOverlayFullscreenRect(x0, y0, x1, y1))
        return;
    if (hkDrawTexturedRect.fOriginal)
        hkDrawTexturedRect.fOriginal(ecx, x0, y0, x1, y1);
}

// The client's view render (client 0x201DB0, 'VGui_DrawHud') sizes the
// client UI roots (PANEL_CLIENTDLL, PANEL_CLIENTDLL_TOOLS) to the view, which
// in VR is the eye (3472x3536). Client-scheme panels scale their proportional
// sizes by that root, so BM's menu UI (its own load dialog included) laid
// out at map start came out ~3.3x too big: a load from the pause menu showed
// the top-left 1920x1080 of an oversized load screen. Menus and HUD are
// window-sized layers in VR, as in flat play: keep both roots at the window.
// SetSize only runs on layout, so this costs nothing per frame.
void __fastcall Hooks::dPanelSetSize(void* ecx, void* edx, unsigned int vpanel, int wide, int tall)
{
    (void)edx;
    if (m_VR && m_Game && m_Game->m_EngineVGui
        && wide == static_cast<int>(m_VR->m_RenderWidth) && tall == static_cast<int>(m_VR->m_RenderHeight))
    {
        // PANEL_CLIENTDLL = 2, PANEL_CLIENTDLL_TOOLS = 6.
        const unsigned int client = static_cast<unsigned int>(
            reinterpret_cast<uintptr_t>(m_Game->m_EngineVGui->GetPanel(2)));
        const unsigned int tools = static_cast<unsigned int>(
            reinterpret_cast<uintptr_t>(m_Game->m_EngineVGui->GetPanel(6)));
        uint32_t winW = 0, winH = 0;
        if ((vpanel == client || vpanel == tools) && bmvr::QueryWindowClientSize(winW, winH)
            && winW >= 640 && winH >= 360)
        {
            static int s_log;
            if (s_log < 2)
            {
                Game::logMsg("Client UI root %dx%d -> window %ux%u", wide, tall, winW, winH);
                ++s_log;
            }
            wide = static_cast<int>(winW);
            tall = static_cast<int>(winH);
        }
    }
    if (hkPanelSetSize.fOriginal)
        hkPanelSetSize.fOriginal(ecx, vpanel, wide, tall);
}

void bmvr::InstallEarlyFramebufferHook()
{
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;

    const MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("MinHook init failed for framebuffer hook (%d)", static_cast<int>(mh));
        return;
    }

    HMODULE mat = GetModuleHandleA("materialsystem.dll");
    if (!mat)
        mat = GetModuleHandleA("MaterialSystem.dll");
    if (!mat)
    {
        Log("Framebuffer hook: materialsystem.dll not loaded yet");
        return;
    }
    // CreateDevice runs on the engine's thread, with the eye size known, and the engine
    // allocates its full-frame targets right after it returns. Hook the allocator here
    // rather than from the Game thread: that thread gets there only after client.dll
    // and the helper's first located views, so on a fast start (seen with SteamVR on
    // Windows) both allocation passes had finished, FullFrame stayed at the window size
    // and the whole session ran on the window-blit fallback.
    {
        const Offset createRt{ "materialsystem.dll", Offsets::kCreateNamedRTExRva, Offsets::kCreateNamedRTExSig };
        if (createRt.valid && !Hooks::hkCreateNamedRTEx.pTarget)
        {
            if (Hooks::hkCreateNamedRTEx.createHook((LPVOID)createRt.address, &Hooks::dCreateNamedRTEx) != 0
                || Hooks::hkCreateNamedRTEx.enableHook() != 0)
                Log("CreateNamedRTEx early hook failed rva=0x%X", createRt.offset);
            else
                Log("Hook enabled: CreateNamedRTEx rva=0x%X (before the first render target)", createRt.offset);
        }
    }

    if (!TryHmdFramebuffer())
        return;

    int off = SigScanner::VerifyOffset("materialsystem.dll", 0x53EF0,
        "55 8B EC 8B 0D ? ? ? ? 8B 01 8B 80 58 04 00 00 5D FF E0");
    if (off < 0)
        off = SigScanner::VerifyOffset("MaterialSystem.dll", 0x53EF0,
            "55 8B EC 8B 0D ? ? ? ? 8B 01 8B 80 58 04 00 00 5D FF E0");
    if (off == -1)
    {
        Log("GetBackBufferDimensions signature not found");
        return;
    }
    if (off == 0)
        off = 0x53EF0;
    void* target = reinterpret_cast<uint8_t*>(mat) + off;
    if (Hooks::hkGetBackBufferDimensions.createHook(target, &Hooks::dGetBackBufferDimensions) != 0
        || Hooks::hkGetBackBufferDimensions.enableHook() != 0)
    {
        Log("GetBackBufferDimensions hook failed rva=0x%X", off);
        return;
    }
    Log("Hook enabled: GetBackBufferDimensions rva=0x%X fb=%ux%u",
        off, g_FramebufferWidth, g_FramebufferHeight);

    HMODULE eng = GetModuleHandleA("engine.dll");
    if (!eng)
    {
        Log("GetScreenSize: engine.dll not loaded yet");
        return;
    }
    int screenOff = SigScanner::VerifyOffset("engine.dll", 0xA6DC0,
        "55 8B EC 8B 0D ? ? ? ? 56 8B 01 FF 90 9C 01 00 00");
    if (screenOff == -1)
    {
        Log("GetScreenSize signature not found");
        return;
    }
    if (screenOff == 0)
        screenOff = 0xA6DC0;
    void* screenTarget = reinterpret_cast<uint8_t*>(eng) + screenOff;
    if (!Hooks::hkGetScreenSize.pTarget)
    {
        if (Hooks::hkGetScreenSize.createHook(screenTarget, &Hooks::dGetScreenSize) != 0
            || Hooks::hkGetScreenSize.enableHook() != 0)
        {
            Log("GetScreenSize hook failed rva=0x%X", screenOff);
        }
        else
            Log("Hook enabled: GetScreenSize rva=0x%X", screenOff);
    }
    if (!Hooks::hkGetScreenAspectRatio.pTarget)
    {
        int aspectOff = SigScanner::VerifyOffset("engine.dll", 0x101670,
            "55 8B EC 8B 0D ? ? ? ? 83 EC 0C 81 F9 ? ? ? ? 75 16 F3 0F 10 0D");
        if (aspectOff == -1)
            Log("GetScreenAspectRatio signature not found");
        else
        {
            if (aspectOff == 0)
                aspectOff = 0x101670;
            void* aspectTarget = reinterpret_cast<uint8_t*>(eng) + aspectOff;
            if (Hooks::hkGetScreenAspectRatio.createHook(aspectTarget, &Hooks::dGetScreenAspectRatio) != 0
                || Hooks::hkGetScreenAspectRatio.enableHook() != 0)
                Log("GetScreenAspectRatio hook failed rva=0x%X", aspectOff);
            else
                Log("Hook enabled: GetScreenAspectRatio rva=0x%X", aspectOff);
        }
    }
}

namespace
{
    // Set while the server spawns a grenade of the local VR player (a thrown
    // frag or an MP5 launcher grenade), so the throw-position hook (also used
    // by NPC and other grenades) only moves that one, to g_VrSpawnPoint.
    bool g_VrGrenadeSpawn = false;
    Vector g_VrSpawnPoint{};
    const char* g_VrSpawnWhat = "";
    // Hornets come several per second: log only the first few spawns.
    int g_VrSpawnLogBudget = -1;   // -1 = unlimited (grenades)
    // How far the VR point may be from the game's own point before the game's
    // is kept (stale pose guard). Hornets: the game's point is ahead of the
    // eye, the muzzle is at arm's length, ~56 units apart headless.
    float g_VrSpawnMaxDist = 60.f;
    // Snark hand throw: set while the server throws a snark of the local
    // player from the hand; the placement hook notes the new snark.
    bool g_VrSnarkThrow = false;
    void* g_VrSnarkEntity = nullptr;
}

void* __cdecl Hooks::dGrenadeCreate(int type, Vector* velocity, void* angImpulse, void* owner, void* thrower, int mode)
{
    constexpr int kGrenadeTypeMp5 = 0;   // CWeapon_MP5 secondary fire (contact grenade)
    constexpr int kGrenadeTypeFrag = 1;
    // Mode 0 (used by both) places the grenade at the owner's EyePosition and
    // ignores the throw position, 1 at the middle of the two; any other value
    // uses the throw position, which dGrenadeThrowPosition moves to g_VrSpawnPoint.
    constexpr int kSpawnAtThrowPosition = 2;
    if (!hkGrenadeCreate.fOriginal)
        return nullptr;
    // MP5 grenade: launch from the drawn launcher tube instead of the eye.
    // Velocity already follows the controller (GetShootAngles).
    if (type == kGrenadeTypeMp5 && m_VR && m_Game && bmvr::g_VrShotOriginFix && ShouldRewriteShootAngles(owner))
    {
        C_BaseEntity* vm = m_Game->GetViewModelEntity();
        Vector tube{};
        QAngle unused{};
        if (vm && m_Game->GetEntityAttachment(vm, "grenade", tube, unused) && tube.LengthSqr() > 1.f)
        {
            g_VrSpawnPoint = tube;
            g_VrSpawnWhat = "MP5 grenade";
            g_VrGrenadeSpawn = true;
            void* grenade = hkGrenadeCreate.fOriginal(type, velocity, angImpulse, owner, thrower, kSpawnAtThrowPosition);
            g_VrGrenadeSpawn = false;
            return grenade;
        }
    }
    // Frag thrown by the local player: the next grenade comes with its pin.
    if (type == kGrenadeTypeFrag && ShouldRewriteShootAngles(owner))
        MagReload::g_PinThrown.store(true);
    // Satchel (CWeapon_Satchel anim event 0xBBD -> type 3, mode 0) thrown
    // from the right hand on the trigger release (VrSatchelHands).
    constexpr int kGrenadeTypeSatchel = 3;
    if (type == kGrenadeTypeSatchel && velocity && m_VR && bmvr::g_VrHandThrow && ShouldRewriteShootAngles(owner))
    {
        Vector satHand{}, satFrom{};
        const unsigned until = m_VR->m_SnarkHandThrowUntil.load(std::memory_order_relaxed);
        if (until != 0 && GetTickCount() < until && m_VR->TryGetThrowVelocity(satHand) && m_VR->TryGetThrowOrigin(satFrom))
        {
            m_VR->m_SnarkHandThrowUntil.store(0, std::memory_order_relaxed);
            Vector playerVel{};
            BMVR_TRY
            {
                constexpr int kAbsVelocity = 0x218;
                playerVel = *reinterpret_cast<const Vector*>(static_cast<const char*>(owner) + kAbsVelocity);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            const Vector before = *velocity;
            *velocity = satHand + playerVel;
            Game::logMsg("VR satchel throw: hand %.0f u/s -> (%.0f,%.0f,%.0f) (game %.0f u/s)",
                satHand.Length(), velocity->x, velocity->y, velocity->z, before.Length());
            g_VrSpawnPoint = satFrom;
            g_VrSpawnWhat = "VR satchel throw";
            g_VrSpawnLogBudget = -1;
            g_VrSpawnMaxDist = 60.f;
            g_VrGrenadeSpawn = true;
            void* satchel = hkGrenadeCreate.fOriginal(type, velocity, angImpulse, owner, thrower, kSpawnAtThrowPosition);
            g_VrGrenadeSpawn = false;
            return satchel;
        }
    }
    Vector hand{};
    if (type != kGrenadeTypeFrag || !bmvr::g_VrHandThrow || !velocity || !m_VR
        || !ShouldRewriteShootAngles(owner) || !m_VR->TryGetThrowVelocity(hand))
        return hkGrenadeCreate.fOriginal(type, velocity, angImpulse, owner, thrower, mode);

    // The game added a share of the player's own velocity to its aimed throw;
    // keep the player's full velocity like Lambda1VR (hand speed is relative
    // to the body).
    Vector playerVel{};
    BMVR_TRY
    {
        constexpr int kAbsVelocity = 0x218;   // FUN_10487250 reads +0x218..0x220
        playerVel = *reinterpret_cast<const Vector*>(static_cast<const char*>(owner) + kAbsVelocity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    const Vector before = *velocity;
    *velocity = hand + playerVel;
    Game::logMsg("VR frag throw: hand %.0f u/s -> (%.0f,%.0f,%.0f) (game %.0f u/s)",
        hand.Length(), velocity->x, velocity->y, velocity->z, before.Length());
    if (bmvr::g_PerfLog)
        m_VR->LogLastThrow();   // estimator details, playtest logging only
    g_VrSpawnWhat = "VR frag throw";
    g_VrGrenadeSpawn = m_VR->TryGetThrowOrigin(g_VrSpawnPoint);
    void* grenade = hkGrenadeCreate.fOriginal(type, velocity, angImpulse, owner, thrower, kSpawnAtThrowPosition);
    g_VrGrenadeSpawn = false;
    return grenade;
}

void* __cdecl Hooks::dProjectileCreate(int type, Vector* target, float speed, void* owner, void* thrower)
{
    if (!hkProjectileCreate.fOriginal)
        return nullptr;
    // Hivehand hornets: the game places them at the eye plus a head-frame
    // offset, which in VR is below and to the side of the drawn weapon. Spawn
    // them at the hivehand's muzzle instead. The target (end of the game's
    // aim trace) is kept, so the flight direction, the alt-fire spread and
    // the homing target are unchanged.
    constexpr int kProjectileHornet = 12;
    // RPG rocket (headless 2026-11-19: type 4): spawned at the eye, ~10 u
    // above a gun held lower, it flew level and then the laser guidance
    // pulled it down onto the gun's line = "rises, then flies forward"
    // (user 2026-11-18). From the RPG's muzzle like the hornets.
    constexpr int kProjectileRpg = 4;
    Vector muzzle{};
    if ((type == kProjectileHornet || type == kProjectileRpg) && m_VR && bmvr::g_VrShotOriginFix
        && ShouldRewriteShootAngles(thrower ? thrower : owner) && m_VR->TryGetVrShootOrigin(muzzle))
    {
        static int s_hornetLogs = 6;
        g_VrSpawnPoint = muzzle;
        g_VrSpawnWhat = type == kProjectileRpg ? "RPG rocket" : "Hornet";
        g_VrSpawnLogBudget = bmvr::g_AimDebug ? -1 : s_hornetLogs;
        g_VrSpawnMaxDist = 100.f;
        g_VrGrenadeSpawn = true;
        // RPG: launched at the laser's end point (the beam the dot sits on),
        // not the game's aim trace end, so it leaves the muzzle along the
        // line the guidance will hold it on.
        Vector rpgTarget{}, beamStart{};
        Vector* useTarget = target;
        if (type == kProjectileRpg && target && m_VR->TryGetVrBeamSegment(beamStart, rpgTarget))
        {
            static int s_rpgLogs;
            if (s_rpgLogs++ < 6)
                Game::logMsg("RPG rocket target: game (%.0f,%.0f,%.0f) -> laser end (%.0f,%.0f,%.0f)",
                    target->x, target->y, target->z, rpgTarget.x, rpgTarget.y, rpgTarget.z);
            useTarget = &rpgTarget;
        }
        void* proj = hkProjectileCreate.fOriginal(type, useTarget, speed, owner, thrower);
        g_VrGrenadeSpawn = false;
        // RPG: before ignition the rocket coasts along the player's aim
        // angles; the laser (where the guidance takes it next) can point
        // elsewhere, so it bent onto the laser line after ~0.2 s. Start it
        // along the beam at its own launch speed.
        if (proj && type == kProjectileRpg && useTarget == &rpgTarget)
        {
            if (HMODULE server = GetModuleHandleA("server.dll"))
            {
                auto* fn = reinterpret_cast<unsigned char*>(server) + Offsets::kSetAbsVelocity_Server;
                static const unsigned char kSig[] = { 0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x10, 0x05 };
                bool ok = false;
                BMVR_TRY { ok = std::memcmp(fn, kSig, sizeof(kSig)) == 0; }
                __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                Vector vel{};
                BMVR_TRY { vel = *reinterpret_cast<const Vector*>(static_cast<const char*>(proj) + 0x218); }
                __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                Vector dir = rpgTarget - muzzle;
                const float len = dir.Length();
                const float spd = vel.Length();
                if (ok && len > 1.f && spd > 1.f)
                {
                    const Vector want = dir * (spd / len);
                    BMVR_TRY { reinterpret_cast<tSetAbsVelocity>(fn)(proj, &want); }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                    static int s_velLogs;
                    if (s_velLogs++ < 6)
                        Game::logMsg("RPG rocket: launch velocity (%.0f,%.0f,%.0f) -> along the laser (%.0f,%.0f,%.0f)",
                            vel.x, vel.y, vel.z, want.x, want.y, want.z);
                }
            }
        }
        if (!bmvr::g_AimDebug)
            s_hornetLogs = g_VrSpawnLogBudget;
        g_VrSpawnLogBudget = -1;
        g_VrSpawnMaxDist = 60.f;
        return proj;
    }
    return hkProjectileCreate.fOriginal(type, target, speed, owner, thrower);
}

void __fastcall Hooks::dSnarkThrow(void* weapon, void* edx, void* owner, float fspeed, float uspeed)
{
    if (!hkSnarkThrow.fOriginal)
        return;
    Vector hand{}, from{};
    const unsigned until = m_VR ? m_VR->m_SnarkHandThrowUntil.load(std::memory_order_relaxed) : 0;
    const bool handThrow = m_VR && bmvr::g_VrHandThrow && until != 0 && GetTickCount() < until
        && ShouldRewriteShootAngles(owner) && m_VR->TryGetThrowVelocity(hand) && m_VR->TryGetThrowOrigin(from);
    if (!handThrow)
    {
        hkSnarkThrow.fOriginal(weapon, edx, owner, fspeed, uspeed);
        return;
    }
    m_VR->m_SnarkHandThrowUntil.store(0, std::memory_order_relaxed);
    Vector playerVel{};
    BMVR_TRY
    {
        constexpr int kAbsVelocity = 0x218;   // as in dGrenadeCreate (the throw reads +0x218..0x220 too)
        playerVel = *reinterpret_cast<const Vector*>(static_cast<const char*>(owner) + kAbsVelocity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_VrSpawnPoint = from;
    g_VrSpawnWhat = "VR snark throw";
    g_VrSpawnLogBudget = -1;
    g_VrSpawnMaxDist = 60.f;
    g_VrGrenadeSpawn = true;
    g_VrSnarkThrow = true;
    g_VrSnarkEntity = nullptr;
    // Zero speeds: the game's forward/up speeds reach the snark after this
    // call returns (headless flight log 2026-11-19: the hand velocity set
    // below came back with ~+780 u/s forward, so even a 0.1 scale flew at
    // ~820 u/s). Only the hand's velocity should move it.
    hkSnarkThrow.fOriginal(weapon, edx, owner, 0.f, 0.f);
    g_VrGrenadeSpawn = false;
    g_VrSnarkThrow = false;
    void* snark = g_VrSnarkEntity;
    g_VrSnarkEntity = nullptr;
    // Same hand speed as the frag felt far too strong on a snark (user
    // 2026-11-18: "almost into orbit"; its own movement carries it further).
    const Vector vel = hand * bmvr::g_SnarkThrowScale + playerVel;
    static tSetAbsVelocity s_setVel = nullptr;
    static bool s_checked = false;
    if (!s_checked)
    {
        s_checked = true;
        if (HMODULE server = GetModuleHandleA("server.dll"))
        {
            auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kSetAbsVelocity_Server;
            static const unsigned char kSig[] = { 0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x10, 0x05 };
            bool ok = false;
            BMVR_TRY { ok = std::memcmp(p, kSig, sizeof(kSig)) == 0; }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            s_setVel = ok ? reinterpret_cast<tSetAbsVelocity>(p) : nullptr;
            Game::logMsg("Snark throw: SetAbsVelocity %s", ok ? "found" : "bytes differ, game speed kept");
        }
    }
    if (snark && s_setVel)
    {
        BMVR_TRY { s_setVel(snark, &vel); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // Flight log: find the abs origin as the vector at the placed point.
    static int s_flightLogs;
    if (snark && s_flightLogs < 6)
    {
        ++s_flightLogs;
        g_SnarkTraceOrgOff = -1;
        BMVR_TRY
        {
            const char* e = static_cast<const char*>(snark);
            for (int off = 0x100; off < 0x400 && g_SnarkTraceOrgOff < 0; off += 4)
                if ((*reinterpret_cast<const Vector*>(e + off) - g_VrSpawnPoint).LengthSqr() < 16.f * 16.f)
                    g_SnarkTraceOrgOff = off;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_SnarkTraceEnt = g_SnarkTraceOrgOff >= 0 ? snark : nullptr;
        g_SnarkTraceStart = GetTickCount();
        g_SnarkTraceUntil = g_SnarkTraceStart + 2500;
        Game::logMsg("Snark flight log: origin at +0x%X", g_SnarkTraceOrgOff);
    }
    Game::logMsg("Snark throw: hand %.0f u/s x %.2f -> (%.0f,%.0f,%.0f) (game fspeed %.0f uspeed %.0f), snark %s",
        hand.Length(), bmvr::g_SnarkThrowScale, vel.x, vel.y, vel.z, fspeed, uspeed,
        snark ? "placed at the hand" : "not seen (game point and speed)");
}

// Pickup auto-switch (AutoSwitchPickup, off by default, user 2026-11-23): with it off a picked-up
// weapon only goes to the inventory, also the first time and with nothing in the hand. In single
// player the game never reads cl_weapon_autoswitch (CMultiplayRules only); setting it made the
// client skip the switch the server still did (draw sound, no gun).
bool __fastcall Hooks::dShouldSwitchWeapon(void* rules, void* edx, void* player, void* weapon)
{
    if (!hkShouldSwitchWeapon.fOriginal)
        return false;
    const bool game = hkShouldSwitchWeapon.fOriginal(rules, edx, player, weapon);
    if (game && !bmvr::g_AutoSwitchPickup)
    {
        Game::logMsg("Pickup auto-switch blocked (AutoSwitchPickup off)");
        return false;
    }
    return game;
}

// The game's auto-switches as one-byte code toggles (single atomic writes, so they can flip live):
// - AutoSwitchEmpty off: ReloadOrSwitchWeapons `test al, 4` -> `or al, 4` = every weapon has
//   ITEM_FLAG_NOAUTOSWITCHEMPTY, so an emptied weapon idles instead of SwitchToNextBestWeapon.
// - AutoSwitchPickup off: CBlackMesaBaseWeapon::Equip (0x490AE0) switches the new owner to the weapon
//   on its own (slot 0x558 decides, the game rules are not asked); `je` over that Weapon_Switch ->
//   `jmp`. Weapon_Equip's game-rules switch is blocked in dShouldSwitchWeapon.
void Hooks::ApplyAutoSwitchPatches()
{
    struct Toggle { unsigned char* p; unsigned char on, off; bool gameDefault; const char* what; };
    const Toggle toggles[] = {
        { s_AutoSwitchEmptyInsn, 0xA8, 0x0C, bmvr::g_AutoSwitchEmpty, "Empty-weapon" },
        { s_AutoSwitchEquipInsn, 0x74, 0xEB, bmvr::g_AutoSwitchPickup, "Pickup (weapon Equip)" },
    };
    for (const Toggle& t : toggles)
    {
        if (!t.p)
            continue;
        const unsigned char want = t.gameDefault ? t.on : t.off;
        if (*t.p == want)
            continue;
        DWORD old = 0;
        if (!VirtualProtect(t.p, 1, PAGE_EXECUTE_READWRITE, &old))
            continue;
        *t.p = want;
        VirtualProtect(t.p, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t.p, 1);
        Game::logMsg("%s auto-switch %s", t.what, t.gameDefault ? "on (game default)" : "off");
    }
}

bool __cdecl Hooks::dSnarkPlace(void* snark, Vector* pos, QAngle* ang, void* owner)
{
    if (!hkSnarkPlace.fOriginal)
        return false;
    if (!g_VrSnarkThrow)
        return hkSnarkPlace.fOriginal(snark, pos, ang, owner);
    g_VrSnarkEntity = snark;
    // Same hand point + wall check as the frag (dGrenadeThrowPosition).
    const auto original = hkGrenadeThrowPosition.fOriginal;
    hkGrenadeThrowPosition.fOriginal = hkSnarkPlace.fOriginal;
    const bool ok = dGrenadeThrowPosition(snark, pos, ang, owner);
    hkGrenadeThrowPosition.fOriginal = original;
    return ok;
}

bool __cdecl Hooks::dGrenadeThrowPosition(void* grenade, Vector* pos, QAngle* ang, void* owner)
{
    if (!hkGrenadeThrowPosition.fOriginal)
        return false;
    const bool ok = hkGrenadeThrowPosition.fOriginal(grenade, pos, ang, owner);
    if (!ok || !g_VrGrenadeSpawn || !pos || !m_VR)
        return ok;
    const Vector hand = g_VrSpawnPoint;
    const bool log = g_VrSpawnLogBudget != 0;
    if (g_VrSpawnLogBudget > 0)
        --g_VrSpawnLogBudget;
    // The game's point is a little ahead of the eye after a trace from the
    // eye; a hand far from it (arm through a wall, stale pose) keeps it.
    if ((hand - *pos).LengthSqr() > g_VrSpawnMaxDist * g_VrSpawnMaxDist)
    {
        if (log)
            Game::logMsg("%s: spawn point %.0f units from the game's point, kept the game's", g_VrSpawnWhat, (hand - *pos).Length());
        return ok;
    }
    // The game's point passed its own trace from the eye. A hand reaching
    // through a wall, a door or a prop must not spawn the grenade on the
    // other side: trace from the game's point to the hand and keep the
    // game's point if anything solid is in between. (Server and client run
    // on the game thread in single player, so the client trace is safe here.)
    if (m_Game && m_Game->m_EngineTrace)
    {
        C_BaseEntity* player = m_Game->GetLocalPlayerEntity();
        C_BaseEntity* vm = m_Game->GetViewModelEntity();
        CTraceFilterSkipTwoEntities filter(player, vm, 0);
        Ray_t ray;
        ray.Init(*pos, hand);
        CGameTrace tr{};
        m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT_HULL, &filter, &tr);
        if (tr.startsolid || tr.fraction < 1.f)
        {
            if (log)
                Game::logMsg("%s: spawn point behind geometry (fraction %.2f), kept the game's point", g_VrSpawnWhat, tr.fraction);
            return ok;
        }
    }
    if (log)
        Game::logMsg("%s: spawned at the VR point, %.0f units from the game's point", g_VrSpawnWhat, (hand - *pos).Length());
    *pos = hand;
    return ok;
}
