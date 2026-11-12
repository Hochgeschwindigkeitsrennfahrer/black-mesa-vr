#include "offset_checks.h"
#include "offsets.h"
#include "game.h"
#include "bmvr_seh.h"

#include <Windows.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "offsets_checks.inc"

namespace
{
    // "8B FF ?? 55" -> bytes, -1 = wildcard.
    std::vector<int> ParsePattern(const char* p)
    {
        std::vector<int> out;
        while (p && *p)
        {
            while (*p == ' ')
                ++p;
            if (!*p)
                break;
            if (p[0] == '?')
            {
                out.push_back(-1);
                while (*p == '?')
                    ++p;
                continue;
            }
            char* end = nullptr;
            out.push_back(static_cast<int>(std::strtoul(p, &end, 16)));
            p = end;
        }
        return out;
    }

    // hookAt: index in the pattern where a mod detour (E9 rel32) may already sit; its 5 bytes are skipped.
    bool MatchAt(uintptr_t addr, const char* pattern, int hookAt = -1)
    {
        std::vector<int> pat = ParsePattern(pattern);
        if (hookAt >= 0 && hookAt + 5 <= static_cast<int>(pat.size())
            && bmvr::seh::IsReadable(reinterpret_cast<const void*>(addr + hookAt), 1)
            && *reinterpret_cast<const uint8_t*>(addr + hookAt) == 0xE9)
        {
            for (int i = hookAt; i < hookAt + 5; ++i)
                pat[i] = -1;
        }
        if (pat.empty() || !bmvr::seh::IsReadable(reinterpret_cast<const void*>(addr), pat.size()))
            return false;
        const auto* b = reinterpret_cast<const uint8_t*>(addr);
        for (size_t i = 0; i < pat.size(); ++i)
        {
            if (pat[i] >= 0 && b[i] != static_cast<uint8_t>(pat[i]))
                return false;
        }
        return true;
    }

    bool ReadU32(uintptr_t addr, uint32_t& out)
    {
        if (!bmvr::seh::IsReadable(reinterpret_cast<const void*>(addr), 4))
            return false;
        std::memcpy(&out, reinterpret_cast<const void*>(addr), 4);
        return true;
    }

    bool PointsToString(uint32_t ptr, const char* s)
    {
        const size_t n = std::strlen(s) + 1;
        if (!ptr || !bmvr::seh::IsReadable(reinterpret_cast<const void*>(static_cast<uintptr_t>(ptr)), n))
            return false;
        return std::memcmp(reinterpret_cast<const void*>(static_cast<uintptr_t>(ptr)), s, n) == 0;
    }

    // Empty = passed, otherwise what did not match.
    std::string RunCheck(uintptr_t base, const OffsetCheck& c)
    {
        switch (c.kind)
        {
        case OffsetCheckKind::Code:
            return MatchAt(base + c.value + c.site, c.pattern, -c.site) ? "" : "code bytes differ";
        case OffsetCheckKind::Data:
        {
            if (!MatchAt(base + c.site, c.pattern))
                return "referencing instruction moved";
            uint32_t v = 0;
            if (!ReadU32(base + c.site + c.ref, v) || v != base + c.value)
                return "game code references another address";
            return "";
        }
        case OffsetCheckKind::FieldPush:
        {
            const auto* b = reinterpret_cast<const uint8_t*>(base + c.site);
            uint32_t off = 0, name = 0;
            if (!bmvr::seh::IsReadable(b, 10) || b[0] != 0x68 || b[5] != 0x68
                || !ReadU32(base + c.site + 1, off) || !ReadU32(base + c.site + 6, name))
                return "field registration moved";
            if (!PointsToString(name, c.field))
                return "field registration moved";
            return off == c.value ? "" : "field offset changed to 0x" + [&] { char t[16]; sprintf_s(t, "%X", off); return std::string(t); }();
        }
        case OffsetCheckKind::FieldRecv:
        {
            uint32_t name = 0, off = 0;
            if (!ReadU32(base + c.site, name) || !PointsToString(name, c.field) || !ReadU32(base + c.site + 4, off))
                return "RecvProp moved";
            return off == c.value ? "" : "RecvProp offset changed";
        }
        }
        return "unknown check";
    }

    bool g_Checked[sizeof(kOffsetFingerprints) / sizeof(kOffsetFingerprints[0])]{};

    std::string Fingerprint(uintptr_t base, const OffsetModuleFingerprint& fp, bool& same)
    {
        same = false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!bmvr::seh::IsReadable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
            return "no PE header";
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (!bmvr::seh::IsReadable(nt, sizeof(*nt)))
            return "no PE header";
        same = nt->FileHeader.TimeDateStamp == fp.timeDateStamp && nt->OptionalHeader.SizeOfImage == fp.sizeOfImage;
        char buf[96];
        sprintf_s(buf, "stamp %08X size %X", static_cast<unsigned>(nt->FileHeader.TimeDateStamp),
            static_cast<unsigned>(nt->OptionalHeader.SizeOfImage));
        return buf;
    }
}

void bmvr::VerifyGameOffsets()
{
    const size_t nModules = sizeof(kOffsetFingerprints) / sizeof(kOffsetFingerprints[0]);
    for (size_t m = 0; m < nModules; ++m)
    {
        if (g_Checked[m])
            continue;
        const OffsetModuleFingerprint& fp = kOffsetFingerprints[m];
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(fp.module));
        if (!base)
            continue;
        g_Checked[m] = true;
        const DWORD t0 = GetTickCount();
        int total = 0, failed = 0;
        for (const OffsetCheck& c : kOffsetChecks)
        {
            if (_stricmp(c.module, fp.module) != 0)
                continue;
            ++total;
            const std::string why = RunCheck(base, c);
            if (why.empty())
                continue;
            ++failed;
            Game::logMsg("Offset check FAILED: %s (%s+0x%X): %s", c.name, c.module, c.value, why.c_str());
            Game::noteMissingOffset((std::string("offset ") + c.name).c_str());
        }
        bool same = false;
        const std::string have = Fingerprint(base, fp, same);
        Game::logMsg("Offset checks %s: %d/%d ok (%u ms); module %s game build %s it was ported for (%s)",
            fp.module, total - failed, total, static_cast<unsigned>(GetTickCount() - t0),
            same ? "IS the" : "is NOT the", kOffsetChecksGameBuild, have.c_str());
    }
}
