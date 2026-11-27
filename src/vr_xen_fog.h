#pragma once
#include <cstdint>
#include <vector>

#include "vr_xen_fog_patch.h"

// Xen fog pixel shaders (global __gbxog0 and the __gbxog* volumes, all
// compiled combos): their animated noise is looked up by view direction, so
// in stereo it sits at infinity and slides with the player through the floor
// (fog dump 2026-11-27). XenFogPatchPixelShader rewrites them at creation so
// the noise comes from one point both eyes share (the centre eye, flat
// look) or from world position x a scale (Debug row "Xen fog noise"). Bytecode work:
// vr_xen_fog_patch.cpp (host test tools/tests/xen_fog_patch_test.cpp).
namespace bmvr
{
    enum class XenFogShader { NotFog, Patched, UnknownFog };
    // CreatePixelShader (DXVK, once per shader). Patched: `out` holds the new
    // bytecode. UnknownFog: reads the fog's constants but its structure is not
    // the known one (game update): logged + counted as a missing gameplay hook.
    XenFogShader XenFogPatchPixelShader(const uint32_t* bytecode, std::vector<uint32_t>& out);
    // c100/c101 for the patched shader: c100.x = 1 / noise size, .y = world
    // mix, .z = anchor mix; c101.xyz = anchor (centre eye, SetXenFogAnchor).
    void XenFogShaderConstant(float out[8]);
    // b15: the patched shader runs its world-noise lookups (World, Auto).
    bool XenFogWorldLookups();
    // Centre eye of the frame being drawn (RenderView, main view).
    void SetXenFogAnchor(float x, float y, float z);
    // CreatePixelShader: DXVK refused the patched bytecode (original used).
    void XenFogPatchFailed(uint32_t bytes);
}
