# HL2VR multicore / render-thread

**Confirmed from HL2VR source** (`vrmanager.cpp`, `tne_RenderTargets.cpp`, `cdll_client_int.cpp`).

## Policy

HL2VR **runs `mat_queue_mode` 2** in gameplay. It does not disable queued rendering to “make VR work.” It **moves WaitGetPoses onto that queue**.

Temporary `mat_queue_mode 0` only when:

- Recreating render targets (`UpdateVRRenderTargets` — “if we try to recreate the render targets in threaded mode, we will crash”).
- Fading to the SteamVR grid / loading overlay (`FadeToGrid`).

Then it restores the previous mode.

Menu maps: commit `ce527141` notes queue mode looks disabled there; they still submit (including depth) rather than inventing a third threading model.

## `ICallQueue` (HL2VR `callqueue.h`)

```
auto* callQueue = materials->GetRenderContext()->GetCallQueue();
if (callQueue != nullptr) {
    callQueue->QueueCall(Mat_AwaitFrame, frame);
    return;
}
dxvkGetHL2VRInterop()->Mat_AwaitFrame(frame);  // sync, this thread
```

`GetCallQueue()` is **null** when not queued. That is the `mat_queue_mode` 0 path.

Source 2013 `IMatRenderContext::GetCallQueue` is absolute vtable slot **146** (this + `IRefCounted` AddRef/Release). **Confirmed from HL2VR source** (count of `virtual` in `imaterialsystem.h`). Black Mesa’s 6-arg PushRT is at `+0x23C` (slot 143) vs Source `+0x1A0` (slot 104), so BM may have extra slots (**Unknown** until probe). Implementation probes 146 and 185 under SEH.

## `OnFrameBegin` (the actual algorithm)

**Confirmed from HL2VR source**, `VRManager::OnFrameBegin`:

```
if (IsMatQueueModeEnabled())
    SyncFrameGetPoses(m_frameCounter, cur, predicted);  // wait for *previous* queued WGP
++m_frameCounter;
Mat_AwaitFrame(m_frameCounter);                         // queue or sync WGP for *this* frame
if (!IsMatQueueModeEnabled())
    SyncFrameGetPoses(m_frameCounter, cur, predicted);
Mat_SetHeadsetRenderingPose(queued ? predicted : cur);
```

Queued: the game thread never blocks in WaitGetPoses except via `SyncFrameGetPoses` (500 ms cap). Rendering uses **predicted** HMD pose. The material thread blocks in WaitGetPoses **before** the queued draws for that frame id, which is the compositor cadence.

Not queued: WaitGetPoses on the game thread; render with **current** pose.

`WaitGetPoses(&hmdPose, 1, &predictedHmdPose, 1)` — both render and game/predicted arrays, HMD only.

## Present timing

Present is D3D9 `PresentEx` → swapchain CS thread → `DxvkSubmissionQueue`. VR Submit is **inside** that queue, not from the client after Present returns. `PostPresentHandoff` is `Submit_PostPresentCallback` on the same worker.

## Why Black Mesa’s previous waiter was worse

**Confirmed from Black Mesa source.** `PoseWaiterThreadMain` calls `WaitGetPoses` at THREAD_PRIORITY_HIGHEST forever. `WaitPosesForStereoFrame` (RenderView / game thread) only waits ~8 ms for a newer tick, then renders whatever pose exists. Present/`VR::Update` then Submits. Three threads touch compositor state. Comments in-tree already record heap corruption and 100↔7 fps from concurrent WaitGetPoses.

HL2VR’s rule: **one WaitGetPoses per frame, on the thread that owns Present, after the previous frame’s PostPresentHandoff.**

## Black Mesa port

1. Delete the pose-waiter thread for OpenVR.
2. `OnFrameBegin` at the first gameplay `RenderView` of the frame (`WaitPosesForStereoFrame`).
3. If `GetCallQueue()` works, queue WGP; game thread `SyncFrameGetPoses`.
4. If not (mode 0, probe fail, OpenXR), WaitGetPoses on the calling thread — HL2VR’s null-queue path.
5. Crash-sticky `hl2vr_wgp` only disables **queued** WGP, not sync WGP.

`mat_queue_mode` 2 stays the gameplay target (`UpdateAutoMatQueueMode`). Do not disable it to paper over WGP placement.
