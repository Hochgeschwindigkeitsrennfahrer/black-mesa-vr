# VR-specific implications (frame pacing, latency, stutter)

Black Mesa VR is not a flat-screen DXVK install. Desktop Present and OpenVR Submit are **two clocks**. Tuning DXVK as if the monitor were the HMD will fight SteamVR.

Evidence: **Confirmed from L4D2VR** (README: pacer complements compositor), **Confirmed from source** (FramePacer hooked in `d3d9_swapchain.cpp`; VR Submit in `d3d9_device.cpp` / `src/vr.cpp`), **Confirmed from current Black Mesa project** (WaitGetPoses, skip flags, fused stereo). Runtime A/B of these modes on BMVR: **Unknown** (this pass did not change conf or run the game).

---

## Two timing domains

| Domain | Who waits | What DXVK settings affect |
| --- | --- | --- |
| **Desktop D3D9 Present** | FramePacer, `latencySleep`, vsync/`presentInterval`, `maxFrameRate` | Yes |
| **OpenVR WaitGetPoses + Submit** | SteamVR compositor, reprojection, `TransferSurface` / optional `WaitDeviceIdle` | Only indirectly (GPU busy, skipped draws, extra CPU sleep before Present) |

L4D2VR README (Confirmed from L4D2VR):

> The frame pacer controls DXVK's D3D9 CPU/GPU and desktop-present scheduling. It complements, but does not replace, SteamVR compositor timing or reprojection.

BMVR already had Present-path stalls from `WaitGetPoses` when Submit fell behind (Reset loops, DoNotHaveFocus). Adding DXVK sleep on the same thread makes that worse. Evidence: **Confirmed from current Black Mesa project** (`docs/RUNTIME.md`).

Motion-to-photon for the HMD is dominated by: pose wait → render both eyes → TransferSurface → Submit → compositor / reprojection. Shaving D3D9 Present latency with `min-latency` (GPU stall between frames) can **increase** eye-texture age at Submit.

---

## Shader compilation vs missed HMD frames

| Mechanism | Hitch type | VR symptom |
| --- | --- | --- |
| Sync pipeline compile | 5–50+ ms on game thread | Reprojection, judder, pose overshoot |
| Async skip-draw | 0 ms hitch, missing draw | Holes, flicker, flashlight pop-in, one-eye garbage if only one view compiled |
| GPL library compile at load | Long CPU spike in menu | Slow loads; 32-bit VA; less in-level hitch if it works |
| State cache hit | Near-zero after first run | Stable revisits of `bm_c0a0a` etc. |

Stereo doubles PSO variety (two views, different origin/projection). Async holes can be **asymmetric** (left compiled, right skipped) → fuse break. **I** — worth watching on first map enter and first flashlight.

`m_rtBindingFrameCount` gate: first frames on new eye RTs stay **sync** (safer image, possible hitch). After 5–6 frames, new shaders on those RTs become skip-draw. Stereo RT create is exactly this pattern.

---

## Frame-pacer modes vs VR

### `max-frame-latency` (GPLALL 2.6.8-2 default; brief's quoted line)

- Lets the classic max-frames-in-flight / DXGI-like latency bound work.
- **Higher** render latency than low-latency in GPU-limit, **smoother** if GPU time is stable (`dxvk.conf` comments, **S**).
- 2.6.8-2 ctor comment: better when CPU frametimes are inconsistent; does not sacrifice FPS/energy; **does not enable timestamp query path**.
- **Does not** use `lowLatencyOffset` / overlap flags.

**VR fit:** Best **first** test. SteamVR already reprojects; consistency > shaving 4 ms of D3D Present. Avoids Intel `timestampValidBits` crash. Source + `mat_queue_mode` already burst CPU.

### `low-latency` (L4D2VR 2.6.1 implicit default)

- Sleeps/starts frames from GPU finish + offset; flush heuristic more aggressive (`m_minPendingSubmissions = 1`).
- Lower input lag on a **monitor**; more frame-to-frame CPU jitter (author: may look less smooth).
- Needs 64-bit timestamps on 2.6.8+ (query pools). Intel iGPU/Arc A / many mobile: **random crash** (GPLALL 2.6.7-1, **R**).
- Optimized for VRR / IMMEDIATE / FIFO **display** present — not OpenVR.

**VR fit:** Second test, NVIDIA/AMD dGPU only. Watch pose-wait overshoot counters already logged by BMVR Present diagnostics. If WaitGetPoses overshoots rise, abandon this mode.

### `min-latency`

- Stalls GPU between frames; lowest (or not) latency; less FPS in GPU-limit.
- GPLALL: "generally not recommended" except CPU-limit insight.

**VR fit:** Do not ship. GPU idle between frames is the opposite of feeding a 90/120 Hz compositor.

### `low-latency-vrr-*` (2.6.8-2 only)

Tied to monitor refresh. **Irrelevant / harmful** for VR (desktop window is not the HMD). Do not set.

---

## `dxvk.latencySleep`

Built-in Reflex-style sleep **inside Present**, assumes input sampling after Present.

BMVR: input is OpenVR poses **before/during** the frame, not after D3D Present. Sleeping in Present delays the next WaitGetPoses.

**Keep False.** NV_low_latency2 already Auto-off on 32-bit.

---

## `lowLatencyOffset` / `lowLatencyAllowCpuFramesOverlap`

Only for low-latency* modes.

Overlap True (default) matches Source queued rendering better. False serializes CPU frames — may reduce FPS and increase time-to-Submit.

Offset: leave 0. Do not tune from theory.

---

## CPU/GPU overlap vs our Present hook

BMVR Present already:

- Optional MinHook `Present` → `CaptureFrameBeforePresent`
- DXVK Present: swapchain + VR lock/barrier + `VR::Update`
- `TransferSurface(..., waitGpu)` gated by flags
- Pose waiter thread (headset-off freeze fix)

FramePacer `startFrame` wait **before** the next CPU frame can block the same thread that must Submit. `max-frame-latency` with `m_waitIsActive = false` on 2.6.8-2 is the least extra wait. Our 2.6.1 `MAX_FRAME_LATENCY` mode is a simpler `FramePacerMode` without query pools.

---

## Reprojection and stutter

- Async skip-draw: compositor still gets a frame (previous eye texture if we don't overwrite, or a partial new one). Prefer **stable old frame + reprojection** over a hitch that misses vsync entirely — **unless** partial new frames break stereo fuse. Test both.
- State cache: biggest real VR win after the first session (maps, flashlight, HDR).
- Do not enable Steam "Vulkan" / `-vulkan`; that bypasses this DLL.

---

## Recommended test matrix (no implementation in this research pass)

Record in `bms_d3d9.log`: `Frame pace:`, `Using N compiler threads`, `Found config file:`, `Effective configuration:`.

Record in `bmvr_log.txt`: submit errors, poseWaitOvershoot, frameIntervalUsMax, compiler-related holes (visual).

| # | Conf | What you are testing |
| --- | --- | --- |
| 0 | Current DLL, **no new dxvk keys** (only existing D3D9 two-liner) | Baseline: Async+cache+GPL off+**implicit low-latency** already on |
| 1 | Add keys + `framePace = "max-frame-latency"` | Smoothness vs baseline; Intel safety |
| 2 | `framePace = "low-latency"` | Motion-to-photon / pose overshoot vs #1 (dGPU only) |
| 3 | `enableAsync = False` | If #0/#1 show flicker/holes |
| 4 | `DXVK_HUD=compiler` one session | Whether compiler stays busy in-level |

Do not change VR render hooks to "make pacing work." If a mode is bad, revert conf only.

---

## What success looks like

- Headset frame interval stable near HMD Hz (reprojection rate not climbing)
- First-time map: acceptable hitch **or** brief holes, not both severe
- Second launch of same map: compiler HUD quiet, no hole storm
- No new Reset loops / device-lost
- Stereo still fused (1584×1440 path or whatever is currently verified)
- `WaitGetPoses` overshoot not worse than baseline
