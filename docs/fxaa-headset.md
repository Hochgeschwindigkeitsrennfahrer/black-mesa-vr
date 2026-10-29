# FXAA missing from the HMD image

**Symptom (user, 2026-10-29):** the headset image shimmers. The desktop view looks fine. Toggling `mat_fxaa_enabled` 1 / 0 with `Msaa=0` makes no visible difference in the HMD. Not tied to multicore.

**Status:** hypothesis from code reading. Not yet verified in Ghidra or at runtime.

## Likely cause: stereo bloom skip also drops `engine_post`

`Hooks::dDrawScreenSpaceRectangle` (`src/hooks.cpp`) returns without drawing any material that `StereoBloomMaterial()` matches while a stereo eye world pass is active:

```cpp
if (StereoBloomMaterial(matName) && Hooks::m_VR
    && Hooks::m_VR->StereoWorldPassActive()
    && (bmvr::OffscreenWorldMatchesEyes() || Hooks::m_VR->ShouldHwndBlitStereo()))
    return; // logs "DSSR skip bloom %s ... stereo=1"
```

`StereoBloomMaterial()` matches any name containing `engine_post`, which includes `dev/engine_post_nxtgen`. The skip was added 2026-09-03 for the `bm_c1a0a` cafeteria bloom ghosts (`RUNTIME.md`, "Verified: stereo bloom skip").

`engine_post` is not bloom only. In the CS:GO-lineage post stack that BM's nxtgen post comes from, it is the final full-screen LDR composite: bloom combine, tonemap, colour correction, and the software AA path (`$AAENABLE` / `$AAINTERNAL*`). FXAA has to run last, on the LDR image, which is that pass. Skipping the whole draw on the eyes therefore most likely skips FXAA too.

This matches every observation:

| Observation | Explanation |
| --- | --- |
| Desktop fine, HMD shimmers | Desktop/mirror is not a stereo eye pass, so `engine_post` runs there. |
| `mat_fxaa_enabled` 1 vs 0 identical in HMD | The pass that reads it never runs on the eyes. |
| `Msaa=0` doesn't help | That only disables HL2VR MSAA. `VR::UpdateHl2vrMsaa` forces `mat_fxaa_enabled 0` only when `Msaa >= 2`. |
| Not multicore-related | The skip has no `mat_queue_mode` dependency. |

Side effect: the eyes also lose `engine_post` tonemap and colour correction, so HMD contrast and grading may differ from the desktop.

## How to verify

1. Log: look for `DSSR skip bloom dev/engine_post_nxtgen dest=... stereo=1` in the bmvr log during gameplay.
2. Log: the first 24 unique `DSSR <material>` lines. If a separate FXAA material shows up there, FXAA is its own pass and this hypothesis is wrong.
3. Ghidra `client.dll`: xref the `mat_fxaa_enabled` string to its reader. Expect it to set a material var on `engine_post_nxtgen` (inside `DoEnginePostProcessing`), not to draw its own material.
4. HMD A/B: temporarily let `engine_post*` through the skip and compare shimmer with `mat_fxaa_enabled` 1 vs 0 (ghosts are expected to return in `bm_c1a0a`).

## Candidate fix (not implemented)

Keep skipping the bloom-only steps (`lumcompare` / `downsample` / `blurfilter` / `blur_combine`) on stereo eyes, but let `engine_post*` draw with its bloom contribution zeroed via the material's bloom-amount var (exact BM name to be taken from Ghidra). The stale `_rt_Small*FB*` contents then contribute nothing, and FXAA plus colour correction come back.

`RUNTIME.md` forbids retrying dest / UV / viewport rescale of `engine_post`, but explicitly leaves shader constants open as a new integration point. Any fix must be re-checked for the cafeteria ghosts in the HMD.
