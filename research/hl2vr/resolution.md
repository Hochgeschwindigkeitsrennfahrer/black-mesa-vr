# HL2VR VR render resolution

**Confirmed from HL2VR source.**

## Allocation

```
GetRecommendedRenderTargetSize(&w, &h)
GetBufferSize() = (w * 2, h)          // SBS
CreateNamedRenderTargetTextureEx2(..., RT_SIZE_LITERAL, IMAGE_FORMAT_ARGB8888, MATERIAL_RT_DEPTH_SEPARATE)
```

`CTNERenderTargets::InitClientRenderTargets` also:

- `_rt_vgui` 1281×720 `RT_SIZE_LITERAL` (HUD overlay, independent of HMD and window).
- `_rt_FullFrameFB` / `FB1` / `_rt_Fullscreen` / small FBs **resized to one eye** (`GetDesiredFullFrameBufferDimensions` = buffer/2 × height) via `SwapContents`.
- Water, scope, cinema, shared-sky, outline RTs at VR-derived sizes.
- Depth on those RTs is `MATERIAL_RT_DEPTH_SEPARATE` — commit `8624a8ee`: “Decouple render target depth buffers from window depth buffer, as that forces restrictions on our RT resolutions.”

MSAA is **not** `mat_antialias` on the swapchain. `hlvr_msaa` (default 4) is applied in DXVK when a texture matches the VR buffer size.

## Window vs HMD

The HWND / videomode stay at the user’s desktop size. The engine never sets `D3DPRESENT_PARAMETERS.BackBufferWidth/Height` to 3168×3100. Desktop is a **copy** from the SBS RT (`CopyTextureToRenderTargetEx`).

`mat_viewportscale` still exists; VR viewports come from `GetViewportBounds`, not from `vrect_t` (except MONO).

Live SS / MSAA changes: `UpdateVRRenderTargets` compares actual vs desired, forces queue mode 0, `FadeToGrid`, recreates, restores queue mode.

## Black Mesa today

**Confirmed from Black Mesa source** (`docs/DXVK.md`, `VR::ChooseEyeRenderSize`, `ApplyRenderTargetFramebufferOverride`).

- HMD-sized **swapchain** is banned after verify (`hmd_swap`): black desktop, one Submit, Reset.
- Eyes are private `CreateTexture` surfaces. Size tries SteamVR recommended when `ComputeOffscreenEyeSize` / `WorldRenderAtEyeSize` work; otherwise fit HMD aspect into FullFrame/HWND.
- `SetRenderTargetFrameBufferSizeOverrides` can grow `_rt_gb*` / FullFrame; growing G-buffers past FullFrame produced missing flashlight and ghost geometry (**Confirmed at runtime**, 2026-09-01 notes in `vr.cpp`).

HL2VR’s architecture that we want: **logical framebuffer = one eye**, HWND unchanged, HMD reads an offscreen target, desktop blits. Black Mesa already has the override API. The remaining gap is making FullFrame **actually** allocate at eye size (`SwapContents` / grow-at-init) without a 3168 swapchain.

## Port

This pass does **not** retry `hmd_swap`. It keeps offscreen eyes + override. Next resolution pass (see `implementation-plan.md`): grow FullFrame to recommended eye size at map load using the same `RT_SIZE_LITERAL` + separate depth idea, then blit desktop. That is HL2VR, not a taller HWND.
