// Button mapping layer: physical inputs (helper protocol 18) -> actions.
// See vr_button_map.h.

#include "vr_button_map.h"

#include "bmvr_flags.h"
#include "game.h"
#include "openxr_bridge_protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace bmvr
{
    namespace
    {
        struct ActionInfo
        {
            const char* token;
            const char* label;
        };

        const ActionInfo kActions[kBindActionCount] =
        {
            { "Nothing", "Nothing" },
            { "Fire", "Fire" },
            { "AltFire", "Alt-fire" },
            { "Reload", "Reload" },
            { "Jump", "Jump" },
            { "CrouchToggle", "Crouch (toggle)" },
            { "CrouchHold", "Crouch (hold)" },
            { "Use", "Use" },
            { "Sprint", "Sprint (hold)" },
            { "SprintToggle", "Sprint (toggle)" },
            { "Flashlight", "Flashlight" },
            { "WeaponMenu", "Weapon menu" },
            { "NextWeapon", "Next weapon" },
            { "PrevWeapon", "Previous weapon" },
            { "QuickSave", "Quick save" },
            { "QuickLoad", "Quick load" },
            { "Pause", "Pause" },
            { "VrSettings", "VR settings" },
            { "Recenter", "Recenter" },
        };

        const char* const kInputTokens[kBindInputCount] =
        {
            "trigger", "grip", "face1", "face2", "stickclick", "stickup", "stickdown"
        };

        // Overrides from VR/config.txt / the panel, key "touch.right.face1[.hold]".
        std::mutex g_BindMutex;
        std::map<std::string, BindAction> g_BindOverrides;

        uint32_t StorageFamily(uint32_t family)
        {
            // Unknown controllers use the Touch layout (most common), and share its settings.
            return family == L4D2VR_OPENXR_CONTROLLER_FAMILY_UNKNOWN ? L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH : family;
        }

        std::string SlotKey(uint32_t family, int hand, BindInput input, bool hold)
        {
            std::string k = L4D2VR_ControllerFamilyName(StorageFamily(family));
            k += hand == 0 ? ".left." : ".right.";
            k += kInputTokens[static_cast<int>(input)];
            if (hold)
                k += ".hold";
            return k;
        }

        bool IsPlayStation(const char* systemName)
        {
            return systemName && (L4D2VR_TextContainsI(systemName, "PlayStation")
                || L4D2VR_TextContainsI(systemName, "PS VR2") || L4D2VR_TextContainsI(systemName, "PSVR2"));
        }
    }

    const char* BindActionToken(BindAction a)
    {
        const int i = static_cast<int>(a);
        return (i >= 0 && i < kBindActionCount) ? kActions[i].token : "Nothing";
    }

    const char* BindActionLabel(BindAction a)
    {
        const int i = static_cast<int>(a);
        return (i >= 0 && i < kBindActionCount) ? kActions[i].label : "Nothing";
    }

    const char* BindInputToken(BindInput i)
    {
        const int n = static_cast<int>(i);
        return (n >= 0 && n < kBindInputCount) ? kInputTokens[n] : "?";
    }

    bool ParseBindAction(const char* token, BindAction& out)
    {
        if (!token)
            return false;
        for (int i = 0; i < kBindActionCount; ++i)
        {
            if (_stricmp(token, kActions[i].token) == 0)
            {
                out = static_cast<BindAction>(i);
                return true;
            }
        }
        return false;
    }

    const char* BindInputLabel(uint32_t family, const char* systemName, int hand, BindInput input)
    {
        const bool left = hand == 0;
        switch (input)
        {
        case BindInput::Trigger: return "Trigger";
        case BindInput::Grip: return "Grip";
        case BindInput::StickClick: return family == L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE ? "Pad click" : "Stick click";
        case BindInput::StickUp: return "Stick up";
        case BindInput::StickDown: return "Stick down";
        case BindInput::Face1:
            if (IsPlayStation(systemName))
                return left ? "Square" : "Cross";
            if (family == L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES)
                return "A";
            return left ? "X" : "A";
        case BindInput::Face2:
            if (IsPlayStation(systemName))
                return left ? "Triangle" : "Circle";
            if (family == L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES)
                return "B";
            return left ? "Y" : "B";
        default: return "?";
        }
    }

    bool BindInputExists(uint32_t family, uint32_t rawButtons, int hand, BindInput input)
    {
        switch (input)
        {
        case BindInput::StickUp:
        case BindInput::StickDown:
            return hand == 1;
        case BindInput::Trigger:
            return true;
        case BindInput::Grip:
            return rawButtons == 0 || (rawButtons & L4D2VR_RAW_BUTTON_GRIP_PRESENT) != 0;
        case BindInput::StickClick:
            return rawButtons == 0 || (rawButtons & L4D2VR_RAW_BUTTON_STICK_CLICK_PRESENT) != 0;
        case BindInput::Face1:
            if (rawButtons)
                return (rawButtons & L4D2VR_RAW_BUTTON_FACE1_PRESENT) != 0;
            return family != L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE && family != L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2;
        case BindInput::Face2:
            if (rawButtons)
                return (rawButtons & L4D2VR_RAW_BUTTON_FACE2_PRESENT) != 0;
            return family != L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE && family != L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2;
        default:
            return false;
        }
    }

    BindSlot DefaultBind(uint32_t family, int hand, BindInput input)
    {
        // The layout of the helper's hard-coded bindings before mapping existed
        // (AddTouchFaceButtonBindings / AddIndexFaceButtonBindings / AddStickAndTriggerBindings).
        family = StorageFamily(family);
        const bool knuckles = family == L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES;
        const bool faces = family == L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH || knuckles;
        // Fixed since 2026-11-18. The old switches that changed these defaults
        // (FlashlightOnSquare, SettingsTriangleHold, CrouchStickHold) are
        // turned into mappings once (MigrateLegacyBindSettings) and only
        // still drive the fixed layout (ButtonMapping=false).
        const BindAction pauseHold = BindAction::VrSettings;
        BindSlot s;
        if (hand == 0)
        {
            switch (input)
            {
            case BindInput::Trigger: s.press = BindAction::Use; break;
            case BindInput::Grip: break; // two-hand hold / quick-save gesture read the grip itself
            case BindInput::Face1:
                if (faces && !knuckles)
                    s.press = BindAction::PrevWeapon;
                break;
            case BindInput::Face2:
                if (faces)
                {
                    s.press = BindAction::Pause;
                    s.hold = pauseHold;
                }
                break;
            case BindInput::StickClick: s.press = BindAction::SprintToggle; break;
            default: break;
            }
        }
        else
        {
            switch (input)
            {
            case BindInput::Trigger: s.press = BindAction::Fire; break;
            case BindInput::Grip: s.press = BindAction::Flashlight; break;
            case BindInput::Face1: if (faces) s.press = BindAction::Reload; break;
            case BindInput::Face2: if (faces) s.press = BindAction::AltFire; break;
            case BindInput::StickClick: s.press = BindAction::WeaponMenu; break;
            case BindInput::StickUp: s.press = BindAction::Jump; break;
            case BindInput::StickDown:
                s.press = BindAction::CrouchHold;
                break;
            default: break;
            }
        }
        return s;
    }

    BindSlot EffectiveBind(uint32_t family, int hand, BindInput input)
    {
        BindSlot s = DefaultBind(family, hand, input);
        std::lock_guard<std::mutex> lock(g_BindMutex);
        auto p = g_BindOverrides.find(SlotKey(family, hand, input, false));
        if (p != g_BindOverrides.end())
            s.press = p->second;
        auto h = g_BindOverrides.find(SlotKey(family, hand, input, true));
        if (h != g_BindOverrides.end())
            s.hold = h->second;
        return s;
    }

    bool SetBindFromConfig(const char* key, const char* value)
    {
        // <family>.<hand>.<input>[.hold]
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s", key ? key : "");
        char* parts[4] = {};
        int n = 0;
        for (char* p = buf; n < 4;)
        {
            parts[n++] = p;
            char* dot = std::strchr(p, '.');
            if (!dot)
                break;
            *dot = 0;
            p = dot + 1;
        }
        BindAction a;
        if (n < 3 || !ParseBindAction(value, a))
        {
            Game::logMsg("Button map: ignored config Bind.%s=%s", key ? key : "", value ? value : "");
            return false;
        }
        uint32_t family = ~0u;
        for (uint32_t f : { L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH, L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2,
                 L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES, L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE })
            if (_stricmp(parts[0], L4D2VR_ControllerFamilyName(f)) == 0)
                family = f;
        const int hand = _stricmp(parts[1], "left") == 0 ? 0 : _stricmp(parts[1], "right") == 0 ? 1 : -1;
        int input = -1;
        for (int i = 0; i < kBindInputCount; ++i)
            if (_stricmp(parts[2], kInputTokens[i]) == 0)
                input = i;
        const bool hold = n >= 4 && _stricmp(parts[3], "hold") == 0;
        if (family == ~0u || hand < 0 || input < 0 || (n >= 4 && !hold))
        {
            Game::logMsg("Button map: ignored config Bind.%s=%s", key, value);
            return false;
        }
        std::lock_guard<std::mutex> lock(g_BindMutex);
        g_BindOverrides[SlotKey(family, hand, static_cast<BindInput>(input), hold)] = a;
        return true;
    }

    void SetBind(uint32_t family, int hand, BindInput input, bool hold, BindAction a,
        std::string& configKey, std::string& configValue)
    {
        const BindSlot def = DefaultBind(family, hand, input);
        const std::string key = SlotKey(family, hand, input, hold);
        configKey = "Bind." + key;
        std::lock_guard<std::mutex> lock(g_BindMutex);
        if ((hold ? def.hold : def.press) == a)
        {
            g_BindOverrides.erase(key);
            configValue.clear();
        }
        else
        {
            g_BindOverrides[key] = a;
            configValue = BindActionToken(a);
        }
    }

    void MigrateLegacyBindSettings(bool flashlightOnSquare, bool triangleHold, bool crouchStickHold, bool sprintToggle,
        std::vector<std::pair<std::string, std::string>>& set)
    {
        // Each old switch at its non-default value becomes the mapping it
        // used to stand for, on every controller family where that button
        // has that default, unless the player already mapped the slot.
        struct Want { int hand; BindInput input; bool hold; BindAction a; const char* why; };
        std::vector<Want> wants;
        if (flashlightOnSquare)
            wants.push_back({ 0, BindInput::Face1, false, BindAction::Flashlight, "FlashlightOnSquare" });
        if (!triangleHold)
            wants.push_back({ 0, BindInput::Face2, true, BindAction::Nothing, "SettingsTriangleHold" });
        if (!crouchStickHold)
            wants.push_back({ 1, BindInput::StickDown, false, BindAction::CrouchToggle, "CrouchStickHold" });
        if (!sprintToggle)
            wants.push_back({ 0, BindInput::StickClick, false, BindAction::Sprint, "SprintToggle" });
        std::lock_guard<std::mutex> lock(g_BindMutex);
        for (uint32_t f : { L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH, L4D2VR_OPENXR_CONTROLLER_FAMILY_HP_G2,
                 L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES, L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE })
        {
            for (const Want& w : wants)
            {
                const BindSlot def = DefaultBind(f, w.hand, w.input);
                const BindAction defA = w.hold ? def.hold : def.press;
                // Only where the old switch changed something (e.g. Square is
                // Previous weapon only on Touch-style controllers).
                const bool applies = w.input == BindInput::Face1 ? defA == BindAction::PrevWeapon
                    : w.input == BindInput::Face2 ? defA == BindAction::VrSettings
                    : w.input == BindInput::StickClick ? defA == BindAction::SprintToggle
                    : defA == BindAction::CrouchHold;
                if (!applies || defA == w.a)
                    continue;
                const std::string key = SlotKey(f, w.hand, w.input, w.hold);
                if (g_BindOverrides.count(key))
                    continue;   // the player's own mapping wins
                g_BindOverrides[key] = w.a;
                set.emplace_back("Bind." + key, BindActionToken(w.a));
                Game::logMsg("Button map: %s migrated to Bind.%s=%s", w.why, key.c_str(), BindActionToken(w.a));
            }
        }
    }

    bool VrSettingsMapped(uint32_t family)
    {
        for (int hand = 0; hand < kBindHands; ++hand)
            for (int in = 0; in < kBindInputCount; ++in)
            {
                const BindSlot s = EffectiveBind(family, hand, static_cast<BindInput>(in));
                if (s.press == BindAction::VrSettings || s.hold == BindAction::VrSettings)
                    return true;
            }
        return false;
    }

    void ResetBinds(uint32_t family)
    {
        const std::string prefix = std::string(L4D2VR_ControllerFamilyName(StorageFamily(family))) + ".";
        std::lock_guard<std::mutex> lock(g_BindMutex);
        for (auto it = g_BindOverrides.begin(); it != g_BindOverrides.end();)
            it = it->first.compare(0, prefix.size(), prefix) == 0 ? g_BindOverrides.erase(it) : std::next(it);
    }

    namespace
    {
        struct PresetEntry { int hand; BindInput input; bool hold; BindAction action; };
        struct BindPreset { const char* name; bool needsFaces; std::vector<PresetEntry> entries; };
        const std::vector<BindPreset>& BindPresets()
        {
            static const std::vector<BindPreset> presets = {
                // = HL2VR's own Touch defaults (trigger fire, A eject/reload, B
                // alt-fire, stick click weapons, stick up/down jump/crouch,
                // left stick click sprint, Y menu).
                { "Default (HL2VR style)", false, {} },
                // Jump on a button instead of the stick (upstream issue #16 asked
                // for it): stick up does nothing, so turning never jumps.
                { "Jump on A / Cross", true, {
                    { 1, BindInput::Face1, false, BindAction::Jump },
                    { 1, BindInput::Face2, false, BindAction::Reload },
                    { 1, BindInput::Face2, true, BindAction::AltFire },
                    { 1, BindInput::StickUp, false, BindAction::Nothing } } },
            };
            return presets;
        }

        BindSlot PresetSlot(const BindPreset& p, uint32_t family, int hand, BindInput input)
        {
            BindSlot s = DefaultBind(family, hand, input);
            for (const PresetEntry& e : p.entries)
            {
                if (e.hand != hand || e.input != input)
                    continue;
                (e.hold ? s.hold : s.press) = e.action;
            }
            return s;
        }

        bool FamilyHasFaces(uint32_t family)
        {
            family = StorageFamily(family);
            return family == L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH || family == L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES;
        }
    }

    int BindPresetCount() { return static_cast<int>(BindPresets().size()); }

    const char* BindPresetName(int index)
    {
        return (index >= 0 && index < BindPresetCount()) ? BindPresets()[index].name : "Custom";
    }

    bool BindPresetUsable(int index, uint32_t family)
    {
        return index >= 0 && index < BindPresetCount() && (!BindPresets()[index].needsFaces || FamilyHasFaces(family));
    }

    int MatchBindPreset(uint32_t family)
    {
        for (int i = 0; i < BindPresetCount(); ++i)
        {
            if (!BindPresetUsable(i, family))
                continue;
            bool match = true;
            for (int hand = 0; hand < kBindHands && match; ++hand)
                for (int in = 0; in < kBindInputCount && match; ++in)
                {
                    const BindSlot want = PresetSlot(BindPresets()[i], family, hand, static_cast<BindInput>(in));
                    const BindSlot have = EffectiveBind(family, hand, static_cast<BindInput>(in));
                    match = want.press == have.press && want.hold == have.hold;
                }
            if (match)
                return i;
        }
        return -1;
    }

    void ApplyBindPreset(uint32_t family, int index)
    {
        if (!BindPresetUsable(index, family))
            return;
        ResetBinds(family);
        std::string key, value;
        for (const PresetEntry& e : BindPresets()[index].entries)
            SetBind(family, e.hand, e.input, e.hold, e.action, key, value);
        Game::logMsg("Button map: preset '%s' (%s)", BindPresets()[index].name,
            L4D2VR_ControllerFamilyName(StorageFamily(family)));
    }

    void CollectBindConfig(uint32_t family, std::vector<std::pair<std::string, std::string>>& set,
        std::vector<std::string>& drop)
    {
        std::lock_guard<std::mutex> lock(g_BindMutex);
        for (int hand = 0; hand < kBindHands; ++hand)
            for (int in = 0; in < kBindInputCount; ++in)
                for (int hold = 0; hold < 2; ++hold)
                {
                    const std::string k = SlotKey(family, hand, static_cast<BindInput>(in), hold != 0);
                    auto it = g_BindOverrides.find(k);
                    if (it == g_BindOverrides.end())
                        drop.push_back("Bind." + k);
                    else
                        set.emplace_back("Bind." + k, BindActionToken(it->second));
                }
    }

    bool ButtonMapper::RawAvailable(const L4D2VROpenXrInputStateDesc& in)
    {
        return in.rawHands[0].valid != 0 || in.rawHands[1].valid != 0;
    }

    void ButtonMapper::Suspend()
    {
        if (m_Active)
            Game::logMsg("Button map: suspended (menu / VR panel / no raw inputs)");
        m_Active = false;
        std::memset(m_Held, 0, sizeof(m_Held));
        std::memset(m_PrevHeld, 0, sizeof(m_PrevHeld));
    }

    void ButtonMapper::LogLayout(const L4D2VROpenXrInputStateDesc& in) const
    {
        Game::logMsg("Button map: controller %s (system '%s'), hold %d ms",
            L4D2VR_ControllerFamilyName(m_Family), in.systemName, static_cast<int>(g_ButtonHoldMs));
        for (int hand = 0; hand < kBindHands; ++hand)
        {
            char line[512];
            int len = std::snprintf(line, sizeof(line), "Button map: %s:", hand == 0 ? "left" : "right");
            for (int i = 0; i < kBindInputCount && len < static_cast<int>(sizeof(line)); ++i)
            {
                const BindInput input = static_cast<BindInput>(i);
                if (!BindInputExists(m_Family, in.rawHands[hand].buttons, hand, input))
                    continue;
                const BindSlot s = EffectiveBind(m_Family, hand, input);
                const BindSlot d = DefaultBind(m_Family, hand, input);
                len += std::snprintf(line + len, sizeof(line) - len, " %s=%s%s%s%s",
                    BindInputLabel(m_Family, in.systemName, hand, input), BindActionToken(s.press),
                    s.hold != BindAction::Nothing ? "/hold " : "",
                    s.hold != BindAction::Nothing ? BindActionToken(s.hold) : "",
                    (s.press != d.press || s.hold != d.hold) ? "*" : "");
            }
            Game::logMsg("%s", line);
        }
    }

    void ButtonMapper::Update(const L4D2VROpenXrInputStateDesc& in, const ButtonMapContext& ctx, uint32_t nowMs)
    {
        const bool starting = !m_Active;
        m_Active = true;
        std::memcpy(m_PrevHeld, m_Held, sizeof(m_Held));
        std::memset(m_Held, 0, sizeof(m_Held));

        const uint32_t family = in.reserved0;
        // Re-log on a family change and on every bind change (panel).
        uint32_t bindGen;
        {
            std::lock_guard<std::mutex> lock(g_BindMutex);
            bindGen = static_cast<uint32_t>(g_BindOverrides.size()) * 7919u;
            for (const auto& [k, v] : g_BindOverrides)
                bindGen = bindGen * 31u + static_cast<uint32_t>(v) + static_cast<uint32_t>(k.size());
        }
        if (family != m_Family || bindGen != m_LayoutGeneration)
        {
            m_Family = family;
            m_LayoutGeneration = bindGen;
            LogLayout(in);
        }

        // Physical state per input.
        bool down[kBindHands][kBindInputCount + 1] = {};
        for (int hand = 0; hand < kBindHands; ++hand)
        {
            const L4D2VROpenXrRawHandDesc& r = in.rawHands[hand];
            if (!r.valid)
                continue;
            // Same thresholds as the helper's float actions (trigger 0.45),
            // with a release point below them.
            m_TriggerDown[hand] = r.trigger >= (m_TriggerDown[hand] ? 0.35f : 0.45f);
            m_GripDown[hand] = r.grip >= (m_GripDown[hand] ? 0.45f : 0.55f);
            down[hand][static_cast<int>(BindInput::Trigger)] = m_TriggerDown[hand];
            down[hand][static_cast<int>(BindInput::Grip)] = m_GripDown[hand];
            down[hand][static_cast<int>(BindInput::Face1)] = (r.buttons & L4D2VR_RAW_BUTTON_FACE1) != 0;
            down[hand][static_cast<int>(BindInput::Face2)] = (r.buttons & L4D2VR_RAW_BUTTON_FACE2) != 0;
            down[hand][static_cast<int>(BindInput::StickClick)] = (r.buttons & L4D2VR_RAW_BUTTON_STICK_CLICK) != 0;
            down[hand][kBindInputCount] = (r.buttons & L4D2VR_RAW_BUTTON_MENU) != 0;
        }

        // Right stick up / down: past StickJumpCrouchThreshold, within
        // StickJumpCrouchAngle of vertical; with TurnBlocksJumpCrouch a stick
        // that went sideways (turn) does neither until it is back near the
        // centre. A started "down" lasts until the stick is back near the
        // centre (or pushed up), so sliding along the bottom edge to turn
        // keeps the player crouched. (Moved from ProcessInput.)
        {
            const L4D2VROpenXrRawHandDesc& r = in.rawHands[1];
            bool up = false, dn = false;
            if (r.valid && !ctx.stickBlocked)
            {
                const float tx = r.stick[0], ty = r.stick[1];
                const float ax = std::fabs(tx), ay = std::fabs(ty);
                const bool centred = ax * ax + ay * ay < 0.25f * 0.25f;
                if (centred)
                    m_StickTurned = false;
                else if (ax > 0.5f && ax > ay && !m_StickCrouchHeld)
                    m_StickTurned = true;
                const float coneTan = std::tan(g_StickJumpCrouchAngle * 0.01745329f);
                const bool vertical = ay >= g_StickJumpCrouchThreshold && ax <= ay * coneTan;
                if (vertical && !(g_TurnBlocksJumpCrouch && m_StickTurned))
                {
                    up = ty > 0.f;
                    dn = ty < 0.f;
                }
                if (dn)
                    m_StickCrouchHeld = true;
                else if (centred || ty >= 0.25f)
                    m_StickCrouchHeld = false;
                if (m_StickCrouchHeld)
                    dn = true;
            }
            else
            {
                m_StickTurned = false;
                m_StickCrouchHeld = false;
            }
            down[1][static_cast<int>(BindInput::StickUp)] = up;
            down[1][static_cast<int>(BindInput::StickDown)] = dn;
        }

        const uint32_t holdMs = static_cast<uint32_t>(g_ButtonHoldMs);
        constexpr uint32_t kPulseMs = 80; // a tap's press action when a hold is mapped too
        for (int hand = 0; hand < kBindHands; ++hand)
        {
            for (int i = 0; i <= kBindInputCount; ++i)
            {
                InputState& st = m_State[hand][i];
                const bool isDown = down[hand][i];
                BindSlot slot;
                if (i == kBindInputCount)
                {
                    // Menu / system button: fixed Pause on the left hand.
                    if (hand == 0)
                    {
                        slot.press = BindAction::Pause;
                        slot.hold = BindAction::VrSettings;
                    }
                }
                else
                    slot = EffectiveBind(family, hand, static_cast<BindInput>(i));

                if (starting && isDown)
                    st.waitRelease = true;
                // Inside a holster/shoulder zone the grip is "grab": whatever
                // is mapped on it waits for the grip's release.
                if (i == static_cast<int>(BindInput::Grip) && isDown && ctx.gripInGestureZone[hand])
                {
                    st.waitRelease = true;
                    st.pulseUntilMs = 0;
                }
                if (st.waitRelease)
                {
                    if (!isDown)
                        st.waitRelease = false;
                    st.down = isDown;
                    continue;
                }

                if (isDown && !st.down)
                {
                    st.downMs = nowMs;
                    st.holdFired = false;
                }
                if (slot.hold == BindAction::Nothing)
                {
                    if (isDown)
                        m_Held[static_cast<int>(slot.press)] = true;
                }
                else
                {
                    if (isDown && !st.holdFired && nowMs - st.downMs >= holdMs)
                        st.holdFired = true;
                    if (isDown && st.holdFired)
                        m_Held[static_cast<int>(slot.hold)] = true;
                    if (!isDown && st.down && !st.holdFired)
                    {
                        st.pulseAction = slot.press;
                        st.pulseUntilMs = nowMs + kPulseMs;
                    }
                }
                if (st.pulseUntilMs && static_cast<int32_t>(st.pulseUntilMs - nowMs) > 0)
                    m_Held[static_cast<int>(st.pulseAction)] = true;
                else
                    st.pulseUntilMs = 0;
                st.down = isDown;
            }
        }
        m_Held[static_cast<int>(BindAction::Nothing)] = false;

        // One line per action press (bounded), so a log shows what a button did.
        static int s_pressLogs;
        for (int a = 1; a < kBindActionCount && s_pressLogs < 400; ++a)
        {
            if (m_Held[a] && !m_PrevHeld[a])
            {
                ++s_pressLogs;
                Game::logMsg("Button map: %s", kActions[a].token);
            }
        }
    }
}
