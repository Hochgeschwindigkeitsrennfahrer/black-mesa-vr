#include "vr.h"
#include "game.h"
#include "sdk.h"
#include "trace.h"
#include "in_buttons.h"

#include <algorithm>
#include <cstring>

namespace
{
    constexpr float kSwingThresholdIps = 400.f; // hlvr_crowbar_swing_threshold
    constexpr float kMotionCheckRateSec = 0.01f;
    constexpr float kBludgeonHullDim = 10.f;
    constexpr float kShaftHullDim = 2.0f;
    constexpr float kPastTipHu = 3.f;
    constexpr float kCrowbarModelScale = 1.60f;
    constexpr float kCrowbarRefireSec = 0.4f;
    constexpr DWORD kAttackPulseMs = 180;
    constexpr float kSwingSoundIps = 160.f;
    const Vector kGripLocal(-3.f, 0.f, 0.f);

    // HL2VR vr_crowbar.mdl bind-pose attachment "grip" (model space, Source 3x4).
    // Column 0 = attachment forward, column 1 = left, column 2 = up.
    void GripLocalMatrix(VMatrix& out)
    {
        out.Identity();
        out.m[0][0] = 0.f;  out.m[0][1] = 0.f;   out.m[0][2] = 1.f; out.m[0][3] = -3.f;
        out.m[1][0] = 0.f;  out.m[1][1] = -1.f;  out.m[1][2] = 0.f; out.m[1][3] = 0.f;
        out.m[2][0] = 1.f;  out.m[2][1] = 0.f;   out.m[2][2] = 0.f; out.m[2][3] = 0.f;
        out.m[3][0] = 0.f;  out.m[3][1] = 0.f;   out.m[3][2] = 0.f; out.m[3][3] = 1.f;
    }

    // Bone v_crowbar.top in model space (attachment origin is identity on that bone).
    const Vector kTopLocal(15.016f, 0.f, 6.078f);

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

    Vector WorldToYawLocal(const Vector& world, const Vector& body, float yawDeg)
    {
        const Vector d = world - body;
        float s = 0.f, c = 1.f;
        SinCos(DEG2RAD(-yawDeg), &s, &c);
        return Vector(d.x * c - d.y * s, d.x * s + d.y * c, d.z);
    }

    bool NameHas(const char* hay, const char* needle)
    {
        return hay && needle && std::strstr(hay, needle) != nullptr;
    }

    bool LooksLikeSmallMeleeTarget(Game* game, void* ent)
    {
        if (!game || !ent)
            return false;
        auto hit = [](const char* s) -> bool {
            if (!s || !s[0])
                return false;
            return std::strstr(s, "headcrab") || std::strstr(s, "Headcrab")
                || std::strstr(s, "zombie") || std::strstr(s, "Zombie")
                || std::strstr(s, "antlion") || std::strstr(s, "Antlion")
                || std::strstr(s, "manhack") || std::strstr(s, "Manhack")
                || std::strstr(s, "cscanner") || std::strstr(s, "scanner")
                || std::strstr(s, "snark") || std::strstr(s, "houndeye")
                || std::strstr(s, "bullsquid") || std::strstr(s, "ichthy");
        };
        auto* base = static_cast<C_BaseEntity*>(ent);
        return hit(game->GetEntityClientClassName(base)) || hit(game->GetEntityModelName(base));
    }

    // Isolated so DrawPhysicalCrowbar can keep C++ objects with destructors
    // (lock_guard) in the caller. MSVC forbids __try in those functions.
    int DrawPhysicalCrowbarModelSeh(void* modelRender, int flags, void* renderable,
        int entityIndex, void* model, const Vector* origin, const QAngle* angles,
        const float* world3x4)
    {
        if (!modelRender || !model || !origin || !angles)
            return -1;
        auto* render = static_cast<IModelRender*>(modelRender);
        const matrix3x4_t* xform = world3x4
            ? reinterpret_cast<const matrix3x4_t*>(world3x4) : nullptr;
        int drawn = 0;
        __try
        {
            drawn = render->DrawModel(
                flags,
                renderable,
                static_cast<int>(0xFFFFu),
                entityIndex,
                model,
                *origin,
                *angles,
                0,
                0,
                0,
                xform,
                nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return drawn;
    }

    void BuildScaledStudioWorld(const QAngle& ang, const Vector& org, float scale, float out[3][4])
    {
        Vector fwd, right, up;
        QAngle::AngleVectors(ang, &fwd, &right, &up);
        out[0][0] = fwd.x * scale;   out[0][1] = -right.x * scale; out[0][2] = up.x * scale; out[0][3] = org.x;
        out[1][0] = fwd.y * scale;   out[1][1] = -right.y * scale; out[1][2] = up.y * scale; out[1][3] = org.y;
        out[2][0] = fwd.z * scale;   out[2][1] = -right.z * scale; out[2][2] = up.z * scale; out[2][3] = org.z;
    }
}

void VR::EnsurePhysicalCrowbarModel()
{
    if (m_PhysicalCrowbarModel || !m_Game)
        return;
    // RegisterDynamicModel can return before the filesystem has the mdl;
    // retry for a couple of seconds of CreateMove, then give up until LevelInit.
    if (m_PhysicalCrowbarLoadTries >= 180)
        return;
    ++m_PhysicalCrowbarLoadTries;
    m_PhysicalCrowbarModel = m_Game->FindOrLoadModel("models/weapons/vr_crowbar.mdl");
    if (m_PhysicalCrowbarModel)
        Game::logMsg("Physical crowbar loaded models/weapons/vr_crowbar.mdl tries=%d",
            m_PhysicalCrowbarLoadTries);
    else if (m_PhysicalCrowbarLoadTries == 1 || m_PhysicalCrowbarLoadTries == 180)
        Game::logMsg("Physical crowbar model missing (try %d) — keep v_crowbar, HL2VR swing logic still runs",
            m_PhysicalCrowbarLoadTries);
}

bool VR::IsCrowbarWeaponModel(const char* model)
{
    if (!model || !model[0])
        return false;
    return NameHas(model, "crowbar") || NameHas(model, "Crowbar")
        || NameHas(model, "wrench") || NameHas(model, "Wrench");
}

bool VR::IsCrowbarEquipped() const
{
    if (m_EmptyHands || !m_HasHeldWeapon)
        return false;
    return ViewmodelIsCrowbar();
}

bool VR::ComputePhysicalCrowbarPose(Vector& origin, QAngle& angles, Vector& topPos, Vector& topFwd, Vector& shaftStart) const
{
    Vector body{};
    Vector handTracking{};
    QAngle handAng{};
    bool phys = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (!m_ControllerPoseValid)
            return false;
        body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        phys = m_PhysicalRightTrackingValid;
        handTracking = phys ? m_PhysicalRightPosAbs : m_RightControllerPosAbs;
        handAng = phys ? m_PhysicalRightAngAbs : m_RightControllerAngAbs;
    }
    if (body.LengthSqr() <= 1.f)
        return false;

    const Vector handPos = ControllerTrackingToWorld(body, handTracking);
    VMatrix hand{};
    AngleMatrixSource(handAng, handPos, hand);
    VMatrix grip{};
    GripLocalMatrix(grip);
    VMatrix invGrip = grip.InverseTR();
    VMatrix weapon{};
    hand.MatrixMul(invGrip, weapon);
    MatrixToOriginAngles(weapon, origin, angles);
    float nx = 0.f, ny = 0.f, nz = 0.f, nax = 0.f, nay = 0.f, naz = 0.f;
    ApplyViewmodelNumpadExtras(nx, ny, nz, nax, nay, naz);
    if (fabsf(nax) >= 0.01f || fabsf(nay) >= 0.01f || fabsf(naz) >= 0.01f
        || fabsf(nx) >= 0.01f || fabsf(ny) >= 0.01f || fabsf(nz) >= 0.01f)
    {
        Vector fwd, right, up;
        QAngle::AngleVectors(angles, &fwd, &right, &up);
        fwd = VectorRotate(fwd, up, nay);
        right = VectorRotate(right, up, nay);
        fwd = VectorRotate(fwd, right, nax);
        up = VectorRotate(up, right, nax);
        right = VectorRotate(right, fwd, naz);
        up = VectorRotate(up, fwd, naz);
        QAngle::VectorAngles(fwd, up, angles);
        origin -= fwd * nx;
        origin -= right * ny;
        origin -= up * nz;
        AngleMatrixSource(angles, origin, weapon);
    }
    // Scale the mesh around the grip so the handle stays in the palm while
    // the tip grows. Scaling about the model origin would shove the shaft
    // through the HEV glove.
    const Vector topLocal = kTopLocal * kCrowbarModelScale
        + kGripLocal * (1.f - kCrowbarModelScale);
    topPos = TransformPoint(weapon, topLocal);
    shaftStart = TransformPoint(weapon, kGripLocal);
    topFwd = Vector(weapon.m[0][0], weapon.m[1][0], weapon.m[2][0]);
    if (VectorNormalize(topFwd) <= 0.01f)
        QAngle::AngleVectors(angles, &topFwd, nullptr, nullptr);
    return origin.LengthSqr() > 1.f;
}

bool VR::DrawPhysicalCrowbar(void* modelRender, const ModelRenderInfo_t& vmInfo)
{
    thread_local int s_drawDepth = 0;
    if (s_drawDepth > 0)
        return false;
    if (!modelRender || !m_Game || !m_Game->m_ModelRender)
        return false;
    if (IsMenuUp())
        return false;
    const char* infoName = nullptr;
    if (vmInfo.pModel && m_Game->m_ModelInfo)
        infoName = m_Game->m_ModelInfo->GetModelName(vmInfo.pModel);
    if (NameHas(infoName, "vr_crowbar"))
        return false;
    const bool infoCrowbar = NameHas(infoName, "crowbar") || NameHas(infoName, "Crowbar");
    bool heldCrowbar = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        heldCrowbar = NameHas(m_LastViewmodelModel.c_str(), "crowbar")
            || NameHas(m_LastViewmodelModel.c_str(), "Crowbar");
    }
    if (!infoCrowbar && !heldCrowbar)
        return false;
    EnsurePhysicalCrowbarModel();
    if (!m_PhysicalCrowbarModel)
        return false;

    Vector origin{}, top{}, topFwd{}, shaftStart{};
    QAngle angles{};
    if (!ComputePhysicalCrowbarPose(origin, angles, top, topFwd, shaftStart))
        return false;

    int flags = vmInfo.flags;
    if (flags == 0)
        flags = 1; // STUDIO_RENDER
    const Vector drawOrigin = origin + (shaftStart - origin) * (1.f - kCrowbarModelScale);
    float world3x4[3][4]{};
    BuildScaledStudioWorld(angles, drawOrigin, kCrowbarModelScale, world3x4);
    ++s_drawDepth;
    const int drawn = DrawPhysicalCrowbarModelSeh(
        modelRender,
        flags,
        nullptr,
        -1,
        m_PhysicalCrowbarModel,
        &drawOrigin,
        &angles,
        &world3x4[0][0]);
    --s_drawDepth;
    if (drawn < 0)
    {
        Game::logMsg("Physical crowbar DrawModel SEH — disabling mesh swap");
        m_PhysicalCrowbarModel = nullptr;
        m_PhysicalCrowbarLoadTries = 180;
        return false;
    }
    static int s_drawLog;
    if (s_drawLog < 6)
    {
        Game::logMsg("Physical crowbar DrawModel origin=(%.1f,%.1f,%.1f) drawn=%d",
            origin.x, origin.y, origin.z, drawn);
        ++s_drawLog;
    }
    return true;
}

void VR::UpdateCrowbarMelee()
{
    const bool attackWindow = GetTickCount() < m_MeleeAttackUntilMs;
    if (!m_ControllerPoseValid || !m_Game)
    {
        m_PerformingMelee = false;
        m_MeleeBladeAnglesValid = false;
        m_CrowbarLastMotionCheckMs = 0;
        return;
    }

    EnsurePhysicalCrowbarModel();

    // Same crowbar/wrench test as IsCrowbarWeaponModel, classified once in
    // NoteViewmodelModel; the old per-frame std::string copy of the model
    // path under m_ControllerMutex heap-allocated every Present.
    if (!ViewmodelIsCrowbar())
    {
        m_MeleeNewSwing = true;
        m_MeleeHitEntity = nullptr;
        m_PerformingMelee = false;
        m_MeleeBladeAnglesValid = false;
        m_CrowbarLastMotionCheckMs = 0;
        VectorClear(m_CrowbarPrevTipLocal);
        return;
    }

    Vector origin{}, top{}, topFwd{}, shaftStart{};
    QAngle angles{};
    if (!ComputePhysicalCrowbarPose(origin, angles, top, topFwd, shaftStart))
    {
        m_PerformingMelee = attackWindow;
        return;
    }

    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        body = m_SetupOrigin;
    const float yaw = GetViewAngle().y;
    const Vector tipLocal = WorldToYawLocal(top, body, yaw);

    const DWORD nowMs = GetTickCount();
    if (m_CrowbarNextAttackMs != 0 && nowMs < m_CrowbarNextAttackMs)
    {
        m_CrowbarPrevTipLocal = tipLocal;
        m_CrowbarLastMotionCheckMs = nowMs;
        m_PerformingMelee = attackWindow;
        if (!m_PerformingMelee)
            m_MeleeBladeAnglesValid = false;
        return;
    }

    if (m_CrowbarPrevTipLocal.LengthSqr() <= 0.01f || m_CrowbarLastMotionCheckMs == 0)
    {
        m_CrowbarPrevTipLocal = tipLocal;
        m_CrowbarLastMotionCheckMs = nowMs;
        m_PerformingMelee = attackWindow;
        return;
    }

    float dt = static_cast<float>(nowMs - m_CrowbarLastMotionCheckMs) * 0.001f;
    if (dt < kMotionCheckRateSec)
    {
        m_PerformingMelee = attackWindow;
        return;
    }
    if (dt > 0.08f)
        dt = 0.08f;

    Vector motion = tipLocal - m_CrowbarPrevTipLocal;
    const float distHu = VectorNormalize(motion);
    const float velocityIps = distHu / dt;
    const bool isSwinging = velocityIps > kSwingThresholdIps && distHu > 0.05f;

    m_CrowbarPrevTipLocal = tipLocal;
    m_CrowbarLastMotionCheckMs = nowMs;

    if (!isSwinging)
    {
        m_PerformingMelee = attackWindow;
        if (!m_PerformingMelee)
            m_MeleeBladeAnglesValid = false;
        return;
    }

    Vector dir{};
    {
        float s = 0.f, c = 1.f;
        SinCos(DEG2RAD(yaw), &s, &c);
        dir.x = motion.x * c - motion.y * s;
        dir.y = motion.x * s + motion.y * c;
        dir.z = motion.z;
    }
    if (VectorNormalize(dir) <= 0.01f)
        dir = topFwd;

    // Whole-bar contact: HL2VR uses the mdl's vphysics hull. We don't own a
    // server physics object, so sweep a tight hull along the visible shaft.
    Vector shaftEnd = top + topFwd * kPastTipHu;
    C_BaseEntity* player = nullptr;
    if (m_Game->m_EngineClient)
        player = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
    CTraceFilterSkipSelf filter(player, 0);
    CGameTrace tr{};
    tr.fraction = 1.f;
    tr.m_pEnt = nullptr;
    bool hit = false;
    if (m_Game->m_EngineTrace)
    {
        const Vector shaftMins(-kShaftHullDim, -kShaftHullDim, -kShaftHullDim);
        const Vector shaftMaxs(kShaftHullDim, kShaftHullDim, kShaftHullDim);
        Ray_t shaft;
        shaft.Init(shaftStart, shaftEnd, shaftMins, shaftMaxs);
        m_Game->m_EngineTrace->TraceRay(shaft, MASK_SHOT_HULL, &filter, &tr);
        hit = tr.fraction < 1.f && tr.m_pEnt && tr.m_pEnt != player;
        if (!hit)
        {
            const Vector tipStart = top - dir * 2.f;
            const Vector tipEnd = top + dir * (2.f + kPastTipHu);
            Ray_t tip;
            tip.Init(tipStart, tipEnd);
            CGameTrace tipTr{};
            tipTr.fraction = 1.f;
            tipTr.m_pEnt = nullptr;
            m_Game->m_EngineTrace->TraceRay(tip, MASK_SHOT_HULL, &filter, &tipTr);
            if (tipTr.fraction < 1.f && tipTr.m_pEnt && tipTr.m_pEnt != player)
            {
                std::memcpy(&tr, &tipTr, sizeof(tr));
                hit = true;
            }
        }
        if (!hit)
        {
            const float hullR = 1.732f * kBludgeonHullDim;
            const Vector hullMins(-kBludgeonHullDim, -kBludgeonHullDim, -kBludgeonHullDim);
            const Vector hullMaxs(kBludgeonHullDim, kBludgeonHullDim, kBludgeonHullDim);
            const Vector hullStart = top + dir * hullR;
            Ray_t hull;
            hull.Init(hullStart, shaftEnd, hullMins, hullMaxs);
            CGameTrace hullTr{};
            hullTr.fraction = 1.f;
            hullTr.m_pEnt = nullptr;
            m_Game->m_EngineTrace->TraceRay(hull, MASK_SHOT_HULL, &filter, &hullTr);
            if (hullTr.fraction < 1.f && hullTr.m_pEnt && hullTr.m_pEnt != player
                && LooksLikeSmallMeleeTarget(m_Game, hullTr.m_pEnt))
            {
                std::memcpy(&tr, &hullTr, sizeof(tr));
                hit = true;
            }
        }
    }

    if (velocityIps > kSwingSoundIps && nowMs >= m_CrowbarNextSwingSoundMs)
    {
        m_Game->EmitPlayerSound("Weapon_Crowbar.Single");
        m_CrowbarNextSwingSoundMs = nowMs + static_cast<DWORD>(kCrowbarRefireSec * 1000.f);
    }

    if (!hit)
    {
        m_PerformingMelee = attackWindow;
        if (!m_PerformingMelee)
            m_MeleeBladeAnglesValid = false;
        return;
    }

    m_MeleeHitEntity = tr.m_pEnt;
    // Client Weapon_ShootPosition can start at the bar. Server melee still
    // comes from the player's eyes along cmd viewangles, so aim from the
    // view origin at the contact so that 56 hu ray can actually land.
    Vector hitPos = tr.endpos;
    if (hitPos.LengthSqr() <= 1.f)
        hitPos = top;
    // Hull contact is on the box skin. Aim the server melee ray at the
    // shaft centreline so hits don't register high / wide / past the tip.
    {
        Vector along = shaftEnd - shaftStart;
        const float len = VectorNormalize(along);
        if (len > 0.01f)
        {
            const float t = std::clamp(DotProduct(hitPos - shaftStart, along), 0.f, len);
            hitPos = shaftStart + along * t;
        }
    }
    Vector eye = m_SetupOrigin;
    if (eye.LengthSqr() <= 1.f)
        eye = body;
    Vector toHit = hitPos - eye;
    if (VectorNormalize(toHit) <= 0.01f)
        toHit = dir;
    m_MeleeTraceOrigin = hitPos;
    QAngle::VectorAngles(toHit, m_MeleeBladeAngles);
    if (m_MeleeBladeAngles.x > 180.f) m_MeleeBladeAngles.x -= 360.f;
    if (m_MeleeBladeAngles.x < -180.f) m_MeleeBladeAngles.x += 360.f;
    if (m_MeleeBladeAngles.x < -89.f) m_MeleeBladeAngles.x = -89.f;
    if (m_MeleeBladeAngles.x > 89.f) m_MeleeBladeAngles.x = 89.f;
    m_MeleeBladeAngles.z = 0.f;
    m_MeleeBladeAnglesValid = true;
    m_MeleeAttackUntilMs = nowMs + kAttackPulseMs;
    m_CrowbarNextAttackMs = nowMs + static_cast<DWORD>(kCrowbarRefireSec * 1000.f);
    m_PerformingMelee = true;
    PulseAimHaptic(3999);

    static int s_hitLog;
    if (s_hitLog < 16)
    {
        Game::logMsg("Crowbar MotionSwing hit vel=%.0f in/s origin=(%.1f,%.1f,%.1f) frac=%.2f",
            velocityIps, m_MeleeTraceOrigin.x, m_MeleeTraceOrigin.y, m_MeleeTraceOrigin.z, tr.fraction);
        ++s_hitLog;
    }
}
