# OpenXR eye publish pipeline (2026-08-31)

How a rendered stereo pair gets from the game's D3D9 eye RTs into the helper's
Vulkan swapchain, and the five defects found while chasing "ghost frames when I
move my head, perfect when I hold still".

## Pipeline

Game (`d3d9.dll`, 32-bit, ~200 fps) and helper (`OpenXRHelper64.exe`, 90 Hz)
are separate processes sharing a bridge struct in shared memory.

1. `RenderView` draws both eyes into the engine eye RTs.
2. `PrepareOpenXrEyeSurfacesForRead` StretchRects both eyes into a free publish
   slot `k`, `TransferSurface`s them, then either
   - (`OpenXrDeferredPublish=true`) issues a `D3DQUERYTYPE_EVENT` behind the
     copies, `FlushCommands()` so the CS thread submits them, and queues
     `{slot, pose, panel2d}` as pending; or
   - (`false`, default after 2026-09-06 HMD runs) `WaitDeviceIdle` and
     publishes in the same Present. WMR motion smoothing needs this cadence.
3. `PollOpenXrDeferredPublish` (start and end of every `SubmitVRTextures`)
   tests the pending slots' events without `D3DGETDATA_FLUSH`; the newest
   completed one is published via `PublishOpenXrPair`, older completed ones are
   dropped as stale. `PublishOpenXrResolvedEyeTextures` writes both eye
   descriptors plus a frame id into the bridge.
4. Helper reads the descriptors, imports/reuses the Vulkan images, blits each
   eye into its swapchain, `xrEndFrame`.

## The symptom and what it actually was

Ghosting only during head rotation, clean when still, "dominantly left eye".
That signature is a **binocular** defect, not a latency one: latency alone
produces uniform lag, whereas a left/right pair drawn from *different game
frames* produces horizontal disparity error that your brain resolves as a
double image, and only while the image is changing (i.e. while the head moves).
In-game stick turning did not reproduce it as strongly, which is consistent -
VOR keeps your gaze locked during head motion, so disparity error is far more
visible than during a stick turn.

Originally the helper blitted straight from the live engine eye RTs. The game
overwrote those 2-3 times per compositor frame, and the helper's left blit and
right blit happen at different moments, so the eyes could legitimately land on
different game frames.

## Defects fixed

### 1. Descriptor snapshot taken before `xrWaitFrame`

`ImportSharedGameTexturesIfNeeded()` ran before `xrWaitFrame`, so the eye
descriptors could be a full display frame (~11 ms) old. Harmless while there
was one texture per eye (a stale descriptor still pointed at the continuously
updated image), but **once publish textures rotate, a stale descriptor pins
both blits to an already-superseded slot**. That is a hard 11-19 ms of extra
staleness and is what made the image drag behind the head. Now re-imported
immediately before the `RenderEye` loop. Safe: `RenderEye` defaults to
`waitForQueueIdle = true`, so the queue is idle and `vkUpdateDescriptorSets`
cannot touch an in-flight descriptor set.

### 2. Non-atomic left/right descriptor publish

The two eyes were published with separate `L4D2VR_PublishOpenXrSharedTexture`
calls, and the helper read them from shared memory with no lock (the game-side
`std::mutex` is process-local and buys nothing cross-process). The helper could
read left from slot `k` and right from slot `k-1` - **each eye a different game
frame**, exactly the double image above. Replaced with
`L4D2VR_PublishOpenXrSharedTexturePair` using the odd/even seqlock convention
already used by `gameRenderPoseGeneration`, plus release/acquire fences, and a
retrying `BridgeWriter::ReadSharedTexturePair` on the helper side.

### 3. `GetTickCount()` used for a millisecond rate cap

The publish gate was `(now - last) < 8` on `GetTickCount()`, whose resolution is
~15.6 ms. The intended ~125/s cap actually produced **~64/s against a 90 Hz
compositor**, starving roughly every third display frame - the helper then had
no new pair, reused its previous swapchain content, and SteamVR reprojected.
Now QPC with a 7 ms interval (~143/s), so every 11.1 ms compositor frame sees a
fresh pair with margin.

### 4. Log write per published frame

The helper's "Shared game eye textures ready" line was gated on
`sharedTextureGeneration` changing. With rotating handles that changes every
publish, so the helper wrote a log line to disk **inside the frame loop at the
publish rate**. Now gated on the eye layout (width/height/format).

### 5. Submitted vertical FOV did not match the rendered frustum

`PublishOpenXrEyeTexture` carried `renderAspect = m_Aspect` latched at texture
creation. Logged 1.0972, while `NormalizeViewSetupForVREye` renders each eye at
the eye RT's own aspect:

```
view.m_flAspectRatio = (float)eyeWidth / (float)eyeHeight;   // hooks.cpp
view.fov             = vr->m_Fov;
```

which for a 3664x3584 eye is 1.0223 (game log: `fov=106.3->98.5
aspect=1.778->1.022`). The helper derives the submitted vertical FOV from
`renderFovXDeg` + `renderAspect`, so it submitted +/-0.8135 rad where the image
was actually rendered at +/-0.8482 rad - a frustum ~7% short vertically, so the
compositor reprojected through the wrong projection. `renderAspect` is now
derived from the dimensions of the image actually being submitted.

### 6. Publish textures were never shareable, so none of the above ran

The first test of all the above changed nothing, because the rotating publish
path had been silently inactive the whole time. Game log:

```
OpenXR publish rate present=111/s published=108/s skipStale=0/s skipRate=3/s slots=3 copy=0
OpenXR publish RT GetVRDesc failed eye=0 slot=0
```

`copy=0` every line, and the helper only ever saw `gen=2` - one descriptor, never
rotating. `EnsureOpenXrPublishTextures` created its textures with a plain
`device->CreateTexture(...)`, but the DXVK fork only requests an exportable
shared handle when the `m_CreatingTextureID` sentinel is set to an ID that
`ShouldExportOpenXrEyeTexture` accepts:

```cpp
// third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_device.cpp
if (pSharedHandle == nullptr && Pool == D3DPOOL_DEFAULT && g_Game && g_Game->m_VR) {
  const VR::TextureID texID = g_Game->m_VR->m_CreatingTextureID;
  if (g_Game->m_VR->ShouldExportOpenXrEyeTexture(texID, submitSampleCount))
    effectiveSharedHandle = &openXrSharedHandle;
}
```

Without the sentinel the texture is unshared, `GetVRDesc` returns no handle,
`EnsureOpenXrPublishTextures` bails, and `PrepareOpenXrEyeSurfacesForRead` falls
back to publishing the live engine eye RT - the exact race the slots exist to
remove. Fixed with a dedicated `Texture_OpenXrPublish` ID.

That ID is deliberately absent from the post-create `if (texID == ...)` chain in
`d3d9_device.cpp`. Reusing `Texture_LeftEye` would have overwritten
`m_D9LeftEyeSurface` with a publish copy and applied the eye MSAA sample count.

The failure log was also unthrottled, so it wrote to disk at the publish rate
(the game log reached 65 MB). Any per-submit failure log must be rate limited.

### 7. `WaitDeviceIdle` on every publish (performance, 2026-09-06)

Step 2 ended with `IDirect3DVR9::WaitDeviceIdle()`: `Flush()`,
`SynchronizeCsThread(SynchronizeAll)`, `vkDeviceWaitIdle`. At ~143 publishes/s
that drained the whole DXVK CS queue and stalled the render thread until the
GPU had finished *everything* recorded so far (the entire stereo frame plus the
two eye copies), roughly once per Present. The copies only need to be complete
before the *descriptor* goes out, not before the game continues recording.

When `OpenXrDeferredPublish=true` (opt-in A/B, not the default):

- after the two `StretchRect`s and `TransferSurface`s, `IDirect3DQuery9`
  (`D3DQUERYTYPE_EVENT`, one per slot, created lazily) is `Issue(D3DISSUE_END)`
  and `IDirect3DVR9::FlushCommands()` (new fork method: `D3D9DeviceEx::Flush()`
  under the exclusive device lock, no CS sync) makes the CS thread submit the
  command buffer this frame;
- the pair is queued in `m_OpenXrPending` (capacity `kOpenXrPublishSlots - 1`,
  in-flight count capped by `OpenXrMaxPending`, default 1 — see §7a);
- `PollOpenXrDeferredPublish` calls `GetData(nullptr, 0, 0)` — no
  `D3DGETDATA_FLUSH`, so DXVK's `ConsiderFlush(ImplicitSynchronization)`
  heuristic never fires from here. `D3D9Query::GetQueryData` returns `S_FALSE`
  until the CS thread has processed the `End` (`m_resetCtr`) *and*
  `vkGetEventStatus` reports the event set, which orders after the copies and
  layout barriers on the same queue;
- the pose captured when the eyes were copied travels with the pending entry
  (`OpenXrPendingPublish::pose`), so a slot published one Present later is
  still tagged with the pose it was rendered with, not the live one;
- `PickFreeOpenXrPublishSlot` never picks the helper's visible slot, a pending
  slot, or a slot freed less than `OpenXrSlotCoolingMs` (4 ms) ago (the helper
  may still be blitting from it). If no slot qualifies the eyes stay flagged
  new and the next Present retries (`throttled=` in the rate log).

Fallbacks: no event query support -> synchronous publish of that slot; no
publish ring -> live eye RTs + `WaitDeviceIdle` as before. `Reset`/release
paths call `ResetOpenXrDeferredPublish` (pending copies are abandoned).

Rate log gained `deferred=N/s throttled=N/s dropped=N/s pending=a/b
maxPending=c`. Expect `dropped` near 0 and `throttled` low; a high `throttled`
means the GPU is more than one frame behind the copy cadence.

### 7a. First HMD run: tearing while turning, judder while walking (2026-09-06)

User report on the build above (WMR / HP G2 90 Hz, RTX 4070 Ti, eyes
3168x3104, `OpenXrDeferredPublish=true`): overall faster than `false`, but
tearing during head turns and a picture that "cannot keep up" in low-fps areas;
with `false` reprojection had hidden the drops. Game log (`bmvr_log.txt`) for
that run, every 5 s interval:

```
present=74-84/s published=67-72/s throttled=1-10/s dropped=1-3/s pending=0-1/2 maxPending=2
```

Two defects, both consequences of removing the `WaitDeviceIdle`:

1. **Unbounded GPU run-ahead.** `maxPending=2` in every interval: the render
   thread was two full frames ahead of the GPU. The pose paired with a slot is
   the render pose, so by the time the helper read it the pose was two GPU
   frames (~25-30 ms at ~75 fps) old, and the runtime's reprojection had that
   much more rotation to correct and no way to correct the position at all
   (no depth layer) — that is the walking judder. `dropped` are whole rendered
   frames thrown away because two copies completed between Presents.
   `WaitDeviceIdle` had forced zero run-ahead, which is why `false` felt
   smoother while being slower.

   Fix: `OpenXrMaxPending` (default 1, range 1-3). Before copying new eyes,
   Present blocks in `WaitOldestOpenXrPending` until the *previous* copy's
   event has signalled (`GetData(nullptr,0,0)` + `SwitchToThread`, 250 ms
   timeout then `WaitDeviceIdle`). The CPU still overlaps one frame with the
   GPU (the whole win of §7), the pose is at most one GPU frame old, and no
   completed frame is dropped. Rate log: `paceWait=N/s avg=ms max=ms`.

2. **Slot reuse gated by a timer.** `PickFreeOpenXrPublishSlot` treated a slot
   as writable 4 ms after the game published a newer one. But the helper's
   blit out of that slot runs on a second `VkQueue` in another process, and
   when the GPU is saturated by the game it executes *behind* the game's
   queued command buffers — 10-25 ms later, well after the game has begun the
   next `StretchRect` into the same slot. The helper's swapchain image then
   mixes two game frames: tearing that only shows when consecutive frames
   differ (head turning) and only when the GPU is behind (low-fps areas).
   Exactly the reported pattern.

   Fix: bridge protocol v14 adds `helperConsumingFrameId` /
   `helperConsumedFrameId` / `helperConsumedCount`. The helper writes the frame
   id before its two `RenderEye` blits and again after them (both end in
   `vkQueueWaitIdle`, so the pixels are in the swapchain). The game
   (`RefreshOpenXrHelperConsumed`, once per copy attempt) refuses to write a
   slot whose recorded frame id (`m_OpenXrSlotFrameId[slot]`) is greater than
   `helperConsumedFrameId`. The helper reads the frame id *before* it re-reads
   the descriptor pair, and the game publishes pair-then-id, so the reported
   id is <= the pair it actually blitted — the gate is conservative. The 4 ms
   cooling stays as a margin. If the helper stops consuming for > 500 ms
   (`helperFeedback=0` in the rate log) the gate is dropped so the latest
   descriptor keeps refreshing for when it resumes. `helperHold=N/s` counts
   copies skipped only because of this gate.

   Slots 3 -> 4: visible + helper-blitting (differ when the helper lags a
   frame) + one pending + one to write. ~79 MB per slot at this eye size.

3. **Helper queue priority.** The helper's device now requests
   `VK_KHR_global_priority` / `VK_EXT_global_priority` `HIGH` on its graphics
   queue (falls back to default on `VK_ERROR_NOT_PERMITTED`; log line
   `Vulkan graphics queue global priority: HIGH`). This is what compositors do
   so their per-frame blit is not scheduled behind a saturated game queue; it
   shortens the helper's `vkQueueWaitIdle` and therefore how long a slot is
   held, and helps xrEndFrame land on time.

Status: implemented, compiled, installed; **not HMD-verified**. Check the rate
line for `maxPending=1`, `dropped=0`, `helperFeedback=1`, `consumedLag` 0-2,
`helperHold` low, `paceWait` avg well under a frame; check the helper log for
the priority line; and in the HMD: no tearing while turning in a low-fps area,
walking judder back to the `false` level or better.

### 7b. HMD run 2: still super stuttery, frames better (2026-09-06)

Same session (`bm_c4a1b1`, WMR 90 Hz). Rate line after §7a:

```
present=54-85/s published=54-70/s helperHold=0-8/s consumedLag=4-8
paceWait=0-5/s avg=1-3ms max=5ms helperFeedback=1
```

Helper log: `Vulkan graphics queue global priority: default (HIGH rejected)`
and compositor submits dropped from ~90/s in the menu to ~60/s in-world.

Cause: `RenderEye` reused one command buffer, so it `vkQueueWaitIdle`'d
**twice per display frame before `xrEndFrame`**. When the GPU was busy with
the game those waits made EndFrame miss vsync. WMR then could not
reproject, which is the "super stuttery in the headset despite better
frames" report. Game-side `WaitOldestOpenXrPending` also stalled Present
with `SwitchToThread` (paceWait spikes).

Fix:

- Helper: three command buffers (L/R/overlay) and a fence. Eye and overlay
  blits submit without waiting. `xrEndFrame` runs immediately. The fence is
  polled with timeout 0 after the next `xrBeginFrame`; if it is still busy
  the helper re-submits last swapchain this vsync (blitSkip++) and blits
  the latest game frame on a later vsync. `helperConsumedFrameId` is written
  when the fence signals, not after WaitIdle.
- Game: never block Present on the copy event. If a copy is already in
  flight, skip this Present (`throttled=`). `OpenXrLastPublishMs` is only
  advanced after a successful copy so a helper-hold retries next Present
  instead of waiting 7 ms.

### 7c. HMD run 3: backpressure made it worse (2026-09-06)

Tried cap 11 ms + one-in-flight (`published > consumed`) + 4 ms fence wait
before `xrEndFrame`. Log: `present=61/s published=30/s helperHold=30/s`.
Deferred publish already needs two Presents per pair; the extra hold made
every other Present a no-op, so the HMD got ~half the game's frames.
Reverted those three.

### 7d. Default back to synchronous publish (2026-09-06)

Further HMD runs with `deferred=1` still felt stuttery versus the pre-pass
build. Keeping deferred on and trying to pace the helper (skip-blit, fences,
backpressure) never restored WMR smoothing. The path that hid framedrops
is the one before this pass:

- Game: `OpenXrDeferredPublish=false` — `WaitDeviceIdle` then publish in
  the same Present. CPU/GPU serial, but the compositor sees a regular
  finished pair. `scripts/install.ps1` overwrites an existing `true`.
- Helper: `RenderEye` / overlay blit `vkQueueWaitIdle` again before
  `xrEndFrame`, and `helperConsumedFrameId` is written after those waits
  (not a timeout-0 fence). When the game already drained the GPU, those
  waits are short enough to hit vsync; when it does not, EndFrame is late
  and WMR cannot reproject.

CPU-side caches from the performance pass stay. Slot consumed-id gate and
4 publish slots stay (harmless on the sync path). Deferred remains as an
opt-in (`true` in `VR/config.txt`).

**HMD-verified 2026-09-06:** `false` feels better than `true` even though
`true` reports higher FPS. Extra game frames that miss WMR vsync do not
reproject; motion smoothing needs the slower regular cadence.

## Invariants to preserve

- Both eyes must be copied into the **same** publish slot and published as one
  seqlock'd pair. Never publish eyes independently.
- Import/refresh eye descriptors **after** `xrWaitFrame`, not before.
- Publish rate must stay at or above the display rate; use QPC, never `GetTickCount`.
- A slot is rewritten only when the helper's `helperConsumedFrameId` is >= the
  frame id last published from it (plus the `OpenXrSlotCoolingMs` margin). Do
  not go back to a pure timer: the helper's blit time is unbounded when the GPU
  is saturated by the game (§7a).
- A slot's descriptor must not go out before its event query has signaled.
  Never publish a pending slot from anywhere but `PollOpenXrDeferredPublish`
  / `WaitOldestOpenXrPending`.
- Keep GPU run-ahead bounded (`OpenXrMaxPending`); the published pose must
  not be more than one GPU frame behind the helper's read.

### 8. Multicore Present skip with a dead ICallQueue (Index, 2026-09-09)

`MulticoreMode=true` grows a 2×3 atlas (`MC ring ready`) and, after
pass-through warmup, sets `mat_queue_mode` 2. `McPipelineEnabled()` then made
`SubmitVRTextures` return without copying 1x eyes, expecting
`QueueMcCompositeToRing` → material-thread `CopyEyesToOpenXrPublishSlot`.

Black Mesa's IMatRenderContext is shifted vs Source. Slot 146 can return a
non-null object that is **not** `ICallQueue`; `QueueFunctorInternal` SEH-fails
and is later `no ICallQueue`. Stereo still runs (`mc=1`, `direct=1`, NPCs
audible) but `published=0/s`. The helper keeps the last 2D menu letterbox
(stuck cursor, dark HMD). SteamVR's desktop preview can still flicker the
live game because the HWND Present path is unrelated.

Index 1848×2052 → ring ~3728×6192, which fits typical D3D9 caps so this path
**arms**. A taller HMD (WMR G2 3168×3104 → ring height ~9312) can fail ring
create and stay on the single-thread publish path — that is the headset
split, not an Index compositor quirk.

Fix: do not skip Present copy unless a composite actually queued or finished.
If `QueueFunctorInternal` SEH-fails, forget that slot and copy 1x eyes from
Present. Do not retry the other vtable slot in the same call, and do not
bind/copy on the game thread — that killed `bms.exe` at first stereo
(2026-09-09, exitCode=1). `mat_queue_mode` 2 stays on.

## 9. High FPS, non-native 90 Hz, occasional broken frame (2026-09-20)

Desktop Present and `bmvr_log.txt` `present tick` can sit at 180–320/s while
the helper's `Submitted OpenXR Vulkan frame` timestamps are ~60/s against a
90 Hz G2. That is not "probably reprojection" as a guess: the compositor
client is missing vsync, so the runtime fills the other 30 Hz with ATW /
reprojection. Extra game frames do not help.

Same log interval in-game:

```
present=186-228/s published=186-228/s skipRate=0/s helperFeedback=0 consumedLag=-1
```

Helper gen skips (`sourceGen=22` then `26`) show dropped publishes. Slot reuse
on `OpenXrDeferredPublish=false` ignored `helperConsumedFrameId` and
round-robined 4 slots at ~200/s (~20 ms wrap) while two `vkQueueWaitIdle`s
per eye plus overlay idle held the blit ~16 ms — a slot the compositor was
still reading could be overwritten. That is the occasional broken / old frame.

HL2VR does not do this. `OnFrameBegin` → `Mat_AwaitFrame` (`WaitGetPoses`) →
render with that pose → DXVK submit thread `Submit_TextureWithPose` →
`PostPresentHandoff`. One compositor frame in flight. Next `WaitGetPoses`
waits for that handoff.

OpenXR equivalent that **failed on HMD** (2026-09-20, WMR G2 90 Hz):

1. Game waited up to 16 ms for `helperConsumedFrameId` and 32 ms for a new
   `displayTime` before stereo (`handoffWait` every published frame,
   `await avg=5–15ms max=16–47ms`).
2. Helper polled ≤9 ms after `xrWaitFrame` for a pair tagged with this
   `predictedDisplayTime` (`matchHit=2–6` out of thousands,
   `matchWaitAvg=9.01ms` — the timeout every frame).
3. Same-`displayTime` skip starved the menu (`published=0/s skipVsync=258`
   because the latched panel pose never changes).

`waitFrameAvg` sat at 12.5–15.5 ms, in-game `published≈helperHz≈44–78`.
That is the documented 2026-08-30 failure: waiting after `xrWaitFrame`
makes `xrEndFrame` late, so WMR cannot hold 90 Hz and ATW/reprojects.
Glitched frames did drop (`helperFeedback=1` slot gate). Keep the gate;
do not keep the waits.

Corrected path after HMD run 1 (removed helper 9 ms poll and consume waits).
**HMD run 2 (user, 2026-09-20):** desktop FPS improved; still heavy
reprojection; “not true 90 Hz”. In-game logs: `present=150-250/s`
`await=130-190/s` stereo pairs of 3168x3088, `helperHz` 89 → 45,
`helperHold` 9 → 132, `consumedLag=2-4`. Helper `waitFrameWin` lifetime
avg ended at 14.9 ms (~67 Hz) with `blitSkip`/`fenceBusyFrames` climbing.
Stereo CPU is ~1 ms; the extra pairs saturate the GPU so the helper blit
fence misses vsync and the runtime ATWs.

HL2VR waits `WaitGetPoses` then draws **one** pair with that pose. OpenXR
now:

1. Game `AwaitOpenXrCompositorFrame`: wait ≤22 ms for a new helper
   `displayTime`/`gen` only (no consume wait). No new pose → skip this
   engine frame’s stereo (last swapchain stays). Desktop FPS in a map
   should sit near 90.
2. Helper still EndFrames immediately. Two blit command-buffer/fence sets
   so one in-flight blit does not skip the next vsync.
3. Slot consume-id gate stays.

Expect in-game `await≈90/s`, `helperHz≈90`, helper `waitFrameWin≈11ms`,
`blitSkip` not climbing. Do not claim native 90 Hz until the headset run
shows that. Desktop FPS will look lower in-map; that is the compositor
cadence, not a settings cut.

## 10. Deferred publish again, waiting instead of dropping (2026-10-14)

Public beta 25844216, `bm_c1a0a` tram, WMR G2 90 Hz, RTX 4070 Ti, eyes
3168x3104, multicore. User: "a lot of tearing during movement, like some
frames can't keep up or aren't synchronized with the head".

Sync publish (`OpenXrDeferredPublish=false`): the material thread spun in
`pollEvent` for the GPU after every eye copy.

```
pairs=25-42/s copyWait=25-42/s avg=10.6-14.0ms max=19-212ms (copyTid = presentTid)
pairs=68-89/s on lighter stretches, GPU 14-71%
```

CPU and GPU never overlap, so every frame cost CPU + GPU time and the
compositor repeated frames under a moving world.

Deferred with the §7b throttle rendered at the compositor rate but dropped
every other pair, because the previous copy was still on the GPU when the next
pair was ready:

```
pairs=87-90/s published=41-45/s throttled=44/s pending=1/1
```

Fix: in `CopyEyesToOpenXrPublishSlot`, when `OpenXrMaxPending` copies are
pending, `WaitOldestOpenXrPending` (publish it) before this copy instead of
returning `throttled`. The GPU is already working on the new pair, so the wait
is only the tail of the previous one. Default `OpenXrDeferredPublish` is now
`true`.

```
pairs=82-89/s published=83-89/s throttled=0/s dropped=0/s pending=1/1
paceWait=70-83/s avg=4-8ms helperHz=89-90 waitFrameWin=11.1ms GPU 83-98%
```

Invariants still hold: one frame in flight, descriptor goes out only after its
EVENT signaled, consumed-id slot gate unchanged. Remaining gap to 90 is GPU
time (~98% busy).

### 10a. User session: mostly 90, dips in crowded rooms (2026-10-14)

User played the §10 build on `bm_c1a0a` then `bm_c1a0b`: "really good now",
then "the odd stutter and tearing every now and then, especially the last
2 minutes". Rate lines for those minutes:

```
published=89-94/s helperHz=90 throttled=0-5/s   (open areas)
published=64-73/s pairs=64-70/s helperHz=90     (crowded)
```

`helperHz` never left 90 and `dropped=0` throughout, so the compositor kept
its cadence and the dips are the game producing fewer pairs, not the helper
repeating or mixing frames. `blitSkip` rose 0 → 3 over the whole session
(cumulative), i.e. the blit fence was busy on 3 of ~64,000 frames.

Stereo-cost samples (599 of them; the timer covers only the two RenderViews,
not the queue wait): 117 ≥ 5 ms, 6 ≥ 10 ms, max 30.1 ms. Every slow sample is
the same shape: `shadowPush=76` and `dme` in the hundreds
(`models/humans/scientist.mdl`, `guard.mdl`, `scientist_02.mdl`). `gbDepth`
stays 4, so the deferred path never switched to the cheap one. Two outliers
are not GPU frames: a 4406 ms stall is the `bm_c1a0a` → `bm_c1a0b` load, and a
1016 ms stall has 0.6–5 ms pairs on both sides of it (a scheduling hitch, one
instance all session).

Not changed. The earlier CSM-quality cuts were measured unplayable on this
engine (RUNTIME.md, Surface Tension). `RenderScale=0.9` is the available
headroom knob.

### 10b. Closed session: desktop 150, headset 90 (2026-10-14)

User closed the session and said frames still glitch while the shown FPS is
150. That counter is `present=` (150–180/s on `bm_c1a0b`). The headset stayed
at `helperHz=90` and received `published=82–91/s`. `dropped=0`, `blitSkip`
did not climb, `waitFrameLast` stayed ~11 ms. The 150 is not a headset rate.

The leftover hitch is the submit clock. The eyes are drawn for WaitFrame N's
predicted time, then the helper's next `xrEndFrame` stamps them with WaitFrame
N+1. The runtime treats the old pose as already current and does not timewarp
that ~11 ms (`consumedLag=2`). Fix, not yet headset-checked: when the latched
render pose time is 0–40 ms behind this WaitFrame, `xrEndFrame.displayTime` is
that pose time. Helper log field `poseSkewMs` is the correction. The desktop
counter will still read ~150.

## Status

Defects 1-5 are implemented and confirmed live in the game/helper logs (publish
rate ~110/s, `aspect=1.0143`, `U=0.8526`), and on their own they did **not** fix
the ghosting. Defect 6 is implemented and installed but **not yet
runtime-verified**; until a run shows `copy=1` and a rotating `gen=` in the
helper log, the read/write race is still untested.

Check after the next run:

```
Select-String -Path "...\bin\bmvr_log.txt" -Pattern 'OpenXR publish rate|publish RT'
Select-String -Path "...\openxr_helper64_from_game.log" -Pattern 'Shared game eye textures ready'
```

Expect `copy=1` and `OpenXR publish RTs ready 1136x1120 slots=3`.
