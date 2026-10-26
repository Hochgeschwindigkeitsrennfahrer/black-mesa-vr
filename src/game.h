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
    void ScanWristHudNetVars();
    bool ReadWristHudValues(int& health, int& armor, int& clip, int& reserve, int& secondary, int& air);
    // False only when m_bWearingSuit resolved and reads 0 (pre-HEV intro).
    bool LocalPlayerHasSuit();
    // True only when m_bZooming resolved and reads 1 (crossbow scope).
    bool LocalPlayerZooming();
    // -1 if the netvar is missing/retired, otherwise 0 or 1.
    int LocalPlayerZoomFlag();
    // MOVETYPE_* of the local player (9 = ladder), -1 when unavailable.
    int LocalPlayerMoveType();
    bool LocalPlayerAbsOrigin(Vector& out);
    // World-space bottom/top mount points of the map's func_useableladders
    // (CFuncLadder; entity list rescanned at most once a second).
    int CollectLadders(Vector* bottoms, Vector* tops, int maxLadders);
    int RpgLaserOnOffset() const;
    int RpgLaserDotOffset() const;
    C_BaseEntity* FindEntityByNetworkNameContains(const char* token);
    // Networked bool at a scanned player offset. Retires the offset if it ever
    // reads a value that is not 0 or 1. Returns -1 when unavailable.
    int ReadPlayerFlagByte(int& offset);
    int ReadWeaponClip(C_BaseEntity* weapon);
    bool WeaponHasNoAmmo(C_BaseEntity* weapon);
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
    static bool InstallVertexFormatWarningFilter() { return false; }
    static void UninstallVertexFormatWarningFilter() {}
};

#ifdef _DEBUG
#define LOG(fmt, ...) Game::logMsg("[LOG] " fmt, ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif
