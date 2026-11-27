// Host test for src/vr_xen_fog_patch.cpp (no game, no Windows):
//   g++ -std=c++17 -O1 -Isrc tools/tests/xen_fog_patch_test.cpp src/vr_xen_fog_patch.cpp -o /tmp/xfp
//   /tmp/xfp <shader.dxso>... [--write-dir DIR]
// Shaders: VR/audit-ps-*.dxso or VR/xenfog-ps-*.dxso from a game run (game
// bytecode, never committed). Prints per file: fog-like / patched / skipped,
// checks the patch grows the shader by the world insert + the noise block copy, and
// with --write-dir writes <name>.patched.dxso for a disassembler.
#include "vr_xen_fog_patch.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::string writeDir;
    int failures = 0, fog = 0, patched = 0;
    for (int a = 1; a < argc; ++a)
    {
        if (std::strcmp(argv[a], "--write-dir") == 0 && a + 1 < argc)
        {
            writeDir = argv[++a];
            continue;
        }
        std::ifstream f(argv[a], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.size() < 8 || bytes.size() % 4)
        {
            std::printf("%s: not a shader\n", argv[a]);
            ++failures;
            continue;
        }
        std::vector<uint32_t> t(bytes.size() / 4 + 1, 0);
        std::memcpy(t.data(), bytes.data(), bytes.size());
        std::vector<uint32_t> out;
        const bmvr::XenFogPatchInfo info = bmvr::XenFogPatchBytecode(t.data(), out);
        if (!info.fogLike)
            continue;
        ++fog;
        std::printf("%s: %zu bytes FNV %08X world %d lookups %d r%u maxTemp r%u -> %s\n", argv[a], info.bytes,
            info.fnv, info.worldHits, info.lookupHits, info.worldReg, info.maxTemp, info.patched ? "patched" : "NOT patched");
        if (!info.patched)
            continue;
        ++patched;
        const size_t n = info.bytes / 4;
        // world insert + the block's second run (if b15 ... endif, lrp)
        if (out.size() < n + bmvr::kXenFogInsertTokens + 8 || out.back() != 0x0000FFFFu)
        {
            std::printf("  FAIL: %zu tokens from %zu\n", out.size(), n);
            ++failures;
        }
        else
            std::printf("  %zu -> %zu tokens\n", n, out.size());
        if (!writeDir.empty())
        {
            std::string name = argv[a];
            name = name.substr(name.find_last_of('/') + 1);
            std::ofstream o(writeDir + "/" + name + ".patched.dxso", std::ios::binary);
            o.write(reinterpret_cast<const char*>(out.data()), out.size() * 4);
        }
    }
    std::printf("%d fog shader(s), %d patched, %d failure(s)\n", fog, patched, failures);
    return failures ? 1 : 0;
}
