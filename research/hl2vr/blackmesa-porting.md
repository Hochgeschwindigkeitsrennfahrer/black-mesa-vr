# Black Mesa porting notes (from HL2VR)

HL2VR is the architecture reference. Black Mesa has no public engine source; we inject a combined Win32 `d3d9.dll`. Ports reproduce **mechanisms**, not SDK layout.

Load path stays: `d3d9.dll` next to `bms.exe`, in `bin\`, and in `bin\thirdparty\dxvk-windows-x86\`. Keep `IDirect3DVR9` on this DXVK fork. Do not drop in HL2VR’s 2.7.1 `d3d9.dll`.

## Dedicated `VRRuntimeBackend=hl2vr` (OpenVR) — reverted 2026-09-05

This backend was removed. Headset presentation is the published OpenXR helper + `GetProjectionRaw` crop (same as [black-mesa-vr](https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr)). Do not restore `src/hl2vr_render.cpp`, full-frustum Submit 0–1, or `m_eStereoEye` LEFT/RIGHT (white freeze). Notes below are the verified miss.

Separate from `openxr` (helper) and from `openvr` (older fused/crop path). SteamVR compositor, HL2VR stereo without those crops:

| HL2VR (`HL2VR_SDK`) | Black Mesa `hl2vr` mode |
| --- | --- |
| `OverrideStereoView`: `viewAsMatrix * ToSource(GetEyeToHeadTransform)` | `src/hl2vr_render.cpp` `P * T_ov * P^{-1}` (OpenVR→Source). Column-only remap of identity EyeToHead was a 90° permutation (sideways, 2026-09-05) |
| `GetEyeProjectionMatrix` → `m_ViewToProjection` | **Engine consumes CS:GO `m_bCustomProjMatrix` (`+0x6C` / `+0x70`)**. `engine.dll` `ComputeViewMatrices` at `0x101008F4`: custom view `+0x38`, then `cmp [esi+0x6C]`, copy `+0x70`, else ortho `+0x24` / perspective from fov. **No `cmp [reg+0x102]` in engine.dll.** Client `FUN_10106a30` still writes `+0x102/+0x104` (sourcevr leftover). Writing only `+0x102` left a symmetric fov + Submit 0–1 (giant world, near-only fusion, 2026-09-05). Sticky `hl2vr_proj` |
| `CalcFovFromProjection` for vis | Same four-half-angle max; Source frustum matches the real eye |
| RIGHT then LEFT, `m_eStereoEye` 2 then 1 | **Verified miss** 2026-09-05: first RIGHT copy wrote `stereoEye=2` and presented white freeze (`CViewRender` freeze-frame slot `[m_eStereoEye]`). MONO copies, LEFT then RIGHT. Persist-skip `hl2vr_eye` is **not** ignored. Sticky `hl2vr_neye` |
| Full recommended frustum, Submit SBS 0–0.5 / 0.5–1 | Two private eye textures (named SBS RT already failed). Submit **0–1** per eye — no `GetProjectionRaw` UV crop |
| MONO = blit, not a third world pass | Skip leftover 16:9; `MirrorStereoToDesktopWindow` |
| FullFrame = one eye | Forced `WorldRenderAtEyeSize` |
| `WaitGetPoses` on mat queue, `Submit_TextureWithPose`, unconditional handoff | Existing OpenVR timeline (no pose-waiter thread) |
| HWND stays desktop-sized | Unchanged (`hmd_swap` still banned) |

`src/hl2vr_render.cpp` is the port of `client_virtualreality.cpp` / `vrmanager.cpp` view math. Input, hands, weapons stay BMVR.

Switch back: `VRRuntimeBackend=openxr` or `openvr`. `hl2vr` retries EyeToHead + projection with **MONO** `m_eStereoEye` (do not write LEFT/RIGHT — white freeze). `scripts/install.ps1` no longer overwrites an existing backend key.

## What we keep from current BMVR


| Piece                                        | Why                                                        |
| -------------------------------------------- | ---------------------------------------------------------- |
| Combined `d3d9.dll` + `IDirect3DVR9`         | Only load path that actually injects into Steam Black Mesa |
| HWND-sized swapchain                         | `hmd_swap` verified black desktop / Reset                  |
| Private offscreen eye surfaces               | Named-RT / `steamvr_rt` already failed on this engine      |
| Desktop blit (`MirrorStereoToDesktopWindow`) | Same idea as HL2VR MONO blit                               |
| HUD overlay, `-oldgameui`, `-enabledxvk`     | Verified BM constraints                                    |
| OpenXR helper path                           | Independent of OpenVR WaitGetPoses                         |
| Crash-sticky flags                           | Disable only the attempt that crashed                      |




## What we replace this pass


| Current BMVR                                 | HL2VR replacement                                               |
| -------------------------------------------- | --------------------------------------------------------------- |
| Dedicated pose-waiter thread                 | `OnFrameBegin`: WaitGetPoses on mat queue or calling thread     |
| `WaitGetPoses(..., predicted=nullptr)`       | Both current and predicted pose arrays                          |
| `Submit_Default`                             | `Submit_TextureWithPose` with the pose used for the views       |
| Handoff suspended after 5 slow calls         | Unconditional `PostPresentHandoff` + `SubmitExplicitTimingData` |
| Eye origins = mid + IPD along view-right     | `GetEyeToHeadTransform` → Source, applied with mid view basis   |
| `m_eStereoEye` left as MONO; LEFT then RIGHT | Set LEFT/RIGHT; draw **RIGHT then LEFT**                        |




## What we do not invent this pass

- Side-by-side named MaterialSystem RT (`Push3DView` onto `_rt_vr`). Prior named-RT attempts failed on BM.
- HMD-sized swapchain.
- HL2VR DXVK 2.7.1 tree / `IHL2VRInterop` / VRS foveation / GPL.
- Activating BM `sourcevr.dll` / `ISourceVirtualReality::UseVR`. Client OverrideStereoView writes `+0x102`; engine projection is `+0x6C`.
- Shared sky, water `_rt_WaterReflectionVR`, `SwapContents`.



## Vtable / offset facts

**Confirmed from Black Mesa source** (`src/offsets.h`, `src/sdk/sdk.h`):

- `CViewSetup` size `0x148`, `m_eStereoEye` at `+0x1C` (never write height here).
- `m_bCustomProjMatrix` at `+0x6C`, `m_matCustomProjMatrix` at `+0x70` — **this is what engine `ComputeViewMatrices` uses**. `m_bViewToProjectionOverride` at `+0x102` is client-only.
- `IMaterialSystem::GetRenderContext` byte offset `+0x19C` (slot 103).
- 6-arg PushRT at context `+0x23C` (slot 143). Source 2013 PushRT is slot 104 (`+0x1A0`). Extra BM slots ≈ **+39**.
- HL2VR `IMatRenderContext::GetCallQueue` is absolute slot **146**. BM may be **146 or 185**. Probe both under SEH.

**Unknown:** exact `GetCallQueue` slot until the first successful probe log.

## Crash-sticky flags for this pass


| Flag               | Meaning if it fires                                                                                                                   |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------- |
| `hl2vr_wgp`        | Queued `QueueCall(WaitGetPoses)` crashed. Fall back to **sync** WGP on the calling thread. Do **not** restore the pose-waiter thread. |
| `hl2vr_neye`       | Native-mode `m_eStereoEye` LEFT/RIGHT crashed. Leave as MONO.                                                                         |
| `hl2vr_cproj`      | Writing `CViewSetup+0x6C/+0x70` custom proj crashed. FOV+aspect only.                                                                  |
| `hl2vr_proj`       | Historical false-ban from the stereo-eye freeze. Ignored.                                                                             |
| `hl2vr_ham`        | Hidden-area mesh early-Z after depth clear crashed.                                                                                   |
| `hl2vr_pixelvis`   | `PixelVisibility_EndCurrentView` once-per-pair hook crashed.                                                                          |
| `hl2vr_predraw`    | Material iterator / dummy `DrawScreenSpaceRectangle` crashed.                                                                         |
| `hl2vr_playercull` | Local-player `GetRenderBoundsWorldspace` hook crashed.                                                                                |




## OpenXR

The 64-bit helper already owns poses. Do not call OpenVR `WaitGetPoses` or `Submit` on that path. Frame-sync / pose-submit / handoff apply to **OpenVR only**.