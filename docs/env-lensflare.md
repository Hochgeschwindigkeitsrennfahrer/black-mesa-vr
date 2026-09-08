# env_lensflare sitting on 16:9 / floating off lights

**Symptom:** `env_lensflare` sprites stay laid out for the desktop 16:9 backbuffer and drift off the world lights in the HMD.

## Cause (Ghidra `client.dll`, not `server.dll`)

- `CLensflare::DrawModel` = `FUN_1022dbe0` (`client.dll+0x22DBE0`), VProf at `0x1047a07c`.
- Draw path: `FUN_1022e080` → WorldToScreen `FUN_1022fc00` → mesh quads `FUN_1022e470`.
- DrawModel calls **IMatRenderContext+0x20** (`GetRenderTargetDimensions`, two `int*`) and passes that size into WorldToScreen (NDC→pixels).
- The mesh converts those pixels back to NDC with **GetViewport (+0x9C)**.

Stereo already forces `GetViewport` to the eye (`ClampStereoViewport`). GetRTDimensions still returned the HWND (e.g. 2560×1440) vs eye ~3168×3104, so the sprites parked at 16:9 positions on the square eye RT.

World reconstruct uses the same call. Leaving HWND 16:9 here while the eye RT is square looks **softer / lower res** in the HMD (2026-09-07). Do not scope this lie to flares only.

`CSprite::DrawModel` (`FUN_101dbf70`) is world-space and is not this bug. `CGlowOverlay` typically uses GetViewport (already hooked).

## Multicore (`mat_queue_mode` 2)

Hardware `GetViewport` (`materialsystem+0x68A70`) and `GetRenderTargetDimensions` (`+0x68840`) are different functions from the queued context.

- Queued GetRTDimensions (`materialsystem+0x5F890`) is `ret 8` and **never writes** the out sizes. Flares then WorldToScreen with HWND leftovers or uninit stack.
- Queued GetViewport (`+0x5F8C0`) is also not the hardware hook, so `ClampStereoViewport` did not run on the record thread.

Hook both. HWND-only remap on the hardware function; on the queued stub, fill eye size during the stereo pair (not HUD, not leftover, not nested, not sub-640×360).

## Fix

Hook context vtable **+0x20** (hardware, base, and queued) and queued **+0x9C**. Remap when the returned size matches the HWND client rect, during stereo, not HUD, not nested `RenderView`, not sub-640×360. Queued stub may also log `queued empty` / `queued stub`.

Log: `GetRenderTargetDimensions 2560x1440 -> 3168x3104 (HWND stereo)`. You must **not** see `1232x1200 -> 3168x3104`.

## Ban: global eye-size lie

The first hook rewrote **every** large RT to the eye, including half-res SmallFB (`1232×1200`). That is a reconstruct / depth mismatch. Do not do it again.

Walking Xen `C0000005` at `engine.dll+0x11222F` (`ITexture::GetMappingWidth` on null inside DME) was **not** this HWND remap. That crash stayed after the lie was scoped to flares. Cause was `SetViewAngles` during the stereo pair (`docs/xen-3d-skybox-head-pitch.md`).

3D skybox through walls on head pitch is a **different** bug. Do not remove this HWND-only remap to “fix” skybox.
