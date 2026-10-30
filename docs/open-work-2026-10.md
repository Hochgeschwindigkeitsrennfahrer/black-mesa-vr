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

## 7. Headset test of ed7c349 (user REPORT.md, 2026-10-30)

Sessions: RenderScale 1.2 (eye 3472×3536) and 1.39 (eye 4016×4096). PSVR2 recommended 2884×2940 after resetting SteamVR resolution.

| Item | Result / analysis |
| --- | --- |
| Helper 1:1 path | Active (swapchain recreated at source size, `fit=fill`). Shimmer still there, maybe slightly better. **Helper resample ruled out** as the shimmer source. |
| FXAA | Toggle visible in the HMD, on is better. User now runs stronger `mat_fxaa_subpixel_Q 1.0`, `mat_fxaa_edge_threshold_Q 0.063`, `mat_fxaa_edge_threshold_min_Q 0.0312` (+ `_C`): somewhat better, not fixed. |
| Load screens | Still clipped (top-left of the logo text, no background). **Not the eye `engine_post` path**: the log shows both load-time calls skipped by the gate (`engine_post not gameplay, skipped ... inGame=0`), and in this setup the src rect already matched (`uv ... -> unchanged`), so the remap never changed anything. Next: check the same load screen on experimental-beta 6cc0009 (pre-branch) to see whether it is a regression at all. |
| Pause menu background on the desktop | Not shown; unknown whether it ever was. Not investigated. |
| Recenter after pause | Works (`Recenter position (pause menu closed)`). |
| MC copy | 28 179 queued, 9 queue fails, 46 Present fallbacks in 27 min: the fallback is negligible. Closed. |
| Light flicker | `r_dopixelvisibility 0` or `r_pixelvisibility_partial 0` each stop it (and remove the glows). **Cause: the glow pixel-visibility queries.** See below. |
| Bloom ghosts `bm_c1a0a` | Not tested yet. |

### Light flicker: per-eye pixel-visibility queries (plan, needs Ghidra)

Source 2013 `pixelvisibility.cpp`: `PixelVisibility_FractionVisible` → `CPixelVisibilitySystem::GetFractionVisible` → `FindQueryForView(set, CurrentViewID())`. Each glow keeps one query per **view id**. Both stereo eyes render as `VIEW_MAIN`, so they share one query per light: each eye re-issues it against its own depth and reads the other eye's result. Partially occluded or far (few pixels) glows then flip every frame — the same class of bug as the occlusion tracker before 73de657.

Fix along the 73de657 pattern: during the right eye's views, offset the view id **only where pixel visibility uses it** (not the global `g_CurrentViewID`, which much else reads). Ghidra work on Steam BM beta 26234270 `client.dll`:

1. `PixelVisibility_EndCurrentView` thunk is known: `client+0xE3B00` (`MOV ECX,g_PixelVisibilitySystem; JMP EndView`), `PixelVisibility_EndScene` `client+0xE3B10` (`research/hl2vr/optimization.md` §14).
2. From `EndView` find `CPixelVisibilitySystem::FindQueryForView(CPixelVisSet*, int viewID)` / `AllocQuery` and the place that reads `CurrentViewID()` for pixel vis (`GetFractionVisible`, `EndView`).
3. Hook whichever function takes the view id as an argument (or the small `CurrentViewID` read inside it) and add the right-eye offset for `VIEW_MAIN`-class ids, as `dOcclusionBeginView` does.

The old `hl2vr_pixelvis` (skip `EndCurrentView` on the second eye) is a different, performance-only change and stays off.

### New bugs (not investigated)

1. Monitor-cluster shadow drawn above the screens (main-menu background map).
2. Left-eye flashlight shadow updates choppily; right eye smooth (MulticoreMode on). Lead: the flashlight shadow-depth render runs once per frame tied to one view; check the order of shadow-depth renders against the two eye passes, and whether our flashlight-state hooks (`dUpdateFlashlightState`, `dBmFlashlightUpdate`) update the pose after the left eye already rendered.

### Eye size cap (done in this pass, not compiled or tested)

`ComputeOffscreenEyeSize` now shrinks both axes by **one factor** when RenderScale asks for more than the cap (aspect kept), and the cap is the tunable `MaxEyeSize` (code default 4096 = unchanged behaviour, 2048..8192, not in the shipped config). Log: `Eye size WxH capped to WxH (MaxEyeSize N...)`.

Limits checked:

- DXVK d3d9 caps: `MaxTextureDimension = 16384` (`third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_caps.h`).
- **MulticoreMode ring = 2×3 eyes** (`MC ring ready 6960x10608 (2x3 +16 gutter)` at eye 3472×3536; `McRingHeight() = 3 * h`). So with MulticoreMode the height is also capped at 5456 (16384 / 3, 16-aligned). This, not the eye RT itself, is the practical ceiling.
- Helper: swapchains follow the source size within the runtime's `maxImageRect`; a larger source keeps the recommended swapchain (fill blit) and logs `stays ... outside ...`.
- Other `4096` checks in `vr.cpp` (`KnownWindowWidth/Height`, HUD overlay) are about the desktop window, not the eye.
- Not verified: Black Mesa's own RT allocation above 4096 (`_rt_FullFrameFB*`, `_rt_gb*`, small FB). Source reads the max texture size from the device caps (16384 here), so no hard 4096 limit is expected, but only a run will tell. The old "near the 2544 first-stereo crash" warning belongs to the retired HMD-framebuffer path.
- VRAM rough estimate at 5456²: ring ≈ 0.7 GB (8-bit) to 1.4 GB (16F), eye RTs + 4 publish slots per eye ≈ 1 GB, helper swapchains ≈ 0.7 GB, G-buffers 1–2.5 GB. Fits 16 GB.

Test: `MaxEyeSize=5120` (or 5456) in `VR/config.txt`, RenderScale e.g. 1.75 (2884×1.75 = 5047, 2940×1.75 = 5145 → about 5024×5120 with the cap at 5120). Check `Eye RT`, `MC ring ready`, `Eye N swapchain ... ->`, and that the world renders.

## 8. Follow-up tests (user REPORT.md, 2026-10-30 afternoon)

- `dxvk.conf` (`d3d9.samplerAnisotropy = 16`, `d3d9.samplerLodBias = 0.5`): **no change**. Texture filtering is not the shimmer source.
- `MaxEyeSize=5456`, RenderScale 1.75: eye 5056×5152, `MC ring ready 10128x15456`, helper swapchains followed, world rendered. Distant and close floor/wall shimmer **unchanged** with ~1.6× the pixels of 1.39, so not plain resolution aliasing. Perf 62–75 fps, ~20 % reprojected. After ~5 min **bms.exe exited with code 1**, no fault entry. Most likely 32-bit address-space exhaustion: bms.exe is 32-bit and DXVK keeps mappings/staging for those RTs in the process; the ring alone is ~0.6 GB at that size. Treat MaxEyeSize above ~4096 with MulticoreMode as unsafe until a `PROTON_LOG=1` run says otherwise. User is back on 1.39.

### Mirrored floors and refract glass are 1024×1024 renders (likely cause of the "resolution-proof" stair-steps)

The log shows `_rt_waterreflection`, `_rt_waterrefraction` and `_rt_poweroftwofb` all at **1024×1024**, while the eye is 3472–5056 px. All three are deliberately excluded from the eye-size grow (`skipGrow` in `dCreateNamedRTEx`).

- **Lobby floor (`floor-and-wall-aa-issues.png`):** the shiny floor is the water-type mirror (bc7cf79). Its reflection of the baseboard and carpet edge is rendered into the 1024² reflection RT and magnified 3–5× → regular stair-steps that shimmer as the reflection view moves. No RenderScale or FXAA change can touch it, which matches the report exactly.
- **Door glass (`door-glass-reflection-wrong-res.jpg`):** wired glass is a refract material sampling `_rt_PowerOfTwoFB`. The eye is copied into the 1024² texture (`CopyRTEx refract stretch drop srcRect 0,0 3472x3536 into dest 1024x1024`), so at best it is a 3.4× blow-up. The visibly wrong *region* (magnified crop) is not explained yet; likely the source RT of that copy is not the plain eye at that moment.
- The "do not grow Water/Refract RTs" rule in RUNTIME.md has no verified failure for these names on this path. The verified failure was growing `_rt_Small*FB*` (zoom/warp). The related real risk is the reflection-view viewport: `ClampStereoViewport` skips square sub-eye viewports so a 1024 reflection view is not expanded and stamped as a view-locked world.

**Confirm first (console, no restart):** in the `bm_c1a0a` lobby, `r_WaterDrawReflection 0`. If the floor stair-steps disappear (the floor stops mirroring), it is the reflection RT. Set it back to `1` afterwards.

**Experiment (done, not compiled or tested):** `ReflectionRtScale` (code default 0 = engine 1024²; 0.25–1 = fraction of the eye size, not in the shipped config). It grows `_rt_Water*`, `*Reflect*` / `*Refract*` and `_rt_PowerOfTwoFB` to that fraction of the eye at creation (restart needed). `_rt_Camera` stays. Log: `CreateNamedRT ... reflection grow ... LITERAL (ReflectionRtScale ...)`. Try `0.5` first: the reflection is a second scene render per eye, so 1.0 costs real GPU time. Check the lobby floor, the door glass, Xen/coast water, and that no view-locked image appears in mirrors or water.

### Still open

- Distant shimmer that is not on mirrors/glass: the user's proposed motionless test (headset on a table facing a shimmering wall) is the right next step. If it still shimmers when nothing moves, something changes per frame in the render.
- Load screens: compare on experimental-beta 6cc0009.
- Light flicker: per-eye pixel-visibility ids (Ghidra), §7.

## 9. Helper box downsample (OpenXRHelperDownsample)

Since bae6915 the helper submits the eye 1:1 at the game eye size (e.g. 4016×4096 at RenderScale 1.39, recommended 2884×2940), and SteamVR's distortion pass does the whole downscale to the panel. If that pass is a single bilinear tap without mips, most supersampled pixels never contribute, which would fit "RenderScale 1.75 barely helps".

`OpenXRHelperDownsample=box` (default `off`, not in the shipped config): the eye swapchain stays at the recommended size and the eye blit uses `shaders/blit_box.frag`. Each output pixel averages `ceil(footprint)²` bilinear taps spread over its source footprint (up to 8×8), in linear light, so every source pixel contributes. The runtime's distortion resample is then close to its normal ratio. Box wins over `OpenXRHelperSwapchainAtSourceSize`. Overlays (menu/HUD) keep the plain blit. Needs the sRGB shader blit path (the PSVR2 log uses it: swapchain format 50).

Logs (helper):

- `OpenXR eye downsample: box (recommended-size swapchain)` / `off`
- `Box downsample blit pipeline=1 flipPipeline=1`
- `Eye N scale path: box downsample source 4016x4096 -> swapchain 2884x2940` (or `1:1 copy` / `single-tap resample`), on every change.

Status: implemented; shader compiled with glslangValidator 15.1 and passes `spirv-val`; C++ not compiled here; not headset-tested.

**Test plan (user):** RenderScale 1.39, two launches, same spots (lobby walls/floor, distant cabinets/wiring):

1. `OpenXRHelperDownsample=off` (current 1:1 path): helper log `Eye 0 scale path: 1:1 copy source 4016x4096 -> swapchain 4016x4096`.
2. `OpenXRHelperDownsample=box`: `Eye 0 scale path: box downsample source 4016x4096 -> swapchain 2884x2940`.

Compare the shimmer and the GPU cost (SteamVR frame timing GPU ms, `OpenXR publish rate` / `paceWait`). The box pass reads ~16 M source pixels per eye in 4 taps per output pixel, so expect a fraction of a millisecond. The runtime then composites a smaller swapchain, which can win some of that back.

## 10. ReflectionRtScale headset result and fix (2026-10-30)

User, build 36a09b7, `ReflectionRtScale=0.5` (reflection RTs 1024² → 2000×2048 at eye 4016×4096): **the lobby floor/wall edge shimmer is gone**, so the 1024² reflection RT was the cause. But the mirror floor showed correct, ~2× magnified content only in the top-left quarter of each eye, black elsewhere (headset video frames).

Log analysis:

- The reflection view runs its own deferred pass: `PushRT null 2000x2048` → `PushRT _rt_gbdepthrt2 2000x2048` → `PushRT _rt_waterreflection`. Its G-buffer is `_rt_gbDepth2`, `_rt_gbDepthRT2`, `_rt_gbNormal2`, `_rt_gbLightDiff2`, all still **1024×1024** (mode 2, same size class as the water RTs). Under a 2000×2048 viewport only their top-left quarter was written.
- In addition, `PushRT null/_rt_gbdepthrt2 viewport 2000x2048 -> eye 4016x4096 (offscreen native)`: the old "square aux" test (`|w-h| < 32`) no longer recognised the 2000×2048 reflection viewport and expanded it to the eye. `ClampStereoViewport` had the same test.

Fix (not compiled or tested):

1. `ReflectionRtScale` also grows `_rt_gb*2` (non-shadow) with the water/refract RTs, so the whole reflection set has one size. The 2026-08-21 failure was these grown to the *eye* while the water RTs stayed at 1024 — the mismatch in the other direction.
2. The grown reflection size is remembered. PushRT and `ClampStereoViewport` leave viewports of that size alone (log: `PushRT ... viewport ... kept (reflection RT size, not the eye)`).
3. `SrcLooksLikePowerOfTwoFb` (underwater overlay) also accepts the grown size.

Refract `CopyRTEx` into `_rt_poweroftwofb`: in the eye pass the copy has no dst rect (whole 2000×2048 dest) and no src rect (whole source), so it already stretches the full eye into the full grown texture. Nothing to change there.

Re-test: lobby floor fills the whole mirror at both 0.5 and 1.0; door glass; Xen/coast water; underwater overlay; no view-locked image in water. Expected log: `CreateNamedRT _rt_gbDepth2 reflection grow 1024x1024 mode=2 -> 2000x2048 ...` (and the other three), `PushRT ... kept (reflection RT size, not the eye)`, and no `PushRT null viewport 2000x2048 -> eye`.
