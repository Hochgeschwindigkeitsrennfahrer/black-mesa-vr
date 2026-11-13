// In-VR settings panel (docs/plans/bmvr-vr-settings-panel.md in the notes repo).
//
// Opened from the weapon menu's gear slot. Drawn by the mod in the per-eye hand
// overlay pass like the weapon menu, so it needs nothing from Black Mesa's UI and
// behaves the same on every game build. Text is rendered with GDI into a 32-bit
// DIB and uploaded to a D3D9 texture only when something changes. Values apply
// live. On close, settings that differ from the built-in default are written to
// VR/config.txt and ones back at the default are removed from it, so a later
// default change still reaches players who never changed that setting.
//
// Threads: UpdateSettingsPanel (ProcessInput) and DrawSettingsPanel (hand
// overlay) both run on the render thread; the game thread only reads the flags.

#include "vr.h"
#include "game.h"
#include "bmvr_flags.h"
#include "d3d9_vr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

namespace
{
    constexpr float kPi = 3.14159265f;
    // Two columns of kColW with a divider: one tall column had grown to
    // ~1.2 m and was pushed far back to fit the view.
    constexpr int kColW = 1024;
    constexpr int kTexW = 2 * kColW;
    constexpr int kHeaderPx = 96;
    constexpr int kRowPx = 64;
    constexpr int kSectionPx = 48;
    constexpr int kTabPx = 72;          // tab bar under the title
    constexpr int kTabGapPx = 16;
    constexpr int kContentTop = kHeaderPx + kTabPx + 24;
    constexpr float kPanelWidthHu = 40.f;      // ~1 m: two ~51 cm columns, same text size as before
    constexpr float kPanelMinDistanceHu = 44.f;  // ~1.12 m in front of the eyes, near the game menu
    constexpr float kPanelHalfFovDeg = 22.f;      // pushed back until the panel is about +-22 deg tall
    constexpr float kPanelDropHu = 3.f;        // a little below eye height
    // Zones in texture pixels.
    constexpr int kMinusX0 = 600, kMinusX1 = 690;
    constexpr int kValueX0 = 690, kValueX1 = 880;
    constexpr int kPlusX0 = 880, kPlusX1 = 970;
    constexpr int kPillX0 = 790, kPillX1 = 970;

    // Panel copies of engine cvars (rows with a "cvar:" key).
    bool g_EngSvCheats = false;
    bool g_EngOcclusion = true;
    bool g_EngPixelVis = true;
    bool g_Eng3dSky = true;
    float g_EngRopeMinWidth = 0.3f;
    std::mutex g_EngCmdLock;
    std::vector<std::string> g_EngCmds;   // run by FlushPanelEngineCommands (game thread)

    bool IsEngineCvarKey(const char* key) { return key && std::strncmp(key, "cvar:", 5) == 0; }
    // Toggle console commands (god, noclip, notarget): "cmd:" rows send the
    // command on every click and are never saved. Noclip is read back from
    // the player's move type; god/notarget live only in server flags, so
    // their rows show what the panel last set this run.
    bool g_DevGod = false;
    bool g_DevNoclip = false;
    bool g_DevNotarget = false;
    bool IsEngineCmdKey(const char* key) { return key && std::strncmp(key, "cmd:", 4) == 0; }
    bool IsEngineKey(const char* key) { return IsEngineCvarKey(key) || IsEngineCmdKey(key); }

    enum class RowKind { Tab, Section, Toggle, Number, Button, Close };

    struct Row
    {
        const char* label;
        RowKind kind;
        const char* key;      // VR/config.txt key
        bool* b;
        float* f;
        float step, lo, hi;
        const char* fmt;      // number display
        bool defB = false;    // built-in default (CapturePanelDefaults)
        float defF = 0.f;
    };
    bool g_DefaultsCaptured = false;

    std::vector<Row>& Rows()
    {
        // A Tab row starts a tab; its rows follow until the next Tab row.
        static std::vector<Row> rows = {
            { "INTERACTION", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "AIMING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Aim marker", RowKind::Toggle, "VrCrosshair", &bmvr::g_VrCrosshair, nullptr, 0, 0, 0, nullptr },
            { "Marker as red dot", RowKind::Toggle, "VrCrosshairDot", &bmvr::g_VrCrosshairDot, nullptr, 0, 0, 0, nullptr },
            { "Marker size", RowKind::Number, "VrCrosshairScale", nullptr, &bmvr::g_VrCrosshairScale, 0.1f, 0.2f, 3.f, "%.1f" },
            { "Laser beam", RowKind::Toggle, "VrLaserBeam", &bmvr::g_VrLaserBeam, nullptr, 0, 0, 0, nullptr },
            { "Laser width", RowKind::Number, "VrLaserWidth", nullptr, &bmvr::g_VrLaserWidth, 0.01f, 0.01f, 0.3f, "%.2f" },
            { "Gun angle", RowKind::Number, "GunPitchOffset", nullptr, &bmvr::g_GunPitchOffset, 1.f, -45.f, 45.f, "%+.0f\xB0" },
            { "Two-hand grip toggles", RowKind::Toggle, "TwoHandGripToggle", &bmvr::g_TwoHandGripToggle, nullptr, 0, 0, 0, nullptr },
            { "FLASHLIGHT", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Flashlight in left hand", RowKind::Toggle, "FlashlightLeftHand", &bmvr::g_FlashlightLeftHand, nullptr, 0, 0, 0, nullptr },
            { "Left square toggles light", RowKind::Toggle, "FlashlightOnSquare", &bmvr::g_FlashlightOnSquare, nullptr, 0, 0, 0, nullptr },
            { "CROSSBOW SCOPE", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Zoom follows the gun", RowKind::Toggle, "ScopeFollowsGun", &bmvr::g_ScopeFollowsGun, nullptr, 0, 0, 0, nullptr },
            { "Raise to face zooms (2 hands)", RowKind::Toggle, "ScopeRaiseToZoom", &bmvr::g_ScopeRaiseToZoom, nullptr, 0, 0, 0, nullptr },
            { "Raise distance", RowKind::Number, "ScopeRaiseMeters", nullptr, &bmvr::g_ScopeRaiseMeters, 0.02f, 0.1f, 0.6f, "%.2f m" },
            { "Zoom smoothing", RowKind::Number, "ScopeGunSmoothSec", nullptr, &bmvr::g_ScopeGunSmoothSec, 0.02f, 0.f, 0.5f, "%.2f s" },
            { "MOUNTED GUNS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Turn per hand move", RowKind::Number, "MountedGunGain", nullptr, &bmvr::g_MountedGunGain, 0.25f, 0.5f, 4.f, "%.2fx" },
            { "GRENADES", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Throw at hand speed", RowKind::Toggle, "VrHandThrow", &bmvr::g_VrHandThrow, nullptr, 0, 0, 0, nullptr },
            { "Throw strength", RowKind::Number, "ThrowScale", nullptr, &bmvr::g_ThrowScale, 0.25f, 1.f, 8.f, "%.2f" },
            { "Throw from whole stroke (test)", RowKind::Toggle, "ThrowStroke", &bmvr::g_ThrowStroke, nullptr, 0, 0, 0, nullptr },
            { "TURNING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Snap turning", RowKind::Toggle, "SnapTurning", &bmvr::g_SnapTurning, nullptr, 0, 0, 0, nullptr },
            { "Snap angle", RowKind::Number, "SnapTurnAngle", nullptr, &bmvr::g_SnapTurnAngle, 15.f, 15.f, 90.f, "%.0f\xB0" },
            { "Smooth turn speed", RowKind::Number, "TurnSpeed", nullptr, &bmvr::g_TurnSpeed, 0.05f, 0.05f, 1.f, "%.2f" },
            { "MOVEMENT", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Crouch only while held", RowKind::Toggle, "CrouchStickHold", &bmvr::g_CrouchStickHold, nullptr, 0, 0, 0, nullptr },
            { "Jump/crouch stick distance", RowKind::Number, "StickJumpCrouchThreshold", nullptr, &bmvr::g_StickJumpCrouchThreshold, 0.05f, 0.3f, 0.95f, "%.2f" },
            { "Turning blocks jump/crouch", RowKind::Toggle, "TurnBlocksJumpCrouch", &bmvr::g_TurnBlocksJumpCrouch, nullptr, 0, 0, 0, nullptr },
            { "Sprint click toggles", RowKind::Toggle, "SprintToggle", &bmvr::g_SprintToggle, nullptr, 0, 0, 0, nullptr },
            { "SHOULDER GESTURES", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Grip: left save, right crowbar", RowKind::Toggle, "ShoulderGestures", &bmvr::g_ShoulderGestures, nullptr, 0, 0, 0, nullptr },
            { "VIEW", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Recenter view", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Recenter after pause menu", RowKind::Toggle, "AutoRecenterOnResume", &bmvr::g_AutoRecenterOnResume, nullptr, 0, 0, 0, nullptr },
            { "Cutscene cameras move the view", RowKind::Toggle, "FollowScriptedCamera", &bmvr::g_FollowScriptedCamera, nullptr, 0, 0, 0, nullptr },
            { "GRAPHICS", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "LENS FLARES", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Reset flare values", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "flare_default" },
            { "Bloom and auto-exposure", RowKind::Toggle, "EyeBloom", &bmvr::g_EyeBloom, nullptr, 0, 0, 0, nullptr },
            { "Fade out at angle", RowKind::Number, "FlareFadeAngle", nullptr, &bmvr::g_FlareFadeAngle, 5.f, 0.f, 90.f, "%.0f\xB0" },
            { "Strength on the light", RowKind::Number, "FlareLightScale", nullptr, &bmvr::g_FlareLightScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "Large round haze", RowKind::Number, "FlareHazeScale", nullptr, &bmvr::g_FlareHazeScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "Haze from size", RowKind::Number, "FlareHazeMinSize", nullptr, &bmvr::g_FlareHazeMinSize, 0.05f, 0.05f, 4.f, "%.2f" },
            { "Ghosts away from light", RowKind::Number, "FlareGhostScale", nullptr, &bmvr::g_FlareGhostScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "LIGHT GLOWS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Reset glow values", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "glow_default" },
            { "Glow smoothing", RowKind::Number, "GlowSmooth", nullptr, &bmvr::g_GlowSmooth, 0.05f, 0.f, 0.5f, "%.2f s" },
            { "Dark when hidden after", RowKind::Number, "GlowHideMs", nullptr, &bmvr::g_GlowHideMs, 10.f, 0.f, 500.f, "%.0f ms" },
            { "Glows level, face you", RowKind::Toggle, "SpriteLevel", &bmvr::g_SpriteLevel, nullptr, 0, 0, 0, nullptr },
            { "Smoke stays upright", RowKind::Toggle, "ParticleLevel", &bmvr::g_ParticleLevel, nullptr, 0, 0, 0, nullptr },
            { "ANTI-ALIASING (MSAA)", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "MSAA on edges (0 = off)", RowKind::Number, "Msaa", nullptr, &bmvr::g_MsaaF, 2.f, 0.f, 4.f, "%.0fx" },
            // Leaves, grass, fences: their cut-out edges get the MSAA samples
            // too (softer, less crawl; can look a bit thinner). Needs MSAA on.
            { "MSAA on foliage edges", RowKind::Toggle, "AlphaToCoverage", &bmvr::g_AlphaToCoverage, nullptr, 0, 0, 0, nullptr },
            // Mip LOD bias in the 3D views: + = calmer, softer textures (sand,
            // gravel, grates shimmer less); menus and the HUD stay sharp.
            { "Texture calm (LOD bias)", RowKind::Number, "LodBias", nullptr, &bmvr::g_LodBias, 0.25f, -0.5f, 2.f, "%+.2f" },
            { "Glass refraction once per eye", RowKind::Toggle, "RefractOncePerEye", &bmvr::g_RefractOncePerEye, nullptr, 0, 0, 0, nullptr },
            { "ANTI-ALIASING (FXAA)", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Reset FXAA values", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "fxaa_default" },
            { "Use these settings", RowKind::Toggle, "FxaaApply", &bmvr::g_FxaaApply, nullptr, 0, 0, 0, nullptr },
            { "Sub-pixel smoothing", RowKind::Number, "FxaaSubpixel", nullptr, &bmvr::g_FxaaSubpixel, 0.05f, 0.f, 1.f, "%.2f" },
            { "Edge threshold", RowKind::Number, "FxaaEdgeThreshold", nullptr, &bmvr::g_FxaaEdgeThreshold, 0.01f, 0.f, 0.5f, "%.3f" },
            { "Dark edge threshold", RowKind::Number, "FxaaEdgeThresholdMin", nullptr, &bmvr::g_FxaaEdgeThresholdMin, 0.005f, 0.f, 0.1f, "%.3f" },
            { "PERFORMANCE", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Frame cap (0 off, 1 +10, 2 x2)", RowKind::Number, "FrameCap", nullptr, &bmvr::g_FrameCapMode, 1.f, 0.f, 2.f, "%.0f" },
            { "REFLECTIONS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Walls/floors (cubemap)", RowKind::Number, "EnvmapScaleWorld", nullptr, &bmvr::g_EnvmapScaleWorld, 0.1f, 0.f, 1.f, "%.1f" },
            { "Props/NPCs (cubemap)", RowKind::Number, "EnvmapScaleModels", nullptr, &bmvr::g_EnvmapScaleModels, 0.1f, 0.f, 1.f, "%.1f" },
            { "HAND HUD", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Reset hand HUD values", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "handhud_default" },
            { "Health/ammo on the hands", RowKind::Toggle, "VrHandHud", &bmvr::g_HandHud, nullptr, 0, 0, 0, nullptr },
            { "Panel size", RowKind::Number, "HandHudSize", nullptr, &bmvr::g_HandHudSize, 0.25f, 1.f, 6.f, "%.2f" },
            { "Up from the hand", RowKind::Number, "HandHudUp", nullptr, &bmvr::g_HandHudUp, 0.25f, -4.f, 8.f, "%+.2f" },
            { "Forward (- = to the wrist)", RowKind::Number, "HandHudForward", nullptr, &bmvr::g_HandHudForward, 0.25f, -8.f, 8.f, "%+.2f" },
            { "Sideways (+ = left)", RowKind::Number, "HandHudSide", nullptr, &bmvr::g_HandHudSide, 0.25f, -6.f, 6.f, "%+.2f" },
            { "Tilt", RowKind::Number, "HandHudTilt", nullptr, &bmvr::g_HandHudTilt, 2.f, -90.f, 90.f, "%+.0f\xB0" },
            { "Roll toward you", RowKind::Number, "HandHudRoll", nullptr, &bmvr::g_HandHudRoll, 2.f, -90.f, 90.f, "%+.0f\xB0" },
            { "WEAPON MENU", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Tilt to face the eye", RowKind::Toggle, "WeaponMenuTilt", &bmvr::g_WeaponMenuTilt, nullptr, 0, 0, 0, nullptr },
            // The gear only hides while the Triangle hold can open this panel.
            { "VR gear slot", RowKind::Toggle, "WeaponMenuSettingsGear", &bmvr::g_WeaponMenuSettingsGear, nullptr, 0, 0, 0, nullptr },
            { "Hold Triangle/Y for this panel", RowKind::Toggle, "SettingsTriangleHold", &bmvr::g_SettingsTriangleHold, nullptr, 0, 0, 0, nullptr },
            { "MENU", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Menu distance", RowKind::Number, "MenuForwardMeters", nullptr, &bmvr::g_MenuForwardMeters, 0.1f, 0.5f, 3.f, "%.1f m" },
            { "Menu size", RowKind::Number, "MenuWidthMeters", nullptr, &bmvr::g_MenuWidthMeters, 0.1f, 0.6f, 3.f, "%.1f m" },
            { "Menu height", RowKind::Number, "MenuHeightOffsetMeters", nullptr, &bmvr::g_MenuHeightOffsetMeters, 0.05f, -0.4f, 0.4f, "%+.2f m" },
            { "Pointer smoothing", RowKind::Number, "MenuCursorSmoothSec", nullptr, &bmvr::g_MenuCursorSmoothSec, 0.01f, 0.02f, 0.3f, "%.2f s" },
            { "DEBUG", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Capture/view tests are not saved (no key): always off after a restart.
            { "PERFORMANCE", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Performance log (costs fps)", RowKind::Toggle, "PerfLog", &bmvr::g_PerfLog, nullptr, 0, 0, 0, nullptr },
            { "CAPTURE", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Capture debug frame", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "debug_capture" },
            { "Glow trace (20 s)", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "glow_trace" },
            { "VIEW TESTS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Freeze head pose", RowKind::Toggle, nullptr, &bmvr::g_FreezeHead, nullptr, 0, 0, 0, nullptr },
            // Glow fades and other effects run on game time: with the game
            // paused a change only shows after the panel closes.
            { "Panel pauses the game", RowKind::Toggle, "VrSettingsPause", &bmvr::g_VrSettingsPause, nullptr, 0, 0, 0, nullptr },
            { "Blank eye (0 off, 1 L, 2 R)", RowKind::Number, nullptr, nullptr, &bmvr::g_DebugBlankEye, 1.f, 0.f, 2.f, "%.0f" },
            // Experiments; saved like the other settings, off by default.
            { "TESTS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Glow test area size", RowKind::Number, "GlowProxyScale", nullptr, &bmvr::g_GlowProxyScale, 0.5f, 1.f, 8.f, "%.1fx" },
            { "Head hold: angle", RowKind::Number, "PoseHoldDeg", nullptr, &bmvr::g_PoseHoldDeg, 0.02f, 0.f, 1.f, "%.2f\xB0" },
            { "Head hold: distance", RowKind::Number, "PoseHoldMm", nullptr, &bmvr::g_PoseHoldMm, 0.5f, 0.f, 20.f, "%.1f mm" },
            // Engine cvars: "cvar:" keys go to the console on the game thread
            // (never saved); the shown value is read back while the panel is open.
            { "ENGINE (CONSOLE)", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "sv_cheats", RowKind::Toggle, "cvar:sv_cheats", &g_EngSvCheats, nullptr, 0, 0, 0, nullptr },
            { "r_occlusion", RowKind::Toggle, "cvar:r_occlusion", &g_EngOcclusion, nullptr, 0, 0, 0, nullptr },
            { "r_dopixelvisibility", RowKind::Toggle, "cvar:r_dopixelvisibility", &g_EngPixelVis, nullptr, 0, 0, 0, nullptr },
            { "r_3dsky", RowKind::Toggle, "cvar:r_3dsky", &g_Eng3dSky, nullptr, 0, 0, 0, nullptr },
            { "rope_smooth_minwidth", RowKind::Number, "cvar:rope_smooth_minwidth", nullptr, &g_EngRopeMinWidth, 0.1f, 0.f, 3.f, "%.1f" },
            // Turning one on also sets sv_cheats 1.
            { "God mode", RowKind::Toggle, "cmd:god", &g_DevGod, nullptr, 0, 0, 0, nullptr },
            { "No clip", RowKind::Toggle, "cmd:noclip", &g_DevNoclip, nullptr, 0, 0, 0, nullptr },
            { "No target", RowKind::Toggle, "cmd:notarget", &g_DevNotarget, nullptr, 0, 0, 0, nullptr },
            // Hand/glove placement tuning; saved like the other settings.
            { "HANDS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Hand roll (thumb up)", RowKind::Number, "VrHandsRollDeg", nullptr, &bmvr::g_VrHandsRollDeg, 5.f, -135.f, 135.f, "%+.0f\xB0" },
            { "Hand on gun: right", RowKind::Number, "VrHandsGunGripRight", nullptr, &bmvr::g_VrHandsGunGripRight, 0.25f, -8.f, 8.f, "%+.2f" },
            { "Hand on gun: up", RowKind::Number, "VrHandsGunGripUp", nullptr, &bmvr::g_VrHandsGunGripUp, 0.25f, -8.f, 8.f, "%+.2f" },
            { "Hand on gun: forward", RowKind::Number, "VrHandsGunGripForward", nullptr, &bmvr::g_VrHandsGunGripForward, 0.25f, -12.f, 12.f, "%+.2f" },
            { "G-Man gesture", RowKind::Toggle, "VrGmanGesture", &bmvr::g_VrGmanGesture, nullptr, 0, 0, 0, nullptr },
            // bmvr_* fixes and tests (on/off for A/B); not saved: a restart
            // brings back the defaults.
            { "MOD SWITCHES", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Glow queries per eye", RowKind::Toggle, nullptr, &bmvr::g_PixelVisPerEye, nullptr, 0, 0, 0, nullptr },
            { "Glow through objects fix", RowKind::Toggle, nullptr, &bmvr::g_GlowCountDepthFix, nullptr, 0, 0, 0, nullptr },
            { "Hands + laser per-frame snapshot", RowKind::Toggle, nullptr, &bmvr::g_McHandSnapshot, nullptr, 0, 0, 0, nullptr },
            { "Lights culled per eye", RowKind::Toggle, nullptr, &bmvr::g_LightCullEyeView, nullptr, 0, 0, 0, nullptr },
            { "Flares: one layout", RowKind::Toggle, nullptr, &bmvr::g_FlareSharedLayout, nullptr, 0, 0, 0, nullptr },
            { "Flares level with horizon", RowKind::Toggle, nullptr, &bmvr::g_FlareLevel, nullptr, 0, 0, 0, nullptr },
            { "Flares on each eye's axis", RowKind::Toggle, nullptr, &bmvr::g_FlareEyeAxis, nullptr, 0, 0, 0, nullptr },
            { "Sky water per eye", RowKind::Toggle, nullptr, &bmvr::g_SkyWaterPerEye, nullptr, 0, 0, 0, nullptr },
            { "Underwater world fix", RowKind::Toggle, nullptr, &bmvr::g_EyeUnderwaterWorld, nullptr, 0, 0, 0, nullptr },
            { "Rope normals fix", RowKind::Toggle, nullptr, &bmvr::g_ViewProjNormalFix, nullptr, 0, 0, 0, nullptr },
            { "No stun/blur overlays", RowKind::Toggle, nullptr, &bmvr::g_VrSkipScreenEffects, nullptr, 0, 0, 0, nullptr },
            { "Guns skip draw animation", RowKind::Toggle, nullptr, &bmvr::g_SkipDrawAnim, nullptr, 0, 0, 0, nullptr },
            { "Tau/crossbow switch fix", RowKind::Toggle, nullptr, &bmvr::g_DeltaSeqIdle, nullptr, 0, 0, 0, nullptr },
            { "No crossbow scope animation", RowKind::Toggle, nullptr, &bmvr::g_ScopeClipIdle, nullptr, 0, 0, 0, nullptr },
            { "Mute weapon fidget sounds", RowKind::Toggle, nullptr, &bmvr::g_MuteFidgetSounds, nullptr, 0, 0, 0, nullptr },
            { "Muzzle flash follows gun", RowKind::Toggle, nullptr, &bmvr::g_ViewmodelFxCatchUp, nullptr, 0, 0, 0, nullptr },
            { "LOADING card", RowKind::Toggle, nullptr, &bmvr::g_LoadingCard, nullptr, 0, 0, 0, nullptr },
            { "Glow hold", RowKind::Number, nullptr, nullptr, &bmvr::g_GlowHold, 0.1f, 0.f, 2.f, "%.1f s" },
            { "G-MAN GESTURE", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Force gesture (tuning)", RowKind::Toggle, nullptr, &bmvr::g_VrGmanGestureForce, nullptr, 0, 0, 0, nullptr },
            { "Finger straight", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanFingerStraight, 0.05f, 0.f, 0.5f, "%.2f" },
            { "Thumb curl", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanThumbCurl, 0.1f, 0.f, 2.f, "%.1f" },
            { "Thumb swing", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanThumbSwing, 0.1f, 0.f, 1.5f, "%.1f" },
            // Drawn at the bottom of every tab.
            { "Close", RowKind::Close, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
        };
        return rows;
    }

    int RowHeight(const Row& r) { return r.kind == RowKind::Section ? kSectionPx : kRowPx; }

    // Tab shown on the panel; kept while the game runs.
    int g_Tab = 0;

    // Each tab's rows split into two columns at the section heading that best
    // halves that tab's height. The panel is as tall as the tallest tab, so it
    // does not change size or move when you switch tabs. Tab buttons sit in a
    // bar under the title, Close at the bottom of every tab. The row table is
    // fixed, so this is built once.
    struct Layout
    {
        std::vector<int> tab, col, top, x0, x1;   // tab -1 = shown on every tab
        int tabCount = 0;
        int closeTop = 0;
        int bottom = 0;
    };
    const Layout& GetLayout()
    {
        static Layout l = [] {
            const auto& rows = Rows();
            const int n = static_cast<int>(rows.size());
            Layout out;
            out.tab.assign(n, -1);
            out.col.assign(n, 0);
            out.top.assign(n, 0);
            out.x0.assign(n, 0);
            out.x1.assign(n, 0);
            std::vector<int> tabRows;
            for (int i = 0; i < n; ++i)
                if (rows[i].kind == RowKind::Tab)
                    tabRows.push_back(i);
            out.tabCount = static_cast<int>(tabRows.size());
            const int tabW = out.tabCount > 0 ? (kTexW - 80 - (out.tabCount - 1) * kTabGapPx) / out.tabCount : 0;
            int contentBottom = kContentTop;
            for (int t = 0; t < out.tabCount; ++t)
            {
                const int ti = tabRows[t];
                out.top[ti] = kHeaderPx;
                out.x0[ti] = 40 + t * (tabW + kTabGapPx);
                out.x1[ti] = out.x0[ti] + tabW;
                const int first = ti + 1;
                int last = first;   // one past this tab's rows
                while (last < n && rows[last].kind != RowKind::Tab && rows[last].kind != RowKind::Close)
                    ++last;
                int total = 0;
                for (int i = first; i < last; ++i)
                    total += RowHeight(rows[i]);
                int split = last, best = total, acc = 0;
                for (int i = first; i < last; ++i)
                {
                    if (i > first && rows[i].kind == RowKind::Section)
                    {
                        const int worst = acc > total - acc ? acc : total - acc;
                        if (worst < best)
                        {
                            best = worst;
                            split = i;
                        }
                    }
                    acc += RowHeight(rows[i]);
                }
                int y[2] = { kContentTop, kContentTop };
                for (int i = first; i < last; ++i)
                {
                    const int c = i < split ? 0 : 1;
                    out.tab[i] = t;
                    out.col[i] = c;
                    out.top[i] = y[c];
                    y[c] += RowHeight(rows[i]);
                }
                contentBottom = (std::max)({ contentBottom, y[0], y[1] });
            }
            out.closeTop = contentBottom + 16;
            out.bottom = out.closeTop + kRowPx;
            for (int i = 0; i < n; ++i)
            {
                if (rows[i].kind != RowKind::Close)
                    continue;
                out.top[i] = out.closeTop;
                out.x0[i] = 0;
                out.x1[i] = kTexW;
            }
            return out;
        }();
        return l;
    }
    int RowTop(int index) { return GetLayout().top[index]; }
    int RowX(int index) { return GetLayout().col[index] * kColW; }
    bool RowVisible(int index)
    {
        const int t = GetLayout().tab[index];
        return t < 0 || t == g_Tab;
    }
    // Row under texture x/y, or -1 (headings are not interactive). Tab
    // buttons and Close are rows too.
    int RowAt(int px, int py)
    {
        const auto& rows = Rows();
        const Layout& l = GetLayout();
        const int c = px < kColW ? 0 : 1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            const RowKind k = rows[i].kind;
            if (k == RowKind::Tab || k == RowKind::Close)
            {
                const int h = k == RowKind::Tab ? kTabPx : kRowPx;
                if (px >= l.x0[i] && px < l.x1[i] && py >= l.top[i] && py < l.top[i] + h)
                    return i;
                continue;
            }
            if (l.tab[i] != g_Tab || l.col[i] != c)
                continue;
            if (py >= l.top[i] && py < l.top[i] + RowHeight(rows[i]))
                return k == RowKind::Section ? -1 : i;
        }
        return -1;
    }
    int TexH() { return GetLayout().bottom + 16; }
    float PanelHeightHu() { return kPanelWidthHu * static_cast<float>(TexH()) / kTexW; }
    // The panel grows with the tallest tab, so it moves back instead of making
    // you look far up and down (two columns keep it near the minimum distance).
    float PanelDistanceHu()
    {
        const float fit = PanelHeightHu() * 0.5f / tanf(kPanelHalfFovDeg * kPi / 180.f);
        return fit > kPanelMinDistanceHu ? fit : kPanelMinDistanceHu;
    }

    std::string FormatValue(const Row& r)
    {
        char buf[64]{};
        if (r.kind == RowKind::Toggle)
            return (r.b && *r.b) ? "true" : "false";
        _snprintf(buf, sizeof(buf) - 1, "%g", r.f ? *r.f : 0.f);
        return buf;
    }

    bool IsDefault(const Row& r)
    {
        if (!g_DefaultsCaptured)
            return false;
        if (r.kind == RowKind::Toggle)
            return r.b && *r.b == r.defB;
        return r.f && std::fabs(*r.f - r.defF) <= 1e-4f * (std::max)(1.f, std::fabs(r.defF));
    }

    // Replace `Key=...` (first non-comment line) or append it; `Key=` lines of
    // the keys in `drop` are removed. Keeps the file's line endings and every
    // other line as it was.
    bool WriteConfigValues(const std::vector<std::pair<std::string, std::string>>& kv,
        const std::vector<std::string>& drop)
    {
        const std::string path = bmvr::VrFilePath("config.txt");
        std::string text;
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                text.append(buf, n);
            std::fclose(f);
        }
        const std::string eol = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
        for (const std::string& key : drop)
        {
            const std::string want = key + "=";
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                end = (end == std::string::npos) ? text.size() : end + 1;
                if (text.compare(pos, want.size(), want) == 0)
                {
                    text.erase(pos, end - pos);
                    continue;
                }
                pos = end;
            }
        }
        for (const auto& [key, value] : kv)
        {
            const std::string want = key + "=";
            size_t pos = 0;
            bool done = false;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos)
                    end = text.size();
                if (text.compare(pos, want.size(), want) == 0)
                {
                    size_t lineEnd = end;
                    if (lineEnd > pos && text[lineEnd - 1] == '\r')
                        --lineEnd;
                    text.replace(pos, lineEnd - pos, want + value);
                    done = true;
                    break;
                }
                pos = end + 1;
            }
            if (!done)
            {
                if (!text.empty() && text.back() != '\n')
                    text += eol;
                text += want + value + eol;
            }
        }
        const std::string tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            std::remove(tmp.c_str());
            return false;
        }
        return true;
    }

    struct PanelGfx
    {
        IDirect3DDevice9* device = nullptr;
        IDirect3DTexture9* sys = nullptr;
        IDirect3DTexture9* gpu = nullptr;
        IDirect3DTexture9* gearGpu = nullptr;
        IDirect3DTexture9* toastGpu = nullptr;
        char toastText[32]{};
        char toastDetail[64]{};
        int w = 0, h = 0;
    };
    PanelGfx g_Gfx;

    void ReleaseGfx()
    {
        if (g_Gfx.sys) g_Gfx.sys->Release();
        if (g_Gfx.gpu) g_Gfx.gpu->Release();
        if (g_Gfx.gearGpu) g_Gfx.gearGpu->Release();
        if (g_Gfx.toastGpu) g_Gfx.toastGpu->Release();
        g_Gfx = PanelGfx{};
    }

    // GDI canvas: top-down 32-bit DIB.
    struct Canvas
    {
        HDC dc = nullptr;
        HBITMAP bmp = nullptr;
        HGDIOBJ old = nullptr;
        uint32_t* px = nullptr;
        int w = 0, h = 0;
        Canvas(int width, int height) : w(width), h(height)
        {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            dc = CreateCompatibleDC(nullptr);
            void* bits = nullptr;
            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            px = static_cast<uint32_t*>(bits);
            if (dc && bmp)
                old = SelectObject(dc, bmp);
            SetBkMode(dc, TRANSPARENT);
        }
        ~Canvas()
        {
            if (dc && old) SelectObject(dc, old);
            if (bmp) DeleteObject(bmp);
            if (dc) DeleteDC(dc);
        }
        bool ok() const { return dc && bmp && px; }
        void fill(int x0, int y0, int x1, int y1, COLORREF c)
        {
            RECT r{ x0, y0, x1, y1 };
            HBRUSH b = CreateSolidBrush(c);
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        void text(const char* s, int x0, int y0, int x1, int y1, COLORREF c, int height, bool bold, UINT align)
        {
            HFONT f = CreateFontA(-height, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_SWISS, "Arial");
            HGDIOBJ of = SelectObject(dc, f);
            SetTextColor(dc, c);
            RECT r{ x0, y0, x1, y1 };
            DrawTextA(dc, s, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
            DeleteObject(f);
        }
        // GDI leaves alpha undefined; the whole panel gets one opacity.
        void setAlpha(uint8_t a)
        {
            GdiFlush();
            for (int i = 0; i < w * h; ++i)
                px[i] = (px[i] & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
        }
    };

    bool Upload(IDirect3DDevice9* device, const Canvas& c, IDirect3DTexture9*& sys, IDirect3DTexture9*& gpu)
    {
        if (!sys && FAILED(device->CreateTexture(c.w, c.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            return false;
        if (!gpu && FAILED(device->CreateTexture(c.w, c.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpu, nullptr)))
            return false;
        D3DLOCKED_RECT lr{};
        if (FAILED(sys->LockRect(0, &lr, nullptr, 0)))
            return false;
        for (int y = 0; y < c.h; ++y)
            std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                c.px + static_cast<size_t>(y) * c.w, static_cast<size_t>(c.w) * 4u);
        sys->UnlockRect(0);
        return SUCCEEDED(device->UpdateTexture(sys, gpu));
    }

    constexpr COLORREF kBg = RGB(18, 22, 27);
    constexpr COLORREF kRowHover = RGB(46, 54, 64);
    constexpr COLORREF kAmber = RGB(255, 176, 0);
    constexpr COLORREF kText = RGB(225, 230, 235);
    constexpr COLORREF kDim = RGB(120, 128, 136);
    constexpr COLORREF kBtn = RGB(60, 68, 78);
    constexpr COLORREF kBtnHover = RGB(255, 176, 0);
    constexpr COLORREF kOn = RGB(60, 150, 80);
    constexpr COLORREF kOff = RGB(70, 74, 80);

    void Paint(Canvas& c, int hoverRow, int hoverZone)
    {
        c.fill(0, 0, c.w, c.h, kBg);
        c.fill(0, 0, c.w, 6, kAmber);
        c.text("VR SETTINGS", 40, 10, c.w - 40, kHeaderPx - 10, kAmber, 46, true, DT_LEFT);
        c.text("trigger: change   circle: close", 40, 10, c.w - 40, kHeaderPx - 10, kDim, 24, false, DT_RIGHT);
        const Layout& l = GetLayout();
        // Tab bar: active tab amber, the others like buttons.
        c.fill(40, kHeaderPx + kTabPx + 4, c.w - 40, kHeaderPx + kTabPx + 8, kAmber);
        // Column divider.
        c.fill(kColW - 2, kContentTop, kColW + 2, l.closeTop - 8, RGB(90, 70, 20));
        const auto& rows = Rows();
        int tabIndex = 0;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            const Row& r = rows[i];
            const bool hover = (i == hoverRow);
            if (r.kind == RowKind::Tab)
            {
                const bool active = (tabIndex++ == g_Tab);
                const int y0 = l.top[i], y1 = y0 + kTabPx;
                c.fill(l.x0[i], y0 + 4, l.x1[i], y1, active ? kAmber : hover ? kRowHover : kBtn);
                c.text(r.label, l.x0[i], y0 + 4, l.x1[i], y1, active ? RGB(0, 0, 0) : kText, 32, true, DT_CENTER);
                continue;
            }
            if (!RowVisible(i))
                continue;
            const int y0 = RowTop(i), y1 = y0 + RowHeight(r);
            const int x = r.kind == RowKind::Close ? 0 : RowX(i);
            const int cw = r.kind == RowKind::Close ? c.w : kColW, mid = x + cw / 2;
            if (r.kind == RowKind::Section)
            {
                c.text(r.label, x + 40, y0 + 8, x + cw - 40, y1, kAmber, 26, true, DT_LEFT);
                // 4 px: the panel is drawn minified without mipmaps, so 2 px lines dropped out.
                c.fill(x + 40, y1 - 6, x + cw - 40, y1 - 2, RGB(90, 70, 20));
                continue;
            }
            if (hover && r.kind != RowKind::Close)
                c.fill(x + 16, y0 + 2, x + cw - 16, y1 - 2, kRowHover);
            if (r.kind == RowKind::Close)
            {
                c.fill(mid - 120, y0 + 8, mid + 120, y1 - 8, hover ? kBtnHover : kBtn);
                c.text("Close", x, y0, x + cw, y1, hover ? RGB(0, 0, 0) : kText, 34, true, DT_CENTER);
                continue;
            }
            if (r.kind == RowKind::Button)
            {
                c.fill(mid - 180, y0 + 8, mid + 180, y1 - 8, hover ? kBtnHover : kBtn);
                c.text(r.label, x, y0, x + cw, y1, hover ? RGB(0, 0, 0) : kText, 34, true, DT_CENTER);
                continue;
            }
            c.text(r.label, x + 40, y0, x + kMinusX0 - 10, y1, kText, 34, false, DT_LEFT);
            if (r.kind == RowKind::Toggle)
            {
                const bool on = r.b && *r.b;
                c.fill(x + kPillX0, y0 + 10, x + kPillX1, y1 - 10, on ? kOn : kOff);
                c.text(on ? "ON" : "OFF", x + kPillX0, y0, x + kPillX1, y1, RGB(255, 255, 255), 30, true, DT_CENTER);
            }
            else
            {
                char val[32]{};
                _snprintf(val, sizeof(val) - 1, r.fmt, r.f ? *r.f : 0.f);
                c.fill(x + kMinusX0, y0 + 10, x + kMinusX1 - 8, y1 - 10, (hover && hoverZone == 0) ? kBtnHover : kBtn);
                c.fill(x + kPlusX0 + 8, y0 + 10, x + kPlusX1, y1 - 10, (hover && hoverZone == 2) ? kBtnHover : kBtn);
                c.text("-", x + kMinusX0, y0, x + kMinusX1 - 8, y1 - 4, kText, 44, true, DT_CENTER);
                c.text("+", x + kPlusX0 + 8, y0, x + kPlusX1, y1 - 2, kText, 40, true, DT_CENTER);
                c.text(val, x + kValueX0, y0, x + kValueX1, y1, kText, 34, true, DT_CENTER);
            }
        }
        c.setAlpha(238);
    }

    struct MenuVertTex
    {
        float x, y, z, rhw;
        D3DCOLOR color;
        float u, v;
    };
    struct MenuVert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };
}

void bmvr::CapturePanelDefaults()
{
    for (Row& r : Rows())
    {
        if (r.b)
            r.defB = *r.b;
        if (r.f)
            r.defF = *r.f;
    }
    g_DefaultsCaptured = true;
}

// Game thread (FlushPendingGameUi): runs the panel's engine console commands
// and, while the panel is open, reads the engine cvars back into the rows so
// a refused command (cheat-protected) shows the real value.
void VR::FlushPanelEngineCommands()
{
    if (!m_Game)
        return;
    std::vector<std::string> cmds;
    {
        std::lock_guard<std::mutex> lock(g_EngCmdLock);
        cmds.swap(g_EngCmds);
    }
    for (const std::string& c : cmds)
    {
        const bool ok = m_Game->ClientCmd_Unrestricted(c.c_str());
        Game::logMsg("VR settings console '%s' ok=%d", c.c_str(), ok ? 1 : 0);
    }
    static DWORD s_lastRead;
    const DWORD now = GetTickCount();
    if (!m_SettingsOpen || (cmds.empty() && now - s_lastRead < 500))
        return;
    s_lastRead = now;
    bool changed = false;
    // MOVETYPE_NOCLIP = 8.
    const int moveType = m_Game->LocalPlayerMoveType();
    if (moveType >= 0 && g_DevNoclip != (moveType == 8))
    {
        g_DevNoclip = moveType == 8;
        changed = true;
    }
    for (Row& r : Rows())
    {
        if (!IsEngineCvarKey(r.key))
            continue;
        float v = 0.f;
        if (!m_Game->GetConVarFloat(r.key + 5, v))
            continue;
        if (r.kind == RowKind::Toggle && r.b && *r.b != (v != 0.f))
        {
            *r.b = v != 0.f;
            changed = true;
        }
        else if (r.kind == RowKind::Number && r.f && std::fabs(*r.f - v) > 1e-4f)
        {
            *r.f = v;
            changed = true;
        }
    }
    if (changed)
        m_SettingsDirty = true;
}

IDirect3DTexture9* VR::SettingsGearTexture(IDirect3DDevice9* device)
{
    if (!device)
        return nullptr;
    if (g_Gfx.device && g_Gfx.device != device)
        ReleaseGfx();
    g_Gfx.device = device;
    if (!g_Gfx.gearGpu)
    {
        Canvas c(256, 128);
        if (!c.ok())
            return nullptr;
        c.fill(0, 0, c.w, c.h, RGB(0, 0, 0));
        c.text("VR", 0, 0, c.w, 92, RGB(255, 255, 255), 84, true, DT_CENTER);
        c.text("SETTINGS", 0, 84, c.w, 124, RGB(255, 255, 255), 30, true, DT_CENTER);
        // Alpha from brightness: white text on a transparent background.
        GdiFlush();
        for (int i = 0; i < c.w * c.h; ++i)
        {
            const uint32_t p = c.px[i];
            const uint32_t a = (std::max)({ p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF });
            c.px[i] = (a << 24) | 0x00FFFFFFu;
        }
        IDirect3DTexture9* sys = nullptr;
        if (!Upload(device, c, sys, g_Gfx.gearGpu))
        {
            if (g_Gfx.gearGpu) { g_Gfx.gearGpu->Release(); g_Gfx.gearGpu = nullptr; }
        }
        if (sys)
            sys->Release();
    }
    return g_Gfx.gearGpu;
}

void VR::OpenSettingsPanel()
{
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    Vector yawFwd = fwd;
    yawFwd.z = 0.f;
    if (VectorNormalize(yawFwd) <= 0.01f)
        yawFwd = Vector(1.f, 0.f, 0.f);
    const Vector worldUp(0.f, 0.f, 1.f);
    // Source axes: x forward, y left, z up, so forward x up points right.
    Vector yawRight = CrossProduct(yawFwd, worldUp);
    VectorNormalize(yawRight);
    const Vector eye = GetViewOrigin(body);
    const Vector center = eye + yawFwd * PanelDistanceHu() - worldUp * kPanelDropHu;
    // Same latching as the weapon menu: stored relative to the body and the
    // turn offset, so the panel stays put in the room while you look around.
    m_SettingsLatchDelta = center - body;
    m_SettingsLatchFwd = yawFwd;
    m_SettingsLatchRight = yawRight;
    m_SettingsLatchYaw = m_RotationOffsetY.load(std::memory_order_acquire);
    m_SettingsOpen = true;
    m_SettingsHover = -1;
    m_SettingsHoverZone = -1;
    m_SettingsDirty = true;
    m_SettingsChanged = false;
    m_SettingsClickHeld = true;   // ignore the trigger until it is released once
    m_SettingsBackHeld = true;
    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 900, 0.35f);
    // Pause without Black Mesa's menu (that would close the panel).
    m_SettingsPausedGame = bmvr::g_VrSettingsPause;
    if (m_SettingsPausedGame)
        m_PendingPauseCmd.store(1, std::memory_order_release);
    Game::logMsg("VR settings panel opened pause=%d", m_SettingsPausedGame ? 1 : 0);
}

void VR::CloseSettingsPanel(const char* why)
{
    if (!m_SettingsOpen)
        return;
    m_SettingsOpen = false;
    if (m_SettingsChanged)
    {
        std::vector<std::pair<std::string, std::string>> kv;
        std::vector<std::string> drop;
        for (const Row& r : Rows())
        {
            if (!r.key || IsEngineKey(r.key))
                continue;
            if (IsDefault(r))
                drop.emplace_back(r.key);
            else
                kv.emplace_back(r.key, FormatValue(r));
        }
        const bool ok = WriteConfigValues(kv, drop);
        Game::logMsg("VR settings saved to config.txt: %s (%d set, %d at default left out)",
            ok ? "ok" : "FAILED", static_cast<int>(kv.size()), static_cast<int>(drop.size()));
    }
    m_SettingsChanged = false;
    // Closed by Black Mesa's pause menu: leave the pause to that menu.
    if (m_SettingsPausedGame && !(why && std::strcmp(why, "pause") == 0))
        m_PendingPauseCmd.store(2, std::memory_order_release);
    m_SettingsPausedGame = false;
    Game::logMsg("VR settings panel closed (%s)", why ? why : "");
}

void VR::SettingsPanelFrame(Vector& origin, Vector& fwd, Vector& right, Vector& up) const
{
    const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    const float yawDelta = m_RotationOffsetY.load(std::memory_order_acquire) - m_SettingsLatchYaw;
    const float a = yawDelta * (kPi / 180.f), c = cosf(a), s = sinf(a);
    auto yaw = [&](const Vector& v) { return Vector(v.x * c - v.y * s, v.x * s + v.y * c, v.z); };
    origin = body + yaw(m_SettingsLatchDelta);
    fwd = yaw(m_SettingsLatchFwd);
    right = yaw(m_SettingsLatchRight);
    up = Vector(0.f, 0.f, 1.f);
}

void VR::UpdateSettingsPanel()
{
    if (!m_SettingsOpen)
        return;
    if (!m_GameplayEligible || PauseUiActive())
    {
        CloseSettingsPanel("pause");
        return;
    }
    Vector origin, fwd, right, up;
    SettingsPanelFrame(origin, fwd, right, up);

    // Pointer: the right controller's own ray (not the shot trim).
    const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    const Vector start = GetRightControllerAbsPos(body);
    Vector dir;
    QAngle::AngleVectors(GetRightControllerAbsAngle(), &dir, nullptr, nullptr);
    int row = -1, zone = -1;
    int hitPx = -1, hitPy = -1;
    m_SettingsHitValid = false;
    const float denom = dir.Dot(fwd);
    if (fabsf(denom) > 1e-4f)
    {
        const float t = (origin - start).Dot(fwd) / denom;
        if (t > 0.f && t < 200.f)
        {
            const Vector hit = start + dir * t;
            const Vector rel = hit - origin;
            const float x = rel.Dot(right), y = rel.Dot(up);
            const float hw = kPanelWidthHu * 0.5f, hh = PanelHeightHu() * 0.5f;
            if (fabsf(x) <= hw && fabsf(y) <= hh)
            {
                m_SettingsHitX = x;
                m_SettingsHitY = y;
                m_SettingsHitValid = true;
                const int px = static_cast<int>((x / kPanelWidthHu + 0.5f) * kTexW);
                const int py = static_cast<int>((0.5f - y / PanelHeightHu()) * TexH());
                const int r = RowAt(px, py);
                hitPx = px;
                hitPy = py;
                if (r >= 0)
                {
                    row = r;
                    // -/+ zones only on number rows; elsewhere the zone
                    // flickered across the pill and repainted every frame.
                    const int cx = px - RowX(r);
                    if (Rows()[r].kind == RowKind::Number)
                        zone = (cx >= kMinusX0 && cx < kMinusX1) ? 0 : (cx >= kPlusX0 && cx < kPlusX1) ? 2 : 1;
                    else
                        zone = 1;
                }
            }
        }
    }
    if (row != m_SettingsHover || zone != m_SettingsHoverZone)
    {
        if (row >= 0 && row != m_SettingsHover)
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 300, 0.15f);
        m_SettingsHover = row;
        m_SettingsHoverZone = zone;
        m_SettingsDirty = true;
    }

    // Trigger only, full pull (Cross and a resting finger on the trigger
    // both read as presses on the PS VR2 Sense, see the menu).
    static bool s_panelTrigLatch;
    const bool click = MenuTriggerClickHeld(s_panelTrigLatch);
    const bool back = PressedDigitalAction(m_ActionMenuBack);
    const bool clickEdge = click && !m_SettingsClickHeld;
    const bool backEdge = back && !m_SettingsBackHeld;
    // Diagnostic (2026-11-11: clicks on some Debug rows seemed lost): every
    // trigger press/release with what is under the pointer.
    if (click != m_SettingsClickHeld)
    {
        vr::InputAnalogActionData_t trig{};
        const bool haveTrig = GetAnalogActionData(m_ActionPrimaryAttack, trig);
        Game::logMsg("VR settings trigger %s: row %d (%s) zone %d at %d,%d tab %d trigger %.2f",
            click ? "press" : "release", row, row >= 0 ? Rows()[row].label : "-", zone, hitPx, hitPy, g_Tab,
            haveTrig ? trig.x : -1.f);
    }
    m_SettingsClickHeld = click;
    m_SettingsBackHeld = back;
    if (backEdge)
    {
        CloseSettingsPanel("circle");
        return;
    }
    // Holding the trigger on a number row's -/+ repeats the step: first repeat
    // after 0.4 s, then ~11/s, ~25/s once held 1.5 s. Moving off the zone stops it.
    static int s_repeatRow = -1, s_repeatZone = -1;
    static DWORD s_holdStart = 0, s_nextRepeat = 0;
    static bool s_repeated = false;
    const DWORD now = GetTickCount();
    bool repeat = false;
    if (clickEdge)
    {
        s_repeatRow = row;
        s_repeatZone = zone;
        s_holdStart = now;
        s_nextRepeat = now + 400;
        s_repeated = false;
    }
    else if (click && s_repeatRow >= 0)
    {
        if (row != s_repeatRow || zone != s_repeatZone)
            s_repeatRow = -1;
        else if (Rows()[row].kind == RowKind::Number && zone != 1
            && static_cast<int>(now - s_nextRepeat) >= 0)
        {
            repeat = true;
            s_nextRepeat = now + (now - s_holdStart > 1500 ? 40 : 90);
        }
    }
    else if (!click && s_repeatRow >= 0)
    {
        if (s_repeated)
            Game::logMsg("VR settings: %s = %s (held)", Rows()[s_repeatRow].label,
                FormatValue(Rows()[s_repeatRow]).c_str());
        s_repeatRow = -1;
    }
    if ((!clickEdge && !repeat) || row < 0)
        return;
    if (repeat)
        s_repeated = true;
    Row& r = Rows()[row];
    switch (r.kind)
    {
    case RowKind::Section:
        return;
    case RowKind::Tab:
    {
        int t = 0;
        for (int i = 0; i < row; ++i)
            t += Rows()[i].kind == RowKind::Tab ? 1 : 0;
        if (t != g_Tab)
        {
            g_Tab = t;
            m_SettingsDirty = true;
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 500, 0.25f);
            Game::logMsg("VR settings tab: %s", r.label);
        }
        return;
    }
    case RowKind::Close:
        CloseSettingsPanel("close row");
        return;
    case RowKind::Button:
        // The FXAA values back at their defaults; on or off stays as it is.
        if (r.fmt && std::strcmp(r.fmt, "fxaa_default") == 0)
        {
            bmvr::g_FxaaSubpixel = 0.25f;
            bmvr::g_FxaaEdgeThreshold = 0.166f;
            bmvr::g_FxaaEdgeThresholdMin = 0.0833f;
            break;
        }
        // The hand HUD placement back at its defaults; on or off stays as it is.
        if (r.fmt && std::strcmp(r.fmt, "handhud_default") == 0)
        {
            bmvr::g_HandHudSize = 2.5f;
            bmvr::g_HandHudUp = 2.2f;
            bmvr::g_HandHudForward = -0.55f;
            bmvr::g_HandHudSide = 0.3f;
            bmvr::g_HandHudTilt = -18.f;
            bmvr::g_HandHudRoll = 14.f;
            break;
        }
        // The glow values back at their defaults.
        if (r.fmt && std::strcmp(r.fmt, "glow_default") == 0)
        {
            bmvr::g_GlowSmooth = 0.15f;
            bmvr::g_GlowHideMs = 20.f;
            break;
        }
        // The bloom/flare values back at their defaults; on or off stays as it is.
        if (r.fmt && std::strcmp(r.fmt, "flare_default") == 0)
        {
            bmvr::g_FlareFadeAngle = 45.f;
            bmvr::g_FlareLightScale = 1.f;
            bmvr::g_FlareHazeScale = 0.f;
            bmvr::g_FlareHazeMinSize = 0.5f;
            bmvr::g_FlareGhostScale = 0.f;
            break;
        }
        // Closes the panel; 2 s later one frame is captured (eyes, audit,
        // water/fog inputs) into VR/, so a rare render bug can be looked at.
        if (r.fmt && std::strcmp(r.fmt, "debug_capture") == 0)
        {
            bmvr::g_DebugCaptureAt = GetTickCount() + 2000;
            if (!bmvr::g_DebugCaptureAt)
                bmvr::g_DebugCaptureAt = 1;
            Game::logMsg("debug capture: armed, in 2 s");
            CloseSettingsPanel("debug capture");
            return;
        }
        // Closes the panel and records 20 s of glow brightness + frame timing
        // (bmvr_glowtrace.h).
        if (r.fmt && std::strcmp(r.fmt, "glow_trace") == 0)
        {
            bmvr::g_GlowTraceSeconds = 20.f;
            Game::logMsg("glow trace: armed from the panel (20 s)");
            CloseSettingsPanel("glow trace");
            return;
        }
        // Recenter (was a right-stick tap, now hands/weapon).
        Recenter("settings panel");
        CloseSettingsPanel("recenter");
        return;
    case RowKind::Toggle:
        if (r.b)
            *r.b = !*r.b;
        break;
    case RowKind::Number:
        if (r.f && zone != 1)
        {
            float v = *r.f + (zone == 2 ? r.step : -r.step);
            v = std::round(v / r.step) * r.step;   // no drift from repeated float steps
            *r.f = std::clamp(v, r.lo, r.hi);
        }
        else
            return;
        break;
    }
    // Pause switched while the panel is open: apply it now.
    if (r.b == &bmvr::g_VrSettingsPause && bmvr::g_VrSettingsPause != m_SettingsPausedGame)
    {
        m_SettingsPausedGame = bmvr::g_VrSettingsPause;
        m_PendingPauseCmd.store(m_SettingsPausedGame ? 1 : 2, std::memory_order_release);
    }
    if (IsEngineCmdKey(r.key))
    {
        std::lock_guard<std::mutex> lock(g_EngCmdLock);
        if (r.b && *r.b)
            g_EngCmds.emplace_back("sv_cheats 1");
        g_EngCmds.emplace_back(r.key + 4);
    }
    else if (IsEngineCvarKey(r.key))
    {
        char cmd[96]{};
        if (r.kind == RowKind::Toggle)
            _snprintf(cmd, sizeof(cmd) - 1, "%s %d", r.key + 5, (r.b && *r.b) ? 1 : 0);
        else
            _snprintf(cmd, sizeof(cmd) - 1, "%s %g", r.key + 5, r.f ? *r.f : 0.f);
        std::lock_guard<std::mutex> lock(g_EngCmdLock);
        g_EngCmds.emplace_back(cmd);
    }
    else
        m_SettingsChanged = true;
    m_SettingsDirty = true;
    if (repeat)
    {
        PulseHandHaptic(vr::TrackedControllerRole_RightHand, 200, 0.1f);
        return;
    }
    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 700, 0.3f);
    Game::logMsg("VR settings: %s = %s", r.label, FormatValue(r).c_str());
}

void VR::DrawSettingsPanel(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    if (!m_SettingsOpen || !device)
        return;
    if (g_Gfx.device && g_Gfx.device != device)
        ReleaseGfx();
    g_Gfx.device = device;
    // exchange before Paint: a click on the game thread while this paints
    // sets the flag again, so it is drawn next frame. (A plain bool cleared
    // after Upload lost such clicks: pills needed 2-3 pulls, 2026-11-11.)
    if (m_SettingsDirty.exchange(false) || !g_Gfx.gpu)
    {
        Canvas c(kTexW, TexH());
        if (c.ok())
        {
            Paint(c, m_SettingsHover, m_SettingsHoverZone);
            if (g_Gfx.w != c.w || g_Gfx.h != c.h)
            {
                if (g_Gfx.sys) { g_Gfx.sys->Release(); g_Gfx.sys = nullptr; }
                if (g_Gfx.gpu) { g_Gfx.gpu->Release(); g_Gfx.gpu = nullptr; }
                g_Gfx.w = c.w;
                g_Gfx.h = c.h;
            }
            if (!Upload(device, c, g_Gfx.sys, g_Gfx.gpu))
                m_SettingsDirty = true;
        }
        else
            m_SettingsDirty = true;
    }
    if (!g_Gfx.gpu)
        return;

    // Same projection as DrawWeaponMenu (union frustum + this eye's off-centre remap).
    const float pixelAspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float aspect = m_OffCenterActive ? m_OffCenterAspect
        : (bmvr::UseGbMatchViewLock() && m_Aspect > 0.1f) ? m_Aspect : pixelAspect;
    const float projFov = m_OffCenterActive ? WorldRenderFov() : HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * kPi / 180.f);
    if (!(tanHalf > 0.01f))
        return;
    float ocSx = 1.f, ocOx = 0.f, ocSy = 1.f, ocOy = 0.f;
    OffCenterNdc(stereoEye, ocSx, ocOx, ocSy, ocOy);
    auto project = [&](const Vector& world, float& sx, float& sy, float& rhw) -> bool {
        const Vector d = world - eyeOrig;
        const float z = d.Dot(fwd);
        if (z < 0.5f)
            return false;
        rhw = 1.f / z;
        const float ndcX = (d.Dot(right) * rhw) / tanHalf * ocSx + ocOx;
        const float ndcY = ((d.Dot(up) * rhw) * aspect) / tanHalf * ocSy + ocOy;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return true;
    };

    Vector po, pf, pr, pu;
    SettingsPanelFrame(po, pf, pr, pu);
    const float hw = kPanelWidthHu * 0.5f, hh = PanelHeightHu() * 0.5f;
    const Vector corners[4] = { po - pr * hw + pu * hh, po + pr * hw + pu * hh, po - pr * hw - pu * hh, po + pr * hw - pu * hh };
    MenuVertTex v[4]{};
    const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; ++i)
    {
        if (!project(corners[i], v[i].x, v[i].y, v[i].rhw))
            return;
        v[i].color = D3DCOLOR_ARGB(255, 255, 255, 255);
        v[i].u = uv[i][0];
        v[i].v = uv[i][1];
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetTexture(0, g_Gfx.gpu);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVertTex));
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    // Pointer dot where the controller ray meets the panel.
    if (m_SettingsHitValid)
    {
        const Vector p = po + pr * m_SettingsHitX + pu * m_SettingsHitY - pf * 0.05f;
        constexpr int kSegs = 16;
        constexpr float kR = 0.18f;
        MenuVert fan[kSegs + 2]{};
        float cx, cy, crhw;
        if (project(p, cx, cy, crhw))
        {
            fan[0] = { cx, cy, 0.f, crhw, D3DCOLOR_ARGB(255, 255, 255, 255) };
            bool ok = true;
            for (int k = 0; k <= kSegs && ok; ++k)
            {
                const float a = static_cast<float>(k) * (2.f * kPi / kSegs);
                ok = project(p + pr * (cosf(a) * kR) + pu * (sinf(a) * kR), fan[k + 1].x, fan[k + 1].y, fan[k + 1].rhw);
                fan[k + 1].z = 0.f;
                fan[k + 1].color = D3DCOLOR_ARGB(255, 255, 255, 255);
            }
            if (ok)
                device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kSegs, fan, sizeof(MenuVert));
        }
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void VR::ShowToast(const char* text, DWORD ms, const char* detail)
{
    strncpy_s(m_ToastText, text ? text : "", _TRUNCATE);
    strncpy_s(m_ToastDetail, detail ? detail : "", _TRUNCATE);
    m_ToastUntilMs = GetTickCount() + ms;
}

bool VR::ToastVisible() const
{
    return m_ToastUntilMs != 0 && static_cast<int>(m_ToastUntilMs - GetTickCount()) > 0;
}

void VR::DrawToast(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    // Short head-locked message (quick save), slightly below the view centre.
    if (!device || !ToastVisible())
        return;
    if (g_Gfx.device && g_Gfx.device != device)
        ReleaseGfx();
    g_Gfx.device = device;
    const bool detail = m_ToastDetail[0] != 0;
    const int texH = detail ? 144 : 96;
    if (!g_Gfx.toastGpu || std::strcmp(g_Gfx.toastText, m_ToastText) != 0
        || std::strcmp(g_Gfx.toastDetail, m_ToastDetail) != 0)
    {
        if (g_Gfx.toastGpu) { g_Gfx.toastGpu->Release(); g_Gfx.toastGpu = nullptr; }
        Canvas c(512, texH);
        if (!c.ok())
            return;
        c.fill(0, 0, c.w, c.h, kBg);
        c.fill(0, 0, c.w, 5, kAmber);
        c.text(m_ToastText, 0, 5, c.w, 96, kAmber, detail ? 46 : 54, true, DT_CENTER);
        if (detail)
            c.text(m_ToastDetail, 8, 92, c.w - 8, c.h - 8, kText, 24, false, DT_CENTER);
        c.setAlpha(225);
        IDirect3DTexture9* sys = nullptr;
        if (!Upload(device, c, sys, g_Gfx.toastGpu) && g_Gfx.toastGpu)
        {
            g_Gfx.toastGpu->Release();
            g_Gfx.toastGpu = nullptr;
        }
        if (sys)
            sys->Release();
        strncpy_s(g_Gfx.toastText, m_ToastText, _TRUNCATE);
        strncpy_s(g_Gfx.toastDetail, m_ToastDetail, _TRUNCATE);
    }
    if (!g_Gfx.toastGpu)
        return;

    const float pixelAspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float aspect = m_OffCenterActive ? m_OffCenterAspect
        : (bmvr::UseGbMatchViewLock() && m_Aspect > 0.1f) ? m_Aspect : pixelAspect;
    const float projFov = m_OffCenterActive ? WorldRenderFov() : HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * kPi / 180.f);
    if (!(tanHalf > 0.01f))
        return;
    float ocSx = 1.f, ocOx = 0.f, ocSy = 1.f, ocOy = 0.f;
    OffCenterNdc(stereoEye, ocSx, ocOx, ocSy, ocOy);
    auto project = [&](const Vector& world, float& sx, float& sy, float& rhw) -> bool {
        const Vector d = world - eyeOrig;
        const float z = d.Dot(fwd);
        if (z < 0.5f)
            return false;
        rhw = 1.f / z;
        const float ndcX = (d.Dot(right) * rhw) / tanHalf * ocSx + ocOx;
        const float ndcY = ((d.Dot(up) * rhw) * aspect) / tanHalf * ocSy + ocOy;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return true;
    };
    // Both eyes share the head-centred placement, so it fuses at 30 HU.
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    const Vector head = GetViewOrigin(body);
    const Vector po = head + fwd * 30.f - up * 5.f;
    const float hw = detail ? 10.f : 6.f, hh = hw * static_cast<float>(texH) / 512.f;  // warnings: larger, readable
    const Vector corners[4] = { po - right * hw + up * hh, po + right * hw + up * hh, po - right * hw - up * hh, po + right * hw - up * hh };
    const int left = static_cast<int>(m_ToastUntilMs - GetTickCount());
    const int alpha = (std::min)(255, (std::max)(0, left * 255 / 400));
    MenuVertTex v[4]{};
    const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; ++i)
    {
        if (!project(corners[i], v[i].x, v[i].y, v[i].rhw))
            return;
        v[i].color = D3DCOLOR_ARGB(alpha, 255, 255, 255);
        v[i].u = uv[i][0];
        v[i].v = uv[i][1];
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetTexture(0, g_Gfx.toastGpu);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVertTex));
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}
