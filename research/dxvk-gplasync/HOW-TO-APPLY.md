# HOW TO APPLY: DXVK-GPLALL 2.6.8-2 + VR drop-in

This folder is a **source tree**, not a game `d3d9.dll`. Copying it into Black Mesa will not produce headset video.

**Status:** staging only. Live `third_party/l4d2vr/dxvk_new`, `CMakeLists.txt`, `tools/dxvk_sources.txt`, `scripts/install.ps1`, and the game install were **not** changed.

**Rebuild the drop-in** (optional) from repo root:

```
python research/dxvk-gplasync/_build_drop_in.py
```

---

## What you are copying

| Path | Role |
| --- | --- |
| [`drop-in-dxvk_new/`](drop-in-dxvk_new/) | Full replacement for `third_party/l4d2vr/dxvk_new` |
| [`drop-in-dxvk_sources.txt`](drop-in-dxvk_sources.txt) | Replacement for `tools/dxvk_sources.txt` |

Base: DXVK-GPLALL tag `DXVK-GPLALL-2.6.8-2` (`RELEASE` = `2.6.8-2+bmvr`). Vulkan/SPIR-V headers, pregenerated `include/shaders`, and `subprojects/libdisplay-info` were copied from the **current** live `dxvk_new` so MSVC Win32 CMake can compile without meson.

`include/version.h` is `DXVK_VERSION "v2.6.8-2+bmvr"`.

---

## Apply later (implementation step — do not do this until you mean to switch DXVK)

Close `bms.exe`. Keep a git branch/tag of the current stereo-good `dxvk_new`.

From the repo root in PowerShell:

```powershell
# 1. Replace the DXVK tree CMake compiles
Remove-Item -Recurse -Force .\third_party\l4d2vr\dxvk_new
Copy-Item -Recurse .\research\dxvk-gplasync\drop-in-dxvk_new .\third_party\l4d2vr\dxvk_new

# 2. Point the Win32 source list at 2.6.8-2 files
Copy-Item .\research\dxvk-gplasync\drop-in-dxvk_sources.txt .\tools\dxvk_sources.txt -Force

# 3. Then the usual MSVC Win32 configure/build/install of THIS repo's d3d9.dll
#    cmake -A Win32 -B build
#    cmake --build build --config Release
#    scripts\install.ps1
```

Do **not** copy a GPLALL/Ph42oN prebuilt `d3d9.dll` into the game. VR lives inside our compiled DLL (`IDirect3DVR9`, Present → `VR::Update`).

After install, the log should say `DXVK: v2.6.8-2+bmvr`, not `v2.6.1+`. Headset stereo/6DoF is still the acceptance test.

---

## What was merged (so a compile error is traceable)

Copied as-is from live `dxvk_new`:

- `src/d3d9/d3d9_vr.cpp` / `d3d9_vr.h`
- `src/d3d9/d3d9_reshade_vr.cpp` / `d3d9_reshade_vr.h` / `d3d9_reshade_api.h`
- `src/d3d9/d3d9_multithread.h` (exclusive lock + Source frame ownership — PresentEx needs this)

Spliced onto GPLALL 2.6.8-2 (GPLALL D3D9 CPU/bind changes kept):

| File | VR / BMVR hunks |
| --- | --- |
| `d3d9_device.cpp` | Helpers, ReShade depth atlas, overlay draw, Reset/ResetEx windowed skip, CreateTexture eye capture, CreateDepthStencilSurfaceEx MSAA/ReShade, SetViewport eye RT, **entire live PresentEx**, NotifyWindowActivated skip, destructor ReShade clear |
| `d3d9_device.h` | `class VR`, exclusive lock, overlay methods, ReShade atlas members, null-safe `EmitCs` |
| `d3d9_interface.cpp` | `-nohmd` / CreateDevice HMD backbuffer / `Direct3DCreateVRImpl` |
| `d3d9_window.cpp` | Skip exclusive-fullscreen alt-tab helper while VR is on |
| `d3d9_main.cpp` | `D3D9ReShadeVrPrepareConfiguration` |
| `config.cpp` | `bms.exe` profile: `customVendorId=10de`, `deviceLossOnFocusLoss=False` |
| `dxvk_openvr.cpp` | `-nohmd` also sets `m_no_vr` |

GPLALL already had `DXVK_FORCE_WINDOWED` in `d3d9_swapchain.cpp` and FramePacer with `DxvkDevice*` — not re-ported from 2.6.1.

**InScene:** 2.6.8-2 uses `m_inScene`, not `D3D9DeviceFlag::InScene`. Overlay submit uses `!m_inScene`.

---

## New compile units vs current `tools/dxvk_sources.txt`

Added:

- `src/dxvk/dxvk_descriptor_pool.cpp` (replaces removed `dxvk_descriptor.cpp`)
- `src/dxvk/framepacer/dxvk_calibrated_device_timestamps.cpp`
- `src/dxvk/hud/dxvk_hud_item_latency.cpp`
- `src/util/util_unmap.cpp` (replaces removed `d3d9_mem.cpp` / `MemoryFilePool`)
- `src/util/com/com_destruction_notifier.cpp`

Not added: GLFW/SDL WSI (Win32 only).

---

## First compile may still fail

PresentEx was replayed from the 2.6.1 BMVR file. If 2.6.8-2 renamed a D3D9 helper that PresentEx calls, the linker/compiler will name it. Fix that in the drop-in (or after copy), not by swapping in a prebuilt DLL.

Do not enable GPL (`enableGraphicsPipelineLibrary = True`). Keep `deviceLossOnFocusLoss = False`. A/B `dxvk.framePace` starting with `max-frame-latency` (2.6.8-2 default), not L4D2VR’s implicit `low-latency`.

---

## Revert

Do not apply this tree, or restore `third_party/l4d2vr/dxvk_new` + `tools/dxvk_sources.txt` from git and rebuild `v2.6.1+`.
