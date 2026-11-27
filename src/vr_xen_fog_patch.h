#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Core of the Xen fog stereo fix (no game dependencies; host-testable with
// tools/tests/xen_fog_patch_test.cpp). See vr_xen_fog.h.
namespace bmvr
{
    struct XenFogPatchInfo
    {
        bool fogLike = false;      // reads the fog's constant set
        bool patched = false;
        size_t bytes = 0;
        uint32_t fnv = 0;          // FNV-1a of the bytecode
        int worldHits = 0, lookupHits = 0;
        uint32_t worldReg = 0, maxTemp = 0;
    };
    // Patched: `out` holds the new bytecode. fogLike && !patched = a fog
    // shader whose structure is not the known one (game update).
    XenFogPatchInfo XenFogPatchBytecode(const uint32_t* bytecode, std::vector<uint32_t>& out);
    constexpr unsigned kXenFogConstReg = 100;    // c100 + c101
    constexpr unsigned kXenFogConstCount = 2;
    constexpr size_t kXenFogInsertTokens = 35;   // after the world instruction
    // b15 = run the world-noise copy (World and Auto modes), set on bind.
    constexpr unsigned kXenFogBoolReg = 15;
}
