# Xen 3D skybox through map geometry (head pitch)

**Symptom (HMD-verified 2026-09-07):** part of the Xen **3D** skybox (`sky_camera` meshes, e.g. asteroids) shows through opaque map geo when looking **up/down with the headset**. Stick look is fine. GitHub drop-in without this vis path looked better because engine viewangles happened to stay closer to the camera.

This is **not** one-eye 2D sky holes (`hl2vr_pixelvis` / EyeToHead visleaf), HUD Z-leak, or freeze-frame `m_bCacheFullSceneState`.

## Actual cause (do not re-blame the false leads)

CreateMove writes **controller aim** into `cmd->viewangles`. Stereo `CViewSetup.angles` is **HMD pitch/yaw**. Until 2026-09-07 we never `SetViewAngles` from RenderView (comment feared rubberband from a *permanent* overwrite).

On `mat_queue_mode 0` (required; BM stays single-thread), Source areaportals, leaf vis, and HW occlusion still read **IEngineClient viewangles**. Nodding the HMD while the controller points forward culls ceilings/floors as if you were still looking ahead. The 3D skybox already filled those pixels, so asteroids appear inside the room.

Secondary (same symptom at the top/bottom of the HMD): `WorldRenderAtEyeSize=true` makes a **square** eye RT. Engine `ClearBuffers` after `CSkyboxView` still used a **16:9 HWND** viewport/rect. Leftover skybox depth/color sat in the extra vertical band; pitching looks into that band.

Tertiary: `GetViewOrigin` used to add OpenVR `EyeToHead` Z along **look-forward**. That offset follows pitch, so nodding slides the camera into nearby Xen ceilings / scaled skybox.

## What the code does now

- `StereoVisReadScope` + hooked `IEngineClient::GetViewAngles` (slot 19): during the stereo pair only, vis **reads** HMD pitch/yaw. CreateMove still writes controller aim. Do **not** `SetViewAngles` from RenderView — that write/restore crashed DME `ITexture::GetMappingWidth` on null (`engine+0x11222F`) after walking Xen (2026-09-07).
- `ViewportNeedsEyeExpand` in `src/vr.cpp`: `HookedClear` / `SetViewport` / `SetScissorRect` expand any large 16:9 or mixed eye/window band on an eye-sized world RT, not only an exact HWND match. Do **not** add `D3DCLEAR_TARGET` on that path (blacks between sky and world).
- `VR::GetViewOrigin` is IPD-only (`Left`/`Right` ± right*(IPD/2)). No EyeZ along look-forward.

Log: `Stereo vis GetViewAngles HMD pitch=...` and `D3D Clear expand viewport ...`.

## False leads (tried, did not cause this)

| Guess | Why it looked plausible | Why it was wrong |
| --- | --- | --- |
| `VRNearClip=1` / `view.zNear=1` | Far z-fight with 3D sky meshes | Log after revert still had `zNear=7.0` and the leak remained |
| `GetRenderTargetDimensions` lying **every** large RT to eye size | `1232x1200` (SmallFB) → `2464x2400` is a real reconstruct bug | HWND-only remap is the **lens flare** fix; 3D skybox is `Push3DView` (nest stays 1), not that hook |
| HWND-only GetRTDimensions | Same RenderView as skybox | Leak survived after the hook was fully removed |
| `ForceOpenVis` / `r_occlusion 0` | Old stereo empty-vis workaround | Draws the whole map; not the head-pitch hole |
| Disabling 3D skybox / copying prototype mono crop | Prototype is one RenderView | AGENTS.md: do not copy that architecture |

## Do not

- `ExitProcess` or skip stereo because of this.
- Y-flip or change `-oldgameui`.
- Reintroduce `view.zNear = 1` as the skybox “fix”.
- Remap GetRenderTargetDimensions for **non-HWND** sizes (see `docs/env-lensflare.md`).
- `SetViewAngles` from RenderView (L4D2VR queue-0 write/restore). On BM it crashed DME after walking.
