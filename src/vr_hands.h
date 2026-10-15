#pragma once

#include "openvr.h"
#include "vector.h"

#include <memory>
#include <string>

struct IDirect3DDevice9;

// Game thread: sample GetLightForPoint at a hand (0 left, 1 right) for the
// glove shading; the draw path (material thread) only reads the cache.
void VrHands_UpdatePalmLight(int hand, const Vector& pos);

// BM hybrid gloves: L4D2VR GLB + summary-curl mechanism only.
// No ValveBiped VM-pose, magazine interaction, or world-model pose relay.
class BmVrGloves
{
public:
    BmVrGloves();
    ~BmVrGloves();

    BmVrGloves(const BmVrGloves&) = delete;
    BmVrGloves& operator=(const BmVrGloves&) = delete;

    void OnDeviceLost();

    // Draw SteamVR vr_glove_*.glb. Caller binds the eye color (and optional
    // matching depth) before this call. Returns true if a mesh was submitted.
    bool DrawForEye(
        IDirect3DDevice9* device,
        int stereoEye,
        const Vector& eyeOrigin,
        const Vector& viewAngles,
        float horizontalFovDegrees,
        float aspectRatio,
        float sourceUnitsPerMeter,
        float modelScale,
        vr::IVRInput* input,
        vr::VRActionHandle_t leftSkeleton,
        vr::VRActionHandle_t rightSkeleton,
        bool leftOk,
        const Vector& leftWorld,
        const QAngle& leftAngles,
        bool rightOk,
        const Vector& rightWorld,
        const QAngle& rightAngles,
        float zNear,
        float zFar,
        bool overlayNoDepth = false);

    bool AssetsReady() const;
    bool HasBareHands() const;
    bool Failed() const;
    const std::string& FailureReason() const;
    bool WarmupGpu(IDirect3DDevice9* device);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
