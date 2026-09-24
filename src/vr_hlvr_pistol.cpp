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
            return true;
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
        if (g_pistol.gun && g_pistol.slideModel && g_pistol.clipModel)
        {
            Game::logMsg("HL2VR pistol loaded vr_pistol / slide / clip tries=%d", g_pistol.loadTries);
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
