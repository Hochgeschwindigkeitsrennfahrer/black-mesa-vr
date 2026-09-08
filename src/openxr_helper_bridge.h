#pragma once

#include <cstdint>

#include "openxr_bridge_protocol.h"

struct OpenXrHelperLaunchConfig
{
    bool enabled = false;
    // -1 auto (Oculus/Meta OpenXR), 0 off, 1 on.
    int swapProjectionEyes = -1;
    bool swapProjectionViewOrder = false;
    bool mirrorProjectionHorizontal = false;
    bool swapGameEyeOrigins = false;
    bool disableQuadOverlays = false;
    bool enableHandTracking = false;
    bool disableProjectionLayer = false;
    bool useSymmetricProjectionFov = false;
    bool useGameRenderPoseForProjection = true;
    int forceMonoProjectionEye = -1;
    int forceMonoProjectionView = -1;
    // -1 auto, 0 off (default — SteamVR was inverted with auto-on), 1 on.
    int flipSubmitY = 0;
    // 0 = run until the game process exits (L4D2VR config default after
    // 2026-06-24). A positive count is a helper self-test that then quits.
    uint32_t submitTestFrames = 0;
    uint32_t waitReadySeconds = 45;
};

OpenXrHelperLaunchConfig L4D2VR_ReadOpenXrHelperLaunchConfig();
bool L4D2VR_StartOpenXrHelper(const OpenXrHelperLaunchConfig& config);
bool L4D2VR_OpenXrHelperBridgeIsStarted();
bool L4D2VR_OpenXrHelperHasSubmittedFrame();
bool L4D2VR_ReadOpenXrHelperSubmittedFrames(uint32_t& submittedFrames);
// Helper blit progress (bridge v14): frame id being blitted and the last one
// whose blit finished on the GPU, plus a count of finished blits. False when
// the bridge is not mapped.
bool L4D2VR_ReadOpenXrHelperConsumedFrame(uint32_t& consuming, uint32_t& consumed, uint32_t& consumedCount);
bool L4D2VR_ReadOpenXrHmdPose(L4D2VROpenXrPoseDesc& pose, uint32_t* generation = nullptr);
bool L4D2VR_ReadOpenXrRuntimeViewConfig(L4D2VROpenXrRuntimeViewConfigDesc& config, uint32_t* generation = nullptr);
bool L4D2VR_PeekOpenXrVisibilityMaskGeneration(uint32_t* generation);
bool L4D2VR_ReadOpenXrVisibilityMask(L4D2VROpenXrVisibilityMaskDesc& mask, uint32_t* generation = nullptr);
bool L4D2VR_ReadOpenXrInputState(L4D2VROpenXrInputStateDesc& inputState, uint32_t* generation = nullptr);
void L4D2VR_PublishOpenXrGameRenderPose(const L4D2VROpenXrPoseDesc& pose);
void L4D2VR_PublishOpenXrHapticRequest(uint32_t handIndex, float durationSeconds, float frequency, float amplitude);
void L4D2VR_PublishOpenXrSharedTexture(uint32_t eyeIndex, const L4D2VROpenXrSharedTextureDesc& texture);
void L4D2VR_PublishOpenXrSharedTexturePair(
    const L4D2VROpenXrSharedTextureDesc& left,
    const L4D2VROpenXrSharedTextureDesc& right);
void L4D2VR_PublishOpenXrSharedTextureFrame(uint32_t frameId);
void L4D2VR_PublishOpenXrOverlay(uint32_t overlayIndex, const L4D2VROpenXrOverlayDesc& overlay);
void L4D2VR_PublishOpenXrOverlayFrame(uint32_t frameId);
