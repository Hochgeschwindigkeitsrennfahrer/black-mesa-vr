#pragma once
#include <cstddef>
#include <cstdint>

// Black Mesa's god rays (np_gr_quality, passes __grmask0 / __grblur0 /
// __grfinal0): the mask pass draws a full-screen quad at the light's depth
// (VS c27.z) against the scene depth, so "farther than the light" is a test on
// the depth along the view axis. A surface the same distance away as the
// light passes or fails depending on where it sits in the view: with the
// head pitched the planar depth of a point off to the side shrinks by
// cos(angle), and the rays through a cave opening came and went as the
// player looked up and down (bm_c4a1a1, `fogdistancetest`, 2026-11-27:
// threshold 2851 u level, 3162 u looking up 25 deg, 2007 u looking down).
// The level mask (GodRayRadialMask, default on) draws the quad with a pixel
// shader that writes, per pixel, the depth the stock test would use if the
// head looked level toward the light's yaw (how flat players see these maps
// when looking at the light), so it no longer depends on head pitch. A pure
// radial test ("farther from the eye than the light") was stable too but
// left only a faint shaft there. The light's position comes from its
// depth (c27.z) + its screen position (the blur pass's c10, same eye, the
// previous frame) through the frame's inverse view-projection (PS c52-c55,
// eye c51). Bytecode + maths here; the draw-time swap is in DXVK's
// d3d9_device.cpp (BmvrGodRayMaskBegin).
namespace bmvr
{
    // CreateVertexShader: the mask VS (full-screen quad, `mov o0.z, c27.z`).
    bool GodRayIsMaskVs(const uint32_t* bytecode);
    // CreatePixelShader: the blur PS (first `add rN.xy, c10, -vM`, then
    // `mul rN.xy, rN, c9.x`, radial taps toward the light at c10.xy).
    bool GodRayIsBlurPs(const uint32_t* bytecode);
    // CreatePixelShader: the final PS (blends the blur result into the eye:
    // reads c10 + c11, dp2add for the distance to the light, pow falloff).
    bool GodRayIsFinalPs(const uint32_t* bytecode);
    // GodRayBlurScale (Debug row "God rays blur size"): the blur pass draws
    // into a 1/divisor-size stand-in for the game's eye-sized _rt_lsBlur*
    // (swapped at the draw, read back by the final pass with linear
    // filtering). 1 = stock full size. The blur is 64 taps per pixel at eye
    // resolution (~0.8 ms/frame for both eyes, 2026-11-27).
    int GodRayBlurDivisor();
    // Replacement mask PS (oC0 = 0, oDepth = radial threshold; c104-c105 from
    // GodRayMaskConstants, c106.xy = 1 / target size from the draw's viewport).
    const uint32_t* GodRayMaskPs(size_t* tokens);
    constexpr unsigned kGodRayConstReg = 104;
    // Blur draw: the light's screen position (c10.xy, 0..1) for this eye.
    void GodRayNoteBlur(int eye, const float c10[4]);
    // 1/2 = the eye being drawn (MulticoreMode playback aware), 0 = none.
    int CurrentDrawEye();
    // Mask draw: c104-c105 for the replacement PS from the mask's light depth
    // (VS c27.z) and the frame's PS c51 (eye) + c52-c55 (inverse
    // view-projection). False = keep the game's mask (no light position
    // yet, light behind the eye, odd numbers).
    bool GodRayMaskConstants(int eye, float lightDepth, const float c51[4], const float c52to55[16], float out[8]);
}
