#pragma once
#include "vr_clip_hand_poses.h"

#include <atomic>

// Manual magazines (VrMagReload): MP5, glock, crossbow, .357, the shotgun (pump + shells) and the RPG
// (rocket pushed into the tube from the front). State is shared between the
// input code (game thread, VR::UpdateMagReload) and the viewmodel draw
// (render thread, ApplyMagReloadBones in hooks.cpp), which moves the gun's
// own magazine and bolt/slide bones. Nothing new is drawn: the magazine in
// the hand is the gun's own magazine bone placed at the left hand.
namespace MagReload
{
    enum : int
    {
        kLoaded = 0, // magazine in the gun
        kEmpty = 1,  // no magazine in the gun, trigger blocked
        kInHand = 2, // a fresh magazine in the left hand
        kDry = 3,    // fired empty: magazine still in, trigger blocked until a swap
    };
    enum : int
    {
        kWeaponNone = 0,
        kWeaponMp5 = 1,
        kWeaponGlock = 2,
        kWeaponShotgun = 3,
        kWeaponCrossbow = 4,
        kWeapon357 = 5,
        kWeaponRpg = 6,
        kWeaponCount = 7,
    };

    struct WeaponDef
    {
        const char* name;
        const char* magBone;   // magazine, rigid on its own bone
        const char* boltBone;  // charging handle (MP5) / slide (glock)
        int maxClip;
        float boltTravel;      // model units the bolt/slide moves back
        bool slideLocksBack;   // empty gun: slide held open until racked
        const char* emptyClick;
        const char* boltSound; // bolt/slide let go after a full pull
        // Mesh centres in the bone's own frame (from the model's vertices),
        // since the bone origins sit elsewhere on the gun.
        float magCenter[3];
        float boltCenter[3];
        // Tube-fed (shotgun): "magazine" = one shell, "bolt" = the pump,
        // "well" = the loading port on wellBone (not the shell's own pose).
        bool tube;
        const char* wellBone;
        float wellCenter[3];
        // Gun body bone: held at its idle pose while a stock clip is
        // scrubbed by the hands (nullptr = no scrubbing for this gun).
        const char* bodyBone;
    };
    const WeaponDef& Def(int weapon);

    // Game thread -> draw.
    extern std::atomic<int> g_Weapon;      // weapon the state below is for
    // Gun put away with empty hands (still the game's active weapon), else
    // kWeaponNone: its stock reload clips stay hidden too, or the game's own
    // refill played their shell / cylinder sounds (user 2026-11-27).
    extern std::atomic<int> g_HolsteredWeapon;
    extern std::atomic<int> g_State;
    extern std::atomic<bool> g_SlideLocked; // draw the slide/bolt held back
    // Held back = this fraction of boltTravel: almost fully, so a short pull
    // still moves it (glock fired dry).
    constexpr float kSlideHeldBack = 0.9f;
    extern std::atomic<bool> g_BoltGrab;    // left grip holds the bolt/slide
    extern std::atomic<bool> g_NeedRack;    // chamber empty: the bolt/slide is grabbable
    extern std::atomic<unsigned> g_DropTick; // a magazine left the gun/hand at this GetTickCount
    extern std::atomic<int> g_DropFrom;     // 0 = from the gun, 1 = from the hand
    extern std::atomic<unsigned> g_357EjectTick; // .357 cylinder open: old bullets fall out from here
    extern std::atomic<bool> g_EjectedEmpty;    // the magazine that left the gun last had no rounds
    extern std::atomic<int> g_DisplayClip;  // ammo HUD count while on (-1 = the game's)
    // Added to the HUD reserve: shotgun shells the game's background reload
    // already took into its tube that the hands have not loaded yet (they
    // still come off the hip), so the reserve does not drop by a tube's worth
    // when the gun runs dry (user 2026-11-19). 0 for every other gun.
    extern std::atomic<int> g_DisplayReserveExtra;

    // Stock clip scrubbed by the hands: sequence label + cycle, frozen (the
    // gun body is held at its idle pose so only the moving parts animate).
    extern std::atomic<const char*> g_ScrubSeq; // nullptr = not scrubbing
    extern std::atomic<float> g_ScrubCycle;
    // Set by the viewmodel code when a scrub (or the debug hold) is applied.
    extern std::atomic<bool> g_ScrubActive;

    // Draw -> game thread.
    extern std::atomic<bool> g_NearWell;   // magazine in the hand is at the well
    extern std::atomic<float> g_SeatDistCm;   // last magazine -> well distance (cm)
    extern std::atomic<float> g_SeatAngleDeg; // last magazine angle off its seated pose
    extern std::atomic<bool> g_NearBolt;   // left hand is at the bolt/slide
    extern std::atomic<float> g_BoltPull;  // 0..1 of boltTravel while grabbed
    // RPG: the rocket in the hand is on the tube axis in front of the muzzle
    // (0 = just engaged at the muzzle .. 1 = seated), -1 = not in the tube.
    extern std::atomic<float> g_RpgInsert;

    // Glove draw -> viewmodel draw: centre of the drawn left fist in the
    // left controller's frame (forward, right, up; game units) and when it
    // was last written (GetTickCount, 0 = never). The magazine/shell in the
    // hand sits there, so it follows the glove's own pose offsets and size.
    extern std::atomic<float> g_LeftPalmLocal[3];
    extern std::atomic<unsigned> g_LeftPalmMs;
    // Point between the drawn left thumb and index (distal joints), same
    // frame as g_LeftPalmLocal: where a pinched shotgun shell is held.
    extern std::atomic<float> g_LeftPinchLocal[3];
    extern std::atomic<unsigned> g_LeftPinchMs;
    // VrClipHandPose: the reload-clip hand pose (ClipHandPose::Id) the left
    // glove drew last, when (GetTickCount), and Black Mesa's L_Wrist frame on
    // that glove in the same controller frame as g_LeftPalmLocal (rows =
    // forward, right, up; columns = the wrist's x, y, z axes, unit length,
    // then its origin) plus game units per viewmodel model unit at the
    // glove's size. The held magazine is placed from it.
    // Fist tunnel of the drawn left glove, same controller frame: centre
    // (0..2) and unit knuckle-line direction pinky -> index (3..5).
    extern std::atomic<float> g_LeftTunnelLocal[6];
    extern std::atomic<unsigned> g_LeftTunnelMs;
    // RPG rocket in the left hand, held through a closed fist (VrClipHandRpg).
    bool RocketInFist();
    extern std::atomic<int> g_LeftClipPose;
    extern std::atomic<unsigned> g_LeftClipMs;
    extern std::atomic<float> g_LeftClipWrist[12];
    extern std::atomic<float> g_LeftClipUnit;
    // Which reload-clip hand pose the left glove should take now
    // (ClipHandPose::Id, -1 = none): magazine in the hand, MP5 bolt or
    // glock slide held.
    int WantedClipHandPose();
    // The reload-clip pose for this weapon's magazine/loader/shell in the hand (-1 = none).
    int InHandClipPose(int weapon);
    // GetTickCount of the last viewmodel draw that showed the gun's own
    // left hand as the support hand (VrSupportHandLock): the glove skips it.
    extern std::atomic<unsigned> g_SupportHandFromGunMs;
    // GetTickCount of the last satchel radio (detonator) draw: until it
    // exists (first charge out) the right glove shows instead.
    extern std::atomic<unsigned> g_SatchelRadioMs;
    // GetTickCount of the last satchel bag draw (hung from the left glove):
    // the left glove closes into a fist round its top while it is drawn.
    extern std::atomic<unsigned> g_SatchelBagMs;
    // Gun's own hands as drawn (viewmodel R_Wrist / L_Wrist bone-to-world,
    // 3x4 rows, after every bone edit) and when (GetTickCount, 0 = never):
    // the glove renderer caps their open HEV cuffs there.
    extern std::atomic<float> g_GunWrist[2][12];
    extern std::atomic<unsigned> g_GunWristMs[2];
    // GetTickCount of the last input frame with the detonate trigger held
    // (satchel hands): the radio's own thumb is drawn on the button at once,
    // the game's detonate clip only arrives a few frames after the blast.
    extern std::atomic<unsigned> g_DetonateHeldMs;
    // Shotgun pump back stroke: a spent hull flies out of the ejection port
    // (GetTickCount, 0 = none).
    extern std::atomic<unsigned> g_EjectTick;

    // Frag grenade pin (VrGrenadePin). Game thread -> draw: state; draw ->
    // game thread: left fist at the ring, how far the pin has been pulled.
    enum : int { kPinIn = 0, kPinHeld = 1, kPinPulledInHand = 2, kPinDropped = 3 };
    extern std::atomic<bool> g_PinWeapon;   // the frag grenade is drawn with the feature on
    extern std::atomic<int> g_PinState;
    extern std::atomic<unsigned> g_PinDropTick;
    extern std::atomic<bool> g_PinNear;
    extern std::atomic<float> g_PinPull;    // model units at scale 1 since the grab
    extern std::atomic<bool> g_PinThrown;   // set by the frag spawn hook: next grenade has its pin

    // Shotgun: the attached support hand holds the pump and works it (toggle
    // hold: no squeeze needed; hold mode: while the grip is held).
    bool PumpFollowsSupportHand();
}
