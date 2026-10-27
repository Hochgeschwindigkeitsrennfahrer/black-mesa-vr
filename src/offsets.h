#pragma once
#include "sigscanner.h"
#include "game.h"

// Signatures verified offline against the installed Steam Black Mesa modules.
// public-beta buildid 25607072 (2026-10-02, "Anomalous Improvements"):
// steam.inf PatchVersion is still 100002, but client/engine/materialsystem
// were rebuilt. Vtable slot numbers held. CMatRenderContext RT-stack fields
// moved +0xC (0x4C/0x40 -> 0x58/0x4C). CMaterialSystem alloc flags moved
// +0x24 (isGameRunning 0x2AA4 -> 0x2AC8, RT-lock 0x2AA0 -> 0x2AC4).

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
            // A jump at the old offset = the mod's own early hook (GetScreenAspectRatio, GetBackBufferDimensions),
            // not a game update: the bytes were there until MinHook replaced them.
            const auto* at = reinterpret_cast<const unsigned char*>(GetModuleHandleA(this->moduleName.c_str())) + currentOffset;
            if (!optional && currentOffset > 0 && *at == 0xE9)
                return;
            if (!optional)
            {
                Game::logMsg("Signature not found in %s: %s", this->moduleName.c_str(), this->signature.c_str());
                Game::noteMissingOffset((this->moduleName + ": " + this->signature).c_str());
            }
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
    Offset RenderView{ "client.dll", 0x201DB0,
        "55 8B EC 81 EC DC 02 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 8B 5D 08 56" };

    Offset g_pClientMode{ "client.dll", 0x15D576,
        "56 57 8B F9 8B 0D ? ? ? ? 8B 01 FF 50 24", 6 };

    Offset CreateMove{ "client.dll", 0x102590,
        "55 8B EC E8 ? ? ? ? 8B C8 85 C9 75 06 B0 01 5D C2 08 00" };

    // FindNearbyVguiScreen(eye, angles, team). cdecl. Only caller is
    // C_BasePlayer::DetermineVguiInputMode. Hover (C_VGuiScreen::ClientThink)
    // uses EyePosition/EyeAngles; clicks need m_nButtonState at +0x6B4, which
    // DetermineVguiInputMode skips when ISurface::IsCursorVisible is true.
    Offset FindNearbyVguiScreen{ "client.dll", 0xEE490,
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC B8 00 00 00" };
    Offset SetVGuiScreenButtonState{ "client.dll", 0xEEDA0,
        "55 8B EC 8B 4D 08 85 C9 74 09 8B 45 0C 89 81 B4 06 00 00 5D C3" };
    Offset IntersectVguiScreenRay{ "client.dll", 0xEEA50,
        "55 8B EC 83 EC 48 56 8B F1 8B 06 FF 50 24" };
    static constexpr int kC_VGuiScreen_ButtonState = 0x6B4;
    static constexpr int kC_VGuiScreen_PixelWidth = 0x6A0;
    static constexpr int kC_VGuiScreen_PixelHeight = 0x6A4;
    static constexpr int kC_VGuiScreen_PanelPtr = 0x72C;
    // IClientMode* in client.dll .data. C_VGuiScreen::ClientThink 0xFB7F0.
    static constexpr int kClientModeGlobalRva = 0x6D6D00; // beta 26234270 (25844216: 0x6D5B58)
    static constexpr int kIClientMode_ActivateInGameVGuiContext = 30;
    // vgui2 VGUI_InputInternal001 as used by ClientThink (no extra GetMouseFocus).
    static constexpr int kVguiVtInternalCursorMoved = 0x108 / 4;
    static constexpr int kVguiVtInternalMousePressed = 0x10C / 4;
    static constexpr int kVguiVtInternalMouseReleased = 0x114 / 4;
    static constexpr int kVguiVtSetMouseCodeState = 0x14C / 4;
    static constexpr int kVguiMouseLeftCode = 0x6B;
    static constexpr int kCBasePlayer_GetTeamNumberVt = 0x134;
    static constexpr int kCBasePlayer_EyePositionVt = 0x268;
    static constexpr int kCBasePlayer_EyeAnglesVt = 0x26C;

    // Build 25844216 (2026-10-10): C_BlackMesaViewModel vtable slot 230 (0x398).
    // Register allocation changed, so the 25607072 prologue no longer matches.
    Offset CalcViewModelView{ "client.dll", 0x29CC70,
        "55 8B EC 83 EC 24 53 56 57 8B 7D 08 8B F1 85 FF" };

    // Ghidra client.dll image 0x10000000. FUN_1029d930 (CalcViewModelView)
    // ends in these. FUN_100af720 writes abs origin at this+0x294 and calls
    // FUN_10077220(this,1) to invalidate; FUN_100af600 writes abs angles at
    // this+0x2D0 via FUN_10077220(this,2). Raw field writes skip invalidate,
    // so bones/render origin stay on the camera pivot.
    // Build 25844216 (2026-10-10): both moved -0xE160 from 25607072. Each
    // compares this+0x294 / this+0x2D0 (ucomiss) before writing; the call
    // site checks those bytes. Field offsets themselves did not move.
    static constexpr int kCBaseEntity_SetAbsOrigin = 0xA18E0; // beta 26234270 (25844216: 0xA1820)
    static constexpr int kCBaseEntity_SetAbsAngles = 0xA17C0; // beta 26234270 (25844216: 0xA1700)
    // C_BaseEntity abs origin written by SetAbsOrigin (Ghidra client FUN_100af720).
    static constexpr int kCBaseEntity_AbsOrigin = 0x294;

    // CBaseEntity::EmitSound(name, soundtime=0, duration=NULL). Ghidra
    // FUN_101d80d0 (thiscall). Vanilla HUD uses this for Player.WeaponSelected
    // (common/wpn_select.wav at SNDLVL_75dB — a world emit at the player).
    // The weapon wheel plays weapon_*.Draw on the weapon instead, and HUD
    // ticks go through IEngineSound::EmitAmbientSound (2D).
    static constexpr int kCBaseEntity_EmitSound = 0x1D8250; // beta 26234270 (25844216: 0x1D80D0); mid-function on both, never called
    static constexpr bool kCBaseEntity_EmitSoundVerified = false;

    // C_BlackMesaPlayer vtable +0x414. Thunk: call EyePosition (+0x268) and
    // return that vector. Distinct from EyePosition (do not hook +0x268 —
    // that moves the camera). Bytes: 55 8B EC 8B 11 FF 75 08 FF 92 68 02 00 00.
    static constexpr int kCBasePlayer_Weapon_ShootPosition = 0x6E1E0; // beta 26234270 (25844216: 0x6E120)

    // server.dll CBlackMesaPlayer vtable +0x23C. Same thunk onto EyePosition
    // at +0x230. Bytes: 55 8B EC 8B 11 FF 75 08 FF 92 30 02 00 00.
    static constexpr int kCBasePlayer_Weapon_ShootPosition_Server = 0x11E520;

    // server.dll CBasePlayer vtable +0x474 = FUN_10077240. CGrabController::
    // UpdateObject (FUN_10470da0) uses this as the hold origin, not +0x23C.
    // Bytes: 55 8B EC 83 EC 24 57 8B F9 8D 4D E8 6A 00 51 6A 00 8B 07 8B CF
    // FF 90 4C 02 00 00. Hold distance is 2*radius + 24 (DAT_105db264).
    static constexpr int kCBasePlayer_GrabHoldOrigin_Server = 0x772D0;

    // server.dll CBlackMesaPlayer vtable +0x474: `push [ebp+8]; call <eye
    // position>; ret 4`. Bullet weapons read their shot origin from this slot.
    // Beta 25844216: the only thunk at slot +0x474 of the two BM player
    // vtables (0x1067C7D8, 0x107268F0); stable 19042901 had it at 0x47C810.
    // Bytes: 55 8B EC FF 75 08 E8 ?? ?? ?? ?? 8B 45 08 5D C2 04 00.
    static constexpr int kBlackMesaPlayer_ShootSource_Server = 0x47D460; // beta 26234270 (25844216: 0x47D1E0)

    // server.dll CGrabController::SetTargetPosition FUN_10470990.
    // UpdateObject writes the hold point here after 2*radius+24. Bytes:
    // 55 8B EC 8B 45 08 8B D1 D9 00 D9 5A 04. ret 8.
    // client.dll C_BlackMesaPlayer::GetShootAngles, vtable slot 0x9D (RTTI .?AVC_BlackMesaPlayer@@),
    // beta 26234270. Was a server-byte pattern scan that never matched the client.
    static constexpr int kC_BlackMesaPlayer_GetShootAngles = 0x25AC20;
    static constexpr int kCGrabController_SetTargetPosition = 0x4715E0; // beta 26234270 (25844216: 0x471360)

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
    static constexpr int kBlackMesaPlayer_GetShootAngles_Server = 0x47F8E0; // beta 26234270 (25844216: 0x47F660)

    // server.dll grenade spawn, cdecl (type, Vector* velocity, AngularImpulse*,
    // CBaseEntity* owner, CBaseEntity* thrower, int mode) -> grenade. Type 1 =
    // frag: CWeapon_Frag's throw (FUN_10487250, vtable 0x10729204) builds
    // forward*speed + up*lift + 0.x * player velocity from GetShootAngles and
    // calls this. It places the grenade via kGrenadeThrowPosition_Server, then
    // DispatchSpawn, then the grenade's SetVelocity (vtable +0x3B4).
    // mode: 0 = origin at owner EyePosition (vtable +0x230), 1 = halfway
    // between eye and throw position, else the throw position. The frag
    // passes 0.
    // Bytes: 55 8B EC 8B 45 08 83 EC 30 83 F8 15.
    static constexpr int kGrenadeCreate_Server = 0x379B20; // beta 26234270 (25844216: 0x3798A0)
    // server.dll throw position, cdecl (grenade, Vector* pos, QAngle* ang,
    // CBaseEntity* owner) -> bool (false = spawn refused, grenade removed):
    // owner EyePosition (+0x230) / EyeAngles (+0x234), an offset, then a
    // trace back to the eye. Bytes: 55 8B EC 81 EC 88 00 00 00.
    static constexpr int kGrenadeThrowPosition_Server = 0x4832E0; // beta 26234270 (25844216: 0x483060)
    static constexpr int kBlackMesaPlayer_RecoilPunchAngles_Server = 0x14BC;

    // C_BaseAnimating IClientRenderable GetAttachment. `this` is entity+4.
    // Vec: FUN_10097a80 (index, Vector*, QAngle*). Matrix: FUN_100979b0.
    static constexpr int kCBaseAnimating_GetAttachmentVec = 0x89C40; // beta 26234270 (25844216: 0x89B80)
    static constexpr int kCBaseAnimating_GetAttachmentMatrix = 0x89B70; // beta 26234270 (25844216: 0x89AB0)

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
    static constexpr int kCTauBeam_ViewMuzzle = 0x228090; // beta 26234270 (25844216: 0x227E40)

    // client.dll CTauBeam impact glow (FUN_102346A0). ProgressBeam calls it as
    // (trace.endpos, trace.plane.normal, charge), not (beam start, beam end).
    // Unique bytes: 55 8B EC 83 EC 78 8B 45 08 0F 57 C9 56 57 8B 7D
    static constexpr int kCTauBeam_WorldBeam = 0x228400; // beta 26234270 (25844216: 0x2281B0)

    // client.dll CTauBeam fire: AngleVectors(this+0x18) then TraceRay from
    // this+0xC (FUN_102348B0). thiscall, 0 stack args. Unique bytes:
    // 53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC EC 00 00 00
    static constexpr int kCTauBeam_FireTrace = 0x228610; // beta 26234270 (25844216: 0x2283C0)

    // client.dll C_Weapon_Gluon impact trace (FUN_1029D430). ShootPosition +
    // EyeAngles (+0x274), not GetShootAngles, so the glow sits on the HMD look.
    // thiscall, 2 stack args (player, CGameTrace*), ret 8. Unique bytes:
    // 53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC D8 00 00 00
    static constexpr int kCWeaponGluon_ImpactTrace = 0x29C750; // beta 26234270 (25844216: 0x29B4F0)
    // client.dll C_Weapon_Gluon first-person beam update (FUN_1029A7D0).
    // thiscall, 0 stack args. Calls impact trace then SetControlPoint(0, end)
    // and C_GluonBeamFx::SetBeam (FUN_1029C960). Unique bytes:
    // 55 8B EC 81 EC D0 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F9
    static constexpr int kCWeaponGluon_BeamUpdate = 0x299AF0; // beta 26234270 (25844216: 0x298890)
    // client.dll C_GluonBeamFx::SetBeam (FUN_1029C960). thiscall, 4 stack args
    // (viewmodel, start, mid, end), ret 0x10. Unique bytes:
    // 55 8B EC 8B 45 0C 56 8B F1 C6 46 0C 01
    static constexpr int kCGluonBeamFx_SetBeam = 0x29BC80; // beta 26234270 (25844216: 0x29AA20)
    // client.dll C_GluonBeamFx Draw (FUN_1029B290) on IClientRenderable.
    // Start at this+0xC, end at this+0x24. thiscall, 1 stack arg, ret 4.
    // Unique bytes: 53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 68 05 00 00
    static constexpr int kCGluonBeamFx_Draw = 0x29A5B0; // beta 26234270 (25844216: 0x299350)
    // client.dll CNewParticleEffect::SetControlPoint (FUN_101A5170).
    // thiscall, 2 stack args (index, Vector*), ret 8.
    // Unique bytes: 55 8B EC 53 8B 5D 0C 57 8B F9 83 BF 54 1B 00 00 FF
    static constexpr int kParticleSetControlPoint = 0x197860; // beta 26234270 (25844216: 0x1976E0)
    // client.dll CParticleMgr::AddEffect (FUN_1019D940). thiscall, 1 stack arg
    // (CNewParticleEffect*), ret 4. If def+0x200 (view model effect) is set,
    // it puts the effect in render group 0xB (viewmodel FOV). Unique bytes:
    // 55 8B EC 8B 41 4C 53 56 8B 75 08
    static constexpr int kParticleMgr_AddEffect = 0x190030; // beta 26234270 (25844216: 0x18FEB0)
    // Both from `mov ecx,[g_pParticleSystemMgr]; push name; call FindParticleSystem` before the
    // "Attempted to create unknown particle system" warnings (3 sites agree). The old 0x5631CC/0x313390
    // were stale since an earlier build (no refs / mid-function on 25844216).
    static constexpr int kParticleSystemMgr = 0x56024C; // beta 26234270
    static constexpr int kParticleSystemMgr_Find = 0x3101E0; // beta 26234270
    // Re-verified on 26234270 (PatchGluonImpactParticleDefs also checks Find's prologue at runtime).
    static constexpr bool kParticleSystemMgrVerified = true;
    static constexpr int kCParticleDef_ViewModelEffect = 0x200;
    static constexpr int kCParticleDef_Name = 0x228;
    static constexpr int kCNewParticleEffect_Def = 0x58;
    static constexpr int kCNewParticleEffect_ViewModel = 0x1C80;
    // client.dll g_pClientLeafSystem (IClientLeafSystem*). vtable+0x38 is
    // SetRenderGroup(handle, group); AddEffect uses group 0xB for viewmodel.
    static constexpr int kClientLeafSystemPtr = 0x553D10;
    // client.dll gpGlobals (CGlobalVarsBase*). CHudAir FUN_10271c90 subtracts
    // curtime at +0xC from m_AirFinished. Same .data neighborhood as the leaf
    // system pointer.
    static constexpr int kClientGpGlobalsPtr = 0x550A04; // beta 26234270, 1195 refs (25844216: 0x54FA04)
    static constexpr int kCGlobalVars_curtime = 0xC;
    // DT_LocalPlayerExclusive (FUN_100b7240): m_nWaterLevel +0x180 (byte),
    // m_AirFinished +0x16e4 (float). WL_Eyes == 3 is fully underwater.
    static constexpr int kCBasePlayer_nWaterLevel = 0x180;
    // client.dll C_BaseEntity prediction datamap m_MoveType (FIELD_CHARACTER),
    // one byte; MOVETYPE_LADDER = 9.
    static constexpr int kCBaseEntity_MoveType = 0x17C;
    // client.dll DT_FuncLadder (HL2 func_useableladder): player mount points,
    // relative to the ladder's origin (func_ladder.cpp Spawn subtracts it).
    static constexpr int kCFuncLadder_MountTop = 0x6C0;
    static constexpr int kCFuncLadder_MountBottom = 0x6CC;
    static constexpr int kCBasePlayer_AirFinished = 0x16e4;

    // client.dll C_Weapon_RPG laser update (FUN_10291F80). CHudCrosshair Paint
    // calls weapon vtable +0x5C4. Draws ISurface circles at ScreenWidth/2,
    // ScreenHeight/2 (HMD centre in VR). Extra red geometry if m_bLaserOn.
    // thiscall, 0 stack args.
    static constexpr int kCWeaponRpg_UpdateLaser = 0x2912A0; // beta 26234270 (25844216: 0x290040)
    // RecvTable DT_Weapon_RPG (FUN_10291d30): m_hHomingGrenade +0xAE0,
    // m_bNeedReload +0xAE4, m_bLaserOn +0xAE5. There is no m_hLaserDot netvar;
    // the visible mark is a separate CEnvLaserDot sprite (CSprite::DrawModel
    // FUN_101dbf70) whose origin is the look-trace, so it also sits at centre.
    static constexpr int kCWeaponRpg_hHomingGrenade = 0xAE0;
    static constexpr int kCWeaponRpg_hLaserDot = 0xAE0; // stale name; do not resolve as the dot
    static constexpr int kCWeaponRpg_bLaserOn = 0xAE5;
    // client.dll CHudCrosshair::Paint (FUN_10278460). Calls weapon vtable +0x5C4.
    // thiscall, 0 stack args. Unique bytes: 55 8B EC 51 53 56 8B 35 ? ? ? ? 8B D9
    static constexpr int kCHudCrosshair_Paint = 0x276D30; // beta 26234270 (25844216: 0x275E00)
    // DT_BaseEntity m_fEffects (FUN_100a4cb0). EF_NODRAW = 0x20.
    static constexpr int kCBaseEntity_fEffects = 0x80;
    static constexpr int kEF_NODRAW = 0x20;
    // client.dll C_SpriteRenderer::DrawSprite (FUN_100f6900). thiscall,
    // 15 stack args, ret 0x3C. ecx is the renderer at CSprite+0x69C.
    // Copies origin from arg3; CEnvLaserDot's origin is the look-trace.
    static constexpr int kCSpriteRenderer_DrawSprite = 0xE8B80; // beta 26234270 (25844216: 0xE8A00)
    // client.dll FUN_102dcbf0. stdcall 8 args, ret 0x20. Alternate sprite
    // draw: origin from IClientRenderable vtable+4, then FUN_100f6b40.
    // Unique bytes: 55 8B EC 83 EC 64 53 8B 5D 08 8B CB 56 8B 03 FF 50 04
    static constexpr int kCSpriteRenderable_Draw = 0x2DBED0; // beta 26234270 (25844216: 0x2DAC70)
    // client.dll C_EnvLaserDot DrawModel override (FUN_10221fd0). thiscall,
    // 1 stack arg (studio flags), ret 4. Traces from attachment "laser" or
    // owner EyePosition/EyeAngles (+0x268/+0x26c), then FUN_100821a0.
    // Unique bytes (beta 25844216; stable 19042901 had `83 EC A0` at 0x221FD0):
    // 55 8B EC 81 EC A0 00 00 00 A1 ? ? ? ? 33 C5 89 45 FC F7 45 08 00 00 00 4E
    static constexpr int kCEnvLaserDot_Draw = 0x215D30; // beta 26234270 (25844216: 0x215AE0)
    // client.dll camera-facing sprite quad (FUN_100821a0). cdecl, origin* +
    // width/height/color. Unique bytes: 53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04
    static constexpr int kSpriteQuad = 0x74360; // beta 26234270 (25844216: 0x742A0)
    // client.dll CViewRenderBeams::DrawBeam (FUN_101f5be0). thiscall, Beam_t*
    // on the stack, ret 4. Unique bytes: 55 8B EC 83 EC 2C A1 ? ? ? ? 33 C5 89 45 FC 89 4D E8
    static constexpr int kCViewRenderBeams_DrawBeam = 0x1E84A0; // beta 26234270 (25844216: 0x1E82B0)
    static constexpr int kBeam_t_type = 0x30;
    static constexpr int kBeam_t_start = 0x3C;
    static constexpr int kBeam_t_delta = 0xB4;
    static constexpr int kBeam_t_r = 0xE0;

    // IClientMode slot 32. Reads viewmodel_fov_override then weapon GetViewModelFOV
    // (~54). That is why console fov does not change gun/near scale in VR.
    Offset GetViewModelFOV{ "client.dll", 0x20A140,
        "55 8B EC 51 8B 0D ? ? ? ? 81 F9 ? ? ? ? 75 16 F3 0F 10 0D" };

    Offset AdjustEngineViewport{ "client.dll", 0x102540,
        "C2 10 00 CC CC CC CC CC CC CC CC CC CC CC CC CC B0 01 C2 08 00" };

    Offset LevelInit{ "client.dll", 0x102D00,
        "55 8B EC 83 EC 20 56 8B F1 6A 01 68 ? ? ? ?" };

    Offset LevelShutdown{ "client.dll", 0x102DB0,
        "55 8B EC 83 EC 20 56 8B F1 B9 ? ? ? ? E8" };

    // CModelRender::DrawModelExecute — IModelRender vtable slot 19 (+0x4C).
    // thiscall (state, ModelRenderInfo_t&, bones), ret 0xC. Ghidra FUN_10113e80.
    // Do NOT use 0xF6A20: that prologue matches DispInfo_LoadDisplacements'
    // cdecl 5-arg helper (FUN_100f6a20). Hooking it as DME hung load-to-menu
    // (2026-08-18). DrawModelEx (+0x40) calls Setup (+0x48) then this slot.
    Offset DrawModelExecute{ "engine.dll", 0x1141F0,
        "55 8B EC 81 EC 68 03 00 00 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 53 8B 5D 0C" };

    // engine.dll FindNearest lightcache (LRU walk, sentinel 0x200).
    // mat_queue_mode 2 + GetLightForPoint/studio lighting can cycle +0x1D0
    // next pointers; the engine walk never hits 0x200 and freezes the game
    // thread (verified 2026-09-08, PID 25228, 12-node cycle).
    Offset LightcacheFindNearest{ "engine.dll", 0x1D1B50,
        "55 8B EC 83 EC 1C 0F B7 0D ? ? ? ? 33 D2 53 56 BB 00 02 00 00" };

    Offset VGui_Paint{ "engine.dll", 0x23AD00,
        "55 8B EC 83 EC 18 53 8B D9 8B 0D ? ? ? ? FF 15" };

    // CMatRenderContext slot 7. RT-stack count moved 0x4C -> 0x58 on the beta.
    Offset GetRenderTarget{ "materialsystem.dll", 0x69BB0,
        "83 79 58 00 7E 0E 8B 41 58 8D 14 C0" };
    // CMatRenderContext::GetRenderTargetDimensions (vtable +0x20 after GetRT).
    // CLensflare WorldToScreen uses this as pixel size; GetViewport (+0x9C) is
    // the mesh NDC divisor. HWND 16:9 vs square eye parks flares off the lights.
    // Hardware slot 8. Stack base moved 0x40 -> 0x4C along with the count.
    Offset GetRenderTargetDimensions{ "materialsystem.dll", 0x69BD0,
        "55 8B EC 8B 41 58 56 8D 14 C0 8B 41 4C 8B 74 90 DC 85 F6 74 1D" };
    // Same body as the queued context on this beta (one copy). Optional.
    Offset GetRenderTargetDimensionsBase{ "materialsystem.dll", 0x65460,
        "55 8B EC 56 8B F1 57 83 7E 58 00 74 2F 8B 46 58 8D 14 C0 8B 46 4C 8B 7C 90 DC",
        0, true };
    // CMatQueuedRenderContext slot 8. The old `ret 8` stub is gone; this
    // reads the RT stack (count at +0x58) the way the hardware context does.
    Offset GetRenderTargetDimensionsQueued{ "materialsystem.dll", 0x65460,
        "55 8B EC 56 8B F1 57 83 7E 58 00 74 2F 8B 46 58 8D 14 C0 8B 46 4C 8B 7C 90 DC",
        0, true };
    Offset GetViewport{ "materialsystem.dll", 0x69DF0,
        "55 8B EC 8B 41 58 56 8D 14 C0 8B 41 4C 83 7C 90 F8 00" };
    // Queued GetViewport (slot 39). Hardware 0x69DF0 is a different function.
    Offset GetViewportQueued{ "materialsystem.dll", 0x654E0,
        "55 8B EC 56 57 8B F9 8B 47 58 8D 14 C0 8B 47 4C 83 7C 90 F8 00 8D 34 90 7C 2C",
        0, true };
    Offset Viewport{ "materialsystem.dll", 0x6B2B0,
        "55 8B EC 56 FF 75 14 8B F1 FF 75 10 FF 75 0C FF 75 08 E8" };
    Offset PushRenderTargetAndViewport{ "materialsystem.dll", 0x6B750,
        "55 8B EC 83 EC 24 8B 45 08 89 45 DC 8B 45 0C 89 45 EC" };
    Offset PopRenderTargetAndViewport{ "materialsystem.dll", 0x6B5D0,
        "56 8B F1 83 7E 58 00 74 15 8B 06 6A 00 FF 50 10 FF 4E 58" };
    // Hardware CMatRenderContext vtable: DrawScreenSpaceRectangle is two
    // slots before the 6-arg PushRT (0x6A3D0). Wrapper ret 0x38. Fire/glass/
    // AMS overlays pass dest 2560x1440 onto the 3168 G-buffer (bm_c1a1d).
    Offset DrawScreenSpaceRectangle{ "materialsystem.dll", 0x69120,
        "55 8B EC 8B 4D 08 8B 01 FF 90 7C 01 00 00 D9 EE" };
    // CopyRenderTargetToTextureEx — UpdateRefractTexture / POT FB blit.
    Offset CopyRenderTargetToTextureEx{ "materialsystem.dll", 0x687A0,
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
    // BM 6-arg PushRT is slot 143 vs Source 104 (~+39). Probe the BM slot
    // first, then 146, under SEH. Do not latch 146 if QueueFunctorInternal
    // SEH-fails — that object is not ICallQueue (Index SteamVR 2026-09-09).
    // public-beta materialsystem inserted one slot between 149 and 180:
    // CMatQueuedRenderContext slot 186 is `lea eax,[ecx+0x298]; ret` and
    // CMatRenderContext slot 186 returns 0. Old slot 185 now writes
    // [ecx+0x208] = 0, so probing it corrupts the context.
    static constexpr int kIMatRenderContext_GetCallQueueSlotSource = 146;
    static constexpr int kIMatRenderContext_GetCallQueueSlotBmShift = 186;

    // IMaterialSystem vtable is shifted vs L4D2 (GetBackBufferFormat returned a
    // pointer). Call these by RVA. BeginRT no-ops after startup unless
    // isGameRunning. public-beta moved this field +0x24 (was 0x2AA4).
    static constexpr int kCMaterialSystem_isGameRunning = 0x2AC8;
    Offset BeginRTAlloc{ "materialsystem.dll", 0x48810,
        "56 8B F1 80 BE C8 2A 00 00 00 74 10" };
    Offset EndRTAlloc{ "materialsystem.dll", 0x4B150,
        "80 B9 C8 2A 00 00 00 75 62" };
    // Real CreateNamedRenderTargetTextureEx. Distinct from Ex2, which ends
    // the shared prologue with `mov eax,[ecx]`. Lock flag is this+0x2AC4.
    Offset CreateNamedRTEx{ "materialsystem.dll", 0x49A30,
        "55 8B EC 83 B9 C4 2A 00 00 00 75 14 68 ? ? ? ? FF 15 ? ? ? ? 83 C4 04 33 C0 5D C2 20 00 8B 0D" };
    // CreateNamedRenderTargetTextureEx2 (Begin+Ex+End). Same args as Ex.
    Offset CreateNamedRTEx2{ "materialsystem.dll", 0x499D0,
        "55 8B EC 83 B9 C4 2A 00 00 00 75 14 68 ? ? ? ? FF 15 ? ? ? ? 83 C4 04 33 C0 5D C2 20 00 8B 01" };
    // CMaterialSystem vtable slot 30 (+0x78). Thunk: g_pShaderAPI +0x458.
    // Slot 31 (+0x7C) is NOT GetBackBufferFormat on BM (returns a pointer).
    Offset GetBackBufferDimensions{ "materialsystem.dll", 0x53EF0,
        "55 8B EC 8B 0D ? ? ? ? 8B 01 8B 80 58 04 00 00 5D FF E0" };
    // IVEngineClient slot 1 = GetLightForPoint (Vector by value, bClamp).
    // Same early-vtable layout as Source SDK 2013; slot 5/7 already match BM.
    static constexpr int kIEngineClient_GetLightForPoint = 1;
    // IVEngineClient slot 5. Goes through videomode, not the D3D backbuffer.
    // After load, Source Reset(2560) while we keep a 1576 swapchain left this
    // at 2560 and HUD downsample (client FUN_10267420) used CViewSetup
    // width/height against _rt_Hud created from GetBackBufferDimensions.
    Offset GetScreenSize{ "engine.dll", 0xA6D30,
        "55 8B EC 8B 0D ? ? ? ? 56 8B 01 FF 90 9C 01 00 00" };
    // IVEngineClient slot 96 (+0x180). client.dll DrawViewModels (FUN_1020a8f0)
    // copies CViewSetup, then overwrites m_flAspectRatio with this (window
    // 16:9) before Push3DView. GetScreenSize is a different function and is
    // not on this path — the primary return is a videomode/config float.
    Offset GetScreenAspectRatio{ "engine.dll", 0x101560,
        "55 8B EC 8B 0D ? ? ? ? 83 EC 0C 81 F9 ? ? ? ? 75 16 F3 0F 10 0D" };

    // CViewSetup::ComputeViewMatrices(worldToView, viewToProjection,
    // worldToProjection), thiscall, ret 0xC. Honours m_bOffCenter (0xEC).
    // The areaportal view setup calls it on its private view copy (beta
    // 25844216: global engine+0x3F7820, return engine+0x139663; stable
    // 19042901: 0x3F5820 / 0x138DF3); it pairs
    // the result with a centred +-tan(fov/2) window.
    Offset ComputeViewMatrices{ "engine.dll", 0x100B40,
        "55 8B EC 51 56 8B F1 57 F3 0F 10 86 E8 00 00 00 0F 2E 05", 0, true };
    static constexpr uint32_t kAreaPortalComputeViewReturnRva = 0x139743; // beta 26234270 (25844216: 0x139663)

    Offset ProcessUsercmds{ "server.dll", 0x532D40,
        "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08", 0, true };

    // CBasePlayer::ImpulseCommands. Ghidra server.dll FUN_1022dd40.
    // Hooked lazily once server.dll is loaded (not at Offset() time).
    static constexpr int kCBasePlayer_ImpulseCommands = 0x22E420; // betas 25844216 + 26234270 (0x22DD40 was mid-function since an earlier build)

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
    // CClientShadowMgr::UpdateFlashlightState. public-beta +0x260.
    // Pushes the string "CClientShadowMgr::UpdateFlashlightState".
    // client.dll build 25844216 (2026-10-10). 0x11BD80 is mid-function and
    // the detour AVs at client+0x11B6A4 on startup (exit code 1).
    static constexpr int kUpdateFlashlightState = 0x10DDA0; // beta 26234270 (25844216: 0x10DC20)

    // L4D2 leftover referenced by copied sdk.h melee helpers. Unused on Black Mesa.
    struct { int address = 0; } GetMeleeWeaponInfoClient;
};
