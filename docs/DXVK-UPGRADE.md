# Agent playbook: upgrade DXVK in an L4D2VR-style Black Mesa VR mod

Instructions for another AI agent working on **this repo or a fork** that still uses the combined Win32 `d3d9.dll` (DXVK + `IDirect3DVR9` + OpenVR + BMVR `src/`). Read this file end to end before touching DXVK. Do not invent a second VR architecture.

Primary architecture reference: https://github.com/keyou91/l4d2vr  
This repo’s rules: `AGENTS.md`. Failed approaches: `docs/RETIRED.md`.

**Acceptance is visible fused Black Mesa gameplay in the headset.** Compiling, loading, `VR_Init`, and head tracking are not success.

---

## 0. Hard rules

1. **Never drop in a prebuilt** GPLALL / Ph42oN / upstream `d3d9.dll`. That binary has no `IDirect3DVR9`, no Present→`VR::Update`, no eye capture. Headset video dies even if desktop D3D9 still works.
2. **Never switch to meson/MinGW** for the ship artifact. Keep MSVC Win32 CMake (`cmake -A Win32`). DXVK’s meson build is a **source inventory**, not the build you ship.
3. **Never enable GPL** (`dxvk.enableGraphicsPipelineLibrary = True`). Keep `d3d9.deviceLossOnFocusLoss = False`.
4. **Never `ExitProcess` if `VR_Init` fails** at `CreateDevice`. Desktop Black Mesa must still launch.
5. **Do not Y-flip the 2D capture path** to “fix” the new game UI. Require `-oldgameui`.
6. **Do not change Steam launch options** unless a verified incompatibility is documented. Install the DLL to **all three** load paths (below). Add `-enabledxvk` only if the launcher video menu is still native D3D9.
7. **Do not inherit prototype bans.** Crash-sticky `bmvr_in_*.flag` files next to a loaded `d3d9.dll` disable only the attempt that killed the *previous* launch of *this* DLL.
8. **Do not claim it works** without a session log **and** a headset check. Status vocabulary: implemented / compiled / launched / runtime-initialized / head tracking verified / rendering verified / stereo verified / headset presentation verified / gameplay verified.
9. **Do not splice VR onto an old tree by overlaying new DXVK files.** Always copy the **new** DXVK source as the base, then replay VR hunks onto it. The reverse (drop new files into the live VR tree) misses renames and silently keeps stale `.cpp`.
10. **Do not commit** unless the human asked. Do not push.

---

## 1. What you are upgrading

One DLL. CMake compiles every DXVK `.cpp` listed in `tools/dxvk_sources.txt` **plus** `src/*.cpp` into `build/Release/d3d9.dll`.

| Piece | Lives in | Survives a DXVK upgrade? |
| --- | --- | --- |
| Gameplay, hooks, OpenVR session, Submit | `src/` (`vr.cpp`, `hooks.cpp`, …) | Yes, if `IDirect3DVR9` / `DxvkImage` / Present still match |
| DXVK-side VR hooks | `third_party/l4d2vr/dxvk_new/src/d3d9/` | **No** unless you replay them |
| Include shims | `L4D2VR/` (`game.h`, `vr.h`) | Yes — DXVK `#include "L4D2VR/game.h"`. Do not put new code here |

`IDirect3DVR9` UUID must stay `7e272b32-a49c-46c7-b1a4-ef52936bec87` (see `d3d9_vr.h`). `DxvkImage::handle()` must remain a `VkImage` for OpenVR `TextureType_Vulkan`.

---

## 2. Pick a compatible target (do this first)

Black Mesa is **Win32**. The VR DLL is a **DXVK D3D9 fork with in-tree patches**. Compatible means: 32-bit D3D9, Vulkan 1.3, GPLAsync + state cache still present, and a merge you can finish without rewriting `src/vr.cpp`.

### Allowed family

**DXVK-GPLALL 2.6.x** (https://github.com/doitsujin/dxvk fork used by L4D2VR / Ph42oN-era GPLALL). Pin a **git tag**, not `master`.

| Tag | Notes |
| --- | --- |
| `DXVK-GPLALL-2.6.8-2` | Latest 2.6.x GPLALL as of 2026-08. This repo’s live tree after 2026-08-25. Vulkan 1.3, FramePacer, `gplAsyncCache`. **Never use 2.6.8-1.** |
| Older 2.6.1+ (L4D2VR) | What this mod started from (`v2.6.1+`, GPLALL 2.6.1-6). Fine as a revert baseline. |

### Forbidden without a new research pass (treat as a different project)

- **Ph42oN / upstream DXVK 3.x** (v3.0-1, 3.0.2, …): Vulkan 1.4, no state cache, no `gplAsyncCache`, no Low Latency FramePacer, `dxbc-spirv` subproject, descriptor-heap rewrite. DXVK **3.0 regressed Black Mesa rendering**; 3.0.1 is the fix — and it still is not this architecture.
- Stock Steam Black Mesa `d3d9.dll` (gcc 2.6.2 in `bin\thirdparty\dxvk-windows-x86\`). Larger file ≠ newer/better; it is a different toolchain and has **no VR**.
- AVX2 / x64 packages. `bms.exe` is 32-bit.

**How to confirm “latest compatible”:** GitHub releases for DXVK-GPLALL, newest **2.6.8-*** tag that is not `-1`, `RELEASE` file in the tag, and still D3D9 + Vulkan 1.3. If the next tag is 2.7 or 3.x, **stop** and write a research note; do not merge.

---

## 3. Workflow (order is the method)

```
investigate  →  branch/tag current stereo DLL  →  stage new DXVK source
    →  splice VR onto the NEW tree  →  refresh source list + CMake glue
    →  clean MSVC Win32 Release build  →  three-path install  →  log + headset
```

Not: assume → copy zip into the game → hope.

Work on a git branch. Tag or copy aside the last **headset-verified** `build/Release/d3d9.dll` before you overwrite the game.

---

## 4. Inventory the VR hunks (from the *current* live tree)

Grep the **currently working** `third_party/l4d2vr/dxvk_new` (or the fork’s equivalent). You are looking for BMVR/L4D2VR, not GPLALL comments.

### Copy as-is (no port)

These files do not exist upstream. Copy from the live VR tree into the new tree:

- `src/d3d9/d3d9_vr.cpp` / `d3d9_vr.h`
- `src/d3d9/d3d9_reshade_vr.cpp` / `d3d9_reshade_vr.h` / `d3d9_reshade_api.h`
- `src/d3d9/d3d9_multithread.h` — **must** keep exclusive lock + `BeginSourceFrameOwnership`. PresentEx and `IDirect3DVR9` need this. Do not take upstream’s multithread header.

### Port by hand (upstream file exists; VR is a patch)

Diff live vs a clean copy of the *current* DXVK version, then replay those diffs onto the **new** version of the same file. Keep the new version’s D3D9 CPU/bind/swapchain changes unless a VR hunk contradicts them.

| File | What the VR/BMVR hunks do |
| --- | --- |
| `src/d3d9/d3d9_device.cpp` | Helpers, ReShade depth atlas, overlay draw, Reset/ResetEx **windowed skip**, `CreateTexture` **eye capture** (`m_CreatingTextureID`), CreateDepthStencilSurfaceEx MSAA/ReShade, SetViewport eye RT, **entire Present / PresentEx** (desktop present then `VR::Update`), `NotifyWindowActivated` skip, destructor ReShade clear |
| `src/d3d9/d3d9_device.h` | `class VR;`, `LockDeviceExclusive`, overlay methods, ReShade atlas members, null-safe `EmitCs` |
| `src/d3d9/d3d9_interface.cpp` | `-nohmd`, CreateDevice HMD backbuffer (if still used), `Direct3DCreateVRImpl` |
| `src/d3d9/d3d9_window.cpp` | Skip exclusive-fullscreen alt-tab helper while VR is on |
| `src/d3d9/d3d9_main.cpp` | `D3D9ReShadeVrPrepareConfiguration` |
| `src/d3d9/d3d9_swapchain.cpp` | Usually already has `DXVK_FORCE_WINDOWED` on GPLALL; FramePacer ctor may take `DxvkDevice*` — use the **new** signature, do not paste the old one |
| `src/util/config/config.cpp` | `bms.exe` profile: `d3d9.customVendorId=10de`, `d3d9.deviceLossOnFocusLoss=False` (regex `\\bms\.exe$`) |
| `src/dxvk/dxvk_openvr.cpp` | `-nohmd` also sets `m_no_vr` |

Grep anchors after the splice (must still hit):

- `Direct3DCreateVRImpl`
- `m_CreatingTextureID`
- `LockDeviceExclusive`
- `g_Game->m_VR`
- `VR::Update`
- `-nohmd`

### Gameplay code (`src/`)

Do **not** rewrite `src/vr.cpp` to match DXVK. If a new DXVK type/signature breaks the compile, fix the **DXVK-side** hook or a thin adapter. New eye texture IDs or Submit changes require another splice, not a new compositor stack.

---

## 5. Stage the new DXVK source

```text
# Example pin
git clone --depth 1 --branch DXVK-GPLALL-2.6.8-2 https://github.com/<gplall-remote>/dxvk.git staging/dxvk-gplall-<ver>
```

Confirm `RELEASE` / `include/version.h.in` matches the tag.

Build a **drop-in directory** (do not MIR over live until the drop-in compiles in your head):

1. Copy the GPLALL tree (exclude `.git`).
2. Overlay from the **then-live** MSVC tree things meson generates that this CMake build does not:

   - `include/vulkan/` and `include/spirv/` (headers already vendored in this repo)
   - `include/shaders/` (pregenerated SPIR-V C arrays)
   - `subprojects/libdisplay-info/` (C sources already on `tools/dxvk_sources.txt`)
   - `lib32/vulkan-1.lib` (**easy to destroy** — see §7)
   - `src/d3d9/d3d9.def` if the new tree’s `.def` is missing VR-unrelated DXVK ordinals; prefer the **new** `.def` plus a check that `Direct3DCreate9` still exports

3. Copy the as-is VR files (§4).
4. Port the hand-splice files (§4).
5. Set `include/version.h` to a unique string, e.g. `DXVK_VERSION "v2.6.8-2+bmvr"`. That string is how you prove the game loaded **this** build.

The previous upgrade (2.6.1 → 2.6.8-2, removed `research/dxvk-gplasync/` in git history: `_build_drop_in.py`, `HOW-TO-APPLY.md`) is an example. Treat it as a **2.6.1 → 2.6.8-2** recipe, not a script you blindly re-run onto a newer tag. Re-diff every ported file against the new upstream.

---

## 6. Refresh `tools/dxvk_sources.txt`

CMake does **not** glob. It compiles only paths in `tools/dxvk_sources.txt`, relative to `third_party/l4d2vr/dxvk_new`.

Diff the new `meson.build` files (`src/d3d9`, `src/dxvk`, `src/util`, `src/wsi`, `src/spirv`, `src/dxbc`, `src/dxso`) against the current list.

- **Add** new `.cpp` (2.6.8-2 examples: `dxvk_descriptor_pool.cpp` replacing `dxvk_descriptor.cpp`, `framepacer/dxvk_calibrated_device_timestamps.cpp`, `hud/dxvk_hud_item_latency.cpp`, `util/util_unmap.cpp` replacing `d3d9_mem.cpp`, `com/com_destruction_notifier.cpp`).
- **Remove** files that no longer exist.
- **Keep** `d3d9_vr.cpp` and `d3d9_reshade_vr.cpp`.
- **Do not add** GLFW/SDL WSI (`src/wsi/sdl*`, `src/wsi/glfw*`). Win32 only: `wsi_edid.cpp`, `wsi_platform.cpp`, `win32/wsi_*`.
- **Do not add** d3d11/dxgi/d3d8/d3d10.

Unresolved `LNK2019` for a `dxvk::` symbol that clearly lives in a `.cpp` you can open usually means **that file is missing from the list** or **stale .obj** (§8).

---

## 7. MSVC glue meson hides (will fail the first compile)

### `include/buildenv.h`

New DXVK logs `DXVK_TARGET` / `DXVK_COMPILER` / `DXVK_COMPILER_VERSION` from meson-generated `buildenv.h`. Create it by hand:

```c
#pragma once
#define DXVK_TARGET "x86"
#define DXVK_COMPILER "msvc"
#define DXVK_COMPILER_VERSION "19"
```

### Shader headers in `include/shaders/`

`src/dxvk/*.cpp` `#include <dxvk_blit_frag_2d_ms.h>` etc. If the new meson `dxvk_shaders` list has a `.frag`/`.comp`/`.vert` with **no** matching `.h` in the copied `include/shaders/`, compile it:

```text
glslangValidator --target-env vulkan1.3 --vn <basename> <shader> -o include/shaders/<basename>.h
```

Match `glsl_args` in the new `meson.build` (`--target-env vulkan1.3` on 2.6.8). Wrap like existing headers (`#pragma once` + `const uint32_t <basename>[] = { ... }`).

2.6.8-2 added `dxvk_blit_frag_2d_ms.frag` — the 2.6.1 shader dump did not have it.

### CMake include dirs

Files under `src/dxvk/framepacer/` include `"dxvk_options.h"` (parent dir). Meson adds `src/dxvk` to the include path. CMake must too:

- `${DXVK}/src`
- `${DXVK}/src/dxvk`
- existing `${DXVK}/include`, `include/shaders`, `src/d3d9`, vulkan/spirv, libdisplay-info

Without this: `C1083: Cannot open include file: 'dxvk_options.h'`.

### `synchronization.lib`

2.6.8+ uses `WaitOnAddress` / `WakeByAddressAll`. Link `synchronization` in `CMakeLists.txt` next to `vulkan-1`. Missing this: `LNK2019: unresolved external symbol _WaitOnAddress@16`.

### `lib32/vulkan-1.lib`

CMake `target_link_directories` includes `${DXVK}/lib32`. This file is **not** in the GPLALL git tree. It lives only in this repo’s MSVC layout.

**`robocopy /MIR` of a drop-in that omits `lib32/` deletes `vulkan-1.lib`.** Restore from git before linking (`LNK1181: cannot open input file 'vulkan-1.lib'`). Copy `lib32/vulkan-1.lib` **into the drop-in** so the next MIR does not wipe it.

### Version / config defines

Keep `DXVK_WSI_WIN32`, `/bigobj`, `/MP`. C++17. Win32 only (`CMAKE_SIZEOF_VOID_P EQUAL 4`).

---

## 8. Build until it links (expected breakage)

```sh
ninja -C build-beta d3d9   # configure: AGENTS.md → Build
```

### Clean rebuild is mandatory after replacing `dxvk_new`

Robocopy preserves source timestamps. MSBuild then **keeps 2.6.1 `.obj` files** that are newer than the 2.6.8 sources. Symptom: compile “succeeds,” then `LNK2001` for `g_formatInfos` with a new `std::array<...,157>` size, `tryInverse`, `opSinCos`, `FramePacer::FramePacer`, `wsi::saveWindowState`, etc. — symbols that **are** in files on the source list.

Fix: delete `build\d3d9.dir\Release` (and the old `build\Release\d3d9.dll`) then rebuild. Do not incrementally link a mixed tree.

### Replay PresentEx against the new D3D9 types

Present/PresentEx was copied from the old BMVR `d3d9_device.cpp`. New DXVK will rename fields. **The compiler names them.** Examples from 2.6.1 → 2.6.8-2:

| Old | New |
| --- | --- |
| `D3D9DeviceFlag::InScene` | `m_inScene` (bool). Overlay submit uses `!m_inScene`. |
| `pSoftwareCursor->ResetCursor` | `pSoftwareCursor->ClearCursor` (`d3d9_cursor.h`). Do **not** rename `m_cursor.ResetCursor()` (that is a method). |

Fix the splice. Do not revert to a prebuilt DLL.

### More compile errors

Keep going. One error per rename is normal. Do not “simplify” Present by deleting `VR::Update`.

---

## 9. Apply the drop-in to the live tree

Only after the drop-in is something you are willing to compile:

```powershell
# Prefer preserving lib32: copy onto, then restore vulkan-1.lib if needed
# MIR is OK only if the drop-in contains lib32\vulkan-1.lib
Copy-Item -Recurse -Force .\<drop-in>\dxvk_new\* .\third_party\l4d2vr\dxvk_new\
Copy-Item -Force .\...\drop-in-dxvk_sources.txt .\tools\dxvk_sources.txt
```

CMake `DXVK` path stays `third_party/l4d2vr/dxvk_new`. Do not point CMake at the drop-in folder.

Then **clean** Release build (§8).

Proof of compile: `build\Release\d3d9.dll` contains the version string (search the binary for `v2.6.x-y+bmvr`).

---

## 10. Install (three paths, always)

Close `bms.exe`, then copy `d3d9.dll` to:

1. `Black Mesa\bin\thirdparty\dxvk-windows-x86\d3d9.dll` — launcher `AddDllDirectory` / `DXVK_CONFIG_FILE`
2. `Black Mesa\bin\d3d9.dll` — `shaderapidx9` `LoadLibrary("d3d9.dll")` with `LOAD_LIBRARY_SEARCH_USER_DIRS`
3. `Black Mesa\d3d9.dll` — next to `bms.exe`

A copy in only one of these is a **failed** install. Native D3D9 never loads the thirdparty folder; `-enabledxvk` is required if the video menu is Direct3D 9.

Keep `d3d9.dll.stock-dxvk` in the thirdparty folder (non-VR rollback only).

Black Mesa sets `DXVK_CONFIG_FILE` to the **thirdparty** `dxvk.conf`. A file next to `bms.exe` is ignored. Keep the required keys in that file.

Minimum conf (do not drop these):

```text
d3d9.deferSurfaceCreation = True
d3d9.deviceLossOnFocusLoss = False
```

Safe explicit defaults (already true in GPLALL 2.6.x unless you override):

```text
dxvk.enableAsync = True
dxvk.gplAsyncCache = True
dxvk.latencySleep = False
dxvk.enableGraphicsPipelineLibrary = False
```

**`dxvk.framePace`:** L4D2VR 2.6.1 implicit default was `low-latency`. GPLALL 2.6.8-2 default is `max-frame-latency`. For VR, A/B on the **same map** after cache is warm. Do not ship `min-latency`. Intel + `low-latency` has crashed in GPLALL notes — if so, force `max-frame-latency`.

Do not enable `gplAsyncCache` on any 3.x build (key does not exist).

---

## 11. Verify (log first, headset second)

Launch from Steam with existing L4D2VR options **plus `-oldgameui`**. Add `-enabledxvk` if needed.

### Log proof the new DXVK loaded

- DXVK log next to cwd / `bms_d3d9.log`: line `DXVK: v2.6.x-y+bmvr` (the string from `include/version.h`). If you still see `v2.6.1+`, the game loaded an old copy — check all three paths and that `bms.exe` was closed during install.
- `Found config file:` must be the **thirdparty** `dxvk.conf`.
- `Effective configuration:` must show `deviceLossOnFocusLoss` False, GPL False.

### Headset proof (the actual test)

1. Old game UI visible in the HMD (not upside-down new UI).
2. Load a real map (`bm_c0a0a`), not `background*`.
3. Stereo fused, 6DoF tracking, world not black after load.
4. Watch `bmvr_log.txt` for Submit errors, `poseWaitOvershoot`, `frameIntervalUsMax`.

Crash-sticky `bmvr_in_*.flag` (game root and `bin\`) disable individual VR **attempts** (named eye RTs, relative look, …). They do **not** mean “DXVK upgrade failed.” Delete a flag only if you intend to retry that attempt on **this** DLL.

### If stereo dies

Revert the **DLL** to the last headset-good `d3d9.dll` on all three paths. Then git-restore `dxvk_new` + `tools/dxvk_sources.txt` + `CMakeLists.txt` if you changed glue. Restoring `d3d9.dll.stock-dxvk` is **desktop-only**, no VR.

---

## 12. Config-only vs source upgrade

If the human only wanted less stutter on the **current** DLL: change `dxvk.conf` only. Do not replace `dxvk_new`. That is transparent to hooks.

A source upgrade is for a specific 2.6.x D3D9/CPU/pacer fix, or to stay on the latest **compatible** GPLALL. It is highly risky. Do it on a branch with a one-command DLL revert.

---

## 13. What this repo looked like after the 2.6.8-2 upgrade (worked example)

Completed 2026-08-25 on this tree. Use as a checklist, not as “already done on every fork.”

- Base: GPLALL tag `DXVK-GPLALL-2.6.8-2`, version `v2.6.8-2+bmvr`.
- VR spliced onto 2.6.8-2 (not the reverse).
- Glue added: `include/buildenv.h`, `include/shaders/dxvk_blit_frag_2d_ms.h`, CMake `${DXVK}/src` + `${DXVK}/src/dxvk`, `synchronization.lib`, restored `lib32/vulkan-1.lib` after MIR.
- PresentEx: `ResetCursor` → `ClearCursor`; InScene → `m_inScene`.
- Clean Release rebuild required (stale objs).
- Installed 2,576,896-byte DLL to all three Black Mesa paths.
- **Headset stereo was not verified in that implementation session.** A fork must still pass §11.

---

## 14. Stop conditions

Stop and report (do not keep merging) if:

- The only available “latest” is DXVK 3 / Ph42oN 3.
- `IDirect3DVR9` or `DxvkImage::handle()` no longer maps to a `VkImage` you can Submit.
- CreateDevice/Reset exclusive-fullscreen comes back and you are about to “fix” it by forcing HMD swapchain size (already verified bad on Black Mesa — see `docs/RETIRED.md`, `hmd_swap`).
- You are tempted to ship a gcc zip `x32/d3d9.dll` to “just test DXVK.” That test has no VR; do it only as a desktop experiment with the BMVR DLL moved aside, and say so.

When uncertain: investigate (Ghidra / x32dbg if the **runtime** path is in doubt) → verify → implement. Not assume → implement → hope.
