// Ladder rungs from the map's static props, see vr_ladder_rungs.h.
#include "vr_ladder_rungs.h"
#include "game.h"
#include "bmvr_seh.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // Rungs of one ladder model in model space: horizontal bars at
    // z = z0 + k * step (k < count), centred on the lateral axis at
    // +-half, at `depth` on the other horizontal axis. Measured from the
    // models' .vvd vertices (2026-11-18, all ladder models placed next to a
    // func_useableladder in the 78 campaign maps). Cage pieces, the curved
    // metalladder002b top and other frames have no rungs of their own (the
    // ladder inside the cage is a metalladder prop).
    // Sewer staples (props_sewer/*rung*) leave the wall, bend down ~4 u and
    // only then run across: z is the cross bar, not the wall entry.
    struct RungModel
    {
        const char* name;   // path below models/, lower case
        bool lateralX;      // rungs run along model x (else along y)
        float depth;        // the other horizontal coordinate
        float half;         // half rung length
        float z0, step;
        int count;
    };
    const RungModel kModels[] = {
        { "props_c17/metalladder001.mdl", false, 0.0f, 10.5f, 8.f, 16.f, 8 },
        { "props_c17/metalladder002.mdl", false, 8.0f, 10.8f, 8.f, 16.f, 8 },
        { "props_c17/metalladder003.mdl", false, 8.0f, 10.8f, 8.f, 16.f, 73 },
        { "props_sewer/ladderrung.mdl", true, 7.4f, 9.4f, -54.6f, 15.88f, 8 },
        { "props_sewer/single_rung01.mdl", true, 8.9f, 9.4f, -2.5f, 0.f, 1 },
        { "props_sewer/single_rung02.mdl", true, 8.9f, 9.4f, -2.1f, 0.f, 1 },
        { "props_lambda/pool_ladder_section.mdl", false, 9.8f, 18.f, -14.6f, 14.f, 4 },
        { "props_lambda/pool_ladder_top.mdl", false, 9.8f, 18.f, -30.6f, 14.f, 3 },
        { "props_lambda/pool_ladder_bottom.mdl", false, 9.8f, 18.f, 3.4f, 0.f, 1 },
        { "props_questionableethics/qe_ladder.mdl", false, 1.5f, 12.f, -73.5f, 17.f, 9 },
        { "props_office/ladder_hatch.mdl", true, -9.9f, 9.4f, 6.f, 16.f, 4 },
        { "props_silo/c2a2b_ladder_01.mdl", true, 0.f, 16.f, -248.f, 16.f, 31 },
        { "props_silo/c2a2b_ladder_02.mdl", true, 0.f, 16.f, -200.f, 16.f, 25 },
        { "props_silo/c2a2b_ladder_03.mdl", true, 0.f, 16.f, -216.f, 16.f, 27 },
        { "props_silo/silo_platformladder.mdl", true, 0.5f, 16.f, 11.5f, 20.f, 60 },
        { "props_xen/c4a1a/scafold_ladder00.mdl", true, 0.f, 18.f, 2.f, 17.1f, 8 },
    };

    // IBaseFileSystem (VBaseFileSystem011), first six entries; none of them
    // is overloaded, so the vtable order is the declaration order.
    struct IBaseFileSystemMin
    {
        virtual int Read(void* out, int size, void* file) = 0;
        virtual int Write(const void* in, int size, void* file) = 0;
        virtual void* Open(const char* name, const char* options, const char* pathId) = 0;
        virtual void Close(void* file) = 0;
        virtual void Seek(void* file, int pos, int seekType) = 0;
        virtual unsigned int Tell(void* file) = 0;
    };

    IBaseFileSystemMin* FileSystem()
    {
        static IBaseFileSystemMin* s_fs = nullptr;
        static bool s_tried = false;
        if (s_tried)
            return s_fs;
        s_tried = true;
        HMODULE mod = GetModuleHandleA("filesystem_stdio.dll");
        using tCreateInterface = void*(__cdecl*)(const char*, int*);
        auto create = mod ? reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface")) : nullptr;
        int rc = 0;
        s_fs = create ? static_cast<IBaseFileSystemMin*>(create("VBaseFileSystem011", &rc)) : nullptr;
        Game::logMsg("Ladder rungs: file system %s", s_fs ? "VBaseFileSystem011 ok" : "missing (no rung snapping)");
        return s_fs;
    }

    bool ReadAt(IBaseFileSystemMin* fs, void* f, int pos, void* out, int size)
    {
        fs->Seek(f, pos, 0);   // FILESYSTEM_SEEK_HEAD
        return fs->Read(out, size, f) == size;
    }

    // Reads the static prop lump of maps/<map>.bsp. Guarded: a file system
    // interface that does not match this layout must not take the game down.
    bool ReadStaticProps(const char* map, std::vector<char>& lump, int& lumpVersion)
    {
        BMVR_SEH_SAVE_REGS();
        IBaseFileSystemMin* fs = FileSystem();
        if (!fs)
            return false;
        char path[160];
        sprintf_s(path, "maps/%s.bsp", map);
        bool ok = false;
        void* f = nullptr;
        BMVR_TRY
        {
            f = fs->Open(path, "rb", "GAME");
            if (f)
            {
                int header[2 + 64 * 4]{};
                if (ReadAt(fs, f, 0, header, sizeof(header)) && header[0] == 0x50534256)   // "VBSP"
                {
                    const int gameOfs = header[2 + 35 * 4];
                    int count = 0;
                    if (gameOfs > 0 && ReadAt(fs, f, gameOfs, &count, 4) && count > 0 && count < 64)
                    {
                        struct GameLump { int id; unsigned short flags, version; int ofs, len; };
                        GameLump gl[64]{};
                        if (ReadAt(fs, f, gameOfs + 4, gl, count * static_cast<int>(sizeof(GameLump))))
                        {
                            for (int i = 0; i < count; ++i)
                            {
                                if (gl[i].id != 0x73707270)   // 'sprp'
                                    continue;
                                if ((gl[i].flags & 1) || gl[i].len <= 12 || gl[i].len > (64 << 20))
                                {
                                    Game::logMsg("Ladder rungs: %s static props compressed or odd (flags %d len %d)",
                                        map, gl[i].flags, gl[i].len);
                                    break;
                                }
                                lump.resize(gl[i].len);
                                lumpVersion = gl[i].version;
                                ok = ReadAt(fs, f, gl[i].ofs, lump.data(), gl[i].len);
                                break;
                            }
                        }
                    }
                }
                fs->Close(f);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Game::logMsg("Ladder rungs: reading %s faulted (file system layout?)", path);
            ok = false;
        }
        return ok;
    }

    template <typename T> bool Get(const std::vector<char>& d, size_t at, T& v)
    {
        if (at + sizeof(T) > d.size())
            return false;
        std::memcpy(static_cast<void*>(&v), d.data() + at, sizeof(T));
        return true;
    }

    struct Cache
    {
        std::string map;
        std::vector<bmvr::ladders::Rung> rungs;
    };
    Cache& TheCache()
    {
        static Cache c;
        return c;
    }

    void Load(const char* map, std::vector<bmvr::ladders::Rung>& rungs)
    {
        rungs.clear();
        std::vector<char> d;
        int version = 0;
        if (!ReadStaticProps(map, d, version))
        {
            Game::logMsg("Ladder rungs: %s: no static prop lump read", map);
            return;
        }
        size_t p = 0;
        int nDict = 0;
        if (!Get(d, p, nDict) || nDict < 0 || nDict > 8192)
            return;
        p += 4;
        // Which dictionary entries are ladder models.
        std::vector<int> modelOf(nDict, -1);
        for (int i = 0; i < nDict; ++i)
        {
            if (p + 128 > d.size())
                return;
            char name[129]{};
            std::memcpy(name, d.data() + p, 128);
            p += 128;
            for (char* c = name; *c; ++c)
                *c = (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a') : (*c == '\\' ? '/' : *c);
            const char* rel = std::strncmp(name, "models/", 7) == 0 ? name + 7 : name;
            for (int m = 0; m < static_cast<int>(sizeof(kModels) / sizeof(kModels[0])); ++m)
                if (std::strcmp(rel, kModels[m].name) == 0)
                    modelOf[i] = m;
        }
        int nLeaf = 0;
        if (!Get(d, p, nLeaf) || nLeaf < 0)
            return;
        p += 4 + 2 * static_cast<size_t>(nLeaf);
        int nProps = 0;
        if (!Get(d, p, nProps) || nProps <= 0)
            return;
        p += 4;
        const size_t rec = (d.size() - p) / static_cast<size_t>(nProps);
        if (rec < 26)
        {
            Game::logMsg("Ladder rungs: %s static prop records too small (%u bytes, lump v%d)",
                map, static_cast<unsigned>(rec), version);
            return;
        }
        int ladderProps = 0;
        for (int i = 0; i < nProps; ++i)
        {
            const size_t o = p + rec * i;
            Vector origin{};
            QAngle ang{};
            unsigned short type = 0;
            if (!Get(d, o, origin) || !Get(d, o + 12, ang) || !Get(d, o + 24, type))
                break;
            if (type >= modelOf.size() || modelOf[type] < 0)
                continue;
            const RungModel& m = kModels[modelOf[type]];
            Vector f{}, r{}, u{};
            QAngle::AngleVectors(ang, &f, &r, &u);
            const Vector left = r * -1.f;
            // Model x -> forward, y -> left, z -> up.
            auto toWorld = [&](float x, float y, float z) { return origin + f * x + left * y + u * z; };
            for (int k = 0; k < m.count; ++k)
            {
                const float z = m.z0 + m.step * k;
                bmvr::ladders::Rung rung;
                rung.model = modelOf[type];
                if (m.lateralX)
                {
                    rung.a = toWorld(-m.half, m.depth, z);
                    rung.b = toWorld(m.half, m.depth, z);
                }
                else
                {
                    rung.a = toWorld(m.depth, -m.half, z);
                    rung.b = toWorld(m.depth, m.half, z);
                }
                rungs.push_back(rung);
            }
            ++ladderProps;
        }
        Game::logMsg("Ladder rungs: %s: %d ladder props, %d rungs (lump v%d, %u-byte props)",
            map, ladderProps, static_cast<int>(rungs.size()), version, static_cast<unsigned>(rec));
    }
}

namespace bmvr::ladders
{
    int MapRungs(const char* map, const Rung** out)
    {
        Cache& c = TheCache();
        if (!map || !map[0])
        {
            *out = nullptr;
            return 0;
        }
        if (c.map != map)
        {
            c.map = map;
            Load(map, c.rungs);
        }
        *out = c.rungs.empty() ? nullptr : c.rungs.data();
        return static_cast<int>(c.rungs.size());
    }

    const char* ModelName(int model)
    {
        if (model < 0 || model >= static_cast<int>(sizeof(kModels) / sizeof(kModels[0])))
            return nullptr;
        return kModels[model].name;
    }

    bool NearestRung(const char* map, const Vector& p, float maxDist, Vector& onRung, float* dist, Vector* axis,
        int* model)
    {
        const Rung* rungs = nullptr;
        const int n = MapRungs(map, &rungs);
        float best = maxDist * maxDist;
        bool found = false;
        for (int i = 0; i < n; ++i)
        {
            const Vector ab = rungs[i].b - rungs[i].a;
            const float len2 = ab.LengthSqr();
            if (len2 < 1.f)
                continue;
            // Keep a hand's half width off the rails.
            const float len = sqrtf(len2);
            const float margin = len > 12.f ? 4.f / len : 0.f;
            float t = (p - rungs[i].a).Dot(ab) / len2;
            t = t < margin ? margin : (t > 1.f - margin ? 1.f - margin : t);
            const Vector c = rungs[i].a + ab * t;
            const float d2 = (p - c).LengthSqr();
            if (d2 < best)
            {
                best = d2;
                onRung = c;
                if (axis)
                    *axis = ab * (1.f / len);
                if (model)
                    *model = rungs[i].model;
                found = true;
            }
        }
        if (found && dist)
            *dist = sqrtf(best);
        return found;
    }
}
