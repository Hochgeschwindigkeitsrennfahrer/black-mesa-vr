# HL2VR desktop / HMD render consistency

**Confirmed from HL2VR source** unless noted.

## Dedicated VR path

Yes. `CVRViewRender` replaces `CViewRender`. Eyes always `Push3DView` onto the SBS VR RT. The backbuffer is not the world target during LEFT/RIGHT. MONO only copies.

Black Mesa still renders the engine’s deferred path into FullFrame/G-buffer/backbuffer and copies or redirects onto private eye surfaces (**Confirmed from Black Mesa source**). Divergence (desktop OK, HMD missing flashlight/textures) is exactly “two destinations, two lifetimes.”

## Mechanisms HL2VR uses

| Mechanism | What it prevents |
| --- | --- |
| One SBS color + matching separate depth | Stale or window-sized depth forcing RT size; eye-to-eye RT mix-up |
| Capture-by-size in DXVK at SetRenderTarget | Submitting a different surface than the one just drawn |
| MSAA resolve into a dedicated image before Submit | Compositor/driver seeing an unresolved MSAA image |
| `WaitGetPoses` → draw → Present/Submit → `PostPresentHandoff` on one timeline | Submitting last frame’s image with this frame’s pose (or vice versa) |
| `Submit_TextureWithPose` with the pose used for `CViewSetup` | Reprojection against the wrong pose |
| Water `_rt_WaterReflectionVR` + per-eye copy | Right eye sampling left’s reflection (stereo mismatch, “ghost” water) |
| Refract copy of **current viewport only** | Both eyes in one refract map |
| Shadow/flashlight/particles once on RIGHT | Flashlight/shadows existing in one eye / desktop leftover pass only |
| MONO = blit | Desktop showing a different vis set than the HMD |
| `mat_queue_mode 0` only while swapping RT guts | Queue thread holding a destroyed ITexture |
| Do not clear depth after the view | Soft particles / depth submit seeing an emptied buffer |

## What Black Mesa was doing wrong relative to this

**Confirmed from Black Mesa source** + prior runtime notes in-tree:

- Two `RenderView`s with `m_eStereoEye` left as MONO → BM’s `cmp [ebx+0x1C], 2` never sees RIGHT, so flashlight/shadow update can run twice or on a leftover 16:9 pass.
- Pose waiter vs Present vs RenderView.
- Submit without the render pose.
- App handoff suspended → runtime timing → WaitGetPoses on a high-priority thread touching the queue again.
- Eye RTs larger than FullFrame with G-buffer still HWND-sized (flashlight apply to the wrong size).

## Port priorities that actually fix divergence

1. Frame timeline (WaitGetPoses / pose submit / handoff) — **this pass**.
2. `m_eStereoEye` + RIGHT-first — **this pass**.
3. EyeToHead origins so both eyes share one mid pose — **this pass**.
4. FullFrame = one eye, desktop blit — next pass (resolution.md).
5. Water stereo RT — needs named RT we can FindTexture; optional later.
