#include "bmvr_cvars.h"
#include "bmvr_flags.h"
#include "game.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// No Source SDK here, so a ConVar is built by cloning an existing client.dll
// ConVar and patching its fields. Layout (BM, Alien Swarm-era tier1; the same
// offsets Game::SetConVarInt writes):
//   0x00 ConCommandBase vtable   0x04 m_pNext   0x08 m_bRegistered
//   0x0C m_pszName   0x10 m_pszHelpString   0x14 m_nFlags
//   0x18 IConVar vtable   0x1C m_pParent   0x20 m_pszDefaultValue
//   0x24 m_pszString   0x28 m_StringLength   0x2C m_fValue ^ this
//   0x30 m_nValue ^ this   0x34 m_bHasMin   0x38 m_fMinVal
//   0x3C m_bHasMax   0x40 m_fMaxVal   0x44 change callbacks (CUtlVector)
// The cloned vtables are client.dll's tier1 code: the console and the
// options dialog set our values through it, and its GetDLLIdentifier makes
// the engine unregister our cvars together with client.dll's at shutdown.
// m_pszString points at our own buffer with a capacity the short values
// never exceed, so tier1 never reallocates or frees it.

namespace bmvr
{
    namespace
    {
        constexpr size_t kCloneBytes = 0x58;
        constexpr size_t kObjBytes = 0x80;
        constexpr int kStrCap = 64;
        // ICvar (VEngineCvar004 on BM): RegisterConCommand.
        constexpr int kICvarRegisterConCommand = 9;
        // ConCommandBase::GetName: slot 6 (2013 layout), 4 on 2007 builds
        // (same probe as Game's ConVarGetName).
        constexpr int kGetNameSlots[] = { 6, 4 };

        enum class Kind { Bool, Float };

        struct VrCvar
        {
            const char* name;
            const char* help;
            Kind kind;
            float minVal;
            float maxVal;
            bool* boolTarget;
            float* floatTarget;
            alignas(8) uint8_t obj[kObjBytes];
            char str[kStrCap];
            char def[kStrCap];
            float lastSeen;
        };

        VrCvar g_Cvars[] = {
            { "bmvr_crouch_hold", "VR: right stick down crouches only while held (0 = each push toggles)",
              Kind::Bool, 0.f, 1.f, &g_CrouchStickHold, nullptr },
            { "bmvr_sprint_toggle", "VR: a sprint click keeps sprinting until the move stick is released (0 = hold the click)",
              Kind::Bool, 0.f, 1.f, &g_SprintToggle, nullptr },
            { "bmvr_gun_pitch", "VR: extra gun pitch in degrees on top of the controller's own tilt (negative = lower)",
              Kind::Float, -45.f, 45.f, nullptr, &g_GunPitchOffset },
            { "bmvr_snap_turn", "VR: snap turning (0 = smooth)",
              Kind::Bool, 0.f, 1.f, &g_SnapTurning, nullptr },
            { "bmvr_snap_angle", "VR: snap turn angle in degrees",
              Kind::Float, 15.f, 90.f, nullptr, &g_SnapTurnAngle },
            { "bmvr_turn_speed", "VR: smooth turn speed",
              Kind::Float, 0.05f, 1.f, nullptr, &g_TurnSpeed },
            { "bmvr_recenter_yaw", "VR: recenter (VR settings panel) also resets the turn direction",
              Kind::Bool, 0.f, 1.f, &g_RecenterResetsYaw, nullptr },
            { "bmvr_recenter_on_resume", "VR: closing the pause menu recenters the head position (not the turn direction)",
              Kind::Bool, 0.f, 1.f, &g_AutoRecenterOnResume, nullptr },
            { "bmvr_aim_marker", "VR: mark where the gun points",
              Kind::Bool, 0.f, 1.f, &g_VrCrosshair, nullptr },
            { "bmvr_aim_dot", "VR: aim marker as a red laser dot (0 = amber crosshair)",
              Kind::Bool, 0.f, 1.f, &g_VrCrosshairDot, nullptr },
            { "bmvr_menu_smooth", "VR: menu pointer smoothing in seconds (lower = more direct)",
              Kind::Float, 0.02f, 0.6f, nullptr, &g_MenuCursorSmoothSec },
        };

        enum class Reg { NotYet, Done, Failed };
        bool g_Registered = false;
        bool g_Gave_up = false;
        int g_Attempts = 0;

        template <typename T>
        T& Field(void* obj, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + off); }

        float CurrentDefault(const VrCvar& c)
        {
            if (c.kind == Kind::Bool)
                return (c.boolTarget && *c.boolTarget) ? 1.f : 0.f;
            return c.floatTarget ? *c.floatTarget : 0.f;
        }

        void FormatValue(const VrCvar& c, float v, char* out, int cap)
        {
            if (c.kind == Kind::Bool)
                _snprintf(out, cap, "%d", v != 0.f ? 1 : 0);
            else
                _snprintf(out, cap, "%g", v);
            out[cap - 1] = 0;
        }

        // Same representation tier1's InternalSetValue leaves behind.
        void WriteValue(VrCvar& c, float v)
        {
            if (v < c.minVal)
                v = c.minVal;
            if (v > c.maxVal)
                v = c.maxVal;
            void* o = c.obj;
            const uint32_t key = reinterpret_cast<uint32_t>(o);
            union { float f; uint32_t u; } fu;
            fu.f = v;
            Field<uint32_t>(o, 0x2C) = fu.u ^ key;
            Field<uint32_t>(o, 0x30) = static_cast<uint32_t>(static_cast<int>(v)) ^ key;
            FormatValue(c, v, c.str, kStrCap);
        }

        float ReadValue(const VrCvar& c)
        {
            const void* o = c.obj;
            const uint32_t key = reinterpret_cast<uint32_t>(o);
            union { float f; uint32_t u; } fu;
            fu.u = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(o) + 0x2C) ^ key;
            return fu.f;
        }

        bool CallGetNameAt(void* obj, int slot, const char*& out)
        {
            out = nullptr;
            __try
            {
                void** vt = *reinterpret_cast<void***>(obj);
                using tGetName = const char*(__thiscall*)(void*);
                out = reinterpret_cast<tGetName>(vt[slot])(obj);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return true;
        }

        bool ReportsName(void* obj, const char* name)
        {
            for (int slot : kGetNameSlots)
            {
                const char* got = nullptr;
                if (CallGetNameAt(obj, slot, got) && got == name)
                    return true;
            }
            return false;
        }

        bool CallRegister(void* icvar, void* obj)
        {
            __try
            {
                void** vt = *reinterpret_cast<void***>(icvar);
                using tRegister = void(__thiscall*)(void*, void*);
                reinterpret_cast<tRegister>(vt[kICvarRegisterConCommand])(icvar, obj);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return true;
        }

        bool CopyTemplate(const void* tmpl, void* dst)
        {
            __try
            {
                std::memcpy(dst, tmpl, kCloneBytes);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return true;
        }

        void* FindTemplate(Game* game)
        {
            // Plain archived client.dll cvars (no callbacks, no material-thread flag).
            for (const char* name : { "cl_auto_crouch_jump", "cl_ironsight_sticky", "hud_fastswitch" })
            {
                void* c = game->FindConVar(name);
                if (!c)
                    continue;
                void* parent = nullptr;
                __try { parent = Field<void*>(c, 0x1C); }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (parent != c)
                    continue;
                Game::logMsg("VR cvars: template '%s' at %p", name, c);
                return c;
            }
            return nullptr;
        }

        Reg RegisterAll(Game* game)
        {
            void* icvar = game->m_Cvar;
            void* tmpl = icvar ? FindTemplate(game) : nullptr;
            if (!tmpl)
                return Reg::NotYet;
            int ok = 0;
            for (VrCvar& c : g_Cvars)
            {
                if (game->FindConVar(c.name))
                {
                    Game::logMsg("VR cvars: %s already exists, skipped", c.name);
                    continue;
                }
                std::memset(c.obj, 0, sizeof(c.obj));
                if (!CopyTemplate(tmpl, c.obj))
                    return Reg::Failed;
                void* o = c.obj;
                Field<void*>(o, 0x04) = nullptr;                 // m_pNext
                Field<uint32_t>(o, 0x08) = 0;                    // m_bRegistered (+pad)
                Field<const char*>(o, 0x0C) = c.name;
                Field<const char*>(o, 0x10) = c.help;
                // Not archived: VR/config.txt (the VR settings panel) is the one
                // place these are stored. Archived, config.cfg overrode the panel
                // at every start and kept stale values (2026-10-27).
                Field<int>(o, 0x14) = 0;
                Field<void*>(o, 0x1C) = o;                       // m_pParent
                FormatValue(c, CurrentDefault(c), c.def, kStrCap);
                Field<const char*>(o, 0x20) = c.def;
                Field<char*>(o, 0x24) = c.str;
                Field<int>(o, 0x28) = kStrCap;
                std::memset(static_cast<uint8_t*>(o) + 0x34, 0, kCloneBytes - 0x34); // min/max off, no callbacks
                WriteValue(c, CurrentDefault(c));

                if (!ReportsName(o, c.name))
                {
                    Game::logMsg("VR cvars: clone of the template does not report its name - layout differs, giving up");
                    return Reg::Failed;
                }
                if (!CallRegister(icvar, o) || game->FindConVar(c.name) != o)
                {
                    Game::logMsg("VR cvars: RegisterConCommand failed for %s, giving up", c.name);
                    return Reg::Failed;
                }
                ++ok;
            }
            Game::logMsg("VR cvars: registered %d", ok);
            return Reg::Done;
        }
    }

    void TickVrCvars(Game* game)
    {
        if (!game || g_Gave_up)
            return;
        if (!g_Registered)
        {
            if (++g_Attempts > 600)
            {
                g_Gave_up = true;
                Game::logMsg("VR cvars: no usable template cvar, menu/console settings off (VR/config.txt still applies)");
                return;
            }
            const Reg r = RegisterAll(game);
            if (r == Reg::Failed)
            {
                g_Gave_up = true;
                return;
            }
            if (r == Reg::NotYet)
            {
                if (g_Attempts == 1 || g_Attempts % 120 == 0)
                    Game::logMsg("VR cvars: not registered yet (attempt %d)", g_Attempts);
                return;
            }
            g_Registered = true;
            for (VrCvar& c : g_Cvars)
            {
                WriteValue(c, CurrentDefault(c));
                c.lastSeen = ReadValue(c);
            }
        }
        // A console change writes its flag; a panel change (flag moved, cvar
        // still at the last seen value) is mirrored into the cvar.
        for (VrCvar& c : g_Cvars)
        {
            float v = ReadValue(c);
            if (v == c.lastSeen)
            {
                const float flag = CurrentDefault(c);
                if (flag != v)
                {
                    WriteValue(c, flag);
                    c.lastSeen = ReadValue(c);
                }
                continue;
            }
            c.lastSeen = v;
            if (!(v >= c.minVal))
                v = c.minVal;
            if (!(v <= c.maxVal))
                v = c.maxVal;
            if (c.kind == Kind::Bool && c.boolTarget)
                *c.boolTarget = v != 0.f;
            else if (c.floatTarget)
                *c.floatTarget = v;
        }
    }
}
