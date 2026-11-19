#pragma once
// Ladder rungs for hand climbing (PhysicalLadders): the world segments of
// the rungs of the ladder models placed as static props in the current map.
// Black Mesa's func_useableladder only knows two mount points; what you see
// is static props (metalladder002 sections, sewer rungs, pool ladders...).
// The map's BSP static prop lump is read once per map through the game's
// file system, and each prop with a known ladder model (table in the .cpp,
// rung heights measured from the model vertices) adds its rungs.
#include "sdk.h"

namespace bmvr::ladders
{
    struct Rung
    {
        Vector a, b;   // world end points (where the rails hold it)
        int model = -1; // ladder model (ModelName), for per-model grip tuning
    };
    // Path below models/ of a ladder model index ("props_c17/metalladder001.mdl").
    const char* ModelName(int model);
    // Rungs of the given map ("bm_c1a1e"); loads on the first call for a map.
    // Empty when the map could not be read or has no known ladder props.
    int MapRungs(const char* map, const Rung** out);
    // Nearest rung point to p within maxDist (world units). The point keeps a
    // little distance from the rails. False when none is that close.
    bool NearestRung(const char* map, const Vector& p, float maxDist, Vector& onRung, float* dist = nullptr,
        Vector* axis = nullptr,    // axis: unit vector along that rung (a -> b)
        int* model = nullptr);     // that rung's ladder model
}
