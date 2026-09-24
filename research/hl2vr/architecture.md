# HL2VR architecture

**Source:** local `C:\Users\amien\Documents\cursor\HL2VR_SDK`, branch `main` @ `93bc7127`.  
**Permission:** the HL2VR developer granted this project the right to study, adapt, and port this code.  
**Evidence tags:** **Confirmed from HL2VR source** / **Confirmed from Black Mesa source** / **Confirmed at runtime** / **Strong inference** / **Unknown**.

HL2VR is a full Source SDK 2013 fork (client, server, materialsystem, custom shaders) plus a custom DXVK 2.7.1 that owns OpenVR submit. It is not an injected `d3d9.dll`. Black Mesa has no public engine source, so ports must reproduce HL2VR *mechanisms* inside our combined DXVK + game DLL, not copy the SDK layout.

## Why HL2VR is structured this way

**Confirmed from HL2VR source.** Three layers, each with one job:

| Layer | Owner | Job |
| --- | --- | --- |
| Client `VRManager` + `CVRViewRender` | `src/game/client/hlvr/` | Poses, `CViewSetup`, stereo views, named RTs, HUD overlay |
| Material system | Source queued renderer | `mat_queue_mode` 2; `ICallQueue` carries WaitGetPoses onto the thread that will Present |
| DXVK `HL2VRInterop` | `src/dxvk/src/hl2vr/` | Capture the SBS color/depth/HUD images by size, Submit on the Vulkan submit thread, `PostPresentHandoff` |

The client never calls `IVRCompositor::Submit`. DXVK does, on `dxvk-queue`, after the D3D9 Present has been turned into a Vulkan present job. That is how WaitGetPoses, GPU work, and compositor submit stay on a single ownership story.

## Process / thread model

**Confirmed from HL2VR source** (`vrmanager.cpp` `OnFrameBegin` / `Mat_AwaitFrame`, `cdll_client_int.cpp` `OnRenderStart`, `HL2VRInterop.cpp`, `dxvk_queue.cpp`).

```
game thread                          material / D3D thread              dxvk-queue
-----------                          ---------------------              ----------
OnRenderStart
  OnFrameBegin
    if queued: SyncFrameGetPoses     (wait for last WaitGetPoses)
    ++frameId
    QueueCall(Mat_AwaitFrame)   -->  WaitGetPoses
                                     SetHeadsetRenderingPose
    use predicted pose for views
  SetUpViews / OverrideStereoView
  Render RIGHT, LEFT, then MONO blit
                                     Present
                                       Mat_OnPrePresent (resolve MSAA)
                                       EmitCs present job          -->  SubmitExplicitTimingData
                                                                        Submit(left,right,SBS bounds)
                                                                        desktop present
                                                                        PostPresentHandoff
                                                                        signal handoff condvar
```

When `GetCallQueue()` is null (`mat_queue_mode` 0, menus, RT recreate), `Mat_AwaitFrame` runs WaitGetPoses **on the calling thread**. There is no dedicated pose-waiter thread.

## Stereo representation

**Confirmed from HL2VR source** (`vrviewrender.h` `GetFirstEye`/`GetLastEye`, `view.cpp` `CViewRender::Render`, `tne_RenderTargets.cpp`).

- One **side-by-side** named RT (`CreateVRTwoEyesHMDRenderTarget`) at `GetRecommendedRenderTargetSize().x * 2` by `.y`.
- `STEREO_EYE_LEFT` / `RIGHT` are viewports on that RT (`GetViewportBounds`).
- `STEREO_EYE_MONO` does **not** re-render the world. `RenderVREyeToScreen` `CopyTextureToRenderTargetEx` one half (or both, `hlvr_r_show_both_eyes`) into the window.
- Engine loop is **last eye first**: RIGHT → LEFT → MONO (`for (eEye = GetLastEye(); eEye >= GetFirstEye(); --eEye)`). Shadows, flashlight depth, particle `IncrementFrameCode`, and `IGameSystem::PreRenderAllSystems` run on RIGHT only.

## Camera

**Confirmed from HL2VR source** (`client_virtualreality.cpp` `OverrideStereoView`, `vrmanager.cpp` `GetEyeViewFromMidEyeView` / `GetEyeProjectionMatrix`).

1. Player `CalcView` fills the **mid-eye** `CViewSetup` (origin, angles, zNear/zFar).
2. `m_bViewToProjectionOverride = true` on mid, left, and right.
3. Left/right origins = `SetupMatrixOrgAngles(mid) * ToSource(GetEyeToHeadTransform(eye))`.
4. Left/right projection = OpenVR `GetProjectionMatrix(eye, zNear, zFar)` into `m_ViewToProjection`.
5. `CalcFovFromProjection` writes the **widest** FOV from that matrix so Source frustum culling matches the real frustum.

Black Mesa `CViewSetup` is 0x148 with `m_eStereoEye` at `+0x1C` (**Confirmed from Black Mesa source**, `src/sdk/sdk.h`). The view-to-projection override fields live in the trailing pad (**Unknown** exact offsets until Ghidra on this build). Origin + FOV + `m_eStereoEye` are portable now.

## DXVK

**Confirmed from HL2VR source** (`src/dxvk/RELEASE` = `2.7.1`, `HL2VRInterop.cpp`).

Custom pieces vs stock DXVK:

- `IHL2VRInterop` exported as `dxvkGetHL2VRInterop()` (`hmdWrapper.cpp`).
- `VkSubmitThreadCallback` hooked in `DxvkSubmissionQueue`.
- `Mat_ModifyTextureCreationDetails`: textures whose size equals the VR buffer get `hlvr_msaa` samples.
- `Mat_OnSetRenderTarget` / `OnSetDepthStencil`: first RT matching VR width×height is the color/depth to submit; 1281×720 is the HUD overlay.
- Explicit compositor timing: `VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff`.
- Submit uses `Submit_TextureWithPose` and optional `Submit_TextureWithDepth`, SBS `VRTextureBounds_t` `{0,0,0.5,1}` / `{0.5,0,1,1}`.
- Optional D3D9 VRS foveation (`D3DUSAGE_VRS`).

## What Black Mesa cannot copy literally

**Confirmed from Black Mesa source.** We do not own `CViewRender`, `InitClientRenderTargets`, or `IMaterialSystem` internals. We inject a Win32 `d3d9.dll`. Portable pieces are: WaitGetPoses ownership, explicit timing + pose submit, EyeToHead stereo origins, once-per-frame simulation *hints* (`m_eStereoEye`, right-eye-first), and keeping the HWND swapchain off the HMD size.
