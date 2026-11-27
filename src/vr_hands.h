#pragma once

#include "openvr.h"
#include "vector.h"

#include <memory>
#include <string>

struct IDirect3DDevice9;

// Game thread: sample GetLightForPoint at a hand (0 left, 1 right) for the
// glove shading; the draw path (material thread) only reads the cache.
void VrHands_UpdatePalmLight(int hand, const Vector& pos);

// Ladder rung wrap applied to the glove drawn this eye (0 left, 1 right; set
// by BmVrGloves::DrawForEye, read by the wrist HUD drawn after it): a world
// point p of the controller frame is drawn at R (p - from) + to. The wrist
// panel goes through the same map so it stays on the hand, not the
// controller (user 2026-11-27).
struct VrGloveRungXform
{
    bool valid = false;
    float R[3][3]{};
    Vector from{}, to{};
    Vector Point(const Vector& p) const
    {
        const Vector d = p - from;
        return Vector(R[0][0] * d.x + R[0][1] * d.y + R[0][2] * d.z + to.x,
            R[1][0] * d.x + R[1][1] * d.y + R[1][2] * d.z + to.y,
            R[2][0] * d.x + R[2][1] * d.y + R[2][2] * d.z + to.z);
    }
    Vector Dir(const Vector& v) const
    {
        return Vector(R[0][0] * v.x + R[0][1] * v.y + R[0][2] * v.z,
            R[1][0] * v.x + R[1][1] * v.y + R[1][2] * v.z,
            R[2][0] * v.x + R[2][1] * v.y + R[2][2] * v.z);
    }
};
extern VrGloveRungXform g_VrGloveRungXform[2];

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
