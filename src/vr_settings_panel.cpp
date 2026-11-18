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
#include "bmvr_cvars.h"
#include "vr_button_map.h"
#include "vr_controller_models.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
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
    bool g_EngOccTracker = true;
    bool g_EngOccTrackerShadow = true;
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

    // Info: a read-only status line (label left, text from InfoText right); not clickable.
    // Choice: a float stepped lo..hi by step, shown by name; fmt = "Name0|Name1|..."
    // (index = (value - lo) / step), a click cycles to the next (wraps).
    // DevSection: a heading whose rows (up to the next heading or tab) only show
    // with "Show developer rows" on (bmvr::g_PanelDevRows); still saved/loaded.
    // Preset: no value of its own; fmt names a table in Presets() that sets
    // several rows at once. Shows the matching preset's name ("Custom" if none
    // matches), a click applies the next one. Never saved (the values are).
    // Bind: one controller button (fmt "L.face1", "R.face2.hold"); label and value
    // come from the detected controller and the button map; a click opens the
    // action picker. ColumnBreak: the rows after it go to the right column
    // (CONTROLS tab: left / right controller). BindPreset: like Preset, for the
    // button-map presets (vr_button_map.h). BindPicture (fmt "L"/"R"): that
    // controller drawn from its SteamVR model with a card per button; the card
    // lines are the hand's Bind rows (hidden while the picture shows).
    enum class RowKind { Tab, Section, Toggle, Number, Button, Close, Info, Choice, DevSection, Preset,
        Bind, ColumnBreak, BindPreset, BindPicture };
    bool IsHeading(RowKind k) { return k == RowKind::Section || k == RowKind::DevSection; }

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
            { "MOVEMENT", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "TURNING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Snap turning", RowKind::Toggle, "SnapTurning", &bmvr::g_SnapTurning, nullptr, 0, 0, 0, nullptr },
            { "Snap angle", RowKind::Number, "SnapTurnAngle", nullptr, &bmvr::g_SnapTurnAngle, 15.f, 15.f, 90.f, "%.0f\xB0" },
            { "Smooth turn speed", RowKind::Number, "TurnSpeed", nullptr, &bmvr::g_TurnSpeed, 0.05f, 0.05f, 1.f, "%.2f" },
            { "MOVEMENT", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Sprint click toggles", RowKind::Toggle, "SprintToggle", &bmvr::g_SprintToggle, nullptr, 0, 0, 0, nullptr },
            { "Climb ladders with grip (hands)", RowKind::Toggle, "PhysicalLadders", &bmvr::g_PhysicalLadders, nullptr, 0, 0, 0, nullptr },
            { "Smooth hand climbing", RowKind::Toggle, "PhysicalLadderSmooth", &bmvr::g_PhysicalLadderSmooth, nullptr, 0, 0, 0, nullptr },
            { "Gun hand on ladders", RowKind::Choice, "LadderGunHand", nullptr, &bmvr::g_LadderGunHand, 1.f, 0.f, 1.f, "Holster weapon|Needs empty hand" },
            { "Hand distance from rung", RowKind::Number, "LadderHandBack", nullptr, &bmvr::g_LadderHandBack, 0.5f, 0.f, 10.f, "%.1f" },
            { "Stick climbs ladders too", RowKind::Toggle, "LadderStickClimb", &bmvr::g_LadderStickClimb, nullptr, 0, 0, 0, nullptr },
            { "GRAB GESTURES", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Grip at shoulders (left: save)", RowKind::Toggle, "ShoulderGestures", &bmvr::g_ShoulderGestures, nullptr, 0, 0, 0, nullptr },
            { "Grip at hips", RowKind::Toggle, "HipGestures", &bmvr::g_HipGestures, nullptr, 0, 0, 0, nullptr },
            // Which weapon each zone takes (when you carry it).
            { "Right shoulder takes", RowKind::Choice, "GestureWeaponShoulder", nullptr, &bmvr::g_GestureWeaponShoulder, 1.f, 0.f, 10.f, "Crowbar|Pistol|.357|MP5|Shotgun|Crossbow|RPG|Tau|Gluon|Hivehand|Grenade" },
            { "Right hip takes", RowKind::Choice, "GestureWeaponHipRight", nullptr, &bmvr::g_GestureWeaponHipRight, 1.f, 0.f, 10.f, "Crowbar|Pistol|.357|MP5|Shotgun|Crossbow|RPG|Tau|Gluon|Hivehand|Grenade" },
            { "Left hip takes", RowKind::Choice, "GestureWeaponHipLeft", nullptr, &bmvr::g_GestureWeaponHipLeft, 1.f, 0.f, 10.f, "Crowbar|Pistol|.357|MP5|Shotgun|Crossbow|RPG|Tau|Gluon|Hivehand|Grenade" },
            { "FLASHLIGHT", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Flashlight in left hand", RowKind::Toggle, "FlashlightLeftHand", &bmvr::g_FlashlightLeftHand, nullptr, 0, 0, 0, nullptr },
            { "VIEW", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Recenter view", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Recenter after pause menu", RowKind::Toggle, "AutoRecenterOnResume", &bmvr::g_AutoRecenterOnResume, nullptr, 0, 0, 0, nullptr },
            { "Cutscene cameras move the view", RowKind::Toggle, "FollowScriptedCamera", &bmvr::g_FollowScriptedCamera, nullptr, 0, 0, 0, nullptr },
            { "Near clip (lower = gun at the face clips later, 7 = game)", RowKind::Number, "VrNearZ", nullptr, &bmvr::g_VrNearZ, 0.5f, 1.f, 7.f, "%.1f" },
            { "MENUS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Weapon menu faces the eye", RowKind::Toggle, "WeaponMenuTilt", &bmvr::g_WeaponMenuTilt, nullptr, 0, 0, 0, nullptr },
            { "Game menu distance", RowKind::Number, "MenuForwardMeters", nullptr, &bmvr::g_MenuForwardMeters, 0.1f, 0.5f, 3.f, "%.1f m" },
            { "Game menu size", RowKind::Number, "MenuWidthMeters", nullptr, &bmvr::g_MenuWidthMeters, 0.1f, 0.6f, 3.f, "%.1f m" },
            { "Game menu height", RowKind::Number, "MenuHeightOffsetMeters", nullptr, &bmvr::g_MenuHeightOffsetMeters, 0.05f, -0.4f, 0.4f, "%+.2f m" },
            { "Menu pointer smoothing", RowKind::Number, "MenuCursorSmoothSec", nullptr, &bmvr::g_MenuCursorSmoothSec, 0.01f, 0.02f, 0.3f, "%.2f s" },
            { "THIS PANEL", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Hold Triangle/Y for this panel", RowKind::Toggle, "SettingsTriangleHold", &bmvr::g_SettingsTriangleHold, nullptr, 0, 0, 0, nullptr },
            // The gear only hides while the Triangle hold can open this panel.
            { "VR gear in weapon menu", RowKind::Toggle, "WeaponMenuSettingsGear", &bmvr::g_WeaponMenuSettingsGear, nullptr, 0, 0, 0, nullptr },
            // Glow fades and other effects run on game time: with the game
            // paused a change only shows after the panel closes.
            { "Panel pauses the game", RowKind::Toggle, "VrSettingsPause", &bmvr::g_VrSettingsPause, nullptr, 0, 0, 0, nullptr },
            { "CONTROLS", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Left column = left controller, right column = right controller. Headings name
            // the detected controller; button names follow it (A/B, Cross/Circle).
            { "LAYOUT", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Preset", RowKind::BindPreset, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Hold this long for a button's hold action; let go earlier for its press action.
            { "Hold time", RowKind::Number, "ButtonHoldMs", nullptr, &bmvr::g_ButtonHoldMs, 50.f, 200.f, 1500.f, "%.0f ms" },
            // Off: the fixed layout from before mapping (if a mapping ever blocks a button).
            { "Button mapping", RowKind::Toggle, "ButtonMapping", &bmvr::g_ButtonMapping, nullptr, 0, 0, 0, nullptr },
            // Off (or no SteamVR model found): the button list below.
            { "Controller pictures", RowKind::Toggle, "ControlsPicture", &bmvr::g_ControlsPicture, nullptr, 0, 0, 0, nullptr },
            { "Reset buttons to defaults", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "binds_default" },
            { "LEFT CONTROLLER", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, "bind_head_left" },
            { "", RowKind::BindPicture, nullptr, nullptr, nullptr, 0, 0, 0, "L" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.trigger" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.grip" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face1" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face1.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face2" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face2.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.stickclick" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.stickclick.hold" },
            { "Stick", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "fixedL:Move" },
            { "Menu button", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "fixedL:Pause" },
            { "", RowKind::ColumnBreak, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "STICK UP / DOWN", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Stick distance", RowKind::Number, "StickJumpCrouchThreshold", nullptr, &bmvr::g_StickJumpCrouchThreshold, 0.05f, 0.3f, 0.95f, "%.2f" },
            { "Stick angle", RowKind::Number, "StickJumpCrouchAngle", nullptr, &bmvr::g_StickJumpCrouchAngle, 5.f, 10.f, 45.f, "%.0f\xB0" },
            { "Turning blocks up/down", RowKind::Toggle, "TurnBlocksJumpCrouch", &bmvr::g_TurnBlocksJumpCrouch, nullptr, 0, 0, 0, nullptr },
            { "Stick down: crouch while held", RowKind::Toggle, "CrouchStickHold", &bmvr::g_CrouchStickHold, nullptr, 0, 0, 0, nullptr },
            { "RIGHT CONTROLLER", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, "bind_head_right" },
            { "", RowKind::BindPicture, nullptr, nullptr, nullptr, 0, 0, 0, "R" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.trigger" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.grip" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face1" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face1.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face2" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face2.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.stickclick" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.stickclick.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.stickup" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.stickdown" },
            { "Stick sideways", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "fixedR:Turn" },
            { "HANDS & WEAPONS", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "HUD", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Health/ammo on the hands", RowKind::Toggle, "VrHandHud", &bmvr::g_HandHud, nullptr, 0, 0, 0, nullptr },
            { "Red edges when hit", RowKind::Toggle, "DamageFlash", &bmvr::g_DamageFlash, nullptr, 0, 0, 0, nullptr },
            { "Hit flash strength", RowKind::Number, "DamageFlashStrength", nullptr, &bmvr::g_DamageFlashStrength, 0.1f, 0.1f, 1.f, "%.1f" },
            { "HANDS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // The original release's look: flat gloves, HUD on the back of the hand.
            // Built from the installed game's own first-person arms; nothing shipped.
            { "Black Mesa hands", RowKind::Toggle, "VrHandsBlackMesa", &bmvr::g_VrHandsBlackMesa, nullptr, 0, 0, 0, nullptr },
            { "Black Mesa hand size", RowKind::Number, "VrHandsBlackMesaScale", nullptr, &bmvr::g_VrHandsBlackMesaScale, 0.05f, 0.4f, 1.5f, "%.2f" },
            { "Manual reloads (mags, shells, RPG)", RowKind::Toggle, "VrMagReload", &bmvr::g_VrMagReload, nullptr, 0, 0, 0, nullptr },
            { "Grenade pin by hand (with manual magazines)", RowKind::Toggle, "VrGrenadePin", &bmvr::g_VrGrenadePin, nullptr, 0, 0, 0, nullptr },
            { "Reload hints (pump / charging handle)", RowKind::Toggle, "VrReloadHints", &bmvr::g_VrReloadHints, nullptr, 0, 0, 0, nullptr },
            { "Gun's own right hand", RowKind::Toggle, "VrGunRightHand", &bmvr::g_VrGunRightHand, nullptr, 0, 0, 0, nullptr },
            { "Support hand: gun's finger pose + pinch", RowKind::Toggle, "VrStockHandPose", &bmvr::g_VrStockHandPose, nullptr, 0, 0, 0, nullptr },
            { "Classic flat hands + HUD", RowKind::Toggle, "VrHandsClassicPose", &bmvr::g_VrHandsClassicPose, nullptr, 0, 0, 0, nullptr },
            { "Hand roll (thumb up)", RowKind::Number, "VrHandsRollDeg", nullptr, &bmvr::g_VrHandsRollDeg, 5.f, -135.f, 135.f, "%+.0f\xB0" },
            { "Hand on gun: right", RowKind::Number, "VrHandsGunGripRight", nullptr, &bmvr::g_VrHandsGunGripRight, 0.25f, -8.f, 8.f, "%+.2f" },
            { "Hand on gun: up", RowKind::Number, "VrHandsGunGripUp", nullptr, &bmvr::g_VrHandsGunGripUp, 0.25f, -8.f, 8.f, "%+.2f" },
            { "Hand on gun: forward", RowKind::Number, "VrHandsGunGripForward", nullptr, &bmvr::g_VrHandsGunGripForward, 0.25f, -12.f, 12.f, "%+.2f" },
            { "G-Man gesture", RowKind::Toggle, "VrGmanGesture", &bmvr::g_VrGmanGesture, nullptr, 0, 0, 0, nullptr },
            { "WEAPONS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Recoil (less with two hands)", RowKind::Toggle, "VrRecoil", &bmvr::g_VrRecoil, nullptr, 0, 0, 0, nullptr },
            { "Recoil strength", RowKind::Number, "VrRecoilScale", nullptr, &bmvr::g_VrRecoilScale, 0.25f, 0.25f, 3.f, "%.2f" },
            { "HUD POSITION", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "HUD size", RowKind::Number, "HandHudSize", nullptr, &bmvr::g_HandHudSize, 0.25f, 1.f, 6.f, "%.2f" },
            { "Up from the hand", RowKind::Number, "HandHudUp", nullptr, &bmvr::g_HandHudUp, 0.25f, -4.f, 8.f, "%+.2f" },
            { "Forward (- = to the wrist)", RowKind::Number, "HandHudForward", nullptr, &bmvr::g_HandHudForward, 0.25f, -8.f, 8.f, "%+.2f" },
            { "Sideways (+ = left)", RowKind::Number, "HandHudSide", nullptr, &bmvr::g_HandHudSide, 0.25f, -6.f, 6.f, "%+.2f" },
            { "Tilt", RowKind::Number, "HandHudTilt", nullptr, &bmvr::g_HandHudTilt, 2.f, -90.f, 90.f, "%+.0f\xB0" },
            { "Roll toward you", RowKind::Number, "HandHudRoll", nullptr, &bmvr::g_HandHudRoll, 2.f, -90.f, 90.f, "%+.0f\xB0" },
            // Resets the HANDS and HUD POSITION rows above (also turns classic hands off).
            { "Reset hands + HUD position", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "handhud_default" },
            { "AIMING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Aim marker", RowKind::Toggle, "VrCrosshair", &bmvr::g_VrCrosshair, nullptr, 0, 0, 0, nullptr },
            { "Marker as red dot", RowKind::Toggle, "VrCrosshairDot", &bmvr::g_VrCrosshairDot, nullptr, 0, 0, 0, nullptr },
            { "Marker size", RowKind::Number, "VrCrosshairScale", nullptr, &bmvr::g_VrCrosshairScale, 0.1f, 0.2f, 3.f, "%.1f" },
            { "Laser beam", RowKind::Toggle, "VrLaserBeam", &bmvr::g_VrLaserBeam, nullptr, 0, 0, 0, nullptr },
            { "Laser width", RowKind::Number, "VrLaserWidth", nullptr, &bmvr::g_VrLaserWidth, 0.01f, 0.01f, 0.3f, "%.2f" },
            { "Gun angle", RowKind::Number, "GunPitchOffset", nullptr, &bmvr::g_GunPitchOffset, 1.f, -45.f, 45.f, "%+.0f\xB0" },
            { "Two-hand grip toggles", RowKind::Toggle, "TwoHandGripToggle", &bmvr::g_TwoHandGripToggle, nullptr, 0, 0, 0, nullptr },
            { "CROSSBOW SCOPE", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Zoom follows the gun", RowKind::Toggle, "ScopeFollowsGun", &bmvr::g_ScopeFollowsGun, nullptr, 0, 0, 0, nullptr },
            { "Raise to face zooms (2 hands)", RowKind::Toggle, "ScopeRaiseToZoom", &bmvr::g_ScopeRaiseToZoom, nullptr, 0, 0, 0, nullptr },
            { "Raise distance", RowKind::Number, "ScopeRaiseMeters", nullptr, &bmvr::g_ScopeRaiseMeters, 0.02f, 0.1f, 0.6f, "%.2f m" },
            { "Zoom smoothing", RowKind::Number, "ScopeGunSmoothSec", nullptr, &bmvr::g_ScopeGunSmoothSec, 0.02f, 0.f, 0.5f, "%.2f s" },
            { "GRENADES", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Throw at hand speed", RowKind::Toggle, "VrHandThrow", &bmvr::g_VrHandThrow, nullptr, 0, 0, 0, nullptr },
            { "Throw strength", RowKind::Number, "ThrowScale", nullptr, &bmvr::g_ThrowScale, 0.25f, 1.f, 8.f, "%.2f" },
            { "Throw from whole stroke (test)", RowKind::Toggle, "ThrowStroke", &bmvr::g_ThrowStroke, nullptr, 0, 0, 0, nullptr },
            { "MOUNTED GUNS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Turn per hand move", RowKind::Number, "MountedGunGain", nullptr, &bmvr::g_MountedGunGain, 0.25f, 0.5f, 4.f, "%.2fx" },
            { "GRAPHICS", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "PERFORMANCE", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Frame cap", RowKind::Choice, "FrameCap", nullptr, &bmvr::g_FrameCapMode, 1.f, 0.f, 2.f, "Off|Headset +10|2x headset" },
            // Coarser shading away from the centre: fixed = around each lens centre,
            // eye-tracked = where you look (lens centre while there is no gaze). The GPU
            // feature turns on at game start when this is not off; "Foveation now" says when a restart is needed.
            { "Foveated rendering", RowKind::Choice, "Foveated", nullptr, &bmvr::g_Foveated, 1.f, 0.f, 2.f, "Off|Fixed|Eye-tracked" },
            { "Foveation now", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "foveation" },
            { "Foveated: sharp radius", RowKind::Number, "FoveatedInner", nullptr, &bmvr::g_FoveatedInner, 0.05f, 0.1f, 1.f, "%.2f" },
            { "Foveated: half-rate radius", RowKind::Number, "FoveatedMid", nullptr, &bmvr::g_FoveatedMid, 0.05f, 0.2f, 1.5f, "%.2f" },
            { "ANTI-ALIASING (MSAA)", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "MSAA on edges", RowKind::Choice, "Msaa", nullptr, &bmvr::g_MsaaF, 2.f, 0.f, 4.f, "Off|2x|4x" },
            // Leaves, grass, fences: their cut-out edges get the MSAA samples
            // too (softer, less crawl; can look a bit thinner). Needs MSAA on.
            { "MSAA on leaves + fences", RowKind::Toggle, "AlphaToCoverage", &bmvr::g_AlphaToCoverage, nullptr, 0, 0, 0, nullptr },
            // Smoke, sparks and drips are drawn last, after the MSAA resolve:
            // much cheaper where particles fill the view. Models, wires and
            // glass keep MSAA; the gun loses it only while particles are in view.
            { "MSAA skips particles", RowKind::Toggle, "MsaaParticles1x", &bmvr::g_MsaaParticles1x, nullptr, 0, 0, 0, nullptr },
            { "ANTI-ALIASING (FXAA)", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Use these settings", RowKind::Toggle, "FxaaApply", &bmvr::g_FxaaApply, nullptr, 0, 0, 0, nullptr },
            // Sets the three values below; "Custom" when they match no preset.
            { "Preset", RowKind::Preset, nullptr, nullptr, nullptr, 0, 0, 0, "fxaa" },
            { "Sub-pixel smoothing", RowKind::Number, "FxaaSubpixel", nullptr, &bmvr::g_FxaaSubpixel, 0.05f, 0.f, 1.f, "%.2f" },
            { "Edge threshold", RowKind::Number, "FxaaEdgeThreshold", nullptr, &bmvr::g_FxaaEdgeThreshold, 0.01f, 0.f, 0.5f, "%.3f" },
            { "Dark edge threshold", RowKind::Number, "FxaaEdgeThresholdMin", nullptr, &bmvr::g_FxaaEdgeThresholdMin, 0.005f, 0.f, 0.1f, "%.3f" },
            { "IMAGE", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Bloom and auto-exposure", RowKind::Toggle, "EyeBloom", &bmvr::g_EyeBloom, nullptr, 0, 0, 0, nullptr },
            // Mip LOD bias in the 3D views (experimental): + = softer textures;
            // foliage and fences look more solid, at the cost of detail. Does
            // not fix the shimmering sand outdoors. Menus and the HUD stay sharp.
            { "Texture calm (LOD bias)", RowKind::Number, "LodBias", nullptr, &bmvr::g_LodBias, 0.25f, -0.5f, 2.f, "%+.2f" },
            { "Smoke stays upright", RowKind::Toggle, "ParticleLevel", &bmvr::g_ParticleLevel, nullptr, 0, 0, 0, nullptr },
            { "LENS FLARES", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Fade out at angle", RowKind::Number, "FlareFadeAngle", nullptr, &bmvr::g_FlareFadeAngle, 5.f, 0.f, 90.f, "%.0f\xB0" },
            { "Strength on the light", RowKind::Number, "FlareLightScale", nullptr, &bmvr::g_FlareLightScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "Large round haze", RowKind::Number, "FlareHazeScale", nullptr, &bmvr::g_FlareHazeScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "Haze from size", RowKind::Number, "FlareHazeMinSize", nullptr, &bmvr::g_FlareHazeMinSize, 0.05f, 0.05f, 4.f, "%.2f" },
            { "Ghosts away from light", RowKind::Number, "FlareGhostScale", nullptr, &bmvr::g_FlareGhostScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "Reset flare values", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "flare_default" },
            { "LIGHT GLOWS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Glows level, face you", RowKind::Toggle, "SpriteLevel", &bmvr::g_SpriteLevel, nullptr, 0, 0, 0, nullptr },
            { "Glow smoothing", RowKind::Number, "GlowSmooth", nullptr, &bmvr::g_GlowSmooth, 0.05f, 0.f, 0.5f, "%.2f s" },
            { "Dark when hidden after", RowKind::Number, "GlowHideMs", nullptr, &bmvr::g_GlowHideMs, 10.f, 0.f, 500.f, "%.0f ms" },
            { "Reset glow values", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "glow_default" },
            // Cubemap reflection strength: less = less shimmer on shiny floors/walls and props.
            { "REFLECTIONS (SHIMMER)", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Preset", RowKind::Preset, nullptr, nullptr, nullptr, 0, 0, 0, "envmap" },
            { "Walls/floors (cubemap)", RowKind::Number, "EnvmapScaleWorld", nullptr, &bmvr::g_EnvmapScaleWorld, 0.1f, 0.f, 1.f, "%.1f" },
            { "Props/NPCs (cubemap)", RowKind::Number, "EnvmapScaleModels", nullptr, &bmvr::g_EnvmapScaleModels, 0.1f, 0.f, 1.f, "%.1f" },
            { "DEBUG", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // One section, so "Show developer rows" keeps its spot (left column) when the rows below appear.
            { "DIAGNOSTICS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Offsets that no longer match the game: gameplay ones also raise the "VR MOD OUT
            // OF DATE" toast, debug-only ones (PerfLog stats, profiling) only show here.
            { "Missing hooks", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "missing_hooks" },
            { "Capture debug frame", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "debug_capture" },
            { "Performance log (costs fps)", RowKind::Toggle, "PerfLog", &bmvr::g_PerfLog, nullptr, 0, 0, 0, nullptr },
            // Eye GPU ms every ~5 s (timestamp queries); with the always-on "Frame budget" CPU line it shows
            // whether a scene is CPU- or GPU-limited.
            { "GPU timing in log", RowKind::Toggle, "GpuPassTiming", &bmvr::g_GpuPassTiming, nullptr, 0, 0, 0, nullptr },
            // Shows the DevSection groups below (A/B switches, tests, engine cvars); saved.
            { "Show developer rows", RowKind::Toggle, "VrPanelDevRows", &bmvr::g_PanelDevRows, nullptr, 0, 0, 0, nullptr },
            // Every row of this tab back to its built-in default (logs, A/B fixes, tests, engine rows;
            // god/noclip/notarget off). "Show developer rows" stays as it is.
            { "Reset dev rows", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "dev_default" },
            // CPU/GPU work removed without changing the image; off = the stock path, for A/B.
            { "SAVINGS (SAME IMAGE)", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Post copies: swap, no copy", RowKind::Toggle, "PostCopySwap", &bmvr::g_PostCopySwap, nullptr, 0, 0, 0, nullptr },
            { "Light shadows: once per frame", RowKind::Toggle, "ShadowsOncePerFrame", &bmvr::g_ShadowsOncePerFrame, nullptr, 0, 0, 0, nullptr },
            { "Glass refraction once per eye", RowKind::Toggle, "RefractOncePerEye", &bmvr::g_RefractOncePerEye, nullptr, 0, 0, 0, nullptr },
            // Capture/view tests are not saved (no key): always off after a restart.
            { "LOGS", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Lens flare positions per eye into the log; switch on when a flare sits in the wrong place.
            { "Flare position log", RowKind::Toggle, nullptr, &bmvr::g_FlareLog, nullptr, 0, 0, 0, nullptr },
            { "Glow trace (20 s)", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "glow_trace" },
            { "VIEW TESTS", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Freeze head pose", RowKind::Toggle, nullptr, &bmvr::g_FreezeHead, nullptr, 0, 0, 0, nullptr },
            // Skips Black Mesa's Xen fog (Xog) draws by material: global = __gbxog0,
            // volumes = the local fog volumes. Draw hooks run while on.
            { "Hide Xen fog", RowKind::Choice, nullptr, nullptr, &bmvr::g_DebugHideXog, 1.f, 0.f, 3.f, "Off|Global|Volumes|Both" },
            { "Blank eye", RowKind::Choice, nullptr, nullptr, &bmvr::g_DebugBlankEye, 1.f, 0.f, 2.f, "Off|Left|Right" },
            // Experiments; saved like the other settings, off by default.
            { "TESTS", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Glow test area size", RowKind::Number, "GlowProxyScale", nullptr, &bmvr::g_GlowProxyScale, 0.5f, 1.f, 8.f, "%.1fx" },
            { "Head hold: angle", RowKind::Number, "PoseHoldDeg", nullptr, &bmvr::g_PoseHoldDeg, 0.02f, 0.f, 1.f, "%.2f\xB0" },
            { "Head hold: distance", RowKind::Number, "PoseHoldMm", nullptr, &bmvr::g_PoseHoldMm, 0.5f, 0.f, 20.f, "%.1f mm" },
            // Engine cvars: "cvar:" keys go to the console on the game thread
            // (never saved); the shown value is read back while the panel is open.
            { "ENGINE (CONSOLE)", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "sv_cheats", RowKind::Toggle, "cvar:sv_cheats", &g_EngSvCheats, nullptr, 0, 0, 0, nullptr },
            { "r_occlusion", RowKind::Toggle, "cvar:r_occlusion", &g_EngOcclusion, nullptr, 0, 0, 0, nullptr },
            // Black Mesa's GPU-query occlusion (engine/occlusionproxy draws + query readback, per eye).
            { "r_occlusion_tracker_enabled", RowKind::Toggle, "cvar:r_occlusion_tracker_enabled", &g_EngOccTracker, nullptr, 0, 0, 0, nullptr },
            { "r_occlusion_tracker_shadowdepthview_enabled", RowKind::Toggle, "cvar:r_occlusion_tracker_shadowdepthview_enabled", &g_EngOccTrackerShadow, nullptr, 0, 0, 0, nullptr },
            { "r_dopixelvisibility", RowKind::Toggle, "cvar:r_dopixelvisibility", &g_EngPixelVis, nullptr, 0, 0, 0, nullptr },
            { "r_3dsky", RowKind::Toggle, "cvar:r_3dsky", &g_Eng3dSky, nullptr, 0, 0, 0, nullptr },
            { "rope_smooth_minwidth", RowKind::Number, "cvar:rope_smooth_minwidth", nullptr, &g_EngRopeMinWidth, 0.1f, 0.f, 3.f, "%.1f" },
            // Turning one on also sets sv_cheats 1.
            { "God mode", RowKind::Toggle, "cmd:god", &g_DevGod, nullptr, 0, 0, 0, nullptr },
            { "No clip", RowKind::Toggle, "cmd:noclip", &g_DevNoclip, nullptr, 0, 0, 0, nullptr },
            { "No target", RowKind::Toggle, "cmd:notarget", &g_DevNotarget, nullptr, 0, 0, 0, nullptr },
            // sv_cheats 1 + impulse 101: all weapons and ammo (applied when the game runs again).
            { "All weapons", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "impulse101" },
            // bmvr_* fixes and tests (on/off for A/B); not saved: a restart
            // brings back the defaults.
            { "FIXES: RENDERING", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Glow queries per eye", RowKind::Toggle, nullptr, &bmvr::g_PixelVisPerEye, nullptr, 0, 0, 0, nullptr },
            { "Glow through objects fix", RowKind::Toggle, nullptr, &bmvr::g_GlowCountDepthFix, nullptr, 0, 0, 0, nullptr },
            { "Glow hold", RowKind::Number, nullptr, nullptr, &bmvr::g_GlowHold, 0.1f, 0.f, 2.f, "%.1f s" },
            { "Lights culled per eye", RowKind::Toggle, nullptr, &bmvr::g_LightCullEyeView, nullptr, 0, 0, 0, nullptr },
            { "Flares: one layout", RowKind::Toggle, nullptr, &bmvr::g_FlareSharedLayout, nullptr, 0, 0, 0, nullptr },
            { "Flares level with horizon", RowKind::Toggle, nullptr, &bmvr::g_FlareLevel, nullptr, 0, 0, 0, nullptr },
            { "Flares on each eye's axis", RowKind::Toggle, nullptr, &bmvr::g_FlareEyeAxis, nullptr, 0, 0, 0, nullptr },
            { "Sky water per eye", RowKind::Toggle, nullptr, &bmvr::g_SkyWaterPerEye, nullptr, 0, 0, 0, nullptr },
            { "Underwater world fix", RowKind::Toggle, nullptr, &bmvr::g_EyeUnderwaterWorld, nullptr, 0, 0, 0, nullptr },
            { "Water line: no clip", RowKind::Toggle, nullptr, &bmvr::g_WaterLineClipFix, nullptr, 0, 0, 0, nullptr },
            { "Screen quads: identity model", RowKind::Toggle, nullptr, &bmvr::g_ScreenSpaceModelFix, nullptr, 0, 0, 0, nullptr },
            { "Xen ground fog off", RowKind::Toggle, nullptr, &bmvr::g_XenGlobalFogOff, nullptr, 0, 0, 0, nullptr },
            { "Rope normals fix", RowKind::Toggle, nullptr, &bmvr::g_ViewProjNormalFix, nullptr, 0, 0, 0, nullptr },
            { "Particles after water", RowKind::Toggle, "ParticlesLast", &bmvr::g_ParticlesLast, nullptr, 0, 0, 0, nullptr },
            { "Titles level in the room", RowKind::Toggle, "TitleLevel", &bmvr::g_TitleLevel, nullptr, 0, 0, 0, nullptr },
            { "FIXES: TRACKING, WEAPONS, GAME", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Pose valid never blinks", RowKind::Toggle, nullptr, &bmvr::g_PoseFlagsNoBlink, nullptr, 0, 0, 0, nullptr },
            { "Hands + laser per-frame snapshot", RowKind::Toggle, nullptr, &bmvr::g_McHandSnapshot, nullptr, 0, 0, 0, nullptr },
            { "No stun/blur overlays", RowKind::Toggle, nullptr, &bmvr::g_VrSkipScreenEffects, nullptr, 0, 0, 0, nullptr },
            { "Guns skip draw animation", RowKind::Toggle, nullptr, &bmvr::g_SkipDrawAnim, nullptr, 0, 0, 0, nullptr },
            { "Tau/crossbow switch fix", RowKind::Toggle, nullptr, &bmvr::g_DeltaSeqIdle, nullptr, 0, 0, 0, nullptr },
            { "Two-hand: gun not lowered", RowKind::Toggle, nullptr, &bmvr::g_TwoHandNoLower, nullptr, 0, 0, 0, nullptr },
            { "Support hand follows lowered gun", RowKind::Toggle, nullptr, &bmvr::g_TwoHandLoweredFollow, nullptr, 0, 0, 0, nullptr },
            { "No crossbow scope animation", RowKind::Toggle, nullptr, &bmvr::g_ScopeClipIdle, nullptr, 0, 0, 0, nullptr },
            { "RPG rocket shown when loaded", RowKind::Toggle, nullptr, &bmvr::g_RpgRocketShown, nullptr, 0, 0, 0, nullptr },
            { "Mute weapon fidget sounds", RowKind::Toggle, nullptr, &bmvr::g_MuteFidgetSounds, nullptr, 0, 0, 0, nullptr },
            { "Muzzle flash follows gun", RowKind::Toggle, nullptr, &bmvr::g_ViewmodelFxCatchUp, nullptr, 0, 0, 0, nullptr },
            { "LOADING card", RowKind::Toggle, nullptr, &bmvr::g_LoadingCard, nullptr, 0, 0, 0, nullptr },
            // Live per-weapon angle trim for the weapon in the hand (saved in
            // VR/viewmodel_offsets.txt by VR::SyncHeldWeaponAngleRows).
            { "HELD WEAPON TUNING (SAVED PER WEAPON)", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Support hand (+ forward)", RowKind::Number, nullptr, nullptr, &bmvr::g_VmTuneSupportCm, 1.f, -20.f, 20.f, "%+.0f cm" },
            { "Pitch", RowKind::Number, nullptr, nullptr, &bmvr::g_VmTunePitch, 2.5f, -90.f, 90.f, "%.1f deg" },
            { "Yaw", RowKind::Number, nullptr, nullptr, &bmvr::g_VmTuneYaw, 2.5f, -90.f, 90.f, "%.1f deg" },
            { "Roll", RowKind::Number, nullptr, nullptr, &bmvr::g_VmTuneRoll, 2.5f, -90.f, 90.f, "%.1f deg" },
            { "G-MAN GESTURE TUNING", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Force gesture (tuning)", RowKind::Toggle, nullptr, &bmvr::g_VrGmanGestureForce, nullptr, 0, 0, 0, nullptr },
            { "Finger straight", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanFingerStraight, 0.05f, 0.f, 0.5f, "%.2f" },
            { "Thumb curl", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanThumbCurl, 0.1f, 0.f, 2.f, "%.1f" },
            { "Thumb swing", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanThumbSwing, 0.1f, 0.f, 1.5f, "%.1f" },
            // Drawn at the bottom of every tab.
            { "Close", RowKind::Close, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
        };
        return rows;
    }

    int PictureAreaHeight(int hand);
    int RowHeight(const Row& r)
    {
        if (r.kind == RowKind::ColumnBreak)
            return 0;
        if (r.kind == RowKind::BindPicture)
            return PictureAreaHeight(r.fmt && r.fmt[0] == 'R' ? 1 : 0);
        return IsHeading(r.kind) ? kSectionPx : kRowPx;
    }

    // Controller the CONTROLS tab shows (VR::SetPanelController from the input state).
    uint32_t g_PanelFamily = 0;
    uint32_t g_PanelRaw[2] = {};
    char g_PanelSystem[64] = {};
    bool g_BindsChanged = false;     // saved on close (Bind.<family>.* keys)
    int g_PickerRow = -1;            // Bind row whose action picker is open

    struct BindRowSpec { int hand = 0; bmvr::BindInput input = bmvr::BindInput::Trigger; bool hold = false; bool ok = false; };
    BindRowSpec ParseBindRow(const Row& r)
    {
        BindRowSpec b;
        if (r.kind != RowKind::Bind || !r.fmt || (r.fmt[0] != 'L' && r.fmt[0] != 'R') || r.fmt[1] != '.')
            return b;
        b.hand = r.fmt[0] == 'L' ? 0 : 1;
        std::string rest = r.fmt + 2;
        const size_t dot = rest.find('.');
        b.hold = dot != std::string::npos && rest.compare(dot + 1, std::string::npos, "hold") == 0;
        if (dot != std::string::npos)
            rest.resize(dot);
        for (int i = 0; i < bmvr::kBindInputCount; ++i)
            if (rest == bmvr::BindInputToken(static_cast<bmvr::BindInput>(i)))
            {
                b.input = static_cast<bmvr::BindInput>(i);
                b.ok = true;
            }
        return b;
    }
    bool BindRowHidden(const Row& r)
    {
        const BindRowSpec b = ParseBindRow(r);
        return !b.ok || !bmvr::BindInputExists(g_PanelFamily, g_PanelRaw[b.hand], b.hand, b.input);
    }
    std::string BindRowLabel(const Row& r)
    {
        const BindRowSpec b = ParseBindRow(r);
        if (!b.ok)
            return r.label;
        std::string s = bmvr::BindInputLabel(g_PanelFamily, g_PanelSystem, b.hand, b.input);
        if (b.hold)
            s += " (hold)";
        return s;
    }
    bmvr::BindAction BindRowAction(const Row& r, bool defaultOnly = false)
    {
        const BindRowSpec b = ParseBindRow(r);
        const bmvr::BindSlot s = defaultOnly ? bmvr::DefaultBind(g_PanelFamily, b.hand, b.input)
                                             : bmvr::EffectiveBind(g_PanelFamily, b.hand, b.input);
        return b.hold ? s.hold : s.press;
    }
    const char* ControllerName()
    {
        if (L4D2VR_TextContainsI(g_PanelSystem, "PlayStation") || L4D2VR_TextContainsI(g_PanelSystem, "PS VR2"))
            return "PS VR2 Sense";
        switch (g_PanelFamily)
        {
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES: return "Index";
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2: return "WMR";
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE: return "Vive";
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH: return "Touch";
        default: return "not detected yet";
        }
    }
    std::string SectionText(const Row& r)
    {
        if (r.fmt && std::strcmp(r.fmt, "bind_head_left") == 0)
            return std::string("LEFT CONTROLLER (") + ControllerName() + ")";
        if (r.fmt && std::strcmp(r.fmt, "bind_head_right") == 0)
            return std::string("RIGHT CONTROLLER (") + ControllerName() + ")";
        return r.label;
    }
    std::string BindPresetText()
    {
        const int i = bmvr::MatchBindPreset(g_PanelFamily);
        return i < 0 ? "Custom" : bmvr::BindPresetName(i);
    }
    // Returns true when the detected controller changed (relayout + repaint).
    bool SetPanelController(const L4D2VROpenXrInputStateDesc& in)
    {
        const uint32_t raw0 = in.rawHands[0].valid ? (in.rawHands[0].buttons & 0xFFFF0000u) : 0u;
        const uint32_t raw1 = in.rawHands[1].valid ? (in.rawHands[1].buttons & 0xFFFF0000u) : 0u;
        const bool changed = g_PanelFamily != in.reserved0 || g_PanelRaw[0] != raw0 || g_PanelRaw[1] != raw1
            || std::strncmp(g_PanelSystem, in.systemName, sizeof(g_PanelSystem)) != 0;
        g_PanelFamily = in.reserved0;
        g_PanelRaw[0] = raw0;
        g_PanelRaw[1] = raw1;
        std::memcpy(g_PanelSystem, in.systemName, sizeof(g_PanelSystem));
        g_PanelSystem[sizeof(g_PanelSystem) - 1] = 0;
        return changed;
    }

    std::string InfoText(const Row& r);

    // ---- Controller pictures (CONTROLS tab) ----
    // Like the PS5 button assignment screen: each controller from its thumb
    // side, drawn beside the panel (left one past the left edge, right one past
    // the right edge, in "wings" of the panel texture), so the panel keeps its
    // size. Inside, one card per button; a line runs from each card out of the
    // panel to its button. Card lines are the hand's Bind rows (same picker).
    constexpr int kPicW = 460, kPicH = 760;
    constexpr int kWingPx = kPicW + 40;
    constexpr int kCardX0 = 40, kCardW = kColW - 80, kCardHeadPx = 38, kCardLinePx = 46, kCardGap = 10;

    std::shared_ptr<const bmvr::ctlmodel::Models> g_PanelModels;
    int g_PanelModelsGen = -1;
    const bmvr::ctlmodel::Models* PanelModels()
    {
        const int gen = bmvr::ctlmodel::Generation();
        if (gen != g_PanelModelsGen)
        {
            g_PanelModels = bmvr::ctlmodel::Current();
            g_PanelModelsGen = gen;
        }
        return g_PanelModels.get();
    }
    // That hand's column shows the picture instead of its button rows.
    bool PictureShown(int hand)
    {
        if (!bmvr::g_ControlsPicture || hand < 0 || hand > 1)
            return false;
        const bmvr::ctlmodel::Models* m = PanelModels();
        return m && m->hand[hand].ok;
    }
    // Hand of a Bind / BindPicture / fixed Info row, else -1.
    int RowHand(const Row& r)
    {
        if (!r.fmt)
            return -1;
        if (r.kind == RowKind::Bind || r.kind == RowKind::BindPicture)
            return r.fmt[0] == 'L' ? 0 : r.fmt[0] == 'R' ? 1 : -1;
        if (r.kind == RowKind::Info && std::strncmp(r.fmt, "fixedL:", 7) == 0)
            return 0;
        if (r.kind == RowKind::Info && std::strncmp(r.fmt, "fixedR:", 7) == 0)
            return 1;
        return -1;
    }

    struct CardLine
    {
        int row = -1;            // Bind row (clickable) or the fixed Info row
        std::string caption;     // "Hold", "Up", ... ("" = press)
        std::string text;        // action
        bool changed = false;
        bool fixed = false;
    };
    struct Card
    {
        int part = 0;
        std::string head;
        std::vector<CardLine> lines;
        float ax = 0.f, ay = 0.f;   // anchor, column-local (area top = 0)
        int y0 = 0, y1 = 0;
    };
    // Picture position in column-local x (outside the column: in the wing).
    int PicX(int hand) { return hand == 0 ? 20 - kWingPx : kColW + 20; }
    int PictureTop(int hand);   // picture y (area-local): its buttons level with the card stack
    int CardX0(int) { return kCardX0; }
    // Leader lines: card's outer edge -> elbow just past the panel edge -> button.
    int LeaderElbowX(int hand) { return hand == 0 ? -18 : kColW + 18; }

    std::vector<Card> PictureCards(int hand, int* picTopOut = nullptr)
    {
        std::vector<Card> cards;
        const bmvr::ctlmodel::Models* m = PanelModels();
        if (!m || !m->hand[hand].ok)
            return cards;
        const bmvr::ctlmodel::HandModel& hm = m->hand[hand];
        auto cardFor = [&](int part, const std::string& head) -> Card& {
            for (Card& c : cards)
                if (c.part == part)
                    return c;
            Card c;
            c.part = part;
            c.head = head;
            c.ax = static_cast<float>(PicX(hand)) + hm.picture.anchorX[part];
            c.ay = hm.picture.anchorY[part];   // picture-local until the stack is known
            cards.push_back(c);
            return cards.back();
        };
        const bool pad = g_PanelFamily == L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE;
        const auto& rows = Rows();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            const Row& r = rows[i];
            if (RowHand(r) != hand)
                continue;
            if (r.kind == RowKind::Bind)
            {
                if (BindRowHidden(r))
                    continue;
                const BindRowSpec b = ParseBindRow(r);
                int part = bmvr::ctlmodel::kStick;
                std::string caption;
                switch (b.input)
                {
                case bmvr::BindInput::Trigger: part = bmvr::ctlmodel::kTrigger; break;
                case bmvr::BindInput::Grip: part = bmvr::ctlmodel::kGrip; break;
                case bmvr::BindInput::Face1: part = bmvr::ctlmodel::kFace1; break;
                case bmvr::BindInput::Face2: part = bmvr::ctlmodel::kFace2; break;
                case bmvr::BindInput::StickClick: caption = "Click"; break;
                case bmvr::BindInput::StickUp: caption = "Up"; break;
                case bmvr::BindInput::StickDown: caption = "Down"; break;
                default: break;
                }
                if (b.hold)
                    caption = caption.empty() ? "Hold" : caption + " hold";
                if (!hm.has[part])
                    part = bmvr::ctlmodel::kStick;   // drawn without a line if that is missing too
                const std::string head = part == bmvr::ctlmodel::kStick ? (pad ? "Pad" : "Stick")
                    : bmvr::BindInputLabel(g_PanelFamily, g_PanelSystem, hand, b.input);
                CardLine line;
                line.row = i;
                line.caption = caption;
                const bmvr::BindAction a = BindRowAction(r);
                line.text = bmvr::BindActionLabel(a);
                line.changed = a != BindRowAction(r, true);
                cardFor(part, head).lines.push_back(line);
            }
            else if (r.kind == RowKind::Info)
            {
                const bool menu = std::strcmp(r.label, "Menu button") == 0;
                // The menu button (PS VR2: Create) cannot be mapped: left out of the picture.
                if (menu)
                    continue;
                const int part = menu ? bmvr::ctlmodel::kMenu : bmvr::ctlmodel::kStick;
                CardLine line;
                line.row = i;
                line.caption = menu ? "" : std::strcmp(r.label, "Stick sideways") == 0 ? "Sideways" : "Tilt";
                line.text = InfoText(r);
                line.fixed = true;
                const bool ps = L4D2VR_TextContainsI(g_PanelSystem, "PlayStation") || L4D2VR_TextContainsI(g_PanelSystem, "PS VR2");
                cardFor(part, menu ? (ps ? "Create" : "Menu") : (pad ? "Pad" : "Stick")).lines.push_back(line);
            }
        }
        // What the grip grabs besides its mapped action (gesture zones, ladders,
        // the support hand): fixed lines, so a "Nothing" grip is not misread.
        for (Card& c : cards)
        {
            if (c.part != bmvr::ctlmodel::kGrip)
                continue;
            auto fixedLine = [&](const char* caption, const std::string& text) {
                CardLine ln;
                ln.caption = caption;
                ln.text = text;
                ln.fixed = true;
                c.lines.push_back(ln);
            };
            std::string grab = hand == 0 ? "Gun with two hands" : "";
            if (bmvr::g_PhysicalLadders)
                grab += grab.empty() ? "Ladders" : ", ladders";
            if (!grab.empty())
                fixedLine("Grab", grab);
            if (hand == 0 && bmvr::g_ShoulderGestures)
                fixedLine("Shoulder", "Quick save");
            if (hand == 1)
            {
                std::string zones;
                if (bmvr::g_ShoulderGestures)
                    zones = std::string("Shoulder ") + bmvr::GestureWeaponName(static_cast<int>(bmvr::g_GestureWeaponShoulder));
                if (bmvr::g_HipGestures)
                {
                    zones += zones.empty() ? "" : ", ";
                    zones += std::string("hips ") + bmvr::GestureWeaponName(static_cast<int>(bmvr::g_GestureWeaponHipRight))
                        + " / " + bmvr::GestureWeaponName(static_cast<int>(bmvr::g_GestureWeaponHipLeft));
                }
                if (!zones.empty())
                    fixedLine("Take", zones);
            }
        }

        // Cards stack from the top of the area; the picture is moved so its
        // buttons sit level with the middle of the stack. Cards are ordered by
        // the angle of their line seen from the panel edge, so the lines fan
        // out without crossing; any pair that still crosses is swapped.
        int stackH = -kCardGap;
        for (const Card& c : cards)
            stackH += kCardHeadPx + static_cast<int>(c.lines.size()) * kCardLinePx + 6 + kCardGap;
        float meanY = 0.f;
        for (const Card& c : cards)
            meanY += c.ay;
        meanY = cards.empty() ? 0.f : meanY / static_cast<float>(cards.size());
        const int picTop = 8 + stackH / 2 - static_cast<int>(meanY);
        if (picTopOut)
            *picTopOut = picTop;
        for (Card& c : cards)
            c.ay += static_cast<float>(picTop);
        const float elbowX = static_cast<float>(LeaderElbowX(hand));
        const float midY = 8.f + stackH * 0.5f;
        std::stable_sort(cards.begin(), cards.end(), [&](const Card& a, const Card& b) {
            const float sa = (a.ay - midY) / (std::max)(1.f, std::fabs(elbowX - a.ax));
            const float sb = (b.ay - midY) / (std::max)(1.f, std::fabs(elbowX - b.ax));
            return sa < sb;
        });
        auto stack = [&]() {
            int y = 8;
            for (Card& c : cards)
            {
                c.y0 = y;
                c.y1 = y + kCardHeadPx + static_cast<int>(c.lines.size()) * kCardLinePx + 6;
                y = c.y1 + kCardGap;
            }
        };
        auto crosses = [&](const Card& a, const Card& b) {
            const float ax0 = elbowX, ay0 = static_cast<float>(a.y0 + kCardHeadPx / 2);
            const float bx0 = elbowX, by0 = static_cast<float>(b.y0 + kCardHeadPx / 2);
            auto side = [](float px, float py, float qx, float qy, float rx, float ry) {
                return (qx - px) * (ry - py) - (qy - py) * (rx - px);
            };
            const float d1 = side(ax0, ay0, a.ax, a.ay, bx0, by0), d2 = side(ax0, ay0, a.ax, a.ay, b.ax, b.ay);
            const float d3 = side(bx0, by0, b.ax, b.ay, ax0, ay0), d4 = side(bx0, by0, b.ax, b.ay, a.ax, a.ay);
            return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
        };
        stack();
        for (int iter = 0; iter < 24; ++iter)
        {
            bool swapped = false;
            for (size_t i = 0; i < cards.size() && !swapped; ++i)
                for (size_t j = i + 1; j < cards.size() && !swapped; ++j)
                    if (crosses(cards[i], cards[j]))
                    {
                        std::swap(cards[i], cards[j]);
                        stack();
                        swapped = true;
                    }
            if (!swapped)
                break;
        }
        return cards;
    }
    int PictureTop(int hand)
    {
        int top = 8;
        PictureCards(hand, &top);
        return top;
    }
    int PictureAreaHeight(int hand)
    {
        int bottom = 0;
        for (const Card& c : PictureCards(hand))
            bottom = (std::max)(bottom, c.y1);
        return bottom + 12;
    }

    // Bind row under a point of the picture area (column-local), or -1.
    int PictureHit(int hand, int lx, int ly)
    {
        const int x0 = CardX0(hand);
        if (lx < x0 || lx >= x0 + kCardW)
            return -1;
        for (const Card& c : PictureCards(hand))
            for (size_t k = 0; k < c.lines.size(); ++k)
            {
                const int y = c.y0 + kCardHeadPx + static_cast<int>(k) * kCardLinePx;
                if (ly >= y && ly < y + kCardLinePx && !c.lines[k].fixed)
                    return c.lines[k].row;
            }
        return -1;
    }

    // Action picker: a grid over the content area. Cells 0..Count-1 = actions,
    // kPickerCancel = cancel.
    constexpr int kPickerCols = 3;
    constexpr int kPickerCellW = 600, kPickerCellH = 76, kPickerGap = 12;
    constexpr int kPickerCancel = 100;
    int PickerX0() { return (kTexW - (kPickerCols * kPickerCellW + (kPickerCols - 1) * kPickerGap)) / 2; }
    int PickerTitleY() { return kContentTop + 8; }
    int PickerGridY() { return PickerTitleY() + 80; }
    int PickerRows() { return (bmvr::kBindActionCount + kPickerCols - 1) / kPickerCols; }
    int PickerCancelY() { return PickerGridY() + PickerRows() * (kPickerCellH + kPickerGap) + 16; }
    int PickerCellAt(int px, int py)
    {
        if (py >= PickerCancelY() && py < PickerCancelY() + kPickerCellH && px >= kTexW / 2 - 200 && px < kTexW / 2 + 200)
            return kPickerCancel;
        const int gx = px - PickerX0(), gy = py - PickerGridY();
        if (gx < 0 || gy < 0)
            return -1;
        const int col = gx / (kPickerCellW + kPickerGap), row = gy / (kPickerCellH + kPickerGap);
        if (col >= kPickerCols || gx % (kPickerCellW + kPickerGap) >= kPickerCellW || gy % (kPickerCellH + kPickerGap) >= kPickerCellH)
            return -1;
        const int idx = row * kPickerCols + col;
        return idx < bmvr::kBindActionCount ? idx : -1;
    }

    // "Reset dev rows": every row of the DEBUG tab back to its built-in default (CapturePanelDefaults,
    // taken before config.txt), except "Show developer rows". Engine rows get console commands:
    // god/noclip/notarget (toggles) are switched off before the cvars, so sv_cheats 0 comes last.
    // Returns the number of rows changed; each one is logged.
    int ResetDevRows()
    {
        std::vector<Row>& rows = Rows();
        size_t i = 0;
        while (i < rows.size() && !(rows[i].kind == RowKind::Tab && std::strcmp(rows[i].label, "DEBUG") == 0))
            ++i;
        std::vector<std::string> toggles, cvars;
        int changed = 0;
        for (++i; i < rows.size() && rows[i].kind != RowKind::Tab && rows[i].kind != RowKind::Close; ++i)
        {
            Row& r = rows[i];
            if (r.b == &bmvr::g_PanelDevRows)
                continue;
            char cmd[96]{};
            if (r.kind == RowKind::Toggle && r.b && *r.b != r.defB)
            {
                *r.b = r.defB;
                if (IsEngineCmdKey(r.key))
                    toggles.emplace_back(r.key + 4);
                else if (IsEngineCvarKey(r.key))
                {
                    _snprintf(cmd, sizeof(cmd) - 1, "%s %d", r.key + 5, r.defB ? 1 : 0);
                    cvars.emplace_back(cmd);
                }
                Game::logMsg("VR settings reset: %s -> %s", r.label, r.defB ? "on" : "off");
                ++changed;
            }
            else if ((r.kind == RowKind::Number || r.kind == RowKind::Choice) && r.f && std::fabs(*r.f - r.defF) > 1e-4f)
            {
                *r.f = r.defF;
                if (IsEngineCvarKey(r.key))
                {
                    _snprintf(cmd, sizeof(cmd) - 1, "%s %g", r.key + 5, r.defF);
                    cvars.emplace_back(cmd);
                }
                Game::logMsg("VR settings reset: %s -> %g", r.label, r.defF);
                ++changed;
            }
        }
        std::lock_guard<std::mutex> lock(g_EngCmdLock);
        for (std::string& c : toggles)
            g_EngCmds.push_back(std::move(c));
        for (std::string& c : cvars)
            g_EngCmds.push_back(std::move(c));
        Game::logMsg("VR settings reset: developer rows back to defaults, %d changed", changed);
        return changed;
    }

    struct PresetSet
    {
        const char* id;
        int count;                 // values per preset
        int def;                   // index of the built-in default (bmvr_flags.cpp)
        float* targets[3];
        struct { const char* name; float v[3]; } items[3];
    };
    // Items in order (one direction per click); the default is marked on the panel.
    const std::vector<PresetSet>& Presets()
    {
        static const std::vector<PresetSet> sets = {
            // FXAA 3.11 terms: sub-pixel smoothing, edge threshold, dark edge threshold.
            // Sharp keeps texture detail (about as sharp as the stock game); Smooth is
            // FXAA's own sub-pixel default with most edges treated (softer, calmer).
            { "fxaa", 3, 0, { &bmvr::g_FxaaSubpixel, &bmvr::g_FxaaEdgeThreshold, &bmvr::g_FxaaEdgeThresholdMin },
              { { "Sharp", { 0.25f, 0.166f, 0.0833f } },
                { "Balanced", { 0.5f, 0.125f, 0.0625f } },
                { "Smooth", { 0.75f, 0.063f, 0.0312f } } } },
            // Cubemap reflection strength, walls/floors and props/NPCs.
            { "envmap", 2, 1, { &bmvr::g_EnvmapScaleWorld, &bmvr::g_EnvmapScaleModels, nullptr },
              { { "Full", { 1.f, 1.f, 0.f } },   // as in the game
                { "Balanced", { 0.4f, 0.8f, 0.f } },
                { "Subtle", { 0.2f, 0.5f, 0.f } } } },
        };
        return sets;
    }
    const PresetSet* FindPresetSet(const char* id)
    {
        for (const PresetSet& p : Presets())
            if (id && std::strcmp(p.id, id) == 0)
                return &p;
        return nullptr;
    }
    // Index of the preset the current values match, or -1 (custom).
    int PresetIndex(const PresetSet& p)
    {
        for (int i = 0; i < 3; ++i)
        {
            bool match = true;
            for (int k = 0; k < p.count && match; ++k)
                match = std::fabs(*p.targets[k] - p.items[i].v[k]) <= 1e-3f;
            if (match)
                return i;
        }
        return -1;
    }
    std::string PresetText(const Row& r)
    {
        const PresetSet* p = FindPresetSet(r.fmt);
        if (!p)
            return {};
        const int i = PresetIndex(*p);
        if (i < 0)
            return "Custom";
        return std::string(p->items[i].name) + (i == p->def ? " (default)" : "");
    }

    // Name of a Choice row's current value ("" if out of range).
    std::string ChoiceText(const Row& r)
    {
        if (!r.f || !r.fmt || r.step <= 0.f)
            return {};
        int idx = static_cast<int>(std::lround((*r.f - r.lo) / r.step));
        const char* p = r.fmt;
        while (idx > 0 && *p)
        {
            if (*p++ == '|')
                --idx;
        }
        if (idx != 0 || !*p)
        {
            char buf[32]{};
            _snprintf(buf, sizeof(buf) - 1, "%g", *r.f);
            return buf;
        }
        const char* e = std::strchr(p, '|');
        std::string name = e ? std::string(p, e) : std::string(p);
        if (g_DefaultsCaptured && std::lround((*r.f - r.lo) / r.step) == std::lround((r.defF - r.lo) / r.step))
            name += " (default)";
        return name;
    }

    // Tab shown on the panel; kept while the game runs.
    int g_Tab = 0;
    // Distance the open panel was placed at (PanelDistanceHu at open / last relayout).
    float g_LatchDistHu = 0.f;

    // Each tab's rows split into two columns at the section heading that best
    // halves that tab's height. The panel is as tall as the tallest tab, so it
    // does not change size or move when you switch tabs. Tab buttons sit in a
    // bar under the title, Close at the bottom of every tab. The row table is
    // fixed, so this is built once.
    // Rows under a DevSection are left out (tab -2) while developer rows are off; the layout is
    // rebuilt when that switch changes, so the panel shrinks to the tallest visible tab.
    struct Layout
    {
        std::vector<int> tab, col, top, x0, x1;   // tab -1 = shown on every tab, -2 = hidden
        int tabCount = 0;
        int closeTop = 0;
        int bottom = 0;
    };
    Layout BuildLayout(bool devRows)
    {
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
        // Developer-only rows: everything from a DevSection to the next heading/tab.
        std::vector<bool> hidden(n, false);
        bool inDev = false;
        for (int i = 0; i < n; ++i)
        {
            const RowKind k = rows[i].kind;
            if (k == RowKind::Tab || k == RowKind::Close || k == RowKind::Section)
                inDev = false;
            else if (k == RowKind::DevSection)
                inDev = true;
            // Picture shown: the hand's Bind and fixed rows are its card lines instead.
            const int hand = RowHand(rows[i]);
            hidden[i] = (inDev && !devRows) || k == RowKind::ColumnBreak
                || (k == RowKind::Bind && BindRowHidden(rows[i]))
                || (k == RowKind::BindPicture && !PictureShown(hand))
                || (k != RowKind::BindPicture && hand >= 0 && PictureShown(hand));
        }
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
                total += hidden[i] ? 0 : RowHeight(rows[i]);
            int split = last, best = total, acc = 0;
            int forced = -1;
            for (int i = first; i < last; ++i)
                if (rows[i].kind == RowKind::ColumnBreak)
                    forced = i;
            for (int i = first; i < last && forced < 0; ++i)
            {
                if (hidden[i])
                    continue;
                if (i > first && IsHeading(rows[i].kind))
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
            if (forced >= 0)
                split = forced;
            int y[2] = { kContentTop, kContentTop };
            for (int i = first; i < last; ++i)
            {
                if (hidden[i])
                {
                    out.tab[i] = -2;
                    continue;
                }
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
    }
    const Layout& GetLayout()
    {
        static Layout l;
        static int s_builtFor = -1;
        // Rebuilt when developer rows or the detected controller (its buttons) change.
        const int want = (bmvr::g_PanelDevRows ? 1 : 0) | static_cast<int>(g_PanelFamily << 1)
            | static_cast<int>(((g_PanelRaw[0] >> 16) & 0xF) << 8) | static_cast<int>(((g_PanelRaw[1] >> 16) & 0xF) << 12)
            | (PictureShown(0) ? 1 << 16 : 0) | (PictureShown(1) ? 1 << 17 : 0);
        if (s_builtFor != want)
        {
            // Bit 0 only: the other bits are the controller (was want != 0, so
            // developer rows showed once a controller was detected).
            l = BuildLayout((want & 1) != 0);
            s_builtFor = want;
        }
        return l;
    }
    int RowTop(int index) { return GetLayout().top[index]; }
    int RowX(int index) { return GetLayout().col[index] * kColW; }
    bool RowVisible(int index)
    {
        const int t = GetLayout().tab[index];
        return t == -1 || t == g_Tab;
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
            {
                if (k == RowKind::BindPicture)
                    return PictureHit(RowHand(rows[i]), px - c * kColW, py - l.top[i]);
                return (IsHeading(k) || k == RowKind::Info) ? -1 : i;
            }
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
        int textWidth(const char* s, int height, bool bold)
        {
            HFONT f = CreateFontA(-height, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_SWISS, "Arial");
            HGDIOBJ of = SelectObject(dc, f);
            SIZE sz{};
            GetTextExtentPoint32A(dc, s, static_cast<int>(std::strlen(s)), &sz);
            SelectObject(dc, of);
            DeleteObject(f);
            return sz.cx;
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
    constexpr COLORREF kAlert = RGB(210, 40, 30);

    // Missing-hook alert: 0 none, 1 debug-only hooks missing (amber), 2 gameplay hooks missing (red).
    // Shown as a filled bar on the Debug tab's "Missing hooks" row and a badge on the DEBUG tab
    // button, so it is seen without opening the tab.
    int MissingHooksLevel()
    {
        return Game::missingOffsetCount() > 0 ? 2 : Game::missingDebugOffsetCount() > 0 ? 1 : 0;
    }

    // Text of an Info row.
    std::string InfoText(const Row& r)
    {
        if (r.fmt && std::strncmp(r.fmt, "fixed", 5) == 0 && std::strchr(r.fmt, ':'))
            return std::string(std::strchr(r.fmt, ':') + 1) + " (fixed)";
        if (r.fmt && std::strcmp(r.fmt, "foveation") == 0)
            return bmvr::FoveationStatusText();
        if (!r.fmt || std::strcmp(r.fmt, "missing_hooks") != 0)
            return {};
        const int game = Game::missingOffsetCount(), debug = Game::missingDebugOffsetCount();
        if (game == 0 && debug == 0)
            return "none";
        char buf[64]{};
        if (game == 0)
            _snprintf(buf, sizeof(buf) - 1, "%d debug only", debug);
        else if (debug == 0)
            _snprintf(buf, sizeof(buf) - 1, "%d gameplay", game);
        else
            _snprintf(buf, sizeof(buf) - 1, "%d gameplay, %d debug", game, debug);
        return buf;
    }

    // One controller's cards (RowKind::BindPicture); picture and lines: ComposeWings.
    void PaintPicture(Canvas& c, int hand, int top, int hoverRow)
    {
        const int colX = hand * kColW;
        const int cx0 = colX + CardX0(hand), cx1 = cx0 + kCardW;
        for (const Card& card : PictureCards(hand))
        {
            const int y0 = top + card.y0;
            c.fill(cx0, y0, cx1, top + card.y1, RGB(26, 31, 38));
            c.fill(cx0, y0, cx1, y0 + 4, RGB(90, 70, 20));
            c.text(card.head.c_str(), cx0 + 16, y0 + 4, cx1 - 16, y0 + kCardHeadPx, kAmber, 28, true, DT_LEFT);
            for (size_t k = 0; k < card.lines.size(); ++k)
            {
                const CardLine& ln = card.lines[k];
                const int ly0 = y0 + kCardHeadPx + static_cast<int>(k) * kCardLinePx;
                const int ly1 = ly0 + kCardLinePx;
                const bool hv = !ln.fixed && ln.row == hoverRow;
                const int capW = ln.caption.empty() ? 0 : 180;
                if (!ln.fixed)
                    c.fill(cx0 + 10, ly0 + 3, cx1 - 10, ly1 - 3, hv ? kBtnHover : kBtn);
                if (capW)
                    c.text(ln.caption.c_str(), cx0 + 24, ly0, cx0 + 24 + capW, ly1, hv ? RGB(0, 0, 0) : kDim, 26, false, DT_LEFT);
                const int tx0 = cx0 + 24 + capW, tx1 = cx1 - (ln.fixed ? 20 : 44);
                int fontPx = 30;
                while (fontPx > 20 && c.textWidth(ln.text.c_str(), fontPx, true) > tx1 - tx0)
                    fontPx -= 2;
                const COLORREF ink = hv ? RGB(0, 0, 0) : ln.fixed ? kDim : ln.changed ? kAmber : kText;
                c.text(ln.text.c_str(), tx0, ly0, tx1, ly1, ink, fontPx, !ln.fixed, DT_LEFT);
                if (!ln.fixed)
                    c.text(">", cx1 - 44, ly0, cx1 - 14, ly1 - 2, hv ? RGB(0, 0, 0) : kDim, 28, true, DT_CENTER);
            }
        }
    }

    // Width of each side wing: the panel texture is the body plus a wing on
    // both sides while controller pictures can show (all tabs, so the panel
    // does not move when you switch; the wings are clear off the CONTROLS tab).
    int WingPx() { return (PictureShown(0) || PictureShown(1)) ? kWingPx : 0; }

    // body (painted panel) -> out (wings + body): controller pictures in the
    // wings and the lines from the cards to the buttons (CONTROLS tab only).
    void ComposeWings(const Canvas& body, Canvas& out, int hoverRow)
    {
        const int wing = (out.w - body.w) / 2;
        GdiFlush();
        std::memset(out.px, 0, static_cast<size_t>(out.w) * out.h * 4u);
        for (int y = 0; y < body.h && y < out.h; ++y)
            std::memcpy(out.px + static_cast<size_t>(y) * out.w + wing, body.px + static_cast<size_t>(y) * body.w,
                static_cast<size_t>(body.w) * 4u);
        const auto& rows = Rows();
        const Layout& l = GetLayout();
        const bmvr::ctlmodel::Models* m = PanelModels();
        for (int i = 0; m && i < static_cast<int>(rows.size()); ++i)
        {
            if (rows[i].kind != RowKind::BindPicture || l.tab[i] != g_Tab)
                continue;
            const int hand = RowHand(rows[i]);
            if (!PictureShown(hand))
                continue;
            const int top = l.top[i];
            const int colX = wing + hand * kColW;   // column origin in out
            const bmvr::ctlmodel::Picture& pic = m->hand[hand].picture;
            const int px0 = colX + PicX(hand), py0 = top + PictureTop(hand);
            for (int y = 0; y < pic.h; ++y)
            {
                const int dy = py0 + y;
                if (dy < 0 || dy >= out.h)
                    continue;
                for (int x = 0; x < pic.w; ++x)
                {
                    const int dx = px0 + x;
                    const uint32_t sp = pic.px[static_cast<size_t>(y) * pic.w + x];
                    if ((sp >> 24) && dx >= 0 && dx < out.w)
                        out.px[static_cast<size_t>(dy) * out.w + dx] = sp;
                }
            }
            for (const Card& card : PictureCards(hand))
            {
                // Fixed-only cards (the menu button) get no line: it sits next to
                // the stick and read as a second stick line.
                bool fixedOnly = true;
                for (const CardLine& ln : card.lines)
                    fixedOnly &= ln.fixed;
                if (fixedOnly)
                    continue;
                bool hot = false;
                for (const CardLine& ln : card.lines)
                    hot |= !ln.fixed && ln.row == hoverRow;
                const COLORREF ink = hot ? kAmber : RGB(170, 140, 80);
                const int ly = top + card.y0 + kCardHeadPx / 2;
                const int edge = colX + (hand == 0 ? CardX0(hand) : CardX0(hand) + kCardW);
                const int elbow = colX + LeaderElbowX(hand);
                const int ax = colX + static_cast<int>(card.ax), ay = top + static_cast<int>(card.ay);
                HPEN pen = CreatePen(PS_SOLID, hot ? 5 : 3, ink);
                HGDIOBJ op = SelectObject(out.dc, pen);
                MoveToEx(out.dc, edge, ly, nullptr);
                LineTo(out.dc, elbow, ly);
                LineTo(out.dc, ax, ay);
                HBRUSH br = CreateSolidBrush(ink);
                HGDIOBJ ob = SelectObject(out.dc, br);
                Ellipse(out.dc, ax - 8, ay - 8, ax + 9, ay + 9);
                SelectObject(out.dc, ob);
                SelectObject(out.dc, op);
                DeleteObject(br);
                DeleteObject(pen);
            }
        }
        // GDI writes alpha 0: the body keeps the panel opacity, drawn wing pixels are opaque.
        GdiFlush();
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x)
            {
                uint32_t& p = out.px[static_cast<size_t>(y) * out.w + x];
                if (x >= wing && x < wing + body.w)
                    p = (p & 0x00FFFFFFu) | (238u << 24);
                else if (!(p >> 24) && (p & 0x00FFFFFFu))
                    p |= 0xFF000000u;
            }
    }

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
                // Missing hooks: the DEBUG tab button is filled red (gameplay) or amber (debug only)
                // with "!" on both sides, so it shows from any tab; selected, a red/amber frame.
                const int alert = std::strcmp(r.label, "DEBUG") == 0 ? MissingHooksLevel() : 0;
                const COLORREF alertFill = alert == 2 ? kAlert : kAmber;
                if (alert && !active)
                {
                    c.fill(l.x0[i], y0 + 4, l.x1[i], y1, alertFill);
                    c.text("!   DEBUG   !", l.x0[i], y0 + 4, l.x1[i], y1, alert == 2 ? RGB(255, 255, 255) : RGB(0, 0, 0),
                        34, true, DT_CENTER);
                    continue;
                }
                if (alert)
                    c.fill(l.x0[i] - 6, y0 - 2, l.x1[i] + 6, y1 + 6, alert == 2 ? kAlert : RGB(255, 255, 255));
                c.fill(l.x0[i], y0 + 4, l.x1[i], y1, active ? kAmber : hover ? kRowHover : kBtn);
                c.text(alert ? "!   DEBUG   !" : r.label, l.x0[i], y0 + 4, l.x1[i], y1, active ? RGB(0, 0, 0) : kText,
                    32, true, DT_CENTER);
                continue;
            }
            if (!RowVisible(i))
                continue;
            const int y0 = RowTop(i), y1 = y0 + RowHeight(r);
            const int x = r.kind == RowKind::Close ? 0 : RowX(i);
            const int cw = r.kind == RowKind::Close ? c.w : kColW, mid = x + cw / 2;
            if (IsHeading(r.kind))
            {
                c.text(SectionText(r).c_str(), x + 40, y0 + 8, x + cw - 40, y1, kAmber, 26, true, DT_LEFT);
                if (r.kind == RowKind::DevSection)
                    c.text("developer", x + 40, y0 + 8, x + cw - 40, y1, kDim, 22, false, DT_RIGHT);
                // 4 px: the panel is drawn minified without mipmaps, so 2 px lines dropped out.
                c.fill(x + 40, y1 - 6, x + cw - 40, y1 - 2, RGB(90, 70, 20));
                continue;
            }
            if (r.kind == RowKind::BindPicture)
            {
                PaintPicture(c, RowHand(r), y0, hoverRow);
                continue;
            }
            if (hover && r.kind != RowKind::Close)
                c.fill(x + 16, y0 + 2, x + cw - 16, y1 - 2, kRowHover);
            if (r.kind == RowKind::Bind || r.kind == RowKind::BindPreset)
            {
                // Like a Choice row: one wide button; a bind opens the action picker,
                // the preset row applies the next preset. Changed binds in amber.
                std::string name;
                COLORREF ink = kText;
                if (r.kind == RowKind::Bind)
                {
                    c.text(BindRowLabel(r).c_str(), x + 40, y0, x + kMinusX0 - 10, y1, kText, 34, false, DT_LEFT);
                    const bmvr::BindAction a = BindRowAction(r);
                    name = bmvr::BindActionLabel(a);
                    if (a != BindRowAction(r, true))
                        ink = kAmber;
                }
                else
                {
                    c.text(r.label, x + 40, y0, x + kMinusX0 - 10, y1, kText, 34, false, DT_LEFT);
                    name = BindPresetText();
                }
                const int bx0 = x + kMinusX0 - 120;
                c.fill(bx0, y0 + 10, x + kPlusX1, y1 - 10, hover ? kBtnHover : kBtn);
                int fontPx = 30;
                while (fontPx > 20 && c.textWidth(name.c_str(), fontPx, true) > (kPlusX1 - 36 - kMinusX0 + 120) - 16)
                    fontPx -= 2;
                c.text(name.c_str(), bx0, y0, x + kPlusX1 - 36, y1, hover ? RGB(0, 0, 0) : ink, fontPx, true, DT_CENTER);
                c.text(">", x + kPlusX1 - 40, y0, x + kPlusX1 - 8, y1 - 2, hover ? RGB(0, 0, 0) : kDim, 30, true, DT_CENTER);
                continue;
            }
            if (r.kind == RowKind::Close)
            {
                c.fill(mid - 120, y0 + 8, mid + 120, y1 - 8, hover ? kBtnHover : kBtn);
                c.text("Close", x, y0, x + cw, y1, hover ? RGB(0, 0, 0) : kText, 34, true, DT_CENTER);
                continue;
            }
            if (r.kind == RowKind::Button)
            {
                // At least 360 px, wider for a long label, never past the column.
                const int half = (std::min)(cw / 2 - 24, (std::max)(180, c.textWidth(r.label, 34, true) / 2 + 28));
                c.fill(mid - half, y0 + 8, mid + half, y1 - 8, hover ? kBtnHover : kBtn);
                c.text(r.label, x, y0, x + cw, y1, hover ? RGB(0, 0, 0) : kText, 34, true, DT_CENTER);
                continue;
            }
            if (r.kind == RowKind::Info)
            {
                const std::string info = InfoText(r);
                const int level = (r.fmt && std::strcmp(r.fmt, "missing_hooks") == 0) ? MissingHooksLevel() : 0;
                if (level == 0)
                {
                    c.text(r.label, x + 40, y0, x + kMinusX0 - 10, y1, kDim, 34, false, DT_LEFT);
                    c.text(info.c_str(), x + kMinusX0, y0, x + cw - 40, y1, kOn, 30, true, DT_RIGHT);
                    continue;
                }
                // Something is missing: a filled bar across the column, red for gameplay hooks.
                const COLORREF bar = level == 2 ? kAlert : kAmber;
                const COLORREF ink = level == 2 ? RGB(255, 255, 255) : RGB(0, 0, 0);
                c.fill(x + 16, y0 + 4, x + cw - 16, y1 - 4, bar);
                c.text(level == 2 ? "! MISSING HOOKS" : "! Missing hooks", x + 40, y0, x + kMinusX0 - 10, y1, ink, 34, true, DT_LEFT);
                c.text(info.c_str(), x + kMinusX0 - 60, y0, x + cw - 40, y1, ink, 30, true, DT_RIGHT);
                continue;
            }
            c.text(r.label, x + 40, y0, x + kMinusX0 - 10, y1, kText, 34, false, DT_LEFT);
            if (r.kind == RowKind::Choice || r.kind == RowKind::Preset)
            {
                // One wide button (the -/+ span); a click picks the next value.
                c.fill(x + kMinusX0, y0 + 10, x + kPlusX1, y1 - 10, hover ? kBtnHover : kBtn);
                const std::string name = r.kind == RowKind::Preset ? PresetText(r) : ChoiceText(r);
                // Long names ("Headset +10 (default)") get a smaller font instead of spilling over.
                int fontPx = 30;
                while (fontPx > 20 && c.textWidth(name.c_str(), fontPx, true) > (kPlusX1 - 36 - kMinusX0) - 16)
                    fontPx -= 2;
                c.text(name.c_str(), x + kMinusX0, y0, x + kPlusX1 - 36, y1, hover ? RGB(0, 0, 0) : kText,
                    fontPx, true, DT_CENTER);
                c.text(">", x + kPlusX1 - 40, y0, x + kPlusX1 - 8, y1 - 2, hover ? RGB(0, 0, 0) : kDim, 30, true, DT_CENTER);
            }
            else if (r.kind == RowKind::Toggle)
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
        // Action picker over the content area.
        if (g_PickerRow >= 0 && g_PickerRow < static_cast<int>(rows.size()))
        {
            const Row& pr = rows[g_PickerRow];
            const bmvr::BindAction cur = BindRowAction(pr), def = BindRowAction(pr, true);
            c.fill(24, kContentTop - 8, c.w - 24, l.closeTop - 8, RGB(10, 12, 15));
            c.fill(24, kContentTop - 8, c.w - 24, kContentTop - 4, kAmber);
            const std::string title = std::string(pr.fmt[0] == 'L' ? "Left " : "Right ") + BindRowLabel(pr) + ":  pick an action";
            c.text(title.c_str(), PickerX0(), PickerTitleY(), c.w - PickerX0(), PickerTitleY() + 64, kAmber, 40, true, DT_LEFT);
            for (int a = 0; a < bmvr::kBindActionCount; ++a)
            {
                const int col = a % kPickerCols, row = a / kPickerCols;
                const int cx0 = PickerX0() + col * (kPickerCellW + kPickerGap);
                const int cy0 = PickerGridY() + row * (kPickerCellH + kPickerGap);
                const bool hv = hoverRow == 1000 + a;
                const bool isCur = static_cast<int>(cur) == a;
                if (isCur)
                    c.fill(cx0 - 4, cy0 - 4, cx0 + kPickerCellW + 4, cy0 + kPickerCellH + 4, kAmber);
                c.fill(cx0, cy0, cx0 + kPickerCellW, cy0 + kPickerCellH, hv ? kBtnHover : kBtn);
                std::string label = bmvr::BindActionLabel(static_cast<bmvr::BindAction>(a));
                if (static_cast<int>(def) == a)
                    label += " (default)";
                c.text(label.c_str(), cx0, cy0, cx0 + kPickerCellW, cy0 + kPickerCellH, hv ? RGB(0, 0, 0) : kText, 32, true, DT_CENTER);
            }
            const bool hvCancel = hoverRow == 1000 + kPickerCancel;
            const int cy = PickerCancelY();
            c.fill(c.w / 2 - 200, cy, c.w / 2 + 200, cy + kPickerCellH, hvCancel ? kBtnHover : kOff);
            c.text("Cancel", c.w / 2 - 200, cy, c.w / 2 + 200, cy + kPickerCellH, hvCancel ? RGB(0, 0, 0) : kText, 32, true, DT_CENTER);
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

std::string bmvr::CaptureSettingsScenario(Game* game)
{
    std::string out = "# VR panel switches and numbers (bmvr_* cvars) and engine settings at capture time\n";
    char line[256];
    for (const Row& r : Rows())
    {
        if (!r.b && !r.f && !IsEngineCvarKey(r.key))
            continue;
        if (IsEngineCvarKey(r.key))
        {
            float v = 0.f;
            if (game && game->GetConVarFloat(r.key + 5, v))
            {
                std::snprintf(line, sizeof(line), "cmd %s %g\n", r.key + 5, v);
                out += line;
            }
            continue;
        }
        const void* target = r.b ? static_cast<const void*>(r.b) : static_cast<const void*>(r.f);
        const float v = r.b ? (*r.b ? 1.f : 0.f) : *r.f;
        if (const char* cv = VrCvarNameFor(target))
            std::snprintf(line, sizeof(line), "cmd %s %g\n", cv, v);
        else if (r.key && r.key[0])
            std::snprintf(line, sizeof(line), "# config.txt %s = %g (from the capture's config copy)\n", r.key, v);
        else
            std::snprintf(line, sizeof(line), "# NOT RESTORED (session-only switch, no cvar): %s = %g\n", r.label, v);
        out += line;
    }
    // The settings the harness guard checks plus other picture cvars.
    static const char* kEngine[] = { "cl_csm_enabled", "cl_csm_qualitymode", "cl_csm_memoryusage", "np_gr_quality",
        "mat_motion_blur_enabled", "mat_fxaa_enabled", "r_occlusion", "r_occlusion_tracker_enabled", "mat_hdr_level", "nr_lights_quality",
        "nr_shadow_quality", "nr_shadow_res", "nr_shadow_filter_quality", "mat_specular", "mat_bloomscale",
        "r_waterforceexpensive", "r_waterforcereflectentities", "mat_antialias", "mat_picmip", "r_3dsky",
        "r_dopixelvisibility", "mat_queue_mode", "host_timescale", "fov_desired" };
    for (const char* name : kEngine)
    {
        float v = 0.f;
        if (game && game->GetConVarFloat(name, v))
        {
            std::snprintf(line, sizeof(line), "engine %s %g\n", name, v);
            out += line;
        }
    }
    return out;
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
    // The panel pauses the game, so the client sees noclip's new move type
    // only once the game runs again: until the read-back moves away from the
    // value at the click, the row keeps what was clicked (user 2026-11-17:
    // the row flipped back to off while noclip was on).
    static int s_noclipClickMoveType = -2;
    for (const std::string& c : cmds)
    {
        const bool ok = m_Game->ClientCmd_Unrestricted(c.c_str());
        Game::logMsg("VR settings console '%s' ok=%d", c.c_str(), ok ? 1 : 0);
        if (c == "noclip" && s_noclipClickMoveType == -2)
            s_noclipClickMoveType = m_Game->LocalPlayerMoveType();
    }
    static DWORD s_lastRead;
    const DWORD now = GetTickCount();
    if (!m_SettingsOpen || (cmds.empty() && now - s_lastRead < 500))
        return;
    s_lastRead = now;
    bool changed = false;
    // MOVETYPE_NOCLIP = 8.
    const int moveType = m_Game->LocalPlayerMoveType();
    static int s_lastMoveType = -2;
    if (moveType != s_lastMoveType)
    {
        static int s_mtLog;
        if (s_mtLog++ < 20)
            Game::logMsg("VR settings: player move type %d (noclip row %d)", moveType, g_DevNoclip ? 1 : 0);
        s_lastMoveType = moveType;
    }
    if (s_noclipClickMoveType != -2 && moveType != s_noclipClickMoveType)
        s_noclipClickMoveType = -2;   // the server's answer arrived
    if (s_noclipClickMoveType == -2 && moveType >= 0 && g_DevNoclip != (moveType == 8))
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
    // Test only: BMVR_TEST_PANEL_TAB=<n> opens the panel on tab n (headless screenshots).
    static bool s_testTab = false;
    if (!s_testTab)
    {
        s_testTab = true;
        char v[8] = {};
        if (GetEnvironmentVariableA("BMVR_TEST_PANEL_TAB", v, sizeof(v)))
            g_Tab = std::atoi(v);
    }
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
    SetPanelController(m_OpenXrLastInputState);
    g_PickerRow = -1;
    g_LatchDistHu = PanelDistanceHu();
    const Vector center = eye + yawFwd * g_LatchDistHu - worldUp * kPanelDropHu;
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
        if (g_BindsChanged)
            bmvr::CollectBindConfig(g_PanelFamily, kv, drop);
        const bool ok = WriteConfigValues(kv, drop);
        Game::logMsg("VR settings saved to config.txt: %s (%d set, %d at default left out)",
            ok ? "ok" : "FAILED", static_cast<int>(kv.size()), static_cast<int>(drop.size()));
    }
    m_SettingsChanged = false;
    g_BindsChanged = false;
    g_PickerRow = -1;
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
    if (SetPanelController(m_OpenXrLastInputState))
    {
        m_SettingsDirty = true;
        Game::logMsg("VR settings panel: controller %s (%s)", ControllerName(),
            L4D2VR_ControllerFamilyName(g_PanelFamily));
    }
    if (bmvr::g_ControlsPicture && (g_PanelFamily != 0 || g_PanelSystem[0]))
        bmvr::ctlmodel::Request(g_PanelFamily, g_PanelSystem, kPicW, kPicH);
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
                const int r = g_PickerRow >= 0 ? -1 : RowAt(px, py);
                hitPx = px;
                hitPy = py;
                if (g_PickerRow >= 0)
                {
                    // Picker open: hover = 1000 + cell (action index or kPickerCancel).
                    const int cell = PickerCellAt(px, py);
                    if (cell >= 0)
                    {
                        row = 1000 + cell;
                        zone = 1;
                    }
                }
                else if (r >= 0)
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
            click ? "press" : "release", row,
            row >= 1000 ? "picker" : row >= 0 ? (Rows()[row].kind == RowKind::Bind ? Rows()[row].fmt : Rows()[row].label) : "-",
            zone, hitPx, hitPy, g_Tab,
            haveTrig ? trig.x : -1.f);
    }
    m_SettingsClickHeld = click;
    m_SettingsBackHeld = back;
    if (backEdge && g_PickerRow >= 0)
    {
        g_PickerRow = -1;
        m_SettingsDirty = true;
        return;
    }
    if (backEdge)
    {
        CloseSettingsPanel("circle");
        return;
    }
    if (g_PickerRow >= 0)
    {
        if (!clickEdge)
            return;
        const int cell = row >= 1000 ? row - 1000 : -1;
        if (cell >= 0 && cell < bmvr::kBindActionCount)
        {
            const BindRowSpec b = ParseBindRow(Rows()[g_PickerRow]);
            std::string key, value;
            bmvr::SetBind(g_PanelFamily, b.hand, b.input, b.hold, static_cast<bmvr::BindAction>(cell), key, value);
            g_BindsChanged = true;
            m_SettingsChanged = true;
            Game::logMsg("VR settings: %s = %s", key.c_str(), value.empty() ? "(default)" : value.c_str());
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 700, 0.3f);
        }
        // An action, Cancel or a click beside the grid closes the picker.
        g_PickerRow = -1;
        m_SettingsHover = -1;
        m_SettingsDirty = true;
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
    case RowKind::DevSection:
    case RowKind::Info:
    case RowKind::ColumnBreak:
    case RowKind::BindPicture:
        return;
    case RowKind::Bind:
        g_PickerRow = row;
        m_SettingsHover = -1;
        m_SettingsDirty = true;
        PulseHandHaptic(vr::TrackedControllerRole_RightHand, 500, 0.25f);
        Game::logMsg("VR settings: picker for %s", r.fmt);
        return;
    case RowKind::BindPreset:
    {
        // Next usable preset; "Custom" goes back to the defaults (0).
        const int cur = bmvr::MatchBindPreset(g_PanelFamily);
        int next = 0;
        if (cur >= 0)
            for (int k = 1; k <= bmvr::BindPresetCount(); ++k)
            {
                const int cand = (cur + k) % bmvr::BindPresetCount();
                if (bmvr::BindPresetUsable(cand, g_PanelFamily))
                {
                    next = cand;
                    break;
                }
            }
        bmvr::ApplyBindPreset(g_PanelFamily, next);
        g_BindsChanged = true;
        break;
    }
    case RowKind::Preset:
    {
        const PresetSet* p = FindPresetSet(r.fmt);
        if (!p)
            return;
        const int cur = PresetIndex(*p);
        const int next = cur < 0 ? p->def : (cur + 1) % 3;   // custom -> the default
        for (int k = 0; k < p->count; ++k)
            *p->targets[k] = p->items[next].v[k];
        Game::logMsg("VR settings: %s preset = %s", r.fmt, p->items[next].name);
        break;
    }
    case RowKind::Choice:
        if (!r.f || r.step <= 0.f)
            return;
        {
            float v = std::round((*r.f - r.lo) / r.step) * r.step + r.lo + r.step;
            *r.f = v > r.hi + r.step * 0.5f ? r.lo : std::clamp(v, r.lo, r.hi);
        }
        break;
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
        // Hands + hand HUD placement back at their defaults (classic pose off, thumb-up roll,
        // gun grip, HUD spot); HUD and hit flash on/off stay as they are.
        if (r.fmt && std::strcmp(r.fmt, "handhud_default") == 0)
        {
            bmvr::g_VrHandsClassicPose = false;
            bmvr::g_VrHandsRollDeg = 90.f;
            bmvr::g_VrHandsGunGripRight = 1.2f;
            bmvr::g_VrHandsGunGripUp = 0.f;
            bmvr::g_VrHandsGunGripForward = 0.f;
            bmvr::g_HandHudSize = 2.5f;
            bmvr::g_HandHudUp = 2.2f;
            bmvr::g_HandHudForward = -0.55f;
            bmvr::g_HandHudSide = 0.3f;
            bmvr::g_HandHudTilt = -18.f;
            bmvr::g_HandHudRoll = 14.f;
            break;
        }
        if (r.fmt && std::strcmp(r.fmt, "binds_default") == 0)
        {
            bmvr::ResetBinds(g_PanelFamily);
            g_BindsChanged = true;
            break;
        }
        if (r.fmt && std::strcmp(r.fmt, "dev_default") == 0)
        {
            ResetDevRows();
            break;
        }
        if (r.fmt && std::strcmp(r.fmt, "impulse101") == 0)
        {
            std::lock_guard<std::mutex> lock(g_EngCmdLock);
            g_EngCmds.emplace_back("sv_cheats 1");
            g_EngCmds.emplace_back("impulse 101");
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
    // Developer rows shown/hidden: the panel changes height, so it moves to the
    // distance that fits it again (in the latched frame, like on open).
    if (r.b == &bmvr::g_PanelDevRows)
    {
        const float d = PanelDistanceHu();
        m_SettingsLatchDelta = m_SettingsLatchDelta + m_SettingsLatchFwd * (d - g_LatchDistHu);
        Game::logMsg("VR settings panel: developer rows %s, %d px tall, distance %.1f -> %.1f",
            bmvr::g_PanelDevRows ? "shown" : "hidden", TexH(), g_LatchDistHu, d);
        g_LatchDistHu = d;
        m_SettingsHover = -1;
        m_SettingsHoverZone = -1;
        s_repeatRow = -1;
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
    if (r.kind == RowKind::Button)
        Game::logMsg("VR settings: %s pressed", r.label);
    else if (r.kind == RowKind::Preset || r.kind == RowKind::BindPreset)
        ;   // logged with the preset name above
    else
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
    // Hooks resolved at a map load can go missing while the panel is open: repaint for the alert.
    static int s_paintedHookCounts = -1;
    const int hookCounts = Game::missingOffsetCount() * 1000 + Game::missingDebugOffsetCount();
    if (hookCounts != s_paintedHookCounts)
    {
        s_paintedHookCounts = hookCounts;
        m_SettingsDirty = true;
    }
    // The "Foveation now" row changes on its own (eye tracking found / paused).
    static std::string s_paintedFovStatus;
    std::string fovStatus = bmvr::FoveationStatusText();
    if (fovStatus != s_paintedFovStatus)
    {
        s_paintedFovStatus = std::move(fovStatus);
        m_SettingsDirty = true;
    }
    // Controller models finished loading (or changed): relayout + repaint.
    static int s_paintedModelGen = -1;
    if (bmvr::ctlmodel::Generation() != s_paintedModelGen)
    {
        s_paintedModelGen = bmvr::ctlmodel::Generation();
        m_SettingsDirty = true;
    }
    if (m_SettingsDirty.exchange(false) || !g_Gfx.gpu)
    {
        Canvas body(kTexW, TexH());
        const int wing = WingPx();
        Canvas wide(wing ? kTexW + 2 * wing : 1, wing ? TexH() : 1);
        if (body.ok() && (!wing || wide.ok()))
        {
            Paint(body, m_SettingsHover, m_SettingsHoverZone);
            if (wing)
                ComposeWings(body, wide, m_SettingsHover);
            Canvas& c = wing ? wide : body;
            // Test only: BMVR_TEST_PANEL_DUMP=1 saves every repaint as
            // VR/panel-tab<N>[-picker].bmp (full size, for screenshots).
            static int s_dumpPanel = -1;
            if (s_dumpPanel < 0)
            {
                char v[8]{};
                s_dumpPanel = GetEnvironmentVariableA("BMVR_TEST_PANEL_DUMP", v, sizeof(v)) && v[0] == '1' ? 1 : 0;
            }
            if (s_dumpPanel == 1)
            {
                GdiFlush();
                char name[64]{};
                _snprintf(name, sizeof(name) - 1, "panel-tab%d%s.bmp", g_Tab, g_PickerRow >= 0 ? "-picker" : "");
                if (FILE* f = std::fopen(bmvr::VrFilePath(name).c_str(), "wb"))
                {
                    BITMAPFILEHEADER fh{};
                    BITMAPINFOHEADER ih{};
                    ih.biSize = sizeof(ih);
                    ih.biWidth = c.w;
                    ih.biHeight = -c.h;
                    ih.biPlanes = 1;
                    ih.biBitCount = 32;
                    ih.biCompression = BI_RGB;
                    const DWORD bytes = static_cast<DWORD>(c.w) * c.h * 4u;
                    fh.bfType = 0x4D42;
                    fh.bfOffBits = sizeof(fh) + sizeof(ih);
                    fh.bfSize = fh.bfOffBits + bytes;
                    std::fwrite(&fh, sizeof(fh), 1, f);
                    std::fwrite(&ih, sizeof(ih), 1, f);
                    std::fwrite(c.px, 1, bytes, f);
                    std::fclose(f);
                }
            }
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
    // The texture may be wider than the panel body (controller picture wings).
    const float hw = kPanelWidthHu * 0.5f * (g_Gfx.w > 0 ? static_cast<float>(g_Gfx.w) / kTexW : 1.f);
    const float hh = PanelHeightHu() * 0.5f;
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

void VR::ShowReloadHint(int id, const char* text)
{
    static int s_shown[8]{};
    if (!bmvr::g_VrReloadHints || id < 0 || id >= 8 || s_shown[id] >= 2)
        return;
    ++s_shown[id];
    ShowToast(text, 1800);
    m_ToastSmall = true;
}

void VR::ShowToast(const char* text, DWORD ms, const char* detail)
{
    m_ToastSmall = false;
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
    const float hw = m_ToastSmall ? 3.5f : detail ? 10.f : 6.f, hh = hw * static_cast<float>(texH) / 512.f;  // warnings: larger, readable
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
