# HL2VR stereo rendering

**Confirmed from HL2VR source** unless noted.

## End-to-end path

```
OnRenderStart
  VRManager::OnFrameBegin          // WaitGetPoses ownership (see multicore.md)
  CVRViewRender::SetUpViews
    CalcView → mid CViewSetup
    m_ViewLeft / m_ViewRight = mid
    OverrideStereoView             // EyeToHead + GetProjectionMatrix
CViewRender::Render
  for eye = RIGHT, LEFT, MONO:
    GetViewportBounds              // SBS halves, or window rect for MONO
    RenderView(view, flags)
      MONO: RenderVREyeToScreen + return   // blit, no third world pass
      LEFT/RIGHT:
        SetupMain3DView → Push3DView(SBS RT)
        optional shared sky
        ViewDrawSceneVR            // once-per-frame work gated on RIGHT
        postprocess, HUD into overlay RT
        CleanupMain3DView          // do not clear depth (needed for Submit)
Present → DXVK submit thread → OpenVR Submit
```

## How the two eyes are represented

| Item | Value | File |
| --- | --- | --- |
| Buffer | One RT, width = 2 × recommended, height = recommended | `VRManager::GetBufferSize` |
| Left viewport | `x=0, w=width/2` | `GetViewportBounds(VREye_Left)` |
| Right viewport | `x=width/2, w=width/2` | `GetViewportBounds(VREye_Right)` |
| `m_eStereoEye` | MONO=0 LEFT=1 RIGHT=2 | `view_shared.h` |
| Desktop | Copy of one half (default right, `hlvr_mirror_eye`) | `RenderVREyeToScreen` |

Submit crops that SBS image with texture bounds 0–0.5 and 0.5–1 (`HL2VRInterop::Submit_PrePresentCallBack`). Hidden-area mesh is the compositor’s problem; the game renders the full recommended frustum.

## `CViewSetup`

`SetUpViews` (**Confirmed from HL2VR source**, `vrviewrender.cpp`):

- `m_bViewToProjectionOverride = true` on the mid view *before* stereo copies. Comment in-tree: the engine uses this flag to override projection, including for reflections.
- `zNear` scaled by `vr_projection_znear_multiplier`.
- After `OverrideStereoView`, `ComputeCameraVariables` runs only on the **mid** eye so `g_vecRenderOrigin` / audio stay cyclopean.

`OverrideStereoView` (**Confirmed from HL2VR source**, `client_virtualreality.cpp`):

```
viewAsMatrix = SetupMatrixOrgAngles(mid.origin, mid.angles)
left  = viewAsMatrix * ToSource(GetEyeToHeadTransform(Left))
right = viewAsMatrix * ToSource(GetEyeToHeadTransform(Right))
GetEyeProjectionMatrix(left/right, zNear, zFar)
CalcFovFromProjection → view.fov   // culling, not a symmetric 16:9 FOV
```

Canted HMDs (Pimax) get a real rotation in EyeToHead, not only IPD along `right`.

## How stereo avoids corrupting global view state

**Confirmed from HL2VR source.**

- Each eye is a **copy** of `CViewSetup`. Mid origin/angles are not overwritten with an eye.
- `Push3DView` / `PopView` restore the material frustum.
- `saveRenderTarget` is captured at RenderView entry and restored after `CleanupMain3DView`.
- Shadow `AdvanceFrame` only on `GetLastEye()` so both eyes share one shadow frame index (“both eyes need to have the same shadow frame index, otherwise it can lead to visual artifacts”).
- Water: `_rt_WaterReflectionVR` is 2× wide; `CopyProperWaterReflectionForEye` copies the current eye’s half into `_rt_WaterReflection` before sky so the other eye’s previous reflection is not sampled (**Confirmed from HL2VR source**, `tne_RenderTargets.cpp`, `CopyProperWaterReflectionForEye`).
- Refract: viewport-sized copy only (commit `dd5806dd`) so both eyes are not smashed into one refract texture.

## Culling / visibility

`CalcFovFromProjection` takes the max of the four half-angles from the OpenVR matrix. Source’s vis / frustum then uses a FOV **at least as wide** as the asymmetric projection. Geometry does not disappear at the nasal/temporal edges.

**Black Mesa `openvr` (fused) path:** one union FOV (`m_Fov` from `GetProjectionRaw` max tan) + IPD along view-right.

**Black Mesa `hl2vr` path:** `src/hl2vr_render.cpp` ports OverrideStereoView: EyeToHead origin+angles, `GetProjectionMatrix` into `m_matCustomProjMatrix` (`+0x6C/+0x70`) because engine `ComputeViewMatrices` (`0x101008F4`) copies that slot and never reads `+0x102`. Also writes `+0x102/+0x104` for the client leftover. `CalcFovFromProjection` for vis.

## Desktop vs HMD

The desktop path is a **blit of already-rendered HMD pixels**, not a third `ViewDrawScene`. `hlvr_skip_mirror_view` can skip it. Window size never drives the VR RT.

## What we port into Black Mesa (this pass)

See `blackmesa-porting.md`. We cannot `Push3DView` onto a named SBS RT without engine source. We can: EyeToHead origins, `m_eStereoEye` on copies, draw RIGHT then LEFT, WaitGetPoses ownership, Submit with pose.
