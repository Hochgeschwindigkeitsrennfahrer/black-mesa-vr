#pragma once

#include <atomic>
#include <cstdint>
#include <cctype>
#include <cstring>

// Odd generation = writer is in the middle of the payload. Release fences
// keep MSVC from moving the payload outside that window. The game and the
// helper are different processes; a process-local mutex does not order them.
inline void L4D2VR_SeqlockBegin(uint32_t& generation)
{
    ++generation;
    std::atomic_thread_fence(std::memory_order_release);
}

inline void L4D2VR_SeqlockEnd(uint32_t& generation)
{
    std::atomic_thread_fence(std::memory_order_release);
    ++generation;
}

inline void L4D2VR_SeqlockAcquire()
{
    std::atomic_thread_fence(std::memory_order_acquire);
}

constexpr uint32_t L4D2VR_OPENXR_BRIDGE_MAGIC = 0x5258344Cu; // L4XR
constexpr uint32_t L4D2VR_OPENXR_BRIDGE_VERSION = 19;
// 13: XR_KHR_visibility_mask hidden triangle list (NDC xy per eye).
// 14: helperConsumingFrameId / helperConsumedFrameId (helper -> game blit
//     progress; the game reuses a publish slot only once the helper's blit
//     out of it has finished on the GPU).
// 17: L4D2VROpenXrInputStateDesc.eyeGaze (XR_EXT_eye_gaze_interaction ray in
//     head view space, for eye-tracked foveated rendering).
// 18: L4D2VROpenXrInputStateDesc.rawHands + systemName (physical buttons per
//     hand for the DLL's button mapping; legacy actions stay as they were).
// 19: stagePose (STAGE space origin located in the LOCAL app space; the
//     room's floor + forward, for menus anchored to the room front) and
//     L4D2VROpenXrOverlayDesc.anchorPosition (OVERLAY_FLAG_ANCHOR_POSE).
// L4D2VROpenXrRuntimeViewConfigDesc.reserved0: 0 = recommended size only
// (CreateDevice), 1 = xrLocateViews FOV (InitOpenXR stereo fusion).
constexpr uint32_t L4D2VR_OPENXR_RUNTIME_VIEW_SIZE_ONLY = 0;
constexpr uint32_t L4D2VR_OPENXR_RUNTIME_VIEW_FOV_LOCATED = 1;

enum class L4D2VROpenXrBridgeStatus : uint32_t
{
    Idle = 0,
    Starting = 1,
    LoaderLoaded = 2,
    InstanceCreated = 3,
    SessionCreated = 4,
    SessionRunning = 5,
    SubmittedFrame = 6,
    Completed = 7,
    Failed = 8,
    WaitingForSharedTextures = 9,
    SharedTexturesReady = 10
};

enum : uint32_t
{
    L4D2VR_OPENXR_EYE_LEFT = 0,
    L4D2VR_OPENXR_EYE_RIGHT = 1,
    L4D2VR_OPENXR_EYE_COUNT = 2,
    L4D2VR_OPENXR_EYE_LEFT_READY = 1u << L4D2VR_OPENXR_EYE_LEFT,
    L4D2VR_OPENXR_EYE_RIGHT_READY = 1u << L4D2VR_OPENXR_EYE_RIGHT,
    L4D2VR_OPENXR_EYES_READY_MASK = L4D2VR_OPENXR_EYE_LEFT_READY | L4D2VR_OPENXR_EYE_RIGHT_READY
};

enum : uint32_t
{
    L4D2VR_OPENXR_OVERLAY_MAIN_MENU = 0,
    L4D2VR_OPENXR_OVERLAY_HUD = 1,
    L4D2VR_OPENXR_OVERLAY_COUNT = 2,
    L4D2VR_OPENXR_OVERLAY_MAIN_MENU_READY = 1u << L4D2VR_OPENXR_OVERLAY_MAIN_MENU,
    L4D2VR_OPENXR_OVERLAY_HUD_READY = 1u << L4D2VR_OPENXR_OVERLAY_HUD
};

enum : uint32_t
{
    L4D2VR_OPENXR_HAND_LEFT = 0,
    L4D2VR_OPENXR_HAND_RIGHT = 1,
    L4D2VR_OPENXR_HAND_COUNT = 2,
    L4D2VR_OPENXR_HAND_JOINT_COUNT = 26
};

enum : uint32_t
{
    L4D2VR_OPENXR_INPUT_FEATURE_CONTROLLERS = 1u << 0,
    L4D2VR_OPENXR_INPUT_FEATURE_HAND_TRACKING = 1u << 1,
    L4D2VR_OPENXR_INPUT_FEATURE_HAPTICS = 1u << 2
};

// L4D2VROpenXrInputStateDesc.reserved0. G2/WMR defaults stay on UNKNOWN.
enum : uint32_t
{
    L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN = 0,
    L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH = 1, // Quest / Rift / Pico Touch-like
    L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2 = 2,
    L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES = 3,
    L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE = 4,
    // Steam Frame controllers (XR_VALVE_frame_controller_interaction). Without
    // that profile SteamVR emulates Touch and folds bumper / right X+Y / D-pad
    // onto trigger / B / X+Y (frame_controller_remapping.json, upstream #20).
    L4D2VR_OPENXR_CONTROLLER_FAMILY_FRAME = 5
};

inline bool L4D2VR_TextContainsI(const char* haystack, const char* needle)
{
    if (!haystack || !needle || !*needle)
        return false;
    for (const char* p = haystack; *p; ++p)
    {
        const char* h = p;
        const char* n = needle;
        while (*h && *n)
        {
            const unsigned char hc = static_cast<unsigned char>(*h);
            const unsigned char nc = static_cast<unsigned char>(*n);
            if (std::tolower(hc) != std::tolower(nc))
                break;
            ++h;
            ++n;
        }
        if (!*n)
            return true;
    }
    return false;
}

inline uint32_t L4D2VR_ClassifyOpenXrInteractionProfile(const char* path)
{
    if (!path || !*path)
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
    if (L4D2VR_TextContainsI(path, "touch_controller") ||
        L4D2VR_TextContainsI(path, "pico4_controller") ||
        L4D2VR_TextContainsI(path, "pico_neo") ||
        L4D2VR_TextContainsI(path, "pico4") ||
        L4D2VR_TextContainsI(path, "mixed_reality_controller"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH;
    if (L4D2VR_TextContainsI(path, "index_controller"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES;
    if (L4D2VR_TextContainsI(path, "motion_controller"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2;
    if (L4D2VR_TextContainsI(path, "vive_controller") ||
        L4D2VR_TextContainsI(path, "vive_cosmos") ||
        L4D2VR_TextContainsI(path, "vive_focus"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE;
    if (L4D2VR_TextContainsI(path, "frame_controller"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_FRAME;
    return L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
}

inline uint32_t L4D2VR_ClassifyOpenVrControllerType(const char* type)
{
    if (!type || !*type)
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
    if (L4D2VR_TextContainsI(type, "oculus_touch") ||
        L4D2VR_TextContainsI(type, "oculus_plus") ||
        L4D2VR_TextContainsI(type, "meta_quest") ||
        L4D2VR_TextContainsI(type, "rift") ||
        L4D2VR_TextContainsI(type, "pico"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH;
    if (L4D2VR_TextContainsI(type, "knuckles"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES;
    if (L4D2VR_TextContainsI(type, "hpmotion") ||
        L4D2VR_TextContainsI(type, "hp_motion") ||
        L4D2VR_TextContainsI(type, "holographic"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2;
    if (L4D2VR_TextContainsI(type, "vive"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE;
    if (L4D2VR_TextContainsI(type, "frame_controller"))
        return L4D2VR_OPENXR_CONTROLLER_FAMILY_FRAME;
    return L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
}

inline bool L4D2VR_ControllerFamilyPrefersAimPose(uint32_t family)
{
    // Touch and Index grip -Z follow the handle (up when pointing). OpenXR
    // aim is the pointing axis. L4D2VR instead pitches every OpenVR device
    // pose -45°. Index hands still need a 90° inward roll around that aim
    // (thumbstick face vs palm); weapons keep the unrolled aim pose.
    return family == L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH
        || family == L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES
        || family == L4D2VR_OPENXR_CONTROLLER_FAMILY_FRAME;
}

// Pose tuning (hand offsets, gun tilt, viewmodel ox) per family. The Frame
// used to arrive as SteamVR's Touch emulation, so it keeps the Touch tuning
// until a Frame owner reports otherwise.
inline uint32_t L4D2VR_ControllerFamilyPoseTuning(uint32_t family)
{
    return family == L4D2VR_OPENXR_CONTROLLER_FAMILY_FRAME ? L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH : family;
}

inline const char* L4D2VR_ControllerFamilyName(uint32_t family)
{
    switch (family)
    {
    case L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH: return "touch";
    case L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2: return "g2";
    case L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES: return "knuckles";
    case L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE: return "vive";
    case L4D2VR_OPENXR_CONTROLLER_FAMILY_FRAME: return "frame";
    default: return "unknown";
    }
}

enum class L4D2VROpenXrActionId : uint32_t
{
    Invalid = 0,
    ActivateVR,
    Jump,
    PrimaryAttack,
    Reload,
    Use,
    Teleport,
    Walk,
    Turn,
    SecondaryAttack,
    NextItem,
    PrevItem,
    ResetPosition,
    Crouch,
    Flashlight,
    InventoryGripLeft,
    InventoryGripRight,
    InventoryQuickSwitch,
    SpecialInfectedAutoAimToggle,
    SpecialInfectedDodgeToggle,
    LedgeGuardToggle,
    EffectiveAttackRangeAutoFireToggle,
    SpeechToText,
    MenuSelect,
    MenuBack,
    MenuUp,
    MenuDown,
    MenuLeft,
    MenuRight,
    Spray,
    Scoreboard,
    ShowHUD,
    Pause,
    NonVRServerMovementAngleToggle,
    ScopeToggle,
    FriendlyFireBlockToggle,
    CustomAction1,
    CustomAction2,
    CustomAction3,
    CustomAction4,
    CustomAction5,
    Count
};

constexpr uint32_t L4D2VR_OPENXR_ACTION_COUNT =
    static_cast<uint32_t>(L4D2VROpenXrActionId::Count);

struct L4D2VROpenXrSharedTextureDesc
{
    uint32_t valid = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t sampleCount = 0;
    uint32_t handleType = 0;
    uint32_t queueFamilyIndex = 0;
    // L4D2VR_OPENXR_SHARED_UV_EXPLICIT: game already cropped the source
    // (GMod ring band / eye half). Helper must not overwrite UVs with a
    // full-texture projection crop.
    uint32_t reserved0 = 0;
    uint64_t kmtHandle = 0;
    uint64_t image = 0;
    float uMin = 0.0f;
    float vMin = 0.0f;
    float uMax = 1.0f;
    float vMax = 1.0f;
    float renderFovXDeg = 90.0f;
    float renderAspect = 1.0f;
    // Bridge v15: tangents of the frustum the whole texture was rendered with
    // (left < 0 < right, down < 0 < up). When all four are set the helper
    // submits exactly this FOV and never crops; it overrides renderFovXDeg /
    // renderAspect and the runtime FOV. 0 = unset (old behaviour). Used for
    // an eye rendered off-centre, e.g. the left eye widened to the right by
    // LeftEyeCullPad.
    float tanLeft = 0.0f;
    float tanRight = 0.0f;
    float tanDown = 0.0f;
    float tanUp = 0.0f;
};

struct L4D2VROpenXrOverlayDesc
{
    uint32_t valid = 0;
    uint32_t visible = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    L4D2VROpenXrSharedTextureDesc texture = {};
    float widthMeters = 1.5f;
    float heightMeters = 0.84375f;
    float distanceMeters = 3.0f;
    float curvature = 0.0f;
    float offsetMeters[3] = { 0.0f, -0.25f, 0.0f };
    float orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    // v19, with L4D2VR_OPENXR_OVERLAY_FLAG_ANCHOR_POSE: the head position the
    // overlay is placed from (LOCAL space); orientation gives its yaw.
    float anchorPosition[3] = { 0.0f, 0.0f, 0.0f };
};

// reserved1 bits on L4D2VROpenXrPoseDesc. reserved0 is still the packed IPD
// (0 means "unset" and must not be used for a real 0 mm IPD).
constexpr uint32_t L4D2VR_OPENXR_POSE_FLAG_MONO = 1u << 0;
// L4D2VROpenXrOverlayDesc.reserved1. HUD overlay uses texture alpha (HL2VR
// IgnoreTextureAlpha=false). reserved0 stays the spatial-lock epoch.
constexpr uint32_t L4D2VR_OPENXR_OVERLAY_FLAG_BLEND_ALPHA = 1u << 0;
// Placed in front of the head every frame (full head pose) instead of the
// pose captured when the epoch started: chapter titles, intro logo, "Saved...".
constexpr uint32_t L4D2VR_OPENXR_OVERLAY_FLAG_HEAD_LOCKED = 1u << 1;
// Colour is premultiplied by alpha (title texts: Black Mesa's additive fonts
// paint colour*a). Without it the HUD quad is submitted UNPREMULTIPLIED.
// Older helpers ignore the bit (text then composites a little darker).
constexpr uint32_t L4D2VR_OPENXR_OVERLAY_FLAG_PREMULTIPLIED = 1u << 2;
// Placed from anchorPosition + orientation (yaw) instead of the head pose when
// the epoch starts: the pause menu uses the game's latched menu panel pose
// (room front at eye height), the same pose its pointer ray hits.
constexpr uint32_t L4D2VR_OPENXR_OVERLAY_FLAG_ANCHOR_POSE = 1u << 3;
// L4D2VROpenXrSharedTextureDesc.reserved0. UVs are a subrect of a shared
// ring (or similar); do not treat 0..1 as one eye.
constexpr uint32_t L4D2VR_OPENXR_SHARED_UV_EXPLICIT = 1u << 0;

struct L4D2VROpenXrPoseDesc
{
    uint32_t valid = 0;
    uint32_t viewStateFlags = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    int64_t displayTime = 0;
    float position[3] = {};
    float orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

// Bridge v16, game -> helper: where the mono 2D menu quad (main menu / load
// GameUI) sits, the same numbers the game's menu pointer ray uses. distance /
// width / offsetY in metres from the latched head pose; contentScale is the
// letterbox fraction the game drew the menu into the eye image at
// (MenuPanelScale). 0 = helper default.
struct L4D2VROpenXrMenuQuadDesc
{
    float distanceMeters = 0.0f;
    float widthMeters = 0.0f;
    float offsetYMeters = 0.0f;
    float contentScale = 0.0f;
};

struct L4D2VROpenXrControllerPoseDesc
{
    uint32_t valid = 0;
    uint32_t active = 0;
    uint64_t locationFlags = 0;
    int64_t displayTime = 0;
    float position[3] = {};
    float orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct L4D2VROpenXrDigitalActionDesc
{
    uint32_t active = 0;
    uint32_t state = 0;
    uint32_t changed = 0;
    uint32_t activeOrigin = 0;
    int64_t lastChangeTime = 0;
};

struct L4D2VROpenXrAnalogActionDesc
{
    uint32_t active = 0;
    uint32_t changed = 0;
    uint32_t activeOrigin = 0;
    uint32_t reserved0 = 0;
    int64_t lastChangeTime = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct L4D2VROpenXrHandJointDesc
{
    uint64_t locationFlags = 0;
    float radius = 0.0f;
    float position[3] = {};
    float orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct L4D2VROpenXrHandTrackingDesc
{
    uint32_t valid = 0;
    uint32_t active = 0;
    uint32_t jointCount = 0;
    uint32_t reserved0 = 0;
    float fingerCurls[5] = {};
    L4D2VROpenXrHandJointDesc joints[L4D2VR_OPENXR_HAND_JOINT_COUNT] = {};
};

// Combined eye gaze (XR_EXT_eye_gaze_interaction), located in the VIEW
// space at the frame's predicted display time. dir = gaze forward, unit
// vector, x right, y up, -z forward (OpenXR view axes).
struct L4D2VROpenXrEyeGazeDesc
{
    uint32_t valid = 0;          // extension active, action bound
    uint32_t tracked = 0;        // this sample's orientation is tracked
    uint32_t focused = 0;        // xrSyncActions returned XR_SUCCESS
    uint32_t reserved0 = 0;
    uint64_t publishTickMs = 0;  // GetTickCount64() when the helper wrote it
    int64_t sampleTime = 0;      // XrEyeGazeSampleTimeEXT (0 = unknown)
    float dir[3] = { 0.0f, 0.0f, -1.0f };
    float reserved1 = 0.0f;
};

// Physical inputs per hand (protocol 18). face1 = A/X/Cross (lower face
// button), face2 = B/Y/Circle (upper). Index: A/B on both hands. Controllers
// without face buttons (WMR, Vive wand) report none of those bits.
// Steam Frame (Xbox-style diamond): face1..4 = A/B/X/Y on the right hand and
// D-pad down/right/left/up on the left (SteamVR's own mirror pairs), plus a
// bumper on each hand. Only the Frame sets FACE3/FACE4/BUMPER.
enum : uint32_t
{
    L4D2VR_RAW_BUTTON_FACE1 = 1u << 0,
    L4D2VR_RAW_BUTTON_FACE2 = 1u << 1,
    L4D2VR_RAW_BUTTON_STICK_CLICK = 1u << 2,
    L4D2VR_RAW_BUTTON_MENU = 1u << 3,
    L4D2VR_RAW_BUTTON_FACE3 = 1u << 4,
    L4D2VR_RAW_BUTTON_FACE4 = 1u << 5,
    L4D2VR_RAW_BUTTON_BUMPER = 1u << 6,
    L4D2VR_RAW_BUTTON_FACE1_PRESENT = 1u << 16, // bound on the active profile
    L4D2VR_RAW_BUTTON_FACE2_PRESENT = 1u << 17,
    L4D2VR_RAW_BUTTON_STICK_CLICK_PRESENT = 1u << 18,
    L4D2VR_RAW_BUTTON_GRIP_PRESENT = 1u << 19,
    L4D2VR_RAW_BUTTON_FACE3_PRESENT = 1u << 20,
    L4D2VR_RAW_BUTTON_FACE4_PRESENT = 1u << 21,
    L4D2VR_RAW_BUTTON_BUMPER_PRESENT = 1u << 22,
};

struct L4D2VROpenXrRawHandDesc
{
    uint32_t valid = 0;   // raw actions created and the hand is active
    uint32_t buttons = 0; // L4D2VR_RAW_BUTTON_*
    float trigger = 0.0f; // 0..1
    float grip = 0.0f;    // 0..1 (squeeze/click profiles: 0 or 1)
    float stick[2] = {};  // thumbstick / joystick / trackpad
};

struct L4D2VROpenXrInputStateDesc
{
    uint32_t valid = 0;
    uint32_t featureFlags = 0;
    uint32_t actionCount = L4D2VR_OPENXR_ACTION_COUNT;
    uint32_t reserved0 = 0; // L4D2VR_OPENXR_CONTROLLER_FAMILY_*
    L4D2VROpenXrControllerPoseDesc controllerPoses[L4D2VR_OPENXR_HAND_COUNT] = {};
    L4D2VROpenXrControllerPoseDesc controllerAimPoses[L4D2VR_OPENXR_HAND_COUNT] = {};
    L4D2VROpenXrDigitalActionDesc digitalActions[L4D2VR_OPENXR_ACTION_COUNT] = {};
    L4D2VROpenXrAnalogActionDesc analogActions[L4D2VR_OPENXR_ACTION_COUNT] = {};
    L4D2VROpenXrHandTrackingDesc handTracking[L4D2VR_OPENXR_HAND_COUNT] = {};
    L4D2VROpenXrEyeGazeDesc eyeGaze = {};
    L4D2VROpenXrRawHandDesc rawHands[L4D2VR_OPENXR_HAND_COUNT] = {};
    char systemName[64] = {}; // XrSystemProperties.systemName (PS VR2 vs Quest labels)
};

struct L4D2VROpenXrHapticRequestDesc
{
    uint32_t sequence = 0;
    uint32_t valid = 0;
    float durationSeconds = 0.0f;
    float frequency = 0.0f;
    float amplitude = 0.0f;
};

struct L4D2VROpenXrRuntimeViewDesc
{
    uint32_t valid = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t recommendedSampleCount = 0;
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleUp = 0.0f;
    float angleDown = 0.0f;
};

struct L4D2VROpenXrRuntimeViewConfigDesc
{
    uint32_t valid = 0;
    uint32_t viewCount = 0;
    uint32_t reserved0 = 0; // L4D2VR_OPENXR_RUNTIME_VIEW_*
    uint32_t reserved1 = 0;
    L4D2VROpenXrRuntimeViewDesc views[L4D2VR_OPENXR_EYE_COUNT] = {};
};

// Hidden-area triangle list from XR_KHR_visibility_mask. Vertices are view
// NDC (origin center, +X right, +Y up). vertexCount is a multiple of 3.
constexpr uint32_t L4D2VR_OPENXR_VIS_MASK_MAX_VERTS = 3072;

struct L4D2VROpenXrVisibilityMaskEyeDesc
{
    uint32_t valid = 0;
    uint32_t vertexCount = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    float xy[L4D2VR_OPENXR_VIS_MASK_MAX_VERTS * 2] = {};
};

struct L4D2VROpenXrVisibilityMaskDesc
{
    uint32_t valid = 0;
    uint32_t eyeCount = L4D2VR_OPENXR_EYE_COUNT;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
    L4D2VROpenXrVisibilityMaskEyeDesc eyes[L4D2VR_OPENXR_EYE_COUNT] = {};
};

struct L4D2VROpenXrBridgeState
{
    uint32_t magic = L4D2VR_OPENXR_BRIDGE_MAGIC;
    uint32_t version = L4D2VR_OPENXR_BRIDGE_VERSION;
    uint32_t size = sizeof(L4D2VROpenXrBridgeState);
    uint32_t status = static_cast<uint32_t>(L4D2VROpenXrBridgeStatus::Idle);
    uint32_t gamePid = 0;
    uint32_t helperPid = 0;
    uint32_t submittedFrames = 0;
    int32_t exitCode = 0;
    uint64_t heartbeatTickMs = 0;
    uint32_t sharedTextureGeneration = 0;
    uint32_t sharedTexturesReadyMask = 0;
    uint32_t sharedTextureFrameGeneration = 0;
    uint32_t sharedTextureFrameId = 0;
    L4D2VROpenXrSharedTextureDesc eyeTextures[L4D2VR_OPENXR_EYE_COUNT] = {};
    uint32_t trackingPoseGeneration = 0;
    L4D2VROpenXrPoseDesc hmdPose = {};
    uint32_t gameRenderPoseGeneration = 0;
    L4D2VROpenXrPoseDesc gameRenderPose = {};
    uint32_t runtimeViewConfigGeneration = 0;
    L4D2VROpenXrRuntimeViewConfigDesc runtimeViewConfig = {};
    uint32_t inputStateGeneration = 0;
    L4D2VROpenXrInputStateDesc inputState = {};
    L4D2VROpenXrHapticRequestDesc hapticRequests[L4D2VR_OPENXR_HAND_COUNT] = {};
    uint32_t overlayGeneration = 0;
    uint32_t overlayReadyMask = 0;
    uint32_t overlayFrameGeneration = 0;
    uint32_t overlayFrameId = 0;
    L4D2VROpenXrOverlayDesc overlays[L4D2VR_OPENXR_OVERLAY_COUNT] = {};
    uint32_t visibilityMaskGeneration = 0;
    L4D2VROpenXrVisibilityMaskDesc visibilityMask = {};
    char detail[256] = {};
    // Helper -> game. sharedTextureFrameId the helper is blitting into its
    // swapchain (written before the blit) and the one whose blit has finished
    // (written after vkQueueWaitIdle). Single aligned words, release/acquire
    // fenced. The frame id the helper reports is <= the frame of the pair it
    // actually blitted (pair is re-read after the id), so gating a slot on
    // helperConsumedFrameId >= slotFrameId is conservative.
    uint32_t helperConsumingFrameId = 0;
    uint32_t helperConsumedFrameId = 0;
    uint32_t helperConsumedCount = 0;
    uint32_t reservedTail0 = 0;
    uint32_t menuQuadGeneration = 0;
    L4D2VROpenXrMenuQuadDesc menuQuad = {};
    // Helper -> game (v19). valid = 0 when the runtime has no STAGE space
    // or cannot locate it (orientation not tracked).
    uint32_t stagePoseGeneration = 0;
    L4D2VROpenXrPoseDesc stagePose = {};
};
