#pragma once

#include <atomic>

// Manual magazines (VrMagReload): MP5 and glock. State is shared between the
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
        kWeaponCount = 3,
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
    };
    const WeaponDef& Def(int weapon);

    // Game thread -> draw.
    extern std::atomic<int> g_Weapon;      // weapon the state below is for
    extern std::atomic<int> g_State;
    extern std::atomic<bool> g_SlideLocked; // draw the slide/bolt held back
    extern std::atomic<bool> g_BoltGrab;    // left grip holds the bolt/slide
    extern std::atomic<bool> g_NeedRack;    // chamber empty: the bolt/slide is grabbable
    extern std::atomic<unsigned> g_DropTick; // a magazine left the gun/hand at this GetTickCount
    extern std::atomic<int> g_DropFrom;     // 0 = from the gun, 1 = from the hand

    // Draw -> game thread.
    extern std::atomic<bool> g_NearWell;   // magazine in the hand is at the well
    extern std::atomic<bool> g_NearBolt;   // left hand is at the bolt/slide
    extern std::atomic<float> g_BoltPull;  // 0..1 of boltTravel while grabbed
}
