#include "vr_xen_fog_patch.h"

#include <iterator>
#include <set>

namespace
{
    // Black Mesa's Xen fog pixel shaders (global __gbxog0 and the __gbxog*
    // volumes, several compiled combos each) look their noise up by view
    // direction (disassembly: 2026-11-27 log): a triplanar noise from
    // `dir * c15|c17 + time`, then pow(noise, c14.w). Patched by structure,
    // not by hash, so every combo is covered:
    //   world:   the one `mad rW.<3 comps>, .., -c51` (world - eye)
    //   lookups: the two `mad .., <dir>, c15|c17, ..` (noise scale)
    //   block:   from the first c49 (time) read after the world instruction
    //            up to `pow .., rN.c, c14.w` (rN.c = the noise)
    // After the world instruction:
    //   add r7.xyz, c51, -c101          eye - anchor
    //   mad r7.xyz, r7, c100.z, rW      world - (anchor mix: z 0 = this eye, 1 = c101)
    //   add r6.xyz, rW.<comps>, c51     world position
    //   mul r6.xyz, r6, c100.x          x 1 / noise size
    //   nrm r8.xyz, r7                  direction from the anchor
    //   dp3 r9.x, r7, r7
    //   rsq r9.x, r9.x
    //   rcp r9.x, r9.x                  distance from the anchor
    //   mad_sat r9.x, r9.x, -c100.w, c101.w   world mix by distance
    // The game's lookups read r8 (direction). At the block start r0-r5 are
    // copied to r10-r15; before the pow, under `if b15` (world or auto mode),
    // the block runs again on r10-r15 with its lookups reading r6 (world)
    // and its output writes dropped, then
    //   lrp rN.c, r9.x, r(N+10).c, rN.c
    // blends the two noise values. Blending the lookup coordinates instead
    // (before 2026-11-27 evening) gave marbled swirls wherever the mix was
    // between 0 and 1 (bm_c4a1a1 canyon, `fogdistancetest`).
    // World mix = saturate(c101.w - distance * c100.w): c100.w 0 gives the
    // constant c101.w (0 = direction, 1 = world). c100.z 0 = stock (each
    // eye's own view direction, noise at infinity in stereo); c100.z 1 with
    // c101 = the centre eye = the stock look from one point both eyes share
    // (noise on the surfaces, follows the player like flat); mix 1 = fixed
    // to the world, size c100.x. Auto: world up close, centre-eye direction
    // far off, the two noise values cross-faded between near and far.
    // Lookups: the k-th component they write gets r6/r8's k-th (they compute
    // dir * scale + time * scroll per component).
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

        size_t worldAt = 0, worldEnd = 0, lookups[2] = { 0, 0 }, blockAt = 0, powAt = 0;
        int worldHits = 0, lookupHits = 0, powHits = 0;
        uint32_t worldReg = 0, worldSwz = 0xE4u, maxTemp = 0, noiseSrc = 0;
        bool flowOrRel = false;
        std::vector<size_t> insts, relAt;   // instruction token indices
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
            insts.push_back(i);
            if (op != 0x51 && op != 0x1F)
                for (uint32_t k = 1; k <= len && i + k < n; ++k)
                {
                    if (RegType(t[i + k]) == kTemp && RegNum(t[i + k]) > maxTemp)
                        maxTemp = RegNum(t[i + k]);
                    if (t[i + k] & (1u << 13))
                        relAt.push_back(i); // relative addressing
                }
            if ((op >= 0x19 && op <= 0x1E) || (op >= 0x26 && op <= 0x2D) || op == 0x60)
                flowOrRel = true;           // flow control (the copy must not sit in a branch)
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
            // first time read after the world instruction = noise block start
            if (worldAt && !blockAt && i > worldEnd && op != 0x51 && op != 0x1F)
                for (uint32_t k = 2; k <= len && i + k < n; ++k)
                    if (RegType(t[i + k]) == kConst && RegNum(t[i + k]) == 49)
                        blockAt = i;
            // pow dst, rN.c, c14.w (noise exponent)
            if (op == 0x20 && len == 3 && i + 3 < n && RegType(t[i + 2]) == kTemp
                && RegType(t[i + 3]) == kConst && RegNum(t[i + 3]) == 14 && ((t[i + 3] >> 16) & 0xFFu) == 0xFFu)
            {
                ++powHits;
                powAt = i;
                noiseSrc = t[i + 2];
            }
            i += 1 + len;
        }
        for (size_t at : relAt)
            if (at >= blockAt && at < powAt)
                flowOrRel = true;           // relative addressing inside the copied block
        for (size_t at : insts)
            if (at >= blockAt && at < powAt && (t[at] & 0xFFFFu) == 0x41)
                flowOrRel = true;           // texkill inside the copied block
        const uint32_t noiseComp = (noiseSrc >> 16) & 3u;
        const bool ok = worldHits == 1 && lookupHits == 2 && powHits == 1 && maxTemp < 6 && !flowOrRel
            && blockAt > worldEnd && blockAt <= lookups[0] && lookups[0] < lookups[1] && lookups[1] < powAt
            && ((noiseSrc >> 16) & 0xFFu) == noiseComp * 0x55u   // replicate swizzle
            && ((noiseSrc >> 24) & 0xFu) == 0u;                  // no source modifier
        info.worldHits = worldHits;
        info.lookupHits = lookupHits;
        info.worldReg = worldReg;
        info.maxTemp = maxTemp;
        if (!ok)
            return info;
        const uint32_t wYzww = 0x80000000u | (worldSwz << 16) | worldReg;   // world - eye as .xyz
        const uint32_t afterWorld[] = {
            0x03000002u, 0x80070007u, 0xA0E40033u, 0xA1E40065u,        // add r7.xyz, c51, -c101
            0x04000004u, 0x80070007u, 0x80E40007u, 0xA0AA0064u, wYzww, // mad r7.xyz, r7, c100.z, rW.<comps>
            0x03000002u, 0x80070006u, wYzww, 0xA0E40033u,              // add r6.xyz, rW.<comps>, c51
            0x03000005u, 0x80070006u, 0x80E40006u, 0xA0000064u,        // mul r6.xyz, r6, c100.x
            0x02000024u, 0x80070008u, 0x80E40007u,                     // nrm r8.xyz, r7
            0x03000008u, 0x80010009u, 0x80E40007u, 0x80E40007u,        // dp3 r9.x, r7, r7
            0x02000007u, 0x80010009u, 0x80000009u,                     // rsq r9.x, r9.x
            0x02000006u, 0x80010009u, 0x80000009u,                     // rcp r9.x, r9.x
            0x04000004u, 0x80110009u, 0x80000009u, 0xA1FF0064u, 0xA0FF0065u,   // mad_sat r9.x, r9.x, -c100.w, c101.w
        };
        static_assert(std::size(afterWorld) == kXenFogInsertTokens, "insert size");

        // Lookup source a -> r<reg> (temp), source modifier kept; the k-th
        // component the lookup writes reads r<reg>'s k-th.
        auto lookupSrc = [&](size_t at, uint32_t reg) {
            const uint32_t mask = (t[at + 1] >> 16) & 0xFu;
            uint32_t swz = 0, k = 0, last = 0;
            for (uint32_t c = 0; c < 4; ++c)
            {
                uint32_t pick = last;
                if (mask & (1u << c))
                    pick = (k < 3 ? k++ : 2u);
                swz |= pick << (2 * c);
                last = pick;
            }
            return 0x80000000u | (t[at + 2] & (0xFu << 24)) | (swz << 16) | reg;
        };
        auto renamed = [](uint32_t p) {
            return RegType(p) == kTemp && RegNum(p) < 6 ? p + 10u : p;
        };

        // Temps written before the block (copied to r10-r15 at its start).
        uint32_t written = 0;
        for (size_t at : insts)
        {
            if (at >= blockAt)
                break;
            const uint32_t op = t[at] & 0xFFFFu;
            if (op != 0x51 && op != 0x1F && ((t[at] >> 24) & 0xFu) >= 1 && RegType(t[at + 1]) == kTemp)
                written |= 1u << RegNum(t[at + 1]);
        }

        // Second run of the block on r10-r15, lookups from the world (r6).
        std::vector<uint32_t> copy;
        copy.push_back(0x01000028u);   // if b15
        copy.push_back(0xE0E4080Fu);   // b15 (constbool, type 14)
        for (size_t at : insts)
        {
            if (at < blockAt || at >= powAt)
                continue;
            const uint32_t len = (t[at] >> 24) & 0xFu;
            if (len == 0 || RegType(t[at + 1]) != kTemp)
                continue;              // output writes stay the game's
            copy.push_back(t[at]);
            for (uint32_t k = 1; k <= len; ++k)
            {
                uint32_t p = t[at + k];
                if (k == 2 && (at == lookups[0] || at == lookups[1]))
                    p = lookupSrc(at, 6u);
                else
                    p = renamed(p);
                copy.push_back(p);
            }
        }
        copy.push_back(0x0000002Bu);   // endif
        const uint32_t nReg = RegNum(noiseSrc);
        const uint32_t rep = (noiseComp * 0x55u) << 16;
        copy.push_back(0x04000012u);   // lrp rN.c, r9.x, r(N+10).c, rN.c
        copy.push_back(0x80000000u | ((1u << noiseComp) << 16) | nReg);
        copy.push_back(0x80000009u);
        copy.push_back(0x80000000u | rep | (nReg + 10u));
        copy.push_back(0x80000000u | rep | nReg);

        out.clear();
        out.reserve(n + std::size(afterWorld) + 24 + copy.size());
        for (size_t i = 0; i < n; ++i)
        {
            if (i == blockAt)
                for (uint32_t r = 0; r < 6; ++r)
                    if (written & (1u << r))
                    {
                        out.push_back(0x02000001u);                 // mov r(10+r), r
                        out.push_back(0x800F0000u | (r + 10u));
                        out.push_back(0x80E40000u | r);
                    }
            if (i == powAt)
                out.insert(out.end(), copy.begin(), copy.end());
            uint32_t tok = t[i];
            if (i == lookups[0] + 2 || i == lookups[1] + 2)
                tok = lookupSrc(i - 2, 8u);
            out.push_back(tok);
            if (i == worldEnd)
                out.insert(out.end(), std::begin(afterWorld), std::end(afterWorld));
        }
        info.patched = true;
        return info;
    }


}
