#include "game.h"
#include "vr_mag_reload.h"
#include "offset_checks.h"
#include "sdk.h"
#include "vr.h"
#include "hooks.h"
#include "offsets.h"
#include "d3d9_vr.h"
#include "bmvr_flags.h"

#include <unordered_map>
#include <initializer_list>
#include <cstdarg>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <set>
#include "bmvr_seh.h"
using tCreateInterface = void*(__cdecl*)(const char* name, int* returnCode);

static HMODULE GetModuleWithRetry(const char* dllname, int maxTries = 1800, int delayMs = 100)
{
    for (int i = 0; i < maxTries; ++i)
    {
        if (HMODULE handle = GetModuleHandleA(dllname))
            return handle;
        if (i == 0 || (i + 1) % 50 == 0)
            Game::logMsg("Waiting for %s (%d/%d)", dllname, i + 1, maxTries);
        Sleep(delayMs);
    }
    Game::logMsg("Timed out waiting for %s", dllname);
    return nullptr;
}

static void* GetInterfaceSafe(const char* dllname, const char* interfacename)
{
    static std::unordered_map<std::string, void*> cache;
    const std::string key = std::string(dllname) + "::" + interfacename;
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    HMODULE mod = GetModuleWithRetry(dllname);
    if (!mod)
        return nullptr;

    auto CreateInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface"));
    if (!CreateInterface)
    {
        Game::errorMsg(("CreateInterface missing in " + std::string(dllname)).c_str());
        return nullptr;
    }

    int rc = 0;
    void* iface = CreateInterface(interfacename, &rc);
    if (!iface)
        Game::logMsg("Interface not found: %s", interfacename);
    else
        cache[key] = iface;
    return iface;
}

static void* GetInterfaceAny(const char* dll, std::initializer_list<const char*> names)
{
    for (const char* n : names)
    {
        if (void* p = GetInterfaceSafe(dll, n))
            return p;
    }
    return nullptr;
}

static void* SehVtableSlot(void* iface, int slot)
{
    void* fn = nullptr;
    if (!iface)
        return nullptr;
    BMVR_TRY
    {
        void** vt = *reinterpret_cast<void***>(iface);
        if (vt)
            fn = vt[slot];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        fn = nullptr;
    }
    return fn;
}

Game::Game()
{
    m_BaseEngine = reinterpret_cast<uintptr_t>(GetModuleWithRetry("engine.dll"));
    m_BaseMaterialSystem = reinterpret_cast<uintptr_t>(GetModuleWithRetry("MaterialSystem.dll"));
    m_BaseClient = reinterpret_cast<uintptr_t>(GetModuleWithRetry("client.dll"));
    if (!m_BaseClient)
    {
        Game::errorMsg("Timed out waiting for client.dll.");
        return;
    }

    m_BaseServer = reinterpret_cast<uintptr_t>(GetModuleHandleA("server.dll"));
    m_BaseVgui2 = reinterpret_cast<uintptr_t>(GetModuleWithRetry("vgui2.dll"));

    m_ClientEntityList = static_cast<IClientEntityList*>(GetInterfaceAny("client.dll", { "VClientEntityList003" }));
    m_EngineTrace = static_cast<IEngineTrace*>(GetInterfaceAny("engine.dll", { "EngineTraceClient004", "EngineTraceClient003" }));
    m_EngineClient = static_cast<IEngineClient*>(GetInterfaceAny("engine.dll", { "VEngineClient015", "VEngineClient014", "VEngineClient013" }));
    m_EngineVGui = static_cast<IEngineVGui*>(GetInterfaceAny("engine.dll", { "VEngineVGui001" }));
    m_MaterialSystem = static_cast<IMaterialSystem*>(GetInterfaceAny("MaterialSystem.dll", { "VMaterialSystem081", "VMaterialSystem080" }));
    m_ModelInfo = static_cast<IModelInfo*>(GetInterfaceAny("engine.dll", { "VModelInfoClient006", "VModelInfoClient004" }));
    m_ModelRender = static_cast<IModelRender*>(GetInterfaceAny("engine.dll", { "VEngineModel016" }));
    m_VguiInput = static_cast<IInput*>(GetInterfaceAny("vgui2.dll", { "VGUI_InputInternal001", "VGUI_Input005" }));
    // Do not bind ISurface until IsCursorVisible's vtable slot is verified on BM.
    // DXVK Present helpers call it when m_VguiSurface is non-null.
    m_VguiSurface = nullptr;
    m_EngineSound = GetInterfaceAny("engine.dll", { "IEngineSoundClient003", "IEngineSoundClient002" });
    Game::logMsg("IEngineSound=%p", m_EngineSound);
    m_BaseFileSystem = static_cast<IBaseFileSystemBm*>(GetInterfaceAny("filesystem_stdio.dll", { "VBaseFileSystem011" }));
    Game::logMsg("IBaseFileSystem=%p", m_BaseFileSystem);
    // BM vstdlib only exports VEngineCvar004 (not 007). 007 FindVar slots
    // were what the 2026-08-18 probe called into until load hung.
    m_Cvar = GetInterfaceSafe("vstdlib.dll", "VEngineCvar004");
    const char* cvarIface = m_Cvar ? "VEngineCvar004" : nullptr;
    if (!m_Cvar)
    {
        m_Cvar = GetInterfaceSafe("vstdlib.dll", "VEngineCvar007");
        cvarIface = m_Cvar ? "VEngineCvar007" : nullptr;
    }
    if (!m_Cvar)
        m_Cvar = GetInterfaceAny("engine.dll", { "VEngineCvar004", "VEngineCvar007" });

    Game::logMsg("Interfaces: engine=%p vgui=%p matsys=%p clientent=%p icvar=%p %s",
        m_EngineClient, m_EngineVGui, m_MaterialSystem, m_ClientEntityList, m_Cvar,
        cvarIface ? cvarIface : "none");
    Game::logMsg("IEngineClient vtbl ClientCmd7=%p slot107=%p slot108=%p",
        SehVtableSlot(m_EngineClient, 7),
        SehVtableSlot(m_EngineClient, 107),
        SehVtableSlot(m_EngineClient, 108));

    // Before any hook patches the bytes the checks read.
    bmvr::VerifyGameOffsets();
    m_Offsets = new Offsets();
    ResolveMaterialThreadSlots();
    m_VR = new VR(this);
    m_Hooks = new Hooks(this);
    ScanWristHudNetVars();

    // Install D3D Present/SetRT hooks from this thread, not from inside Present.
    auto tryInstallD3DHooks = [this]() {
        if (!m_VR || m_VR->m_D3DHooksInstalled || !g_D3DVR9)
            return false;
        IDirect3DDevice9* device = nullptr;
        if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
            return false;
        m_VR->InstallDeviceHooks(device);
        device->Release();
        return m_VR->m_D3DHooksInstalled;
    };
    if (!tryInstallD3DHooks())
    {
        CreateThread(nullptr, 0, [](LPVOID ctx) -> DWORD {
            Game* game = static_cast<Game*>(ctx);
            for (int i = 0; i < 100; ++i)
            {
                Sleep(50);
                if (!g_D3DVR9 || !game->m_VR)
                    continue;
                IDirect3DDevice9* device = nullptr;
                if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
                    continue;
                game->m_VR->InstallDeviceHooks(device);
                device->Release();
                if (game->m_VR->m_D3DHooksInstalled)
                    break;
            }
            return 0;
        }, this, 0, nullptr);
    }

    m_Initialized = true;
    bmvr::SetStage("game_ready");
    Game::logMsg("BMVR Game initialized (L4D2VR architecture, Black Mesa offsets). namedRT=%d stereoRV=%d hmdFb=%d hmdNative=%d offscreen=%d hmd_world=%d menuVR=%d relLook=%d",
        bmvr::TryNamedRenderTargets() ? 1 : 0,
        bmvr::TryStereoRenderView() ? 1 : 0,
        bmvr::TryHmdFramebuffer() ? 1 : 0,
        bmvr::TryHmdNative() ? 1 : 0,
        bmvr::TryOffscreenHmd() ? 1 : 0,
        bmvr::TryOffscreenWorldGrow() ? 1 : 0,
        bmvr::TryMenuCompositor() ? 1 : 0,
        bmvr::TryRelativeHmdLook() ? 1 : 0);
}

void* Game::GetInterface(const char* dllname, const char* interfacename)
{
    return GetInterfaceSafe(dllname, interfacename);
}

C_BaseEntity* Game::GetClientEntity(int entityIndex)
{
    if (!m_ClientEntityList)
        return nullptr;
    return static_cast<C_BaseEntity*>(m_ClientEntityList->GetClientEntity(entityIndex));
}

C_BaseEntity* Game::GetLocalPlayerEntity()
{
    if (!m_EngineClient || !m_ClientEntityList)
        return nullptr;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return nullptr;
    return static_cast<C_BaseEntity*>(m_ClientEntityList->GetClientEntity(local));
}

C_BaseEntity* Game::LocalPlayerUseEntity()
{
    C_BaseEntity* player = GetLocalPlayerEntity();
    if (!player)
        return nullptr;

    uint32_t handle = 0;
    BMVR_TRY
    {
        handle = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(player) + Offsets::kCBasePlayer_hUseEntity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }

    return ResolveEntityFromHandle(handle);
}

C_BaseEntity* Game::ResolveEntityFromHandle(uint32_t handle)
{
    if (!m_ClientEntityList || handle == 0 || handle == 0xFFFFFFFFu)
        return nullptr;
    constexpr uint32_t kEntryMask = 0x1FFFu;
    void* entity = m_ClientEntityList->GetClientEntityFromHandle(static_cast<int>(handle));
    if (!entity)
        return nullptr;
    int idx = static_cast<int>(handle & kEntryMask);
    if (m_ClientEntityList->GetClientEntity(idx) != entity)
    {
        idx = 0;
        const int hi = m_ClientEntityList->GetHighestEntityIndex();
        for (int e = 1; e <= hi; ++e)
        {
            if (m_ClientEntityList->GetClientEntity(e) == entity)
            {
                idx = e;
                break;
            }
        }
    }
    return idx > 0 ? static_cast<C_BaseEntity*>(entity) : nullptr;
}

void* Game::ServerEntityByIndex(int index)
{
    static void* s_engineServer = nullptr;
    if (!s_engineServer)
        s_engineServer = GetInterfaceAny("engine.dll", { "VEngineServer021" });
    if (!s_engineServer || index <= 0 || index >= 0x1000)
        return nullptr;
    using tPEntityOfEntIndex = void*(__thiscall*)(void*, int);
    void** vt = *reinterpret_cast<void***>(s_engineServer);
    void* edict = reinterpret_cast<tPEntityOfEntIndex>(vt[Offsets::kEngineServer_PEntityOfEntIndexVt])(s_engineServer, index);
    return edict ? *reinterpret_cast<void**>(static_cast<char*>(edict) + Offsets::kEdict_Unknown) : nullptr;
}

bool Game::SetServerActiveWeaponClip(int expectClip, int newClip)
{
    void* player = GetLocalPlayerEntity();
    // Black Mesa is single player: the server runs in this process.
    if (!player)
        return false;
    static void* s_engineServer = nullptr;
    static bool s_tried = false;
    if (!s_tried)
    {
        s_tried = true;
        s_engineServer = GetInterfaceAny("engine.dll", { "VEngineServer021" });
        Game::logMsg("Mag discard: VEngineServer021 %s", s_engineServer ? "found" : "missing");
    }
    if (!s_engineServer)
        return false;
    using tPEntityOfEntIndex = void*(__thiscall*)(void*, int);
    bool ok = false;
    int serverClip = -1;
    BMVR_TRY
    {
        constexpr int kActiveWeapon = 0xFA4;
        const uint32_t h = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(player) + kActiveWeapon);
        const int index = static_cast<int>(h & 0xFFF);
        if (index > 0 && index < 0xFFF)
        {
            void** vt = *reinterpret_cast<void***>(s_engineServer);
            auto pEnt = reinterpret_cast<tPEntityOfEntIndex>(vt[Offsets::kEngineServer_PEntityOfEntIndexVt]);
            void* edict = pEnt(s_engineServer, index);
            void* entity = edict ? *reinterpret_cast<void**>(static_cast<char*>(edict) + Offsets::kEdict_Unknown) : nullptr;
            if (entity)
            {
                int* clip = reinterpret_cast<int*>(static_cast<char*>(entity) + Offsets::kServerWeapon_Clip1);
                serverClip = *clip;
                if (serverClip == expectClip && newClip >= 0 && newClip <= serverClip)
                {
                    *clip = newClip;
                    ok = true;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    Game::logMsg("Mag discard: server clip %d (expected %d) -> %s", serverClip, expectClip,
        ok ? std::to_string(newClip).c_str() : "not changed");
    return ok;
}

bool Game::ClampServerWeaponDeploy(float capSec)
{
    static uint32_t s_lastHandle = 0;
    static int s_ticksLeft = 0;
    static float s_switchTime = -1.f;
    static int s_switchClip = 0;
    void* player = GetLocalPlayerEntity();
    if (!player || !m_BaseClient)
        return false;
    static void* s_engineServer = nullptr;
    if (!s_engineServer)
        s_engineServer = GetInterfaceAny("engine.dll", { "VEngineServer021" });
    if (!s_engineServer)
        return false;
    bool changed = false;
    BMVR_TRY
    {
        constexpr int kActiveWeapon = 0xFA4;
        const uint32_t h = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(player) + kActiveWeapon);
        if (h != s_lastHandle)
        {
            s_lastHandle = h;
            s_ticksLeft = 4; // the deploy timer is set in the switch, a tick or two before/after the handle shows
            s_switchTime = -1.f;
        }
        const int index = static_cast<int>(h & 0xFFF);
        if (index > 0 && index < 0xFFF && s_ticksLeft == 0 && s_switchTime >= 0.f)
        {
            // Log how long after the switch the first shot left (clip dropped).
            void** vt0 = *reinterpret_cast<void***>(s_engineServer);
            using tPEnt0 = void*(__thiscall*)(void*, int);
            void* ed0 = reinterpret_cast<tPEnt0>(vt0[Offsets::kEngineServer_PEntityOfEntIndexVt])(s_engineServer, index);
            char* en0 = ed0 ? *reinterpret_cast<char**>(static_cast<char*>(ed0) + Offsets::kEdict_Unknown) : nullptr;
            const uintptr_t gp0 = *reinterpret_cast<uintptr_t*>(m_BaseClient + Offsets::kClientGpGlobalsPtr);
            if (en0 && gp0)
            {
                const float t = *reinterpret_cast<float*>(gp0 + Offsets::kCGlobalVars_curtime);
                if (*reinterpret_cast<int*>(en0 + Offsets::kServerWeapon_Clip1) < s_switchClip)
                {
                    Game::logMsg("Quick draw: first shot %.3f s after the switch", t - s_switchTime);
                    s_switchTime = -1.f;
                }
                else if (t - s_switchTime > 3.f)
                    s_switchTime = -1.f;
            }
        }
        if (s_ticksLeft > 0 && index > 0 && index < 0xFFF)
        {
            --s_ticksLeft;
            const uintptr_t gp = *reinterpret_cast<uintptr_t*>(m_BaseClient + Offsets::kClientGpGlobalsPtr);
            void** vt = *reinterpret_cast<void***>(s_engineServer);
            using tPEntityOfEntIndex = void*(__thiscall*)(void*, int);
            auto pEnt = reinterpret_cast<tPEntityOfEntIndex>(vt[Offsets::kEngineServer_PEntityOfEntIndexVt]);
            void* edict = pEnt(s_engineServer, index);
            char* entity = edict ? *reinterpret_cast<char**>(static_cast<char*>(edict) + Offsets::kEdict_Unknown) : nullptr;
            if (gp && entity)
            {
                const float now = *reinterpret_cast<float*>(gp + Offsets::kCGlobalVars_curtime);
                const float limit = now + capSec;
                float* nextPri = reinterpret_cast<float*>(entity + Offsets::kServerWeapon_NextPrimaryAttack);
                float* nextSec = reinterpret_cast<float*>(entity + Offsets::kServerWeapon_NextSecondaryAttack);
                const float oldPri = *nextPri, oldSec = *nextSec;
                if (s_switchTime < 0.f)
                {
                    s_switchTime = now;
                    s_switchClip = *reinterpret_cast<int*>(entity + Offsets::kServerWeapon_Clip1);
                }
                if (oldPri > limit && oldPri < now + 5.f)
                {
                    *nextPri = limit;
                    changed = true;
                }
                if (oldSec > limit && oldSec < now + 5.f)
                {
                    *nextSec = limit;
                    changed = true;
                }
                {
                    const int lp = m_EngineClient ? m_EngineClient->GetLocalPlayer() : 0;
                    void* edp = lp > 0 ? pEnt(s_engineServer, lp) : nullptr;
                    char* sp = edp ? *reinterpret_cast<char**>(static_cast<char*>(edp) + Offsets::kEdict_Unknown) : nullptr;
                    if (sp)
                    {
                        float* pn = reinterpret_cast<float*>(sp + Offsets::kServerPlayer_NextAttack);
                        if (*pn > limit && *pn < now + 5.f)
                        {
                            *pn = limit;
                            changed = true;
                        }
                    }
                }
                if (changed)
                    Game::logMsg("Quick draw: next attack %.2f/%.2f s -> %.2f s after the switch", oldPri - now,
                        oldSec - now, capSec);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        changed = false;
    }
    return changed;
}

C_BaseEntity* Game::GetActiveWeaponEntity()
{
    void* player = GetLocalPlayerEntity();
    if (!player)
        return nullptr;
    constexpr int kActiveWeapon = 0xFA4;
    const uint32_t h = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(player) + kActiveWeapon);
    return ResolveEntityFromHandle(h);
}

const char* Game::GetEntityModelName(C_BaseEntity* entity)
{
    if (!entity || !m_ModelInfo)
        return nullptr;
    short modelIndex = 0;
    BMVR_TRY
    {
        modelIndex = *reinterpret_cast<const short*>(
            reinterpret_cast<uintptr_t>(entity) + 0x94);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    if (modelIndex <= 0)
        return nullptr;
    void* model = m_ModelInfo->GetModel(modelIndex);
    if (!model)
        return nullptr;
    return m_ModelInfo->GetModelName(model);
}

void* Game::FindOrLoadModel(const char* name)
{
    if (!m_ModelInfo || !name || !name[0])
        return nullptr;
    auto modelLooksRight = [&](void* model) -> bool {
        if (!model)
            return false;
        const char* got = nullptr;
        BMVR_TRY
        {
            got = m_ModelInfo->GetModelName(model);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (!got || !got[0])
            return false;
        if (std::strstr(got, name))
            return true;
        const char* leaf = name;
        for (const char* p = name; *p; ++p)
        {
            if (*p == '/' || *p == '\\')
                leaf = p + 1;
        }
        return leaf && leaf[0] && std::strstr(got, leaf) != nullptr;
    };
    int idx = -1;
    BMVR_TRY
    {
        idx = m_ModelInfo->GetModelIndex(name);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        idx = -1;
    }
    if (idx != -1)
    {
        void* model = nullptr;
        BMVR_TRY
        {
            model = m_ModelInfo->GetModel(idx);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            model = nullptr;
        }
        if (modelLooksRight(model))
            return model;
    }

    void** vt = nullptr;
    BMVR_TRY
    {
        vt = *reinterpret_cast<void***>(m_ModelInfo);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt = nullptr;
    }
    if (!vt)
        return nullptr;

    // IVModelInfoClient006: RegisterDynamicModel ~slot 52, FindOrLoadModel ~slot 59
    // (Source 2013 layout). Probe those two only; a wide slot sweep can call
    // OnLevelChange / CleanupDynamicModels.
    void* found = nullptr;
    BMVR_TRY
    {
        using FindFn = void*(__thiscall*)(void*, const char*);
        auto find = reinterpret_cast<FindFn>(vt[59]);
        if (find)
        {
            void* model = find(m_ModelInfo, name);
            if (modelLooksRight(model))
                found = model;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (found)
        return found;
    BMVR_TRY
    {
        using RegFn = int(__thiscall*)(void*, const char*, bool);
        auto reg = reinterpret_cast<RegFn>(vt[52]);
        if (reg)
        {
            const int dyn = reg(m_ModelInfo, name, true);
            if (dyn != -1)
            {
                void* model = m_ModelInfo->GetModel(dyn);
                if (model)
                    found = model;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return found;
}

const char* Game::GetActiveWeaponModelName()
{
    return GetEntityModelName(GetActiveWeaponEntity());
}

C_BaseEntity* Game::GetViewModelEntity()
{
    void* player = GetLocalPlayerEntity();
    if (!player)
        return nullptr;
    // DT_BasePlayer m_hViewModel[0] +0x13F0, count 2 (Ghidra FUN_100b6a00).
    constexpr int kViewModel0 = 0x13F0;
    constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    uint32_t h = 0;
    BMVR_TRY
    {
        h = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<uintptr_t>(player) + kViewModel0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    if (h == 0 || h == kInvalid)
        return nullptr;
    return static_cast<C_BaseEntity*>(m_ClientEntityList->GetClientEntityFromHandle(static_cast<int>(h)));
}

bool Game::GetEntityAttachment(C_BaseEntity* entity, const char* name, Vector& origin, QAngle& angles)
{
    if (!entity || !name || !name[0])
        return false;
    int ok = 0;
    BMVR_TRY
    {
        unsigned char* rend = reinterpret_cast<unsigned char*>(entity) + 4;
        void** vt = *reinterpret_cast<void***>(rend);
        using LookupFn = int(__thiscall*)(void*, const char*);
        using GetFn = int(__thiscall*)(void*, int, Vector*, QAngle*);
        auto lookup = reinterpret_cast<LookupFn>(vt[Offsets::kIClientRenderable_LookupAttachment / 4]);
        auto get = reinterpret_cast<GetFn>(vt[Offsets::kIClientRenderable_GetAttachmentVec / 4]);
        if (!lookup || !get)
            return false;
        const int idx = lookup(rend, name);
        if (idx <= 0)
            return false;
        ok = get(rend, idx, &origin, &angles);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return ok != 0;
}

void Game::EmitEntitySound(C_BaseEntity* entity, const char* soundName)
{
    if (!entity || !soundName || !soundName[0])
        return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client)
        return;
    // client build 25844216: kCBaseEntity_EmitSound is not re-verified and
    // the old RVA is mid-function there. Do not call it.
    if (!Offsets::kCBaseEntity_EmitSoundVerified)
        return;
    using EmitFn = void(__thiscall*)(void*, const char*, float, float*);
    auto emit = reinterpret_cast<EmitFn>(
        reinterpret_cast<uintptr_t>(client) + Offsets::kCBaseEntity_EmitSound);
    BMVR_TRY
    {
        emit(entity, soundName, 0.f, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void Game::EmitPlayerSound(const char* soundName)
{
    EmitEntitySound(GetLocalPlayerEntity(), soundName);
}

void Game::PlayUiSound(const char* sample)
{
    if (!sample || !sample[0])
        return;
    if (!m_EngineSound)
        m_EngineSound = GetInterfaceAny("engine.dll", { "IEngineSoundClient003", "IEngineSoundClient002" });
    if (!m_EngineSound)
        return;
    void** vt = nullptr;
    BMVR_TRY
    {
        vt = *reinterpret_cast<void***>(m_EngineSound);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    if (!vt)
        return;
    // Source 2013 IEngineSoundClient003: PrecacheSound=0, EmitAmbientSound=11.
    using PrecacheFn = bool(__thiscall*)(void*, const char*, bool, bool);
    using AmbientFn = void(__thiscall*)(void*, const char*, float, int, int, float);
    static int s_logged;
    BMVR_TRY
    {
        auto precache = reinterpret_cast<PrecacheFn>(vt[0]);
        if (precache)
            precache(m_EngineSound, sample, false, true);
        auto ambient = reinterpret_cast<AmbientFn>(vt[11]);
        if (ambient)
            ambient(m_EngineSound, sample, 1.f, 100, 0, 0.f);
        if (s_logged < 4)
        {
            Game::logMsg("PlayUiSound '%s' engine=%p slot11=%p", sample, m_EngineSound, vt[11]);
            ++s_logged;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (s_logged < 4)
        {
            Game::logMsg("PlayUiSound SEH for '%s'", sample);
            ++s_logged;
        }
    }
}

const char* Game::GetEntityNetworkName(int entityIndex)
{
    if (!m_ClientEntityList || entityIndex <= 0)
        return nullptr;
    void* net = nullptr;
    BMVR_TRY
    {
        net = m_ClientEntityList->GetClientNetworkable(entityIndex);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    if (!net)
        return nullptr;
    struct ClientClassLite
    {
        void* createFn;
        void* createEventFn;
        const char* networkName;
    };
    const char* name = nullptr;
    BMVR_TRY
    {
        void** vt = *reinterpret_cast<void***>(net);
        using tGetClientClass = ClientClassLite*(__thiscall*)(void*);
        ClientClassLite* cc = reinterpret_cast<tGetClientClass>(vt[2])(net);
        if (cc)
            name = cc->networkName;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    return name;
}

const char* Game::GetEntityClientClassName(C_BaseEntity* entity)
{
    if (!entity)
        return nullptr;
    struct ClientClassLite
    {
        void* createFn;
        void* createEventFn;
        const char* networkName;
    };
    const char* name = nullptr;
    BMVR_TRY
    {
        unsigned char* net = reinterpret_cast<unsigned char*>(entity) + 8;
        void** vt = *reinterpret_cast<void***>(net);
        using tGetClientClass = ClientClassLite*(__thiscall*)(void*);
        ClientClassLite* cc = reinterpret_cast<tGetClientClass>(vt[2])(net);
        if (cc)
            name = cc->networkName;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    return name;
}

// Wrist HUD netvars.
// BM RecvProp in client.dll .data is { char* name; int offset; } at +0/+4
// (not Source 2013's offset-at-+44). Only trust that +4 dword — scanning
// +8..+56 picked C_PlayerResource junk (m_iHealth "136" / 0x88 → HUD stuck
// at 01, which was m_nModelIndex-adjacent). m_ArmorValue has no RecvProp;
// datamap is `push offset; push "m_ArmorValue"`: C_BasePlayer 0x134C (stale)
// and C_BlackMesaPlayer 0x17C0 (live HEV).
namespace
{
    int g_nvHealth = -1;
    int g_nvViewEntity = -1;
    int g_nvArmor = -1;
    int g_nvClip1 = -1;
    int g_nvClip2 = -1;
    int g_nvBody = -1; // C_BaseAnimating m_nBody (bodygroups)
    int g_nvNeedReload = -1; // C_Weapon_RPG m_bNeedReload
    int g_nvInReload = -1;   // C_BaseCombatWeapon m_bInReload
    int g_nvAmmoBase = -1;
    int g_nvPrimaryAmmoType = -1;
    int g_nvSecondaryAmmoType = -1;
    int g_nvWearingSuit = -1;
    int g_nvZooming = -1;
    int g_nvRpgLaserOn = -1;
    int g_nvRpgLaserDot = -1;
    int g_nvWaterLevel = -1;
    int g_nvAirFinished = -1;
    int g_nvMoveType = -1;
    int g_nvFlags = -1;
    bool g_nvScanned = false;

    const char* FindStringInImage(const unsigned char* base, size_t size, const char* s)
    {
        const size_t len = strlen(s);
        for (size_t i = 0; i + len < size; ++i)
        {
            if (base[i] == static_cast<unsigned char>(s[0]) && memcmp(base + i, s, len) == 0
                && base[i + len] == 0)
                return reinterpret_cast<const char*>(base + i);
        }
        return nullptr;
    }

    // RecvProp offset at +4 from a pointer to the property-name string.
    // requireAligned rejects offsets that are not dword-aligned, which filters
    // out garbage for int fields. Single-byte props (bools) are usually not
    // aligned, so those callers must pass false.
    int FindRecvPropOffsetPlus4(const unsigned char* base, size_t size, const char* propName,
        int minOff, int maxOff, bool requireAligned = true, bool preferLargest = false)
    {
        const char* s = FindStringInImage(base, size, propName);
        if (!s)
        {
            Game::logMsg("WristHUD RecvProp '%s': string not found", propName);
            // m_hLaserDot is HL2's RPG prop; BM has no such string in any build, so it is no update sign.
            if (std::strcmp(propName, "m_hLaserDot") != 0)
                Game::noteMissingOffset(propName);
            return -1;
        }
        const uintptr_t nameVA = reinterpret_cast<uintptr_t>(s);
        int best = -1;
        int hits = 0;
        int plus4Hits = 0;
        for (size_t i = 0; i + 8 < size; i += 4)
        {
            uintptr_t v = 0;
            memcpy(&v, base + i, sizeof(v));
            if (v != nameVA)
                continue;
            ++hits;
            int val = 0;
            memcpy(&val, base + i + 4, sizeof(val));
            if (val < minOff || val > maxOff)
                continue;
            if (requireAligned && (val & 3) != 0)
                continue;
            ++plus4Hits;
            if (best < 0 || (preferLargest ? val > best : val < best))
                best = val;
        }
        Game::logMsg("WristHUD RecvProp '%s': hits=%d plus4=%d offset=%d (range %d..%d%s)",
            propName, hits, plus4Hits, best, minOff, maxOff,
            preferLargest ? ", largest" : "");
        return best;
    }

    // Datamap DEFINE_FIELD: 68 <offset>  68 <nameVA>. Prefer the largest
    // offset in range (derived class shadows the HL2 base field).
    int FindDatamapFieldOffset(const unsigned char* base, size_t size, const char* propName,
        int minOff, int maxOff)
    {
        const char* s = FindStringInImage(base, size, propName);
        if (!s)
        {
            Game::logMsg("WristHUD datamap '%s': string not found", propName);
            Game::noteMissingOffset(propName);
            return -1;
        }
        const uintptr_t nameVA = reinterpret_cast<uintptr_t>(s);
        int best = -1;
        int hits = 0;
        // i = the name's imm32: 68 <offset> 68 <nameVA> puts the offset at i-5 and the pushes at i-6 / i-1
        // (it read i-4 behind a test of i-5 and never matched: m_ArmorValue hits=0 on every build).
        for (size_t i = 6; i + 4 < size; i += 1)
        {
            uintptr_t v = 0;
            memcpy(&v, base + i, sizeof(v));
            if (v != nameVA)
                continue;
            if (base[i - 1] != 0x68 || base[i - 6] != 0x68)
                continue;
            int val = 0;
            memcpy(&val, base + i - 5, sizeof(val));
            ++hits;
            if (val < minOff || val > maxOff || (val & 3) != 0)
                continue;
            if (best < 0 || val > best)
                best = val;
        }
        Game::logMsg("WristHUD datamap '%s': hits=%d offset=%d (range %d..%d)",
            propName, hits, best, minOff, maxOff);
        return best;
    }
}

void Game::ScanWristHudNetVars()
{
    if (g_nvScanned)
        return;
    g_nvScanned = true;
    HMODULE mod = GetModuleHandleA("client.dll");
    if (!mod)
        return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const unsigned char*>(mod) + dos->e_lfanew);
    const size_t imgSize = nt->OptionalHeader.SizeOfImage;
    const auto* base = reinterpret_cast<const unsigned char*>(mod);
    // C_BaseEntity RecvProp + datamap m_iHealth = 0x98 (m_lifeState at 0x97).
    // Do not take C_PlayerResource m_iHealth[MAX_PLAYERS] at 0x994.
    g_nvHealth = FindRecvPropOffsetPlus4(base, imgSize, "m_iHealth", 0x80, 0x200);
    if (g_nvHealth < 0)
        g_nvHealth = 0x98;
    // Live HEV is C_BlackMesaPlayer::m_ArmorValue 0x17C0, not C_BasePlayer 0x134C.
    // DT_LocalPlayerExclusive m_hViewEntity (0x12E8 on 26234270), registered
    // as 68 <offset> 68 <name> like a datamap field.
    g_nvViewEntity = FindDatamapFieldOffset(base, imgSize, "m_hViewEntity", 0x800, 0x3000);
    g_nvArmor = FindDatamapFieldOffset(base, imgSize, "m_ArmorValue", 0x1000, 0x4000);
    if (g_nvArmor < 0)
        g_nvArmor = 0x17C0;
    g_nvBody = FindRecvPropOffsetPlus4(base, imgSize, "m_nBody", 0x100, 0x1000);
    g_nvNeedReload = FindRecvPropOffsetPlus4(base, imgSize, "m_bNeedReload", 0x800, 0x3000, false);
    g_nvInReload = FindRecvPropOffsetPlus4(base, imgSize, "m_bInReload", 0x800, 0x3000, false);
    g_nvClip1 = FindRecvPropOffsetPlus4(base, imgSize, "m_iClip1", 0x800, 0x3000);
    g_nvClip2 = FindRecvPropOffsetPlus4(base, imgSize, "m_iClip2", 0x800, 0x3000);
    g_nvAmmoBase = FindRecvPropOffsetPlus4(base, imgSize, "m_iAmmo", 0x800, 0x3000);
    g_nvPrimaryAmmoType = FindRecvPropOffsetPlus4(base, imgSize, "m_iPrimaryAmmoType", 0x400, 0x3000);
    g_nvSecondaryAmmoType = FindRecvPropOffsetPlus4(base, imgSize, "m_iSecondaryAmmoType", 0x400, 0x3000);
    // Ghidra DT_LocalWeaponData FUN_10070f80: clip1=0xa64 clip2=0xa68
    // primaryType=0xa5c secondaryType=0xa60.
    if (g_nvClip1 < 0)
        g_nvClip1 = 0xA64;
    if (g_nvClip2 < 0)
        g_nvClip2 = 0xA68;
    if (g_nvPrimaryAmmoType < 0)
        g_nvPrimaryAmmoType = 0xA5C;
    if (g_nvSecondaryAmmoType < 0)
        g_nvSecondaryAmmoType = 0xA60;
    // m_bWearingSuit is not a player-relative prop. It lives inside DT_Local,
    // so its RecvProp offset is measured from CPlayerLocalData and has to be
    // added to m_Local's own offset. It is also a single byte, so it is not
    // dword-aligned and the aligned scan would throw it away. Nothing is
    // hardcoded: a wrong offset here would hide the gloves for a whole
    // playthrough, so an unresolved scan means "assume suited".
    const int localData = FindRecvPropOffsetPlus4(base, imgSize, "m_Local", 0x400, 0x2000);
    const int suitInLocal = FindRecvPropOffsetPlus4(
        base, imgSize, "m_bWearingSuit", 0x10, 0x400, false);
    if (localData > 0 && suitInLocal > 0)
        g_nvWearingSuit = localData + suitInLocal;
    // Client m_bZooming sits next to m_bFlashlightEnabled in
    // DT_BlackMesaLocalPlayerExclusive, directly on the player, not in m_Local.
    // The client RecvProp is near flashlight (+0x17E8). client.dll also contains
    // the server SendProp leftover at +0x1374; taking the smallest hit in a wide
    // range landed on that leftover, so zoom never read as 1 and scoped aim
    // stayed on the controller.
    int flashlight = FindRecvPropOffsetPlus4(
        base, imgSize, "m_bFlashlightEnabled", 0x1700, 0x1A00, false, true);
    g_nvZooming = FindRecvPropOffsetPlus4(
        base, imgSize, "m_bZooming", 0x1700, 0x1A00, false, true);
    // BM builds these two RecvProps in code (push offset; push name; call), so there is no table entry
    // with the offset after the name (both -1 on betas 25844216 and 26234270: zoom 0x17C8, flashlight
    // 0x17E8). The datamap scan reads exactly that push pair.
    if (flashlight < 0)
        flashlight = FindDatamapFieldOffset(base, imgSize, "m_bFlashlightEnabled", 0x1700, 0x1A00);
    if (g_nvZooming < 0)
        g_nvZooming = FindDatamapFieldOffset(base, imgSize, "m_bZooming", 0x1700, 0x1A00);
    if (g_nvZooming < 0 && flashlight > 0)
        g_nvZooming = flashlight - 1;
    g_nvRpgLaserOn = FindRecvPropOffsetPlus4(
        base, imgSize, "m_bLaserOn", 0x800, 0xC00, false);
    g_nvRpgLaserDot = FindRecvPropOffsetPlus4(
        base, imgSize, "m_hLaserDot", 0x800, 0xC00);
    g_nvWaterLevel = FindRecvPropOffsetPlus4(
        base, imgSize, "m_nWaterLevel", 0x100, 0x200, false);
    if (g_nvWaterLevel < 0)
        g_nvWaterLevel = Offsets::kCBasePlayer_nWaterLevel;
    g_nvAirFinished = FindRecvPropOffsetPlus4(
        base, imgSize, "m_AirFinished", 0x1400, 0x1800);
    if (g_nvAirFinished < 0)
        g_nvAirFinished = Offsets::kCBasePlayer_AirFinished;
    // Datamap typedescription_t: name pointer, then the field offset.
    g_nvMoveType = FindRecvPropOffsetPlus4(
        base, imgSize, "m_MoveType", 0x100, 0x200, false);
    if (g_nvMoveType < 0)
        g_nvMoveType = Offsets::kCBaseEntity_MoveType;
    // m_fFlags (FL_ONGROUND bit 0, FL_ONTRAIN bit 4, FL_CLIENT bit 8): train controls.
    g_nvFlags = FindRecvPropOffsetPlus4(
        base, imgSize, "m_fFlags", 0x100, 0x600);
    Game::logMsg("Wrist HUD netvars health=%d armor=%d clip1=%d clip2=%d ammo=%d primType=%d secType=%d",
        g_nvHealth, g_nvArmor, g_nvClip1, g_nvClip2, g_nvAmmoBase, g_nvPrimaryAmmoType,
        g_nvSecondaryAmmoType);
    Game::logMsg("VR netvar m_hViewEntity=%d (%s)", g_nvViewEntity,
        g_nvViewEntity >= 0 ? "cutscene cameras need it" : "not found: cutscene follow uses the angles alone");
    Game::logMsg("VR netvars m_Local=%d m_bWearingSuit(+local)=%d suit=%d zooming=%d flashlight=%d rpgLaserOn=%d rpgLaserDot=%d water=%d airFinished=%d moveType=%d flags=%d",
        localData, suitInLocal, g_nvWearingSuit, g_nvZooming, flashlight,
        g_nvRpgLaserOn, g_nvRpgLaserDot, g_nvWaterLevel, g_nvAirFinished, g_nvMoveType, g_nvFlags);
}

// Reads a networked bool. Returns -1 when the offset is unresolved, has been
// retired for reading something that was never a bool, or cannot be read.
int Game::ReadPlayerFlagByte(int& offset)
{
    if (offset < 0 || !m_EngineClient || !m_ClientEntityList)
        return -1;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return -1;
    void* player = m_ClientEntityList->GetClientEntity(local);
    if (!player)
        return -1;
    unsigned char value = 0;
    BMVR_TRY
    {
        value = *reinterpret_cast<volatile unsigned char*>(
            static_cast<unsigned char*>(player) + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        offset = -1;
        return -1;
    }
    // A scan that landed on something other than a bool is worse than no scan
    // at all, so retire the offset permanently the first time it disagrees.
    if (value > 1)
    {
        Game::logMsg("VR netvar at +0x%X read %u, not a bool — offset retired", offset, value);
        offset = -1;
        return -1;
    }
    return value;
}

// MOVETYPE_* of the local player, -1 when unavailable. Retired like the
// bools above when it reads past MOVETYPE_CUSTOM (11).
int Game::LocalPlayerMoveType()
{
    ScanWristHudNetVars();
    if (g_nvMoveType < 0 || !m_EngineClient || !m_ClientEntityList)
        return -1;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return -1;
    void* player = m_ClientEntityList->GetClientEntity(local);
    if (!player)
        return -1;
    unsigned char value = 0;
    BMVR_TRY
    {
        value = *reinterpret_cast<volatile unsigned char*>(
            static_cast<unsigned char*>(player) + g_nvMoveType);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_nvMoveType = -1;
        return -1;
    }
    if (value > 11)
    {
        Game::logMsg("VR movetype at +0x%X read %u, offset retired", g_nvMoveType, value);
        g_nvMoveType = -1;
        return -1;
    }
    return value;
}

// m_nWaterLevel of the local player (3 = eyes under water), -1 when unavailable.
int Game::LocalPlayerWaterLevel()
{
    ScanWristHudNetVars();
    if (g_nvWaterLevel < 0 || !m_EngineClient || !m_ClientEntityList)
        return -1;
    const int local = m_EngineClient->GetLocalPlayer();
    void* player = local > 0 ? m_ClientEntityList->GetClientEntity(local) : nullptr;
    if (!player)
        return -1;
    unsigned char value = 0;
    BMVR_TRY
    {
        value = *reinterpret_cast<volatile unsigned char*>(static_cast<unsigned char*>(player) + g_nvWaterLevel);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
    return value <= 3 ? value : -1;
}

// m_fFlags of the local player, -1 when unavailable. Retired when FL_CLIENT
// (bit 8) is missing: then the scan did not land on the player's flags.
int Game::LocalPlayerFlags()
{
    ScanWristHudNetVars();
    if (g_nvFlags < 0 || !m_EngineClient || !m_ClientEntityList)
        return -1;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return -1;
    void* player = m_ClientEntityList->GetClientEntity(local);
    if (!player)
        return -1;
    int value = 0;
    BMVR_TRY
    {
        value = *reinterpret_cast<volatile int*>(static_cast<unsigned char*>(player) + g_nvFlags);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_nvFlags = -1;
        return -1;
    }
    if (!(value & (1 << 8)))
    {
        Game::logMsg("VR m_fFlags at +0x%X read 0x%X, not player flags: offset retired", g_nvFlags, value);
        g_nvFlags = -1;
        return -1;
    }
    return value;
}

bool Game::LocalPlayerAbsOrigin(Vector& out)
{
    if (!m_EngineClient || !m_ClientEntityList)
        return false;
    const int local = m_EngineClient->GetLocalPlayer();
    void* player = local > 0 ? m_ClientEntityList->GetClientEntity(local) : nullptr;
    if (!player)
        return false;
    BMVR_TRY
    {
        out = *reinterpret_cast<const Vector*>(static_cast<const char*>(player) + Offsets::kCBaseEntity_AbsOrigin);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return out.LengthSqr() > 0.f;
}

bool Game::LocalPlayerVelocity(Vector& out)
{
    // DT_LocalPlayerExclusive m_vecVelocity[0] (Ghidra FUN_100b7240; datamap 0xF8 too).
    constexpr int kVecVelocity = 0xF8;
    if (!m_EngineClient || !m_ClientEntityList)
        return false;
    const int local = m_EngineClient->GetLocalPlayer();
    void* player = local > 0 ? m_ClientEntityList->GetClientEntity(local) : nullptr;
    if (!player)
        return false;
    BMVR_TRY
    {
        out = *reinterpret_cast<const Vector*>(static_cast<const char*>(player) + kVecVelocity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

float Game::TickInterval()
{
    float t = 0.f;
    if (m_BaseClient)
    {
        BMVR_TRY
        {
            const uintptr_t gp = *reinterpret_cast<uintptr_t*>(m_BaseClient + Offsets::kClientGpGlobalsPtr);
            if (gp)
                t = *reinterpret_cast<float*>(gp + Offsets::kCGlobalVars_interval_per_tick);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            t = 0.f;
        }
    }
    return (t >= 0.005f && t <= 0.05f) ? t : 0.015f;
}

int Game::CollectLadders(Vector* bottoms, Vector* tops, int maxLadders)
{
    static int s_index[64];
    static int s_count = 0;
    static DWORD s_scanMs = 0;
    if (!m_ClientEntityList)
        return 0;
    const DWORD now = GetTickCount();
    if (s_scanMs == 0 || now - s_scanMs > 1000)
    {
        s_scanMs = now;
        s_count = 0;
        const int hi = m_ClientEntityList->GetHighestEntityIndex();
        for (int e = 1; e <= hi && s_count < 64; ++e)
        {
            const char* name = GetEntityNetworkName(e);
            if (name && std::strcmp(name, "CFuncLadder") == 0)
                s_index[s_count++] = e;
        }
    }
    int n = 0;
    for (int i = 0; i < s_count && n < maxLadders; ++i)
    {
        const char* ent = static_cast<const char*>(m_ClientEntityList->GetClientEntity(s_index[i]));
        if (!ent)
            continue;
        BMVR_TRY
        {
            const Vector org = *reinterpret_cast<const Vector*>(ent + Offsets::kCBaseEntity_AbsOrigin);
            bottoms[n] = org + *reinterpret_cast<const Vector*>(ent + Offsets::kCFuncLadder_MountBottom);
            tops[n] = org + *reinterpret_cast<const Vector*>(ent + Offsets::kCFuncLadder_MountTop);
            ++n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    static int s_loggedCount = -1;
    if (n != s_loggedCount)
    {
        s_loggedCount = n;
        Vector me{};
        const bool haveMe = LocalPlayerAbsOrigin(me);
        if (n > 0)
            Game::logMsg("Ladders: %d on the map; first bottom (%.0f,%.0f,%.0f) top (%.0f,%.0f,%.0f), player (%.0f,%.0f,%.0f)%s",
                n, bottoms[0].x, bottoms[0].y, bottoms[0].z, tops[0].x, tops[0].y, tops[0].z,
                me.x, me.y, me.z, haveMe ? "" : " (unknown)");
        else
            Game::logMsg("Ladders: none on the map");
        for (int i = 0; i < n && i < 16; ++i)
            Game::logMsg("Ladder %d: bottom (%.0f,%.0f,%.0f) top (%.0f,%.0f,%.0f)", i,
                bottoms[i].x, bottoms[i].y, bottoms[i].z, tops[i].x, tops[i].y, tops[i].z);
    }
    return n;
}

int Game::CollectMountedGunProps(int* indexes, int maxProps)
{
    static int s_index[32];
    static int s_count = 0;
    static DWORD s_scanMs = 0;
    if (!m_ClientEntityList)
        return 0;
    const DWORD now = GetTickCount();
    if (s_scanMs == 0 || now - s_scanMs > 1000)
    {
        s_scanMs = now;
        s_count = 0;
        const int hi = m_ClientEntityList->GetHighestEntityIndex();
        for (int e = 1; e <= hi && s_count < 32; ++e)
        {
            const char* model = GetEntityModelName(GetClientEntity(e));
            if (model && (std::strstr(model, "props_marines/50cal.mdl")
                || std::strstr(model, "props_marines/tow_missile_system")))
                s_index[s_count++] = e;
        }
        static int s_loggedCount = -1;
        if (s_count != s_loggedCount)
        {
            s_loggedCount = s_count;
            Game::logMsg("Mounted guns: %d on the map", s_count);
        }
    }
    int n = 0;
    for (int i = 0; i < s_count && n < maxProps; ++i)
        indexes[n++] = s_index[i];
    return n;
}

bool Game::GetEntityAbsOrigin(C_BaseEntity* entity, Vector& out)
{
    if (!entity)
        return false;
    BMVR_TRY
    {
        out = *reinterpret_cast<const Vector*>(reinterpret_cast<const char*>(entity) + Offsets::kCBaseEntity_AbsOrigin);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return out.LengthSqr() > 0.f;
}

int Game::ModelPrecacheIndex(const char* name)
{
    if (!m_ModelInfo || !name || !name[0])
        return -1;
    BMVR_TRY
    {
        return m_ModelInfo->GetModelIndex(name);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

namespace
{
    // One engine filesystem call each, so the SEH frames hold no C++ objects.
    void* SehFsOpen(IBaseFileSystemBm* fs, const char* path)
    {
        BMVR_TRY
        {
            return fs->Open(path, "rb", "GAME");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    int SehFsReadAt(IBaseFileSystemBm* fs, void* file, int pos, void* out, int size)
    {
        BMVR_TRY
        {
            fs->Seek(file, pos, 0);
            return fs->Read(out, size, file);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    void SehFsClose(IBaseFileSystemBm* fs, void* file)
    {
        BMVR_TRY
        {
            fs->Close(file);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

// ---- Shotgun pump in two halves ----
// Black Mesa's pump.wav holds the whole stroke: back (~0-190 ms), a short
// quiet dip, forward (~215-480 ms). The manual pump plays each half at its
// own stroke (the original author's HL2VR pump had separate wavs; we cannot
// ship those, so the halves are cut from the player's own game file).
const char* Game::PumpHalfSound(bool forward)
{
    static int s_state = 0; // 0 not tried, 1 ok, -1 failed
    if (s_state == 0)
    {
        s_state = -1;
        std::vector<unsigned char> wav;
        if (m_BaseFileSystem)
        {
            if (void* file = SehFsOpen(m_BaseFileSystem, "sound/weapons/shotgun/pump.wav"))
            {
                unsigned char buf[4096];
                for (int pos = 0; pos < (4 << 20); )
                {
                    const int n = SehFsReadAt(m_BaseFileSystem, file, pos, buf, sizeof(buf));
                    if (n <= 0)
                        break;
                    wav.insert(wav.end(), buf, buf + n);
                    pos += n;
                }
                SehFsClose(m_BaseFileSystem, file);
            }
        }
        // RIFF/WAVE, PCM 16-bit: find fmt + data.
        auto u32 = [&](size_t o) { return static_cast<uint32_t>(wav[o] | (wav[o + 1] << 8) | (wav[o + 2] << 16) | (wav[o + 3] << 24)); };
        auto u16 = [&](size_t o) { return static_cast<uint32_t>(wav[o] | (wav[o + 1] << 8)); };
        size_t fmtOfs = 0, dataOfs = 0;
        uint32_t dataLen = 0;
        if (wav.size() > 44 && std::memcmp(wav.data(), "RIFF", 4) == 0 && std::memcmp(wav.data() + 8, "WAVE", 4) == 0)
        {
            for (size_t o = 12; o + 8 <= wav.size(); )
            {
                const uint32_t len = u32(o + 4);
                if (std::memcmp(wav.data() + o, "fmt ", 4) == 0)
                    fmtOfs = o + 8;
                else if (std::memcmp(wav.data() + o, "data", 4) == 0)
                {
                    dataOfs = o + 8;
                    dataLen = (std::min)(len, static_cast<uint32_t>(wav.size() - dataOfs));
                    break;
                }
                o += 8 + len + (len & 1);
            }
        }
        const uint32_t channels = fmtOfs ? u16(fmtOfs + 2) : 0;
        const uint32_t rate = fmtOfs ? u32(fmtOfs + 4) : 0;
        const uint32_t bits = fmtOfs ? u16(fmtOfs + 14) : 0;
        if (fmtOfs && dataOfs && u16(fmtOfs) == 1 && bits == 16 && channels >= 1 && channels <= 2 && rate >= 8000)
        {
            const uint32_t frameBytes = channels * 2;
            const uint32_t frames = dataLen / frameBytes;
            const int16_t* pcm = reinterpret_cast<const int16_t*>(wav.data() + dataOfs);
            // Cut at the quietest 10 ms between 150 and 260 ms.
            const uint32_t win = rate / 100;
            uint32_t cut = 0, best = UINT32_MAX;
            for (uint32_t f = rate * 150 / 1000; f + win <= (std::min)(frames, rate * 260 / 1000); f += win / 2)
            {
                uint32_t peak = 0;
                for (uint32_t k = 0; k < win * channels; ++k)
                    peak = (std::max)(peak, static_cast<uint32_t>(std::abs(static_cast<int>(pcm[f * channels + k]))));
                if (peak < best)
                {
                    best = peak;
                    cut = f + win / 2;
                }
            }
            char exe[MAX_PATH]{};
            GetModuleFileNameA(nullptr, exe, MAX_PATH - 1);
            std::string root(exe);
            root = root.substr(0, root.find_last_of("\\/") + 1);
            // The running game's folder (Blue Shift / Azure Sheep run from their own).
            const char* gameDir = bmvr::g_IsBlueShift ? "bshift" : bmvr::g_IsAzureSheep ? "asheep" : "bms";
            const std::string dir = root + gameDir + "\\sound\\bmvr\\";
            CreateDirectoryA((root + gameDir + "\\sound").c_str(), nullptr);
            CreateDirectoryA(dir.c_str(), nullptr);
            auto writePart = [&](const char* name, uint32_t from, uint32_t to) {
                std::vector<int16_t> part(pcm + from * channels, pcm + to * channels);
                // 5 ms fades so the cut does not click.
                const uint32_t fade = (std::min)(rate / 200, (to - from) / 4);
                const uint32_t nf = to - from;
                for (uint32_t i = 0; i < fade; ++i)
                    for (uint32_t c = 0; c < channels; ++c)
                    {
                        const float g = static_cast<float>(i) / static_cast<float>(fade);
                        part[i * channels + c] = static_cast<int16_t>(part[i * channels + c] * g);
                        part[(nf - 1 - i) * channels + c] = static_cast<int16_t>(part[(nf - 1 - i) * channels + c] * g);
                    }
                const uint32_t bytes = static_cast<uint32_t>(part.size() * 2);
                std::vector<unsigned char> out(44 + bytes);
                auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) out[o + i] = static_cast<unsigned char>(v >> (8 * i)); };
                auto put16 = [&](size_t o, uint32_t v) { out[o] = static_cast<unsigned char>(v); out[o + 1] = static_cast<unsigned char>(v >> 8); };
                std::memcpy(&out[0], "RIFF", 4); put32(4, 36 + bytes); std::memcpy(&out[8], "WAVEfmt ", 8);
                put32(16, 16); put16(20, 1); put16(22, channels); put32(24, rate); put32(28, rate * frameBytes);
                put16(32, frameBytes); put16(34, 16); std::memcpy(&out[36], "data", 4); put32(40, bytes);
                std::memcpy(&out[44], part.data(), bytes);
                const std::string path = dir + name;
                const std::string tmp = path + ".tmp";
                HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    return false;
                DWORD wrote = 0;
                const BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
                CloseHandle(h);
                if (!ok || wrote != out.size())
                    return false;
                return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            };
            if (cut > 0 && cut < frames && writePart("pump_back.wav", 0, cut) && writePart("pump_forward.wav", cut, frames))
                s_state = 1;
            Game::logMsg("Shotgun pump sound split at %u ms of %u ms: %s", cut * 1000 / rate, frames * 1000 / rate,
                s_state == 1 ? "ok" : "write failed");
        }
        else
            Game::logMsg("Shotgun pump sound split: pump.wav not read (%zu bytes, fmt %zu, data %zu)", wav.size(), fmtOfs, dataOfs);
    }
    if (s_state != 1)
        return nullptr;
    return forward ? "bmvr/pump_forward.wav" : "bmvr/pump_back.wav";
}

int Game::ReadMapEntityLump(const char* map, std::string& out)
{
    out.clear();
    if (!m_BaseFileSystem || !map || !map[0])
        return -1;
    char path[160];
    std::snprintf(path, sizeof(path), "maps/%s.bsp", map);
    void* file = SehFsOpen(m_BaseFileSystem, path);
    if (!file)
        return -2;
    // dheader_t: ident, version, then lump_t { fileofs, filelen, version, fourCC }[64].
    int header[4] = {};
    int rc = 0;
    if (SehFsReadAt(m_BaseFileSystem, file, 0, header, sizeof(header)) != static_cast<int>(sizeof(header))
        || std::memcmp(header, "VBSP", 4) != 0)
        rc = -3;
    const int ofs = header[2];
    const int len = header[3];
    if (rc == 0 && (ofs < static_cast<int>(sizeof(header)) || len <= 0 || len > (16 << 20)))
        rc = -4;
    if (rc == 0)
    {
        out.resize(static_cast<size_t>(len));
        if (SehFsReadAt(m_BaseFileSystem, file, ofs, &out[0], len) != len)
            rc = -5;
        else if (out.compare(0, 4, "LZMA") == 0)
            rc = -6;
        else if (out.find('{') > 64)
            rc = -4;
    }
    SehFsClose(m_BaseFileSystem, file);
    if (rc != 0)
    {
        out.clear();
        return rc;
    }
    return len;
}

namespace
{
    int ReadEntityModelIndexRaw(const void* entity)
    {
        BMVR_TRY
        {
            return *reinterpret_cast<const short*>(reinterpret_cast<uintptr_t>(entity) + 0x94);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }
}

int Game::CollectEntitiesWithModelIndex(const int* modelIndexes, int modelCount, int* out, int maxOut)
{
    if (!m_ClientEntityList || !modelIndexes || modelCount <= 0 || !out || maxOut <= 0)
        return 0;
    int n = 0;
    const int hi = m_ClientEntityList->GetHighestEntityIndex();
    for (int e = 1; e <= hi && n < maxOut; ++e)
    {
        const void* ent = m_ClientEntityList->GetClientEntity(e);
        if (!ent)
            continue;
        const int mi = ReadEntityModelIndexRaw(ent);
        if (mi <= 0)
            continue;
        for (int m = 0; m < modelCount; ++m)
        {
            if (modelIndexes[m] > 0 && mi == modelIndexes[m])
            {
                out[n++] = e;
                break;
            }
        }
    }
    return n;
}

int Game::VisibleEntityModelIndex(int entityIndex, Vector& origin)
{
    if (!m_ClientEntityList || entityIndex <= 0)
        return -1;
    void* ent = m_ClientEntityList->GetClientEntity(entityIndex);
    if (!ent)
        return -1;
    BMVR_TRY
    {
        // Dormancy (out of PVS, stale origin) is not checked here: the caller
        // traces line of sight instead of calling an unverified vtable slot.
        const int fx = *reinterpret_cast<const int*>(
            reinterpret_cast<const char*>(ent) + Offsets::kCBaseEntity_fEffects);
        if (fx & Offsets::kEF_NODRAW)
            return -1;
        origin = *reinterpret_cast<const Vector*>(
            reinterpret_cast<const char*>(ent) + Offsets::kCBaseEntity_AbsOrigin);
        return *reinterpret_cast<const short*>(reinterpret_cast<const char*>(ent) + 0x94);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

bool Game::LocalPlayerHasSuit()
{
    ScanWristHudNetVars();
    int wearing = ReadPlayerFlagByte(g_nvWearingSuit);
    if (wearing < 0)
        return true;
    // Dying strips the suit (PlayerDeathThink -> PackDeadPlayerItems ->
    // RemoveAllItems(true)), so the gloves turned bare while dead. Keep the
    // last alive value until the player lives again (reload restores it).
    static int s_aliveWearing = -1;
    int health = 1;
    if (g_nvHealth > 0 && m_EngineClient && m_ClientEntityList)
    {
        const int local = m_EngineClient->GetLocalPlayer();
        void* player = local > 0 ? m_ClientEntityList->GetClientEntity(local) : nullptr;
        if (player)
        {
            BMVR_TRY
            {
                health = *reinterpret_cast<volatile int*>(static_cast<unsigned char*>(player) + g_nvHealth);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                health = 1;
            }
        }
    }
    if (health <= 0 && s_aliveWearing >= 0)
        wearing = s_aliveWearing;
    else if (health > 0)
        s_aliveWearing = wearing;
    static int s_suitLog;
    static int s_lastWearing = -1;
    if (wearing != s_lastWearing && s_suitLog < 8)
    {
        Game::logMsg("HEV suit %s (m_bWearingSuit at +0x%X)",
            wearing ? "on" : "off", g_nvWearingSuit);
        s_lastWearing = wearing;
        ++s_suitLog;
    }
    return wearing != 0;
}

bool Game::LocalPlayerZooming()
{
    return LocalPlayerZoomFlag() == 1;
}

int Game::LocalPlayerZoomFlag()
{
    ScanWristHudNetVars();
    return ReadPlayerFlagByte(g_nvZooming);
}

int Game::RpgLaserOnOffset() const
{
    return g_nvRpgLaserOn > 0 ? g_nvRpgLaserOn : Offsets::kCWeaponRpg_bLaserOn;
}

bool Game::RpgLaserOnResolved() const
{
    return g_nvRpgLaserOn > 0;
}

int Game::RpgLaserDotOffset() const
{
    return g_nvRpgLaserDot > 0 ? g_nvRpgLaserDot : Offsets::kCWeaponRpg_hLaserDot;
}

C_BaseEntity* Game::FindEntityByNetworkNameContains(const char* token)
{
    if (!token || !token[0] || !m_ClientEntityList)
        return nullptr;
    char needle[64]{};
    size_t n = 0;
    for (; n + 1 < sizeof(needle) && token[n]; ++n)
        needle[n] = static_cast<char>(tolower(static_cast<unsigned char>(token[n])));
    needle[n] = 0;
    const int hi = m_ClientEntityList->GetHighestEntityIndex();
    for (int e = 1; e <= hi; ++e)
    {
        const char* name = GetEntityNetworkName(e);
        if (!name || !name[0])
            continue;
        char lower[80]{};
        size_t i = 0;
        for (; i + 1 < sizeof(lower) && name[i]; ++i)
            lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
        lower[i] = 0;
        if (std::strstr(lower, needle))
            return static_cast<C_BaseEntity*>(m_ClientEntityList->GetClientEntity(e));
    }
    return nullptr;
}

int Game::ReadWeaponReserve(C_BaseEntity* weapon)
{
    if (!weapon)
        return -1;
    if (!g_nvScanned)
        ScanWristHudNetVars();
    C_BaseEntity* player = GetLocalPlayerEntity();
    if (!player || g_nvAmmoBase < 0 || g_nvPrimaryAmmoType < 0)
        return -1;
    BMVR_TRY
    {
        const int type = *reinterpret_cast<volatile int*>(reinterpret_cast<unsigned char*>(weapon) + g_nvPrimaryAmmoType);
        if (type < 0 || type >= 32)
            return -1;
        return *reinterpret_cast<volatile int*>(reinterpret_cast<unsigned char*>(player) + g_nvAmmoBase + 4 * type);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

int Game::ReadWeaponClip(C_BaseEntity* weapon)
{
    if (!weapon)
        return -1;
    if (!g_nvScanned)
        ScanWristHudNetVars();
    if (g_nvClip1 < 0)
        return -1;
    int clip = -1;
    BMVR_TRY
    {
        clip = *reinterpret_cast<volatile int*>(
            reinterpret_cast<unsigned char*>(weapon) + g_nvClip1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
    return clip;
}

int Game::ReadEntityBody(void* entity)
{
    if (!g_nvScanned)
        ScanWristHudNetVars();
    if (!entity || g_nvBody < 0)
        return -1;
    BMVR_TRY
    {
        return *reinterpret_cast<volatile int*>(reinterpret_cast<unsigned char*>(entity) + g_nvBody);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

int Game::ReadWeaponReloadFlags(void* weapon, int& needReload, int& inReload)
{
    if (!g_nvScanned)
        ScanWristHudNetVars();
    needReload = inReload = -1;
    if (!weapon)
        return -1;
    BMVR_TRY
    {
        if (g_nvNeedReload >= 0)
            needReload = *reinterpret_cast<volatile unsigned char*>(reinterpret_cast<unsigned char*>(weapon) + g_nvNeedReload);
        if (g_nvInReload >= 0)
            inReload = *reinterpret_cast<volatile unsigned char*>(reinterpret_cast<unsigned char*>(weapon) + g_nvInReload);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
    return 0;
}

bool Game::WriteEntityBody(void* entity, int body)
{
    if (!entity || g_nvBody < 0)
        return false;
    BMVR_TRY
    {
        *reinterpret_cast<volatile int*>(reinterpret_cast<unsigned char*>(entity) + g_nvBody) = body;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Game::WeaponHasNoAmmo(C_BaseEntity* weapon)
{
    if (!weapon)
        return false;
    if (!g_nvScanned)
        ScanWristHudNetVars();
    auto rdInt = [](void* ent, int off) -> int {
        if (!ent || off < 0)
            return -1;
        BMVR_TRY
        {
            return *reinterpret_cast<volatile int*>(reinterpret_cast<unsigned char*>(ent) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    };
    const int ammoType = rdInt(weapon, g_nvPrimaryAmmoType);
    if (ammoType < 0)
        return false;
    const int clip = rdInt(weapon, g_nvClip1);
    int reserve = -1;
    C_BaseEntity* player = GetLocalPlayerEntity();
    if (player && g_nvAmmoBase >= 0 && ammoType < 32)
        reserve = rdInt(player, g_nvAmmoBase + 4 * ammoType);
    if (clip < 0)
        return reserve == 0;
    if (reserve < 0)
        return clip == 0;
    return clip <= 0 && reserve <= 0;
}

bool Game::ReadLocalPlayerViewEntitySet(bool& set)
{
    ScanWristHudNetVars();
    if (!m_EngineClient || !m_ClientEntityList || g_nvViewEntity < 0)
        return false;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return false;
    void* player = m_ClientEntityList->GetClientEntity(local);
    if (!player)
        return false;
    BMVR_TRY
    {
        const unsigned int h = *reinterpret_cast<volatile unsigned int*>(
            reinterpret_cast<unsigned char*>(player) + g_nvViewEntity);
        set = h != 0xFFFFFFFFu && h != 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Game::ReadWristHudValues(int& health, int& armor, int& clip, int& reserve, int& secondary, int& air)
{
    ScanWristHudNetVars();
    health = armor = clip = reserve = secondary = air = -1;
    if (!m_EngineClient || !m_ClientEntityList)
        return false;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return false;
    void* player = m_ClientEntityList->GetClientEntity(local);
    if (!player)
        return false;
    auto rdInt = [](void* ent, int off) -> int {
        if (!ent || off < 0)
            return -1;
        BMVR_TRY
        {
            return *reinterpret_cast<volatile int*>(reinterpret_cast<unsigned char*>(ent) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    };
    auto rdByte = [](void* ent, int off) -> int {
        if (!ent || off < 0)
            return -1;
        BMVR_TRY
        {
            return *reinterpret_cast<volatile unsigned char*>(reinterpret_cast<unsigned char*>(ent) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    };
    auto rdFloat = [](void* ent, int off) -> float {
        if (!ent || off < 0)
            return -1.f;
        BMVR_TRY
        {
            return *reinterpret_cast<volatile float*>(reinterpret_cast<unsigned char*>(ent) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1.f;
        }
    };
    health = rdInt(player, g_nvHealth);
    armor = rdInt(player, g_nvArmor);
    void* weapon = GetActiveWeaponEntity();
    clip = rdInt(weapon, g_nvClip1);
    const int clip2 = rdInt(weapon, g_nvClip2);
    if (clip < 0 && clip2 >= 0)
        clip = clip2;
    // Manual magazines: what is really in the gun (no magazine = 0, the
    // shotgun's own shell count).
    if (const int shown = MagReload::g_DisplayClip.load(std::memory_order_relaxed); shown >= 0)
        clip = shown;
    const int ammoType = rdInt(weapon, g_nvPrimaryAmmoType);
    if (player && g_nvAmmoBase >= 0 && ammoType >= 0 && ammoType < 32)
        reserve = rdInt(player, g_nvAmmoBase + 4 * ammoType);
    if (reserve >= 0 && MagReload::g_DisplayClip.load(std::memory_order_relaxed) >= 0)
        reserve += MagReload::g_DisplayReserveExtra.load(std::memory_order_relaxed);
    const int secType = rdInt(weapon, g_nvSecondaryAmmoType);
    if (secType >= 0 && secType < 32 && player && g_nvAmmoBase >= 0)
        secondary = rdInt(player, g_nvAmmoBase + 4 * secType);
    else if (clip2 >= 0 && clip2 <= 255)
        secondary = clip2;

    // CHudAir::OnThink (FUN_10271dd0): visible only at waterlevel == 3 (eyes).
    // Percent is (m_AirFinished - gpGlobals->curtime) / player_airtimer * 100.
    const int water = rdByte(player, g_nvWaterLevel);
    if (water == 3)
    {
        float curtime = -1.f;
        if (m_BaseClient)
        {
            BMVR_TRY
            {
                const uintptr_t gp = *reinterpret_cast<uintptr_t*>(
                    m_BaseClient + Offsets::kClientGpGlobalsPtr);
                if (gp)
                    curtime = *reinterpret_cast<float*>(gp + Offsets::kCGlobalVars_curtime);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                curtime = -1.f;
            }
        }
        const float finished = rdFloat(player, g_nvAirFinished);
        float maxAir = 12.f;
        if (!GetConVarFloat("player_airtimer", maxAir) || !(maxAir > 0.5f) || maxAir > 120.f)
            maxAir = 12.f;
        if (curtime >= 0.f && finished > 0.f)
        {
            float pct = (finished - curtime) / maxAir * 100.f;
            if (pct < 0.f)
                pct = 0.f;
            if (pct > 100.f)
                pct = 100.f;
            air = static_cast<int>(pct + 0.5f);
        }
        else
            air = 0;
    }

    static int s_hudLog;
    if (s_hudLog < 4)
    {
        Game::logMsg("WristHUD values health=%d armor=%d clip=%d reserve=%d sec=%d air=%d water=%d (off h=%d a=%d clip2=%d secType=%d water=%d airF=%d)",
            health, armor, clip, reserve, secondary, air, water, g_nvHealth, g_nvArmor, g_nvClip2, g_nvSecondaryAmmoType,
            g_nvWaterLevel, g_nvAirFinished);
        ++s_hudLog;
    }
    return true;
}

int Game::CycleWeaponSelect(int direction)
{
    // invnext/invprev via ClientCmd_Unrestricted crash BM from CreateMove
    // (2026-08-18, same bucket as gameui_activate). Drive CUserCmd::weaponselect
    // from DT_BaseCombatCharacter::m_hMyWeapons instead.
    if (!m_EngineClient || !m_ClientEntityList || direction == 0)
        return 0;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return 0;
    void* player = m_ClientEntityList->GetClientEntity(local);
    if (!player)
        return 0;

    // client.dll FUN_100a3de0 RecvTable: m_hMyWeapons 0xEE4 count 0x30,
    // m_hActiveWeapon 0xFA4. EHANDLE RecvProxy FUN_101d0220 uses 13-bit entry
    // (mask 0x1FFF) with 12-bit networked index.
    constexpr int kMyWeapons = 0xEE4;
    constexpr int kActiveWeapon = 0xFA4;
    constexpr int kMaxWeapons = 48;
    constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    constexpr uint32_t kEntryMask = 0x1FFFu;

    const auto* handles = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(player) + kMyWeapons);
    const uint32_t active = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(player) + kActiveWeapon);

    int indices[kMaxWeapons];
    int n = 0;
    int current = -1;
    for (int i = 0; i < kMaxWeapons; ++i)
    {
        const uint32_t h = handles[i];
        if (h == 0 || h == kInvalid)
            continue;
        void* weapon = m_ClientEntityList->GetClientEntityFromHandle(static_cast<int>(h));
        if (!weapon)
            continue;
        int idx = static_cast<int>(h & kEntryMask);
        if (m_ClientEntityList->GetClientEntity(idx) != weapon)
        {
            idx = 0;
            const int hi = m_ClientEntityList->GetHighestEntityIndex();
            for (int e = 1; e <= hi; ++e)
            {
                if (m_ClientEntityList->GetClientEntity(e) == weapon)
                {
                    idx = e;
                    break;
                }
            }
        }
        if (idx <= 0)
            continue;
        if (h == active)
            current = n;
        indices[n++] = idx;
    }
    if (n <= 1)
        return 0;
    int next = 0;
    if (current >= 0)
        next = (current + (direction > 0 ? 1 : n - 1)) % n;
    else
        next = direction > 0 ? 0 : n - 1;
    return indices[next];
}

int Game::CollectInventoryWeapons(InventoryWeapon* out, int maxCount)
{
    if (!out || maxCount <= 0 || !m_EngineClient || !m_ClientEntityList)
        return 0;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return 0;
    void* player = m_ClientEntityList->GetClientEntity(local);
    if (!player)
        return 0;

    constexpr int kMyWeapons = 0xEE4;
    constexpr int kMaxWeapons = 48;
    constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    constexpr uint32_t kEntryMask = 0x1FFFu;

    const auto* handles = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(player) + kMyWeapons);
    int n = 0;
    for (int i = 0; i < kMaxWeapons && n < maxCount; ++i)
    {
        uint32_t h = 0;
        BMVR_TRY
        {
            h = handles[i];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            break;
        }
        if (h == 0 || h == kInvalid)
            continue;
        void* weapon = m_ClientEntityList->GetClientEntityFromHandle(static_cast<int>(h));
        if (!weapon)
            continue;
        int idx = static_cast<int>(h & kEntryMask);
        if (m_ClientEntityList->GetClientEntity(idx) != weapon)
        {
            idx = 0;
            const int hi = m_ClientEntityList->GetHighestEntityIndex();
            for (int e = 1; e <= hi; ++e)
            {
                if (m_ClientEntityList->GetClientEntity(e) == weapon)
                {
                    idx = e;
                    break;
                }
            }
        }
        if (idx <= 0)
            continue;
        out[n].entityIndex = idx;
        out[n].modelName = GetEntityModelName(static_cast<C_BaseEntity*>(weapon));
        out[n].networkName = GetEntityNetworkName(idx);
        ++n;
    }
    return n;
}

bool Game::ClientCmd(const char* szCmdString)
{
    if (!m_EngineClient || !szCmdString)
        return false;
    bool ok = false;
    BMVR_TRY
    {
        m_EngineClient->ClientCmd(szCmdString);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logMsg("ClientCmd SEH for '%s'", szCmdString);
    }
    return ok;
}

bool Game::ClientCmd_Unrestricted(const char* szCmdString)
{
    if (!m_EngineClient || !szCmdString)
        return false;
    bool ok = false;
    BMVR_TRY
    {
        m_EngineClient->ClientCmd_Unrestricted(szCmdString);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logMsg("ClientCmd_Unrestricted SEH for '%s'", szCmdString);
    }
    return ok;
}

bool Game::TryGetLightForPoint(const Vector& pos, Vector& outRgb, bool clamp) const
{
    outRgb.x = 1.f;
    outRgb.y = 1.f;
    outRgb.z = 1.f;
    static bool s_broken;
    if (s_broken || !m_EngineClient)
        return false;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
        return false;

    void** vt = nullptr;
    BMVR_TRY
    {
        vt = *reinterpret_cast<void***>(m_EngineClient);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt = nullptr;
    }
    if (!vt || !vt[Offsets::kIEngineClient_GetLightForPoint])
        return false;

    // MSVC x86 thiscall: Vector-by-value is a hidden first stack pointer.
    using Fn = Vector* (__thiscall*)(void*, Vector*, const Vector*, int);
    Vector sampled;
    sampled.x = 0.f;
    sampled.y = 0.f;
    sampled.z = 0.f;
    Vector* ret = nullptr;
    BMVR_TRY
    {
        ret = reinterpret_cast<Fn>(vt[Offsets::kIEngineClient_GetLightForPoint])(
            m_EngineClient, &sampled, &pos, clamp ? 1 : 0);
        (void)ret;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s_broken = true;
        logMsg("GetLightForPoint SEH — VR glove scene light disabled");
        return false;
    }

    if (!std::isfinite(sampled.x) || !std::isfinite(sampled.y) || !std::isfinite(sampled.z)
        || sampled.x < -0.05f || sampled.y < -0.05f || sampled.z < -0.05f
        || sampled.x > 8.f || sampled.y > 8.f || sampled.z > 8.f)
    {
        static int s_junk;
        if (s_junk < 4)
        {
            logMsg("GetLightForPoint junk (%.3f %.3f %.3f) — using fill light",
                sampled.x, sampled.y, sampled.z);
            ++s_junk;
        }
        return false;
    }

    outRgb = sampled;
    static int s_ok;
    if (s_ok < 4)
    {
        logMsg("GetLightForPoint (%.1f %.1f %.1f) rgb=(%.3f %.3f %.3f)",
            pos.x, pos.y, pos.z, outRgb.x, outRgb.y, outRgb.z);
        ++s_ok;
    }
    return true;
}

void Game::ResolveMaterialThreadSlots() const
{
    m_MatSetThreadSlot = -1;
    m_MatGetThreadSlot = -1;
    m_MatExecuteQueuedSlot = -1;
    m_MatEndFrameSlot = -1;
    m_MatSlotsValid = false;

    if (!m_MaterialSystem)
    {
        logMsg("IMaterialSystem thread slots: no MaterialSystem");
        return;
    }

    void** vtbl = nullptr;
    BMVR_TRY
    {
        vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vtbl = nullptr;
    }
    if (!vtbl)
    {
        logMsg("IMaterialSystem thread slots: no vtable");
        return;
    }

    uintptr_t gbdAddr = 0;
    if (m_Offsets && m_Offsets->GetBackBufferDimensions.valid)
        gbdAddr = static_cast<uintptr_t>(m_Offsets->GetBackBufferDimensions.address);
    if (!gbdAddr)
    {
        HMODULE mat = GetModuleHandleA("materialsystem.dll");
        if (!mat)
            mat = GetModuleHandleA("MaterialSystem.dll");
        if (mat)
            gbdAddr = reinterpret_cast<uintptr_t>(mat) + 0x53EF0;
    }
    if (!gbdAddr)
    {
        logMsg("IMaterialSystem thread slots: no GetBackBufferDimensions address");
        return;
    }

    const void* gbd = reinterpret_cast<void*>(gbdAddr);
    int gbdSlot = -1;
    BMVR_TRY
    {
        for (int i = 0; i < 96; ++i)
        {
            if (vtbl[i] == gbd)
            {
                gbdSlot = i;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        gbdSlot = -1;
    }
    if (gbdSlot < 0)
    {
        logMsg("IMaterialSystem GBD %p not in vtbl[0..95]; skip Get/SetThreadMode and EndFrame", gbd);
        return;
    }

    const int delta = gbdSlot - Offsets::kDumpGetBackBufferDimensionsVt;
    m_MatSetThreadSlot = Offsets::kDumpSetThreadModeVt + delta;
    m_MatGetThreadSlot = Offsets::kDumpGetThreadModeVt + delta;
    m_MatExecuteQueuedSlot = Offsets::kDumpExecuteQueuedVt + delta;
    m_MatEndFrameSlot = Offsets::kDumpEndFrameVt + delta;
    m_MatSlotsValid = (m_MatSetThreadSlot >= 0 && m_MatGetThreadSlot >= 0
        && m_MatExecuteQueuedSlot >= 0 && vtbl[m_MatSetThreadSlot]
        && vtbl[m_MatGetThreadSlot] && vtbl[m_MatExecuteQueuedSlot]);
    logMsg("IMaterialSystem vtable GBD slot=%d delta=%d SetThreadMode=%d GetThreadMode=%d ExecuteQueued=%d EndFrame=%d valid=%d",
        gbdSlot, delta, m_MatSetThreadSlot, m_MatGetThreadSlot,
        m_MatExecuteQueuedSlot, m_MatEndFrameSlot, m_MatSlotsValid ? 1 : 0);
}

int Game::GetMatQueueMode() const
{
    // Cache only. DXVK Present calls this with the device lock held
    // (x32dbg PID 32704, 2026-08-18: GetThreadMode nested stdshader memcpy
    // until Not Responding). Probe from RenderView, not from D3D.
    return m_CachedMatQueueMode.load(std::memory_order_acquire);
}

void Game::ProbeMatQueueModeFromRenderView()
{
    if (!bmvr::TryMatQueue() || !m_MaterialSystem || !m_MatSlotsValid
        || !m_VR || !m_VR->IsGameplayEligible()
        || !m_EngineClient || !m_EngineClient->IsInGame()
        || !m_VR->PassThroughWarmupDone())
    {
        m_CachedMatQueueMode.store(0, std::memory_order_release);
        return;
    }
    // Do not call GetThreadMode here. GBD is dump slot 30 so slot 11 is the
    // real GetThreadMode, but the first stereo RenderView probe stalled
    // ~9s/frame (2026-08-26). Present must not call it either (device lock,
    // 2026-08-18). Trust SetThreadMode + cache.
}

bool Game::MaterialVTableMatchesDump() const
{
    if (!m_MaterialSystem || !m_Offsets || !m_Offsets->GetBackBufferDimensions.valid)
        return false;
    BMVR_TRY
    {
        void** vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
        if (!vtbl)
            return false;
        return vtbl[30] == reinterpret_cast<void*>(m_Offsets->GetBackBufferDimensions.address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Game::SetMatQueueMode(int mode) const
{
    if (!m_MaterialSystem || mode < 0 || mode > 2 || !m_MatSlotsValid)
        return false;
    // Mode 2 is the queued path. Mode 0 must stay callable so we can undo an
    // archived cvar 2 / leftover SetThreadMode without waiting for warmup.
    if (mode != 0)
    {
        if (!m_VR || !m_VR->IsGameplayEligible())
            return false;
        if (!m_EngineClient || !m_EngineClient->IsInGame())
            return false;
        if (!m_VR->PassThroughWarmupDone())
            return false;
    }
    BMVR_TRY
    {
        void** vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
        if (!vtbl || m_MatSetThreadSlot < 0 || !vtbl[m_MatSetThreadSlot])
            return false;
        using tSetThreadMode = void(__thiscall*)(IMaterialSystem*, int, int);
        reinterpret_cast<tSetThreadMode>(vtbl[m_MatSetThreadSlot])(m_MaterialSystem, mode, -1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    m_CachedMatQueueMode.store(mode, std::memory_order_release);
    const bool cvarOk = SetConVarInt("mat_queue_mode", mode);
    static int s_cvarLog;
    if (s_cvarLog < 8)
    {
        logMsg("mat_queue_mode cvar write %d ok=%d (console reads this; SetThreadMode is separate)",
            mode, cvarOk ? 1 : 0);
        ++s_cvarLog;
    }
    return true;
}

void Game::ExecuteQueuedMaterials() const
{
    if (!m_MaterialSystem)
        return;
    if (!m_MatSlotsValid || m_CachedMatQueueMode.load(std::memory_order_acquire) == 0)
        return;
    bool ran = false;
    BMVR_TRY
    {
        void** vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
        if (vtbl && m_MatExecuteQueuedSlot >= 0 && vtbl[m_MatExecuteQueuedSlot])
        {
            using tExecuteQueued = void(__thiscall*)(IMaterialSystem*);
            reinterpret_cast<tExecuteQueued>(vtbl[m_MatExecuteQueuedSlot])(m_MaterialSystem);
            ran = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ran = false;
    }
    static int s_eqLog;
    if (ran && s_eqLog < 4)
    {
        logMsg("ExecuteQueued after stereo eye (cachedMode=%d)",
            m_CachedMatQueueMode.load(std::memory_order_acquire));
        ++s_eqLog;
    }
}

namespace
{
    const char* SehCStringAt(void* obj, int offset)
    {
        const char* s = nullptr;
        BMVR_TRY
        {
            s = *reinterpret_cast<const char**>(reinterpret_cast<uint8_t*>(obj) + offset);
            if (s && s[0] == '\0')
                s = nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s = nullptr;
        }
        return s;
    }

    const char* ConVarGetName(void* cvar, int& nameSlot)
    {
        nameSlot = -1;
        if (!cvar)
            return nullptr;
        void** vt = nullptr;
        BMVR_TRY { vt = *reinterpret_cast<void***>(cvar); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        if (!vt)
            return nullptr;
        // 2007 ConCommandBase::GetName is slot 4; 2013 is slot 6.
        const int slots[] = { 4, 6 };
        for (int slot : slots)
        {
            const char* n = nullptr;
            BMVR_TRY
            {
                if (!vt[slot])
                    continue;
                using tGetName = const char*(__thiscall*)(void*);
                n = reinterpret_cast<tGetName>(vt[slot])(cvar);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                n = nullptr;
            }
            if (n && n[0])
            {
                nameSlot = slot;
                return n;
            }
        }
        for (int off : { 0x0C, 0x10, 0x14 })
        {
            if (const char* n = SehCStringAt(cvar, off))
            {
                nameSlot = -off;
                return n;
            }
        }
        return nullptr;
    }

    bool ConVarNameIs(void* cvar, const char* name)
    {
        int slot = -1;
        const char* got = ConVarGetName(cvar, slot);
        return got && name && std::strcmp(got, name) == 0;
    }

    void* FindConVarAtSlot(void* icvar, const char* name, int slot)
    {
        if (!icvar || !name || slot < 0)
            return nullptr;
        void* found = nullptr;
        BMVR_TRY
        {
            void** vt = *reinterpret_cast<void***>(icvar);
            if (!vt || !vt[slot])
                return nullptr;
            using tFindVar = void*(__thiscall*)(void*, const char*);
            found = reinterpret_cast<tFindVar>(vt[slot])(icvar, name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            found = nullptr;
        }
        return found;
    }

    void* FindConVarProbe(void* icvar, const char* name)
    {
        if (!icvar || !name)
            return nullptr;
        static void* s_icvar;
        static char s_name[64];
        static void* s_found;
        static int s_slot = -1;
        if (s_icvar == icvar && s_found && std::strcmp(s_name, name) == 0)
            return s_found;
        // VEngineCvar004 on BM is the 2013 ICvar vtable (RTTI CCvar in
        // vstdlib.dll). IAppSystem 0-7, AllocateDLLIdentifier 8, Register 9,
        // Unregister 10/11, GetCommandLineValue 12, FindCommandBase 13/14,
        // FindVar 15, FindVar const 16. Slots 9 and 12 returned null (those
        // are Register / GetCommandLineValue). Never scan a range.
        const int slots[] = { 15, 16 };
        void* found = nullptr;
        int used = -1;
        void* firstRaw = nullptr;
        int rawSlot = -1;
        for (int slot : slots)
        {
            void* raw = FindConVarAtSlot(icvar, name, slot);
            if (!raw)
                continue;
            if (rawSlot < 0)
            {
                rawSlot = slot;
                firstRaw = raw;
            }
            if (!ConVarNameIs(raw, name))
                continue;
            found = raw;
            used = slot;
            break;
        }
        static int s_log;
        const bool always = name && std::strcmp(name, "mat_queue_mode") == 0;
        if (always || s_log < 8)
        {
            int nameSlot = -1;
            void* logPtr = found ? found : firstRaw;
            const char* got = logPtr ? ConVarGetName(logPtr, nameSlot) : "";
            Game::logMsg("ICvar FindVar '%s' slot=%d ptr=%p rawSlot=%d raw=%p name='%s' nameSlot=%d",
                name, used, found, rawSlot, firstRaw, got ? got : "", nameSlot);
            if (!always)
                ++s_log;
        }
        if (!found)
            return nullptr;
        s_icvar = icvar;
        std::strncpy(s_name, name, sizeof(s_name) - 1);
        s_found = found;
        s_slot = used;
        (void)s_slot;
        return found;
    }

    bool ConVarWriteFields(void* cvar, float fval, int ival, bool writeStringAsInt)
    {
        if (!cvar)
            return false;
        void* parent = nullptr;
        BMVR_TRY { parent = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cvar) + 0x1C); }
        __except (EXCEPTION_EXECUTE_HANDLER) { parent = nullptr; }
        void* objs[2] = { cvar, (parent && parent != cvar) ? parent : nullptr };
        bool ok = false;
        for (void* obj : objs)
        {
            if (!obj)
                continue;
            uint8_t* b = reinterpret_cast<uint8_t*>(obj);
            union { float f; uint32_t u; } fu;
            fu.f = fval;
            fu.u ^= reinterpret_cast<uint32_t>(obj);
            const uint32_t storedInt = static_cast<uint32_t>(ival) ^ reinterpret_cast<uint32_t>(obj);
            BMVR_TRY
            {
                *reinterpret_cast<uint32_t*>(b + 0x2C) = fu.u;
                *reinterpret_cast<uint32_t*>(b + 0x30) = storedInt;
                if (writeStringAsInt)
                {
                    char* str = *reinterpret_cast<char**>(b + 0x24);
                    const int cap = *reinterpret_cast<int*>(b + 0x28);
                    if (str && cap >= 2)
                    {
                        _snprintf(str, static_cast<size_t>(cap), "%d", ival);
                        str[cap - 1] = '\0';
                    }
                }
                ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return ok;
    }

    bool ConVarSetInt(void* cvar, int value)
    {
        if (!cvar)
            return false;
        int nameSlot = -1;
        if (!ConVarGetName(cvar, nameSlot) || nameSlot < 0)
            return false;
        // Do not call virtual SetValue(int). FCVAR_MATERIAL_THREAD cvars
        // queue through IMaterialSystem vt[0x88] and crashed on mat_vsync
        // during the first RenderView (2026-08-26). Console reads the xor'd
        // int at +0x30 and the string at +0x24 (InternalSetValue layout).
        return ConVarWriteFields(cvar, static_cast<float>(value), value, true);
    }

    bool ConVarSetFloat(void* cvar, float value)
    {
        if (!cvar)
            return false;
        int nameSlot = -1;
        if (!ConVarGetName(cvar, nameSlot) || nameSlot < 0)
            return false;
        return ConVarWriteFields(cvar, value, static_cast<int>(value), true);
    }
}

void* Game::FindConVar(const char* name) const
{
    return FindConVarProbe(m_Cvar, name);
}

bool Game::SetConVarInt(const char* name, int value) const
{
    void* cvar = FindConVarProbe(m_Cvar, name);
    if (!cvar)
        return false;
    return ConVarSetInt(cvar, value);
}

bool Game::SetConVarFloat(const char* name, float value) const
{
    void* cvar = FindConVarProbe(m_Cvar, name);
    if (!cvar)
        return false;
    return ConVarSetFloat(cvar, value);
}

bool Game::GetConVarInt(const char* name, int& value) const
{
    value = 0;
    void* cvar = FindConVarProbe(m_Cvar, name);
    if (!cvar)
        return false;
    uint32_t stored = 0;
    BMVR_TRY
    {
        stored = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(cvar) + 0x30);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    value = static_cast<int>(stored ^ reinterpret_cast<uint32_t>(cvar));
    return true;
}

bool Game::GetConVarFloat(const char* name, float& value) const
{
    value = 0.f;
    void* cvar = FindConVarProbe(m_Cvar, name);
    if (!cvar)
        return false;
    uint32_t stored = 0;
    BMVR_TRY
    {
        stored = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(cvar) + 0x2C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    const uint32_t bits = stored ^ reinterpret_cast<uint32_t>(cvar);
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
}

void Game::logMsg(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    bmvr::Log("%s", buf);
}

namespace
{
    std::mutex g_MissingOffsetsMutex;
    std::set<std::string> g_MissingOffsets;
    std::set<std::string> g_MissingDebugOffsets;
}

void Game::noteMissingOffset(const char* what, bool debugOnly)
{
    std::lock_guard<std::mutex> lock(g_MissingOffsetsMutex);
    (debugOnly ? g_MissingDebugOffsets : g_MissingOffsets).insert(what ? what : "?");
}

int Game::missingOffsetCount()
{
    std::lock_guard<std::mutex> lock(g_MissingOffsetsMutex);
    return static_cast<int>(g_MissingOffsets.size());
}

int Game::missingDebugOffsetCount()
{
    std::lock_guard<std::mutex> lock(g_MissingOffsetsMutex);
    return static_cast<int>(g_MissingDebugOffsets.size());
}

void Game::errorMsg(const char* msg)
{
    logMsg("[ERROR] %s", msg);
}
