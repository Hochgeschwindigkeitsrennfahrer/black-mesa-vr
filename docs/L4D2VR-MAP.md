# L4D2VR → Black Mesa map

Inspected `third_party/l4d2vr` for the mechanisms that actually produce VR, then mapped each onto this install.

The project work folder is `src/`, not `L4D2VR/`. `L4D2VR/` exists only because DXVK `#include "L4D2VR/..."`.

## Load

- L4D2VR: replace `d3d9.dll` next to the game exe.
- BMVR: that **and** `bin\` **and** `bin\thirdparty\dxvk-windows-x86\`, because BM’s launcher uses `SetDefaultDllDirectories` + `AddDllDirectory` instead of always `LoadLibraryW` the DXVK full path.

`DllMain` → `InitBMVR` thread → `g_Game = new Game()`. Combined DLL name remains `d3d9`.

## Source access

Same wait + `VClient018`, `VEngineClient015`, `VMaterialSystem081`, `VClientEntityList003`.

`GetMatQueueMode` = `IMaterialSystem` vfunc **11** `GetThreadMode`.

## VR runtime

OpenVR scene app, `WaitGetPoses`, `Submit` + `LockSubmissionQueue`. Vulkan Y-flip on texture bounds.

`CreateDevice` retries L4D2VR `VR_Init` + HMD swapchain size (no `ExitProcess` on failure).

OpenXR (L4D2VR `openxr` branch, not the old BM in-process prototype): Win32 `d3d9.dll` still renders in DXVK. `OpenXRHelper64.exe` is a 64-bit process that creates the OpenXR session (D3D11 and/or Vulkan), imports OPAQUE_WIN32_KMT eye textures over a shared-memory bridge (`L4D2VROpenXrBridgeState`, magic `0x5258344C`, version 12), and calls `xrEndFrame`. Config: `VRRuntimeBackend=openxr`, `OpenXRHelper=true`, `OpenXRHelperSubmitTestFrames=0`. Helper lives at `{game}\VR\openxr_helper64\` with `openxr_loader.dll` beside the exe. If the helper is missing, this build falls back to OpenVR so the desktop game still launches.

`gamePoseValid=0` in the helper log is expected: BM does not publish a separate game-camera pose (`L4D2VR_PublishOpenXrGameRenderPose`), and `useGameRenderPoseForProjection` is off. The helper submits with the OpenXR-located HMD pose.

OpenXR does not use `VR/SteamVRActionManifest`. Helper suggested bindings follow BM G2 (`bindings_hpmotioncontroller.json`): left-stick click sprint, right-stick click recenter/weapon-menu hold, right grip flashlight, X/Y weapon cycle, stick north/south jump/crouch, left menu pause. L4D2VR’s Touch layout (flashlight on right-stick click, recenter on left) is intentionally not used. Crowbar melee needs controller linear velocity; OpenXR poses have none, so the game differentiates consecutive hand positions (OpenVR supplied `vVelocity`).

## Stereo / camera

L4D2VR: two `RenderView` calls with per-eye `CViewSetup` into named eye RTs.

BMVR: **retry that** with BM’s 3-arg `RenderView` (`client.dll+0x207730`). Nested/offscreen calls pass through. If the process dies, `bmvr_in_stereo_rv.flag` falls back to one RenderView + horizontal crop of a captured frame.

Look: HMD yaw/pitch on stereo `CViewSetup` copies after LevelInit gate (reject `background*`). Absolute HMD on the live setup is skipped (`abs_view`). CreateMove writes controller `viewangles` for aiming when the right hand tracks; analog walk is remapped to stay HMD-relative. Viewmodel is uncoupled via `CalcViewModelView` (sd805 / Portal 2).

## Getting the game image

L4D2VR: `CreateNamedRenderTargetTextureEx` (`leftEye0` / `rightEye0`) with `m_CreatingTextureID`. Overlay `AntiAliasing` is DXVK MSAA on those named eyes, resolved into `leftEyeSubmit0` / `rightEyeSubmit0`.

BMVR: retry that **without** poking L4D2’s `isGameRunning` offset (wrong on BM and a likely prototype crash). If it returns null or AVs (SEH), private `CreateTexture` + unbind/pre-Present capture. Scene AA is HL2VR `Msaa` on separate-depth `_rt_FullFrame*` and `_rt_gb*` (resolve-on-sample). `Msaa` in `VR/config.txt` (restart). Do not hook Advanced Video Antialiasing mode. Private eyes stay single-sample. Do not retry named per-eye RTs for MSAA.

Eye size is OpenVR recommended × `RenderScale` (`hmd_offscreen`), not HWND-clamped. Gameplay `_rt_FullFrameFB` / `_rt_gb*` stay at the window (`hmd_world` persist-skip): LITERAL grow to 2544×2480 still PushRT'd 2560×1440, so the HMD showed a warped top strip and garbage below. SetRT redirect only if those RTs actually match the eyes. `steamvr_rt` (redirect without a matching G-buffer) stays persist-skipped.

## Sync

Retry `WaitDeviceIdle` after `TransferSurface`. Crash-sticky disable if the process dies.

## Multicore / queued rendering (L4D2VR `main2`, BM-safe subset)

L4D2VR AutoMatQueueMode issues `ClientCmd_Unrestricted("mat_queue_mode N")` from `VR::Update`. That is **not** copied — Present/CreateMove/RenderView `ClientCmd` crashed BM.

Ported instead:

- Real `GetMatQueueMode` = vfunc **11** `GetThreadMode` **only after a gameplay map is in-game**. Load/menu/`!IsInGame` return 0 without calling into `IMaterialSystem`. Present may call it; **DXVK `SetViewport` / `SetRenderTarget` must not**.
- `SetThreadMode` vfunc **10**, then ICvar `FindVar` slot **15** and a field write of `mat_queue_mode` (`+0x30` xor / string `+0x24`). Never `ClientCmd("mat_queue_mode")` and never virtual `SetValue` (material-thread queue crash).
- Menu/load/pause/first 8 in-game pass-through RenderViews → 0. Gameplay **stays 0** until named per-eye RTs work. **Verified 2026-08-26:** mode 2 + HMD-fb blit = mono/flicker/~180fps. `AutoMatQueueMode` default false. `bmvr.cfg` `mat_queue_mode 0` (ARCHIVE from the cvar write would otherwise reload 2). Do not `SetThreadMode(2)` on the first spawn Present (that hung after pass-through 2/8, 2026-08-18). Windowed swapchain stays at the HWND size while `hmd_swap` is skipped.
- DXVK `SetViewport` only rewrites when RT0 is a private eye/HUD surface. No `IsInGame` / `GetMatQueueMode` from that call (those nested stdshader until Present died, 2026-08-18).
- `GetScreenSize` / `GetBackBufferDimensions` / `CreateNamedRT` keep the HMD size lie whenever the G-buffer exists, including map load. Skipping the lie during `eligible && !IsInGame` made shaders query 2560 while `SetRT` reset the viewport to 2384.
- `SourceRenderQueueBuildScope` around outer `RenderView` when queued (`m_SourceRenderQueueBuildCount`).
- `IMaterialSystem::EndFrame` vfunc **37** only if `vtbl[30] == GetBackBufferDimensions`. Completes queue markers after the original (queued/completed stay 0 unless a later path fills them).

Not ported: ICallQueue ownership markers, queued compositor worker, Neko EndFrame gate, thousands of lines of present-spike / ReShade.

## L4D2-only (not ported)

Melee/terror weapons, workshop, ozz hands, pose relay, Neko HDR post, ReShade VR compat, desktop HUD overlays, queued compositor worker, index-buffer / datacache forcing in `dllmain.cpp`, `ExitProcess` on `VR_Init` failure.
