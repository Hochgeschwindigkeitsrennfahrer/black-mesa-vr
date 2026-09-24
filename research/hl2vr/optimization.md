# HL2VR optimization

Every item: problem, where, how, assumptions, Black Mesa equivalent, portability.

## 1. WaitGetPoses on the material queue (not a side thread)

- **Problem:** WaitGetPoses is the compositor pacing barrier. A second thread racing Present/Submit fights the Vulkan queue and ages poses relative to the image.
- **Where:** `vrmanager.cpp` `Mat_AwaitFrame` / `OnFrameBegin`, commit `bc1fc6fa`. Earlier: `583c30a1`, `5ea32844`, `a9aa8468`.
- **How:** `ICallQueue::QueueCall(Mat_AwaitFrame, frameId)` so WGP runs on the thread that will Present. Main thread `SyncFrameGetPoses` and renders with **predicted** poses while queued.
- **Assumes:** `GetCallQueue()` is valid in `MATERIAL_QUEUED_THREADED`; explicit app handoff so WGP does not take the Vulkan queue.
- **Black Mesa:** dedicated pose-waiter thread; RenderView only sleeps up to ~8 ms for a new snapshot (**Confirmed from Black Mesa source**, `VR::WaitPosesForStereoFrame`).
- **Port:** replace the waiter. See `multicore.md`. **Action: replace.**

## 2. Explicit timing + SubmitExplicitTimingData + PostPresentHandoff

- **Problem:** Without explicit timing, WaitGetPoses submits GPU timestamps / touches the queue (`a9aa8468`).
- **Where:** `HL2VRInterop::Init`, `Submit_PreSubmitCallback`, `Submit_PostPresentCallback`.
- **How:** ApplicationPerformsPostPresentHandoff; timing data immediately before queue submit; handoff after present. No suspend-to-runtime fallback.
- **Black Mesa:** app handoff default true, but `TryCompositorPostPresentHandoff` **suspends** after 5 slow calls (**Confirmed from Black Mesa source**). No `SubmitExplicitTimingData`.
- **Port:** add timing data; stop suspending. **Action: port.**

## 3. `Submit_TextureWithPose` (and optional depth)

- **Problem:** Reprojection using a pose that does not match the rendered image causes judder.
- **Where:** `Submit_PrePresentCallBack`; pose set by `Mat_SetHeadsetRenderingPose`. Depth: `a78abc22` (do not clear depth), `5b27033f` then `f2f1bc94` revert on Oculus, later `ce527141` menu + depth to dxvk.
- **Assumes:** pose is the one used for the eye views (predicted when queued).
- **Black Mesa:** `Submit_Default` only.
- **Port:** pose submit now; depth later (needs a stable depth image). **Action: port pose.**

## 4. Single SBS RT + desktop blit (no third world pass)

- **Problem:** A third `RenderView` for the monitor doubles CPU/GPU and desyncs lighting.
- **Where:** `CVRViewRender::RenderView` MONO branch; `RenderVREyeToScreen`.
- **Black Mesa:** two `RenderView`s into private eyes; leftover 16:9 main is skipped when possible; desktop often a blit (`MirrorStereoToDesktopWindow`) (**Confirmed from Black Mesa source**).
- **Port:** keep skip-leftover + desktop blit. Full SBS needs named RT + `Push3DView`. **Action: retain blit; SBS later.**

## 5. Once-per-frame simulation (RIGHT eye first)

- **Problem:** Particles, beams, shadows, flashlight depth, `PreRender` running twice per stereo pair.
- **Where:** `ViewDrawSceneVR`; commit `acb6befd` (particles/beams); shadow `AdvanceFrame` / `ComputeShadowDepthTextures` on RIGHT only.
- **Assumes:** engine loop draws `GetLastEye()` first; `m_eStereoEye` is set.
- **Black Mesa:** two full `RenderView`s. `m_eStereoEye` write persist-skipped (`hl2vr_eye`, 2026-09-04). Copies stay MONO; draw **LEFT then RIGHT**. If that sticky is cleared, copies get LEFT/RIGHT and draw **RIGHT then LEFT** so BM’s `cmp [ebx+0x1C], 2` branches can fire.
- **Port:** order gated on `TryHl2vrStereoEye()`. **Action: ported (gated).**

## 6. Skip full RT reallocation (`SwapContents`)

- **Problem:** Recreating every `_rt_FullFrameFB*` on resolution/MSAA change hitchs.
- **Where:** `CTNERenderTargets::UpdateBaseGameRenderTexture`, commit `69472a8e` (materialsystem.dll binary).
- **Assumes:** `ITextureInternal::SwapContents` + refcount fixup; must force `mat_queue_mode 0` during recreate (`tne_RenderTargets.cpp`).
- **Black Mesa:** `SetRenderTargetFrameBufferSizeOverrides`; no SwapContents.
- **Port:** only if we grow FullFrame after startup. **Action: investigate.**

## 7. Predraw all materials after load

- **Problem:** DXVK pipeline compile stutter on first use.
- **Where:** `CVRViewRender::DrawAllMaterials`, commit `aeaf3f03`.
- **Assumes:** `IMaterialSystem::FirstMaterial/NextMaterial`, dummy triangle, scoped RT.
- **Black Mesa:** `IMaterialSystem` FirstMaterial slot 79 (FindMaterial 78, FindTexture 84). Dummy `DrawScreenSpaceRectangle` 1×1 via the existing hook original. Sticky `hl2vr_predraw`. Config `MaterialPredraw`.
- **Port:** `src/vr_cull_opt.cpp` `RunMaterialPredrawIfNeeded`. **Action: ported.**

## 8. Shared sky (optional)

- **Problem:** 3D skybox is expensive twice.
- **Where:** `hlvr_shared_sky` (default **0**), `RenderSharedSky` / `BlitSharedSkyToEye`.
- **Port:** needs a named RT and sky view we do not own. **Action: not this pass.**

## 9. MSAA only on VR-sized textures; skip unused resolves

- **Where:** `Mat_ModifyTextureCreationDetails`; commit `971d50e2`.
- **Black Mesa:** MSAA on private eye RTs, resolve to submit surfaces (**Confirmed from Black Mesa source**).
- **Action: retain.**

## 10. HUD as OpenVR overlay, not in-eye pixels

- **Problem:** In-eye HUD is mush at low SS.
- **Where:** commit `fd52d4f5`; overlay 1281×720; `SetHUDOverlayFromBounds`.
- **Black Mesa:** already has a HUD overlay path.
- **Action: retain overlay; do not Y-flip 2D UI.**

## 11. Mobile / particle density

HL2VR has many `hlvr_*` mobile cvars. Not a Black Mesa target. **Action: ignore.**

## 12. Hidden-area mesh early-Z (3adc23b2, reverted bf24bc8f for mobile)

- **Problem:** Lens-hidden pixels still run the world pixel shader.
- **Where:** HL2VR `DrawHiddenAreaMesh` after depth clear, identity MVP, `dev/hidden_area_mesh`. OpenVR UV (0,0) bottom-left.
- **Black Mesa:** no Source mesh API. After `D3DCLEAR_ZBUFFER` on an eye-sized world RT, draw D3D9 `XYZRHW`. OpenXR used `XR_KHR_visibility_mask` (helper published 102 verts/eye; game drew 34 tris at `3168x3104`).
- **Verified miss 2026-09-05:** black overlay on the world, no FPS gain. Persist-skipped `hl2vr_ham`. Config `HiddenAreaMesh=false`. Do not retry. Helper may still publish the mask; the game must not draw it.
- **Port:** `src/vr_cull_opt.cpp`. **Action: verified miss; off.**

## 13. Per-eye `CalcFovFromProjection`

- **Problem:** Union FOV on both eyes still undershoots a single eye’s temporal/nasal half-angle if that eye is wider than the union-X used for submit crop.
- **Where:** `client_virtualreality.cpp` `CalcFovFromProjection`: max of four `2*atan(|half-tan|)`.
- **Black Mesa:** `m_FovLeft` / `m_FovRight` from `GetProjectionRaw` tans. Submit still uses union `m_Fov` / UV crop. `WorldRenderFov()` writes that union into `CViewSetup.fov`. Keeping engine ~106 (2026-09-05) **warped on head motion** against the 98.5 crop. One-eye 2D sky / walls is the MONO once-per-frame vis slot, not FOV.
- **Port:** **Action: HMD union FOV on CViewSetup (warp-verified). `stereo_vis` was freeze-frame, not vis; persist-skipped.**

## 14. Pixel vis once per stereo pair (26a5fe68)

- **Problem:** `PixelVisibility_EndCurrentView` issues HW occlusion queries twice.
- **Where:** HL2VR `ViewDrawSceneVR`: only `GetLastEye()` (RIGHT, drawn first). BM `CBlackMesaViewRender_ViewDrawScene` @ `102114d5` always calls `FUN_100e3b00` (`client.dll+0xE3B00`, `MOV ECX,g_PixelVisibilitySystem; JMP EndView`). Do not skip `PixelVisibility_EndScene` (`FUN_100e3b10`).
- **Port:** hook the thunk; first stereo eye of the pair runs original, second returns. **Verified miss 2026-09-05:** one-eye 2D skybox hole. Persist-skipped `hl2vr_pixelvis`. **Action: off.**

## 15. Local player render bounds (feb36883)

- **Problem:** Looking down / jumping culls the player; hands vanish.
- **Where:** HL2VR `C_BaseVRPlayer::GetRenderBoundsWorldspace` origin ±1000.
- **Black Mesa:** MinHook `IClientRenderable` slot 21 (`GetRenderBoundsWorldspace`). Source LookupAttachment is slot 35; BM is 36 (`+0x90`).
- **Port:** **Action: off.** Do not re-arm; leftover cull-opt from 2026-09-05.

## 16. Second-eye vis / flashlight (MONO once-per-frame)

- **Problem:** Both stereo copies leave `m_eStereoEye` MONO. Per-eye lighting / walls / 2D sky still wrong on some maps.
- **Where:** `CViewRender+0x744` is **`m_rbTakeFreezeFrame`**, not vis. `cmp [eax+this+0x744]` + `this+0x748` (`m_flFreezeFrameUntil`) vs `curtime`; if the byte is 0 and until is live, RenderView draws `CFreezeFrameView` and jumps to `0x1020F698` (no world). Zeroing it (`stereo_vis`) presented an empty freeze: white void, white sky until you walk, then the old per-eye bug. Do **not** write LEFT/RIGHT — that died on the first stereo pair (OpenXR, 2026-09-04).
- **Port:** persist-skipped `stereo_vis` (2026-09-05). Do not force `m_bCacheFullSceneState=false` (black void + glow overlays on Xen). Keep `r_occlusion 1` / `r_fastzreject 1` / `r_portalsopenall 0`. **Action: verified miss; off.**

## Not ported (engine source / already failed / mobile)

- Shared sky (`hlvr_shared_sky`) — needs a named RT we do not own.
- `cl_reduced_bone_setup_dist` / `BuildTransformations` merge — BM client has no such cvar.
- Detail-prop SIMD, VRS foveation (`IHL2VRInterop`), tone-map/bloom once-per-eye (`viewpostprocess.cpp`).
- HMD-sized swapchain (`hmd_swap`). `m_eStereoEye` write remains persist-skipped.
