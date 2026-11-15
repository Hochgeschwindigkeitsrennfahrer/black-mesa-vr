# Compositor / present stalls (2026-08-19)

## Symptom

Intermittent or **constant** severe HMD stutter (~10–15 fps perceived) while the **desktop FPS counter stays high**. Removing the headset clears stalls in some sessions.

## Log evidence

From `bmvr_log.txt` during glock / combat sessions:

- `present tick ~11–14fps` coincides with repeated `Pose waiter WaitGetPoses err=0 dt=110ms`
- Crowbar swings at ~115 fps did not always trigger the same pattern
- No DXVK shader-compile line correlated with the stall window

## Root cause (2026-08-19 regression)

**ApplicationPerformsPostPresentHandoff** requires `PostPresentHandoff()` every frame. A prior mitigation gated handoff on `poseAge ≤ 35 ms` and unchanged compositor frame index. The pose waiter often delivers poses older than 35 ms, so handoff was **skipped almost every frame** → compositor starvation → constant HMD stutter with fine desktop FPS.

## Fix (current default)

| Setting | Default | Effect |
|---|---|---|
| `CompositorPostPresentHandoff=false` | **false** | `VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff` — runtime handles handoff |
| App `PostPresentHandoff()` | Skipped | `TryCompositorPostPresentHandoff` is a no-op unless config forces app mode |

Set `CompositorPostPresentHandoff=true` only to A/B test L4D2VR-style app handoff (unconditional every frame, no poseAge gating).

## Instrumentation retained

- `Present stall interval=…` when Present gap ≥ 50 ms (rate-limited)
- Slow handoff log when app mode enabled and `dt ≥ 50 ms`

## What this is not

- Not fixed by hiding weapons or disabling VR input
- Not proven as DXVK pipeline compile (no log correlation yet)
- Not solved by skipping `SendWeaponAnim`

## 2026-09-25 — smooth again after crowbar / explosion shear

Submit, `WaitGetPoses`, and `PostPresentHandoff` were already the same as main. The headset still felt terrible while the desktop counter stayed near 200 fps, and a crowbar hit or explosion sheared the view.

`bmvr_log.txt` on the hit frame:

- `MC QueueMcEyeBind: call queue missing slot=185 — single-thread stereo publish`
- then a run of `HMD BB blit 0,0 2560x1440 -> eye 3168x3104`
- then `Allow aux RenderView after stereo 2560x1440` with a punched pitch

`QueueMcMatFunctor` treated one null `GetCallQueue` as permanent (`m_McCallQueueDead`). `McPipelineEnabled()` then stayed false, so Present stopped publishing the eye textures and every later frame stretched the 16:9 window into the HMD. The punch camera was also allowed through `LooksLikeAuxSceneView` because the origin moved just past 25 units.

What restored mainline pacing:

- A missing call queue does not latch the dead flag. The cached slot is probed again, and the other known slot is tried. The dead flag is set only after the queue stays missing (30 failures).
- A window-sized view whose origin is still within 96 units of the player is the punched player camera. It is skipped after the stereo pair instead of being drawn into the eye.

Do not latch `m_McCallQueueDead` on a single null queue. That is the regression that makes the desktop counter stay high while the headset swims.

Do not adopt call-queue slot 146 when slot 185 misses. Slot 146 can return a pointer whose `QueueFunctorInternal` throws. Each throw took the single-thread path and stretched the 16:9 window into the eyes (`HMD BB blit 2560x1440 -> eye 3168x3104`). Keep slot 185. A failed eye-queue bind must not run that blit.

## Next verification

1. HMD smoothness after runtime timing mode (default)
2. If stalls return, grep log for `WaitGetPoses dt=110` vs `Submit()` blocking
3. App handoff A/B: `CompositorPostPresentHandoff=true` in config next to `d3d9.dll`
