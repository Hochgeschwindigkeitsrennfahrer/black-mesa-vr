# Runtime observations

## Why nothing happened last session

- `bms.exe` PID 17044 started **19:40:51**. Our `d3d9.dll` was copied at **19:49:24**. The running process never mapped the new file.
- `bmvr_log.txt` did not exist — our `DllMain` never ran.
- `bms_d3d9.log` last write **19:01:17** (stock DXVK 2.6.2 gcc). The 19:40 session did **not** write a DXVK log → **native D3D9**, not DXVK.
- Install was only `bin\thirdparty\dxvk-windows-x86\d3d9.dll`. On modern Windows the launcher `SetDefaultDllDirectories(0xC00)` and **skips** full-path `LoadLibraryW` of that file (`FUN_00401470` / `FUN_00401620`). If DXVK is off, that folder is never `AddDllDirectory`'d, so `shaderapidx9` `LoadLibrary("d3d9.dll")` finds System32.

## Load path (what we do now)

Install to:

- `bin\thirdparty\dxvk-windows-x86\` (when DXVK is on)
- `bin\` (USER_DIRS search; added before game root)
- next to `bms.exe` (L4D2VR; game root is also AddDllDirectory'd)

Optional Steam extra: `-enabledxvk` if the launcher video menu is Direct3D 9.

## Verified fail: OpenVR Submit DoNotHaveFocus (101)

2026-08-16 after Reset skip: `OpenVR Submit ... eL=101 eR=101` then unbind capture then crash. 101 is `VRCompositorError_DoNotHaveFocus`. OpenVR throttles WaitGetPoses to 10 Hz in that state. Cause: `GetLastPoses` does not take scene focus. Fix: always `WaitGetPoses`, `CompositorBringToFront`, do not Submit until focus, skip unbind StretchRect, skip `wait_idle`.

2026-08-16 after `hmd_swap` skip: windowed start, capture+Submit of the loading bar to the headset (`eL=0 eR=0`, 1920×1080 from FullFrameFB 2048×2048). Then `IDirect3DDevice9::Reset` with **Windowed=false**, DXVK `Setting display mode: 1920x1080@0` in a tight loop, ~1 FPS, crash on `LevelInit background01`. Crash left the monitor at 1920×1080.

Cause: exclusive fullscreen + SteamVR focus. `d3d9.deviceLossOnFocusLoss = True` in BM's `dxvk.conf` then reports device lost, Source Resets again.

Fix: `DXVK_FORCE_WINDOWED=1` at `DllMain`, and Reset/CreateDevice keep `Windowed=TRUE` **on a local copy** (do not write it back into the game's `D3DPRESENT_PARAMETERS`). Identical 1920×1080 windowed Resets are skipped so Source's fullscreen retry cannot destroy the swapchain and VR eye textures every frame.

2026-08-16 follow-up: after the Windowed force, the game **stayed windowed** but still Reset-looped (`BMVR refusing exclusive fullscreen` then `Device reset` + recreate 1920×1080 eyes until crash). Cause: we mutated the caller's `Windowed` flag to TRUE; Source copied it back, saw mismatch with saved fullscreen settings, and Reset again. `WaitGetPoses` then blocked ~1s because Submit never kept up.

## Verified fail: HMD-sized swapchain (`hmd_swap`)

2026-08-16 live run (PID 39344): CreateDevice + `InitOpenVR` set the swapchain / `m_RenderWidth` to OpenVR recommended **3168×3100**. One capture+Submit succeeded (`eL=0 eR=0`), then `Released VR render targets for device reset`. After Reset, `GetRenderTarget` was null, capture NONE, `submitCalls=0`. Desktop was black; headset stayed in the SteamVR waiting room.

Durable skip: `hmd_swap` in `bmvr_skip.txt`. DXVK `Reset` no longer forces the backbuffer to eye size while that skip is set. Capture falls back to `GetBackBuffer` + `StretchRect` when RT0 is missing.

## Also verified fail: named eye RTs (`named_rt`)

`GetBackBufferFormat` returned a pointer, not an `ImageFormat`. Skipped.

## Verified fail: VR textures during menu (`background01`)

2026-08-16: `IsInGame()` is true on `background01`. Creating `IDirect3DVR9` eye textures then made DXVK `Present` `VrResolveEyeSurfacesToSubmit` / `VrMirrorEyeToDesktopBackBuffer` every frame. Desktop: 1 FPS black load bar; log stopped after the first PrePresent capture (latch) even when the process was still alive. MinHook on `LevelInit` crashed inside the original on `background01`.

Fix: poll `IEngineClient::GetLevelNameShort` (vtable slot 52), reject `background*`, do not create eye textures / capture / WaitGetPoses / Submit until a real map. Keep `LevelInit` unhooked until menu load is stable.

## Verified: 2D capture in headset (bm_c0a0a)

2026-08-16: repeating `OpenVR submit eL=0 eR=0` at ~90 FPS on `bm_c0a0a`. User confirmed headset image. Remaining: no true stereo (same 16:9 blit both eyes), no HMD camera, vertical stretch (16:9 submitted into HMD aspect ~1.097).

## CViewSetup / RenderView (2026-08-16)

- Hooked `client.dll+0x207730` is **not** `CViewRender::RenderView`. Callers push floats; `fld [ebp+8]`. Pass-through was harmless; stereo copies interpreted a float as `CViewSetup&`.
- Real function: `CBlackMesaViewRender` `IViewRender` vtable slot 6 at `client.dll+0x20EE40`, `ret 0xC` (3 stack args).
- BM `CViewSetup` is `0x148`, not L4D2 `0x6E4`. Origin is `0xBC`, fov `0xB4`, aspect `0xE8`. Writing HMD into L4D2 offsets `0x3C`/`0x48` overwrites `m_matCustomViewMatrix`.

## Verified fail: double RenderView (`stereo_rv`)

2026-08-16: after 90 capture Submits at ~69 FPS, named `leftEye0`/`rightEye0` 1920×1878 created and bound. First L4D2VR-style two-`RenderView` into those RTs died with heartbeat `in_stereo_rv`. `CViewSetup` fields were valid (`fov=106.3`, world origin). Crash-sticky disables double RenderView. Look uses HMD angles on the original setup (`angles` at `0xC8`); stretch uses UV crop of the 16:9 capture.

## Stereo copy retry (`stereo_copy`, rejected 2026-08-17)

Two 16:9 `RenderView`s + `StretchRect` into private 1080 RTs. User: performs poorly and is **not fused**. Durable skip. True stereo needs named HMD-aspect eye RTs like L4D2VR / Portal 2 VR.

## Named-RT double RenderView retry (`stereo_rv`, not yet verified)

Portal 2 VR (`Gistix/portal2vr`, L4D2VR fork) does **not** call `PushRenderTargetAndViewport` itself for the world. It `CreateNamedRenderTargetTextureEx("leftEye0"/"rightEye0")`, `SetRenderTarget` on `IMatRenderContext`, then two `RenderView`s. Black Mesa HDR still `PushRT`s `_rt_FullFrameFB` inside `RenderView`.

**Verified fail 2026-08-17:** named RTs created `1920×1878`, stereo began, then the first frame redirected `_rt_gbshadowmaprt` / `static` / `flashlight` **8192×8192** into the eye (viewport forced to 1920×1878, original 8K depth kept). Process died (`in_stereo_rv`). User then played the capture path (`namedRT=0 direct=0`) — same 16:9 blit both eyes, “a bit offset, does not fuse.” That is not L4D2VR stereo.

**Misread, corrected 2026-08-17:** `materialsystem+0x68480` (`ret 24`) is an adjacent **function in the binary**, not the vtable slot before GetRT. `CBlackMesaViewRender_RenderView` (`client.dll+0x20EE40`) calls IMaterialSystem **`+0x19C`** GetRenderContext, context **`+0x1C`** GetRT, then later **`+0x18`** SetRT with that saved pointer (`1020f5e4`). 6-arg PushRT is context **`+0x23C`**; HUD Pop is **`+0x24C`**. Do not call `+0x68480` as SetRT. Do call **`+0x18`** as `SetRenderTarget(ITexture*)`.

**Verified fail 2026-08-17 (morning):** same death with the FullFrameFB-only fix. Log:

```
LevelInit map=bm_c0a0b eligible=1
Eye RT size 1580x1440 … Named eye RTs ready … created=1
Stereo namedRT RenderView begin setup=2560x1440 eye=1580x1440 fov=98.5 aspect=1.097
Stereo PushRT _rt_gbshadowmaprt 8192x8192 redirect=0
Stereo PushRT _rt_gbshadowmapstaticrt 8192x8192 redirect=0
Stereo PushRT _rt_gbshadowmapflashlightrt 8192x8192 redirect=0
```

Heartbeat stuck `in_stereo_rv`. No 4th PushRT (log flushes). Crash is inside the **first** stereo `RenderView` during/after flashlight CSM `PushRT`, **before** FullFrameFB. Common with the 8K-redirect death: `EnsureNamedEyeTextures` from that same `RenderView` (`BeginRTAlloc` + **Release** of in-use capture eye surfaces / Vulkan shared images) then an immediate modified-size `RenderView`. `stereo_copy` (two same-size `RenderView`s, no named RTs) previously survived.

Durable skip of that exact attempt: `stereo_rv` skip-file is **ignored** on this retry. Crash-sticky for the new attempt is `bmvr_in_named_push.flag`.

**stereo_fov (user rejected 2026-08-17):** same-size double `RenderView` + `StretchRect` into 16:9 capture eyes. Different eye poses confirmed; image **not fused**, **squeezed horizontally**, poor FPS. Cause: HMD FOV/aspect drawn into 16:9 pixels. Kept only as fallback if `named_push` crash-stickies.

## Named-RT PushRT stereo retry (`named_push`, verified fail 2026-08-17)

Present-time create **worked**: `leftEye0`/`rightEye0` 1580×1440, `created=1`, then `namedRT=1` on present 604. First gameplay `RenderView` (`setup=2560x1440` tram `bm_c0a0b`, eye 1580×1440, `ctx=12129D18`) died with heartbeat `in_named_push`.

Last flushed lines (not CSM this time):

```
Stereo namedRT PushRT begin setup=2560x1440 eye=1580x1440 fov=98.5 aspect=1.097 ctx=12129D18 ipd=2.36
Stereo PushRT null 0x0 redirect=0
Stereo PushRT null 0x0 redirect=0
Stereo PushRT null 0x0 redirect=0
```

Our L4D2VR `EyeRenderTargetScope` wrap called **original** `PushRT(eye, 1580×1440)` (bypasses the hook, so it does not log), then BM `RenderView` immediately `PushRT(NULL, 0, 0, 0, 0)` three times through the hook. Source `PushRT(NULL)` binds the **backbuffer**. That is a 2560×1440 LDR swapchain + **0×0 viewport** on top of a 1580×1440 HDR eye already on the RT stack. Crash is inside the 3rd original `PushRT(null 0x0)` (log flushes; no 4th line, no FullFrameFB). Viewport/GetViewport clamp never ran on those 0×0 sizes (clamp only shrinks when larger than the eye).

Ghidra MCP was still attached to **`bms.exe`** while both DLL CodeBrowsers were open. PushRT RVA remains `materialsystem+0x6A3D0`.

**named_bind (not play-tested):** rewrite-only, no outer wrap. Incomplete vs L4D2: RenderView's prologue GetRT / SetRT(`+0x18`) restore would put the **backbuffer** back if the eye was never pushed first.

**This retry (`named_l4d`, verified fail 2026-08-17):** wrap + null rewrite + SEPARATE depth. First gameplay `RenderView` logged three `PushRT null 0x0 redirect=1 -> lefteye0 1580x1440` then died (`in_named_l4d`). Next launch auto-persisted `named_l4d` and ran `stereo_fov` (16:9 IPD blit). User: fusion only at close range, world massive and squeezed — that blit, not named-RT stereo.

**named_eye (verified fail 2026-08-17):** SHARED depth + viewport clamp, still **1580×1440** `CViewSetup` / named RT. Same death: three `PushRT null 0x0 redirect=1 -> lefteye0 1580x1440` then crash. Headset image changed at end of load because capture 2560×1440 eyes were replaced with 1580×1440 named RTs, then stereo `RenderView` died.

**Framebuffer-sized named RT (verified fail 2026-08-17):** same wrap + rewrite with `leftEye0` at **2560×1440** (G-buffer size). Log: `Stereo namedRT eye begin setup=2560x1440 eye=2560x1440` then three `PushRT null 0x0 redirect=1 -> lefteye0 2560x1440`. Heartbeat `in_named_eye`. Size mismatch is **not** the crash. `CSimpleWorldView` draws after those three binds; redirecting BM's deferred `PushRT(NULL)` (= backbuffer) onto a separate HDR named RT is what dies.

**hmd_fb G-buffer lie only (verified fail 2026-08-17):** `GetBackBufferDimensions` / `CreateNamedRTEx` made `_rt_FullFrameFB` and `_rt_gb*` **1580×1440**. Swapchain and Present capture stayed **2560×1440**. First gameplay `RenderView` was already `setup=1580x1440`. Then `EnsureStereoEyeSurfaces` `CreateTexture` mid-frame and the process died (`in_hmd_fb`) before `callOriginal`. Deferred G-buffer 1580 + D3D depth/backbuffer 2560 is the mismatch.

**This retry (matching windowed backbuffer, verified fail 2026-08-17 evening):** CreateDevice/Reset rewrote the windowed swapchain to **1576×1440**. Menu survived 1000+ frames (`rt0-vp 1576x1440`, Submit `eL=0`). After `LevelInit bm_c1a0a` the process died before any `RenderView setup=` / `Stereo HMD-fb begin`. Last flushed lines: `HMD-fb stereo: queued cl_csm_enabled 0` then one present (`inGame=0 eligible=1`). WER: BEX `0xC0000005` / Exception Data `00000008` (execute), `engine.dll_unloaded` + `0x0021E110` — same second-chance bucket as earlier post-load deaths, not a useful first-chance site (Ghidra front was still `client.dll`).

Cause (static + this log, not another named-RT wrap):

1. `IDirect3DDevice9::Reset` uses a **local** copy so we do not write `Windowed` back. Load Reset(2560) is skipped against the existing 1576 swapchain. Engine **videomode / `GetScreenSize`** (IVEngineClient slot 5, `engine+0xA6BD0`) stays **2560**. `GetBackBufferDimensions` / `_rt_Hud` / `_rt_gb*` stay **1576**.
2. `client.dll` HUD downsample `FUN_10267420` (xrefs `_rt_Hud`) reads `CViewSetup` `width`/`height` at `param_1[4]`/`[6]` (offsets `0x10`/`0x18`) and PushRTs `_rt_Hud`. Menu skips that path (`iVar3==0`); first in-game HUD does not. 2560 view on a 1576 HUD RT.
3. `ClientCmd("cl_csm_enabled 0; …")` from Present during load (`inGame=0`) is a named-RT CSM leftover and fired at the same moment. `stereo_copy` reached 3D **without** that command. Removed; not required for G-buffer blit stereo.

**GetScreenSize + Reset write-back (2026-08-17, next launch):** hooks installed (`GetScreenSize`). Menu 1584×1440 Submit `eL=0`. After `LevelInit bm_c1a0a`: `GetScreenSize 2560x1440 -> 1584x1440`, `RenderView setup=1584x1440 fov=79`. Videomode split is **fixed**. Then `Stereo HMD-fb begin` `fov=79.0->98.7 zNear=6.0 ipd=2.36` and death inside `Stereo HMD-fb left RenderView 1584x1440` (heartbeat `in_hmd_fb`). First gameplay view was immediately replaced with a double RenderView + HMD FOV/IPD/zNear during spawn.

**WER `engine.dll+0x21E110`:** with `engine.dll` in Ghidra this is `FUN_1021e110` — `SetMiniDumpFunction` / `MinidumpSetUnhandledExceptionFunction` target. It calls `SteamAPI_WriteMiniDump` then `FUN_1006c0b0`. Installed from `FUN_1021d4f0`. Every BM abort lands here; it is not the first-chance fault.

**This retry:** one `RenderView` pass-through for 8 main views at the engine's 1584 setup (no HMD FOV/IPD/zNear), then stereo. If the log stops on `HMD-fb pass-through 1/8`, 1584 world render is the fault. If it reaches `Stereo HMD-fb left RenderView` again, spawn timing was the fault and double-RV/FOV is next. Fusion is **not** verified.

**Pass-through then stereo (2026-08-17):** 8/8 pass-through `setup=1584x1440 fov=79 zNear=7 inGame=1` succeeded. First stereo left RenderView died (`fov=79->98.7 zNear=6 ipd=2.36`). Cause: `NormalizeViewSetupForVREye` wrote eye height 1440 into `CViewSetup+0x1C`. On BM that dword is `m_eStereoEye` (RenderView `cmp [ebx+0x1c], 2` and `byte [eax+this+0x744]`). Pass-through left it 0 (mono). Stereo turned it into an OOB index. `m_nUnscaledHeight` is at `0x20`. Also stop forcing L4D2 `zNear=6` / viewmodel FOV.

**Headset fusion (2026-08-17, user-verified):** after the `m_eStereoEye` fix, stereo at 1584×1440 **fuses**. Image was upside-down. Cause: direct HMD-aspect Submit applied `ApplyVulkanYFlip` on projection UVs. The same `StretchRect`→`TransferSurface` capture path is upright with `{0,0,1,1}` and no v-flip (old UI). Do not Y-flip that blit for Vulkan. Per-frame `Stereo HMD-fb left/right RenderView` logs were flushing twice a frame (~12 FPS); throttled.

**L4D2VR/Portal2 look + recommended size (2026-08-17, crashed):** fused HMD-aspect stereo made relative look cancel against physical head motion. This retry used L4D2VR angles on copies plus `SetViewAngles`/`CreateMove`, and OpenVR recommended G-buffer **2544×2480**. Menu ~90 FPS and **8/8 pass-through** `setup=2544x2480 zNear=7` succeeded. Log stopped there — no `Stereo HMD-fb begin`, no `Applied Portal 2 VR perf cvars`. First stereo frame ran `ClientCmd_Unrestricted("mat_queue_mode 0;…")` *before* that log. Portal 2 sets those as **launch options**, not from `RenderView`. Same first-stereo crash bundle previously included forcing `zNear=6` / viewmodel FOV. Do not ClientCmd cvars from RenderView. Keep engine zNear. Native G-buffer itself survived pass-through.

## Verified fail: absolute HMD on live CViewSetup (`abs_view`)

2026-08-16: load logo submitted (`eL=0`). After ~90 Submits we wrote HMD yaw/pitch onto the live `CViewSetup`. On `bm_c0a0b` the tram camera was `ang=(4.6, 96.5)` and became HMD `(-0.5, 14.7)` — looking off the train into unlit void. Headset went black; logo had been visible because look/crop only started after load. Durable skip: `abs_view`. L4D2VR applies HMD only on stereo view copies, not the caller’s setup.

## Verified fail: projection UV on 16:9 capture (WMR portal 2026-08-16)

Headset / Mixed Reality Portal showed a **left vertical strip** of the pause menu, **upside-down and mirrored**, rest black. Desktop pause menu was upright 16:9. Cause: capture Submit used `GetProjectionRaw` / L4D2VR hidden-area `m_TextureBounds` meant for an HMD-aspect eye RT. On a 16:9 D3D blit those UVs sample a flipped corner. Known-good in-headset session used full-frame capture Submit `{0,0,1,1}` (stretched, both eyes the same). Extra `vMin`/`vMax` swap on that blit submitted upside down.

Fix: capture path Submits `{0,0,1,1}`. Projection bounds stay for a future direct-eye / named-RT path only.

## Menu compositor retry (`menu_vr`)

Main menu never appeared in VR because capture/`WaitGetPoses`/Submit were gated on `m_GameplayEligible` (reject `background*`). That gate was from an earlier 1 FPS hang when DXVK `VrResolveEyeSurfacesToSubmit` ran with `m_StereoRenderViewActive`. The capture path sets that false.

**2026-09-03:** skip-file and crash-sticky `menu_vr` are both ignored. Quitting GameUI used to leave `bmvr_in_menu_vr.flag` and the next launch disabled Submit (`menuVR=0`, helper `submitted=0`, black HMD). Look/CreateMove stay off until a gameplay map.

**Verified miss 2026-09-03:** `-oldgameui` never loads `background01`. Log: ~11k Presents at ~288 fps, `inGame=0 eligible=0 map=` `createdRT=0`. Requiring a LevelInit map name for capture/Submit left the HMD black until `LevelInit bm_c2a5c`. Submit now always runs on the no-map GameUI; capture still no-ops if the backbuffer is tiny. Do not CreateNamedRT `bmvrHUD` until a gameplay map. Point the right controller at the captured 2D menu; trigger/MenuSelect clicks. Left-menu/Pause is Escape (back).

**Verified crash 2026-09-03:** empty-map Submit worked (CreateVRTextures, OpenXR publish, ~286 fps to present 704). Process died on the first `Update` after `controller poses L=1 R=1`. Cause: VGUI `IInput` from the DXVK Present thread. Cursor is HWND `SetCursorPos` + throttled mouse messages only. `BeginRisky(menu_vr)` only covers `CreateVRTextures`.

**Verified miss 2026-09-03 (black HMD, desktop cursor worked):** OpenXR left `m_FrameCopyLatched` set, then `CreateVRTextures` replaced the 2560×1440 capture with an empty 3168×3104 RT and kept submitting it. Capture stays HWND-sized; letterbox into the eyes; clear the latch after publish. Left stick = arrows, A = Enter, B = Escape. Do not Y-flip this 2D path.

**Verified miss 2026-09-03 (still black, no Submit):** previous GameUI exit left `bmvr_in_menu_vr.flag` (risky stayed armed until LevelInit). Next launch logged `Disabled menu/background compositor Submit this launch only`, `menuVR=0`, `createdRT=0`. Helper: `Waiting for shared game eye textures`, `submitted=0`. Crash-sticky for `menu_vr` is ignored; BeginRisky only covers `CreateVRTextures`.

**Pause GameUI 2026-09-03:** Direct-eye Submit blit the BB into the eyes, then OpenXR letterboxed a stale frame copy over them. Desktop showed pause; HMD did not. Pause now uses the same HWND 2D panel as the main menu (cursor drawn in). Do not extra-paint VGUI onto `bmvrHUD` for pause.

**Menu follow-head 2026-09-03:** 2D Submit published the live HMD pose every frame, so the letterboxed GameUI stuck to the visor. Latch that pose when the panel appears; look around it. Recenter clears the latch.

**Menu tilt / 2D after save 2026-09-03:** Latch was the full HMD quaternion, so head roll made the panel crooked. Store yaw-only (world up). After a save load, `IsPaused()` or our `m_GameUiVisible` latch kept the 2D panel up while desktop GameUI was already gone. In-game 2D is `VEngineVGui001::IsGameUIVisible` (and our pause toggle), not `IsPaused`. LevelInit clears the pause latch.

## WaitGetPoses on Present thread (verified hang 2026-08-16)

PID 19580 froze with `Responding=False` after `LevelInit map=bm_c0a0b`. No further present ticks. Next call on that thread is `WaitGetPoses`. Same hang when the headset is taken off (WMR stops vsync; WaitGetPoses never returns). Low FPS then freeze is WaitGetPoses blocking, then never returning.

Fix: dedicated pose-waiter thread + `SetExplicitTimingMode(Explicit_ApplicationPerformsPostPresentHandoff)` so WaitGetPoses does not touch the Vulkan queue. Present only consumes the last pose and Submits; if the waiter stalls >500ms, skip Submit **and** `PostPresentHandoff` so the desktop game keeps running (headset may go waiting-room). `rel_look` skip-file entries from this hang are ignored (look was not the cause).

Do not pause WaitGetPoses during map load — a previous attempt to skip it when the window looked hidden froze after the load screen. Keep the waiter running across LevelShutdown; just keep it off the Present thread.

## Tab-out (2026-08-16, verified in log)

Latest crash: `bm_c0a0b` at ~90 FPS, then 62→34→**15 FPS** while OpenVR Submit still returned `eL=0`. No `Released VR render targets`, no DXVK `Device reset`. DXVK's `WM_ACTIVATEAPP` handler **minimizes** the game (`ShowWindow(SW_MINIMIZE)`) unless `D3DCREATE_NOWINDOWCHANGES`, then `WaitGetPoses` keeps running against a minimized swapchain until a later tab-out dies.

Fix: skip that exclusive-fullscreen minimize/`SetWindowPos` while a VR session is active. Do **not** skip `IDirect3DDevice9::Reset` for tiny/iconic sizes and do **not** drop `WaitGetPoses` when the window looks hidden — those two guards froze the game after the load screen (log stuck on `background01` at 90 FPS, process alive, never reached map `LevelInit`). Map load Reset must run. Keep calling WaitGetPoses so the compositor does not hold the GPU queue.

Stock `bin\thirdparty\dxvk-windows-x86\dxvk.conf` still has `d3d9.deviceLossOnFocusLoss=True`. `NotifyWindowActivated` returns immediately while VR is enabled.

## `-oldgameui` (verified 2026-08-16)

Black Mesa's **new** game UI is upside-down in the HMD and the world stays black after the load screen. **`-oldgameui`** shows an upright menu and the actual game in the headset. Do not Y-flip the 2D capture path to chase the new UI — that would invert the working old UI. New UI is a different RT / Y origin; leave it.

## Head look (L4D2VR / Portal 2)

Head tracking cannot move the desktop unless the **rendered** camera changes. Absolute HMD on the live `CViewSetup` (`abs_view`) is still skipped — that replaced the tram camera and blacked the headset. L4D2VR/Portal2 write HMD yaw/pitch and eye origin on **stereo copies** plus `IEngineClient::SetViewAngles` and CreateMove `viewangles`. Relative look (`HMD - latched` on the copy) is what fused stereo made feel broken: the world rotated with the headset. Crash-sticky `bmvr_in_rel_look.flag` is cleared after the 8 pass-through frames. CreateMove is hooked but is a no-op on `background*` maps.

## Confirmed fused gameplay (2026-08-17 evening)

User-verified fused stereo on `bm_c1a0a` at **1584×1440**, `zNear=7`, `direct=1`. `hmd_native` (2544×2480) stayed skipped.

Log issues on that session (addressed in the following build, not re-verified):

- `OpenVR submit … eL=108 eR=108` = `VRCompositorError_AlreadySubmitted`. BM issues many main-sized `RenderView`s per Present; each ran a full double eye `RenderView` + Submit. Stereo is now **once per Present**; extra main views pass through.
- `Stereo HMD-fb enter` was unthrottled (one line per extra `RenderView`). Throttled to the first few.
- Present ~42 then ~24 FPS, `WaitGetPoses dt=110ms`, `poseAge=62ms` — extra stereo Submit was a large part of that.
- Analog walk existed in CreateMove but `m_ProcessInputEnabled` stayed false and no SteamVR action manifest was loaded.

## RenderScale (`VR\config.txt`)

`1.0` is OpenVR `GetRecommendedRenderTargetSize` (SteamVR overlay SS), 16-aligned, cap 4096. **Not** clamped to the HWND. Eyes use that size (`hmd_offscreen`). World `_rt_FullFrameFB` / `_rt_gb*` stay at the window (`hmd_world` persist-skip). `GetScreenSize` and the swapchain stay at the window. gbmatch squash-blits the window scene into the eyes.

Restart after edits. `hmd_native` / `hmd_swap` remain skip-file disabled.

## Frame time (2026-08-26)

Open/complex maps were drawing the whole vis set: QoL forced `r_occlusion 0` + `r_portalsopenall 1` after a stereo areaportal latch. Default is PVS + occlusion again (`ForceOpenVis=true` restores the old pair). `r_visocclusion` is a cheat/debug overlay, not HW occlusion — keep it `0` (`1` was a mistaken default and is a Xen crash suspect). Each stereo eye also ran BM new-renderer CSM (`cl_csm_qualitymode` / `nr_shadow_*`; logs showed `_rt_gbshadowmaprt` 8192²). Lowest quality + `nr_shadow_res` 2048 + one shadow pass. **Surface Tension (`bm_c2a5a`):** the stereo-cost / CSM-quality-0 build was the last one the user called a bit better (~38–46 fps combat, pair ~26 ms, ~110 NPCs). Later cuts (`cl_csm_enabled 0`, `nr_shadow_active 0`, shoot/dlights off, then `nr_dev_gb_debug_type 1` + `r_lod 2`) were **unplayable**: `gbDepth` stayed 4 (Fast2 never switched), combat ~27–32 fps, uniq ~150, one pair 62.7 ms with DME orig 51.7 ms. Those cvars are reverted; do not retry. Do **not** set `nr_lights_active 0` (flashlight is `__GBLightSpot_FlashLight`). L4D2VR `r_flashlightdepthtexture` is **not** copied. Multicore still off.

## AntiAliasing (`VR\config.txt`)

L4D2VR overlay key, not `mat_antialias`. `0` / `2` / `4` / `8` / `16`. Restart after edits. DXVK stamps D3D9 MSAA on `CreateTexture` for `Texture_LeftEye` / `Texture_RightEye`, then `StretchRect` resolves into non-MSAA submit textures that OpenVR can consume. Engine video AA stays off.

On the current HMD-fb **blit** path the world is rasterized into the window backbuffer first, so MSAA on the eye copy does not anti-alias geometry. Gloves drawn into the eye RTs still get MSAA. Named per-eye RTs (verified fail here) are what made L4D2VR's world MSAA real.

## SteamVR controllers (L4D2VR/Portal2 actions)

`SetActionManifestPath` → `Black Mesa\VR\SteamVRActionManifest\action_manifest.json`. `UpdateActionState` after `WaitGetPoses` on the pose-waiter thread. CreateMove applies Walk (`forwardmove`/`sidemove` ×450) and held `IN_USE`/`IN_ATTACK`/etc. Right stick adds `m_RotationOffsetY` to HMD yaw (Portal 2 `TurnSpeed`).

G2: `controller_type` `hpmotioncontroller` (copy of Touch). WMR: `holographic_controller`. Also Touch, Knuckles, Cosmos, Vive. Quest 3 / Pico / Focus 3 reuse the Touch defaults (`oculus_touch_plus`, `pico_controller`, `vive_focus3_controller`) so Pause stays on left Y (and left system). OpenXR Quest 3 needs `XR_META_touch_controller_plus` or Pause never binds — helper log 2026-09-08: `meta/touch_controller_plus` was `PATH_UNSUPPORTED` until that extension was enabled.

After launch, `bmvr_log.txt` next to `bms.exe` and next to the loaded `d3d9.dll`.

G2 left Y was Pause (`gameui_activate`). Log 2026-08-18: `Pause queued on CreateMove (gameui_activate)` then a new process (`VR config`). Same crash as Present-thread `ClientCmd`. Do not `gameui_activate` from any thread.

G2 left X and Y after remapping to NextItem still crashed (fusion session, no `Ignoring Pause` line). Both buttons were `invnext` via `ClientCmd_Unrestricted` from CreateMove. Same ClientCmd bucket. Weapon cycle now sets `CUserCmd::weaponselect` from `DT_BaseCombatCharacter::m_hMyWeapons` (`0xEE4`, 48 handles) / `m_hActiveWeapon` (`0xFA4`). Left Y next, left X previous.

## Uncoupled viewmodel / motion aim (user-verified 2026-08-18)

Copied **sd805/l4d2vr** + **Gistix/portal2vr**, not keyou91 hands/reload/dual-wield models.

- `CalcViewModelView` feeds `GetRecommendedViewmodelAbsPos/Angle` from the right controller. Same world pose both eyes (IPD on the gun drew two weapons). Fallback is still head-locked if the controller pose is invalid.
- Camera stays HMD on stereo `CViewSetup` copies. CreateMove sets `cmd->viewangles` from the controller so hitscan follows the gun direction. Analog walk is rotated from HMD yaw into controller yaw so the stick stays look-relative.
- Portal 2 keeps HMD `viewangles` and overrides `Weapon_ShootPosition` / `EyeAngles` only around fire. Those symbols are stripped here; do not hook `EyePosition` (that would move the camera to the gun). Bullets still spawn from the eyes.
- Config: `ViewmodelPosOffsetX/Y/Z` (default 16, 3, −2), `ViewmodelAngOffsetX/Y/Z`, `ControllerPitchTilt` (default −35; sd805 Vive wands used −45).

## Near fusion (HMD-aspect blit, 2026-08-18)

Unbind blit is 2384×2160 `fmt=21` (`A8R8G8B8`) into private eye surfaces. Incoming engine fov stays 79.2; copies use 98.5. `GetViewModelFOV` hook (user: weapon size better) does not by itself fix world fusion.

Both eyes share `_rt_FullFrameFB`. `StretchRect` is queued; the right-eye `RenderView` can overwrite that RT before the left copy lands, so both eyes submit the same image and near field cannot fuse. Fix: `D3DQUERYTYPE_EVENT` flush after each eye blit — not `WaitDeviceIdle` (`wait_idle` load crash). User-verified 2026-08-18: near fusion looks correct.

## Live debugging

Use **x32dbg** (Black Mesa is 32-bit). Attach to `bms.exe` and check the path of loaded `d3d9.dll`. If it is `C:\Windows\System32\d3d9.dll`, our code is not running.

## Load-to-menu hang (2026-08-18)

Stuck on the loading plaque, never reached `LevelInit background01`.

First hypothesis (two `d3d9.dll` mappings / `DrawModelExecute`) was incomplete: after a process-wide init mutex and leaving `DrawModelExecute` unhooked, the hang remained.

x32dbg on the Steam process (PID 25076): main thread in `ntdll.ZwReadFile` of `bms\bms_textures_033.vpk`, callers `materialsystem` → `stdshader_dx9` → DXVK `SetRenderTarget`, repeating. CPU was busy; Present had run once. Skipping `RenderScale` for the engine G-buffer did **not** unstick load (user 2026-08-18); that change was reverted.

Same hang at 1584×1440 (PID 21052) and again PID 38376: main thread in `d3d9` (`NtClose` / `memset`) from nested `stdshader_dx9` / `SetRenderTarget`. `GetMatQueueMode` logged 0 (queued Present lock was not on). Two load-time calls from the main2 port:

- `GetThreadMode` vfunc 11 during empty-map Present
- DXVK `SetViewport` rewriting every `!IsInGame` viewport to `m_RenderWidth` (2384×2160) during precache

Queued mode is unchanged for gameplay: `AutoMatQueueMode` still `SetThreadMode(2)` after a non-background map is **in-game**. Load/`!IsInGame` return 0 without touching `IMaterialSystem`. The L4D2VR `!IsInGame` HMD `SetViewport` rewrite is off on BM. `GetScreenSize` / `ClampStereoViewport` also stay off until `IsInGame` — map load sets `eligible=1` while `IsInGame` is still false, and those hooks recreated the nested `SetRenderTarget` freeze after the menu (2026-08-18).

## Map-load hang after menu (2026-08-18)

Menu `background05` ran ~90 FPS with OpenVR submit. Starting a game: `LevelInit map=bm_c1a0a eligible=1` with `inGame=0`, a few Presents at ~21 FPS, then Present stopped. Desktop eventually a solid gray Windows not-responding frame.

x32dbg PID 37216: same nested `materialsystem` → `stdshader_dx9` → DXVK. CIP in `SetViewport` **after** `Game::GetMatQueueMode()` (device lock held). Overnight L4D2VR `SetViewport` called `IsInGame` + `GetThreadMode` on every viewport change. Size-lie skip during `eligible && !IsInGame` was a misdiagnosis: G-buffers stay 2384×2160 from the empty map, so passing through 2560×1440 `GetScreenSize` dirties the viewport on every `SetRT`. Fix: no engine calls from `SetViewport`; keep the HMD size lie for load; `SetRT` capture only during stereo blit.

After that, load reached `bm_c1a0a` in-game (PID 33612). Picture-in-picture warehouse, then the window resized, then a black hung window. Log: `AutoMatQueueMode set 2` three times on the first in-game Presents, `GetScreenSize 1920x1080 -> 2384x2160`, pass-through 2/8, then silence. `BmvrForceHmdAspectWindowedBackbuffer` Reset the HWND to 2384×2160 (`hmd_swap` failure mode) and released VR RTs. `SetThreadMode(2)` waits until 8 pass-through RenderViews finish. Swapchain size stays at the game window when `hmd_swap` is skipped.

PID 27004: menu smear + PiP, then hang after spawn. Pass-through 8/8 at `2560x1440`, then stereo `2384x2160` from `1920x1080` setup; first stereo Present completed, second died. `GetScreenSize 2560x1440 -> 2384x2160` on a 1440-tall HWND is the bottom smear (menu too). `RenderScale` 1.5 must not make the engine G-buffer taller than the window; fit HMD aspect in the HWND (~1584×1440).

## Spawn hang + split desktop (2026-08-18)

PID 16108: pass-through 8/8 and the first stereo frame completed at **1584×1440**, then `Eye/G-buffer size 1200x1072` and Present died. Desktop was the tram in the left ~62% of a 2560×1440 window (`1584/2560`) with the loading UI still in the right 40%, title Not Responding.

Cause: spawn Reset shrank `GetClientRect` to 1920×1080 while eyes/G-buffers were 1584×1440. Live-clamping `HaveHmdFramebufferSize` to the HWND made `EnsureStereoEyeSurfaces` null the 1584 pointers without Release and `CreateTexture` 1200×1072 mid-stereo. Engine Present copies the 1584 G-buffer 1:1 into the 2560 swapchain, so the unused region kept the load screen.

Fix: latch G-buffer size at CreateDevice; do not recreate existing eye textures when the HWND changes; ColorFill the unused desktop backbuffer during in-game RenderView (not the menu).

PID 32704 (2026-08-18): 1200 rebuild gone. Pass-through 8/8 and the first stereo pair completed at 1584×1440 (`setup=1920x1080` copies). Present then died. x32dbg: main thread `rep movsb` in `materialsystem` from DXVK `Present` → `Game::GetMatQueueMode` → `GetThreadMode` vfunc 11 with the device lock held (same nest as overnight). Pass-through 8 armed that probe. Spawn also issues a leftover 1920×1080 main `RenderView` after the stereo pair.

Fix: `GetMatQueueMode` is a cache filled from RenderView only; Present must not call into `IMaterialSystem`. Skip leftover main RenderViews after stereo this frame. `mat_queue` skip still means no `SetThreadMode(2)` until that retry is on.

## Desktop split + black HMD (2026-08-18)

PID 25200 stayed responding at ~40 FPS. Desktop tram occupied the left ~62% of a 2560-wide window; the rest was ColorFill black (`1584/2560`). SteamVR showed Standing by. OpenVR Submit often returned `eL=0` mixed with `108 AlreadySubmitted`. Stereo unbind blit logged fmt 111 (`D3DFMT_R16F`) then fmt 35 then fmt 21 last-wins.

Not headset-verified. Code now: keep the highest-rank scene-color blit (skip R16F; do not let later A8 HUD overwrite A2R10/RGBA16F); wait the blit event query up to 8ms instead of 256 tight `S_FALSE` polls; StretchRect the left eye across the desktop backbuffer after the stereo pair; Submit once per WaitGetPoses when a new stereo frame exists.

That stretch copied fmt=35 `A2R10G10B10` (rank 3) into the eyes and over the full backbuffer — both desktop and HMD went black. The tram the user had seen was the engine's top-left 1584×1440 resolve on the 2560 backbuffer, not that HDR unbind. Next: copy that backbuffer crop into each eye, and only then stretch to the desktop. Revert the 15ms event-query wait (`GetTickCount` granularity logged `S_FALSE` every eye).

## Tall image, fisheye, black pillar, menu mouse (2026-08-18)

User-verified: gameplay visible again, but 1584×1440 G-buffer lie inside a 2560×1440 window. Desktop left ~62% tall/fisheye (~99° OpenVR FOV), right third black. VGUI layout 1584 vs HWND 2560 so menu clicks miss. Head-turn warp from that FOV plus GetProjectionRaw UVs on an already-cropped blit.

Change: stop rewriting `GetScreenSize` / `GetBackBufferDimensions` / `CreateNamedRT` to HMD aspect. Stereo copies keep engine 16:9 size and FOV. Center-crop the backbuffer into the private eye textures. Direct Submit `{0,0,1,1}`. Not headset-verified yet.

User-verified the next day: fusion only at arm's length, world giant, camera above NPC eye level. That 16:9 center-crop displayed ~50° of a 99° HMD frustum while IPD stayed ~2.5in (L4D2VR `GetProjectionRaw` scale). Source SDK VR (`client_virtualreality.cpp`) uses off-center / `m_ViewToProjection` overrides; BM `CViewSetup` is 0x148 and `+0x1C` is `m_eStereoEye`, so do not port those fields. Restore L4D2VR on the **copies only**: `width/height/fov/aspect` = HMD, Submit `m_TextureBounds`, top-left 1584×1440 blit. Do not permanently rewrite videomode (menu mouse). Recenter latch ignores tracking Z≈0 so standing height is not stacked on `setup.origin`. User-verified 2026-08-18: thumbstick recenter fixes height; fusion restored.

## 6DOF roll / viewmodel feet / SteamVR res (2026-08-18)

- L4D2VR `GetViewAngle` includes HMD roll. We had been zeroing `ang.z` and clamping pitch to ±89, so a head tilt rotated the compositor pose against an unrolled image. Camera copies now use `HmdMatrixToSourceAnglesWithRoll`. CreateMove still writes roll=0 so the body does not roll.
- Viewmodel used `controller - hmdZero` on the engine eye input, which after recenter put the gun at the player's feet. L4D2VR 1:1 is camera + `(controller - current HMD)`. Same here, with a 80hu reach clamp (Portal 2 prototype).
- `steamvr_rt` (SteamVR recommended 3296×3216 private eyes + `SetRenderTarget`/`SetDepthStencil` redirect, skip backbuffer blit) is **verified black**. Process stayed at ~90 FPS, audio played, Escape menu drew, world was pitch black on desktop and HMD. Engine G-buffers stayed 2560×1440; `redirected=1` stole the composite off the window backbuffer. Persist-skip `steamvr_rt`. Do not retry redirect unless FullFrame **and** G-buffer actual size match the eyes (`hmd_offscreen`).

## Verified fail: `ff_gbfit` (2026-08-22)

LITERAL `_rt_FullFrameFB*` and `_rt_gb*` at 1584×1440 (PICMIP 1024 downsamples left alone). Alloc logged `actual 1584x1440`. Process died on `background04` before any stereo pair (`BeginRisky` still set). User: miss. Persist-skip `ff_gbfit`. Same class of failure as `ff_hmdfit` (do not resize world RTs to eye size). Also do not retry 16:9 stereo at 2560 — flashlight returned but fusion/stretch broke.

## Multicore / QoL / motion polish (compiled 2026-08-18, not user-verified)

Overnight port of the L4D2VR `main2` multicore **subset** plus remaining safe QoL. Do not treat as headset-verified.

- `GetMatQueueMode` returns 0 until gameplay is eligible, then calls vfunc 11. DXVK's queued Present exclusive lock stays off during load.
- `AutoMatQueueMode` uses `SetThreadMode`, not `ClientCmd`. First switch to mode 2 writes `bmvr_in_mat_queue.flag`. If that launch dies, next launch skips auto-queue (`mat_queue` in `bmvr_skip.txt`).
- Per-weapon viewmodel offsets from `v_` model names; haptics; `IPDScale` / `HeightOffset`; `LeftHanded`; recenter zeros yaw. `DrawModelExecute` stays unhooked (overnight createHook coincided with the load freeze; ABI unverified).
- ICvar: BM exports `VEngineCvar004` with the 2013 vtable. `FindVar` is slot **15**. ConVar `GetName` is slot **4**. Do not call virtual `SetValue` — `FCVAR_MATERIAL_THREAD` queues through `IMaterialSystem` vt+0x88 and crashed on `mat_vsync` (first RenderView, 2026-08-26). Console value is the xor'd int at `+0x30` and the string at `+0x24`. Never scan ICvar slots (2026-08-18 hang). Crosshair stays in `bmvr.cfg`.
- **Verified fail (2026-08-26):** `mat_queue_mode` 2 + HMD-fb backbuffer blit = **mono, permanent shake, flicker, ~180fps stutter**. Both eyes StretchRect the same 2560×1440 window after queued RenderViews. L4D2VR survives queue 2 because named per-eye RTs run on the material thread; those RTs are skipped here. `AutoMatQueueMode=false`, `bmvr.cfg` `mat_queue_mode 0`. Do not retry 2 until each eye has its own RT or ExecuteQueued-after-eye is proven on this blit path.

## HUD / pause / sprint / viewmodel scale / crowbar melee (compiled 2026-08-18)

- HUD/pause VGUI is laid out for a ~60° inset inside the 1584×1440 HMD crop (Source `vr_hud_max_fov` idea) so lens FOV does not clip the edges. Pause menu is copied from the backbuffer into both eyes at Present.
- Left menu/system button posts `VK_ESCAPE` to the game window (not `gameui_activate`). Trigger aims a VGUI cursor at that inset while paused.
- Sprint is `IN_SPEED` on left X (G2/Touch). Crowbar swing synthesizes `IN_ATTACK` from controller speed (BM has no L4D2 `TestMeleeSwing`).
- Viewmodels write `C_BaseAnimating::m_flModelScale` at +0x7C0 (Ghidra DT_BaseAnimating). Default `ViewmodelScale=0.5`.
- **SteamVR overlay SS still cannot enlarge BM G-buffers past the HWND.** `hmd_world` persist-skip. Eyes can still be the recommended size; unmatched G-buffers upscale from the window blit.
- **Verified miss (2026-08-26, user):** FullFrame + G-buffer + eyes all 2544×2480 (`worldMatch=1 redirected=1`). Engine PushRT/Viewport stayed **2560×1440** (rewrite never logged). HMD: warped top strip + garbage/black below. Desktop letterbox copied that broken eye. Persist-skip `hmd_world` — do not LITERAL-grow world RTs taller than the HWND. Eyes can still be SteamVR rec; gbmatch squash-blit from the window. Native pixels above the window need `hmd_swap` (already persist-skipped, black desktop) or a larger game window.

## Quest 3 + Horizon Link / Oculus OpenXR 2D menu double (2026-09-03)

User: Meta Horizon Link + Oculus OpenXR. **In-game stereo is fine.** 2D main menu / pause is **double**.

The 2D path letterboxes the same HWND capture into both eyes with `POSE_FLAG_MONO`. First helper attempt copied left pose+FOV onto the right projection view. Logs confirmed that ran (`OpenXR 2D menu/pause: identical pose+FOV both eyes`); the tester still saw double. Matching a stereo **projection** layer is not enough on Oculus 1.207 — the runtime still maps each eye with its native frustum.

Next: when `POSE_FLAG_MONO`, skip the projection layer and submit one `XrCompositionLayerQuad` (`XR_EYE_VISIBILITY_BOTH`) from the left eye swapchain. HUD overlays stay hidden during 2D. In-game stereo is unchanged.

**2026-09-03:** first quad used pause-HUD 1.15 m × 1.35 m and a 16:9 crop of the portrait swapchain. The 0.70 letterbox sat at ~43° and felt far. Quad is now 0.65 m × 1.05 m (~78°) cropped to that letterbox. Confirm in the helper log:

    OpenXR 2D menu/pause: quad layer both-eyes ... dist=0.65m; stereo projection skipped
    [OpenXR][EndFrameSubmit] ... projection=0 ... overlays=1 mono2dQuad=1

Do not Y-flip the 2D capture. Do not turn stereo eye-swap back on (Link duplication is not L/R swap).

## Quest 3 + Steam Link (2026-09-03)

Runtime name: `SteamVR/OpenXR in Meta compatibility mode` (SteamVR 2.16.7). Auto Y-flip treated that as SteamVR+Touch and enabled the vertex NDC Y-flip (`flipY=1 ndcYFlip=1`). User view in the Quest was **upside-down**. Steam Link uses Meta's image Y (same as Oculus/Link, which must not flip). Auto-flip now returns off when the runtime name contains `Meta compatibility`. Virtual Desktop → SteamVR (not this string) still flips. Do **not** Y-flip the 2D/menu capture path.

## Quest 3 + Virtual Desktop / SteamVR OpenXR (2026-09-01)

User-reported: Quest 3 via Virtual Desktop, world **upside-down** when Streamer Options → OpenXR Runtime = **SteamVR**. Same machine upright when that dropdown is **VDXR**. Windows ActiveRuntime can still be `virtualdesktop-openxr.json` in both cases; the helper's `xrGetInstanceProperties` runtime name plus VD Settings.json/registry decide the blit.

Do **not** Y-flip the 2D/menu capture path (`AGENTS.md`).

**Verified fail (2026-09-01, G2):** SteamVR OpenXR + a **negative viewport** produced yellow/white bands. WMR OpenXR (no flip) was fine.

**Quest logs 2026-09-01 (`bm_c1a0a`, unique L/R origins):** SteamVR fused but inverted. Link/Oculus duplicated. Auto image-swap did not fix Link. Transfer-blit dstOffsets Y-swap did not change pixels (NVIDIA min/max). Shader UV vMin/vMax swap (`path=shader flipY=1`) also left SteamVR inverted. Next attempt: second blit VS that **negates NDC Y** only for SteamVR+Touch (not G2, not negative viewport). Oculus crops the blit to L4D2VR hidden-area UVs, fills the swapchain, and still submits runtime FOV. Do not crop SteamVR (it fused with full 0–1 + game FOV).

Quest / Touch hands sitting **below** the controllers: `ControllerPitchTilt=-35` is a G2/WMR grip correction. Touch OpenXR **aim** already points; extra −35° pitches the gun and gloves down. Per-family table: Touch tilt 0 + aim pose for weapons / grip for gloves; G2 keeps −35 and grip. Log line: `Controller tracking ... tilt=0.0 family=touch`. Touch weapons get extra `ViewmodelPosOffsetXTouch=5.5` (aim origin is ahead of grip). Extra glove meters: `QuestHandsPoseOffsetMeters`.

## Verified: stereo bloom skip (2026-09-03)

Anomalous Materials cafeteria (`bm_c1a0a`): fluorescent-tube copies on the dark ceiling and a table/NPC stamp on the olive wall. Same leftover-pixel class as the old 16:9 fires (square HMD eye, window 2560×1440).

A/B: god rays on, `bms_postprocess` / xog / DOF on, **bloom off** → ghosts gone. Bloom chain only (`engine_post` / `lumcompare` / `downsample` / `blurfilter` / `blur_combine`) → ghosts back.

Failed fix (user 2026-09-03): run bloom, draw `engine_post_nxtgen` at eye dest (2656×2592) from the 664×648 buffer, and write that size into the material context `Viewport` so Source’s internal `GetViewport` FLerp matches. Ghosts unchanged.

Keep skipping that bloom chain on stereo eyes. Do not disable the rest of post. Do not retry dest/UV/viewport rescale of `engine_post` without a new integration point (shader constants / bloom RT contents, not dest size).

**Verified fail 2026-09-03 (user):** grow `_rt_Small*FB*` to eye/N — ghosts gone but the **entire picture zoomed and warped with head movement**. Same after reverting the `DoEnginePostProcessing` eye w/h override. Do not grow bloom scratch RTs. Skip bloom on stereo eyes.

## Water (2026-09-03; FPS verified 2026-09-04)

`r_WaterDrawRefraction` / cheap-water force was 0 so HMD-sized planar water would not stamp a view-locked ghost world (DrawSetup is not a nested RenderView). That left Xen/coast water as a flat fog fill.

Refraction-only (user): surface better. Full water retry: `r_WaterDrawReflection` / `r_waterforcereflectentities` / `r_waterforceexpensive` / `nr_gbuffer_for_reflection_enabled` on. Keep `nr_gbuffer_for_refraction_enabled 0` (wall color-buffer stamps, not the water surface). Cafeteria ghosts were bloom, already skipped on stereo eyes. Do not grow `_rt_Water*` / Refract RTs to the eye.

**Verified 2026-09-04 (user):** that full-water set was the GitHub 47 FPS vs local 32 FPS regression in the same spot. Hands and menu were not the cause. Do not retry stereo **full** water (reflection + expensive + gbuffer reflection).

**Verified 2026-09-06 (user):** `r_WaterDrawRefraction 1` alone costs no noticeable performance and looks better than the fog fill. Default is refraction-only again (`waterrefract1`). `nr_gbuffer_for_refraction_enabled` stays 0. Do not grow Water/Refract RTs to the eye.
