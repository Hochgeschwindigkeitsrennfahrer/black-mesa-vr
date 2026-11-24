#include "vr_hands.h"

#include "bmvr_flags.h"
#include "game.h"
#include "vr.h"
#include "vr_bm_hands.h"
#include "vr_hand_asset_loader.h"
#include "vr_hand_math.h"
#include "vr_hand_renderer_d3d9.h"
#include "vr_mag_reload.h"

#include "openvr.h"

#include <algorithm>
#include <mutex>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr float kEyeZNear = 7.f;
    constexpr float kEyeZFar = 28377.f;

    int FindNameIndex(const std::vector<std::string>& names, const std::string& name)
    {
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    VrHandMatrix4 BuildBindLocalMatrix(const VrHandMeshAsset& asset, int joint)
    {
        const int parent = asset.jointParents[static_cast<size_t>(joint)];
        if (parent >= 0 && parent < static_cast<int>(asset.inverseBindMatrices.size()))
        {
            return VrHandMath::Multiply(
                asset.inverseBindMatrices[static_cast<size_t>(parent)],
                asset.bindMatrices[static_cast<size_t>(joint)]);
        }
        return asset.bindMatrices[static_cast<size_t>(joint)];
    }

    VrHandMatrix4 MakeLocalYRotation(float radians)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        VrHandMath::Set(out, 0, 0, c);
        VrHandMath::Set(out, 0, 2, s);
        VrHandMath::Set(out, 2, 0, -s);
        VrHandMath::Set(out, 2, 2, c);
        return out;
    }

    // Rotation (column vectors) turning unit vector a towards unit vector b
    // by `weight` of the angle between them.
    VrHandMatrix4 MakeTurnTowards(const float a[3], const float b[3], float weight)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        float axis[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
        const float s = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        const float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        if (s < 1e-6f)
            return out;
        for (float& v : axis)
            v /= s;
        const float ang = std::atan2(s, c) * weight;
        const float ca = std::cos(ang), sa = std::sin(ang), t = 1.f - ca;
        const float x = axis[0], y = axis[1], z = axis[2];
        VrHandMath::Set(out, 0, 0, t * x * x + ca);     VrHandMath::Set(out, 0, 1, t * x * y - sa * z); VrHandMath::Set(out, 0, 2, t * x * z + sa * y);
        VrHandMath::Set(out, 1, 0, t * x * y + sa * z); VrHandMath::Set(out, 1, 1, t * y * y + ca);     VrHandMath::Set(out, 1, 2, t * y * z - sa * x);
        VrHandMath::Set(out, 2, 0, t * x * z - sa * y); VrHandMath::Set(out, 2, 1, t * y * z + sa * x); VrHandMath::Set(out, 2, 2, t * z * z + ca);
        return out;
    }

    VrHandMatrix4 MakeLocalZRotation(float radians)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        VrHandMath::Set(out, 0, 0, c);
        VrHandMath::Set(out, 0, 1, -s);
        VrHandMath::Set(out, 1, 0, s);
        VrHandMath::Set(out, 1, 1, c);
        return out;
    }

    // L4D2VR VrHandSkeletonRuntime::BuildSummaryCurlPalette — GLB bind locals
    // plus OpenVR summary curls. No ozz.
    bool PathContainsI(const std::string& path, const char* needle)
    {
        if (!needle || needle[0] == '\0')
            return false;
        const size_t nlen = std::strlen(needle);
        if (nlen == 0 || nlen > path.size())
            return false;
        for (size_t i = 0; i + nlen <= path.size(); ++i)
        {
            size_t j = 0;
            for (; j < nlen; ++j)
            {
                char a = path[i + j];
                char b = needle[j];
                if (a >= 'A' && a <= 'Z')
                    a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z')
                    b = static_cast<char>(b - 'A' + 'a');
                if (a != b)
                    break;
            }
            if (j == nlen)
                return true;
        }
        return false;
    }

    // HEV and bare-hand rips share ValveBiped hinges (index–pinky opposite
    // SteamVR glove +Z). Do not negate SteamVR vr_glove fallback assets.
    bool AssetUsesValveBipedCurl(const VrHandMeshAsset& asset)
    {
        return PathContainsI(asset.sourcePath, "hev_glove")
            || PathContainsI(asset.sourcePath, "bare_hand");
    }

    // Everything about a hand asset that does not depend on the live finger
    // curls: joint lookups, bind-local matrices, a parents-first evaluation
    // order and the curl sign. Built once per asset. The old per-call version
    // rebuilt 15 joint-name strings, ran ~450 std::string compares, allocated
    // four vectors and resolved the hierarchy in O(joints^2) passes — per
    // hand, per eye, per frame.
    struct HandRig
    {
        bool built = false;
        bool valid = false;
        bool valveBiped = false;
        bool rightHand = false;
        int fingerJoint[vr::VRFinger_Count][3]{};
        std::vector<VrHandMatrix4> bindLocal;
        std::vector<int> order;
        std::vector<VrHandMatrix4> scratchLocal;
        std::vector<VrHandMatrix4> scratchModel;
        // Inputs of the last palette so an identical request (the second eye
        // of the same frame, or idle hands) is a no-op.
        bool paletteValid = false;
        float lastCurl[vr::VRFinger_Count]{};
        float lastGripCurlMin = -1.f;
        float lastGmanThumb[3]{};
        float lastStraight = 0.f;
    };

    void BuildHandRig(const VrHandMeshAsset& asset, HandRig& rig)
    {
        rig.built = true;
        rig.valid = false;
        rig.paletteValid = false;
        const bool rightHand = FindNameIndex(asset.jointNames, "wrist_r") >= 0;
        const bool leftHand = FindNameIndex(asset.jointNames, "wrist_l") >= 0;
        if (!rightHand && !leftHand)
            return;

        rig.rightHand = rightHand;
        const char suffix = rightHand ? 'r' : 'l';
        static const char* kFingerNames[vr::VRFinger_Count] =
        {
            "thumb",
            "index",
            "middle",
            "ring",
            "pinky"
        };
        for (int finger = 0; finger < vr::VRFinger_Count; ++finger)
        {
            for (int segment = 0; segment < 3; ++segment)
            {
                const std::string jointName = std::string("finger_") +
                    kFingerNames[finger] + "_" + static_cast<char>('0' + segment) + "_" + suffix;
                const int joint = FindNameIndex(asset.jointNames, jointName);
                if (joint < 0)
                    return;
                rig.fingerJoint[finger][segment] = joint;
            }
        }

        const size_t n = asset.jointNames.size();
        if (asset.jointParents.size() < n || asset.inverseBindMatrices.size() < n
            || asset.bindMatrices.size() < n)
            return;
        rig.bindLocal.resize(n);
        for (size_t joint = 0; joint < n; ++joint)
            rig.bindLocal[joint] = BuildBindLocalMatrix(asset, static_cast<int>(joint));

        // Parents-first order, same fixpoint the old loop computed each call.
        rig.order.clear();
        rig.order.reserve(n);
        std::vector<bool> resolved(n, false);
        size_t unresolved = n;
        for (size_t pass = 0; pass < n && unresolved > 0; ++pass)
        {
            bool progressed = false;
            for (size_t joint = 0; joint < n; ++joint)
            {
                if (resolved[joint])
                    continue;
                const int parent = asset.jointParents[joint];
                if (parent >= 0 && (parent >= static_cast<int>(n) || !resolved[static_cast<size_t>(parent)]))
                    continue;
                rig.order.push_back(static_cast<int>(joint));
                resolved[joint] = true;
                --unresolved;
                progressed = true;
            }
            if (!progressed)
                return;
        }
        // HEV/bare ValveBiped index–pinky hinges are opposite SteamVR glove +Z.
        // Thumb already curls inward on the ripped mesh. Do not negate
        // SteamVR vr_glove fallback assets.
        rig.valveBiped = AssetUsesValveBipedCurl(asset);
        rig.scratchLocal.resize(n);
        rig.scratchModel.resize(n);
        rig.valid = true;
    }

    // L4D2VR VrHandSkeletonRuntime::BuildSummaryCurlPalette — GLB bind locals
    // plus OpenVR summary curls. No ozz.
    bool BuildSummaryCurlPalette(
        const VrHandMeshAsset& asset,
        HandRig& rig,
        const vr::VRSkeletalSummaryData_t& summary,
        std::vector<VrHandMatrixRows3x4>& outPalette,
        float gripCurlMin,
        float gmanThumb = 0.f,
        float straightMiddle = 0.f)
    {
        if (!rig.built)
            BuildHandRig(asset, rig);
        if (!rig.valid)
            return false;

        float curls[vr::VRFinger_Count];
        for (int finger = 0; finger < vr::VRFinger_Count; ++finger)
        {
            float curl = std::clamp(summary.flFingerCurl[finger], -0.5f, 1.0f);
            if (gripCurlMin > 0.f)
            {
                // Thumb (finger 0): keep nearly open so it does not clip into
                // the pistol grip. Other fingers use the grip floor.
                const float minCurl = (finger == 0) ? 0.05f : gripCurlMin;
                if (curl < minCurl)
                    curl = minCurl;
            }
            curls[finger] = curl;
        }
        // Gesture thumb: lies across the folded fingers instead of curling
        // through them, which takes a swing at its root as well as a curl.
        // Only the ripped gloves have these hinges; others just curl.
        const float thumbWeight = rig.valveBiped ? gmanThumb : 0.f;
        if (!rig.valveBiped && gmanThumb > curls[0])
            curls[0] = gmanThumb;
        const float gmanThumbState[3] = {
            thumbWeight,
            thumbWeight * bmvr::g_VrGmanThumbCurl,
            thumbWeight * bmvr::g_VrGmanThumbSwing,
        };
        const size_t n = asset.jointNames.size();
        if (rig.paletteValid && outPalette.size() == n && rig.lastGripCurlMin == gripCurlMin
            && std::memcmp(curls, rig.lastCurl, sizeof(curls)) == 0
            && std::memcmp(gmanThumbState, rig.lastGmanThumb, sizeof(gmanThumbState)) == 0
            && rig.lastStraight == straightMiddle)
            return true;

        static const float kMaxCurlRadians[vr::VRFinger_Count][3] =
        {
            { 0.75f, 0.90f, 0.65f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
        };
        // Thumb joint angles of the gesture at g_VrGmanThumbCurl 1.
        static const float kGmanThumbRadians[3] = { 0.30f, 0.60f, 0.40f };

        std::vector<VrHandMatrix4>& localMatrices = rig.scratchLocal;
        std::vector<VrHandMatrix4>& modelMatrices = rig.scratchModel;
        localMatrices.assign(rig.bindLocal.begin(), rig.bindLocal.end());
        for (int finger = 0; finger < vr::VRFinger_Count; ++finger)
        {
            const float sign = (rig.valveBiped && finger > 0) ? -1.f : 1.f;
            for (int segment = 0; segment < 3; ++segment)
            {
                const size_t joint = static_cast<size_t>(rig.fingerJoint[finger][segment]);
                float radians = sign * curls[finger] * kMaxCurlRadians[finger][segment];
                if (finger == 0 && thumbWeight > 0.f)
                {
                    radians = (1.f - thumbWeight) * radians + gmanThumbState[1] * kGmanThumbRadians[segment];
                    if (segment == 0)
                    {
                        // The rigs are mirrored, so the swing changes sign per hand.
                        const float swing = (rig.rightHand ? -1.f : 1.f) * gmanThumbState[2];
                        localMatrices[joint] = VrHandMath::Multiply(localMatrices[joint], MakeLocalYRotation(swing));
                    }
                }
                localMatrices[joint] = VrHandMath::Multiply(
                    localMatrices[joint],
                    MakeLocalZRotation(radians));
            }
        }

        // Straight middle finger (G-Man gesture on the Black Mesa gloves):
        // each segment turned so it continues the one before it (the first
        // along the wrist -> knuckle line). Bending back about the curl axis
        // made it zig-zag sideways on these rigs, whose joint axes are not
        // square to the finger (user 2026-11-19: "crooked").
        if (straightMiddle > 0.f)
        {
            auto trans = [&](size_t j, float out[3]) {
                for (int r = 0; r < 3; ++r)
                    out[r] = VrHandMath::Get(rig.bindLocal[j], r, 3);
            };
            auto norm = [](float v[3]) {
                const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (l < 1e-6f)
                    return false;
                v[0] /= l; v[1] /= l; v[2] /= l;
                return true;
            };
            const int* fj = rig.fingerJoint[2];
            for (int segment = 0; segment < 3; ++segment)
            {
                const size_t joint = static_cast<size_t>(fj[segment]);
                // Bone direction in the joint's own frame: towards the next
                // joint; the tip has none, so it reuses the previous
                // segment's (same joint-frame convention along the finger).
                float bone[3];
                trans(static_cast<size_t>(fj[segment < 2 ? segment + 1 : 2]), bone);
                // Where it should point, in the parent frame: along this
                // joint's offset from the parent (the parent's bone).
                float want[3];
                trans(joint, want);
                if (!norm(bone) || !norm(want))
                    continue;
                // Into the joint frame (rotation part of the bind local, transposed).
                float a[3];
                for (int r = 0; r < 3; ++r)
                    a[r] = VrHandMath::Get(rig.bindLocal[joint], 0, r) * want[0]
                        + VrHandMath::Get(rig.bindLocal[joint], 1, r) * want[1]
                        + VrHandMath::Get(rig.bindLocal[joint], 2, r) * want[2];
                localMatrices[joint] = VrHandMath::Multiply(rig.bindLocal[joint], MakeTurnTowards(bone, a, straightMiddle));
            }
        }

        for (const int j : rig.order)
        {
            const size_t joint = static_cast<size_t>(j);
            const int parent = asset.jointParents[joint];
            modelMatrices[joint] = (parent >= 0)
                ? VrHandMath::Multiply(modelMatrices[static_cast<size_t>(parent)], localMatrices[joint])
                : localMatrices[joint];
        }

        outPalette.resize(n);
        for (size_t joint = 0; joint < n; ++joint)
        {
            outPalette[joint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(
                modelMatrices[joint],
                asset.inverseBindMatrices[joint]));
        }
        std::memcpy(rig.lastCurl, curls, sizeof(curls));
        std::memcpy(rig.lastGmanThumb, gmanThumbState, sizeof(gmanThumbState));
        rig.lastStraight = straightMiddle;
        rig.lastGripCurlMin = gripCurlMin;
        rig.paletteValid = true;
        return true;
    }

    // VrStockHandPose: left glove fingers in the gun's own stock left-hand
    // pose (idle), wrist kept on the controller. rel = each finger joint
    // relative to L_Wrist on the viewmodel (VR::GetStockOffhandFingers).
    // Only the joint rotations come from the viewmodel; bone lengths come
    // from the glove's source bones (asset.sourceBoneBind: BM bone frames
    // in glove space), so units and per-model bind differences drop out.
    // Extra thumb bend on a wrist-relative finger pose (rows 3x4, joints
    // thumb1..3 first), each about the joint's own z (BM bend axis; + closes
    // the left thumb toward the palm, checked on the glock clip pose), joints
    // further out carried along: Thumb1 (root: the whole thumb) by baseDeg,
    // Thumb2 and Thumb3 by tipDeg.
    void CurlThumb(float rel[15][12], float baseDeg, float tipDeg)
    {
        for (int seg = 0; seg < 3; ++seg)
        {
            const float deg = seg == 0 ? baseDeg : tipDeg;
            if (deg == 0.f)
                continue;
            const float a = deg * (3.14159265f / 180.f);
            const float ca = std::cos(a), sa = std::sin(a);
            const float* pj = rel[seg];
            const float ax[3] = { pj[2], pj[6], pj[10] };
            const float len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            if (len < 1e-6f)
                continue;
            const float k[3] = { ax[0] / len, ax[1] / len, ax[2] / len };
            const float piv[3] = { pj[3], pj[7], pj[11] };
            // Rodrigues matrix about k.
            float R[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    R[i][j] = (i == j ? ca : 0.f) + k[i] * k[j] * (1.f - ca);
            R[0][1] -= k[2] * sa; R[0][2] += k[1] * sa;
            R[1][0] += k[2] * sa; R[1][2] -= k[0] * sa;
            R[2][0] -= k[1] * sa; R[2][1] += k[0] * sa;
            for (int j = seg; j < 3; ++j)
            {
                float* m = rel[j];
                float out[12];
                for (int r = 0; r < 3; ++r)
                {
                    for (int c = 0; c < 3; ++c)
                        out[r * 4 + c] = R[r][0] * m[0 * 4 + c] + R[r][1] * m[1 * 4 + c] + R[r][2] * m[2 * 4 + c];
                    const float d[3] = { m[3] - piv[0], m[7] - piv[1], m[11] - piv[2] };
                    out[r * 4 + 3] = piv[r] + R[r][0] * d[0] + R[r][1] * d[1] + R[r][2] * d[2];
                }
                std::memcpy(m, out, sizeof(out));
            }
        }
    }

    bool BuildStockPosePalette(const VrHandMeshAsset& asset, HandRig& rig, const float rel[15][12],
        std::vector<VrHandMatrixRows3x4>& outPalette)
    {
        if (!rig.built)
            BuildHandRig(asset, rig);
        const size_t n = asset.jointNames.size();
        if (!rig.valid || rig.rightHand || asset.sourceBoneBind.size() != n)
            return false;
        const int wrist = FindNameIndex(asset.jointNames, "wrist_l");
        if (wrist < 0)
            return false;
        auto fromRows = [](const float* r) {
            VrHandMatrix4 m = VrHandMath::Identity();
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col)
                    VrHandMath::Set(m, row, col, r[row * 4 + col]);
            return m;
        };
        const VrHandMatrix4& gw = asset.sourceBoneBind[static_cast<size_t>(wrist)];
        outPalette.resize(n);
        const VrHandMatrixRows3x4 identity = VrHandMath::ToRows3x4(VrHandMath::Identity());
        for (size_t j = 0; j < n; ++j)
            outPalette[j] = identity;
        for (int finger = 0; finger < 5; ++finger)
        {
            VrHandMatrix4 parentPosed = gw;            // glove space
            VrHandMatrix4 parentBind = gw;
            VrHandMatrix4 parentRel = VrHandMath::Identity(); // viewmodel, wrist-relative
            for (int seg = 0; seg < 3; ++seg)
            {
                const int joint = rig.fingerJoint[finger][seg];
                if (joint < 0 || static_cast<size_t>(joint) >= n)
                    return false;
                const VrHandMatrix4 relB = fromRows(rel[finger * 3 + seg]);
                VrHandMatrix4 parentRelInv, parentBindInv, bindInv;
                const VrHandMatrix4& bind = asset.sourceBoneBind[static_cast<size_t>(joint)];
                if (!VrHandMath::Invert4x4(parentRel, parentRelInv) || !VrHandMath::Invert4x4(parentBind, parentBindInv)
                    || !VrHandMath::Invert4x4(bind, bindInv))
                    return false;
                VrHandMatrix4 local = VrHandMath::Multiply(parentRelInv, relB);
                // Rotation, normalised per column (viewmodel bones can carry scale).
                for (int col = 0; col < 3; ++col)
                {
                    float len = 0.f;
                    for (int row = 0; row < 3; ++row)
                        len += VrHandMath::Get(local, row, col) * VrHandMath::Get(local, row, col);
                    len = std::sqrt(len);
                    if (len < 1e-6f)
                        return false;
                    for (int row = 0; row < 3; ++row)
                        VrHandMath::Set(local, row, col, VrHandMath::Get(local, row, col) / len);
                }
                const VrHandMatrix4 bindLocal = VrHandMath::Multiply(parentBindInv, bind);
                for (int row = 0; row < 3; ++row)
                    VrHandMath::Set(local, row, 3, VrHandMath::Get(bindLocal, row, 3));
                const VrHandMatrix4 posed = VrHandMath::Multiply(parentPosed, local);
                outPalette[static_cast<size_t>(joint)] = VrHandMath::ToRows3x4(VrHandMath::Multiply(posed, bindInv));
                parentPosed = posed;
                parentBind = bind;
                parentRel = relB;
            }
        }
        // The curl palette cache must rebuild once the stock pose ends.
        rig.paletteValid = false;
        return true;
    }

    // Pinch (VrStockHandPose): thumb and index meet, the rest tuck in. For
    // the MP5 charging handle, the glock slide and the grenade pin ring.
    bool LeftGlovePinch()
    {
        if (!bmvr::g_VrStockHandPose || bmvr::g_LeftHanded)
            return false;
        if (bmvr::g_VrPinchForce)
            return true;
        if (MagReload::g_BoltGrab.load(std::memory_order_relaxed))
        {
            const int w = MagReload::g_Weapon.load(std::memory_order_relaxed);
            if (w == MagReload::kWeaponMp5 || w == MagReload::kWeaponGlock)
                return true;
        }
        // A shotgun shell in the hand, held by its base like the stock
        // shell reload (user, 2026-11-18).
        if (MagReload::g_Weapon.load(std::memory_order_relaxed) == MagReload::kWeaponShotgun
            && MagReload::g_State.load(std::memory_order_relaxed) == MagReload::kInHand)
            return true;
        if (MagReload::g_PinWeapon.load(std::memory_order_relaxed))
        {
            const int s = MagReload::g_PinState.load(std::memory_order_relaxed);
            return s == MagReload::kPinHeld || s == MagReload::kPinPulledInHand;
        }
        return false;
    }
    constexpr float kPinchRest[3] = { 0.80f, 0.90f, 0.95f }; // middle, ring, pinky

    void BuildBindPosePalette(const VrHandMeshAsset& asset, std::vector<VrHandMatrixRows3x4>& outPalette)
    {
        outPalette.resize(asset.jointNames.size());
        const VrHandMatrixRows3x4 identity = VrHandMath::ToRows3x4(VrHandMath::Identity());
        for (size_t i = 0; i < outPalette.size(); ++i)
            outPalette[i] = identity;
    }

    // BuildSummaryCurlPalette only reads flFingerCurl: VR::GetFingerCurls.
    bool TryGetSummary(vr::VRActionHandle_t action, vr::VRSkeletalSummaryData_t& out)
    {
        out = {};
        if (g_Game && g_Game->m_VR)
        {
            float curls[5]{};
            if (g_Game->m_VR->GetFingerCurls(action, curls))
            {
                for (int i = 0; i < vr::VRFinger_Count && i < 5; ++i)
                    out.flFingerCurl[i] = curls[i];
                return true;
            }
        }
        return false;
    }

    Vector FlashlightBoostAt(const Vector& palm)
    {
        Vector add(0.f, 0.f, 0.f);
        if (!g_Game || !g_Game->m_VR)
            return add;
        Vector origin{};
        Vector forward{};
        if (!g_Game->m_VR->CopyFlashlightState(origin, forward))
            return add;
        Vector delta = palm - origin;
        const float dist = delta.Length();
        if (!(dist > 1.f) || !(dist < 2500.f))
            return add;
        delta = delta * (1.f / dist);
        const float spot = delta.Dot(forward);
        if (spot < 0.35f)
            return add;
        const float cone = (spot - 0.35f) / 0.65f;
        const float atten = 750.f / (750.f + dist);
        const float beam = cone * atten * 0.85f;
        add.x = beam;
        add.y = beam;
        add.z = beam * 0.82f;
        return add;
    }

    // GetLightForPoint goes through the engine's light cache, which is not
    // thread-safe: called from the material thread (multicore) it raced the
    // game thread and left a cycle in a cache bucket — engine.dll spun
    // forever in the lookup (train intro, 2026-10-15). The game thread fills
    // this cache (VR::TickMatQueueFromRenderView); drawing only reads it.
    struct PalmLightEntry { Vector pos{}; Vector rgb{}; bool valid = false; };
    std::mutex g_PalmLightMutex;
    PalmLightEntry g_PalmLight[2];

    bool CachedPalmLight(const Vector& palm, Vector& rgb)
    {
        std::lock_guard<std::mutex> lock(g_PalmLightMutex);
        int best = -1;
        float bestD = 48.f * 48.f;   // within ~1.2 m of a sampled hand
        for (int i = 0; i < 2; ++i)
        {
            if (!g_PalmLight[i].valid)
                continue;
            const float d = (g_PalmLight[i].pos - palm).LengthSqr();
            if (d < bestD)
            {
                bestD = d;
                best = i;
            }
        }
        if (best < 0)
            return false;
        rgb = g_PalmLight[best].rgb;
        return true;
    }

    Vector SamplePalmSceneLight(const Vector& palm, float& outEnvExposure)
    {
        // Diffuse uses a high floor so indoor gloves stay readable.
        // Spec/cubemap uses raw luma so dark rooms do not stay chrome.
        outEnvExposure = 0.45f;
        Vector rgb(1.f, 1.f, 1.f);
        if (bmvr::g_VrHandLightRaw)
        {
            // A/B: the light the game reports at the palm, unchanged (0..1 scale),
            // plus the flashlight; no floor, lift or tint.
            Vector raw(0.f, 0.f, 0.f);
            if (CachedPalmLight(palm, raw))
            {
                if (raw.x > 1.5f || raw.y > 1.5f || raw.z > 1.5f)
                {
                    raw.x *= (1.f / 255.f);
                    raw.y *= (1.f / 255.f);
                    raw.z *= (1.f / 255.f);
                }
                rgb = Vector(std::max(0.f, raw.x), std::max(0.f, raw.y), std::max(0.f, raw.z));
            }
            const float luma = 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
            outEnvExposure = std::clamp(luma, 0.03f, 1.35f);
            const Vector beam = FlashlightBoostAt(palm);
            rgb.x = std::clamp(rgb.x + beam.x * 0.28f, 0.f, 1.28f);
            rgb.y = std::clamp(rgb.y + beam.y * 0.28f, 0.f, 1.28f);
            rgb.z = std::clamp(rgb.z + beam.z * 0.28f, 0.f, 1.28f);
            outEnvExposure = std::clamp(outEnvExposure + beam.x * 0.55f, 0.03f, 1.5f);
            return rgb;
        }
        if (CachedPalmLight(palm, rgb))
        {
            if (rgb.x > 1.5f || rgb.y > 1.5f || rgb.z > 1.5f)
            {
                rgb.x *= (1.f / 255.f);
                rgb.y *= (1.f / 255.f);
                rgb.z *= (1.f / 255.f);
            }
            rgb.x = std::max(0.f, rgb.x);
            rgb.y = std::max(0.f, rgb.y);
            rgb.z = std::max(0.f, rgb.z);
            const float luma = 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
            outEnvExposure = std::clamp(luma, 0.03f, 1.35f);
            const float safe = std::max(luma, 0.0001f);
            const float lift = 0.18f + 0.92f * (luma / (luma + 0.22f));
            const float tx = rgb.x / safe;
            const float ty = rgb.y / safe;
            const float tz = rgb.z / safe;
            constexpr float kTint = 0.28f;
            rgb.x = lift * (1.f - kTint + kTint * tx);
            rgb.y = lift * (1.f - kTint + kTint * ty);
            rgb.z = lift * (1.f - kTint + kTint * tz);
        }
        const Vector beam = FlashlightBoostAt(palm);
        rgb.x += beam.x * 0.28f;
        rgb.y += beam.y * 0.28f;
        rgb.z += beam.z * 0.28f;
        outEnvExposure = std::clamp(outEnvExposure + beam.x * 0.55f, 0.03f, 1.5f);
        rgb.x = std::clamp(rgb.x, 0.06f, 1.28f);
        rgb.y = std::clamp(rgb.y, 0.06f, 1.28f);
        rgb.z = std::clamp(rgb.z, 0.06f, 1.28f);
        return rgb;
    }
}

struct BmVrGloves::Impl
{
    struct Hand
    {
        const char* fileName = nullptr;
        VrHandMeshAsset asset;
        std::vector<VrHandMatrixRows3x4> palette;
        HandRig rig;
        // Palm scene light is sampled per hand per eye; both eyes of a frame
        // ask for the same palm position.
        bool lightValid = false;
        uint32_t lightTick = 0;
        Vector lightPalm{};
        Vector lightRgb{};
        float lightExposure = 0.45f;
    };

    Hand hev[2];
    Hand bare[2];
    // HEV cuff caps drawn over the gun's own hands (BmHands::GunCuffCap).
    VrHandMeshAsset gunCap[2];
    bool gunCapLoaded[2] = { false, false };
    VrHandRendererD3D9 renderer;
    bool assetAttempted = false;
    bool assetsLoaded = false;
    bool hevLoaded = false;
    bool bareLoaded = false;
    bool unavailable = false;
    bool loggedReady = false;
    std::string failure;
    // What the loaded assets were built from, so the panel can switch live.
    bool builtBlackMesa = false;
    float builtBlackMesaScale = 0.f;

    void Fail(const std::string& reason)
    {
        unavailable = true;
        failure = reason;
        Game::logMsg("VR gloves fail: %s", reason.c_str());
    }

    bool ResolveSteamVrAssetPath(const char* fileName, std::string& outPath) const
    {
        outPath.clear();
        auto tryRoot = [&](const std::filesystem::path& root) -> bool {
            if (root.empty())
                return false;
            const std::filesystem::path path = root / "resources" / "rendermodels" / "vr_glove" / fileName;
            if (!std::filesystem::exists(path))
                return false;
            outPath = path.string();
            return true;
        };

        static const char* kFallbacks[] = {
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR",
            "C:\\Program Files\\Steam\\steamapps\\common\\SteamVR",
        };
        for (const char* root : kFallbacks)
        {
            if (tryRoot(root))
                return true;
        }
        return false;
    }

    bool ResolveLocalGlb(const char* fileName, std::string& outPath) const
    {
        outPath.clear();
        auto tryPath = [&](const std::filesystem::path& path) -> bool {
            if (path.empty() || !std::filesystem::exists(path))
                return false;
            outPath = path.string();
            return true;
        };

        wchar_t exeBuf[MAX_PATH]{};
        wchar_t dllBuf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        HMODULE mod = bmvr::DllModule();
        if (mod)
            GetModuleFileNameW(mod, dllBuf, MAX_PATH);

        const std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
        const std::filesystem::path dllDir = dllBuf[0] ? std::filesystem::path(dllBuf).parent_path() : std::filesystem::path();
        const std::filesystem::path names[] = {
            exeDir / "VR" / "hands" / fileName,
            dllDir / "VR" / "hands" / fileName,
            exeDir / "VR" / fileName,
            dllDir / fileName,
        };
        for (const auto& path : names)
        {
            if (tryPath(path))
                return true;
        }
        return false;
    }

    void ResetAssets()
    {
        for (Hand* h : { &hev[0], &hev[1], &bare[0], &bare[1] })
        {
            h->asset = VrHandMeshAsset{};
            h->palette.clear();
            h->rig = HandRig{};
        }
        assetAttempted = assetsLoaded = hevLoaded = bareLoaded = unavailable = false;
        loggedReady = false;
        failure.clear();
    }

    // Black Mesa's own hands, built from the installed game files.
    bool LoadBlackMesaHands(float scale)
    {
        std::string error;
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i)
            ok = BmHands::Build(BmHands::Variant::Hev, i, scale, hev[i].asset, error);
        for (int i = 0; i < 2 && ok; ++i)
            ok = BmHands::Build(BmHands::Variant::Bare, i, scale, bare[i].asset, error);
        if (!ok)
        {
            Game::logMsg("VR Black Mesa hands failed (%s); using the glove files", error.c_str());
            for (Hand* h : { &hev[0], &hev[1], &bare[0], &bare[1] })
                h->asset = VrHandMeshAsset{};
            return false;
        }
        for (Hand* h : { &hev[0], &hev[1], &bare[0], &bare[1] })
            BuildBindPosePalette(h->asset, h->palette);
        // Mesh size per glove (user 2026-11-19: right glove looked bigger).
        for (Hand* h : { &hev[0], &hev[1], &bare[0], &bare[1] })
        {
            float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
            for (const auto& v : h->asset.vertices)
                for (int k = 0; k < 3; ++k)
                {
                    lo[k] = (std::min)(lo[k], v.position[k]);
                    hi[k] = (std::max)(hi[k], v.position[k]);
                }
            Game::logMsg("VR glove %s mesh size x %.3f y %.3f z %.3f m (z from %.3f)", h->asset.sourcePath.c_str(),
                hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], lo[2]);
        }
        hev[0].fileName = "bm:hev_left";
        hev[1].fileName = "bm:hev_right";
        bare[0].fileName = "bm:bare_left";
        bare[1].fileName = "bm:bare_right";
        hevLoaded = bareLoaded = true;
        Game::logMsg("VR gloves loaded source=blackmesa scale=%.2f hev=%u/%u bare=%u/%u verts",
            scale,
            static_cast<unsigned>(hev[0].asset.vertices.size()), static_cast<unsigned>(hev[1].asset.vertices.size()),
            static_cast<unsigned>(bare[0].asset.vertices.size()), static_cast<unsigned>(bare[1].asset.vertices.size()));
        return true;
    }

    bool EnsureAssets()
    {
        const bool wantBm = bmvr::g_VrHandsBlackMesa;
        const float wantScale = std::clamp(bmvr::g_VrHandsBlackMesaScale, 0.4f, 1.5f);
        if (assetAttempted && (wantBm != builtBlackMesa || (wantBm && std::fabs(wantScale - builtBlackMesaScale) > 1e-4f)))
        {
            Game::logMsg("VR gloves: source changed (Black Mesa hands %d, size %.2f), reloading", wantBm ? 1 : 0, wantScale);
            ResetAssets();
        }
        if (unavailable)
            return false;
        if (assetsLoaded)
            return true;
        if (assetAttempted)
            return false;
        assetAttempted = true;
        builtBlackMesa = wantBm;
        builtBlackMesaScale = wantScale;
        if (wantBm && LoadBlackMesaHands(wantScale))
        {
            assetsLoaded = true;
            return true;
        }

        const bool wantHev = bmvr::g_VrHandsUseHevGloves;
        const char* hevNames[2] = { "hev_glove_left_model.glb", "hev_glove_right_model.glb" };
        const char* steamNames[2] = { "vr_glove_left_model.glb", "vr_glove_right_model.glb" };
        const char* bareNames[2] = { "bare_hand_left_model.glb", "bare_hand_right_model.glb" };
        bool usingHev = false;
        if (wantHev)
        {
            std::string leftPath, rightPath;
            usingHev = ResolveLocalGlb(hevNames[0], leftPath) && ResolveLocalGlb(hevNames[1], rightPath);
        }

        auto loadPair = [&](Hand* dest, const char* leftName, const char* rightName, bool local) -> bool {
            dest[0].fileName = leftName;
            dest[1].fileName = rightName;
            for (int i = 0; i < 2; ++i)
            {
                Hand& hand = dest[i];
                std::string path;
                const bool found = local
                    ? ResolveLocalGlb(hand.fileName, path)
                    : ResolveSteamVrAssetPath(hand.fileName, path);
                if (!found)
                    return false;
                std::string error;
                if (!VrHandAssetLoader::LoadGlb(path, hand.asset, error))
                {
                    Game::logMsg("VR glove load failed %s: %s", path.c_str(), error.c_str());
                    return false;
                }
                BuildBindPosePalette(hand.asset, hand.palette);
            }
            return true;
        };

        if (usingHev && loadPair(hev, hevNames[0], hevNames[1], true))
        {
            hevLoaded = true;
            Game::logMsg(
                "VR gloves loaded source=hev left=%u verts right=%u verts",
                static_cast<unsigned>(hev[0].asset.vertices.size()),
                static_cast<unsigned>(hev[1].asset.vertices.size()));
        }
        else
        {
            if (usingHev)
                Game::logMsg("HEV gloves missing or invalid, falling back to SteamVR GLB");
            if (loadPair(hev, steamNames[0], steamNames[1], false))
            {
                hevLoaded = true;
                Game::logMsg(
                    "VR gloves loaded source=steamvr left=%u verts right=%u verts",
                    static_cast<unsigned>(hev[0].asset.vertices.size()),
                    static_cast<unsigned>(hev[1].asset.vertices.size()));
            }
        }

        if (loadPair(bare, bareNames[0], bareNames[1], true))
        {
            bareLoaded = true;
            Game::logMsg(
                "VR bare hands loaded left=%u verts right=%u verts",
                static_cast<unsigned>(bare[0].asset.vertices.size()),
                static_cast<unsigned>(bare[1].asset.vertices.size()));
        }
        else
            Game::logMsg("VR bare hands missing; intro will hide hands until HEV suit");

        if (!hevLoaded && !bareLoaded)
        {
            Fail("missing SteamVR glove assets");
            return false;
        }

        assetsLoaded = true;
        return true;
    }
};

BmVrGloves::BmVrGloves()
    : m_Impl(std::make_unique<Impl>())
{
}

BmVrGloves::~BmVrGloves() = default;

void BmVrGloves::OnDeviceLost()
{
    if (m_Impl)
        m_Impl->renderer.OnDeviceLost();
}

bool BmVrGloves::AssetsReady() const
{
    return m_Impl && m_Impl->assetsLoaded;
}

bool BmVrGloves::HasBareHands() const
{
    return m_Impl && m_Impl->bareLoaded;
}

bool BmVrGloves::Failed() const
{
    return m_Impl && m_Impl->unavailable;
}

const std::string& BmVrGloves::FailureReason() const
{
    static const std::string kEmpty;
    return m_Impl ? m_Impl->failure : kEmpty;
}

bool BmVrGloves::WarmupGpu(IDirect3DDevice9* device)
{
    if (!device || !m_Impl || !m_Impl->EnsureAssets())
        return false;
    auto warmup = [&](int gpuIndex, Impl::Hand& hand, const char* label) {
        std::string error;
        if (m_Impl->renderer.EnsureHandMesh(device, gpuIndex, hand.asset, error))
            return true;
        if (!error.empty())
        {
            static int s_warmLog;
            if (s_warmLog < 8)
            {
                Game::logMsg("VR glove warmup %s: %s", label, error.c_str());
                ++s_warmLog;
            }
        }
        return false;
    };
    bool ok = false;
    if (m_Impl->hevLoaded)
    {
        ok |= warmup(0, m_Impl->hev[0], "hev-left");
        warmup(1, m_Impl->hev[1], "hev-right");
    }
    if (m_Impl->bareLoaded)
    {
        ok |= warmup(2, m_Impl->bare[0], "bare-left");
        warmup(3, m_Impl->bare[1], "bare-right");
    }
    return ok;
}

namespace
{
    // Centre of the drawn left fist for the magazine in the hand
    // (MagReload::g_LeftPalmLocal): the mean of the wrist and the middle
    // finger's three joints as posed (curled around a held magazine, they
    // ring the inside of the fist), in the controller frame the viewmodel
    // draw uses (OffhandWorldPoseForDraw).
    // VrClipHandPose: Black Mesa's L_Wrist frame on the drawn left glove
    // (MagReload::g_LeftClipWrist, controller frame) while it shows a
    // reload-clip pose. BuildStockPosePalette leaves the wrist joint at
    // identity, so the frame is world * the glove's source wrist bone.
    // Units: the source bones are Black Mesa's in glove metres (x size), so
    // the wrist -> middle knuckle distance against Black Mesa's own (5.894
    // model units, rigid in every clip frame measured) gives game units per
    // model unit at the glove's size.
    void StoreLeftClipWrist(const VrHandMeshAsset& asset, const VrHandMatrix4& world, const Vector& controller,
        const QAngle& controllerAng, int clip)
    {
        if (clip < 0)
            return;
        static const VrHandMeshAsset* s_asset = nullptr;
        static int s_wrist = -1, s_middle = -1;
        if (s_asset != &asset)
        {
            s_asset = &asset;
            s_wrist = FindNameIndex(asset.jointNames, "wrist_l");
            s_middle = FindNameIndex(asset.jointNames, "finger_middle_0_l");
        }
        const size_t n = asset.sourceBoneBind.size();
        if (s_wrist < 0 || s_middle < 0 || static_cast<size_t>(s_wrist) >= n || static_cast<size_t>(s_middle) >= n)
            return;
        const VrHandMatrix4 w = VrHandMath::Multiply(world, asset.sourceBoneBind[static_cast<size_t>(s_wrist)]);
        const VrHandMatrix4 mid = VrHandMath::Multiply(world, asset.sourceBoneBind[static_cast<size_t>(s_middle)]);
        const Vector wo(VrHandMath::Get(w, 0, 3), VrHandMath::Get(w, 1, 3), VrHandMath::Get(w, 2, 3));
        const Vector mo(VrHandMath::Get(mid, 0, 3), VrHandMath::Get(mid, 1, 3), VrHandMath::Get(mid, 2, 3));
        constexpr float kBmWristToMiddle = 5.894f;
        const float unit = (mo - wo).Length() / kBmWristToMiddle;
        if (!(unit > 1e-4f))
            return;
        Vector f, r, u;
        QAngle::AngleVectors(controllerAng, &f, &r, &u);
        const Vector d = wo - controller;
        float out[12];
        for (int col = 0; col < 3; ++col)
        {
            Vector a(VrHandMath::Get(w, 0, col), VrHandMath::Get(w, 1, col), VrHandMath::Get(w, 2, col));
            const float len = a.Length();
            if (len < 1e-6f)
                return;
            a = a * (1.f / len);
            out[0 * 4 + col] = a.Dot(f);
            out[1 * 4 + col] = a.Dot(r);
            out[2 * 4 + col] = a.Dot(u);
        }
        out[3] = d.Dot(f);
        out[7] = d.Dot(r);
        out[11] = d.Dot(u);
        for (int k = 0; k < 12; ++k)
            MagReload::g_LeftClipWrist[k].store(out[k], std::memory_order_relaxed);
        MagReload::g_LeftClipUnit.store(unit, std::memory_order_relaxed);
        MagReload::g_LeftClipPose.store(clip, std::memory_order_relaxed);
        const DWORD now = GetTickCount();
        MagReload::g_LeftClipMs.store(now ? now : 1, std::memory_order_relaxed);
        static DWORD s_logMs = 0;
        if (now - s_logMs > 5000)
        {
            s_logMs = now;
            Game::logMsg("Clip hand pose: %s wrist %.1f fwd %.1f right %.1f up of the controller, %.3f units per model unit",
                ClipHandPose::kPoses[clip].name, out[3], out[7], out[11], unit);
        }
    }

    // A glove joint's world position from the skinning palette and the glove's world matrix.
    bool GloveJointWorld(const VrHandMeshAsset& asset, const std::vector<VrHandMatrixRows3x4>& palette,
        const VrHandMatrix4& world, int j, Vector& out)
    {
        if (j < 0 || static_cast<size_t>(j) >= palette.size() || static_cast<size_t>(j) >= asset.bindMatrices.size())
            return false;
        const VrHandMatrix4& bind = asset.bindMatrices[static_cast<size_t>(j)];
        const float b[3] = { VrHandMath::Get(bind, 0, 3), VrHandMath::Get(bind, 1, 3), VrHandMath::Get(bind, 2, 3) };
        const auto& r = palette[static_cast<size_t>(j)].v;
        float m[3];
        for (int row = 0; row < 3; ++row)
            m[row] = r[row * 4 + 0] * b[0] + r[row * 4 + 1] * b[1] + r[row * 4 + 2] * b[2] + r[row * 4 + 3];
        float w[3];
        for (int row = 0; row < 3; ++row)
            w[row] = VrHandMath::Get(world, row, 0) * m[0] + VrHandMath::Get(world, row, 1) * m[1]
                + VrHandMath::Get(world, row, 2) * m[2] + VrHandMath::Get(world, row, 3);
        out = Vector(w[0], w[1], w[2]);
        return true;
    }

    // LadderHandWrap: turn and move the glove's world matrix so its fist closes
    // round the rung. The knuckle line (pinky -> index knuckle) is the axis of
    // the tunnel through a closed fist: the smallest rotation lays it along
    // the rung (thumb side = axis), which keeps the controller's twist about
    // the rung (the wrist tipping up or down). The centre of the middle
    // finger's curl (circle through its three joints) lands on the rung.
    void WrapGloveOnRung(const VrHandMeshAsset& asset, const std::vector<VrHandMatrixRows3x4>& palette,
        VrHandMatrix4& world, bool rightHand, const Vector& rung, const Vector& axis)
    {
        const char* suf = rightHand ? "r" : "l";
        auto idx = [&](const char* base) { return FindNameIndex(asset.jointNames, std::string(base) + suf); };
        static const VrHandMeshAsset* s_asset[2] = { nullptr, nullptr };
        static int s_j[2][5];
        const int h = rightHand ? 1 : 0;
        if (s_asset[h] != &asset)
        {
            s_asset[h] = &asset;
            s_j[h][0] = idx("finger_index_0_");
            s_j[h][1] = idx("finger_pinky_0_");
            s_j[h][2] = idx("finger_middle_0_");
            s_j[h][3] = idx("finger_middle_1_");
            s_j[h][4] = idx("finger_middle_2_");
        }
        Vector p[5];
        for (int k = 0; k < 5; ++k)
            if (!GloveJointWorld(asset, palette, world, s_j[h][k], p[k]))
                return;
        Vector a = p[0] - p[1];
        if (VectorNormalize(a) < 1e-3f)
            return;
        // Curl centre: circumcentre of the middle finger's joints.
        Vector c = (p[2] + p[3] + p[4]) * (1.f / 3.f);
        {
            const Vector u = p[3] - p[2], v = p[4] - p[2];
            const Vector n = CrossProduct(u, v);
            const float n2 = n.Dot(n);
            if (n2 > 1e-4f)
                c = p[2] + (CrossProduct(n, u) * v.Dot(v) + CrossProduct(v, n) * u.Dot(u)) * (0.5f / n2);
            if ((c - p[3]).LengthSqr() > 6.f * 6.f)
                c = (p[2] + p[3] + p[4]) * (1.f / 3.f);   // nearly straight finger: no usable circle
        }
        // Rotation taking a onto axis (Rodrigues); opposite: half turn about
        // the palm normal-ish (a x (c - p[2])).
        float R[3][3];
        const Vector v = CrossProduct(a, axis);
        const float cs = a.Dot(axis);
        if (cs > -0.999f)
        {
            const float k = 1.f / (1.f + cs);
            const float r[3][3] = {
                { 1.f - (v.y * v.y + v.z * v.z) * k, -v.z + v.x * v.y * k, v.y + v.x * v.z * k },
                { v.z + v.x * v.y * k, 1.f - (v.x * v.x + v.z * v.z) * k, -v.x + v.y * v.z * k },
                { -v.y + v.x * v.z * k, v.x + v.y * v.z * k, 1.f - (v.x * v.x + v.y * v.y) * k } };
            std::memcpy(R, r, sizeof(R));
        }
        else
        {
            Vector q = CrossProduct(a, p[3] - p[2]);
            if (VectorNormalize(q) < 1e-3f)
                return;
            const float qq[3] = { q.x, q.y, q.z };
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    R[i][j] = 2.f * qq[i] * qq[j] - (i == j ? 1.f : 0.f);
        }
        float M[3][4];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j)
                M[i][j] = VrHandMath::Get(world, i, j);
        const float t[3] = { M[0][3] - c.x, M[1][3] - c.y, M[2][3] - c.z };
        const float dst[3] = { rung.x, rung.y, rung.z };
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                VrHandMath::Set(world, i, j, R[i][0] * M[0][j] + R[i][1] * M[1][j] + R[i][2] * M[2][j]);
            VrHandMath::Set(world, i, 3, R[i][0] * t[0] + R[i][1] * t[1] + R[i][2] * t[2] + dst[i]);
        }
        static DWORD s_logMs[2] = { 0, 0 };
        const DWORD now = GetTickCount();
        if (now - s_logMs[h] > 2000)
        {
            s_logMs[h] = now;
            Game::logMsg("Ladder hand wrap: %s glove turned %.0f deg onto the rung, curl centre moved %.1f units",
                rightHand ? "right" : "left", std::acos(std::clamp(cs, -1.f, 1.f)) * 57.2958f, (rung - c).Length());
        }
    }

    void StoreLeftFistCentre(const VrHandMeshAsset& asset, const std::vector<VrHandMatrixRows3x4>& palette,
        const VrHandMatrix4& world, const Vector& controller, const QAngle& controllerAng)
    {
        static const VrHandMeshAsset* s_asset = nullptr;
        static int s_joints[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        if (s_asset != &asset)
        {
            s_asset = &asset;
            const char* names[8] = { "wrist_l", "finger_middle_0_l", "finger_middle_1_l", "finger_middle_2_l",
                "finger_thumb_2_l", "finger_index_2_l", "finger_index_0_l", "finger_pinky_0_l" };
            for (int k = 0; k < 8; ++k)
                s_joints[k] = FindNameIndex(asset.jointNames, names[k]);
        }
        auto jointWorld = [&](int j, Vector& out) {
            if (j < 0 || static_cast<size_t>(j) >= palette.size() || static_cast<size_t>(j) >= asset.bindMatrices.size())
                return false;
            const VrHandMatrix4& bind = asset.bindMatrices[static_cast<size_t>(j)];
            const float b[3] = { VrHandMath::Get(bind, 0, 3), VrHandMath::Get(bind, 1, 3), VrHandMath::Get(bind, 2, 3) };
            const auto& r = palette[static_cast<size_t>(j)].v;
            float m[3];
            for (int row = 0; row < 3; ++row)
                m[row] = r[row * 4 + 0] * b[0] + r[row * 4 + 1] * b[1] + r[row * 4 + 2] * b[2] + r[row * 4 + 3];
            float w[3];
            for (int row = 0; row < 3; ++row)
                w[row] = VrHandMath::Get(world, row, 0) * m[0] + VrHandMath::Get(world, row, 1) * m[1]
                    + VrHandMath::Get(world, row, 2) * m[2] + VrHandMath::Get(world, row, 3);
            out = Vector(w[0], w[1], w[2]);
            return true;
        };
        Vector sum(0.f, 0.f, 0.f);
        for (int k = 0; k < 4; ++k)
        {
            Vector p;
            if (!jointWorld(s_joints[k], p))
                return;
            sum = sum + p;
        }
        const Vector centre = sum * 0.25f;
        Vector f, r, u;
        QAngle::AngleVectors(controllerAng, &f, &r, &u);
        Vector thumbP, indexP;
        if (jointWorld(s_joints[4], thumbP) && jointWorld(s_joints[5], indexP))
        {
            const Vector dp = (thumbP + indexP) * 0.5f - controller;
            MagReload::g_LeftPinchLocal[0].store(dp.Dot(f), std::memory_order_relaxed);
            MagReload::g_LeftPinchLocal[1].store(dp.Dot(r), std::memory_order_relaxed);
            MagReload::g_LeftPinchLocal[2].store(dp.Dot(u), std::memory_order_relaxed);
            const DWORD t = GetTickCount();
            MagReload::g_LeftPinchMs.store(t ? t : 1, std::memory_order_relaxed);
        }
        // Fist tunnel (what a closed fist holds a rod through): along the
        // knuckle line pinky -> index, through the circumcentre of the middle
        // finger's joints (as WrapGloveOnRung). Holds the RPG rocket.
        {
            Vector ip, pp, m0, m1, m2;
            if (jointWorld(s_joints[6], ip) && jointWorld(s_joints[7], pp) && jointWorld(s_joints[1], m0)
                && jointWorld(s_joints[2], m1) && jointWorld(s_joints[3], m2))
            {
                Vector a = ip - pp;
                if (VectorNormalize(a) > 1e-3f)
                {
                    Vector c = (m0 + m1 + m2) * (1.f / 3.f);
                    const Vector uu = m1 - m0, vv = m2 - m0;
                    const Vector n = CrossProduct(uu, vv);
                    const float n2 = n.Dot(n);
                    if (n2 > 1e-4f)
                        c = m0 + (CrossProduct(n, uu) * vv.Dot(vv) + CrossProduct(vv, n) * uu.Dot(uu)) * (0.5f / n2);
                    const Vector dc = c - controller;
                    MagReload::g_LeftTunnelLocal[0].store(dc.Dot(f), std::memory_order_relaxed);
                    MagReload::g_LeftTunnelLocal[1].store(dc.Dot(r), std::memory_order_relaxed);
                    MagReload::g_LeftTunnelLocal[2].store(dc.Dot(u), std::memory_order_relaxed);
                    MagReload::g_LeftTunnelLocal[3].store(a.Dot(f), std::memory_order_relaxed);
                    MagReload::g_LeftTunnelLocal[4].store(a.Dot(r), std::memory_order_relaxed);
                    MagReload::g_LeftTunnelLocal[5].store(a.Dot(u), std::memory_order_relaxed);
                    const DWORD t = GetTickCount();
                    MagReload::g_LeftTunnelMs.store(t ? t : 1, std::memory_order_relaxed);
                }
            }
        }
        const Vector d = centre - controller;
        MagReload::g_LeftPalmLocal[0].store(d.Dot(f), std::memory_order_relaxed);
        MagReload::g_LeftPalmLocal[1].store(d.Dot(r), std::memory_order_relaxed);
        MagReload::g_LeftPalmLocal[2].store(d.Dot(u), std::memory_order_relaxed);
        const DWORD now = GetTickCount();
        MagReload::g_LeftPalmMs.store(now ? now : 1, std::memory_order_relaxed);
        static DWORD s_logMs = 0;
        if (now - s_logMs > 5000)
        {
            s_logMs = now;
            Game::logMsg("Mag reload: left fist centre %.1f fwd %.1f right %.1f up of the controller",
                d.Dot(f), d.Dot(r), d.Dot(u));
        }
    }
}

bool BmVrGloves::DrawForEye(
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
    bool overlayNoDepth)
{
    if (!device || !m_Impl || !m_Impl->EnsureAssets())
        return false;

    const float scale = std::clamp(modelScale, 0.2f, 2.0f);
    const float nearPlane = (zNear > 0.01f) ? zNear : kEyeZNear;
    const float farPlane = (zFar > nearPlane + 1.f) ? zFar : kEyeZFar;
    VrHandMatrix4 projection = VrHandMath::BuildPerspective(
        horizontalFovDegrees, aspectRatio, nearPlane, farPlane);
    // OffCenterEyes: clip' = clip * s + w * o on x/y (union NDC -> eye NDC).
    float ocSx = 1.f, ocOx = 0.f, ocSy = 1.f, ocOy = 0.f;
    if (g_Game && g_Game->m_VR && g_Game->m_VR->OffCenterNdc(stereoEye, ocSx, ocOx, ocSy, ocOy))
    {
        for (int c = 0; c < 4; ++c)
        {
            const float w = VrHandMath::Get(projection, 3, c);
            VrHandMath::Set(projection, 0, c, VrHandMath::Get(projection, 0, c) * ocSx + w * ocOx);
            VrHandMath::Set(projection, 1, c, VrHandMath::Get(projection, 1, c) * ocSy + w * ocOy);
        }
    }
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(eyeOrigin, viewAngles);

    bool drew = false;
    const vr::VRActionHandle_t actions[2] = { leftSkeleton, rightSkeleton };
    const bool valid[2] = { leftOk, rightOk };
    const Vector* origins[2] = { &leftWorld, &rightWorld };
    const QAngle* angles[2] = { &leftAngles, &rightAngles };

    const bool wearingSuit = g_Game && g_Game->m_VR && g_Game->m_VR->WearingHevSuit();
    Impl::Hand* pair = nullptr;
    int gpuBase = 0;
    const char* source = "hev";
    // Blue Shift / Azure Sheep: security uniform — never HEV gloves.
    const bool forceBare = bmvr::IsBlueShift() || bmvr::IsAzureSheep();
    if (m_Impl->bareLoaded && (forceBare || !wearingSuit))
    {
        pair = m_Impl->bare;
        gpuBase = 2;
        source = "bare";
    }
    else if (m_Impl->hevLoaded)
    {
        pair = m_Impl->hev;
        gpuBase = 0;
        source = "hev";
    }
    else if (m_Impl->bareLoaded)
    {
        pair = m_Impl->bare;
        gpuBase = 2;
        source = "bare";
    }
    else
        return false;

    for (int i = 0; i < 2; ++i)
    {
        const bool rightHand = (i == 1);
        const bool showRight = g_Game && g_Game->m_VR
            ? g_Game->m_VR->WantsRightGloveVisible()
            : bmvr::g_VrHandsRightEnabled;
        if (rightHand && !showRight)
            continue;
        if (!valid[i])
            continue;
        // The gun's own left hand is the support hand this frame (not with
        // empty hands: the right glove shows then and no gun is drawn).
        if (!rightHand)
        {
            const unsigned fromGun = MagReload::g_SupportHandFromGunMs.load(std::memory_order_relaxed);
            const bool emptyHands = g_Game && g_Game->m_VR && g_Game->m_VR->WantsRightGloveVisible();
            if (fromGun != 0 && GetTickCount() - fromGun < 150 && !emptyHands)
                continue;
        }

        Impl::Hand& hand = pair[i];
        vr::VRSkeletalSummaryData_t summary{};
        int drawnClip = ClipHandPose::kNone; // reload-clip hand pose drawn on this glove
        Vector rungPoint{}, rungAxis{};
        float rungCurl = bmvr::g_LadderGripCurl;
        const bool rungGrip = g_Game && g_Game->m_VR && g_Game->m_VR->LadderRungGrip(!rightHand, rungPoint, rungAxis, &rungCurl);
        const bool gripCurl = !rungGrip && g_Game && g_Game->m_VR && (
            (rightHand && g_Game->m_VR->WantsRightGloveWeaponGripCurl())
            || (!rightHand && g_Game->m_VR->WantsOffhandGloveWeaponGripCurl()));
        if (TryGetSummary(actions[i], summary))
        {
            // G-Man in view: grip folds every finger but the middle one.
            // Grip drives the middle/ring/pinky curls, so their max is the
            // grip pull; the gesture follows it in and out. The fingers fold
            // over the first half of the pull, then the thumb comes over
            // them: moved together, it passes through them on the way.
            float gmanThumb = 0.f;
            float straightMiddle = 0.f;
            if (!gripCurl && g_Game && g_Game->m_VR && (g_Game->m_VR->GmanGestureActive() || bmvr::g_VrGmanGestureForce))
            {
                const float grip = std::max({ summary.flFingerCurl[2], summary.flFingerCurl[3], summary.flFingerCurl[4] });
                if (grip > 0.05f)
                {
                    const float fold = std::min(1.f, grip / 0.5f);
                    gmanThumb = std::clamp((grip - 0.5f) / 0.3f, 0.f, 1.f);
                    summary.flFingerCurl[1] = fold;
                    // Negative curl = bent back past the rest pose's
                    // natural bend (BuildSummaryCurlPalette allows it).
                    summary.flFingerCurl[2] = -bmvr::g_VrGmanFingerStraight * fold;
                    // Black Mesa gloves: straightened along the finger instead.
                    if (hand.asset.sourcePath.rfind("bm:", 0) == 0)
                    {
                        summary.flFingerCurl[2] = 0.f;
                        straightMiddle = fold;
                    }
                    summary.flFingerCurl[3] = fold;
                    summary.flFingerCurl[4] = fold;
                }
            }
            // Reload-clip hand pose (VrClipHandPose): Black Mesa's own left
            // fingers around the held magazine / bolt / slide, baked. The
            // palette depends only on the pose and the glove, so it is built
            // once per pose and reused.
            bool clipPose = false;
            // Satchel bag hung from the left glove (SatchelBagToLeftHand):
            // the fist closes round its top. The left grip is Use there, so
            // the glove otherwise stayed open with the bag under the palm
            // (user screenshot 2026-11-19).
            // Same for the RPG rocket held through the fist (RocketInFist).
            unsigned bagMs = 0;
            const bool bagGrip = !rightHand && !rungGrip
                && (MagReload::RocketInFist()
                    || ((bagMs = MagReload::g_SatchelBagMs.load(std::memory_order_relaxed)) != 0
                        && GetTickCount() - bagMs < 200));
            if (rungGrip || bagGrip)
            {
                // Round the rung / bag top: one fixed curl, thumb over the fingers.
                for (int f = 0; f < 5; ++f)
                    summary.flFingerCurl[f] = rungGrip ? rungCurl
                        : (MagReload::RocketInFist() ? bmvr::g_RpgFistCurl : bmvr::g_SatchelGripCurl);
                gmanThumb = 0.f;
                straightMiddle = 0.f;
            }
            const int clipId = (rightHand || rungGrip) ? ClipHandPose::kNone : MagReload::WantedClipHandPose();
            if (clipId >= 0 && clipId < ClipHandPose::kCount)
            {
                static const VrHandMeshAsset* s_clipAsset = nullptr;
                static int s_clipId = -1;
                static float s_clipThumb = 0.f, s_clipThumbBase = 0.f;
                static std::vector<VrHandMatrixRows3x4> s_clipPalette;
                // Glock magazine: thumb curl tuning (DEBUG "GLOCK MAGAZINE IN THE HAND").
                const bool glockMag = clipId == ClipHandPose::kGlockMag;
                const float thumb = glockMag ? bmvr::g_GlockThumbCurl : 0.f;
                const float thumbBase = glockMag ? bmvr::g_GlockThumbBase : 0.f;
                float fingers[15][12];
                std::memcpy(fingers, ClipHandPose::kPoses[clipId].fingers, sizeof(fingers));
                if (thumb != 0.f || thumbBase != 0.f)
                    CurlThumb(fingers, thumbBase, thumb);
                if (s_clipAsset == &hand.asset && s_clipId == clipId && s_clipThumb == thumb
                    && s_clipThumbBase == thumbBase && !s_clipPalette.empty())
                {
                    hand.palette = s_clipPalette;
                    clipPose = true;
                }
                else if (BuildStockPosePalette(hand.asset, hand.rig, fingers, hand.palette))
                {
                    s_clipAsset = &hand.asset;
                    s_clipId = clipId;
                    s_clipThumb = thumb;
                    s_clipThumbBase = thumbBase;
                    s_clipPalette = hand.palette;
                    clipPose = true;
                }
                hand.rig.paletteValid = false;
            }
            drawnClip = clipPose ? clipId : ClipHandPose::kNone;
            const bool pinch = !clipPose && !rightHand && !rungGrip && LeftGlovePinch();
            if (pinch)
            {
                summary.flFingerCurl[0] = bmvr::g_VrPinchThumb;
                summary.flFingerCurl[1] = bmvr::g_VrPinchIndex;
                for (int f = 2; f < 5; ++f)
                    summary.flFingerCurl[f] = kPinchRest[f - 2];
                gmanThumb = 0.f;
            }
            float stockRel[15][12];
            const bool stockPose = !clipPose && !pinch && !rightHand && gripCurl && bmvr::g_VrStockHandPose
                && g_Game->m_VR->GetStockOffhandFingers(stockRel)
                && BuildStockPosePalette(hand.asset, hand.rig, stockRel, hand.palette);
            {
                static int s_lastMode = -1;
                const int mode = clipPose ? 10 + clipId : pinch ? 2 : (stockPose ? 1 : (gripCurl ? 3 : 0));
                if (!rightHand && mode != s_lastMode)
                {
                    s_lastMode = mode;
                    if (clipPose)
                        Game::logMsg("Stock hand pose: left glove clip pose (%s: %s @ %.2f)", ClipHandPose::kPoses[clipId].name,
                            ClipHandPose::kPoses[clipId].clip, ClipHandPose::kPoses[clipId].cycle);
                    else
                        Game::logMsg("Stock hand pose: left glove %s", mode == 2 ? "pinch" : mode == 1 ? "stock fingers"
                            : mode == 3 ? "grip curl (no stock pose)" : "controller curls");
                }
            }
            if (!stockPose && !clipPose
                && !BuildSummaryCurlPalette(hand.asset, hand.rig, summary, hand.palette,
                    (gripCurl && !pinch) ? 0.40f : 0.f, gmanThumb, pinch ? 0.f : straightMiddle))
                BuildBindPosePalette(hand.asset, hand.palette);
        }
        else if (hand.palette.empty())
        {
            BuildBindPosePalette(hand.asset, hand.palette);
        }

        Vector rotOffset(
            bmvr::g_VrHandsPoseRotX + (rightHand && gripCurl ? bmvr::g_VrHandsRightGripRotX : 0.f),
            bmvr::g_VrHandsPoseRotY + (rightHand && gripCurl ? bmvr::g_VrHandsRightGripRotY : 0.f),
            bmvr::g_VrHandsPoseRotZ + (rightHand && gripCurl ? bmvr::g_VrHandsRightGripRotZ : 0.f));
        Vector posOffset(
            0.f, 0.f, 0.f);
        const uint32_t family = (g_Game && g_Game->m_VR)
            ? g_Game->m_VR->m_ControllerFamily
            : L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN;
        bmvr::EffectiveVrHandsPoseOffset(family, posOffset.x, posOffset.y, posOffset.z);
        if (i == 0)
        {
            posOffset.x += bmvr::g_VrHandsLeftPoseOffX;
            posOffset.y += bmvr::g_VrHandsLeftPoseOffY;
            posOffset.z += bmvr::g_VrHandsLeftPoseOffZ;
        }
        else
        {
            posOffset.x += bmvr::g_VrHandsRightPoseOffX;
            posOffset.y += bmvr::g_VrHandsRightPoseOffY;
            posOffset.z += bmvr::g_VrHandsRightPoseOffZ;
        }
        // Hands around a grip sit thumb-up, not palm-down: roll the drawn
        // glove (only) around the controller's forward axis, mirrored per
        // hand. One setting for every controller: Index poses already carry
        // VrHandsIndexRollDeg (that roll also feeds crowbar/flashlight), so
        // the glove only adds the difference. Also on a hand holding the gun:
        // rolled, it sits under the gun instead of on its side.
        QAngle gloveAngles = *angles[i];
        float gloveRoll = bmvr::g_VrHandsRollDeg;
        if (family == L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES)
            gloveRoll -= bmvr::g_VrHandsIndexRollDeg;
        if (gloveRoll != 0.f)
            gloveAngles.z += rightHand ? gloveRoll : -gloveRoll;
        // Support hand on the gun: toward the gun, so the gun sits in the palm.
        Vector gloveOrigin = *origins[i];
        if (gripCurl && g_Game && g_Game->m_VR)
            gloveOrigin = gloveOrigin + g_Game->m_VR->GunGripHandOffset(rightHand, *angles[1 - i]);
        VrHandMatrix4 world = VrHandMath::BuildControllerWorld(
            gloveOrigin,
            gloveAngles,
            sourceUnitsPerMeter,
            scale,
            posOffset,
            rotOffset);
        if (rungGrip)
            WrapGloveOnRung(hand.asset, hand.palette, world, rightHand, rungPoint, rungAxis);
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
        if (i == 0 && !gripCurl)
            StoreLeftFistCentre(hand.asset, hand.palette, world, *origins[0], *angles[0]);
        if (i == 0)
            StoreLeftClipWrist(hand.asset, world, *origins[0], *angles[0], drawnClip);

        // GetLightForPoint + flashlight cone once per hand per frame; the
        // second eye reuses it (same palm position, same Present tick).
        const uint32_t tick = (g_Game && g_Game->m_VR) ? g_Game->m_VR->m_PresentTick : 0u;
        const Vector& palm = *origins[i];
        if (!hand.lightValid || hand.lightTick != tick
            || hand.lightPalm.x != palm.x || hand.lightPalm.y != palm.y || hand.lightPalm.z != palm.z)
        {
            hand.lightExposure = 0.45f;
            hand.lightRgb = SamplePalmSceneLight(palm, hand.lightExposure);
            hand.lightPalm = palm;
            hand.lightTick = tick;
            hand.lightValid = true;
        }
        const float envExposure = hand.lightExposure;
        const Vector sceneRgb = hand.lightRgb;
        IDirect3DBaseTexture9* envCube = (g_Game && g_Game->m_VR)
            ? g_Game->m_VR->SceneCubemap() : nullptr;

        std::string error;
        if (!m_Impl->renderer.Draw(
                device,
                gpuBase + i,
                hand.asset,
                hand.palette,
                world,
                wvp,
                (overlayNoDepth || stereoEye == 0)
                    ? VrHandDrawPass::OverlayNoDepth : VrHandDrawPass::WorldDepth,
                sceneRgb,
                eyeOrigin,
                envExposure,
                envCube,
                error))
        {
            if (!error.empty())
            {
                static int s_drawFailLog[2]{};
                if (s_drawFailLog[i] < 4)
                {
                    Game::logMsg("VR glove draw hand=%d: %s", i, error.c_str());
                    ++s_drawFailLog[i];
                }
            }
            continue;
        }
        drew = true;
    }

    // Gun's own hands (viewmodel arms, forearm folded away): the HEV glove's
    // open cuff is capped with the same fan as the gloves', placed on the
    // wrist bone as drawn (user 2026-11-19: hands looked hollow from the
    // wrist side). Bare hands are closed.
    if (bmvr::g_GunHandCuffCap && pair == m_Impl->hev)
    {
        // Right only: the left one (support-hand lock) sat off the hand and
        // that cuff was not seen open (user 2026-11-19). Never with the
        // right glove showing (switch to empty hands: the cap stayed a frame).
        const bool rightGlove = g_Game && g_Game->m_VR && g_Game->m_VR->WantsRightGloveVisible();
        for (int h = 1; h < 2 && !rightGlove; ++h)
        {
            float W[12];
            if (!g_Game || !g_Game->m_VR || !g_Game->m_VR->GunCapWristForDraw(W))
                continue;
            if (!m_Impl->gunCapLoaded[h])
            {
                m_Impl->gunCapLoaded[h] = BmHands::GunCuffCap(h, m_Impl->gunCap[h]);
                if (m_Impl->gunCapLoaded[h])
                    Game::logMsg("Gun hand cuff cap %s: %zu triangles", h ? "right" : "left",
                        m_Impl->gunCap[h].indices.size() / 3);
                else
                    continue;
            }
            VrHandMatrix4 world;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    world.m[c * 4 + r] = W[r * 4 + c];
            world.m[3] = world.m[7] = world.m[11] = 0.f;
            world.m[15] = 1.f;
            const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
            std::vector<VrHandMatrixRows3x4> palette(1);
            palette[0].v = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f };
            float exposure = 0.45f;
            const Vector at(W[3], W[7], W[11]);
            const Vector rgb = SamplePalmSceneLight(at, exposure);
            IDirect3DBaseTexture9* envCube = (g_Game && g_Game->m_VR) ? g_Game->m_VR->SceneCubemap() : nullptr;
            std::string error;
            if (!m_Impl->renderer.Draw(device, 5 + h, m_Impl->gunCap[h], palette, world, wvp,
                    (overlayNoDepth || stereoEye == 0) ? VrHandDrawPass::OverlayNoDepth : VrHandDrawPass::WorldDepth,
                    rgb, eyeOrigin, exposure, envCube, error))
            {
                static int s_capFail;
                if (s_capFail++ < 4)
                    Game::logMsg("Gun hand cuff cap draw %d: %s", h, error.c_str());
            }
        }
    }

    if (drew && !m_Impl->loggedReady)
    {
        m_Impl->loggedReady = true;
        Game::logMsg("VR gloves drew source=%s eye=%d fov=%.1f aspect=%.3f scale=%.2f cubemap=%d",
            source, stereoEye, horizontalFovDegrees, aspectRatio, scale,
            (g_Game && g_Game->m_VR && g_Game->m_VR->SceneCubemap()) ? 1 : 0);
    }
    return drew;
}

void VrHands_UpdatePalmLight(int hand, const Vector& pos)
{
    // Game thread only (see CachedPalmLight).
    if (hand < 0 || hand > 1 || !g_Game)
        return;
    Vector rgb(1.f, 1.f, 1.f);
    const bool ok = g_Game->TryGetLightForPoint(pos, rgb, false);
    std::lock_guard<std::mutex> lock(g_PalmLightMutex);
    g_PalmLight[hand].pos = pos;
    g_PalmLight[hand].rgb = rgb;
    g_PalmLight[hand].valid = ok;
}
