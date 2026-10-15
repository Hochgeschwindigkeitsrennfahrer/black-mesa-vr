// In-VR settings panel (docs/plans/bmvr-vr-settings-panel.md in the notes repo).
//
// Opened from the weapon menu's gear slot. Drawn by the mod in the per-eye hand
// overlay pass like the weapon menu, so it needs nothing from Black Mesa's UI and
// behaves the same on every game build. Text is rendered with GDI into a 32-bit
// DIB and uploaded to a D3D9 texture only when something changes. Values apply
// live; changed ones are written to VR/config.txt when the panel closes.
//
// Threads: UpdateSettingsPanel (ProcessInput) and DrawSettingsPanel (hand
// overlay) both run on the render thread; the game thread only reads the flags.

#include "vr.h"
#include "game.h"
#include "bmvr_flags.h"
#include "d3d9_vr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr float kPi = 3.14159265f;
    constexpr int kTexW = 1024;
    constexpr int kHeaderPx = 96;
    constexpr int kRowPx = 64;
    constexpr int kSectionPx = 48;
    constexpr float kPanelWidthHu = 20.f;      // ~51 cm
    constexpr float kPanelDistanceHu = 24.f;   // ~61 cm in front of the eyes
    constexpr float kPanelDropHu = 3.f;        // a little below eye height
    // Zones in texture pixels.
    constexpr int kMinusX0 = 600, kMinusX1 = 690;
    constexpr int kValueX0 = 690, kValueX1 = 880;
    constexpr int kPlusX0 = 880, kPlusX1 = 970;
    constexpr int kPillX0 = 790, kPillX1 = 970;

    enum class RowKind { Section, Toggle, Number, Close };

    struct Row
    {
        const char* label;
        RowKind kind;
        const char* key;      // VR/config.txt key
        bool* b;
        float* f;
        float step, lo, hi;
        const char* fmt;      // number display
    };

    std::vector<Row>& Rows()
    {
        static std::vector<Row> rows = {
            { "AIMING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Aim marker", RowKind::Toggle, "VrCrosshair", &bmvr::g_VrCrosshair, nullptr, 0, 0, 0, nullptr },
            { "Marker as red dot", RowKind::Toggle, "VrCrosshairDot", &bmvr::g_VrCrosshairDot, nullptr, 0, 0, 0, nullptr },
            { "Marker size", RowKind::Number, "VrCrosshairScale", nullptr, &bmvr::g_VrCrosshairScale, 0.1f, 0.2f, 3.f, "%.1f" },
            { "Laser beam", RowKind::Toggle, "VrLaserBeam", &bmvr::g_VrLaserBeam, nullptr, 0, 0, 0, nullptr },
            { "Laser width", RowKind::Number, "VrLaserWidth", nullptr, &bmvr::g_VrLaserWidth, 0.01f, 0.01f, 0.3f, "%.2f" },
            { "Gun angle", RowKind::Number, "GunPitchOffset", nullptr, &bmvr::g_GunPitchOffset, 1.f, -45.f, 45.f, "%+.0f\xB0" },
            { "TURNING", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Snap turning", RowKind::Toggle, "SnapTurning", &bmvr::g_SnapTurning, nullptr, 0, 0, 0, nullptr },
            { "Snap angle", RowKind::Number, "SnapTurnAngle", nullptr, &bmvr::g_SnapTurnAngle, 15.f, 15.f, 90.f, "%.0f\xB0" },
            { "Smooth turn speed", RowKind::Number, "TurnSpeed", nullptr, &bmvr::g_TurnSpeed, 0.05f, 0.05f, 1.f, "%.2f" },
            { "Recenter resets turning", RowKind::Toggle, "RecenterResetsYaw", &bmvr::g_RecenterResetsYaw, nullptr, 0, 0, 0, nullptr },
            { "MOVEMENT", RowKind::Section, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
            { "Crouch only while held", RowKind::Toggle, "CrouchStickHold", &bmvr::g_CrouchStickHold, nullptr, 0, 0, 0, nullptr },
            { "Sprint click toggles", RowKind::Toggle, "SprintToggle", &bmvr::g_SprintToggle, nullptr, 0, 0, 0, nullptr },
            { "Close", RowKind::Close, nullptr, nullptr, nullptr, 0, 0, 0, nullptr },
        };
        return rows;
    }

    int RowHeight(const Row& r) { return r.kind == RowKind::Section ? kSectionPx : kRowPx; }
    int RowTop(int index)
    {
        int y = kHeaderPx;
        for (int i = 0; i < index; ++i)
            y += RowHeight(Rows()[i]);
        return y;
    }
    // Row under texture y, or -1 (headings are not interactive).
    int RowAt(int py)
    {
        const auto& rows = Rows();
        int y = kHeaderPx;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            const int h = RowHeight(rows[i]);
            if (py >= y && py < y + h)
                return rows[i].kind == RowKind::Section ? -1 : i;
            y += h;
        }
        return -1;
    }
    int TexH() { return RowTop(static_cast<int>(Rows().size())) + 16; }
    float PanelHeightHu() { return kPanelWidthHu * static_cast<float>(TexH()) / kTexW; }

    std::string FormatValue(const Row& r)
    {
        char buf[64]{};
        if (r.kind == RowKind::Toggle)
            return (r.b && *r.b) ? "true" : "false";
        _snprintf(buf, sizeof(buf) - 1, "%g", r.f ? *r.f : 0.f);
        return buf;
    }

    // Replace `Key=...` (first non-comment line) or append it; keeps the file's
    // line endings and every other line as it was.
    bool WriteConfigValues(const std::vector<std::pair<std::string, std::string>>& kv)
    {
        const std::string path = bmvr::VrFilePath("config.txt");
        std::string text;
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                text.append(buf, n);
            std::fclose(f);
        }
        const std::string eol = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
        for (const auto& [key, value] : kv)
        {
            const std::string want = key + "=";
            size_t pos = 0;
            bool done = false;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos)
                    end = text.size();
                if (text.compare(pos, want.size(), want) == 0)
                {
                    size_t lineEnd = end;
                    if (lineEnd > pos && text[lineEnd - 1] == '\r')
                        --lineEnd;
                    text.replace(pos, lineEnd - pos, want + value);
                    done = true;
                    break;
                }
                pos = end + 1;
            }
            if (!done)
            {
                if (!text.empty() && text.back() != '\n')
                    text += eol;
                text += want + value + eol;
            }
        }
        const std::string tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            std::remove(tmp.c_str());
            return false;
        }
        return true;
    }

    struct PanelGfx
    {
        IDirect3DDevice9* device = nullptr;
        IDirect3DTexture9* sys = nullptr;
        IDirect3DTexture9* gpu = nullptr;
        IDirect3DTexture9* gearGpu = nullptr;
        IDirect3DTexture9* toastGpu = nullptr;
        char toastText[32]{};
        int w = 0, h = 0;
    };
    PanelGfx g_Gfx;

    void ReleaseGfx()
    {
        if (g_Gfx.sys) g_Gfx.sys->Release();
        if (g_Gfx.gpu) g_Gfx.gpu->Release();
        if (g_Gfx.gearGpu) g_Gfx.gearGpu->Release();
        if (g_Gfx.toastGpu) g_Gfx.toastGpu->Release();
        g_Gfx = PanelGfx{};
    }

    // GDI canvas: top-down 32-bit DIB.
    struct Canvas
    {
        HDC dc = nullptr;
        HBITMAP bmp = nullptr;
        HGDIOBJ old = nullptr;
        uint32_t* px = nullptr;
        int w = 0, h = 0;
        Canvas(int width, int height) : w(width), h(height)
        {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            dc = CreateCompatibleDC(nullptr);
            void* bits = nullptr;
            bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            px = static_cast<uint32_t*>(bits);
            if (dc && bmp)
                old = SelectObject(dc, bmp);
            SetBkMode(dc, TRANSPARENT);
        }
        ~Canvas()
        {
            if (dc && old) SelectObject(dc, old);
            if (bmp) DeleteObject(bmp);
            if (dc) DeleteDC(dc);
        }
        bool ok() const { return dc && bmp && px; }
        void fill(int x0, int y0, int x1, int y1, COLORREF c)
        {
            RECT r{ x0, y0, x1, y1 };
            HBRUSH b = CreateSolidBrush(c);
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        void text(const char* s, int x0, int y0, int x1, int y1, COLORREF c, int height, bool bold, UINT align)
        {
            HFONT f = CreateFontA(-height, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_SWISS, "Arial");
            HGDIOBJ of = SelectObject(dc, f);
            SetTextColor(dc, c);
            RECT r{ x0, y0, x1, y1 };
            DrawTextA(dc, s, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
            DeleteObject(f);
        }
        // GDI leaves alpha undefined; the whole panel gets one opacity.
        void setAlpha(uint8_t a)
        {
            GdiFlush();
            for (int i = 0; i < w * h; ++i)
                px[i] = (px[i] & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
        }
    };

    bool Upload(IDirect3DDevice9* device, const Canvas& c, IDirect3DTexture9*& sys, IDirect3DTexture9*& gpu)
    {
        if (!sys && FAILED(device->CreateTexture(c.w, c.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            return false;
        if (!gpu && FAILED(device->CreateTexture(c.w, c.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpu, nullptr)))
            return false;
        D3DLOCKED_RECT lr{};
        if (FAILED(sys->LockRect(0, &lr, nullptr, 0)))
            return false;
        for (int y = 0; y < c.h; ++y)
            std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                c.px + static_cast<size_t>(y) * c.w, static_cast<size_t>(c.w) * 4u);
        sys->UnlockRect(0);
        return SUCCEEDED(device->UpdateTexture(sys, gpu));
    }

    constexpr COLORREF kBg = RGB(18, 22, 27);
    constexpr COLORREF kRowHover = RGB(46, 54, 64);
    constexpr COLORREF kAmber = RGB(255, 176, 0);
    constexpr COLORREF kText = RGB(225, 230, 235);
    constexpr COLORREF kDim = RGB(120, 128, 136);
    constexpr COLORREF kBtn = RGB(60, 68, 78);
    constexpr COLORREF kBtnHover = RGB(255, 176, 0);
    constexpr COLORREF kOn = RGB(60, 150, 80);
    constexpr COLORREF kOff = RGB(70, 74, 80);

    void Paint(Canvas& c, int hoverRow, int hoverZone)
    {
        c.fill(0, 0, c.w, c.h, kBg);
        c.fill(0, 0, c.w, 6, kAmber);
        c.text("VR SETTINGS", 40, 10, c.w - 40, kHeaderPx - 10, kAmber, 46, true, DT_LEFT);
        c.text("trigger: change   circle: close", 40, 10, c.w - 40, kHeaderPx - 10, kDim, 24, false, DT_RIGHT);
        const auto& rows = Rows();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        {
            const Row& r = rows[i];
            const int y0 = RowTop(i), y1 = y0 + RowHeight(r);
            const bool hover = (i == hoverRow);
            if (r.kind == RowKind::Section)
            {
                c.text(r.label, 40, y0 + 8, c.w - 40, y1, kAmber, 26, true, DT_LEFT);
                c.fill(40, y1 - 4, c.w - 40, y1 - 2, RGB(90, 70, 20));
                continue;
            }
            if (hover)
                c.fill(16, y0 + 2, c.w - 16, y1 - 2, kRowHover);
            if (r.kind == RowKind::Close)
            {
                c.fill(c.w / 2 - 120, y0 + 8, c.w / 2 + 120, y1 - 8, hover ? kBtnHover : kBtn);
                c.text("Close", 0, y0, c.w, y1, hover ? RGB(0, 0, 0) : kText, 34, true, DT_CENTER);
                continue;
            }
            c.text(r.label, 40, y0, kMinusX0 - 10, y1, kText, 34, false, DT_LEFT);
            if (r.kind == RowKind::Toggle)
            {
                const bool on = r.b && *r.b;
                c.fill(kPillX0, y0 + 10, kPillX1, y1 - 10, on ? kOn : kOff);
                c.text(on ? "ON" : "OFF", kPillX0, y0, kPillX1, y1, RGB(255, 255, 255), 30, true, DT_CENTER);
            }
            else
            {
                char val[32]{};
                _snprintf(val, sizeof(val) - 1, r.fmt, r.f ? *r.f : 0.f);
                c.fill(kMinusX0, y0 + 10, kMinusX1 - 8, y1 - 10, (hover && hoverZone == 0) ? kBtnHover : kBtn);
                c.fill(kPlusX0 + 8, y0 + 10, kPlusX1, y1 - 10, (hover && hoverZone == 2) ? kBtnHover : kBtn);
                c.text("-", kMinusX0, y0, kMinusX1 - 8, y1 - 4, kText, 44, true, DT_CENTER);
                c.text("+", kPlusX0 + 8, y0, kPlusX1, y1 - 2, kText, 40, true, DT_CENTER);
                c.text(val, kValueX0, y0, kValueX1, y1, kText, 34, true, DT_CENTER);
            }
        }
        c.setAlpha(238);
    }

    struct MenuVertTex
    {
        float x, y, z, rhw;
        D3DCOLOR color;
        float u, v;
    };
    struct MenuVert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };
}

IDirect3DTexture9* VR::SettingsGearTexture(IDirect3DDevice9* device)
{
    if (!device)
        return nullptr;
    if (g_Gfx.device && g_Gfx.device != device)
        ReleaseGfx();
    g_Gfx.device = device;
    if (!g_Gfx.gearGpu)
    {
        Canvas c(256, 128);
        if (!c.ok())
            return nullptr;
        c.fill(0, 0, c.w, c.h, RGB(0, 0, 0));
        c.text("VR", 0, 0, c.w, 92, RGB(255, 255, 255), 84, true, DT_CENTER);
        c.text("SETTINGS", 0, 84, c.w, 124, RGB(255, 255, 255), 30, true, DT_CENTER);
        // Alpha from brightness: white text on a transparent background.
        GdiFlush();
        for (int i = 0; i < c.w * c.h; ++i)
        {
            const uint32_t p = c.px[i];
            const uint32_t a = (std::max)({ p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF });
            c.px[i] = (a << 24) | 0x00FFFFFFu;
        }
        IDirect3DTexture9* sys = nullptr;
        if (!Upload(device, c, sys, g_Gfx.gearGpu))
        {
            if (g_Gfx.gearGpu) { g_Gfx.gearGpu->Release(); g_Gfx.gearGpu = nullptr; }
        }
        if (sys)
            sys->Release();
    }
    return g_Gfx.gearGpu;
}

void VR::OpenSettingsPanel()
{
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    Vector fwd, right, up;
    GetViewBasis(&fwd, &right, &up);
    Vector yawFwd = fwd;
    yawFwd.z = 0.f;
    if (VectorNormalize(yawFwd) <= 0.01f)
        yawFwd = Vector(1.f, 0.f, 0.f);
    const Vector worldUp(0.f, 0.f, 1.f);
    // Source axes: x forward, y left, z up, so forward x up points right.
    Vector yawRight = CrossProduct(yawFwd, worldUp);
    VectorNormalize(yawRight);
    const Vector eye = GetViewOrigin(body);
    const Vector center = eye + yawFwd * kPanelDistanceHu - worldUp * kPanelDropHu;
    // Same latching as the weapon menu: stored relative to the body and the
    // turn offset, so the panel stays put in the room while you look around.
    m_SettingsLatchDelta = center - body;
    m_SettingsLatchFwd = yawFwd;
    m_SettingsLatchRight = yawRight;
    m_SettingsLatchYaw = m_RotationOffsetY.load(std::memory_order_acquire);
    m_SettingsOpen = true;
    m_SettingsHover = -1;
    m_SettingsHoverZone = -1;
    m_SettingsDirty = true;
    m_SettingsChanged = false;
    m_SettingsClickHeld = true;   // ignore the trigger until it is released once
    m_SettingsBackHeld = true;
    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 900, 0.35f);
    // Pause without Black Mesa's menu (that would close the panel).
    m_SettingsPausedGame = bmvr::g_VrSettingsPause;
    if (m_SettingsPausedGame)
        m_PendingPauseCmd.store(1, std::memory_order_release);
    Game::logMsg("VR settings panel opened pause=%d", m_SettingsPausedGame ? 1 : 0);
}

void VR::CloseSettingsPanel(const char* why)
{
    if (!m_SettingsOpen)
        return;
    m_SettingsOpen = false;
    if (m_SettingsChanged)
    {
        std::vector<std::pair<std::string, std::string>> kv;
        for (const Row& r : Rows())
        {
            if (r.key)
                kv.emplace_back(r.key, FormatValue(r));
        }
        const bool ok = WriteConfigValues(kv);
        Game::logMsg("VR settings saved to config.txt: %s", ok ? "ok" : "FAILED");
    }
    m_SettingsChanged = false;
    // Closed by Black Mesa's pause menu: leave the pause to that menu.
    if (m_SettingsPausedGame && !(why && std::strcmp(why, "pause") == 0))
        m_PendingPauseCmd.store(2, std::memory_order_release);
    m_SettingsPausedGame = false;
    Game::logMsg("VR settings panel closed (%s)", why ? why : "");
}

void VR::SettingsPanelFrame(Vector& origin, Vector& fwd, Vector& right, Vector& up) const
{
    const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    const float yawDelta = m_RotationOffsetY.load(std::memory_order_acquire) - m_SettingsLatchYaw;
    const float a = yawDelta * (kPi / 180.f), c = cosf(a), s = sinf(a);
    auto yaw = [&](const Vector& v) { return Vector(v.x * c - v.y * s, v.x * s + v.y * c, v.z); };
    origin = body + yaw(m_SettingsLatchDelta);
    fwd = yaw(m_SettingsLatchFwd);
    right = yaw(m_SettingsLatchRight);
    up = Vector(0.f, 0.f, 1.f);
}

void VR::UpdateSettingsPanel()
{
    if (!m_SettingsOpen)
        return;
    if (!m_GameplayEligible || PauseUiActive())
    {
        CloseSettingsPanel("pause");
        return;
    }
    Vector origin, fwd, right, up;
    SettingsPanelFrame(origin, fwd, right, up);

    // Pointer: the right controller's own ray (not the shot trim).
    const Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    const Vector start = GetRightControllerAbsPos(body);
    Vector dir;
    QAngle::AngleVectors(GetRightControllerAbsAngle(), &dir, nullptr, nullptr);
    int row = -1, zone = -1;
    m_SettingsHitValid = false;
    const float denom = dir.Dot(fwd);
    if (fabsf(denom) > 1e-4f)
    {
        const float t = (origin - start).Dot(fwd) / denom;
        if (t > 0.f && t < 200.f)
        {
            const Vector hit = start + dir * t;
            const Vector rel = hit - origin;
            const float x = rel.Dot(right), y = rel.Dot(up);
            const float hw = kPanelWidthHu * 0.5f, hh = PanelHeightHu() * 0.5f;
            if (fabsf(x) <= hw && fabsf(y) <= hh)
            {
                m_SettingsHitX = x;
                m_SettingsHitY = y;
                m_SettingsHitValid = true;
                const int px = static_cast<int>((x / kPanelWidthHu + 0.5f) * kTexW);
                const int py = static_cast<int>((0.5f - y / PanelHeightHu()) * TexH());
                const int r = RowAt(py);
                if (r >= 0)
                {
                    row = r;
                    zone = (px >= kMinusX0 && px < kMinusX1) ? 0 : (px >= kPlusX0 && px < kPlusX1) ? 2 : 1;
                }
            }
        }
    }
    if (row != m_SettingsHover || zone != m_SettingsHoverZone)
    {
        if (row >= 0 && row != m_SettingsHover)
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 300, 0.15f);
        m_SettingsHover = row;
        m_SettingsHoverZone = zone;
        m_SettingsDirty = true;
    }

    // Trigger only, full pull (Cross and a resting finger on the trigger
    // both read as presses on the PS VR2 Sense, see the menu).
    static bool s_panelTrigLatch;
    const bool click = MenuTriggerClickHeld(s_panelTrigLatch);
    const bool back = PressedDigitalAction(m_ActionMenuBack);
    const bool clickEdge = click && !m_SettingsClickHeld;
    const bool backEdge = back && !m_SettingsBackHeld;
    m_SettingsClickHeld = click;
    m_SettingsBackHeld = back;
    if (backEdge)
    {
        CloseSettingsPanel("circle");
        return;
    }
    if (!clickEdge || row < 0)
        return;
    Row& r = Rows()[row];
    switch (r.kind)
    {
    case RowKind::Section:
        return;
    case RowKind::Close:
        CloseSettingsPanel("close row");
        return;
    case RowKind::Toggle:
        if (r.b)
            *r.b = !*r.b;
        break;
    case RowKind::Number:
        if (r.f && zone != 1)
        {
            float v = *r.f + (zone == 2 ? r.step : -r.step);
            v = std::round(v / r.step) * r.step;   // no drift from repeated float steps
            *r.f = std::clamp(v, r.lo, r.hi);
        }
        else
            return;
        break;
    }
    m_SettingsChanged = true;
    m_SettingsDirty = true;
    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 700, 0.3f);
    Game::logMsg("VR settings: %s = %s", r.label, FormatValue(r).c_str());
}

void VR::DrawSettingsPanel(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    if (!m_SettingsOpen || !device)
        return;
    if (g_Gfx.device && g_Gfx.device != device)
        ReleaseGfx();
    g_Gfx.device = device;
    if (m_SettingsDirty || !g_Gfx.gpu)
    {
        Canvas c(kTexW, TexH());
        if (c.ok())
        {
            Paint(c, m_SettingsHover, m_SettingsHoverZone);
            if (g_Gfx.w != c.w || g_Gfx.h != c.h)
            {
                if (g_Gfx.sys) { g_Gfx.sys->Release(); g_Gfx.sys = nullptr; }
                if (g_Gfx.gpu) { g_Gfx.gpu->Release(); g_Gfx.gpu = nullptr; }
                g_Gfx.w = c.w;
                g_Gfx.h = c.h;
            }
            if (Upload(device, c, g_Gfx.sys, g_Gfx.gpu))
                m_SettingsDirty = false;
        }
    }
    if (!g_Gfx.gpu)
        return;

    // Same projection as DrawWeaponMenu (union frustum + this eye's off-centre remap).
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
        const Vector d = world - eyeOrig;
        const float z = d.Dot(fwd);
        if (z < 0.5f)
            return false;
        rhw = 1.f / z;
        const float ndcX = (d.Dot(right) * rhw) / tanHalf * ocSx + ocOx;
        const float ndcY = ((d.Dot(up) * rhw) * aspect) / tanHalf * ocSy + ocOy;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return true;
    };

    Vector po, pf, pr, pu;
    SettingsPanelFrame(po, pf, pr, pu);
    const float hw = kPanelWidthHu * 0.5f, hh = PanelHeightHu() * 0.5f;
    const Vector corners[4] = { po - pr * hw + pu * hh, po + pr * hw + pu * hh, po - pr * hw - pu * hh, po + pr * hw - pu * hh };
    MenuVertTex v[4]{};
    const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; ++i)
    {
        if (!project(corners[i], v[i].x, v[i].y, v[i].rhw))
            return;
        v[i].color = D3DCOLOR_ARGB(255, 255, 255, 255);
        v[i].u = uv[i][0];
        v[i].v = uv[i][1];
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetTexture(0, g_Gfx.gpu);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVertTex));
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    // Pointer dot where the controller ray meets the panel.
    if (m_SettingsHitValid)
    {
        const Vector p = po + pr * m_SettingsHitX + pu * m_SettingsHitY - pf * 0.05f;
        constexpr int kSegs = 16;
        constexpr float kR = 0.18f;
        MenuVert fan[kSegs + 2]{};
        float cx, cy, crhw;
        if (project(p, cx, cy, crhw))
        {
            fan[0] = { cx, cy, 0.f, crhw, D3DCOLOR_ARGB(255, 255, 255, 255) };
            bool ok = true;
            for (int k = 0; k <= kSegs && ok; ++k)
            {
                const float a = static_cast<float>(k) * (2.f * kPi / kSegs);
                ok = project(p + pr * (cosf(a) * kR) + pu * (sinf(a) * kR), fan[k + 1].x, fan[k + 1].y, fan[k + 1].rhw);
                fan[k + 1].z = 0.f;
                fan[k + 1].color = D3DCOLOR_ARGB(255, 255, 255, 255);
            }
            if (ok)
                device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kSegs, fan, sizeof(MenuVert));
        }
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void VR::ShowToast(const char* text)
{
    strncpy_s(m_ToastText, text ? text : "", _TRUNCATE);
    m_ToastUntilMs = GetTickCount() + kToastMs;
}

bool VR::ToastVisible() const
{
    return m_ToastUntilMs != 0 && static_cast<int>(m_ToastUntilMs - GetTickCount()) > 0;
}

void VR::DrawToast(IDirect3DDevice9* device, int stereoEye, UINT w, UINT h,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    // Short head-locked message (quick save), slightly below the view centre.
    if (!device || !ToastVisible())
        return;
    if (g_Gfx.device && g_Gfx.device != device)
        ReleaseGfx();
    g_Gfx.device = device;
    if (!g_Gfx.toastGpu || std::strcmp(g_Gfx.toastText, m_ToastText) != 0)
    {
        if (g_Gfx.toastGpu) { g_Gfx.toastGpu->Release(); g_Gfx.toastGpu = nullptr; }
        Canvas c(512, 96);
        if (!c.ok())
            return;
        c.fill(0, 0, c.w, c.h, kBg);
        c.fill(0, 0, c.w, 5, kAmber);
        c.text(m_ToastText, 0, 5, c.w, c.h, kAmber, 54, true, DT_CENTER);
        c.setAlpha(225);
        IDirect3DTexture9* sys = nullptr;
        if (!Upload(device, c, sys, g_Gfx.toastGpu) && g_Gfx.toastGpu)
        {
            g_Gfx.toastGpu->Release();
            g_Gfx.toastGpu = nullptr;
        }
        if (sys)
            sys->Release();
        strncpy_s(g_Gfx.toastText, m_ToastText, _TRUNCATE);
    }
    if (!g_Gfx.toastGpu)
        return;

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
        const Vector d = world - eyeOrig;
        const float z = d.Dot(fwd);
        if (z < 0.5f)
            return false;
        rhw = 1.f / z;
        const float ndcX = (d.Dot(right) * rhw) / tanHalf * ocSx + ocOx;
        const float ndcY = ((d.Dot(up) * rhw) * aspect) / tanHalf * ocSy + ocOy;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return true;
    };
    // Both eyes share the head-centred placement, so it fuses at 30 HU.
    Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
    const Vector head = GetViewOrigin(body);
    const Vector po = head + fwd * 30.f - up * 5.f;
    const float hw = 6.f, hh = hw * 96.f / 512.f;
    const Vector corners[4] = { po - right * hw + up * hh, po + right * hw + up * hh, po - right * hw - up * hh, po + right * hw - up * hh };
    const int left = static_cast<int>(m_ToastUntilMs - GetTickCount());
    const int alpha = (std::min)(255, (std::max)(0, left * 255 / 400));
    MenuVertTex v[4]{};
    const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; ++i)
    {
        if (!project(corners[i], v[i].x, v[i].y, v[i].rhw))
            return;
        v[i].color = D3DCOLOR_ARGB(alpha, 255, 255, 255);
        v[i].u = uv[i][0];
        v[i].v = uv[i][1];
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetTexture(0, g_Gfx.toastGpu);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVertTex));
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}
