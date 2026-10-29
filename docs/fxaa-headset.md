# FXAA missing from the HMD image

**Symptom (user, 2026-10-29):** the headset image shimmers. The desktop view looks fine. Toggling `mat_fxaa_enabled` 1 / 0 with `Msaa=0` makes no visible difference in the HMD. Not tied to multicore.

**Status:** cause confirmed in the code and consistent with the user's log; fix implemented (below), not yet compiled or headset-tested.

Log note (user's local check, 2026-10-29): `dev/engine_post_nxtgen` does reach `dDrawScreenSpaceRectangle` on eye passes, but the `DSSR skip bloom` line is capped at 12 and the bloom stages use all 12. Its absence from that log line proves nothing.

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

1. Log: `DSSR skip bloom` is capped at 12 lines, which the bloom stages fill, so `engine_post_nxtgen` may never show there. Use the new `DSSR engine_post on eye` line instead (below).
2. Log: the first 24 unique `DSSR <material>` lines. If a separate FXAA material shows up there, FXAA is its own pass and this hypothesis is wrong.
3. Ghidra `client.dll`: xref the `mat_fxaa_enabled` string to its reader. Expect it to set a material var on `engine_post_nxtgen` (inside `DoEnginePostProcessing`), not to draw its own material.
4. HMD A/B: temporarily let `engine_post*` through the skip and compare shimmer with `mat_fxaa_enabled` 1 vs 0 (ghosts are expected to return in `bm_c1a0a`).

## Candidate fix (original proposal)

Keep skipping the bloom-only steps (`lumcompare` / `downsample` / `blurfilter` / `blur_combine`) on stereo eyes, but let `engine_post*` draw with its bloom contribution zeroed via the material's bloom-amount var (exact BM name to be taken from Ghidra). The stale `_rt_Small*FB*` contents then contribute nothing, and FXAA plus colour correction come back.

`RUNTIME.md` forbids retrying dest / UV / viewport rescale of `engine_post`, but explicitly leaves shader constants open as a new integration point. Any fix must be re-checked for the cafeteria ghosts in the HMD.

## Implemented: `StereoEnginePost` (2026-10-29)

`VR/config.txt` key `StereoEnginePost`, default `true` (code default in `bmvr_flags.cpp`, not listed in the shipped config, same as `VrSkipMotionBlur`). `StereoEnginePost=false` restores the old full skip for A/B.

When on, in `dDrawScreenSpaceRectangle` during a stereo eye pass:

- `engine_post*` (not HUD) is drawn instead of skipped.
- `lumcompare` / `downsample*` / `blurfilter*` / `blur_combine*` stay skipped.
- Before the draw, the material's bloom var is zeroed so the stale `_rt_Small*FB*` contents add nothing (lookup below).
- The original value is restored after the next render-target change (`PushRenderTargetAndViewport` / `PopRenderTargetAndViewport`) or `EndFrame` on the same thread. The DSSR quad can stay in the buffered dynamic mesh after the call returns, and the shader reads the var when that batch flushes; Source 2013 `SetRenderTargetEx` flushes buffered primitives. Restoring right after the DSSR call could let the bloom value back in.
- On a 1x eye RT (context RT == eye size), viewport and dest are pinned to `0,0 eyeW×eyeH` for the draw, and the src uv is widened to the whole `_rt_FullFrameFB` when the engine reports it eye-sized (±32). The previous viewport is restored right after the call. An mc ring RT is logged and left alone.

### Headless A/B on 9a0994e (user, 2026-10-29)

`bm_c1a0a` lobby, MulticoreMode on, window 1280×640, eye 1152×1184:

- `engine_post_nxtgen` ran on the eyes, but its output was **squeezed into the top 640/1184 rows** (full width, bottom black). First call `dest=1280x640`, later `dest=1152x1184`: dest was already eye-sized, so the window-height part is the context viewport. Fixed by the viewport/dest pin above (next run: check `pinned=1` and the `vp=` before values).
- `DSSR engine_post params dev/engine_post_nxtgen (0):` — `ShaderParamCount` / `GetShaderParams` (sdk.h slots) return nothing on this build, and no bloom var was found (`bloomVar=none set=0`). Those slots are no longer called.

### Bloom var discovery (second pass)

1. `FindVar` (IMaterial slot 11) probes `$basetexture`, `$fbtexture`, `$aaenable` (to prove the slot works), then `$bloomamount`, `$bloomscale`, `$bloom_amount`, `$bloomstrength`, `$bloomamt`, `$bloom`, then `$bloomenable`, `$bloom_enable`. A hit counts only if the returned var's `GetName` matches the name asked for.
2. Param scan, reads only: FindVar returns the shared dummy var even on a miss, which gives the `CMaterialVar` vtable. The first 0x200 bytes of the material object are scanned for a pointer to the longest run of pointers whose objects carry that vtable — the material's param array. Every name is logged.
3. Pick order: known float name → scanned `*bloom*` float param (not texture / enable / sampler) → `$bloomenable`-style bool, zeroed with `SetIntValue` (slot 4) and restored the same way.

Log lines (first 4 materials):

```
DSSR engine_post vars dev/engine_post_nxtgen varVt=<ptr> FindVar: $basetexture=1 $fbtexture=0 ... $bloomamount=0 ...
DSSR engine_post params dev/engine_post_nxtgen (<n> scanned): $basetexture $fbtexture ...
```

FindVar codes: `1` found and the name matches, `2` found but the var reports a different name (slot or layout mismatch), `0` not found. If `$basetexture` is not `1`, FindVar itself is wrong on this build. If the scan finds 0 params, paste the `varVt` value; the bloom name can also be taken from `client.dll` strings near `DoEnginePostProcessing` (`$bloom*` literals it sets on the material).

Eye line (first 8 calls, own counter):

```
DSSR engine_post on eye dev/engine_post_nxtgen bloomVar=<name|none> set=<0|1> orig=<v> rt=WxH vp=x,y WxH pinned=<0|1> dest ... -> ... src=WxH uv=(..)-(..) -> (..)-(..)
```

### Assumptions to verify at runtime

- `IMaterialVar` slot 1 = `GetName`, slot 3 = `SetFloatValue`, slot 4 = `SetIntValue`; `m_intVal` at `+0x8`, `m_VecVal[0]` at `+0xC` (Source 2013 / CS:GO `imaterialvar.h`). Calls are under SEH; an implausible original (float outside 0–1000, bool not 0/1) leaves the var untouched (`set=0`).
- The viewport restore right after the DSSR call assumes a viewport change flushes the buffered quad first (same assumption as the existing `EyeRtPush` / `SehViewport` draws). If `pinned=1` and the squeeze persists, that assumption is wrong for this path.
- Under `mat_queue_mode 2` this hook runs on the render thread. If BM routes material-var writes through queued temp storage, the zero may not take effect (ghosts return with `set=1`). Compare single-core vs multicore.
- If no render-target change follows the eye `engine_post` draw on that thread before the desktop draws `engine_post`, the desktop may lose bloom for that frame.

### HMD test

1. `bm_c1a0a` cafeteria: no fluorescent-tube / table ghosts (bloom regression check).
2. Shimmer with `mat_fxaa_enabled 1` vs `0`: should now differ.
3. HMD contrast / grading should now match the desktop.
4. `StereoEnginePost=false`: old behaviour.
