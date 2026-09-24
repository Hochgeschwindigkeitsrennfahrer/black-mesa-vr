# DXVK-GPLALL 2.6.8-2

Repository: https://github.com/Digger1955/dxvk-gplall  
Also published historically as DXVK-GPLAsync-LowLatency (`Digger1955/dxvk-gplasync-lowlatency` README is the same product family).

## Bottom line for BMVR

This is the **correct future DXVK feature branch** if we ever leave L4D2VR's 2.6.1 snapshot: Vulkan 1.3, 32-bit D3D9, state cache kept, GPLAsync kept, Low Latency kept, 3.x explicitly cancelled after DXVK 3.x failed 32-bit D3D9 titles.

It is **not** a drop-in `d3d9.dll` for Black Mesa VR. Use it only as a **source merge** into `third_party/l4d2vr/dxvk_new` with L4D2VR VR patches reapplied.

Evidence mix: **Confirmed from release/build artifacts**, **Confirmed from source** (fetched `GPLALL-master-2.6.8` files), **Confirmed from L4D2VR** (2.6.1-6 ancestry).

## Release identity

| Field | Value | Evidence |
| --- | --- | --- |
| Tag | `DXVK-GPLALL-2.6.8-2` | GitHub API latest |
| Name | `2.6.8-2` (also titled DXVK-GPLAsync-LowLatency 2.6.8-2) | Release notes |
| Published | 2026-07-28T02:18:46Z | GitHub API |
| Target branch | `GPLALL-master-2.6.8` | GitHub API `target_commitish` |
| Previous | 2.6.8-1 (regression) → 2.6.8-2 hotfix | Release notes |

## Upstream base

Release notes (Confirmed from release/build artifacts):

> Based on **DXVK 2.6.2** and commits from **DXVK 2.7, 2.7.1 and later**, **DXVK GPLAsync 2.6.2** and **DXVK GPLAsync 2.7.1-1 patch**, **DXVK Low Latency 2.7.1 commit `9659672`**.

Naming is frozen at 2.6.x **on purpose** so Vulkan/driver requirements stay those of DXVK 2.6 (Vulkan 1.3), even though the changelog is large enough that the author considered calling it 2.8.0-1.

**Not** DXVK 3.x. The 3.x GPLALL branch was **cancelled**. Reasons given in 2.6.8-2 notes:

- DXVK 3.x performance/stability/rendering issues across OS/GPU/driver
- **Borderlands 2 Remastered (D3D9 x32) failed** on the author's test system with DXVK 3.x with and without GPL
- Vulkan 1.4 requirement called out as inappropriate for 2000s–2010s titles
- Upstream removed `DXVK_FRAME_RATE`, latency tracker, state cache; no present-timing interest

Evidence: **Confirmed from release/build artifacts**. Directly relevant to Black Mesa (32-bit D3D9 Source game).

## What 2.6.8-2 adds on top of L4D2VR's 2.6.1-6 port

L4D2VR stopped at `7927e64` / 2.6.1-6. 2.6.8-2 is many maintenance releases later. Non-exhaustive, from 2.6.8-1 notes + 2.6.8-2 hotfix (Confirmed from release/build artifacts):

### Hotfix (2.6.8-2 only)

- Rendering regression in 2.6.8-1 (issue #105). Not reproduced on the author's machine; identified with JaegerFox / EPOCHvoyager. **Do not use 2.6.8-1.**

### GPLAsync / compile policy

- Switched to a more performant GPLAsync variant (Britt Yazel / Ph42oN MR lineage, backport)
- Raised RT binding frames before Async: `m_rtBindingFrameCount >= 2` → **`>= 6`** in 2.6.8-1 (fewer first-time missing draws; more sync compiles). L4D2VR 2.6.1 tree still uses **`>= 5`** in `dxvk_image.h`. Evidence: **Confirmed from source** (our tree) vs **Confirmed from release/build artifacts** (2.6.8-1 notes)
- State-cache improvements (dedupe depth-stencil; Laitinlok backport)

### D3D9 CPU / GPU (valuable for Source)

- SetRenderState tracking, RT bind rework, vertex buffer bind optimize, StretchRect/MSAA blit rewrite **without** raising `shaderInt64` / keeping GLSL 450
- Pipeline layout refactor: push constants **128 → 64** bytes (lower `maxPushConstantSize` than upstream DXVK that later raised it to 256)
- Limit compiler threads on 32-bit; GPLALL sets **12** (upstream 2.6.8-era note said 8). Our L4D2VR tree still caps 32-bit workers at **16** in `dxvk_pipemanager.cpp`. Evidence: **Confirmed from source** vs release notes

### Low Latency / pacing (API changed)

Fetched `GPLALL-master-2.6.8` `dxvk_framepacer.cpp` (Confirmed from source):

- **Default mode is `MAX_FRAME_LATENCY`**, not low-latency. Comment: GPLALL sleep/limiter improvements; better for inconsistent CPU frametimes; does not sacrifice performance.
- New mode **`low-latency-vrr-<refresh>`**
- `FramePacer` constructor now takes `DxvkDevice*`
- Creates **256 timestamp query pools** (`VK_QUERY_TYPE_TIMESTAMP`) — this is why GPUs with `timestampValidBits != 64` (Intel iGPU / Arc A, some mobile) **crash** in `low-latency` / `low-latency-vrr-*`. Workaround: `max-frame-latency` or `min-latency`. Cannot implement a runtime check on the 2.6.x codebase (author's note on 2.6.7-1).
- `dxvk.framePace` option default string: `"max-frame-latency"` (`dxvk_options.cpp` / `.h`)

L4D2VR 2.6.1 constructor still defaults to **LOW_LATENCY** when `framePace` is empty. Evidence: **Confirmed from source** (both trees).

### Other

- `dxvk.enableDescriptorUpdateTemplates` **default True** (CPU win on native x86; **disable** on VirGL/Venus, MoltenVK, FEX/Box64/Winlator)
- Reverted some upstream FPS-limiter deadline heuristics that hurt `dxvk.framePace`
- Default FPS limiter special-case `-1` in some GPLALL notes (always off) — verify in source before copying limiter conf. **Unknown** exact 2.6.8-2 limiter default vs our tree without reading `util_fps_limiter.cpp` on that branch.

## GPLAsync / state cache (still present)

2.6.x GPLALL **keeps** upstream DXVK state cache and Ph42oN `gplAsyncCache`. README at `7927e64` and current project README (Confirmed from release/build artifacts / fetched README):

- `dxvk.enableAsync` default True; `DXVK_ASYNC=0/1`
- `dxvk.gplAsyncCache` default True; `DXVK_GPLASYNCCACHE=0/1`
- `dxvk.enableGraphicsPipelineLibrary` default **False** since 2.6.1-4 (VRAM; Async is the stutter path)
- State cache env: `DXVK_STATE_CACHE=disable|reset`, `DXVK_STATE_CACHE_PATH`

Fetched 2.6.8 `dxvk_options.cpp` snapshot showed `enableAsync` from config only (no `DXVK_ASYNC` in that file). README still documents the env var. **Unknown** whether 2.6.8-2 dropped `DXVK_ASYNC` in code; do not rely on the env var after a merge without re-reading `dxvk_options.cpp`. L4D2VR 2.6.1 **does** honor `DXVK_ASYNC`. Evidence: **Confirmed from source** (our tree) vs **Unknown** (2.6.8-2 env).

## Low Latency options (2.6.8-2)

| Option | 2.6.8-2 default | Notes |
| --- | --- | --- |
| `dxvk.framePace` | `"max-frame-latency"` | Also `low-latency`, `min-latency`, `low-latency-vrr-<n>` |
| `DXVK_FRAME_PACE` | same strings | Env wins in framepacer ctor |
| `dxvk.latencySleep` | False | Must stay False so FramePacer owns pacing (Reflex/NV_low_latency2 fights it) |
| `dxvk.lowLatencyOffset` | 0 | Only meaningful for low-latency* modes, µs, −10000..10000 |
| `dxvk.lowLatencyAllowCpuFramesOverlap` | True | Only low-latency* |
| `dxvk.disableNvLowLatency2` | Auto | Auto disables on **32-bit** (driver bugs) |

Evidence: **Confirmed from source** (fetched options + framepacer) + **Confirmed from release/build artifacts**.

## Async implementation (same idea as L4D2VR / Ph42oN)

See [`gpl-async-architecture.md`](gpl-async-architecture.md). Short version:

- Draw path asks `getPipelineHandle(state, asyncCompat)`
- If async: queue High-priority compile, return **null handle**, **skip the draw**
- RT must have been bound for N consecutive frames (`N=5` in L4D2VR, `N=6` in 2.6.8-1 notes) so loading-screen first compiles stay synchronous
- `gplAsyncCache` still writes state-cache entries even for pipelines that could GPL-fast-link, so later runs precompile

## Vulkan / GPU / 32-bit

| Requirement | 2.6.8-2 | Evidence |
| --- | --- | --- |
| Vulkan | **1.3** (not 1.4) | Release notes + 2.6.x naming freeze |
| CPU ISA (prebuilt) | SSE2 baseline; SSE4.2 and AVX2 variants | Asset names |
| 32-bit Windows D3D9 | **Yes** — this branch exists specifically for that class of game | Roadmap + BL2 x32 comment on 3.x |
| `timestampValidBits=64` | Required for **low-latency*** modes; not for max-frame-latency | 2.6.7-1 notes |
| RDNA1/2 Windows | Stay on 2.x (upstream 3.0 explicitly tells those users to stay on 2.x) | doitsujin 3.0 notes |

## Windows binary variants (if someone tests non-VR drop-in)

Release assets (Confirmed from release/build artifacts):

| Asset | Use for BMVR? |
| --- | --- |
| `DXVK-GPLALL-GCC-WinMacLinux-SSE2-O3-LTO.2.6.8-2.zip` | **Safest prebuilt** if ever testing stock DXVK without VR. Contains `x32/` + `x64/`. SSE2. |
| `...-SSE4.2-O3-LTO-GENERIC...` | Faster on SSE4.2 CPUs; still has 32-bit |
| `...-SSE4.2-O3-LTO-INTEL...` | Intel-tuned; do not prefer for AMD |
| `DXVK-GPLALL-MSVC-Windows-SSE2-O1_Oi_Ob3-LTCG.2.6.8-2.zip` | MSVC, SSE2, needs VC runtime |
| `...-AVX2-...-AMD64...` | **64-bit AMD only — wrong for Black Mesa** |
| `...-SSE4.2-...-INTEL64...` | **64-bit Intel — wrong for Black Mesa** |
| Native-GPLALL `*.tar.gz` | Linux native, not Windows `d3d9.dll` |
| `DXVK-GPLALL-2.6.8-2_Source.Code.zip` | The merge input for Phase 2 |
| `dxvk.conf` | Comment template |

Black Mesa is **Win32**. Never recommend AVX2/INTEL64/AMD64 packages as the VR DLL.

**For BMVR the prebuilt zip is the wrong product.** Our `d3d9.dll` is MSVC-built from CMake (`CMakeLists.txt` Win32, `/DEF:d3d9.def`). Phase 2 would keep compiling with MSVC, not swapping in GCC `d3d9.dll`.

## Risks if merged into BMVR

| Risk | Severity | Why |
| --- | --- | --- |
| Lose VR patches during merge | **High** | `d3d9_device.cpp` / `d3d9_vr.cpp` / Present are heavily forked |
| Frame pacer API change | **High** | ctor takes `DxvkDevice*`; 256 query pools; new VRR mode |
| D3D9 bind/state tracking rewrite | Medium | Touch every Present/draw; BM stereo already sensitive |
| Descriptor update templates | Medium | Default on; good for Win32, bad if someone tests under emulator |
| Async RT threshold 5→6 | Low | Slightly more first-frame sync stalls, fewer missing draws |
| 2.6.8-1 visual regression | **Must skip 2.6.8-1** | 2.6.8-2 exists because of it |
| Intel + `low-latency` crash | Medium if someone enables it | Use `max-frame-latency` |

## Compatibility with our VR hooks

Transparent? **No.**  
Moderately invasive? **Only if cherry-picking tiny patches.**  
Highly risky? **Yes for a wholesale `dxvk_new` replace.**  
Impossible? **No** — L4D2VR already did this once (2.6.1-6 port). Doing it again is a dedicated merge + stereo re-verify.

Do not merge until the current 2.6.1+ VR path is the accepted baseline and there is a git revert plan.
