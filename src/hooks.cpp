#include "hooks.h"
#include "game.h"
#include "sdk.h"
#include "vr.h"
#include "offsets.h"
#include "bmvr_flags.h"
#include "sigscanner.h"
#include "in_buttons.h"
#include "trace.h"
#include "texture.h"
#include <Windows.h>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
    void** SehMaterialSystemVTable(IMaterialSystem* mat)
    {
        void** vt = nullptr;
        if (!mat)
            return nullptr;
        __try
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
        __try
        {
            map = eng->GetLevelNameShort();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            map = nullptr;
        }
        if (map && map[0])
            return map;
        __try
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
    enableIfReady(hkCreateNamedRTEx, "CreateNamedRTEx");
    enableIfReady(hkEndRTAlloc, "EndRTAlloc");
    enableIfReady(hkDrawFilledRect, "DrawFilledRect");
    enableIfReady(hkDrawTexturedRect, "DrawTexturedRect");
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
            __try
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
    // background01 (docs/RUNTIME.md). Map names come from GetLevelNameShort.
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
            return;
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
        __try
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
        __try
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
        __try
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
        __try
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
    };
    constexpr int kMaxStudioHdrPatches = 96;
    StudioHdrPatch g_ArmHdrPatches[kMaxStudioHdrPatches]{};
    int g_ArmHdrPatchCount = 0;
    constexpr int kVmDrawSlots = 4;
    thread_local int g_VmDrawSlot = 0;
    thread_local float g_ScaledViewmodelBones[kVmDrawSlots][kMaxStudioBones][3][4];
    thread_local float g_ScaledViewmodelModelToWorld[kVmDrawSlots][3][4];
    thread_local unsigned char g_ScaledViewmodelInfo[kVmDrawSlots][sizeof(ModelRenderInfo_t)];
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
        __try
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

    void ForceViewmodelIdlePose(void* viewmodel, unsigned char* hdr)
    {
        if (!viewmodel)
            return;
        const int idle = FindIdleSequence(hdr);
        __try
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
        __try
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
        __try
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
        __try
        {
            labelOff = *reinterpret_cast<int*>(desc + kStudioSeqLabelIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        if (labelOff <= 0)
            return nullptr;
        return reinterpret_cast<const char*>(desc + labelOff);
    }

    void SuppressViewmodelMovementAnims(void* viewmodel)
    {
        if (!viewmodel || !Hooks::m_VR || !Hooks::m_VR->IsGameplayEligible())
            return;
        unsigned char* hdr = g_LastViewmodelStudioHdr;
        int seq = 0;
        __try
        {
            seq = *reinterpret_cast<int*>(static_cast<char*>(viewmodel) + kViewmodelSequence);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        const char* label = StudioSequenceLabel(hdr, seq);
        const bool melee = Hooks::m_VR->IsPerformingMelee();
        // Labels live inside the studiohdr, so the pointer identifies the
        // sequence; classify once per sequence change instead of three
        // lowercase passes + ~30 strstr on every CalcViewModelView.
        struct LabelClass { const char* label; char text[16]; bool action; bool loco; bool equip; };
        static LabelClass s_lc{};
        if (!label || label != s_lc.label
            || std::strncmp(s_lc.text, label, sizeof(s_lc.text) - 1) != 0)
        {
            s_lc.label = label;
            s_lc.text[0] = 0;
            if (label)
                strncpy_s(s_lc.text, label, _TRUNCATE);
            s_lc.action = SequenceLabelLooksLikeWeaponAction(label);
            s_lc.loco = SequenceLabelLooksLikeLocomotionOnly(label);
            s_lc.equip = SequenceLabelLooksLikeEquipOnly(label);
        }
        const bool isAction = s_lc.action;
        const bool isLoco = s_lc.loco;
        // Lock-free class flag from the weapon model dCalcViewModelView just
        // classified; replaces a model-info lookup + four strstr per call (this
        // runs twice per CalcViewModelView) and an unlocked read of the
        // std::string m_LastViewmodelModel that ProcessInput rewrites.
        const bool crowbar = Hooks::m_VR->ViewmodelIsCrowbar();

        auto restoreRate = [&]() {
            __try
            {
                float* rate = reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate);
                if (*rate <= 0.01f)
                    *rate = 1.f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        };
        auto freeze = [&]() {
            __try
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
        __try
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
        __try
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
        __try { n = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (n < 1 || n > kMaxStudioBones)
            return 0;
        return n;
    }

    bool SehCopyAndFixViewmodelMatrices(float* dst, const void* src, int count,
        const float pivot[3], float scale,
        const float yFixOrigin[3], const float right[3], const float up[3], const float fwd[3],
        float yFix, const float* rigidDelta)
    {
        if (!dst || !src || count < 1)
            return false;
        __try
        {
            std::memcpy(dst, src, static_cast<size_t>(count) * 12 * sizeof(float));
            for (int i = 0; i < count; ++i)
            {
                float* m = dst + i * 12;
                auto matrix = reinterpret_cast<float(*)[4]>(m);
                if (rigidDelta)
                    ApplyMatrix3x4Delta(matrix, reinterpret_cast<const float(*)[4]>(rigidDelta));
                if (yFix > 0.2f && yFix < 2.f && fabsf(yFix - 1.f) > 0.03f)
                    UnstretchMatrix3x4ViewY(matrix, yFixOrigin, right, up, fwd, yFix);
                if (scale > 0.2f && scale < 1.5f && fabsf(scale - 1.f) > 0.001f)
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
        __try
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
        __try
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
        __try
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

    void RememberHdrPatch(int* p, int original)
    {
        if (!p || g_ArmHdrPatchCount >= kMaxStudioHdrPatches)
            return;
        for (int i = 0; i < g_ArmHdrPatchCount; ++i)
        {
            if (g_ArmHdrPatches[i].p == p)
                return;
        }
        g_ArmHdrPatches[g_ArmHdrPatchCount].p = p;
        g_ArmHdrPatches[g_ArmHdrPatchCount].original = original;
        ++g_ArmHdrPatchCount;
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

    void NoteWeaponRootFromHdr(unsigned char* hdr, const char* modelName)
    {
        if (!hdr || !Hooks::m_VR)
            return;
        int length = 0;
        int numbones = 0;
        int boneindex = 0;
        __try
        {
            length = *reinterpret_cast<int*>(hdr + kStudioHdrLength);
            numbones = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones);
            boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (length < 256 || length > 4 * 1024 * 1024)
            return;
        if (numbones <= 0 || numbones > 256)
            return;
        if (boneindex < 0 || boneindex + numbones * kStudioBoneSize > length)
            return;

        int bestScore = -1;
        int bestIndex = -1;
        float rest[3]{};
        char bestName[64]{};
        for (int i = 0; i < numbones; ++i)
        {
            unsigned char* bone = hdr + boneindex + i * kStudioBoneSize;
            int parent = -2;
            int szname = 0;
            float pos[3]{};
            __try
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
            return;
        Hooks::m_VR->NoteViewmodelWeaponBake(modelName, bestName, rest[0], rest[1], rest[2]);
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
            __try
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
        __try
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
            __try
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
            if (_stricmp(bpName, "arms") != 0)
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
                __try { nMesh = *pMeshes; }
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
                RememberHdrPatch(pMeshes, nMesh);
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
            g_LastViewmodelStudioHdr = hdr;
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
        int n = 0;
        for (int i = 0; i < g_ArmHdrPatchCount; ++i)
        {
            if (g_ArmHdrPatches[i].p)
            {
                PokeInt(g_ArmHdrPatches[i].p, g_ArmHdrPatches[i].original);
                ++n;
            }
            g_ArmHdrPatches[i] = {};
        }
        if (g_ArmHdrPatchCount)
            Game::logMsg("HideViewmodelArms restored %d studiohdr nummeshes", n);
        g_ArmHdrPatchCount = 0;
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

    void ContextRelease(IMatRenderContext* ctx)
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
        auto setOrigin = reinterpret_cast<SetVecFn>(
            reinterpret_cast<uintptr_t>(client) + Offsets::kCBaseEntity_SetAbsOrigin);
        auto setAngles = reinterpret_cast<SetVecFn>(
            reinterpret_cast<uintptr_t>(client) + Offsets::kCBaseEntity_SetAbsAngles);
        __try
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
        __try
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
        __try
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
        __try
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

    void ClearEffectViewModelFlag(void* effect)
    {
        if (!effect)
            return;
        __try
        {
            *(static_cast<unsigned char*>(effect) + Offsets::kCNewParticleEffect_ViewModel) = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void PatchGluonImpactParticleDefs()
    {
        if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled)
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
        auto* find = reinterpret_cast<tFind>(
            reinterpret_cast<unsigned char*>(client) + Offsets::kParticleSystemMgr_Find);
        void* mgr = nullptr;
        __try
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
            __try
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
        __try
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
        __try
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
            __try
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
            __try
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
        __try
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

    void NormalizeViewSetupForVREye(CViewSetup& view, const VR* vr)
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
        // fl_gbmatch: keep engine 2560 width/height so deferred flashlight
        // apply matches the G-buffer. Still write HMD fov/aspect (below).
        if (!bmvr::UseGbMatchViewLock())
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
        // L4D2VR: native Source viewmodels use a separate projection and a
        // compressed 0..0.1 depth range so they draw over the world. In VR the
        // gun is a world-space mesh; keep Z (and therefore size vs world
        // geometry) on the same projection as the eye scene.
        if (view.zNear > 0.01f)
            view.zNearViewmodel = view.zNear;
        if (view.zFar > view.zNear)
            view.zFarViewmodel = view.zFar;
    }

    bool IsOffscreenWorldRtName(const char* name)
    {
        if (!name || !name[0] || name[0] == '?')
            return false;
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

    bool OffscreenStereoSizeLie(int& width, int& height)
    {
        if (!bmvr::OffscreenWorldMatchesEyes() || !Hooks::m_VR)
            return false;
        if (Hooks::m_VR->HudPaintActive())
            return false;
        // Flashlight/CSM stay aux so their 8192 views keep atlas size. The
        // deferred apply Viewport/GetScreenSize(2560) while that shadow is
        // still parent, but D3D RT0 is already the 3168 G-buffer.
        if (AuxSceneRtBound() && !RtStackTopIsWorldScene()
            && !Hooks::m_VR->D3dRt0IsEyeSized())
            return false;
        if (McLeftoverRecording())
            return false;
        const bool stereo = Hooks::m_VR->StereoWorldPassActive();
        if (!stereo && !Hooks::m_VR->CachedRt0MatchesEyes()
            && !Hooks::m_VR->D3dRt0IsEyeSized())
            return false;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return false;
        width = eyeW;
        height = eyeH;
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
        // Flashlight apply PushRT/Viewport is 2560 (G-buffer actual). Forcing
        // 1584 here is why the beam never landed in fused eyes. Keep engine
        // size; squash-blit after RenderView.
        if (bmvr::UseGbMatchViewLock())
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
        __try
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
        return StereoEyeWorldActive() || Hooks::m_VR->IsGameplayEligible();
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
    // god rays).
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
            || std::strstr(name, "blur_combine") != nullptr;
    }

    bool QueryTextureSize(ITexture* texture, int& w, int& h)
    {
        w = 0;
        h = 0;
        if (!texture)
            return false;
        __try
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
        __try
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
        if (!vr->WantPauseWorldOverlay())
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
        vr->SetHudPaintActive(true);
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
        if (scope.ctx && Hooks::hkViewport.fOriginal)
            Hooks::hkViewport.fOriginal(scope.ctx, 0, 0, static_cast<int>(tw), static_cast<int>(th));
        // HL2VR RenderHUD: full dest, alpha write, clear 0,0,0,0,
        // then PAINT_UIPANELS. Software CURSOR fights the ColorFill arrow
        // and strobes. D3D bind replaces named PushRT.
        if (!queued)
            vr->PreparePauseHudForVgui(scope.ctx);
        const int paintMode = PAINT_UIPANELS;
        ++g_VguiOverlayReentry;
        Hooks::hkVgui_Paint.fOriginal(vgui, paintMode);
        --g_VguiOverlayReentry;
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
    (void)edx;
    NoteMatContext(ecx);
    return hkGetRenderTarget.fOriginal(ecx);
}

void __fastcall Hooks::dRenderView(void* ecx, void* edx, CViewSetup& setup, int nClearFlags, int whatToDraw)
{
    (void)edx;
    RenderViewNestScope nest;
    CurrentViewScope curView(setup);
    if (!hkRenderView.fOriginal)
        return;

    if (g_RenderViewNest > 1)
    {
        hkRenderView.fOriginal(ecx, setup, nClearFlags, whatToDraw);
        return;
    }

    auto callOriginal = [&](CViewSetup& view, int clearFlags, int drawFlags) {
        hkRenderView.fOriginal(ecx, view, clearFlags, drawFlags);
    };

    if (m_VR)
        m_VR->FlushPendingGameUi();

    EnsureServerFlashlightHook();
    if (m_VR)
    {
        m_VR->ApplyRenderTargetFramebufferOverride();
        m_VR->LogFullFrameSizeIfReady();
    }

    const bool queued = m_Game && m_Game->GetMatQueueMode() != 0;
    SourceRenderQueueBuildScope sourceRenderQueueBuildScope(m_VR, queued);

    const bool mainView = setup.width >= 640 && setup.height >= 360;
    const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();

    const bool originOk = std::isfinite(setup.origin.x) && std::isfinite(setup.origin.y)
        && std::isfinite(setup.origin.z)
        && fabsf(setup.origin.x) < 100000.f && fabsf(setup.origin.y) < 100000.f;
    const bool fovOk = std::isfinite(setup.fov) && setup.fov > 10.f && setup.fov < 170.f;

    if (m_VR && mainView)
        m_VR->m_SetupOrigin = setup.origin;

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
            CViewSetup vrView = setup;
            applyL4d2VrHead(vrView, false, true);

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
        && fovOk
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
        && fovOk
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
                return;
            }
            // Leftover policy: gbmatch + gb_leftskip skips the 16:9 desktop
            // main (2 scene renders). gbmatch without the skip renders it.
            // No-gbmatch uses DesktopLeftoverRender (default off).
            const bool gbSkipLeftover = bmvr::TryFlashlightGbMatch() && bmvr::TryGbLeftSkip();
            const bool skipLeftover = bmvr::OffscreenWorldMatchesEyes()
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
        NormalizeViewSetupForVREye(leftEyeView, m_VR);
        NormalizeViewSetupForVREye(rightEyeView, m_VR);
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

        static int s_hmdFbOnce;
        if (s_hmdFbOnce == 0)
        {
            s_hmdFbOnce = 1;
            bmvr::BeginRisky(L"hmd_fb");
            if (bmvr::TryOffscreenHmd())
                bmvr::BeginRisky(L"hmd_offscreen");
            if (bmvr::TryOffscreenWorldGrow())
                bmvr::BeginRisky(L"hmd_world");
            if (bmvr::TrySteamVrEyeRt())
                bmvr::BeginRisky(L"steamvr_rt");
            if (bmvr::UseGbMatchViewLock())
                bmvr::BeginRisky(L"fl_gbmatch");
            Game::logMsg("Stereo HMD-fb begin setup=%dx%d unscaled=%dx%d stereoEye=%d eye=%dx%d fov=%.1f->%.1f aspect=%.3f->%.3f zNear=%.1f ipd=%.2f L=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) steamvr_rt=%d offscreen=%d worldMatch=%d gbmatch=%d",
                setup.width, setup.height, setup.m_nUnscaledWidth, setup.m_nUnscaledHeight,
                setup.m_eStereoEye, eyeW, eyeH,
                setup.fov, leftEyeView.fov, setup.m_flAspectRatio, leftEyeView.m_flAspectRatio,
                leftEyeView.zNear, ipd,
                leftEyeView.origin.x, leftEyeView.origin.y, leftEyeView.origin.z,
                rightEyeView.origin.x, rightEyeView.origin.y, rightEyeView.origin.z,
                bmvr::TrySteamVrEyeRt() ? 1 : 0,
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
        g_Cost.eye = 1;
        m_VR->m_StereoEye = 1;
        const bool mcLeft = mc && m_VR->QueueMcEyeBind(1, false);
        if (!mcLeft)
        {
            m_VR->BeginStereoEyeBlit(m_VR->m_D9LeftEyeSurface);
            m_VR->ClearStereoEyeSurfaces();
        }
        {
            const int eyeDraw = whatToDraw & ~kRenderViewDrawHud;
            const long long t0 = QpcNow();
            callOriginal(leftEyeView, nClearFlags, eyeDraw);
            g_Cost.leftTicks += QpcNow() - t0;
        }
        if (!mcLeft)
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
                && (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes || bmvr::g_HandHud
                || m_VR->WeaponMenuOpen() || m_VR->AimCrosshairVisible()))
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
        const bool mcRight = mc && m_VR->QueueMcEyeBind(2, true);
        if (!mcRight)
        {
            m_VR->BeginStereoEyeBlit(m_VR->m_D9RightEyeSurface);
            m_VR->ClearStereoEyeSurfaces();
        }
        {
            const int eyeDraw = whatToDraw & ~kRenderViewDrawHud;
            const long long t0 = QpcNow();
            callOriginal(rightEyeView, nClearFlags, eyeDraw);
            g_Cost.rightTicks += QpcNow() - t0;
        }
        if (!mcRight)
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
                && (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes || bmvr::g_HandHud
                || m_VR->WeaponMenuOpen() || m_VR->AimCrosshairVisible()))
            {
                const long long t0 = QpcNow();
                m_VR->DrawIndependentHandMarkers(
                    m_VR->ColorTargetForStereoEye(2), 2, true, !glovesInBlit);
                g_Cost.handsTicks += QpcNow() - t0;
            }
        }
        } // !mc right post-RV
        m_VR->m_StereoEye = 0;
        if (mcLeft && mcRight)
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
        if (!(mcLeft && mcRight))
        {
        if (bmvr::OffscreenWorldMatchesEyes())
            m_VR->MirrorStereoToDesktopWindow();
        else if (m_VR->m_IsVREnabled && !m_VR->WantPauseWorldOverlay())
            m_VR->ClearUnusedDesktopBackbuffer();
        }
        // gbmatch already draws flashlight in the eye passes. A third window
        // DRAWHUD pass was a 15fps regression. Keep it only for the fused
        // fallback (eyes stripped HUD).
        const bool runDrawHudPass = !(mcLeft && mcRight) && !bmvr::TryFlashlightGbMatch() && bmvr::TryDrawHud();
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
        if (!(mcLeft && mcRight)
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
            if (bmvr::TrySteamVrEyeRt())
                bmvr::EndRisky(L"steamvr_rt");
            if (bmvr::TryFullFrameStereo())
                bmvr::EndRisky(L"ff_stereo");
            if (bmvr::TryHmdFitFullFrame())
                bmvr::EndRisky(L"ff_hmdfit");
            if (bmvr::TryEyeFitWorldRts())
                bmvr::EndRisky(L"ff_gbfit");
            if (bmvr::TryFlashlightGbMatch())
                bmvr::EndRisky(L"fl_gbmatch");
            if (bmvr::TryGbLeftSkip())
                bmvr::EndRisky(L"gb_leftskip");
        }
        return;
    }

    passThrough();
}

bool __fastcall Hooks::dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd)
{
    (void)edx;
    if (!hkCreateMove.fOriginal)
        return false;
    if (!cmd)
        return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    // Yaw actually written to cmd->viewangles this tick. Movement is expressed
    // relative to viewangles, so the stick-to-usercmd conversion below has to
    // use the same yaw or strafing skews whenever the aim source changes.
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
        // A live crowbar swing overrides the aim so the engine's melee trace
        // follows the blade instead of the controller's forward ray.
        Vector meleeOrigin{};
        QAngle meleeAim{};
        if (m_VR->TryGetMeleeAim(meleeOrigin, meleeAim))
        {
            apply(meleeAim);
            return;
        }
        // Pickup follow uses the grab hand. Weapon fire / flashlight must not:
        // leftover left-Use latch used to keep viewangles and shoot origin on
        // that hand after a door, button, or charger.
        if (m_VR->UseGrabRedirectsFire() && m_VR->GrabHandTrackingValid())
        {
            apply(m_VR->GetUseAimAngles());
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
    if (m_VR && m_VR->EmptyHands())
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
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
        }
        else
        {
            cmd->forwardmove += analogF;
            cmd->sidemove += analogS;
        }
        cmd->buttons |= static_cast<int>(m_VR->HeldButtons());
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

    bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    if (m_VR && cmd->command_number && m_VR->IsGameplayEligible())
        m_VR->m_SeenGameplay = true;

    if (!m_VR || !m_VR->m_IsVREnabled || !cmd->command_number)
        return result;

    // Camera stays HMD on RenderView copies. Shooting uses controller
    // viewangles. Do not hook EyePosition (that would move the camera).
    applyControllerAim();
    if (m_VR)
        ZeroPlayerViewRecoil(localPlayer);
    if (m_VR && m_VR->EmptyHands())
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
    else
        cmd->buttons |= static_cast<int>(m_VR->HeldButtons());

    EnsureServerFlashlightHook();
    EnsureWeaponShootOriginHooks();

    m_VR->AfterCreateMoveFireHaptics();
    return result;
}

void __fastcall Hooks::dLevelInit(void* ecx, void* edx, const char* newmap)
{
    (void)edx;
    EnsureServerFlashlightHook();
    EnsureClientFlashlightHook();
    EnsureWeaponShootOriginHooks();
    if (m_VR)
    {
        m_VR->OnLevelInit(newmap);
        m_VR->ApplyRenderTargetFramebufferOverride();
    }
    if (hkLevelInit.fOriginal)
        hkLevelInit.fOriginal(ecx, newmap);
}

void __fastcall Hooks::dLevelShutdown(void* ecx, void* edx)
{
    (void)edx;
    if (m_VR)
        m_VR->OnLevelShutdown();
    if (hkLevelShutdown.fOriginal)
        hkLevelShutdown.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
    (void)edx;
    if (!hkCalcViewModelView.fOriginal)
        return;
    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && EngineInGame() && m_VR->m_HmdPoseValid)
    {
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
            const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos(eyePosition);
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
        const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos(eyePosition);
        const QAngle targetAng = m_VR->GetRecommendedViewmodelAbsAngle();
        CallCalcViewModelViewOriginal(ecx, owner, targetOrigin, targetAng);
        const float origin3[3] = { targetOrigin.x, targetOrigin.y, targetOrigin.z };
        const float angles3[3] = { targetAng.x, targetAng.y, targetAng.z };
        CallSetAbsOriginAngles(ecx, origin3, angles3);
        return;
    }
    hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
}

float __fastcall Hooks::dGetViewModelFOV(void* ecx, void* edx)
{
    (void)edx;
    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && EngineInGame() && m_VR->m_Fov > 10.f)
        return m_VR->WorldRenderFov();
    if (hkGetViewModelFOV.fOriginal)
        return hkGetViewModelFOV.fOriginal(ecx);
    return 54.f;
}

void __fastcall Hooks::dAdjustEngineViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    if (hkAdjustEngineViewport.fOriginal)
        hkAdjustEngineViewport.fOriginal(ecx, x, y, width, height);
    ClampStereoViewport(x, y, width, height);
}

void __fastcall Hooks::dViewport(void* ecx, void* edx, int x, int y, int width, int height)
{
    NoteMatContext(ecx);
    ClampStereoViewport(x, y, width, height);
    if (hkViewport.fOriginal)
        hkViewport.fOriginal(ecx, x, y, width, height);
}

void __fastcall Hooks::dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    (void)edx;
    NoteMatContext(ecx);
    CallOriginalGetViewport(ecx, x, y, width, height);
    ClampStereoViewport(x, y, width, height);
}

void __fastcall Hooks::dGetRenderTargetDimensions(void* ecx, void* edx, int& width, int& height)
{
    (void)edx;
    NoteMatContext(ecx);
    const bool queuedStub = CallOriginalGetRenderTargetDimensions(ecx, width, height);
    ApplyHwndStereoRtDimLie(width, height, queuedStub);
}

void __fastcall Hooks::dDepthRange(void* ecx, void* edx, float zNear, float zFar)
{
    (void)edx;
    NoteMatContext(ecx);
    if (StereoViewmodelUsesWorldDepth() && IsCompressedViewmodelDepthRange(zNear, zFar))
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
    __try
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
        hideArms = bmvr::g_HideViewmodelArms
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

    if (isViewmodel && m_VR && m_VR->IsGameplayEligible() && EngineInGame())
        ApplyViewmodelStudioWork(state, modelName, body, skin, hideArms, pCustomBoneToWorld);

    void* bonesToDraw = pCustomBoneToWorld;
    const ModelRenderInfo_t* infoToDraw = &info;
    if (isViewmodel && m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
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
            bool usedBones = false;
            if (pCustomBoneToWorld && nBones > 0
                && SehCopyAndFixViewmodelMatrices(&g_ScaledViewmodelBones[slot][0][0][0], pCustomBoneToWorld,
                    nBones, pivot3, needScale ? scale : 1.f, pivot3, right3, up3, fwd3,
                    needYFix ? yFix : 1.f, rigidPtr))
            {
                bonesToDraw = g_ScaledViewmodelBones[slot];
                usedBones = true;
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
        __try
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

void __fastcall Hooks::dEndFrame(void* ecx, void* edx)
{
    (void)edx;
    if (hkEndFrame.fOriginal)
        hkEndFrame.fOriginal(ecx);
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
    NoteMatContext(ecx);
    const char* pushName = SafeTextureName(pTexture);
    const RtNameClass nameClass = ClassifyRtName(pushName);
    // Grow world G-buffer viewports before NotePushRt so the stack records
    // 3168, and so a `_rt_gb*` push while flashlight is still parent is not
    // skipped by AuxSceneRtBound().
    if (m_VR && bmvr::OffscreenWorldMatchesEyes()
        && !m_VR->HudPaintActive()
        && (m_VR->StereoWorldPassActive()
            || (m_VR->IsGameplayEligible() && !m_VR->WantPauseWorldOverlay())))
    {
        const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(m_VR->m_RenderHeight);
        const bool namedWorld = nameClass.world;
        const bool backbuffer = (pTexture == nullptr)
            || (pushName && (std::strcmp(pushName, "null") == 0));
        const bool smallVp = nViewW > 0 && nViewH > 0 && (nViewW < 640 || nViewH < 360);
        const bool squareAux = nViewW > 0 && nViewH > 0
            && std::abs(nViewW - nViewH) < 32 && nViewW + 32 < eyeW;
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
    if (hkPushRenderTargetAndViewport.fOriginal)
        hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    if (m_VR && !m_VR->HudPaintActive() && nViewW > 0 && nViewH > 0
        && static_cast<UINT>(nViewW) == m_VR->m_RenderWidth
        && static_cast<UINT>(nViewH) == m_VR->m_RenderHeight)
        m_VR->NoteCachedRt0Size(static_cast<UINT>(nViewW), static_cast<UINT>(nViewH));
}

void __fastcall Hooks::dPopRenderTargetAndViewport(void* ecx, void* edx)
{
    if (m_VR && m_VR->EngineHudRtPushed())
        m_VR->BlitEngineHudRtToOverlay();
    if (hkPopRenderTargetAndViewport.fOriginal)
        hkPopRenderTargetAndViewport.fOriginal(ecx);
    NotePopRt();
}

void __fastcall Hooks::dDrawScreenSpaceRectangle(void* ecx, void* edx, IMaterial* material,
    int destX, int destY, int width, int height,
    float srcX0, float srcY0, float srcX1, float srcY1,
    int srcWidth, int srcHeight, void* clientRenderable, int xDice, int yDice)
{
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
    if (StereoEyeWorldActive() && StereoBloomMaterial(matName))
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
    if (OffscreenEyePassActive() && (windowDest || windowUv)
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
    if (hkDrawScreenSpaceRectangle.fOriginal)
        hkDrawScreenSpaceRectangle.fOriginal(ecx, material, destX, destY, width, height,
            srcX0, srcY0, srcX1, srcY1, srcWidth, srcHeight, clientRenderable, xDice, yDice);
}

void __fastcall Hooks::dCopyRenderTargetToTextureEx(void* ecx, void* edx, ITexture* texture,
    int renderTargetId, SourceRect_t* srcRect, SourceRect_t* dstRect)
{
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
    if (srcRect && windowSrc && !destWindow && srcEye && OffscreenEyePassActive())
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
    if (hkCopyRenderTargetToTextureEx.fOriginal)
        hkCopyRenderTargetToTextureEx.fOriginal(ecx, texture, renderTargetId, srcRect, dstRect);
}

void __fastcall Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
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
    if (inGame && eligible && m_VR && m_VR->WantPauseWorldOverlay()
        && !m_VR->Want2dMenuPanel())
        PaintVguiToOverlay(ecx, mode);
}

void __fastcall Hooks::dTraceRay(void* ecx, void* edx, const Ray_t& ray, unsigned int fMask, CTraceFilter* filter, CGameTrace* pTrace)
{
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
    __try
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
    if (hkImpulseCommands.fOriginal)
        hkImpulseCommands.fOriginal(ecx);
}

void Hooks::EnsureClientFlashlightHook()
{
    if (hkUpdateFlashlightState.pTarget)
        return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client)
        return;
    auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kUpdateFlashlightState;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
    {
        Game::logMsg("UpdateFlashlightState RVA 0x%X not executable", Offsets::kUpdateFlashlightState);
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

void __fastcall Hooks::dUpdateFlashlightState(void* ecx, void* edx, void* flashlightState)
{
    (void)edx;
    // Desktop beam works; HMD missed it because FlashlightState_t is filled
    // from player EyePosition/EyeAngles (body) while stereo cameras use the
    // HMD (research/resolution-hud-flashlight.md §8). Retarget to HMD view.
            if (flashlightState && m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
        && m_VR->m_HmdPoseValid && EngineInGame())
    {
        __try
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

    unsigned char* FindPatternInModule(HMODULE mod, const char* hex)
    {
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
                return base + i;
        }
        return nullptr;
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
        return FindPatternInModule(mod, hex);
    }

    int ServerEntityIndex(void* ent)
    {
        if (!ent)
            return 0;
        int idx = 0;
        __try
        {
            unsigned char* net = reinterpret_cast<unsigned char*>(ent) + 8;
            void** vt = *reinterpret_cast<void***>(net);
            using Fn = int(__thiscall*)(void*);
            idx = reinterpret_cast<Fn>(vt[0x24 / 4])(net);
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

    Vector* RewriteShootOrigin(void* player, Vector* out, tWeaponShootPosition original, bool grabHold)
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
            *out = muzzle;
            return out;
        }
        if (Hooks::m_VR->ScopeZoomActive())
            return out;
        if (!Hooks::m_VR->TryGetVrShootOrigin(muzzle))
            return out;
        static int s_log;
        if (s_log < 8)
        {
            Game::logMsg("Weapon_ShootPosition VR muzzle (%.1f,%.1f,%.1f) was (%.1f,%.1f,%.1f)",
                muzzle.x, muzzle.y, muzzle.z, out->x, out->y, out->z);
            ++s_log;
        }
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
            hkGetAttachmentVec.pTarget = p;
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
            hkServerGrabHoldOrigin.pTarget = p;
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
            hkGrabSetTarget.pTarget = p;
        }
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
            hkGetShootAngles.pTarget = p;
        }
    }
    if (client && !hkClientGetShootAngles.pTarget)
    {
        // Same GetShootAngles body as server (EyeAngles vtable +0x234, then punch).
        constexpr const char* kShootAngBytes = "558BEC568BF18B06FF90340200008B5508";
        auto* p = FindPatternInModule(client, kShootAngBytes);
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
            Game::logMsg("client GetShootAngles pattern not found");
            hkClientGetShootAngles.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCBasePlayer_Weapon_ShootPosition;
        }
    }
    const bool vfxDone = EnsureWeaponVfxHooks();
    s_done = vfxDone
        && hkClientWeaponShootPosition.pTarget && hkGetAttachmentVec.pTarget
        && hkGetAttachmentMatrix.pTarget && hkServerWeaponShootPosition.pTarget
        && hkServerGrabHoldOrigin.pTarget && hkGrabSetTarget.pTarget
        && hkGetShootAngles.pTarget && hkClientGetShootAngles.pTarget;
}

void __fastcall Hooks::dGetShootAngles(void* ecx, void* edx, QAngle* out)
{
    (void)edx;
    if (!hkGetShootAngles.fOriginal)
        return;
    hkGetShootAngles.fOriginal(ecx, out);
    ApplyVrShootAngles(ecx, out);
}

void __fastcall Hooks::dClientGetShootAngles(void* ecx, void* edx, QAngle* out)
{
    (void)edx;
    if (hkClientGetShootAngles.fOriginal)
        hkClientGetShootAngles.fOriginal(ecx, out);
    ApplyVrShootAngles(ecx, out);
}

Vector* __fastcall Hooks::dClientWeaponShootPosition(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    return RewriteShootOrigin(ecx, out, hkClientWeaponShootPosition.fOriginal, false);
}

Vector* __fastcall Hooks::dServerWeaponShootPosition(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    return RewriteShootOrigin(ecx, out, hkServerWeaponShootPosition.fOriginal, false);
}

Vector* __fastcall Hooks::dServerGrabHoldOrigin(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    return RewriteShootOrigin(ecx, out, hkServerGrabHoldOrigin.fOriginal, true);
}

void __fastcall Hooks::dGrabSetTarget(void* ecx, void* edx, Vector* pos, QAngle* ang)
{
    (void)edx;
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

int __fastcall Hooks::dGetAttachmentVec(void* ecx, void* edx, int number, Vector* origin, QAngle* angles)
{
    (void)edx;
    int result = 0;
    if (hkGetAttachmentVec.fOriginal)
        result = hkGetAttachmentVec.fOriginal(ecx, number, origin, angles);
    if (result && origin && m_VR)
        m_VR->ScaleViewmodelRenderableAttachment(ecx, *origin);
    return result;
}

int __fastcall Hooks::dGetAttachmentMatrix(void* ecx, void* edx, int number, float* matrix)
{
    (void)edx;
    int result = 0;
    if (hkGetAttachmentMatrix.fOriginal)
        result = hkGetAttachmentMatrix.fOriginal(ecx, number, matrix);
    if (result && matrix && m_VR)
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
            hkSpriteRenderableDraw.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kCSpriteRenderable_Draw;
        }
    }
    if (!hkEnvLaserDotDraw.pTarget)
    {
        constexpr const char* kDotBytes =
            "558BEC83ECA0A1????????33C58945FCF745080000004E";
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
            hkSpriteQuad.pTarget = reinterpret_cast<unsigned char*>(client)
                + Offsets::kSpriteQuad;
        }
    }
    s_done = hkTauBeamView.pTarget && hkTauBeamWorld.pTarget && hkTauBeamFireTrace.pTarget
        && hkGluonImpactTrace.pTarget && hkGluonBeamUpdate.pTarget && hkGluonBeamFxSet.pTarget
        && hkGluonBeamFxDraw.pTarget && hkParticleSetControlPoint.pTarget
        && hkParticleMgrAddEffect.pTarget && hkRpgUpdateLaser.pTarget
        && hkHudCrosshairPaint.pTarget && hkSpriteRendererDraw.pTarget
        && hkViewRenderBeamsDraw.pTarget && hkSpriteRenderableDraw.pTarget
        && hkEnvLaserDotDraw.pTarget && hkSpriteQuad.pTarget;
    return s_done;
}

void __fastcall Hooks::dTauBeamView(void* ecx, void* edx, Vector* startEnd, void* a2, float a3, int a4, int a5)
{
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
    if (hkParticleSetControlPoint.fOriginal)
        hkParticleSetControlPoint.fOriginal(ecx, index, origin);
}

void __fastcall Hooks::dParticleMgrAddEffect(void* ecx, void* edx, void* effect)
{
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
        __try
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
        __try
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

void __cdecl Hooks::dSpriteQuad(float* origin, float width, float height, unsigned color)
{
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

void __fastcall Hooks::dGetBackBufferDimensions(void* ecx, void* edx, int& width, int& height)
{
    (void)edx;
    if (hkGetBackBufferDimensions.fOriginal)
        hkGetBackBufferDimensions.fOriginal(ecx, width, height);
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
    if (bmvr::TryHmdFitFullFrame()
        && m_VR && !NestedRenderView() && !AuxSceneRtBound()
        && m_VR->StereoWorldPassActive()
        && m_VR->m_RenderWidth >= 640 && m_VR->m_RenderHeight >= 360)
    {
        width = static_cast<int>(m_VR->m_RenderWidth);
        height = static_cast<int>(m_VR->m_RenderHeight);
        return;
    }
    uint32_t fbW = 0, fbH = 0;
    if (bmvr::TryHmdFitFullFrame() && bmvr::HaveHmdFramebufferSize(fbW, fbH)
        && !NestedRenderView() && !AuxSceneRtBound()
        && m_VR && m_VR->StereoWorldPassActive())
    {
        static int s_bbLog;
        if (s_bbLog < 8 && (width != static_cast<int>(fbW) || height != static_cast<int>(fbH)))
        {
            Game::logMsg("GetBackBufferDimensions %dx%d -> %ux%u (HMD-fit)",
                width, height, fbW, fbH);
            ++s_bbLog;
        }
        width = static_cast<int>(fbW);
        height = static_cast<int>(fbH);
        return;
    }
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
    if (OffscreenStereoSizeLie(width, height))
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
    if (bmvr::TryHmdFitFullFrame()
        && m_VR && !NestedRenderView() && !AuxSceneRtBound()
        && m_VR->StereoWorldPassActive()
        && m_VR->m_RenderWidth >= 640 && m_VR->m_RenderHeight >= 360)
    {
        width = static_cast<int>(m_VR->m_RenderWidth);
        height = static_cast<int>(m_VR->m_RenderHeight);
        return;
    }
    uint32_t fbW = 0, fbH = 0;
    if (bmvr::TryHmdFitFullFrame() && bmvr::HaveHmdFramebufferSize(fbW, fbH)
        && !NestedRenderView() && !AuxSceneRtBound()
        && m_VR && m_VR->StereoWorldPassActive())
    {
        static int s_ssFit;
        if (s_ssFit < 8 && (width != static_cast<int>(fbW) || height != static_cast<int>(fbH)))
        {
            Game::logMsg("GetScreenSize %dx%d -> %ux%u (HMD-fit)",
                width, height, fbW, fbH);
            ++s_ssFit;
        }
        width = static_cast<int>(fbW);
        height = static_cast<int>(fbH);
        return;
    }
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

float __fastcall Hooks::dGetScreenAspectRatio(void* ecx, void* edx)
{
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
        && m_VR->StereoWorldPassActive()
        && m_VR->m_RenderWidth >= 640 && m_VR->m_RenderHeight >= 360
        && (bmvr::OffscreenWorldMatchesEyes() || m_VR->CachedRt0MatchesEyes()
            || m_VR->D3dRt0IsEyeSized()))
    {
        const float eyeAspect = static_cast<float>(m_VR->m_RenderWidth)
            / static_cast<float>(m_VR->m_RenderHeight);
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
    (void)edx;
    {
        const char* map = PeekLevelName();
        if (map && map[0])
            bmvr::NoteEngineMapName(map);
    }
    if (m_VR)
        m_VR->ApplyRenderTargetFramebufferOverride(ecx);

    uint32_t fbW = 0, fbH = 0;
    const bool haveFb = bmvr::TryHmdFramebuffer() && bmvr::HaveHmdFramebufferSize(fbW, fbH);
    const bool fullMode = sizeMode == RT_SIZE_FULL_FRAME_BUFFER
        || sizeMode == RT_SIZE_FULL_FRAME_BUFFER_ROUNDED_UP;
    const bool namedFb = name && (std::strstr(name, "_rt_FullFrameFB")
        || std::strstr(name, "_rt_ResolvedFullFrame"));
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
    const bool wantGrow = wantGrowFb || wantGrowGb;
    if (wantGrow)
    {
        const int oldW = w;
        const int oldH = h;
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
    else if (bmvr::TryHmdFitFullFrame()
        && haveFb && !skipGrow && (namedFb || namedGb)
        && name
        && sizeMode != RT_SIZE_PICMIP
        && !(w >= 256 && h >= 256))
    {
        const int oldW = w;
        const int oldH = h;
        const int oldMode = sizeMode;
        w = static_cast<int>(fbW);
        h = static_cast<int>(fbH);
        sizeMode = RT_SIZE_LITERAL;
        static int s_fitLog;
        if (s_fitLog < 12)
        {
            Game::logMsg("CreateNamedRT %s %dx%d mode=%d -> LITERAL %ux%u (%s, rec=%ux%u unused)",
                name, oldW, oldH, oldMode, fbW, fbH,
                namedGb ? "eye-fit G-buffer" : "HMD-fit FullFrame",
                bmvr::g_RecommendedEyeWidth, bmvr::g_RecommendedEyeHeight);
            ++s_fitLog;
        }
        static bool s_ffFitRisky;
        if (!s_ffFitRisky)
        {
            s_ffFitRisky = true;
            if (bmvr::TryHmdFitFullFrame())
                bmvr::BeginRisky(L"ff_hmdfit");
            if (bmvr::TryEyeFitWorldRts())
                bmvr::BeginRisky(L"ff_gbfit");
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
    ITexture* tex = hkCreateNamedRTEx.fOriginal(ecx, name, w, h, sizeMode, format, depth, textureFlags, renderTargetFlags);
    if (tex && name && (fullMode || namedFb || namedGb || wantGrow))
    {
        int aw = 0, ah = 0;
        __try
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
            if (bmvr::TryHmdFitFullFrame() && haveFb
                && std::abs(aw - static_cast<int>(fbW)) < 32
                && std::abs(ah - static_cast<int>(fbH)) < 32)
                bmvr::EndRisky(L"ff_hmdfit");
        }
        else if (aw > 0 && namedFb && bmvr::g_FullFrameActualWidth < 640)
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

void bmvr::InstallEarlyFramebufferHook()
{
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;
    if (!TryHmdFramebuffer())
        return;

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

    int off = SigScanner::VerifyOffset("materialsystem.dll", 0x52d20,
        "55 8B EC 8B 0D ? ? ? ? 8B 01 8B 80 58 04 00 00 5D FF E0");
    if (off < 0)
        off = SigScanner::VerifyOffset("MaterialSystem.dll", 0x52d20,
            "55 8B EC 8B 0D ? ? ? ? 8B 01 8B 80 58 04 00 00 5D FF E0");
    if (off == -1)
    {
        Log("GetBackBufferDimensions signature not found");
        return;
    }
    if (off == 0)
        off = 0x52d20;
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
    int screenOff = SigScanner::VerifyOffset("engine.dll", 0xA6BD0,
        "55 8B EC 8B 0D ? ? ? ? 56 8B 01 FF 90 9C 01 00 00");
    if (screenOff == -1)
    {
        Log("GetScreenSize signature not found");
        return;
    }
    if (screenOff == 0)
        screenOff = 0xA6BD0;
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
        int aspectOff = SigScanner::VerifyOffset("engine.dll", 0x1012D0,
            "55 8B EC 8B 0D ? ? ? ? 83 EC 0C 81 F9 ? ? ? ? 75 16 F3 0F 10 0D");
        if (aspectOff == -1)
            Log("GetScreenAspectRatio signature not found");
        else
        {
            if (aspectOff == 0)
                aspectOff = 0x1012D0;
            void* aspectTarget = reinterpret_cast<uint8_t*>(eng) + aspectOff;
            if (Hooks::hkGetScreenAspectRatio.createHook(aspectTarget, &Hooks::dGetScreenAspectRatio) != 0
                || Hooks::hkGetScreenAspectRatio.enableHook() != 0)
                Log("GetScreenAspectRatio hook failed rva=0x%X", aspectOff);
            else
                Log("Hook enabled: GetScreenAspectRatio rva=0x%X", aspectOff);
        }
    }
}
