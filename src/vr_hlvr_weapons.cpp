#include "vr.h"
#include "game.h"
#include "sdk.h"
#include "bmvr_flags.h"
#include "in_buttons.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr int kMaxBones = 96;
    DWORD g_HlvrRightHandMs = 0;
    DWORD g_HlvrLeftHandMs = 0;

    void NoteHandSide(const char* name)
    {
        if (!name)
            return;
        const DWORD now = GetTickCount();
        if (std::strncmp(name, "ValveBiped.Bip01_R_", 19) == 0)
            g_HlvrRightHandMs = now;
        else if (std::strncmp(name, "ValveBiped.Bip01_L_", 19) == 0)
            g_HlvrLeftHandMs = now;
    }

    struct Pose
    {
        bool ready = false;
        bool failed = false;
        int num = 0;
        float bone[kMaxBones][3][4]{};
        char name[kMaxBones][40]{};
        float grip[3]{};
        float gripMat[3][4]{};
        bool haveGripMat = false;
        float foregrip[3]{};
        bool haveForegrip = false;
        float reload[3]{};
        bool haveReload = false;
        float top[3]{};
        bool haveTop = false;
        float muzzle[3]{1.f, 0.f, 0.f};
        float muzzlePoint[3]{};
        bool haveGrip = false;
        bool haveMuzzle = false;
        bool haveMuzzlePoint = false;
        float hudMat[3][4]{};
        bool haveHud = false;
        float hudL[3][4]{};
        bool haveHudL = false;
        float hudR[3][4]{};
        bool haveHudR = false;
        bool animFound = false;
    };

    struct Part
    {
        const char* path = nullptr;
        void* model = nullptr;
    };

    struct Weapon
    {
        const char* needle = nullptr;
        const char* path = nullptr;
        Part parts[3]{};
        void* model = nullptr;
        Pose pose{};
        int loadTries = 0;
    };

    Weapon g_weapons[] = {
        {"crowbar", "models/weapons/vr_crowbar.mdl", {{nullptr, nullptr}}, nullptr, {}, 0},
        {"shotgun", "models/weapons/vr_shotgun.mdl",
            {{"models/weapons/vr_shotgun_foregrip.mdl", nullptr}, {nullptr, nullptr}, {nullptr, nullptr}},
            nullptr, {}, 0},
        {"mp5", "models/weapons/vr_smg.mdl",
            {{"models/weapons/vr_smg_clip.mdl", nullptr},
             {"models/weapons/vr_smg_foregrip.mdl", nullptr},
             {"models/weapons/vr_smg_charginghandle.mdl", nullptr}},
            nullptr, {}, 0},
        {"grenade", "models/weapons/vr_grenade.mdl", {{nullptr, nullptr}}, nullptr, {}, 0},
    };

    void* g_hands = nullptr;
    Pose g_slidePull[31];
    int g_slidePullCount = 0;
    bool g_slidePullFailed = false;
    Pose g_handBind{};
    float g_gunWorld[3][4]{};
    bool g_gunWorldValid = false;
    const Pose* g_activePose = nullptr;
    bool g_freeActive = false;
    int g_freeCount = 0;
    float g_freeBones[kMaxBones][3][4]{};
    int g_freeDepth = 0;

    enum class SmgMag { Loaded, Hand, World };
    struct SmgMagState
    {
        SmgMag state = SmgMag::Loaded;
        bool gripWas = false;
        bool reloadWas = false;
        Vector origin{};
        QAngle angles{};
        Vector vel{};
        float floorZ = 0.f;
        DWORD until = 0;
    };
    SmgMagState g_smg;

    const unsigned char* StudioHdr(void* model);
    void BuildPose(const unsigned char* hdr, Pose& pose, const char* animWant = nullptr);

    Pose* CachedPartPose(void* model)
    {
        struct Slot { void* model = nullptr; Pose pose{}; };
        static Slot slots[8]{};
        for (Slot& slot : slots)
        {
            if (slot.model == model)
                return &slot.pose;
        }
        for (Slot& slot : slots)
        {
            if (slot.model)
                continue;
            slot.model = model;
            const unsigned char* hdr = StudioHdr(model);
            if (!hdr)
                return nullptr;
            __try { BuildPose(hdr, slot.pose); }
            __except (EXCEPTION_EXECUTE_HANDLER) { slot.pose.failed = true; }
            return &slot.pose;
        }
        return nullptr;
    }

    struct Cache
    {
        void* obj = nullptr;
        void** vt = nullptr;
    };

    Cache FindCache()
    {
        Cache out{};
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
        __try { h = *reinterpret_cast<unsigned short*>(static_cast<char*>(model) + 0x38); }
        __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
        return h;
    }

    bool HardwareReady(void* model, const char* path)
    {
        if (!model || !path)
            return false;
        static Cache cache{};
        static bool tried = false;
        if (!tried)
        {
            cache = FindCache();
            tried = true;
        }
        unsigned short handle = StudioHandle(model);
        if ((handle == 0 || handle == 0xFFFF) && cache.obj && cache.vt && cache.vt[9])
        {
            using FindFn = unsigned short(__thiscall*)(void*, const char*);
            __try { handle = reinterpret_cast<FindFn>(cache.vt[9])(cache.obj, path); }
            __except (EXCEPTION_EXECUTE_HANDLER) { handle = 0; }
            if (handle != 0 && handle != 0xFFFF)
            {
                __try { *reinterpret_cast<unsigned short*>(static_cast<char*>(model) + 0x38) = handle; }
                __except (EXCEPTION_EXECUTE_HANDLER) { handle = 0; }
            }
        }
        if (!handle || handle == 0xFFFF || !cache.obj || !cache.vt || !cache.vt[14])
            return false;
        using HwFn = void*(__thiscall*)(void*, unsigned short);
        void* hw = nullptr;
        __try { hw = reinterpret_cast<HwFn>(cache.vt[14])(cache.obj, handle); }
        __except (EXCEPTION_EXECUTE_HANDLER) { hw = nullptr; }
        return hw != nullptr;
    }

    const unsigned char* StudioHdr(void* model)
    {
        unsigned short handle = StudioHandle(model);
        static Cache cache{};
        static bool tried = false;
        if (!tried)
        {
            cache = FindCache();
            tried = true;
        }
        if (!handle || handle == 0xFFFF || !cache.obj || !cache.vt || !cache.vt[13])
            return nullptr;
        using HdrFn = const unsigned char*(__thiscall*)(void*, unsigned short);
        const unsigned char* hdr = nullptr;
        __try { hdr = reinterpret_cast<HdrFn>(cache.vt[13])(cache.obj, handle); }
        __except (EXCEPTION_EXECUTE_HANDLER) { hdr = nullptr; }
        return hdr;
    }

    float Half(unsigned short h)
    {
        const unsigned sign = (h >> 15) & 1;
        const unsigned exp = (h >> 10) & 0x1F;
        const unsigned mant = h & 0x3FF;
        if (exp == 0)
            return mant == 0 ? 0.f : (sign ? -1.f : 1.f) * (static_cast<float>(mant) / 1024.f) * (1.f / 16384.f);
        if (exp == 31)
            return 0.f;
        const float val = (1.f + static_cast<float>(mant) / 1024.f) * std::ldexp(1.f, static_cast<int>(exp) - 15);
        return sign ? -val : val;
    }

    void Quat64(const unsigned char* raw, float q[4])
    {
        unsigned long long v = 0;
        std::memcpy(&v, raw, 8);
        auto c = [](unsigned n) { return (static_cast<int>(n) - 1048576) * (1.f / 1048576.5f); };
        q[0] = c(static_cast<unsigned>(v & 0x1FFFFF));
        q[1] = c(static_cast<unsigned>((v >> 21) & 0x1FFFFF));
        q[2] = c(static_cast<unsigned>((v >> 42) & 0x1FFFFF));
        const float w2 = 1.f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
        q[3] = w2 > 0.f ? std::sqrt(w2) : 0.f;
        if ((v >> 63) & 1)
            q[3] = -q[3];
    }

    void QuatMatrix(const float q[4], const float pos[3], float out[3][4])
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        out[0][0] = 1.f - 2.f * (y * y + z * z);
        out[0][1] = 2.f * (x * y - w * z);
        out[0][2] = 2.f * (x * z + w * y);
        out[0][3] = pos[0];
        out[1][0] = 2.f * (x * y + w * z);
        out[1][1] = 1.f - 2.f * (x * x + z * z);
        out[1][2] = 2.f * (y * z - w * x);
        out[1][3] = pos[1];
        out[2][0] = 2.f * (x * z - w * y);
        out[2][1] = 2.f * (y * z + w * x);
        out[2][2] = 1.f - 2.f * (x * x + y * y);
        out[2][3] = pos[2];
    }

    void Mul(const float a[3][4], const float b[3][4], float out[3][4])
    {
        float t[3][4];
        for (int r = 0; r < 3; ++r)
        {
            t[r][0] = a[r][0] * b[0][0] + a[r][1] * b[1][0] + a[r][2] * b[2][0];
            t[r][1] = a[r][0] * b[0][1] + a[r][1] * b[1][1] + a[r][2] * b[2][1];
            t[r][2] = a[r][0] * b[0][2] + a[r][1] * b[1][2] + a[r][2] * b[2][2];
            t[r][3] = a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3];
        }
        std::memcpy(out, t, sizeof(t));
    }

    void TransformPoint(const float m[3][4], const float p[3], float out[3])
    {
        out[0] = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
        out[1] = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
        out[2] = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
    }

    void TransformVector(const float m[3][4], const float p[3], float out[3])
    {
        out[0] = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2];
        out[1] = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2];
        out[2] = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2];
    }

    void BuildPose(const unsigned char* hdr, Pose& pose, const char* animWant);

    int FindBone(const Pose& pose, const char* name)
    {
        if (!name)
            return -1;
        for (int i = 0; i < pose.num; ++i)
        {
            if (std::strcmp(pose.name[i], name) == 0)
                return i;
        }
        return -1;
    }

    void BuildPose(const unsigned char* hdr, Pose& pose, const char* animWant)
    {
        if (!hdr || pose.ready || pose.failed)
            return;
        const int num = *reinterpret_cast<const int*>(hdr + 156);
        const int boneIndex = *reinterpret_cast<const int*>(hdr + 160);
        if (num < 1 || num > kMaxBones)
        {
            pose.failed = true;
            return;
        }
        struct Local { int parent; float pos[3]; float q[4]; };
        Local locals[kMaxBones]{};
        for (int i = 0; i < num; ++i)
        {
            const unsigned char* b = hdr + boneIndex + i * 216;
            const int sz = *reinterpret_cast<const int*>(b);
            locals[i].parent = *reinterpret_cast<const int*>(b + 4);
            std::memcpy(locals[i].pos, b + 32, sizeof(float) * 3);
            std::memcpy(locals[i].q, b + 44, sizeof(float) * 4);
            const char* nm = reinterpret_cast<const char*>(b + sz);
            strncpy_s(pose.name[i], nm, _TRUNCATE);
        }
        const int numAnim = *reinterpret_cast<const int*>(hdr + 180);
        const int animIndex = *reinterpret_cast<const int*>(hdr + 184);
        const unsigned char* animPtr = nullptr;
        for (int i = 0; i < numAnim && i < 80; ++i)
        {
            const unsigned char* desc = hdr + animIndex + i * 100;
            const int sz = *reinterpret_cast<const int*>(desc + 4);
            if (sz <= 0 || sz > 200000)
                continue;
            const char* name = reinterpret_cast<const char*>(desc + sz);
            const bool match = (animWant && animWant[0])
                ? std::strcmp(name, animWant) == 0
                : (std::strcmp(name, "@idle") == 0 || std::strcmp(name, "idle") == 0);
            if (match)
            {
                const int rel = *reinterpret_cast<const int*>(desc + 56);
                animPtr = desc + rel;
                pose.animFound = true;
                break;
            }
        }
        bool hasQuat[kMaxBones]{};
        bool hasPos[kMaxBones]{};
        float aquat[kMaxBones][4]{};
        float apos[kMaxBones][3]{};
        if (animPtr)
        {
            const unsigned char* p = animPtr;
            for (int n = 0; n < kMaxBones; ++n)
            {
                const int bone = p[0];
                const unsigned flags = p[1];
                const unsigned short nxt = *reinterpret_cast<const unsigned short*>(p + 2);
                const unsigned char* data = p + 4;
                if (bone >= 0 && bone < num)
                {
                    if (flags & 0x20)
                    {
                        Quat64(data, aquat[bone]);
                        hasQuat[bone] = true;
                        data += 8;
                    }
                    if (flags & 0x01)
                    {
                        const auto* h = reinterpret_cast<const unsigned short*>(data);
                        apos[bone][0] = Half(h[0]);
                        apos[bone][1] = Half(h[1]);
                        apos[bone][2] = Half(h[2]);
                        hasPos[bone] = true;
                    }
                }
                if (nxt == 0)
                    break;
                p += nxt;
            }
        }
        for (int i = 0; i < num; ++i)
        {
            float q[4];
            float pos[3];
            std::memcpy(q, hasQuat[i] ? aquat[i] : locals[i].q, sizeof(q));
            std::memcpy(pos, hasPos[i] ? apos[i] : locals[i].pos, sizeof(pos));
            float local[3][4];
            QuatMatrix(q, pos, local);
            if (locals[i].parent < 0 || locals[i].parent >= i)
                std::memcpy(pose.bone[i], local, sizeof(local));
            else
                Mul(pose.bone[locals[i].parent], local, pose.bone[i]);
        }
        pose.num = num;
        const int nAtt = *reinterpret_cast<const int*>(hdr + 0xF0);
        const int attIndex = *reinterpret_cast<const int*>(hdr + 0xF4);
        if (nAtt > 0 && nAtt < 30 && attIndex > 0)
        {
            for (int i = 0; i < nAtt; ++i)
            {
                const unsigned char* a = hdr + attIndex + i * 92;
                const int sz = *reinterpret_cast<const int*>(a);
                if (sz <= 0 || sz > 80000)
                    break;
                const int bone = *reinterpret_cast<const int*>(a + 8);
                const char* nm = reinterpret_cast<const char*>(a + sz);
                float local[3][4];
                std::memcpy(local, a + 12, sizeof(local));
                if (bone < 0 || bone >= num)
                    continue;
                if (std::strcmp(nm, "grip") == 0)
                {
                    float p[3] = {local[0][3], local[1][3], local[2][3]};
                    TransformPoint(pose.bone[bone], p, pose.grip);
                    Mul(pose.bone[bone], local, pose.gripMat);
                    pose.haveGrip = true;
                    pose.haveGripMat = true;
                }
                else if (std::strcmp(nm, "foregrip") == 0)
                {
                    float p[3] = {local[0][3], local[1][3], local[2][3]};
                    TransformPoint(pose.bone[bone], p, pose.foregrip);
                    pose.haveForegrip = true;
                }
                else if (std::strcmp(nm, "reloadpoint") == 0)
                {
                    float p[3] = {local[0][3], local[1][3], local[2][3]};
                    TransformPoint(pose.bone[bone], p, pose.reload);
                    pose.haveReload = true;
                }
                else if (std::strcmp(nm, "top") == 0)
                {
                    float p[3] = {local[0][3], local[1][3], local[2][3]};
                    TransformPoint(pose.bone[bone], p, pose.top);
                    pose.haveTop = true;
                }
                else if (std::strcmp(nm, "hud") == 0 || std::strcmp(nm, "hud_l") == 0 || std::strcmp(nm, "hud_r") == 0)
                {
                    float placed[3][4];
                    Mul(pose.bone[bone], local, placed);
                    if (std::strcmp(nm, "hud_l") == 0)
                    {
                        std::memcpy(pose.hudL, local, sizeof(local));
                        pose.haveHudL = true;
                    }
                    else if (std::strcmp(nm, "hud_r") == 0)
                    {
                        std::memcpy(pose.hudR, local, sizeof(local));
                        pose.haveHudR = true;
                    }
                    else
                    {
                        std::memcpy(pose.hudMat, placed, sizeof(placed));
                        pose.haveHud = true;
                    }
                }
                else if (std::strcmp(nm, "muzzle") == 0)
                {
                    float p[3] = {local[0][3], local[1][3], local[2][3]};
                    TransformPoint(pose.bone[bone], p, pose.muzzlePoint);
                    pose.haveMuzzlePoint = true;
                    // Column 0 is the attachment forward (shotgun -Y, MP5 -X).
                    // The translation is the point, not the barrel axis; using
                    // it as a direction lifted shotgun shots above the gun.
                    float dir[3] = {local[0][0], local[1][0], local[2][0]};
                    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    if (len > 0.1f)
                    {
                        dir[0] /= len; dir[1] /= len; dir[2] /= len;
                        TransformVector(pose.bone[bone], dir, pose.muzzle);
                        const float ml = std::sqrt(pose.muzzle[0] * pose.muzzle[0]
                            + pose.muzzle[1] * pose.muzzle[1] + pose.muzzle[2] * pose.muzzle[2]);
                        if (ml > 0.001f)
                        {
                            pose.muzzle[0] /= ml; pose.muzzle[1] /= ml; pose.muzzle[2] /= ml;
                            pose.haveMuzzle = true;
                        }
                    }
                }
            }
        }
        if (!pose.haveMuzzle)
        {
            pose.muzzle[0] = pose.bone[0][0][0];
            pose.muzzle[1] = pose.bone[0][1][0];
            pose.muzzle[2] = pose.bone[0][2][0];
        }
        if (!pose.haveGrip)
        {
            pose.grip[0] = pose.bone[0][0][3];
            pose.grip[1] = pose.bone[0][1][3];
            pose.grip[2] = pose.bone[0][2][3];
        }
        if (animWant && animWant[0] && !pose.animFound)
        {
            pose.failed = true;
            return;
        }
        pose.ready = true;
    }

    Weapon* WeaponForModel(void* model)
    {
        if (!model)
            return nullptr;
        for (Weapon& w : g_weapons)
        {
            if (w.model == model)
                return &w;
        }
        return nullptr;
    }

    bool StockViewmodelFile(const char* name, const char* file)
    {
        if (!name || !file)
            return false;
        const char* base = name;
        for (const char* p = name; *p; ++p)
        {
            if (*p == '/' || *p == '\\')
                base = p + 1;
        }
        return _stricmp(base, file) == 0;
    }

    // Only the stock viewmodel files. "shotgun" also matches vr_shotgun,
    // w_shotgun, and ejected shells; swapping those draws a second gun and
    // drags world props onto the controller.
    Weapon* WeaponForNeedle(const char* name)
    {
        if (StockViewmodelFile(name, "v_crowbar.mdl"))
            return &g_weapons[0];
        if (StockViewmodelFile(name, "v_shotgun.mdl"))
            return &g_weapons[1];
        if (StockViewmodelFile(name, "v_mp5.mdl"))
            return &g_weapons[2];
        if (StockViewmodelFile(name, "v_grenade.mdl"))
            return &g_weapons[3];
        return nullptr;
    }

    void EnsureWeapon(VR* vr, Weapon& w)
    {
        if (w.pose.ready || w.pose.failed || !vr || !vr->m_Game || w.loadTries >= 60)
            return;
        ++w.loadTries;
        if (!w.model)
            w.model = vr->m_Game->FindOrLoadModel(w.path);
        if (!w.model || !HardwareReady(w.model, w.path))
            return;
        const unsigned char* hdr = StudioHdr(w.model);
        if (!hdr)
            return;
        __try { BuildPose(hdr, w.pose); }
        __except (EXCEPTION_EXECUTE_HANDLER) { w.pose.failed = true; }
        if (w.pose.ready)
        {
            Game::logMsg("HL2VR weapon %s idle bones=%d grip=(%.2f,%.2f,%.2f) muzzle=(%.2f,%.2f,%.2f)",
                w.needle, w.pose.num, w.pose.grip[0], w.pose.grip[1], w.pose.grip[2],
                w.pose.muzzle[0], w.pose.muzzle[1], w.pose.muzzle[2]);
        }
        for (Part& part : w.parts)
        {
            if (part.path && !part.model)
            {
                part.model = vr->m_Game->FindOrLoadModel(part.path);
                if (part.model)
                    HardwareReady(part.model, part.path);
            }
        }
    }

    void EnsureHands(VR* vr)
    {
        if (g_hands || !vr || !vr->m_Game || g_handBind.failed)
            return;
        if (!g_hands)
            g_hands = vr->m_Game->FindOrLoadModel("models/weapons/vr_hlvr_hands.mdl");
        if (!g_hands || !HardwareReady(g_hands, "models/weapons/vr_hlvr_hands.mdl"))
            return;
        const unsigned char* hdr = StudioHdr(g_hands);
        if (!hdr)
            return;
        __try { BuildPose(hdr, g_handBind); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_handBind.failed = true; }
    }

    void Collapse(float* rows, int n)
    {
        // Identity rotation. A zero 3x3 makes the shader's normal matrix
        // blow up, so the mesh turns flat cyan, magenta, or fully dark.
        float away[3][4] = {
            {1.f, 0.f, 0.f, 1.0e6f},
            {0.f, 1.f, 0.f, 1.0e6f},
            {0.f, 0.f, 1.f, 1.0e6f},
        };
        for (int i = 0; i < n; ++i)
            std::memcpy(rows + i * 12, away, sizeof(away));
    }

    int DrawEx(void* modelRender, const ModelRenderInfo_t& gunInfo, void* model, const float world[3][4])
    {
        // A null renderable faults inside DrawModelSetup after the shader
        // mutex is taken. SEH then returns without unlocking, and the next
        // shot or weapon select waits on that mutex until the game dies.
        if (!modelRender || !model || !gunInfo.pRenderable || !world)
            return 0;
        void** vt = nullptr;
        __try { vt = *reinterpret_cast<void***>(modelRender); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (!vt || !vt[16])
            return 0;
        ModelRenderInfo_t inf = gunInfo;
        inf.pModel = static_cast<model_t*>(model);
        inf.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(world);
        inf.instance = -1;
        // Light the mesh where it is drawn. The viewmodel entity origin is
        // the eyes, so the hand was sampling a dark or tinted visleaf.
        static float s_light[8][3];
        static int s_lightSlot = 0;
        float* lit = s_light[s_lightSlot++ & 7];
        lit[0] = world[0][3];
        lit[1] = world[1][3];
        lit[2] = world[2][3];
        inf.pLightingOrigin = reinterpret_cast<const Vector*>(lit);
        using ExFn = int(__thiscall*)(void*, void*);
        // Same guard the pistol parts use: Execute must not run the viewmodel
        // relocator, or the hand bones and the depth range stick to world props.
        HlvrPistolEnterDraw();
        int drawn = 0;
        __try { drawn = reinterpret_cast<ExFn>(vt[16])(modelRender, &inf); }
        __except (EXCEPTION_EXECUTE_HANDLER) { drawn = -1; }
        HlvrPistolLeaveDraw();
        return drawn;
    }

    void InvertTR(const float in[3][4], float out[3][4])
    {
        out[0][0] = in[0][0]; out[0][1] = in[1][0]; out[0][2] = in[2][0];
        out[1][0] = in[0][1]; out[1][1] = in[1][1]; out[1][2] = in[2][1];
        out[2][0] = in[0][2]; out[2][1] = in[1][2]; out[2][2] = in[2][2];
        const float tx = -in[0][3], ty = -in[1][3], tz = -in[2][3];
        out[0][3] = tx * out[0][0] + ty * out[0][1] + tz * out[0][2];
        out[1][3] = tx * out[1][0] + ty * out[1][1] + tz * out[1][2];
        out[2][3] = tx * out[2][0] + ty * out[2][1] + tz * out[2][2];
    }
}

void VR::PreloadHlvrWeapons()
{
    if (!m_Game || !m_GameplayEligible)
        return;
    for (Weapon& w : g_weapons)
        EnsureWeapon(this, w);
    EnsureHands(this);
}

void* VR::HlvrWeaponDrawModel(const char* stockModelName)
{
    // DrawModelSetup runs inside the material lock. Loading here deadlocks
    // the shader compiler. CreateMove preloads; until then keep the stock mesh.
    Weapon* w = WeaponForNeedle(stockModelName);
    if (!w || !w->model || !w->pose.ready)
        return nullptr;
    return w->model;
}

bool VR::IsHlvrWeaponModel(void* model) const
{
    return WeaponForModel(model) != nullptr;
}

bool VR::IsHlvrWeaponRigged(void* model) const
{
    if (!model)
        return false;
    if (WeaponForModel(model))
        return true;
    for (const Weapon& w : g_weapons)
    {
        for (const Part& part : w.parts)
        {
            if (part.model && part.model == model)
                return true;
        }
    }
    return model == g_hands;
}

bool VR::HlvrHandsReplaceGloves() const
{
    return HlvrHandCoversGlove(true) && HlvrHandCoversGlove(false);
}

bool VR::HlvrHandCoversGlove(bool rightHand) const
{
    const DWORD stamp = rightHand ? g_HlvrRightHandMs : g_HlvrLeftHandMs;
    if (!stamp || !g_hands || !g_handBind.ready)
        return false;
    const DWORD now = GetTickCount();
    return now - stamp < 120;
}

void VR::BlendHlvrControllerHand(bool right, float* boneRows, int boneCount)
{
    if (!boneRows || boneCount < 1 || !g_handBind.ready)
        return;
    const char* handName = right ? "ValveBiped.Bip01_R_Hand" : "ValveBiped.Bip01_L_Hand";
    const char* prefix = right ? "ValveBiped.Bip01_R_" : "ValveBiped.Bip01_L_";
    const int handBone = FindBone(g_handBind, handName);
    if (handBone < 0 || handBone >= boneCount)
        return;
    Vector track{};
    QAngle ang{};
    bool tracked = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        if (right)
        {
            tracked = m_ControllerPoseValid;
            const bool phys = m_PhysicalRightTrackingValid;
            track = phys ? m_PhysicalRightPosAbs : m_RightControllerPosAbs;
            ang = phys ? m_PhysicalRightAngAbs : m_RightControllerAngAbs;
        }
        else
        {
            tracked = m_LeftControllerTrackingValid || m_ControllerPoseValid;
            track = m_LeftControllerPosAbs;
            ang = m_LeftControllerAngAbs;
        }
    }
    if (!tracked)
        return;
    const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        return;
    const Vector pos = ControllerTrackingToWorld(body, track);
    Vector fwd, side, up;
    QAngle::AngleVectors(ang, &fwd, &side, &up);
    if (right)
    {
        // Both controllers report the same grip orientation. The right-hand
        // skeleton is mirrored, so roll 180° about forward or the empty
        // right hand sits twisted. HL2VR uses Quaternion(1,0,0,0) for this.
        side = side * -1.f;
        up = up * -1.f;
    }
    float handWorld[3][4];
    handWorld[0][0] = fwd.x; handWorld[0][1] = -side.x; handWorld[0][2] = up.x; handWorld[0][3] = pos.x;
    handWorld[1][0] = fwd.y; handWorld[1][1] = -side.y; handWorld[1][2] = up.y; handWorld[1][3] = pos.y;
    handWorld[2][0] = fwd.z; handWorld[2][1] = -side.z; handWorld[2][2] = up.z; handWorld[2][3] = pos.z;
    float inv[3][4];
    InvertTR(g_handBind.bone[handBone], inv);
    float delta[3][4];
    Mul(handWorld, inv, delta);
    const int n = g_handBind.num < boneCount ? g_handBind.num : boneCount;
    for (int i = 0; i < n; ++i)
    {
        if (std::strncmp(g_handBind.name[i], prefix, 19) != 0)
            continue;
        float world[3][4];
        Mul(delta, g_handBind.bone[i], world);
        std::memcpy(boneRows + i * 12, world, sizeof(world));
    }
    if ((right && g_handBind.haveHudR) || (!right && g_handBind.haveHudL))
    {
        // Mesh on this bone spans Y about -2.3..2. The palm is -Y; the
        // back is +Y. Column 0 is the face. Sit it on the back, mid-hand.
        float flat[3][4] = {
            { 0.f, 0.f, 1.f, 1.6f },
            { 1.f, 0.f, 0.f, 1.55f },
            { 0.f, 1.f, 0.f, 0.f },
        };
        float hudWorld[3][4];
        Mul(handWorld, flat, hudWorld);
        NoteHlvrHandHud(right, hudWorld);
    }
    NoteHandSide(prefix);
}

void VR::NoteHlvrHandCover(bool right)
{
    NoteHandSide(right ? "ValveBiped.Bip01_R_" : "ValveBiped.Bip01_L_");
}

void VR::NoteHlvrHandHud(bool right, const float world[3][4])
{
    if (!world || m_StereoEye == 2)
        return;
    const int side = right ? 1 : 0;
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    std::memcpy(m_HandHudMat[side], world, sizeof(m_HandHudMat[side]));
    m_HandHudValid[side] = true;
    m_HandHudMs[side] = GetTickCount();
}

void VR::NoteHlvrWeaponHud(const float world[3][4])
{
    if (!world || m_StereoEye == 2)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    std::memcpy(m_WeaponHudMat, world, sizeof(m_WeaponHudMat));
    m_WeaponHudValid = true;
    m_WeaponHudMs = GetTickCount();
}

void VR::EnsureHlvrSlidePullPoses()
{
    if (g_slidePullCount == 31 || g_slidePullFailed || !m_Game)
        return;
    void* model = m_Game->FindOrLoadModel("models/weapons/vr_pistol.mdl");
    const unsigned char* hdr = StudioHdr(model);
    if (!hdr)
        return;
    for (int i = 0; i < 31; ++i)
    {
        if (g_slidePull[i].ready)
            continue;
        char name[32];
        sprintf_s(name, "a_slide_pull_%02d", i);
        BuildPose(hdr, g_slidePull[i], name);
        if (!g_slidePull[i].ready)
        {
            g_slidePullFailed = true;
            return;
        }
    }
    g_slidePullCount = 31;
}

bool VR::ApplyHlvrSlidePullHand(float pull, const float basis[3][4], const float visualSlide[3][4], float* boneRows, int boneCount)
{
    if (g_slidePullCount != 31 || !basis || !visualSlide || !boneRows || !g_handBind.ready)
        return false;
    if (pull < 0.f) pull = 0.f;
    if (pull > 1.f) pull = 1.f;
    int frame = static_cast<int>(pull * 30.f + 0.5f);
    if (frame < 0) frame = 0;
    if (frame > 30) frame = 30;
    const Pose& pose = g_slidePull[frame];
    const int slideBone = FindBone(pose, "v_pistol.slide");
    if (slideBone < 0)
        return false;
    // Hand relative to the animated slide, then seated on the slide we
    // actually draw. a_slide_pull_00..30 are the authored grip poses.
    float invSlide[3][4];
    InvertTR(pose.bone[slideBone], invSlide);
    const int n = g_handBind.num < boneCount ? g_handBind.num : boneCount;
    bool any = false;
    for (int i = 0; i < n; ++i)
    {
        if (std::strncmp(g_handBind.name[i], "ValveBiped.Bip01_L_", 19) != 0)
            continue;
        const int src = FindBone(pose, g_handBind.name[i]);
        if (src < 0)
            continue;
        float rel[3][4];
        float local[3][4];
        float world[3][4];
        Mul(invSlide, pose.bone[src], rel);
        Mul(visualSlide, rel, local);
        Mul(basis, local, world);
        std::memcpy(boneRows + i * 12, world, sizeof(world));
        any = true;
    }
    if (any)
        NoteHandSide("ValveBiped.Bip01_L_");
    return any;
}

void VR::InvalidateHlvrHeldPose()
{
    g_gunWorldValid = false;
    g_activePose = nullptr;
    g_freeActive = false;
}

bool VR::ApplyHlvrPistolSupportHand(void* pistolModel, const float basis[3][4], float* boneRows, int boneCount)
{
    if (!pistolModel || !basis || !boneRows || boneCount < 1 || !g_handBind.ready)
        return false;
    static Pose pose;
    if (!pose.ready && !pose.failed)
    {
        const unsigned char* hdr = StudioHdr(pistolModel);
        if (!hdr)
            return false;
        __try { BuildPose(hdr, pose); }
        __except (EXCEPTION_EXECUTE_HANDLER) { pose.failed = true; }
    }
    if (!pose.ready)
        return false;
    bool any = false;
    const int n = g_handBind.num < boneCount ? g_handBind.num : boneCount;
    for (int i = 0; i < n; ++i)
    {
        if (std::strncmp(g_handBind.name[i], "ValveBiped.Bip01_L_", 19) != 0)
            continue;
        const int src = FindBone(pose, g_handBind.name[i]);
        if (src < 0)
            continue;
        float world[3][4];
        Mul(basis, pose.bone[src], world);
        std::memcpy(boneRows + i * 12, world, sizeof(world));
        any = true;
    }
    if (any)
        NoteHandSide("ValveBiped.Bip01_L_");
    return any;
}

void VR::ResetHlvrHandDrawState()
{
    // A thrown draw used to leave this above zero, and every later hand
    // draw returned immediately.
    g_freeDepth = 0;
    // A free-hand draw that didn't clear this left every later hand skin
    // copying the world pose from the frame the weapon was equipped.
    g_freeActive = false;
}

int VR::WriteHlvrWeaponSkin(const float modelToWorld[3][4], void* model, float* boneRows, int maxBones)
{
    if (!modelToWorld || !boneRows || maxBones < 1 || !model)
        return 0;
    Weapon* main = WeaponForModel(model);
    const Pose* pose = nullptr;
    float basis[3][4];
    std::memcpy(basis, modelToWorld, sizeof(basis));
    if (main && main->pose.ready)
    {
        g_freeActive = false;
        pose = &main->pose;
        std::memcpy(g_gunWorld, modelToWorld, sizeof(g_gunWorld));
        g_gunWorldValid = true;
        g_activePose = pose;
    }
    else if (g_activePose)
    {
        pose = g_activePose;
        const float span = modelToWorld[0][3] * modelToWorld[0][3]
            + modelToWorld[1][3] * modelToWorld[1][3]
            + modelToWorld[2][3] * modelToWorld[2][3];
        if (span < 4.f && g_gunWorldValid)
            std::memcpy(basis, g_gunWorld, sizeof(basis));
    }
    if (!pose)
        return 0;

    auto slidePump = [&](float local[3][4], const char* boneName)
    {
        if (m_ShotgunPump <= 0.01f || !pose->haveForegrip || !boneName)
            return;
        const bool pumpBone = std::strstr(boneName, "foregrip") != nullptr
            || std::strncmp(boneName, "ValveBiped.Bip01_L_", 19) == 0;
        if (!pumpBone)
            return;
        Vector back(pose->grip[0] - pose->foregrip[0],
            pose->grip[1] - pose->foregrip[1],
            pose->grip[2] - pose->foregrip[2]);
        if (VectorNormalize(back) <= 0.01f)
            return;
        local[0][3] += back.x * m_ShotgunPump;
        local[1][3] += back.y * m_ShotgunPump;
        local[2][3] += back.z * m_ShotgunPump;
    };

    if (model == g_hands && g_freeActive && g_freeCount > 0)
    {
        const int n = g_freeCount < maxBones ? g_freeCount : maxBones;
        std::memcpy(boneRows, g_freeBones, sizeof(float) * 12 * static_cast<size_t>(n));
        return n;
    }
    if (!main)
    {
        const Pose* partPosePtr = (model == g_hands && g_handBind.ready) ? &g_handBind : CachedPartPose(model);
        if (!partPosePtr || !partPosePtr->ready)
            return 0;
        const Pose& partPose = *partPosePtr;
        const int n = partPose.num < maxBones ? partPose.num : maxBones;
        if (n < 1)
            return 0;
        Collapse(boneRows, n);
        bool wroteRight = false;
        bool wroteLeft = false;
        for (int i = 0; i < n; ++i)
        {
            if (model == g_hands)
            {
                const bool rightBone = std::strncmp(partPose.name[i], "ValveBiped.Bip01_R_", 19) == 0;
                const bool leftBone = m_HlvrTwoHand
                    && std::strncmp(partPose.name[i], "ValveBiped.Bip01_L_", 19) == 0;
                if (!rightBone && !leftBone)
                    continue;
            }
            const int src = FindBone(*pose, partPose.name[i]);
            float world[3][4];
            if (src >= 0)
            {
                float local[3][4];
                std::memcpy(local, pose->bone[src], sizeof(local));
                slidePump(local, pose->name[src]);
                Mul(basis, local, world);
                if (model == g_hands)
                {
                    NoteHandSide(partPose.name[i]);
                    if (std::strncmp(partPose.name[i], "ValveBiped.Bip01_R_", 19) == 0)
                        wroteRight = true;
                    else if (std::strncmp(partPose.name[i], "ValveBiped.Bip01_L_", 19) == 0)
                        wroteLeft = true;
                }
            }
            else if (i == 0 && model != g_hands)
                Mul(basis, partPose.bone[0], world);
            else
                continue;
            std::memcpy(boneRows + i * 12, world, sizeof(world));
        }
        if (model == g_hands && g_handBind.ready)
        {
            // A weapon pose that has the palm but not the fingers used to
            // leave those bones collapsed, so the hand drew as a stump.
            auto fillUnwritten = [&](const char* handName, const char* prefix) {
                const int hand = FindBone(partPose, handName);
                if (hand < 0 || hand >= n)
                    return;
                const float* handRow = boneRows + hand * 12;
                const float handSpan = handRow[3] * handRow[3] + handRow[7] * handRow[7] + handRow[11] * handRow[11];
                if (handSpan > 1.0e10f)
                    return;
                float handWorld[3][4];
                std::memcpy(handWorld, handRow, sizeof(handWorld));
                float invBind[3][4];
                InvertTR(g_handBind.bone[hand], invBind);
                for (int i = 0; i < n; ++i)
                {
                    if (std::strncmp(partPose.name[i], prefix, 19) != 0)
                        continue;
                    const float* row = boneRows + i * 12;
                    const float span = row[3] * row[3] + row[7] * row[7] + row[11] * row[11];
                    if (span < 1.0e10f)
                        continue;
                    float rel[3][4];
                    float world[3][4];
                    Mul(invBind, g_handBind.bone[i], rel);
                    Mul(handWorld, rel, world);
                    std::memcpy(boneRows + i * 12, world, sizeof(world));
                }
            };
            if (wroteRight)
                fillUnwritten("ValveBiped.Bip01_R_Hand", "ValveBiped.Bip01_R_");
            if (wroteLeft)
                fillUnwritten("ValveBiped.Bip01_L_Hand", "ValveBiped.Bip01_L_");
            // Authored grip when the weapon pose has that hand. Otherwise
            // the controller, so a missing bone does not hide the hand.
            if (!wroteRight)
                BlendHlvrControllerHand(true, boneRows, n);
            if (!m_HlvrTwoHand || !wroteLeft)
                BlendHlvrControllerHand(false, boneRows, n);
            // Wrist readout sits on the glove, not the controller. Two-handing
            // moves this bone onto the gun while the controller stays back.
            const bool hudRight = bmvr::g_LeftHanded;
            const int hudHand = FindBone(partPose, hudRight
                ? "ValveBiped.Bip01_R_Hand" : "ValveBiped.Bip01_L_Hand");
            if (hudHand >= 0 && hudHand < n)
            {
                const float* row = boneRows + hudHand * 12;
                const float span = row[3] * row[3] + row[7] * row[7] + row[11] * row[11];
                if (span < 1.0e10f)
                {
                    float hw[3][4];
                    std::memcpy(hw, row, sizeof(hw));
                    float hud[3][4];
                    hud[0][0] = hw[0][1]; hud[1][0] = hw[1][1]; hud[2][0] = hw[2][1];
                    hud[0][1] = hw[0][2]; hud[1][1] = hw[1][2]; hud[2][1] = hw[2][2];
                    hud[0][2] = hw[0][0]; hud[1][2] = hw[1][0]; hud[2][2] = hw[2][0];
                    hud[0][3] = hw[0][3] + hw[0][1] * 0.35f + hw[0][0] * 1.2f;
                    hud[1][3] = hw[1][3] + hw[1][1] * 0.35f + hw[1][0] * 1.2f;
                    hud[2][3] = hw[2][3] + hw[2][1] * 0.35f + hw[2][0] * 1.2f;
                    NoteHlvrHandHud(hudRight, hud);
                }
            }
        }
        return n;
    }

    const int n = pose->num < maxBones ? pose->num : maxBones;
    for (int i = 0; i < n; ++i)
    {
        float local[3][4];
        std::memcpy(local, pose->bone[i], sizeof(local));
        slidePump(local, pose->name[i]);
        float world[3][4];
        Mul(basis, local, world);
        std::memcpy(boneRows + i * 12, world, sizeof(world));
    }
    // The weapon mesh also has the HL2VR glove. Leave those bones off so
    // only vr_hlvr_hands.mdl draws the hand, on the same grip.
    {
        float away[3][4] = {
            {1.f, 0.f, 0.f, 1.0e6f},
            {0.f, 1.f, 0.f, 1.0e6f},
            {0.f, 0.f, 1.f, 1.0e6f},
        };
        for (int i = 0; i < n; ++i)
        {
            if (std::strncmp(pose->name[i], "ValveBiped", 10) == 0)
                std::memcpy(boneRows + i * 12, away, sizeof(away));
        }
    }
    if (pose->haveMuzzlePoint)
    {
        float tip[3] = {pose->muzzlePoint[0], pose->muzzlePoint[1], pose->muzzlePoint[2]};
        float muzzleWorld[3];
        TransformPoint(basis, tip, muzzleWorld);
        float dirModel[3] = {pose->muzzle[0], pose->muzzle[1], pose->muzzle[2]};
        if (!pose->haveMuzzle)
        {
            dirModel[0] = tip[0] - pose->grip[0];
            dirModel[1] = tip[1] - pose->grip[1];
            dirModel[2] = tip[2] - pose->grip[2];
        }
        float dirWorld[3];
        TransformVector(basis, dirModel, dirWorld);
        NoteHlvrDrawnMuzzle(
            Vector(muzzleWorld[0], muzzleWorld[1], muzzleWorld[2]),
            Vector(dirWorld[0], dirWorld[1], dirWorld[2]));
    }
    if (main && main->needle && std::strcmp(main->needle, "crowbar") == 0)
    {
        // Every vertex of vr_crowbar.mdl is weighted to bone v_crowbar.
        // Attachment "top" is bone v_crowbar.top at (15, 0, 6), beside the
        // mesh. The drawn hook is about (0, -4, 16) in v_crowbar space.
        // Sampling the attachment swings the trace 90 degrees off the bar.
        float tipModel[3] = {0.f, -4.f, 16.f};
        const int bar = FindBone(*pose, "v_crowbar");
        if (bar >= 0)
        {
            const float local[3] = {0.f, -4.f, 16.f};
            TransformPoint(pose->bone[bar], local, tipModel);
        }
        else if (!pose->haveTop)
            tipModel[0] = tipModel[1] = tipModel[2] = 0.f;
        else
            std::memcpy(tipModel, pose->top, sizeof(tipModel));
        float topWorld[3];
        TransformPoint(basis, tipModel, topWorld);
        NoteHlvrCrowbarTop(Vector(topWorld[0], topWorld[1], topWorld[2]));
    }
    if (pose->haveHud && main)
    {
        float hudWorld[3][4];
        Mul(basis, pose->hudMat, hudWorld);
        if (main->needle && std::strcmp(main->needle, "shotgun") == 0)
        {
            // Attachment forward (col0) points back along the barrel, and
            // col2 is up, so the panel either faced the player or the sky.
            // The MP5's readable face is the mesh side: bone +X, which is
            // -col1 here. Drop it off the top of the receiver onto that side.
            const float sx = -hudWorld[0][1];
            const float sy = -hudWorld[1][1];
            const float sz = -hudWorld[2][1];
            float flat[3][4];
            flat[0][0] = sx;
            flat[1][0] = sy;
            flat[2][0] = sz;
            flat[0][1] = hudWorld[0][0];
            flat[1][1] = hudWorld[1][0];
            flat[2][1] = hudWorld[2][0];
            flat[0][2] = hudWorld[0][2];
            flat[1][2] = hudWorld[1][2];
            flat[2][2] = hudWorld[2][2];
            flat[0][3] = hudWorld[0][3] + sx * 1.5f - hudWorld[0][2] * 1.3f;
            flat[1][3] = hudWorld[1][3] + sy * 1.5f - hudWorld[1][2] * 1.3f;
            flat[2][3] = hudWorld[2][3] + sz * 1.5f - hudWorld[2][2] * 1.3f;
            NoteHlvrWeaponHud(flat);
        }
        else
            NoteHlvrWeaponHud(hudWorld);
    }
    if (pose->haveForegrip)
    {
        float fg[3];
        TransformPoint(basis, pose->foregrip, fg);
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        m_HlvrForegripWorld = Vector(fg[0], fg[1], fg[2]);
        m_HlvrForegripValid = true;
    }
    return n;
}

bool VR::AnchorHlvrWeaponInHand(void* model, float outGun[3][4])
{
    Weapon* w = WeaponForModel(model);
    if (!w || !w->pose.haveGripMat || !m_ControllerPoseValid)
        return false;
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
        return false;
    Vector rightTrack{};
    Vector leftTrack{};
    QAngle rightAng{};
    bool twoHand = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        const bool phys = m_PhysicalRightTrackingValid;
        rightTrack = phys ? m_PhysicalRightPosAbs : m_RightControllerPosAbs;
        rightAng = phys ? m_PhysicalRightAngAbs : m_RightControllerAngAbs;
        leftTrack = m_LeftControllerPosAbs;
        twoHand = m_HlvrTwoHand && w->pose.haveForegrip && m_LeftControllerTrackingValid;
    }
    const Vector rightPos = ControllerTrackingToWorld(body, rightTrack);
    Vector fwd, right, up;
    QAngle aimAng = rightAng;
    if (twoHand)
    {
        const Vector leftPos = ControllerTrackingToWorld(body, leftTrack);
        Vector aim = leftPos - rightPos;
        if (VectorNormalize(aim) > 0.01f)
        {
            QAngle::AngleVectors(rightAng, &fwd, &right, &up);
            QAngle pointed{};
            QAngle::VectorAngles(aim, up, pointed);
            pointed.z = rightAng.z;
            aimAng = pointed;
        }
    }
    QAngle::AngleVectors(aimAng, &fwd, &right, &up);
    float hand[3][4];
    hand[0][0] = fwd.x; hand[0][1] = -right.x; hand[0][2] = up.x; hand[0][3] = rightPos.x;
    hand[1][0] = fwd.y; hand[1][1] = -right.y; hand[1][2] = up.y; hand[1][3] = rightPos.y;
    hand[2][0] = fwd.z; hand[2][1] = -right.z; hand[2][2] = up.z; hand[2][3] = rightPos.z;
    float carry[3][4];
    if (twoHand)
    {
        Vector barrel(w->pose.foregrip[0] - w->pose.grip[0],
            w->pose.foregrip[1] - w->pose.grip[1],
            w->pose.foregrip[2] - w->pose.grip[2]);
        const float bl = std::sqrt(barrel.x * barrel.x + barrel.y * barrel.y + barrel.z * barrel.z);
        if (bl > 0.01f)
        {
            barrel.x /= bl; barrel.y /= bl; barrel.z /= bl;
            const Vector gripUp(w->pose.gripMat[0][2], w->pose.gripMat[1][2], w->pose.gripMat[2][2]);
            QAngle carryAng{};
            QAngle::VectorAngles(barrel, gripUp, carryAng);
            Vector cf, cr, cu;
            QAngle::AngleVectors(carryAng, &cf, &cr, &cu);
            carry[0][0] = cf.x; carry[0][1] = -cr.x; carry[0][2] = cu.x; carry[0][3] = w->pose.grip[0];
            carry[1][0] = cf.y; carry[1][1] = -cr.y; carry[1][2] = cu.y; carry[1][3] = w->pose.grip[1];
            carry[2][0] = cf.z; carry[2][1] = -cr.z; carry[2][2] = cu.z; carry[2][3] = w->pose.grip[2];
        }
        else
            std::memcpy(carry, w->pose.gripMat, sizeof(carry));
    }
    else
        std::memcpy(carry, w->pose.gripMat, sizeof(carry));
    float inv[3][4];
    InvertTR(carry, inv);
    Mul(hand, inv, outGun);
    return true;
}

void VR::UpdateHlvrTwoHand()
{
    if (ViewmodelIsShotgun() && m_Game)
    {
        const int clip = m_Game->ReadWeaponClip(m_Game->GetActiveWeaponEntity());
        if (clip >= 0)
        {
            if (m_ShotgunLastClip >= 0 && clip < m_ShotgunLastClip)
            {
                // CHLVRWeaponShotgun::PrimaryAttack sets m_bNeedPump and
                // samples the hand distance at the moment of the shot.
                m_ShotgunNeedsPump = true;
                m_ShotgunHandRef = -1.f;
                const int lost = m_ShotgunLastClip - clip;
                const char* fireWavs[] = {
                    "weapons/shotgun/shotgun_fire_dav01a.wav",
                    "weapons/shotgun/shotgun_fire_dav01b.wav",
                    "weapons/shotgun/shotgun_fire_dav01c.wav",
                    "weapons/shotgun/shotgun_fire_dav01d.wav",
                };
                const char* wav = (lost >= 2)
                    ? "weapons/shotgun/shotgun_alt_fire_dav01.wav"
                    : fireWavs[GetTickCount() & 3];
                if (m_Game)
                    m_Game->PlayUiSound(wav);
            }
            m_ShotgunLastClip = clip;
        }
    }
    else
        m_ShotgunLastClip = -1;

    if (m_EmptyHands || !m_LeftControllerTrackingValid || !m_HlvrForegripValid)
    {
        m_HlvrTwoHand = false;
        m_ShotgunPump = 0.f;
        return;
    }
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    if (body.LengthSqr() <= 1.f)
    {
        m_HlvrTwoHand = false;
        m_ShotgunPump = 0.f;
        return;
    }
    bool grip = m_ActionInventoryGripLeft != vr::k_ulInvalidActionHandle
        && PressedDigitalAction(m_ActionInventoryGripLeft);
    if (!grip && m_ActionInventoryGripLeft != vr::k_ulInvalidActionHandle)
    {
        vr::InputAnalogActionData_t analog{};
        if (GetAnalogActionData(m_ActionInventoryGripLeft, analog) && analog.bActive && analog.x >= 0.55f)
            grip = true;
    }
    Vector fore{};
    Vector leftTrack{};
    Vector rightTrack{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        fore = m_HlvrForegripWorld;
        leftTrack = m_LeftControllerPosAbs;
        rightTrack = m_PhysicalRightTrackingValid ? m_PhysicalRightPosAbs : m_RightControllerPosAbs;
    }
    const Vector left = ControllerTrackingToWorld(body, leftTrack);
    const Vector right = ControllerTrackingToWorld(body, rightTrack);
    Vector axis = fore - right;
    const float reach = VectorNormalize(axis);
    const Vector rel = left - right;
    const float along = reach > 1.f ? rel.Dot(axis) : 0.f;
    Vector lateral = rel;
    if (reach > 1.f)
    {
        lateral.x -= axis.x * along;
        lateral.y -= axis.y * along;
        lateral.z -= axis.z * along;
    }
    const float radial = lateral.Length();
    const bool onBarrel = reach > 1.f && radial < 14.f && along > -6.f && along < reach + 14.f;
    const bool wasTwoHand = m_HlvrTwoHand;
    if (m_HlvrTwoHand)
    {
        if (!grip || radial > 22.f || along < -12.f || (reach > 1.f && along > reach + 24.f))
            m_HlvrTwoHand = false;
    }
    else if (grip && onBarrel)
        m_HlvrTwoHand = true;

    if (!(m_HlvrTwoHand && ViewmodelIsShotgun()))
    {
        m_ShotgunPump = 0.f;
        m_ShotgunPumpRefValid = false;
    }
    if (m_HlvrTwoHand && ViewmodelIsShotgun() && reach > 1.f)
    {
        if (!m_ShotgunPumpRefValid)
        {
            // Rest is wherever the off hand grabbed, not the full
            // right-hand-to-foregrip span. That span already slid the
            // forend several inches before the pump started.
            m_ShotgunPumpRef = along;
            m_ShotgunPumpRefValid = true;
        }
        float pull = m_ShotgunPumpRef - along;
        if (pull < 0.f)
            pull = 0.f;
        // HL2VR plays ACT_SHOTGUN_RELOAD_START after a 2 inch hand move.
        // The authored forend travel is about 3.5 inches, not the 8 inch
        // clamp that ran the pump off the barrel.
        if (pull > 3.5f)
            pull = 3.5f;
        m_ShotgunPump = pull;
        // The forend travel is what the player sees. Hand-to-hand distance
        // does not always move two inches on that stroke, so the pump sound
        // and the extra-shell eject follow the forend.
        if (!m_ShotgunForendBack && m_ShotgunPump >= 2.f)
        {
            m_ShotgunForendBack = true;
            m_ShotgunPumpWasNeeded = m_ShotgunNeedsPump;
            if (m_Game)
                m_Game->PlayUiSound((GetTickCount() & 1)
                    ? "weapons/shotgun/shotgun_reload_pump-in_dav03b.wav"
                    : "weapons/shotgun/shotgun_reload_pump-in_dav03a.wav");
        }
        else if (m_ShotgunForendBack && m_ShotgunPump <= 0.35f)
        {
            m_ShotgunForendBack = false;
            // A pump that was not required pushed a live shell out, so the
            // next shot has to be pumped in. Do not rewrite the clip here:
            // the server still owns that count, and a local write made the
            // next shot silent.
            if (!m_ShotgunPumpWasNeeded)
                m_ShotgunNeedsPump = true;
            else
                m_ShotgunNeedsPump = false;
            if (m_Game)
                m_Game->PlayUiSound((GetTickCount() & 1)
                    ? "weapons/shotgun/shotgun_reload_pump-out_dav03b.wav"
                    : "weapons/shotgun/shotgun_reload_pump-out_dav03a.wav");
        }
    }
    if (ViewmodelIsShotgun())
    {
        // CHLVRWeaponShotgun::ItemPostFrame + ToggleSecondHand.
        // hlvr_shotgun_pump_distance is 2 inches of hand-to-hand change.
        const float handDist = (left - right).Length();
        constexpr float kPumpDist = 2.f;
        if (m_ShotgunHandRef < 0.f)
            m_ShotgunHandRef = handDist;
        if (m_HlvrTwoHand && !wasTwoHand)
        {
            m_ShotgunHandRef = handDist;
            m_ShotgunPumpBack = false;
        }
        if (!m_HlvrTwoHand && wasTwoHand && (m_ShotgunPumpBack || m_ShotgunForendBack))
        {
            const bool unloading = m_ShotgunForendBack && !m_ShotgunPumpWasNeeded;
            m_ShotgunPumpBack = false;
            m_ShotgunForendBack = false;
            if (unloading)
                m_ShotgunNeedsPump = true;
            else
                m_ShotgunNeedsPump = false;
            if (m_Game)
                m_Game->PlayUiSound((GetTickCount() & 1)
                    ? "weapons/shotgun/shotgun_reload_pump-out_dav03b.wav"
                    : "weapons/shotgun/shotgun_reload_pump-out_dav03a.wav");
        }
        else if (m_HlvrTwoHand)
        {
            if (m_ShotgunPumpBack)
            {
                if (handDist >= m_ShotgunHandRef + kPumpDist)
                {
                    m_ShotgunNeedsPump = false;
                    m_ShotgunPumpBack = false;
                }
                else if (handDist < m_ShotgunHandRef)
                    m_ShotgunHandRef = handDist;
            }
            else if (handDist <= m_ShotgunHandRef - kPumpDist)
            {
                m_ShotgunPumpWasNeeded = m_ShotgunNeedsPump;
                m_ShotgunPumpBack = true;
                m_ShotgunHandRef = handDist;
            }
            else if (handDist > m_ShotgunHandRef)
                m_ShotgunHandRef = handDist;
        }
    }
    if (wasTwoHand != m_HlvrTwoHand)
        Game::logMsg("HL2VR two-hand %s pump=%.2f", m_HlvrTwoHand ? "grab" : "release", m_ShotgunPump);
}

void VR::AnchorHlvrWeapon(void* model, const float visibleBone0[3][4], const QAngle& aim, float outGun[3][4])
{
    Weapon* w = WeaponForModel(model);
    const float* grip = (w && w->pose.haveGrip) ? w->pose.grip : nullptr;
    float gripFallback[3] = {visibleBone0[0][3], visibleBone0[1][3], visibleBone0[2][3]};
    if (!grip)
        grip = gripFallback;
    const float* muzzle = (w && w->pose.ready) ? w->pose.muzzle : nullptr;
    float muzzleFallback[3] = {1.f, 0.f, 0.f};
    if (!muzzle)
        muzzle = muzzleFallback;

    Vector fwd, right, up;
    QAngle::AngleVectors(aim, &fwd, &right, &up);
    float md[3] = {muzzle[0], muzzle[1], muzzle[2]};
    float ml = std::sqrt(md[0] * md[0] + md[1] * md[1] + md[2] * md[2]);
    if (ml < 0.001f)
    {
        md[0] = 1.f; md[1] = 0.f; md[2] = 0.f; ml = 1.f;
    }
    md[0] /= ml; md[1] /= ml; md[2] /= ml;
    float hint[3] = {0.f, 0.f, 1.f};
    if (std::fabs(md[0] * hint[0] + md[1] * hint[1] + md[2] * hint[2]) > 0.9f)
    {
        hint[0] = 0.f; hint[1] = 1.f; hint[2] = 0.f;
    }
    float side[3] = {
        md[1] * hint[2] - md[2] * hint[1],
        md[2] * hint[0] - md[0] * hint[2],
        md[0] * hint[1] - md[1] * hint[0],
    };
    float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    if (sl < 0.001f)
        sl = 1.f;
    side[0] /= sl; side[1] /= sl; side[2] /= sl;
    float mup[3] = {
        side[1] * md[2] - side[2] * md[1],
        side[2] * md[0] - side[0] * md[2],
        side[0] * md[1] - side[1] * md[0],
    };
    const float wf[3] = {fwd.x, fwd.y, fwd.z};
    const float wr[3] = {right.x, right.y, right.z};
    const float wu[3] = {up.x, up.y, up.z};
    auto col = [&](int c, const float* modelAxis) {
        outGun[0][c] = wf[0] * modelAxis[0] + wr[0] * modelAxis[1] + wu[0] * modelAxis[2];
        outGun[1][c] = wf[1] * modelAxis[0] + wr[1] * modelAxis[1] + wu[1] * modelAxis[2];
        outGun[2][c] = wf[2] * modelAxis[0] + wr[2] * modelAxis[1] + wu[2] * modelAxis[2];
    };
    // Columns of the model basis are muzzle, side, up. Map them to forward, right, up.
    col(0, md);
    col(1, side);
    col(2, mup);
    const float hold[3] = {visibleBone0[0][3], visibleBone0[1][3], visibleBone0[2][3]};
    outGun[0][3] = hold[0] - (outGun[0][0] * grip[0] + outGun[0][1] * grip[1] + outGun[0][2] * grip[2]);
    outGun[1][3] = hold[1] - (outGun[1][0] * grip[0] + outGun[1][1] * grip[1] + outGun[1][2] * grip[2]);
    outGun[2][3] = hold[2] - (outGun[2][0] * grip[0] + outGun[2][1] * grip[1] + outGun[2][2] * grip[2]);
}

namespace
{
    void* g_pendingRender = nullptr;
    ModelRenderInfo_t g_pendingInfo{};
    float g_pendingWorld[3][4]{};
    bool g_pending = false;
    void* g_freeRender = nullptr;
    ModelRenderInfo_t g_freeInfo{};
    float g_freeWorld[3][4]{};
    bool g_freePending = false;
    bool g_freeLeftOnly = false;
}

void VR::ClearHlvrWeaponPending()
{
    g_pending = false;
    g_freePending = false;
    g_freeLeftOnly = false;
}

void VR::NoteHlvrWeaponPending(void* modelRender, const ModelRenderInfo_t& info)
{
    if (!modelRender || !info.pModelToWorld)
        return;
    g_pendingRender = modelRender;
    g_pendingInfo = info;
    std::memcpy(g_pendingWorld, info.pModelToWorld, sizeof(g_pendingWorld));
    g_pendingInfo.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(g_pendingWorld);
    g_pending = true;
}

void VR::NoteHlvrFreeHands(void* modelRender, const ModelRenderInfo_t& info, bool leftOnly)
{
    if (!modelRender || !info.pRenderable || !info.pModelToWorld)
        return;
    g_freeRender = modelRender;
    g_freeInfo = info;
    std::memcpy(g_freeWorld, info.pModelToWorld, sizeof(g_freeWorld));
    g_freeInfo.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(g_freeWorld);
    g_freeLeftOnly = leftOnly;
    g_freePending = true;
}

void VR::FlushHlvrWeaponPending()
{
    const bool weapon = g_pending && g_pendingRender;
    const bool hands = g_freePending && g_freeRender;
    const bool leftOnly = g_freeLeftOnly;
    g_pending = false;
    g_freePending = false;
    g_freeLeftOnly = false;
    if (weapon)
        DrawHlvrWeaponAttachments(g_pendingRender, g_pendingInfo);
    if (hands)
        DrawHlvrFreeHands(g_freeRender, true, !leftOnly, &g_freeInfo);
}

void VR::UpdateHlvrSmg(uint32_t& buttons)
{
    if (!ViewmodelIsMp5() || m_EmptyHands || !m_Game)
    {
        g_smg.state = SmgMag::Loaded;
        g_smg.gripWas = false;
        g_smg.reloadWas = false;
        return;
    }
    const Weapon* gun = nullptr;
    for (const Weapon& w : g_weapons)
    {
        if (w.needle && std::strcmp(w.needle, "mp5") == 0)
        {
            gun = &w;
            break;
        }
    }
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    Vector leftTrack{};
    QAngle leftAng{};
    bool tracked = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        tracked = m_LeftControllerTrackingValid || m_ControllerPoseValid;
        leftTrack = m_LeftControllerPosAbs;
        leftAng = m_LeftControllerAngAbs;
    }
    const bool leftHanded = bmvr::g_LeftHanded;
    bool grip = leftHanded
        ? PressedDigitalAction(m_ActionSecondaryAttack)
        : PressedDigitalAction(m_ActionUse);
    if (!grip && m_ActionInventoryGripLeft != vr::k_ulInvalidActionHandle
        && PressedDigitalAction(m_ActionInventoryGripLeft))
        grip = true;
    const bool gripEdge = grip && !g_smg.gripWas;
    const Vector off = tracked ? ControllerTrackingToWorld(body, leftTrack) : Vector{};
    Vector well{};
    bool haveWell = false;
    if (gun && gun->pose.haveReload && g_gunWorldValid)
    {
        const float* g = &g_gunWorld[0][0];
        const float* p = gun->pose.reload;
        well = Vector(
            g[0] * p[0] + g[1] * p[1] + g[2] * p[2] + g[3],
            g[4] * p[0] + g[5] * p[1] + g[6] * p[2] + g[7],
            g[8] * p[0] + g[9] * p[1] + g[10] * p[2] + g[11]);
        haveWell = well.LengthSqr() > 1.f;
    }
    const float nearMag = haveWell && tracked ? (off - well).Length() : 1.0e6f;
    const DWORD now = GetTickCount();

    if (g_smg.state == SmgMag::World)
    {
        const float dt = 0.016f;
        g_smg.vel.z -= 600.f * dt;
        g_smg.origin = g_smg.origin + g_smg.vel * dt;
        if (g_smg.origin.z < g_smg.floorZ)
        {
            g_smg.origin.z = g_smg.floorZ;
            g_smg.vel = Vector(0.f, 0.f, 0.f);
        }
    }
    Vector shoulder = body;
    if (m_HmdForward.LengthSqr() > 0.01f)
    {
        const float side = leftHanded ? 10.f : -10.f;
        shoulder = shoulder - m_HmdForward * 4.f + m_HmdRight * side + m_HmdUp * -8.f;
    }
    const bool nearShoulder = tracked && (off - shoulder).Length() < 16.f;
    if (g_smg.state == SmgMag::World && gripEdge && nearShoulder)
    {
        g_smg.state = SmgMag::Hand;
        g_smg.vel = Vector(0.f, 0.f, 0.f);
        if (m_Game)
            m_Game->PlayUiSound("weapons/pistol/pistol_clip_insert.wav");
    }
    // Grip on the mag well pulls it. Reload (A) stays the engine reload.
    if (g_smg.state == SmgMag::Loaded && gripEdge && nearMag < 5.f && !m_HlvrTwoHand)
    {
        const float nearFore = m_HlvrForegripValid
            ? (off - m_HlvrForegripWorld).Length() : 1.0e6f;
        // The foregrip sits next to the mag well. Grabbing it was close
        // enough to count as a mag pull, then the same grip seated it again.
        if (nearFore >= nearMag)
        {
            g_smg.state = SmgMag::Hand;
            g_smg.origin = haveWell ? well : off;
            g_smg.angles = leftAng;
            g_smg.vel = Vector(0.f, 0.f, 0.f);
            g_smg.floorZ = (body.LengthSqr() > 1.f ? body.z : g_smg.origin.z) - 60.f;
            g_smg.until = now + 20000;
            if (m_Game)
                m_Game->PlayUiSound("weapons/pistol/pistol_clip_release.wav");
        }
    }
    else if (g_smg.state == SmgMag::Hand)
    {
        if (!grip)
        {
            g_smg.state = SmgMag::World;
            g_smg.origin = tracked ? off : g_smg.origin;
            g_smg.angles = leftAng;
            g_smg.vel = Vector(0.f, 0.f, -40.f);
            g_smg.floorZ = (body.LengthSqr() > 1.f ? body.z : g_smg.origin.z) - 60.f;
            g_smg.until = now + 20000;
        }
        else if (nearMag < 5.f)
        {
            // HL2VR SMG_CHAMBER_TO_MAGAZINE_LOAD_DISTANCE is 5 inches.
            g_smg.state = SmgMag::Loaded;
            if (m_Game)
                m_Game->PlayUiSound("weapons/pistol/pistol_clip_insert.wav");
            const int clip = m_Game->ReadWeaponClip(m_Game->GetActiveWeaponEntity());
            if (clip == 0)
                buttons |= IN_RELOAD;
        }
    }
    else if (g_smg.state == SmgMag::World)
    {
        if (gripEdge && tracked && (off - g_smg.origin).Length() < 8.f)
            g_smg.state = SmgMag::Hand;
        else if (g_smg.until != 0 && now > g_smg.until)
            g_smg.state = SmgMag::Loaded;
    }
    if (g_smg.state != SmgMag::Loaded)
        buttons &= ~(IN_ATTACK | IN_ATTACK2);
    g_smg.gripWas = grip;
    g_smg.reloadWas = (buttons & IN_RELOAD) != 0;
}

void VR::DrawHlvrWeaponAttachments(void* modelRender, const ModelRenderInfo_t& gunInfo)
{
    if (!modelRender || !g_activePose)
        return;
    // The matrix the gun mesh was just skinned with. Rebuilding it here, or
    // using the viewmodel entity matrix, left the hands off the gun.
    if (!g_gunWorldValid)
        return;
    float world[3][4];
    std::memcpy(world, g_gunWorld, sizeof(world));
    for (Weapon& w : g_weapons)
    {
        if (&w.pose != g_activePose)
            continue;
        for (Part& part : w.parts)
        {
            if (!part.model)
                continue;
            const bool clip = part.path && std::strstr(part.path, "clip") != nullptr;
            if (clip && w.needle && std::strcmp(w.needle, "mp5") == 0 && g_smg.state != SmgMag::Loaded)
                continue;
            DrawEx(modelRender, gunInfo, part.model, world);
        }
        if (w.needle && std::strcmp(w.needle, "mp5") == 0 && g_smg.state != SmgMag::Loaded)
        {
            void* clipModel = nullptr;
            for (Part& part : w.parts)
            {
                if (part.model && part.path && std::strstr(part.path, "clip") != nullptr)
                    clipModel = part.model;
            }
            if (clipModel)
            {
                float basis[3][4];
                bool placed = false;
                if (g_smg.state == SmgMag::Hand)
                {
                    Vector track{};
                    QAngle ang{};
                    bool ok = false;
                    {
                        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
                        ok = m_LeftControllerTrackingValid || m_ControllerPoseValid;
                        track = m_LeftControllerPosAbs;
                        ang = m_LeftControllerAngAbs;
                    }
                    if (ok)
                    {
                        const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
                        const Vector pos = ControllerTrackingToWorld(body, track);
                        Vector fwd, side, up;
                        QAngle::AngleVectors(ang, &fwd, &side, &up);
                        float hand[3][4];
                        hand[0][0] = fwd.x; hand[0][1] = -side.x; hand[0][2] = up.x; hand[0][3] = pos.x;
                        hand[1][0] = fwd.y; hand[1][1] = -side.y; hand[1][2] = up.y; hand[1][3] = pos.y;
                        hand[2][0] = fwd.z; hand[2][1] = -side.z; hand[2][2] = up.z; hand[2][3] = pos.z;
                        const Pose* clipPose = CachedPartPose(clipModel);
                        if (clipPose && clipPose->haveGripMat)
                        {
                            float inv[3][4];
                            InvertTR(clipPose->gripMat, inv);
                            Mul(hand, inv, basis);
                            placed = true;
                        }
                    }
                }
                if (!placed && g_smg.origin.LengthSqr() > 1.f)
                {
                    Vector fwd, side, up;
                    QAngle::AngleVectors(g_smg.angles, &fwd, &side, &up);
                    basis[0][0] = fwd.x; basis[0][1] = -side.x; basis[0][2] = up.x; basis[0][3] = g_smg.origin.x;
                    basis[1][0] = fwd.y; basis[1][1] = -side.y; basis[1][2] = up.y; basis[1][3] = g_smg.origin.y;
                    basis[2][0] = fwd.z; basis[2][1] = -side.z; basis[2][2] = up.z; basis[2][3] = g_smg.origin.z;
                    placed = true;
                }
                if (placed)
                    DrawEx(modelRender, gunInfo, clipModel, basis);
            }
        }
        break;
    }
    // One hands model, skinned with the weapon. A second DrawEx of the same
    // mesh at the controller was the flicker.
    if (g_hands && gunInfo.pRenderable)
        DrawEx(modelRender, gunInfo, g_hands, world);
}

void VR::DrawHlvrFreeHands(void* modelRender, bool left, bool right, const ModelRenderInfo_t* anchor)
{
    if (!modelRender || !anchor || !anchor->pRenderable || (!left && !right) || g_freeDepth > 0)
        return;
    ++g_freeDepth;
    if (!g_hands || !g_handBind.ready)
    {
        --g_freeDepth;
        return;
    }
    const int rightBone = FindBone(g_handBind, "ValveBiped.Bip01_R_Hand");
    const int leftBone = FindBone(g_handBind, "ValveBiped.Bip01_L_Hand");
    auto drawSide = [&](bool isRight, const Vector& trackPos, const QAngle& ang) {
        const int handBone = isRight ? rightBone : leftBone;
        if (handBone < 0)
            return;
        const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        const Vector pos = ControllerTrackingToWorld(body, trackPos);
        Vector fwd, side, up;
        QAngle::AngleVectors(ang, &fwd, &side, &up);
        if (isRight)
        {
            side = side * -1.f;
            up = up * -1.f;
        }
        float handWorld[3][4];
        handWorld[0][0] = fwd.x; handWorld[0][1] = -side.x; handWorld[0][2] = up.x; handWorld[0][3] = pos.x;
        handWorld[1][0] = fwd.y; handWorld[1][1] = -side.y; handWorld[1][2] = up.y; handWorld[1][3] = pos.y;
        handWorld[2][0] = fwd.z; handWorld[2][1] = -side.z; handWorld[2][2] = up.z; handWorld[2][3] = pos.z;
        // The hands-mdl bind pose draws as a palm with the fingers folded
        // away. A weapon idle pose is the hand that already looks right.
        // On the first frame after a load those poses are not built yet,
        // so build them before falling back to the fingerless bind.
        const Pose* donor = nullptr;
        int srcHand = -1;
        const char* handName = isRight ? "ValveBiped.Bip01_R_Hand" : "ValveBiped.Bip01_L_Hand";
        auto findDonor = [&]() {
            donor = nullptr;
            srcHand = -1;
            for (const Weapon& w : g_weapons)
            {
                if (!w.pose.ready)
                    continue;
                const int h = FindBone(w.pose, handName);
                const int finger = FindBone(w.pose, isRight
                    ? "ValveBiped.Bip01_R_Finger1" : "ValveBiped.Bip01_L_Finger1");
                if (h < 0 || finger < 0)
                    continue;
                donor = &w.pose;
                srcHand = h;
                break;
            }
        };
        findDonor();
        if (!donor)
        {
            PreloadHlvrWeapons();
            findDonor();
        }
        if (!donor)
        {
            donor = &g_handBind;
            srcHand = handBone;
        }
        float inv[3][4];
        InvertTR(donor->bone[srcHand], inv);
        float delta[3][4];
        Mul(handWorld, inv, delta);
        float bones[kMaxBones][3][4];
        const int n = g_handBind.num < kMaxBones ? g_handBind.num : kMaxBones;
        float away[3][4] = {{1, 0, 0, 1.0e6f}, {0, 1, 0, 1.0e6f}, {0, 0, 1, 1.0e6f}};
        for (int i = 0; i < n; ++i)
            std::memcpy(bones[i], away, sizeof(away));
        const char* prefix = isRight ? "ValveBiped.Bip01_R_" : "ValveBiped.Bip01_L_";
        for (int i = 0; i < n; ++i)
        {
            if (std::strncmp(g_handBind.name[i], prefix, std::strlen(prefix)) != 0)
                continue;
            const int src = FindBone(*donor, g_handBind.name[i]);
            if (src < 0)
                continue;
            Mul(delta, donor->bone[src], bones[i]);
        }
        g_freeCount = n;
        std::memcpy(g_freeBones, bones, sizeof(float) * 12 * static_cast<size_t>(n));
        g_freeActive = true;
        ModelRenderInfo_t inf = *anchor;
        inf.pModel = static_cast<model_t*>(g_hands);
        inf.origin = pos;
        inf.angles = ang;
        inf.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(handWorld);
        inf.instance = -1;
        const int drew = DrawEx(modelRender, inf, g_hands, handWorld);
        if (drew >= 0)
            NoteHandSide(prefix);
        g_freeActive = false;
    };
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (right && m_ControllerPoseValid)
        drawSide(true, m_RightControllerPosAbs, m_RightControllerAngAbs);
    if (left && (m_LeftControllerTrackingValid || m_ControllerPoseValid))
        drawSide(false, m_LeftControllerPosAbs, m_LeftControllerAngAbs);
    --g_freeDepth;
}
