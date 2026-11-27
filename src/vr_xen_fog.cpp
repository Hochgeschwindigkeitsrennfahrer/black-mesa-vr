#include "vr_xen_fog.h"

#include "bmvr_flags.h"
#include "game.h"

#include <windows.h>

#include <cstdio>
#include <set>

namespace bmvr
{
    XenFogShader XenFogPatchPixelShader(const uint32_t* t, std::vector<uint32_t>& out)
    {
        const XenFogPatchInfo info = XenFogPatchBytecode(t, out);
        if (!info.fogLike)
            return XenFogShader::NotFog;
        // Test aid: BMVR_XENFOG_DUMP=1 keeps every fog shader's original
        // bytecode (VR/xenfog-orig-<fnv>.dxso) for the host test.
        static const bool s_dump = [] {
            char v[8] = {};
            return GetEnvironmentVariableA("BMVR_XENFOG_DUMP", v, sizeof(v)) > 0 && v[0] == '1';
        }();
        if (s_dump)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "xenfog-orig-%08X.dxso", info.fnv);
            if (FILE* f = std::fopen(VrFilePath(name).c_str(), "wb"))
            {
                std::fwrite(t, 1, info.bytes, f);
                std::fclose(f);
            }
        }
        if (info.patched)
        {
            static int s_logs;
            if (s_logs++ < 8)
                Game::logMsg("Xen fog shader: %u bytes, FNV %08X: noise patched to world position (r%u world)",
                    static_cast<unsigned>(info.bytes), info.fnv, info.worldReg);
            return XenFogShader::Patched;
        }
        // A fog shader this fix does not understand (another game build): its
        // noise stays view-locked. Counted as a missing gameplay hook (headset
        // warning); bytecode kept once as VR/xenfog-ps-<fnv>.dxso for the port.
        static std::set<uint32_t> s_seen;
        if (s_seen.insert(info.fnv).second && s_seen.size() <= 8)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "xenfog-ps-%08X.dxso", info.fnv);
            if (FILE* f = std::fopen(VrFilePath(name).c_str(), "wb"))
            {
                std::fwrite(t, 1, info.bytes, f);
                std::fclose(f);
            }
            Game::logMsg("Xen fog shader: fog pixel shader %u bytes, FNV %08X not patched (world %d, lookups %d, temps r0-r%u) - Black Mesa updated? Its noise stays view-locked (bytecode: VR/%s)",
                static_cast<unsigned>(info.bytes), info.fnv, info.worldHits, info.lookupHits, info.maxTemp, name);
            Game::noteMissingOffset("Xen fog shader (stereo noise)");
        }
        return XenFogShader::UnknownFog;
    }

    void XenFogPatchFailed(uint32_t bytes)
    {
        Game::logMsg("Xen fog shader: patched shader (%u bytes) rejected by the shader compiler, original used", bytes);
        Game::noteMissingOffset("Xen fog shader (stereo noise)");
    }

    namespace
    {
        float g_Anchor[3] = { 0.f, 0.f, 0.f };
    }

    void SetXenFogAnchor(float x, float y, float z)
    {
        g_Anchor[0] = x;
        g_Anchor[1] = y;
        g_Anchor[2] = z;
    }

    bool XenFogWorldLookups()
    {
        const int mode = static_cast<int>(g_XenFogNoiseMode + 0.5f);
        return mode == 2 || mode == 3;
    }

    void XenFogShaderConstant(float out[8])
    {
        const float size = g_XenFogNoiseSize < 16.f ? 16.f : g_XenFogNoiseSize;
        const int mode = static_cast<int>(g_XenFogNoiseMode + 0.5f);
        out[0] = 1.f / size;
        out[1] = 0.f;
        out[2] = (mode == 1 || mode == 3) ? 1.f : 0.f;   // direction from the shared centre eye
        out[3] = 0.f;                     // world mix slope (per unit of distance)
        out[4] = g_Anchor[0];
        out[5] = g_Anchor[1];
        out[6] = g_Anchor[2];
        out[7] = mode == 2 ? 1.f : 0.f;   // world mix (constant unless Auto)
        if (mode == 3)
        {
            // Auto: mix = (far - distance) / (far - near), clamped.
            const float nearU = g_XenFogWorldNear < 0.f ? 0.f : g_XenFogWorldNear;
            const float farU = g_XenFogWorldFar < nearU + 16.f ? nearU + 16.f : g_XenFogWorldFar;
            out[3] = 1.f / (farU - nearU);
            out[7] = farU / (farU - nearU);
        }
    }
}
