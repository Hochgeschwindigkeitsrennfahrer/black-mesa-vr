# GPL vs Async vs GPLAsync vs state cache

This is the decision-critical model. Implementation agent: do not enable GPL "to get Async." They are different mechanisms. L4D2VR's conf is a **2.6.x GPLALL** architecture, not Ph42oN 3.x.

Evidence: **Confirmed from source** (our `dxvk_graphics.cpp`, `dxvk_image.h`, `dxvk_context.cpp`, `dxvk_options.cpp`, `dxvk_state_cache.cpp`) unless noted.

---

## 1. Traditional DXVK Graphics Pipeline Library (GPL)

Vulkan extension: `VK_EXT_graphics_pipeline_library`.

**What is created**

- Shader **pipeline libraries** (VS library, FS library, …) when the game creates/sets D3D shaders — often at load, not at first draw.
- At draw: **fast-link** a complete `VkPipeline` from those libraries + the current render-state vector (`DxvkGraphicsPipelineStateInfo`: vertex layout, blend, depth, RT formats, …).
- Later, a worker may compile an **optimized monolithic** pipeline (`fastHandle`) and swap it in.

**When compilation blocks**

- Library compile can hammer CPU during menus/loads (DXVK README).
- Fast-link is cheap vs a full pipeline compile.
- If fast-link fails, DXVK falls back to a full optimized pipeline **on the calling thread** unless a worker already has it.

**Cache**

- Upstream 2.6: state cache **skips** pipelines that `canCreateBasePipeline` (GPL can reconstruct them). Cache is for the leftover that cannot be fast-linked.
- Upstream **2.7 removed the state cache**, arguing GPL + driver caches replaced it. Evidence: **Confirmed from release/build artifacts** (doitsujin v2.7; Ph42oN v2.7-1 notes).

**VRAM / 32-bit**

- Libraries stay alive → more VRAM and 32-bit VA. `trackPipelineLifetime` Auto enables aggressive library free **on 32-bit** when GPL is on (`mustTrackPipelineLifetime`).

**Driver**

- Needs the extension **and** `graphicsPipelineLibraryIndependentInterpolationDecoration` (`canUseGraphicsPipelineLibrary`).

---

## 2. DXVK Async (classic dxvk-async)

**What is created**

- The **same** full (or GPL-linked) graphics pipelines as usual.
- Difference is **when** and **on which thread**.

**When compilation normally blocks**

- Stock DXVK: first draw with a never-seen (shader, state) pair compiles (or waits for compile) **before** the draw proceeds. That hitch is a missed HMD frame in VR.

**What moves to a background thread**

- `getPipelineHandle(state, async=true)`:
  - If no instance yet and `enableAsync`: take `m_asyncMutex`, re-check, **queue** `compileGraphicsPipeline(..., High, true)`, return **empty handle**.
  - Caller (`commitGraphicsState` / bind pipeline): `if (!handle) skip draw`.
- Worker runs `compilePipeline(state, async=true)` which `createInstance` + optimized compile.

**First-use gate (not all draws are async)**

`checkAsyncCompilationCompat()`: every framebuffer attachment's image view must have `m_rtBindingFrameCount >= 5` (L4D2VR) / 6 (GPLALL 2.6.8-1 notes). Count increments only when the view is used as RT on consecutive frame IDs.

So: brand-new RT (load screen, first flashlight RT, first stereo eye) still compiles **synchronously**. After a few frames, new PSO variants on that RT may skip-draw instead of hitch.

**Visual cost**

- Missing geometry / flashes / wrong lighting for a few frames on new shaders. README: disable Async if that happens.

**Does not require GPL.** That is why GPLALL defaults GPL **off**.

---

## 3. GPLAsync (Ph42oN)

**Problem on stock DXVK 2.1+**

Upstream treated GPL and Async as competing:

- If GPL is on, many pipelines never go through the async/state-cache worker path (`compilePipeline` returns immediately when `enableGraphicsPipelineLibrary == True`; cache skip when `canCreateBasePipeline`).
- If Async is on without GPL fixes, locking/`m_async` vs GPL library lifetime races, and cache policy fights fast-link.

**What the patch changes**

- Pass `async` into `getPipelineHandle`.
- Separate mutex so async compile does not deadlock the sync path.
- Allow Async **even when GPL exists**: skip-draw still works; libraries may still fast-link when not taking the async early-out.
- `gplAsyncCache`: change cache eligibility so GPL-fast-linkable pipelines **are** stored when the flag is on, and Async compiles **write** the cache (`writePipelineStateToCache` under `if (gplAsyncCache)` in `compilePipeline`).

Our `compilePipeline`:

```text
if (enableGraphicsPipelineLibrary == True) return;  // GPL-only mode: no optimized compile, no cache work
if (!gplAsyncCache && !async && canCreateBasePipeline) return;  // skip cache for GPL-able unless async/cache-fix
...
if (gplAsyncCache) writePipelineStateToCache(state);
```

**Coexistence policy in GPLALL 2.6.x (what we ship)**

- Async **on**
- GPL **off**
- gplAsyncCache **on** (so the cache still fills without GPL)

That is **not** "GPL + Async together." It is **Async + cache**, with GPL compiled out. Ph42oN's marketing is "you *may* use both"; GPLALL measured GPL as worse default (VRAM, 2.6.1-4).

---

## 4. GPL + Async + state-cache integration (gplAsyncCache)

**What the cache stores**

`DxvkStateCache` records: shader hashes + graphics state vector (enough to rebuild the pipeline without the game re-creating that combo). On later launches, workers `compileGraphicsPipeline` from disk **before** those draws.

**Why it matters with Async**

Async skip-draw only helps the **first** encounter. The cache makes the second session compile ahead of time so skip-draw (and hitch) mostly disappear.

**Why it matters with GPL**

Upstream: "if GPL can rebuild it, don't cache." Some games (and Windows AMD driver reports Ph42oN cited when defaulting gplAsyncCache True in 2.6) still hitch without those cache entries. `gplAsyncCache=True` forces cache writes anyway.

**When upstream removed state cache (DXVK 2.7)**

- Ph42oN **deleted** `gplAsyncCache` (nothing to integrate).
- GPLALL 2.7-1/2 also lost it; **2.7-3 restored a ported cache**; then GPLALL **dropped the 2.7.x line** in favor of 2.6.x (more backports) and **cancelled 3.x**.
- DXVK 3 replaced this with a **shader IR disk cache** (`%LOCALAPPDATA%/dxvk`) — caches translated shaders, **not** the old pipeline-state combos. Different, and not a drop-in for `gplAsyncCache`.

---

## 5. Do newer DXVK versions still benefit from GPLAsync?

**2.6.x GPLALL: yes.** That is the maintained "old games / 32-bit / keep cache" line.

**2.7 Ph42oN: Async still skip-draws; cache integration is gone.** Benefit is first-run stutter only, plus GPL if left on.

**3.x: mixed / worse for BMVR.** Worker SPIR-V translation helps load CPU; descriptor heaps / ubershaders change compile shape; **Black Mesa broke visually in 3.0**; no FramePacer; Vulkan 1.4. GPLALL's author spent that budget on 2.6.8 instead.

---

## 6. Should GPL be disabled when Async is enabled?

**On the selected build (L4D2VR 2.6.1+ / GPLALL 2.6.8-2): yes, keep GPL False.**

Reasons (Confirmed from source comments + GPLALL README):

- Async does not need the extension
- GPL increases VRAM
- 32-bit VA pressure (eye RTs + SteamVR overlay already heavy)
- DXVK 3.0.1: Intel Windows hangs **with GPL enabled** (forward-looking if anyone tests 3.x)

Ph42oN 3.x would leave GPL Auto; that is a reason **not** to take 3.x, not a reason to turn GPL on in 2.6.

If a future test wants "both": `enableGraphicsPipelineLibrary = Auto` **and** `enableAsync = True` **and** `gplAsyncCache = True` on 2.6.x only. That is an experiment, not the ship default. Watch VRAM, 32-bit crashes, and Intel.

---

## 7. Does L4D2VR's conf reflect an older architecture?

**Partially.**

| Piece | Still current on 2.6.8-2? | Current on Ph42oN 3.x? |
| --- | --- | --- |
| enableAsync True | Yes | Yes |
| gplAsyncCache True | Yes | **No — feature removed** |
| GPL False | Yes (still GPLALL default) | **Against Ph42oN's "use both" default** |
| latencySleep False | Yes | Different product |
| framePace max-frame-latency | **Newer GPLALL default**; **older L4D2VR default is low-latency** | N/A |
| lowLatency* | Yes, but inert if max-frame-latency | N/A |

So: the brief's seven lines are a **2.6.x GPLALL 2.6.8-era explicit conf**, not a 3.x conf, and not a 1:1 dump of L4D2VR 2.6.1 compiled defaults (`framePace`).

---

## Pipeline objects — one-screen summary

```text
D3D shaders created
  ├─ GPL on: compile VkPipelineLibrary (VS/FS/…) on workers / load
  └─ GPL off: nothing yet

First draw with state S
  ├─ Async off: compile full VkPipeline on game thread (hitch)
  ├─ Async on, RT too new: same as Async off (gate)
  └─ Async on, RT aged: queue High worker, skip this draw (hole)

Worker
  └─ createInstance(S): GPL fast-link if allowed, else optimized pipeline
        └─ gplAsyncCache: write S to .dxvk-cache

Later process start
  └─ state cache workers precompile S (2.6.x only)
```
