#pragma once

#include <atomic>

// Manual magazine (prototype, MP5 only): state shared between the input code
// (game thread, VR::UpdateMagReload) and the viewmodel draw (render thread,
// which moves or hides the gun's own magazine bone).
namespace MagReload
{
    enum : int
    {
        kLoaded = 0, // magazine in the gun (stock look)
        kEmpty = 1,  // ejected: gun without magazine, trigger blocked
        kInHand = 2, // a fresh magazine in the left hand
        kDry = 3,    // fired empty: magazine still in, trigger blocked until a swap
    };
    extern std::atomic<int> g_State;
    // Set by the draw: the magazine in the hand is within seating distance
    // of the magazine well.
    extern std::atomic<bool> g_NearWell;
}
