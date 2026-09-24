# Implementation plan: DXVK GPLAsync / Low Latency for Black Mesa VR

For the main development agent. This research pass **did not** modify VR source, replace DXVK, write the live `dxvk.conf`, or build.

**Read first:** [`comparison.md`](comparison.md), [`config.md`](config.md).

---

## 1. Which DXVK version to use

**Ship and develop against the DXVK already compiled into this repo:**

`third_party/l4d2vr/dxvk_new` — **DXVK 2.6.1+** with GPLALL **2.6.1-6** (`7927e64`) + L4D2VR `IDirect3DVR9` + BMVR Present hooks.

`include/version.h`: `DXVK_VERSION "v2.6.1+"`. Evidence: **Confirmed from source**.

**Do not** switch the running game to:

- Steam stock DXVK 2.6.2 gcc (`bin\thirdparty\dxvk-windows-x86\d3d9.dll.stock-dxvk` backup)
- Prebuilt DXVK-GPLALL 2.6.8-2 zips
- Ph42oN v3.0-1 (or any 2.7+ Ph42oN)

**Optional later (Phase 2 only):** copy [`drop-in-dxvk_new/`](drop-in-dxvk_new/) over `third_party/l4d2vr/dxvk_new` and replace `tools/dxvk_sources.txt` with [`drop-in-dxvk_sources.txt`](drop-in-dxvk_sources.txt). Procedure: [`HOW-TO-APPLY.md`](HOW-TO-APPLY.md). Never 2.6.8-1. Never a prebuilt GPLALL `d3d9.dll`. The drop-in is **not applied** until that copy + MSVC rebuild + three-path install.

---

## 2. Why

| Requirement | L4D2VR 2.6.1+ (now) | GPLALL 2.6.8-2 | Ph42oN v3.0-1 |
| --- | --- | --- | --- |
| Combined VR `d3d9.dll` | **Already** | Would destroy VR if drop-in | Would destroy VR |
| Async stutter reduction | **Already default on** | Yes | Yes |
| `gplAsyncCache` / state cache | **Yes** | Yes | **Removed** |
| Low Latency FramePacer | **Yes** | Yes (better default) | **No** |
| Vulkan | 1.3 | 1.3 | **1.4** |
| 32-bit D3D9 | Yes | Yes (branch purpose) | 3.x failed BL2 x32 for GPLALL author; 3.0.1 still patching x32 Nvidia |
| Black Mesa rendering | Current verified stereo/6DoF on this DLL | Unknown until merged | **3.0 regressed BM**; fix is 3.0.1, after v3.0-1 |
| ABI vs our hooks | **Current** | Invasive merge | New port |

Ph42oN v3.0-1 is **not** “latest compatible.” It is latest **incompatible**.

GPLALL 2.6.8-2 is better *DXVK* than 2.6.1-6, but **not** better *this week* than keeping a working VR binary. Stereo Submit is the acceptance criterion (`AGENTS.md`).

---

## 3. Which DLLs to deploy

**Only our CMake output:** `build\Release\d3d9.dll` (Win32).

Also already copied by `scripts\install.ps1`: 32-bit `openvr_api.dll` (SteamVR `bin\win32` preferred).

**Do not deploy** from GPLALL/Ph42oN zips:

- `d3d9.dll` / `d3d11.dll` / `dxgi.dll` / `d3d8.dll` / `d3d10core.dll`
- AVX2 or `*64*` packages

Those prebuilts have **no** `Direct3DCreateVRImpl`, **no** Present→`VR::Update`.

If someone wants a **non-VR** sanity check of GPLALL (desktop BM only, VR off): GCC **SSE2** zip, **x32/d3d9.dll** only, and they must **not** overwrite the BMVR DLL. That experiment is optional and out of scope until Phase 1 conf is done.

---

## 4. Where they should go

Unchanged from current BMVR load path (Confirmed from current Black Mesa project):

1. `Black Mesa\bin\thirdparty\dxvk-windows-x86\d3d9.dll`
2. `Black Mesa\bin\d3d9.dll`
3. `Black Mesa\d3d9.dll` (next to `bms.exe`)

Launch: existing L4D2VR options + **`-oldgameui`** + **`-enabledxvk`** if the video menu is native D3D9.

Close `bms.exe` before copy. Keep `d3d9.dll.stock-dxvk` backup in the thirdparty folder.

---

## 5. How to configure them

Black Mesa sets **`DXVK_CONFIG_FILE`** to the **thirdparty** `dxvk.conf`. Edit **that** file, not a new file next to `bms.exe` (ignored).

`install.ps1` currently **overwrites** that file. Any Phase 1 change must update `install.ps1` so the next install does not wipe Async keys. Until then, a manual edit is lost on next `install.ps1`.

Built-in `bms.exe` profile still supplies `customVendorId=10de` unless the user file overrides it. Do not add a conflicting vendor id.

---

## 6. Whether to bundle `dxvk.conf`

**Yes — but as part of the existing thirdparty rewrite, not a second competing file.**

Approach mapping:

| Approach | Verdict |
| --- | --- |
| **A. Bundle DXVK DLLs** | **Already done** (compiled into our `d3d9.dll`). Do not add a second DXVK. |
| **B. Per-game dxvk.conf** | **Yes**, via `DXVK_CONFIG_FILE` path. |
| **C. Custom DXVK fork** | **Already are one.** Further fork = Phase 2 merge of 2.6.8-2, not a new repo. |
| **D. Use stock BM DXVK** | **No.** Stock 2.6.2 has no Async/FramePacer and is not our VR DLL. |

---

## 7. Which L4D2VR settings to copy

Copy **intent**, not a mythical zip conf.

Safe to set explicitly (already default True/False as noted):

- `dxvk.enableAsync = True`
- `dxvk.gplAsyncCache = True`
- `dxvk.latencySleep = False`
- `dxvk.enableGraphicsPipelineLibrary = False`

Keep BMVR-required:

- `d3d9.deviceLossOnFocusLoss = False`
- `d3d9.deferSurfaceCreation = True`

---

## 8. Which settings to change vs L4D2VR

**`dxvk.framePace`:** L4D2VR 2.6.1 **implicit default is `low-latency`**. GPLALL 2.6.8-2 **default is `max-frame-latency`**. The research brief quoted the latter.

For VR, **do not assume L4D2VR's implicit low-latency is best.** Implementation sequence:

1. Measure baseline (current DLL, **no** `framePace` line) — this **is** L4D2VR low-latency.
2. Set `dxvk.framePace = "max-frame-latency"` and compare headset smoothness / pose overshoot / Intel stability.
3. Only then consider explicit `"low-latency"` on NVIDIA/AMD dGPU.
4. Never ship `"min-latency"` or `low-latency-vrr-*`.

**`lowLatencyOffset` / `lowLatencyAllowCpuFramesOverlap`:** leave defaults (0 / True). Do not copy them unless documenting a full block; they do nothing in max-frame-latency mode.

**Do not copy `gplAsyncCache` onto any 2.7+/3.x Ph42oN build.**

---

## 9. How to preserve current VR hooks

Our VR stack **is** DXVK internals. Grep anchors:

| Hook | Files |
| --- | --- |
| `IDirect3DVR9` | `dxvk_new/src/d3d9/d3d9_vr.cpp`, `d3d9_vr.h` |
| Create VR iface | `d3d9_interface.cpp` `Direct3DCreateVRImpl` |
| Eye textures | `d3d9_device.cpp` `CreateTexture` + `m_CreatingTextureID` |
| Present → VR | `d3d9_device.cpp` Present; `src/vr.cpp` Submit / `TransferSurface` / `GetVRDesc` |
| Window/Reset | `d3d9_window.cpp`, device Reset windowed force |
| Exports | `d3d9.def` — stock D3D9 + DXVK annotation ordinals; VR is not a separate export |
| CMake | `tools/dxvk_sources.txt` + `src/*.cpp` |

**Drop-in GPLALL/Ph42oN DLL = all of the above gone.** Risk: **impossible without adapting hooks** if the DLL is replaced; **highly risky** if `dxvk_new` is replaced wholesale; **transparent** if only `dxvk.conf` changes.

Phase 1 (conf only): **transparent** to hooks.

Phase 2 (2.6.8-2 source merge): **highly risky**. Procedure if ever approved:

1. Branch. Do not work on the stereo-critical tree without a revert tag.
2. Copy VR-only files aside: `d3d9_vr.*`, `d3d9_reshade_vr.*`, and a diff of `d3d9_device.cpp`, `d3d9_interface.cpp`, `d3d9_window.cpp`, `d3d9_swapchain.cpp`, `config.cpp` (`bms.exe` profile).
3. Overlay GPLALL 2.6.8-2 **source** onto `dxvk_new` (not the binary zip).
4. Re-apply VR diffs. FramePacer ctor now needs `DxvkDevice*` — `d3d9_swapchain.cpp` will not compile until updated.
5. Refresh `tools/dxvk_sources.txt` for new `.cpp` files (framepacer, any new d3d9).
6. Keep MSVC Win32 CMake; do not switch to meson mingw unless the whole build is redesigned.
7. Confirm `IDirect3DVR9` UUID unchanged.
8. Confirm `DxvkImage::handle()` still a `VkImage` for OpenVR.
9. Re-run stereo/6DoF acceptance — compiling is not success.

---

## 10. How to test safely

1. Quit Black Mesa. Install current `d3d9.dll` as today.
2. Backup `bin\thirdparty\dxvk-windows-x86\dxvk.conf`.
3. Confirm log `bms_d3d9.log` (or `dxvk` log next to cwd): `DXVK: v2.6.1+`, `Frame pace: low-latency` (baseline).
4. Apply Test 1 conf from [`config.md`](config.md). Relaunch with `-oldgameui -enabledxvk` as needed.
5. Confirm `Found config file:` points at the **thirdparty** path and `Effective configuration:` lists the new keys.
6. Play a known map (`bm_c0a0a`), flashlight on/off, first vs second launch.
7. Watch `bmvr_log.txt` Present diagnostics (poseWaitOvershoot, frameIntervalUsMax, Submit errors).
8. Optional one session: `DXVK_HUD=compiler` (env), then unset.
9. Change **one** variable per run (`framePace` **or** `enableAsync`, not both).

Do not enable named-RT / hmd_swap / other skip-flagged render experiments in the same session as a DXVK conf A/B.

---

## 11. How to revert if rendering breaks

**Conf-only failure**

- Restore backed-up `dxvk.conf` or re-run current `install.ps1` (two D3D9 lines).
- Or `DXVK_ASYNC=0` for one session if the file is sticky.
- No need to replace the DLL.

**DLL / Phase 2 failure**

- Recopy last known-good `build\Release\d3d9.dll` to all three paths.
- Restore `d3d9.dll.stock-dxvk` only if you need **non-VR** stock DXVK (no headset).
- Git revert the `dxvk_new` merge.
- Crash-sticky `bmvr_in_*.flag` files disable VR *attempts*, not DXVK Async — do not confuse them.

---

## 12. How to verify shader compilation / stutter

| Signal | How |
| --- | --- |
| Compiler busy | `DXVK_HUD=compiler` or HUD `cs`; log `Using N compiler threads` |
| Cache working | Second launch: smaller compiler HUD; `.dxvk-cache` grows next to cwd / `DXVK_STATE_CACHE_PATH` |
| Async holes | Visual: missing props/flashlight for a few frames; compare `enableAsync=False` |
| GPL accidentally on | Log / `canUseGraphicsPipelineLibrary`; `Effective configuration` must show False |
| 32-bit pressure | Crashes / alloc failures — lower compiler threads (`dxvk.numCompilerThreads`) only if proven |

First-run stutter **with** Async is expected. Judge **second** run + in-level hitch after cache fill.

---

## 13. How to verify frame pacing / latency in VR

Desktop FPS HUD is **not** motion-to-photon.

Use:

- SteamVR frame-timing overlay (reprojection %, dropped frames)
- Existing BMVR Present log: `frameIntervalUsMax`, `poseWaitOvershoot*`
- `DXVK_HUD=frametimes` on **desktop** only as a secondary signal
- Subjective: tracking swim vs hitch vs stereo fuse

Compare modes on the **same map, same SS, same `mat_queue_mode`**.

If `max-frame-latency` increases dropped frames vs baseline low-latency **and** overshoot does not improve, keep L4D2VR implicit default and document it.

If low-latency crashes on Intel, that matches GPLALL 2.6.7-1; force max-frame-latency.

---

## Phase plan (implementation agent)

### Phase 0 — already true; verify, do not rewrite DXVK

Confirm a current play session log contains `v2.6.1+` and `Frame pace:`. If Frame pace is missing, the pacer is not constructing — investigate before conf work. **Unknown** until a log from this machine is read in a later session.

### Phase 1 — configuration only (do this first)

Risk: **transparent** to VR hooks.

1. Extend `scripts/install.ps1` thirdparty `dxvk.conf` with the Test 1 block in [`config.md`](config.md), **keeping** the two `d3d9.*` lines.
2. Optionally ship the same snippet in `packaging/` notes — do not document “put dxvk.conf next to bms.exe” without saying BM overrides that via `DXVK_CONFIG_FILE`.
3. A/B `framePace` as in §8.
4. Stop if stereo/6DoF regresses; revert conf.

**Do not** replace DLLs in this phase.

### Phase 2 — optional GPLALL 2.6.8-2 source merge

Risk: **highly risky**. Only if Phase 1 shows compile stutter / D3D9 CPU as the limiter **after** cache is warm, or a specific 2.6.8-2 D3D9 fix is needed.

Pin: GitHub tag `DXVK-GPLALL-2.6.8-2`. Staging merge already built at [`drop-in-dxvk_new/`](drop-in-dxvk_new/). Apply with [`HOW-TO-APPLY.md`](HOW-TO-APPLY.md) only when Phase 2 is approved. First MSVC compile after the copy is part of implementation, not of this research drop.

Not: Ph42oN v3.0-1, upstream 3.0.2, GPLALL 2.6.8-1, AVX2 zips.

### Phase 3 — never without a new research pass

- DXVK 3.x / Ph42oN 3.x
- Enabling GPL by default
- `latencySleep = True`
- Drop-in prebuilt `d3d9.dll`

---

## Compatibility of an upgrade (summary for the agent)

| Change | Likely impact |
| --- | --- |
| `dxvk.conf` only | **Transparent** |
| New compiler-thread / HUD env | Transparent |
| Replace installed `d3d9.dll` with GPLALL/Ph42oN prebuilt | **Impossible** for VR (hooks gone) |
| Merge 2.6.8-2 into `dxvk_new` | **Highly risky**, possible with VR patch replay |
| Merge DXVK 3 / Ph42oN 3 | **New project** (dxbc-spirv, descriptor heaps, Vulkan 1.4, BM 3.0 bug) |

We do **not** hook DXVK via a separate export table beyond `d3d9.def`. We **do** compile DXVK as a static set of `.cpp` files in the same DLL (`CMakeLists.txt` `DXVK = third_party/l4d2vr/dxvk_new`). Version layout **does** matter: `D3D9DeviceEx::Present` and `DxvkImage` are not a stable ABI.

---

## Safest default *prebuilt* (only if testing non-VR)

`DXVK-GPLALL-GCC-WinMacLinux-SSE2-O3-LTO.2.6.8-2.zip` → **`x32/d3d9.dll`**. SSE2, 32-bit, Windows.

Optimized: SSE4.2 GENERIC x32 if CPU supports SSE4.2.

Never AVX2/AMD64/INTEL64 for `bms.exe`.

**This is not the BMVR ship artifact.**

---

## One-paragraph handoff

Black Mesa VR already **is** L4D2VR’s DXVK 2.6.1+ with asynchronous pipeline compilation, `gplAsyncCache`, GPL off, and a Low Latency frame pacer compiled in. Do not replace that DLL with GPLALL or Ph42oN binaries. Do not move to Ph42oN v3.0-1 (no state cache, no pacer, Vulkan 1.4, Black Mesa 3.0 regression). Configure via the launcher’s `DXVK_CONFIG_FILE` (`bin\thirdparty\dxvk-windows-x86\dxvk.conf`), keep `deviceLossOnFocusLoss=False`, explicitly set Async/cache/GPL/latencySleep, and **A/B `framePace` starting with `max-frame-latency` rather than copying L4D2VR’s implicit `low-latency` as gospel**. Treat DXVK-GPLALL 2.6.8-2 as a later **source merge** only, after VR is stable and with a revert path for `d3d9_vr` / Present.
