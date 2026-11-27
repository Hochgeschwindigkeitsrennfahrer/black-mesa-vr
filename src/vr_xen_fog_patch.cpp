#include "vr_xen_fog_patch.h"

#include <iterator>
#include <set>

namespace
{
    // Black Mesa's Xen fog pixel shaders (global __gbxog0 and the __gbxog*
    // volumes, several compiled combos each) look their noise up by view
    // direction (disassembly: 2026-11-27 log). Patched by structure, not by
    // hash, so every combo is covered:
    //   world:  the one `mad rW.<3 comps>, .., -c51` (world - eye)
    //   lookups: the two `mad .., <dir>, c15|c17, ..` (noise scale)
    // After the world instruction:
    //   add r7.xyz, c51, -c101          eye - anchor
    //   mad r7.xyz, r7, c100.z, rW      world - (anchor mix: z 0 = this eye, 1 = c101)
    //   add r6.xyz, rW.<comps>, c51     world position
    //   mul r6.xyz, r6, c100.x          x 1 / noise size
    //   nrm r8.xyz, r7                  direction from the anchor
    //   lrp r6.xyz, c100.y, r6, r8      world mix (0 = direction)
    // c100.y 0 + c100.z 0 = stock (each eye's own view direction, noise at
    // infinity in stereo); c100.z 1 with c101 = the centre eye = the stock
    // look from one point both eyes share (noise on the surfaces, follows the
    // player like flat); c100.y 1 = fixed to the world, size c100.x.
    // and the lookups read r6: the k-th component they write gets r6's k-th
    // (they compute dir * scale + time * scroll per component).
    constexpr uint32_t kTemp = 0u, kConst = 2u;

    uint32_t RegType(uint32_t p) { return ((p >> 28) & 7u) | (((p >> 11) & 3u) << 3); }
    uint32_t RegNum(uint32_t p) { return p & 0x7FFu; }

    size_t ShaderTokens(const uint32_t* t)
    {
        // Version token, then instructions/comments up to the end token.
        size_t i = 1;
        for (; i < 16384; )
        {
            const uint32_t tok = t[i];
            if (tok == 0x0000FFFFu)
                return i + 1;
            if ((tok & 0xFFFFu) == 0xFFFEu)
                i += 1 + ((tok >> 16) & 0x7FFFu);
            else
                i += 1 + ((tok >> 24) & 0xFu);
        }
        return 0;
    }

    uint32_t Fnv1a(const uint8_t* p, size_t n)
    {
        uint32_t h = 0x811C9DC5u;
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 0x01000193u;
        return h;
    }

    // The fog's constant set read as shader inputs (constants the shader
    // defines itself do not count: deferred light shaders def c14..c17):
    // fog distance/height c12/c13, noise c14..c17, time c49, eye c51,
    // inverse view-projection c52..c55.
    bool ReadsFogConstants(const uint32_t* t, size_t n)
    {
        std::set<uint32_t> used, defined;
        for (size_t i = 1; i < n; )
        {
            const uint32_t tok = t[i];
            if (tok == 0x0000FFFFu)
                break;
            if ((tok & 0xFFFFu) == 0xFFFEu)
            {
                i += 1 + ((tok >> 16) & 0x7FFFu);
                continue;
            }
            const uint32_t len = (tok >> 24) & 0xFu;
            const uint32_t op = tok & 0xFFFFu;
            if (op == 0x51 && i + 1 < n)
                defined.insert(t[i + 1] & 0x7FFu);
            else if (op != 0x1F)
            {
                for (uint32_t k = 2; k <= len && i + k < n; ++k)
                {
                    const uint32_t p = t[i + k];
                    const uint32_t type = ((p >> 28) & 7u) | (((p >> 11) & 3u) << 3);
                    if (type == 2)
                        used.insert(p & 0x7FFu);
                }
            }
            i += 1 + len;
        }
        for (uint32_t c : { 12u, 13u, 14u, 15u, 16u, 17u, 49u, 51u, 52u, 53u, 54u, 55u })
            if (!used.count(c) || defined.count(c))
                return false;
        return true;
    }
}

namespace bmvr
{
    XenFogPatchInfo XenFogPatchBytecode(const uint32_t* t, std::vector<uint32_t>& out)
    {
        XenFogPatchInfo info;
        if (!t || (t[0] & 0xFFFF0000u) != 0xFFFF0000u)
            return info;
        const size_t n = ShaderTokens(t);
        if (n == 0 || !ReadsFogConstants(t, n))
            return info;
        info.fogLike = true;
        info.bytes = n * 4;
        info.fnv = Fnv1a(reinterpret_cast<const uint8_t*>(t), n * 4);

        size_t worldAt = 0, worldEnd = 0, lookups[2] = { 0, 0 };
        int worldHits = 0, lookupHits = 0;
        uint32_t worldReg = 0, worldSwz = 0xE4u, maxTemp = 0;
        for (size_t i = 1; i < n; )
        {
            const uint32_t tok = t[i];
            if (tok == 0x0000FFFFu)
                break;
            if ((tok & 0xFFFFu) == 0xFFFEu)
            {
                i += 1 + ((tok >> 16) & 0x7FFFu);
                continue;
            }
            const uint32_t len = (tok >> 24) & 0xFu;
            const uint32_t op = tok & 0xFFFFu;
            if (op != 0x51 && op != 0x1F)
                for (uint32_t k = 1; k <= len && i + k < n; ++k)
                    if (RegType(t[i + k]) == kTemp && RegNum(t[i + k]) > maxTemp)
                        maxTemp = RegNum(t[i + k]);
            if (op == 0x04 && len == 4 && i + 4 < n)   // mad dst, a, b, c
            {
                const uint32_t d = t[i + 1], b2 = t[i + 3], c = t[i + 4];
                // world - eye: 3 components written, c = -c51 with the k-th
                // written component reading c51's k-th (.yzw/-c51.xxyz,
                // .xyw/-c51.xyzz, .xyz/-c51 in the combos seen).
                const uint32_t mask = (d >> 16) & 0xFu;
                if (RegType(d) == kTemp && RegType(c) == kConst && RegNum(c) == 51 && ((c >> 24) & 0xFu) == 1u
                    && (mask == 0x7u || mask == 0xBu || mask == 0xDu || mask == 0xEu))
                {
                    const uint32_t cs = (c >> 16) & 0xFFu;
                    uint32_t k = 0, swz = 0, last = 0;
                    bool inOrder = true;
                    for (uint32_t comp = 0; comp < 4; ++comp)
                    {
                        if (!(mask & (1u << comp)))
                            continue;
                        inOrder &= ((cs >> (2 * comp)) & 3u) == k;
                        swz |= comp << (2 * k);
                        last = comp;
                        ++k;
                    }
                    swz |= last << 6;   // 4th = 3rd
                    if (inOrder)
                    {
                        ++worldHits;
                        worldAt = i;
                        worldEnd = i + len;
                        worldReg = RegNum(d);
                        worldSwz = swz;
                    }
                }
                // noise lookup: b = c15 / c17, a = a temp (the direction)
                if (RegType(b2) == kConst && (RegNum(b2) == 15 || RegNum(b2) == 17) && RegType(t[i + 2]) == kTemp)
                {
                    if (lookupHits < 2)
                        lookups[lookupHits] = i;
                    ++lookupHits;
                }
            }
            i += 1 + len;
        }
        const bool ok = worldHits == 1 && lookupHits == 2 && maxTemp < 6
            && lookups[0] > worldAt && lookups[1] > worldAt;
        info.worldHits = worldHits;
        info.lookupHits = lookupHits;
        info.worldReg = worldReg;
        info.maxTemp = maxTemp;
        if (!ok)
            return info;
        const uint32_t wYzww = 0x80000000u | (worldSwz << 16) | worldReg;   // world - eye as .xyz
        const uint32_t insert[] = {
            0x03000002u, 0x80070007u, 0xA0E40033u, 0xA1E40065u,        // add r7.xyz, c51, -c101
            0x04000004u, 0x80070007u, 0x80E40007u, 0xA0AA0064u, wYzww, // mad r7.xyz, r7, c100.z, rW.<comps>
            0x03000002u, 0x80070006u, wYzww, 0xA0E40033u,              // add r6.xyz, rW.<comps>, c51
            0x03000005u, 0x80070006u, 0x80E40006u, 0xA0000064u,        // mul r6.xyz, r6, c100.x
            0x02000024u, 0x80070008u, 0x80E40007u,                     // nrm r8.xyz, r7
            0x04000012u, 0x80070006u, 0xA0550064u, 0x80E40006u, 0x80E40008u,   // lrp r6.xyz, c100.y, r6, r8
        };
        static_assert(std::size(insert) == kXenFogInsertTokens, "insert size");
        out.clear();
        out.reserve(n + 32);
        for (size_t i = 0; i < n; ++i)
        {
            uint32_t tok = t[i];
            if (i == lookups[0] + 2 || i == lookups[1] + 2)
            {
                // src a -> r6 (temp), source modifier kept; the k-th
                // component the lookup writes reads r6's k-th.
                const uint32_t mask = (t[i - 1] >> 16) & 0xFu;
                uint32_t swz = 0, k = 0, last = 0;
                for (uint32_t c = 0; c < 4; ++c)
                {
                    uint32_t pick = last;
                    if (mask & (1u << c))
                        pick = (k < 3 ? k++ : 2u);
                    swz |= pick << (2 * c);
                    last = pick;
                }
                tok = 0x80000000u | (t[i] & (0xFu << 24)) | (swz << 16) | 6u;
            }
            out.push_back(tok);
            if (i == worldEnd)
                out.insert(out.end(), std::begin(insert), std::end(insert));
        }
        info.patched = true;
        return info;
    }


}
