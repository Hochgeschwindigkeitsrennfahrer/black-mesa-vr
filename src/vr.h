#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "openvr.h"
#include "vector.h"
#include "openxr_bridge_protocol.h"
#include "vr_runtime_backend.h"
#include <cstdint>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <cstring>
#include <chrono>
#include <d3d9.h>

struct D3D9_TEXTURE_VR_DESC;
struct ModelRenderInfo_t;
class Game;
class ITexture;
class IMatRenderContext;
class ICallQueue;
class CFunctor;
class CViewSetup;
class CUserCmd;
class Hl2vrAwaitFrameFunctor;
class McPauseHudFunctor;

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
struct IDirect3DPixelShader9;

using TextureStateMutex = std::recursive_mutex;

struct SharedTextureHolder
{
    vr::VRVulkanTextureData_t m_VulkanData{};
    vr::Texture_t m_VRTexture{};
    uint64_t m_SharedHandle = 0;
    uint32_t m_SharedHandleType = 0;
    uint32_t m_SharedHandleValid = 0;
};

struct D3DAimLineOverlayEyeState
{
    bool valid = false;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float widthPixels = 0.0f;
    float outlinePixels = 0.0f;
    float endpointPixels = 0.0f;
    uint32_t color = 0;
    uint32_t outlineColor = 0;
};

class VR
{
public:
    Game* m_Game = nullptr;

    VrRuntimeBackend m_RuntimeBackend = VrRuntimeBackend::OpenVR;
    VrRuntimeBackend m_RequestedRuntimeBackend = VrRuntimeBackend::OpenVR;
    bool m_RuntimeBackendFallbackToOpenVR = true;
    bool m_OpenXrLoaderAvailable = false;
    bool m_OpenXrHelperBridgeActive = false;
    bool m_OpenXrSwapGameEyeOrigins = false;
    L4D2VROpenXrPoseDesc m_OpenXrLastHmdPose{};
    uint32_t m_OpenXrLastHmdPoseGeneration = 0;
    // Pose actually used for the stereo RenderView pair. Published with the
    // eye textures so OpenXR xrEndFrame does not pair a fresh locate with a
    // late image (head-turn jitter). SteamVR "motion smoothing" does not
    // apply on this OpenXR helper path.
    L4D2VROpenXrPoseDesc m_OpenXrStereoRenderPose{};
    bool m_OpenXrStereoRenderPoseValid = false;
    L4D2VROpenXrInputStateDesc m_OpenXrLastInputState{};
    uint32_t m_OpenXrLastInputStateGeneration = 0;
    uint32_t m_ControllerFamily = L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
    L4D2VROpenXrSharedTextureDesc m_OpenXrSharedEyeTextures[L4D2VR_OPENXR_EYE_COUNT]{};
    std::atomic<uint32_t> m_OpenXrSharedEyeTextureReadyMask{ 0 };
    // Dedicated publish copies. Without these the helper blits straight out of
    // the live engine eye RT, which the next RenderView overwrites mid-copy at
    // ~200 stereo pairs/s, so its swapchain image mixes several game frames
    // (ghost edges while the head turns, 2026-08-30). Rotating slots let the
    // game keep writing while the helper still reads the previous image.
    // 4: visible + helper-blitting (may differ when the helper lags a frame)
    // + one pending GPU copy + one free to write. 3 forced a throttle whenever
    // the helper held a superseded slot (bridge v14 consumed-frame gate).
    static constexpr uint32_t kOpenXrPublishSlots = 4;
    IDirect3DTexture9* m_D9OpenXrPublishTexture[L4D2VR_OPENXR_EYE_COUNT][kOpenXrPublishSlots]{};
    IDirect3DSurface9* m_D9OpenXrPublishSurface[L4D2VR_OPENXR_EYE_COUNT][kOpenXrPublishSlots]{};
    L4D2VROpenXrSharedTextureDesc m_OpenXrPublishDesc[L4D2VR_OPENXR_EYE_COUNT][kOpenXrPublishSlots]{};
    uint32_t m_OpenXrPublishSlot = 0;
    uint32_t m_OpenXrPublishWidth = 0;
    uint32_t m_OpenXrPublishHeight = 0;
    bool m_OpenXrPublishReady = false;
    bool m_OpenXrPublishActive = false;
    double m_OpenXrLastPublishMs = 0.0;
    std::atomic<uint32_t> m_OpenXrLastPublishedSharedTextureFrameId{ 0 };
    std::atomic<uint32_t> m_OpenXrSubmitFrameId{ 1 };
    // Deferred publish (OpenXrDeferredPublish=true). The eye copy
    // into a slot is followed by a D3DQUERYTYPE_EVENT; the slot is handed to
    // the helper on a later Present once the GPU signalled it, instead of
    // vkDeviceWaitIdle on every publish (that serialised CPU and GPU: frame
    // time = CPU + GPU). The helper has no GPU fence to us, so a descriptor
    // may only go out after the copy is complete; the event is that proof.
    // The pose the pair was rendered with travels with it. Slot states:
    // visible (helper may read), pending (GPU copy in flight), free.
    // See docs/openxr-eye-publish.md.
    struct OpenXrPendingPublish
    {
        uint32_t slot = 0;
        bool havePose = false;
        bool panel2d = false;
        L4D2VROpenXrPoseDesc pose{};
        double issuedMs = 0.0;
    };
    static constexpr uint32_t kOpenXrNoSlot = 0xffffffffu;
    static constexpr uint32_t kOpenXrMaxPending = kOpenXrPublishSlots - 1;
    OpenXrPendingPublish m_OpenXrPending[kOpenXrMaxPending]{};
    uint32_t m_OpenXrPendingCount = 0;
    IDirect3DQuery9* m_OpenXrPublishQuery[kOpenXrPublishSlots]{};
    // Slot the helper was last told about; it may be reading it any time
    // until a newer pair is published.
    uint32_t m_OpenXrVisibleSlot = kOpenXrNoSlot;
    // QPC ms at which each slot last stopped being visible (safety margin,
    // OpenXrSlotCoolingMs).
    double m_OpenXrSlotFreedMs[kOpenXrPublishSlots]{};
    // Frame id last published from each slot (0 = never). The helper reports
    // the frame id whose blit finished (helperConsumedFrameId); a slot is
    // writable only once that is >= its frame id. Before this gate the reuse
    // window was a 4 ms timer, and the helper's blit queue sits behind the
    // game's GPU work when GPU-bound, so the game overwrote slots mid-blit:
    // tearing while turning in low-fps areas (2026-09-06).
    uint32_t m_OpenXrSlotFrameId[kOpenXrPublishSlots]{};
    uint32_t m_OpenXrHelperConsumedFrameId = 0;
    uint32_t m_OpenXrHelperConsumedCount = 0;
    double m_OpenXrHelperConsumedChangedMs = 0.0;
    bool m_OpenXrHelperFeedbackLive = false;
    uint32_t m_OpenXrDeferredPublishes = 0;
    uint32_t m_OpenXrDeferredThrottles = 0;
    uint32_t m_OpenXrDeferredHelperHolds = 0;
    uint32_t m_OpenXrDeferredDropped = 0;
    uint32_t m_OpenXrDeferredMaxPending = 0;
    uint32_t m_OpenXrPaceWaits = 0;
    double m_OpenXrPaceWaitMs = 0.0;
    double m_OpenXrPaceWaitMaxMs = 0.0;
    // Publish every pending slot whose copy the GPU has completed (newest
    // wins; older completed ones are dropped as stale).
    void PollOpenXrDeferredPublish();
    // Block until the oldest pending copy's event signals (bounded GPU
    // run-ahead, OpenXrMaxPending). Never WaitDeviceIdle: that is
    // vkDeviceWaitIdle with no timeout and hangs the process.
    void WaitOldestOpenXrPending();
    void RefreshOpenXrHelperConsumed(double nowMs);
    uint32_t PickFreeOpenXrPublishSlot(double nowMs) const;
    void ResetOpenXrDeferredPublish();
    // Pose -> pair -> frame id, in that order (helper samples in the same
    // order). slot == kOpenXrNoSlot publishes the live eye RTs.
    void PublishOpenXrPair(uint32_t slot, const OpenXrPendingPublish& pend);

    vr::IVRSystem* m_System = nullptr;
    vr::IVRInput* m_Input = nullptr;
    vr::IVROverlay* m_Overlay = nullptr;
    vr::VROverlayHandle_t m_HUDTopHandle = vr::k_ulOverlayHandleInvalid;
    vr::IVRCompositor* m_Compositor = nullptr;

    // Eye / G-buffer size. L4D2VR uses OpenVR recommended size. Named RTs at
    // that size died on BM, so the blit path matches G-buffer to recommended
    // (hmd_native) or fits HMD aspect in the window if that crash-stickies.
    uint32_t m_RenderWidth = 0;
    uint32_t m_RenderHeight = 0;
    uint32_t m_AntiAliasing = 0;
    bool UseVrMsaa() const
    {
        return m_AntiAliasing == 2 || m_AntiAliasing == 4
            || m_AntiAliasing == 8 || m_AntiAliasing == 16;
    }
    float m_Aspect = 1.f;
    float m_Fov = 90.f;
    // Per-eye CalcFovFromProjection (widest of the four raw half-angles).
    float m_FovLeft = 90.f;
    float m_FovRight = 90.f;
    float m_VRScale = 39.37f;
    float m_Ipd = 0.063f;
    float m_IpdScale = 1.0f;
    float m_EyeZ = 0.0f;

    Vector m_HmdForward{};
    Vector m_HmdRight{};
    Vector m_HmdUp{};
    Vector m_HmdPosAbs{};
    Vector m_HmdPosAbsZero{};
    bool m_HmdOriginLatched = false;
    QAngle m_HmdAngAbs{};
    QAngle m_HmdAngAbsZero{};
    // Right-hand tracking in the same Source space as m_HmdPosAbs (sd805 / Portal 2).
    bool m_ControllerPoseValid = false;
    Vector m_RightControllerPosAbs{};
    QAngle m_RightControllerAngAbs{};
    Vector m_RightControllerForward{};
    Vector m_RightControllerRight{};
    Vector m_RightControllerUp{};
    float m_RightControllerSpeedMs = 0.f;
    Vector m_RightControllerRelVel{};
    Vector m_HmdTrackForward{};
    vr::HmdMatrix34_t m_RightControllerTracking{};
    bool m_RightControllerTrackingValid = false;
    // Physical left/right (not gameplay-swapped). Gun still uses m_RightController*
    // filled from AimControllerRole(). Independent hand meshes use these.
    Vector m_LeftControllerPosAbs{};
    QAngle m_LeftControllerAngAbs{};
    vr::HmdMatrix34_t m_LeftControllerTracking{};
    vr::TrackedDeviceIndex_t m_LeftControllerDevice = vr::k_unTrackedDeviceIndexInvalid;
    bool m_LeftControllerTrackingValid = false;
    Vector m_PhysicalRightPosAbs{};
    QAngle m_PhysicalRightAngAbs{};
    vr::HmdMatrix34_t m_PhysicalRightTracking{};
    vr::TrackedDeviceIndex_t m_PhysicalRightDevice = vr::k_unTrackedDeviceIndexInvalid;
    bool m_PhysicalRightTrackingValid = false;
    Vector m_ViewmodelForward{};
    Vector m_ViewmodelRight{};
    Vector m_ViewmodelUp{};
    mutable std::recursive_mutex m_ControllerMutex;
    std::string m_LastViewmodelModel;
    // Everything derived from m_LastViewmodelModel by substring matching,
    // computed once in NoteViewmodelModel when the model changes. The pose /
    // scale / two-hand / numpad lookups used to lowercase-copy the model path
    // (heap allocation) and run 20-40 strstr per call, several calls per
    // DrawModelExecute and per attachment query. Guarded by m_ControllerMutex.
    struct ViewmodelClass
    {
        std::string numpadKey;      // vm_numpad key ("" = no model)
        float ox = 0.f, oy = 0.f, oz = 0.f;   // built-in pose table (Source units)
        float ax = 0.f, ay = 0.f, az = 0.f;   // built-in angle table (degrees)
        float fixedScale = 0.f;     // >0: use this instead of g_ViewmodelScale
        float scaleMul = 1.f;       // multiplier on top of the base scale
        bool shotgun = false;
        bool crowbar = false;
        bool scoped = false;
        bool rpg = false;
        bool gluon = false;
        bool mp5 = false;
    };
    ViewmodelClass m_VmClass;
    static void ClassifyViewmodel(const char* modelName, ViewmodelClass& out);
    // Lock-free-ish mirror of the class flags for hot hooks (sprite quads,
    // beam FX) that must not take m_ControllerMutex per call.
    std::atomic<uint32_t> m_VmClassFlags{ 0 };
    enum : uint32_t
    {
        kVmFlagShotgun = 1u << 0,
        kVmFlagCrowbar = 1u << 1,
        kVmFlagScoped = 1u << 2,
        kVmFlagRpg = 1u << 3,
        kVmFlagGluon = 1u << 4,
        kVmFlagMp5 = 1u << 5,
    };
    bool ViewmodelIsMp5() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagMp5) != 0; }
    bool ViewmodelIsRpg() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagRpg) != 0; }
    bool ViewmodelIsGluon() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagGluon) != 0; }
    bool ViewmodelIsScoped() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagScoped) != 0; }
    bool ViewmodelIsCrowbar() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagCrowbar) != 0; }
    bool m_HasViewmodelBake = false;
    float m_ViewmodelBakeOx = 0.f;
    float m_ViewmodelBakeOy = 0.f;
    float m_ViewmodelBakeOz = 0.f;
    bool m_FirstAttackLogged = false;
    uint32_t m_FirstAttackPresentTick = 0;
    int m_FirstAttackSpikeLogs = 0;
    std::mutex m_PoseMutex;
    vr::TrackedDevicePose_t m_WaitedPoses[vr::k_unMaxTrackedDeviceCount]{};
    vr::TrackedDevicePose_t m_RenderPoses[vr::k_unMaxTrackedDeviceCount]{};
    vr::TrackedDevicePose_t m_PredictedPoses[vr::k_unMaxTrackedDeviceCount]{};
    vr::HmdMatrix34_t m_HmdTrackingPose{};
    vr::HmdMatrix34_t m_StereoFrameTrackingPose{};
    bool m_StereoFrameTrackingPoseValid = false;
    std::atomic<DWORD> m_WaitedPoseTick{ 0 };
    std::mutex m_Hl2vrFrameMutex;
    std::condition_variable m_Hl2vrFrameCv;
    uint64_t m_Hl2vrFrameCounter = 0;
    std::atomic<uint64_t> m_Hl2vrAwaitedId{ 0 };
    std::atomic<uint64_t> m_Hl2vrHandoffId{ 0 };
    int m_Hl2vrCallQueueSlot = -1;
    bool m_Hl2vrCallQueueProbed = false;
    uint32_t m_Hl2vrQueuedWgpFrames = 0;
    Vector m_SetupOrigin{};
    Vector m_SetupOriginToHMD{};
    float m_StereoZNear = 7.f;
    float m_StereoZFar = 28377.f;
    Vector m_FlashlightOrigin{};
    Vector m_FlashlightForward{};
    bool m_FlashlightLive = false;
    bool m_VrGlovesDrawnIntoScene = false;
    IDirect3DBaseTexture9* m_SceneCubemap = nullptr;

    bool m_ReShadeVRCompat = false;
    vr::VRTextureBounds_t m_TextureBounds[2]{};

    enum TextureID
    {
        Texture_None = -1,
        Texture_LeftEye,
        Texture_RightEye,
        Texture_LeftEyeSubmit,
        Texture_RightEyeSubmit,
        Texture_NekoPostLinearInput,
        Texture_NekoPostSmallInput,
        Texture_HUD,
        Texture_Scope,
        Texture_RearMirror,
        Texture_DesktopMirror,
        Texture_Blank,
        // Rotating OpenXR publish copies. Deliberately has no branch in the
        // DXVK CreateTexture post-create chain: it must only make the texture
        // exportable, never bind itself to m_D9*EyeSurface / m_VK*.
        Texture_OpenXrPublish,
        // GMod vrmod 2W x 3H shared ring. Exportable so the helper can UV-crop
        // band (M-2) without a game-thread StretchRect.
        Texture_McRing
    };

    TextureID m_CreatingTextureID = Texture_None;

    ITexture* m_LeftEyeTexture = nullptr;
    ITexture* m_RightEyeTexture = nullptr;
    ITexture* m_LeftEyeSubmitTexture = nullptr;
    ITexture* m_RightEyeSubmitTexture = nullptr;
    ITexture* m_NekoPostLinearInputTexture = nullptr;
    ITexture* m_NekoPostSmallInputTexture = nullptr;
    ITexture* m_HUDTexture = nullptr;
    ITexture* m_ScopeTexture = nullptr;
    ITexture* m_RearMirrorTexture = nullptr;
    ITexture* m_DesktopMirrorTexture = nullptr;
    ITexture* m_BlankTexture = nullptr;

    IDirect3DSurface9* m_D9LeftEyeSurface = nullptr;
    IDirect3DSurface9* m_D9RightEyeSurface = nullptr;
    IDirect3DSurface9* m_D9LeftEyeDepthSurface = nullptr;
    IDirect3DSurface9* m_D9RightEyeDepthSurface = nullptr;
    IDirect3DSurface9* m_D9LeftEyeSubmitSurface = nullptr;
    IDirect3DSurface9* m_D9RightEyeSubmitSurface = nullptr;
    IDirect3DSurface9* m_D9NekoPostLinearInputSurface = nullptr;
    IDirect3DSurface9* m_D9NekoPostSmallInputSurface = nullptr;
    IDirect3DPixelShader9* m_D9NekoPostOutputTransferPixelShader = nullptr;
    IDirect3DDevice9* m_D9NekoPostOutputTransferShaderDevice = nullptr;
    IDirect3DTexture9* m_D9HUDTexture = nullptr;
    IDirect3DSurface9* m_D9HUDSurface = nullptr;
    IDirect3DSurface9* m_D9ScopeSurface = nullptr;
    IDirect3DTexture9* m_D9ScopeLensScratchTexture = nullptr;
    IDirect3DSurface9* m_D9ScopeLensScratchSurface = nullptr;
    IDirect3DTexture9* m_D9ScopeLensTexture = nullptr;
    IDirect3DSurface9* m_D9ScopeLensSurface = nullptr;
    IDirect3DSurface9* m_D9RearMirrorSurface = nullptr;
    IDirect3DTexture9* m_D9DesktopMirrorTexture = nullptr;
    IDirect3DSurface9* m_D9DesktopMirrorSurface = nullptr;
    IDirect3DSurface9* m_D9BlankSurface = nullptr;
    IDirect3DSurface9* m_D9FrameColorSurface = nullptr;
    IDirect3DTexture9* m_D9LeftEyeTexture = nullptr;
    IDirect3DTexture9* m_D9RightEyeTexture = nullptr;
    IDirect3DTexture9* m_D9LeftEyeSubmitTexture = nullptr;
    IDirect3DTexture9* m_D9RightEyeSubmitTexture = nullptr;
    IDirect3DTexture9* m_D9FrameColorTexture = nullptr;

    SharedTextureHolder m_VKLeftEye;
    SharedTextureHolder m_VKRightEye;
    SharedTextureHolder m_VKBackBuffer;
    SharedTextureHolder m_VKHUD;
    SharedTextureHolder m_VKScope;
    SharedTextureHolder m_VKScopeLens;
    SharedTextureHolder m_VKRearMirror;
    SharedTextureHolder m_VKBlankTexture;
    SharedTextureHolder m_VKMcRing;
    bool m_BackBufferTextureValid = false;

    mutable TextureStateMutex m_TextureMutex;

    bool m_IsVREnabled = false;
    bool m_IsInitialized = false;
    std::atomic<bool> m_RenderedNewFrame{ false };
    std::atomic<bool> m_RenderedHud{ false };
    std::atomic<bool> m_NativeDesktopHudPainted{ false };
    bool m_MenuBlankSubmitted = false;
    std::atomic<bool> m_HasSubmittedSceneFrame{ false };
    std::atomic<uint32_t> m_SubmitPoseToken{ 0 };
    std::atomic<uint32_t> m_LastSubmittedPoseToken{ 0 };
    std::atomic<bool> m_SubmitInFlight{ false };
    std::atomic<uint32_t> m_LastSubmittedCompositorFrameIndex{ 0 };
    std::atomic<bool> m_CreatedVRTextures{ false };
    bool m_SkipBlockingPoseWait = false;
    std::atomic<int> m_LastPoseWaitError{ static_cast<int>(vr::VRCompositorError_None) };
    std::atomic<uint32_t> m_RenderCompletedFrameId{ 0 };
    std::atomic<uint32_t> m_LastSubmittedFrameId{ 0 };
    std::atomic<uint32_t> m_QueuedSubmitStaleStreak{ 0 };
    std::atomic<uint64_t> m_PresentExclusiveLockWaitUsLast{ 0 };
    std::atomic<uint64_t> m_PresentExclusiveLockWaitUsMax{ 0 };
    std::atomic<uint32_t> m_PresentCallCount{ 0 };
    std::atomic<uint64_t> m_PresentFrameIntervalUsMax{ 0 };
    std::atomic<uint32_t> m_SubmitVRTexturesEntryCount{ 0 };
    std::atomic<uint32_t> m_SubmitInFlightSkipCount{ 0 };
    std::atomic<uint32_t> m_CompositorFrameIndexDedupSkipCount{ 0 };
    std::atomic<uint32_t> m_ActualCompositorSubmitCount{ 0 };
    std::atomic<uint32_t> m_SubmitEyeNoneCount{ 0 };
    std::atomic<uint32_t> m_SubmitEyeAlreadySubmittedCount{ 0 };
    std::atomic<uint32_t> m_SubmitEyeOtherErrorCount{ 0 };
    std::atomic<uint32_t> m_PoseWaitCount{ 0 };
    std::atomic<uint32_t> m_PoseWaitOvershootCount{ 0 };
    std::atomic<uint64_t> m_PoseWaitOvershootUsMax{ 0 };
    std::atomic<uint32_t> m_RenderThreadId{ 0 };
    std::atomic<bool> m_QueuedDesktopMirrorPreOverlayReady{ false };
    std::atomic<bool> m_QueuedEyeSubmitIsolationReady{ false };

    int m_QueuedSubmitWaitMs = 0;
    bool m_RenderPipelineDebugLog = false;
    float m_RenderPipelineDebugLogHz = 1.0f;
    bool m_ShadowTweaksEnabled = false;
    std::atomic<bool> m_DesktopMirrorHasImage{ false };
    bool m_DesktopMirrorEnabled = false;
    int m_DesktopMirrorEye = 1;
    bool m_DesktopMirrorKeepAspect = true;
    bool m_DesktopMirrorLinearFilter = true;
    bool m_DesktopMirrorHidePluginOverlaysRequested = false;
    bool m_DesktopMirrorHidePluginOverlays = false;
    bool m_ItemModelLabelEnabled = false;
    bool m_D3DAimLineOverlayEnabled = false;
    std::array<IDirect3DSurface9*, 2> m_D3DAimLineOverlayBackupSurfaces{};
    std::array<bool, 2> m_D3DAimLineOverlayBackupValid{};

    // Fields required by L4D2VR's d3d9_device.cpp Present path. Defaults keep
    // the queued/overlay/spike branches inactive.
    mutable std::recursive_mutex m_SourceRenderConsumerGate;
    std::atomic<uint32_t> m_SourceRenderQueueBuildCount{ 0 };
    std::atomic<uint32_t> m_SourceRenderQueueAuxPendingCount{ 0 };
    std::atomic<bool> m_SourceRenderQueueOwnershipUncertain{ false };
    std::atomic<uint32_t> m_SourceRenderQueueMarkerQueuedId{ 0 };
    std::atomic<uint32_t> m_SourceRenderQueueMarkerCompletedId{ 0 };
    inline bool IsSourceRenderQueueBusy() const
    {
        if (m_SourceRenderQueueBuildCount.load(std::memory_order_acquire) != 0)
            return true;
        if (m_SourceRenderQueueAuxPendingCount.load(std::memory_order_acquire) != 0)
            return true;
        if (m_SourceRenderQueueOwnershipUncertain.load(std::memory_order_acquire))
            return true;
        return m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire) !=
            m_SourceRenderQueueMarkerCompletedId.load(std::memory_order_acquire);
    }

    bool m_PresentSpikeDebugLog = false;
    float m_PresentSpikeThresholdMs = 32.0f;
    float m_PresentSpikeDebugLogHz = 1.0f;
    uint64_t m_PresentSpikeUpdatePrePoseUs = 0;
    uint64_t m_PresentSpikeUpdatePosesUs = 0;
    uint64_t m_PresentSpikeUpdateSettingsUs = 0;
    uint64_t m_PresentSpikeUpdateSubmitUs = 0;
    uint64_t m_PresentSpikeUpdatePlayerUs = 0;
    uint64_t m_PresentSpikeUpdateTrackingUs = 0;
    uint64_t m_PresentSpikeUpdateInputUs = 0;
    uint64_t m_PresentSpikeUpdateTailUs = 0;
    uint64_t m_PresentSpikeSubmitTimingDataUs = 0;
    uint64_t m_PresentSpikeSubmitTextureLockUs = 0;
    uint64_t m_PresentSpikeSubmitPrepareUs = 0;
    uint64_t m_PresentSpikeSubmitQueueLockUs = 0;
    uint64_t m_PresentSpikeSubmitOverlayBindUs = 0;
    uint64_t m_PresentSpikeSubmitLeftEyeUs = 0;
    uint64_t m_PresentSpikeSubmitRightEyeUs = 0;
    uint64_t m_PresentSpikeSubmitFinishUs = 0;
    uint64_t m_PresentSpikeSubmitHandHudUs = 0;
    uint64_t m_PresentSpikeSubmitSlotGateUs = 0;
    int m_PresentSpikeSubmitSlotIndex = 0;
    uint32_t m_PresentSpikeSubmitSlotLast = 0;
    bool m_PresentSpikeSubmitSlotSkipped = false;
    bool m_PresentSpikeSubmitSlotAdvanced = false;
    std::atomic<uint32_t> m_PresentSpikeEndFrameSeq{ 0 };
    std::atomic<uint64_t> m_PresentSpikeEndFrameEntryUs{ 0 };
    std::atomic<uint64_t> m_PresentSpikeEndFrameExitUs{ 0 };
    std::atomic<uint32_t> m_PresentSpikeEndFrameThreadId{ 0 };

    std::atomic<bool> m_QueuedCompositorSubmitPending{ false };
    std::atomic<uint64_t> m_QueuedCompositorSubmitTotalUsLast{ 0 };
    std::atomic<uint64_t> m_QueuedCompositorSubmitSlotUsLast{ 0 };
    std::atomic<uint64_t> m_QueuedCompositorSubmitQueueUsLast{ 0 };
    std::atomic<uint64_t> m_QueuedCompositorSubmitEyesUsLast{ 0 };
    std::atomic<uint64_t> m_QueuedCompositorSubmitHandoffUsLast{ 0 };
    std::atomic<uint32_t> m_QueuedCompositorSubmitQueuedCount{ 0 };
    std::atomic<uint32_t> m_QueuedCompositorSubmitCompletedCount{ 0 };
    std::atomic<uint32_t> m_QueuedCompositorSubmitErrorCount{ 0 };

    bool m_HmdPoseValid = false;
    bool m_SafeLookActive = false;
    bool m_LookApplyEnabled = false;
    float m_PrevAppliedHmdYaw = 0.f;
    float m_PrevAppliedHmdPitch = 0.f;
    bool m_SoftPitchLook = false;
    bool m_ProcessInputEnabled = false;
    std::atomic<bool> m_ActionsReady{ false };
    vr::VRActionSetHandle_t m_ActionSet = vr::k_ulInvalidActionSetHandle;
    vr::VRActionSetHandle_t m_BaseActionSet = vr::k_ulInvalidActionSetHandle;
    vr::VRActiveActionSet_t m_ActiveActionSets[2]{};
    vr::VRActionHandle_t m_ActionJump = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionPrimaryAttack = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionSecondaryAttack = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionReload = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionUse = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionWalk = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionTurn = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionBooleanTurnLeft = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionBooleanTurnRight = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionNextItem = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionPrevItem = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionResetPosition = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionCrouch = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionCrouchToggle = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionFlashlight = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionScoreboard = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionPause = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionSprint = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionMenuSelect = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionMenuBack = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionMenuUp = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionMenuDown = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionMenuLeft = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionMenuRight = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionWeaponMenu = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionInventoryQuickSwitch = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionSkeletonLeft = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t m_ActionSkeletonRight = vr::k_ulInvalidActionHandle;
    bool m_CompositorAppHandoff = false;
    uint32_t m_CompositorHandoffSlowCount = 0;
    std::atomic<float> m_WalkForward{ 0.f };
    std::atomic<float> m_WalkSide{ 0.f };
    std::atomic<uint32_t> m_HeldButtons{ 0 };
    std::atomic<float> m_RotationOffsetY{ 0.f };
    std::atomic<uint32_t> m_PendingImpulse{ 0 };
    std::atomic<int> m_PendingInvDelta{ 0 };
    std::atomic<int> m_PendingWeaponSelect{ 0 };
    std::atomic<uint32_t> m_PendingWeaponSounds{ 0 };
    std::atomic<int> m_PendingWeaponSoundKind{ 0 };
    std::atomic<int> m_PendingWeaponSoundEntity{ 0 };
    std::atomic<uint32_t> m_PendingFireHaptic{ 0 };
    std::atomic<int> m_PendingGameUi{ 0 };
    std::atomic<int> m_PendingQuit{ 0 };
    bool m_GameUiVisible = false;
    DWORD m_GameUiActivateMs = 0;
    DWORD m_MenuLastVisibleMs = 0;
    int m_MenuPointerHand = 1;
    void* m_EngineVGuiFromPaint = nullptr;
    bool m_PressedTurn = false;
    bool m_StereoEyesDrawnThisFrame = false;
    // 0 = mono, 1 = left, 2 = right. Matches Source StereoEye_t.
    int m_StereoEye = 0;
    bool m_HasStereoBodyOrigin = false;
    Vector m_StereoBodyOrigin{};
    bool m_StereoCopyOffset = false;
    bool m_SeenGameplay = false;
    bool m_GameplayEligible = false;
    int m_GameplayFrames = 0;
    uint32_t m_PresentTick = 0;
    uint32_t m_EligiblePresents = 0;
    uint32_t m_PassThroughMainViews = 0;
    bool m_AppliedVrPerfCvars = false;
    std::string m_CurrentMapName;
    bool m_FrameCopyLatched = false;
    uint32_t m_FrameCopyWidth = 0;
    uint32_t m_FrameCopyHeight = 0;
    bool m_D3DHooksInstalled = false;
    int m_SubmitCount = 0;
    std::string m_CaptureSrc = "auto";
    std::string m_CaptureSrcLocked;
    int m_StereoOffsetPx = 0;
    bool m_CaptureReentry = false;
    bool m_LoggedFirstSubmit = false;
    bool m_LookApplyWanted = true;
    int m_OpenVRInitAttempts = 0;
    bool m_DirectEyeSubmit = false;
    bool m_UsedNamedRenderTargets = false;
    bool m_StereoRenderViewActive = false;
    bool m_StereoFramePoseActive = false;
    QAngle m_StereoFrameAngles{};
    Vector m_StereoFrameHmdPosAbs{};
    bool m_StereoFrameLeftCtrlValid = false;
    bool m_StereoFrameRightCtrlValid = false;
    QAngle m_StereoFrameLeftCtrlAng{};
    QAngle m_StereoFrameRightCtrlAng{};
    Vector m_StereoFrameLeftCtrlPos{};
    Vector m_StereoFrameRightCtrlPos{};
    bool m_PosesWaitedThisFrame = false;
    uint32_t m_OpenXrLastSeenHelperSubmitted = 0;
    uint32_t m_OpenXrSubmitAttempts = 0;
    uint32_t m_OpenXrPublishes = 0;
    uint32_t m_OpenXrSkippedNoNewFrame = 0;
    uint32_t m_OpenXrSkippedHelperBusy = 0;
    uint32_t m_OpenXrPublishRateTick = 0;
    bool m_NamedCreateFailed = false;
    uint32_t m_NamedRtReadyPresent = 0;

    VR() = default;
    explicit VR(Game* game);

    void Update();
    void CreateVRTextures();
    void SubmitVRTextures();
    void WaitPosesForStereoFrame(bool allowQueued = true);
    void BeginStereoFramePose();
    void EndStereoFramePose();
    void LogOpenXrPublishRate();
    void LogOpenXrPublishSetupFailure(const char* stage, uint32_t eye, uint32_t slot, unsigned hr);
    bool EnsureOpenXrPublishTextures(IDirect3DDevice9* device, UINT w, UINT h);
    void ReleaseOpenXrPublishTextures();
    Vector GetViewAngle() const;
    void UpdateScopeZoomSmooth();
    Vector GetViewOrigin(const Vector& setupOrigin) const;
    void GetViewBasis(Vector* forward, Vector* right, Vector* up) const;
    Vector GetViewOriginLeft(const Vector& setupOrigin) const;
    Vector GetViewOriginRight(const Vector& setupOrigin) const;
    Vector ControllerTrackingToWorld(const Vector& setupOrigin, const Vector& trackingPos) const;
    Vector GetRightControllerAbsPos(const Vector& eyePosition) const;
    QAngle GetRightControllerAbsAngle() const;
    Vector GetLeftControllerAbsPos(const Vector& eyePosition) const;
    QAngle GetLeftControllerAbsAngle() const;
    QAngle GetAimAngles() const;
    QAngle GetUseAimAngles() const;
    bool UseFollowsLeftHand() const { return m_GrabLatched && m_GrabHandLeft; }
    bool UseGrabActive() const { return m_GrabLatched; }
    // Weapon fire, shoot origin, and flashlight must not follow a leftover
    // left-hand Use latch. Only a live physics hold redirects those.
    bool UseGrabRedirectsFire() const { return m_GrabLatched && m_GrabUseEntityHeld; }
    bool GrabHandTrackingValid() const
    {
        if (!m_GrabLatched)
            return false;
        return m_GrabHandLeft ? m_LeftControllerTrackingValid : m_RightControllerTrackingValid;
    }
    bool TryGetVrUseOrigin(Vector& origin) const;
    bool TryGetVrGrabTarget(Vector& origin) const;
    void UpdateUseGrab(uint32_t& buttons, bool leftUse, bool rightUse);
    void ClearUseGrab();
    // Last aim written to cmd->viewangles. GetShootAngles must use this, not
    // the player's EyeAngles (those stay on the HMD for the camera).
    void RememberFireAim(const QAngle& aim);
    bool TryGetFireAim(QAngle& out) const;
    Vector GetRecommendedViewmodelAbsPos(const Vector& eyePosition) const;
    QAngle GetRecommendedViewmodelAbsAngle() const;
    float HorizontalFovForAspect(float targetAspect) const;
    // HMD union FOV, or a narrower frustum while the crossbow scope is up.
    // Stereo CViewSetup must use this (not the engine ~106 desktop FOV):
    // 106 into a ~98.5 GetProjectionRaw crop warps on head motion (2026-09-05).
    float WorldRenderFov() const;
    // Same union frustum as WorldRenderFov (per-eye Source FOV swam the gun).
    float WorldRenderFovForEye(bool left) const;
    // Incoming engine CViewSetup.fov before we overwrite it. Turns VR zoom
    // off when the engine is clearly unscoped, so a stuck latch cannot keep
    // the picture magnified.
    void NoteEngineScopeFov(float engineFov);
    void NoteStereoClipPlanes(float zNear, float zFar);
    void NoteFlashlightState(const Vector& origin, const Vector& forward);
    bool CopyFlashlightState(Vector& origin, Vector& forward) const;
    bool VrGlovesDrawnIntoScene() const { return m_VrGlovesDrawnIntoScene; }
    void CaptureFrameBeforePresent();
    bool BlitCurrentGameColorTo(IDirect3DSurface9* dst, bool flushGpu = false);
    bool BlitHmdViewFromBackbuffer(IDirect3DSurface9* dst, bool flushGpu = false);
    void FlushStereoBlitGpu();
    IDirect3DSurface9* ColorTargetForStereoEye(int stereoEye) const;
    void BeginStereoEyeBlit(IDirect3DSurface9* dst);
    void ClearStereoEyeSurfaces();
    bool EndStereoEyeBlit();
    bool StereoUnbindMatchesEye() const;
    void CaptureGameColorOnUnbind(IDirect3DSurface9* oldRt, uint32_t vpX, uint32_t vpY, uint32_t vpW, uint32_t vpH);
    void MirrorStereoToDesktopWindow();
    void BlitDesktopMirrorToBackbuffer();
    bool EnsureDesktopMirrorSurface(IDirect3DDevice9* device, UINT w, UINT h);
    void ReleaseVRRenderTargetsForDeviceReset();
    bool RefreshBackBufferTexture(bool forceRefresh = false);
    void EnsureOpticsRTTTextures() {}
    void HandleMissingRenderContext(const char* location);
    void LogVAS(const char*) {}
    void ClearD3DAimLineOverlay() {}
    void ClearD3DAimLineOverlayEye(int) {}
    bool GetD3DAimLineOverlayEye(int, D3DAimLineOverlayEyeState& out) const { out = {}; return false; }
    void ClearQueuedProjectedItemLabels() {}
    void DrawQueuedProjectedItemLabelsToSurface(IDirect3DDevice9*, int, IDirect3DSurface9*, bool = true) {}
    bool IsGameplayHudRequested() const { return false; }
    bool IsQueuedHudFresh() const { return false; }
    void OnLevelInit(const char* newmap);
    void OnLevelShutdown();
    void ApplyRenderTargetFramebufferOverride(void* materialSystem = nullptr);
    void LogFullFrameSizeIfReady();
    bool IsGameplayEligible() const { return m_GameplayEligible; }
    bool HasEngineMap() const { return !m_CurrentMapName.empty(); }
    bool ShouldCompositorSubmit() const;
    bool StereoEyeBlitActive() const { return m_StereoEyeBlitActive; }
    // GMod: queued material system writes band M of a 2W x 3H ring; helper
    // copies band (M-2). False keeps the verified single-core 1x-eye path.
    bool McPipelineEnabled() const;
    // True only while *this thread* is in a stereo world pass. Shared
    // StereoEyeBlitActive / McPipelineEnabled stay on during pause VGUI and
    // must not steal GameUI onto the 1x eyes or lie GetScreenSize to HMD.
    bool StereoWorldPassActive() const;
    bool McRingReady() const { return m_McRingReady; }
    uint32_t McWriteBand() const { return m_McFrame % 3u; }
    // HWND leftover is always D3D 16:9 @0,0. World+lighting draw to 1x eyes
    // (WorldRenderAtEyeSize); the ring is only the helper submit atlas.
    // Gutter keeps leftover 16:9 @0,0 off the left submit slice if anything
    // still stamps the ring.
    static constexpr int kMcGutter = 16;
    int McLeftOriginX() const { return kMcGutter; }
    int McRightOriginX() const { return kMcGutter + static_cast<int>(m_RenderWidth); }
    UINT McRingWidth() const
    {
        return static_cast<UINT>(McRightOriginX() + static_cast<int>(m_RenderWidth));
    }
    UINT McRingHeight() const { return m_RenderHeight * 3u; }
    bool McOriginIsRingBand(int x, int y) const;
    bool McClipAmbiguousWindowVp(int inX, int inY, int inW, int inH, int& x, int& y, int& w, int& h) const;
    // Ring dest origin for a window or eye rect. Recording (game thread) uses
    // m_StereoEye + write band. Playback (material thread) must not: queue 2
    // overlaps the next pair, so McWriteBand/m_StereoEye are the *next* frame
    // (log: functor y=0 then D3D @0,3104 / @3168,3104). Playback keeps a
    // baked band origin or the last-known eye rect on this thread.
    bool McResolveRingEyeOrigin(int inX, int inY, int inW, int inH, int& outX, int& outY) const;
    void McNoteRingEyeViewport(int x, int y, int w, int h);
    bool McLastRingEyeOrigin(int& x, int& y) const;
    bool QueueMcEyeBind(int stereoEye, bool flushGpu);
    bool QueueMcCompositeToRing(uint32_t band);
    bool QueueMcPauseHudBegin();
    bool QueueMcPauseHudEnd();
    void McBindPlaybackEye(int stereoEye, bool flushGpu);
    void McCompositeEyesToRing(uint32_t band, const L4D2VROpenXrPoseDesc* pose);
    bool CopyEyesToOpenXrPublishSlot(IDirect3DSurface9* left, IDirect3DSurface9* right,
        const OpenXrPendingPublish& pend);
    IDirect3DSurface9* McPlaybackEyeSurface() const;
    IDirect3DSurface9* McPlaybackEyeDepth() const;
    void NoteMcRecordThread();
    bool McOnRecordThread() const;
    bool SurfaceIsMcRing(IDirect3DSurface9* s) const;
    bool D3dRt0IsMcRing() const;
    IDirect3DSurface9* McRingSurface() const { return m_D9McRingSurface; }
    IDirect3DSurface9* McRingDepth() const { return m_D9McRingDepth; }
    void FinishMcStereoPair();
    void ResetMcPipeline();
    bool HudPaintActive() const;
    void SetHudPaintActive(bool active);
    bool ShouldRedirectHudRt() const { return false; }
    void SetRedirectHudRt(bool) {}
    void SetVguiPaintActive(bool active) { m_VguiPaintActive = active; }
    void NoteEngineHudRtPush(const char* name, int w, int h);
    bool EngineHudRtPushed() const { return m_EngineHudRtPushed; }
    void BlitEngineHudRtToOverlay();
    bool ComputeHudInset(int fbW, int fbH, int& x, int& y, int& w, int& h) const;
    bool StereoRedirectedToEye() const { return m_StereoRedirectedToEye; }
    IDirect3DSurface9* StereoEyeBlitDest() const { return m_StereoEyeBlitDest; }
    // True when D3D RT0 is an eye-sized world/eye surface. Lighting apply
    // Viewport(2560) often happens while a flashlight RT is still the
    // material-system stack top — IMat must still advertise HMD size.
    bool D3dRt0IsEyeSized() const;
    bool CachedRt0MatchesEyes() const;
    void NoteCachedRt0Size(UINT w, UINT h);
    void NoteStereoRedirectedToEye() { m_StereoRedirectedToEye = true; }
    // First gameplay RenderViews stay single-threaded. SetThreadMode(2) on
    // the first in-game Present (2026-08-18) ran during spawn Reset to
    // 2384x2160 and Present died after pass-through 2/8.
    static constexpr uint32_t kPassThroughViewsBeforeQueued = 8;
    bool PassThroughWarmupDone() const
    {
        return m_PassThroughMainViews >= kPassThroughViewsBeforeQueued;
    }
    void InstallDeviceHooks(IDirect3DDevice9* device);
    bool ShouldExportOpenXrEyeTexture(TextureID texID, uint32_t sampleCount) const;
    void PublishOpenXrEyeTexture(TextureID texID, const D3D9_TEXTURE_VR_DESC& desc);
    void PublishOpenXrResolvedEyeTextures(uint32_t frameId, bool panel2d);
    bool EnsureNamedEyeTextures();
    void PrepareNamedStereoFromPresent();
    bool NamedStereoReady() const;
    bool EnsureStereoEyeSurfaces();
    bool StereoEyesReady() const;
    void ClearUnusedDesktopBackbuffer();
    void ProcessInput();
    void ApplyMenuCursor();
    void DrawMenuCursorOnSurface(IDirect3DDevice9* device, IDirect3DSurface9* surf);
    void ApplyMenuNavigation();
    void QueueEscapeKey();
    void QueueVirtualKey(int vk);
    void QueueGameUiToggle(bool currentlyPaused);
    void FlushPendingGameUi();
    void FlushPendingQuit();
    void PollVrMenuEvents();
    void NoteEngineVGui(void* engineVgui);
    bool GameUiVisible() const { return m_GameUiVisible; }
    // HL2VR IsMenuUp: GameUI is actually up (engine IsGameUIVisible).
    bool IsMenuUp() const;
    // HL2VR ProcessInput: gameplay booleans stay off while GameUI is up and
    // for 0.5s after hide.
    bool MenuGameplayBlocked() const;
    // True when the pause/GameUI overlay should exist. Extra VGui_Paint of
    // PAINT_UIPANELS during gameplay is GameUI glass, not HEV HUD.
    bool PauseUiActive() const;
    // HL2VR pause overlay: GameUI while the engine is actually paused in a
    // loaded map. Loading is GameUI && !paused — that stays a 2D panel.
    bool WantPauseWorldOverlay() const;
    bool EngineGameUiVisible() const;
    void SyncGameUiFromEngine();
    bool Want2dMenuPanel() const;
    bool WantMenuPanelLatch() const;
    void GetMenuOverlayMetrics(float& distM, float& widthM, float& yOffM) const;
    void LatchMenuPanelIfNeeded();
    void BlitPauseMenuToHudOverlay();
    // Pause GameUI extra-paint dest (window-sized D3D A8R8G8B8). Named
    // MaterialSystem RTs fail after startup on this DLL.
    bool HudOverlayPixelSize(UINT& w, UINT& h) const;
    bool ForceHudOverlayViewport(int& x, int& y, int& w, int& h) const;
    bool BindPauseHudForExtraPaint();
    void UnbindPauseHudAfterExtraPaint();
    // Alpha write on the bound pause HUD. D3D bind/clear is BindPauseHudForExtraPaint.
    void PreparePauseHudForVgui(IMatRenderContext* ctx);
    // HL2VR restores OverrideAlphaWriteEnable(false, true) after RenderHUD.
    void FinishPauseHudExtraPaint(IMatRenderContext* ctx);
    void StampPauseOverlayCursor();
    bool IntersectMenuPanelUv(float& u, float& v);
    void NoteHudPainted() { m_HudPaintedThisFrame.store(true, std::memory_order_release); }
    bool HudPaintedThisFrame() const { return m_HudPaintedThisFrame.load(std::memory_order_acquire); }
    void UpdateCrowbarMelee();
    // HL2VR physical crowbar (vr_crowbar.mdl on the hand, tip-velocity hits).
    static bool IsCrowbarWeaponModel(const char* model);
    bool IsCrowbarEquipped() const;
    bool PhysicalCrowbarModelReady() const { return m_PhysicalCrowbarModel != nullptr; }
    bool ComputePhysicalCrowbarPose(Vector& origin, QAngle& angles, Vector& topPos, Vector& topFwd, Vector& shaftStart) const;
    bool DrawPhysicalCrowbar(void* modelRender, const ModelRenderInfo_t& vmInfo);
    void EnsurePhysicalCrowbarModel();
    void GetRightHandAttach(Vector& posMeters, Vector& rotDeg) const;
    bool GetRightHandAttachCurls(float curls[5]) const;
    bool RightHandAttachActive() const;
    // Crowbar HEV fist (thumb..pinky, 0-1). Other weapons do not pose the glove.
    bool GetDefaultRightHandWeaponCurls(float curls[5]) const;
    // Traces the firing ray on the game thread and caches where it lands, so
    // the per-eye overlay only has to project a point. Engine traces are not
    // safe from the render thread.
    void UpdateAimCrosshair();
    // Cleared by UpdateAimCrosshair when the reticle is off or the ray missed.
    bool AimCrosshairVisible() const { return m_AimCrosshairValid; }
    bool TryGetAimCrosshairWorld(Vector& out) const;
    // Gordon has no gloves before the HEV suit (intro tram). Sampled on the
    // game thread; defaults true so a failed netvar scan cannot hide the hands.
    // Combined with VrHideHandsWithoutSuit — false hides HEV gloves and the
    // wrist HUD. Bare-hand GLBs still draw when HasBareHands() is true.
    bool HasHevSuit() const { return m_HasHevSuit; }
    // Raw m_bWearingSuit sample. False on the intro tram; true after EquipSuit.
    bool WearingHevSuit() const { return m_WearingHevSuit; }
    // True only while a scoped weapon is actually zoomed. Aim then comes from
    // the headset, because the scope picture is centred on the view.
    bool ScopeZoomActive() const { return m_ScopeZoomActive; }
    static bool IsScopedWeaponModel(const char* model);
    static bool IsRpgWeaponModel(const char* model);
    static bool IsGluonWeaponModel(const char* model);
    bool RpgLaserLatched() const { return m_RpgLaserLatched; }
    void SetRpgLaserActive(bool on) { m_RpgLaserActive = on; }
    bool RpgLaserActive() const { return m_RpgLaserActive || m_RpgLaserLatched; }
    void UpdateRpgLaserPoint();
    void NoteRpgLaserWorld(const Vector& world);
    bool TryGetRpgLaserWorld(Vector& out) const;
    bool RpgLaserVisible() const { return m_RpgLaserPointValid; }
    bool WeaponMenuOpen() const { return m_WeaponMenuOpen; }
    bool WeaponMenuClickHeld() const { return m_WeaponMenuClickHeld; }
    bool WeaponMenuStickHeld() const;
    bool EmptyHands() const { return m_EmptyHands; }
    void UpdateWeaponMenu(bool stickClickHeld, float deltaMs);
    void ApplyWeaponMenuWorldPose();
    void DrawWeaponMenu(IDirect3DDevice9* device, UINT w, UINT h,
        const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up);
    bool UpdateWeaponFireHaptics();
    void AfterCreateMoveFireHaptics();
    bool TryGetVrMuzzleWorld(Vector& origin) const;
    bool TryGetVrShootOrigin(Vector& origin) const;
    // Visual beam/laser segment: muzzle start, aim-ray impact end.
    // Cached per frame so tau/gluon hooks get the same result.
    bool TryGetVrBeamSegment(Vector& start, Vector& end, Vector* outNormal = nullptr) const;
    mutable Vector m_CachedBeamStart{};
    mutable Vector m_CachedBeamEnd{};
    mutable Vector m_CachedBeamNormal{};
    mutable int m_CachedBeamFrame = -1;
    bool ScaleViewmodelRenderableAttachment(void* renderable, Vector& origin) const;
    void FlushPendingWeaponSounds();
    void QueueWeaponMenuSound(uint32_t bit, int kind = 0, int entityIndex = 0);
    const char* WeaponMenuDrawSoundName(int kind) const;
    float ViewmodelVisualScale() const;
    void ApplyViewmodelVisualScale(Vector& world) const;
    bool IsPerformingMelee() const { return m_PerformingMelee; }
    bool TryGetMeleeBladeViewAngles(QAngle& out) const;
    bool TryGetMeleeTraceOrigin(Vector& origin) const;
    // Visible crowbar strike point and blade axis while a VR swing is live, so
    // the engine's melee trace runs along the bar the player can see.
    bool TryGetMeleeAim(Vector& origin, QAngle& angles) const;
    // True while VR should keep fire/reload/equip sequences running.
    bool WantsWeaponActionAnim() const;
    void GetRightGlovePalmOffsetMeters(Vector& meters) const;
    bool WantsRightGloveVisible() const;
    bool WantsRightGloveWeaponGripCurl() const;
    bool HudOverlayReady() const { return m_HudOverlayReady; }
    void EnsureHudOverlay();
    void SubmitHudOverlay();
    // Bind HUD overlay texture. Caller must already hold LockSubmissionQueue.
    void BindHudOverlayWhileQueueLocked();
    void ClearHudSurface(bool opaque);
    void TickMatQueueFromRenderView();
    uint32_t HeldButtons() const { return m_HeldButtons.load(std::memory_order_acquire); }
    static constexpr uint32_t kWeaponSoundHover = 1u;
    static constexpr uint32_t kWeaponSoundSelect = 2u;
    void NoteViewmodelModel(const char* modelName);
    void NoteViewmodelWeaponBake(const char* modelName, const char* boneName, float restX, float restY, float restZ);
    vr::ETrackedControllerRole AimControllerRole() const;
    bool DrawIndependentHandMarkers(IDirect3DSurface9* eyeSurf, int stereoEye, bool drawOverlays = true, bool drawGloves = true);
    bool DrawVrGlovesIntoBlitSource(int stereoEye);
    void DrawIndependentHandsOnDesktop();
    // Last cubemap the engine bound (weapon $envmap). Gloves sample this.
    void NoteSceneCubemap(IDirect3DBaseTexture9* texture);
    IDirect3DBaseTexture9* SceneCubemap() const { return m_SceneCubemap; }
    bool GetFingerCurls(vr::VRActionHandle_t skeletonAction, float outCurls[5]) const;
    void TryCompositorPostPresentHandoff(DWORD nowMs, DWORD poseAgeMs);

private:
    friend class Hl2vrAwaitFrameFunctor;
    friend class McPauseHudFunctor;
    void MatAwaitFrame(uint64_t frameId);
    void SyncFrameGetPoses(uint64_t frameId);
    bool QueueMatAwaitFrame(uint64_t frameId);
    ICallQueue* ProbeCallQueue();
    void ApplyHl2vrWaitedPoses(bool usePredicted);
    Vector GetViewOriginUnshifted(const Vector& setupOrigin) const;
    void SetActionManifest();
    bool GetDigitalActionData(vr::VRActionHandle_t handle, vr::InputDigitalActionData_t& out) const;
    bool GetAnalogActionData(vr::VRActionHandle_t handle, vr::InputAnalogActionData_t& out) const;
    bool PressedDigitalAction(vr::VRActionHandle_t handle, bool onChanged = false) const;
    void ApplyTurnStick(float stickX, float deltaMs);
    static bool IsGameplayMapName(const char* map);
    void PollMapFromEngine();
    bool InitOpenVR();
    bool InitOpenXR();
    bool ConsumeOpenXrTracking();
    // Copies the eyes into a free publish slot and either queues the slot
    // behind a GPU event (deferred) or publishes it right away after a device
    // idle (fallback). Returns false when nothing was consumed this Present.
    bool PrepareOpenXrEyeSurfacesForRead(const OpenXrPendingPublish& pend);
    bool EnsureMcRing(IDirect3DDevice9* device);
    void ReleaseMcRing();
    bool PublishMcRingBand(const OpenXrPendingPublish& pend);
    bool QueueMcMatFunctor(CFunctor* functor, const char* failTag);
    void McPauseHudBeginOnMatThread();
    void McPauseHudEndOnMatThread();
    void BindOpenXrActionHandles();
    bool PublishOpenXrHudOverlay(uint32_t frameId);
    void HideOpenXrHudOverlay();
    void UpdateTracking();
    void ChooseEyeRenderSize();
    bool EnsurePrivateEyeSurfaces(IDirect3DDevice9* device);
    bool EnsureFrameCopySurface(IDirect3DDevice9* device, uint32_t width, uint32_t height);
    bool FillSharedTexture(IDirect3DSurface9* surface, SharedTextureHolder& holder);
    IDirect3DSurface9* SubmitSurfaceForEye(IDirect3DSurface9* eye) const;
    void NoteMsaaEyeScene(IDirect3DSurface9* dst, bool copied);
    void ResolveMsaaEyesToSubmit(IDirect3DDevice9* device);
    void ApplyVulkanYFlip(vr::VRTextureBounds_t& bounds);
    void RefreshIpdFromHmd();
    void UpdateControllerTracking(const vr::TrackedDevicePose_t& hmdPose);
    void UpdateAutoMatQueueMode();
    void ApplyVrQualityOfLifeCvars();
    void PollSteamVrRecommendedSize();
    void TryApplySteamVrRecommendedEyeSize();
    void TickCompositorFocus();
    void ReclaimCompositorFocus(const char* reason);
    void PulseAimHaptic(unsigned short durationUs = 2500);
    void PulseHandHaptic(vr::ETrackedControllerRole hand, unsigned short durationUs, float amplitude = 0.6f);
    void UpdateViewmodelNumpadAdjust(bool paused);
    void ResolveWeaponViewmodelPose(float& ox, float& oy, float& oz, float& ax, float& ay, float& az) const;
    void ApplyViewmodelNumpadExtras(float& ox, float& oy, float& oz, float& ax, float& ay, float& az) const;
    void DrawHandHud(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
        bool leftOk, const Vector& leftOrigin, const QAngle& leftAng,
        bool rightOk, const Vector& rightOrigin, const QAngle& rightAng,
        const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up);
    void RefreshActiveWeaponModel();
    void RefreshHeldWeaponState();
    void ApplyViewmodelBasisOffsets();
    void ApplyTwoHandShotgunAim();
    bool m_TwoHandShotgunActive = false;
    int m_AutoMatQueueModeLastRequested = -999;
    std::chrono::steady_clock::time_point m_AutoMatQueueModeLastCmdTime{};
    bool m_VrCvarsApplied = false;
    bool m_MenuFpsMaxSent = false;
    int m_MenuFpsMaxLastHz = -1;
    bool m_LastCanRenderScene = true;
    DWORD m_EyeResizeSettleMs = 0;
    DWORD m_LastCompositorReclaimMs = 0;
    uint32_t m_MatQueueOkPresents = 0;
    IDirect3DQuery9* m_BlitEventQuery = nullptr;
    // Persistent D3DSBT_ALL block for DrawIndependentHandMarkers (was a
    // CreateStateBlock + Capture + Apply + Release per eye per frame).
    IDirect3DStateBlock9* m_HandEngineStateBlock = nullptr;
    UINT KnownWindowWidth() const;
    UINT KnownWindowHeight() const;
    static bool ResolveSurfaceSize(IDirect3DSurface9* surf, UINT& w, UINT& h, D3DSURFACE_DESC* outDesc = nullptr);

    IDirect3DTexture9* m_D9McRingTexture = nullptr;
    IDirect3DSurface9* m_D9McRingSurface = nullptr;
    IDirect3DSurface9* m_D9McRingDepth = nullptr;
    L4D2VROpenXrSharedTextureDesc m_OpenXrMcRingDesc{};
    L4D2VROpenXrPoseDesc m_McPoses[3]{};
    uint32_t m_McFrame = 0;
    std::atomic<DWORD> m_McRecordThreadId{ 0 };
    bool m_McRingReady = false;
    // GetCallQueue / QueueFunctorInternal failed (Quest Link log 2026-09-08).
    // Stay on the single-thread 1x-eye blit + Present publish path.
    bool m_McCallQueueDead = false;
    std::mutex m_OpenXrPublishMutex;
    IDirect3DSurface9* m_StereoEyeBlitDest = nullptr;
    bool m_StereoEyeBlitActive = false;
    bool m_LeftEyeMsaaHasScene = false;
    bool m_RightEyeMsaaHasScene = false;
    bool m_StereoRedirectedToEye = false;
    bool m_EngineHudRtPushed = false;
    bool m_VguiPaintActive = false;
    bool m_RedirectHudRt = false;
    mutable int m_HudInsetX = 0;
    mutable int m_HudInsetY = 0;
    mutable int m_HudInsetW = 0;
    mutable int m_HudInsetH = 0;
    Vector m_PrevControllerPosAbs{};
    QAngle m_PrevControllerAngAbs{};
    DWORD m_PrevControllerTick = 0;
    DWORD m_MeleeAttackUntilMs = 0;
    DWORD m_MeleeNextSwingMs = 0;
    DWORD m_PrevMeleeSampleMs = 0;
    bool m_PerformingMelee = false;
    bool m_MeleeNewSwing = true;
    void* m_MeleeHitEntity = nullptr;
    Vector m_MeleeTraceOrigin{};
    QAngle m_MeleeBladeAngles{};
    bool m_MeleeBladeAnglesValid = false;
    Vector m_CrowbarPrevTipLocal{};
    DWORD m_CrowbarLastMotionCheckMs = 0;
    DWORD m_CrowbarNextAttackMs = 0;
    DWORD m_CrowbarNextSwingSoundMs = 0;
    void* m_PhysicalCrowbarModel = nullptr;
    int m_PhysicalCrowbarLoadTries = 0;
    Vector m_AimCrosshairWorld{};
    bool m_AimCrosshairValid = false;
    bool m_HasHevSuit = true;
    bool m_WearingHevSuit = true;
    bool m_ScopeZoomActive = false;
    bool m_CrossbowZoomLatched = false;
    float m_ZoomSmoothPitch = 0.f;
    float m_ZoomSmoothYaw = 0.f;
    float m_ZoomSmoothRoll = 0.f;
    bool m_ZoomSmoothValid = false;
    double m_ZoomSmoothMs = 0.0;
    float m_EngineViewFov = 0.f;
    bool m_SawEngineZoomFov = false;
    bool m_RpgLaserLatched = false;
    bool m_RpgLaserActive = false;
    Vector m_RpgLaserWorld{};
    bool m_RpgLaserPointValid = false;
    QAngle m_LastFireAim{};
    bool m_HasLastFireAim = false;
    void DrawAimCrosshair(IDirect3DDevice9* device, float sx, float sy, UINT h) const;
    DWORD m_WeaponActionAnimUntilMs = 0;
    int m_LatchedViewmodelIdleSeq = -1;
    bool m_CrouchToggled = false;
    bool m_HudOverlayReady = false;
    bool m_HudOverlayCreateAttempted = false;
    IDirect3DSurface9* m_PauseHudPrevRt = nullptr;
    IDirect3DSurface9* m_PauseHudPrevDs = nullptr;
    D3DVIEWPORT9 m_PauseHudPrevVp{};
    DWORD m_PauseHudPrevColorWrite = 0xF;
    bool m_PauseHudColorWriteSaved = false;
    bool m_PauseHudRtSaved = false;
    bool m_PauseHudAlphaOverridden = false;
    bool m_HudOverlayHasImage = false;
    bool m_OpenXrHudOverlayPublished = false;
    std::atomic<bool> m_HudPaintedThisFrame{ false };
    bool m_MenuTriggerWasDown = false;
    bool m_MenuCursorValid = false;
    bool m_MenuCursorSmoothValid = false;
    float m_MenuCursorSmoothX = 0.f;
    float m_MenuCursorSmoothY = 0.f;
    int m_MenuCursorX = 0;
    int m_MenuCursorY = 0;
    double m_MenuCursorSmoothMs = 0.0;
    bool m_MenuPanelPoseValid = false;
    uint32_t m_MenuOverlayEpoch = 0;
    L4D2VROpenXrPoseDesc m_MenuPanelPose{};
    Vector m_MenuPanelFwd{};
    Vector m_MenuPanelRight{};
    Vector m_MenuPanelUp{};
    DWORD m_MenuClickMs = 0;
    int m_MenuClickX = 0;
    int m_MenuClickY = 0;
    bool m_WeaponMenuOpen = false;
    bool m_WeaponMenuClickHeld = false;
    bool m_WeaponMenuOpenedThisHold = false;
    bool m_WeaponMenuLatched = false;
    bool m_EmptyHands = false;
    // Pickup stays on the hand that pressed Use until drop. Vanilla is a
    // Use-toggle, so release has to pulse a second press to drop.
    bool m_GrabLatched = false;
    bool m_GrabHandLeft = false;
    bool m_LastGrabHandLeft = true;
    bool m_GrabHoldingPhysics = false;
    bool m_GrabUseEntityHeld = false;
    bool m_UseWasHeld = false;
    double m_UseDropGapUntilMs = 0.0;
    double m_UseDropPulseUntilMs = 0.0;
    double m_UseHeldSinceMs = 0.0;
    double m_UseDropClassifyUntilMs = 0.0;
    double m_UseDropSettleUntilMs = 0.0;
    int m_UseDropRetries = 0;
    // True while inventory has a weapon and empty-hands is not selected.
    // Sampled on the game thread for right-glove visibility.
    bool m_HasHeldWeapon = false;
    DWORD m_WeaponMenuClickStartMs = 0;
    int m_WeaponMenuHover = -1;
    uint32_t m_PrevHeldButtons = 0;
    int m_WeaponMenuCount = 0;
    Vector m_WeaponMenuOrigin{};
    Vector m_WeaponMenuFwd{};
    Vector m_WeaponMenuRight{};
    Vector m_WeaponMenuUp{};
    Vector m_WeaponMenuLatchBody{};
    Vector m_WeaponMenuLatchWorld{};
    Vector m_WeaponMenuLatchDelta{};
    Vector m_WeaponMenuLatchFwd{};
    Vector m_WeaponMenuLatchRight{};
    Vector m_WeaponMenuLatchUp{};
    Vector m_WeaponMenuLatchBillboardFwd{};
    Vector m_WeaponMenuLatchBillboardRight{};
    Vector m_WeaponMenuLatchBillboardUp{};
    float m_WeaponMenuLatchYaw = 0.f;
    float m_WeaponMenuHandX = 0.f;
    float m_WeaponMenuHandY = 0.f;
    int m_WeaponMenuPrevEntity = 0;
    int m_WeaponMenuPrevKind = 0;
    struct WeaponMenuSlot
    {
        int entityIndex = 0;
        int kind = 0;
        int hudSlot = 0;
        int hudPos = 0;
        float planeX = 0.f;
        float planeY = 0.f;
        Vector center{};
        char label[16]{};
        bool equipped = false;
        bool emptyHand = false;
        bool dry = false;
        bool throwable = false;
    };
    WeaponMenuSlot m_WeaponMenuSlots[20]{};
    int m_LastMuzzleFlashParity = -1;
    int m_LastFireClip = -1;
    void* m_LastFireWeapon = nullptr;
    bool m_StereoEyeBlitOk = false;
    int m_StereoEyeBlitRank = 0;
    uint32_t m_LastStereoBlitWidth = 0;
    uint32_t m_LastStereoBlitHeight = 0;
    bool m_LastEyeBlitWasWindowCrop = false;
};
