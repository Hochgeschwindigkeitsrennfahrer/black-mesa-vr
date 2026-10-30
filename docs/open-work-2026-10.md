# Open work and findings (2026-10-31)

Running list for the `fxaa-troubleshooting` branch and the HMD issues discussed alongside it. Setup in the user's reports: PSVR2, SteamVR on Linux, `VRRuntimeBackend=openxr` (OpenXR helper), `MulticoreMode=true`, `OffCenterEyes=true`, `WorldRenderAtEyeSize=true`, `RenderScale=1.2`, `Msaa=0`, window 1920×1080. Details for the FXAA part are in `fxaa-headset.md`.

Status words follow `AGENTS.md` rule 17.

## 1. FXAA / engine_post on the eyes

| Item | Commit | Status |
| --- | --- | --- |
| Stereo bloom skip also dropped `engine_post*` (final LDR pass: tonemap, colour correction, FXAA) | — | cause confirmed in code |
| `StereoEnginePost` (default true): run `engine_post*` on eyes, rest of the bloom chain stays skipped | 9a0994e | headset: FXAA visibly better, marginal |
| Bloom off on eye `engine_post` via `$bloomenable` (CS:GO `IMaterialVar` layout detected, GetName slot 2) | 0db7d66 | runtime-verified in log (`bloomVar=$bloomenable set=1 varSlot=2`); cafeteria ghost check (`bm_c1a0a`) not yet reported |
| Src rect mapped onto the whole src texture when dest is the eye (headless window/eye mismatch squeezed the image) | 0db7d66 | headless-verified fix target; no-op in the user's setup (rect already matched) |
| Load screen zoomed into its top-left | 5ce0683 | fix: eye `engine_post` path gated on gameplay (`IsGameplayEligible() && IsInGame()`); **not yet tested** |

FXAA can only do so much here: it smooths 1–2 px edges at 4096 px, which the compositor then shrinks about 2×. Most of the remaining wall/floor shimmer is not something FXAA can fix (see §2).

## 2. Wall/floor shimmer: OpenXR helper resample (main suspect)

Evidence (`VR/openxr_helper64/openxr_helper64_from_game.log`, 2026-10-30):

```
Using sRGB shader Vulkan eye blit path ... crop=0
[OpenXR][EyeBlit] sourceSize=4096x4096 sourceBounds=(0 0 1 1) dstSize=3824x3904 containBlit=(0,40 3824x3824) path=shader
Using explicit game projection FOV eye=0 (L=-1.0734 R=0.7583 U=0.9257 D=-0.9257) runtime=(same)
```

- Desktop mirror (the left eye image, shrunk about 2×) looks clean while the HMD shimmers, so the problem is added after our render. Other games on the same SteamVR/PSVR2 setup are fine.
- The helper does not copy the eye 1:1. It redraws 4096² → 3824×3824 (×0.934) with a single-mip bilinear shader, then SteamVR resamples again for lens correction. A ratio that close to 1 creates a sliding moiré pattern on fine detail as the head moves. Other games submit at the swapchain size, so they get only SteamVR's resample.
- `ComputeContainBlitRect` keeps the source aspect and never scales up. With an explicit full-swapchain FOV that is wrong:
  - 40 px black bars top/bottom and a ~2 % vertical squeeze at RenderScale 1.2.
  - Any RenderScale < 1.0 draws a shrunken image with wide black borders (do **not** use RenderScale 0.8 as an aliasing test).
- The 4096 eye cap (`ComputeOffscreenEyeSize`) is why raising the SteamVR resolution made no difference this session: 3824×3904 × 1.2 is cut to 4096×4096.
- Correction to an earlier claim: with `OffCenterEyes=true` the game renders the asymmetric frustum directly and submits it uncropped; the `boundsL=0.7571` in the helper-ready line is not used in gameplay. Pixel density is fine (about 1.05–1.07× the swapchain).

**User (2026-10-31):** `RenderScale=1.0` (eye = swapchain, blit already ~1:1) still aliases, as in many earlier sessions. So the helper resample is **not** the main cause of the shimmer. The two helper fixes below are still correct and remove a resample at every other RenderScale, but don't expect them to cure the shimmer. Gamma checked: the blit shader decodes sRGB (`srgbToLinear`) before writing the sRGB swapchain.

**Helper changes (done, not compiled or headset-tested):**
1. 13695af — fill the whole image area when the game supplies the FOV (explicit, or game renders the runtime FOV). Contain fit only for eyes without a matching FOV. Log: `Using ... eye blit path ... fit=fill`.
2. bae6915 — eye swapchains recreated at the game eye size (within `maxImageRect`) before the next blit, so the copy is 1:1 and only the runtime resamples. A refused size is not retried; the recommended size comes back. `OpenXRHelperSwapchainAtSourceSize=false` turns it off (code default true, not in the shipped config). Log: `Eye N swapchain WxH -> WxH (game eye size; ...)`.

The wall/floor shimmer cause is still open. Remaining candidates: BM content itself (specular/normal-map detail with no temporal AA, far more visible in a moving HMD than on a monitor), and the dxvk.conf AF / LOD bias test.

Other things already checked: in-game anisotropic 16x is on. `dxvk.conf` with `d3d9.samplerAnisotropy = 16` / `d3d9.samplerLodBias = 0.5` next to `bms.exe` is being tested by the user; do not ship it yet.

## 3. Lights flicker unless close

Reported: flickers the same in both eyes; `MulticoreMode=false` does not change it.

- **Not the helper resample.** That makes detail crawl, not lights turn on and off.
- **Stereo pixel-visibility mismatch: less likely now.** A per-eye query mix-up usually differs between eyes.
- **HL2VR `hl2vr_pixelvis` is not a flicker fix.** It is a performance change: `PixelVisibility_EndCurrentView` runs once per stereo pair instead of per eye (`research/hl2vr/optimization.md` §14). The 2026-09-05 retry gave a one-eye 2D skybox hole. Its hook code is gone; only the `TryHl2vrPixelVis` flag and the persist-skip remain (`bmvr_flags.cpp`). Retrying it would not address flicker that is identical in both eyes, so it is parked.
- **Next tests (user, console):**
  - `r_dopixelvisibility 0` — glows ignore occlusion. If the flicker stops, it's the glow/halo occlusion queries.
  - `r_pixelvisibility_partial 0` — L4D2VR sets this (`third_party/l4d2vr/L4D2VR/local_client_convars.nut`).
  - `OffCenterEyes=false` — commit 090318f was already chasing "the light that drops out under OffCenterEyes (bm_c2a2b)". BM's deferred-light per-camera update (`CNewRenderer::UpdatePerCamera`, client+0x1870D0) builds its frustum from the view. An off-centre window that its culling does not honour would drop lights near the frustum edges but keep lights whose volume contains the camera, which fits "fine when close". A `trace N` run logs each `UpdatePerCamera` view.
  - Note which part flickers: the glow/halo sprite, or the lit area on walls/floors.

## 4. Seeing through walls since the culling changes

Commits in question: 73de657 (per-eye occlusion tracker ids, `OcclusionTrackerPerEye`, default true) and 6cc0009 (`LeftEyeCullPad` default 0).

- Neither changes the camera origin, the near plane (`zNear=7.0`) or the projection.
  - 73de657 only gives the right eye its own occlusion-tracker view id (+0x100) in `COcclusionTracker::BeginView`.
  - 6cc0009 only changes a default, and the user's config already had `LeftEyeCullPad=0`.
- Extra objects drawn are still depth-tested against the wall, so per-eye occlusion alone cannot make a wall transparent.
- Possible link: before 73de657, objects behind a wall that only the right eye's queries would have shown stayed culled. When the camera gets into a wall (head leaning past the player hull with a 7-unit near plane), more of what is behind it is now drawn, so the same clipping is far more noticeable.
- Unknown: whether the tracker's per-view state is sized for ids ≥ 0x100. An out-of-range id would be a real bug; needs Ghidra on the tracker's per-view storage.
- User (2026-10-31): happens on physical lean **and** stick-walking, in both eyes.
- **Not the bulletproof-glass change (58d54df).** It only adds a `TraceRay` from the body's eye to the muzzle inside the shoot-position rewrite, and only when a shot is fired. It does not touch the view, the player hull or movement. `dTraceRay` is a pure pass-through.
- **No camera-vs-wall handling exists.** The view is the body origin + the HMD offset from the recenter point (uncoupled 6DoF). Nothing traces from the body to the eye or keeps the eye out of walls. The player hull stops the body about 16 units from a wall. If the head is more than about 9 units (≈23 cm) from the recenter point towards the wall, the 7-unit near plane is inside it, whether you lean or stick-walk.
- **User (2026-10-31): explained by not recentering after walking around with the game paused** (passthrough, desk, back). Added `AutoRecenterOnResume` (default true; VR panel VIEW row "Recenter after pause menu"; cvar `bmvr_recenter_on_resume`). Closing GameUI in a map re-latches the head position only. The stick-turn direction is kept regardless of "Recenter resets turning". Log: `Recenter position (pause menu closed)`. Not compiled or tested yet.
- Earlier test list, kept for reference:
  1. Recenter while standing at your normal spot, then stick-walk straight into a wall. If the see-through is gone, it is the head offset (long-standing, not a regression). Fix: push the eye back to the body with a hull trace, or move the body with the head.
  2. `OcclusionTrackerPerEye=false` — does it change anything?

## 5. Other open items

- MulticoreMode eye copy (`MC QueueMcCompositeToRing: no ICallQueue` / `MC Present fallback: ICallQueue did not copy 1x eyes`). Both lines are log-capped (4 / 8 lines), so "every frame" was wrong: the same log also shows the queued path working (`MC queued 1x-eye OpenXR copy ... slot=186`, `MC 1x eyes -> OpenXR publish slot copied=1`). With multicore, the finished eye images are copied into the OpenXR publish slot by a functor queued onto the material (render) thread. When `GetCallQueue` returns nothing for that frame, the copy is done on the Present path instead. It only moves where the finished images get copied: it cannot cause aliasing, light flicker or culling changes. Frequency unknown; the `OpenXR publish rate` line now reports `mcQueued / mcQueueFail / mcMatCopied / mcPresentFallback` per second. Act only if the fallback rate is high.
- `DSSR engine_post params ... (0 scanned)`: the material param scan finds nothing on this build. Harmless now (`$bloomenable` comes from FindVar).
- `AntiAliasing=0` in the user's config is a retired key (ignored).

## 6. Test checklist (next build)

1. Load screen no longer zoomed (5ce0683).
2. Cafeteria `bm_c1a0a`: no bloom ghosts with `StereoEnginePost=true`.
3. `RenderScale=1.0`: helper log `containBlit=(0,0 3824x3904)`; wall/floor shimmer better or not.
4. Lights: `r_dopixelvisibility 0`, `r_pixelvisibility_partial 0`, `OffCenterEyes=false`; say which part flickers.
5. Walls: `OcclusionTrackerPerEye=false`; lean vs stick; one eye or both.
6. `dxvk.conf` AF / LOD bias result.
