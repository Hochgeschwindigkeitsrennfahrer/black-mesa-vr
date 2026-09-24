# What L4D2VR actually ships (DXVK)

## Bottom line

L4D2VR does **not** drop a separate DXVK package next to Left 4 Dead 2. It **compiles DXVK into the same Win32 `d3d9.dll` as the VR code**. Async / GPLAsync / Low Latency are **source-ported into that tree**, with compiled-in defaults. The git repo does **not** contain a populated per-game `dxvk.conf` that sets the seven lines quoted in the research brief.

Evidence: **Confirmed from L4D2VR** + **Confirmed from source**.

## Exact version

| Field | Value | Evidence |
| --- | --- | --- |
| `dxvk_new/RELEASE` | `2.6.1` | Confirmed from source: `third_party/l4d2vr/dxvk_new/RELEASE` |
| `dxvk_new/include/version.h` | `#define DXVK_VERSION "v2.6.1+"` | Confirmed from source |
| README claim | "bundled DXVK 2.6.1" + source port from GPLALL 2.6.1-6 | Confirmed from L4D2VR |
| GPLALL commit named in README | `https://github.com/Digger1955/dxvk-gplall/tree/7927e64ac836da1ae31e4205f15a8e653d81d654` | Confirmed from L4D2VR |

The `+` in `v2.6.1+` is the usual DXVK dirty/fork suffix. This tree is **not** stock doitsujin 2.6.1.

## Source vs binaries

| Question | Answer | Evidence |
| --- | --- | --- |
| Bundles source? | **Yes** — full tree at `third_party/l4d2vr/dxvk_new/` (this repo) / `dxvk_new/` upstream | Confirmed from source |
| Bundles prebuilt DXVK DLLs in git? | **No** — zero `.dll` files under `third_party/l4d2vr` | Confirmed from source (glob) |
| How a player gets a DLL | Build `Release\|x86` (`build_release_x86.ps1` / `l4d2vr.sln`) **or** download `L4D2VR.zip` from `liu547161153/l4d2vr` releases and extract into `Left 4 Dead 2` | Confirmed from L4D2VR README |
| D3D8 / D3D11 DLLs shipped for L4D2? | **No for the VR mod.** The DXVK *source* still contains D3D8/D3D11/DXGI, but the VR artifact is one `d3d9.dll` | Confirmed from L4D2VR (install copies `d3d9.dll` only) |
| Vulkan requirement | Vulkan **1.3** (`dxvk_instance.cpp`: "A Vulkan 1.3 capable driver is required.") | Confirmed from source |

L4D2VR README troubleshooting explicitly: do **not** use Steam's Vulkan launch option / `-vulkan`. Launch the normal D3D9 game so this `d3d9.dll` loads.

## Where DXVK lives in the tree

Relevant paths (this repo's vendored copy):

| Path | Role |
| --- | --- |
| `third_party/l4d2vr/dxvk_new/` | Full DXVK 2.6.1+ fork (D3D8/9/11 source, Vulkan, frame pacer, state cache) |
| `third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_vr.cpp` | L4D2VR-only `IDirect3DVR9` |
| `third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_vr.h` | UUID `7e272b32-a49c-46c7-b1a4-ef52936bec87` |
| `third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_reshade_vr.cpp` | ReShade VR compat (L4D2VR-only) |
| `third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_device.cpp` | Present / CreateTexture VR hooks (`#include "L4D2VR/vr.h"`) |
| `third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_interface.cpp` | `Direct3DCreateVRImpl` after device create |
| `third_party/l4d2vr/dxvk_new/src/d3d9/d3d9_window.cpp` | Windowed / focus VR hooks |
| `third_party/l4d2vr/dxvk_new/src/dxvk/dxvk_graphics.cpp` | GPLAsync `getPipelineHandle(..., async)` |
| `third_party/l4d2vr/dxvk_new/src/dxvk/dxvk_options.cpp` | `enableAsync`, `gplAsyncCache`, `framePace` defaults |
| `third_party/l4d2vr/dxvk_new/src/dxvk/framepacer/` | netborg-afps Low Latency pacer |
| `third_party/l4d2vr/dxvk_new/src/dxvk/dxvk_state_cache.cpp` | Upstream-style state cache (still present) |
| `third_party/l4d2vr/dxvk_new/dxvk.conf` | **Comment template only** — all feature lines are commented |
| `third_party/l4d2vr/README.md` | Documents compiled-in defaults + env vars |

Black Mesa VR compiles that same `dxvk_new` via `CMakeLists.txt` + `tools/dxvk_sources.txt` into **our** `d3d9.dll`. Evidence: **Confirmed from current Black Mesa project**.

## Does L4D2VR modify DXVK?

**Yes.** Three layers:

### 1. GPLALL 2.6.1-6 source port (not unique to L4D2)

Async, `gplAsyncCache`, GPL default-off, Low Latency frame pacer, state cache kept. See [`gpl-async-architecture.md`](gpl-async-architecture.md).

### 2. L4D2VR VR patches (the reason we cannot swap DLLs)

These files/call sites do **not** exist in stock DXVK or GPLALL:

- `IDirect3DVR9` / `GetVRDesc` / `TransferSurface` / `WaitDeviceIdle` / `LockSubmissionQueue`
- `Direct3DCreateVRImpl` from `D3D9InterfaceEx` device creation
- `CreateTexture` filling OpenVR Vulkan descriptors when `VR::m_CreatingTextureID` is set
- `D3D9DeviceEx::Present` calling into `VR::Update` after desktop present
- ReShade VR path (`d3d9_reshade_vr.cpp`)

Evidence: **Confirmed from source** (`d3d9_vr.cpp`, `d3d9_device.cpp` includes `L4D2VR/vr.h`).

### 3. Black Mesa VR patches already in this tree (not L4D2VR upstream)

`config.cpp` `bms.exe` profile:

```text
d3d9.customVendorId = 10de
d3d9.deviceLossOnFocusLoss = False
```

Comment cites Intel flashlight artifacts and stock BM `dxvk.conf` racing VR Reset. Evidence: **Confirmed from current Black Mesa project**.

## Fork / patch set

README attribution (Confirmed from L4D2VR):

| Feature | Origin |
| --- | --- |
| Async pipeline compilation | jomihaka (`dxvk-poe-hack`) → Sporif (`dxvk-async`) |
| GPL + Async coexistence | Ph42oN (`dxvk-gplasync`) |
| Low-latency frame pacing | netborg-afps |
| Combined 2.6.1 package | Digger1955 DXVK-GPLALL **2.6.1-6** (`7927e64`) |
| Base | doitsujin DXVK 2.6.1 + L4D2VR VR files |

This is **D3D9-for-the-game**, with D3D11 still in the source tree. The VR submit path is **D3D9 → Vulkan image handles → OpenVR `TextureType_Vulkan`**. DXVK's own `dxvk_openvr.cpp` is the *optional compositor interop helper for DXVK itself* (can be disabled with `DXVK_NO_VR=1`); L4D2VR VR is a **separate** OpenVR session in `VR::InitOpenVR`. Do not confuse the two. Evidence: **Confirmed from source**.

## How `dxvk.conf` is supplied

**L4D2VR does not install a feature `dxvk.conf` in the git tree.**

Loading order in this DXVK fork (`dxvk_instance.cpp` + `config.cpp`):

1. `Config::getUserConfig()`:
   - If `DXVK_CONFIG_FILE` is set → **only that path** (plus optional `DXVK_CONFIG` env semicolon list)
   - Else try `./dxvk.conf` (cwd, usually next to the exe)
   - Else Linux `~/.config/dxvk.conf` / Windows `%APPDATA%/dxvk.conf`
2. Then `merge(getAppConfig(exe))` using `insert` — **existing user keys are not overwritten**

So a per-game file next to the exe **does** win over built-in app profiles **if** `DXVK_CONFIG_FILE` is unset. Evidence: **Confirmed from source**.

L4D2VR README: "A per-game `dxvk.conf` next to `left4dead2.exe` takes priority over the optional global `%APPDATA%/dxvk.conf`." That matches this code **when the launcher does not set `DXVK_CONFIG_FILE`**.

**Black Mesa is different.** The BM launcher sets `DXVK_CONFIG_FILE` to `bin\thirdparty\dxvk-windows-x86\dxvk.conf`. A file next to `bms.exe` is then **ignored**. See [`implementation-plan.md`](implementation-plan.md). Evidence: **Confirmed from current Black Mesa project** (`docs/DXVK.md`).

## Compiled-in defaults (no dxvk.conf)

From `dxvk_options.cpp` / `dxvk_options.h` / `dxvk_framepacer.cpp` in **this** tree:

| Option | Default if unset | Evidence |
| --- | --- | --- |
| `dxvk.enableAsync` | **True** (`DXVK_ASYNC=0/1` overrides) | Confirmed from source |
| `dxvk.gplAsyncCache` | **True** (`DXVK_GPLASYNCCACHE=0/1` overrides) | Confirmed from source |
| `dxvk.latencySleep` | **False** (`Tristate::False`) | Confirmed from source |
| `dxvk.framePace` | `""` (empty) | Confirmed from source |
| Frame pacer if `framePace` empty | **`LOW_LATENCY`** (constructor starts there) | Confirmed from source |
| `dxvk.lowLatencyOffset` | `0` | Confirmed from source |
| `dxvk.lowLatencyAllowCpuFramesOverlap` | **True** | Confirmed from source |
| `dxvk.enableGraphicsPipelineLibrary` | **False** | Confirmed from source |
| `dxvk.enableStateCache` | **True** | Confirmed from source |

**Important discrepancy vs the seven-line block in the research brief:**

The brief lists `dxvk.framePace = "max-frame-latency"`. That is **not** L4D2VR 2.6.1's compiled default. L4D2VR's constructor default is **low-latency**. `max-frame-latency` is:

- one of the three modes the README says you *may* select
- the **GPLALL 2.6.8-2 compiled default** (later change)

Do not treat the brief's seven lines as "what L4D2VR ships in a zip." Treat them as a **plausible explicit conf**, with `framePace` matching **newer GPLALL**, not L4D2VR 2.6.1.

Whether any L4D2VR **release zip** includes a real `dxvk.conf` with those values: **Unknown** (zip not unpacked in this pass). The git tree does not.

## Launch options that cause DXVK to load (L4D2)

L4D2VR README launch options:

```text
-heapsize 524288 -processheap -high -novid -windowed
```

There is **no** `-enabledxvk`. L4D2 loads `d3d9.dll` from the game root the normal Windows way. Players extract `d3d9.dll` next to `left4dead2.exe`. Disable VR with `-nohmd` or by renaming `d3d9.dll` (README still recommends keeping it as DXVK).

Evidence: **Confirmed from L4D2VR**.

## Can Black Mesa use the exact same loading mechanism?

**No.** Documented and Ghidra-verified:

- `bms.exe` on modern Windows: `SetDefaultDllDirectories(0xC00)` = `USER_DIRS | SYSTEM32`, and **skips** full-path `LoadLibraryW` of the thirdparty DXVK DLL.
- `shaderapidx9` `LoadLibrary("d3d9.dll")`.
- DXVK on → `AddDllDirectory(bin\thirdparty\dxvk-windows-x86)` + `DXVK_CONFIG_FILE` pointing at that folder's `dxvk.conf`.
- DXVK off (video menu native D3D9) → thirdparty folder never added → a thirdparty-only install never loads.

BMVR already installs the combined DLL to **all three**: thirdparty folder, `bin\`, next to `bms.exe`. Optional `-enabledxvk` if the video menu is native D3D9. **`-oldgameui` is required** (unrelated to DXVK, but required for VR).

Evidence: **Confirmed from current Black Mesa project** (`docs/DXVK.md`, `AGENTS.md`, `scripts/install.ps1`).

## Does L4D2VR rely on DXVK behavior BMVR also needs?

**Yes — the combined-DLL design, not the GPLALL binary package.**

| Behavior | Why BMVR needs it |
| --- | --- |
| D3D9 → Vulkan | OpenVR `TextureType_Vulkan` submit |
| `IDirect3DVR9::GetVRDesc` | `VkImage` / device / queue / format for `VRVulkanTextureData_t` |
| `TransferSurface` + layout to `TRANSFER_SRC_OPTIMAL` | Compositor can sample the eye image |
| `WaitDeviceIdle` / `WaitForResource` | Optional GPU idle before submit (BMVR gates this; crash-sticky) |
| `CreateDevice` / `Present` inside the same DLL | Hooks without a second injector |
| Named RT / stereo RenderView | L4D2VR architecture (BMVR retried; several paths skip-flagged) |
| Async | Avoid draw-time compile stalls that miss HMD frames |
| State cache | Repeat-visit maps compile less |
| Frame pacer | Desktop Present scheduling only — **does not replace SteamVR** |

Evidence: **Confirmed from source** + **Confirmed from current Black Mesa project** (`docs/ARCHITECTURE.md`).

## Game-specific patches in bundled DXVK

- Built-in DXVK app profiles in `config.cpp` (hundreds of exe regexes), including **`bms.exe`** in *this* tree.
- **No** L4D2-specific `left4dead2.exe` async/framePace profile was found in `config.cpp` (grep). L4D2 relies on compiled defaults. Evidence: **Confirmed from source**.
- `dxvk_openvr.cpp` `-nohmd` skip is L4D2VR-adjacent (Windows command line). Evidence: **Confirmed from source**.

## How VR rendering interacts with DXVK

Target path (L4D2VR / BMVR architecture):

```text
shaderapidx9 LoadLibrary(d3d9.dll)
  → DXVK D3D9 device (Vulkan)
  → Direct3DCreateVRImpl → g_D3DVR9
  → CreateTexture while m_CreatingTextureID set → Vulkan eye images
  → (stereo) two RenderViews into named RTs
  → D3D9 Present:
       desktop swapchain present (frame pacer applies HERE)
       VR::Update: WaitGetPoses, TransferSurface, IVRCompositor::Submit
```

The frame pacer runs on the **D3D9/DXGI Present** of the **desktop swapchain**, not on OpenVR Submit. L4D2VR README states this explicitly. Evidence: **Confirmed from L4D2VR** + **Confirmed from source** (`d3d9_swapchain.cpp` FramePacer).

Async applies when committing graphics state (`checkAsyncCompilationCompat` → skip draw if pipeline handle is null while compiling). First-seen shaders on a render target can drop draws for a few frames. Evidence: **Confirmed from source**.

## What L4D2VR 2.6.1-6 does **not** have vs GPLALL 2.6.8-2

See [`gplall.md`](gplall.md) and [`comparison.md`](comparison.md). Headline: default frame pace flipped to `max-frame-latency`; extra `low-latency-vrr-*` mode; GPU timestamp query pools; D3D9 CPU bind/track refactors; `enableDescriptorUpdateTemplates`; compiler-thread cap changes; 2.6.8-1 rendering hotfix in 2.6.8-2.

L4D2VR can be superseded **as a DXVK snapshot**. It cannot be superseded **as a VR integration** by downloading GPLALL zip.
