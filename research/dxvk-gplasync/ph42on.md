# Ph42oN DXVK-GPLAsync v3.0-1

Repository: https://gitlab.com/Ph42oN/dxvk-gplasync

## Bottom line for BMVR

**Do not use v3.0-1 for Black Mesa VR.**

It is a **thin Async patch on upstream DXVK 3.0**, not a GPLALL-style "Async + state cache + Low Latency" product. It removes the exact features L4D2VR's conf assumes (`gplAsyncCache`, state cache). It inherits DXVK 3.0's Vulkan 1.4 requirement and a **Black Mesa rendering regression** that upstream only fixed in **3.0.1** (seven days after this tag).

Evidence: **Confirmed from release/build artifacts**, **Confirmed from source** (fetched `dxvk-gplasync-master.patch` and README), **Confirmed from release/build artifacts** (doitsujin DXVK 3.0 / 3.0.1 notes).

## Release identity

GitLab API (Confirmed from release/build artifacts):

| Tag | Released | Notes |
| --- | --- | --- |
| **v3.0-1** | **2026-06-28** | Description: only "Release v3.0-1". Assets: zip job artifact + `releases/dxvk-gplasync-v3.0-1.tar.gz` |
| v2.7.1-1 | 2025-09-17 | "Update to dxvk 2.7.1" |
| v2.7-1 | 2025-07-13 | **Removed `gplAsyncCache`** because DXVK 2.7 removed state cache |
| v2.6.2-1 | 2025-06-27 | Last 2.6.x Ph42oN line with state cache |

The GitLab `main` tree's `releases/` folder listing still showed tarballs only through v2.7.1-1 when fetched; v3.0-1 is distributed as a **CI artifact zip** plus a tar.gz URL under `releases/`. Evidence: **Confirmed from release/build artifacts**.

README on `main` was last updated around the 2.7.1-1 era and still describes 2.x options with `gplAsyncCache` struck through. Evidence: **Confirmed from source** (fetched README).

## What upstream version is v3.0-1?

| Question | Answer | Evidence |
| --- | --- | --- |
| Based on DXVK 3.0 / 3.0.1? | **DXVK 3.0**, not 3.0.1+ | v3.0-1 = 2026-06-28; DXVK 3.0 = 2026-06-25; DXVK **3.0.1 = 2026-07-05** |
| Based on DXVK 3.0.2? | **No** | 3.0.2 = 2026-07-17 |
| New shader compiler? | **Yes if it is really 3.0** — dxbc-spirv replaces in-tree DXBC/DXSO translation | doitsujin 3.0 notes |
| Vulkan | **1.4 required** | doitsujin 3.0 driver-support section |

The v3.0-1 GitLab description does **not** list a commit SHA. Matching tag name + date to doitsujin 3.0 is a **Strong inference**, not a hashed pin. Implementation agent should unpack the tar.gz and read `RELEASE` / `version.h` before any experiment.

## How GPLAsync is implemented on the 3.x patch

Fetched `patches/dxvk-gplasync-master.patch` (Confirmed from source). It is the same family as 2.7.1-1, adapted to DXVK 3 context:

1. `getPipelineHandle(state, async)` — if async and no instance: unlock, `compileGraphicsPipeline` High, **return null handle** (skip draw).
2. Separate `m_asyncMutex`.
3. RT-age gate: `setRtBindingFrameId` / `getRtBindingAsyncCompilationCompat` with **`m_rtBindingFrameCount >= 5`**.
4. `dxvk.enableAsync` default **true** in `DxvkOptions`.
5. Version string suffix `-gplasync`.

**Not in the 3.x patch:**

- `gplAsyncCache`
- state-cache write path
- `DXVK_GPLASYNCCACHE`
- frame pacer / `dxvk.framePace` / `latencySleep` interaction
- Low Latency from netborg-afps

2.7-1 commit message (Confirmed from release/build artifacts): "Update to dxvk 2.7. Remove gplAsyncCache as it relies on dxvk state cache that was removed in dxvk 2.7."

v3.0-1 does **not** restore that. GPLALL 2.7-3+ reimplemented state cache on 2.7; Ph42oN did not. GPLALL then **abandoned 3.x entirely**.

## Configuration that changed vs 2.6.x L4D2VR

| Option | Ph42oN ≤2.6.2 | Ph42oN ≥2.7-1 / v3.0-1 |
| --- | --- | --- |
| `dxvk.enableAsync` | Yes, default True | Yes, default True |
| `DXVK_ASYNC` | Documented | Still in 2.7.1-1 patch as `env != "0"` extra check; master 3.x patch: `enableAsync` config + `DXVK_ASYNC != "0"` in `useAsync` |
| `dxvk.gplAsyncCache` | Yes | **Removed** |
| `DXVK_GPLASYNCCACHE` | Yes | **Removed** |
| State cache | Yes (upstream) | **Removed upstream 2.7**; Ph42oN did not restore |
| `dxvk.framePace` | **Not in Ph42oN** | **Not in Ph42oN** |
| `dxvk.latencySleep` | Upstream Auto | Upstream 3.x (no FramePacer default-False) |
| `dxvk.enableGraphicsPipelineLibrary` | Often False in GPLALL; Ph42oN leaves upstream **Auto** | Upstream Auto — **GPL on if the driver has the extension** |
| `DXVK_FRAME_RATE` | Upstream | **Removed in DXVK 3.0** |
| Shader cache | State cache files | DXVK 3 **IR shader cache** under `%LOCALAPPDATA%/dxvk` (`DXVK_SHADER_CACHE_PATH`) — different mechanism |

Copying L4D2VR's seven-line conf onto v3.0-1 would:

- Silently ignore `gplAsyncCache` (unknown key)
- Leave GPL **Auto/on** unless you still set `enableGraphicsPipelineLibrary = False`
- Do nothing for frame pacing (keys unknown except `latencySleep` if still parsed)

## Async semantics vs 2.6 GPLALL

Same skip-draw idea. Differences that matter:

- **No state cache precompile** on later runs — first visit of a pipeline after a driver cache flush stutters again (or skips draws). DXVK 3's disk IR cache helps *translation* cost, not the old pipeline-state cache.
- GPL is **intended to coexist** with Async in Ph42oN (that's the project's point). GPLALL 2.6.x **defaults GPL off** because Async replaced it for stutter and GPL burns VRAM.
- DXVK 3 compiles SPIR-V on workers (3.0 notes). Async still skips the *pipeline* until ready.
- `m_rtBindingFrameCount >= 5` (master patch) vs GPLALL 2.6.8-1's **6**.

## 32-bit D3D9 / Windows / Vulkan

| Topic | v3.0-1 | Evidence |
| --- | --- | --- |
| 32-bit builds | Ph42oN historically ships `x32/` + `x64/` in tarballs. **Unknown** whether v3.0-1 zip still includes x32 without unpacking. | Strong inference from prior releases; **Unknown** for v3.0-1 |
| Vulkan 1.4 | Required for DXVK 3.0 | Confirmed from release/build artifacts |
| Nvidia descriptor heap | Needs driver **≥ 595.84** or feature disabled | doitsujin 3.0 |
| RDNA1/2 Windows | Upstream: "stick to DXVK 2.x" | doitsujin 3.0 |
| 32-bit Nvidia stutter | 3.0.1 had to fix descriptor-heap stutter on 32-bit Nvidia — **after** v3.0-1 | doitsujin 3.0.1 |
| Intel + GPL hang | 3.0.1 workaround: hangs with GPL on Windows Intel | doitsujin 3.0.1 |
| **Black Mesa** | 3.0.1: "Fixed a rendering regression affecting numerous D3D9 games (including **Black Mesa**, Gothic 3 and GTA IV)" | Confirmed from release/build artifacts |

v3.0-1 is dated **before** that Black Mesa fix. Using it on BM is asking to rediscover #5732/#5768.

## Low latency

**Ph42oN does not include the netborg-afps FramePacer.** There is no `dxvk.framePace`. Latency control is whatever **upstream DXVK 3** still has (`dxvk.latencySleep`, etc.), which GPLALL authors argue was gutted (`DXVK_FRAME_RATE` removed, no present timing).

For VR, this means v3.0-1 cannot reproduce L4D2VR's pacing story at all.

## Suitability for Black Mesa's graphics path

Black Mesa on Steam is Win32 D3D9 via DXVK, with a **forked** `d3d9.dll` that:

- Uses in-tree DXSO/DXBC compilers (`tools/dxvk_sources.txt`)
- Exposes `DxvkImage::handle()` to OpenVR
- Hooks Present / CreateTexture / WaitForResource

DXVK 3.0 replaces the shader compiler with **dxbc-spirv** (submodule), changes descriptor binding (`VK_EXT_descriptor_heap`), changes buffer upload for 32-bit, changes fixed-function to ubershaders. Porting `IDirect3DVR9` onto that is a **new VR port**, not a rebase.

GPLALL cancelled 3.x after 32-bit D3D9 failures. We should not attempt the port GPLALL declined.

## Known 3.x concerns relevant to BMVR

| Issue | Why it matters |
| --- | --- |
| BM rendering regression in 3.0 | Directly our game |
| Vulkan 1.4 | Older GPUs that run BM + SteamVR on 1.3 drop out |
| Descriptor heaps / 32-bit Nvidia stutter | Our process is 32-bit |
| GPL + Intel Windows hang | If GPL left Auto |
| No state cache | Source maps + flashlight/HDR permutations = lots of pipelines |
| RDNA1/2 Windows | Common SteamVR PCs |
| Huge internal rewrite vs our Present hooks | Combined DLL |

## If someone still wants "newest Ph42oN"

The last Ph42oN that resembles L4D2VR's feature set is **v2.6.2-1** (state cache + gplAsyncCache), **not** v3.0-1. Even that lacks Low Latency and is older than GPLALL 2.6.8-2. There is no reason to pick Ph42oN 2.6.2 over GPLALL 2.6.8-2 or the already-vendored L4D2VR 2.6.1+ fork.
