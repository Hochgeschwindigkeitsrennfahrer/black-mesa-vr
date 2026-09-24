#include "vr.h"
#include "game.h"
#include "sdk.h"

#include <cmath>
#include <cstring>

namespace
{
    constexpr int kMaxBones = 96;

    struct Pose
    {
        bool ready = false;
        bool failed = false;
        int num = 0;
        float bone[kMaxBones][3][4]{};
        char name[kMaxBones][40]{};
        float grip[3]{};
        float muzzle[3]{1.f, 0.f, 0.f};
        bool haveGrip = false;
        bool haveMuzzle = false;
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
    };

    void* g_hands = nullptr;
    Pose g_handBind{};
    float g_gunWorld[3][4]{};
    bool g_gunWorldValid = false;
    const Pose* g_activePose = nullptr;
    bool g_freeActive = false;
    int g_freeCount = 0;
    float g_freeBones[kMaxBones][3][4]{};
    int g_freeDepth = 0;

    const unsigned char* StudioHdr(void* model);
    void BuildPose(const unsigned char* hdr, Pose& pose);

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

    void BuildPose(const unsigned char* hdr, Pose& pose);

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

    void BuildPose(const unsigned char* hdr, Pose& pose)
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
            if (std::strcmp(name, "@idle") == 0 || std::strcmp(name, "idle") == 0)
            {
                const int rel = *reinterpret_cast<const int*>(desc + 56);
                animPtr = desc + rel;
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
                    pose.haveGrip = true;
                }
                else if (std::strcmp(nm, "muzzle") == 0)
                {
                    float dir[3] = {local[0][3], local[1][3], local[2][3]};
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

    Weapon* WeaponForNeedle(const char* name)
    {
        if (!name)
            return nullptr;
        for (Weapon& w : g_weapons)
        {
            if (std::strstr(name, w.needle))
                return &w;
        }
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
        float away[3][4] = {
            {0.f, 0.f, 0.f, 1.0e6f},
            {0.f, 0.f, 0.f, 1.0e6f},
            {0.f, 0.f, 0.f, 1.0e6f},
        };
        for (int i = 0; i < n; ++i)
            std::memcpy(rows + i * 12, away, sizeof(away));
    }

    int DrawEx(void* modelRender, const ModelRenderInfo_t& gunInfo, void* model, const float world[3][4])
    {
        if (!modelRender || !model)
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
        using ExFn = int(__thiscall*)(void*, void*);
        int drawn = 0;
        __try { drawn = reinterpret_cast<ExFn>(vt[16])(modelRender, &inf); }
        __except (EXCEPTION_EXECUTE_HANDLER) { drawn = -1; }
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

void* VR::HlvrWeaponDrawModel(const char* stockModelName)
{
    Weapon* w = WeaponForNeedle(stockModelName);
    if (!w)
        return nullptr;
    EnsureWeapon(this, *w);
    EnsureHands(this);
    if (!w->model || !w->pose.ready)
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
    return g_hands != nullptr && g_handBind.ready;
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
        pose = &main->pose;
        std::memcpy(g_gunWorld, modelToWorld, sizeof(g_gunWorld));
        g_gunWorldValid = true;
        g_activePose = pose;
    }
    else if (g_activePose && g_gunWorldValid)
    {
        pose = g_activePose;
        std::memcpy(basis, g_gunWorld, sizeof(basis));
    }
    if (!pose)
        return 0;

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
        for (int i = 0; i < n; ++i)
        {
            if (model == g_hands && std::strncmp(partPose.name[i], "ValveBiped.Bip01_R_", 19) != 0)
                continue;
            const int src = FindBone(*pose, partPose.name[i]);
            float world[3][4];
            if (src >= 0)
                Mul(basis, pose->bone[src], world);
            else if (i == 0 && model != g_hands)
                Mul(basis, partPose.bone[0], world);
            else
                continue;
            std::memcpy(boneRows + i * 12, world, sizeof(world));
        }
        return n;
    }

    const int n = pose->num < maxBones ? pose->num : maxBones;
    for (int i = 0; i < n; ++i)
    {
        float world[3][4];
        Mul(basis, pose->bone[i], world);
        std::memcpy(boneRows + i * 12, world, sizeof(world));
    }
    return n;
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
}

void VR::ClearHlvrWeaponPending()
{
    g_pending = false;
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

void VR::FlushHlvrWeaponPending()
{
    if (!g_pending || !g_pendingRender)
        return;
    g_pending = false;
    DrawHlvrWeaponAttachments(g_pendingRender, g_pendingInfo);
}

void VR::DrawHlvrWeaponAttachments(void* modelRender, const ModelRenderInfo_t& gunInfo)
{
    if (!modelRender || !g_activePose)
        return;
    float world[3][4];
    if (g_gunWorldValid)
        std::memcpy(world, g_gunWorld, sizeof(world));
    else if (gunInfo.pModelToWorld)
        std::memcpy(world, gunInfo.pModelToWorld, sizeof(world));
    else
        return;
    for (Weapon& w : g_weapons)
    {
        if (&w.pose != g_activePose)
            continue;
        for (Part& part : w.parts)
        {
            if (!part.model)
                continue;
            DrawEx(modelRender, gunInfo, part.model, world);
        }
        break;
    }
    if (g_hands)
        DrawEx(modelRender, gunInfo, g_hands, world);
    DrawHlvrFreeHands(modelRender, true, false);
}

void VR::DrawHlvrFreeHands(void* modelRender, bool left, bool right)
{
    if (!modelRender || (!left && !right) || g_freeDepth > 0)
        return;
    ++g_freeDepth;
    EnsureHands(this);
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
        float handWorld[3][4];
        handWorld[0][0] = fwd.x; handWorld[0][1] = -side.x; handWorld[0][2] = up.x; handWorld[0][3] = pos.x;
        handWorld[1][0] = fwd.y; handWorld[1][1] = -side.y; handWorld[1][2] = up.y; handWorld[1][3] = pos.y;
        handWorld[2][0] = fwd.z; handWorld[2][1] = -side.z; handWorld[2][2] = up.z; handWorld[2][3] = pos.z;
        float inv[3][4];
        InvertTR(g_handBind.bone[handBone], inv);
        float delta[3][4];
        Mul(handWorld, inv, delta);
        float bones[kMaxBones][3][4];
        const int n = g_handBind.num < kMaxBones ? g_handBind.num : kMaxBones;
        float away[3][4] = {{0, 0, 0, 1.0e6f}, {0, 0, 0, 1.0e6f}, {0, 0, 0, 1.0e6f}};
        for (int i = 0; i < n; ++i)
            std::memcpy(bones[i], away, sizeof(away));
        const char* prefix = isRight ? "ValveBiped.Bip01_R_" : "ValveBiped.Bip01_L_";
        for (int i = 0; i < n; ++i)
        {
            if (std::strncmp(g_handBind.name[i], prefix, std::strlen(prefix)) != 0)
                continue;
            Mul(delta, g_handBind.bone[i], bones[i]);
        }
        g_freeCount = n;
        std::memcpy(g_freeBones, bones, sizeof(float) * 12 * static_cast<size_t>(n));
        g_freeActive = true;
        ModelRenderInfo_t inf{};
        inf.pModel = static_cast<model_t*>(g_hands);
        inf.origin = pos;
        inf.angles = ang;
        inf.pModelToWorld = reinterpret_cast<const matrix3x4_t*>(handWorld);
        inf.instance = -1;
        DrawEx(modelRender, inf, g_hands, handWorld);
        g_freeActive = false;
    };
    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
    if (right && m_ControllerPoseValid)
        drawSide(true, m_RightControllerPosAbs, m_RightControllerAngAbs);
    if (left && (m_LeftControllerTrackingValid || m_ControllerPoseValid))
        drawSide(false, m_LeftControllerPosAbs, m_LeftControllerAngAbs);
    --g_freeDepth;
}
