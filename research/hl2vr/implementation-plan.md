# Implementation plan (HL2VR → Black Mesa VR)

Work against local `HL2VR_SDK` (`main` @ `93bc7127`) and this repo’s `src/`. Attribute HL2VR in comments.

## Pass 1 — frame timeline + stereo hints (this work)

Goal: one WaitGetPoses per frame on the thread that owns Present; submit the pose that was used to draw; stop suspending handoff; EyeToHead origins; RIGHT then LEFT with `m_eStereoEye` set.

### 1a. Kill the OpenVR pose-waiter thread

- Stop `StartPoseWaiter` / `PoseWaiterThreadMain` on the OpenVR path.
- OpenXR helper tracking unchanged.

### 1b. `OnFrameBegin` at `WaitPosesForStereoFrame`

HL2VR (`vrmanager.cpp`):

```
if queued: SyncFrameGetPoses(prev)
++frameId
Mat_AwaitFrame(frameId)   // QueueCall or sync WaitGetPoses
if !queued: SyncFrameGetPoses(this)
use predicted pose if queued else current
```

Black Mesa:

1. Probe `GetCallQueue` on the live render context (slots 146 and 185, SEH). Log once.
2. If queue non-null and `hl2vr_wgp` not sticky: `QueueFunctor` a `CFunctor` that calls `WaitGetPoses` (current + predicted, HMD-sized arrays), then `SyncFrameGetPoses` on the game thread (500 ms cap). Latch **predicted** `mDeviceToAbsoluteTracking` for `CViewSetup` and Submit.
3. Else: `WaitGetPoses` on the calling thread; latch **current**.
4. `BeginStereoFramePose` copies that latch for submit.

Crash-sticky: enter `hl2vr_wgp` before queued WGP, leave after first success. If present at startup, skip queue forever (sync only).

### 1c. Submit

- `SubmitExplicitTimingData()` immediately before `Submit`.
- Build `VRTextureWithPose_t` from the latched HMD matrix; `Submit(..., Submit_TextureWithPose)`.
- `PostPresentHandoff()` every successful submit. Remove the 5-slow-call suspend in `TryCompositorPostPresentHandoff`.

### 1d. EyeToHead origins

`GetViewOriginLeft/Right`: convert `GetEyeToHeadTransform` with existing `HmdMatrixToSourcePos` / `m_VRScale`, apply in the mid-eye view basis. Do not add IPD along view-right **and** `m_EyeZ` (double apply).

### 1e. `m_eStereoEye` + draw order

- Copy setup, set `m_eStereoEye` to 1 (LEFT) or 2 (RIGHT) — never a pixel height.
- Draw RIGHT, then LEFT (HL2VR last-eye-first) so BM `cmp [ebx+0x1C], 2` can skip duplicate flashlight/particle work.
- Sticky `hl2vr_eye` if that write crashes.

### Out of pass 1

Swapchain size, named SBS RT, DXVK tree swap, projection-matrix override, water RT, material predraw.

## Pass 2 — resolution (after pass 1 is verified in a headset)

Grow FullFrame / G-buffer to **one recommended eye**, keep HWND desktop-sized, blit desktop. Do not retry `hmd_swap`. Use HL2VR’s `RT_SIZE_LITERAL` + separate depth idea via `SetRenderTargetFrameBufferSizeOverrides` / `SwapContents` if we can find the vtable.

## Pass 3 — named SBS RT (only if pass 2 is stable)

One 2×-wide named RT + viewports. Abort if `FindTexture` / `Push3DView` fail the way prior `steamvr_rt` did.

## Pass 4 — DXVK submit-thread port (optional)

If Present-thread Submit still fights the queue after pass 1: port `VkSubmitThreadCallback` **into this** DXVK fork, keep `IDirect3DVR9`. Do not enable GPL. Do not jump to 3.x.

## Verification (do not claim headset success without a session)

After pass 1 build:

- DLL compiles Win32 Release.
- Log: GetCallQueue probe result; WGP path (queued / sync); `SubmitExplicitTimingData` / `Submit_TextureWithPose` / handoff errors; `m_eStereoEye` values 1 and 2.
- Headset: stereo, 6DoF, controllers, weapons/hands, map load still work.
- If `hl2vr_wgp` or `hl2vr_eye` appears, that attempt is done; do not restore the waiter thread.
