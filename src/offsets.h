#pragma once
#include "sigscanner.h"
#include "game.h"

// Signatures verified offline against the installed Steam Black Mesa modules
// (tools/scan_offsets.py). All MATCH on this machine except OverrideView
// (pattern too short; not hooked). Prototype build stamp 19042901 is still
// valid for steam.inf PatchVersion=100002.

struct Offset
{
    std::string moduleName;
    int offset;
    int address;
    std::string signature;
    int sigOffset;
    bool optional;
    bool valid;

    Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0, bool optional = false)
        : moduleName(std::move(moduleName))
        , offset(currentOffset)
        , address(0)
        , signature(std::move(signature))
        , sigOffset(sigOffset)
        , optional(optional)
        , valid(false)
    {
        if (!GetModuleHandleA(this->moduleName.c_str()))
            return;

        int newOffset = SigScanner::VerifyOffset(this->moduleName, currentOffset, this->signature, this->sigOffset);
        if (newOffset > 0)
            this->offset = newOffset;
        if (newOffset == -1)
        {
            if (!optional)
                Game::logMsg("Signature not found in %s: %s", this->moduleName.c_str(), this->signature.c_str());
            return;
        }
        this->address = (int)((uintptr_t)GetModuleHandleA(this->moduleName.c_str()) + this->offset);
        this->valid = this->address != 0;
    }
};

class Offsets
{
public:
    // CBlackMesaViewRender IViewRender vtable slot 6. 3-arg thiscall
    // (CViewSetup&, nClearFlags, whatToDraw), ret 0xC.
    // 0x207730 matches a nearby helper whose callers push floats — not RenderView.
    Offset RenderView{ "client.dll", 0x20EE40,
        "55 8B EC 81 EC DC 02 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 8B 5D 08 56" };

    Offset g_pClientMode{ "client.dll", 0x16AD56,
        "56 57 8B F9 8B 0D ? ? ? ? 8B 01 FF 50 24", 6 };

    Offset CreateMove{ "client.dll", 0x110310,
        "55 8B EC E8 ? ? ? ? 8B C8 85 C9 75 06 B0 01 5D C2 08 00" };

    Offset CalcViewModelView{ "client.dll", 0x29D930,
        "55 8B EC 83 EC 24 53 56 8B 75 08 57 8B F9 85 F6" };

    // Ghidra client.dll image 0x10000000. FUN_1029d930 (CalcViewModelView)
    // ends in these. FUN_100af720 writes abs origin at this+0x294 and calls
    // FUN_10077220(this,1) to invalidate; FUN_100af600 writes abs angles at
    // this+0x2D0 via FUN_10077220(this,2). Raw field writes skip invalidate,
    // so bones/render origin stay on the camera pivot.
    static constexpr int kCBaseEntity_SetAbsOrigin = 0xAF720;
    static constexpr int kCBaseEntity_SetAbsAngles = 0xAF600;
    // C_BaseEntity abs origin written by SetAbsOrigin (Ghidra client FUN_100af720).
    static constexpr int kCBaseEntity_AbsOrigin = 0x294;

    // CBaseEntity::EmitSound(name, soundtime=0, duration=NULL). Ghidra
    // FUN_101d80d0 (thiscall). Vanilla HUD uses this for Player.WeaponSelected
    // (common/wpn_select.wav at SNDLVL_75dB — a world emit at the player).
    // The weapon wheel plays weapon_*.Draw on the weapon instead, and HUD
    // ticks go through IEngineSound::EmitAmbientSound (2D).
    static constexpr int kCBaseEntity_EmitSound = 0x1D80D0;

    // C_BlackMesaPlayer vtable +0x414. Thunk: call EyePosition (+0x268) and
    // return that vector. Distinct from EyePosition (do not hook +0x268 —
    // that moves the camera). Bytes: 55 8B EC 8B 11 FF 75 08 FF 92 68 02 00 00.
    static constexpr int kCBasePlayer_Weapon_ShootPosition = 0x7C030;

    // server.dll CBlackMesaPlayer vtable +0x23C. Same thunk onto EyePosition
    // at +0x230. Bytes: 55 8B EC 8B 11 FF 75 08 FF 92 30 02 00 00.
    static constexpr int kCBasePlayer_Weapon_ShootPosition_Server = 0x11E490;

    // server.dll CBasePlayer vtable +0x474 = FUN_10077240. CGrabController::
    // UpdateObject (FUN_10470da0) uses this as the hold origin, not +0x23C.
    // Bytes: 55 8B EC 83 EC 24 57 8B F9 8D 4D E8 6A 00 51 6A 00 8B 07 8B CF
    // FF 90 4C 02 00 00. Hold distance is 2*radius + 24 (DAT_105db264).
    static constexpr int kCBasePlayer_GrabHoldOrigin_Server = 0x77240;

    // server.dll CGrabController::SetTargetPosition FUN_10470990.
    // UpdateObject writes the hold point here after 2*radius+24. Bytes:
    // 55 8B EC 8B 45 08 8B D1 D9 00 D9 5A 04. ret 8.
    static constexpr int kCGrabController_SetTargetPosition = 0x470990;

    // client.dll DT_BasePlayer RecvTable FUN_100b6a00: m_hUseEntity.
    static constexpr int kCBasePlayer_hUseEntity = 0x1344;

    // server.dll CBlackMesaPlayer::GetShootAngles, vtable +0x240. Verified by
    // disassembling all 0x8C bytes at this RVA in the shipped server.dll: it
    // writes EyeAngles() (vtable +0x234) into the QAngle* stack arg, then adds
    // m_Local.m_vecPunchAngle (+0x934/938/93C) and m_recoilPunchAngles
    // (+0x14BC/14C0/14C4), and returns with `ret 4`. Every weapon's attack
    // builds its bullet direction from this call, and the weapon's ApplyRecoil
    // runs after the shot, so the recoil term is exactly what walks sustained
    // fire upward. These are server offsets; the client copies the mod already
    // zeroes live at different addresses and only drive the viewmodel.
    static constexpr int kBlackMesaPlayer_GetShootAngles_Server = 0x47EC90;
    static constexpr int kBlackMesaPlayer_RecoilPunchAngles_Server = 0x14BC;

    // C_BaseAnimating IClientRenderable GetAttachment. `this` is entity+4.
    // Vec: FUN_10097a80 (index, Vector*, QAngle*). Matrix: FUN_100979b0.
    static constexpr int kCBaseAnimating_GetAttachmentVec = 0x97A80;
    static constexpr int kCBaseAnimating_GetAttachmentMatrix = 0x979B0;

    // IClientRenderable (entity+4) vtable: LookupAttachment +0x90, GetAttachment
    // origin/angles +0x98. Ghidra TAU Fire02 path (FUN_10234330).
    // Source 2013 LookupAttachment is slot 35; BM is slot 36 (0x90/4). The extra
    // method sits after GetRenderBoundsWorldspace (slot 21, +0x54).
    static constexpr int kIClientRenderable_LookupAttachment = 0x90;
    static constexpr int kIClientRenderable_GetAttachmentVec = 0x98;
    static constexpr int kIClientRenderable_GetRenderBoundsWorldspace = 21;

    // client.dll CTauBeam first-person Fire02 particle path (FUN_10234330).
    // thiscall, 5 stack args, ret 0x14. Arg1 is six floats: start xyz + end xyz.
    // Unique bytes: 55 8B EC 83 EC 60 80 7D 14 00 8B 55 08 56 57 8B
    static constexpr int kCTauBeam_ViewMuzzle = 0x234330;

    // client.dll CTauBeam impact glow (FUN_102346A0). ProgressBeam calls it as
    // (trace.endpos, trace.plane.normal, charge), not (beam start, beam end).
    // Unique bytes: 55 8B EC 83 EC 78 8B 45 08 0F 57 C9 56 57 8B 7D
    static constexpr int kCTauBeam_WorldBeam = 0x2346A0;

    // client.dll CTauBeam fire: AngleVectors(this+0x18) then TraceRay from
    // this+0xC (FUN_102348B0). thiscall, 0 stack args. Unique bytes:
    // 53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC EC 00 00 00
    static constexpr int kCTauBeam_FireTrace = 0x2348B0;

    // client.dll C_Weapon_Gluon impact trace (FUN_1029D430). ShootPosition +
    // EyeAngles (+0x274), not GetShootAngles, so the glow sits on the HMD look.
    // thiscall, 2 stack args (player, CGameTrace*), ret 8. Unique bytes:
    // 53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC D8 00 00 00
    static constexpr int kCWeaponGluon_ImpactTrace = 0x29D430;
    // client.dll C_Weapon_Gluon first-person beam update (FUN_1029A7D0).
    // thiscall, 0 stack args. Calls impact trace then SetControlPoint(0, end)
    // and C_GluonBeamFx::SetBeam (FUN_1029C960). Unique bytes:
    // 55 8B EC 81 EC D0 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F9
    static constexpr int kCWeaponGluon_BeamUpdate = 0x29A7D0;
    // client.dll C_GluonBeamFx::SetBeam (FUN_1029C960). thiscall, 4 stack args
    // (viewmodel, start, mid, end), ret 0x10. Unique bytes:
    // 55 8B EC 8B 45 0C 56 8B F1 C6 46 0C 01
    static constexpr int kCGluonBeamFx_SetBeam = 0x29C960;
    // client.dll C_GluonBeamFx Draw (FUN_1029B290) on IClientRenderable.
    // Start at this+0xC, end at this+0x24. thiscall, 1 stack arg, ret 4.
    // Unique bytes: 53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 68 05 00 00
    static constexpr int kCGluonBeamFx_Draw = 0x29B290;
    // client.dll CNewParticleEffect::SetControlPoint (FUN_101A5170).
    // thiscall, 2 stack args (index, Vector*), ret 8.
    // Unique bytes: 55 8B EC 53 8B 5D 0C 57 8B F9 83 BF 54 1B 00 00 FF
    static constexpr int kParticleSetControlPoint = 0x1A5170;
    // client.dll CParticleMgr::AddEffect (FUN_1019D940). thiscall, 1 stack arg
    // (CNewParticleEffect*), ret 4. If def+0x200 (view model effect) is set,
    // it puts the effect in render group 0xB (viewmodel FOV). Unique bytes:
    // 55 8B EC 8B 41 4C 53 56 8B 75 08
    static constexpr int kParticleMgr_AddEffect = 0x19D940;
    // client.dll CParticleSystemMgr singleton used as this for FUN_10313390.
    static constexpr int kParticleSystemMgr = 0x5631CC;
    static constexpr int kParticleSystemMgr_Find = 0x313390;
    static constexpr int kCParticleDef_ViewModelEffect = 0x200;
    static constexpr int kCParticleDef_Name = 0x228;
    static constexpr int kCNewParticleEffect_Def = 0x58;
    static constexpr int kCNewParticleEffect_ViewModel = 0x1C80;
    // client.dll g_pClientLeafSystem (IClientLeafSystem*). vtable+0x38 is
    // SetRenderGroup(handle, group); AddEffect uses group 0xB for viewmodel.
    static constexpr int kClientLeafSystemPtr = 0x553D10;

    // client.dll C_Weapon_RPG laser update (FUN_10291F80). CHudCrosshair Paint
    // calls weapon vtable +0x5C4. Draws ISurface circles at ScreenWidth/2,
    // ScreenHeight/2 (HMD centre in VR). Extra red geometry if m_bLaserOn.
    // thiscall, 0 stack args.
    static constexpr int kCWeaponRpg_UpdateLaser = 0x291F80;
    // RecvTable DT_Weapon_RPG (FUN_10291d30): m_hHomingGrenade +0xAE0,
    // m_bNeedReload +0xAE4, m_bLaserOn +0xAE5. There is no m_hLaserDot netvar;
    // the visible mark is a separate CEnvLaserDot sprite (CSprite::DrawModel
    // FUN_101dbf70) whose origin is the look-trace, so it also sits at centre.
    static constexpr int kCWeaponRpg_hHomingGrenade = 0xAE0;
    static constexpr int kCWeaponRpg_hLaserDot = 0xAE0; // stale name; do not resolve as the dot
    static constexpr int kCWeaponRpg_bLaserOn = 0xAE5;
    // client.dll CHudCrosshair::Paint (FUN_10278460). Calls weapon vtable +0x5C4.
    // thiscall, 0 stack args. Unique bytes: 55 8B EC 51 53 56 8B 35 ? ? ? ? 8B D9
    static constexpr int kCHudCrosshair_Paint = 0x278460;
    // DT_BaseEntity m_fEffects (FUN_100a4cb0). EF_NODRAW = 0x20.
    static constexpr int kCBaseEntity_fEffects = 0x80;
    static constexpr int kEF_NODRAW = 0x20;
    // client.dll C_SpriteRenderer::DrawSprite (FUN_100f6900). thiscall,
    // 15 stack args, ret 0x3C. ecx is the renderer at CSprite+0x69C.
    // Copies origin from arg3; CEnvLaserDot's origin is the look-trace.
    static constexpr int kCSpriteRenderer_DrawSprite = 0xF6900;
    // client.dll FUN_102dcbf0. stdcall 8 args, ret 0x20. Alternate sprite
    // draw: origin from IClientRenderable vtable+4, then FUN_100f6b40.
    // Unique bytes: 55 8B EC 83 EC 64 53 8B 5D 08 8B CB 56 8B 03 FF 50 04
    static constexpr int kCSpriteRenderable_Draw = 0x2DCBF0;
    // client.dll C_EnvLaserDot DrawModel override (FUN_10221fd0). thiscall,
    // 1 stack arg (studio flags), ret 4. Traces from attachment "laser" or
    // owner EyePosition/EyeAngles (+0x268/+0x26c), then FUN_100821a0.
    // Unique bytes: 55 8B EC 83 EC A0 A1 ? ? ? ? 33 C5 89 45 FC F7 45 08 00 00 00 4E
    static constexpr int kCEnvLaserDot_Draw = 0x221FD0;
    // client.dll camera-facing sprite quad (FUN_100821a0). cdecl, origin* +
    // width/height/color. Unique bytes: 53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04
    static constexpr int kSpriteQuad = 0x821A0;
    // client.dll CViewRenderBeams::DrawBeam (FUN_101f5be0). thiscall, Beam_t*
    // on the stack, ret 4. Unique bytes: 55 8B EC 83 EC 2C A1 ? ? ? ? 33 C5 89 45 FC 89 4D E8
    static constexpr int kCViewRenderBeams_DrawBeam = 0x1F5BE0;
    static constexpr int kBeam_t_type = 0x30;
    static constexpr int kBeam_t_start = 0x3C;
    static constexpr int kBeam_t_delta = 0xB4;
    static constexpr int kBeam_t_r = 0xE0;

    // IClientMode slot 32. Reads viewmodel_fov_override then weapon GetViewModelFOV
    // (~54). That is why console fov does not change gun/near scale in VR.
    Offset GetViewModelFOV{ "client.dll", 0x216510,
        "55 8B EC 51 8B 0D ? ? ? ? 81 F9 ? ? ? ? 75 16 F3 0F 10 0D" };

    Offset AdjustEngineViewport{ "client.dll", 0x1102C0,
        "C2 10 00 CC CC CC CC CC CC CC CC CC CC CC CC CC B0 01 C2 08 00" };

    Offset LevelInit{ "client.dll", 0x110A80,
        "55 8B EC 83 EC 20 56 8B F1 6A 01 68 ? ? ? ?" };

    Offset LevelShutdown{ "client.dll", 0x110B30,
        "55 8B EC 83 EC 20 56 8B F1 B9 ? ? ? ? E8" };

    // CModelRender::DrawModelExecute — IModelRender vtable slot 19 (+0x4C).
    // thiscall (state, ModelRenderInfo_t&, bones), ret 0xC. Ghidra FUN_10113e80.
    // Do NOT use 0xF6A20: that prologue matches DispInfo_LoadDisplacements'
    // cdecl 5-arg helper (FUN_100f6a20). Hooking it as DME hung load-to-menu
    // (2026-08-18). DrawModelEx (+0x40) calls Setup (+0x48) then this slot.
    Offset DrawModelExecute{ "engine.dll", 0x113E80,
        "55 8B EC 81 EC 68 03 00 00 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 53 8B 5D 0C" };

    // engine.dll FindNearest lightcache (LRU walk, sentinel 0x200).
    // mat_queue_mode 2 + GetLightForPoint/studio lighting can cycle +0x1D0
    // next pointers; the engine walk never hits 0x200 and freezes the game
    // thread (verified 2026-09-08, PID 25228, 12-node cycle).
    Offset LightcacheFindNearest{ "engine.dll", 0x1D0E20,
        "55 8B EC 83 EC 1C 0F B7 0D ? ? ? ? 33 D2 53 56 BB 00 02 00 00" };

    Offset VGui_Paint{ "engine.dll", 0x238C50,
        "55 8B EC 83 EC 18 53 8B D9 8B 0D ? ? ? ? FF 15" };

    Offset GetRenderTarget{ "materialsystem.dll", 0x68820,
        "83 79 4C 00 7E 0E 8B 41 4C 8D 14 C0" };
    // CMatRenderContext::GetRenderTargetDimensions (vtable +0x20 after GetRT).
    // CLensflare WorldToScreen uses this as pixel size; GetViewport (+0x9C) is
    // the mesh NDC divisor. HWND 16:9 vs square eye parks flares off the lights.
    Offset GetRenderTargetDimensions{ "materialsystem.dll", 0x68840,
        "55 8B EC 8B 41 4C 56 8D 14 C0 8B 41 40 8B 74 90 DC 85 F6 74 1D" };
    // CMatRenderContextBase copy (internal context). Optional.
    Offset GetRenderTargetDimensionsBase{ "materialsystem.dll", 0x64180,
        "55 8B EC 56 8B F1 57 83 7E 4C 00 74 2F 8B 46 4C 8D 14 C0 8B 46 40 8B 7C 90 DC",
        0, true };
    // CMatQueuedRenderContext +0x20 is `ret 8` — it never writes the outs.
    // mat_queue_mode 2 records flares against leftover HWND / uninit size.
    Offset GetRenderTargetDimensionsQueued{ "materialsystem.dll", 0x5F890,
        "C2 08 00 CC CC CC CC CC CC CC CC CC CC CC CC CC CC 55 8B EC 8B 45 08 C7 00 00 00 00 00",
        0, true };
    Offset GetViewport{ "materialsystem.dll", 0x68A70,
        "55 8B EC 8B 41 4C 56 8D 14 C0 8B 41 40 83 7C 90 F8 00" };
    // Queued GetViewport (+0x9C). Hardware 0x68A70 is a different function;
    // ClampStereoViewport never ran on the record thread without this.
    Offset GetViewportQueued{ "materialsystem.dll", 0x5F8C0,
        "55 8B EC 56 57 8B F9 8B 47 4C 8D 14 C0 8B 47 40 83 7C 90 F8 00 8D 34 90 7C 2C",
        0, true };
    Offset Viewport{ "materialsystem.dll", 0x69F30,
        "55 8B EC 56 FF 75 14 8B F1 FF 75 10 FF 75 0C FF 75 08 E8" };
    Offset PushRenderTargetAndViewport{ "materialsystem.dll", 0x6A3D0,
        "55 8B EC 83 EC 24 8B 45 08 89 45 DC 8B 45 0C 89 45 EC" };
    Offset PopRenderTargetAndViewport{ "materialsystem.dll", 0x6A250,
        "56 8B F1 83 7E 4C 00 74 15 8B 06 6A 00 FF 50 10 FF 4E 4C" };
    // Hardware CMatRenderContext vtable: DrawScreenSpaceRectangle is two
    // slots before the 6-arg PushRT (0x6A3D0). Wrapper ret 0x38. Fire/glass/
    // AMS overlays pass dest 2560x1440 onto the 3168 G-buffer (bm_c1a1d).
    Offset DrawScreenSpaceRectangle{ "materialsystem.dll", 0x67DC0,
        "55 8B EC 8B 4D 08 8B 01 FF 90 7C 01 00 00 D9 EE" };
    // CopyRenderTargetToTextureEx — UpdateRefractTexture / POT FB blit.
    Offset CopyRenderTargetToTextureEx{ "materialsystem.dll", 0x67440,
        "55 8B EC 53 8B 5D 14 56 8B 75 08 57 8B 7D 10 85 F6" };

    // Ghidra on Steam client.dll CBlackMesaViewRender_RenderView (0x20EE40):
    // IMaterialSystem +0x19C = GetRenderContext (AddRef'd). Context then:
    // +0x8 BeginRender, +0xC EndRender, +0x4 Release, +0x1C GetRenderTarget,
    // +0x18 SetRenderTarget(ITexture*) — RenderView restores the prologue
    // GetRT with this slot at 1020f5e4. Do not use materialsystem+0x68480
    // (adjacent 6-arg function) as SetRT. 6-arg PushRT is +0x23C; Pop is
    // +0x24C (HUD PushRT/Pop pair). Call Push/Pop through the hooked RVAs.
    static constexpr int kIMaterialSystem_GetRenderContext = 0x19C;
    static constexpr int kIMatRenderContext_Release = 0x4;
    static constexpr int kIMatRenderContext_BeginRender = 0x8;
    static constexpr int kIMatRenderContext_EndRender = 0xC;
    static constexpr int kIMatRenderContext_SetRenderTarget = 0x18;
    static constexpr int kIMatRenderContext_GetRenderTarget = 0x1C;
    static constexpr int kIMatRenderContext_GetRenderTargetDimensions = 0x20;
    // IMatRenderContext::DepthRange(zNear, zFar). client.dll DrawViewModels
    // (FUN_1020a8f0) calls this slot with (0, 0.1) so the gun wins every world
    // Z test. Same slot as L4D2VR sdk.h. Hardware and queued vtables both
    // implement it; hook from TryHookMatContextQueries like GetViewport.
    static constexpr int kIMatRenderContext_DepthRange = 0x2C;
    static constexpr int kIMatRenderContext_GetViewport = 0x9C;
    static constexpr int kIMatRenderContext_PushRT6 = 0x23C;
    static constexpr int kIMatRenderContext_PopRT = 0x24C;
    // IMatRenderContext::GetCallQueue. HL2VR/Source 2013 absolute slot 146.
    // BM 6-arg PushRT is slot 143 vs Source 104 (~+39). Probe 146 and 185
    // under SEH; do not call through the fake IMatRenderContext C++ vtable.
    static constexpr int kIMatRenderContext_GetCallQueueSlotSource = 146;
    static constexpr int kIMatRenderContext_GetCallQueueSlotBmShift = 185;

    // IMaterialSystem vtable is shifted vs L4D2 (GetBackBufferFormat returned a
    // pointer). Call these by RVA. BeginRT no-ops after startup unless
    // isGameRunning at kCMaterialSystem_isGameRunning is cleared (BM, not L4D2's +0x2AB8).
    static constexpr int kCMaterialSystem_isGameRunning = 0x2AA4;
    Offset BeginRTAlloc{ "materialsystem.dll", 0x48450,
        "56 8B F1 80 BE A4 2A 00 00 00 74 10" };
    Offset EndRTAlloc{ "materialsystem.dll", 0x4AD80,
        "80 B9 A4 2A 00 00 00 75 62" };
    // Real CreateNamedRenderTargetTextureEx (BeginRT+3). Distinct from Ex2 at
    // 0x49600 which ends the shared prologue with `mov eax,[ecx]`.
    Offset CreateNamedRTEx{ "materialsystem.dll", 0x49660,
        "55 8B EC 83 B9 A0 2A 00 00 00 75 14 68 ? ? ? ? FF 15 ? ? ? ? 83 C4 04 33 C0 5D C2 20 00 8B 0D" };
    // CreateNamedRenderTargetTextureEx2 (Begin+Ex+End). Same args as Ex.
    // Distinguishes from Ex at the `mov eax,[ecx]` vs `mov ecx,[imm]`.
    Offset CreateNamedRTEx2{ "materialsystem.dll", 0x49600,
        "55 8B EC 83 B9 A0 2A 00 00 00 75 14 68 ? ? ? ? FF 15 ? ? ? ? 83 C4 04 33 C0 5D C2 20 00 8B 01" };
    // CMaterialSystem vtable slot 30 (+0x78). Thunk: g_pShaderAPI +0x458.
    // Slot 31 (+0x7C) is NOT GetBackBufferFormat on BM (returns a pointer).
    Offset GetBackBufferDimensions{ "materialsystem.dll", 0x52d20,
        "55 8B EC 8B 0D ? ? ? ? 8B 01 8B 80 58 04 00 00 5D FF E0" };
    // IVEngineClient slot 1 = GetLightForPoint (Vector by value, bClamp).
    // Same early-vtable layout as Source SDK 2013; slot 5/7 already match BM.
    static constexpr int kIEngineClient_GetLightForPoint = 1;
    // IVEngineClient slot 5. Goes through videomode, not the D3D backbuffer.
    // After load, Source Reset(2560) while we keep a 1576 swapchain left this
    // at 2560 and HUD downsample (client FUN_10267420) used CViewSetup
    // width/height against _rt_Hud created from GetBackBufferDimensions.
    Offset GetScreenSize{ "engine.dll", 0xA6BD0,
        "55 8B EC 8B 0D ? ? ? ? 56 8B 01 FF 90 9C 01 00 00" };
    // IVEngineClient slot 96 (+0x180). client.dll DrawViewModels (FUN_1020a8f0)
    // copies CViewSetup, then overwrites m_flAspectRatio with this (window
    // 16:9) before Push3DView. GetScreenSize is a different function and is
    // not on this path — the primary return is a videomode/config float.
    Offset GetScreenAspectRatio{ "engine.dll", 0x1012D0,
        "55 8B EC 8B 0D ? ? ? ? 83 EC 0C 81 F9 ? ? ? ? 75 16 F3 0F 10 0D" };

    Offset ProcessUsercmds{ "server.dll", 0x5320F0,
        "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08", 0, true };

    // CBasePlayer::ImpulseCommands. Ghidra server.dll FUN_1022dd40.
    // Hooked lazily once server.dll is loaded (not at Offset() time).
    static constexpr int kCBasePlayer_ImpulseCommands = 0x22DD40;

    // L4D2 IMaterialSystem dump slots. BM is +7 (FindTexture 77→84,
    // GetRenderContext 96→103). Runtime ResolveMaterialThreadSlots confirms
    // by locating GetBackBufferDimensions in the live vtable.
    static constexpr int kDumpSetThreadModeVt = 10;
    static constexpr int kDumpGetThreadModeVt = 11;
    static constexpr int kDumpExecuteQueuedVt = 13;
    static constexpr int kDumpGetBackBufferDimensionsVt = 30;
    static constexpr int kDumpEndFrameVt = 37;
    static constexpr int kIMaterialSystemVTableShift = 7;

    // IMaterialSystem vtable (Ghidra materialsystem.dll, GetRenderContext slot 103).
    static constexpr int kIMaterialSystem_FindTextureVt = 84; // +0x150
    // Dump slot 71 + 7 (same shift as FindTexture 77→84 / GetRenderContext 96→103).
    static constexpr int kIMaterialSystem_FindMaterialVt = 78; // +0x138
    // FindMaterial 78, then First/Next/Invalid/Get/GetNum, FindTexture 84.
    // No IsMaterialLoaded between FindMaterial and FirstMaterial (not dedicated).
    static constexpr int kIMaterialSystem_FirstMaterialVt = 79;
    static constexpr int kIMaterialSystem_NextMaterialVt = 80;
    static constexpr int kIMaterialSystem_InvalidMaterialVt = 81;
    static constexpr int kIMaterialSystem_GetMaterialVt = 82;
    static constexpr int kIMaterialSystem_SetRTFBOverrideVt = 142; // +0x238
    static constexpr int kIMaterialSystem_GetRTFBDimensionsVt = 143; // +0x23C

    // CBlackMesaPlayer (server). Impulse int +0xe44; flashlight virtuals.
    static constexpr int kCBasePlayer_m_nImpulse = 0xE44;
    static constexpr int kCBasePlayer_FlashlightIsOnVt = 0x5D4 / 4;
    static constexpr int kCBasePlayer_FlashlightTurnOnVt = 0x5D8 / 4;
    static constexpr int kCBasePlayer_FlashlightTurnOffVt = 0x5DC / 4;

    // client.dll CClientShadowMgr::UpdateFlashlightState (research/resolution-hud-flashlight.md).
    // FlashlightState_t: origin float[0..2], forward float[3..5], fov float[9].
    static constexpr int kUpdateFlashlightState = 0x11BB20;

    // L4D2 leftover referenced by copied sdk.h melee helpers. Unused on Black Mesa.
    struct { int address = 0; } GetMeleeWeaponInfoClient;
};
