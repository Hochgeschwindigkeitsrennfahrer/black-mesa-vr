# HL2VR DXVK

**Confirmed from HL2VR source** (`src/dxvk/RELEASE`, `meson.build` `version : '2.7.1'`, `src/dxvk/src/hl2vr/`).

## Version and fork

| Item | Value |
| --- | --- |
| Base | DXVK **2.7.1** |
| Tree | `HL2VR_SDK/src/dxvk` (git submodule history; `dxvk_update` branch / `be2063d9` “Update dxvk for loading screen fixes”) |
| Output | `d3d9.dll` built from `src/dxvk/dxvk.sln`, copied to the HL2VR install |
| VR API | `IHL2VRInterop` + `VkSubmitThreadCallback`, **not** `IDirect3DVR9` |

This is a different VR contract than Black Mesa’s current combined DLL (`IDirect3DVR9` UUID `7e272b32-…`, `TransferSurface`, Present → `VR::Update`). Replacing our DXVK *tree* with HL2VR 2.7.1 wholesale would drop `IDirect3DVR9` and break the load path. **Port the submit/timing mechanism into our Present/Submit path.** Do not drop in HL2VR’s prebuilt `d3d9.dll`.

## Loading

HL2VR is a native Source game: the process loads *their* `d3d9.dll` as D3D9. Client `VRManager::Activate` calls `dxvkGetHL2VRInterop()->Init(VRSystem, VRCompositor, VROverlay, hudOverlay)`.

Black Mesa’s launcher still requires the combined DLL in `bin\thirdparty\dxvk-windows-x86\`, `bin\`, and next to `bms.exe` (**Confirmed from Black Mesa source**, `docs/DXVK.md`). That load path stays.

## Render-target handling

**Confirmed from HL2VR source** (`HL2VRInterop.cpp`):

- `ResetRenderTextures(width, height, msaa)` stores the SBS size. `msaa <= 1` → 0.
- `Mat_ModifyTextureCreationDetails`: if `desc.Width/Height` match, set `desc.MultiSample`.
- Capture is **by size on SetRenderTarget**, not by a game-side `IDirect3DVR9` cookie: first matching color RT and depth RT of the frame are the submit images.
- HUD: hardcoded 1281×720 (`CreateVGuiTexture`).
- `Mat_OnPrePresent`: resolve MSAA (`VK_RESOLVE_MODE_AVERAGE_BIT` color, `SAMPLE_ZERO` depth) and `transformImage` to `TRANSFER_SRC_OPTIMAL`.
- Submit uses the **resolve** image when `sampleCount > 1` (commit `ef255d0e`, AMD crash otherwise).

## Synchronization and command submission

**Confirmed from HL2VR source** (`HL2VRInterop::Init`, `dxvk_queue.cpp`, `d3d9_swapchain.cpp`).

1. `SetExplicitTimingMode(Explicit_ApplicationPerformsPostPresentHandoff)` so `WaitGetPoses` does **not** touch the Vulkan queue.
2. Frame handshake: `m_mat_frameAwaitedId`, `m_submit_frameSubmitId`, `m_submit_frameHandoffId`, two condition variables, 500 ms timeouts.
3. On the submit thread, **before** `cmdList->submit` / present: `SubmitExplicitTimingData()` once per awaited frame.
4. On the present job: `Submit` both eyes with pose (and depth if present).
5. After present: `PostPresentHandoff()`, then notify `m_condFrameHandedOff`.
6. Next `Mat_AwaitFrame` waits until `m_submit_frameHandoffId >= previous awaited id` before calling WaitGetPoses again.

There is no “suspend app handoff after N slow calls”. Handoff is unconditional for submitted frames.

## Low-latency / GPL / async

**Confirmed from HL2VR source.** Tree is DXVK 2.7.1 with its own latency tracker (`entry.latency.tracker` in `dxvk_queue.cpp`). Foveated rendering is **VRS**, off by default (`hlvr_enable_foveated_rendering 0`). This is not DXVK-GPLALL 2.6.8.

**Black Mesa:** we stay on the existing Win32 MSVC DXVK fork that still exports `IDirect3DVR9` (**Confirmed from Black Mesa source**, `docs/DXVK-UPGRADE.md`). Do not enable GPL for this port. Do not jump to DXVK 3.x.

## VR-specific DXVK hooks (inventory)

| Hook | File | Purpose |
| --- | --- | --- |
| `Mat_ModifyTextureCreationDetails` | `d3d9_device.cpp` CreateTexture / CreateRenderTarget | MSAA on VR-sized surfaces |
| `Mat_OnSetRenderTarget` | `d3d9_device.cpp` | Pick submit color / HUD / VRS enable |
| `Mat_OnSetDepthStencil` | `d3d9_device.cpp` | Pick submit depth |
| `Mat_OnPrePresent` / `OnPostPresent` | `d3d9_device.cpp` `PresentEx` | Resolve + clear per-frame RT pointers |
| `Mat_GetVRSubmissionInfo` | `d3d9_swapchain.cpp` Present | Pass images + pose into the CS present lambda |
| `Submit_*Callback` | `dxvk_queue.cpp` | Timing, Submit, Handoff |

## Port to Black Mesa (mechanism, not the DLL)

Inside `VR::SubmitVRTextures` / `TryCompositorPostPresentHandoff`:

1. Keep `ApplicationPerformsPostPresentHandoff`.
2. Call `SubmitExplicitTimingData()` immediately before `Submit`.
3. Submit `VRTextureWithPose_t` with the pose latched for that stereo pair (`Submit_TextureWithPose`).
4. Always `PostPresentHandoff()` after Submit on the Present thread. Do not flip to runtime timing after slow calls — that path exists to paper over WaitGetPoses on the wrong thread.
5. Do **not** import HL2VR VRS / size-based RT capture until we own a single SBS RT.
