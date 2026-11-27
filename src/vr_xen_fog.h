#pragma once
#include <cstdint>
#include <vector>

#include "vr_xen_fog_patch.h"

// Xen fog pixel shaders (global __gbxog0 and the __gbxog* volumes, all
// compiled combos): their animated noise is looked up by view direction, so
// in stereo it sits at infinity and slides with the player through the floor
// (fog dump 2026-11-27). XenFogPatchPixelShader rewrites them at creation so
// the noise comes from world position x a scale (c100.x), mixed with the old
// direction term by c100.y (live Debug rows). Bytecode work:
// vr_xen_fog_patch.cpp (host test tools/tests/xen_fog_patch_test.cpp).
namespace bmvr
{
    enum class XenFogShader { NotFog, Patched, UnknownFog };
    // CreatePixelShader (DXVK, once per shader). Patched: `out` holds the new
    // bytecode. UnknownFog: reads the fog's constants but its structure is not
    // the known one (game update): logged + counted as a missing gameplay hook.
    XenFogShader XenFogPatchPixelShader(const uint32_t* bytecode, std::vector<uint32_t>& out);
    // c100 for the patched shader: x = 1 / noise size, y = world mix (0..1).
    void XenFogShaderConstant(float out[4]);
    // CreatePixelShader: DXVK refused the patched bytecode (original used).
    void XenFogPatchFailed(uint32_t bytes);
}
