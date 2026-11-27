#include "bmvr_cvars.h"
#include "bmvr_flags.h"
#include "game.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "bmvr_seh.h"

// No Source SDK here, so a ConVar is built by cloning an existing client.dll
// ConVar and patching its fields. Layout (BM, Alien Swarm-era tier1; the same
// offsets Game::SetConVarInt writes):
//   0x00 ConCommandBase vtable   0x04 m_pNext   0x08 m_bRegistered
//   0x0C m_pszName   0x10 m_pszHelpString   0x14 m_nFlags
//   0x18 IConVar vtable   0x1C m_pParent   0x20 m_pszDefaultValue
//   0x24 m_pszString   0x28 m_StringLength   0x2C m_fValue ^ this
//   0x30 m_nValue ^ this   0x34 m_bHasMin   0x38 m_fMinVal
//   0x3C m_bHasMax   0x40 m_fMaxVal   0x44 change callbacks (CUtlVector)
// The cloned vtables are client.dll's tier1 code: the console and the
// options dialog set our values through it, and its GetDLLIdentifier makes
// the engine unregister our cvars together with client.dll's at shutdown.
// m_pszString points at our own buffer with a capacity the short values
// never exceed, so tier1 never reallocates or frees it.

namespace bmvr
{
    namespace
    {
        constexpr size_t kCloneBytes = 0x58;
        constexpr size_t kObjBytes = 0x80;
        constexpr int kStrCap = 64;
        // ICvar (VEngineCvar004 on BM): RegisterConCommand.
        constexpr int kICvarRegisterConCommand = 9;
        // ConCommandBase::GetName: slot 6 (2013 layout), 4 on 2007 builds
        // (same probe as Game's ConVarGetName).
        constexpr int kGetNameSlots[] = { 6, 4 };

        enum class Kind { Bool, Float };

        struct VrCvar
        {
            const char* name;
            const char* help;
            Kind kind;
            float minVal;
            float maxVal;
            bool* boolTarget;
            float* floatTarget;
            alignas(8) uint8_t obj[kObjBytes];
            char str[kStrCap];
            char def[kStrCap];
            float lastSeen;
        };

        VrCvar g_Cvars[] = {
            { "bmvr_mc_frame_reset", "VR: MulticoreMode resets per-frame eye/pose flags at the game thread's frame end (0 = at the render thread's Present, skips frames when it lags)",
              Kind::Bool, 0.f, 1.f, &g_McFrameReset, nullptr },
            { "bmvr_pose_flags_noblink", "VR: pose valid flags never blink off during a tracking update (0 = old: laser could start at the eye for a frame)",
              Kind::Bool, 0.f, 1.f, &g_PoseFlagsNoBlink, nullptr },
            { "bmvr_crouch_hold", "VR: right stick down crouches only while held (0 = each push toggles)",
              Kind::Bool, 0.f, 1.f, &g_CrouchStickHold, nullptr },
            { "bmvr_stick_jump_crouch", "VR: how far the right stick goes up/down before it jumps/crouches (0.3..0.95)",
              Kind::Float, 0.3f, 0.95f, nullptr, &g_StickJumpCrouchThreshold },
            { "bmvr_stick_jump_crouch_angle", "VR: right stick must point within this many degrees of straight up/down to jump/crouch (10..45)",
              Kind::Float, 10.f, 45.f, nullptr, &g_StickJumpCrouchAngle },
            { "bmvr_skip_screen_effects", "VR: leave out the stun / intro-blur screen overlays (they paste one flat copy of the view over both eyes)",
              Kind::Bool, 0.f, 1.f, &g_VrSkipScreenEffects, nullptr },
            { "bmvr_envmap_scale_world", "VR: cubemap reflection strength on walls/floors (brush materials), 0..1",
              Kind::Float, 0.f, 1.f, nullptr, &g_EnvmapScaleWorld },
            { "bmvr_envmap_scale_models", "VR: cubemap reflection strength on props/NPCs (not your weapons/hands), 0..1",
              Kind::Float, 0.f, 1.f, nullptr, &g_EnvmapScaleModels },
            { "bmvr_turn_blocks_jump_crouch", "VR: after the right stick turned, no stick jump/crouch until it is centred",
              Kind::Bool, 0.f, 1.f, &g_TurnBlocksJumpCrouch, nullptr },
            { "bmvr_sprint_toggle", "VR: a sprint click keeps sprinting until the move stick is released (0 = hold the click)",
              Kind::Bool, 0.f, 1.f, &g_SprintToggle, nullptr },
            { "bmvr_scope_mono_zoom", "VR: A/B: zoomed crossbow renders both eyes from the head centre (same picture in both eyes)",
              Kind::Bool, 0.f, 1.f, &g_ScopeMonoZoom, nullptr },
            { "bmvr_scope_follows_gun", "VR: zoomed crossbow view follows the gun's aim instead of the head",
              Kind::Bool, 0.f, 1.f, &g_ScopeFollowsGun, nullptr },
            { "bmvr_scope_gun_smooth", "VR: smoothing time in seconds for the gun-driven scope view (0 = raw aim)",
              Kind::Float, 0.f, 0.5f, nullptr, &g_ScopeGunSmoothSec },
            { "bmvr_scope_raise", "VR: two-handed crossbow raised to the face zooms in, lowered zooms out",
              Kind::Bool, 0.f, 1.f, &g_ScopeRaiseToZoom, nullptr },
            { "bmvr_scope_raise_dist", "VR: gun hand to headset distance in metres that counts as raised to the face (out at +0.10)",
              Kind::Float, 0.1f, 0.6f, nullptr, &g_ScopeRaiseMeters },
            { "bmvr_mounted_gun_gain", "VR: mounted gun (50cal/TOW) barrel turn per degree of hand swing (1 = handle stays in the hand)",
              Kind::Float, 0.5f, 4.f, nullptr, &g_MountedGunGain },
            { "bmvr_gun_pitch", "VR: extra gun pitch in degrees on top of the controller's own tilt (negative = lower)",
              Kind::Float, -45.f, 45.f, nullptr, &g_GunPitchOffset },
            { "bmvr_hand_gun_right", "VR: support hand on the gun, along the gun: toward the gun hand's side, game units",
              Kind::Float, -8.f, 8.f, nullptr, &g_VrHandsGunGripRight },
            { "bmvr_hand_gun_up", "VR: support hand on the gun, along the gun: up, game units",
              Kind::Float, -8.f, 8.f, nullptr, &g_VrHandsGunGripUp },
            { "bmvr_hand_gun_forward", "VR: support hand on the gun, along the gun: toward the muzzle, game units",
              Kind::Float, -12.f, 12.f, nullptr, &g_VrHandsGunGripForward },
            { "bmvr_hand_hud", "VR: health/suit panel on the off-hand and ammo beside the gun hand",
              Kind::Bool, 0.f, 1.f, &g_HandHud, nullptr },
            { "bmvr_damage_flash", "VR: red flash on the eye edges when health or armor drops (flat HUD damage indicator)",
              Kind::Bool, 0.f, 1.f, &g_DamageFlash, nullptr },
            { "bmvr_damage_flash_strength", "VR: damage flash peak opacity (0..1)",
              Kind::Float, 0.f, 1.f, nullptr, &g_DamageFlashStrength },
            { "bmvr_hand_hud_size", "VR: off-hand health/suit panel size, game units",
              Kind::Float, 1.f, 6.f, nullptr, &g_HandHudSize },
            { "bmvr_hand_hud_up", "VR: off-hand panel height above the controller, game units",
              Kind::Float, -4.f, 8.f, nullptr, &g_HandHudUp },
            { "bmvr_hand_hud_forward", "VR: off-hand panel along the controller (negative = toward the wrist), game units",
              Kind::Float, -8.f, 8.f, nullptr, &g_HandHudForward },
            { "bmvr_hand_hud_side", "VR: off-hand panel sideways (positive = left), game units",
              Kind::Float, -6.f, 6.f, nullptr, &g_HandHudSide },
            { "bmvr_hand_hud_tilt", "VR: off-hand panel pitch vs the controller, degrees (negative = front edge down)",
              Kind::Float, -90.f, 90.f, nullptr, &g_HandHudTilt },
            { "bmvr_hand_hud_roll", "VR: off-hand panel roll toward the body, degrees (mirrored for the right hand)",
              Kind::Float, -180.f, 180.f, nullptr, &g_HandHudRoll },
            { "bmvr_hand_roll", "VR: hand models rolled thumb-up around the controller, degrees (90 = palms facing each other; not while holding a weapon)",
              Kind::Float, -135.f, 135.f, nullptr, &g_VrHandsRollDeg },
            { "bmvr_hands_blackmesa", "VR: hands built from Black Mesa's own first-person arms (game files) instead of the glove files",
              Kind::Bool, 0.f, 1.f, &g_VrHandsBlackMesa, nullptr },
            { "bmvr_physical_weapons", "VR: physical weapons master switch: manual reloads, grenade pin, support hand pose/lock, satchel/snark hand throw, recoil",
              Kind::Bool, 0.f, 1.f, &g_PhysicalWeapons, nullptr },
            { "bmvr_mag_reload", "VR: manual magazines, MP5 + glock (reload ejects, left grip at the left hip takes one, seat it, pull the bolt/slide)",
              Kind::Bool, 0.f, 1.f, &g_VrMagReload, nullptr },
            { "bmvr_mag_hand_forward", "VR: magazine centre ahead of the drawn left fist (game units, hand frame)",
              Kind::Float, -8.f, 8.f, nullptr, &g_VrMagHandForward },
            { "bmvr_mag_hand_up", "VR: magazine centre above the drawn left fist (game units; positive = sticks up out of the fist)",
              Kind::Float, -8.f, 8.f, nullptr, &g_VrMagHandUp },
            { "bmvr_shell_out", "VR: shotgun shell centre beyond the left hand's pinch point (fraction of its length)",
              Kind::Float, -0.5f, 1.f, nullptr, &g_VrShellOut },
            { "bmvr_357_flick", "VR: .357 cylinder stays open after the speed loader until a flick of the gun wrist (or the reload button)",
              Kind::Bool, 0.f, 1.f, &g_Vr357Flick, nullptr },
            { "bmvr_357_flick_speed", "VR: gun wrist turn speed that closes the .357 cylinder (deg/s, two updates in a row)",
              Kind::Float, 100.f, 2000.f, nullptr, &g_Vr357FlickSpeed },
            { "bmvr_357_tilt", "VR: .357 empties stay in the open cylinder until the gun is tipped muzzle-up",
              Kind::Bool, 0.f, 1.f, &g_Vr357Tilt, nullptr },
            { "bmvr_357_tilt_pitch", "VR: muzzle-up angle that dumps the .357 empties (deg, raw controller)",
              Kind::Float, 0.f, 85.f, nullptr, &g_Vr357TiltPitch },
            { "bmvr_357_fire_beam_hide", "VR: laser beam hidden while the .357 fire clip kicks the gun",
              Kind::Bool, 0.f, 1.f, &g_Vr357FireBeamHide, nullptr },
            { "bmvr_shell_flip", "VR test: the shotgun shell's other end in the fingers",
              Kind::Bool, 0.f, 1.f, &g_VrShellFlip, nullptr },
            { "bmvr_bolt_grab_radius", "VR: left hand within this distance of the bolt/slide grabs it (game units)",
              Kind::Float, 1.f, 15.f, nullptr, &g_VrBoltGrabRadius },
            { "bmvr_debug_vm_seq", "VR debug: hold the weapon viewmodel on this sequence index (-1 = off)",
              Kind::Float, -1.f, 255.f, nullptr, &g_DebugVmSeq },
            { "bmvr_debug_vm_cycle", "VR debug: cycle (0..1) for bmvr_debug_vm_seq",
              Kind::Float, 0.f, 1.f, nullptr, &g_DebugVmCycle },
            { "bmvr_ladder_hand_wrap", "VR: physical ladders: the glove on a rung closes round it (knuckles along the rung, overhand)",
              Kind::Bool, 0.f, 1.f, &g_LadderHandWrap, nullptr },
            { "bmvr_ladder_grip_curl", "VR: physical ladders: finger curl of a glove wrapped round a rung (0..1)",
              Kind::Float, 0.f, 1.f, nullptr, &g_LadderGripCurl },
            { "bmvr_satchel_grip_curl", "VR: left glove finger curl round the satchel bag (0..1)",
              Kind::Float, 0.f, 1.f, nullptr, &g_SatchelGripCurl },
            { "bmvr_satchel_bag_depth", "VR: satchel bag top edge this far into the left fist (units at scale 1)",
              Kind::Float, -5.f, 8.f, nullptr, &g_SatchelBagDepth },
            { "bmvr_gun_hand_cuff", "VR: gun's own right hand: the forearm folds this far behind the wrist, closing the cuff (units at scale 1, 0 = into the wrist)",
              Kind::Float, 0.f, 6.f, nullptr, &g_GunHandCuff },
            { "bmvr_tripmine_face_aim", "VR: the tripmine is placed along the mine's face (controller up) instead of the controller's forward",
              Kind::Bool, 0.f, 1.f, &g_TripmineFaceAim, nullptr },
            { "bmvr_desktop_mouse_guard", "VR: ignore desktop mouse fire and release desktop mouse buttons Windows reports held > 0.5 s",
              Kind::Bool, 0.f, 1.f, &g_DesktopMouseGuard, nullptr },
            { "bmvr_gun_hand_cuff_cap", "VR: cap the open HEV glove cuff of the gun's own hands (seen from the wrist side)",
              Kind::Bool, 0.f, 1.f, &g_GunHandCuffCap, nullptr },
            { "bmvr_pump_stroke_controller", "VR: A/B: bolt/pump stroke measured at the controller, shotgun back stroke at 0.75 (0 = before 99c2b29: grab anchor, 0.85)",
              Kind::Bool, 0.f, 1.f, &g_PumpStrokeAtController, nullptr },
            { "bmvr_pause_skip_paints", "VR: pause-menu paints dropped at the start of each pause (the first carry the flat menu background)",
              Kind::Float, 0.f, 10.f, nullptr, &g_PauseSkipPaints },
            { "bmvr_clip_hand_pose", "VR: left glove takes Black Mesa's reload-clip hand pose on a held magazine, the MP5 bolt and the glock slide",
              Kind::Bool, 0.f, 1.f, &g_VrClipHandPose, nullptr },
            { "bmvr_clip_hand_snap", "VR: while the MP5 bolt / glock slide is held, the gun's own left hand sits on it in Black Mesa's clip pose",
              Kind::Bool, 0.f, 1.f, &g_VrClipHandSnap, nullptr },
            { "bmvr_clip_hand_rpg", "VR: the RPG rocket in the hand runs through a closed left fist (along the knuckle line)",
              Kind::Bool, 0.f, 1.f, &g_VrClipHandRpg, nullptr },
            { "bmvr_clip_hand_rpg_flip", "VR: the RPG rocket in the fist with its tail out of the pinky side (off = out of the thumb side)",
              Kind::Bool, 0.f, 1.f, &g_VrClipHandRpgFlip, nullptr },
            { "bmvr_dump_hand_pose", "VR debug: with bmvr_debug_vm_seq, keep the gun's own left hand and log it relative to the magazine/bolt",
              Kind::Bool, 0.f, 1.f, &g_DebugDumpHandPose, nullptr },
            { "bmvr_rpg_rocket_fix", "VR fix: the RPG shows its rocket whenever it is loaded (Black Mesa loses it on a save load or a cut-off reload/pickup clip)",
              Kind::Bool, 0.f, 1.f, &g_RpgRocketShown, nullptr },
            { "bmvr_gun_right_hand", "VR: the weapon's own right hand holds the gun (first-person arms with the left arm and forearm hidden)",
              Kind::Bool, 0.f, 1.f, &g_VrGunRightHand, nullptr },
            { "bmvr_stock_hand_pose", "VR: Black Mesa hands: the support glove on the gun takes the gun's own left-hand finger pose; pinch on the MP5 bolt and the grenade pin",
              Kind::Bool, 0.f, 1.f, &g_VrStockHandPose, nullptr },
            { "bmvr_support_hand_lock", "VR: support hand on the gun = the gun's own stock left hand (left glove hidden; needs Gun's own right hand)",
              Kind::Bool, 0.f, 1.f, &g_VrSupportHandLock, nullptr },
            { "bmvr_hand_light_raw", "VR: A/B: VR hands lit with the raw light at the palm (no brightness floor / lift / tint)",
              Kind::Bool, 0.f, 1.f, &g_VrHandLightRaw, nullptr },
            { "bmvr_satchel_hands", "VR: satchel: bag in the left hand (left trigger throws from it), detonator in the right (right trigger detonates), left grip = Use",
              Kind::Bool, 0.f, 1.f, &g_VrSatchelHands, nullptr },
            { "bmvr_snark_throw_scale", "VR: snark hand throw speed relative to a frag throw (0.5 = half)",
              Kind::Float, 0.1f, 1.5f, nullptr, &g_SnarkThrowScale },
            { "bmvr_grenade_wrist_align", "VR: grenade's own right hand turned to the gun hands' wrist pose (needs a glock/.357/MP5 drawn once)",
              Kind::Bool, 0.f, 1.f, &g_VrGrenadeWristAlign, nullptr },
            { "bmvr_satchel_fist_fwd", "VR: satchel hands: fist centre ahead of the controller (cm)", Kind::Float, -25.f, 25.f, nullptr, &g_VrSatchelFist[0] },
            { "bmvr_satchel_fist_right", "VR: satchel hands: fist centre right of the controller (cm, mirrored for the left)", Kind::Float, -15.f, 15.f, nullptr, &g_VrSatchelFist[1] },
            { "bmvr_satchel_fist_up", "VR: satchel hands: fist centre above the controller (cm)", Kind::Float, -15.f, 15.f, nullptr, &g_VrSatchelFist[2] },
            { "bmvr_mag_discard", "VR: manual reloads: rounds left in an ejected magazine / opened .357 cylinder are lost",
              Kind::Bool, 0.f, 1.f, &g_VrMagDiscard, nullptr },
            { "bmvr_hud_health_on_gun", "VR: holding the gun with both hands, the health panel sits above the ammo panel on the gun",
              Kind::Bool, 0.f, 1.f, &g_HudHealthOnGun, nullptr },
            { "bmvr_support_hand_force", "VR test: show the gun's own left hand as if the left hand held the gun",
              Kind::Bool, 0.f, 1.f, &g_VrSupportHandForce, nullptr },
            { "bmvr_pinch_thumb", "VR: thumb curl of the left-hand pinch (MP5 bolt, glock slide, grenade pin)",
              Kind::Float, -0.5f, 1.f, nullptr, &g_VrPinchThumb },
            { "bmvr_pinch_index", "VR: index curl of the left-hand pinch",
              Kind::Float, -0.5f, 1.f, nullptr, &g_VrPinchIndex },
            { "bmvr_pinch_force", "VR test: the left glove pinches everywhere (for tuning bmvr_pinch_thumb/index)",
              Kind::Bool, 0.f, 1.f, &g_VrPinchForce, nullptr },
            { "bmvr_hands_blackmesa_size", "VR: size of the Black Mesa hands (1 = the game's first-person size)",
              Kind::Float, 0.4f, 1.5f, nullptr, &g_VrHandsBlackMesaScale },
            { "bmvr_gman_gesture", "VR: grip shows a different gesture while the G-Man is in front of you",
              Kind::Bool, 0.f, 1.f, &g_VrGmanGesture, nullptr },
            { "bmvr_gman_finger_straight", "VR: how far the G-Man gesture finger bends back past the glove's relaxed pose (0 = relaxed, slightly curved)",
              Kind::Float, 0.f, 0.5f, nullptr, &g_VrGmanFingerStraight },
            { "bmvr_gman_thumb_curl", "VR: how far the G-Man gesture thumb folds (1 = rests on the folded fingers)",
              Kind::Float, 0.f, 2.f, nullptr, &g_VrGmanThumbCurl },
            { "bmvr_gman_thumb_swing", "VR: sideways swing of the G-Man gesture thumb at its root, radians (0 = plain curl, goes through the fingers)",
              Kind::Float, 0.f, 1.5f, nullptr, &g_VrGmanThumbSwing },
            { "bmvr_gman_finger_twist", "VR: G-Man gesture middle finger turned about its own length, degrees",
              Kind::Float, -30.f, 30.f, nullptr, &g_VrGmanFingerTwistDeg },
            { "bmvr_gman_gesture_force", "VR test: grip shows the G-Man gesture everywhere (for tuning bmvr_gman_finger_straight)",
              Kind::Bool, 0.f, 1.f, &g_VrGmanGestureForce, nullptr },
            { "bmvr_snap_turn", "VR: snap turning (0 = smooth)",
              Kind::Bool, 0.f, 1.f, &g_SnapTurning, nullptr },
            { "bmvr_snap_angle", "VR: snap turn angle in degrees",
              Kind::Float, 15.f, 90.f, nullptr, &g_SnapTurnAngle },
            { "bmvr_turn_speed", "VR: smooth turn speed",
              Kind::Float, 0.05f, 1.f, nullptr, &g_TurnSpeed },
            { "bmvr_recenter_on_resume", "VR: closing the pause menu recenters the head position (not the turn direction)",
              Kind::Bool, 0.f, 1.f, &g_AutoRecenterOnResume, nullptr },
            { "bmvr_aim_marker", "VR: aim marker (0 = off, 1 = red dot on guns without sights, 2 = red dot, 3 = red dot + laser beam)",
              Kind::Float, 0.f, 3.f, nullptr, &g_AimMarker },
            { "bmvr_aim_dot", "VR: aim marker as a red laser dot (0 = amber crosshair)",
              Kind::Bool, 0.f, 1.f, &g_VrCrosshairDot, nullptr },
            { "bmvr_skip_draw_anim", "VR: guns appear in the hand in their idle pose (no draw/holster/pickup animation)",
              Kind::Bool, 0.f, 1.f, &g_SkipDrawAnim, nullptr },
            { "bmvr_quick_draw_sec", "VR: after a weapon switch the gun can fire after this many seconds (the game's own draw wait is ~0.5 s; -1 = game default)",
              Kind::Float, -1.f, 1.f, nullptr, &g_QuickDrawSec },
            { "bmvr_throw_stroke", "VR: frag throw speed and direction from the whole throwing stroke, far point of the hold to the release (0 = strongest ~66 ms fit, default)",
              Kind::Bool, 0.f, 1.f, &g_ThrowStroke, nullptr },
            { "bmvr_delta_seq_idle", "VR: tau/crossbow show idle instead of the sideways reference pose after a fast weapon-menu switch",
              Kind::Bool, 0.f, 1.f, &g_DeltaSeqIdle, nullptr },
            { "bmvr_crowbar_hit_drawn_bar", "VR: crowbar hits along the drawn crowbar bar (0 = the old HL2VR grip frame)",
              Kind::Bool, 0.f, 1.f, &g_CrowbarHitDrawnBar, nullptr },
            { "bmvr_no_lower_at_friendly", "VR: aiming at a friendly does not lower the gun, one- or two-handed (laser stays on)",
              Kind::Bool, 0.f, 1.f, &g_NoLowerAtFriendly, nullptr },
            { "bmvr_scope_clip_idle", "VR: the crossbow stays in the hand while the scope opens/closes (no to-scope animation)",
              Kind::Bool, 0.f, 1.f, &g_ScopeClipIdle, nullptr },
            { "bmvr_mute_fidget_sound", "VR: weapon fidget clips play no sound (the gun does not fidget in VR)",
              Kind::Bool, 0.f, 1.f, &g_MuteFidgetSounds, nullptr },
            { "bmvr_laser_width", "VR: laser beam half width in game units (0.06 = ~3 mm beam)",
              Kind::Float, 0.01f, 0.5f, nullptr, &g_VrLaserWidth },
            { "bmvr_flare_shared", "VR: both eyes use one flare layout, from the point between the eyes",
              Kind::Bool, 0.f, 1.f, &g_FlareSharedLayout, nullptr },
            { "bmvr_flare_fade_angle", "VR: degrees off the view axis where a flare has faded out (0 = no fade)",
              Kind::Float, 0.f, 90.f, nullptr, &g_FlareFadeAngle },
            { "bmvr_flare_light", "VR: strength of the flare layers on the light (0 = off, 1 = stock)",
              Kind::Float, 0.f, 1.f, nullptr, &g_FlareLightScale },
            { "bmvr_flare_ghosts", "VR: strength of the flare layers placed away from the light (0 = off, 1 = stock)",
              Kind::Float, 0.f, 1.f, nullptr, &g_FlareGhostScale },
            { "bmvr_flare_haze", "VR: strength of the large square flare layers placed away from the light (0 = off, 1 = stock)",
              Kind::Float, 0.f, 1.f, nullptr, &g_FlareHazeScale },
            { "bmvr_flare_haze_size", "VR: a square flare layer away from the light this wide, as a fraction of the eye height, counts as haze",
              Kind::Float, 0.05f, 4.f, nullptr, &g_FlareHazeMinSize },
            { "bmvr_sky_water_per_eye", "VR: water drawn before an eye's own reflection (3D skybox) gets that eye's previous reflection/refraction, not the other eye's",
              Kind::Bool, 0.f, 1.f, &g_SkyWaterPerEye, nullptr },
            { "bmvr_eye_underwater_world", "VR: the G-buffer also gets the world under the water surface, as at nr_lights_quality 3 (0 = stock at 2 and below: clear patches in the water)",
              Kind::Bool, 0.f, 1.f, &g_EyeUnderwaterWorld, nullptr },
            { "bmvr_xen_fog_off", "VR: Black Mesa's global Xen ground fog is not drawn (default 0: drawn, noise per Xen fog noise mode); fog volumes stay",
              Kind::Bool, 0.f, 1.f, &g_XenGlobalFogOff, nullptr },
            { "bmvr_waterline_clip_fix", "VR: eye at the water line: the eye view is not clipped at the water surface, so the part of the view under the water is lit (0 = stock: black lower part)",
              Kind::Bool, 0.f, 1.f, &g_WaterLineClipFix, nullptr },
            { "bmvr_screenspace_model_fix", "VR: screen-space quads (underwater warp, post passes, lens flares) are drawn with an identity model matrix, whatever an earlier draw left (0 = stock: inherit it)",
              Kind::Bool, 0.f, 1.f, &g_ScreenSpaceModelFix, nullptr },
            { "bmvr_world_model_fix", "VR: when the leftover (+1,+1,0) model matrix is seen on uploads (world ~2.5 cm off doors/props), load an identity model once (0 = leave it)",
              Kind::Bool, 0.f, 1.f, &g_WorldModelFix, nullptr },
            { "bmvr_debug_model_reset", "VR debug: load an identity MODEL matrix before each eye pass (repairs world brushes drawn with a leftover (+1,+1,0) model offset)",
              Kind::Bool, 0.f, 1.f, &g_DebugModelReset, nullptr },
            { "bmvr_light_cull_eye", "VR: deferred lights are culled from each eye's view, not the main view that follows the aim",
              Kind::Bool, 0.f, 1.f, &g_LightCullEyeView, nullptr },
            { "bmvr_flare_level", "VR: lens flare streaks stay level with the horizon when the head rolls (0 = stock: they roll with the head)",
              Kind::Bool, 0.f, 1.f, &g_FlareLevel, nullptr },
            { "bmvr_sprite_level", "VR: light glows and other view-facing sprites face each eye and stay level (0 = stock: they roll and turn with the head)",
              Kind::Bool, 0.f, 1.f, &g_SpriteLevel, nullptr },
            { "bmvr_particle_level", "VR: smoke/fog particles stay upright when the head rolls (0 = stock: they roll with the head)",
              Kind::Bool, 0.f, 1.f, &g_ParticleLevel, nullptr },
            { "bmvr_vm_fx_catchup", "VR: muzzle flashes follow the gun while moving (0 = old: they trail behind when strafing)",
              Kind::Bool, 0.f, 1.f, &g_ViewmodelFxCatchUp, nullptr },
            { "bmvr_flare_eye_axis", "VR: lens flares lay out around each eye's view axis instead of the image centre",
              Kind::Bool, 0.f, 1.f, &g_FlareEyeAxis, nullptr },
            { "bmvr_pixelvis_per_eye", "VR: each eye keeps its own glow visibility queries (0 = stock: the eyes share one, glows can drop out)",
              Kind::Bool, 0.f, 1.f, &g_PixelVisPerEye, nullptr },
            { "bmvr_glow_count_depth_fix", "VR: glow visibility counts the full proxy (0 = glows show at full brightness through objects when a sliver peeks out)",
              Kind::Bool, 0.f, 1.f, &g_GlowCountDepthFix, nullptr },
            { "bmvr_mc_hand_snapshot", "VR (MulticoreMode): hands and laser of both eyes come from one snapshot per frame (0 = live state, can differ per eye)",
              Kind::Bool, 0.f, 1.f, &g_McHandSnapshot, nullptr },
            { "bmvr_godray_radial_mask", "VR: god ray mask tested as if the head looked level toward the light (0 = stock depth along the view axis: rays come and go as the head pitches)",
              Kind::Bool, 0.f, 1.f, &g_GodRayRadialMask, nullptr },
            { "bmvr_godray_samples", "VR test: god ray taps 0 = game's (64), 1 = 32, 2 = 16; only while in a level, the game's value is put back on level shutdown",
              Kind::Float, 0.f, 2.f, nullptr, &g_GodRaySamples },
            { "bmvr_godray_blur_scale", "VR: god ray blur size: 0 full (stock), 1 half, 2 quarter (default)",
              Kind::Float, 0.f, 2.f, nullptr, &g_GodRayBlurScale },
            { "bmvr_godray_log", "VR debug: log the next N god ray blur/mask draws (eye, light index, screen position, mask result)",
              Kind::Float, 0.f, 20000.f, nullptr, &g_GodRayLog },
            { "bmvr_xen_fog_noise", "VR: Xen fog noise: 0 stock (each eye's view direction, at infinity in stereo), 1 from the centre eye (flat look, on the surfaces), 2 fixed to the world (bmvr_xen_fog_noise_size), 3 auto (world near, centre eye far)",
              Kind::Float, 0.f, 3.f, nullptr, &g_XenFogNoiseMode },
            { "bmvr_xen_fog_world_near", "VR: Xen fog noise auto mode: fixed to the world up to this distance from the eye, units",
              Kind::Float, 0.f, 8192.f, nullptr, &g_XenFogWorldNear },
            { "bmvr_xen_fog_world_far", "VR: Xen fog noise auto mode: centre-eye direction from this distance on, units",
              Kind::Float, 16.f, 16384.f, nullptr, &g_XenFogWorldFar },
            { "bmvr_xen_fog_noise_size", "VR: Xen global fog noise size, units per texture repeat",
              Kind::Float, 16.f, 4096.f, nullptr, &g_XenFogNoiseSize },
            { "bmvr_sync_probe", "VR test: the next N eye draws log SyncProbe lines (eye, HUD hand, gun wrist, cuff cap, laser start)",
              Kind::Float, 0.f, 20000.f, nullptr, &g_SyncProbeEyes },
            { "bmvr_viewproj_normal_fix", "VR: ropes get the same normals in both off-centre eyes (0 = stock: one eye's ropes can sit in their own shadow)",
              Kind::Bool, 0.f, 1.f, &g_ViewProjNormalFix, nullptr },
            { "bmvr_eye_bloom", "VR: bloom + auto-exposure on the eyes (0 = skipped on the eyes)",
              Kind::Bool, 0.f, 1.f, &g_EyeBloom, nullptr },
            { "bmvr_fxaa_apply", "VR: send the bmvr_fxaa_* values to FXAA (0 = stock: the game sends all 0, mat_fxaa_* have no effect)",
              Kind::Bool, 0.f, 1.f, &g_FxaaApply, nullptr },
            { "bmvr_fxaa_subpixel", "VR: FXAA sub-pixel smoothing (0 = off, 0.25 = mod default, 0.75 = FXAA default, 1 = softest)",
              Kind::Float, 0.f, 1.f, nullptr, &g_FxaaSubpixel },
            { "bmvr_fxaa_edge", "VR: FXAA edge contrast needed to smooth (0.063 = most edges, 0.166 = FXAA default, 0.333 = fewest)",
              Kind::Float, 0.f, 0.5f, nullptr, &g_FxaaEdgeThreshold },
            { "bmvr_fxaa_edge_min", "VR: FXAA skips darks below this contrast (0.0312 = treats most, 0.0833 = FXAA default)",
              Kind::Float, 0.f, 0.1f, nullptr, &g_FxaaEdgeThresholdMin },
            { "bmvr_follow_scripted_camera", "VR: cutscene cameras (drag scene, tram) turn the view; the head turns relative to them (0 = head only)",
              Kind::Bool, 0.f, 1.f, &g_FollowScriptedCamera, nullptr },
            { "bmvr_glow_hold", "VR: seconds a dimming light glow keeps its brightness before it fades (0 = off; stops flicker behind dirty glass / at the view edge)",
              Kind::Float, 0.f, 2.f, nullptr, &g_GlowHold },
            { "bmvr_glow_smooth", "VR: light glow brightness averaged over both eyes and smoothed over this many seconds (0 = off, per-eye raw)",
              Kind::Float, 0.f, 0.5f, nullptr, &g_GlowSmooth },
            { "bmvr_glow_hide_ms", "VR: a glow hidden this many ms in an eye goes dark at once despite smoothing (no glow through walls)",
              Kind::Float, 0.f, 500.f, nullptr, &g_GlowHideMs },
            { "bmvr_loading_card", "VR: LOADING card on the eyes during in-game loads (0 = old frozen world; test switch)",
              Kind::Bool, 0.f, 1.f, &g_LoadingCard, nullptr },
            { "bmvr_post_copy_swap", "VR: post-process eye copies swap images instead of copying (same image, less GPU work; 0 = stock copies)",
              Kind::Bool, 0.f, 1.f, &g_PostCopySwap, nullptr },
            { "bmvr_shadows_once", "VR: light shadow maps rendered once per frame, the second eye reuses them (same image, less CPU/GPU; 0 = per eye)",
              Kind::Bool, 0.f, 1.f, &g_ShadowsOncePerFrame, nullptr },
            { "bmvr_shadows_once_test", "VR test: second eye skips every light shadow render, matched or not (negative control for bmvr_shadows_once)",
              Kind::Bool, 0.f, 1.f, &g_ShadowsOnceTestSkipAll, nullptr },
            { "bmvr_cpu_sample", "VR test: sample the main and render thread's CPU for this many seconds and log where it goes",
              Kind::Float, 0.f, 120.f, nullptr, &g_CpuSampleSeconds },
            { "bmvr_post_copy_swap_alpha", "VR test: post copy swap also on RGB-only passes (1); 2/3 = and clear the owed eye to alpha 0/1 (shows any use of that alpha)",
              Kind::Float, 0.f, 3.f, nullptr, &g_PostCopySwapAlpha },
            { "bmvr_foveated", "VR: foveated rendering, coarser shading away from the centre (0 off, 1 fixed lens centre, 2 eye-tracked; needs Foveated 1/2 at game start)",
              Kind::Float, 0.f, 2.f, nullptr, &g_Foveated },
            { "bmvr_foveated_gaze_step", "VR: eye-tracked foveation moves its centre only after the gaze moved this far (tangent units, 0.02 = ~1 deg)",
              Kind::Float, 0.f, 0.2f, nullptr, &g_FoveatedGazeStep },
            { "bmvr_foveated_inner", "VR: foveated full-rate radius (fraction of half the eye height on the PS VR2; same angle on other headsets)",
              Kind::Float, 0.05f, 2.f, nullptr, &g_FoveatedInner },
            { "bmvr_foveated_mid", "VR: foveated half-rate (2x1/1x2) ring outer radius",
              Kind::Float, 0.05f, 3.f, nullptr, &g_FoveatedMid },
            { "bmvr_foveated_outer", "VR: foveated 2x2 ring outer radius (4x4 beyond, where supported)",
              Kind::Float, 0.05f, 4.f, nullptr, &g_FoveatedOuter },
            { "bmvr_particles_last", "VR: particles drawn after the rest of the translucent pass also without MSAA (splash over water; with MSAA the 1x switch already does it)",
              Kind::Bool, 0.f, 1.f, &g_ParticlesLast, nullptr },
            { "bmvr_title_level", "VR: intro logo / chapter titles level in the room at the head's yaw (0 = head-locked)",
              Kind::Bool, 0.f, 1.f, &g_TitleLevel, nullptr },
            { "bmvr_title_glow", "VR: intro logo / credits / chapter titles with flat Black Mesa's HUD bloom + scanlines",
              Kind::Bool, 0.f, 1.f, &g_TitleGlow, nullptr },
            { "bmvr_boot_hud_inset", "VR: HEV boot-up HUD: left text / suit figure moved this many pixels (1920-wide screen) toward the centre",
              Kind::Float, 0.f, 500.f, nullptr, &g_BootHudInset },
            { "bmvr_boot_hud_scale", "VR: HEV boot-up HUD layer width (1 = pause menu / title width)",
              Kind::Float, 0.5f, 2.f, nullptr, &g_BootHudScale },
            { "bmvr_glow_proxy_scale", "VR test: glow occlusion test area this many times wider (1 = stock): finer visibility steps, less flicker",
              Kind::Float, 1.f, 8.f, nullptr, &g_GlowProxyScale },
            { "bmvr_rpg_ring_scale", "VR: size of the spinning red lines around the RPG laser point (1 = flat HUD size)",
              Kind::Float, 0.1f, 3.f, nullptr, &g_VrRpgRingScale },
            { "bmvr_pose_hold_deg", "VR test: keep the previous head pose until the head turns more than this many degrees (0 = off)",
              Kind::Float, 0.f, 1.f, nullptr, &g_PoseHoldDeg },
            { "bmvr_pose_hold_mm", "VR test: keep the previous head pose until the head moves more than this many mm (0 = off)",
              Kind::Float, 0.f, 20.f, nullptr, &g_PoseHoldMm },
            { "bmvr_hide_xog", "VR debug: skip Xen fog (Xog) draws: 1 = global fog, 2 = fog volumes, 3 = both",
              Kind::Float, 0.f, 3.f, nullptr, &g_DebugHideXog },
            { "bmvr_flare_log", "VR debug: log lens flare screen positions + visibility per eye (each switch-on starts fresh line caps)",
              Kind::Bool, 0.f, 1.f, &g_FlareLog, nullptr },
            { "bmvr_freeze_head", "VR debug: the game view keeps the current head pose (SteamVR still reprojects it to the real head); hands keep tracking",
              Kind::Bool, 0.f, 1.f, &g_FreezeHead, nullptr },
            { "bmvr_glow_trace", "VR debug: record every light glow's brightness + frame timing for N seconds into VR/glowtrace-*.csv",
              Kind::Float, 0.f, 120.f, nullptr, &g_GlowTraceSeconds },
            { "bmvr_menu_smooth", "VR: menu pointer smoothing in seconds (lower = more direct)",
              Kind::Float, 0.02f, 0.6f, nullptr, &g_MenuCursorSmoothSec },
        };

        enum class Reg { NotYet, Done, Failed };
        bool g_Registered = false;
        bool g_Gave_up = false;
        int g_Attempts = 0;

        template <typename T>
        T& Field(void* obj, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + off); }

        float CurrentDefault(const VrCvar& c)
        {
            if (c.kind == Kind::Bool)
                return (c.boolTarget && *c.boolTarget) ? 1.f : 0.f;
            return c.floatTarget ? *c.floatTarget : 0.f;
        }

        void FormatValue(const VrCvar& c, float v, char* out, int cap)
        {
            if (c.kind == Kind::Bool)
                _snprintf(out, cap, "%d", v != 0.f ? 1 : 0);
            else
                _snprintf(out, cap, "%g", v);
            out[cap - 1] = 0;
        }

        // Same representation tier1's InternalSetValue leaves behind.
        void WriteValue(VrCvar& c, float v)
        {
            if (v < c.minVal)
                v = c.minVal;
            if (v > c.maxVal)
                v = c.maxVal;
            void* o = c.obj;
            const uint32_t key = reinterpret_cast<uint32_t>(o);
            union { float f; uint32_t u; } fu;
            fu.f = v;
            Field<uint32_t>(o, 0x2C) = fu.u ^ key;
            Field<uint32_t>(o, 0x30) = static_cast<uint32_t>(static_cast<int>(v)) ^ key;
            FormatValue(c, v, c.str, kStrCap);
        }

        float ReadValue(const VrCvar& c)
        {
            const void* o = c.obj;
            const uint32_t key = reinterpret_cast<uint32_t>(o);
            union { float f; uint32_t u; } fu;
            fu.u = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(o) + 0x2C) ^ key;
            return fu.f;
        }

        bool CallGetNameAt(void* obj, int slot, const char*& out)
        {
            out = nullptr;
            BMVR_TRY
            {
                void** vt = *reinterpret_cast<void***>(obj);
                using tGetName = const char*(__thiscall*)(void*);
                out = reinterpret_cast<tGetName>(vt[slot])(obj);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return true;
        }

        bool ReportsName(void* obj, const char* name)
        {
            for (int slot : kGetNameSlots)
            {
                const char* got = nullptr;
                if (CallGetNameAt(obj, slot, got) && got == name)
                    return true;
            }
            return false;
        }

        bool CallRegister(void* icvar, void* obj)
        {
            BMVR_TRY
            {
                void** vt = *reinterpret_cast<void***>(icvar);
                using tRegister = void(__thiscall*)(void*, void*);
                reinterpret_cast<tRegister>(vt[kICvarRegisterConCommand])(icvar, obj);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return true;
        }

        bool CopyTemplate(const void* tmpl, void* dst)
        {
            BMVR_TRY
            {
                std::memcpy(dst, tmpl, kCloneBytes);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return true;
        }

        void* FindTemplate(Game* game)
        {
            // Plain archived client.dll cvars (no callbacks, no material-thread flag).
            for (const char* name : { "cl_auto_crouch_jump", "cl_ironsight_sticky", "hud_fastswitch" })
            {
                void* c = game->FindConVar(name);
                if (!c)
                    continue;
                void* parent = nullptr;
                BMVR_TRY { parent = Field<void*>(c, 0x1C); }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (parent != c)
                    continue;
                Game::logMsg("VR cvars: template '%s' at %p", name, c);
                return c;
            }
            return nullptr;
        }

        Reg RegisterAll(Game* game)
        {
            void* icvar = game->m_Cvar;
            void* tmpl = icvar ? FindTemplate(game) : nullptr;
            if (!tmpl)
                return Reg::NotYet;
            int ok = 0;
            for (VrCvar& c : g_Cvars)
            {
                if (game->FindConVar(c.name))
                {
                    Game::logMsg("VR cvars: %s already exists, skipped", c.name);
                    continue;
                }
                std::memset(c.obj, 0, sizeof(c.obj));
                if (!CopyTemplate(tmpl, c.obj))
                    return Reg::Failed;
                void* o = c.obj;
                Field<void*>(o, 0x04) = nullptr;                 // m_pNext
                Field<uint32_t>(o, 0x08) = 0;                    // m_bRegistered (+pad)
                Field<const char*>(o, 0x0C) = c.name;
                Field<const char*>(o, 0x10) = c.help;
                // Not archived: VR/config.txt (the VR settings panel) is the one
                // place these are stored. Archived, config.cfg overrode the panel
                // at every start and kept stale values (2026-10-27).
                Field<int>(o, 0x14) = 0;
                Field<void*>(o, 0x1C) = o;                       // m_pParent
                FormatValue(c, CurrentDefault(c), c.def, kStrCap);
                Field<const char*>(o, 0x20) = c.def;
                Field<char*>(o, 0x24) = c.str;
                Field<int>(o, 0x28) = kStrCap;
                std::memset(static_cast<uint8_t*>(o) + 0x34, 0, kCloneBytes - 0x34); // min/max off, no callbacks
                WriteValue(c, CurrentDefault(c));

                if (!ReportsName(o, c.name))
                {
                    Game::logMsg("VR cvars: clone of the template does not report its name - layout differs, giving up");
                    return Reg::Failed;
                }
                if (!CallRegister(icvar, o) || game->FindConVar(c.name) != o)
                {
                    Game::logMsg("VR cvars: RegisterConCommand failed for %s, giving up", c.name);
                    return Reg::Failed;
                }
                ++ok;
            }
            Game::logMsg("VR cvars: registered %d", ok);
            return Reg::Done;
        }
    }

    void TickVrCvars(Game* game)
    {
        if (!game || g_Gave_up)
            return;
        if (!g_Registered)
        {
            if (++g_Attempts > 600)
            {
                g_Gave_up = true;
                Game::logMsg("VR cvars: no usable template cvar, menu/console settings off (VR/config.txt still applies)");
                return;
            }
            const Reg r = RegisterAll(game);
            if (r == Reg::Failed)
            {
                g_Gave_up = true;
                return;
            }
            if (r == Reg::NotYet)
            {
                if (g_Attempts == 1 || g_Attempts % 120 == 0)
                    Game::logMsg("VR cvars: not registered yet (attempt %d)", g_Attempts);
                return;
            }
            g_Registered = true;
            for (VrCvar& c : g_Cvars)
            {
                WriteValue(c, CurrentDefault(c));
                c.lastSeen = ReadValue(c);
            }
        }
        // A console change writes its flag; a panel change (flag moved, cvar
        // still at the last seen value) is mirrored into the cvar.
        for (VrCvar& c : g_Cvars)
        {
            float v = ReadValue(c);
            if (v == c.lastSeen)
            {
                const float flag = CurrentDefault(c);
                if (flag != v)
                {
                    WriteValue(c, flag);
                    c.lastSeen = ReadValue(c);
                }
                continue;
            }
            Game::logMsg("VR cvar %s: console value %g copied into the flag (was %g)", c.name, v, CurrentDefault(c));
            c.lastSeen = v;
            if (!(v >= c.minVal))
                v = c.minVal;
            if (!(v <= c.maxVal))
                v = c.maxVal;
            if (c.kind == Kind::Bool && c.boolTarget)
                *c.boolTarget = v != 0.f;
            else if (c.floatTarget)
                *c.floatTarget = v;
        }
    }

    const char* VrCvarNameFor(const void* target)
    {
        if (!target)
            return nullptr;
        for (const VrCvar& c : g_Cvars)
            if (static_cast<const void*>(c.boolTarget) == target || static_cast<const void*>(c.floatTarget) == target)
                return c.name;
        return nullptr;
    }
}
