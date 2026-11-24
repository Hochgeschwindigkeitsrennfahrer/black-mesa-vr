#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "openvr.h"
#include "vector.h"
#include "openxr_bridge_protocol.h"
#include "vr_runtime_backend.h"
#include "vr_button_map.h"
#include <cstdint>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <chrono>
#include <d3d9.h>

struct D3D9_TEXTURE_VR_DESC;
struct ModelRenderInfo_t;
class Game;
class ITexture;
class IMaterial;
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

// Title HUD panels: HudCredits, HudMessage, CHudIntroCredits.
constexpr int kTitleHudCount = 3;

class VR
{
public:
    Game* m_Game = nullptr;

    VrRuntimeBackend m_RuntimeBackend = VrRuntimeBackend::OpenXR;
    VrRuntimeBackend m_RequestedRuntimeBackend = VrRuntimeBackend::OpenXR;
    bool m_OpenXrLoaderAvailable = false;
    bool m_OpenXrHelperBridgeActive = false;
    DWORD m_OpenXrHelperRestartMs = 0;
    int m_OpenXrHelperRestartCount = 0;
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
    // Latest helper pose latched at stereo start. Do not block the game
    // thread on helper consume / a new displayTime (HMD 2026-09-20).
    int64_t m_OpenXrLatchedDisplayTime = 0;
    uint32_t m_OpenXrLatchedPoseGeneration = 0;
    int64_t m_OpenXrPublishedDisplayTime = 0;
    uint32_t m_OpenXrPublishedPoseGeneration = 0;
    uint32_t m_OpenXrAwaitFrames = 0;
    uint32_t m_OpenXrAwaitHandoffs = 0;
    double m_OpenXrAwaitPoseMs = 0.0;
    double m_OpenXrAwaitPoseMaxMs = 0.0;
    uint32_t m_OpenXrHelperSubmittedLast = 0;
    uint32_t m_OpenXrSkipSameVsync = 0;
    uint32_t m_OpenXrSkipStereoNoPose = 0;
    // Rate-log counters: synchronous copy wait, stereo pairs begun.
    uint32_t m_OpenXrCopyWaits = 0;
    double m_OpenXrCopyWaitMs = 0.0;
    double m_OpenXrCopyWaitMaxMs = 0.0;
    uint32_t m_OpenXrCopyThread = 0;
    uint32_t m_OpenXrPairThread = 0;
    uint32_t m_OpenXrStereoPairs = 0;
    // MulticoreMode eye copy per publish-rate window: functor queued on the
    // material thread, queue unavailable, copied there, Present fallback.
    uint32_t m_McCopyQueuedCount = 0;
    uint32_t m_McCopyQueueFailCount = 0;
    uint32_t m_McCopyMatDoneCount = 0;
    uint32_t m_McPresentFallbackCount = 0;
    // Publish every pending slot whose copy the GPU has completed (newest
    // wins; older completed ones are dropped as stale).
    void PollOpenXrDeferredPublish();
    // Wait for the helper's next xrWaitFrame pose (HL2VR WaitGetPoses).
    // Does not wait for slot consume. Returns true if displayTime/gen changed.
    bool AwaitOpenXrCompositorFrame();
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
    // Test aid: when <VR dir>\dump.txt exists, writes the eye pair being
    // published and the HUD/menu overlay surface to <VR dir>\<name>-*.bmp
    // (name = first word of dump.txt) and deletes dump.txt.
    void MaybeDebugDumpPublished(uint32_t slot, const L4D2VROpenXrPoseDesc* pose);
    // Gesture screenshot: the right eye (16:9 crop) into Steam's library.
    void TakeSteamScreenshot(IDirect3DDevice9* dev, uint32_t slot);
    // Debug capture restore (VR/<capture>-state.txt etc., tools/headless/bm-restore):
    // render-thread part (raw OpenXR poses, view state), game-thread part
    // (engine view, settings, the capture save), and the headless restore
    // (fake runtime only: turn offset + recenter point set exactly, player
    // moved until the engine eye matches the captured one).
    void WriteCaptureState(const std::string& name, const L4D2VROpenXrPoseDesc* pose);
    void FlushCaptureEngineState();
    bool HandleRestoreCommand(const char* cmd);
    void TickRestoreView();
    std::mutex m_CaptureMutex;
    std::string m_CaptureEngineName;          // set by the render thread, written by the game thread
    Vector m_SetupVelPrev{};
    DWORD m_SetupVelPrevMs = 0;
    Vector m_SetupVelocity{};
    bool m_RestoreActive = false;
    Vector m_RestoreTarget{};
    Vector m_RestoreSent{};
    bool m_RestoreHaveSent = false;
    int m_RestoreSteps = 0;
    DWORD m_RestoreNextMs = 0;


    // Eye / G-buffer size. L4D2VR uses OpenVR recommended size. Named RTs at
    // that size died on BM, so the blit path matches G-buffer to recommended
    // (hmd_native) or fits HMD aspect in the window if that crash-stickies.
    uint32_t m_RenderWidth = 0;
    uint32_t m_RenderHeight = 0;
    // Eye MSAA sample count (from bmvr::g_Msaa, 0 = off). Only the private eye
    // colour + depth surfaces are multisampled; 1x submit textures carry the
    // Vulkan desc.
    uint32_t m_AntiAliasing = 0;
    bool UseVrMsaa() const
    {
        return m_AntiAliasing == 2 || m_AntiAliasing == 4
            || m_AntiAliasing == 8 || m_AntiAliasing == 16;
    }
    float m_Aspect = 1.f;
    // OffCenterEyes: the union frustum is fov=m_Fov, aspect=m_OffCenterAspect
    // (tanX/tanY over both eyes); each eye renders the sub-rectangle
    // m_OffCenterFrac[eye] = {left, right, bottom, top} (0..1 of the union).
    bool m_OffCenterActive = false;
    float m_OffCenterAspect = 1.f;
    float m_OffCenterFrac[2][4]{};
    // Runtime per-eye tangents {left, right, down, up} (both modes).
    float m_EyeTan[2][4]{};
    bool m_EyeTanValid = false;
    // Per-eye rendered rectangle in tangent space {L, R, D, U}, unscoped
    // (stereoEye 1 = left, 2 = right): the runtime eye with OffCenterEyes,
    // else the symmetric frustum (tan(m_Fov / 2), vertical from rtAspect, 0 =
    // the eye RT's aspect). The left eye's right edge is pushed out by
    // LeftEyeCullPad so its culling also covers the right eye's view.
    bool EyeRenderRect(int stereoEye, float rect[4], float rtAspect = 0.f) const;
    // True when the eye's rectangle is not the symmetric frustum (render it
    // off-centre, submit its explicit FOV).
    bool EyeRenderIsCustom(int stereoEye) const;
    float LeftEyeCullPadTan() const;
    float EyeRtAspect() const;
    // CViewSetup for a custom eye: centred fov/aspect holding the rectangle
    // (scope-aware) and the 0..1 sub-rectangle {left, right, bottom, top}.
    bool EyeOffCenterView(int stereoEye, float& fovDeg, float& aspect, float frac[4]) const;
    // Horizontal fov / aspect the engine renders this eye with.
    float EyeRenderFov(int stereoEye) const;
    bool EyeRenderAspect(int stereoEye, float& aspect) const;
    // Overlay reference-frustum NDC -> this eye's NDC: ndc' = ndc * s + o.
    bool OffCenterNdc(int stereoEye, float& sx, float& ox, float& sy, float& oy) const;
    bool AnyEyeOffCenter() const { return EyeRenderIsCustom(1) || EyeRenderIsCustom(2); }
    // l/r/d/u = tangents of each eye's left/right/down/up half-angles.
    void SetupOffCenter(const float leftEye[4], const float rightEye[4]);
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
        bool glock = false;
        bool crossbow = false;
        bool revolver = false;
        // Long guns / pistols: off-hand can wrap the model. Table is meters
        // from the rear grip along F/R/U. L_Wrist is used only when
        // offhandUseBone is set (MP5 magwell / pistol monitor pose are wrong).
        bool twoHand = false;
        bool offhandUseBone = false;
        bool offhandPistol = false;
        float offhandMetersX = 0.f;
        float offhandMetersY = 0.f;
        float offhandMetersZ = 0.f;
        // Extra grab length along +F from the point, so a pump/handguard is a
        // rail instead of a single rear bone.
        float offhandRailMeters = 0.f;
        // Radius around the rail within which a squeeze grabs (long guns).
        float offhandEnterMeters = 0.05f;
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
        kVmFlagGlock = 1u << 6,
        kVmFlagCrossbow = 1u << 7,
        kVmFlag357 = 1u << 8,
        kVmFlagSnark = 1u << 9,
        kVmFlagSatchel = 1u << 10,
    };
    // Manual MP5 magazine (VrMagReload): eject / hip grab / seat, game thread.
    void UpdateMagReload(uint32_t& buttons);
    void UpdateGrenadePin(uint32_t& buttons);
    // Snark hand throw (VrHandThrow): trigger held = nothing, let go = the
    // game's throw (pulsed), from the hand at the hand's speed (server hooks).
    void UpdateSnarkThrow(uint32_t& buttons);
    // GetTickCount until which a snark throw of the local player is a hand
    // throw (set at the trigger release).
    std::atomic<unsigned> m_SnarkHandThrowUntil{ 0 };
    std::atomic<bool> m_ThrowFromLeft{ false }; // satchel: throw samples from the left hand
    int MagReloadWeaponNow() const;
    // Left controller world pose for the viewmodel draw (render thread).
    bool OffhandWorldPoseForDraw(Vector& pos, QAngle& ang) const;
    bool ViewmodelIsShotgun() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagShotgun) != 0; }
    bool ViewmodelIsCrossbow() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagCrossbow) != 0; }
    bool ViewmodelIs357() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlag357) != 0; }
    bool ViewmodelIsSnark() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagSnark) != 0; }
    bool ViewmodelIsSatchel() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagSatchel) != 0; }
    // Satchel drawn with VrSatchelHands: bag in the left hand, detonator in the right.
    bool SatchelHandsActive() const;
    bool ViewmodelIsGlock() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagGlock) != 0; }
    bool ViewmodelIsMp5() const { return (m_VmClassFlags.load(std::memory_order_relaxed) & kVmFlagMp5) != 0; }
    // Viewmodel sequence is a reload/draw/holster/pump-style action: the
    // gun moves away from the aim, so the laser beam is hidden.
    void NoteViewmodelBeamHidden(bool hide) { m_ViewmodelBeamHidden.store(hide, std::memory_order_relaxed); }
    // Muzzle in the gun frame, from the bones as drawn (viewmodel DME).
    void NoteDrawnMuzzleLocal(const Vector& local);
    // Lowered set (idletolow / lowidle / lowtoidle, gun held down while aimed
    // at a friendly): the clip moves the gun's bones, while the two-hand
    // support glove sits on the controller-derived grip point. The glove
    // follows the drawn weapon root bone's move away from its idle pose
    // (the gun body; the arms are separate bones and on the pistols the left
    // wrist barely moves). State from CalcViewModelView (game thread) for the
    // weapon's own viewmodel header; bone samples from the draw (render thread).
    enum : int { kVmLowerIdle = 0, kVmLowerLowered = 1, kVmLowerOther = 2 };
    void NoteViewmodelLowerState(int state, const void* studioHdr);
    // Weapon root bone origin and unit axes in the gun frame of that draw.
    void NoteDrawnWeaponRootLocal(const void* studioHdr, const Vector& pos, const Vector axes[3]);
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
    vr::HmdMatrix34_t m_HmdTrackingPose{};
    vr::HmdMatrix34_t m_StereoFrameTrackingPose{};
    bool m_StereoFrameTrackingPoseValid = false;
    std::atomic<DWORD> m_WaitedPoseTick{ 0 };
    int m_Hl2vrCallQueueSlot = -1;
    bool m_Hl2vrCallQueueProbed = false;
    // Source slot 146 can return a non-null non-queue on BM. Remember it so
    // ProbeCallQueue does not latch 146 after QueueFunctorInternal SEH-fails.
    int m_McCallQueueFailedSlot = -1;
    bool m_McEyeCopyQueued = false;
    bool m_McEyeCopyDone = false;
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
    // What the helper reads (m_VKHUD): a copy of m_D9HUDSurface made when a
    // paint is complete. The helper copies the shared image at its own time;
    // reading the RT VGUI clears and repaints every frame showed half-painted
    // or empty layers (title text / pause menu flicker, 2026-11-17).
    IDirect3DTexture9* m_D9HUDPublishTexture = nullptr;
    IDirect3DSurface9* m_D9HUDPublishSurface = nullptr;
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
    std::atomic<uint32_t> m_ActualCompositorSubmitCount{ 0 };  // stats line in d3d9_device.cpp; 0 since OpenVR left
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
    vr::VRActionHandle_t m_ActionInventoryGripLeft = vr::k_ulInvalidActionHandle;
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
    uint32_t m_CompositorHandoffSlowCount = 0;
    std::atomic<float> m_WalkForward{ 0.f };
    std::atomic<float> m_WalkSide{ 0.f };
    std::atomic<uint32_t> m_HeldButtons{ 0 };
    std::atomic<float> m_RotationOffsetY{ 0.f };
    std::atomic<uint32_t> m_PendingImpulse{ 0 };
    std::atomic<int> m_PendingInvDelta{ 0 };
    std::atomic<int> m_PendingWeaponSelect{ 0 };
    std::atomic<bool> m_PendingQuickSave{ false };
    std::atomic<bool> m_PendingQuickLoad{ false };
    // Button mapping (OpenXR helper, protocol 18): while active, the mappable
    // OpenXR action slots come from m_MappedDigital instead of the helper's
    // fixed bindings (GetDigitalActionData). Menus / the VR panel use the
    // fixed layout; a legacy slot still held at that switch reads released
    // until it is let go (m_LegacyMask).
    bmvr::ButtonMapper m_ButtonMapper;
    bool m_ButtonMapActive = false;
    bool m_MappedOverride[L4D2VR_OPENXR_ACTION_COUNT] = {};
    L4D2VROpenXrDigitalActionDesc m_MappedDigital[L4D2VR_OPENXR_ACTION_COUNT] = {};
    mutable bool m_LegacyMask[L4D2VR_OPENXR_ACTION_COUNT] = {};
    // VR recoil (g_VrRecoil): current upward kick in degrees, decays; added
    // to the aim and the viewmodel basis.
    std::atomic<float> m_RecoilKickDeg{ 0.f };
    void* m_RecoilWeapon = nullptr;
    int m_RecoilClip = -1;
    DWORD m_RecoilLastMs = 0;
    DWORD m_RecoilAttackMs = 0;
    void UpdateVrRecoil();
    bool m_AmmoGrabReady = false;     // UpdateMagReload: a grip at the ammo zone would take ammo now
    bool m_GestureZoneLeft = false;   // last frame: left hand in one of its gesture zones
    bool m_GestureZoneRight = false;  // last frame: right hand in a shoulder/hip zone (or just left one)
    // Saves keep the player's view angles, and gameplay usercmds carry the
    // controller aim, so a load faced where the gun pointed. Before a quick
    // save or a pause (saves from the pause menu) a few usercmds send the head's
    // angles instead; the action waits until the server has had them.
    static constexpr int kHeadAimCmds = 3;
    static constexpr ULONGLONG kHeadAimWaitMs = 300;
    bool m_HeadAimWanted = false;
    int m_HeadAimSent = 0;
    ULONGLONG m_HeadAimGiveUpMs = 0;
    // Main thread. True when the action may run now; otherwise starts or
    // continues the head-aim hold.
    bool HeadAimReadyForSave();
    bool TakeHeadAimCmd();
    std::atomic<int> m_PendingPauseCmd{ 0 };   // 1 setpause, 2 unpause
    bool m_SettingsPausedGame = false;
    std::atomic<uint32_t> m_PendingWeaponSounds{ 0 };
    std::atomic<int> m_PendingWeaponSoundKind{ 0 };
    std::atomic<int> m_PendingWeaponSoundEntity{ 0 };
    std::atomic<uint32_t> m_PendingFireHaptic{ 0 };
    std::atomic<int> m_PendingGameUi{ 0 };
    std::atomic<int> m_PendingQuit{ 0 };
    bool m_GameUiVisible = false;
    std::atomic<bool> m_AppActivated{ true };
    DWORD m_GameUiActivateMs = 0;
    DWORD m_MenuLastVisibleMs = 0;
    int m_MenuPointerHand = 1;
    // Last time the right hand's ray hit the menu panel (right has priority).
    DWORD m_MenuRightHitMs = 0;
    void* m_EngineVGuiFromPaint = nullptr;
    void* m_VguiPanel = nullptr; // vgui2.dll VGUI_Panel009
    unsigned int m_TitleHudPanels[kTitleHudCount] = {};
    int m_TitleHudSearches = 0;
    // Panels resized to the HUD RT for the title paint, with their own sizes.
    unsigned int m_TitleFitPanels[16] = {};
    int m_TitleFitSizes[16][2] = {};
    int m_TitleFitCount = 0;
    unsigned long long m_TitleHudNextSearchMs = 0;
    bool m_TitleHudActive = false;
    // The HUD RT paint in flight is the title layer (no menu cursor / colour
    // fix). Read on the material thread for the queued end.
    std::atomic<bool> m_HudPaintTitle{ false };
    bool m_PressedTurn = false;
    bool m_StereoEyesDrawnThisFrame = false;
    // 0 = mono, 1 = left, 2 = right. Matches Source StereoEye_t.
    int m_StereoEye = 0;
    bool m_HasStereoBodyOrigin = false;
    Vector m_StereoBodyOrigin{};
    bool m_StereoCopyOffset = false;
    bool m_SeenGameplay = false;
    // Loading a save (or a level change) restores the player's angles in the
    // engine, but the view is head yaw + turn offset, so the saved direction
    // was lost: after a load the player faced wherever the current offset
    // pointed, sometimes the other way. CreateMove (client mode) is entered with
    // the engine's angles, which are what the previous tick wrote unless the
    // engine set them; for a while after a load such a change re-aims the
    // turn offset once. A load is a gap in usercmds (the LevelInit hook is not
    // installed on BM, and reloading the same map does not change its name).
    int m_YawSyncTicks = 0;
    // Yaw of the last usercmd sent in gameplay: the turn sync's reference.
    // Commands outside gameplay (the new game UI's loading/menu phase, menu
    // background maps) pass the engine's angles through and must not move it.
    float m_LastGameplayCmdYaw = 0.f;
    bool m_LastGameplayCmdYawValid = false;
    ULONGLONG m_LastCmdMs = 0;
    bool m_LastCmdYawValid = false;
    // Headless harness only (fake VR runtime): it starts straight into a save
    // (+load on the command line), so there is no earlier command to compare
    // with and the engine yaw of the first command is the restored view.
    // Real sessions load from the menu map and use the jump check instead.
    bool m_FirstCmdYawPending = false;
    float m_FirstCmdYaw = 0.f;
    float m_LastCmdYaw = 0.f;
    void SyncTurnToRestoredYaw(float engineYaw);
    float m_LastCmdPitch = 0.f;
    void NoteCmdAngles(float pitch, float yaw)
    {
        m_LastCmdPitch = pitch;
        m_LastCmdYaw = yaw;
        m_LastCmdYawValid = true;
        if (m_GameplayEligible)
        {
            m_LastGameplayCmdYaw = yaw;
            m_LastGameplayCmdYawValid = true;
        }
    }
    // Scripted camera (FollowScriptedCamera / bmvr_follow_scripted_camera):
    // in normal play the engine's view angles are the ones the mod sent with
    // the last usercmd; a cutscene camera (point_viewcontrol, the drag scene,
    // the tram) replaces them. While it does, the view takes the camera's
    // orientation and the head turns relative to it (head yaw measured from
    // where it pointed when the camera took over).
    bool m_ScriptedCamActive = false;
    // This frame's engine angles differ from the last usercmd. The camera view
    // is only used on such frames: once control returns the engine angles are
    // the usercmd's (already head-driven), and camera x head would apply the
    // head twice (lying on the floor at the end of the drag scene: pitch doubled).
    bool m_ScriptedCamDiffersNow = false;
    int m_ScriptedCamFrames = 0;
    float m_ScriptedCamHmdYaw0 = 0.f;
    QAngle m_ScriptedCamAngles{};
    void UpdateScriptedCamera(const Vector& engineAngles);
    bool ScriptedCameraActive() const;
    // Camera x head orientation for the eyes (pitch, yaw, roll), and its right vector.
    bool ScriptedCameraView(Vector& angles, Vector& right) const;
    // Save thumbnails: Black Mesa grabs whatever render target is bound at the
    // end of the frame render, which in VR is not a picture of the game (black
    // behind the pause menu; a corner of the eye in gameplay). So every save
    // takes the left eye, scaled down once on the Present thread: when the pause
    // starts, or on request for a gameplay save. FillSaveThumbnail cuts it to the
    // shape of the game's grab around straight ahead.
    static constexpr int kSaveThumbCaptureWidth = 1024;
    static constexpr ULONGLONG kSaveThumbWaitMs = 500;
    enum class SaveThumbState { Waiting, Ready, Failed };
    std::mutex m_SaveThumbMutex;
    std::vector<unsigned char> m_SaveThumbBgr;   // m_SaveThumbW x H, BGR, top row first
    int m_SaveThumbW = 0;
    int m_SaveThumbH = 0;
    bool m_SaveThumbReady = false;
    bool m_SaveThumbPending = false;   // a gameplay save waits for its capture
    ULONGLONG m_SaveThumbGiveUpMs = 0;
    void UpdateSaveThumbnailCapture();  // Present thread
    void CaptureSaveThumbnail();
    bool WantsSaveThumbnail() const;
    SaveThumbState PollSaveThumbnail();
    void EndSaveThumbnail();
    // Fills w x h RGB rows, top row first (the game's ReadPixels layout).
    bool FillSaveThumbnail(unsigned char* rgb, int w, int h);
    bool m_GameplayEligible = false;
    int m_GameplayFrames = 0;
    uint32_t m_PresentTick = 0;
    uint32_t m_EligiblePresents = 0;
    uint32_t m_PassThroughMainViews = 0;
    bool m_AppliedVrPerfCvars = false;
    std::string m_CurrentMapName;
    std::string m_ScreenFxMap;
    IMaterial* m_ScreenFxMaterials[4] = {};   // [3] = Xen global fog __gbxog0 (XenGlobalFogOff)
    bool m_ScreenFxNoDraw[4] = {};
    int m_ScreenFxMaterialCount = -1;
    struct EnvmapTintEntry
    {
        float orig[3];
        float written[3];
    };
    std::unordered_map<IMaterial*, EnvmapTintEntry> m_EnvmapTints;
    // Every material the last walks visited, so a count change (new gibs,
    // decals, effects mid-map) only looks at the new ones.
    std::unordered_set<IMaterial*> m_EnvmapKnown;
    std::string m_EnvmapMap;
    int m_EnvmapMaterialCount = -1;
    float m_EnvmapAppliedWorld = 1.f;
    float m_EnvmapAppliedModels = 1.f;
    unsigned long long m_EnvmapNextWalkMs = 0;
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
    // Frame evenness: interval between published frames, in 90 Hz-tick
    // buckets (helper rate), plus how often the bucket changes frame to frame
    // (an uneven 1-2-1-2 pattern judders more than a steady 2-2-2).
    struct PublishEvenness
    {
        double lastMs = 0, sum = 0, sumSq = 0, minMs = 1e9, maxMs = 0;
        uint32_t n = 0, ticks[5] = {}, changes = 0;
        int lastBucket = -1;
    };
    PublishEvenness m_PublishEven;
    void NotePublishEvenness();
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
    void ApplyFrameCap(int nominalHz, float sourceHz, const char* source);
    int m_FrameCapApplied = 0;
    void TickOpenXrHelperWatchdog();
    void LogOpenXrPublishSetupFailure(const char* stage, uint32_t eye, uint32_t slot, unsigned hr);
    bool EnsureOpenXrPublishTextures(IDirect3DDevice9* device, UINT w, UINT h);
    void ReleaseOpenXrPublishTextures();
    Vector GetViewAngle() const;
    void UpdateScopeZoomSmooth();
    void UpdateScopeAimOffset();
    bool UpdateScopeRaise(bool allowed);
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
    // Angles from the engine eye (m_SetupOrigin, where the server's +use
    // trace starts) to the point the use hand's ray hits within reach. The
    // hand's own angles from the eye miss a button the hand points at from
    // hip height by the eye-hand offset.
    QAngle GetUseAimAnglesFromEye() const;
    // True on a ladder (MOVETYPE_LADDER; onLadder) or with the stick
    // deflected within 48 units of a func_useableladder's mount line. moveBits = IN_FORWARD/BACK/
    // MOVELEFT/MOVERIGHT from the stick at 0.2 deflection (head frame):
    // LadderMove climbs only on those bits, which gameplay sets past 0.5, so
    // a gentle walk-in mounted the ladder and then hung there.
    // mountYaw (optional): set to the flat yaw towards the nearest ladder when
    // the player is within the engine's 32-unit auto-mount range and the
    // stick points roughly at the ladder (mount assist, see vr.cpp); NaN
    // otherwise.
    bool LadderMode(int& moveBits, bool& onLadder, float* mountYaw = nullptr) const;
    // Physical ladders (PhysicalLadders): a grip squeezed with the hand on a
    // ladder mounts it (one IN_USE with the view at the ladder) and then the
    // player moves opposite to that hand's height change, so the hand stays
    // on the rung. Called once per CreateMove; false = not involved (stick
    // ladder mode as before). dir: +1 climb up this tick, -1 down, 0 hold.
    struct PhysicalLadderCmd
    {
        bool onLadder = false;
        bool useEdge = false;   // send IN_USE this tick (mount)
        bool mountPush = false; // mounting: walk toward the ladder (yaw) this tick
        bool mountRise = false; // mounting below the ladder's bottom end: swim up
        bool drop = false;      // let go of the ladder (IN_USE on it, view straight down)
        bool topExit = false;   // step off at the top end (IN_USE on it, level head view)
        bool stickTop = false;  // near the top with the stick moved: stick ladder mode this tick
        float yaw = 0.f;        // view yaw towards the ladder while mounting
        int dir = 0;
    };
    bool PhysicalLadderStep(PhysicalLadderCmd& out);
    // RenderView: height to add to the main view while climbing by hand
    // (PhysicalLadderSmooth). LadderMove moves 0 or 200 u/s per tick, so
    // the body moves in bursts; the view instead follows the climb hand
    // every frame, at most a few units ahead of / behind the body.
    float PhysicalLadderViewOffset(float renderZ);
    // RenderView: horizontal view shift towards / away from the ladder so the
    // real hands reach the rungs they hold (arms pulled in = closer to the
    // ladder, stretched = further). LadderBodyFollowsArms; +-8 units (more
    // where the rungs stand further out than 24 units, up to 24).
    Vector PhysicalLadderViewShift(const Vector& body);
    struct LadderViewState
    {
        bool active = false;     // climbing by hand (set by PhysicalLadderStep)
        bool rebase = false;     // new anchor: keep the view continuous
        int hand = -1;
        float anchorHandZ = 0.f; // tracking z of the climb hand at its grab
        float visAnchorZ = 0.f;  // view z belonging to anchorHandZ
        float offset = 0.f;      // last offset sent
        Vector shift{};          // last horizontal shift sent (PhysicalLadderViewShift)
        float limit = 8.f;
        ULONGLONG lastMs = 0;
    } m_LadderView;
    // ProcessInput: grip state per hand (0 left, 1 right) for PhysicalLadderStep.
    void UpdateLadderGrips();
    std::atomic<bool> m_LadderGrip[2]{};
    // Last CreateMove: hand within reach of a ladder. Its grip is a grab
    // there, not the mapped action (flashlight on the right grip).
    std::atomic<bool> m_LadderTouch[2]{};
    // Last CreateMove: on a ladder, climbing by hand or mounting one.
    std::atomic<bool> m_LadderActive{ false };
    // Player within reach of a ladder (PhysicalLadderStep, every tick it
    // runs; ms of the last time): a right-grip flashlight press waits a
    // moment there in case it is a ladder grab.
    std::atomic<unsigned long long> m_LadderNearMs{ 0 };
    // Hand climbing: the rung each hand took hold of (world), from the
    // map's ladder props (vr_ladder_rungs). The glove is drawn there.
    struct LadderPin
    {
        bool valid = false;
        Vector world{};   // where the controller is drawn (rung + LadderHandBack toward the player)
        Vector rung{};    // the point on the rung itself
        Vector axis{};    // along the rung, toward the thumb of an overhand grip (inward)
        bool hasAxis = false;
        int model = -1;   // ladder model of the rung (per-model grip tuning)
        Vector face{};    // player -> rung, flat, unit (grip tuning "out" = against it)
    };
    LadderPin m_LadderPin[2];
    bool TryGetLadderPinDrawPos(bool physicalLeft, const Vector& body, Vector& trackingPos) const;
    // LadderHandWrap: the glove holding a rung is turned so its fist closes
    // round the rung (vr_hands.cpp). Point on the rung + axis (thumb side).
    // curl: the finger curl for that ladder model (DEBUG ladder grip tuning).
    bool LadderRungGrip(bool physicalLeft, Vector& point, Vector& axis, float* curl = nullptr) const;
    // DEBUG "LADDER GRIP TUNING" rows <-> VR/ladder_grip.txt for the ladder
    // model last held (game thread).
    void SyncLadderGripRows();
    bool UseFollowsLeftHand() const { return m_GrabLatched && m_GrabHandLeft; }
    bool UseGrabActive() const { return m_GrabLatched; }
    // Weapon fire, shoot origin, and flashlight must not follow a leftover
    // left-hand Use latch. Only a live physics hold redirects those.
    bool UseGrabRedirectsFire() const { return m_GrabLatched && m_GrabUseEntityHeld; }
    // Mounted gun (func_50cal / func_tow): the player's use entity while
    // standing at one. Either trigger holds the handles (letting go of both
    // gets off), secondary fire shoots, and the barrel swings around its
    // mount opposite to the hands (MountedGunAim).
    bool OnMountedGun() const { return m_OnGun; }
    void UpdateMountedGun();
    // Usercmd angles that turn the gun's barrel where the hands push it:
    // hands right = barrel left, hands up = barrel down, relative to where
    // the barrel pointed when the hands took hold. The game aims the barrel at
    // what the eye sees along these angles, so they point from the engine eye
    // at the point the wanted barrel line hits. False when not on a gun.
    bool MountedGunAim(QAngle& out);
    // The game's +use trace (eye along the usercmd angles) runs on the press
    // tick, so the angles follow the pressing hand for the first 0.3 s of a
    // press; otherwise the left trigger picks up what the gun points at and
    // holds it in the left hand. For the whole hold, since chargers re-trace
    // the use entity every tick (the 0.3 s limit dropped L2 off a charger),
    // except while firing: Use held on train controls turned the gun's shots
    // to the left hand.
    bool UseAimFollowsGrabHand() const
    {
        return UseGrabRedirectsFire()
            || (m_GrabLatched && m_UseWasHeld
                && (GetTickCount64() - m_UsePressTick < 300 || !m_UseFireHeld));
    }
    // The server grab controller sets its target every tick while the player
    // carries something (Hooks::dGrabSetTarget). The client's m_hUseEntity
    // read never sees BM pickups, so this is what tells a carried prop (drop
    // it with a second Use on release) from train controls, buttons and
    // chargers (leave them alone).
    void NoteGrabControllerActive() { m_GrabControllerSeenTick.store(GetTickCount64(), std::memory_order_release); }
    bool GrabControllerRecentlyActive() const
    {
        const uint64_t t = m_GrabControllerSeenTick.load(std::memory_order_acquire);
        return t != 0 && GetTickCount64() - t < 250;
    }
    // Still carrying after time t. The 250 ms window above looks back past a
    // press that already dropped the prop, so release decisions use this.
    bool GrabControllerActiveAfter(uint64_t t) const
    {
        return m_GrabControllerSeenTick.load(std::memory_order_acquire) > t;
    }
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
    // Eye MSAA: post chain of the eye this thread is drawing runs on its 1x resolve.
    void RetireMsaaEyeForPost();
    // "MSAA skips particles": the main view's translucent pass is done and held-back particles
    // follow (game thread); the eye being drawn switches to its 1x resolve at that point of the draw stream.
    void NoteTranslucentPassStart();
    void RetireMsaaEyeIfBound();
    // RefractOncePerEye: main view translucents done (game thread); later refract copies run again.
    void NoteWorldTranslucentsDone();
    void MsaaEyesTo1x(IDirect3DDevice9* device);
    // Eye surface the calling thread is drawing (multicore TLS or the stereo blit).
    IDirect3DSurface9* ThreadEyeSurface() const;
    IDirect3DSurface9* m_RefractEyeDone = nullptr;   // RefractOncePerEye: eye already copied
    int m_RefractEyeCopies = 0;                      // copies run for m_RefractEyeDone (reset with it)
    uint32_t m_RefractCopiesSkipped = 0;
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
    void LogFullFrameSizeIfReady();
    bool IsGameplayEligible() const { return m_GameplayEligible; }
    bool HasEngineMap() const { return !m_CurrentMapName.empty(); }
    bool ShouldCompositorSubmit() const;
    bool StereoEyeBlitActive() const { return m_StereoEyeBlitActive; }
    // GMod: queued material system writes band M of a 2W x 3H ring; helper
    // copies band (M-2). False keeps the verified single-core 1x-eye path.
    bool McPipelineEnabled() const;
    // One log line when the gameplay render path changes (best / fallback + why).
    void LogRenderPathIfChanged();
    bool McEyesSmallerThanWindow() const;
    // One HWND-sized RenderView + squash-blit both eyes. Not fused.
    // True when FullFrame/G-buffer stayed window-sized and either gbmatch
    // lock is on or the recommended eyes fit inside the HWND. WMR grows
    // world RTs (worldMatch=1) so this stays false and double RV still runs.
    bool ShouldHwndBlitStereo() const;
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
    // MulticoreMode: what the hand/laser overlay of one stereo frame needs, taken on the game thread when the
    // frame's composite is queued, so both eyes are drawn from that frame's state. Reading live members on the
    // render thread raced the next frame's updates: one eye without hand or laser, or the laser from the wrong
    // place for a frame (2026-11-13, heavy scenes).
    struct HandDrawSnap
    {
        bool valid = false;
        bool leftOk = false, rightOk = false;
        QAngle leftAng{}, rightAng{};
        Vector leftPos{}, rightPos{}, body{};
        Vector fwd{}, right{}, up{};
        Vector eyeOrig[3]{};
        Vector viewAngles{};
        bool havePoints = false;
        Vector beamA{}, beamB{}, dot{};
        bool aimBeamValid = false, aimBeamHit = false, aimCrosshairValid = false;
        int aimStage = 0;
        Vector aimCrosshairWorld{};
        // Gun's own right wrist as drawn in this frame (cuff cap), from
        // MagReload::g_GunWrist when the snapshot was taken.
        bool gunWristOk = false;
        float gunWrist[12]{};
        // Turn offset of this frame: world-latched menus (weapon menu, VR
        // panel) rebuild their pose from body + turn at draw time.
        float rotationOffsetY = 0.f;
        // HUD numbers (health, armor, clip, reserve, secondary, air) read on
        // the game thread with the frame: the render thread reading them live
        // caught the next frame's network update before prediction re-applied
        // a reload, so the reserve flicked back to its old value.
        bool hudOk = false;
        int hud[6] = { -1, -1, -1, -1, -1, -1 };
    };
    void CaptureHandDrawSnap(HandDrawSnap& s);
    // HUD values for drawing: the drawing frame's snapshot while one is drawn
    // (MC), else read live.
    bool HudValuesForDraw(int& health, int& armor, int& clip, int& reserve, int& secondary, int& air) const;
    // Body origin + turn offset for drawing world-latched menus: the drawing
    // frame's snapshot while one is drawn (MC), else the live values.
    void MenuDrawBody(Vector& body, float& rotationOffsetY) const;
    // Right wrist of the gun's own hands for the cuff cap: the drawing
    // frame's snapshot while one is drawn (MC), else the latest published
    // (within 100 ms). False = no cap.
    bool GunCapWristForDraw(float out[12]) const;
    void McCompositeEyesToRing(uint32_t band, const L4D2VROpenXrPoseDesc* pose, const HandDrawSnap* hands = nullptr);
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
    void UpdateButtonMap();
    void ApplyMenuCursor();
    // Game thread only. World vgui_screen (bm_c2a5i mortar, etc.): hover is
    // C_VGuiScreen::ClientThink from EyeAngles, but clicks need VGUI
    // InternalMousePressed(MOUSE_LEFT=0). Trigger is otherwise only IN_ATTACK.
    void ApplyWorldVguiTriggerClick(CUserCmd* cmd);
    bool WorldVguiSuppressAttack() const { return m_WorldVguiSuppressAttack; }
    // VR pointer in screen coordinates while it hit a menu in the last
    // 250 ms (Hooks::dGetCursorPos).
    bool TryGetMenuCursorScreen(POINT& out) const;
    // The pointer hit the menu panel in the last 250 ms.
    bool MenuPointerLive() const;
    // Menu / settings-panel trigger click: full pull to click, half release
    // to re-arm (a resting finger reads 0.6-0.7 on the PS VR2 Sense).
    static constexpr float kMenuTriggerPress = 0.9f;
    static constexpr float kMenuTriggerRearm = 0.5f;
    bool MenuTriggerClickHeld(bool& latch) const;
    void DrawMenuCursorOnSurface(IDirect3DDevice9* device, IDirect3DSurface9* surf);
    void ApplyMenuNavigation();
    void QueueEscapeKey();
    void QueueVirtualKey(int vk);
    void QueueGameUiToggle(bool currentlyPaused);
    void FlushPendingGameUi();
    void SyncLadderUseOnly();
    void FlushPanelEngineCommands();
    void FlushPendingQuit();
    void NoteEngineVGui(void* engineVgui);
    // Intro logo (env_credits -> HudCredits) and env_message / game_text
    // (HudMessage): client HUD panels the eyes never get (DRAWHUD is stripped
    // there). While one is visible in gameplay it is painted alone onto the
    // pause HUD RT and shown on the HUD layer.
    void UpdateTitleHud();
    bool WantTitleHudOverlay() const;
    bool WantHudLayer() const { return WantPauseWorldOverlay() || WantTitleHudOverlay(); }
    // Hide every visible panel that is not a visible title panel or one of its
    // ancestors; returns how many were hidden into `hidden`.
    bool TitleHudPanelShowing(int i) const;
    int IsolateTitleHudPanels(unsigned int* hidden, int cap);
    void RestoreTitleHudPanels(const unsigned int* hidden, int count);
    void UpdateScreenEffectNoDraw();
    void UpdateEnvmapScale();
    void SetHudPaintTitle(bool title) { m_HudPaintTitle.store(title, std::memory_order_release); }
    bool GameUiVisible() const { return m_GameUiVisible; }
    // WM_ACTIVATEAPP. Default true so the first frames publish before any
    // activate message. Alt-tab sets false.
    void NoteAppActivated(bool activated);
    // Game thread, once per RenderView. Logs foreground changes of the game
    // window and releases +attack/+attack2 after it comes back: the click
    // that refocuses the window could latch the engine's mouse1 bind
    // (fire held until restart, 2026-11-17).
    void TickWindowFocus();
    bool AppActivated() const { return m_AppActivated.load(std::memory_order_relaxed); }
    // Called from the window proc before the engine's own handler, and once
    // per RenderView. No-op unless a focus transition left sound stuck.
    void NoteAudioFocusTransition(bool activated);
    void TickAudioFocusRecover();
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
    // GameUI went away (engine hide or our gameui_hide): AutoRecenterOnResume.
    void NoteGameUiDismissed();
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
    // Laser-style click at window coordinates (new UI Back, test clicks).
    void StartSyntheticMenuClick(int x, int y);
    void TickSyntheticMenuClick();
    int m_SynthClickX = -1;
    int m_SynthClickY = -1;
    DWORD m_SynthClickAtMs = 0;
    // New game UI pause: no VGUI extra paint (the QML layer is the menu).
    void SetPauseHudSkipVgui(bool skip) { m_PauseHudSkipVgui.store(skip, std::memory_order_release); }
    std::atomic<bool> m_PauseHudSkipVgui{ false };
    IDirect3DTexture9* NewUiLayerTexture();
    bool DrawNewUiLayerTo(IDirect3DDevice9* device, IDirect3DSurface9* dst);
    bool IntersectMenuPanelUv(float& u, float& v);
    // Paint complete: copy the HUD RT to the publish surface, then flag it.
    void NoteHudPainted();
    // Pending painted HUD frame for the layer about to publish (kind 0 = pause
    // menu, 1 = title texts). A paint of the other kind is dropped, and a
    // switch of kind drops the last image (2026-11-17: the map-start pause
    // screen's paint was published as the first frame of the credits layer).
    bool TakeHudPaintFor(int kind);
    bool HudPaintedThisFrame() const { return m_HudPaintedThisFrame.load(std::memory_order_acquire); }
    void UpdateCrowbarMelee();
    // Hand throw: sampled every input frame; read by the server grenade hooks.
    void SampleThrowHand();
    void RecordThrowPose(const Vector& trackingPos, int64_t poseTimeNs);
    bool TryGetThrowVelocity(Vector& out);
    bool TryGetThrowOrigin(Vector& out);
    void LogLastThrow() const;
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
    // Also true while only the laser beam is to be drawn.
    bool AimCrosshairVisible() const { return m_AimCrosshairValid || m_AimBeamValid; }
    bool TryGetAimCrosshairWorld(Vector& out) const;
    // Controller position + aim basis right now (render thread safe).
    bool AimFrameNow(Vector& pivot, Vector& f, Vector& r, Vector& u) const;
    // Beam ends and marker from the current pose and the last trace.
    bool AimOverlayPointsNow(Vector& beamStart, Vector& beamEnd, Vector& dot) const;
    bool GunFrameNow(Vector& origin, Vector& f, Vector& r, Vector& u) const;
    // AimDebug=true (config.txt): compare what the server fires with the
    // reticle ray. Called from the shoot-origin/-angle hooks on the game thread.
    void AimDebugShootOrigin(bool server, void* player, bool rewrote, const Vector& before, const Vector& after);
    void AimDebugShootAngles(bool server, void* player, bool rewrote, const QAngle& before, const QAngle& after);
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
    // Half the render IPD in world units (eye = head centre +- this along
    // right); 0 while ScopeMonoZoom holds both eyes at the head centre.
    float RenderHalfIpd() const;
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
    bool RpgLockRingVisible() const { return m_RpgLockRing && m_RpgLaserPointValid; }
    bool WeaponMenuOpen() const { return m_WeaponMenuOpen; }
    bool WeaponMenuClickHeld() const { return m_WeaponMenuClickHeld; }
    bool WeaponMenuStickHeld() const;
    bool EmptyHands() const { return m_EmptyHands; }
    void UpdateWeaponMenu(bool stickClickHeld, float deltaMs);
    // In-VR settings panel (vr_settings_panel.cpp), opened from the weapon
    // menu's gear slot. Render thread only.
    bool SettingsPanelOpen() const { return m_SettingsOpen; }
    void OpenSettingsPanel();
    void CloseSettingsPanel(const char* why);
    // Short head-locked message (quick save); detail = optional smaller second line.
    void ShowToast(const char* text, DWORD ms = kToastMs, const char* detail = nullptr);
    // Manual-reload hint (small toast), each id at most twice per game start;
    // VrReloadHints off = never.
    void ShowReloadHint(int id, const char* text);
    bool m_ToastSmall = false;
    bool ToastVisible() const;
    void DrawToast(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
        const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up);
    // Window-blit fallback: the hand renderer (which draws toasts) does not run
    // there, so draw a visible toast straight onto an eye surface.
    void DrawToastOnEyeSurface(IDirect3DSurface9* eyeSurf, int stereoEye);
    static constexpr DWORD kToastMs = 2500;
    char m_ToastText[32]{};
    char m_ToastDetail[128]{};
    DWORD m_ToastUntilMs = 0;
    int m_MissingOffsetsWarned = 0;   // Game::missingOffsetCount() at the last "out of date" toast
    bool m_VrConfigsExecuted = false; // bmvr.cfg / bmvr_local.cfg run once per game start
    void ExecVrConfigsOnce();
    void UpdateSettingsPanel();
    void DrawSettingsPanel(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
        const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up);
    void SettingsPanelFrame(Vector& origin, Vector& fwd, Vector& right, Vector& up) const;
    static IDirect3DTexture9* SettingsGearTexture(IDirect3DDevice9* device);
    bool m_SettingsOpen = false;
    // First-run welcome: once per map until the player opened the VR panel
    // from it, pause the game and show "hold <button> to open the VR menu"
    // until they do (WelcomeStep, vr_settings_panel.cpp).
    void WelcomeStep(bool paused);
    bool m_WelcomeActive = false;
    bool m_WelcomePaused = false;      // our setpause is on
    bool m_WelcomeTriedThisMap = false;
    DWORD m_WelcomeGameplaySinceMs = 0;
    char m_WelcomeText[128]{};
    // Welcome toast latched in the room like the VR panel (DrawToast).
    bool m_WelcomeLatched = false;
    float m_WelcomeLatchYaw = 0.f;
    Vector m_WelcomeLatchDelta{};
    Vector m_WelcomeLatchFwd{};
    Vector m_WelcomeLatchRight{};
    // Set by the game thread, cleared by the render thread before it paints:
    // a change during a paint stays set and is painted next frame.
    std::atomic<bool> m_SettingsDirty{ false };
    bool m_SettingsChanged = false;
    bool m_SettingsClickHeld = false;
    bool m_SettingsBackHeld = false;
    bool m_SettingsHitValid = false;
    int m_SettingsHover = -1;
    int m_SettingsHoverZone = -1;
    float m_SettingsHitX = 0.f;
    float m_SettingsHitY = 0.f;
    float m_SettingsLatchYaw = 0.f;
    Vector m_SettingsLatchDelta{};
    Vector m_SettingsLatchFwd{};
    Vector m_SettingsLatchRight{};
    void ApplyWeaponMenuWorldPose();
    void DrawWeaponMenu(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
        const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up);
    bool UpdateWeaponFireHaptics();
    void AfterCreateMoveFireHaptics();
    bool TryGetVrMuzzleWorld(Vector& origin) const;
    bool TryGetRawMuzzleWorld(Vector& origin) const;
    void UpdateMuzzleLocal(const std::string& weaponKey);
    bool TryGetVrShootOrigin(Vector& origin) const;
    bool TryGetScopeShootOrigin(Vector& origin) const;
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
    // Drawn v_crowbar bar (hand end projected on the bar, far end), in the
    // right controller's forward/right/up, noted by the viewmodel draw.
    void NoteDrawnCrowbarBar(const Vector& startLocal, const Vector& tipLocal);
    // Visible crowbar strike point and blade axis while a VR swing is live, so
    // the engine's melee trace runs along the bar the player can see.
    bool TryGetMeleeAim(Vector& origin, QAngle& angles) const;
    // True while VR should keep fire/reload/equip sequences running.
    bool WantsWeaponActionAnim() const;
    void GetRightGlovePalmOffsetMeters(Vector& meters) const;
    bool WantsRightGloveVisible() const;
    bool WantsRightGloveWeaponGripCurl() const;
    bool WantsOffhandGloveWeaponGripCurl() const;
    // Support hand on the gun: tuned offset along the gun's own axes.
    Vector GunGripHandOffset(bool rightHand, const QAngle& gunAngles) const;
    // True while a G-Man is in front of the player (see UpdateGmanGesture).
    // Written on the game thread, read by the glove draw.
    bool GmanGestureActive() const { return m_GmanGestureActive.load(std::memory_order_relaxed); }
    bool TwoHandGripActive() const { return m_TwoHandActive; }
    // VrStockHandPose: the drawn gun's stock left-hand finger joints in idle,
    // relative to L_Wrist (thumb..pinky x 3 segments, 3x4 rows, game units).
    // Viewmodel draw -> glove draw. Get is false for another gun's pose.
    void NoteStockOffhandFingers(const void* studioHdr, const float rel[15][12]);
    bool GetStockOffhandFingers(float rel[15][12]) const;
    bool StockOffhandFingersIdleSettled() const;
    // Live L_Wrist (or L_Hand) world origin from DrawModelExecute bone matrices.
    void NoteViewmodelOffhandWorld(const Vector& worldOrigin, bool sideHandle = false);
    bool TryGetTwoHandOffhandDrawPose(bool physicalLeft, Vector& trackingPos, QAngle& trackingAng) const;
    bool HudOverlayReady() const { return m_HudOverlayReady; }
    void EnsureHudOverlay();
    void ClearHudSurface(bool opaque);
    void TickMatQueueFromRenderView();
    // HL2VR hlvr_msaa from VR/config.txt. Scene targets pick it up on the
    // next map load. Advanced Video is not hooked.
    void UpdateHl2vrMsaa();
    // IN_JUMP is masked while m_LadderJumpLatch is set: a hand mount that
    // caught with jump (stick up = swim up) held does not jump straight off
    // the ladder; cleared once jump is let go (hooks.cpp CreateMove).
    uint32_t HeldButtons() const
    {
        const uint32_t b = m_HeldButtons.load(std::memory_order_acquire);
        constexpr uint32_t kInJump = 1u << 1;   // IN_JUMP (in_buttons.h)
        return m_LadderJumpLatch.load(std::memory_order_relaxed) ? (b & ~kInJump) : b;
    }
    uint32_t RawHeldButtons() const { return m_HeldButtons.load(std::memory_order_acquire); }
    std::atomic<bool> m_LadderJumpLatch{ false };
    // A hand mount is running: HL2's walk-on auto-mount is allowed meanwhile
    // (SyncLadderUseOnly), so the mount needs only one use press.
    std::atomic<bool> m_LadderMountActive{ false };
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
    // Per-eye copies of reflection-sized RTs (SkyWaterPerEye), called by the d3d9 device in playback order.
    void EyeRtNoteWrite(IDirect3DDevice9* device, IDirect3DSurface9* target);
    IDirect3DBaseTexture9* EyeRtSubstitute(IDirect3DBaseTexture9* texture);
    IDirect3DBaseTexture9* SceneCubemap() const { return m_SceneCubemap; }
    bool GetFingerCurls(vr::VRActionHandle_t skeletonAction, float outCurls[5]) const;

private:
    friend class McPauseHudFunctor;
    ICallQueue* ProbeCallQueue();
    Vector GetViewOriginUnshifted(const Vector& setupOrigin) const;
    bool GetDigitalActionData(vr::VRActionHandle_t handle, vr::InputDigitalActionData_t& out) const;
    bool GetAnalogActionData(vr::VRActionHandle_t handle, vr::InputAnalogActionData_t& out) const;
    bool PressedDigitalAction(vr::VRActionHandle_t handle, bool onChanged = false) const;
    void ApplyTurnStick(float stickX, float deltaMs);
    static bool IsGameplayMapName(const char* map);
    void PollMapFromEngine();
    // One explicit phase for what the headset should show (docs: steam repo
    // docs/plans/bmvr-loading-mode.md). Step 1: computed + logged only.
    enum class VrPhase : uint8_t { Unknown, Menu2D, Loading, Gameplay, Pause };
    static const char* VrPhaseName(VrPhase p);
    bool EngineDrawingLoadingImage();
    void UpdateVrPhase(bool inGame);
    VrPhase m_VrPhase = VrPhase::Unknown;
    unsigned m_LoadEpoch = 0;   // +1 each time the Loading phase starts (map change, save load, death reload)
    DWORD m_VrPhaseSinceMs = 0;
    bool m_PauseFromLoading = false;   // this pause phase came straight from loading (load end)
    // In-game load without the game's loading dialog (trigger changelevel,
    // reload after death): a LOADING card painted onto the held eye pair,
    // as flat Black Mesa shows its small LOADING box. Painted on the eyes
    // published during the load's forced Presents; cleared when it ends.
    bool m_LoadingCardShown = false;
    bool m_LoadingCardForcePublish = false;
    // engine scr_drawloading byte, from IVEngineClient slot 29
    // (IsDrawingLoadingImage = mov al,[abs]; ret). nullptr = not found yet,
    // m_LoadingFlagProbed stops re-probing after a failed check.
    const volatile uint8_t* m_LoadingFlagAddr = nullptr;
    bool m_LoadingFlagProbed = false;
    bool m_PhaseDumpBackbuffer = false;
    DWORD m_PhaseDumpAtMs = 0;
    void TickPhaseDump();
    DWORD m_PoseDiagUntilMs = 0;
    void TickPoseDiag();
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
    // DEBUG "Capture next pause": from the Pause press, the next published
    // eye pairs and menu-layer images go to VR/pausecap-*.bmp.
    std::atomic<int> m_PauseCapEyes{ 0 };
    std::atomic<int> m_PauseCapHud{ 0 };
    DWORD m_PauseCapStartMs = 0;
    IDirect3DSurface9* SubmitSurfaceForEye(IDirect3DSurface9* eye) const;
    void NoteMsaaEyeScene(IDirect3DSurface9* dst, bool copied);
    void ResolveMsaaEyesToSubmit(IDirect3DDevice9* device);
    void ApplyVulkanYFlip(vr::VRTextureBounds_t& bounds);
    void UpdateControllerTracking(const vr::TrackedDevicePose_t& hmdPose);
    void UpdateAutoMatQueueMode();
    void ApplyVrQualityOfLifeCvars();
    void PulseAimHaptic(unsigned short durationUs = 2500);
    void PulseHandHaptic(vr::ETrackedControllerRole hand, unsigned short durationUs, float amplitude = 0.6f);
    // Real vibration time (OpenXR); OpenVR maps it to its 1-3999 us click.
    void PulseHandHapticSeconds(vr::ETrackedControllerRole hand, float seconds, float amplitude);
    // Last-shot / empty-trigger buzz patterns (LastShotHaptic, DryFireHaptic); the second
    // pulse of a double is queued and sent by PumpHapticFollowUp (ProcessInput thread).
    void StartLastShotHaptic();
    void StartDryFireHaptic();
    void PumpHapticFollowUp();
    // Reload feel (user, 2026-11-18: "doable by touch"): distinct patterns,
    // felt in both hands where the gun takes something in. The second tick
    // of a pattern is queued and sent by PumpHapticQueue (game thread).
    enum class ReloadFeel { Seat, ShellIn, Chamber, PinTaken, PinPulled, Grab };
    void ReloadHaptic(ReloadFeel feel);
    void PumpHapticQueue();
    void UpdateViewmodelNumpadAdjust(bool paused);
    void SyncHeldWeaponAngleRows();
    void ResolveWeaponViewmodelPose(float& ox, float& oy, float& oz, float& ax, float& ay, float& az) const;
    void ApplyViewmodelNumpadExtras(float& ox, float& oy, float& oz, float& ax, float& ay, float& az) const;
    void DrawHandHud(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
        bool leftOk, const Vector& leftOrigin, const QAngle& leftAng,
        bool rightOk, const Vector& rightOrigin, const QAngle& rightAng,
        const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up);
    // Red eye-edge flash when health/armor drop (the flat HUD's damage
    // indicator is never drawn in the eyes). Sampled once per frame on eye 1.
    void UpdateDamageFlash(int stereoEye);
    void DrawDamageFlash(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h);
    int m_DmgLastHealth = -1;
    int m_DmgLastArmor = -1;
    float m_DmgFlashLevel = 0.f;
    double m_DmgFlashStartMs = 0.0;
    bool m_DmgMapLoaded = false;        // LevelInit since the last gameplay sample: longer settle
    double m_DmgLoadSettleUntilMs = 0.0; // after a map load; kept over the map-start pause
    double m_DmgSettleUntilMs = 0.0;   // drops before this only move the baseline (just back in gameplay)
    void RefreshActiveWeaponModel();
    void ToggleEmptyHands();
    void Recenter(const char* why);
    // Head position only (turn direction kept): AutoRecenterOnResume.
    void RecenterPosition(const char* why);
    void RefreshHeldWeaponState();
    void ApplyViewmodelBasisOffsets();
    void ApplyTwoHandWeaponAim();
    float LeftSupportGripValue() const;
    bool UpdateShoulderGestures(bool paused);
    // Body frame for the grab zones (shoulders, hips, ammo pouch): a body yaw
    // that only follows the head on sustained or large turns, and a point
    // where the eyes would be with the head straight over the neck. With
    // g_GestureBodyFrame off: the headset itself (the old head-locked zones).
    // Boxes, per hand: HipCrossLeft = right hand above the left hip, HipAmmo =
    // left hand at the left hip, HipCrossRight = left hand above the right hip.
    enum class GestureZone { ShoulderLeft, ShoulderRight, HipRight, HipCrossLeft, HipAmmo, HipCrossRight, AmmoWideLeft, AmmoWideRight };
    void UpdateBodyFrame();
    bool HandInGestureZone(const Vector& hand, GestureZone zone, float growM = 0.f) const;
    // Hand in that slot's zone (bmvr::GestureZoneId); the left hand's forehead
    // zone is head-framed. No setting / tracking checks.
    bool LeftHandInSlotZone(int zone, float growM = 0.f) const;
    bool RightHandInSlotZone(int zone, float growM = 0.f) const;
    // Left hand in a zone whose left-hand slot is Ammo (manual-reload pouch).
    bool LeftHandInAmmoZone() const;
    void ZoneCoords(const Vector& hand, float& along, float& lateral, float& height) const;
    bool m_BodyFrameValid = false;
    float m_BodyYawDeg = 0.f;          // tracking space, degrees (atan2 of Source y, x)
    float m_BodyHeadYawDeg = 0.f;      // head yaw from the looking-down-safe estimate
    Vector m_ZoneOrigin{};             // tracking space, Source units
    Vector m_ZoneFwd{ 1.f, 0.f, 0.f };
    Vector m_ZoneRight{ 0.f, -1.f, 0.f };
    bool ResolveOffhandGripLocal(float& gx, float& gy, float& gz) const;
    bool m_TwoHandActive = false;
    std::atomic<bool> m_SupportNearGun{ false };   // support hand at the gun (ApplyTwoHandWeaponAim)
    bool m_TwoHandGripWasDown = false;   // TwoHandGripToggle press edge
    bool m_OffhandGripBoneValid = false;
    float m_OffhandGripGx = 0.f;
    float m_OffhandGripGy = 0.f;
    float m_OffhandGripGz = 0.f;
    float m_TwoHandGrabGx = 0.f;
    float m_TwoHandGrabGy = 0.f;
    float m_TwoHandGrabGz = 0.f;
    Vector m_TwoHandDrawPos{};
    QAngle m_TwoHandDrawAng{};
    bool m_TwoHandDrawValid = false;
    bool m_TwoHandDrawPhysicalLeft = true;
    Vector m_TwoHandSmoothF{};
    Vector m_TwoHandSmoothU{};
    bool m_TwoHandSmoothValid = false;
    // Lowered-gun glove follow (NoteViewmodelLowerState / NoteDrawnWeaponRootLocal).
    std::atomic<int> m_VmLowerState{ kVmLowerOther };
    std::atomic<const void*> m_VmLowerHdr{ nullptr };
    std::atomic<ULONGLONG> m_VmLowerIdleSinceMs{ 0 };
    mutable std::mutex m_StockFingersMutex;
    float m_StockFingers[15][12]{};
    const void* m_StockFingersHdr = nullptr;
    Vector m_GunRootRefPos{};
    Vector m_GunRootRefAx[3]{};
    Vector m_GunRootLivePos{};
    Vector m_GunRootLiveAx[3]{};
    const void* m_GunRootSampleHdr = nullptr;
    bool m_GunRootRefValid = false;
    bool m_GunRootLiveValid = false;
    bool m_TwoHandLoweredLogged = false;
    float m_TwoHandLoweredMaxMove = 0.f;
    float m_TwoHandLoweredMaxTurn = 0.f;
    // Moves the support glove's gun-frame grip point with the gun body's
    // lowered-vs-idle move; its angles stay. Caller holds m_ControllerMutex.
    void ApplyLoweredOffhand(Vector& gripLocal);
    int m_AutoMatQueueModeLastRequested = -999;
    std::chrono::steady_clock::time_point m_AutoMatQueueModeLastCmdTime{};
    bool m_VrCvarsApplied = false;
    bool m_Hl2vrMsaaCvarReady = false;
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
    std::mutex m_OpenXrPublishMutex;
    IDirect3DSurface9* m_StereoEyeBlitDest = nullptr;
    bool m_StereoEyeBlitActive = false;
    bool m_EyeMsaaAllocFailed = false;
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
    Vector m_DrawnCrowbarStartLocal{};
    Vector m_DrawnCrowbarTipLocal{};
    DWORD m_DrawnCrowbarMs = 0;
    QAngle m_MeleeBladeAngles{};
    bool m_MeleeBladeAnglesValid = false;
    Vector m_CrowbarPrevTipLocal{};
    // Hand throw (vr_crowbar.cpp): one sample per new controller pose of the
    // weapon hand's grip, in tracking space (Source units, before the
    // stick-turn yaw), so walking and turning add nothing. poseT is the
    // runtime's pose time (velocity is differenced on it, not on when the
    // game happened to read the pose); t is the arrival time on the
    // steady clock, which the trigger release is compared against.
    struct ThrowSample { double t; double poseT; Vector local; };
    // 90-120 poses/s: 512 is over 4 s.
    static constexpr int kThrowSamples = 512;
    double m_ThrowReleaseT = 0.0;               // trigger let go (IN_ATTACK 1 -> 0)
    // Snark: the real trigger release, set by UpdateSnarkThrow; the pulsed
    // IN_ATTACK that follows must not move it (until this steady-clock time).
    double m_ThrowReleaseLockUntil = 0.0;
    double m_ThrowPressT = 0.0;                 // pin pulled (IN_ATTACK 0 -> 1)
    bool m_ThrowAttackHeld = false;
    ThrowSample m_ThrowSamples[kThrowSamples]{};
    int m_ThrowSampleNext = 0;
    int64_t m_ThrowLastPoseNs = 0;
    Vector m_ThrowLastLocal{};
    Vector m_ThrowHandWorld{};
    Vector m_ThrowBodyWorld{};
    Vector m_ThrowReleaseOffset{};               // hand - body at the trigger release
    bool m_ThrowHandValid = false;
    DWORD m_CrowbarLastMotionCheckMs = 0;
    DWORD m_CrowbarNextAttackMs = 0;
    DWORD m_CrowbarNextSwingSoundMs = 0;
    void* m_PhysicalCrowbarModel = nullptr;
    int m_PhysicalCrowbarLoadTries = 0;
    Vector m_AimCrosshairWorld{};
    bool m_AimCrosshairValid = false;
    bool m_AimBeamValid = false;
    int m_AimUpdateStage = 0;   // UpdateAimCrosshair: last stage reached (laser dropout log)
    bool m_AimBeamHit = false;
    Vector m_AimBeamLocal{};          // muzzle in the gun model frame (fwd, right, up)
    bool m_AimBeamLocalValid = false;
    std::string m_AimBeamLocalKey;
    std::atomic<bool> m_ViewmodelBeamHidden{ false };
    Vector m_DrawnMuzzleLocal{};
    bool m_DrawnMuzzleLocalValid = false;
    std::string m_DrawnMuzzleLocalKey;
    float m_AimShootAlong = 0.f;      // shot origin along the aim from the controller
    float m_AimHitDist = 0.f;         // shot origin -> marker
    bool m_HasHevSuit = true;
    bool m_WearingHevSuit = true;
    bool m_ScopeZoomActive = false;
    bool m_CrossbowZoomLatched = false;
    float m_ZoomSmoothPitch = 0.f;
    float m_ZoomSmoothYaw = 0.f;
    float m_ZoomSmoothRoll = 0.f;
    bool m_ZoomSmoothValid = false;
    double m_ZoomSmoothMs = 0.0;
    // Laser direction at zoom start, in head-local coordinates (forward,
    // right, up). While zoomed the view keeps this offset from the head, so
    // the scope opens on the spot the laser marked and the head still steers.
    Vector m_ScopeAimLocal{ 1.f, 0.f, 0.f };
    bool m_ScopeAimLocalValid = false;
    // ScopeFollowsGun: smoothed world-space aim direction the zoomed view
    // looks along (updated with UpdateScopeAimOffset).
    Vector m_ScopeGunDir{ 1.f, 0.f, 0.f };
    bool m_ScopeGunDirValid = false;
    // ScopeRaiseToZoom: crossbow raised to the face (with hysteresis); alt-fire
    // is held while it is.
    bool m_ScopeRaised = false;
    double m_ScopeGunDirMs = 0.0;
    float m_EngineViewFov = 0.f;
    float m_EngineRestFov = 0.f;
    bool m_EngineZoomOutRamp = false;
    int m_EngineFovRiseFrames = 0;
    float m_EngineFovStableValue = 0.f;
    ULONGLONG m_EngineFovStableSinceMs = 0;
    bool m_SawEngineZoomFov = false;
    bool m_RpgLaserLatched = false;
    bool m_RpgLaserActive = false;
    Vector m_RpgLaserWorld{};
    bool m_RpgLaserPointValid = false;
    // Game's m_bLaserOn for the held RPG: flat draws rotating red lines
    // around the crosshair while it is set; VR draws them at the laser point.
    bool m_RpgLockRing = false;
    QAngle m_LastFireAim{};
    bool m_HasLastFireAim = false;
    void DrawAimCrosshair(IDirect3DDevice9* device, float sx, float sy, UINT h) const;
    // depth: z-buffer value (0 = in front of everything).
    void DrawAimDot(IDirect3DDevice9* device, float sx, float sy, float radiusPx, float depth = 0.f) const;
    void DrawRpgLockRing(IDirect3DDevice9* device, float sx, float sy, UINT h) const;
    DWORD m_WeaponActionAnimUntilMs = 0;
    int m_LatchedViewmodelIdleSeq = -1;
    bool m_CrouchToggled = false;
    bool m_SprintLatched = false;
    bool m_SprintMapToggle = true;   // button map: last sprint action pressed was "Sprint toggle"
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
    std::atomic<int> m_HudPaintedKind{ 0 };
    int m_HudPublishedKind = -1;
    bool m_OpenXrHudOverlayPublished = false;
    std::atomic<bool> m_HudPaintedThisFrame{ false };
    bool m_MenuTriggerWasDown = false;
    bool m_MenuTriggerLatch = false;   // MenuTriggerClickHeld state, menu and gameplay
    bool m_MenuCursorValid = false;
    bool m_MenuCursorSmoothValid = false;
    float m_MenuCursorSmoothX = 0.f;
    float m_MenuCursorSmoothY = 0.f;
    int m_MenuCursorX = 0;
    int m_MenuCursorY = 0;
    double m_MenuCursorSmoothMs = 0.0;
    bool m_MenuPointerLastValid = false;
    // Pointer moving (last change past the dead zone < 0.2 s ago): no dead zone.
    double m_MenuPointerMoveMs = 0.0;
    float m_MenuPointerLastPos[3]{};
    float m_MenuPointerLastDir[3]{};
    bool m_MenuPanelPoseValid = false;
    uint32_t m_MenuOverlayEpoch = 0;
    L4D2VROpenXrPoseDesc m_MenuPanelPose{};
    Vector m_MenuPanelFwd{};
    // Menu re-anchor while open: head moved away from the latch point / turned
    // > 90 deg off the panel since this tick (0 = not pending).
    DWORD m_MenuPanelMovedSinceMs = 0;
    DWORD m_MenuPanelTurnedSinceMs = 0;
    bool m_MenuPanelRoomFront = false;
    Vector m_MenuPanelRight{};
    Vector m_MenuPanelUp{};
    HWND m_MenuCursorHwnd = nullptr;
public:
    // dGetCursorPos answers with the VR pointer / the real cursor (click log).
    std::atomic<long> m_CursorHookHits{ 0 };
    std::atomic<long> m_CursorHookPass{ 0 };
    std::atomic<long> m_CursorHookHitsSec{ 0 };
    std::atomic<long> m_CursorHookPassSec{ 0 };
    std::atomic<bool> m_MenuPanelUpForCursor{ false };
private:
    DWORD m_MenuCursorLiveMs = 0;   // last frame the VR pointer hit the menu
    // Click point (after the rewind) the GetCursorPos hook reports until
    // m_MenuClickHoldUntilMs, so VGUI's press/release see where we clicked.
    static constexpr DWORD kMenuClickHoldMs = 150;
    std::atomic<DWORD> m_MenuClickHoldUntilMs{ 0 };
    // Screen points for the GetCursorPos hook, packed x/y so one atomic load
    // never mixes two frames.
    std::atomic<uint64_t> m_MenuCursorScreen{ 0 };
    std::atomic<uint64_t> m_MenuClickHoldScreen{ 0 };
    static uint64_t PackScreenPoint(const POINT& p)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 32) | static_cast<uint32_t>(p.y);
    }
    static POINT UnpackScreenPoint(uint64_t v)
    {
        return POINT{ static_cast<LONG>(static_cast<int32_t>(v >> 32)), static_cast<LONG>(static_cast<int32_t>(v & 0xffffffffu)) };
    }
    bool m_WorldVguiTriggerWasDown = false;
    bool m_WorldVguiSuppressAttack = false;
    bool m_WeaponMenuOpen = false;
    bool m_WeaponMenuClickHeld = false;
    bool m_WeaponMenuOpenedThisHold = false;
    bool m_WeaponMenuLatched = false;
    bool m_EmptyHands = false;
    // Pickup stays on the hand that pressed Use until drop. Vanilla is a
    // Use-toggle, so release has to pulse a second press to drop.
    // Local player has FL_ONTRAIN (driving a func_tracktrain from its controls).
    // The game lets go of the controls on a second Use, any IN_MOVELEFT/RIGHT
    // or leaving the ground, so the mod must not send a drop pulse or a
    // sideways bit from a slightly tilted stick meanwhile.
    bool m_OnTrainControls = false;
    bool m_GrabLatched = false;
    bool m_GrabHandLeft = false;
    bool m_LastGrabHandLeft = true;
    bool m_GrabHoldingPhysics = false;
    bool m_GrabUseEntityHeld = false;
    // G-Man gesture: the map's G-Man model indexes are looked up a few
    // times right after map start; maps without one never scan entities.
    // On his maps the entity list is rescanned at most once a second and
    // only the cached entities are checked per frame.
    void UpdateGmanGesture();
    void ScanMapForGman();
    int m_GmanModelIndex[2] = { -1, -1 };
    int m_GmanMapState = 0;            // 0 unknown, 1 has a G-Man, 2 none
    int m_GmanMapChecks = 0;
    // From the map's entity lump at LevelInit: 1 lists an npc_gman (placed or
    // a template spawned later), 0 does not, -1 not read (old 10-try check).
    int m_GmanMapListed = -1;
    DWORD m_GmanMapCheckMs = 0;
    DWORD m_GmanScanMs = 0;
    int m_GmanEntities[4] = {};
    int m_GmanEntityCount = 0;
    unsigned m_GmanLoadEpoch = 0;      // m_LoadEpoch the model indexes belong to
    std::atomic<bool> m_GmanGestureActive{ false };
    bool m_OnGun = false;
    int m_GunPropIndex = 0;
    // Right hand within reach of a gun's mount while not on it: R2 grabs
    // (= Use) instead of firing the weapon.
    bool m_GunRightNear = false;
    bool m_GunHeldLeft = false;
    bool m_GunHeldRight = false;
    bool m_GunAimBaseValid = false;
    int m_GunAimHands = 0;
    QAngle m_GunHandBase{};
    QAngle m_GunBarrelBase{};
    QAngle m_GunLastAim{};
    bool m_UseWasHeld = false;
    double m_UseDropGapUntilMs = 0.0;
    double m_UseDropPulseUntilMs = 0.0;
    double m_UseHeldSinceMs = 0.0;
    uint64_t m_UsePressTick = 0;
    std::atomic<uint64_t> m_GrabControllerSeenTick{ 0 };
    double m_UseDropClassifyUntilMs = 0.0;
    double m_UseDropSettleUntilMs = 0.0;
    int m_UseDropRetries = 0;
    // Use release / end of the last drop pulse (GetTickCount64).
    uint64_t m_UseReleaseTick = 0;
    bool m_UseFireHeld = false;
    uint64_t m_UseDropPulseEndTick = 0;
    // True while inventory has a weapon and empty-hands is not selected.
    // Sampled on the game thread for right-glove visibility.
    bool m_HasHeldWeapon = false;
    DWORD m_WeaponMenuClickStartMs = 0;
    int m_WeaponMenuHover = -1;
    uint32_t m_PrevHeldButtons = 0;
    std::atomic<bool> m_TriggerBeforeMagFilter{ false };
    bool m_PrevTriggerBeforeMagFilter = false;
    int m_WeaponMenuCount = 0;
    Vector m_WeaponMenuOrigin{};
    Vector m_WeaponMenuFwd{};
    Vector m_WeaponMenuRight{};
    Vector m_WeaponMenuUp{};
    Vector m_WeaponMenuLatchBody{};
    Vector m_WeaponMenuLatchWorld{};
    Vector m_WeaponMenuLatchDelta{};
    Vector m_WeaponMenuLatchHandDelta{};
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
        bool settingsGear = false;   // opens the VR settings panel on release
    };
    WeaponMenuSlot m_WeaponMenuSlots[20]{};
    int m_LastMuzzleFlashParity = -1;
    int m_LastFireClip = -1;
    unsigned long m_LastShotHapticMs = 0;   // GetTickCount of the last-shot pattern
    unsigned long m_HapticFollowUpMs = 0;   // 0 = none queued
    float m_HapticFollowUpSec = 0.f;
    float m_HapticFollowUpAmp = 0.f;
    void* m_LastFireWeapon = nullptr;
    bool m_StereoEyeBlitOk = false;
    int m_StereoEyeBlitRank = 0;
    uint32_t m_LastStereoBlitWidth = 0;
    uint32_t m_LastStereoBlitHeight = 0;
    bool m_LastEyeBlitWasWindowCrop = false;
};
