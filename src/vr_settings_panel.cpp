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
#include "openxr_helper_bridge.h"

#include <algorithm>
#include <cctype>
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
    constexpr int kSectionGapPx = 18;   // extra space above a heading (not a column's first)
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
    // mat_fxaa_enabled (the game's video-settings FXAA), read while the panel is open.
    bool g_FxaaVideoOn = true;
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
    // their rows show what the panel last set since the last map/save load
    // (ResetDevCheatRows).
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
    // GestureFigure (fmt "L"/"R"): the GESTURES tab's body figure and zone
    // cards; the card lines are the grab-zone slot rows (hidden as rows).
    enum class RowKind { Tab, Section, Toggle, Number, Button, Close, Info, Choice, DevSection, Preset,
        Bind, ColumnBreak, BindPreset, BindPicture, GestureFigure };
    bool IsHeading(RowKind k) { return k == RowKind::Section || k == RowKind::DevSection; }
    struct Row;
    bool r_IsTabReset(const Row& r);

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
    bool r_IsTabReset(const Row& r) { return r.fmt && std::strcmp(r.fmt, "tab_default") == 0; }
    constexpr int kTabResetX0 = 40, kTabResetX1 = 560;   // bottom bar, left of Close
    int TabChangedCount(int tab);
    // "Reset this tab" needs a second click within 3 s (armed = that tab's index).
    int g_TabResetArmed = -1;
    DWORD g_TabResetArmedAt = 0;

    std::vector<Row>& Rows()
    {
        // A Tab row starts a tab; its rows follow until the next Tab row.
        static std::vector<Row> rows = {
            { "GENERAL", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "TURNING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Snap turning", RowKind::Toggle, "SnapTurning", &bmvr::g_SnapTurning, nullptr, 0, 0, 0, nullptr },
            { "Snap angle", RowKind::Number, "SnapTurnAngle", nullptr, &bmvr::g_SnapTurnAngle, 15.f, 15.f, 90.f, "%.0f\xB0" },
            { "Smooth turn speed", RowKind::Number, "TurnSpeed", nullptr, &bmvr::g_TurnSpeed, 0.05f, 0.05f, 1.f, "%.2f" },
            // Off = stick ladders as in the stable release; the rows below only act while it is on.
            { "PHYSICAL LADDERS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Physical ladders", RowKind::Toggle, "PhysicalLadders", &bmvr::g_PhysicalLadders, nullptr, 0, 0, 0, nullptr },
            { "Ladder distance follows arms", RowKind::Toggle, "LadderBodyFollowsArms", &bmvr::g_LadderBodyFollowsArms, nullptr, 0, 0, 0, nullptr },
            { "Centre on the ladder", RowKind::Toggle, "LadderCentreHead", &bmvr::g_LadderCentreHead, nullptr, 0, 0, 0, nullptr },
            { "Gun hand on ladders", RowKind::Choice, "LadderGunHand", nullptr, &bmvr::g_LadderGunHand, 1.f, 0.f, 2.f, "Holster + draw again|Needs empty hand|Holster only" },
            { "Hand wraps the rung", RowKind::Toggle, "LadderHandWrap", &bmvr::g_LadderHandWrap, nullptr, 0, 0, 0, nullptr },
            { "Hand distance from rung", RowKind::Number, "LadderHandBack", nullptr, &bmvr::g_LadderHandBack, 0.5f, 0.f, 10.f, "%.1f" },
            { "Stick climbs ladders too", RowKind::Toggle, "LadderStickClimb", &bmvr::g_LadderStickClimb, nullptr, 0, 0, 0, nullptr },
            // Crouch in real life below the recenter height minus this; the view stays at the real head.
            { "PHYSICAL CROUCHING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Physical crouching", RowKind::Toggle, "PhysicalCrouch", &bmvr::g_PhysicalCrouch, nullptr, 0, 0, 0, nullptr },
            { "Head drop to crouch", RowKind::Number, "PhysicalCrouchDropCm", nullptr, &bmvr::g_PhysicalCrouchDropCm, 5.f, 15.f, 80.f, "%.0f cm" },
            { "VIEW", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Body walks after the real head past the deadzone; view kept near the body (VR::RoomscaleWish/RoomscaleFollow).
            { "Roomscale (beta)", RowKind::Toggle, "Roomscale", &bmvr::g_Roomscale, nullptr, 0, 0, 0, nullptr },
            { "Body follows head past", RowKind::Number, "RoomscaleDeadzoneCm", nullptr, &bmvr::g_RoomscaleDeadzoneCm, 1.f, 5.f, 40.f, "%.0f cm" },
            { "Recenter after pause menu", RowKind::Toggle, "AutoRecenterOnResume", &bmvr::g_AutoRecenterOnResume, nullptr, 0, 0, 0, nullptr },
            { "Cutscene cameras move the view", RowKind::Toggle, "FollowScriptedCamera", &bmvr::g_FollowScriptedCamera, nullptr, 0, 0, 0, nullptr },
            // Main + pause menu at the room's front (STAGE forward) instead of the head (user 2026-11-24: off by default).
            { "Room-anchored menus", RowKind::Toggle, "MenuRoomFront", &bmvr::g_MenuRoomFront, nullptr, 0, 0, 0, nullptr },
            // HeightOffset in cm; applies live to the view and hands.
            { "Height offset", RowKind::Number, "HeightOffset", nullptr, &bmvr::g_HeightOffsetCm, 2.f, -60.f, 60.f, "%+.0f cm" },
            // Subtitles / closed captions (turn them on in the game's audio options).
            { "Subtitle height", RowKind::Number, "CaptionHeight", nullptr, &bmvr::g_CaptionHeightDeg, 1.f, -30.f, 30.f, "%+.0f\xB0" },
            { "Subtitle size", RowKind::Number, "CaptionScale", nullptr, &bmvr::g_CaptionScale, 0.05f, 0.5f, 2.f, "%.2fx" },
            // Small toasts: reload steps, first carried prop / mounted gun (VR::ShowUsageHint).
            { "Usage hints", RowKind::Toggle, "VrUsageHints", &bmvr::g_VrUsageHints, nullptr, 0, 0, 0, nullptr },
            { "Near clip (7 = game)", RowKind::Number, "VrNearZ", nullptr, &bmvr::g_VrNearZ, 0.5f, 1.f, 7.f, "%.1f" },
            // No key/fmt: the button's default action is a recenter (closes the panel).
            { "Recenter", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Main + pause menu panel tuning (user 2026-11-24: whole section dev-only).
            { "GAME MENUS", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Distance", RowKind::Number, "MenuForwardMeters", nullptr, &bmvr::g_MenuForwardMeters, 0.1f, 0.5f, 3.f, "%.1f m" },
            { "Size", RowKind::Number, "MenuWidthMeters", nullptr, &bmvr::g_MenuWidthMeters, 0.1f, 0.6f, 3.f, "%.1f m" },
            { "Height", RowKind::Number, "MenuHeightOffsetMeters", nullptr, &bmvr::g_MenuHeightOffsetMeters, 0.05f, -0.4f, 0.4f, "%+.2f m" },
            { "Pointer smoothing", RowKind::Number, "MenuCursorSmoothSec", nullptr, &bmvr::g_MenuCursorSmoothSec, 0.01f, 0.02f, 0.3f, "%.2f s" },
            { "THIS PANEL", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // The gear only hides while some button opens this panel (VR settings mapped).
            { "VR gear in weapon menu", RowKind::Toggle, "WeaponMenuSettingsGear", &bmvr::g_WeaponMenuSettingsGear, nullptr, 0, 0, 0, nullptr },
            // Glow fades and other effects run on game time: with the game
            // paused a change only shows after the panel closes.
            { "Panel pauses the game", RowKind::Toggle, "VrSettingsPause", &bmvr::g_VrSettingsPause, nullptr, 0, 0, 0, nullptr },
            { "HANDS & WEAPONS", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Hands, hand HUD and the weapon menu (user 2026-11-19: one group). The
            // developer rows (size, gun hand, roll, HUD offsets) only show with
            // "Show developer rows"; the tab reset covers them.
            { "HANDS & HUD", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Health/ammo on the hands", RowKind::Toggle, "VrHandHud", &bmvr::g_HandHud, nullptr, 0, 0, 0, nullptr },
            { "Red edges when hit", RowKind::Toggle, "DamageFlash", &bmvr::g_DamageFlash, nullptr, 0, 0, 0, nullptr },
            { "Hit flash strength", RowKind::Number, "DamageFlashStrength", nullptr, &bmvr::g_DamageFlashStrength, 0.1f, 0.1f, 1.f, "%.1f" },
            // HUD placement presets (rows Up..Roll below): On top / Back of hand / Palm.
            { "HUD placement", RowKind::Preset, nullptr, nullptr, nullptr, 0, 0, 0, "hudspot" },
            { "HUD size", RowKind::Number, "HandHudSize", nullptr, &bmvr::g_HandHudSize, 0.25f, 1.f, 6.f, "%.2f" },
            { "Weapon menu tilt", RowKind::Toggle, "WeaponMenuTilt", &bmvr::g_WeaponMenuTilt, nullptr, 0, 0, 0, nullptr },
            // Gloves are always built from the installed game's own first-person arms.
            { "Hand size", RowKind::Number, "VrHandsBlackMesaScale", nullptr, &bmvr::g_VrHandsBlackMesaScale, 0.05f, 0.4f, 1.5f, "%.2f" },
            { "Gun's own right hand", RowKind::Toggle, "VrGunRightHand", &bmvr::g_VrGunRightHand, nullptr, 0, 0, 0, nullptr },
            { "Hand roll (thumb up)", RowKind::Number, "VrHandsRollDeg", nullptr, &bmvr::g_VrHandsRollDeg, 5.f, -135.f, 135.f, "%+.0f\xB0" },
            { "Up from the hand", RowKind::Number, "HandHudUp", nullptr, &bmvr::g_HandHudUp, 0.25f, -4.f, 8.f, "%+.2f" },
            { "Forward (- = to the wrist)", RowKind::Number, "HandHudForward", nullptr, &bmvr::g_HandHudForward, 0.25f, -8.f, 8.f, "%+.2f" },
            { "Sideways (+ = left)", RowKind::Number, "HandHudSide", nullptr, &bmvr::g_HandHudSide, 0.25f, -6.f, 6.f, "%+.2f" },
            { "Tilt", RowKind::Number, "HandHudTilt", nullptr, &bmvr::g_HandHudTilt, 2.f, -90.f, 90.f, "%+.0f\xB0" },
            { "Roll toward you", RowKind::Number, "HandHudRoll", nullptr, &bmvr::g_HandHudRoll, 2.f, -180.f, 180.f, "%+.0f\xB0" },
            { "FLASHLIGHT", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Flashlight from", RowKind::Choice, "FlashlightFrom", nullptr, &bmvr::g_FlashlightFrom, 1.f, 0.f, 2.f, "Right hand|Left hand|Head" },
            // Support hand on the gun. Grip area scale sizes the zone where a squeeze takes the gun
            // (wider + further forward; its rear end, toward the gun hand, stays put).
            { "WEAPON GRIP", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Toggle grip", RowKind::Toggle, "TwoHandGripToggle", &bmvr::g_TwoHandGripToggle, nullptr, 0, 0, 0, nullptr },
            // Developer row: off = a latched grip lets go only on a second squeeze (or weapon/menu change).
            { "Toggle grip auto release", RowKind::Toggle, "TwoHandToggleAutoRelease", &bmvr::g_TwoHandToggleAutoRelease, nullptr, 0, 0, 0, nullptr },
            { "Grip area scale", RowKind::Number, "TwoHandGripScale", nullptr, &bmvr::g_TwoHandGripScale, 0.1f, 0.5f, 2.f, "%.1fx" },
            { "AIMING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // One preset for dot + beam (user 2026-11-27); "without sights" = tau, gluon,
            // crossbow unscoped. Dot size and beam width are developer rows.
            { "Aim marker", RowKind::Choice, "AimMarker", nullptr, &bmvr::g_AimMarker, 1.f, 0.f, 3.f, "Off|Guns without sights|Red dot|Dot + laser beam" },
            { "Red dot size", RowKind::Number, "VrCrosshairScale", nullptr, &bmvr::g_VrCrosshairScale, 0.1f, 0.2f, 3.f, "%.1f" },
            { "Laser width", RowKind::Number, "VrLaserWidth", nullptr, &bmvr::g_VrLaserWidth, 0.01f, 0.01f, 0.3f, "%.2f" },
            { "Gun angle", RowKind::Number, "GunPitchOffset", nullptr, &bmvr::g_GunPitchOffset, 1.f, -45.f, 45.f, "%+.0f\xB0" },
            { "GRENADES", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Throw at hand speed", RowKind::Toggle, "VrHandThrow", &bmvr::g_VrHandThrow, nullptr, 0, 0, 0, nullptr },
            { "Throw strength", RowKind::Number, "ThrowScale", nullptr, &bmvr::g_ThrowScale, 0.25f, 1.f, 8.f, "%.2f" },
            { "Throw from whole stroke (test)", RowKind::Toggle, "ThrowStroke", &bmvr::g_ThrowStroke, nullptr, 0, 0, 0, nullptr },
            // Left column: hands & HUD, flashlight, aiming, grenades (user 2026-11-23: aiming moved left for balance;
            // 2026-11-24: flashlight above aiming, grenades/scope/mounted guns developer-only).
            { "", RowKind::ColumnBreak, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "AMMO FEEDBACK", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Last shot buzz", RowKind::Choice, "LastShotHaptic", nullptr, &bmvr::g_LastShotHaptic, 1.f, 0.f, 2.f, "Off|Double|Long" },
            { "Last shot strength", RowKind::Number, "LastShotHapticStrength", nullptr, &bmvr::g_LastShotHapticStrength, 0.05f, 0.05f, 1.f, "%.2f" },
            { "Empty trigger buzz", RowKind::Choice, "DryFireHaptic", nullptr, &bmvr::g_DryFireHaptic, 1.f, 0.f, 2.f, "Off|Tick|Nope (double)" },
            { "Empty trigger strength", RowKind::Number, "DryFireHapticStrength", nullptr, &bmvr::g_DryFireHapticStrength, 0.05f, 0.05f, 1.f, "%.2f" },
            // The game's own auto-switches, both off by default (user 2026-11-23).
            { "WEAPON AUTO-SWITCH", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Switch to picked-up weapon", RowKind::Toggle, "AutoSwitchPickup", &bmvr::g_AutoSwitchPickup, nullptr, 0, 0, 0, nullptr },
            { "Switch away when empty", RowKind::Toggle, "AutoSwitchEmpty", &bmvr::g_AutoSwitchEmpty, nullptr, 0, 0, 0, nullptr },
            { "PISTOLS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "One shot per trigger pull", RowKind::Toggle, "VrPistolSemiAuto", &bmvr::g_VrPistolSemiAuto, nullptr, 0, 0, 0, nullptr },
            { "Glock: fast pulls = alt-fire", RowKind::Toggle, "VrGlockRapidAlt", &bmvr::g_VrGlockRapidAlt, nullptr, 0, 0, 0, nullptr },
            { "CROSSBOW SCOPE", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Raise distance", RowKind::Number, "ScopeRaiseMeters", nullptr, &bmvr::g_ScopeRaiseMeters, 0.02f, 0.1f, 0.6f, "%.2f m" },
            { "Zoom smoothing", RowKind::Number, "ScopeGunSmoothSec", nullptr, &bmvr::g_ScopeGunSmoothSec, 0.02f, 0.f, 0.5f, "%.2f s" },
            { "MOUNTED GUNS", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Turn per hand move", RowKind::Number, "MountedGunGain", nullptr, &bmvr::g_MountedGunGain, 0.25f, 0.5f, 4.f, "%.2fx" },
            // Off = the stable release's reloads; the rows below only act while it is on (manual
            // mag/shell/RPG reloads always, snarks thrown from the hand too). The support hand's
            // finger pose + lock and the satchel hands are always on (user 2026-11-19).
            { "PHYSICAL WEAPONS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Physical weapons", RowKind::Toggle, "PhysicalWeapons", &bmvr::g_PhysicalWeapons, nullptr, 0, 0, 0, nullptr },
            { "Ejected rounds are lost", RowKind::Toggle, "VrMagDiscard", &bmvr::g_VrMagDiscard, nullptr, 0, 0, 0, nullptr },
            // Developer row: A/B for the hand poses taken from Black Mesa's reload clips.
            { "Hand pose from reload clips", RowKind::Toggle, "VrClipHandPose", &bmvr::g_VrClipHandPose, nullptr, 0, 0, 0, nullptr },
            { "Hand locks onto bolt/slide", RowKind::Toggle, "VrClipHandSnap", &bmvr::g_VrClipHandSnap, nullptr, 0, 0, 0, nullptr },
            { "Recoil", RowKind::Toggle, "VrRecoil", &bmvr::g_VrRecoil, nullptr, 0, 0, 0, nullptr },
            { "Recoil strength", RowKind::Number, "VrRecoilScale", nullptr, &bmvr::g_VrRecoilScale, 0.25f, 0.25f, 3.f, "%.2f" },
            // Kick with the second hand on the gun, as a share of one-handed (pistols x5/3, capped at 1).
            { "Recoil with two hands", RowKind::Number, "VrRecoilTwoHand", nullptr, &bmvr::g_VrRecoilTwoHand, 0.05f, 0.f, 1.f, "%.2f" },
            { "GESTURES", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Back view of the body with a card per zone (GestureFigure, user
            // 2026-11-23): the left column's figure row paints the figure across
            // the divider and the left-side cards, the right one the right-side
            // cards. The card lines are the slot rows below (always hidden).
            { "", RowKind::GestureFigure, nullptr, nullptr, nullptr, 0, 0, 0, "L" },
            // A row per zone and hand, Off as a choice (user 2026-11-19, per hand
            // 2026-11-23). Left hand: an action (quick load = 1 s hold). Right hand:
            // a weapon, buzzes and takes the grip only while you carry it; Dynamic =
            // the weapon you last put away there (ChoiceText shows which).
            { "Left shoulder, left hand", RowKind::Choice, "GestureLeftShoulder", nullptr, &bmvr::g_GestureLeft[bmvr::kGestureShoulderLeft], 1.f, -1.f, 5.f, "Off|Quick save|Flashlight|Ammo|Quick load (hold)|Screenshot|Recenter" },
            { "Left shoulder, right hand", RowKind::Choice, "GestureWeaponShoulderLeft", nullptr, &bmvr::g_GestureRight[bmvr::kGestureShoulderLeft], 1.f, -1.f, 11.f, "Off|Crowbar|Pistol|.357|MP5|Shotgun|Crossbow|RPG|Tau|Gluon|Hivehand|Grenade|Dynamic" },
            { "Right shoulder, left hand", RowKind::Choice, "GestureLeftHandShoulderRight", nullptr, &bmvr::g_GestureLeft[bmvr::kGestureShoulderRight], 1.f, -1.f, 5.f, "Off|Quick save|Flashlight|Ammo|Quick load (hold)|Screenshot|Recenter" },
            { "Right shoulder, right hand", RowKind::Choice, "GestureWeaponShoulder", nullptr, &bmvr::g_GestureRight[bmvr::kGestureShoulderRight], 1.f, -1.f, 11.f, "Off|Crowbar|Pistol|.357|MP5|Shotgun|Crossbow|RPG|Tau|Gluon|Hivehand|Grenade|Dynamic" },
            { "Left hip, left hand", RowKind::Choice, "GestureLeftHandHipLeft", nullptr, &bmvr::g_GestureLeft[bmvr::kGestureHipLeft], 1.f, -1.f, 5.f, "Off|Quick save|Flashlight|Ammo|Quick load (hold)|Screenshot|Recenter" },
            { "Left hip, right hand", RowKind::Choice, "GestureWeaponHipLeft", nullptr, &bmvr::g_GestureRight[bmvr::kGestureHipLeft], 1.f, -1.f, 11.f, "Off|Crowbar|Pistol|.357|MP5|Shotgun|Crossbow|RPG|Tau|Gluon|Hivehand|Grenade|Dynamic" },
            { "Right hip, left hand", RowKind::Choice, "GestureLeftHandHipRight", nullptr, &bmvr::g_GestureLeft[bmvr::kGestureHipRight], 1.f, -1.f, 5.f, "Off|Quick save|Flashlight|Ammo|Quick load (hold)|Screenshot|Recenter" },
            { "Right hip, right hand", RowKind::Choice, "GestureWeaponHipRight", nullptr, &bmvr::g_GestureRight[bmvr::kGestureHipRight], 1.f, -1.f, 11.f, "Off|Crowbar|Pistol|.357|MP5|Shotgun|Crossbow|RPG|Tau|Gluon|Hivehand|Grenade|Dynamic" },
            { "Forehead, left hand", RowKind::Choice, "GestureLeftHandHead", nullptr, &bmvr::g_GestureLeft[bmvr::kGestureHead], 1.f, -1.f, 5.f, "Off|Quick save|Flashlight|Ammo|Quick load (hold)|Screenshot|Recenter" },
            { "", RowKind::ColumnBreak, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "", RowKind::GestureFigure, nullptr, nullptr, nullptr, 0, 0, 0, "R" },
            { "CONTROLS", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Left column = left controller, right column = right controller. Headings name
            // the detected controller; button names follow it (A/B, Cross/Circle).
            { "LAYOUT", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Preset", RowKind::BindPreset, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Hold this long for a button's hold action; let go earlier for its press action.
            { "Hold time", RowKind::Number, "ButtonHoldMs", nullptr, &bmvr::g_ButtonHoldMs, 50.f, 200.f, 1500.f, "%.0f ms" },
            // ButtonMapping=false in config.txt (no row): the fixed layout from before
            // mapping, for support if a mapping ever blocks a button.
            // Off (or no SteamVR model found): the button list below.
            { "Controller pictures", RowKind::Toggle, "ControlsPicture", &bmvr::g_ControlsPicture, nullptr, 0, 0, 0, nullptr },
            { "LEFT CONTROLLER", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, "bind_head_left" },
            { "", RowKind::BindPicture, nullptr, nullptr, nullptr, 0, 0, 0, "L" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.trigger" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.grip" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.bumper" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.bumper.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face1" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face1.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face2" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face2.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face3" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face3.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face4" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.face4.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.stickclick" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "L.stickclick.hold" },
            { "Stick", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "fixedL:Move" },
            { "Menu button", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "fixedL:Pause" },
            { "", RowKind::ColumnBreak, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Right stick pushed up / down = the Stick up / Stick down actions below.
            // Crouch hold vs toggle is the Stick down mapping (was CrouchStickHold).
            { "RIGHT STICK: JUMP / CROUCH", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "How far to push", RowKind::Number, "StickJumpCrouchThreshold", nullptr, &bmvr::g_StickJumpCrouchThreshold, 0.05f, 0.3f, 0.95f, "%.2f" },
            { "Max sideways angle", RowKind::Number, "StickJumpCrouchAngle", nullptr, &bmvr::g_StickJumpCrouchAngle, 5.f, 10.f, 45.f, "%.0f\xB0" },
            { "No jump/crouch while turning", RowKind::Toggle, "TurnBlocksJumpCrouch", &bmvr::g_TurnBlocksJumpCrouch, nullptr, 0, 0, 0, nullptr },
            { "RIGHT CONTROLLER", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, "bind_head_right" },
            { "", RowKind::BindPicture, nullptr, nullptr, nullptr, 0, 0, 0, "R" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.trigger" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.grip" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.bumper" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.bumper.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face1" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face1.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face2" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face2.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face3" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face3.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face4" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.face4.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.stickclick" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.stickclick.hold" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.stickup" },
            { "", RowKind::Bind, nullptr, nullptr, nullptr, 0, 0, 0, "R.stickdown" },
            { "Stick sideways", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "fixedR:Turn" },
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
            { "ANTI-ALIASING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "MSAA on edges", RowKind::Choice, "Msaa", nullptr, &bmvr::g_MsaaF, 2.f, 0.f, 4.f, "Off|2x|4x" },
            // Leaves, grass, fences: their cut-out edges get the MSAA samples
            // too (softer, less crawl; can look a bit thinner). Needs MSAA on.
            { "MSAA on leaves + fences", RowKind::Toggle, "AlphaToCoverage", &bmvr::g_AlphaToCoverage, nullptr, 0, 0, 0, nullptr },
            // Smoke, sparks and drips are drawn last, after the MSAA resolve:
            // much cheaper where particles fill the view. Models, wires and
            // glass keep MSAA; the gun loses it only while particles are in view.
            { "MSAA skips particles", RowKind::Toggle, "MsaaParticles1x", &bmvr::g_MsaaParticles1x, nullptr, 0, 0, 0, nullptr },
            // Sets the three values below; "Custom" when they match no preset.
            { "FXAA preset", RowKind::Preset, nullptr, nullptr, nullptr, 0, 0, 0, "fxaa" },
            { "Sub-pixel smoothing", RowKind::Number, "FxaaSubpixel", nullptr, &bmvr::g_FxaaSubpixel, 0.05f, 0.f, 1.f, "%.2f" },
            { "Edge threshold", RowKind::Number, "FxaaEdgeThreshold", nullptr, &bmvr::g_FxaaEdgeThreshold, 0.01f, 0.f, 0.5f, "%.3f" },
            { "Dark edge threshold", RowKind::Number, "FxaaEdgeThresholdMin", nullptr, &bmvr::g_FxaaEdgeThresholdMin, 0.005f, 0.f, 0.1f, "%.3f" },
            // Cubemap reflection strength: less = less shimmer on shiny floors/walls and props.
            { "SHIMMER PREVENTION", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Reflections preset", RowKind::Preset, nullptr, nullptr, nullptr, 0, 0, 0, "envmap" },
            { "Walls/floors (cubemap)", RowKind::Number, "EnvmapScaleWorld", nullptr, &bmvr::g_EnvmapScaleWorld, 0.1f, 0.f, 1.f, "%.1f" },
            { "Props/NPCs (cubemap)", RowKind::Number, "EnvmapScaleModels", nullptr, &bmvr::g_EnvmapScaleModels, 0.1f, 0.f, 1.f, "%.1f" },
            { "LIGHTING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Bloom and auto-exposure", RowKind::Toggle, "EyeBloom", &bmvr::g_EyeBloom, nullptr, 0, 0, 0, nullptr },
            // Mip LOD bias in the 3D views (experimental): + = softer textures;
            // foliage and fences look more solid, at the cost of detail. Does
            // not fix the shimmering sand outdoors. Menus and the HUD stay sharp.
            { "Texture calm (LOD bias)", RowKind::Number, "LodBias", nullptr, &bmvr::g_LodBias, 0.25f, -0.5f, 2.f, "%+.2f" },
            { "LENS FLARES", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Fade out at angle", RowKind::Number, "FlareFadeAngle", nullptr, &bmvr::g_FlareFadeAngle, 5.f, 0.f, 90.f, "%.0f\xB0" },
            { "Strength on the light", RowKind::Number, "FlareLightScale", nullptr, &bmvr::g_FlareLightScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "Large round haze", RowKind::Number, "FlareHazeScale", nullptr, &bmvr::g_FlareHazeScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "Haze from size", RowKind::Number, "FlareHazeMinSize", nullptr, &bmvr::g_FlareHazeMinSize, 0.05f, 0.05f, 4.f, "%.2f" },
            { "Ghosts away from light", RowKind::Number, "FlareGhostScale", nullptr, &bmvr::g_FlareGhostScale, 0.05f, 0.f, 1.f, "%.2f" },
            { "Reset flare values", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "flare_default" },
            { "LIGHT GLOWS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Glow smoothing", RowKind::Number, "GlowSmooth", nullptr, &bmvr::g_GlowSmooth, 0.05f, 0.f, 0.5f, "%.2f s" },
            { "Dark when hidden after", RowKind::Number, "GlowHideMs", nullptr, &bmvr::g_GlowHideMs, 10.f, 0.f, 500.f, "%.0f ms" },
            { "Reset glow values", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "glow_default" },
            { "DEBUG", RowKind::Tab, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // One section, so "Show developer rows" keeps its spot (left column) when the rows below appear.
            { "DIAGNOSTICS", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Offsets that no longer match the game: gameplay ones also raise the "VR MOD OUT
            // OF DATE" toast, debug-only ones (PerfLog stats, profiling) only show here.
            { "Missing hooks", RowKind::Info, nullptr, nullptr, nullptr, 0, 0, 0, "missing_hooks" },
            { "Capture debug frame", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "debug_capture" },
            // User 2026-11-26 (Xen ground fog through the floor in the research camp): a capture
            // with the global Xen fog drawn + the same eyes without it (VR/fogdump-*).
            { "Fog dump", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "fog_dump" },
            { "Screenshot (eyes only)", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "screenshot" },
            // Back (user 2026-11-20, Xen plant glow that follows the hand): 20 s of every
            // light glow's world position + brightness into VR/glowtrace-*.csv.
            { "Record glow trace (20 s)", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "glow_trace" },
            // Leftover MODEL matrix on world brushes (2026-11-26): A/B when walls look off doors/props. Not saved.
            { "Reset model each eye", RowKind::Toggle, nullptr, &bmvr::g_DebugModelReset, nullptr, 0, 0, 0, nullptr },
            // One shot: the next Pause press saves 12 eye pairs + 12 menu layers (VR/pausecap-*).
            { "Capture next pause", RowKind::Toggle, nullptr, &bmvr::g_PauseCaptureArm, nullptr, 0, 0, 0, nullptr },
            // User 2026-11-27 (laser stops in mid-air next to cover): per trigger press, what the
            // shot ray hit + the same aim from the eye (bmvr_log "AimHit"). Saved, so it can stay on.
            { "Log what the laser hits", RowKind::Toggle, "AimHitLog", &bmvr::g_AimHitLog, nullptr, 0, 0, 0, nullptr },
            // User 2026-11-27 (ammo HUD a frame ahead of the gun hand, gun hand gone for a frame
            // in heavy scenes): bmvr_log "Hand sync" lines + a 10 s summary. Saved.
            { "Log hand/gun sync", RowKind::Toggle, "HandSyncLog", &bmvr::g_HandSyncLog, nullptr, 0, 0, 0, nullptr },
            { "Performance log (costs fps)", RowKind::Toggle, "PerfLog", &bmvr::g_PerfLog, nullptr, 0, 0, 0, nullptr },
            // Eye GPU ms every ~5 s (timestamp queries); with the always-on "Frame budget" CPU line it shows
            // whether a scene is CPU- or GPU-limited.
            { "GPU timing in log", RowKind::Toggle, "GpuPassTiming", &bmvr::g_GpuPassTiming, nullptr, 0, 0, 0, nullptr },
            // Shows the DevSection groups below (A/B switches, tests, engine cvars); saved.
            { "Blank eye", RowKind::Choice, nullptr, nullptr, &bmvr::g_DebugBlankEye, 1.f, 0.f, 2.f, "Off|Left|Right" },
            { "Show developer rows", RowKind::Toggle, "VrPanelDevRows", &bmvr::g_PanelDevRows, nullptr, 0, 0, 0, nullptr },
            // Every row of this tab back to its built-in default (logs, A/B fixes, tests, engine rows;
            // god/noclip/notarget off). "Show developer rows" stays as it is.
            // CPU/GPU work removed without changing the image; off = the stock path, for A/B.
            { "SAVINGS (SAME IMAGE)", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Post copies: swap, no copy", RowKind::Toggle, "PostCopySwap", &bmvr::g_PostCopySwap, nullptr, 0, 0, 0, nullptr },
            { "Light shadows: once per frame", RowKind::Toggle, "ShadowsOncePerFrame", &bmvr::g_ShadowsOncePerFrame, nullptr, 0, 0, 0, nullptr },
            { "Limit refractions per eye", RowKind::Toggle, "RefractOncePerEye", &bmvr::g_RefractOncePerEye, nullptr, 0, 0, 0, nullptr },
            // Copies per eye the row above lets through (1 = once); more = glass behind glass refracts again.
            { "Refraction copies per eye", RowKind::Number, "RefractCopiesPerEye", nullptr, &bmvr::g_RefractCopiesPerEye, 6.f, 1.f, 20.f, "%.0f" },
            // Capture/view tests are not saved (no key): always off after a restart.
            { "LOGS", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // Lens flare positions per eye into the log; switch on when a flare sits in the wrong place.
            { "Flare position log", RowKind::Toggle, nullptr, &bmvr::g_FlareLog, nullptr, 0, 0, 0, nullptr },
            { "VIEW TESTS", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Freeze head pose", RowKind::Toggle, nullptr, &bmvr::g_FreezeHead, nullptr, 0, 0, 0, nullptr },
            // Skips Black Mesa's Xen fog (Xog) draws by material: global = __gbxog0,
            // volumes = the local fog volumes. Draw hooks run while on.
            { "Hide Xen fog", RowKind::Choice, nullptr, nullptr, &bmvr::g_DebugHideXog, 1.f, 0.f, 3.f, "Off|Global|Volumes|Both" },
            // Experiments; saved like the other settings, off by default.
            { "TESTS", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Glow test area size", RowKind::Number, "GlowProxyScale", nullptr, &bmvr::g_GlowProxyScale, 0.5f, 1.f, 8.f, "%.1fx" },
            // User 2026-11-27: "006" rockets; A/B the HUD without the dim leading zeros.
            { "HUD leading zeros", RowKind::Choice, "HudLeadingZeros", nullptr, &bmvr::g_HudLeadingZeros, 1.f, 0.f, 2.f, "Shown|Off on ammo|Off on ammo + health" },
            { "RPG laser ring size", RowKind::Number, "VrRpgRingScale", nullptr, &bmvr::g_VrRpgRingScale, 0.05f, 0.1f, 3.f, "%.2fx" },
            // Grab-zone body frame (see "Grab zones follow the body" under FIXES).
            { "Body yaw: dead zone", RowKind::Number, "BodyYawDeadzoneDeg", nullptr, &bmvr::g_BodyYawDeadzoneDeg, 5.f, 0.f, 90.f, "%.0f\xB0" },
            { "Body yaw: hard limit", RowKind::Number, "BodyYawHardLimitDeg", nullptr, &bmvr::g_BodyYawHardLimitDeg, 5.f, 20.f, 180.f, "%.0f\xB0" },
            { "Body yaw: catch-up delay", RowKind::Number, "BodyYawCatchUpSec", nullptr, &bmvr::g_BodyYawCatchUpSec, 0.1f, 0.f, 5.f, "%.1f s" },
            { "Head hold: angle", RowKind::Number, "PoseHoldDeg", nullptr, &bmvr::g_PoseHoldDeg, 0.02f, 0.f, 1.f, "%.2f\xB0" },
            { "Head hold: distance", RowKind::Number, "PoseHoldMm", nullptr, &bmvr::g_PoseHoldMm, 0.5f, 0.f, 20.f, "%.1f mm" },
            // Engine cvars: "cvar:" keys go to the console on the game thread
            // (never saved); the shown value is read back while the panel is open.
            { "ENGINE (CONSOLE)", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
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
            { "World model leak fix", RowKind::Toggle, "WorldModelFix", &bmvr::g_WorldModelFix, nullptr, 0, 0, 0, nullptr },
            { "Glow through objects fix", RowKind::Toggle, nullptr, &bmvr::g_GlowCountDepthFix, nullptr, 0, 0, 0, nullptr },
            { "Lights culled per eye", RowKind::Toggle, nullptr, &bmvr::g_LightCullEyeView, nullptr, 0, 0, 0, nullptr },
            { "Flares on each eye's axis", RowKind::Toggle, nullptr, &bmvr::g_FlareEyeAxis, nullptr, 0, 0, 0, nullptr },
            { "Sky water per eye", RowKind::Toggle, nullptr, &bmvr::g_SkyWaterPerEye, nullptr, 0, 0, 0, nullptr },
            { "Underwater world fix", RowKind::Toggle, nullptr, &bmvr::g_EyeUnderwaterWorld, nullptr, 0, 0, 0, nullptr },
            { "Xen ground fog off", RowKind::Toggle, nullptr, &bmvr::g_XenGlobalFogOff, nullptr, 0, 0, 0, nullptr },
            // User 2026-11-27: fog noise was looked up by view direction (at infinity in stereo).
            // Auto (user 2026-11-27): the lab floor needs World, far walls blotch with it.
            { "Xen fog noise", RowKind::Choice, "XenFogNoise", nullptr, &bmvr::g_XenFogNoiseMode, 1.f, 0.f, 3.f, "Stock|Centre eye|World|Auto" },
            { "Xen fog noise size", RowKind::Number, "XenFogNoiseSize", nullptr, &bmvr::g_XenFogNoiseSize, 32.f, 32.f, 2048.f, "%.0f u" },
            { "Xen fog: world noise up to", RowKind::Number, "XenFogWorldNear", nullptr, &bmvr::g_XenFogWorldNear, 50.f, 0.f, 4000.f, "%.0f u" },
            { "Xen fog: centre eye from", RowKind::Number, "XenFogWorldFar", nullptr, &bmvr::g_XenFogWorldFar, 100.f, 100.f, 8000.f, "%.0f u" },
            // User 2026-11-27: god rays changed with head pitch (mask depth along the view axis).
            { "God rays steady with head pitch", RowKind::Toggle, "GodRayRadialMask", &bmvr::g_GodRayRadialMask, nullptr, 0, 0, 0, nullptr },
            { "God rays samples (test)", RowKind::Choice, nullptr, nullptr, &bmvr::g_GodRaySamples, 1.f, 0.f, 2.f, "Game|32|16" },
            { "God rays blur size", RowKind::Choice, "GodRayBlurScale", nullptr, &bmvr::g_GodRayBlurScale, 1.f, 0.f, 2.f, "Full|Half|Quarter" },
            { "Particles after water", RowKind::Toggle, "ParticlesLast", &bmvr::g_ParticlesLast, nullptr, 0, 0, 0, nullptr },
            { "Titles glow + scanlines", RowKind::Toggle, nullptr, &bmvr::g_TitleGlow, nullptr, 0, 0, 0, nullptr },
            { "HEV boot-up HUD inset", RowKind::Number, nullptr, nullptr, &bmvr::g_BootHudInset, 25.f, 0.f, 500.f, "%.0f px" },
            { "HEV boot-up HUD size", RowKind::Number, nullptr, nullptr, &bmvr::g_BootHudScale, 0.05f, 0.5f, 2.f, "%.2fx" },
            { "FIXES: TRACKING, WEAPONS, GAME", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Hands + laser per-frame snapshot", RowKind::Toggle, nullptr, &bmvr::g_McHandSnapshot, nullptr, 0, 0, 0, nullptr },
            { "No stun/blur overlays", RowKind::Toggle, nullptr, &bmvr::g_VrSkipScreenEffects, nullptr, 0, 0, 0, nullptr },
            { "Gun hand cuff closed", RowKind::Toggle, nullptr, &bmvr::g_GunHandCuffCap, nullptr, 0, 0, 0, nullptr },
            { "Grab zones follow the body", RowKind::Toggle, nullptr, &bmvr::g_GestureBodyFrame, nullptr, 0, 0, 0, nullptr },
            { "Pump stroke at the controller", RowKind::Toggle, nullptr, &bmvr::g_PumpStrokeAtController, nullptr, 0, 0, 0, nullptr },
            { "Tripmine placed along its face", RowKind::Toggle, nullptr, &bmvr::g_TripmineFaceAim, nullptr, 0, 0, 0, nullptr },
            // Live per-weapon support hand + mag seat trim for the weapon in the hand (saved in
            // VR/viewmodel_offsets.txt by VR::SyncHeldWeaponAngleRows). The angle trims
            // (pitch/yaw/roll) have no rows since the hand-pose hold (user 2026-11-24);
            // values in the file still apply.
            { "HELD WEAPON TUNING (SAVED PER WEAPON)", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Support hand (+ forward)", RowKind::Number, nullptr, nullptr, &bmvr::g_VmTuneSupportCm, 1.f, -20.f, 20.f, "%+.0f cm" },
            { "Mag seat distance", RowKind::Number, nullptr, nullptr, &bmvr::g_VmTuneSeatCm, 0.5f, 1.f, 30.f, "%.1f cm" },
            { "Mag seat angle (180 = any)", RowKind::Number, nullptr, nullptr, &bmvr::g_VmTuneSeatDeg, 5.f, 5.f, 180.f, "%.0f deg" },
            // Ammo panel beside the gun hand, for the weapon in the hand (VR/ammo_hud.txt,
            // VR::SyncAmmoHudRows). User 2026-11-25: tune each weapon while playing;
            // ranges +25% (user 2026-11-25: some guns hit the limits).
            { "AMMO HUD (SAVED PER WEAPON)", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Ammo HUD forward (- = back)", RowKind::Number, nullptr, nullptr, &bmvr::g_AmmoHudForward, 0.25f, -10.f, 10.f, "%+.2f" },
            { "Ammo HUD sideways (+ = out)", RowKind::Number, nullptr, nullptr, &bmvr::g_AmmoHudSide, 0.25f, -7.5f, 7.5f, "%+.2f" },
            { "Ammo HUD up", RowKind::Number, nullptr, nullptr, &bmvr::g_AmmoHudUp, 0.25f, -7.5f, 7.5f, "%+.2f" },
            { "Ammo HUD tilt (top toward you)", RowKind::Number, nullptr, nullptr, &bmvr::g_AmmoHudTilt, 5.f, -115.f, 115.f, "%+.0f\xB0" },
            { "Ammo HUD turn (front toward you)", RowKind::Number, nullptr, nullptr, &bmvr::g_AmmoHudTurn, 5.f, -115.f, 115.f, "%+.0f\xB0" },
            { "Ammo HUD roll", RowKind::Number, nullptr, nullptr, &bmvr::g_AmmoHudRoll, 5.f, -225.f, 225.f, "%+.0f\xB0" },
            { "Ammo HUD size", RowKind::Number, nullptr, nullptr, &bmvr::g_AmmoHudSize, 0.25f, 0.4f, 7.5f, "%.2f" },
            // Where the health/HEV panel sits next to it with both hands on the gun
            // (user 2026-11-27), in the ammo panel's plane.
            { "Health HUD with two hands", RowKind::Choice, nullptr, nullptr, &bmvr::g_AmmoHudHealthSide, 1.f, 0.f, 3.f, "Above ammo|Left of ammo|Right of ammo|Below ammo" },
            { "Health HUD gap", RowKind::Number, nullptr, nullptr, &bmvr::g_AmmoHudHealthGap, 0.05f, -1.f, 2.f, "%+.2f" },
            // A/B (user 2026-11-25): off = with both hands on a long gun the panel
            // stays on the gun's aim; on = the old gun-hand controller rotation.
            { "Two hands: ammo HUD follows hand", RowKind::Toggle, nullptr, &bmvr::g_TwoHandAmmoHudFollowsHand, nullptr, 0, 0, 0, nullptr },
            // Per ladder model, for the last rung held (VR::SyncLadderGripRows).
            { "LADDER GRIP TUNING (SAVED PER LADDER MODEL)", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Grip out (toward you)", RowKind::Number, nullptr, nullptr, &bmvr::g_LadderTuneOutCm, 0.5f, -10.f, 10.f, "%+.1f cm" },
            { "Grip up", RowKind::Number, nullptr, nullptr, &bmvr::g_LadderTuneUpCm, 0.5f, -10.f, 10.f, "%+.1f cm" },
            { "Grip curl", RowKind::Number, nullptr, nullptr, &bmvr::g_LadderTuneCurl, 0.05f, 0.f, 1.f, "%.2f" },
            { "G-MAN GESTURE TUNING", RowKind::DevSection, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Force gesture (tuning)", RowKind::Toggle, nullptr, &bmvr::g_VrGmanGestureForce, nullptr, 0, 0, 0, nullptr },
            { "Finger straight", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanFingerStraight, 0.05f, 0.f, 0.5f, "%.2f" },
            { "Thumb curl", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanThumbCurl, 0.1f, 0.f, 2.f, "%.1f" },
            { "Thumb swing", RowKind::Number, nullptr, nullptr, &bmvr::g_VrGmanThumbSwing, 0.1f, 0.f, 1.5f, "%.1f" },
            { "Finger twist", RowKind::Number, "VrGmanFingerTwist", nullptr, &bmvr::g_VrGmanFingerTwistDeg, 1.f, -30.f, 30.f, "%+.0f deg" },
            // Drawn at the bottom of every tab.
            { "Close", RowKind::Close, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            // In the bottom bar, far left: resets the tab shown (second click confirms).
            { "Reset tab", RowKind::Button, nullptr, nullptr, nullptr, 0, 0, 0, "tab_default" },
        };
        return rows;
    }

    // Single developer rows inside a player section (user's panel review,
    // 2026-11-19): shown only with "Show developer rows", like a DevSection.
    bool IsDevRow(const Row& r)
    {
        const char* k = r.key ? r.key : (r.kind == RowKind::Button ? r.fmt : nullptr);
        if (!k)
            return false;
        static const char* kDev[] = {
            "LadderBodyFollowsArms", "LadderCentreHead", "LadderHandBack", "LadderHandWrap", "VrNearZ", "MenuHeightOffsetMeters", "MenuCursorSmoothSec",
            "ControlsPicture", "DamageFlashStrength", "VrHandsBlackMesaScale", "VrGunRightHand", "VrHandsRollDeg",
            "HandHudUp", "HandHudForward", "HandHudSide", "HandHudTilt", "HandHudRoll",
            "ScopeRaiseMeters", "ScopeGunSmoothSec", "ThrowStroke", "MountedGunGain", "LastShotHapticStrength", "DryFireHapticStrength", "VrClipHandPose", "VrClipHandSnap",
            "TwoHandToggleAutoRelease", "VrCrosshairScale", "VrLaserWidth",
            "MsaaParticles1x", "FxaaSubpixel", "FxaaEdgeThreshold", "FxaaEdgeThresholdMin",
            "EnvmapScaleWorld", "EnvmapScaleModels", "LodBias",
            "FlareFadeAngle", "FlareLightScale", "FlareHazeScale", "FlareHazeMinSize", "FlareGhostScale", "flare_default",
            "GlowSmooth", "GlowHideMs", "glow_default",
        };
        for (const char* d : kDev)
            if (std::strcmp(k, d) == 0)
                return true;
        return false;
    }

    int PictureAreaHeight(int hand);
    // GESTURES tab figure (PaintGestureFigure): its row height, the slot row
    // under a texture point (px absolute, ly from the row top), and the grab
    // zones a hand is in now (bit z = left hand, bit 8 + z = right hand; set
    // by VR::DrawSettingsPanel while the tab shows, lights the zone up).
    constexpr int kGestureFigureH = 990;
    int GestureFigureHit(bool rightSide, int px, int ly);
    bool IsGestureSlotRow(const Row& r);
    unsigned g_GestureHot = 0;
    int RowHeight(const Row& r)
    {
        if (r.kind == RowKind::ColumnBreak)
            return 0;
        if (r.kind == RowKind::GestureFigure)
            return kGestureFigureH;
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
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_FRAME: return "Steam Frame";
        default: return "not detected yet";
        }
    }
    // The controller's name is shown once, on the pictures switch (user
    // 2026-11-18: it was in both controller headings).
    std::string SectionText(const Row& r)
    {
        return r.label;
    }
    std::string RowLabelText(const Row& r)
    {
        if (r.b == &bmvr::g_ControlsPicture)
        {
            const char* name = ControllerName();
            if (std::strcmp(name, "not detected yet") == 0)
                return "Controller pictures (no controller detected)";
            return std::string(name) + " pictures";
        }
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
                case bmvr::BindInput::Face3: part = bmvr::ctlmodel::kFace3; break;
                case bmvr::BindInput::Face4: part = bmvr::ctlmodel::kFace4; break;
                case bmvr::BindInput::Bumper: part = bmvr::ctlmodel::kBumper; break;
                case bmvr::BindInput::StickClick: caption = "Click"; break;
                case bmvr::BindInput::StickUp: caption = "Up"; break;
                case bmvr::BindInput::StickDown: caption = "Down"; break;
                default: break;
                }
                // Steam Frame left hand: the four D-pad directions are one
                // mesh (found as kFace1): one "D-pad" card, a line per direction.
                const bool dpad = hand == 0 && g_PanelFamily == L4D2VR_OPENXR_CONTROLLER_FAMILY_FRAME
                    && (b.input == bmvr::BindInput::Face1 || b.input == bmvr::BindInput::Face2
                        || b.input == bmvr::BindInput::Face3 || b.input == bmvr::BindInput::Face4);
                if (dpad)
                {
                    part = bmvr::ctlmodel::kFace1;
                    caption = b.input == bmvr::BindInput::Face1 ? "Down" : b.input == bmvr::BindInput::Face2 ? "Right"
                        : b.input == bmvr::BindInput::Face3 ? "Left" : "Up";
                }
                if (b.hold)
                    caption = caption.empty() ? "Hold" : caption + " hold";
                if (!hm.has[part])
                    part = bmvr::ctlmodel::kStick;   // drawn without a line if that is missing too
                const std::string head = part == bmvr::ctlmodel::kStick ? (pad ? "Pad" : "Stick")
                    : dpad ? "D-pad"
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
            // Grab zones of this hand: left = actions, right = weapons.
            std::string zones;
            auto addZone = [&](int z, const char* what) {
                zones += zones.empty() ? "" : ", ";
                zones += std::string(bmvr::GestureZoneName(z)) + " " + what;
            };
            if (hand == 0)
            {
                for (int z = 0; z < bmvr::kGestureZoneCount; ++z)
                {
                    const int a = static_cast<int>(std::lround(bmvr::g_GestureLeft[z]));
                    if (a >= 0 && (a != bmvr::kGestureAmmo || bmvr::PhysicalWeapon(bmvr::g_VrMagReload)))
                        addZone(z, bmvr::GestureActionName(a));
                }
            }
            else
            {
                for (int z = 0; z < bmvr::kGestureWeaponZoneCount; ++z)
                {
                    const int setting = static_cast<int>(std::lround(bmvr::g_GestureRight[z]));
                    const int w = bmvr::GestureWeaponAt(z);
                    if (setting >= 0)
                        addZone(z, setting == bmvr::kGestureDynamic && w < 0 ? "dynamic" : bmvr::GestureWeaponName(w));
                }
            }
            if (!zones.empty())
                fixedLine("Zones", zones);
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
    // The picker also serves Choice rows with many options (the shoulder/hip
    // weapon rows: 11 weapons, cycling forward only was slow; user 2026-11-18).
    std::vector<std::string> ChoiceOptions(const Row& r)
    {
        std::vector<std::string> out;
        if (!r.fmt)
            return out;
        std::string cur;
        for (const char* q = r.fmt; ; ++q)
        {
            if (*q == '|' || *q == 0)
            {
                out.push_back(cur);
                cur.clear();
                if (*q == 0)
                    break;
            }
            else
                cur += *q;
        }
        return out;
    }
    bool ChoiceUsesPicker(const Row& r)
    {
        return r.kind == RowKind::Choice && r.f && r.step > 0.f && ChoiceOptions(r).size() > 4;
    }
    bool PickerIsChoice()
    {
        return g_PickerRow >= 0 && g_PickerRow < static_cast<int>(Rows().size()) && Rows()[g_PickerRow].kind == RowKind::Choice;
    }
    int PickerCount()
    {
        return PickerIsChoice() ? static_cast<int>(ChoiceOptions(Rows()[g_PickerRow]).size()) : bmvr::kBindActionCount;
    }
    int PickerCountFor(const Row& r) { return static_cast<int>(ChoiceOptions(r).size()); }
    int PickerRows() { return (PickerCount() + kPickerCols - 1) / kPickerCols; }
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
        return idx < PickerCount() ? idx : -1;
    }

    // "Reset this tab" (one at the end of every tab): every row of tab `tab` back to its
    // built-in default (CapturePanelDefaults, taken before config.txt), "Show developer rows"
    // too (user 2026-11-19: a reset left it as the one changed row); a tab with button rows also gets the default buttons for this controller. Engine
    // rows get console commands: god/noclip/notarget (toggles) are switched off before the
    // cvars, so sv_cheats 0 comes last. Returns the number of rows changed; each one is logged.
    // Row i is a developer row (DevSection group or IsDevRow) while
    // "Show developer rows" is off: not counted as changed, not reset.
    bool DevRowHidden(size_t i)
    {
        if (bmvr::g_PanelDevRows)
            return false;
        const auto& rows = Rows();
        if (IsDevRow(rows[i]))
            return true;
        for (size_t j = i + 1; j-- > 0;)
        {
            const RowKind k = rows[j].kind;
            if (k == RowKind::DevSection)
                return true;
            if (k == RowKind::Section || k == RowKind::Tab || k == RowKind::Close)
                return false;
        }
        return false;
    }

    int ResetTabRows(int tab)
    {
        std::vector<Row>& rows = Rows();
        size_t i = 0;
        for (int t = -1; i < rows.size(); ++i)
            if (rows[i].kind == RowKind::Tab && ++t == tab)
                break;
        if (i >= rows.size())
            return 0;
        const char* tabName = rows[i].label;
        std::vector<std::string> toggles, cvars;
        int changed = 0;
        bool binds = false;
        for (++i; i < rows.size() && rows[i].kind != RowKind::Tab && rows[i].kind != RowKind::Close; ++i)
        {
            Row& r = rows[i];
            if (DevRowHidden(i))
                continue;   // hidden developer row: left as it is
            if (r.kind == RowKind::Bind)
                binds = true;
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
            else if (float ammoDef = 0.f; (r.kind == RowKind::Number || r.kind == RowKind::Choice) && bmvr::AmmoHudRowDefault(r.f, ammoDef))
            {
                // Per-weapon ammo HUD rows: back to the held weapon's built-in values.
                if (std::fabs(*r.f - ammoDef) > 1e-4f)
                {
                    *r.f = ammoDef;
                    Game::logMsg("VR settings reset: %s -> %g", r.label, ammoDef);
                    ++changed;
                }
            }
            else if ((r.kind == RowKind::Number || r.kind == RowKind::Choice) && r.f
                && (r.f == &bmvr::g_VmTuneSeatCm || r.f == &bmvr::g_VmTuneSeatDeg))
            {
                // Per-weapon seat rows: back to the held weapon's built-in values.
                float cm = 0.f, deg = 0.f;
                bmvr::HeldWeaponSeatDefaults(cm, deg);
                const float want = r.f == &bmvr::g_VmTuneSeatCm ? cm : deg;
                if (std::fabs(*r.f - want) > 1e-4f)
                {
                    *r.f = want;
                    Game::logMsg("VR settings reset: %s -> %g", r.label, want);
                    ++changed;
                }
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
        if (binds)
        {
            bmvr::ResetBinds(g_PanelFamily);
            g_BindsChanged = true;
        }
        Game::logMsg("VR settings reset: tab %s back to defaults, %d rows changed%s", tabName, changed,
            binds ? " + buttons" : "");
        return changed;
    }

    struct PresetSet
    {
        const char* id;
        int count;                 // values per preset
        int def;                   // index of the built-in default (bmvr_flags.cpp)
        float* targets[5];
        int n;                     // presets in items
        struct { const char* name; float v[5]; } items[4];
    };
    // Items in order (one direction per click); the default is marked on the panel.
    const std::vector<PresetSet>& Presets()
    {
        static const std::vector<PresetSet> sets = {
            // FXAA 3.11 terms: sub-pixel smoothing, edge threshold, dark edge threshold.
            // Sharp keeps texture detail (about as sharp as the stock game); Smooth is
            // FXAA's own sub-pixel default with most edges treated (softer, calmer).
            // Stock = what the game itself sends (all 0: it never passes mat_fxaa_*).
            { "fxaa", 3, 0, { &bmvr::g_FxaaSubpixel, &bmvr::g_FxaaEdgeThreshold, &bmvr::g_FxaaEdgeThresholdMin }, 4,
              { { "Sharp", { 0.25f, 0.166f, 0.0833f } },
                { "Balanced", { 0.5f, 0.125f, 0.0625f } },
                { "Smooth", { 0.75f, 0.063f, 0.0312f } },
                { "Stock", { 0.f, 0.f, 0.f } } } },
            // Hand HUD placement: Up, Forward, Sideways, Tilt, Roll (size stays).
            { "hudspot", 5, 0, { &bmvr::g_HandHudUp, &bmvr::g_HandHudForward, &bmvr::g_HandHudSide, &bmvr::g_HandHudTilt, &bmvr::g_HandHudRoll }, 3,
              { { "On top", { 2.5f, -0.55f, 0.3f, -18.f, 14.f } },
                { "Back of hand", { 1.25f, -0.75f, 0.3f, 6.f, 116.f } },
                { "Palm", { 1.25f, -2.25f, 0.f, 6.f, -74.f } } } },
            // Cubemap reflection strength, walls/floors and props/NPCs.
            { "envmap", 2, 1, { &bmvr::g_EnvmapScaleWorld, &bmvr::g_EnvmapScaleModels, nullptr }, 3,
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
        for (int i = 0; i < p.n; ++i)
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
        // A Dynamic grab zone: which weapon it holds now.
        for (int z = 0; z < bmvr::kGestureWeaponZoneCount; ++z)
            if (r.f == &bmvr::g_GestureRight[z] && std::lround(*r.f) == bmvr::kGestureDynamic)
            {
                // The weapon's own option label ("MP5"): option w + 1 (after Off).
                const int w = bmvr::GestureWeaponAt(z);
                const char* o = r.fmt;
                for (int i = 0; i < w + 1 && o; ++i)
                    o = std::strchr(o, '|') ? std::strchr(o, '|') + 1 : nullptr;
                const char* oe = o ? std::strchr(o, '|') : nullptr;
                name += w >= 0 && o ? " (" + (oe ? std::string(o, oe) : std::string(o)) + ")" : " (empty)";
            }
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
        int closeTop = 0;   // of the tab shown (GetLayout sets it per tab)
        int bottom = 0;
        std::vector<int> tabContent;  // content bottom per tab
        int contentMax = 0;           // tallest tab
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
            hidden[i] = ((inDev || IsDevRow(rows[i])) && !devRows) || k == RowKind::ColumnBreak
                || (k == RowKind::Bind && BindRowHidden(rows[i]))
                || (k == RowKind::BindPicture && !PictureShown(hand))
                || (k != RowKind::BindPicture && hand >= 0 && PictureShown(hand))
                || IsGestureSlotRow(rows[i]);   // the figure's card lines
        }
        // A section heading whose rows are all hidden (developer rows) goes too.
        for (int i = 0; i < n; ++i)
        {
            if (!IsHeading(rows[i].kind) || hidden[i])
                continue;
            bool any = false;
            for (int j = i + 1; j < n; ++j)
            {
                const RowKind k = rows[j].kind;
                if (IsHeading(k) || k == RowKind::Tab || k == RowKind::Close || k == RowKind::ColumnBreak)
                    break;
                if (!hidden[j])
                {
                    any = true;
                    break;
                }
            }
            if (!any)
                hidden[i] = true;
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
                // A gap above every heading but a column's first, so a new
                // section reads as one (user 2026-11-19).
                if (IsHeading(rows[i].kind) && y[c] > kContentTop)
                    y[c] += kSectionGapPx;
                out.tab[i] = t;
                out.col[i] = c;
                out.top[i] = y[c];
                y[c] += RowHeight(rows[i]);
            }
            // The two controller headings start level, whatever sits above
            // them (user: the left controller sat a row lower than the right).
            {
                int head[2] = { -1, -1 };
                for (int i = first; i < last; ++i)
                    if (out.tab[i] == t && rows[i].fmt)
                    {
                        if (std::strcmp(rows[i].fmt, "bind_head_left") == 0)
                            head[out.col[i]] = i;
                        else if (std::strcmp(rows[i].fmt, "bind_head_right") == 0)
                            head[out.col[i]] = i;
                    }
                if (head[0] >= 0 && head[1] >= 0)
                {
                    const int lowCol = out.top[head[0]] < out.top[head[1]] ? 0 : 1;
                    const int delta = std::abs(out.top[head[0]] - out.top[head[1]]);
                    if (delta > 0)
                    {
                        for (int i = head[lowCol]; i < last; ++i)
                            if (out.tab[i] == t && out.col[i] == lowCol)
                                out.top[i] += delta;
                        y[lowCol] += delta;
                    }
                }
            }
            const int tabBottom = (std::max)({ kContentTop, y[0], y[1] });
            out.tabContent.push_back(tabBottom);
            contentBottom = (std::max)(contentBottom, tabBottom);
        }
        out.contentMax = contentBottom;
        out.closeTop = contentBottom + 16;
        out.bottom = out.closeTop + kRowPx;
        for (int i = 0; i < n; ++i)
        {
            if (r_IsTabReset(rows[i]))
            {
                out.top[i] = out.closeTop;
                out.x0[i] = kTabResetX0;
                out.x1[i] = kTabResetX1;
                continue;
            }
            if (rows[i].kind != RowKind::Close)
                continue;
            out.top[i] = out.closeTop;
            out.x0[i] = 0;
            out.x1[i] = kTexW;
        }
        return out;
    }
    // Content bottom every tab shares: the tallest tab without developer rows.
    // With developer rows on, only a tab that grows past it (DEBUG) makes the
    // panel taller while it is shown; the other tabs keep their normal size
    // and distance (user, 2026-11-18: dev rows made every tab small).
    int g_BaseContent = 0;
    const Layout& GetLayout()
    {
        static Layout l;
        static int s_builtFor = -1;
        static int s_tabFor = -1;
        // Rebuilt when developer rows or the detected controller (its buttons) change.
        const int want = (bmvr::g_PanelDevRows ? 1 : 0) | static_cast<int>(g_PanelFamily << 1)
            | static_cast<int>(((g_PanelRaw[0] >> 16) & 0xF) << 8) | static_cast<int>(((g_PanelRaw[1] >> 16) & 0xF) << 12)
            | (PictureShown(0) ? 1 << 16 : 0) | (PictureShown(1) ? 1 << 17 : 0);
        if (s_builtFor != want)
        {
            // Bit 0 only: the other bits are the controller (was want != 0, so
            // developer rows showed once a controller was detected).
            l = BuildLayout((want & 1) != 0);
            g_BaseContent = (want & 1) != 0 ? BuildLayout(false).contentMax : l.contentMax;
            s_builtFor = want;
            s_tabFor = -1;
        }
        if (s_tabFor != g_Tab)
        {
            s_tabFor = g_Tab;
            const int own = g_Tab >= 0 && g_Tab < static_cast<int>(l.tabContent.size()) ? l.tabContent[g_Tab] : l.contentMax;
            l.closeTop = (std::max)(g_BaseContent, own) + 16;
            l.bottom = l.closeTop + kRowPx;
            const auto& rows = Rows();
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                if (rows[i].kind == RowKind::Close || r_IsTabReset(rows[i]))
                    l.top[i] = l.closeTop;
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
    // Rows that only act while another setting allows it (user, 2026-11-18:
    // "Physical weapons (beta)" off with everything under it reading ON looked
    // wrong): drawn dim and not clickable until the setting they depend on is on.
    bool RowDisabled(const Row& r)
    {
        const char* k = r.key ? r.key : r.fmt;
        if (!k || r.kind == RowKind::Bind || r.kind == RowKind::Info)
            return false;
        auto is = [k](std::initializer_list<const char*> keys) {
            for (const char* key : keys)
                if (std::strcmp(k, key) == 0)
                    return true;
            return false;
        };
        using namespace bmvr;
        if (is({ "SnapTurnAngle" }))
            return !g_SnapTurning;
        if (is({ "TurnSpeed" }))
            return g_SnapTurning;
        if (is({ "LadderBodyFollowsArms", "LadderCentreHead", "LadderGunHand", "LadderHandBack", "LadderHandWrap", "LadderStickClimb" }))
            return !g_PhysicalLadders;
        if (is({ "PhysicalCrouchDropCm" }))
            return !g_PhysicalCrouch;
        if (is({ "RoomscaleDeadzoneCm" }))
            return !g_Roomscale;
        if (is({ "DamageFlashStrength" }))
            return !g_DamageFlash;
        if (is({ "HandHudSize", "HandHudUp", "HandHudForward", "HandHudSide", "HandHudTilt", "HandHudRoll" }))
            return !g_HandHud;
        if (is({ "VrRecoil", "VrMagDiscard", "VrClipHandPose", "VrClipHandSnap", "VrClipHandRpg", "VrClipHandRpgFlip" }))
            return !g_PhysicalWeapons;
        if (is({ "VrRecoilScale", "VrRecoilTwoHand" }))
            return !PhysicalWeapon(g_VrRecoil);
        if (is({ "VrCrosshairScale" }))
            return AimMarkerMode() == 0;
        if (is({ "VrLaserWidth" }))
            return !AimLaserBeamOn();
        if (is({ "LastShotHapticStrength" }))
            return g_LastShotHaptic < 0.5f;
        if (is({ "DryFireHapticStrength" }))
            return g_DryFireHaptic < 0.5f;
        if (is({ "ThrowScale", "ThrowStroke" }))
            return !g_VrHandThrow;
        if (is({ "FoveatedInner", "FoveatedMid" }))
            return g_Foveated < 0.5f;
        if (is({ "AlphaToCoverage", "MsaaParticles1x" }))
            return g_MsaaF < 0.5f;
        // FXAA preset row (fmt "fxaa") and its values.
        // Off when FXAA is off in the game's video settings (mat_fxaa_enabled).
        if (is({ "fxaa", "FxaaSubpixel", "FxaaEdgeThreshold", "FxaaEdgeThresholdMin" }))
            return !g_FxaaVideoOn;
        return false;
    }
    // Row under texture x/y, or -1 (headings are not interactive). Tab
    // buttons and Close are rows too.
    int RowAt(int px, int py)
    {
        const auto& rows = Rows();
        const Layout& l = GetLayout();
        const int c = px < kColW ? 0 : 1;
        // Bottom bar, far left: "Reset tab" (only while something on the tab is changed).
        for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i)
            if (r_IsTabReset(rows[i]))
            {
                if (px >= l.x0[i] && px < l.x1[i] && py >= l.top[i] && py < l.top[i] + kRowPx)
                    return TabChangedCount(g_Tab) > 0 ? i : -1;
                break;
            }
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
                if (k == RowKind::GestureFigure)
                    return GestureFigureHit(rows[i].fmt && rows[i].fmt[0] == 'R', px, py - l.top[i]);
                return (IsHeading(k) || k == RowKind::Info || RowDisabled(rows[i])) ? -1 : i;
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
    // Set away from its built-in default (= what config.txt keeps): label (and value) amber.
    // Same colour as the changed buttons on the CONTROLS tab. Engine rows (cvar:/cmd:,
    // e.g. sv_cheats, god) count too, against the same defaults "Reset tab" puts back
    // (user 2026-11-19: sv_cheats on was not marked, yet a reset turned it off).
    bool RowChanged(const Row& r)
    {
        if (r.kind == RowKind::Bind)
            return BindRowAction(r) != BindRowAction(r, true);
        if (r.kind != RowKind::Toggle && r.kind != RowKind::Number && r.kind != RowKind::Choice)
            return false;
        // Key-less rows (bmvr_* fixes, tests, tuning) count too: "Reset tab" puts them back,
        // so they must be marked and counted (user 2026-11-22: render fixes were not).
        if (float ammoDef = 0.f; bmvr::AmmoHudRowDefault(r.f, ammoDef))
            return std::fabs(*r.f - ammoDef) > 1e-4f;
        if (r.f && (r.f == &bmvr::g_VmTuneSeatCm || r.f == &bmvr::g_VmTuneSeatDeg))
        {
            float cm = 0.f, deg = 0.f;
            bmvr::HeldWeaponSeatDefaults(cm, deg);   // per weapon, as ResetTabRows
            return std::fabs(*r.f - (r.f == &bmvr::g_VmTuneSeatCm ? cm : deg)) > 1e-4f;
        }
        return !IsDefault(r);
    }
    // Changed developer rows of tab `tab` that are hidden ("Show developer rows"
    // off): shown as a note beside "Reset tab", which leaves them alone.
    int TabHiddenDevChangedCount(int tab)
    {
        if (bmvr::g_PanelDevRows)
            return 0;
        const auto& rows = Rows();
        int t = -1, n = 0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const Row& r = rows[i];
            if (r.kind == RowKind::Close)
                break;
            if (r.kind == RowKind::Tab)
            {
                if (++t > tab)
                    break;
                continue;
            }
            if (t == tab && DevRowHidden(i) && RowChanged(r))
                ++n;
        }
        return n;
    }
    // Changed rows of tab `tab` (its tab button gets a dot, "Reset this tab" the count).
    int TabChangedCount(int tab)
    {
        const auto& rows = Rows();
        int t = -1, n = 0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const Row& r = rows[i];
            if (r.kind == RowKind::Close)
                break;
            if (r.kind == RowKind::Tab)
            {
                if (++t > tab)
                    break;
                continue;
            }
            // Only rows on screen: hidden developer rows would make the count
            // disagree with the amber rows (user 2026-11-19).
            if (t == tab && !DevRowHidden(i) && RowChanged(r))
                ++n;
        }
        return n;
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
        if (text.empty())
            text = bmvr::UserConfigHeader();   // first save: the zip ships no config.txt
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
        char toastDetail[128]{};
        int toastW = 512; // texture width: wider than 512 for a long one-line message
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
        void setAlpha(uint8_t a, const RECT& r)
        {
            GdiFlush();
            const int x0 = (std::max)(0, static_cast<int>(r.left)), x1 = (std::min)(w, static_cast<int>(r.right));
            for (int y = (std::max)(0, static_cast<int>(r.top)); y < (std::min)(h, static_cast<int>(r.bottom)); ++y)
                for (int x = x0; x < x1; ++x)
                {
                    uint32_t& p = px[static_cast<size_t>(y) * w + x];
                    p = (p & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
                }
        }
    };

    // Hover-only repaints (2026-11-27 handover: a full repaint per hover change
    // cost ~41 ms on the render thread on the DEBUG tab): body-texture rects
    // that are redrawn, nullptr = whole panel.
    using PanelClip = std::vector<RECT>;
    bool RectsOverlap(const RECT& a, const RECT& b)
    {
        return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    }
    bool ClipHits(const PanelClip* clip, int x0, int y0, int x1, int y1)
    {
        if (!clip)
            return true;
        const RECT r{ x0, y0, x1, y1 };
        for (const RECT& c : *clip)
            if (RectsOverlap(c, r))
                return true;
        return false;
    }

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

    // Panel canvases, kept between repaints while the panel is open. Each
    // repaint used to create two fresh DIB sections (~27 MB each for the
    // 3354 px Debug tab); late in a long session the 32-bit game ran out of
    // contiguous address space for them (2026-11-27 headset: clicks applied
    // without feedback, then no panel at all and ~20 ms render work per frame
    // from repainting every frame, until a map load freed memory).
    std::unique_ptr<Canvas> g_BodyCanvas, g_WideCanvas;

    Canvas* PanelCanvas(std::unique_ptr<Canvas>& c, int w, int h)
    {
        if (!c || c->w != w || c->h != h)
        {
            c.reset();   // free the old one before asking for the new size
            c = std::make_unique<Canvas>(w, h);
        }
        return c->ok() ? c.get() : nullptr;
    }

    // Closed panel: give the big buffers back (the GPU texture stays for the next open).
    void ReleasePanelBuffers()
    {
        g_BodyCanvas.reset();
        g_WideCanvas.reset();
        if (g_Gfx.sys)
        {
            g_Gfx.sys->Release();
            g_Gfx.sys = nullptr;
        }
    }

    // Largest free block of the process address space, MB (failure reports).
    unsigned LargestFreeVaMb()
    {
        size_t best = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        for (uintptr_t a = 0x10000; VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) == sizeof(mbi);)
        {
            if (mbi.State == MEM_FREE && mbi.RegionSize > best)
                best = mbi.RegionSize;
            const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (next <= a)
                break;
            a = next;
        }
        return static_cast<unsigned>(best >> 20);
    }

    // Upload into the panel textures. New-size textures are made before the
    // old GPU texture goes, so a failed allocation keeps the last panel image
    // on screen. Returns nullptr or the step that failed.
    const char* UploadPanel(IDirect3DDevice9* device, const Canvas& c)
    {
        const bool sameSize = g_Gfx.w == c.w && g_Gfx.h == c.h;
        if (!sameSize && g_Gfx.sys)
        {
            g_Gfx.sys->Release();   // only the GPU texture is needed to draw
            g_Gfx.sys = nullptr;
        }
        IDirect3DTexture9* gpu = sameSize ? g_Gfx.gpu : nullptr;
        bool newGpu = false;
        if (!g_Gfx.sys && FAILED(device->CreateTexture(c.w, c.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_Gfx.sys, nullptr)))
        {
            g_Gfx.sys = nullptr;
            return "system-memory texture";
        }
        if (!gpu)
        {
            if (FAILED(device->CreateTexture(c.w, c.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpu, nullptr)))
                return "GPU texture";
            newGpu = true;
        }
        const char* fail = nullptr;
        D3DLOCKED_RECT lr{};
        if (FAILED(g_Gfx.sys->LockRect(0, &lr, nullptr, 0)))
            fail = "lock";
        else
        {
            for (int y = 0; y < c.h; ++y)
                std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                    c.px + static_cast<size_t>(y) * c.w, static_cast<size_t>(c.w) * 4u);
            g_Gfx.sys->UnlockRect(0);
            if (FAILED(device->UpdateTexture(g_Gfx.sys, gpu)))
                fail = "update";
        }
        if (fail)
        {
            if (newGpu)
                gpu->Release();
            return fail;
        }
        if (newGpu)
        {
            if (g_Gfx.gpu)
                g_Gfx.gpu->Release();
            g_Gfx.gpu = gpu;
            g_Gfx.w = c.w;
            g_Gfx.h = c.h;
        }
        return nullptr;
    }

    // Hover-only repaint upload: only `rects` (body coordinates, shifted by
    // xOff into c) go to the GPU. The locked rect is the system texture's
    // dirty box, and UpdateTexture copies only that, so one update per rect
    // (two far-apart rows would otherwise send everything between them).
    const char* UploadPanelRects(IDirect3DDevice9* device, const Canvas& c, const PanelClip& rects, int xOff)
    {
        if (!g_Gfx.sys || !g_Gfx.gpu || g_Gfx.w != c.w || g_Gfx.h != c.h)
            return "partial upload without textures";
        for (const RECT& r0 : rects)
        {
            RECT r{ (std::max)(0L, r0.left + xOff), (std::max)(0L, r0.top),
                (std::min)(static_cast<LONG>(c.w), r0.right + xOff), (std::min)(static_cast<LONG>(c.h), r0.bottom) };
            if (r.left >= r.right || r.top >= r.bottom)
                continue;
            D3DLOCKED_RECT lr{};
            if (FAILED(g_Gfx.sys->LockRect(0, &lr, &r, 0)))
                return "lock";
            const size_t bytes = static_cast<size_t>(r.right - r.left) * 4u;
            for (int y = r.top; y < r.bottom; ++y)
                std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y - r.top) * lr.Pitch,
                    c.px + static_cast<size_t>(y) * c.w + r.left, bytes);
            g_Gfx.sys->UnlockRect(0);
            if (FAILED(device->UpdateTexture(g_Gfx.sys, g_Gfx.gpu)))
                return "update";
        }
        return nullptr;
    }

    // Hover-only repaint with wings: the body rects into the wide canvas
    // (the wings themselves do not change; TabHasFigures tabs repaint fully).
    void CopyBodyRects(const Canvas& body, Canvas& out, const PanelClip& rects)
    {
        const int wing = (out.w - body.w) / 2;
        GdiFlush();
        for (const RECT& r0 : rects)
        {
            const int x0 = (std::max)(0, static_cast<int>(r0.left)), x1 = (std::min)(body.w, static_cast<int>(r0.right));
            if (x0 >= x1)
                continue;
            for (int y = (std::max)(0, static_cast<int>(r0.top)); y < (std::min)((std::min)(body.h, out.h), static_cast<int>(r0.bottom)); ++y)
                std::memcpy(out.px + static_cast<size_t>(y) * out.w + wing + x0, body.px + static_cast<size_t>(y) * body.w + x0,
                    static_cast<size_t>(x1 - x0) * 4u);
        }
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
    constexpr COLORREF kDisabledBtn = RGB(42, 46, 52);   // buttons of a row whose setting it depends on is off
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

    // One card: amber head, then a line per CardLine (fixed lines dim, the
    // others buttons that open the row's picker). Controller and gesture cards.
    void PaintCard(Canvas& c, int cx0, int cx1, int y0, int y1, const std::string& head,
        const std::vector<CardLine>& lines, int hoverRow)
    {
        c.fill(cx0, y0, cx1, y1, RGB(26, 31, 38));
        c.fill(cx0, y0, cx1, y0 + 4, RGB(90, 70, 20));
        c.text(head.c_str(), cx0 + 16, y0 + 4, cx1 - 16, y0 + kCardHeadPx, kAmber, 28, true, DT_LEFT);
        for (size_t k = 0; k < lines.size(); ++k)
        {
            const CardLine& ln = lines[k];
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

    // One controller's cards (RowKind::BindPicture); picture and lines: ComposeWings.
    void PaintPicture(Canvas& c, int hand, int top, int hoverRow)
    {
        const int colX = hand * kColW;
        const int cx0 = colX + CardX0(hand), cx1 = cx0 + kCardW;
        for (const Card& card : PictureCards(hand))
            PaintCard(c, cx0, cx1, top + card.y0, top + card.y1, card.head, card.lines, hoverRow);
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

    // ---- GESTURES tab: the body seen from behind (your left = panel left) ----
    // Texture px; y from the figure row's top. The body stands on the column
    // divider, left-side zone cards in the left column, right-side ones in the
    // right column, a leader line from each card to its zone. Card lines are
    // the slot rows (picker, save, reset, changed marks as any Choice row).
    constexpr int kFigCx = kColW;
    constexpr int kGCardW = 540;
    constexpr int kGCardL0 = 40, kGCardR0 = kTexW - 40 - kGCardW;
    // The figure sits kFigDy below the row top (user 2026-11-24: lower, so the
    // hip leader lines run straight; only the forehead's slants).
    constexpr int kFigDy = 94;
    struct ZoneBox { int x0, y0, x1, y1; };
    ZoneBox GestureZoneBox(int z)
    {
        ZoneBox b;
        switch (z)
        {
        // Fitted to VR/panel/gesture_body.png (345x860, drawn from y kFigDy):
        // behind the shoulder pads beside the head, the hands / belt sides.
        case bmvr::kGestureShoulderLeft:  b = { kFigCx - 185, 85, kFigCx - 50, 225 }; break;
        case bmvr::kGestureShoulderRight: b = { kFigCx + 50, 85, kFigCx + 185, 225 }; break;
        case bmvr::kGestureHipLeft:       b = { kFigCx - 205, 375, kFigCx - 85, 535 }; break;
        case bmvr::kGestureHipRight:      b = { kFigCx + 85, 375, kFigCx + 205, 535 }; break;
        default:                          b = { kFigCx - 58, 2, kFigCx + 58, 82 }; break;   // forehead (seen through the head)
        }
        b.y0 += kFigDy;
        b.y1 += kFigDy;
        return b;
    }
    bool IsGestureSlotRow(const Row& r)
    {
        if (!r.f)
            return false;
        for (int z = 0; z < bmvr::kGestureZoneCount; ++z)
            if (r.f == &bmvr::g_GestureLeft[z])
                return true;
        for (int z = 0; z < bmvr::kGestureWeaponZoneCount; ++z)
            if (r.f == &bmvr::g_GestureRight[z])
                return true;
        return false;
    }
    struct GestureCard
    {
        int zone = 0;
        std::string head;
        std::vector<CardLine> lines;
        int y0 = 0, y1 = 0;
    };
    std::vector<GestureCard> GestureCards(bool rightSide)
    {
        const auto& rows = Rows();
        auto line = [&](const char* caption, const float* f) {
            CardLine ln;
            ln.caption = caption;
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                if (rows[i].f == f && rows[i].kind == RowKind::Choice)
                {
                    ln.row = i;
                    ln.text = ChoiceText(rows[i]);
                    const size_t d = ln.text.rfind(" (default)");
                    if (d != std::string::npos && d + 10 == ln.text.size())
                        ln.text.erase(d);
                    ln.changed = RowChanged(rows[i]);
                }
            return ln;
        };
        std::vector<GestureCard> cards;
        auto add = [&](int z, int y0, bool withRight) {
            GestureCard c;
            c.zone = z;
            std::string head = bmvr::GestureZoneName(z);
            head[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(head[0])));
            c.head = head;
            c.lines.push_back(line("Left hand", &bmvr::g_GestureLeft[z]));
            if (withRight)
                c.lines.push_back(line("Right hand", &bmvr::g_GestureRight[z]));
            c.y0 = y0;
            c.y1 = y0 + kCardHeadPx + static_cast<int>(c.lines.size()) * kCardLinePx + 6;
            cards.push_back(c);
        };
        // Forehead is left hand only: its card sits on the left (user 2026-11-23).
        if (!rightSide)
        {
            add(bmvr::kGestureHead, 10, false);
            add(bmvr::kGestureShoulderLeft, 230, true);
            add(bmvr::kGestureHipLeft, 530, true);
        }
        else
        {
            add(bmvr::kGestureShoulderRight, 230, true);
            add(bmvr::kGestureHipRight, 530, true);
        }
        return cards;
    }
    int GestureFigureHit(bool rightSide, int px, int ly)
    {
        const int x0 = rightSide ? kGCardR0 : kGCardL0;
        if (px < x0 || px >= x0 + kGCardW)
            return -1;
        for (const GestureCard& c : GestureCards(rightSide))
            for (size_t k = 0; k < c.lines.size(); ++k)
            {
                const int y = c.y0 + kCardHeadPx + static_cast<int>(k) * kCardLinePx;
                if (ly >= y && ly < y + kCardLinePx && !c.lines[k].fixed)
                    return c.lines[k].row;
            }
        return -1;
    }
    bool GestureZoneOn(int z)
    {
        const int a = static_cast<int>(std::lround(bmvr::g_GestureLeft[z]));
        bool on = a >= 0 && (a != bmvr::kGestureAmmo || bmvr::PhysicalWeapon(bmvr::g_VrMagReload));
        if (z < bmvr::kGestureWeaponZoneCount)
            on = on || std::lround(bmvr::g_GestureRight[z]) >= 0;
        return on;
    }

    // The HEV suit from behind (VR/panel/gesture_body.png, transparent
    // background; user 2026-11-23), loaded once. Missing: a grey silhouette.
    struct BodyPicture { bool tried = false; int w = 0, h = 0; std::vector<uint32_t> px; };
    const BodyPicture& GestureBodyPicture()
    {
        static BodyPicture pic;
        if (!pic.tried)
        {
            pic.tried = true;
            // A shipped asset: the game's VR folder, also in test runs (VrFilePath = the test dir).
            char exe[MAX_PATH]{};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string path(exe);
            const size_t slash = path.find_last_of("\\/");
            path = (slash == std::string::npos ? std::string(".") : path.substr(0, slash)) + "\\VR\\panel\\gesture_body.png";
            if (bmvr::ctlmodel::DecodePngFile(path, pic.w, pic.h, pic.px))
                Game::logMsg("VR panel: gesture body picture %dx%d", pic.w, pic.h);
            else
            {
                pic.w = pic.h = 0;
                Game::logMsg("VR panel: %s missing or unreadable, plain figure instead (please report)", path.c_str());
            }
        }
        return pic;
    }
    // Straight-alpha blend of a 0xAARRGGBB picture onto the canvas.
    void BlendPicture(Canvas& c, const BodyPicture& pic, int x0, int y0)
    {
        GdiFlush();
        for (int y = 0; y < pic.h; ++y)
        {
            const int dy = y0 + y;
            if (dy < 0 || dy >= c.h)
                continue;
            for (int x = 0; x < pic.w; ++x)
            {
                const int dx = x0 + x;
                const uint32_t sp = pic.px[static_cast<size_t>(y) * pic.w + x];
                const uint32_t a = sp >> 24;
                if (!a || dx < 0 || dx >= c.w)
                    continue;
                uint32_t& dp = c.px[static_cast<size_t>(dy) * c.w + dx];
                uint32_t out = 0;
                for (int sh = 0; sh <= 16; sh += 8)
                {
                    const uint32_t s = (sp >> sh) & 0xFF, d = (dp >> sh) & 0xFF;
                    out |= ((s * a + d * (255 - a)) / 255) << sh;
                }
                dp = out;
            }
        }
    }
    // Tints a rectangle toward a colour (share 0..255): zone overlays that
    // keep the suit visible.
    void TintRect(Canvas& c, int x0, int y0, int x1, int y1, COLORREF col, uint32_t share)
    {
        GdiFlush();
        const uint32_t cr = GetRValue(col), cg = GetGValue(col), cb = GetBValue(col);
        for (int y = (std::max)(0, y0); y < (std::min)(c.h, y1); ++y)
            for (int x = (std::max)(0, x0); x < (std::min)(c.w, x1); ++x)
            {
                uint32_t& p = c.px[static_cast<size_t>(y) * c.w + x];
                const uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                p = (((r * (255 - share) + cr * share) / 255) << 16) | (((g * (255 - share) + cg * share) / 255) << 8)
                    | ((b * (255 - share) + cb * share) / 255);
            }
    }

    void PaintGestureBody(Canvas& c, int top)
    {
        const int cx = kFigCx;
        // Clear the divider behind the figure.
        c.fill(kGCardL0 + kGCardW + 6, top, kGCardR0 - 6, top + kGestureFigureH, kBg);
        const BodyPicture& pic = GestureBodyPicture();
        if (pic.w > 0)
        {
            BlendPicture(c, pic, cx - pic.w / 2, top + kFigDy);
            return;
        }
        top += kFigDy - 10;   // drawn fallback, laid out for the picture at y 10
        const COLORREF body = RGB(66, 74, 86), shade = RGB(50, 57, 67);
        HDC dc = c.dc;
        HBRUSH br = CreateSolidBrush(body);
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        // Arms and legs: thick round-capped strokes.
        LOGBRUSH lb{ BS_SOLID, body, 0 };
        auto limb = [&](int width, std::initializer_list<POINT> pts) {
            HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, width, &lb, 0, nullptr);
            HGDIOBJ prev = SelectObject(dc, pen);
            bool first = true;
            for (const POINT& p : pts)
            {
                if (first)
                    MoveToEx(dc, p.x, top + p.y, nullptr);
                else
                    LineTo(dc, p.x, top + p.y);
                first = false;
            }
            SelectObject(dc, prev);
            DeleteObject(pen);
        };
        limb(86, { { cx - 70, 600 }, { cx - 84, 770 } });
        limb(86, { { cx + 70, 600 }, { cx + 84, 770 } });
        limb(60, { { cx - 178, 262 }, { cx - 214, 430 }, { cx - 222, 590 } });
        limb(60, { { cx + 178, 262 }, { cx + 214, 430 }, { cx + 222, 590 } });
        // Hands.
        Ellipse(dc, cx - 252, top + 585, cx - 192, top + 655);
        Ellipse(dc, cx + 192, top + 585, cx + 252, top + 655);
        // Torso (back), neck, head.
        const POINT torso[] = { { cx - 190, top + 232 }, { cx + 190, top + 232 }, { cx + 158, top + 330 },
            { cx + 122, top + 515 }, { cx + 152, top + 612 }, { cx - 152, top + 612 }, { cx - 122, top + 515 },
            { cx - 158, top + 330 } };
        Polygon(dc, torso, 8);
        Rectangle(dc, cx - 30, top + 170, cx + 31, top + 250);
        Ellipse(dc, cx - 64, top + 48, cx + 65, top + 192);
        // Back detail: spine and shoulder blades, so it reads as a back.
        HPEN line = CreatePen(PS_SOLID, 4, shade);
        SelectObject(dc, line);
        MoveToEx(dc, cx, top + 252, nullptr);
        LineTo(dc, cx, top + 560);
        Arc(dc, cx - 150, top + 260, cx - 30, top + 400, cx - 40, top + 300, cx - 140, top + 330);
        Arc(dc, cx + 30, top + 260, cx + 150, top + 400, cx + 140, top + 330, cx + 40, top + 300);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(line);
        DeleteObject(br);
    }

    void PaintGestureFigure(Canvas& c, bool rightSide, int top, int hoverRow)
    {
        HDC dc = c.dc;
        if (!rightSide)
        {
            PaintGestureBody(c, top);
            // Zones: outlined while set, tinted amber while a hand is in them now.
            for (int z = 0; z < bmvr::kGestureZoneCount; ++z)
            {
                const ZoneBox b = GestureZoneBox(z);
                const bool on = GestureZoneOn(z);
                const bool hot = (g_GestureHot & (1u << z)) || (g_GestureHot & (1u << (8 + z)));
                if (hot)
                    TintRect(c, b.x0, top + b.y0, b.x1, top + b.y1, kAmber, 130);
                const COLORREF edge = hot ? kAmber : on ? RGB(255, 200, 90) : RGB(140, 146, 154);
                HPEN pen = CreatePen(on || hot ? PS_SOLID : PS_DOT, hot ? 6 : on ? 4 : 1, edge);
                HGDIOBJ op = SelectObject(dc, pen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, b.x0, top + b.y0, b.x1, top + b.y1, 24, 24);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
            // Manual reload with no zone set to Ammo: no way to reload.
            bool ammoZone = false;
            for (int z = 0; z < bmvr::kGestureZoneCount; ++z)
                ammoZone = ammoZone || std::lround(bmvr::g_GestureLeft[z]) == bmvr::kGestureAmmo;
            if (bmvr::PhysicalWeapon(bmvr::g_VrMagReload) && !ammoZone)
                c.text("! Manual reload is on, but no zone gives ammo: set a left hand to Ammo",
                    40, top + kGestureFigureH - 34, kTexW - 40, top + kGestureFigureH, kAlert, 28, true, DT_CENTER);
        }
        const int cx0 = rightSide ? kGCardR0 : kGCardL0, cx1 = cx0 + kGCardW;
        for (const GestureCard& card : GestureCards(rightSide))
        {
            PaintCard(c, cx0, cx1, top + card.y0, top + card.y1, card.head, card.lines, hoverRow);
            bool hot = false;
            for (const CardLine& ln : card.lines)
                hot |= ln.row == hoverRow;
            // The leader line ends on the zone box's side facing the card
            // (user 2026-11-23: no line into the zone, no dot).
            const ZoneBox b = GestureZoneBox(card.zone);
            const int ax = rightSide ? b.x1 : b.x0, ay = top + (b.y0 + b.y1) / 2;
            const int ly = top + card.y0 + kCardHeadPx / 2;
            const int edge = rightSide ? cx0 : cx1;
            const int elbow = rightSide ? cx0 - 24 : cx1 + 24;
            const COLORREF ink = hot ? kAmber : RGB(170, 140, 80);
            HPEN pen = CreatePen(PS_SOLID, hot ? 5 : 3, ink);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, edge, ly, nullptr);
            LineTo(dc, elbow, ly);
            LineTo(dc, ax, ay);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
    }
    // The GESTURES tab is the one shown.
    bool GestureTabShown()
    {
        const auto& rows = Rows();
        const Layout& l = GetLayout();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (rows[i].kind == RowKind::GestureFigure)
                return l.tab[i] == g_Tab;
        return false;
    }

    // Body-texture rect a row paints into (Paint below draws nothing outside it).
    RECT RowPaintRect(int i)
    {
        const Row& r = Rows()[i];
        const Layout& l = GetLayout();
        if (r.kind == RowKind::Tab)   // with the missing-hooks frame
            return RECT{ l.x0[i] - 6, l.top[i] - 2, l.x1[i] + 6, l.top[i] + kTabPx + 6 };
        if (r_IsTabReset(r))          // with the "+N dev rows changed" note
            return RECT{ l.x0[i], l.top[i], l.x1[i] + 340, l.top[i] + kRowPx };
        if (r.kind == RowKind::Close)
            return RECT{ 0, l.top[i], kTexW, l.top[i] + kRowPx };
        const int x = RowX(i), y = RowTop(i);
        return RECT{ x, y, x + kColW, y + RowHeight(r) };
    }

    // Where hover state `hover` (row, 1000 + picker cell, -1) is drawn, for a
    // hover-only repaint. False: not a plain rect (controller-picture card
    // lines and leader lines, gesture zones): repaint everything.
    bool HoverPaintRect(int hover, std::vector<RECT>& out)
    {
        if (hover < 0)
            return true;
        if (hover >= 1000)
        {
            if (g_PickerRow < 0)
                return false;
            const int a = hover - 1000;
            if (a == kPickerCancel)
            {
                const int cy = PickerCancelY();
                out.push_back(RECT{ kTexW / 2 - 200, cy, kTexW / 2 + 200, cy + kPickerCellH });
                return true;
            }
            const int cx0 = PickerX0() + (a % kPickerCols) * (kPickerCellW + kPickerGap);
            const int cy0 = PickerGridY() + (a / kPickerCols) * (kPickerCellH + kPickerGap);
            out.push_back(RECT{ cx0 - 4, cy0 - 4, cx0 + kPickerCellW + 4, cy0 + kPickerCellH + 4 });
            return true;
        }
        const auto& rows = Rows();
        if (hover >= static_cast<int>(rows.size()) || g_PickerRow >= 0)
            return false;
        const Row& r = rows[hover];
        const bool always = r.kind == RowKind::Tab || r.kind == RowKind::Close || r_IsTabReset(r);
        if (!always && (GetLayout().tab[hover] != g_Tab || r.kind == RowKind::BindPicture || r.kind == RowKind::GestureFigure))
            return false;
        out.push_back(RowPaintRect(hover));
        return true;
    }

    // Tab shows controller pictures (ComposeWings draws over the body) or the gesture figure.
    bool TabHasFigures()
    {
        const auto& rows = Rows();
        const Layout& l = GetLayout();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if ((rows[i].kind == RowKind::BindPicture || rows[i].kind == RowKind::GestureFigure) && l.tab[i] == g_Tab)
                return true;
        return false;
    }

    void Paint(Canvas& c, int hoverRow, int hoverZone, const PanelClip* clip = nullptr)
    {
        HRGN clipRgn = nullptr;
        if (clip)
        {
            clipRgn = CreateRectRgn(0, 0, 0, 0);
            for (const RECT& r : *clip)
            {
                HRGN one = CreateRectRgnIndirect(&r);
                CombineRgn(clipRgn, clipRgn, one, RGN_OR);
                DeleteObject(one);
            }
            SelectClipRgn(c.dc, clipRgn);
        }
        c.fill(0, 0, c.w, c.h, kBg);
        c.fill(0, 0, c.w, 6, kAmber);
        c.text("VR SETTINGS", 40, 10, c.w - 40, kHeaderPx - 10, kAmber, 46, true, DT_LEFT);
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
                if (clip)
                {
                    const RECT pr = RowPaintRect(i);
                    if (!ClipHits(clip, pr.left, pr.top, pr.right, pr.bottom))
                        continue;
                }
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
                // Six tabs: long names ("HANDS & WEAPONS") get a smaller font.
                const char* tabText = alert ? "!   DEBUG   !" : r.label;
                int tabPx = 32;
                while (tabPx > 22 && c.textWidth(tabText, tabPx, true) > l.x1[i] - l.x0[i] - 36)
                    tabPx -= 2;
                c.text(tabText, l.x0[i], y0 + 4, l.x1[i], y1, active ? RGB(0, 0, 0) : kText, tabPx, true, DT_CENTER);
                // Something on this tab differs from the defaults: a dot in the corner.
                if (TabChangedCount(tabIndex - 1) > 0)
                    c.fill(l.x1[i] - 22, y0 + 12, l.x1[i] - 10, y0 + 24, active ? RGB(0, 0, 0) : kAmber);
                continue;
            }
            if (clip && (r_IsTabReset(r) || RowVisible(i)))
            {
                const RECT pr = RowPaintRect(i);
                if (!ClipHits(clip, pr.left, pr.top, pr.right, pr.bottom))
                    continue;
            }
            if (r_IsTabReset(r))
            {
                // Danger red; dark grey + dim while the tab is all default (not clickable then).
                const int n = TabChangedCount(g_Tab);
                const bool armed = n > 0 && g_TabResetArmed == g_Tab;
                char label[64]{};
                if (armed)
                    _snprintf(label, sizeof(label) - 1, "Click again: reset %d", n);
                else if (n > 0)
                    _snprintf(label, sizeof(label) - 1, "Reset tab (%d changed)", n);
                else
                    _snprintf(label, sizeof(label) - 1, "Reset tab (all default)");
                const COLORREF fill = n == 0 ? kDisabledBtn : armed ? kAlert : hover ? RGB(170, 50, 40) : RGB(115, 35, 30);
                const int y0 = l.top[i] + 8, y1 = l.top[i] + kRowPx - 8;
                c.fill(l.x0[i], y0, l.x1[i], y1, fill);
                c.text(label, l.x0[i], l.top[i], l.x1[i], l.top[i] + kRowPx, n == 0 ? kDim : RGB(255, 255, 255), 30, true, DT_CENTER);
                // Hidden developer rows that differ from the default: a quiet
                // note after the button (they are not reset from here).
                const int hiddenDev = TabHiddenDevChangedCount(g_Tab);
                if (hiddenDev > 0)
                {
                    char note[64]{};
                    _snprintf(note, sizeof(note) - 1, "+%d dev row%s changed", hiddenDev, hiddenDev == 1 ? "" : "s");
                    c.text(note, l.x1[i] + 20, l.top[i], l.x1[i] + 340, l.top[i] + kRowPx, kDim, 24, false, DT_LEFT);
                }
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
            if (r.kind == RowKind::GestureFigure)
            {
                PaintGestureFigure(c, r.fmt && r.fmt[0] == 'R', y0, hoverRow);
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
                    const bmvr::BindAction a = BindRowAction(r);
                    name = bmvr::BindActionLabel(a);
                    if (a != BindRowAction(r, true))
                        ink = kAmber;
                    c.text(BindRowLabel(r).c_str(), x + 40, y0, x + kMinusX0 - 10, y1, ink, 34, false, DT_LEFT);
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
            // Depends on a setting that is off: everything in kDim, dark buttons.
            const bool dis = RowDisabled(r);
            const bool changed = RowChanged(r);
            const COLORREF ink = dis ? kDim : changed ? kAmber : kText;
            const COLORREF btn = dis ? kDisabledBtn : kBtn;
            c.text(RowLabelText(r).c_str(), x + 40, y0, x + kMinusX0 - 10, y1, ink, 34, false, DT_LEFT);
            if (r.kind == RowKind::Choice || r.kind == RowKind::Preset)
            {
                // One wide button (the -/+ span); a click picks the next value.
                c.fill(x + kMinusX0, y0 + 10, x + kPlusX1, y1 - 10, hover ? kBtnHover : btn);
                const std::string name = r.kind == RowKind::Preset ? PresetText(r) : ChoiceText(r);
                // Long names ("Headset +10 (default)") get a smaller font instead of spilling over.
                int fontPx = 30;
                while (fontPx > 20 && c.textWidth(name.c_str(), fontPx, true) > (kPlusX1 - 36 - kMinusX0) - 16)
                    fontPx -= 2;
                c.text(name.c_str(), x + kMinusX0, y0, x + kPlusX1 - 36, y1, hover ? RGB(0, 0, 0) : ink,
                    fontPx, true, DT_CENTER);
                c.text(">", x + kPlusX1 - 40, y0, x + kPlusX1 - 8, y1 - 2, hover ? RGB(0, 0, 0) : kDim, 30, true, DT_CENTER);
            }
            else if (r.kind == RowKind::Toggle)
            {
                const bool on = r.b && *r.b;
                c.fill(x + kPillX0, y0 + 10, x + kPillX1, y1 - 10, dis ? kDisabledBtn : on ? kOn : kOff);
                c.text(on ? "ON" : "OFF", x + kPillX0, y0, x + kPillX1, y1, dis ? kDim : RGB(255, 255, 255), 30, true, DT_CENTER);
            }
            else
            {
                char val[32]{};
                _snprintf(val, sizeof(val) - 1, r.fmt, r.f ? *r.f : 0.f);
                c.fill(x + kMinusX0, y0 + 10, x + kMinusX1 - 8, y1 - 10, (hover && hoverZone == 0) ? kBtnHover : btn);
                c.fill(x + kPlusX0 + 8, y0 + 10, x + kPlusX1, y1 - 10, (hover && hoverZone == 2) ? kBtnHover : btn);
                c.text("-", x + kMinusX0, y0, x + kMinusX1 - 8, y1 - 4, ink, 44, true, DT_CENTER);
                c.text("+", x + kPlusX0 + 8, y0, x + kPlusX1, y1 - 2, ink, 40, true, DT_CENTER);
                c.text(val, x + kValueX0, y0, x + kValueX1, y1, ink, 34, true, DT_CENTER);
            }
        }
        // Action picker over the content area.
        if (g_PickerRow >= 0 && g_PickerRow < static_cast<int>(rows.size()))
        {
            const Row& pr = rows[g_PickerRow];
            const bool choice = pr.kind == RowKind::Choice;
            const std::vector<std::string> opts = choice ? ChoiceOptions(pr) : std::vector<std::string>{};
            int curIdx = -1, defIdx = -1;
            if (choice)
            {
                curIdx = static_cast<int>(std::lround((*pr.f - pr.lo) / pr.step));
                defIdx = static_cast<int>(std::lround((pr.defF - pr.lo) / pr.step));
            }
            else
            {
                curIdx = static_cast<int>(BindRowAction(pr));
                defIdx = static_cast<int>(BindRowAction(pr, true));
            }
            c.fill(24, kContentTop - 8, c.w - 24, l.closeTop - 8, RGB(10, 12, 15));
            c.fill(24, kContentTop - 8, c.w - 24, kContentTop - 4, kAmber);
            const std::string title = choice ? std::string(pr.label) + ":  pick one"
                : std::string(pr.fmt[0] == 'L' ? "Left " : "Right ") + BindRowLabel(pr) + ":  pick an action";
            c.text(title.c_str(), PickerX0(), PickerTitleY(), c.w - PickerX0(), PickerTitleY() + 64, kAmber, 40, true, DT_LEFT);
            const int count = choice ? static_cast<int>(opts.size()) : bmvr::kBindActionCount;
            for (int a = 0; a < count; ++a)
            {
                const int col = a % kPickerCols, row = a / kPickerCols;
                const int cx0 = PickerX0() + col * (kPickerCellW + kPickerGap);
                const int cy0 = PickerGridY() + row * (kPickerCellH + kPickerGap);
                const bool hv = hoverRow == 1000 + a;
                const bool isCur = curIdx == a;
                if (isCur)
                    c.fill(cx0 - 4, cy0 - 4, cx0 + kPickerCellW + 4, cy0 + kPickerCellH + 4, kAmber);
                c.fill(cx0, cy0, cx0 + kPickerCellW, cy0 + kPickerCellH, hv ? kBtnHover : kBtn);
                std::string label = choice ? opts[a] : std::string(bmvr::BindActionLabel(static_cast<bmvr::BindAction>(a)));
                if (defIdx == a)
                    label += " (default)";
                c.text(label.c_str(), cx0, cy0, cx0 + kPickerCellW, cy0 + kPickerCellH, hv ? RGB(0, 0, 0) : kText, 32, true, DT_CENTER);
            }
            const bool hvCancel = hoverRow == 1000 + kPickerCancel;
            const int cy = PickerCancelY();
            c.fill(c.w / 2 - 200, cy, c.w / 2 + 200, cy + kPickerCellH, hvCancel ? kBtnHover : kOff);
            c.text("Cancel", c.w / 2 - 200, cy, c.w / 2 + 200, cy + kPickerCellH, hvCancel ? RGB(0, 0, 0) : kText, 32, true, DT_CENTER);
        }
        if (!clip)
        {
            c.setAlpha(238);
            return;
        }
        for (const RECT& r : *clip)
            c.setAlpha(238, r);
        SelectClipRgn(c.dc, nullptr);
        DeleteObject(clipRgn);
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

bool bmvr::WriteVrConfigValues(const std::vector<std::pair<std::string, std::string>>& kv,
    const std::vector<std::string>& drop)
{
    return WriteConfigValues(kv, drop);
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

namespace
{
    // Debug row "God rays samples (test)": np_gr_quality_sample_count is an
    // archived cvar the menus set to 3 (64 taps). Override it only while a
    // level runs, keep the game's value, and put it back on level shutdown
    // (before quit writes config.cfg) or when the row goes back to Game.
    int g_GrSamplesSaved = -1;   // the game's value while overridden

    void TickGodRaySamples(Game* game, bool inLevel)
    {
        const int row = static_cast<int>(bmvr::g_GodRaySamples + 0.5f);
        const int want = row == 1 ? 2 : row == 2 ? 1 : -1;   // 2 = 32 taps, 1 = 16
        int cur = -1;
        if (!game->GetConVarInt("np_gr_quality_sample_count", cur))
            return;
        if (want >= 0 && inLevel)
        {
            if (g_GrSamplesSaved < 0)
                g_GrSamplesSaved = cur;
            if (cur != want && game->SetConVarInt("np_gr_quality_sample_count", want))
                Game::logMsg("God rays samples: np_gr_quality_sample_count %d -> %d (test row, game value %d kept)",
                    cur, want, g_GrSamplesSaved);
        }
        else if (g_GrSamplesSaved >= 0)
        {
            if (cur != g_GrSamplesSaved)
                game->SetConVarInt("np_gr_quality_sample_count", g_GrSamplesSaved);
            Game::logMsg("God rays samples: back to the game's %d", g_GrSamplesSaved);
            g_GrSamplesSaved = -1;
        }
    }
}

void VR::RestoreGodRaySamples()
{
    if (m_Game)
        TickGodRaySamples(m_Game, false);
}

// Game thread (FlushPendingGameUi): runs the panel's engine console commands
// and, while the panel is open, reads the engine cvars back into the rows so
// a refused command (cheat-protected) shows the real value.
void VR::FlushPanelEngineCommands()
{
    if (!m_Game)
        return;
    TickGodRaySamples(m_Game, m_GameplayEligible);
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
    {
        // Video-settings FXAA: the FXAA rows are greyed while it is off.
        float fx = 1.f;
        if (m_Game->GetConVarFloat("mat_fxaa_enabled", fx) && g_FxaaVideoOn != (fx != 0.f))
        {
            g_FxaaVideoOn = fx != 0.f;
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

IDirect3DTexture9* VR::PauseVrButtonTexture(IDirect3DDevice9* device, bool hover)
{
    // Own storage, not g_Gfx: this runs on the material thread (pause HUD
    // stamp), the panel and toasts on the render thread. Render-target
    // textures so StretchRect may read them.
    static IDirect3DDevice9* s_device = nullptr;
    static IDirect3DTexture9* s_tex[2] = {};
    if (!device)
        return nullptr;
    if (s_device != device)
    {
        for (IDirect3DTexture9*& t : s_tex)
            if (t) { t->Release(); t = nullptr; }
        s_device = device;
    }
    IDirect3DTexture9*& tex = s_tex[hover ? 1 : 0];
    if (tex)
        return tex;
    Canvas c(512, 128);
    if (!c.ok())
        return nullptr;
    const int b = 6;
    c.fill(0, 0, c.w, c.h, kAmber);
    c.fill(b, b, c.w - b, c.h - b, hover ? kAmber : kBg);
    c.text("VR SETTINGS", 0, 0, c.w, c.h, hover ? kBg : kAmber, 56, true, DT_CENTER);
    c.setAlpha(240);
    IDirect3DTexture9* sys = nullptr;
    if (FAILED(device->CreateTexture(c.w, c.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr))
        || FAILED(device->CreateTexture(c.w, c.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr)))
    {
        if (sys) sys->Release();
        if (tex) { tex->Release(); tex = nullptr; }
        return nullptr;
    }
    D3DLOCKED_RECT lr{};
    bool ok = SUCCEEDED(sys->LockRect(0, &lr, nullptr, 0));
    if (ok)
    {
        for (int y = 0; y < c.h; ++y)
            std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                c.px + static_cast<size_t>(y) * c.w, static_cast<size_t>(c.w) * 4u);
        sys->UnlockRect(0);
        ok = SUCCEEDED(device->UpdateTexture(sys, tex));
    }
    sys->Release();
    if (!ok)
    {
        tex->Release();
        tex = nullptr;
    }
    static int s_log;
    if (s_log++ < 2)
        Game::logMsg("Pause VR button texture (%s): %s", hover ? "hover" : "normal", ok ? "ok" : "FAILED");
    return tex;
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

void VR::ResetDevCheatRows()
{
    // User 2026-11-27: after a load or map change the rows still said on while
    // god / notarget were off. The flags are server-only (not in the client's
    // 11 networked player-flag bits), so the rows follow the load instead.
    if (!g_DevGod && !g_DevNotarget)
        return;
    Game::logMsg("VR settings: map load, God mode / No target rows reset to off (were %d/%d)",
        g_DevGod ? 1 : 0, g_DevNotarget ? 1 : 0);
    g_DevGod = false;
    g_DevNotarget = false;
    m_SettingsDirty = true;
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

void VR::WelcomeStep(bool paused)
{
    if (bmvr::g_VrWelcomeSeen && !m_WelcomeActive)
        return;
    const DWORD now = GetTickCount();
    if (m_WelcomeActive)
    {
        if (m_SettingsOpen)
        {
            // Opened from the welcome: done for good. The panel takes over our
            // pause (its close unpauses); with "Panel pauses the game" off it
            // does not want one, so drop ours.
            m_WelcomeActive = false;
            bmvr::g_VrWelcomeSeen = true;
            const bool ok = WriteConfigValues({ { "VrWelcomeSeen", "true" } }, {});
            if (m_WelcomePaused && !m_SettingsPausedGame)
                m_PendingPauseCmd.store(2, std::memory_order_release);
            m_WelcomePaused = false;
            m_ToastUntilMs = 0;
            Game::logMsg("Welcome: VR panel opened, VrWelcomeSeen saved (%s)", ok ? "ok" : "FAILED");
            return;
        }
        if (m_VrPhase != VrPhase::Gameplay || IsMenuUp() || !m_GameplayEligible)
        {
            // Game menu (Esc / pause tap), map change or load: give up for this
            // map; Black Mesa's menu or the load owns the pause now.
            m_WelcomeActive = false;
            m_WelcomePaused = false;
            m_ToastUntilMs = 0;
            Game::logMsg("Welcome: cancelled (phase %s, gameUi=%d), again next map", VrPhaseName(m_VrPhase),
                IsMenuUp() ? 1 : 0);
            return;
        }
        ShowToast("WELCOME TO BLACK MESA VR", 1000, m_WelcomeText);
        return;
    }
    // Headless runs (fake runtime) would stall on the pause and write the live
    // config.txt: only with BMVR_TEST_WELCOME=1 there.
    static const bool s_allowed = !L4D2VR_FakeHelperActive() || GetEnvironmentVariableA("BMVR_TEST_WELCOME", nullptr, 0) > 0;
    if (!s_allowed)
        return;
    if (m_WelcomeTriedThisMap || !m_IsVREnabled || !m_GameplayEligible || m_VrPhase != VrPhase::Gameplay
        || IsMenuUp() || m_SettingsOpen || m_WeaponMenuOpen || paused)
    {
        m_WelcomeGameplaySinceMs = 0;
        return;
    }
    // Right at gameplay start (turn sync runs before gameplay): a new game
    // fires the BLACK MESA logo (env_credits, OnNewGame) at once and it fades
    // in over ~2 s; at 3 s it drew over the panel (user 2026-11-23). Pausing
    // this early freezes it before it shows.
    if (!m_WelcomeGameplaySinceMs)
        m_WelcomeGameplaySinceMs = now ? now : 1;
    if (now - m_WelcomeGameplaySinceMs < 250)
        return;
    m_WelcomeTriedThisMap = true;

    // Name the button that opens the panel on this controller (hold slot first).
    const bool mapped = m_ButtonMapActive;
    const char* label = nullptr;
    int labelHand = 0;
    bool hold = false;
    FindBoundButton(bmvr::BindAction::VrSettings, true, label, labelHand, hold);
    if (!label || !label[0] || (!mapped && !bmvr::g_SettingsTriangleHold))
    {
        Game::logMsg("Welcome: skipped, no button opens the VR panel (mapping=%d)", mapped ? 1 : 0);
        return;
    }
    std::snprintf(m_WelcomeText, sizeof(m_WelcomeText),
        "You can %s %s (%s hand) to open the VR menu.\nPlease open it now to set up your playstyle preferences.",
        hold ? "hold" : "press", label, labelHand == 0 ? "left" : "right");
    m_WelcomeActive = true;
    m_WelcomePaused = true;
    m_PendingPauseCmd.store(1, std::memory_order_release);
    char logText[sizeof(m_WelcomeText)]{};
    strncpy_s(logText, m_WelcomeText, _TRUNCATE);
    if (char* nl = std::strchr(logText, '\n'))
        *nl = '|';
    Game::logMsg("Welcome shown: \"%s\" (map %s, %u ms into gameplay)", logText, m_CurrentMapName.c_str(),
        static_cast<unsigned>(now - m_WelcomeGameplaySinceMs));
    ShowToast("WELCOME TO BLACK MESA VR", 1000, m_WelcomeText);
}

void VR::SettingsPanelFrame(Vector& origin, Vector& fwd, Vector& right, Vector& up) const
{
    Vector body;
    float rotY = 0.f;
    MenuDrawBody(body, rotY);
    const float yawDelta = rotY - m_SettingsLatchYaw;
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
    // "Click again to reset this tab" goes back to normal after 3 s or on another tab.
    if (g_TabResetArmed >= 0 && (g_TabResetArmed != g_Tab || GetTickCount() - g_TabResetArmedAt > 3000))
    {
        g_TabResetArmed = -1;
        m_SettingsDirty = true;
    }
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
        m_SettingsHoverDirty = true;
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
    bool choicePicked = false;
    if (g_PickerRow >= 0 && PickerIsChoice())
    {
        if (!clickEdge)
            return;
        const int cell = row >= 1000 ? row - 1000 : -1;
        const int pickedRow = g_PickerRow;
        g_PickerRow = -1;
        m_SettingsHover = -1;
        m_SettingsDirty = true;
        Row& pr = Rows()[pickedRow];
        if (cell < 0 || cell >= PickerCountFor(pr))
            return;
        // Set the value, then the usual Choice-row path below saves it.
        *pr.f = std::clamp(pr.lo + pr.step * static_cast<float>(cell), pr.lo, pr.hi);
        PulseHandHaptic(vr::TrackedControllerRole_RightHand, 700, 0.3f);
        row = pickedRow;
        choicePicked = true;
    }
    else if (g_PickerRow >= 0)
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
    case RowKind::GestureFigure:
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
        const int next = cur < 0 ? p->def : (cur + 1) % p->n;   // custom -> the default
        for (int k = 0; k < p->count; ++k)
            *p->targets[k] = p->items[next].v[k];
        Game::logMsg("VR settings: %s preset = %s", r.fmt, p->items[next].name);
        break;
    }
    case RowKind::Choice:
        if (!r.f || r.step <= 0.f)
            return;
        if (choicePicked)
            break;
        if (ChoiceUsesPicker(r))
        {
            g_PickerRow = row;
            m_SettingsHover = -1;
            m_SettingsDirty = true;
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 500, 0.25f);
            Game::logMsg("VR settings: picker for %s", r.label);
            return;
        }
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
            const int hBefore = TexH();
            g_Tab = t;
            m_SettingsDirty = true;
            // A taller tab (DEBUG with developer rows) moves the panel back to
            // fit, the others come forward again (in the latched frame, like
            // the developer-rows switch).
            if (TexH() != hBefore)
            {
                const float d = PanelDistanceHu();
                m_SettingsLatchDelta = m_SettingsLatchDelta + m_SettingsLatchFwd * (d - g_LatchDistHu);
                Game::logMsg("VR settings panel: tab %d is %d px tall (was %d), distance %.1f -> %.1f",
                    t, TexH(), hBefore, g_LatchDistHu, d);
                g_LatchDistHu = d;
                m_SettingsHover = -1;
                m_SettingsHoverZone = -1;
            }
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 500, 0.25f);
            Game::logMsg("VR settings tab: %s", r.label);
        }
        return;
    }
    case RowKind::Close:
        CloseSettingsPanel("close row");
        return;
    case RowKind::Button:
        // First click arms, a second one within 3 s resets the tab (nothing is lost by a stray click).
        if (r_IsTabReset(r))
        {
            if (g_TabResetArmed != g_Tab)
            {
                g_TabResetArmed = g_Tab;
                g_TabResetArmedAt = GetTickCount();
                m_SettingsDirty = true;
                PulseHandHaptic(vr::TrackedControllerRole_RightHand, 700, 0.3f);
                Game::logMsg("VR settings: Reset this tab armed (tab %d)", g_Tab);
                return;
            }
            g_TabResetArmed = -1;
            const bool devRowsBefore = bmvr::g_PanelDevRows;
            ResetTabRows(g_Tab);
            if (bmvr::g_PanelDevRows != devRowsBefore)
            {
                // Developer rows hidden again: the panel changes height (as
                // for the row's own click below).
                const float d = PanelDistanceHu();
                m_SettingsLatchDelta = m_SettingsLatchDelta + m_SettingsLatchFwd * (d - g_LatchDistHu);
                g_LatchDistHu = d;
                m_SettingsHover = -1;
                m_SettingsHoverZone = -1;
                s_repeatRow = -1;
            }
            if (bmvr::g_VrSettingsPause != m_SettingsPausedGame)
            {
                m_SettingsPausedGame = bmvr::g_VrSettingsPause;
                m_PendingPauseCmd.store(m_SettingsPausedGame ? 1 : 2, std::memory_order_release);
            }
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
        // Closes the panel; 2 s later a debug capture with the global Xen fog
        // drawn, then the eyes again with it hidden (head pose frozen between).
        if (r.fmt && std::strcmp(r.fmt, "fog_dump") == 0)
        {
            bmvr::g_FogDumpAt = GetTickCount() + 2000;
            if (!bmvr::g_FogDumpAt)
                bmvr::g_FogDumpAt = 1;
            Game::logMsg("fog dump: armed, in 2 s");
            CloseSettingsPanel("fog dump");
            return;
        }
        // Lightweight share (user 2026-11-18): closes the panel, 2 s later the
        // two eye images at 1/3 size, no save / audit (VR/screenshot-*).
        if (r.fmt && std::strcmp(r.fmt, "screenshot") == 0)
        {
            bmvr::g_ScreenshotAt = GetTickCount() + 2000;
            if (!bmvr::g_ScreenshotAt)
                bmvr::g_ScreenshotAt = 1;
            Game::logMsg("screenshot: armed, in 2 s");
            CloseSettingsPanel("screenshot");
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
            // Foveation rings stay ordered: a half-rate radius inside the sharp
            // one does nothing (DXVK uses max(Mid, Inner)), so raising sharp
            // pushes half-rate along and half-rate stops at sharp.
            if (r.f == &bmvr::g_FoveatedInner && bmvr::g_FoveatedMid < bmvr::g_FoveatedInner)
                bmvr::g_FoveatedMid = bmvr::g_FoveatedInner;
            else if (r.f == &bmvr::g_FoveatedMid && bmvr::g_FoveatedMid < bmvr::g_FoveatedInner)
                bmvr::g_FoveatedMid = bmvr::g_FoveatedInner;
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
    {
        if (!m_SettingsOpen && (g_BodyCanvas || g_WideCanvas || g_Gfx.sys) && device && g_Gfx.device == device)
            ReleasePanelBuffers();
        return;
    }
    if (g_Gfx.device && g_Gfx.device != device)
    {
        g_BodyCanvas.reset();
        g_WideCanvas.reset();
        ReleaseGfx();
    }
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
    // GESTURES tab: a grab zone lights up while a hand is in it.
    if (GestureTabShown())
    {
        unsigned hot = 0;
        for (int z = 0; z < bmvr::kGestureZoneCount; ++z)
            if (m_LeftControllerTrackingValid && LeftHandInSlotZone(z))
                hot |= 1u << z;
        for (int z = 0; z < bmvr::kGestureWeaponZoneCount; ++z)
            if (m_PhysicalRightTrackingValid && RightHandInSlotZone(z))
                hot |= 1u << (8 + z);
        if (hot != g_GestureHot)
        {
            g_GestureHot = hot;
            m_SettingsDirty = true;
        }
    }
    // Controller models finished loading (or changed): relayout + repaint.
    static int s_paintedModelGen = -1;
    if (bmvr::ctlmodel::Generation() != s_paintedModelGen)
    {
        s_paintedModelGen = bmvr::ctlmodel::Generation();
        m_SettingsDirty = true;
    }
    // Repaint cost (render thread, inside the eye's overlay): per 10 s while
    // the panel is open, count + mean/max ms of paint, wings, upload (user
    // 2026-11-27: panel frames p99 ~40 ms vs ~20 in play).
    // [0] full repaints, [1] hover-only ones.
    struct RepaintStats { int n = 0; double sum[3]{}, max[3]{}; };
    static RepaintStats s_rs[2];
    static DWORD s_rsSince = 0;
    auto qpcMs = []() {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    };
    {
        const DWORD t = GetTickCount();
        if (!s_rsSince)
            s_rsSince = t;
        else if (t - s_rsSince >= 10000)
        {
            for (int k = 0; k < 2; ++k)
            {
                const RepaintStats& rs = s_rs[k];
                if (rs.n > 0)
                    Game::logMsg("VR settings panel %s repaints (10 s): %d, paint %.1f ms mean / %.1f max, wings %.1f / %.1f, upload %.1f / %.1f (tab %d)",
                        k ? "hover" : "full", rs.n, rs.sum[0] / rs.n, rs.max[0], rs.sum[1] / rs.n, rs.max[1], rs.sum[2] / rs.n, rs.max[2], g_Tab);
                s_rs[k] = RepaintStats{};
            }
            s_rsSince = t;
        }
    }
    // After a failed repaint, retry every 500 ms instead of every frame.
    static DWORD s_gfxRetryAt = 0;
    static int s_gfxFailLog = 0;
    static DWORD s_gfxFailSince = 0;
    const DWORD nowMs = GetTickCount();
    const bool backoff = s_gfxRetryAt != 0 && static_cast<int32_t>(s_gfxRetryAt - nowMs) > 0;
    // Hover painted into the kept canvases; s_paintedValid false = they are
    // not a complete image (new canvas, failed repaint), so no partial repaint.
    static int s_paintedHover = -1, s_paintedZone = -1;
    static bool s_paintedValid = false;
    if (!g_BodyCanvas || !g_Gfx.gpu)
        s_paintedValid = false;
    // Flags exchanged before the hover is read (see above).
    const bool fullDirty = !backoff && m_SettingsDirty.exchange(false);
    const bool hoverDirty = !backoff && m_SettingsHoverDirty.exchange(false);
    if (!backoff && (fullDirty || hoverDirty || !g_Gfx.gpu))
    {
        const int hover = m_SettingsHover, hoverZone = m_SettingsHoverZone;
        const char* fail = nullptr;
        const int wing = WingPx();
        // Hover-only: old + new hover rects, when both are plain rects and the
        // canvases still hold the last full image at this size.
        PanelClip clipRects;
        const bool canvasesKept = !fullDirty && g_Gfx.gpu && s_paintedValid && g_BodyCanvas
            && g_BodyCanvas->w == kTexW && g_BodyCanvas->h == TexH()
            && (!wing || (g_WideCanvas && g_WideCanvas->w == kTexW + 2 * wing && g_WideCanvas->h == TexH()));
        // Hover moved and came back before this frame: the image is already right.
        const bool unchanged = canvasesKept && s_paintedHover == hover && s_paintedZone == hoverZone;
        const bool partial = canvasesKept && !unchanged && !TabHasFigures()
            && HoverPaintRect(s_paintedHover, clipRects) && HoverPaintRect(hover, clipRects) && !clipRects.empty();
        if (!wing)
            g_WideCanvas.reset();
        Canvas* body = PanelCanvas(g_BodyCanvas, kTexW, TexH());
        Canvas* wide = (body && wing) ? PanelCanvas(g_WideCanvas, kTexW + 2 * wing, TexH()) : nullptr;
        if (!body)
            fail = "panel canvas";
        else if (wing && !wide)
            fail = "controller-picture canvas";
        else if (unchanged)
            ;
        else
        {
            const double t0 = qpcMs();
            Paint(*body, hover, hoverZone, partial ? &clipRects : nullptr);
            GdiFlush();
            const double t1 = qpcMs();
            if (wing && partial)
                CopyBodyRects(*body, *wide, clipRects);
            else if (wing)
                ComposeWings(*body, *wide, hover);
            const double t2 = qpcMs();
            Canvas& c = wing ? *wide : *body;
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
                // With controller pictures: one file per controller model
                // (panel-tab<N>-<right model>.bmp), for the per-controller shots.
                char name[160]{};
                const bmvr::ctlmodel::Models* pm = wing ? PanelModels() : nullptr;
                const std::string model = pm && pm->hand[1].ok ? "-" + pm->hand[1].name : std::string();
                _snprintf(name, sizeof(name) - 1, "panel-tab%d%s%s.bmp", g_Tab, model.c_str(), g_PickerRow >= 0 ? "-picker" : "");
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
            // Test only: BMVR_TEST_PANEL_VERIFY=1 paints every hover-only
            // repaint again in full into scratch canvases and logs the
            // pixels that differ (expect 0).
            static int s_verifyPanel = -1;
            if (s_verifyPanel < 0)
            {
                char v[8]{};
                s_verifyPanel = GetEnvironmentVariableA("BMVR_TEST_PANEL_VERIFY", v, sizeof(v)) && v[0] == '1' ? 1 : 0;
            }
            if (partial && s_verifyPanel == 1)
            {
                Canvas refBody(body->w, body->h);
                std::unique_ptr<Canvas> refWide = wing ? std::make_unique<Canvas>(wide->w, wide->h) : nullptr;
                if (refBody.ok() && (!wing || refWide->ok()))
                {
                    Paint(refBody, hover, hoverZone);
                    if (wing)
                        ComposeWings(refBody, *refWide, hover);
                    GdiFlush();
                    const Canvas& ref = wing ? *refWide : refBody;
                    int diff = 0, bx0 = ref.w, by0 = ref.h, bx1 = -1, by1 = -1;
                    for (int y = 0; y < ref.h; ++y)
                        for (int x = 0; x < ref.w; ++x)
                            if (ref.px[static_cast<size_t>(y) * ref.w + x] != c.px[static_cast<size_t>(y) * c.w + x])
                            {
                                ++diff;
                                bx0 = (std::min)(bx0, x); by0 = (std::min)(by0, y);
                                bx1 = (std::max)(bx1, x); by1 = (std::max)(by1, y);
                            }
                    Game::logMsg("VR settings panel verify: hover %d -> %d (zone %d), %d rect(s), %d px differ%s",
                        s_paintedHover, hover, hoverZone, static_cast<int>(clipRects.size()), diff, diff ? "" : " (ok)");
                    if (diff)
                        Game::logMsg("VR settings panel verify: differences in %d,%d - %d,%d", bx0, by0, bx1, by1);
                }
            }
            const double t3 = qpcMs();   // the verify paint above is not counted
            fail = partial ? UploadPanelRects(device, c, clipRects, wing) : UploadPanel(device, c);
            if (partial && fail)
            {
                // Fall back to a full upload of the (complete) canvas.
                Game::logMsg("VR settings panel: hover upload failed (%s), full upload", fail);
                fail = UploadPanel(device, c);
            }
            const double ms[3] = { t1 - t0, t2 - t1, qpcMs() - t3 };
            RepaintStats& rs = s_rs[partial ? 1 : 0];
            ++rs.n;
            for (int k = 0; k < 3; ++k)
            {
                rs.sum[k] += ms[k];
                rs.max[k] = (std::max)(rs.max[k], ms[k]);
            }
        }
        if (!fail)
        {
            s_paintedHover = hover;
            s_paintedZone = hoverZone;
            s_paintedValid = true;
        }
        else
            s_paintedValid = false;
        if (fail)
        {
            m_SettingsDirty = true;
            s_gfxRetryAt = nowMs + 500;
            if (!s_gfxFailSince)
                s_gfxFailSince = nowMs;
            if (s_gfxFailLog < 20)
            {
                ++s_gfxFailLog;
                Game::logMsg("VR settings panel: repaint failed (%s), %dx%d tab %d, largest free address block %u MB; %s, retry in 500 ms",
                    fail, kTexW + 2 * wing, TexH(), g_Tab, LargestFreeVaMb(),
                    g_Gfx.gpu ? "last image kept" : "no image to show");
            }
        }
        else if (s_gfxFailSince)
        {
            Game::logMsg("VR settings panel: repaint ok again after %u ms", static_cast<unsigned>(nowMs - s_gfxFailSince));
            s_gfxFailSince = 0;
            s_gfxRetryAt = 0;
        }
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

void VR::ShowUsageHint(UsageHint id, const char* text, int maxPerStart, const char* detail)
{
    static int s_shown[static_cast<int>(UsageHint::Count)]{};
    const int i = static_cast<int>(id);
    if (!bmvr::g_VrUsageHints || i < 0 || i >= static_cast<int>(UsageHint::Count) || s_shown[i] >= maxPerStart)
        return;
    ++s_shown[i];
    ShowToast(text, detail ? 4000 : 1800, detail);
    m_ToastSmall = !detail;
    Game::logMsg("Usage hint %d: \"%s\"%s (%d/%d this game start)", i, text, detail ? " + detail line" : "",
        s_shown[i], maxPerStart);
}

bool VR::FindBoundButton(bmvr::BindAction a, bool holdFirst, const char*& label, int& hand, bool& hold) const
{
    const L4D2VROpenXrInputStateDesc& in = m_OpenXrLastInputState;
    const uint32_t family = in.reserved0;
    const bool mapped = m_ButtonMapActive;
    label = nullptr;
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool wantHold = holdFirst ? pass == 0 : pass == 1;
        for (int h = 0; h < bmvr::kBindHands; ++h)
            for (int i = 0; i < bmvr::kBindInputCount; ++i)
            {
                const auto input = static_cast<bmvr::BindInput>(i);
                const bmvr::BindSlot slot = mapped ? bmvr::EffectiveBind(family, h, input)
                                                   : bmvr::DefaultBind(family, h, input);
                if ((wantHold ? slot.hold : slot.press) != a)
                    continue;
                const char* l = bmvr::BindInputLabel(family, in.systemName, h, input);
                if (!l || !l[0] || l[0] == '?')
                    continue;
                label = l;
                hand = h;
                hold = wantHold;
                return true;
            }
    }
    return false;
}

bool VR::BoundButtonHint(bmvr::BindAction a, const char* fallback, const char* what, char* out, size_t n) const
{
    const char* label = nullptr;
    int hand = 0;
    bool hold = false;
    char button[32]{};
    const bool bound = FindBoundButton(a, false, label, hand, hold);
    if (bound)
    {
        // Name the hand when the other hand's button has the same name (Trigger, Grip, A on Index).
        const L4D2VROpenXrInputStateDesc& in = m_OpenXrLastInputState;
        bool shared = false;
        for (int i = 0; i < bmvr::kBindInputCount && !shared; ++i)
        {
            const char* o = bmvr::BindInputLabel(in.reserved0, in.systemName, 1 - hand, static_cast<bmvr::BindInput>(i));
            shared = o && std::strcmp(o, label) == 0;
        }
        std::snprintf(button, sizeof(button), "%s%s%s", hold ? "HOLD " : "", shared ? (hand == 0 ? "LEFT " : "RIGHT ") : "", label);
    }
    else
        strncpy_s(button, fallback, _TRUNCATE);
    for (char* c = button; *c; ++c)
        *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    std::snprintf(out, n, "%s = %s", button, what);
    return bound;
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
    {
        // Test only: BMVR_TEST_TOAST=<text> shows it as a small reload hint
        // for 10 min from the first frame (headless size checks).
        static int s_testToast = -1;
        if (s_testToast < 0)
        {
            char v[64]{};
            s_testToast = GetEnvironmentVariableA("BMVR_TEST_TOAST", v, sizeof(v)) && v[0] ? 1 : 0;
            if (s_testToast)
            {
                ShowToast(v, 600000);
                m_ToastSmall = true;
                Game::logMsg("Test toast: \"%s\"", v);
            }
        }
    }
    if (!device || !ToastVisible())
        return;
    if (g_Gfx.device && g_Gfx.device != device)
        ReleaseGfx();
    g_Gfx.device = device;
    const bool detail = m_ToastDetail[0] != 0;
    // Detail may hold a second line after '\n' (first-run welcome).
    const char* detail2 = detail ? std::strchr(m_ToastDetail, '\n') : nullptr;
    char detail1[128]{};
    if (detail)
        strncpy_s(detail1, m_ToastDetail, detail2 ? static_cast<size_t>(detail2 - m_ToastDetail) : _TRUNCATE);
    if (detail2)
        ++detail2;
    // First-run welcome sits at the panel's texel density (below), so its
    // lines use the panel's row font (34 px) and are opaque like the panel;
    // 24 px there came out ~70% of the panel's text and read blurry (user
    // 2026-11-24). Other detail toasts keep 24 px at their own scale.
    const bool welcome = m_WelcomeActive && !m_ToastSmall;
    const int detailPx = welcome ? 34 : 24;
    const int lineH = welcome ? 46 : 36;
    const int texH = detail2 ? 92 + 2 * lineH + 16 : detail ? 144 : 96;
    if (!g_Gfx.toastGpu || std::strcmp(g_Gfx.toastText, m_ToastText) != 0
        || std::strcmp(g_Gfx.toastDetail, m_ToastDetail) != 0)
    {
        if (g_Gfx.toastGpu) { g_Gfx.toastGpu->Release(); g_Gfx.toastGpu = nullptr; }
        // A long message ("PULL THE CHARGING HANDLE") was cut off at 512 px:
        // widen the texture (and the quad, below) so the letters keep their
        // size, up to 1024 px; past that the font shrinks.
        int texW = 512;
        int titlePx = detail ? 46 : 54;
        {
            Canvas probe(16, 16);
            if (!probe.ok())
                return;
            int need = probe.textWidth(m_ToastText, titlePx, true) + 32;
            if (detail)
                need = (std::max)(need, probe.textWidth(detail1, detailPx, false) + 32);
            if (detail2)
                need = (std::max)(need, probe.textWidth(detail2, detailPx, false) + 32);
            if (need > texW)
                texW = (std::min)(1024, (need + 15) & ~15);
            while (titlePx > 24 && probe.textWidth(m_ToastText, titlePx, true) + 32 > texW)
                titlePx -= 2;
        }
        g_Gfx.toastW = texW;
        Canvas c(texW, texH);
        if (!c.ok())
            return;
        c.fill(0, 0, c.w, c.h, kBg);
        c.fill(0, 0, c.w, 5, kAmber);
        c.text(m_ToastText, 0, 5, c.w, 96, kAmber, titlePx, true, DT_CENTER);
        if (detail2)
        {
            c.text(detail1, 8, 92, c.w - 8, 92 + lineH, kText, detailPx, false, DT_CENTER);
            c.text(detail2, 8, 92 + lineH, c.w - 8, c.h - 12, kText, detailPx, false, DT_CENTER);
        }
        else if (detail)
            c.text(detail1, 8, 92, c.w - 8, c.h - 8, kText, detailPx, false, DT_CENTER);
        c.setAlpha(welcome ? 255 : 225);
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
    Vector po = head + fwd * 30.f - up * 5.f;
    Vector qr = right, qu = up;
    // Small (reload hints): 4.2 (3.5 was hard to read, user 2026-11-18).
    // hw is per 512 texels, so a wider texture keeps the letter size.
    const float texScale = static_cast<float>(g_Gfx.toastW) / 512.f;
    float hw0 = m_ToastSmall ? 4.2f : detail ? 10.f : 6.f;  // warnings: larger, readable
    // First-run welcome: head-locked it smeared under reprojection and filled
    // the view at 30 HU (user 2026-11-23, the VR panel looked sharp). Placed
    // like the panel instead: latched in the room, centred in front at the
    // panel's distance and texel density; follows only when you look >40 deg away.
    if (m_WelcomeActive && !m_ToastSmall)
    {
        Vector mb;
        float rotY = 0.f;
        MenuDrawBody(mb, rotY);
        Vector yawFwd = fwd;
        yawFwd.z = 0.f;
        if (VectorNormalize(yawFwd) <= 0.01f)
            yawFwd = Vector(1.f, 0.f, 0.f);
        const Vector worldUp(0.f, 0.f, 1.f);
        auto frame = [&](Vector& o, Vector& f, Vector& r) {
            const float a = (rotY - m_WelcomeLatchYaw) * (kPi / 180.f), c = cosf(a), sn = sinf(a);
            auto yaw = [&](const Vector& v) { return Vector(v.x * c - v.y * sn, v.x * sn + v.y * c, v.z); };
            o = mb + yaw(m_WelcomeLatchDelta);
            f = yaw(m_WelcomeLatchFwd);
            r = yaw(m_WelcomeLatchRight);
        };
        Vector lo, lf, lr;
        if (m_WelcomeLatched)
            frame(lo, lf, lr);
        if (!m_WelcomeLatched || lf.Dot(yawFwd) < cosf(40.f * kPi / 180.f))
        {
            Vector yawRight = CrossProduct(yawFwd, worldUp);
            VectorNormalize(yawRight);
            m_WelcomeLatchDelta = GetViewOrigin(mb) + yawFwd * kPanelMinDistanceHu - worldUp * kPanelDropHu - mb;
            m_WelcomeLatchFwd = yawFwd;
            m_WelcomeLatchRight = yawRight;
            m_WelcomeLatchYaw = rotY;
            m_WelcomeLatched = true;
            frame(lo, lf, lr);
        }
        po = lo;
        qr = lr;
        qu = worldUp;
        hw0 = kPanelWidthHu * 0.5f * 512.f / kTexW;   // panel density: 512 texels = 10 HU
    }
    else
        m_WelcomeLatched = false;
    const float hw = hw0 * texScale, hh = hw0 * static_cast<float>(texH) / 512.f;
    const Vector corners[4] = { po - qr * hw + qu * hh, po + qr * hw + qu * hh, po - qr * hw - qu * hh, po + qr * hw - qu * hh };
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
