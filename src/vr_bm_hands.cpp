// Black Mesa hands from the installed game files (see vr_bm_hands.h).
// Same approach as the old scripts/rip_hev_gloves.py (made VR/hands/*.glb), from the game files: VPK -> MDL/VVD/VTX arms body part ->
// wrist + finger subtree in the glove's model space, plus the v_hand texture.

#include "vr_bm_hands.h"

#include "bmvr_flags.h"
#include "game.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace
{
    using Bytes = std::vector<std::uint8_t>;
    constexpr float kUnitsPerMeter = 39.3700787f;

    template <typename T>
    bool Rd(const Bytes& d, size_t off, T& out)
    {
        if (off > d.size() || d.size() - off < sizeof(T))
            return false;
        std::memcpy(&out, d.data() + off, sizeof(T));
        return true;
    }

    std::string CStr(const Bytes& d, size_t off)
    {
        std::string s;
        while (off < d.size() && d[off])
            s.push_back(static_cast<char>(d[off++]));
        return s;
    }

    bool ReadFile(const std::filesystem::path& p, Bytes& out)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f)
            return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }

    // ---- VPK (v1/v2 directory + numbered archives) ----
    struct VpkEntry
    {
        std::uint16_t archive = 0;
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
        Bytes preload;
    };

    struct Vpk
    {
        std::filesystem::path dirPath;
        Bytes dir;
        size_t dataStart = 0;
        std::unordered_map<std::string, VpkEntry> entries;

        bool Open(const std::filesystem::path& path)
        {
            dirPath = path;
            // Only the header + directory tree is read: a single-file VPK
            // (no _dir/_NNN split, e.g. Black Mesa: Military's 208 MB
            // bmmilitary01.vpk) keeps its data in the same file, read per
            // entry in Get.
            std::uint32_t sig = 0, ver = 0, tree = 0;
            {
                std::ifstream f(path, std::ios::binary);
                if (!f)
                    return false;
                dir.assign(12, 0);
                f.read(reinterpret_cast<char*>(dir.data()), 12);
                if (f.gcount() != 12 || !Rd(dir, 0, sig) || !Rd(dir, 4, ver) || !Rd(dir, 8, tree) || sig != 0x55aa1234u)
                    return false;
                const size_t want = ((ver == 2) ? 28 : 12) + static_cast<size_t>(tree);
                if (tree > (256u << 20))
                    return false;
                dir.resize(want);
                f.read(reinterpret_cast<char*>(dir.data() + 12), static_cast<std::streamsize>(want - 12));
                if (static_cast<size_t>(f.gcount()) != want - 12)
                    return false;
            }
            const size_t hdr = (ver == 2) ? 28 : 12;
            dataStart = hdr + tree;
            size_t p = hdr;
            auto cs = [&](std::string& s) -> bool {
                s.clear();
                while (p < dir.size() && dir[p])
                    s.push_back(static_cast<char>(dir[p++]));
                if (p >= dir.size())
                    return false;
                ++p;
                return true;
            };
            std::string ext, path_, name;
            for (;;)
            {
                if (!cs(ext))
                    return false;
                if (ext.empty())
                    break;
                for (;;)
                {
                    if (!cs(path_))
                        return false;
                    if (path_.empty())
                        break;
                    for (;;)
                    {
                        if (!cs(name))
                            return false;
                        if (name.empty())
                            break;
                        VpkEntry e;
                        std::uint32_t crc = 0;
                        std::uint16_t pre = 0, term = 0;
                        if (!Rd(dir, p, crc) || !Rd(dir, p + 4, pre) || !Rd(dir, p + 6, e.archive)
                            || !Rd(dir, p + 8, e.offset) || !Rd(dir, p + 12, e.length) || !Rd(dir, p + 16, term))
                            return false;
                        p += 18;
                        if (p + pre > dir.size())
                            return false;
                        e.preload.assign(dir.begin() + p, dir.begin() + p + pre);
                        p += pre;
                        std::string full = (path_ == " " ? std::string() : path_ + "/") + name + "." + ext;
                        std::transform(full.begin(), full.end(), full.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        entries.emplace(std::move(full), std::move(e));
                    }
                }
            }
            return true;
        }

        bool Get(const std::string& name, Bytes& out) const
        {
            const auto it = entries.find(name);
            if (it == entries.end())
                return false;
            const VpkEntry& e = it->second;
            out = e.preload;
            if (e.length == 0)
                return true;
            std::string base = dirPath.string();
            size_t fileOffset = e.offset;
            if (e.archive == 0x7fff)
                fileOffset = dataStart + e.offset;   // data in this file, after the tree
            else
            {
                const size_t at = base.rfind("_dir.vpk");
                if (at == std::string::npos)
                    return false;
                char num[16];
                std::snprintf(num, sizeof(num), "_%03u.vpk", e.archive);
                base = base.substr(0, at) + num;
            }
            std::ifstream f(base, std::ios::binary);
            if (!f)
                return false;
            f.seekg(static_cast<std::streamoff>(fileOffset));
            const size_t start = out.size();
            out.resize(start + e.length);
            f.read(reinterpret_cast<char*>(out.data() + start), e.length);
            return static_cast<size_t>(f.gcount()) == e.length;
        }
    };

    // ---- small column-major affine math (same layout as VrHandMatrix4) ----
    struct M4
    {
        float m[16]{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    };
    struct V3
    {
        float x = 0, y = 0, z = 0;
    };
    V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    float Len(V3 a) { return std::sqrt(Dot(a, a)); }
    V3 Norm(V3 a)
    {
        const float l = Len(a);
        return l < 1e-8f ? V3{ 0, 0, 1 } : a * (1.f / l);
    }

    M4 Mul(const M4& a, const M4& b)
    {
        M4 o;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                o.m[c * 4 + r] = a.m[0 * 4 + r] * b.m[c * 4 + 0] + a.m[1 * 4 + r] * b.m[c * 4 + 1]
                    + a.m[2 * 4 + r] * b.m[c * 4 + 2] + a.m[3 * 4 + r] * b.m[c * 4 + 3];
        return o;
    }

    M4 FromPosQuat(V3 p, float qx, float qy, float qz, float qw)
    {
        float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (n <= 0.f)
            n = 1.f;
        qx /= n; qy /= n; qz /= n; qw /= n;
        const float xx = qx * qx, yy = qy * qy, zz = qz * qz, xy = qx * qy, xz = qx * qz, yz = qy * qz;
        const float wx = qw * qx, wy = qw * qy, wz = qw * qz;
        M4 o;
        const float v[16] = {
            1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), 0,
            2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), 0,
            2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), 0,
            p.x, p.y, p.z, 1 };
        std::memcpy(o.m, v, sizeof(v));
        return o;
    }

    // Affine inverse (orthonormal rotation).
    M4 Inv(const M4& a)
    {
        const float* m = a.m;
        M4 o;
        const float v[16] = {
            m[0], m[4], m[8], 0,
            m[1], m[5], m[9], 0,
            m[2], m[6], m[10], 0,
            -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]),
            -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]),
            -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]),
            1 };
        std::memcpy(o.m, v, sizeof(v));
        return o;
    }

    V3 XformP(const M4& a, V3 p)
    {
        const float* m = a.m;
        return { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                 m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                 m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
    }
    V3 XformN(const M4& a, V3 p)
    {
        const float* m = a.m;
        return { m[0] * p.x + m[4] * p.y + m[8] * p.z,
                 m[1] * p.x + m[5] * p.y + m[9] * p.z,
                 m[2] * p.x + m[6] * p.y + m[10] * p.z };
    }
    V3 Col(const M4& a, int c) { return { a.m[c * 4], a.m[c * 4 + 1], a.m[c * 4 + 2] }; }
    void SetCol(M4& a, int c, V3 v) { a.m[c * 4] = v.x; a.m[c * 4 + 1] = v.y; a.m[c * 4 + 2] = v.z; }

    // Columns x, y, z = basis with z along zAxis, y toward yHint.
    M4 LookBasis(V3 zAxis, V3 yHint)
    {
        const V3 z = Norm(zAxis);
        V3 y = yHint;
        if (Len(y) < 1e-4f)
            y = { 0, 0, 1 };
        V3 x = Cross(y, z);
        if (Len(x) < 1e-4f)
            x = Cross({ 0, 1, 0 }, z);
        x = Norm(x);
        y = Cross(z, x);
        M4 o;
        SetCol(o, 0, x);
        SetCol(o, 1, y);
        SetCol(o, 2, z);
        return o;
    }

    // Roll of each finger joint around its bone, relative to the palm normal,
    // that the HEV glove rig uses (its finger code bends around local Z).
    // Right hand; the left hand is mirrored (negated). Order: thumb, index,
    // middle, ring, pinky; segments 0..2.
    constexpr float kJointRollRightDeg[15] = {
        104.8f, 103.0f, 98.8f,
        2.7f, 2.9f, 3.2f,
        17.5f, 20.6f, 24.6f,
        31.3f, 36.8f, 58.3f,
        47.2f, 66.0f, 82.3f,
    };

    // ---- Studio model parsing ----
    struct Bone
    {
        std::string name;
        int parent = -1;
        M4 world;
    };
    struct SrcVert
    {
        V3 pos, nrm;
        float u = 0, v = 0;
        float w[3]{};
        std::uint8_t b[3]{};
        std::uint8_t nb = 0;
    };

    struct Model
    {
        std::vector<Bone> bones;
        std::vector<SrcVert> verts;
        std::vector<std::array<int, 3>> tris; // hand-skin mesh only
    };

    bool ParseModel(const Bytes& mdl, const Bytes& vvd, const Bytes& vtx, const char* variantSmd,
        const char* skinMaterial, Model& out, std::string& err, const char* bodyPartName = "arms",
        const char* extraMaterial = nullptr)
    {
        auto I = [&](const Bytes& d, size_t o) { std::int32_t v = 0; Rd(d, o, v); return v; };
        // bones
        const int nb = I(mdl, 156), bi = I(mdl, 160);
        if (nb <= 0 || nb > 256)
        {
            err = "bad bone count";
            return false;
        }
        out.bones.resize(nb);
        for (int i = 0; i < nb; ++i)
        {
            const size_t o = bi + static_cast<size_t>(i) * 216;
            Bone& b = out.bones[i];
            b.name = CStr(mdl, o + I(mdl, o));
            b.parent = I(mdl, o + 4);
            V3 p;
            float q[4]{};
            Rd(mdl, o + 32, p);
            Rd(mdl, o + 44, q);
            const M4 local = FromPosQuat(p, q[0], q[1], q[2], q[3]);
            b.world = (b.parent >= 0 && b.parent < i) ? Mul(out.bones[b.parent].world, local) : local;
        }
        // textures
        const int ntex = I(mdl, 204), ti = I(mdl, 208);
        std::vector<std::string> tex;
        for (int i = 0; i < ntex; ++i)
        {
            const size_t o = ti + static_cast<size_t>(i) * 64;
            tex.push_back(CStr(mdl, o + I(mdl, o)));
        }
        // vertices (LOD 0, with fixups)
        const int vds = I(vvd, 56), nfix = I(vvd, 48), fixs = I(vvd, 52), n0 = I(vvd, 16);
        std::vector<int> order;
        if (nfix > 0)
        {
            for (int k = 0; k < nfix; ++k)
            {
                const int lod = I(vvd, fixs + k * 12), src = I(vvd, fixs + k * 12 + 4), num = I(vvd, fixs + k * 12 + 8);
                if (lod >= 0)
                    for (int j = 0; j < num; ++j)
                        order.push_back(src + j);
            }
        }
        const int nverts = nfix > 0 ? static_cast<int>(order.size()) : n0;
        out.verts.resize(nverts);
        for (int i = 0; i < nverts; ++i)
        {
            const size_t o = vds + static_cast<size_t>(nfix > 0 ? order[i] : i) * 48;
            SrcVert& v = out.verts[i];
            if (!Rd(vvd, o, v.w) || !Rd(vvd, o + 12, v.b) || !Rd(vvd, o + 15, v.nb)
                || !Rd(vvd, o + 16, v.pos) || !Rd(vvd, o + 28, v.nrm) || !Rd(vvd, o + 40, v.u) || !Rd(vvd, o + 44, v.v))
            {
                err = "vvd truncated";
                return false;
            }
        }
        // body part ("arms" for the hands), model by smd name
        const int nbp = I(mdl, 232), bpi = I(mdl, 236);
        int armsBp = -1, variant = -1;
        size_t modelOff = 0;
        for (int p = 0; p < nbp && armsBp < 0; ++p)
        {
            const size_t o = bpi + static_cast<size_t>(p) * 16;
            if (CStr(mdl, o + I(mdl, o)) != bodyPartName)
                continue;
            armsBp = p;
            const int nm = I(mdl, o + 4), mi = I(mdl, o + 12);
            for (int m = 0; m < nm; ++m)
            {
                const size_t mo = o + mi + static_cast<size_t>(m) * 148;
                if (CStr(mdl, mo).find(variantSmd) != std::string::npos)
                {
                    variant = m;
                    modelOff = mo;
                    break;
                }
            }
        }
        if (armsBp < 0 || variant < 0)
        {
            err = std::string(bodyPartName) + " model not found: " + variantSmd;
            return false;
        }
        const int nmesh = I(mdl, modelOff + 72), meshi = I(mdl, modelOff + 76), vidx = I(mdl, modelOff + 84);
        std::vector<std::pair<std::string, int>> meshes;
        for (int k = 0; k < nmesh; ++k)
        {
            const size_t mmo = modelOff + meshi + static_cast<size_t>(k) * 116;
            const int mat = I(mdl, mmo);
            meshes.emplace_back(mat >= 0 && mat < ntex ? tex[mat] : std::string(), vidx / 48 + I(mdl, mmo + 12));
        }
        // VTX: same body part / model, LOD 0
        const size_t bp = I(vtx, 32) + static_cast<size_t>(armsBp) * 8;
        const size_t mh = bp + I(vtx, bp + 4) + static_cast<size_t>(variant) * 8;
        const size_t lh = mh + I(vtx, mh + 4);
        const int nm = I(vtx, lh);
        const size_t meshBase = lh + I(vtx, lh + 4);
        for (int mi = 0; mi < nm && mi < static_cast<int>(meshes.size()); ++mi)
        {
            if (_stricmp(meshes[mi].first.c_str(), skinMaterial) != 0
                && (!extraMaterial || _stricmp(meshes[mi].first.c_str(), extraMaterial) != 0))
                continue;
            const int voff = meshes[mi].second;
            const size_t moff = meshBase + static_cast<size_t>(mi) * 9;
            const int nsg = I(vtx, moff);
            for (int si = 0; si < nsg; ++si)
            {
                const size_t sgo = moff + I(vtx, moff + 4) + static_cast<size_t>(si) * 25;
                const int nv = I(vtx, sgo), vrel = I(vtx, sgo + 4), ni = I(vtx, sgo + 8), irel = I(vtx, sgo + 12);
                std::vector<int> orig(nv);
                for (int k = 0; k < nv; ++k)
                {
                    std::uint16_t id = 0;
                    Rd(vtx, sgo + vrel + static_cast<size_t>(k) * 9 + 4, id);
                    orig[k] = voff + id;
                }
                for (int k = 0; k + 2 < ni; k += 3)
                {
                    std::uint16_t a = 0, b = 0, c = 0;
                    Rd(vtx, sgo + irel + 2 * static_cast<size_t>(k), a);
                    Rd(vtx, sgo + irel + 2 * static_cast<size_t>(k + 1), b);
                    Rd(vtx, sgo + irel + 2 * static_cast<size_t>(k + 2), c);
                    if (a >= nv || b >= nv || c >= nv || orig[a] >= nverts || orig[b] >= nverts || orig[c] >= nverts)
                        continue;
                    out.tris.push_back({ orig[a], orig[b], orig[c] });
                }
            }
        }
        if (out.tris.size() < 100)
        {
            err = "too few triangles for material " + std::string(skinMaterial);
            return false;
        }
        return true;
    }

    // ---- VTF (DXT1/DXT5) -> 32-bit BMP in memory (WIC decodes it) ----
    void Rgb565(std::uint16_t c, std::uint8_t* o)
    {
        o[0] = static_cast<std::uint8_t>(((c >> 11) & 31) * 255 / 31);
        o[1] = static_cast<std::uint8_t>(((c >> 5) & 63) * 255 / 63);
        o[2] = static_cast<std::uint8_t>((c & 31) * 255 / 31);
    }

    bool VtfToBmp(const Bytes& d, Bytes& bmp, std::string& err)
    {
        std::uint16_t W = 0, H = 0;
        std::int32_t fmt = 0;
        std::uint32_t minor = 0, hdr = 0;
        Rd(d, 16, W);
        Rd(d, 18, H);
        Rd(d, 52, fmt);
        Rd(d, 8, minor);
        Rd(d, 12, hdr);
        const int mips = d.size() > 56 ? d[56] : 0;
        const bool dxt1 = fmt == 13, dxt5 = fmt == 15;
        if (!dxt1 && !dxt5)
        {
            err = "unsupported VTF format " + std::to_string(fmt);
            return false;
        }
        const int bs = dxt1 ? 8 : 16;
        size_t hi = 0;
        std::uint32_t nres = 0;
        if (minor >= 3 && hdr >= 96 && Rd(d, 68, nres) && nres > 0 && nres < 32)
        {
            for (std::uint32_t i = 0; i < nres; ++i)
            {
                const size_t o = 80 + i * 8;
                if (o + 8 <= d.size() && d[o] == 0x30)
                {
                    std::uint32_t off = 0;
                    Rd(d, o + 4, off);
                    hi = off;
                    break;
                }
            }
        }
        if (!hi)
        {
            const int lw = d.size() > 62 ? d[61] : 0, lh = d.size() > 62 ? d[62] : 0;
            hi = hdr + ((lw && lh) ? ((lw + 3) / 4) * ((lh + 3) / 4) * 8 : 0);
        }
        // Mip 1 when there is one (half size is plenty for a glove).
        const int level = mips > 1 ? 1 : 0;
        auto msize = [&](int w, int h) { return static_cast<size_t>(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * bs; };
        size_t off = hi;
        for (int m = mips - 1; m > level; --m)
            off += msize(std::max(1, W >> m), std::max(1, H >> m));
        const int w = std::max(1, W >> level), h = std::max(1, H >> level);
        if (off + msize(w, h) > d.size())
        {
            err = "VTF truncated";
            return false;
        }
        const size_t rowBytes = static_cast<size_t>(w) * 4;
        const size_t pix = rowBytes * h;
        bmp.assign(54 + pix, 0);
        auto put32 = [&](size_t o, std::uint32_t v) { std::memcpy(bmp.data() + o, &v, 4); };
        auto put16 = [&](size_t o, std::uint16_t v) { std::memcpy(bmp.data() + o, &v, 2); };
        bmp[0] = 'B';
        bmp[1] = 'M';
        put32(2, static_cast<std::uint32_t>(bmp.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, static_cast<std::uint32_t>(w));
        put32(22, static_cast<std::uint32_t>(h)); // bottom-up
        put16(26, 1);
        put16(28, 32);
        put32(34, static_cast<std::uint32_t>(pix));
        const std::uint8_t* p = d.data() + off;
        for (int by = 0; by < h; by += 4)
        {
            for (int bx = 0; bx < w; bx += 4)
            {
                std::uint8_t alpha[16];
                std::fill(std::begin(alpha), std::end(alpha), 255);
                if (dxt5)
                {
                    const int a0 = p[0], a1 = p[1];
                    std::uint64_t bits = 0;
                    for (int k = 0; k < 6; ++k)
                        bits |= static_cast<std::uint64_t>(p[2 + k]) << (8 * k);
                    int pal[8] = { a0, a1 };
                    if (a0 > a1)
                        for (int k = 1; k < 7; ++k)
                            pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
                    else
                    {
                        for (int k = 1; k < 5; ++k)
                            pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
                        pal[6] = 0;
                        pal[7] = 255;
                    }
                    for (int k = 0; k < 16; ++k)
                        alpha[k] = static_cast<std::uint8_t>(pal[(bits >> (3 * k)) & 7]);
                    p += 8;
                }
                std::uint16_t c0 = 0, c1 = 0;
                std::uint32_t bits = 0;
                std::memcpy(&c0, p, 2);
                std::memcpy(&c1, p + 2, 2);
                std::memcpy(&bits, p + 4, 4);
                p += 8;
                std::uint8_t col[4][3];
                Rgb565(c0, col[0]);
                Rgb565(c1, col[1]);
                for (int k = 0; k < 3; ++k)
                {
                    if (c0 > c1 || dxt5)
                    {
                        col[2][k] = static_cast<std::uint8_t>((2 * col[0][k] + col[1][k]) / 3);
                        col[3][k] = static_cast<std::uint8_t>((col[0][k] + 2 * col[1][k]) / 3);
                    }
                    else
                    {
                        col[2][k] = static_cast<std::uint8_t>((col[0][k] + col[1][k]) / 2);
                        col[3][k] = 0;
                    }
                }
                for (int py = 0; py < 4; ++py)
                {
                    const int y = by + py;
                    if (y >= h)
                        continue;
                    for (int px = 0; px < 4; ++px)
                    {
                        const int x = bx + px;
                        if (x >= w)
                            continue;
                        const int ci = (bits >> (2 * (py * 4 + px))) & 3;
                        std::uint8_t* o = bmp.data() + 54 + static_cast<size_t>(h - 1 - y) * rowBytes + static_cast<size_t>(x) * 4;
                        o[0] = col[ci][2];
                        o[1] = col[ci][1];
                        o[2] = col[ci][0];
                        o[3] = alpha[py * 4 + px];
                    }
                }
            }
        }
        return true;
    }

    // ---- cache of the parsed game files (both hands, both variants) ----
    struct Source
    {
        bool tried = false;
        bool ok = false;
        std::string error;
        Model model[2];  // per variant
        Bytes bmp[2];
    };
    std::mutex g_Mutex;
    // Cuff caps for the gun's own hands (BmHands::GunCuffCap), wrist-bone local.
    VrHandMeshAsset g_GunCap[2];
    bool g_GunCapValid[2] = { false, false };
    Source g_Src;

    const char* kVariantSmd[2] = { "_hev_standardrig", "_sci_standardrig" };
    const char* kSkin[2] = { "v_hand", "bare_hands_d" };

    std::filesystem::path GameDir()
    {
        wchar_t exeBuf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        return std::filesystem::path(exeBuf).parent_path() / "bms";
    }

    bool LoadSource(std::string& err)
    {
        if (g_Src.tried)
        {
            err = g_Src.error;
            return g_Src.ok;
        }
        g_Src.tried = true;
        const auto dir = GameDir();
        Vpk models, textures;
        if (!models.Open(dir / "bms_models_dir.vpk") || !textures.Open(dir / "bms_textures_dir.vpk"))
        {
            g_Src.error = "cannot open Black Mesa VPKs in " + dir.string();
            err = g_Src.error;
            return false;
        }
        Bytes mdl, vvd, vtx;
        if (!models.Get("models/weapons/v_glock.mdl", mdl) || !models.Get("models/weapons/v_glock.vvd", vvd)
            || !models.Get("models/weapons/v_glock.dx90.vtx", vtx))
        {
            g_Src.error = "v_glock model files missing";
            err = g_Src.error;
            return false;
        }
        for (int v = 0; v < 2; ++v)
        {
            // HEV: the left glove's orange plates and fingertip pads are a second
            // material, v_hand_m (same v_hand base texture, metal shader).
            if (!ParseModel(mdl, vvd, vtx, kVariantSmd[v], kSkin[v], g_Src.model[v], g_Src.error, "arms",
                    v == 0 ? "v_hand_m" : nullptr))
            {
                err = g_Src.error;
                return false;
            }
            Bytes vtf;
            const std::string tn = std::string("materials/models/weapons/v_hand/") + kSkin[v] + ".vtf";
            if (!textures.Get(tn, vtf) || !VtfToBmp(vtf, g_Src.bmp[v], g_Src.error))
            {
                if (g_Src.error.empty())
                    g_Src.error = "texture missing: " + tn;
                err = g_Src.error;
                return false;
            }
        }
        g_Src.ok = true;
        // Black Mesa: Blue Shift (user, 2026-11-18): the gun's own right hand is
        // the guard's (v_glock "arms" = guard_newrig_rework, skin hand_diffuse_1),
        // so the bare VR hands next to it looked like a second person (Black Mesa's
        // scientist). Take the bare hands from Blue Shift's own files; any failure
        // keeps the scientist hands.
        // Same for Azure Sheep: Reborn (user, 2026-11-19): its v_glock, v_shotgun
        // and v_hands carry the guard's own arms ("as_fps_hands_ref", hands skin
        // as_v_hands, sleeves as_v_sleeves).
        struct ModHands
        {
            const char* name;
            const char* folder;
            const char* modelsVpk;
            const char* materialsVpk;
            const char* smd;
            const char* skin;
            // Texture tint: the game shades these with a skin light-warp
            // (Reborn: models/humans/skin_wrp) that our glove shader does not
            // have; without it Reborn's pale skin texture came out grey-white
            // next to the warm gun hand (headless 2026-11-19: VR hand mean
            // (39,30,22) vs gun hand (42,23,9)).
            float tint[3];
            // Texture from Black Mesa's own bms_textures (null = the mod's
            // materials VPK); both = the HEV variant too (the mod's player
            // is never the scientist / HEV Gordon).
            bool textureFromBms;
            bool both;
        };
        const ModHands* mod = nullptr;
        static const ModHands kBlueShift{ "Blue Shift", "bshift", "bshift_models_dir.vpk", "bshift_materials_dir.vpk",
            "guard_newrig_rework", "hand_diffuse_1", { 1.f, 1.f, 1.f }, false, false };
        static const ModHands kReborn{ "Azure Sheep: Reborn", "asheep_reborn", "asheep_reborn_models_dir.vpk",
            "asheep_reborn_materials_dir.vpk", "as_fps_hands_ref", "as_v_hands", { 1.f, 0.75f, 0.42f }, false, false };
        // Black Mesa: Military (user, 2026-11-19): a HECU marine; its v_glock
        // puts Black Mesa's own marine_d texture on the HEV rig, and the VR
        // hands were the orange HEV glove (suit on) or the scientist (intro).
        static const ModHands kMilitary{ "Black Mesa: Military", "bmmilitary", "bmmilitary01.vpk", nullptr,
            "_hev_standardrig", "marine_d", { 1.f, 1.f, 1.f }, true, true };
        if (bmvr::IsBlueShift())
            mod = &kBlueShift;
        else if (bmvr::IsAzureSheepReborn())
            mod = &kReborn;
        else if (bmvr::IsBmMilitary())
            mod = &kMilitary;
        if (mod)
        {
            const auto modDir = dir.parent_path() / mod->folder;
            Vpk modModels, modMaterials;
            Bytes mMdl, mVvd, mVtx, mVtf, mBmp;
            Model mModel;
            std::string mErr;
            const std::string vtfName = std::string("materials/models/weapons/v_hand/") + mod->skin + ".vtf";
            if (!modModels.Open(modDir / mod->modelsVpk)
                || (!mod->textureFromBms && !modMaterials.Open(modDir / mod->materialsVpk)))
                mErr = std::string("cannot open ") + mod->name + " VPKs in " + modDir.string();
            else if (!modModels.Get("models/weapons/v_glock.mdl", mMdl) || !modModels.Get("models/weapons/v_glock.vvd", mVvd)
                || !modModels.Get("models/weapons/v_glock.dx90.vtx", mVtx))
                mErr = std::string(mod->name) + " v_glock model files missing";
            else if (!ParseModel(mMdl, mVvd, mVtx, mod->smd, mod->skin, mModel, mErr))
                ;
            else if (!(mod->textureFromBms ? textures : modMaterials).Get(vtfName, mVtf))
                mErr = std::string(mod->name) + " " + mod->skin + ".vtf missing";
            else if (!VtfToBmp(mVtf, mBmp, mErr))
                ;
            else
            {
                // 32-bit BMP, BGRA after the 54-byte header.
                for (size_t o = 54; o + 3 < mBmp.size(); o += 4)
                    for (int c = 0; c < 3; ++c)
                        mBmp[o + c] = static_cast<std::uint8_t>(mBmp[o + c] * mod->tint[2 - c] + 0.5f);
                if (mod->both)
                {
                    g_Src.model[0] = mModel;
                    g_Src.bmp[0] = mBmp;
                }
                g_Src.model[1] = std::move(mModel);
                g_Src.bmp[1] = std::move(mBmp);
            }
            Game::logMsg("BM hands: %s player hands %s%s", mod->name, mErr.empty() ? "loaded" : "NOT loaded, scientist hands: ",
                mErr.c_str());
        }
        return true;
    }

    int DominantBone(const SrcVert& v)
    {
        int best = v.b[0];
        float bw = -1.f;
        for (int k = 0; k < std::min<int>(3, v.nb); ++k)
        {
            if (v.w[k] > bw)
            {
                bw = v.w[k];
                best = v.b[k];
            }
        }
        return best;
    }
}

bool BmHands::GunCuffCap(int hand, VrHandMeshAsset& out)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (hand < 0 || hand > 1 || !g_GunCapValid[hand])
        return false;
    out = g_GunCap[hand];
    return true;
}

bool BmHands::Build(Variant variant, int hand, float scale, VrHandMeshAsset& out, std::string& error)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (!LoadSource(error))
        return false;
    const int vi = variant == Variant::Hev ? 0 : 1;
    const Model& mdl = g_Src.model[vi];
    const char side = hand == 0 ? 'L' : 'R';
    const char suf = hand == 0 ? 'l' : 'r';

    // Joint plan: wrist + 5 fingers x 3 (SteamVR glove names).
    static const char* kSteam[5] = { "thumb", "index", "middle", "ring", "pinky" };
    static const char* kBm[5] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
    std::vector<std::string> names{ std::string("wrist_") + suf };
    std::vector<int> parents{ -1 };
    std::vector<std::string> src{ std::string(1, side) + "_Wrist" };
    for (int f = 0; f < 5; ++f)
    {
        for (int s = 0; s < 3; ++s)
        {
            names.push_back(std::string("finger_") + kSteam[f] + "_" + static_cast<char>('0' + s) + "_" + suf);
            parents.push_back(s == 0 ? 0 : static_cast<int>(names.size()) - 2);
            src.push_back(std::string(1, side) + "_" + kBm[f] + static_cast<char>('1' + s));
        }
    }
    std::vector<int> srcIds;
    std::map<int, int> srcToJoint;
    for (const auto& n : src)
    {
        int id = -1;
        for (size_t b = 0; b < mdl.bones.size(); ++b)
            if (mdl.bones[b].name == n)
                id = static_cast<int>(b);
        if (id < 0)
        {
            error = "missing bone " + n;
            return false;
        }
        srcToJoint[id] = static_cast<int>(srcIds.size());
        srcIds.push_back(id);
    }

    // Fingertip "nub" bones (bare hands: L_/R_Middlenub carry the middle
    // fingertip) move with the finger's last joint; without them the tip
    // triangles were dropped.
    for (int f = 0; f < 5; ++f)
    {
        const std::string nub = std::string(1, side) + "_" + kBm[f] + "nub";
        for (size_t b = 0; b < mdl.bones.size(); ++b)
            if (_stricmp(mdl.bones[b].name.c_str(), nub.c_str()) == 0)
                srcToJoint[static_cast<int>(b)] = 1 + f * 3 + 2;
    }

    // Glove model space: fingers +Z, back of hand +Y, wrist at the origin.
    auto pos = [&](int joint) { return Col(mdl.bones[srcIds[joint]].world, 3); };
    const V3 wristP = pos(0);
    const V3 finger = pos(3 * 2 + 1 + 2) - wristP;  // finger_middle_2
    const V3 thumb = pos(0 * 3 + 1 + 2) - wristP;   // finger_thumb_2
    V3 palm = Cross(finger, thumb);
    if (Len(palm) < 1e-4f)
        palm = { 0, 0, 1 };
    if (Dot(palm, { 0, 0, 1 }) < 0.f)
        palm = palm * -1.f;
    {
        const V3 tip = pos(2 * 3 + 1 + 2) - wristP;   // finger_middle_2
        const V3 idx = pos(1 * 3 + 1 + 0) - wristP;   // finger_index_0
        const V3 pky = pos(4 * 3 + 1 + 0) - wristP;   // finger_pinky_0
        Game::logMsg("BM hands %s %c: wrist->middle tip joint %.2f, index-pinky knuckles %.2f units",
            vi == 0 ? "hev" : "bare", side, Len(tip), Len(idx - pky));
    }
    M4 align = LookBasis(finger, palm);
    SetCol(align, 3, wristP);
    const M4 toModel = Inv(align);

    std::vector<M4> bind(srcIds.size());
    for (size_t j = 0; j < srcIds.size(); ++j)
    {
        bind[j] = Mul(toModel, mdl.bones[srcIds[j]].world);
        SetCol(bind[j], 3, Col(bind[j], 3) * (1.f / kUnitsPerMeter));
    }

    std::set<int> used;
    for (const auto& t : mdl.tris)
    {
        if (srcToJoint.count(DominantBone(mdl.verts[t[0]])) || srcToJoint.count(DominantBone(mdl.verts[t[1]]))
            || srcToJoint.count(DominantBone(mdl.verts[t[2]])))
            used.insert(t.begin(), t.end());
    }
    if (used.empty() || used.size() > 65535)
    {
        error = "hand vertex count " + std::to_string(used.size());
        return false;
    }

    out = VrHandMeshAsset{};
    std::unordered_map<int, std::uint16_t> remap;
    float maxZ = -1e9f;
    for (const int vi2 : used)
    {
        const SrcVert& v = mdl.verts[vi2];
        VrHandVertex o{};
        const V3 p = XformP(toModel, v.pos) * (1.f / kUnitsPerMeter);
        const V3 n = Norm(XformN(toModel, v.nrm));
        o.position[0] = p.x; o.position[1] = p.y; o.position[2] = p.z;
        o.normal[0] = n.x; o.normal[1] = n.y; o.normal[2] = n.z;
        o.uv[0] = v.u;
        o.uv[1] = v.v;
        int cnt = 0;
        float sum = 0.f;
        for (int k = 0; k < std::min<int>(3, v.nb); ++k)
        {
            const auto it = srcToJoint.find(v.b[k]);
            if (it == srcToJoint.end())
                continue;
            o.joints[cnt] = static_cast<std::uint8_t>(it->second);
            o.weights[cnt] = v.w[k];
            sum += v.w[k];
            ++cnt;
        }
        if (cnt == 0 || sum <= 0.f)
        {
            o.joints[0] = 0;
            o.weights[0] = 1.f;
        }
        else
            for (int k = 0; k < cnt; ++k)
                o.weights[k] /= sum;
        maxZ = std::max(maxZ, p.z);
        remap[vi2] = static_cast<std::uint16_t>(out.vertices.size());
        out.vertices.push_back(o);
    }
    for (const auto& t : mdl.tris)
    {
        const auto a = remap.find(t[0]), b = remap.find(t[1]), c = remap.find(t[2]);
        if (a == remap.end() || b == remap.end() || c == remap.end())
            continue;
        if (a->second == b->second || b->second == c->second || a->second == c->second)
            continue;
        out.indices.push_back(a->second);
        out.indices.push_back(b->second);
        out.indices.push_back(c->second);
    }

    // Close the cuff. The glove is cut out of the first-person arm at the
    // wrist, which leaves the HEV glove's cuff open (bare hands come out
    // closed): wherever the wrist faced the eye, the glove looked hollow
    // (user, 2026-11-18/19). Open edges = edges of one triangle after
    // welding the seam-split vertices by position; those near the wrist get
    // a fan to their centre, weighted like the ring, winding continuing the
    // mesh's own (back-face shading stays right).
    {
        std::map<std::array<int, 3>, int> weld;
        std::vector<int> wid(out.vertices.size());
        for (size_t v = 0; v < out.vertices.size(); ++v)
        {
            const std::array<int, 3> k = { static_cast<int>(std::lround(out.vertices[v].position[0] * 1e4f)),
                static_cast<int>(std::lround(out.vertices[v].position[1] * 1e4f)),
                static_cast<int>(std::lround(out.vertices[v].position[2] * 1e4f)) };
            auto it = weld.find(k);
            if (it == weld.end())
                it = weld.emplace(k, static_cast<int>(v)).first;
            wid[v] = it->second;
        }
        struct E { int n = 0; std::uint16_t a = 0, b = 0; };
        std::map<std::pair<int, int>, E> edges;
        for (size_t t = 0; t + 2 < out.indices.size(); t += 3)
            for (int e = 0; e < 3; ++e)
            {
                const std::uint16_t a = out.indices[t + e], b = out.indices[t + (e + 1) % 3];
                int u = wid[a], w = wid[b];
                if (u > w) std::swap(u, w);
                E& x = edges[{ u, w }];
                ++x.n;
                x.a = a;
                x.b = b;
            }
        std::vector<std::pair<std::uint16_t, std::uint16_t>> ring;
        int openElsewhere = 0;
        for (const auto& kv : edges)
        {
            if (kv.second.n != 1)
                continue;
            const float z = 0.5f * (out.vertices[kv.second.a].position[2] + out.vertices[kv.second.b].position[2]);
            if (maxZ > 1e-4f && z < 0.3f * maxZ)
                ring.push_back({ kv.second.a, kv.second.b });
            else
                ++openElsewhere;
        }
        if (ring.size() >= 3 && out.vertices.size() < 65535)
        {
            VrHandVertex c = out.vertices[ring[0].first];
            float sum[3] = {};
            for (const auto& r : ring)
                for (int k = 0; k < 3; ++k)
                    sum[k] += out.vertices[r.first].position[k] + out.vertices[r.second].position[k];
            for (int k = 0; k < 3; ++k)
                c.position[k] = sum[k] / (2.f * static_cast<float>(ring.size()));
            c.normal[0] = 0.f; c.normal[1] = 0.f; c.normal[2] = -1.f;   // out of the wrist
            const std::uint16_t ci = static_cast<std::uint16_t>(out.vertices.size());
            out.vertices.push_back(c);
            for (const auto& r : ring)
            {
                out.indices.push_back(r.second);
                out.indices.push_back(r.first);
                out.indices.push_back(ci);
            }
            // The same fan for the gun's own hand (viewmodel arms with the
            // forearm folded away show this open cuff too, user 2026-11-19):
            // positions in the wrist bone's frame, model units, one joint.
            if (variant == Variant::Hev)
            {
                const M4 toWrist = Inv(mdl.bones[srcIds[0]].world);
                VrHandMeshAsset& cap = g_GunCap[hand];
                cap = VrHandMeshAsset{};
                std::map<std::uint16_t, std::uint16_t> capIdx;
                auto addVert = [&](std::uint16_t src) -> std::uint16_t {
                    auto it = capIdx.find(src);
                    if (it != capIdx.end())
                        return it->second;
                    VrHandVertex v = out.vertices[src];
                    const V3 model = XformP(align, V3{ v.position[0], v.position[1], v.position[2] } * kUnitsPerMeter);
                    const V3 p = XformP(toWrist, model);
                    const V3 n = Norm(XformN(toWrist, XformN(align, V3{ v.normal[0], v.normal[1], v.normal[2] })));
                    v.position[0] = p.x; v.position[1] = p.y; v.position[2] = p.z;
                    v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
                    v.joints[0] = 0; v.joints[1] = v.joints[2] = v.joints[3] = 0;
                    v.weights[0] = 1.f; v.weights[1] = v.weights[2] = v.weights[3] = 0.f;
                    const std::uint16_t ni = static_cast<std::uint16_t>(cap.vertices.size());
                    cap.vertices.push_back(v);
                    capIdx[src] = ni;
                    return ni;
                };
                for (const auto& r : ring)
                {
                    cap.indices.push_back(addVert(r.second));
                    cap.indices.push_back(addVert(r.first));
                    cap.indices.push_back(addVert(ci));
                }
                cap.jointNames = { "wrist_cap" };
                cap.jointParents = { -1 };
                cap.bindMatrices.resize(1);
                cap.inverseBindMatrices.resize(1);
                for (int k = 0; k < 4; ++k)
                {
                    cap.bindMatrices[0].m[k * 5] = 1.f;
                    cap.inverseBindMatrices[0].m[k * 5] = 1.f;
                }
                cap.baseColorTextureBytes = g_Src.bmp[vi];
                cap.sourcePath = std::string("bm:gun_cuff_cap_") + (hand == 0 ? "left" : "right");
                g_GunCapValid[hand] = cap.IsValid();
            }
        }
        Game::logMsg("BM hands %s %c: cuff %s (%zu open edges at the wrist, %d elsewhere)",
            variant == Variant::Hev ? "hev" : "bare", side, ring.size() >= 3 ? "capped" : "closed already",
            ring.size(), openElsewhere);
    }

    // Fingertips at the origin (the controller tracking point), wrist at -Z.
    for (auto& v : out.vertices)
        v.position[2] -= maxZ;
    for (auto& m : bind)
        m.m[14] -= maxZ;

    // Source bone frames before the roll below: the stock viewmodel finger
    // pose (VrStockHandPose) maps through these.
    std::vector<M4> rawBind = bind;

    // Finger joints: same frame around the bone as the HEV glove rig, so its
    // curl code (local Z hinge, ValveBiped sign) bends them into the palm.
    const float mirror = hand == 0 ? -1.f : 1.f;
    for (size_t j = 1; j < bind.size(); ++j)
    {
        const V3 d = Norm(Col(bind[j], 0));
        V3 n = V3{ 0, 1, 0 } - d * Dot(V3{ 0, 1, 0 }, d);
        n = Norm(n);
        const V3 c = Cross(d, n);
        const float a = mirror * kJointRollRightDeg[j - 1] * 3.14159265f / 180.f;
        const float ca = std::cos(a), sa = std::sin(a);
        // basis(d, n, c) * RotX(a)
        SetCol(bind[j], 0, d);
        SetCol(bind[j], 1, n * ca + c * sa);
        SetCol(bind[j], 2, n * -sa + c * ca);
    }

    // Size.
    for (auto& v : out.vertices)
        for (float& p : v.position)
            p *= scale;
    for (auto& m : bind)
        for (int k = 12; k < 15; ++k)
            m.m[k] *= scale;
    for (auto& m : rawBind)
        for (int k = 12; k < 15; ++k)
            m.m[k] *= scale;

    out.jointNames = names;
    out.jointParents = parents;
    for (const M4& m : bind)
    {
        VrHandMatrix4 b, ib;
        const M4 inv = Inv(m);
        std::copy(std::begin(m.m), std::end(m.m), b.m.begin());
        std::copy(std::begin(inv.m), std::end(inv.m), ib.m.begin());
        out.bindMatrices.push_back(b);
        out.inverseBindMatrices.push_back(ib);
    }
    for (const M4& m : rawBind)
    {
        VrHandMatrix4 b;
        std::copy(std::begin(m.m), std::end(m.m), b.m.begin());
        out.sourceBoneBind.push_back(b);
    }
    out.baseColorTextureBytes = g_Src.bmp[vi];
    // "hev_glove" / "bare_hand" in the path selects the ValveBiped curl sign.
    out.sourcePath = std::string("bm:") + (vi == 0 ? "hev_glove_" : "bare_hand_") + (hand == 0 ? "left" : "right");
    if (!out.IsValid())
    {
        error = "built asset invalid";
        return false;
    }
    return true;
}

