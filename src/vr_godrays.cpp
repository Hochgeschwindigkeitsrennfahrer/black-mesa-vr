#include "vr_godrays.h"

#include "bmvr_flags.h"
#include "game.h"

#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>

namespace bmvr
{
    namespace
    {
        uint32_t RegType(uint32_t p) { return ((p >> 28) & 7u) | (((p >> 11) & 3u) << 3); }
        uint32_t RegNum(uint32_t p) { return p & 0x7FFu; }

        // Instruction token indices (dcl/def/comments skipped), capped.
        template <class F>
        void ForEachInstruction(const uint32_t* t, F&& f)
        {
            for (size_t i = 1; i < 4096; )
            {
                const uint32_t tok = t[i];
                if (tok == 0x0000FFFFu)
                    return;
                if ((tok & 0xFFFFu) == 0xFFFEu)
                {
                    i += 1 + ((tok >> 16) & 0x7FFFu);
                    continue;
                }
                const uint32_t len = (tok >> 24) & 0xFu;
                const uint32_t op = tok & 0xFFFFu;
                if (op != 0x1F && op != 0x51)
                    if (!f(t + i, op, len))
                        return;
                i += 1 + len;
            }
        }

        constexpr uint32_t F(float v)
        {
            return __builtin_bit_cast(uint32_t, v);
        }

        // ps_3_0, hand-assembled:
        //   def c0, 2, -2, -1, 1
        //   def c1, 0.5, 0, 0, 0
        //   dcl vPos.xy
        //   add r0.xy, vPos, c1.x            pixel centre
        //   mul r0.xy, r0, c106              0..1 (c106.xy = 1 / target size, set by us:
        //                                    the stock mask PS reads no constants, so c50
        //                                    is whatever an earlier pass left, 2^24 on the
        //                                    bm_c0a0c tram = every pixel at the light)
        //   mad r0.xy, r0, c0.xy, c0.zw      NDC
        //   mov r0.z, c104.z                 the light's depth
        //   mov r0.w, c0.w
        //   dp4 r1.x..w, r0, c52..c55        inverse view-projection
        //   rcp r1.w, r1.w
        //   mad r1.xyz, r1, r1.w, -c51       V = ray to the light's depth plane (view depth zL)
        //   dp3 r2.x, r1, c105               V . a (a = reference axis, unit)
        //   mul r2.x, r2.x, c104.w           1 / view depth of the threshold point
        //   mad_sat r2.x, r2.x, c104.x, c104.y   -> depth buffer value
        // Threshold on each pixel's ray: the point whose depth along `a`
        // equals the light's (L . a), i.e. the stock test as seen from a head
        // facing `a`. On the ray V (view depth zL) that point has view depth
        // zL (L . a) / (V . a), so 1 / depth = (V . a) / (zL (L . a)).
        // Pixels facing away from `a` get depth 1 (only the sky passes).
        //   mov oDepth, r2.x
        //   mov oC0, c1.y                    the game's mask colour (0)
        const uint32_t kMaskPs[] = {
            0xFFFF0300u,
            0x05000051u, 0xA00F0000u, F(2.f), F(-2.f), F(-1.f), F(1.f),
            0x05000051u, 0xA00F0001u, F(0.5f), 0u, 0u, 0u,
            0x0200001Fu, 0x80000000u, 0x90031000u,
            0x03000002u, 0x80030000u, 0x90E41000u, 0xA0000001u,
            0x03000005u, 0x80030000u, 0x80E40000u, 0xA0E4006Au,
            0x04000004u, 0x80030000u, 0x80E40000u, 0xA0E40000u, 0xA0EE0000u,
            0x02000001u, 0x80040000u, 0xA0AA0068u,
            0x02000001u, 0x80080000u, 0xA0FF0000u,
            0x03000009u, 0x80010001u, 0x80E40000u, 0xA0E40034u,
            0x03000009u, 0x80020001u, 0x80E40000u, 0xA0E40035u,
            0x03000009u, 0x80040001u, 0x80E40000u, 0xA0E40036u,
            0x03000009u, 0x80080001u, 0x80E40000u, 0xA0E40037u,
            0x02000006u, 0x80080001u, 0x80FF0001u,
            0x04000004u, 0x80070001u, 0x80E40001u, 0x80FF0001u, 0xA1E40033u,
            0x03000008u, 0x80010002u, 0x80E40001u, 0xA0E40069u,
            0x03000005u, 0x80010002u, 0x80000002u, 0xA0FF0068u,
            0x04000004u, 0x80110002u, 0x80000002u, 0xA0000068u, 0xA0550068u,
            0x02000001u, 0x900F0800u, 0x80000002u,
            0x02000001u, 0x800F0800u, 0xA0550001u,
            0x0000FFFFu,
        };

        // Light screen positions per eye, in the order of the blur draws
        // (np_gr_quality 2 draws one set per light). The mask of the k-th
        // light uses the k-th blur of the same eye from the previous frame.
        constexpr int kMaxLights = 8;
        struct EyeLights
        {
            float uv[kMaxLights][2] = {};
            int count = 0;        // stored last frame
            int pending = 0;      // stored this frame
            int maskIndex = 0;
        };
        std::mutex g_Mutex;
        EyeLights g_Eye[3];
        int g_LastEye = 0;
        bool g_Logged = false;
        unsigned g_TraceSeq = 0;

        // bmvr_godray_log: one line per draw while the budget lasts.
        bool TraceTake()
        {
            if (!(g_GodRayLog >= 1.f))
                return false;
            g_GodRayLog -= 1.f;
            ++g_TraceSeq;
            return true;
        }

        void EnterEye(int eye)
        {
            if (eye == g_LastEye)
                return;
            g_LastEye = eye;
            EyeLights& e = g_Eye[eye];
            if (g_GodRayLog >= 1.f)
                Game::logMsg("God rays trace: enter eye %d (blurs stored %d, last %d, masks drawn %d)",
                    eye, e.pending, e.count, e.maskIndex);
            if (e.pending > 0)
                e.count = e.pending;
            e.pending = 0;
            e.maskIndex = 0;
        }
    }

    bool GodRayIsMaskVs(const uint32_t* t)
    {
        if (!t || t[0] != 0xFFFE0300u)
            return false;
        int count = 0;
        bool depthFromC27 = false;
        ForEachInstruction(t, [&](const uint32_t* in, uint32_t op, uint32_t len) {
            ++count;
            // mov o<n>.z, c27.zzzz
            if (op == 0x01 && len == 2 && RegType(in[1]) == 6 && ((in[1] >> 16) & 0xFu) == 0x4u
                && RegType(in[2]) == 2 && RegNum(in[2]) == 27 && ((in[2] >> 16) & 0xFFu) == 0xAAu)
                depthFromC27 = true;
            return count < 16;
        });
        return depthFromC27 && count <= 6;
    }

    bool GodRayIsBlurPs(const uint32_t* t)
    {
        if (!t || t[0] != 0xFFFF0300u)
            return false;
        int index = 0, texld = 0;
        bool add = false, mul = false;
        uint32_t reg = 0;
        ForEachInstruction(t, [&](const uint32_t* in, uint32_t op, uint32_t len) {
            if (index == 0)
                // add rN.xy, c10, -vM
                add = op == 0x02 && len == 3 && RegType(in[1]) == 0 && ((in[1] >> 16) & 0xFu) == 0x3u
                    && RegType(in[2]) == 2 && RegNum(in[2]) == 10
                    && RegType(in[3]) == 1 && ((in[3] >> 24) & 0xFu) == 1u;
            else if (index == 1)
            {
                // mul rN.xy, rN, c9.x
                reg = RegNum(in[1]);
                mul = op == 0x05 && len == 3 && RegType(in[1]) == 0 && RegType(in[2]) == 0 && RegNum(in[2]) == reg
                    && RegType(in[3]) == 2 && RegNum(in[3]) == 9;
            }
            if (op == 0x42)
                ++texld;
            ++index;
            return index < 1024;
        });
        return add && mul && texld >= 4;
    }

    bool GodRayIsFinalPs(const uint32_t* t)
    {
        if (!t || t[0] != 0xFFFF0300u)
            return false;
        bool c10 = false, c11 = false, dp2add = false, pw = false;
        int texld = 0, count = 0;
        ForEachInstruction(t, [&](const uint32_t* in, uint32_t op, uint32_t len) {
            ++count;
            if (op == 0x42)
                ++texld;
            if (op == 0x5A)
                dp2add = true;
            if (op == 0x20)
                pw = true;
            for (uint32_t k = 2; k <= len; ++k)
                if (RegType(in[k]) == 2)
                {
                    c10 |= RegNum(in[k]) == 10;
                    c11 |= RegNum(in[k]) == 11;
                }
            return count < 256;
        });
        return c10 && c11 && dp2add && pw && texld >= 1 && texld <= 4 && count < 64;
    }

    int GodRayBlurDivisor()
    {
        const int v = static_cast<int>(g_GodRayBlurScale + 0.5f);
        return v == 1 ? 2 : v == 2 ? 4 : 1;
    }

    const uint32_t* GodRayMaskPs(size_t* tokens)
    {
        if (tokens)
            *tokens = std::size(kMaskPs);
        return kMaskPs;
    }

    void GodRayNoteBlur(int eye, const float c10[4])
    {
        if (eye != 1 && eye != 2)
            return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        EnterEye(eye);
        EyeLights& e = g_Eye[eye];
        if (e.pending < kMaxLights)
        {
            e.uv[e.pending][0] = c10[0];
            e.uv[e.pending][1] = c10[1];
            ++e.pending;
        }
        if (TraceTake())
            Game::logMsg("God rays trace %u: blur eye %d #%d uv %.3f,%.3f", g_TraceSeq, eye, e.pending - 1, c10[0], c10[1]);
    }

    bool GodRayMaskConstants(int eye, float lightDepth, const float c51[4], const float m[16], float out[8])
    {
        if (!g_GodRayRadialMask || (eye != 1 && eye != 2))
            return false;
        float u, v;
        int k = 0, count = 0;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            EnterEye(eye);
            EyeLights& e = g_Eye[eye];
            k = e.maskIndex++;
            count = e.count;
            if (k >= e.count)
            {
                if (TraceTake())
                    Game::logMsg("God rays trace %u: mask eye %d #%d of %d stored: no blur position, stock mask", g_TraceSeq, eye, k, count);
                return false;
            }
            u = e.uv[k][0];
            v = e.uv[k][1];
        }
        const bool trace = TraceTake();
        if (!(lightDepth > 0.f && lightDepth < 1.f) || !std::isfinite(u) || !std::isfinite(v))
        {
            if (trace)
                Game::logMsg("God rays trace %u: mask eye %d #%d/%d uv %.3f,%.3f depth %.6f: rejected, stock mask", g_TraceSeq, eye, k, count, u, v, lightDepth);
            return false;
        }
        // The light: its screen position at its depth, unprojected.
        const float p[4] = { 2.f * u - 1.f, 1.f - 2.f * v, lightDepth, 1.f };
        float w4[4];
        for (int r = 0; r < 4; ++r)
            w4[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3] * p[3];
        if (!(w4[3] > 1e-12f))
            return false;
        const float dx = w4[0] / w4[3] - c51[0], dy = w4[1] / w4[3] - c51[1], dz = w4[2] / w4[3] - c51[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float viewZ = 1.f / w4[3];           // c55 row = 1 / view depth
        // c51-c55 are the game's per-view constants (eye, inverse view-projection)
        // left by this eye's lighting passes; c55 must be a projection's w row.
        const bool invOk = std::fabs(m[14]) > 1e-12f && std::fabs(m[12]) < 1e-4f * std::fabs(m[14])
            && std::fabs(m[13]) < 1e-4f * std::fabs(m[14]);
        if (!(dist > 1.f && dist < 1e7f) || !invOk)
        {
            if (trace)
                Game::logMsg("God rays trace %u: mask eye %d #%d/%d uv %.3f,%.3f depth %.6f dist %.1f: rejected, stock mask", g_TraceSeq, eye, k, count, u, v, lightDepth, dist);
            return false;
        }
        // Reference axis: level, toward the light's yaw (a flat player
        // looking level at it: the look the maps were made with). A light
        // nearly overhead has no yaw: face it directly.
        const float horiz = std::sqrt(dx * dx + dy * dy);
        float a[3];
        if (horiz > 0.25f * dist)
        {
            a[0] = dx / horiz;
            a[1] = dy / horiz;
            a[2] = 0.f;
        }
        else
        {
            a[0] = dx / dist;
            a[1] = dy / dist;
            a[2] = dz / dist;
        }
        const float along = dx * a[0] + dy * a[1] + dz * a[2];   // light depth along a
        if (trace)
            Game::logMsg("God rays trace %u: mask eye %d #%d/%d uv %.3f,%.3f depth %.6f light (%.0f %.0f %.0f) %.0f u away, view depth %.0f, level axis (%.2f %.2f %.2f) along %.0f%s",
                g_TraceSeq, eye, k, count, u, v, lightDepth, w4[0] / w4[3], w4[1] / w4[3], w4[2] / w4[3], dist, viewZ,
                a[0], a[1], a[2], along, along > 1.f ? "" : ": rejected, stock mask");
        if (trace)
            Game::logMsg("God rays trace %u: depth map A %.6g B %.6g (inv rows c55 %.6g %.6g %.6g %.6g): at the light ours %.6f stock %.6f",
                g_TraceSeq, -m[15] / m[14], 1.f / m[14], m[12], m[13], m[14], m[15],
                (1.f / m[14]) * (1.f / viewZ) - m[15] / m[14], lightDepth);
        if (!(along > 1.f))
            return false;
        out[0] = 1.f / m[14];                      // depth = w * (1 / c55.z) - c55.w / c55.z
        out[1] = -m[15] / m[14];
        out[2] = lightDepth;
        out[3] = 1.f / (along * viewZ);
        out[4] = a[0];
        out[5] = a[1];
        out[6] = a[2];
        out[7] = 0.f;
        if (!g_Logged)
        {
            g_Logged = true;
            Game::logMsg("God rays: level mask on (eye %d light at %.3f,%.3f depth %.6f: %.0f u from the eye, %.0f u along the view, %.0f u along the level axis)",
                eye, u, v, lightDepth, dist, viewZ, along);
        }
        return true;
    }
}
