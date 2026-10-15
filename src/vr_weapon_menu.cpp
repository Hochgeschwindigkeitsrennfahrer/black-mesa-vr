#include "vr.h"
#include "game.h"
#include "in_buttons.h"
#include "bmvr_flags.h"
#include "vr_hud_icons.h"
#include "openxr_helper_bridge.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <d3d9.h>

namespace
{
    constexpr int kMaxMenuSlots = 20;
    constexpr DWORD kMenuHoldMs = 180;
    // HL2VR OpenWeaponSelection: PrimaryHandOrigin() + yaw-forward * 4, then a
    // 20 HU VGUI screen (hlvr_weapon_select_size). We draw the same plane in
    // D3D instead of a vgui_screen entity.
    constexpr float kMenuHandForwardHu = 4.f;
    constexpr float kSelectSizeHu = 20.f;
    constexpr int kPanelPx = 1024;
    constexpr int kRadiusPx = 100;
    constexpr int kGapPx = 0;
    constexpr int kCursorPx = 10;
    // Source shareddefs.h MAX_WEAPON_SLOTS. Black Mesa HUD buckets are 0-4.
    constexpr int kMaxWeaponSlots = 6;
    constexpr int kMaxWeaponPositions = 20;
    constexpr float kPi = 3.14159265f;
    constexpr float kRadiusHu = kSelectSizeHu * (static_cast<float>(kRadiusPx) / kPanelPx);
    constexpr float kGapHu = kSelectSizeHu * (static_cast<float>(kGapPx) / kPanelPx);
    constexpr float kCursorRadiusHu = kSelectSizeHu * (static_cast<float>(kCursorPx) / kPanelPx);
    constexpr float kSqrt3 = 1.73205080757f;
    // Same-column centers 2R apart. The flat-top hex in the 2R quad is only
    // ~1.64R tall, which leaves a gutter like the sketch (close, not touching).
    constexpr float kHexSize = (2.f * kRadiusHu) / kSqrt3;
    constexpr float kHexDrawHu = kRadiusHu * 0.90f;

    void YawAroundZ(Vector& v, float yawDeg)
    {
        const float rad = yawDeg * (3.14159265f / 180.f);
        const float s = sinf(rad);
        const float c = cosf(rad);
        const float nx = v.x * c - v.y * s;
        const float ny = v.x * s + v.y * c;
        v.x = nx;
        v.y = ny;
    }

    Vector MenuPlayerBody(const VR* vr)
    {
        // Pre-HMD engine camera origin (same value stereo copies from). Not
        // GetViewOrigin — that includes HMD pitch and made the wheel nod.
        if (vr->m_HasStereoBodyOrigin && vr->m_StereoBodyOrigin.LengthSqr() > 1.f)
            return vr->m_StereoBodyOrigin;
        return vr->m_SetupOrigin;
    }

    int ReadMuzzleFlashParity(void* vm)
    {
        if (!vm)
            return -1;
        int parity = -1;
        __try
        {
            parity = *reinterpret_cast<const unsigned char*>(
                reinterpret_cast<uintptr_t>(vm) + 0x9F8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            parity = -1;
        }
        return parity;
    }

    enum WeaponKind
    {
        KindUnknown = 0,
        KindCrowbar,
        KindGlock,
        KindRevolver,
        KindMp5,
        KindShotgun,
        KindCrossbow,
        KindRpg,
        KindGauss,
        KindGluon,
        KindHivehand,
        KindGrenade,
        KindSatchel,
        KindTripmine,
        KindSnark,
        KindHeadcrab,
        KindCount
    };

    struct MenuVert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };

    struct MenuVertTex
    {
        float x, y, z, rhw;
        D3DCOLOR color;
        float u, v;
    };

    void LowerCopy(char* dst, size_t dstSize, const char* src)
    {
        if (!dst || dstSize == 0)
            return;
        size_t n = 0;
        if (src)
        {
            for (; n + 1 < dstSize && src[n]; ++n)
                dst[n] = static_cast<char>(tolower(static_cast<unsigned char>(src[n])));
        }
        dst[n] = 0;
    }

    bool NameHas(const char* lower, const char* token)
    {
        return lower && token && std::strstr(lower, token) != nullptr;
    }

    WeaponKind KindFromNames(const char* model, const char* net)
    {
        char m[128]{};
        char n[80]{};
        LowerCopy(m, sizeof(m), model);
        LowerCopy(n, sizeof(n), net);
        auto has = [&](const char* t) { return NameHas(m, t) || NameHas(n, t); };
        if (has("crowbar") || has("wrench"))
            return KindCrowbar;
        if (has("glock") || has("pistol") || has("9mm") || has("beretta"))
            return KindGlock;
        if (has("357") || has("python") || has("revolver"))
            return KindRevolver;
        if (has("mp5") || has("smg") || has("mp5k"))
            return KindMp5;
        if (has("shotgun") || has("spas") || has("pump"))
            return KindShotgun;
        if (has("crossbow"))
            return KindCrossbow;
        if (has("rpg") || has("rocket"))
            return KindRpg;
        if (has("gauss") || has("tau"))
            return KindGauss;
        if (has("egon") || has("gluon"))
            return KindGluon;
        if (has("hgun") || has("hive") || has("hornet"))
            return KindHivehand;
        if (has("grenade") || has("frag"))
            return KindGrenade;
        if (has("satchel"))
            return KindSatchel;
        if (has("tripmine") || has("trip"))
            return KindTripmine;
        if (has("squeak") || has("snark"))
            return KindSnark;
        if (has("headcrab"))
            return KindHeadcrab;
        return KindUnknown;
    }

    const char* LabelForKind(WeaponKind kind)
    {
        switch (kind)
        {
        case KindCrowbar: return "CROWBAR";
        case KindGlock: return "GLOCK";
        case KindRevolver: return "357";
        case KindMp5: return "MP5";
        case KindShotgun: return "SHOTGUN";
        case KindCrossbow: return "XBOW";
        case KindRpg: return "RPG";
        case KindGauss: return "TAU";
        case KindGluon: return "GLUON";
        case KindHivehand: return "HIVE";
        case KindGrenade: return "FRAG";
        case KindSatchel: return "SATCHEL";
        case KindTripmine: return "TRIP";
        case KindSnark: return "SNARK";
        case KindHeadcrab: return "CRAB";
        default: return "WEAPON";
        }
    }

    const char* DrawSoundForKind(WeaponKind kind)
    {
        // scripts/game_sounds from bms_misc_000.vpk. Missing names fall back
        // to 2D common/wpn_select.wav in FlushPendingWeaponSounds.
        switch (kind)
        {
        case KindGlock: return "weapon_glock.Draw";
        case KindRevolver: return "weapon_357.draw";
        case KindMp5: return "weapon_mp5.Draw";
        case KindShotgun: return "weapon_shotgun.draw";
        case KindCrossbow: return "weapon_crossbow.Draw";
        case KindRpg: return "weapon_rpg.Draw";
        case KindGauss: return "weapon_tau.Draw";
        case KindGrenade: return "weapon_frag.Draw";
        case KindSnark: return "weapon_snark.draw";
        default: return nullptr;
        }
    }

    const char* HudVtfNameForKind(WeaponKind kind)
    {
        switch (kind)
        {
        case KindCrowbar: return "weapon_crowbar.vtf";
        case KindGlock: return "weapon_glock.vtf";
        case KindRevolver: return "weapon_357.vtf";
        case KindMp5: return "weapon_mp5.vtf";
        case KindShotgun: return "weapon_shotgun.vtf";
        case KindCrossbow: return "weapon_crossbow.vtf";
        case KindRpg: return "weapon_rpg.vtf";
        case KindGauss: return "weapon_tau.vtf";
        case KindGluon: return "weapon_gluon.vtf";
        case KindHivehand: return "weapon_hivehand.vtf";
        case KindGrenade: return "weapon_frag.vtf";
        case KindSatchel: return "weapon_satchel.vtf";
        case KindTripmine: return "weapon_tripmine.vtf";
        case KindSnark: return "weapon_snark.vtf";
        default: return nullptr;
        }
    }

    bool KindIsThrowable(WeaponKind kind)
    {
        return kind == KindGrenade || kind == KindSatchel
            || kind == KindTripmine || kind == KindSnark;
    }

    bool OffsetForKind(WeaponKind kind, int& col, int& row)
    {
        switch (kind)
        {
        case KindCrowbar:  col =  0; row = -1; return true;
        case KindShotgun:  col =  0; row =  1; return true;
        case KindGlock:    col =  1; row =  0; return true;
        case KindRevolver: col = -1; row =  0; return true;
        case KindMp5:      col = -1; row =  1; return true;
        case KindGrenade:  col =  1; row =  1; return true;
        case KindCrossbow: col =  1; row = -1; return true;
        case KindHivehand: col = -1; row = -1; return true;
        case KindSatchel:  col =  2; row =  1; return true;
        case KindTripmine: col =  2; row =  0; return true;
        case KindSnark:    col =  2; row = -1; return true;
        case KindRpg:      col = -2; row =  1; return true;
        case KindGauss:    col = -2; row =  0; return true;
        case KindGluon:    col = -2; row = -1; return true;
        default: return false;
        }
    }

    // even-q 5x3 brick (15 cells). Inner columns have 3 hexes, same as the
    // sketch — not 2. Holster is (0,0); the other 14 take weapons.
    constexpr int kWeaponHex[][2] = {
        {  0, -1 }, {  0,  1 },
        { -1, -1 }, { -1,  0 }, { -1,  1 },
        {  1, -1 }, {  1,  0 }, {  1,  1 },
        { -2, -1 }, { -2,  0 }, { -2,  1 },
        {  2, -1 }, {  2,  0 }, {  2,  1 },
    };

    void OffsetToPlane(int col, int row, float& x, float& y)
    {
        x = kHexSize * 1.5f * static_cast<float>(col);
        const float shove = (col & 1) ? -0.5f : 0.f;
        y = kHexSize * kSqrt3 * (static_cast<float>(row) + shove);
    }

    Vector CirclePlanePoint(const Vector& center, const Vector& planeRight, const Vector& planeUp,
        float radiusHu, float angleDeg)
    {
        const float a = angleDeg * (kPi / 180.f);
        return center + planeRight * (cosf(a) * radiusHu) + planeUp * (sinf(a) * radiusHu);
    }

    void MenuQuad(IDirect3DDevice9* device, float x, float y, float w, float h, D3DCOLOR color)
    {
        MenuVert v[4] = {
            { x, y, 0.f, 1.f, color },
            { x + w, y, 0.f, 1.f, color },
            { x, y + h, 0.f, 1.f, color },
            { x + w, y + h, 0.f, 1.f, color }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVert));
    }

    constexpr int kCircleSegs = 32;

    void DrawCircleFanPts(IDirect3DDevice9* device, float cx, float cy,
        const float* xs, const float* ys, D3DCOLOR color)
    {
        MenuVert v[kCircleSegs + 2]{};
        v[0] = { cx, cy, 0.f, 1.f, color };
        for (int i = 0; i <= kCircleSegs; ++i)
            v[i + 1] = { xs[i], ys[i], 0.f, 1.f, color };
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kCircleSegs, v, sizeof(MenuVert));
    }

    void DrawCircleRingPts(IDirect3DDevice9* device, const float* xo, const float* yo,
        const float* rhwO, const float* xi, const float* yi, const float* rhwI, D3DCOLOR color)
    {
        MenuVert v[(kCircleSegs + 1) * 2]{};
        for (int i = 0; i <= kCircleSegs; ++i)
        {
            v[i * 2] = { xo[i], yo[i], 0.f, rhwO[i], color };
            v[i * 2 + 1] = { xi[i], yi[i], 0.f, rhwI[i], color };
        }
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, kCircleSegs * 2, v, sizeof(MenuVert));
    }

    bool ReadFileBytes(const wchar_t* path, std::vector<unsigned char>& out)
    {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 8 * 1024 * 1024)
        {
            CloseHandle(h);
            return false;
        }
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
        CloseHandle(h);
        return ok && got == out.size();
    }

    bool ExtractVpkFile(const wchar_t* dirVpk, const char* rel, std::vector<unsigned char>& out)
    {
        std::vector<unsigned char> raw;
        if (!ReadFileBytes(dirVpk, raw) || raw.size() < 32)
            return false;
        unsigned sig = 0, ver = 0, treeSize = 0;
        memcpy(&sig, raw.data(), 4);
        memcpy(&ver, raw.data() + 4, 4);
        memcpy(&treeSize, raw.data() + 8, 4);
        if (sig != 0x55AA1234)
            return false;
        size_t header = (ver == 2) ? 28u : 12u;
        if (header + treeSize > raw.size())
            return false;
        const unsigned char* tree = raw.data() + header;
        size_t i = 0;
        auto readZ = [&]() -> std::string {
            std::string s;
            while (i < treeSize && tree[i])
            {
                s.push_back(static_cast<char>(tree[i]));
                ++i;
            }
            if (i < treeSize)
                ++i;
            return s;
        };
        std::string want = rel;
        for (char& c : want)
        {
            if (c == '\\')
                c = '/';
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        unsigned archive = 0x7FFF, offset = 0, length = 0, preload = 0;
        bool found = false;
        std::vector<unsigned char> preloadBytes;
        while (i < treeSize)
        {
            const std::string ext = readZ();
            if (ext.empty())
                break;
            while (i < treeSize)
            {
                const std::string path = readZ();
                if (path.empty())
                    break;
                while (i < treeSize)
                {
                    const std::string name = readZ();
                    if (name.empty())
                        break;
                    if (i + 18 > treeSize)
                        return false;
                    unsigned crc = 0;
                    unsigned short pre = 0, arch = 0, term = 0;
                    unsigned off = 0, len = 0;
                    memcpy(&crc, tree + i, 4);
                    memcpy(&pre, tree + i + 4, 2);
                    memcpy(&arch, tree + i + 6, 2);
                    memcpy(&off, tree + i + 8, 4);
                    memcpy(&len, tree + i + 12, 4);
                    memcpy(&term, tree + i + 16, 2);
                    i += 18;
                    const unsigned char* prePtr = tree + i;
                    i += pre;
                    std::string key = path + "/" + name + "." + ext;
                    for (char& c : key)
                    {
                        if (c == '\\')
                            c = '/';
                        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    }
                    if (!found && key == want)
                    {
                        found = true;
                        archive = arch;
                        offset = off;
                        length = len;
                        preload = pre;
                        preloadBytes.assign(prePtr, prePtr + pre);
                    }
                    (void)crc;
                    (void)term;
                }
            }
        }
        if (!found || length == 0)
            return false;
        if (archive == 0x7FFF)
        {
            out.assign(preloadBytes.begin(), preloadBytes.begin() + (std::min)(preloadBytes.size(), static_cast<size_t>(length)));
            return !out.empty();
        }
        std::wstring dataPath(dirVpk);
        const size_t under = dataPath.rfind(L"_dir.vpk");
        if (under == std::wstring::npos)
            return false;
        wchar_t num[16]{};
        swprintf_s(num, L"_%03u.vpk", archive);
        dataPath.replace(under, wcslen(L"_dir.vpk"), num);
        HANDLE h = CreateFileW(dataPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER pos{};
        pos.QuadPart = offset;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
        {
            CloseHandle(h);
            return false;
        }
        out.resize(length);
        if (preload > 0 && preload <= length)
            memcpy(out.data(), preloadBytes.data(), preload);
        DWORD got = 0;
        const DWORD need = length - preload;
        const BOOL ok = need == 0 || ReadFile(h, out.data() + preload, need, &got, nullptr);
        CloseHandle(h);
        return ok && (need == 0 || got == need);
    }

    void Unpack565(unsigned short c, unsigned char* rgb)
    {
        rgb[0] = static_cast<unsigned char>(((c >> 11) & 31) * 255 / 31);
        rgb[1] = static_cast<unsigned char>(((c >> 5) & 63) * 255 / 63);
        rgb[2] = static_cast<unsigned char>((c & 31) * 255 / 31);
    }

    void DecodeDxtColors(const unsigned char* block, unsigned char px[16][4], bool dxt1)
    {
        const unsigned short c0 = static_cast<unsigned short>(block[0] | (block[1] << 8));
        const unsigned short c1 = static_cast<unsigned short>(block[2] | (block[3] << 8));
        unsigned char rgb[4][3]{};
        Unpack565(c0, rgb[0]);
        Unpack565(c1, rgb[1]);
        if (c0 > c1 || !dxt1)
        {
            for (int i = 0; i < 3; ++i)
            {
                rgb[2][i] = static_cast<unsigned char>((2 * rgb[0][i] + rgb[1][i]) / 3);
                rgb[3][i] = static_cast<unsigned char>((rgb[0][i] + 2 * rgb[1][i]) / 3);
            }
        }
        else
        {
            for (int i = 0; i < 3; ++i)
                rgb[2][i] = static_cast<unsigned char>((rgb[0][i] + rgb[1][i]) / 2);
            rgb[3][0] = rgb[3][1] = rgb[3][2] = 0;
        }
        const unsigned bits = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
        for (int i = 0; i < 16; ++i)
        {
            const int idx = static_cast<int>((bits >> (2 * i)) & 3u);
            px[i][0] = rgb[idx][2];
            px[i][1] = rgb[idx][1];
            px[i][2] = rgb[idx][0];
            px[i][3] = (dxt1 && c0 <= c1 && idx == 3) ? 0 : 255;
        }
    }

    void DecodeDxt5Alpha(const unsigned char* block, unsigned char a[16])
    {
        const unsigned a0 = block[0];
        const unsigned a1 = block[1];
        unsigned tbl[8] = { a0, a1 };
        if (a0 > a1)
        {
            for (int i = 1; i <= 6; ++i)
                tbl[i + 1] = (a0 * (7 - i) + a1 * i) / 7;
        }
        else
        {
            for (int i = 1; i <= 4; ++i)
                tbl[i + 1] = (a0 * (5 - i) + a1 * i) / 5;
            tbl[6] = 0;
            tbl[7] = 255;
        }
        unsigned long long bits = 0;
        memcpy(&bits, block + 2, 6);
        for (int i = 0; i < 16; ++i)
            a[i] = static_cast<unsigned char>(tbl[(bits >> (3 * i)) & 7u]);
    }

    size_t MipByteSize(unsigned format, int w, int h)
    {
        w = (std::max)(1, w);
        h = (std::max)(1, h);
        if (format == 13)
            return static_cast<size_t>(((w + 3) / 4) * ((h + 3) / 4) * 8);
        if (format == 14 || format == 15)
            return static_cast<size_t>(((w + 3) / 4) * ((h + 3) / 4) * 16);
        if (format == 3)
            return static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
        return static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    }

    bool DecodeVtfToBgra(const std::vector<unsigned char>& vtf, int& w, int& h,
        std::vector<unsigned char>& bgra, unsigned* formatOut)
    {
        if (vtf.size() < 80 || memcmp(vtf.data(), "VTF\0", 4) != 0)
            return false;
        unsigned headerSize = 0, format = 0;
        unsigned short vw = 0, vh = 0, frames = 1;
        memcpy(&headerSize, vtf.data() + 12, 4);
        memcpy(&vw, vtf.data() + 16, 2);
        memcpy(&vh, vtf.data() + 18, 2);
        memcpy(&frames, vtf.data() + 24, 2);
        memcpy(&format, vtf.data() + 52, 4);
        const unsigned char mipCount = vtf.size() > 56 ? vtf[56] : 1;
        if (formatOut)
            *formatOut = format;
        w = vw;
        h = vh;
        if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
            return false;
        if (format != 0 && format != 3 && format != 11 && format != 12 && format != 13 && format != 15)
            return false;
        unsigned imageOff = headerSize;
        if (headerSize >= 80 && vtf.size() >= headerSize)
        {
            unsigned numRes = 0;
            memcpy(&numRes, vtf.data() + 68, 4);
            if (numRes > 0 && numRes < 16)
            {
                const size_t resBase = headerSize - static_cast<size_t>(numRes) * 8u;
                for (unsigned r = 0; r < numRes; ++r)
                {
                    const size_t e = resBase + r * 8u;
                    if (e + 8 > vtf.size())
                        break;
                    if (vtf[e] == 0x30)
                    {
                        memcpy(&imageOff, vtf.data() + e + 4, 4);
                        break;
                    }
                }
            }
        }
        size_t skip = 0;
        for (int m = mipCount - 1; m >= 1; --m)
        {
            const int mw = (std::max)(1, w >> m);
            const int mh = (std::max)(1, h >> m);
            skip += MipByteSize(format, mw, mh) * (std::max)(1, static_cast<int>(frames));
        }
        const size_t bytes = MipByteSize(format, w, h);
        if (imageOff + skip + bytes > vtf.size())
            return false;
        const unsigned char* src = vtf.data() + imageOff + skip;
        bgra.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
        if (format == 12)
        {
            memcpy(bgra.data(), src, bytes);
            return true;
        }
        if (format == 0)
        {
            for (int i = 0; i < w * h; ++i)
            {
                bgra[i * 4 + 0] = src[i * 4 + 2];
                bgra[i * 4 + 1] = src[i * 4 + 1];
                bgra[i * 4 + 2] = src[i * 4 + 0];
                bgra[i * 4 + 3] = src[i * 4 + 3];
            }
            return true;
        }
        if (format == 11)
        {
            for (int i = 0; i < w * h; ++i)
            {
                bgra[i * 4 + 0] = src[i * 4 + 3];
                bgra[i * 4 + 1] = src[i * 4 + 2];
                bgra[i * 4 + 2] = src[i * 4 + 1];
                bgra[i * 4 + 3] = src[i * 4 + 0];
            }
            return true;
        }
        if (format == 3)
        {
            for (int i = 0; i < w * h; ++i)
            {
                bgra[i * 4 + 0] = src[i * 3 + 0];
                bgra[i * 4 + 1] = src[i * 3 + 1];
                bgra[i * 4 + 2] = src[i * 3 + 2];
                bgra[i * 4 + 3] = 255;
            }
            return true;
        }
        const bool dxt1 = (format == 13);
        const int bw = (w + 3) / 4;
        const int bh = (h + 3) / 4;
        const int blockBytes = dxt1 ? 8 : 16;
        for (int by = 0; by < bh; ++by)
        {
            for (int bx = 0; bx < bw; ++bx)
            {
                const unsigned char* block = src + (static_cast<size_t>(by) * bw + bx) * blockBytes;
                unsigned char px[16][4]{};
                unsigned char alpha[16];
                if (dxt1)
                {
                    DecodeDxtColors(block, px, true);
                }
                else
                {
                    DecodeDxt5Alpha(block, alpha);
                    DecodeDxtColors(block + 8, px, false);
                    for (int i = 0; i < 16; ++i)
                        px[i][3] = alpha[i];
                }
                for (int py = 0; py < 4; ++py)
                {
                    const int y = by * 4 + py;
                    if (y >= h)
                        break;
                    for (int px_ = 0; px_ < 4; ++px_)
                    {
                        const int x = bx * 4 + px_;
                        if (x >= w)
                            break;
                        memcpy(bgra.data() + (static_cast<size_t>(y) * w + x) * 4, px[py * 4 + px_], 4);
                    }
                }
            }
        }
        return true;
    }

    void CropBgraToAlpha(int& w, int& h, std::vector<unsigned char>& bgra)
    {
        if (w <= 0 || h <= 0 || bgra.size() < static_cast<size_t>(w) * h * 4u)
            return;
        int minx = w, miny = h, maxx = -1, maxy = -1;
        for (int y = 0; y < h; ++y)
        {
            const unsigned char* row = bgra.data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x)
            {
                if (row[x * 4 + 3] <= 24)
                    continue;
                if (x < minx) minx = x;
                if (y < miny) miny = y;
                if (x > maxx) maxx = x;
                if (y > maxy) maxy = y;
            }
        }
        if (maxx < minx || maxy < miny)
            return;
        minx = (std::max)(0, minx - 2);
        miny = (std::max)(0, miny - 2);
        maxx = (std::min)(w - 1, maxx + 2);
        maxy = (std::min)(h - 1, maxy + 2);
        const int nw = maxx - minx + 1;
        const int nh = maxy - miny + 1;
        if (nw >= w && nh >= h)
            return;
        std::vector<unsigned char> cropped(static_cast<size_t>(nw) * nh * 4u);
        for (int y = 0; y < nh; ++y)
        {
            memcpy(cropped.data() + static_cast<size_t>(y) * nw * 4,
                bgra.data() + (static_cast<size_t>(miny + y) * w + minx) * 4,
                static_cast<size_t>(nw) * 4u);
        }
        bgra.swap(cropped);
        w = nw;
        h = nh;
    }

    // HUD VTFs are authored for a 1080p 2D overlay. The wheel bilinear-upscales
    // until the long edge is at least 512 (cap 1024). Wrist icons sit on the
    // hand, so they need the short edge at 768 (cap 2048) and a sharper kernel.
    void UpscaleBgraForVr(int& w, int& h, std::vector<unsigned char>& bgra, bool wrist = false)
    {
        if (w <= 0 || h <= 0 || bgra.size() < static_cast<size_t>(w) * h * 4u)
            return;
        int scale = 1;
        if (wrist)
        {
            constexpr int kMinShort = 768;
            constexpr int kMaxLong = 2048;
            while ((std::min)(w, h) * scale < kMinShort
                && (std::max)(w, h) * scale * 2 <= kMaxLong)
                scale *= 2;
        }
        else
        {
            constexpr int kMinLong = 512;
            constexpr int kMaxLong = 1024;
            while ((std::max)(w, h) * scale < kMinLong
                && (std::max)(w, h) * scale * 2 <= kMaxLong)
                scale *= 2;
        }
        if (scale <= 1)
            return;
        const int nw = w * scale;
        const int nh = h * scale;
        std::vector<unsigned char> dst(static_cast<size_t>(nw) * nh * 4u);
        const float sx = static_cast<float>(w) / static_cast<float>(nw);
        const float sy = static_cast<float>(h) / static_cast<float>(nh);
        auto at = [&](int x, int y, int c) -> float {
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x >= w) x = w - 1;
            if (y >= h) y = h - 1;
            return static_cast<float>(bgra[(static_cast<size_t>(y) * w + x) * 4 + c]);
        };
        auto catmull = [](float t, float p0, float p1, float p2, float p3) {
            const float t2 = t * t;
            const float t3 = t2 * t;
            return 0.5f * ((2.f * p1) + (-p0 + p2) * t
                + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t2
                + (-p0 + 3.f * p1 - 3.f * p2 + p3) * t3);
        };
        for (int y = 0; y < nh; ++y)
        {
            const float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
            const int y1 = static_cast<int>(floorf(fy));
            const float ty = fy - static_cast<float>(y1);
            for (int x = 0; x < nw; ++x)
            {
                const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
                const int x1 = static_cast<int>(floorf(fx));
                const float tx = fx - static_cast<float>(x1);
                unsigned char* out = dst.data() + (static_cast<size_t>(y) * nw + x) * 4;
                for (int c = 0; c < 4; ++c)
                {
                    float v;
                    if (wrist)
                    {
                        float col[4];
                        for (int ky = 0; ky < 4; ++ky)
                        {
                            col[ky] = catmull(tx,
                                at(x1 - 1, y1 - 1 + ky, c),
                                at(x1,     y1 - 1 + ky, c),
                                at(x1 + 1, y1 - 1 + ky, c),
                                at(x1 + 2, y1 - 1 + ky, c));
                        }
                        v = catmull(ty, col[0], col[1], col[2], col[3]);
                    }
                    else
                    {
                        const float v0 = at(x1, y1, c) + (at(x1 + 1, y1, c) - at(x1, y1, c)) * tx;
                        const float v1 = at(x1, y1 + 1, c) + (at(x1 + 1, y1 + 1, c) - at(x1, y1 + 1, c)) * tx;
                        v = v0 + (v1 - v0) * ty;
                    }
                    const int iv = static_cast<int>(v + 0.5f);
                    out[c] = static_cast<unsigned char>(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
                }
            }
        }
        Game::logMsg("HUD icon upscale %dx%d -> %dx%d wrist=%d", w, h, nw, nh, wrist ? 1 : 0);
        bgra.swap(dst);
        w = nw;
        h = nh;
    }

    // Blue Shift's hud_hev_overlay is a wrapping wipe atlas: the heater
    // shield straddles the right edge, and its missing right side is the
    // fragment on the left. Stitch those two column-runs into one glyph.
    // Non-wrapping overlays (HEV suit) fall through to the largest run.
    void UnwrapOrCropHudOverlay(int& w, int& h, std::vector<unsigned char>& bgra)
    {
        if (w < 12 || h < 8 || bgra.size() < static_cast<size_t>(w) * h * 4u)
            return;
        std::vector<int> occ(static_cast<size_t>(w), 0);
        for (int y = 0; y < h; ++y)
        {
            const unsigned char* row = bgra.data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x)
            {
                if (row[x * 4 + 3] > 24)
                    ++occ[x];
            }
        }
        const int minOcc = (std::max)(3, h / 20);
        struct Run { int x0, x1, area; };
        std::vector<Run> runs;
        int run0 = -1, runArea = 0;
        auto close = [&](int x) {
            if (run0 < 0)
                return;
            runs.push_back({ run0, x - 1, runArea });
            run0 = -1;
            runArea = 0;
        };
        for (int x = 0; x < w; ++x)
        {
            if (occ[x] >= minOcc)
            {
                if (run0 < 0)
                    run0 = x;
                runArea += occ[x];
            }
            else
                close(x);
        }
        close(w);
        if (runs.empty())
            return;

        auto copyCols = [&](int src0, int src1) {
            const int nw = src1 - src0 + 1;
            std::vector<unsigned char> cropped(static_cast<size_t>(nw) * h * 4u);
            for (int y = 0; y < h; ++y)
            {
                memcpy(cropped.data() + static_cast<size_t>(y) * nw * 4,
                    bgra.data() + (static_cast<size_t>(y) * w + src0) * 4,
                    static_cast<size_t>(nw) * 4u);
            }
            bgra.swap(cropped);
            w = nw;
        };

        if (runs.size() >= 2 && runs.front().x0 <= 2 && runs.back().x1 >= w - 3)
        {
            const Run& left = runs.front();
            const Run& right = runs.back();
            const int leftW = left.x1 - left.x0 + 1;
            const int rightW = right.x1 - right.x0 + 1;
            const int nw = leftW + rightW;
            std::vector<unsigned char> out(static_cast<size_t>(nw) * h * 4u, 0);
            for (int y = 0; y < h; ++y)
            {
                memcpy(out.data() + static_cast<size_t>(y) * nw * 4,
                    bgra.data() + (static_cast<size_t>(y) * w + right.x0) * 4,
                    static_cast<size_t>(rightW) * 4u);
                memcpy(out.data() + (static_cast<size_t>(y) * nw + rightW) * 4,
                    bgra.data() + (static_cast<size_t>(y) * w + left.x0) * 4,
                    static_cast<size_t>(leftW) * 4u);
            }
            bgra.swap(out);
            w = nw;
            CropBgraToAlpha(w, h, bgra);
            return;
        }

        int best = 0;
        for (int i = 1; i < static_cast<int>(runs.size()); ++i)
        {
            if (runs[i].area > runs[best].area)
                best = i;
        }
        const int minx = (std::max)(0, runs[best].x0 - 2);
        const int maxx = (std::min)(w - 1, runs[best].x1 + 2);
        if (maxx - minx + 1 < w)
            copyCols(minx, maxx);
        CropBgraToAlpha(w, h, bgra);
    }

    // White-on-alpha heater shield matching Blue Shift's CHudArmor icon, used
    // when the bshift VPK is missing. Same 128px canvas as their overlay.
    void RasterizeHeaterShield(int w, int h, std::vector<unsigned char>& bgra)
    {
        bgra.assign(static_cast<size_t>(w) * h * 4u, 0);
        const float cx = (w - 1) * 0.5f;
        const float top = h * 0.08f;
        const float bot = h * 0.92f;
        const float mid = top + (bot - top) * 0.38f;
        const float hwTop = w * 0.32f;
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                float cover = 0.f;
                for (int sy = 0; sy < 2; ++sy)
                {
                    for (int sx = 0; sx < 2; ++sx)
                    {
                        const float px = static_cast<float>(x) + (sx + 0.5f) * 0.5f;
                        const float py = static_cast<float>(y) + (sy + 0.5f) * 0.5f;
                        if (py < top || py > bot)
                            continue;
                        float hw = hwTop;
                        if (py > mid)
                        {
                            const float t = (py - mid) / (bot - mid);
                            hw = hwTop * (1.f - t * t);
                        }
                        if (fabsf(px - cx) <= hw)
                            cover += 0.25f;
                    }
                }
                if (cover <= 0.f)
                    continue;
                const int o = (y * w + x) * 4;
                bgra[o + 0] = 255;
                bgra[o + 1] = 255;
                bgra[o + 2] = 255;
                bgra[o + 3] = static_cast<unsigned char>(cover * 255.f + 0.5f);
            }
        }
    }

    bool UploadBgraTexture(IDirect3DDevice9* device, int w, int h,
        const std::vector<unsigned char>& bgra, IDirect3DTexture9** out)
    {
        *out = nullptr;
        if (!device || w <= 0 || h <= 0)
            return false;
        IDirect3DTexture9* sys = nullptr;
        IDirect3DTexture9* gpu = nullptr;
        if (FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys)
            return false;
        D3DLOCKED_RECT lr{};
        if (FAILED(sys->LockRect(0, &lr, nullptr, 0)))
        {
            sys->Release();
            return false;
        }
        auto* dst = static_cast<unsigned char*>(lr.pBits);
        for (int y = 0; y < h; ++y)
            memcpy(dst + y * lr.Pitch, bgra.data() + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4u);
        sys->UnlockRect(0);
        if (FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpu, nullptr)) || !gpu)
        {
            sys->Release();
            return false;
        }
        const HRESULT hr = device->UpdateTexture(sys, gpu);
        sys->Release();
        if (FAILED(hr))
        {
            gpu->Release();
            return false;
        }
        *out = gpu;
        return true;
    }

    IDirect3DTexture9* g_WeaponIconTex[KindCount]{};
    IDirect3DDevice9* g_WeaponIconDevice = nullptr;
    bool g_WeaponIconTried = false;
    bool g_WeaponIconBlueShift = false;
    IDirect3DTexture9* g_RadialDefaultTex = nullptr;
    IDirect3DTexture9* g_RadialHighlightTex = nullptr;
    bool g_RadialTried = false;

    void RemapHevAmberToCalhoun(std::vector<unsigned char>& bgra);

    bool DecodeVtfFileBgra(const wchar_t* path, bool remapAmber, bool stretchAlpha,
        int& w, int& h, std::vector<unsigned char>& bgra)
    {
        std::vector<unsigned char> vtf;
        if (!ReadFileBytes(path, vtf))
            return false;
        unsigned format = 0;
        if (!DecodeVtfToBgra(vtf, w, h, bgra, &format))
        {
            Game::logMsg("Weapon menu radial vtf decode fail %ls fmt=%u size=%u",
                path, format, static_cast<unsigned>(vtf.size()));
            return false;
        }
        if (remapAmber)
            RemapHevAmberToCalhoun(bgra);
        if (stretchAlpha)
        {
            for (size_t i = 3; i < bgra.size(); i += 4)
            {
                const unsigned a = bgra[i];
                bgra[i] = static_cast<unsigned char>((std::min)(255u, a * 2u));
            }
        }
        return true;
    }

    bool LoadVtfTexture(IDirect3DDevice9* device, const wchar_t* path, IDirect3DTexture9** out,
        bool remapAmber, bool stretchAlpha)
    {
        *out = nullptr;
        int w = 0, h = 0;
        std::vector<unsigned char> bgra;
        if (!DecodeVtfFileBgra(path, remapAmber, stretchAlpha, w, h, bgra))
            return false;
        if (!UploadBgraTexture(device, w, h, bgra, out) || !*out)
        {
            Game::logMsg("Weapon menu radial upload fail %ls %dx%d", path, w, h);
            return false;
        }
        Game::logMsg("Weapon menu radial loaded %ls %dx%d remap=%d stretchA=%d",
            path, w, h, remapAmber ? 1 : 0, stretchAlpha ? 1 : 0);
        return true;
    }

    void CollectRadialSearchDirs(std::vector<std::wstring>& dirs)
    {
        auto add = [&](std::wstring p) {
            if (p.empty())
                return;
            while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
                p.pop_back();
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                dirs.push_back(std::move(p));
        };
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring exeDir(exe);
        const size_t slash = exeDir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            exeDir.resize(slash);
        add(exeDir + L"\\VR\\weapon_wheel");
        add(exeDir + L"\\bin\\VR\\weapon_wheel");

        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LoadVtfTexture), &self);
        if (self)
        {
            wchar_t dll[MAX_PATH]{};
            GetModuleFileNameW(self, dll, MAX_PATH);
            std::wstring dllDir(dll);
            const size_t ds = dllDir.find_last_of(L"\\/");
            if (ds != std::wstring::npos)
                dllDir.resize(ds);
            add(dllDir + L"\\VR\\weapon_wheel");
            add(dllDir);
        }

        add(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life 2 VR\\hlvr\\materials\\HUD");
        add(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Black Mesa\\VR\\weapon_wheel");
    }

    void KickIconPreload();

    void EnsureRadialTextures(IDirect3DDevice9* device)
    {
        if (!device)
            return;
        if (g_WeaponIconDevice && g_WeaponIconDevice != device)
        {
            g_RadialTried = false;
            if (g_RadialDefaultTex)
            {
                g_RadialDefaultTex->Release();
                g_RadialDefaultTex = nullptr;
            }
            if (g_RadialHighlightTex)
            {
                g_RadialHighlightTex->Release();
                g_RadialHighlightTex = nullptr;
            }
        }
        if (g_RadialTried)
            return;
        g_RadialTried = true;
        KickIconPreload();
    }

    // HEV HUD silhouettes are orange. Blue Shift / Azure Sheep use Calhoun
    // blue (same RGB as the pause-menu cursor). R↔B swap maps (255,176,0) to
    // (0,176,255), next to Calhoun (64,168,255). Skip if the atlas is
    // already blue-dominant so a native bshift VTF is left alone.
    void RemapHevAmberToCalhoun(std::vector<unsigned char>& bgra)
    {
        unsigned long long sumR = 0, sumB = 0, n = 0;
        for (size_t i = 0; i + 3 < bgra.size(); i += 4)
        {
            if (bgra[i + 3] < 16)
                continue;
            sumB += bgra[i];
            sumR += bgra[i + 2];
            ++n;
        }
        if (n == 0 || sumB >= sumR)
            return;
        for (size_t i = 0; i + 3 < bgra.size(); i += 4)
            std::swap(bgra[i], bgra[i + 2]);
    }

    struct WheelPalette
    {
        D3DCOLOR fillHover;
        D3DCOLOR fillIdle;
        D3DCOLOR fillDryHover;
        D3DCOLOR fillDryIdle;
        D3DCOLOR frameHover;
        D3DCOLOR frameEquipped;
        D3DCOLOR frameIdle;
        D3DCOLOR frameDryHover;
        D3DCOLOR frameDryIdle;
        D3DCOLOR glow;
        D3DCOLOR glowDry;
        D3DCOLOR tintHover;
        D3DCOLOR tintIdle;
        D3DCOLOR tintDryHover;
        D3DCOLOR tintDryIdle;
        D3DCOLOR glyphHover;
        D3DCOLOR glyphIdle;
        D3DCOLOR glyphDryHover;
        D3DCOLOR glyphDryIdle;
    };

    WheelPalette MakeWheelPalette()
    {
        WheelPalette p{};
        // Empty-clip warning stays red in both campaigns.
        p.fillDryHover = D3DCOLOR_ARGB(220, 72, 16, 12);
        p.fillDryIdle = D3DCOLOR_ARGB(190, 42, 10, 8);
        p.frameDryHover = D3DCOLOR_XRGB(255, 120, 90);
        p.frameDryIdle = D3DCOLOR_XRGB(255, 56, 40);
        p.glowDry = D3DCOLOR_ARGB(170, 255, 90, 70);
        p.tintDryHover = D3DCOLOR_XRGB(255, 170, 150);
        p.tintDryIdle = D3DCOLOR_XRGB(255, 72, 56);
        p.glyphDryHover = D3DCOLOR_XRGB(255, 160, 140);
        p.glyphDryIdle = D3DCOLOR_XRGB(255, 72, 56);
        if (bmvr::UseBlueHud())
        {
            p.fillHover = D3DCOLOR_ARGB(210, 10, 28, 52);
            p.fillIdle = D3DCOLOR_ARGB(170, 6, 14, 28);
            p.frameHover = D3DCOLOR_XRGB(64, 168, 255);
            p.frameEquipped = D3DCOLOR_XRGB(64, 168, 255);
            p.frameIdle = D3DCOLOR_ARGB(255, 64, 168, 255);
            p.glow = D3DCOLOR_ARGB(255, 90, 190, 255);
            p.tintHover = D3DCOLOR_XRGB(255, 255, 255);
            // HL2VR scheme FgColor analog: Calhoun blue at the same 180 alpha.
            p.tintIdle = D3DCOLOR_ARGB(180, 64, 168, 255);
            // Icons are already Calhoun-blue; a red multiply would go purple.
            p.tintDryHover = D3DCOLOR_XRGB(210, 180, 190);
            p.tintDryIdle = D3DCOLOR_XRGB(140, 110, 130);
            p.glyphHover = D3DCOLOR_XRGB(200, 230, 255);
            p.glyphIdle = D3DCOLOR_XRGB(64, 168, 255);
        }
        else
        {
            p.fillHover = D3DCOLOR_ARGB(210, 48, 36, 10);
            p.fillIdle = D3DCOLOR_ARGB(170, 14, 12, 8);
            p.frameHover = D3DCOLOR_XRGB(255, 196, 32);
            p.frameEquipped = D3DCOLOR_XRGB(255, 184, 16);
            p.frameIdle = D3DCOLOR_ARGB(255, 255, 184, 16);
            p.glow = D3DCOLOR_ARGB(255, 255, 200, 56);
            p.tintHover = D3DCOLOR_XRGB(255, 255, 255);
            p.tintIdle = D3DCOLOR_XRGB(255, 255, 255);
            p.glyphHover = D3DCOLOR_XRGB(255, 255, 200);
            p.glyphIdle = D3DCOLOR_XRGB(255, 176, 0);
        }
        return p;
    }

    void ReleaseHudIconCache();
    void InvalidateIconPreload();
    void KickIconPreload();

    void ReleaseWeaponIcons()
    {
        InvalidateIconPreload();
        for (int i = 0; i < KindCount; ++i)
        {
            if (g_WeaponIconTex[i])
            {
                g_WeaponIconTex[i]->Release();
                g_WeaponIconTex[i] = nullptr;
            }
        }
        g_WeaponIconDevice = nullptr;
        g_WeaponIconTried = false;
        g_WeaponIconBlueShift = false;
        if (g_RadialDefaultTex)
        {
            g_RadialDefaultTex->Release();
            g_RadialDefaultTex = nullptr;
        }
        if (g_RadialHighlightTex)
        {
            g_RadialHighlightTex->Release();
            g_RadialHighlightTex = nullptr;
        }
        g_RadialTried = false;
        ReleaseHudIconCache();
    }

    std::vector<std::wstring> HudVpkPaths()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash);
        const wchar_t* names[] = {
            L"\\bms\\bms_textures_dir.vpk",
            L"\\bms\\bms_misc_dir.vpk",
            L"\\bms\\bms_materials_dir.vpk",
            L"\\bms_textures_dir.vpk",
            L"\\bms_misc_dir.vpk"
        };
        std::vector<std::wstring> out;
        for (const wchar_t* n : names)
        {
            std::wstring p = dir + n;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                out.push_back(std::move(p));
        }
        return out;
    }

    std::vector<std::wstring> BlueShiftHudVpkPaths()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash);
        const wchar_t* names[] = {
            L"\\bshift\\bshift_materials_dir.vpk",
            L"\\bshift\\bshift_misc_dir.vpk",
            L"\\bshift_materials_dir.vpk"
        };
        std::vector<std::wstring> out;
        for (const wchar_t* n : names)
        {
            std::wstring p = dir + n;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                out.push_back(std::move(p));
        }
        return out;
    }

    bool ExtractHudVtfByName(const std::vector<std::wstring>& vpks, const char* name,
        std::vector<unsigned char>& vtf)
    {
        if (!name || !*name)
            return false;
        char rels[4][96]{};
        sprintf_s(rels[0], "materials/vgui/hud/%s", name);
        sprintf_s(rels[1], "materials/vgui/hud/weapons/%s", name);
        sprintf_s(rels[2], "materials/hud/%s", name);
        const char* bare = name;
        if (strncmp(name, "weapon_", 7) == 0)
            bare = name + 7;
        sprintf_s(rels[3], "materials/vgui/hud/%s", bare);
        auto search = [&](const std::vector<std::wstring>& list) -> bool {
            for (const auto& vpk : list)
            {
                for (const auto& rel : rels)
                {
                    if (rel[0] && ExtractVpkFile(vpk.c_str(), rel, vtf))
                        return true;
                }
            }
            return false;
        };
        // Blue Shift replaces the HEV figure with a heater shield at the same
        // overlay path (scripts/hudlayout.res SuitTexture). Search bshift VPKs
        // first so the wrist HUD does not keep the base-game suit icon.
        if (bmvr::IsBlueShift() && _stricmp(name, "hud_hev_overlay.vtf") == 0)
        {
            if (search(BlueShiftHudVpkPaths()))
                return true;
        }
        return search(vpks);
    }

    bool ExtractHudVtf(const std::vector<std::wstring>& vpks, WeaponKind kind,
        std::vector<unsigned char>& vtf)
    {
        return ExtractHudVtfByName(vpks, HudVtfNameForKind(kind), vtf);
    }

    enum class PrepKind { Weapon, Hud, RadialDefault, RadialSelected };

    struct PrepBlob
    {
        PrepKind kind = PrepKind::Hud;
        int weaponKind = 0;
        char name[64]{};
        int w = 0;
        int h = 0;
        int generation = 0;
        std::vector<unsigned char> bgra;
    };

    std::mutex g_prepMu;
    std::vector<PrepBlob> g_prepQueue;
    std::atomic<int> g_prepGen{ 0 };
    int g_prepSignaledGen = -1;
    bool g_prepBlue = false;
    HANDLE g_prepEvent = nullptr;
    std::atomic<bool> g_prepThreadUp{ false };

    void InvalidateIconPreload()
    {
        g_prepGen.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(g_prepMu);
        g_prepQueue.clear();
        g_prepSignaledGen = -1;
    }

    bool PrepareHudBgra(const std::vector<std::wstring>& vpks, const char* name, bool wantBlue,
        bool wrist, int& w, int& h, std::vector<unsigned char>& bgra)
    {
        std::vector<unsigned char> vtf;
        if (!ExtractHudVtfByName(vpks, name, vtf))
        {
            if (bmvr::IsBlueShift() && name && _stricmp(name, "hud_hev_overlay.vtf") == 0)
            {
                w = 128;
                h = 128;
                RasterizeHeaterShield(w, h, bgra);
                CropBgraToAlpha(w, h, bgra);
                UpscaleBgraForVr(w, h, bgra, true);
                return true;
            }
            Game::logMsg("HUD icon preload missing %s", name ? name : "");
            return false;
        }
        unsigned format = 0;
        if (!DecodeVtfToBgra(vtf, w, h, bgra, &format))
        {
            Game::logMsg("HUD icon preload decode fail %s fmt=%u", name, format);
            return false;
        }
        CropBgraToAlpha(w, h, bgra);
        if (name && _stricmp(name, "hud_hev_overlay.vtf") == 0)
            UnwrapOrCropHudOverlay(w, h, bgra);
        if (wantBlue)
            RemapHevAmberToCalhoun(bgra);
        UpscaleBgraForVr(w, h, bgra, wrist);
        return w > 0 && h > 0 && !bgra.empty();
    }

    bool PrepStillCurrent(int gen)
    {
        return gen == g_prepGen.load(std::memory_order_acquire);
    }

    void EnqueuePrep(PrepBlob&& blob)
    {
        std::lock_guard<std::mutex> lock(g_prepMu);
        if (blob.generation != g_prepGen.load(std::memory_order_acquire))
            return;
        g_prepQueue.push_back(std::move(blob));
    }

    void PreloadIconsJob(int gen, bool wantBlue)
    {
        const auto vpks = HudVpkPaths();
        if (vpks.empty())
            Game::logMsg("HUD icon preload: no bms_*_dir.vpk next to bms.exe");
        int weapons = 0;
        for (int k = 1; k < KindCount && PrepStillCurrent(gen); ++k)
        {
            const char* name = HudVtfNameForKind(static_cast<WeaponKind>(k));
            if (!name)
                continue;
            PrepBlob blob;
            blob.kind = PrepKind::Weapon;
            blob.weaponKind = k;
            blob.generation = gen;
            if (!PrepareHudBgra(vpks, name, wantBlue, false, blob.w, blob.h, blob.bgra))
                continue;
            EnqueuePrep(std::move(blob));
            ++weapons;
        }
        const char* hudNames[] = {
            "hud_health_overlay.vtf",
            "hud_hev_overlay.vtf",
            "damage_drown.vtf",
            "ammo_357.vtf",
            "ammo_buckshot.vtf",
            "ammo_bolt.vtf",
            "ammo_energy.vtf",
            "ammo_grenade_rpg.vtf",
            "ammo_grenade_hornet.vtf",
            "ammo_grenade_frag.vtf",
            "ammo_grenade_satchel.vtf",
            "ammo_grenade_tripmine.vtf",
            "ammo_snark.vtf",
            "ammo_9mm.vtf",
            "ammo_grenade_mp5.vtf"
        };
        int hud = 0;
        for (const char* name : hudNames)
        {
            if (!PrepStillCurrent(gen))
                break;
            PrepBlob blob;
            blob.kind = PrepKind::Hud;
            blob.generation = gen;
            strncpy_s(blob.name, name, _TRUNCATE);
            if (!PrepareHudBgra(vpks, name, wantBlue, true, blob.w, blob.h, blob.bgra))
                continue;
            EnqueuePrep(std::move(blob));
            ++hud;
        }
        if (PrepStillCurrent(gen))
        {
            std::vector<std::wstring> dirs;
            CollectRadialSearchDirs(dirs);
            const wchar_t* names[2] = {
                L"RadialMenu_Background_Default.vtf",
                L"RadialMenu_Background_Selected.vtf"
            };
            for (int i = 0; i < 2 && PrepStillCurrent(gen); ++i)
            {
                bool got = false;
                for (const auto& dir : dirs)
                {
                    PrepBlob blob;
                    blob.kind = (i == 0) ? PrepKind::RadialDefault : PrepKind::RadialSelected;
                    blob.generation = gen;
                    const std::wstring path = dir + L"\\" + names[i];
                    if (!DecodeVtfFileBgra(path.c_str(), wantBlue, i == 0, blob.w, blob.h, blob.bgra))
                        continue;
                    EnqueuePrep(std::move(blob));
                    got = true;
                    break;
                }
                if (!got)
                    Game::logMsg("Weapon menu radial missing %ls", names[i]);
            }
        }
        if (PrepStillCurrent(gen))
            Game::logMsg("HUD icon preload ready weapons=%d hud=%d", weapons, hud);
    }

    DWORD WINAPI IconPreloadThread(void*)
    {
        for (;;)
        {
            WaitForSingleObject(g_prepEvent, INFINITE);
            int gen = 0;
            bool blue = false;
            {
                std::lock_guard<std::mutex> lock(g_prepMu);
                gen = g_prepGen.load(std::memory_order_acquire);
                blue = g_prepBlue;
            }
            PreloadIconsJob(gen, blue);
        }
    }

    void KickIconPreload()
    {
        std::lock_guard<std::mutex> lock(g_prepMu);
        if (!g_prepEvent)
            g_prepEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_prepEvent)
            return;
        if (!g_prepThreadUp.load(std::memory_order_relaxed))
        {
            HANDLE thread = CreateThread(nullptr, 0, IconPreloadThread, nullptr, 0, nullptr);
            if (!thread)
                return;
            CloseHandle(thread);
            g_prepThreadUp.store(true, std::memory_order_relaxed);
        }
        g_prepBlue = bmvr::UseBlueHud();
        const int gen = g_prepGen.load(std::memory_order_acquire);
        if (g_prepSignaledGen == gen)
            return;
        g_prepSignaledGen = gen;
        SetEvent(g_prepEvent);
    }

    // Name-keyed cache for HUD icons the wrist HUD asks for by filename
    // (health cross, HEV figure, ammo types). Separate from the weapon-kind
    // array above because those are indexed by WeaponKind.
    struct HudIconEntry
    {
        char name[64]{};
        IDirect3DTexture9* tex = nullptr;
    };
    constexpr int kHudIconCacheMax = 32;
    HudIconEntry g_HudIconCache[kHudIconCacheMax]{};
    int g_HudIconCount = 0;
    IDirect3DDevice9* g_HudIconDevice = nullptr;
    bool g_HudIconBlueShift = false;

    void StoreHudTexture(const char* name, IDirect3DTexture9* tex, bool wantBlue)
    {
        if (!name || !tex)
            return;
        for (int i = 0; i < g_HudIconCount; ++i)
        {
            if (_stricmp(g_HudIconCache[i].name, name) == 0)
            {
                if (g_HudIconCache[i].tex)
                    g_HudIconCache[i].tex->Release();
                g_HudIconCache[i].tex = tex;
                return;
            }
        }
        if (g_HudIconCount >= kHudIconCacheMax)
        {
            tex->Release();
            return;
        }
        HudIconEntry& entry = g_HudIconCache[g_HudIconCount++];
        strncpy_s(entry.name, name, _TRUNCATE);
        entry.tex = tex;
        g_HudIconBlueShift = wantBlue;
    }

    void DrainOneHudIconUpload(IDirect3DDevice9* device)
    {
        if (!device)
            return;
        const bool wantBlue = bmvr::UseBlueHud();
        if ((g_WeaponIconDevice && g_WeaponIconDevice != device)
            || ((g_WeaponIconDevice || g_HudIconCount > 0) && g_WeaponIconBlueShift != wantBlue))
            ReleaseWeaponIcons();
        g_WeaponIconDevice = device;
        g_HudIconDevice = device;
        g_WeaponIconBlueShift = wantBlue;
        g_HudIconBlueShift = wantBlue;
        KickIconPreload();

        PrepBlob blob;
        {
            std::lock_guard<std::mutex> lock(g_prepMu);
            const int gen = g_prepGen.load(std::memory_order_acquire);
            while (!g_prepQueue.empty() && g_prepQueue.front().generation != gen)
                g_prepQueue.erase(g_prepQueue.begin());
            if (g_prepQueue.empty())
                return;
            blob = std::move(g_prepQueue.front());
            g_prepQueue.erase(g_prepQueue.begin());
        }
        IDirect3DTexture9* tex = nullptr;
        if (!UploadBgraTexture(device, blob.w, blob.h, blob.bgra, &tex) || !tex)
        {
            Game::logMsg("HUD icon upload fail kind=%d %s %dx%d",
                static_cast<int>(blob.kind), blob.name, blob.w, blob.h);
            return;
        }
        switch (blob.kind)
        {
        case PrepKind::Weapon:
            if (blob.weaponKind > 0 && blob.weaponKind < KindCount)
            {
                if (g_WeaponIconTex[blob.weaponKind])
                    g_WeaponIconTex[blob.weaponKind]->Release();
                g_WeaponIconTex[blob.weaponKind] = tex;
                Game::logMsg("Weapon icon uploaded kind=%d %dx%d", blob.weaponKind, blob.w, blob.h);
            }
            else
                tex->Release();
            break;
        case PrepKind::Hud:
            StoreHudTexture(blob.name, tex, wantBlue);
            Game::logMsg("Wrist HUD icon uploaded %s %dx%d", blob.name, blob.w, blob.h);
            break;
        case PrepKind::RadialDefault:
            if (g_RadialDefaultTex)
                g_RadialDefaultTex->Release();
            g_RadialDefaultTex = tex;
            Game::logMsg("Weapon menu radial uploaded default %dx%d", blob.w, blob.h);
            break;
        case PrepKind::RadialSelected:
            if (g_RadialHighlightTex)
                g_RadialHighlightTex->Release();
            g_RadialHighlightTex = tex;
            Game::logMsg("Weapon menu radial uploaded selected %dx%d", blob.w, blob.h);
            break;
        }
    }

    void EnsureWeaponIcons(IDirect3DDevice9* device)
    {
        if (!device)
            return;
        const bool wantBlue = bmvr::UseBlueHud();
        if (g_WeaponIconDevice && g_WeaponIconDevice != device)
            ReleaseWeaponIcons();
        if (g_WeaponIconDevice && g_WeaponIconBlueShift != wantBlue)
            ReleaseWeaponIcons();
        g_WeaponIconDevice = device;
        g_WeaponIconBlueShift = wantBlue;
        KickIconPreload();
    }

    void ReleaseHudIconCache()
    {
        for (int i = 0; i < g_HudIconCount; ++i)
        {
            if (g_HudIconCache[i].tex)
                g_HudIconCache[i].tex->Release();
            g_HudIconCache[i] = HudIconEntry{};
        }
        g_HudIconCount = 0;
        g_HudIconDevice = nullptr;
        g_HudIconBlueShift = false;
    }

    void DrawKindIcon(IDirect3DDevice9* device, float x, float y, float s, WeaponKind kind, D3DCOLOR color)
    {
        switch (kind)
        {
        case KindCrowbar:
            MenuQuad(device, x - s * 1.6f, y - s * 0.18f, s * 3.2f, s * 0.36f, color);
            MenuQuad(device, x + s * 1.1f, y - s * 0.55f, s * 0.7f, s * 0.9f, color);
            break;
        case KindGlock:
            MenuQuad(device, x - s * 1.1f, y - s * 0.22f, s * 2.4f, s * 0.44f, color);
            MenuQuad(device, x - s * 0.55f, y - s * 0.22f, s * 0.4f, s * 1.15f, color);
            break;
        case KindRevolver:
            MenuQuad(device, x - s * 1.2f, y - s * 0.28f, s * 2.6f, s * 0.55f, color);
            MenuQuad(device, x + s * 0.7f, y - s * 0.55f, s * 0.7f, s * 0.7f, color);
            break;
        case KindMp5:
            MenuQuad(device, x - s * 1.6f, y - s * 0.22f, s * 3.1f, s * 0.44f, color);
            MenuQuad(device, x - s * 0.2f, y - s * 0.22f, s * 0.4f, s * 1.05f, color);
            MenuQuad(device, x - s * 1.5f, y + s * 0.15f, s * 0.9f, s * 0.28f, color);
            break;
        case KindShotgun:
            MenuQuad(device, x - s * 1.8f, y - s * 0.18f, s * 3.6f, s * 0.36f, color);
            MenuQuad(device, x - s * 0.4f, y - s * 0.18f, s * 0.45f, s * 0.95f, color);
            break;
        case KindCrossbow:
            MenuQuad(device, x - s * 1.5f, y - s * 0.12f, s * 3.0f, s * 0.24f, color);
            MenuQuad(device, x - s * 0.15f, y - s * 0.85f, s * 0.3f, s * 1.7f, color);
            break;
        case KindRpg:
            MenuQuad(device, x - s * 1.8f, y - s * 0.28f, s * 3.6f, s * 0.55f, color);
            break;
        case KindGauss:
        case KindGluon:
            MenuQuad(device, x - s * 1.3f, y - s * 0.22f, s * 2.6f, s * 0.44f, color);
            MenuQuad(device, x + s * 0.6f, y - s * 0.7f, s * 0.5f, s * 1.4f, color);
            break;
        case KindHivehand:
            MenuQuad(device, x - s * 0.9f, y - s * 0.7f, s * 1.8f, s * 1.4f, color);
            MenuQuad(device, x - s * 0.25f, y - s * 1.1f, s * 0.5f, s * 0.5f, color);
            break;
        case KindGrenade:
            MenuQuad(device, x - s * 0.55f, y - s * 0.55f, s * 1.1f, s * 1.1f, color);
            MenuQuad(device, x - s * 0.18f, y - s * 0.95f, s * 0.36f, s * 0.45f, color);
            break;
        case KindSatchel:
        case KindTripmine:
            MenuQuad(device, x - s * 0.9f, y - s * 0.45f, s * 1.8f, s * 0.9f, color);
            break;
        case KindSnark:
            MenuQuad(device, x - s * 0.7f, y - s * 0.45f, s * 1.4f, s * 0.9f, color);
            MenuQuad(device, x - s * 0.2f, y - s * 0.85f, s * 0.4f, s * 0.45f, color);
            break;
        case KindHeadcrab:
            MenuQuad(device, x - s * 0.8f, y - s * 0.45f, s * 1.6f, s * 0.9f, color);
            MenuQuad(device, x - s * 0.35f, y - s * 0.85f, s * 0.7f, s * 0.45f, color);
            break;
        default:
            MenuQuad(device, x - s * 0.7f, y - s * 0.7f, s * 1.4f, s * 1.4f, color);
            break;
        }
    }
}

IDirect3DTexture9* bmvr::AcquireHudIcon(IDirect3DDevice9* device, const char* vtfName)
{
    if (!device || !vtfName || !*vtfName)
        return nullptr;
    g_HudIconDevice = device;
    for (int i = 0; i < g_HudIconCount; ++i)
    {
        if (_stricmp(g_HudIconCache[i].name, vtfName) == 0)
            return g_HudIconCache[i].tex;
    }
    return nullptr;
}

void bmvr::KickHudIconPreload()
{
    KickIconPreload();
}

void bmvr::PumpHudIconUploads(IDirect3DDevice9* device)
{
    DrainOneHudIconUpload(device);
}

const char* bmvr::PrimaryAmmoIconVtf(const char* model, const char* net)
{
    switch (KindFromNames(model, net))
    {
    case KindRevolver: return "ammo_357.vtf";
    case KindShotgun: return "ammo_buckshot.vtf";
    case KindCrossbow: return "ammo_bolt.vtf";
    case KindGauss:
    case KindGluon: return "ammo_energy.vtf";
    case KindRpg: return "ammo_grenade_rpg.vtf";
    // Black Mesa ships no hornet .vtf, only the .vmt. The lookup fails once,
    // logs, and the counter then draws without an icon.
    case KindHivehand: return "ammo_grenade_hornet.vtf";
    case KindGrenade: return "ammo_grenade_frag.vtf";
    case KindSatchel: return "ammo_grenade_satchel.vtf";
    case KindTripmine: return "ammo_grenade_tripmine.vtf";
    case KindSnark: return "ammo_snark.vtf";
    default: return "ammo_9mm.vtf";
    }
}

const char* bmvr::SecondaryAmmoIconVtf(const char* model, const char* net)
{
    switch (KindFromNames(model, net))
    {
    case KindMp5: return "ammo_grenade_mp5.vtf";
    case KindShotgun: return "ammo_buckshot.vtf";
    default: return "ammo_grenade_frag.vtf";
    }
}

bool VR::WeaponMenuStickHeld() const
{
    if (PressedDigitalAction(m_ActionWeaponMenu))
        return true;
    if (PressedDigitalAction(m_ActionInventoryQuickSwitch))
        return true;
    return PressedDigitalAction(m_ActionResetPosition);
}

void VR::UpdateWeaponMenu(bool stickClickHeld, float deltaMs)
{
    (void)deltaMs;
    const DWORD now = GetTickCount();
    if (!m_GameplayEligible || !m_Game || PauseUiActive() || m_SettingsOpen)
    {
        m_WeaponMenuOpen = false;
        m_WeaponMenuClickHeld = false;
        m_WeaponMenuOpenedThisHold = false;
        m_WeaponMenuLatched = false;
        m_WeaponMenuHover = -1;
        return;
    }

    if (stickClickHeld && !m_WeaponMenuClickHeld)
    {
        m_WeaponMenuClickStartMs = now;
        m_WeaponMenuOpenedThisHold = false;
    }

    if (stickClickHeld)
    {
        m_WeaponMenuClickHeld = true;
        if (!m_WeaponMenuOpen && (now - m_WeaponMenuClickStartMs) >= kMenuHoldMs)
        {
            m_WeaponMenuOpen = true;
            m_WeaponMenuOpenedThisHold = true;
            m_WeaponMenuLatched = false;
            m_WeaponMenuHover = -1;
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 900, 0.35f);
            Game::logMsg("Weapon menu opened (right stick hold)");
        }
    }

    if (m_WeaponMenuOpen)
    {
        if (m_HmdPoseValid)
        {
            if (!m_WeaponMenuLatched)
            {
                Vector viewFwd, viewRight, viewUp;
                GetViewBasis(&viewFwd, &viewRight, &viewUp);
                Vector yawFwd = viewFwd;
                yawFwd.z = 0.f;
                if (VectorNormalize(yawFwd) <= 0.01f)
                    yawFwd = Vector(1.f, 0.f, 0.f);
                const Vector worldUp(0.f, 0.f, 1.f);
                Vector yawRight = CrossProduct(worldUp, yawFwd);
                if (yawRight.LengthSqr() < 1e-4f)
                    yawRight = viewRight;
                VectorNormalize(yawRight);

                const Vector body = MenuPlayerBody(this);
                Vector hand{};
                {
                    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
                    hand = ControllerTrackingToWorld(body, m_PhysicalRightPosAbs);
                }

                // HL2VR OpenWeaponSelection: screen centre at
                // PrimaryHandOrigin() + yaw-forward * 4, vertical plane.
                m_WeaponMenuLatchBody = body;
                m_WeaponMenuLatchWorld = hand + yawFwd * kMenuHandForwardHu;
                m_WeaponMenuLatchDelta = m_WeaponMenuLatchWorld - body;
                m_WeaponMenuLatchFwd = yawFwd;
                m_WeaponMenuLatchRight = yawRight;
                m_WeaponMenuLatchUp = worldUp;
                m_WeaponMenuLatchYaw = m_RotationOffsetY.load(std::memory_order_acquire);
                m_WeaponMenuLatched = true;
            }
        }

        Game::InventoryWeapon inv[kMaxMenuSlots]{};
        const int n = m_Game->CollectInventoryWeapons(inv, kMaxMenuSlots);
        C_BaseEntity* active = m_Game->GetActiveWeaponEntity();

        bool prevStillHeld = false;
        for (int i = 0; i < n; ++i)
        {
            if (inv[i].entityIndex == m_WeaponMenuPrevEntity)
            {
                prevStillHeld = true;
                break;
            }
        }
        if (!prevStillHeld)
        {
            m_WeaponMenuPrevEntity = 0;
            m_WeaponMenuPrevKind = 0;
        }
        if (!m_EmptyHands && active)
        {
            for (int i = 0; i < n; ++i)
            {
                C_BaseEntity* ent = m_Game->GetClientEntity(inv[i].entityIndex);
                if (ent != active)
                    continue;
                m_WeaponMenuPrevEntity = inv[i].entityIndex;
                m_WeaponMenuPrevKind = KindFromNames(inv[i].modelName, inv[i].networkName);
                break;
            }
        }

        for (int i = 0; i < kMaxMenuSlots; ++i)
            m_WeaponMenuSlots[i] = {};

        // Holster at origin, then only hexes for weapons you own.
        m_WeaponMenuSlots[0].planeX = 0.f;
        m_WeaponMenuSlots[0].planeY = 0.f;
        m_WeaponMenuSlots[0].hudSlot = 0;
        m_WeaponMenuSlots[0].hudPos = 0;
        m_WeaponMenuSlots[0].emptyHand = true;
        if (m_EmptyHands && m_WeaponMenuPrevEntity > 0)
        {
            m_WeaponMenuSlots[0].entityIndex = m_WeaponMenuPrevEntity;
            m_WeaponMenuSlots[0].kind = m_WeaponMenuPrevKind;
            m_WeaponMenuSlots[0].equipped = false;
            strncpy_s(m_WeaponMenuSlots[0].label, LabelForKind(
                static_cast<WeaponKind>(m_WeaponMenuPrevKind)), _TRUNCATE);
        }
        else
        {
            m_WeaponMenuSlots[0].equipped = m_EmptyHands;
            strncpy_s(m_WeaponMenuSlots[0].label, "HAND", _TRUNCATE);
        }

        bool used[11][11]{};
        auto take = [&](int col, int row) -> bool {
            if (col < -5 || col > 5 || row < -5 || row > 5)
                return false;
            bool& cell = used[col + 5][row + 5];
            if (cell)
                return false;
            cell = true;
            return true;
        };
        take(0, 0);

        int placed = 1;
        auto placeAt = [&](int col, int row, const Game::InventoryWeapon& wpn, WeaponKind kind) -> bool {
            if (placed >= kMaxMenuSlots)
                return false;
            if (!take(col, row))
                return false;
            float ox = 0.f, oy = 0.f;
            OffsetToPlane(col, row, ox, oy);
            C_BaseEntity* ent = m_Game->GetClientEntity(wpn.entityIndex);
            m_WeaponMenuSlots[placed].entityIndex = wpn.entityIndex;
            m_WeaponMenuSlots[placed].kind = kind;
            m_WeaponMenuSlots[placed].hudSlot = col;
            m_WeaponMenuSlots[placed].hudPos = row;
            m_WeaponMenuSlots[placed].planeX = ox;
            m_WeaponMenuSlots[placed].planeY = oy;
            m_WeaponMenuSlots[placed].equipped = !m_EmptyHands && (ent == active);
            m_WeaponMenuSlots[placed].throwable = KindIsThrowable(kind);
            m_WeaponMenuSlots[placed].dry = (kind != KindCrowbar)
                && m_Game->WeaponHasNoAmmo(ent);
            strncpy_s(m_WeaponMenuSlots[placed].label, LabelForKind(kind), _TRUNCATE);
            ++placed;
            return true;
        };

        bool packed[64]{};
        const int invN = (n < 64) ? n : 64;
        for (int i = 0; i < invN; ++i)
        {
            const WeaponKind kind = KindFromNames(inv[i].modelName, inv[i].networkName);
            int col = 0, row = 0;
            if (!OffsetForKind(kind, col, row))
                continue;
            if (placeAt(col, row, inv[i], kind))
                packed[i] = true;
        }
        auto packInto = [&](const int cells[][2], int cellCount) {
            int cell = 0;
            for (int i = 0; i < invN; ++i)
            {
                if (packed[i])
                    continue;
                const WeaponKind kind = KindFromNames(inv[i].modelName, inv[i].networkName);
                while (cell < cellCount && !placeAt(cells[cell][0], cells[cell][1], inv[i], kind))
                    ++cell;
                if (cell >= cellCount)
                    break;
                packed[i] = true;
                ++cell;
            }
        };
        packInto(kWeaponHex, static_cast<int>(sizeof(kWeaponHex) / sizeof(kWeaponHex[0])));
        // VR settings: one cell below the crowbar, outside the weapon brick.
        if (placed < kMaxMenuSlots && take(0, -2))
        {
            WeaponMenuSlot& gear = m_WeaponMenuSlots[placed];
            OffsetToPlane(0, -2, gear.planeX, gear.planeY);
            gear.hudSlot = 0;
            gear.hudPos = -2;
            gear.settingsGear = true;
            strncpy_s(gear.label, "VR", _TRUNCATE);
            ++placed;
        }
        m_WeaponMenuCount = placed;
        ApplyWeaponMenuWorldPose();

        // HL2VR HandleWeaponSelection: project the hand along menu-forward,
        // not the controller aim ray.
        Vector hand{};
        {
            std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
            const Vector trackingBody = MenuPlayerBody(this);
            hand = ControllerTrackingToWorld(trackingBody, m_PhysicalRightPosAbs);
        }
        const Vector rayStart = hand - m_WeaponMenuFwd * 20.f;
        const Vector rayDir = m_WeaponMenuFwd;
        const float planeDenom = rayDir.Dot(m_WeaponMenuFwd);
        if (fabsf(planeDenom) > 0.0001f)
        {
            const float hitT = (m_WeaponMenuOrigin - rayStart).Dot(m_WeaponMenuFwd) / planeDenom;
            if (hitT > 0.f && hitT < 120.f)
            {
                const Vector hit = rayStart + rayDir * hitT;
                const Vector rel = hit - m_WeaponMenuOrigin;
                m_WeaponMenuHandX = rel.Dot(m_WeaponMenuRight);
                m_WeaponMenuHandY = rel.Dot(m_WeaponMenuUp);
            }
        }

        const int prevHover = m_WeaponMenuHover;
        int hover = prevHover;
        const float hx = m_WeaponMenuHandX;
        const float hy = m_WeaponMenuHandY;
        const float r2 = kRadiusHu * kRadiusHu;
        float best = r2;
        bool hitCircle = false;
        for (int i = 0; i < m_WeaponMenuCount; ++i)
        {
            const float dx = hx - m_WeaponMenuSlots[i].planeX;
            const float dy = hy - m_WeaponMenuSlots[i].planeY;
            const float d2 = dx * dx + dy * dy;
            if (d2 > best)
                continue;
            best = d2;
            hover = i;
            hitCircle = true;
        }
        if (!hitCircle && (hover < 0 || hover >= m_WeaponMenuCount))
            hover = 0;
        m_WeaponMenuHover = hover;

        if (hitCircle && hover != prevHover && hover >= 0 && hover < m_WeaponMenuCount)
        {
            const WeaponMenuSlot& slot = m_WeaponMenuSlots[hover];
            const bool skipDryNade = slot.throwable && slot.dry && slot.entityIndex > 0
                && !slot.emptyHand;
            if (!skipDryNade)
            {
                if (slot.emptyHand && slot.entityIndex <= 0)
                {
                    m_EmptyHands = true;
                    Game::logMsg("Weapon menu holster");
                }
                else if (slot.entityIndex > 0)
                {
                    m_EmptyHands = false;
                    m_PendingWeaponSelect.store(slot.entityIndex, std::memory_order_release);
                    m_WeaponMenuPrevEntity = slot.entityIndex;
                    m_WeaponMenuPrevKind = slot.kind;
                    Game::logMsg("Weapon menu hover-select entity=%d %s",
                        slot.entityIndex, slot.label);
                }
                QueueWeaponMenuSound(kWeaponSoundHover);
            }
        }
    }

    if (!stickClickHeld && m_WeaponMenuClickHeld)
    {
        const bool openSettings = m_WeaponMenuOpen && m_WeaponMenuHover >= 0
            && m_WeaponMenuHover < m_WeaponMenuCount && m_WeaponMenuSlots[m_WeaponMenuHover].settingsGear;
        if (openSettings)
            OpenSettingsPanel();
        else if (m_WeaponMenuOpenedThisHold)
            QueueWeaponMenuSound(kWeaponSoundSelect, 0, 0);
        else if ((now - m_WeaponMenuClickStartMs) < kMenuHoldMs)
        {
            m_HmdOriginLatched = false;
            if (bmvr::g_RecenterResetsYaw)
                m_RotationOffsetY.store(0.f, std::memory_order_release);
            Game::logMsg("Right-stick tap: recenter");
        }
        m_WeaponMenuOpen = false;
        m_WeaponMenuClickHeld = false;
        m_WeaponMenuOpenedThisHold = false;
        m_WeaponMenuLatched = false;
        m_WeaponMenuHover = -1;
    }
}

void VR::ApplyWeaponMenuWorldPose()
{
    if (!m_WeaponMenuLatched)
        return;
    const Vector body = MenuPlayerBody(this);
    const float yawDelta = m_RotationOffsetY.load(std::memory_order_acquire)
        - m_WeaponMenuLatchYaw;
    Vector delta = m_WeaponMenuLatchDelta;
    YawAroundZ(delta, yawDelta);
    m_WeaponMenuOrigin = body + delta;
    m_WeaponMenuFwd = m_WeaponMenuLatchFwd;
    m_WeaponMenuRight = m_WeaponMenuLatchRight;
    m_WeaponMenuUp = m_WeaponMenuLatchUp;
    YawAroundZ(m_WeaponMenuFwd, yawDelta);
    YawAroundZ(m_WeaponMenuRight, yawDelta);
    YawAroundZ(m_WeaponMenuUp, yawDelta);
    for (int i = 0; i < m_WeaponMenuCount; ++i)
    {
        m_WeaponMenuSlots[i].center = m_WeaponMenuOrigin
            + m_WeaponMenuRight * m_WeaponMenuSlots[i].planeX
            + m_WeaponMenuUp * m_WeaponMenuSlots[i].planeY;
    }
}

void VR::DrawWeaponMenu(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    if (!m_WeaponMenuOpen || !device || m_WeaponMenuCount <= 0)
        return;
    ApplyWeaponMenuWorldPose();
    EnsureWeaponIcons(device);
    EnsureRadialTextures(device);
    const float pixelAspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float aspect = m_OffCenterActive ? m_OffCenterAspect
        : (bmvr::UseGbMatchViewLock() && m_Aspect > 0.1f) ? m_Aspect : pixelAspect;
    const float projFov = m_OffCenterActive ? WorldRenderFov() : HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * kPi / 180.f);
    if (!(tanHalf > 0.01f))
        return;
    float ocSx = 1.f, ocOx = 0.f, ocSy = 1.f, ocOy = 0.f;
    OffCenterNdc(stereoEye, ocSx, ocOx, ocSy, ocOy);

    auto project = [&](const Vector& world, float& sx, float& sy, float& rhw) -> bool {
        const Vector delta = world - eyeOrig;
        const float z = delta.Dot(fwd);
        if (z < 0.5f)
            return false;
        rhw = 1.f / z;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x * rhw) / tanHalf * ocSx + ocOx;
        const float ndcY = ((y * rhw) * aspect) / tanHalf * ocSy + ocOy;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return true;
    };

    auto projectCircle = [&](const Vector& center, float rHu, float* xs, float* ys, float* rhws) -> bool {
        for (int k = 0; k <= kCircleSegs; ++k)
        {
            const float ang = static_cast<float>(k) * (360.f / kCircleSegs);
            if (!project(CirclePlanePoint(center, m_WeaponMenuRight, m_WeaponMenuUp, rHu, ang),
                xs[k], ys[k], rhws[k]))
                return false;
        }
        return true;
    };

    auto drawTexturedQuad = [&](IDirect3DTexture9* tex, const Vector& center,
        float halfW, float halfH, D3DCOLOR tint) {
        if (!tex)
            return;
        const Vector tl = center - m_WeaponMenuRight * halfW + m_WeaponMenuUp * halfH;
        const Vector tr = center + m_WeaponMenuRight * halfW + m_WeaponMenuUp * halfH;
        const Vector bl = center - m_WeaponMenuRight * halfW - m_WeaponMenuUp * halfH;
        const Vector br = center + m_WeaponMenuRight * halfW - m_WeaponMenuUp * halfH;
        float x0 = 0.f, y0 = 0.f, r0 = 1.f;
        float x1 = 0.f, y1 = 0.f, r1 = 1.f;
        float x2 = 0.f, y2 = 0.f, r2 = 1.f;
        float x3 = 0.f, y3 = 0.f, r3 = 1.f;
        if (!project(tl, x0, y0, r0) || !project(tr, x1, y1, r1)
            || !project(bl, x2, y2, r2) || !project(br, x3, y3, r3))
            return;
        device->SetTexture(0, tex);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        MenuVertTex tv[4] = {
            { x0, y0, 0.f, r0, tint, 0.f, 0.f },
            { x1, y1, 0.f, r1, tint, 1.f, 0.f },
            { x2, y2, 0.f, r2, tint, 0.f, 1.f },
            { x3, y3, 0.f, r3, tint, 1.f, 1.f }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tv, sizeof(MenuVertTex));
        device->SetTexture(0, nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    };

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    const WheelPalette pal = MakeWheelPalette();
    const float radiusHu = kRadiusHu;

    // HL2VR DrawCircle is VGUI DrawTexturedRect (alpha blend), not a world
    // UnlitGeneric pass. $additive on the VMT is unused by that path.
    for (int i = 0; i < m_WeaponMenuCount; ++i)
    {
        const WeaponMenuSlot& slot = m_WeaponMenuSlots[i];
        const bool hover = (i == m_WeaponMenuHover);
        IDirect3DTexture9* radial = hover ? g_RadialHighlightTex : g_RadialDefaultTex;
        if (!radial)
            radial = g_RadialDefaultTex;
        if (!radial)
            continue;
        const D3DCOLOR hexTint = hover ? pal.frameHover : pal.frameEquipped;
        drawTexturedQuad(radial, slot.center, kHexDrawHu, kHexDrawHu, hexTint);
    }
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    for (int i = 0; i < m_WeaponMenuCount; ++i)
    {
        IDirect3DTexture9* outline = g_RadialDefaultTex;
        if (!outline)
            continue;
        drawTexturedQuad(outline, m_WeaponMenuSlots[i].center,
            kHexDrawHu, kHexDrawHu, (pal.frameEquipped & 0x00FFFFFF) | 0x64000000);
    }
    if (m_WeaponMenuHover >= 0 && m_WeaponMenuHover < m_WeaponMenuCount)
    {
        IDirect3DTexture9* glow = g_RadialHighlightTex ? g_RadialHighlightTex : g_RadialDefaultTex;
        if (glow)
        {
            drawTexturedQuad(glow, m_WeaponMenuSlots[m_WeaponMenuHover].center,
                kHexDrawHu, kHexDrawHu, pal.glow);
        }
    }
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    for (int i = 0; i < m_WeaponMenuCount; ++i)
    {
        const WeaponMenuSlot& slot = m_WeaponMenuSlots[i];
        const bool hover = (i == m_WeaponMenuHover);
        const bool dry = slot.dry && !slot.emptyHand;
        if (slot.settingsGear)
        {
            if (IDirect3DTexture9* gear = SettingsGearTexture(device))
                drawTexturedQuad(gear, slot.center, radiusHu * 0.62f, radiusHu * 0.31f,
                    hover ? D3DCOLOR_XRGB(255, 255, 255) : pal.tintIdle);
            continue;
        }
        const bool showIcon = slot.entityIndex > 0 || (!slot.emptyHand && slot.kind > 0);
        if (!showIcon)
            continue;

        const int kind = slot.kind;
        IDirect3DTexture9* icon = (kind > 0 && kind < KindCount) ? g_WeaponIconTex[kind] : nullptr;
        const D3DCOLOR tint = dry
            ? D3DCOLOR_ARGB(180, 255, 0, 0)
            : (hover ? D3DCOLOR_XRGB(255, 255, 255) : pal.tintIdle);
        if (icon)
        {
            float halfW = radiusHu * 0.62f;
            float halfH = radiusHu * 0.38f;
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(icon->GetLevelDesc(0, &desc)) && desc.Width > 0 && desc.Height > 0)
            {
                const float srcAspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
                halfW = radiusHu * 0.58f;
                halfH = halfW / srcAspect;
                if (halfH > radiusHu * 0.42f)
                {
                    halfH = radiusHu * 0.42f;
                    halfW = halfH * srcAspect;
                }
            }
            drawTexturedQuad(icon, slot.center, halfW, halfH, tint);
        }
        else
        {
            float gsx = 0.f, gsy = 0.f, rhw0 = 1.f, rx = 0.f, ry = 0.f, rhw1 = 1.f;
            if (project(slot.center, gsx, gsy, rhw0)
                && project(slot.center + m_WeaponMenuRight * radiusHu, rx, ry, rhw1))
            {
                const float glyph = sqrtf((rx - gsx) * (rx - gsx) + (ry - gsy) * (ry - gsy)) * 0.28f;
                DrawKindIcon(device, gsx, gsy, glyph,
                    static_cast<WeaponKind>(slot.kind), tint);
            }
        }
    }

    {
        // HL2VR: DrawSetColor(255,255,255,255); DrawOutlinedCircle(x, y, 10, 16).
        // One VGUI pixel on the 1024px / 20 HU panel.
        constexpr int kCursorSegs = 16;
        constexpr float kCursorStrokeHu = kSelectSizeHu / static_cast<float>(kPanelPx);
        const Vector cursor = m_WeaponMenuOrigin
            + m_WeaponMenuRight * m_WeaponMenuHandX
            + m_WeaponMenuUp * m_WeaponMenuHandY;
        const float rOut = kCursorRadiusHu + 0.5f * kCursorStrokeHu;
        const float rIn = kCursorRadiusHu - 0.5f * kCursorStrokeHu;
        MenuVert ring[(kCursorSegs + 1) * 2]{};
        bool ok = true;
        for (int k = 0; k <= kCursorSegs; ++k)
        {
            const float ang = static_cast<float>(k) * (360.f / kCursorSegs);
            float xo = 0.f, yo = 0.f, ro = 1.f;
            float xi = 0.f, yi = 0.f, ri = 1.f;
            if (!project(CirclePlanePoint(cursor, m_WeaponMenuRight, m_WeaponMenuUp, rOut, ang),
                    xo, yo, ro)
                || !project(CirclePlanePoint(cursor, m_WeaponMenuRight, m_WeaponMenuUp, rIn, ang),
                    xi, yi, ri))
            {
                ok = false;
                break;
            }
            ring[k * 2] = { xo, yo, 0.f, ro, D3DCOLOR_XRGB(255, 255, 255) };
            ring[k * 2 + 1] = { xi, yi, 0.f, ri, D3DCOLOR_XRGB(255, 255, 255) };
        }
        if (ok)
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, kCursorSegs * 2, ring, sizeof(MenuVert));
    }

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

bool VR::UpdateWeaponFireHaptics()
{
    if (!m_Game || !m_GameplayEligible || PauseUiActive())
        return false;
    C_BaseEntity* vm = m_Game->GetViewModelEntity();
    C_BaseEntity* weapon = m_Game->GetActiveWeaponEntity();
    if (weapon != m_LastFireWeapon)
    {
        m_LastFireWeapon = weapon;
        m_LastMuzzleFlashParity = -1;
        m_LastFireClip = -1;
    }

    bool fired = false;
    if (vm)
    {
        const int parity = ReadMuzzleFlashParity(vm);
        if (parity >= 0)
        {
            if (m_LastMuzzleFlashParity >= 0 && parity != m_LastMuzzleFlashParity)
                fired = true;
            m_LastMuzzleFlashParity = parity;
        }
    }

    if (weapon)
    {
        const int clip = m_Game->ReadWeaponClip(weapon);
        if (clip >= 0)
        {
            if (m_LastFireClip >= 0 && clip < m_LastFireClip)
                fired = true;
            m_LastFireClip = clip;
        }
    }

    if (!fired)
        return false;

    bool melee = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        melee = m_LastViewmodelModel.find("crowbar") != std::string::npos
            || m_LastViewmodelModel.find("wrench") != std::string::npos;
    }
    if (melee)
        return false;
    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 2500, 0.85f);
    return true;
}

void VR::AfterCreateMoveFireHaptics()
{
    const uint32_t held = HeldButtons();
    const bool attackEdge = (held & IN_ATTACK) && !(m_PrevHeldButtons & IN_ATTACK);
    m_PrevHeldButtons = held;
    // Do not consume clip/parity here — ProcessInput on the Present thread
    // is the path that used to rumble each shot. Only latch energy-weapon
    // trigger edges that never decrement clip.
    if (!attackEdge || EmptyHands())
        return;
    bool melee = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        melee = m_LastViewmodelModel.find("crowbar") != std::string::npos
            || m_LastViewmodelModel.find("wrench") != std::string::npos;
    }
    if (!melee)
        m_PendingFireHaptic.store(1, std::memory_order_release);
}

void VR::PulseHandHaptic(vr::ETrackedControllerRole hand, unsigned short durationUs, float amplitude)
{
    if (!bmvr::g_Haptics)
        return;
    if (IsMenuUp())
        return;
    const uint32_t openXrHand = (hand == vr::TrackedControllerRole_LeftHand)
        ? L4D2VR_OPENXR_HAND_LEFT : L4D2VR_OPENXR_HAND_RIGHT;
    if (m_OpenXrHelperBridgeActive)
    {
        // OpenVR TriggerHapticPulse units are microseconds of a click (1-3999).
        // OpenXR duration is real vibration time; 2ms is below SteamVR's floor.
        float seconds = durationUs / 1000000.0f;
        if (durationUs <= 3999)
            seconds = std::clamp(durationUs / 3999.0f * 0.07f, 0.045f, 0.09f);
        L4D2VR_PublishOpenXrHapticRequest(openXrHand, seconds, 160.0f, amplitude);
        static int s_hapticLog;
        if (s_hapticLog < 8)
        {
            Game::logMsg("OpenXR haptic hand=%u dur=%.3fs amp=%.2f", openXrHand, seconds, amplitude);
            ++s_hapticLog;
        }
        return;
    }
    if (!m_System)
        return;
    if (durationUs < 1)
        durationUs = 1;
    if (durationUs > 3999)
        durationUs = 3999;
    const vr::TrackedDeviceIndex_t idx = m_System->GetTrackedDeviceIndexForControllerRole(hand);
    if (idx == vr::k_unTrackedDeviceIndexInvalid)
        return;
    m_System->TriggerHapticPulse(idx, 0, durationUs);
}

const char* VR::WeaponMenuDrawSoundName(int kind) const
{
    return DrawSoundForKind(static_cast<WeaponKind>(kind));
}
