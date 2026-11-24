// Cuts the VR reload steps out of Black Mesa's own sounds, once per game
// start, from the installed game files (nothing is shipped): read through
// the game's file system, written as loose files below bms/sound/bmvr/,
// which the engine then finds like any other sound. User 2026-11-20: the
// whole glock reload sequence played for each step.
#include "vr_sound_cuts.h"
#include "game.h"
#include "bmvr_seh.h"
#include "bmvr_flags.h"
#include <windows.h>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // IBaseFileSystem (VBaseFileSystem011), first six entries (as in
    // vr_ladder_rungs.cpp).
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
        HMODULE mod = GetModuleHandleA("filesystem_stdio.dll");
        using tCreateInterface = void*(__cdecl*)(const char*, int*);
        auto create = mod ? reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface")) : nullptr;
        int rc = 0;
        return create ? static_cast<IBaseFileSystemMin*>(create("VBaseFileSystem011", &rc)) : nullptr;
    }

    bool ReadGameFile(const char* path, std::vector<char>& out)
    {
        BMVR_SEH_SAVE_REGS();
        IBaseFileSystemMin* fs = FileSystem();
        if (!fs)
            return false;
        bool ok = false;
        BMVR_TRY
        {
            void* f = fs->Open(path, "rb", "GAME");
            if (f)
            {
                fs->Seek(f, 0, 2);   // FILESYSTEM_SEEK_TAIL
                const unsigned size = fs->Tell(f);
                fs->Seek(f, 0, 0);
                if (size > 44 && size < (16u << 20))
                {
                    out.resize(size);
                    ok = fs->Read(out.data(), static_cast<int>(size), f) == static_cast<int>(size);
                }
                fs->Close(f);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    struct Pcm
    {
        uint16_t channels = 0, bits = 0;
        uint32_t rate = 0;
        const char* data = nullptr;
        uint32_t bytes = 0;
    };

    bool ParseWav(const std::vector<char>& w, Pcm& p)
    {
        if (w.size() < 12 || std::memcmp(w.data(), "RIFF", 4) != 0 || std::memcmp(w.data() + 8, "WAVE", 4) != 0)
            return false;
        size_t pos = 12;
        bool fmt = false;
        while (pos + 8 <= w.size())
        {
            uint32_t len = 0;
            std::memcpy(&len, w.data() + pos + 4, 4);
            const char* body = w.data() + pos + 8;
            if (pos + 8 + len > w.size())
                len = static_cast<uint32_t>(w.size() - pos - 8);
            if (std::memcmp(w.data() + pos, "fmt ", 4) == 0 && len >= 16)
            {
                uint16_t tag = 0;
                std::memcpy(&tag, body, 2);
                std::memcpy(&p.channels, body + 2, 2);
                std::memcpy(&p.rate, body + 4, 4);
                std::memcpy(&p.bits, body + 14, 2);
                fmt = tag == 1 && p.bits == 16 && p.channels >= 1 && p.channels <= 2 && p.rate > 0;
            }
            else if (std::memcmp(w.data() + pos, "data", 4) == 0)
            {
                p.data = body;
                p.bytes = len;
            }
            pos += 8 + len + (len & 1);
        }
        return fmt && p.data && p.bytes > 0;
    }

    // [t0, t1) seconds of p, 10 ms fade in and 25 ms fade out (no clicks).
    bool WriteCut(const Pcm& p, double t0, double t1, const std::wstring& path)
    {
        const uint32_t frame = p.channels * 2u;
        const uint32_t total = p.bytes / frame;
        uint32_t a = static_cast<uint32_t>(t0 * p.rate), b = static_cast<uint32_t>(t1 * p.rate);
        if (b > total)
            b = total;
        if (a >= b)
            return false;
        std::vector<int16_t> s((b - a) * p.channels);
        std::memcpy(s.data(), p.data + static_cast<size_t>(a) * frame, s.size() * 2);
        const uint32_t n = b - a;
        // Same in both ears: the gun is in front (user 2026-11-20: the HL2
        // MP5 bolt sound came from the left; the glock wav leans right).
        if (p.channels == 2)
            for (uint32_t i = 0; i < n; ++i)
            {
                const int16_t m = static_cast<int16_t>((static_cast<int>(s[i * 2]) + s[i * 2 + 1]) / 2);
                s[i * 2] = s[i * 2 + 1] = m;
            }
        const uint32_t fin = p.rate / 100, fout = p.rate / 40;
        for (uint32_t i = 0; i < n; ++i)
        {
            float g = 1.f;
            if (i < fin)
                g = static_cast<float>(i) / fin;
            if (n - i < fout)
                g = (std::min)(g, static_cast<float>(n - i) / fout);
            if (g < 1.f)
                for (uint32_t c = 0; c < p.channels; ++c)
                    s[i * p.channels + c] = static_cast<int16_t>(s[i * p.channels + c] * g);
        }
        const uint32_t dataBytes = static_cast<uint32_t>(s.size() * 2);
        struct Header
        {
            char riff[4]; uint32_t size; char wave[4];
            char fmt[4]; uint32_t fmtLen; uint16_t tag, ch; uint32_t rate, byteRate; uint16_t align, bits;
            char data[4]; uint32_t dataLen;
        } h{ { 'R', 'I', 'F', 'F' }, 36 + dataBytes, { 'W', 'A', 'V', 'E' },
             { 'f', 'm', 't', ' ' }, 16, 1, p.channels, p.rate, p.rate * frame, static_cast<uint16_t>(frame), 16,
             { 'd', 'a', 't', 'a' }, dataBytes };
        // New file + rename: never rewrite a file the engine may have open.
        const std::wstring tmp = path + L".tmp";
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return false;
        DWORD w1 = 0, w2 = 0;
        const bool ok = WriteFile(f, &h, sizeof(h), &w1, nullptr) && WriteFile(f, s.data(), dataBytes, &w2, nullptr)
            && w1 == sizeof(h) && w2 == dataBytes;
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    struct CutDef
    {
        const char* source;
        double t0, t1;
        const char* sample;   // below sound/
    };
    // Glock reload.wav (2.06 s, measured 2026-11-20; parts named by the
    // user by ear): magazine out 0-0.22 s, new magazine slid in 0.48-0.62
    // (short), slide pulled back and let go 1.56-1.94.
    const CutDef kCuts[bmvr::soundcuts::kClipCount] = {
        { "sound/weapons/glock/reload.wav", 0.00, 0.40, "bmvr/glock_magout.wav" },
        { "sound/weapons/glock/reload.wav", 0.44, 0.70, "bmvr/glock_magin.wav" },
        { "sound/weapons/glock/reload.wav", 1.45, 2.06, "bmvr/glock_slide.wav" },
        // MP5 reload.wav: magazine out 0.38-0.50 s, in 0.96-1.10.
        { "sound/weapons/mp5/reload.wav", 0.30, 0.62, "bmvr/mp5_magout.wav" },
        { "sound/weapons/mp5/reload.wav", 0.88, 1.22, "bmvr/mp5_magin.wav" },
        // Crossbow reload.wav: magazine out 0.12-0.22 s, in 1.36-1.54.
        { "sound/weapons/crossbow/reload.wav", 0.05, 0.34, "bmvr/crossbow_magout.wav" },
        { "sound/weapons/crossbow/reload.wav", 1.30, 1.62, "bmvr/crossbow_magin.wav" },
        // MP5 reload_long.wav: charging handle back 0.16-0.46 s (then
        // magazine out 1.0, in 1.7, handle slapped 2.3).
        { "sound/weapons/mp5/reload_long.wav", 0.10, 0.52, "bmvr/mp5_bolt.wav" },
        // Glock slide split at the quiet gap between its two hits (pulled
        // back 1.58 s, snaps forward 1.76 s; measured 2026-11-24): fired dry,
        // the slide goes back on its own, a short rack lets it go forward.
        { "sound/weapons/glock/reload.wav", 1.45, 1.74, "bmvr/glock_slideback.wav" },
        { "sound/weapons/glock/reload.wav", 1.74, 2.06, "bmvr/glock_slidefwd.wav" },
    };
    bool g_Made[bmvr::soundcuts::kClipCount]{};
    // Played name: "<sample minus .wav>_<t0 ms>_<t1 ms>.wav". A changed cut
    // gets a new file: Source's sound cache keeps a sound's length by name,
    // so a shorter clip under an old name played on past its end (user
    // 2026-11-20: electric noise after the glock magazine-out).
    std::string g_Names[bmvr::soundcuts::kClipCount];
    bool g_Tried = false;

    void MakeAll()
    {
        g_Tried = true;
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash);
        // The running game's folder (Blue Shift / Azure Sheep run from their
        // own), as the shotgun pump split (Game::ShotgunPumpSample).
        dir += bmvr::g_IsBlueShift ? L"\\bshift\\sound" : bmvr::g_IsAzureSheep ? L"\\asheep\\sound" : L"\\bms\\sound";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\bmvr";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::string lastSource;
        std::vector<char> wav;
        Pcm pcm;
        bool parsed = false;
        int made = 0;
        for (int i = 0; i < bmvr::soundcuts::kClipCount; ++i)
        {
            const CutDef& c = kCuts[i];
            if (lastSource != c.source)
            {
                lastSource = c.source;
                wav.clear();
                pcm = Pcm{};
                parsed = ReadGameFile(c.source, wav) && ParseWav(wav, pcm);
                if (!parsed)
                    Game::logMsg("Sound cuts: could not read %s (full sounds stay)", c.source);
            }
            if (!parsed)
                continue;
            std::string sample(c.sample);
            sample.resize(sample.size() - 4);   // ".wav"
            char tag[32];
            sprintf_s(tag, "_%d_%d.wav", static_cast<int>(c.t0 * 1000.0 + 0.5), static_cast<int>(c.t1 * 1000.0 + 0.5));
            sample += tag;
            g_Names[i] = sample;
            const size_t slash = sample.find_last_of('/');
            std::wstring path = dir + L"\\";
            for (const char* q = sample.c_str() + (slash == std::string::npos ? 0 : slash + 1); *q; ++q)
                path += static_cast<wchar_t>(*q);
            // Written once; the same cut keeps its file (and its cache entry).
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                g_Made[i] = true;
            else
                g_Made[i] = WriteCut(pcm, c.t0, c.t1, path);
            made += g_Made[i] ? 1 : 0;
        }
        Game::logMsg("Sound cuts: %d/%d clips ready in sound\\bmvr", made, static_cast<int>(bmvr::soundcuts::kClipCount));
    }
}

namespace bmvr::soundcuts
{
    const char* Sample(Clip clip, const char* fallback)
    {
        if (!g_Tried)
            MakeAll();
        return (clip >= 0 && clip < kClipCount && g_Made[clip]) ? g_Names[clip].c_str() : fallback;
    }
}
