#pragma once
#include <iostream>
#include "MinHook.h"

class Game;
class VR;
class ITexture;
class IMaterial;
struct SourceRect_t
{
    int x;
    int y;
    int width;
    int height;
};
class CViewSetup;
class CUserCmd;
class QAngle;
class Vector;
struct ModelRenderInfo_t;
struct Ray_t;
class CTraceFilter;
class CGameTrace;

template <typename T>
struct Hook
{
    T fOriginal = nullptr;
    LPVOID pTarget = nullptr;
    bool isEnabled = false;

    int createHook(LPVOID targetFunc, LPVOID detourFunc)
    {
        if (!targetFunc)
            return 1;
        if (MH_CreateHook(targetFunc, detourFunc, reinterpret_cast<LPVOID*>(&fOriginal)) != MH_OK)
            return 1;
        pTarget = targetFunc;
        return 0;
    }

    int enableHook()
    {
        if (!pTarget)
            return 1;
        if (isEnabled)
            return 0;
        if (MH_EnableHook(pTarget) != MH_OK)
            return 1;
        isEnabled = true;
        return 0;
    }
};

typedef ITexture*(__thiscall* tGetRenderTarget)(void* thisptr);
// Black Mesa: 3-arg RenderView (no separate hudViewSetup). L4D2VR is 4-arg.
typedef void(__thiscall* tRenderView)(void* thisptr, CViewSetup& setup, int nClearFlags, int whatToDraw);
typedef bool(__thiscall* tCreateMove)(void* thisptr, float flInputSampleTime, CUserCmd* cmd);
typedef void(__thiscall* tLevelInit)(void* thisptr, const char* newmap);
typedef void(__thiscall* tLevelShutdown)(void* thisptr);
typedef void(__thiscall* tCalcViewModelView)(void* thisptr, void* owner, const Vector& eyePosition, const QAngle& eyeAngles);
typedef void(__thiscall* tAdjustEngineViewport)(void* thisptr, int& x, int& y, int& width, int& height);
typedef void(__thiscall* tViewport)(void* thisptr, int x, int y, int width, int height);
typedef void(__thiscall* tGetViewport)(void* thisptr, int& x, int& y, int& width, int& height);
typedef void(__thiscall* tGetRenderTargetDimensions)(void* thisptr, int& width, int& height);
typedef void(__thiscall* tDepthRange)(void* thisptr, float zNear, float zFar);
typedef void(__thiscall* tDrawModelExecute)(void* thisptr, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld);
typedef void*(__cdecl* tLightcacheFindNearest)(int x, int y, int z, int flags);
typedef void(__thiscall* tPushRenderTargetAndViewport)(void* thisptr, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH);
typedef void(__thiscall* tPopRenderTargetAndViewport)(void* thisptr);
typedef void(__thiscall* tDrawScreenSpaceRectangle)(void* thisptr, IMaterial* material,
    int destX, int destY, int width, int height,
    float srcX0, float srcY0, float srcX1, float srcY1,
    int srcWidth, int srcHeight, void* clientRenderable, int xDice, int yDice);
typedef void(__thiscall* tCopyRenderTargetToTextureEx)(void* thisptr, ITexture* texture,
    int renderTargetId, SourceRect_t* srcRect, SourceRect_t* dstRect);
typedef void(__thiscall* tVgui_Paint)(void* thisptr, int mode);
typedef void(__thiscall* tGetBackBufferDimensions)(void* thisptr, int& width, int& height);
typedef void(__thiscall* tGetScreenSize)(void* thisptr, int& width, int& height);
typedef float(__thiscall* tGetScreenAspectRatio)(void* thisptr);
typedef ITexture*(__thiscall* tCreateNamedRTEx)(void* thisptr, const char* name, int w, int h, int sizeMode, int format, int depth, unsigned textureFlags, unsigned renderTargetFlags);
typedef void(__thiscall* tEndRTAlloc)(void* thisptr);
typedef void(__thiscall* tDrawFilledRect)(void* thisptr, int x0, int y0, int x1, int y1);
typedef void(__thiscall* tDrawTexturedRect)(void* thisptr, int x0, int y0, int x1, int y1);
typedef float(__thiscall* tGetViewModelFOV)(void* thisptr);
typedef void(__thiscall* tEndFrame)(void* thisptr);
typedef void(__thiscall* tTraceRay)(void* thisptr, const Ray_t& ray, unsigned int fMask, CTraceFilter* filter, CGameTrace* pTrace);
typedef void(__thiscall* tImpulseCommands)(void* thisptr);
typedef void(__thiscall* tUpdateFlashlightState)(void* thisptr, void* flashlightState);
typedef Vector*(__thiscall* tWeaponShootPosition)(void* thisptr, Vector* out);
typedef void(__thiscall* tGrabSetTarget)(void* thisptr, Vector* pos, QAngle* ang);
typedef void(__thiscall* tGetShootAngles)(void* thisptr, QAngle* out);
typedef int(__thiscall* tGetAttachmentVec)(void* thisptr, int number, Vector* origin, QAngle* angles);
typedef int(__thiscall* tGetAttachmentMatrix)(void* thisptr, int number, float* matrix);
typedef void(__thiscall* tTauBeamView)(void* thisptr, Vector* startEnd, void* a2, float a3, int a4, int a5);
typedef void(__thiscall* tTauBeamWorld)(void* thisptr, Vector* impact, Vector* normal, float width);
typedef void(__thiscall* tTauBeamFireTrace)(void* thisptr);
typedef void(__thiscall* tGluonImpactTrace)(void* thisptr, void* player, CGameTrace* trace);
typedef void(__thiscall* tGluonBeamUpdate)(void* thisptr);
typedef void(__thiscall* tGluonBeamFxSet)(void* thisptr, void* viewmodel, Vector* start, Vector* mid, Vector* end);
typedef int(__thiscall* tGluonBeamFxDraw)(void* thisptr, unsigned flags);
typedef void(__thiscall* tParticleSetControlPoint)(void* thisptr, int index, Vector* origin);
typedef void(__thiscall* tParticleMgrAddEffect)(void* thisptr, void* effect);
typedef void(__thiscall* tRpgUpdateLaser)(void* thisptr);
typedef void(__thiscall* tHudCrosshairPaint)(void* thisptr);
typedef int(__thiscall* tSpriteRendererDraw)(void* thisptr, void* entity, void* model,
    float* origin, float* angles, float scale, void* attach, int a7, int a8, int a9,
    unsigned a10, unsigned a11, unsigned a12, unsigned a13, float frame, int a15);
typedef void(__thiscall* tViewRenderBeamsDraw)(void* thisptr, void* beam);
// client.dll FUN_102dcbf0 — second DrawSprite path (stdcall, ret 0x20).
// IClientRenderable* on the stack; origin comes from vtable+4.
typedef int(__stdcall* tSpriteRenderableDraw)(void* renderable, float scale, float frame,
    int rendermode, int renderfx, unsigned char* color, float hdr, unsigned* unk);
typedef int(__thiscall* tEnvLaserDotDraw)(void* thisptr, unsigned flags);
typedef void(__cdecl* tSpriteQuad)(float* origin, float width, float height, unsigned color);

class Hooks
{
public:
    static inline Game* m_Game;
    static inline VR* m_VR;

    static inline Hook<tGetRenderTarget> hkGetRenderTarget;
    static inline Hook<tRenderView> hkRenderView;
    static inline Hook<tCreateMove> hkCreateMove;
    static inline Hook<tLevelInit> hkLevelInit;
    static inline Hook<tLevelShutdown> hkLevelShutdown;
    static inline Hook<tCalcViewModelView> hkCalcViewModelView;
    static inline Hook<tAdjustEngineViewport> hkAdjustEngineViewport;
    static inline Hook<tViewport> hkViewport;
    static inline Hook<tGetViewport> hkGetViewport;
    static inline Hook<tGetViewport> hkGetViewportQueued;
    static inline Hook<tGetRenderTargetDimensions> hkGetRenderTargetDimensions;
    static inline Hook<tGetRenderTargetDimensions> hkGetRenderTargetDimensionsBase;
    static inline Hook<tGetRenderTargetDimensions> hkGetRenderTargetDimensionsQueued;
    static inline Hook<tDepthRange> hkDepthRange;
    static inline Hook<tDepthRange> hkDepthRangeQueued;
    static inline Hook<tDrawModelExecute> hkDrawModelExecute;
    static inline Hook<tLightcacheFindNearest> hkLightcacheFindNearest;
    static inline Hook<tPushRenderTargetAndViewport> hkPushRenderTargetAndViewport;
    static inline Hook<tPopRenderTargetAndViewport> hkPopRenderTargetAndViewport;
    static inline Hook<tDrawScreenSpaceRectangle> hkDrawScreenSpaceRectangle;
    static inline Hook<tCopyRenderTargetToTextureEx> hkCopyRenderTargetToTextureEx;
    static inline Hook<tVgui_Paint> hkVgui_Paint;
    static inline Hook<tGetBackBufferDimensions> hkGetBackBufferDimensions;
    static inline Hook<tGetScreenSize> hkGetScreenSize;
    static inline Hook<tGetScreenAspectRatio> hkGetScreenAspectRatio;
    static inline Hook<tCreateNamedRTEx> hkCreateNamedRTEx;
    static inline Hook<tEndRTAlloc> hkEndRTAlloc;
    static inline Hook<tDrawFilledRect> hkDrawFilledRect;
    static inline Hook<tDrawTexturedRect> hkDrawTexturedRect;
    static inline Hook<tGetViewModelFOV> hkGetViewModelFOV;
    static inline Hook<tEndFrame> hkEndFrame;
    static inline Hook<tTraceRay> hkTraceRay;
    static inline Hook<tImpulseCommands> hkImpulseCommands;
    static inline Hook<tUpdateFlashlightState> hkUpdateFlashlightState;
    static inline Hook<tWeaponShootPosition> hkClientWeaponShootPosition;
    static inline Hook<tWeaponShootPosition> hkServerWeaponShootPosition;
    static inline Hook<tWeaponShootPosition> hkServerGrabHoldOrigin;
    static inline Hook<tGrabSetTarget> hkGrabSetTarget;
    static inline Hook<tGetShootAngles> hkGetShootAngles;
    static inline Hook<tGetShootAngles> hkClientGetShootAngles;
    static inline Hook<tGetAttachmentVec> hkGetAttachmentVec;
    static inline Hook<tGetAttachmentMatrix> hkGetAttachmentMatrix;
    static inline Hook<tTauBeamView> hkTauBeamView;
    static inline Hook<tTauBeamWorld> hkTauBeamWorld;
    static inline Hook<tTauBeamFireTrace> hkTauBeamFireTrace;
    static inline Hook<tGluonImpactTrace> hkGluonImpactTrace;
    static inline Hook<tGluonBeamUpdate> hkGluonBeamUpdate;
    static inline Hook<tGluonBeamFxSet> hkGluonBeamFxSet;
    static inline Hook<tGluonBeamFxDraw> hkGluonBeamFxDraw;
    static inline Hook<tParticleSetControlPoint> hkParticleSetControlPoint;
    static inline Hook<tParticleMgrAddEffect> hkParticleMgrAddEffect;
    static inline Hook<tRpgUpdateLaser> hkRpgUpdateLaser;
    static inline Hook<tHudCrosshairPaint> hkHudCrosshairPaint;
    static inline Hook<tSpriteRendererDraw> hkSpriteRendererDraw;
    static inline Hook<tViewRenderBeamsDraw> hkViewRenderBeamsDraw;
    static inline Hook<tSpriteRenderableDraw> hkSpriteRenderableDraw;
    static inline Hook<tEnvLaserDotDraw> hkEnvLaserDotDraw;
    static inline Hook<tSpriteQuad> hkSpriteQuad;

    Hooks() = default;
    explicit Hooks(Game* game);
    ~Hooks();

    int initSourceHooks();

    static ITexture* __fastcall dGetRenderTarget(void* ecx, void* edx);
    static void __fastcall dRenderView(void* ecx, void* edx, CViewSetup& setup, int nClearFlags, int whatToDraw);
    static bool __fastcall dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd);
    static void __fastcall dLevelInit(void* ecx, void* edx, const char* newmap);
    static void __fastcall dLevelShutdown(void* ecx, void* edx);
    static void __fastcall dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles);
    static void __fastcall dAdjustEngineViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height);
    static void __fastcall dViewport(void* ecx, void* edx, int x, int y, int width, int height);
    static void __fastcall dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height);
    static void __fastcall dGetRenderTargetDimensions(void* ecx, void* edx, int& width, int& height);
    static void __fastcall dDepthRange(void* ecx, void* edx, float zNear, float zFar);
    static void __fastcall dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld);
    static void* __cdecl dLightcacheFindNearest(int x, int y, int z, int flags);
    static void __fastcall dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH);
    static void __fastcall dPopRenderTargetAndViewport(void* ecx, void* edx);
    static void __fastcall dDrawScreenSpaceRectangle(void* ecx, void* edx, IMaterial* material,
        int destX, int destY, int width, int height,
        float srcX0, float srcY0, float srcX1, float srcY1,
        int srcWidth, int srcHeight, void* clientRenderable, int xDice, int yDice);
    static void __fastcall dCopyRenderTargetToTextureEx(void* ecx, void* edx, ITexture* texture,
        int renderTargetId, SourceRect_t* srcRect, SourceRect_t* dstRect);
    static void __fastcall dVGui_Paint(void* ecx, void* edx, int mode);
    static void __fastcall dGetBackBufferDimensions(void* ecx, void* edx, int& width, int& height);
    static void __fastcall dGetScreenSize(void* ecx, void* edx, int& width, int& height);
    static float __fastcall dGetScreenAspectRatio(void* ecx, void* edx);
    static ITexture* __fastcall dCreateNamedRTEx(void* ecx, void* edx, const char* name, int w, int h, int sizeMode, int format, int depth, unsigned textureFlags, unsigned renderTargetFlags);
    static void __fastcall dEndRTAlloc(void* ecx, void* edx);
    static void __fastcall dDrawFilledRect(void* ecx, void* edx, int x0, int y0, int x1, int y1);
    static void __fastcall dDrawTexturedRect(void* ecx, void* edx, int x0, int y0, int x1, int y1);
    static float __fastcall dGetViewModelFOV(void* ecx, void* edx);
    static void __fastcall dEndFrame(void* ecx, void* edx);
    static void __fastcall dTraceRay(void* ecx, void* edx, const Ray_t& ray, unsigned int fMask, CTraceFilter* filter, CGameTrace* pTrace);
    static void __fastcall dImpulseCommands(void* ecx, void* edx);
    static void __fastcall dUpdateFlashlightState(void* ecx, void* edx, void* flashlightState);
    static Vector* __fastcall dClientWeaponShootPosition(void* ecx, void* edx, Vector* out);
    static Vector* __fastcall dServerWeaponShootPosition(void* ecx, void* edx, Vector* out);
    static Vector* __fastcall dServerGrabHoldOrigin(void* ecx, void* edx, Vector* out);
    static void __fastcall dGrabSetTarget(void* ecx, void* edx, Vector* pos, QAngle* ang);
    static void __fastcall dGetShootAngles(void* ecx, void* edx, QAngle* out);
    static void __fastcall dClientGetShootAngles(void* ecx, void* edx, QAngle* out);
    static int __fastcall dGetAttachmentVec(void* ecx, void* edx, int number, Vector* origin, QAngle* angles);
    static int __fastcall dGetAttachmentMatrix(void* ecx, void* edx, int number, float* matrix);
    static void __fastcall dTauBeamView(void* ecx, void* edx, Vector* startEnd, void* a2, float a3, int a4, int a5);
    static void __fastcall dTauBeamWorld(void* ecx, void* edx, Vector* impact, Vector* normal, float width);
    static void __fastcall dTauBeamFireTrace(void* ecx, void* edx);
    static void __fastcall dGluonImpactTrace(void* ecx, void* edx, void* player, CGameTrace* trace);
    static void __fastcall dGluonBeamUpdate(void* ecx, void* edx);
    static void __fastcall dGluonBeamFxSet(void* ecx, void* edx, void* viewmodel, Vector* start, Vector* mid, Vector* end);
    static int __fastcall dGluonBeamFxDraw(void* ecx, void* edx, unsigned flags);
    static void __fastcall dParticleSetControlPoint(void* ecx, void* edx, int index, Vector* origin);
    static void __fastcall dParticleMgrAddEffect(void* ecx, void* edx, void* effect);
    static void __fastcall dRpgUpdateLaser(void* ecx, void* edx);
    static void __fastcall dHudCrosshairPaint(void* ecx, void* edx);
    static int __fastcall dSpriteRendererDraw(void* ecx, void* edx, void* entity, void* model,
        float* origin, float* angles, float scale, void* attach, int a7, int a8, int a9,
        unsigned a10, unsigned a11, unsigned a12, unsigned a13, float frame, int a15);
    static void __fastcall dViewRenderBeamsDraw(void* ecx, void* edx, void* beam);
    static int __stdcall dSpriteRenderableDraw(void* renderable, float scale, float frame,
        int rendermode, int renderfx, unsigned char* color, float hdr, unsigned* unk);
    static int __fastcall dEnvLaserDotDraw(void* ecx, void* edx, unsigned flags);
    static void __cdecl dSpriteQuad(float* origin, float width, float height, unsigned color);
    static void EnsureServerFlashlightHook();
    static void EnsureClientFlashlightHook();
    static void EnsureWeaponShootOriginHooks();
    // Returns true once every VFX hook target is resolved (installed or skipped).
    static bool EnsureWeaponVfxHooks();
    static void RestoreViewmodelArmHides();
};
