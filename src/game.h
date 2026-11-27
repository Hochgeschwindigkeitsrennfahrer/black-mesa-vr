#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <string>
#include <cstdarg>
#include <Windows.h>
#include "vector.h"

class IClientEntityList;
class IEngineTrace;
class IEngineClient;
class IEngineVGui;
class IMaterialSystem;
class IModelInfo;
class IBaseFileSystemBm;
class IModelRender;
class IInput;
class ISurface;
class C_BaseEntity;
class IMaterial;
struct model_t;

class Offsets;
class VR;
class Hooks;

inline class Game* g_Game = nullptr;

class Game
{
public:
    IClientEntityList* m_ClientEntityList = nullptr;
    IEngineTrace* m_EngineTrace = nullptr;
    IEngineClient* m_EngineClient = nullptr;
    IEngineVGui* m_EngineVGui = nullptr;
    IMaterialSystem* m_MaterialSystem = nullptr;
    IModelInfo* m_ModelInfo = nullptr;
    IModelRender* m_ModelRender = nullptr;
    IInput* m_VguiInput = nullptr;
    ISurface* m_VguiSurface = nullptr;
    void* m_EngineSound = nullptr;
    void* m_Cvar = nullptr;
    IBaseFileSystemBm* m_BaseFileSystem = nullptr;

    uintptr_t m_BaseEngine = 0;
    uintptr_t m_BaseClient = 0;
    uintptr_t m_BaseServer = 0;
    uintptr_t m_BaseMaterialSystem = 0;
    uintptr_t m_BaseVgui2 = 0;

    Offsets* m_Offsets = nullptr;
    VR* m_VR = nullptr;
    Hooks* m_Hooks = nullptr;

    bool m_Initialized = false;
    bool m_SwitchedWeapons = false;
    // Present/DXVK must not call IMaterialSystem. Refresh only from RenderView.
    mutable std::atomic<int> m_CachedMatQueueMode{ 0 };
    mutable int m_MatSetThreadSlot = -1;
    mutable int m_MatGetThreadSlot = -1;
    mutable int m_MatExecuteQueuedSlot = -1;
    mutable int m_MatEndFrameSlot = -1;
    mutable bool m_MatSlotsValid = false;

    float m_AnalogForward = 0.f;
    float m_AnalogSide = 0.f;

    Game();

    void* GetInterface(const char* dllname, const char* interfacename);
    C_BaseEntity* GetClientEntity(int entityIndex);
    C_BaseEntity* GetLocalPlayerEntity();
    // Networked m_hUseEntity (DT_BasePlayer +0x1344). Null when not using /
    // not holding a pickup.
    C_BaseEntity* LocalPlayerUseEntity();
    C_BaseEntity* ResolveEntityFromHandle(uint32_t handle);
    C_BaseEntity* GetActiveWeaponEntity();
    C_BaseEntity* GetViewModelEntity();
    bool GetEntityAttachment(C_BaseEntity* entity, const char* name, Vector& origin, QAngle& angles);
    void EmitPlayerSound(const char* soundName);
    void EmitEntitySound(C_BaseEntity* entity, const char* soundName);
    void PlayUiSound(const char* sample);
    // Shotgun pump in two parts, cut from the game's own pump.wav on first
    // use (bms/sound/bmvr/pump_back.wav, pump_forward.wav). Returns the
    // sample to play, or nullptr if the split is not available.
    const char* PumpHalfSound(bool forward);
    void ScanWristHudNetVars();
    bool ReadWristHudValues(int& health, int& armor, int& clip, int& reserve, int& secondary, int& air);
    // False only when m_bWearingSuit resolved and reads 0 (pre-HEV intro).
    bool LocalPlayerHasSuit();
    // Local player m_hViewEntity is set (a point_viewcontrol camera has the
    // view; death, view punch and shakes never set it). False when it cannot
    // be read (offset not found, no player).
    bool ReadLocalPlayerViewEntitySet(bool& set);
    // True only when m_bZooming resolved and reads 1 (crossbow scope).
    bool LocalPlayerZooming();
    // -1 if the netvar is missing/retired, otherwise 0 or 1.
    int LocalPlayerZoomFlag();
    // MOVETYPE_* of the local player (9 = ladder), -1 when unavailable.
    int LocalPlayerMoveType();
    int LocalPlayerFlags();
    int LocalPlayerWaterLevel();
    bool LocalPlayerAbsOrigin(Vector& out);
    // m_vecVelocity of the local player: its own movement, without what a
    // parent (intro tram) or pusher carries. False = unreadable.
    bool LocalPlayerVelocity(Vector& out);
    // World-space bottom/top mount points of the map's func_useableladders
    // (CFuncLadder; entity list rescanned at most once a second).
    int CollectLadders(Vector* bottoms, Vector* tops, int maxLadders);
    // gpGlobals->interval_per_tick (seconds), 0.015 when unreadable.
    float TickInterval();
    // Entity indexes of the props mounted guns turn (50cal / TOW model; the
    // func_50cal / func_tow itself is a brush with no model of its own).
    // Rescanned at most once a second.
    int CollectMountedGunProps(int* indexes, int maxProps);
    bool GetEntityAbsOrigin(C_BaseEntity* entity, Vector& out);
    // Client model precache index of a model path, -1 if the map does not use it.
    int ModelPrecacheIndex(const char* name);
    // Entity lump (BSP lump 0) of maps/<map>.bsp through the engine filesystem
    // (VPKs included). Returns its size, or a negative code: -1 no filesystem,
    // -2 open failed, -3 not a BSP, -4 bad lump, -5 short read, -6 compressed.
    // Reads up to ~1 MB, so call it once per map load, not per frame.
    int ReadMapEntityLump(const char* map, std::string& out);
    // Entity indexes whose model index is one of modelIndexes (no strings
    // compared; a full entity-list pass, so call it sparingly).
    int CollectEntitiesWithModelIndex(const int* modelIndexes, int modelCount, int* out, int maxOut);
    // Model index of the entity at entityIndex if it exists and is not
    // EF_NODRAW; -1 otherwise. Fills its abs origin (may be stale while the
    // entity is dormant, so check line of sight).
    int VisibleEntityModelIndex(int entityIndex, Vector& origin);
    int RpgLaserOnOffset() const;
    // True when m_bLaserOn was found in DT_Weapon_RPG (not the fallback offset).
    bool RpgLaserOnResolved() const;
    int RpgLaserDotOffset() const;
    C_BaseEntity* FindEntityByNetworkNameContains(const char* token);
    // Networked bool at a scanned player offset. Retires the offset if it ever
    // reads a value that is not 0 or 1. Returns -1 when unavailable.
    int ReadPlayerFlagByte(int& offset);
    int ReadWeaponClip(C_BaseEntity* weapon);
    // The local player's reserve of the weapon's primary ammo (-1 = unknown).
    int ReadWeaponReserve(C_BaseEntity* weapon);
    // Single player: set the active weapon's clip on the server (the game's
    // own count). Only when the server clip equals expectClip. Returns false
    // (nothing written) when the server weapon cannot be reached safely.
    bool SetServerActiveWeaponClip(int expectClip, int newClip);
    // Server CBaseEntity of an entity index (single player: the server is in
    // this process), via VEngineServer021 PEntityOfEntIndex; nullptr if none.
    void* ServerEntityByIndex(int index);
    // Quick draw: right after the active weapon changed, cuts the server weapon's
    // next primary / secondary attack time (the deploy wait) to curtime + capSec.
    // Call every usercmd; returns true on the tick it shortened a timer.
    bool ClampServerWeaponDeploy(float capSec);
    bool WeaponHasNoAmmo(C_BaseEntity* weapon);
    // m_nBody (bodygroups) of a client entity; -1 if unknown.
    int ReadEntityBody(void* entity);
    bool WriteEntityBody(void* entity, int body);
    // RPG m_bNeedReload and base m_bInReload (0/1, -1 = unknown).
    int ReadWeaponReloadFlags(void* weapon, int& needReload, int& inReload);
    const char* GetActiveWeaponModelName();
    const char* GetEntityModelName(C_BaseEntity* entity);
    // Precache/load a client model by path. Null if the file is missing.
    void* FindOrLoadModel(const char* name);
    const char* GetEntityNetworkName(int entityIndex);
    const char* GetEntityClientClassName(C_BaseEntity* entity);
    struct InventoryWeapon
    {
        int entityIndex = 0;
        const char* modelName = nullptr;
        const char* networkName = nullptr;
    };
    int CollectInventoryWeapons(InventoryWeapon* out, int maxCount);
    int CycleWeaponSelect(int direction);
    bool ClientCmd(const char* szCmdString);
    bool ClientCmd_Unrestricted(const char* szCmdString);
    // IVEngineClient slot 1. Samples static + dynamic light at a world point.
    // Does not include the deferred flashlight projection. False on SEH / junk.
    bool TryGetLightForPoint(const Vector& pos, Vector& outRgb, bool clamp = true) const;
    int GetMatQueueMode() const;
    void ResolveMaterialThreadSlots() const;
    bool MaterialThreadSlotsValid() const { return m_MatSlotsValid; }
    int EndFrameVTableSlot() const { return m_MatEndFrameSlot; }
    void ProbeMatQueueModeFromRenderView();
    bool SetMatQueueMode(int mode) const;
    void ExecuteQueuedMaterials() const;
    bool SetConVarInt(const char* name, int value) const;
    bool SetConVarFloat(const char* name, float value) const;
    bool GetConVarInt(const char* name, int& value) const;
    bool GetConVarFloat(const char* name, float& value) const;
    // ICvar::FindVar (slot-probed); null when the cvar does not exist.
    void* FindConVar(const char* name) const;
    bool MaterialVTableMatchesDump() const;

    static void logMsg(const char* fmt, ...);
    static void errorMsg(const char* msg);
    // A signature or byte check that no longer matches the game (usually a Black Mesa update), counted
    // once per name so VR can warn in the headset (ShowToast) instead of only in bmvr_log.txt.
    // debugOnly: the offset only feeds a debug feature (PerfLog stats, profiling, headless logs);
    // players lose nothing, so it shows on the VR panel's Debug tab only, never in the toast.
    static void noteMissingOffset(const char* what, bool debugOnly = false);
    static int missingOffsetCount();        // gameplay offsets (the out-of-date toast)
    static int missingDebugOffsetCount();   // debug-only offsets (Debug tab)
    static bool InstallVertexFormatWarningFilter() { return false; }
    static void UninstallVertexFormatWarningFilter() {}
};

#ifdef _DEBUG
#define LOG(fmt, ...) Game::logMsg("[LOG] " fmt, ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif
