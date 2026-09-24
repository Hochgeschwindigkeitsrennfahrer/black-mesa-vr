# DXVK configuration for Black Mesa VR

## How config is actually loaded (do not assume L4D2VR's "next to exe" story)

### Load order in our compiled DXVK (`dxvk_instance.cpp`)

1. `getUserConfig()`
2. `merge(getAppConfig(exe))` — `insert` only; **user keys win**

Evidence: **Confirmed from source**.

### User file path

| Situation | File that is read |
| --- | --- |
| Black Mesa launcher, DXVK enabled | **`DXVK_CONFIG_FILE` = `Black Mesa\bin\thirdparty\dxvk-windows-x86\dxvk.conf`** |
| `DXVK_CONFIG_FILE` unset | `dxvk.conf` in cwd (often next to `bms.exe`), else `%APPDATA%\dxvk.conf` |
| `DXVK_CONFIG` env | Extra semicolon-separated overrides after the file |

Evidence: **Confirmed from current Black Mesa project** (`docs/DXVK.md`) + **Confirmed from source**.

**A `dxvk.conf` next to `bms.exe` is ignored while the launcher sets `DXVK_CONFIG_FILE`.** L4D2VR's README advice does not apply until that env var is unset.

### What `install.ps1` does today

It **rewrites** the thirdparty `dxvk.conf` to only:

```ini
d3d9.deferSurfaceCreation = True
d3d9.deviceLossOnFocusLoss = False
```

Stock Steam `dxvk.conf` had `deviceLossOnFocusLoss = True`, which **overrides** the built-in `bms.exe` profile (`False`) and races VR Reset. Evidence: **Confirmed from current Black Mesa project**.

Any Async/framePace work **must** add lines to this same file and **keep** those two D3D9 lines. Steam verify restores stock conf — install must rewrite again.

Built-in `bms.exe` profile (user file still wins):

```text
d3d9.customVendorId = 10de
d3d9.deviceLossOnFocusLoss = False
```

Evidence: **Confirmed from source** (`config.cpp`).

---

## The seven-line block from the research brief

```ini
dxvk.enableAsync = True
dxvk.gplAsyncCache = True
dxvk.latencySleep = False
dxvk.framePace = "max-frame-latency"
dxvk.lowLatencyOffset = 0
dxvk.lowLatencyAllowCpuFramesOverlap = True
dxvk.enableGraphicsPipelineLibrary = False
```

This is **not** a file L4D2VR's git tree ships. It is a reasonable **explicit** conf. `framePace` matches **GPLALL 2.6.8-2's compiled default**, not L4D2VR 2.6.1's implicit **low-latency**.

Below: each line for **L4D2VR 2.6.1+ (what we compile)**, **GPLALL 2.6.8-2**, **Ph42oN v3.0-1**.

---

### `dxvk.enableAsync = True`

| | |
| --- | --- |
| What it does | Allows graphics pipeline creation to be queued on compiler threads. Draw is **skipped** (null pipeline handle) until ready, if the RT-age gate passes. |
| Why L4D2VR | Default True in the GPLALL port; README: disable with `DXVK_ASYNC=0` if missing draws/glitches. |
| GPLALL 2.6.8-2 | Still valid, default True. |
| Ph42oN v3.0-1 | Still valid, default True. |
| Copy to BM? | **Yes, explicit True is useful** so a future tree cannot silently change the default. Behavior is already on. |
| VR value | Keep True. Disable only as a stutter vs corruption A/B. |
| Already default? | Yes on L4D2VR / GPLALL 2.6 / Ph42oN. |
| Explicit useful? | Yes (reproducible logs: "Effective configuration"). |

Env: `DXVK_ASYNC=0/1` on L4D2VR 2.6.1 (**S**). 2.6.8-2 env: **U** (README yes, options.cpp snapshot unclear).

---

### `dxvk.gplAsyncCache = True`

| | |
| --- | --- |
| What it does | State-cache integration for Async/GPL: still **write** pipeline state to the DXVK `.dxvk-cache` even when a pipeline *could* be GPL-fast-linked; Async compiles still populate the cache. Without it, GPL-capable pipelines are omitted from the cache (upstream behavior: cache only what cannot use libraries). |
| Why L4D2VR | Default True; README documents `DXVK_GPLASYNCCACHE=0`. |
| GPLALL 2.6.8-2 | **Still valid**, default True. This is a reason 2.6.x exists. |
| Ph42oN v3.0-1 | **Invalid / ignored.** Removed in Ph42oN v2.7-1. |
| Copy to BM? | **Yes if we stay on 2.6.1+ or merge 2.6.8-2.** **No if anyone foolishly used v3.0-1.** |
| VR value | True. Helps subsequent map loads. First session still compiles. |
| Already default? | Yes on 2.6 GPLALL/L4D2VR. |
| Explicit useful? | Yes, documents intent and survives a merge that might flip defaults. |

Requires `dxvk.enableStateCache` (default True). Cache file: cwd of the process (usually next to `bms.exe`) unless `DXVK_STATE_CACHE_PATH` is set. **U** exact filename for BM until a log is read (`*.dxvk-cache`).

---

### `dxvk.latencySleep = False`

| | |
| --- | --- |
| What it does | Upstream Reflex / built-in latency sleep. **True** sleeps in Present based on GPU timings (assumes CPU samples input *after* Present returns). |
| Why L4D2VR / GPLALL | Forced False so **FramePacer** owns CPU/GPU sync. Reflex + FramePacer are not switchable yet (comment in `dxvk.conf`). |
| GPLALL 2.6.8-2 | Still valid, default False. |
| Ph42oN v3.0-1 | Key exists as **upstream**; default is **Auto**, not False. Setting False is valid but does not enable a FramePacer that is not compiled in. |
| Copy to BM? | **Yes, keep False** on L4D2VR/GPLALL. VR Present already waits on OpenVR; extra sleep increases motion-to-photon and can desync WaitGetPoses. |
| 32-bit | `disableNvLowLatency2` Auto already avoids NV extension on 32-bit (**S** comment: extension broken on 32-bit). |
| Already default? | Yes on our tree. |
| Explicit useful? | Yes — stock upstream is Auto; a mistaken merge could re-enable sleep. |

---

### `dxvk.framePace = "max-frame-latency"`

| | |
| --- | --- |
| What it does | Selects FramePacer mode for **D3D9/DXGI Present** (desktop swapchain), not OpenVR Submit. |
| Modes (L4D2VR 2.6.1) | `max-frame-latency`, `low-latency`, `min-latency` (**S**) |
| Extra (GPLALL 2.6.8-2) | `low-latency-vrr-<hz>` (**S**) |
| L4D2VR why | README says low-latency pacing is enabled by default; you *may* pick the other two. **Compiled default = low-latency** when string empty. |
| GPLALL 2.6.8-2 | **Default is max-frame-latency** (option + ctor). Author: better for inconsistent CPU frametimes; low-latency* needs 64-bit timestamps. |
| Ph42oN v3.0-1 | **Does not exist.** |
| Copy to BM? | **Do not copy blindly as "what L4D2VR ships."** For VR, **start A/B with explicit `max-frame-latency`** (smoother GPU-limit, no timestamp crash, does not sleep against compositor). Then test unset/low-latency. See [`vr-implications.md`](vr-implications.md). |
| Already default? | **No** on current BMVR DLL (implicit low-latency). **Yes** on 2.6.8-2. |
| Explicit useful? | **Yes**, because the two trees disagree. |

Env: `DXVK_FRAME_PACE` overrides conf (checked first in ctor) (**S**).

---

### `dxvk.lowLatencyOffset = 0`

| | |
| --- | --- |
| What it does | Microseconds added to low-latency* start time. Positive = start later (tiny latency tweak). Negative = start earlier (maybe more FPS). Range about −10000..10000. |
| Why L4D2VR | Default 0; env `DXVK_LOW_LATENCY_OFFSET` (**S** `dxvk_framepacer_mode_low_latency.cpp`) |
| GPLALL 2.6.8-2 | Still valid; **no effect in max-frame-latency mode** |
| Ph42oN v3.0-1 | **Does not exist** |
| Copy to BM? | Harmless. Only matters if `framePace` is low-latency*. Keep 0 until a measured test says otherwise. |
| Already default? | Yes |
| Explicit useful? | Low. Include only if shipping a full conf block for documentation. |

---

### `dxvk.lowLatencyAllowCpuFramesOverlap = True`

| | |
| --- | --- |
| What it does | In low-latency* mode, allow CPU to start frame N before CPU of N−1 finished. False = snappier, maybe less FPS. |
| Why L4D2VR | Default True; env `DXVK_LOW_LATENCY_ALLOW_CPU_FRAMES_OVERLAP` |
| GPLALL 2.6.8-2 | Still valid; **no effect in max-frame-latency** |
| Ph42oN v3.0-1 | **Does not exist** |
| Copy to BM? | Keep True if testing low-latency. Queued Source rendering (`mat_queue_mode`) already overlaps CPU; False may fight that. **I** |
| Already default? | Yes |
| Explicit useful? | Only for low-latency A/B. |

---

### `dxvk.enableGraphicsPipelineLibrary = False`

| | |
| --- | --- |
| What it does | `False`: never use `VK_EXT_graphics_pipeline_library`. `Auto`: use if supported + background optimized pipelines. `True`: GPL only, do not compile optimized pipelines. |
| Why L4D2VR / GPLALL | GPLALL disabled GPL by default from **2.6.1-4**: Async does not need the extension, uses less VRAM, and was judged better than GPL or cache-only. `dxvk.conf` comments in our tree say this explicitly (**S**). |
| GPLALL 2.6.8-2 | Still default **False**. Still the correct GPLALL choice. |
| Ph42oN v3.0-1 | Upstream default **Auto** — GPL **on** on modern drivers. Ph42oN *wants* GPL+Async together. Setting False is still a valid upstream key and is the safer VR/VRAM choice. |
| Copy to BM? | **Yes, keep False** on the selected 2.6.x builds. Do not enable GPL "because it is newer." 32-bit address space + SteamVR + eye RTs already stress VRAM/VA. DXVK 3.0.1 documented Intel Windows **hangs with GPL**. |
| Already default? | Yes on our tree. |
| Explicit useful? | **Yes** — prevents Auto if a merge reverts the default. |

---

## Other keys BMVR already depends on

| Key | Required value | Why |
| --- | --- | --- |
| `d3d9.deviceLossOnFocusLoss` | **False** | Stock True + SteamVR focus → Reset storm (**P**) |
| `d3d9.deferSurfaceCreation` | **True** | Current install.ps1; some games need surface at first Present (**P**) |
| `d3d9.customVendorId` | `10de` via built-in profile | Intel flashlight/artifacts (**S**) — user file should not set a different vendor unless testing |

Do **not** set `d3d9.maxFrameRate` / `dxvk.maxFrameRate` to the HMD Hz. SteamVR compositor is the pacing authority; a DXVK cap on desktop Present can stall Submit. **I** — test only if desktop tearing/unlimited Present is proven to steal GPU from the compositor.

Do **not** set `dxvk.latencySleep = True`.

`DXVK_HUD=compiler,fps,frametimes,version` is a **test-only** overlay; do not ship it (desktop HUD, extra work in Present).

---

## Recommended conf blocks

### A. Current tree (L4D2VR 2.6.1+) — Phase 1 test file

Goes in `bin\thirdparty\dxvk-windows-x86\dxvk.conf` (the `DXVK_CONFIG_FILE` target). Preserve install.ps1's D3D9 lines.

**Test 1 (recommended first VR A/B):**

```ini
d3d9.deferSurfaceCreation = True
d3d9.deviceLossOnFocusLoss = False

dxvk.enableAsync = True
dxvk.gplAsyncCache = True
dxvk.enableGraphicsPipelineLibrary = False
dxvk.latencySleep = False
dxvk.framePace = "max-frame-latency"
```

**Test 2 (L4D2VR implicit default):** omit `dxvk.framePace` (or set `"low-latency"`).

**Test 3 (debug stutter vs holes):** `dxvk.enableAsync = False` (keep rest).

**Do not** add `gplAsyncCache` if the DLL log ever shows a 3.x / Ph42oN 2.7+ build.

### B. If Phase 2 merges GPLALL 2.6.8-2

Same as Test 1. `framePace = max-frame-latency` then matches compiled default. Optionally add:

```ini
dxvk.enableDescriptorUpdateTemplates = True
```

(already default True on 2.6.8-2 native Windows.)

Intel iGPU / Arc A: **never** set `low-latency` without a timestampValidBits=64 check.

### C. Ph42oN v3.0-1 (not recommended)

If forced:

```ini
dxvk.enableAsync = True
dxvk.enableGraphicsPipelineLibrary = False
dxvk.latencySleep = False
```

Omit `gplAsyncCache`, `framePace`, `lowLatency*`. Still a bad idea for BM.

---

## Defaults cheat sheet

| Key | L4D2VR 2.6.1+ compiled | GPLALL 2.6.8-2 compiled | Ph42oN v3.0-1 |
| --- | --- | --- | --- |
| enableAsync | True | True | True |
| gplAsyncCache | True | True | **absent** |
| latencySleep | False | False | Auto (upstream) |
| framePace | empty → **low-latency** | **max-frame-latency** | **absent** |
| lowLatencyOffset | 0 | 0 | absent |
| lowLatencyAllowCpuFramesOverlap | True | True | absent |
| enableGraphicsPipelineLibrary | False | False | Auto |
