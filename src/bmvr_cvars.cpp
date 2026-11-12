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
            { "bmvr_crouch_hold", "VR: right stick down crouches only while held (0 = each push toggles)",
              Kind::Bool, 0.f, 1.f, &g_CrouchStickHold, nullptr },
            { "bmvr_stick_jump_crouch", "VR: how far the right stick goes up/down before it jumps/crouches (0.3..0.95)",
              Kind::Float, 0.3f, 0.95f, nullptr, &g_StickJumpCrouchThreshold },
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
              Kind::Float, -90.f, 90.f, nullptr, &g_HandHudRoll },
            { "bmvr_hand_roll", "VR: hand models rolled thumb-up around the controller, degrees (90 = palms facing each other; not while holding a weapon)",
              Kind::Float, -135.f, 135.f, nullptr, &g_VrHandsRollDeg },
            { "bmvr_gman_gesture", "VR: grip shows a different gesture while the G-Man is in front of you",
              Kind::Bool, 0.f, 1.f, &g_VrGmanGesture, nullptr },
            { "bmvr_gman_finger_straight", "VR: how far the G-Man gesture finger bends back past the glove's relaxed pose (0 = relaxed, slightly curved)",
              Kind::Float, 0.f, 0.5f, nullptr, &g_VrGmanFingerStraight },
            { "bmvr_gman_thumb_curl", "VR: how far the G-Man gesture thumb folds (1 = rests on the folded fingers)",
              Kind::Float, 0.f, 2.f, nullptr, &g_VrGmanThumbCurl },
            { "bmvr_gman_thumb_swing", "VR: sideways swing of the G-Man gesture thumb at its root, radians (0 = plain curl, goes through the fingers)",
              Kind::Float, 0.f, 1.5f, nullptr, &g_VrGmanThumbSwing },
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
            { "bmvr_aim_marker", "VR: mark where the gun points",
              Kind::Bool, 0.f, 1.f, &g_VrCrosshair, nullptr },
            { "bmvr_aim_dot", "VR: aim marker as a red laser dot (0 = amber crosshair)",
              Kind::Bool, 0.f, 1.f, &g_VrCrosshairDot, nullptr },
            { "bmvr_skip_draw_anim", "VR: guns appear in the hand in their idle pose (no draw/holster/pickup animation)",
              Kind::Bool, 0.f, 1.f, &g_SkipDrawAnim, nullptr },
            { "bmvr_throw_stroke", "VR: frag throw speed and direction from the whole throwing stroke, far point of the hold to the release (0 = strongest ~66 ms fit, default)",
              Kind::Bool, 0.f, 1.f, &g_ThrowStroke, nullptr },
            { "bmvr_delta_seq_idle", "VR: tau/crossbow show idle instead of the sideways reference pose after a fast weapon-menu switch",
              Kind::Bool, 0.f, 1.f, &g_DeltaSeqIdle, nullptr },
            { "bmvr_scope_clip_idle", "VR: the crossbow stays in the hand while the scope opens/closes (no to-scope animation)",
              Kind::Bool, 0.f, 1.f, &g_ScopeClipIdle, nullptr },
            { "bmvr_mute_fidget_sound", "VR: weapon fidget clips play no sound (the gun does not fidget in VR)",
              Kind::Bool, 0.f, 1.f, &g_MuteFidgetSounds, nullptr },
            { "bmvr_laser_beam", "VR: red laser beam from the gun to where it points",
              Kind::Bool, 0.f, 1.f, &g_VrLaserBeam, nullptr },
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
            { "bmvr_glow_proxy_scale", "VR test: glow occlusion test area this many times wider (1 = stock): finer visibility steps, less flicker",
              Kind::Float, 1.f, 8.f, nullptr, &g_GlowProxyScale },
            { "bmvr_pose_hold_deg", "VR test: keep the previous head pose until the head turns more than this many degrees (0 = off)",
              Kind::Float, 0.f, 1.f, nullptr, &g_PoseHoldDeg },
            { "bmvr_pose_hold_mm", "VR test: keep the previous head pose until the head moves more than this many mm (0 = off)",
              Kind::Float, 0.f, 20.f, nullptr, &g_PoseHoldMm },
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
}
