# Black Mesa VR — agent rules

This project is an L4D2VR-style VR mod for the Steam version of Black Mesa.

Primary architecture reference: https://github.com/keyou91/l4d2vr
Secondary Black Mesa-specific research: https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr

Vendored copies live under `third_party/l4d2vr` and `third_party/black-mesa-vr-prototype`.

Real sources live in `src/`. `L4D2VR/` contains **thin include shims** because DXVK `#include "L4D2VR/game.h"` (and `vr.h`, `sdk/sdk.h`). Do not put new code in `L4D2VR/`.

## Architecture

1. Treat L4D2VR as the primary VR architecture reference wherever possible. Reproduce its overall approach: combined `d3d9.dll` load path, Source engine interface acquisition, OpenVR session, camera/pose, stereo via two `RenderView`s into named eye RTs when that works, `IDirect3DVR9` eye textures, compositor submit, input. Do not invent a separate VR architecture.
2. Use the Black Mesa VR prototype mainly for Black Mesa-specific research and comparison (hooks, offsets, camera, rendering quirks, injection). Do not copy its architecture by default. Prototype crash notes are **hypotheses to retest on this L4D2VR DLL**, not permanent bans.
3. Inspect existing/reference code deeply rather than cargo-culting it. Trace callers, callees, and data flow. When diverging from L4D2VR, document the concrete technical reason in `docs/` **after this build verifies the failure**.
4. Prefer the smallest reliable implementation that follows the proven L4D2VR design. No extra abstraction layers for appearance.
5. Steam launch options already come from L4D2VR. Do not change them unless a verified incompatibility is documented. **Verified:** the launcher video default can be native D3D9, which never loads the DXVK folder. Install next to `bms.exe` **and** `bin\` **and** the thirdparty DXVK path. Document `-enabledxvk` if the user is still on native D3D9. **Verified:** Black Mesa's new game UI is upside-down in the HMD and the world stays black after the load screen. Require `-oldgameui`. Do not Y-flip the 2D capture path to "fix" the new UI — that breaks the old UI.

## Reverse engineering

6. Use Ghidra MCP (`user-ghidra`, and `user-ghidrust` when needed) for static reverse engineering of Black Mesa engine functions, rendering, camera, interfaces, vtables, globals, init paths, and hooks.
7. Do not stop at symbol-name search. Trace functions, callers/callees, and execution flow.
8. Use x32dbg / x64dbg MCP (`user-x64dbg`) for live investigation when Ghidra cannot establish required runtime behavior: loaded modules, actual render/present path, backbuffer sizes/formats, swapchain, threads, camera updates, DXVK vs native D3D, VR init failures.
9. Correlate static analysis with live runtime evidence. If they contradict, trust verified runtime behavior and update the implementation and notes.
10. Avoid speculative hooks when a verified integration point can be found.
11. Keep useful findings (addresses, interfaces, hooks, structures, runtime observations) in `docs/` and `src/` comments that cite evidence.

## DXVK

12. Investigate DXVK explicitly rather than assuming old DirectX-only behavior. Black Mesa on Steam can run with DXVK selected.
13. Do not automatically redesign the project around Vulkan because DXVK is enabled. First establish: what API the game calls, what DXVK translates, where L4D2VR hooks, whether equivalent points exist, and whether engine / D3D9 / DXVK / Vulkan layers need to be used together.
14. Default remains L4D2VR’s combined `d3d9.dll` (DXVK fork + `IDirect3DVR9` + OpenVR Vulkan submit) unless runtime evidence shows it cannot work.

Launcher (`bms.exe` `FUN_00401760` / `FUN_00401620`): on modern Windows it calls `SetDefaultDllDirectories(0xC00)` = `LOAD_LIBRARY_SEARCH_USER_DIRS | SYSTEM32` and **skips** the full-path `LoadLibraryW` of the thirdparty DXVK `d3d9.dll`. `shaderapidx9` then `LoadLibrary("d3d9.dll")`. If DXVK is off, the dxvk folder is never `AddDllDirectory`'d, so a copy only in `bin\thirdparty\dxvk-windows-x86\` never loads. Install to **all three**: that folder, `Black Mesa\bin\`, and next to `bms.exe`.

## Build, test, honesty

15. Build and test continuously. Do not wait until the entire implementation is finished.
16. Never claim something works without actually verifying it.
17. Distinguish status clearly: implemented / compiled / launched / runtime-initialized / head tracking verified / rendering verified / stereo verified / headset presentation verified / gameplay verified.
18. The acceptance criterion is visible Black Mesa gameplay in the headset. Compiling, loading, VR init, and head tracking alone are not success.

## Black Mesa constraints

19. Install the combined `d3d9.dll` at `bin\thirdparty\dxvk-windows-x86\`, `bin\`, **and** next to `bms.exe`. Close the game before overwrite. A running `bms.exe` cannot load a DLL copied after it started.
20. Retry L4D2VR mechanisms on this DLL (named MaterialSystem RTs, double 3-arg `RenderView`, HMD-sized swapchain in `CreateDevice`, `WaitDeviceIdle`, absolute HMD into `CViewSetup`). Crash-sticky flags `bmvr_in_*.flag` next to the loaded `d3d9.dll` disable only the attempt that killed the previous launch. Do not inherit prototype bans without a failure on **this** path.
21. Do not `ExitProcess` if `VR_Init` fails at `CreateDevice` — the desktop game must still launch.
22. Use Black Mesa `bin\openvr_api.dll` only after ABI verification. Prefer SteamVR `bin\win32\openvr_api.dll` beside our `d3d9.dll`. **Verified:** stock `bin\sourcevr.dll` imports old `VR_Init`/`VR_Shutdown`; our OpenVR DLL only exports `VR_InitInternal2`/`VR_ShutdownInternal`. That is the startup “entry point VR_Shutdown could not be located in sourcevr.dll” dialog (twice, then the game continues). Disable `sourcevr.dll` → `sourcevr.dll.disabled`. BM already no-ops `ISourceVirtualReality` when that module is missing.
23. Gate look/input on LevelInit map names (reject `background*`). CreateMove-only gates false-arm on menu maps.
24. Keep user-facing config minimal. `VR/config.txt` lists only settings a player edits; new tunables need a code default and stay out of that file unless a player is expected to change them. `VR/bmvr.cfg` is crosshair-off and `engine_no_focus_sleep 0` only. Do not write Portal 2 launch-option cvars (`mat_motion_blur_*`, `mat_grain_scale_override`, `fps_max`, `mat_antialias`) or video-menu ARCHIVE cvars (`cl_csm_qualitymode`, `nr_shadow_*`, `nr_lights_quality`, `r_shadowrendertotexture`, water, lod, particles) from `ApplyVrQualityOfLifeCvars` or `bmvr.cfg` — those save into `config.cfg` and reset the video preset. `scripts/install.ps1` copies `VR/config.txt` only when the game does not already have one. Do not append or overwrite keys on upgrade.

## Workflow

When uncertain: investigate → verify → implement. Not: assume → implement → hope.

Ghidra → understand static structure.
x32dbg/x64dbg → verify runtime behavior.
Then implement the L4D2VR mechanism against the verified Black Mesa integration point.
