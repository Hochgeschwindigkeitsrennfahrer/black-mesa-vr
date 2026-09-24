#include "vr.h"
#include "game.h"
#include "sdk.h"
#include "bmvr_flags.h"
#include "in_buttons.h"

#include <algorithm>
#include <cstring>

namespace
{
    constexpr int kMagCapacity = 17;          // weapon_glock clip
    constexpr float kInsertDistance = 4.f;    // HL2VR PISTOL_CHAMBER_TO_MAGAZINE_LOAD_DISTANCE
    constexpr float kSlideGrabRadius = 6.f;
    constexpr float kMagGrabRadius = 8.f;
    constexpr float kShoulderRadius = 14.f;
    constexpr float kSlideTravel = 1.15f;     // inches, model +Y (back from the muzzle)
    constexpr float kSlidePullThreshold = 0.8f;
    constexpr DWORD kDroppedMagMs = 8000;
    constexpr DWORD kReloadWaitMs = 4000;

    // vr_pistol.mdl bind pose. Bone 0 rotation is identity, so attachment
    // locals plus bone positions are model space. Muzzle looks down model -Y.
    const Vector kGripOrigin(0.f, 0.1847f, 0.2385f);
    const Vector kGripForward(0.f, -0.99939084f, -0.034899496f);
    const Vector kGripLeft(1.f, 0.f, 0.f);
    const Vector kGripUp(0.f, -0.034899496f, 0.99939084f);
    const Vector kGunReloadLocal(0.f, 1.5515f, -3.6176f);
    const Vector kSlideGrabLocal(0.f, 1.0f, 1.5f);
    const Vector kClipEjectLocal(0.f, 0.969f, -1.82f);

    // vr_pistol_clip.mdl. Grip attachment is identity rotation.
    const Vector kClipGripOrigin(-0.014f, -0.069f, -1.561f);
    const Vector kClipReloadLocal(0.f, -0.001f, 2.646f);

    enum MagState { MagNone, MagInHand, MagLoaded, MagWorld };
    enum SlideState { SlideClosed, SlideLocked };

    struct Pistol
    {
        void* gun = nullptr;
        void* slideModel = nullptr;
        void* clipModel = nullptr;
        void* handModel = nullptr;
        int loadTries = 0;
        int modelIndex = 0;
        bool drawFailed = false;
        bool seeded = false;
        MagState mag = MagNone;
        int magAmmo = 0;
        int chamber = 0;
        SlideState slide = SlideClosed;
        float slidePull = 0.f;
        bool slideGrabbed = false;
        bool slidePulledFar = false;
        Vector slideGrabStart{};
        bool gripWas = false;
        bool reloadWas = false;
        bool attackWas = false;
        int lastClip = -1;
        bool waitingReload = false;
        bool reloadPulsed = false;
        DWORD reloadWaitUntil = 0;
        Vector worldMagOrigin{};
        QAngle worldMagAngles{};
        DWORD worldMagUntil = 0;
        DWORD lastTick = 0;
        bool logged = false;
    };

    Pistol g_pistol;

    bool NameHas(const char* hay, const char* needle)
    {
        return hay && needle && std::strstr(hay, needle) != nullptr;
    }

    void GripMatrix(VMatrix& out)
    {
        out.Identity();
        const Vector& f = kGripForward;
        const Vector& l = kGripLeft;
        const Vector& u = kGripUp;
        const Vector& o = kGripOrigin;
        out.m[0][0] = f.x; out.m[0][1] = l.x; out.m[0][2] = u.x; out.m[0][3] = o.x;
        out.m[1][0] = f.y; out.m[1][1] = l.y; out.m[1][2] = u.y; out.m[1][3] = o.y;
        out.m[2][0] = f.z; out.m[2][1] = l.z; out.m[2][2] = u.z; out.m[2][3] = o.z;
        out.m[3][0] = 0.f; out.m[3][1] = 0.f; out.m[3][2] = 0.f; out.m[3][3] = 1.f;
    }

    void ClipGripMatrix(VMatrix& out)
    {
        out.Identity();
        out.m[0][3] = kClipGripOrigin.x;
        out.m[1][3] = kClipGripOrigin.y;
        out.m[2][3] = kClipGripOrigin.z;
    }

    void AngleMatrixSource(const QAngle& ang, const Vector& org, VMatrix& out)
    {
        Vector fwd, right, up;
        QAngle::AngleVectors(ang, &fwd, &right, &up);
        out.m[0][0] = fwd.x;   out.m[0][1] = -right.x; out.m[0][2] = up.x; out.m[0][3] = org.x;
        out.m[1][0] = fwd.y;   out.m[1][1] = -right.y; out.m[1][2] = up.y; out.m[1][3] = org.y;
        out.m[2][0] = fwd.z;   out.m[2][1] = -right.z; out.m[2][2] = up.z; out.m[2][3] = org.z;
        out.m[3][0] = 0.f;     out.m[3][1] = 0.f;      out.m[3][2] = 0.f; out.m[3][3] = 1.f;
    }

    void MatrixToOriginAngles(const VMatrix& m, Vector& origin, QAngle& angles)
    {
        origin.x = m.m[0][3];
        origin.y = m.m[1][3];
        origin.z = m.m[2][3];
        Vector fwd(m.m[0][0], m.m[1][0], m.m[2][0]);
        Vector up(m.m[0][2], m.m[1][2], m.m[2][2]);
        QAngle::VectorAngles(fwd, up, angles);
    }

    Vector TransformPoint(const VMatrix& m, const Vector& p)
    {
        return Vector(
            m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3],
            m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3],
            m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z + m.m[2][3]);
    }

    Vector Column(const VMatrix& m, int col)
    {
        return Vector(m.m[0][col], m.m[1][col], m.m[2][col]);
    }

    void BuildStudioWorld(const QAngle& ang, const Vector& org, float out[3][4])
    {
        Vector fwd, right, up;
        QAngle::AngleVectors(ang, &fwd, &right, &up);
        out[0][0] = fwd.x;   out[0][1] = -right.x; out[0][2] = up.x; out[0][3] = org.x;
        out[1][0] = fwd.y;   out[1][1] = -right.y; out[1][2] = up.y; out[1][3] = org.y;
        out[2][0] = fwd.z;   out[2][1] = -right.z; out[2][2] = up.z; out[2][3] = org.z;
    }

    // IModelRender::DrawModel (slot 0) faults from inside DrawModelExecute on
    // this engine. DrawModelEx is vtable +0x40 (slot 16): Setup then Execute.
    int CallDrawModelExSeh(void* modelRender, void* info)
    {
        if (!modelRender || !info)
            return -1;
        void** vt = nullptr;
        __try
        {
            vt = *reinterpret_cast<void***>(modelRender);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        if (!vt)
            return -1;
        using ExFn = int(__thiscall*)(void*, void*);
        auto ex = reinterpret_cast<ExFn>(vt[16]);
        if (!ex)
            return -1;
        int drawn = 0;
        __try
        {
            drawn = ex(modelRender, info);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return drawn;
    }

    void ResetInteraction()
    {
        g_pistol.seeded = false;
        g_pistol.mag = MagNone;
        g_pistol.magAmmo = 0;
        g_pistol.chamber = 0;
        g_pistol.slide = SlideClosed;
        g_pistol.slidePull = 0.f;
        g_pistol.slideGrabbed = false;
        g_pistol.slidePulledFar = false;
        g_pistol.gripWas = false;
        g_pistol.reloadWas = false;
        g_pistol.attackWas = false;
        g_pistol.lastClip = -1;
        g_pistol.waitingReload = false;
        g_pistol.reloadPulsed = false;
        g_pistol.worldMagUntil = 0;
    }

    bool GlockHeld(VR* vr)
    {
        if (!vr || vr->EmptyHands() || !vr->IsGameplayEligible())
            return false;
        std::lock_guard<std::recursive_mutex> lock(vr->m_ControllerMutex);
        const char* model = vr->m_LastViewmodelModel.c_str();
        return NameHas(model, "glock") || NameHas(model, "Glock");
    }

    bool EnsureModels(VR* vr)
    {
        if (g_pistol.drawFailed || !vr || !vr->m_Game)
            return false;
        if (g_pistol.gun && g_pistol.slideModel && g_pistol.clipModel)
        {
            if (!g_pistol.handModel && vr->m_Game)
                g_pistol.handModel = vr->m_Game->FindOrLoadModel("models/weapons/vr_hlvr_hands.mdl");
            return true;
        }
        if (g_pistol.loadTries >= 180)
            return false;
        ++g_pistol.loadTries;
        Game* game = vr->m_Game;
        if (!g_pistol.gun)
            g_pistol.gun = game->FindOrLoadModel("models/weapons/vr_pistol.mdl");
        if (!g_pistol.slideModel)
            g_pistol.slideModel = game->FindOrLoadModel("models/weapons/vr_pistol_slide.mdl");
        if (!g_pistol.clipModel)
            g_pistol.clipModel = game->FindOrLoadModel("models/weapons/vr_pistol_clip.mdl");
        if (!g_pistol.handModel)
            g_pistol.handModel = game->FindOrLoadModel("models/weapons/vr_hlvr_hands.mdl");
        if (g_pistol.gun && g_pistol.slideModel && g_pistol.clipModel)
        {
            Game::logMsg("HL2VR pistol loaded vr_pistol / slide / clip hand=%d tries=%d",
                g_pistol.handModel ? 1 : 0, g_pistol.loadTries);
            return true;
        }
        if (g_pistol.loadTries == 1 || g_pistol.loadTries == 180)
            Game::logMsg("HL2VR pistol models missing (try %d) — v_glock stays", g_pistol.loadTries);
        return false;
    }

    bool AimGunPose(VR* vr, VMatrix& weapon, Vector& origin, QAngle& angles)
    {
        Vector body{};
        Vector handTracking{};
        QAngle handAng{};
        {
            std::lock_guard<std::recursive_mutex> lock(vr->m_ControllerMutex);
            if (!vr->m_ControllerPoseValid)
                return false;
            body = vr->m_HasStereoBodyOrigin ? vr->m_StereoBodyOrigin : vr->m_SetupOrigin;
            handTracking = vr->m_RightControllerPosAbs;
            handAng = vr->m_RightControllerAngAbs;
        }
        if (body.LengthSqr() <= 1.f)
            return false;
        const Vector handPos = vr->ControllerTrackingToWorld(body, handTracking);
        VMatrix hand{};
        AngleMatrixSource(handAng, handPos, hand);
        VMatrix grip{};
        GripMatrix(grip);
        VMatrix invGrip = grip.InverseTR();
        hand.MatrixMul(invGrip, weapon);
        MatrixToOriginAngles(weapon, origin, angles);
        return origin.LengthSqr() > 1.f;
    }

    bool OffHand(VR* vr, Vector& world, QAngle& angles)
    {
        Vector body{};
        Vector track{};
        QAngle ang{};
        bool ok = false;
        {
            std::lock_guard<std::recursive_mutex> lock(vr->m_ControllerMutex);
            body = vr->m_HasStereoBodyOrigin ? vr->m_StereoBodyOrigin : vr->m_SetupOrigin;
            if (bmvr::g_LeftHanded)
            {
                ok = vr->m_PhysicalRightTrackingValid;
                track = ok ? vr->m_PhysicalRightPosAbs : vr->m_RightControllerPosAbs;
                ang = ok ? vr->m_PhysicalRightAngAbs : vr->m_RightControllerAngAbs;
                if (!ok)
                    ok = vr->m_ControllerPoseValid;
            }
            else
            {
                ok = vr->m_LeftControllerTrackingValid;
                track = vr->m_LeftControllerPosAbs;
                ang = vr->m_LeftControllerAngAbs;
            }
        }
        if (!ok || body.LengthSqr() <= 1.f)
            return false;
        world = vr->ControllerTrackingToWorld(body, track);
        angles = ang;
        return world.LengthSqr() > 1.f;
    }

    void LerpSlide(float dt)
    {
        if (g_pistol.slideGrabbed)
            return;
        const float target = (g_pistol.slide == SlideLocked) ? 1.f : 0.f;
        const float step = std::clamp(dt, 0.f, 0.05f) * 10.f;
        if (g_pistol.slidePull < target)
            g_pistol.slidePull = (std::min)(target, g_pistol.slidePull + step);
        else
            g_pistol.slidePull = (std::max)(target, g_pistol.slidePull - step);
    }

    void FinishPullback(VR* vr)
    {
        const bool had = g_pistol.chamber > 0;
        g_pistol.chamber = 0;
        if (g_pistol.mag == MagLoaded && g_pistol.magAmmo > 0)
        {
            --g_pistol.magAmmo;
            g_pistol.chamber = 1;
        }
        if (had && g_pistol.chamber > 0 && vr && vr->m_Game)
            vr->m_Game->PlayUiSound("weapons/pistol/Pistol_SlideReleased_DAv01.wav");
        g_pistol.slide = (g_pistol.chamber > 0) ? SlideClosed : SlideLocked;
    }

    void SeedFromClip(int clip)
    {
        g_pistol.seeded = true;
        g_pistol.lastClip = clip;
        if (clip > 0)
        {
            g_pistol.chamber = 1;
            g_pistol.magAmmo = clip - 1;
            g_pistol.mag = MagLoaded;
            g_pistol.slide = SlideClosed;
            g_pistol.slidePull = 0.f;
        }
        else
        {
            g_pistol.chamber = 0;
            g_pistol.magAmmo = 0;
            g_pistol.mag = MagNone;
            g_pistol.slide = SlideLocked;
            g_pistol.slidePull = 1.f;
        }
    }

    void ConsumeOneFromServerDrop()
    {
        if (g_pistol.mag == MagLoaded && g_pistol.magAmmo > 0)
            --g_pistol.magAmmo;
        else
        {
            g_pistol.chamber = 0;
            g_pistol.slide = SlideLocked;
        }
    }
}

namespace bmvr_pistol_depth
{
    thread_local int g_depth = 0;
}

int HlvrPistolDrawDepth()
{
    return bmvr_pistol_depth::g_depth;
}

void HlvrPistolEnterDraw()
{
    ++bmvr_pistol_depth::g_depth;
}

void HlvrPistolLeaveDraw()
{
    if (bmvr_pistol_depth::g_depth > 0)
        --bmvr_pistol_depth::g_depth;
}

bool VR::DrawHlvrPistol(void* modelRender, const ModelRenderInfo_t& vmInfo)
{
    (void)modelRender;
    (void)vmInfo;
    // Nested DrawModelEx returns 0 while this viewmodel is already inside
    // DrawModelExecute (setup rejects the extra draw). The mesh is selected
    // by swapping the viewmodel model index before that setup.
    return false;
}

int VR::HlvrPistolModelIndex()
{
    if (g_pistol.modelIndex > 0)
        return g_pistol.modelIndex;
    if (!m_Game || !m_Game->m_ModelInfo)
        return 0;
    EnsureModels(this);
    int idx = 0;
    __try
    {
        idx = m_Game->m_ModelInfo->GetModelIndex("models/weapons/vr_pistol.mdl");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        idx = 0;
    }
    if (idx <= 0)
    {
        void** vt = nullptr;
        __try
        {
            vt = *reinterpret_cast<void***>(m_Game->m_ModelInfo);
            using RegFn = int(__thiscall*)(void*, const char*, bool);
            if (vt && vt[52])
                idx = reinterpret_cast<RegFn>(vt[52])(m_Game->m_ModelInfo, "models/weapons/vr_pistol.mdl", true);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            idx = 0;
        }
    }
    if (idx <= 0)
    {
        static int s_fail;
        if (s_fail < 3)
        {
            Game::logMsg("HL2VR pistol model index missing");
            ++s_fail;
        }
        return 0;
    }
    g_pistol.modelIndex = idx;
    void* model = nullptr;
    __try
    {
        model = m_Game->m_ModelInfo->GetModel(idx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        model = nullptr;
    }
    unsigned studio = 0;
    if (model)
    {
        g_pistol.gun = model;
        __try
        {
            studio = *reinterpret_cast<unsigned short*>(static_cast<char*>(model) + 0x38);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            studio = 0;
        }
    }
    Game::logMsg("HL2VR pistol viewmodel index=%d model=%p studio=%u", idx, model, studio);
    if (studio == 0)
    {
        g_pistol.modelIndex = 0;
        g_pistol.gun = nullptr;
        return 0;
    }
    return idx;
}

bool VR::IsHlvrPistolModel(void* model) const
{
    return model && g_pistol.gun && model == g_pistol.gun;
}

bool VR::IsHlvrPistolRigged(void* model) const
{
    if (!model)
        return false;
    return model == g_pistol.gun || model == g_pistol.slideModel || model == g_pistol.clipModel
        || model == g_pistol.handModel;
}

namespace
{
    struct MdlCache
    {
        void* obj = nullptr;
        void** vt = nullptr;
    };

    MdlCache FindMdlCache()
    {
        MdlCache out{};
        HMODULE eng = GetModuleHandleA("engine.dll");
        if (!eng)
            return out;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(eng);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return out;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const unsigned char*>(eng) + dos->e_lfanew);
        const auto* base = reinterpret_cast<const unsigned char*>(eng);
        const size_t size = nt->OptionalHeader.SizeOfImage;
        for (size_t i = 24; i + 8 < size; ++i)
        {
            if (base[i] != 0x0F || base[i + 1] != 0xB7 || base[i + 2] != 0x40 || base[i + 3] != 0x38
                || base[i + 4] != 0x50 || base[i + 5] != 0xFF || base[i + 6] != 0x52 || base[i + 7] != 0x38)
                continue;
            for (size_t b = 2; b < 20; ++b)
            {
                if (base[i - b] == 0x8B && base[i - b + 1] == 0x0D)
                {
                    void** global = nullptr;
                    std::memcpy(&global, base + i - b + 2, sizeof(global));
                    if (!global)
                        return out;
                    __try
                    {
                        out.obj = *global;
                        if (out.obj)
                            out.vt = *reinterpret_cast<void***>(out.obj);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        out = {};
                    }
                    return out;
                }
            }
        }
        return out;
    }

    unsigned short StudioHandle(void* model)
    {
        if (!model)
            return 0;
        unsigned short h = 0;
        __try
        {
            h = *reinterpret_cast<unsigned short*>(static_cast<char*>(model) + 0x38);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            h = 0;
        }
        return h;
    }
}

bool EnsureHardware(void* model, const char* path)
{
    if (!model || !path)
        return false;
    unsigned short handle = StudioHandle(model);
    static MdlCache s_cache{};
    static bool s_cacheTried = false;
    if (!s_cacheTried)
    {
        s_cache = FindMdlCache();
        s_cacheTried = true;
        Game::logMsg("HL2VR pistol mdlcache=%p", s_cache.obj);
    }
    MdlCache cache = s_cache;
    if ((handle == 0 || handle == 0xFFFF) && cache.obj && cache.vt && cache.vt[9])
    {
        using FindFn = unsigned short(__thiscall*)(void*, const char*);
        __try
        {
            handle = reinterpret_cast<FindFn>(cache.vt[9])(cache.obj, path);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            handle = 0;
        }
        if (handle != 0 && handle != 0xFFFF)
        {
            __try
            {
                *reinterpret_cast<unsigned short*>(static_cast<char*>(model) + 0x38) = handle;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                handle = 0;
            }
        }
    }
    void* hw = nullptr;
    if (handle != 0 && handle != 0xFFFF && cache.obj && cache.vt && cache.vt[14])
    {
        using HwFn = void*(__thiscall*)(void*, unsigned short);
        __try
        {
            hw = reinterpret_cast<HwFn>(cache.vt[14])(cache.obj, handle);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hw = nullptr;
        }
    }
    static int s_log;
    if (s_log < 6)
    {
        Game::logMsg("HL2VR pistol hw %s model=%p studio=%u hw=%p", path, model, handle, hw);
        ++s_log;
    }
    return hw != nullptr;
}

void* VR::HlvrPistolDrawModel()
{
    if (!EnsureModels(this) || !g_pistol.gun)
        return nullptr;
    if (!EnsureHardware(g_pistol.gun, "models/weapons/vr_pistol.mdl"))
        return nullptr;
    return g_pistol.gun;
}

namespace
{
    int g_attachDepth = 0;

    // @idle pose of the calibrated glock. Bone 0 is yawed so the barrel,
    // which is bone-local -Y, points along model +X (HL2VR forward).
    const float kPistolBind[8][3][4] = {
        {{-0.00000f, -1.00000f, 0.00000f, -1.18359f}, {1.00000f, -0.00000f, 0.00000f, -0.00001f}, {0.00000f, 0.00000f, 1.00000f, -1.06055f}},
        {{-0.00000f, -1.00000f, -0.00000f, 0.06578f}, {1.00000f, -0.00000f, -0.00000f, 0.62734f}, {0.00000f, -0.00000f, 1.00000f, -0.61226f}},
        {{-0.00000f, -1.00000f, 0.00000f, -2.67122f}, {1.00000f, -0.00000f, -0.00000f, -0.00001f}, {0.00000f, 0.00000f, 1.00000f, 0.80614f}},
        {{-0.00000f, -1.00000f, 0.00000f, -2.17171f}, {1.00000f, -0.00000f, -0.00000f, 0.62733f}, {0.00000f, 0.00000f, 1.00000f, 0.49561f}},
        {{-0.00000f, -1.00000f, -0.00001f, 1.87520f}, {1.00000f, -0.00000f, -0.00000f, 0.62734f}, {0.00000f, -0.00001f, 1.00000f, 0.51381f}},
        {{-0.00000f, -1.00000f, 0.00000f, 1.07617f}, {1.00000f, -0.00000f, -0.00000f, -0.00001f}, {0.00000f, 0.00000f, 1.00000f, 0.29883f}},
        {{-0.00000f, -1.00000f, 0.00000f, -0.99886f}, {1.00000f, -0.00000f, 0.00000f, -0.00001f}, {0.00000f, 0.00000f, 1.00000f, 1.50093f}},
        {{-0.00001f, -0.97419f, 0.22571f, -0.96745f}, {1.00000f, -0.00001f, -0.00000f, -0.00000f}, {0.00000f, 0.22571f, 0.97419f, -1.81814f}},
    };
    // Pull travels along model -X, which is back from the muzzle in this pose.
    constexpr float kSlideClosedX = -0.99886f;
    constexpr float kSlideOpenX = -3.413f;
    // Grip attachment after the idle bone-0 rotation. Pivot stays in the palm.
    const Vector kIdleGrip(-0.1836f, 0.f, 0.2395f);
    // player.mdl right-hand bone index, then the matching @idle pistol matrix.
    struct HandBone { int playerBone; float m[3][4]; };
    const HandBone kRightHand[] = {
        {27, {{0.96879f, -0.24200f, 0.05365f, -4.80140f}, {-0.24156f, -0.97028f, -0.01465f, -0.49298f}, {0.05560f, 0.00123f, -0.99845f, -1.80265f}}},
        {28, {{0.65074f, 0.69176f, -0.31305f, -0.96609f}, {0.74301f, -0.66509f, 0.07483f, -1.31418f}, {-0.15644f, -0.28129f, -0.94679f, -2.77950f}}},
        {29, {{-0.08275f, 0.94587f, -0.31381f, -0.11196f}, {0.99111f, 0.11102f, 0.07329f, -0.33892f}, {0.10416f, -0.30495f, -0.94665f, -2.98483f}}},
        {30, {{-0.86163f, 0.39892f, -0.31379f, -0.17232f}, {0.39780f, 0.91475f, 0.07061f, 0.38396f}, {0.31521f, -0.06399f, -0.94686f, -2.90885f}}},
        {31, {{0.68444f, 0.71624f, -0.13615f, -0.97119f}, {0.72162f, -0.69215f, -0.01350f, -1.50044f}, {-0.10391f, -0.08901f, -0.99060f, -2.01380f}}},
        {32, {{-0.11842f, 0.98343f, -0.13724f, 0.08225f}, {0.99296f, 0.11738f, -0.01568f, -0.38979f}, {0.00069f, -0.13813f, -0.99041f, -2.17374f}}},
        {33, {{-0.99015f, 0.03368f, -0.13590f, -0.05939f}, {0.03663f, 0.99914f, -0.01927f, 0.79812f}, {0.13514f, -0.02406f, -0.99053f, -2.17295f}}},
        {34, {{0.86486f, 0.48224f, -0.13949f, -1.04989f}, {0.46843f, -0.87515f, -0.12122f, -1.37600f}, {-0.18054f, 0.03950f, -0.98278f, -1.15643f}}},
        {35, {{-0.33872f, 0.92956f, -0.14556f, 0.43729f}, {0.93774f, 0.32088f, -0.13300f, -0.57048f}, {-0.07692f, -0.18154f, -0.98037f, -1.46686f}}},
        {36, {{-0.98730f, 0.07791f, -0.13842f, 0.02771f}, {0.09736f, 0.98539f, -0.13974f, 0.56342f}, {0.12551f, -0.15144f, -0.98047f, -1.55988f}}},
        {37, {{0.97617f, 0.20407f, 0.07376f, -1.09740f}, {0.20870f, -0.97604f, -0.06158f, -1.26137f}, {0.05943f, 0.07550f, -0.99537f, -0.25903f}}},
        {38, {{0.83794f, 0.54172f, 0.06634f, 0.58104f}, {0.54543f, -0.83548f, -0.06699f, -0.90250f}, {0.01914f, 0.09231f, -0.99555f, -0.15680f}}},
        {39, {{-0.24693f, 0.96707f, 0.06173f, 1.50250f}, {0.96487f, 0.25127f, -0.07680f, -0.30272f}, {-0.08978f, 0.04059f, -0.99513f, -0.13577f}}},
        {40, {{0.82589f, 0.42209f, -0.37382f, -4.00418f}, {0.48839f, -0.20423f, 0.84839f, -0.31924f}, {0.28176f, -0.88325f, -0.37481f, -0.44199f}}},
        {41, {{0.92756f, -0.02654f, -0.37273f, -2.52602f}, {0.32934f, -0.41321f, 0.84900f, 0.55486f}, {-0.17655f, -0.91025f, -0.37453f, 0.06229f}}},
        {42, {{0.71943f, -0.58714f, -0.37106f, -1.40645f}, {0.00788f, -0.52729f, 0.84965f, 0.95237f}, {-0.69452f, -0.61419f, -0.37472f, -0.15082f}}},
    };
    // v_pistol bone 0 rest translation. Identity rotation.
    constexpr float kGunRootT[3] = { 0.f, 1.184739f, -1.061483f };
    float g_gunMatrix[3][4]{};
    bool g_gunMatrixValid = false;

    void MulModel(const float world[3][4], const float local[3][4], float out[3][4])
    {
        float tmp[3][4];
        for (int r = 0; r < 3; ++r)
        {
            tmp[r][0] = world[r][0] * local[0][0] + world[r][1] * local[1][0] + world[r][2] * local[2][0];
            tmp[r][1] = world[r][0] * local[0][1] + world[r][1] * local[1][1] + world[r][2] * local[2][1];
            tmp[r][2] = world[r][0] * local[0][2] + world[r][1] * local[1][2] + world[r][2] * local[2][2];
            tmp[r][3] = world[r][0] * local[0][3] + world[r][1] * local[1][3] + world[r][2] * local[2][3] + world[r][3];
        }
        std::memcpy(out, tmp, sizeof(tmp));
    }

    void SlideLocal(float out[3][4])
    {
        std::memcpy(out, kPistolBind[6], sizeof(float) * 12);
        const float pull = g_pistol.slidePull < 0.f ? 0.f : (g_pistol.slidePull > 1.f ? 1.f : g_pistol.slidePull);
        out[0][3] = kSlideClosedX + (kSlideOpenX - kSlideClosedX) * pull;
    }
}

void VR::AnchorHlvrPistolGun(const float visibleBone0[3][4], const QAngle& aim, float outGun[3][4]) const
{
    float placed[3][4];
    std::memcpy(placed, visibleBone0, sizeof(placed));
    const float* t = kGunRootT;
    placed[0][3] -= visibleBone0[0][0] * t[0] + visibleBone0[0][1] * t[1] + visibleBone0[0][2] * t[2];
    placed[1][3] -= visibleBone0[1][0] * t[0] + visibleBone0[1][1] * t[1] + visibleBone0[1][2] * t[2];
    placed[2][3] -= visibleBone0[2][0] * t[0] + visibleBone0[2][1] * t[1] + visibleBone0[2][2] * t[2];

    // Keep the grip where the viewmodel bone put it. The glock bone rotation
    // points this mesh sideways; the muzzle is model -Y.
    const Vector& held = kGripOrigin;
    const Vector gripWorld(
        placed[0][0] * held.x + placed[0][1] * held.y + placed[0][2] * held.z + placed[0][3],
        placed[1][0] * held.x + placed[1][1] * held.y + placed[1][2] * held.z + placed[1][3],
        placed[2][0] * held.x + placed[2][1] * held.y + placed[2][2] * held.z + placed[2][3]);

    Vector fwd, right, up;
    QAngle::AngleVectors(aim, &fwd, &right, &up);
    // Idle pose points the barrel along model +X. Column 0 is aim forward.
    outGun[0][0] = fwd.x;   outGun[0][1] = -right.x; outGun[0][2] = up.x;
    outGun[1][0] = fwd.y;   outGun[1][1] = -right.y; outGun[1][2] = up.y;
    outGun[2][0] = fwd.z;   outGun[2][1] = -right.z; outGun[2][2] = up.z;
    const Vector& grip = kIdleGrip;
    outGun[0][3] = gripWorld.x - (outGun[0][0] * grip.x + outGun[0][1] * grip.y + outGun[0][2] * grip.z);
    outGun[1][3] = gripWorld.y - (outGun[1][0] * grip.x + outGun[1][1] * grip.y + outGun[1][2] * grip.z);
    outGun[2][3] = gripWorld.z - (outGun[2][0] * grip.x + outGun[2][1] * grip.y + outGun[2][2] * grip.z);
}

int VR::WriteHlvrPistolSkin(const float modelToWorld[3][4], void* model, float* boneRows, int maxBones)
{
    if (!boneRows || maxBones < 1 || !model)
        return 0;
    const bool gun = model == g_pistol.gun;
    const bool slide = model == g_pistol.slideModel;
    const bool clip = model == g_pistol.clipModel;
    const bool hand = model == g_pistol.handModel;
    if (!gun && !slide && !clip && !hand)
        return 0;
    if (gun)
    {
        if (!modelToWorld)
            return 0;
        std::memcpy(g_gunMatrix, modelToWorld, sizeof(g_gunMatrix));
        g_gunMatrixValid = true;
    }
    const float (*basisSrc)[4] = modelToWorld;
    if (!gun && g_gunMatrixValid && (slide || hand || (clip && g_pistol.mag == MagLoaded)))
        basisSrc = g_gunMatrix;
    if (!basisSrc)
        return 0;
    float basis[3][4];
    std::memcpy(basis, basisSrc, sizeof(basis));

    float slideM[3][4];
    SlideLocal(slideM);
    if (hand)
    {
        const int n = maxBones < 43 ? maxBones : 43;
        float away[3][4] = {
            {0.f, 0.f, 0.f, 1.0e6f},
            {0.f, 0.f, 0.f, 1.0e6f},
            {0.f, 0.f, 0.f, 1.0e6f},
        };
        for (int i = 0; i < n; ++i)
            std::memcpy(boneRows + i * 12, away, sizeof(away));
        for (const HandBone& bone : kRightHand)
        {
            if (bone.playerBone < 0 || bone.playerBone >= n)
                continue;
            float world[3][4];
            MulModel(basis, bone.m, world);
            std::memcpy(boneRows + bone.playerBone * 12, world, sizeof(world));
        }
        return n;
    }
    if (slide)
    {
        float bone[3][4];
        MulModel(basis, slideM, bone);
        std::memcpy(boneRows, bone, sizeof(bone));
        return 1;
    }
    if (clip)
    {
        float bone[3][4];
        MulModel(basis, kPistolBind[7], bone);
        const int n = maxBones < 41 ? maxBones : 41;
        for (int i = 0; i < n; ++i)
            std::memcpy(boneRows + i * 12, bone, sizeof(bone));
        return n;
    }

    const int n = maxBones < 47 ? maxBones : 47;
    float root[3][4];
    MulModel(basis, kPistolBind[0], root);
    for (int i = 0; i < n; ++i)
    {
        if (i == 6)
        {
            float bone[3][4];
            MulModel(basis, slideM, bone);
            std::memcpy(boneRows + i * 12, bone, sizeof(bone));
        }
        else if (i < 8)
        {
            float bone[3][4];
            MulModel(basis, kPistolBind[i], bone);
            std::memcpy(boneRows + i * 12, bone, sizeof(bone));
        }
        else
            std::memcpy(boneRows + i * 12, root, sizeof(root));
    }
    return n;
}

void VR::DrawHlvrPistolAttachments(void* modelRender, const ModelRenderInfo_t& gunInfo)
{
    if (g_attachDepth > 0 || !modelRender || !gunInfo.pModelToWorld || !EnsureModels(this))
        return;
    ++g_attachDepth;

    auto drawPart = [&](void* model, const char* path, const float world[3][4], const Vector& origin, const QAngle& angles) {
        if (!model || !EnsureHardware(model, path))
            return;
        ModelRenderInfo_t inf = gunInfo;
        inf.pModel = static_cast<model_t*>(model);
        inf.origin = origin;
        inf.angles = angles;
        inf.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(world);
        inf.instance = -1;
        const int drawn = CallDrawModelExSeh(modelRender, &inf);
        static int s_partLog;
        if (s_partLog < 6)
        {
            Game::logMsg("HL2VR pistol part %s DrawModelEx ret=%d", path, drawn);
            ++s_partLog;
        }
    };

    // Slide and loaded magazine share the gun matrix. Their meshes are
    // bone-merged onto v_pistol.slide and v_pistol.clip in the skin writer.
    float gunWorld[3][4]{};
    std::memcpy(gunWorld, gunInfo.pModelToWorld, sizeof(gunWorld));
    drawPart(g_pistol.slideModel, "models/weapons/vr_pistol_slide.mdl", gunWorld, gunInfo.origin, gunInfo.angles);
    if (g_pistol.handModel)
        drawPart(g_pistol.handModel, "models/weapons/vr_hlvr_hands.mdl", gunWorld, gunInfo.origin, gunInfo.angles);
    DrawHlvrFreeHands(modelRender, true, false);

    if (g_pistol.mag == MagLoaded)
    {
        float magWorld[3][4]{};
        std::memcpy(magWorld, gunInfo.pModelToWorld, sizeof(magWorld));
        drawPart(g_pistol.clipModel, "models/weapons/vr_pistol_clip.mdl", magWorld, gunInfo.origin, gunInfo.angles);
    }
    else if (g_pistol.mag == MagWorld)
    {
        float magWorld[3][4]{};
        BuildStudioWorld(g_pistol.worldMagAngles, g_pistol.worldMagOrigin, magWorld);
        drawPart(g_pistol.clipModel, "models/weapons/vr_pistol_clip.mdl", magWorld, g_pistol.worldMagOrigin, g_pistol.worldMagAngles);
    }
    else if (g_pistol.mag == MagInHand)
    {
        Vector hand{};
        QAngle handAng{};
        if (OffHand(this, hand, handAng))
        {
            VMatrix handM{};
            AngleMatrixSource(handAng, hand, handM);
            VMatrix grip{};
            ClipGripMatrix(grip);
            VMatrix magM{};
            handM.MatrixMul(grip.InverseTR(), magM);
            Vector magOrg{};
            QAngle magAng{};
            MatrixToOriginAngles(magM, magOrg, magAng);
            float magWorld[3][4]{};
            BuildStudioWorld(magAng, magOrg, magWorld);
            drawPart(g_pistol.clipModel, "models/weapons/vr_pistol_clip.mdl", magWorld, magOrg, magAng);
        }
    }

    --g_attachDepth;
}

#if 0
bool VR::DrawHlvrPistol_Unused(void* modelRender, const ModelRenderInfo_t& vmInfo)
{
    if (g_pistol.drawFailed)
        return false;
    if (!modelRender || !m_Game || !m_Game->m_ModelRender || IsMenuUp() || EmptyHands())
        return false;
    const char* infoName = nullptr;
    if (vmInfo.pModel && m_Game->m_ModelInfo)
        infoName = m_Game->m_ModelInfo->GetModelName(vmInfo.pModel);
    if (NameHas(infoName, "vr_pistol"))
        return false;
    if (!NameHas(infoName, "glock") && !NameHas(infoName, "Glock") && !GlockHeld(this))
        return false;
    if (!NameHas(infoName, "glock") && !NameHas(infoName, "Glock"))
        return false;
    if (!EnsureModels(this))
        return false;

    VMatrix weapon{};
    Vector origin{};
    QAngle angles{};
    if (!AimGunPose(this, weapon, origin, angles))
        return false;

    int flags = vmInfo.flags;
    if (flags == 0)
        flags = 1;

    auto drawOne = [&](void* model, const Vector& org, const QAngle& ang) -> bool {
        // DrawModelSetup reads info.pModel+0x38 and info.pRenderable. A null
        // renderable faults after the shader mutex is taken; SEH then returns
        // without unlocking, and the next shot waits on that mutex forever.
        ModelRenderInfo_t inf = vmInfo;
        inf.pModel = reinterpret_cast<model_t*>(model);
        inf.origin = org;
        inf.angles = ang;
        inf.pLightingOffset = nullptr;
        float world[3][4]{};
        BuildStudioWorld(ang, org, world);
        inf.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(world);
        HlvrPistolEnterDraw();
        const int drawn = CallDrawModelExSeh(modelRender, &inf);
        HlvrPistolLeaveDraw();
        static int s_retLog;
        if (s_retLog < 6)
        {
            Game::logMsg("HL2VR pistol DrawModelEx ret=%d", drawn);
            ++s_retLog;
        }
        if (drawn < 0)
        {
            Game::logMsg("HL2VR pistol DrawModelEx SEH — keeping v_glock");
            g_pistol.drawFailed = true;
            g_pistol.gun = nullptr;
            return false;
        }
        if (drawn == 0)
            return false;
        return true;
    };

    if (!drawOne(g_pistol.gun, origin, angles))
        return false;

    const Vector back = Column(weapon, 1);
    const float travel = g_pistol.slidePull * kSlideTravel;
    const Vector slideOrg = origin + back * travel;
    if (!drawOne(g_pistol.slideModel, slideOrg, angles))
        return false;

    if (g_pistol.mag == MagLoaded || g_pistol.mag == MagWorld)
    {
        Vector magOrg = origin;
        QAngle magAng = angles;
        if (g_pistol.mag == MagWorld)
        {
            magOrg = g_pistol.worldMagOrigin;
            magAng = g_pistol.worldMagAngles;
        }
        if (!drawOne(g_pistol.clipModel, magOrg, magAng))
            return false;
    }
    else if (g_pistol.mag == MagInHand)
    {
        Vector hand{};
        QAngle handAng{};
        if (OffHand(this, hand, handAng))
        {
            VMatrix handM{};
            AngleMatrixSource(handAng, hand, handM);
            VMatrix grip{};
            ClipGripMatrix(grip);
            VMatrix inv = grip.InverseTR();
            VMatrix magM{};
            handM.MatrixMul(inv, magM);
            Vector magOrg{};
            QAngle magAng{};
            MatrixToOriginAngles(magM, magOrg, magAng);
            if (!drawOne(g_pistol.clipModel, magOrg, magAng))
                return false;
        }
    }

    static int s_drawLog;
    if (s_drawLog < 4)
    {
        Game::logMsg("HL2VR pistol draw origin=(%.1f,%.1f,%.1f) mag=%d chamber=%d slide=%.2f",
            origin.x, origin.y, origin.z, static_cast<int>(g_pistol.mag), g_pistol.chamber, g_pistol.slidePull);
        ++s_drawLog;
    }
    return true;
}

#endif

void VR::UpdateHlvrPistol(uint32_t& buttons)
{
    if (!GlockHeld(this) || !EnsureModels(this) || g_pistol.drawFailed)
    {
        if (g_pistol.seeded)
            ResetInteraction();
        return;
    }

    const DWORD now = GetTickCount();
    float dt = 0.016f;
    if (g_pistol.lastTick != 0)
    {
        dt = static_cast<float>(now - g_pistol.lastTick) * 0.001f;
        if (dt < 0.f || dt > 0.1f)
            dt = 0.016f;
    }
    g_pistol.lastTick = now;

    C_BaseEntity* weapon = m_Game ? m_Game->GetActiveWeaponEntity() : nullptr;
    const int clip = m_Game ? m_Game->ReadWeaponClip(weapon) : -1;
    int reserve = -1;
    if (m_Game)
    {
        int health = 0, armor = 0, hudClip = 0, secondary = 0;
        m_Game->ReadWristHudValues(health, armor, hudClip, reserve, secondary);
    }

    if (!g_pistol.seeded && clip >= 0)
    {
        SeedFromClip(clip);
        if (!g_pistol.logged)
        {
            Game::logMsg("HL2VR pistol on glock clip=%d reserve=%d — reload ejects, off-hand trigger grabs mag and slide",
                clip, reserve);
            g_pistol.logged = true;
        }
    }

    if (g_pistol.waitingReload)
    {
        if (clip > 0)
        {
            const bool wasLocked = g_pistol.slide == SlideLocked;
            g_pistol.mag = MagLoaded;
            g_pistol.lastClip = clip;
            g_pistol.waitingReload = false;
            g_pistol.reloadPulsed = false;
            g_pistol.seeded = true;
            if (wasLocked)
            {
                g_pistol.chamber = 0;
                g_pistol.magAmmo = clip;
                g_pistol.slidePull = 1.f;
            }
            else
            {
                g_pistol.chamber = 1;
                g_pistol.magAmmo = (std::max)(0, clip - 1);
                g_pistol.slide = SlideClosed;
                g_pistol.slidePull = 0.f;
            }
            if (m_Game)
                m_Game->PlayUiSound("weapons/pistol/pistol_reload_finished.wav");
        }
        else if (now >= g_pistol.reloadWaitUntil)
        {
            g_pistol.waitingReload = false;
            g_pistol.reloadPulsed = false;
            Game::logMsg("HL2VR pistol engine reload timed out clip=%d", clip);
        }
    }
    else if (g_pistol.seeded && clip >= 0 && g_pistol.lastClip >= 0)
    {
        if (clip < g_pistol.lastClip)
        {
            int lost = g_pistol.lastClip - clip;
            if (lost > 30)
                lost = 30;
            while (lost-- > 0)
                ConsumeOneFromServerDrop();
        }
        else if (clip > g_pistol.lastClip && g_pistol.mag == MagLoaded)
        {
            SeedFromClip(clip);
        }
        g_pistol.lastClip = clip;
    }

    if (g_pistol.mag == MagWorld && g_pistol.worldMagUntil != 0 && now >= g_pistol.worldMagUntil)
    {
        g_pistol.mag = MagNone;
        g_pistol.magAmmo = 0;
        g_pistol.worldMagUntil = 0;
    }

    VMatrix gunMatrix{};
    Vector gunOrigin{};
    QAngle gunAngles{};
    const bool haveGun = AimGunPose(this, gunMatrix, gunOrigin, gunAngles);
    Vector off{};
    QAngle offAng{};
    const bool haveOff = OffHand(this, off, offAng);

    const bool leftHanded = bmvr::g_LeftHanded;
    const bool grip = leftHanded
        ? PressedDigitalAction(m_ActionSecondaryAttack)
        : PressedDigitalAction(m_ActionUse);
    const bool reload = PressedDigitalAction(m_ActionReload);
    const bool gripEdge = grip && !g_pistol.gripWas;
    const bool reloadEdge = reload && !g_pistol.reloadWas;

    bool usedGrip = false;
    if (haveGun && haveOff && !g_pistol.waitingReload)
    {
        const Vector muzzleFwd = Column(gunMatrix, 1) * -1.f;
        const Vector slidePoint = TransformPoint(gunMatrix, kSlideGrabLocal);
        const Vector reloadPoint = TransformPoint(gunMatrix, kGunReloadLocal);
        const bool nearSlide = (off - slidePoint).LengthSqr() <= kSlideGrabRadius * kSlideGrabRadius;
        const bool nearDrop = g_pistol.mag == MagWorld
            && (off - g_pistol.worldMagOrigin).LengthSqr() <= kMagGrabRadius * kMagGrabRadius;

        Vector shoulder = m_SetupOrigin;
        if (m_HmdRight.LengthSqr() > 0.01f)
        {
            const float side = leftHanded ? 8.f : -8.f;
            shoulder += m_HmdRight * side;
            shoulder += m_HmdUp * -6.f;
        }
        const bool nearShoulder = (off - shoulder).LengthSqr() <= kShoulderRadius * kShoulderRadius;

        if (g_pistol.mag != MagInHand && (nearSlide || g_pistol.slideGrabbed))
        {
            if (grip && (nearSlide || g_pistol.slideGrabbed))
            {
                usedGrip = true;
                if (!g_pistol.slideGrabbed)
                {
                    g_pistol.slideGrabbed = true;
                    g_pistol.slideGrabStart = off;
                    g_pistol.slidePulledFar = false;
                    if (m_Game)
                        m_Game->PlayUiSound("weapons/pistol/Pistol_SlidePulled_DAv01.wav");
                }
                const float delta = DotProduct(muzzleFwd, g_pistol.slideGrabStart - off);
                float pull = delta / kSlideTravel;
                if (pull < 0.f) pull = 0.f;
                if (pull > 1.f) pull = 1.f;
                g_pistol.slidePull = pull;
                if (delta >= kSlidePullThreshold)
                    g_pistol.slidePulledFar = true;
            }
            else if (g_pistol.slideGrabbed)
            {
                g_pistol.slideGrabbed = false;
                if (g_pistol.slidePulledFar)
                {
                    FinishPullback(this);
                    g_pistol.slidePulledFar = false;
                    if (m_Game)
                        m_Game->PlayUiSound("weapons/pistol/Pistol_Slide_DAv01r01.wav");
                }
            }
        }
        else if (g_pistol.slideGrabbed && !grip)
        {
            g_pistol.slideGrabbed = false;
        }

        if (!g_pistol.slideGrabbed && g_pistol.mag == MagInHand)
        {
            usedGrip = grip;
            VMatrix handM{};
            AngleMatrixSource(offAng, off, handM);
            VMatrix clipGrip{};
            ClipGripMatrix(clipGrip);
            VMatrix magM{};
            handM.MatrixMul(clipGrip.InverseTR(), magM);
            const Vector clipReload = TransformPoint(magM, kClipReloadLocal);
            if (grip && (clipReload - reloadPoint).LengthSqr() <= kInsertDistance * kInsertDistance)
            {
                const int carried = g_pistol.magAmmo;
                g_pistol.mag = MagLoaded;
                g_pistol.magAmmo = carried;
                if (m_Game)
                    m_Game->PlayUiSound("weapons/pistol/pistol_clip_insert.wav");
                PulseHandHaptic(leftHanded
                    ? vr::TrackedControllerRole_RightHand
                    : vr::TrackedControllerRole_LeftHand, 1200, 0.7f);
                const bool needEngine = (reserve > 0 || reserve < 0)
                    && (clip < 0 || clip < kMagCapacity);
                if (needEngine && (clip <= 0 || g_pistol.slide == SlideLocked || g_pistol.chamber == 0))
                {
                    g_pistol.waitingReload = true;
                    g_pistol.reloadPulsed = false;
                    g_pistol.reloadWaitUntil = now + kReloadWaitMs;
                }
                else if (clip > 0 && g_pistol.slide == SlideLocked)
                {
                    g_pistol.chamber = 0;
                    g_pistol.magAmmo = clip;
                }
                else if (clip > 0)
                {
                    g_pistol.chamber = 1;
                    g_pistol.magAmmo = (std::max)(0, clip - 1);
                }
                Game::logMsg("HL2VR pistol mag seated carried=%d clip=%d engineReload=%d",
                    carried, clip, needEngine ? 1 : 0);
            }
        }
        else if (!g_pistol.slideGrabbed && gripEdge && g_pistol.mag == MagWorld && nearDrop)
        {
            usedGrip = true;
            g_pistol.mag = MagInHand;
            g_pistol.worldMagUntil = 0;
        }
        else if (!g_pistol.slideGrabbed && gripEdge && g_pistol.mag == MagNone && nearShoulder
            && (reserve > 0 || clip <= 0))
        {
            usedGrip = true;
            g_pistol.mag = MagInHand;
            if (clip <= 0)
                g_pistol.magAmmo = (reserve > 0) ? (std::min)(reserve, kMagCapacity) : kMagCapacity;
            else
                g_pistol.magAmmo = (std::max)(0, clip - 1);
            if (m_Game)
                m_Game->PlayUiSound("weapons/pistol/pistol_clip_insert.wav");
        }

        if (reloadEdge && g_pistol.mag == MagLoaded && !g_pistol.slideGrabbed)
        {
            g_pistol.mag = MagWorld;
            g_pistol.worldMagOrigin = TransformPoint(gunMatrix, kClipEjectLocal);
            g_pistol.worldMagAngles = gunAngles;
            g_pistol.worldMagUntil = now + kDroppedMagMs;
            if (m_Game)
                m_Game->PlayUiSound("weapons/pistol/pistol_clip_release.wav");
            PulseAimHaptic(1000);
            Game::logMsg("HL2VR pistol mag ejected ammo=%d chamber=%d clip=%d",
                g_pistol.magAmmo, g_pistol.chamber, clip);
        }
    }
    else if (!grip)
    {
        g_pistol.slideGrabbed = false;
    }

    LerpSlide(dt);
    g_pistol.gripWas = grip;
    g_pistol.reloadWas = reload;

    const bool canFire = g_pistol.seeded
        && !g_pistol.waitingReload
        && !g_pistol.slideGrabbed
        && g_pistol.slide == SlideClosed
        && g_pistol.chamber > 0
        && g_pistol.mag != MagInHand;
    if (!canFire)
        buttons &= ~IN_ATTACK;
    const bool attack = PressedDigitalAction(m_ActionPrimaryAttack);
    if (attack && !g_pistol.attackWas && g_pistol.chamber <= 0 && g_pistol.mag != MagInHand)
    {
        g_pistol.slide = SlideLocked;
        if (m_Game)
            m_Game->PlayUiSound("weapons/pistol/Pistol_Slide_DAv01r01.wav");
    }
    g_pistol.attackWas = attack;

    buttons &= ~IN_RELOAD;
    if (g_pistol.waitingReload && !g_pistol.reloadPulsed)
    {
        buttons |= IN_RELOAD;
        g_pistol.reloadPulsed = true;
    }
    if (usedGrip)
    {
        if (leftHanded)
            buttons &= ~IN_ATTACK2;
        else
            buttons &= ~IN_USE;
    }
}
