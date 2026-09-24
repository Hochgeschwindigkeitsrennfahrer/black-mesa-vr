#include "vr_hands.h"

#include "bmvr_flags.h"
#include "game.h"
#include "vr.h"
#include "vr_hand_asset_loader.h"
#include "vr_hand_math.h"
#include "vr_hand_renderer_d3d9.h"

#include "openvr.h"

#include <algorithm>
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
        float gripCurlMin)
    {
        if (!rig.built)
            BuildHandRig(asset, rig);
        if (!rig.valid)
            return false;

        float curls[vr::VRFinger_Count];
        for (int finger = 0; finger < vr::VRFinger_Count; ++finger)
        {
            float curl = std::clamp(summary.flFingerCurl[finger], 0.0f, 1.0f);
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
        const size_t n = asset.jointNames.size();
        if (rig.paletteValid && outPalette.size() == n && rig.lastGripCurlMin == gripCurlMin
            && std::memcmp(curls, rig.lastCurl, sizeof(curls)) == 0)
            return true;

        static const float kMaxCurlRadians[vr::VRFinger_Count][3] =
        {
            { 0.75f, 0.90f, 0.65f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
            { 1.15f, 1.25f, 0.90f },
        };

        std::vector<VrHandMatrix4>& localMatrices = rig.scratchLocal;
        std::vector<VrHandMatrix4>& modelMatrices = rig.scratchModel;
        localMatrices.assign(rig.bindLocal.begin(), rig.bindLocal.end());
        for (int finger = 0; finger < vr::VRFinger_Count; ++finger)
        {
            const float sign = (rig.valveBiped && finger > 0) ? -1.f : 1.f;
            for (int segment = 0; segment < 3; ++segment)
            {
                const size_t joint = static_cast<size_t>(rig.fingerJoint[finger][segment]);
                localMatrices[joint] = VrHandMath::Multiply(
                    localMatrices[joint],
                    MakeLocalZRotation(sign * curls[finger] * kMaxCurlRadians[finger][segment]));
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
        rig.lastGripCurlMin = gripCurlMin;
        rig.paletteValid = true;
        return true;
    }

    void BuildBindPosePalette(const VrHandMeshAsset& asset, std::vector<VrHandMatrixRows3x4>& outPalette)
    {
        outPalette.resize(asset.jointNames.size());
        const VrHandMatrixRows3x4 identity = VrHandMath::ToRows3x4(VrHandMath::Identity());
        for (size_t i = 0; i < outPalette.size(); ++i)
            outPalette[i] = identity;
    }

    // BuildSummaryCurlPalette only reads flFingerCurl, so go through
    // VR::GetFingerCurls, which knows both backends. Calling
    // GetSkeletalSummaryData directly meant the gloves froze in their bind pose
    // on OpenXR, where there is no OpenVR input session to ask.
    bool TryGetSummary(vr::IVRInput* input, vr::VRActionHandle_t action, vr::VRSkeletalSummaryData_t& out)
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
        if (!input || action == vr::k_ulInvalidActionHandle)
            return false;
        return input->GetSkeletalSummaryData(action, vr::VRSummaryType_FromAnimation, &out)
            == vr::VRInputError_None;
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

    Vector SamplePalmSceneLight(const Vector& palm, float& outEnvExposure)
    {
        // Diffuse uses a high floor so indoor gloves stay readable.
        // Spec/cubemap uses raw luma so dark rooms do not stay chrome.
        outEnvExposure = 0.45f;
        Vector rgb(1.f, 1.f, 1.f);
        if (g_Game && g_Game->TryGetLightForPoint(palm, rgb, false))
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
    VrHandRendererD3D9 renderer;
    bool assetAttempted = false;
    bool assetsLoaded = false;
    bool hevLoaded = false;
    bool bareLoaded = false;
    bool unavailable = false;
    bool loggedReady = false;
    std::string failure;

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

        std::vector<char> buffer(vr::k_unMaxPropertyStringSize, '\0');
        uint32_t required = 0;
        if (vr::VR_GetRuntimePath(buffer.data(), static_cast<uint32_t>(buffer.size()), &required))
        {
            if (required > buffer.size())
            {
                buffer.assign(static_cast<size_t>(required) + 1u, '\0');
                vr::VR_GetRuntimePath(buffer.data(), static_cast<uint32_t>(buffer.size()), &required);
            }
            if (buffer.front() != '\0' && tryRoot(std::filesystem::path(buffer.data())))
                return true;
        }

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

    bool EnsureAssets()
    {
        if (unavailable)
            return false;
        if (assetsLoaded)
            return true;
        if (assetAttempted)
            return false;
        assetAttempted = true;

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

bool BmVrGloves::DrawForEye(
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
    bool overlayNoDepth)
{
    if (!device || !m_Impl || !m_Impl->EnsureAssets())
        return false;

    const float scale = std::clamp(modelScale, 0.2f, 2.0f);
    const float nearPlane = (zNear > 0.01f) ? zNear : kEyeZNear;
    const float farPlane = (zFar > nearPlane + 1.f) ? zFar : kEyeZFar;
    const VrHandMatrix4 projection = VrHandMath::BuildPerspective(
        horizontalFovDegrees, aspectRatio, nearPlane, farPlane);
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

        Impl::Hand& hand = pair[i];
        vr::VRSkeletalSummaryData_t summary{};
        const bool gripCurl = g_Game && g_Game->m_VR && (
            (rightHand && g_Game->m_VR->WantsRightGloveWeaponGripCurl())
            || (!rightHand && g_Game->m_VR->WantsOffhandGloveWeaponGripCurl()));
        if (TryGetSummary(input, actions[i], summary))
        {
            if (!BuildSummaryCurlPalette(hand.asset, hand.rig, summary, hand.palette, gripCurl ? 0.40f : 0.f))
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
        const VrHandMatrix4 world = VrHandMath::BuildControllerWorld(
            *origins[i],
            *angles[i],
            sourceUnitsPerMeter,
            scale,
            posOffset,
            rotOffset);
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));

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

    if (drew && !m_Impl->loggedReady)
    {
        m_Impl->loggedReady = true;
        Game::logMsg("VR gloves drew source=%s eye=%d fov=%.1f aspect=%.3f scale=%.2f cubemap=%d",
            source, stereoEye, horizontalFovDegrees, aspectRatio, scale,
            (g_Game && g_Game->m_VR && g_Game->m_VR->SceneCubemap()) ? 1 : 0);
    }
    return drew;
}
