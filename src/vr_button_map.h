#pragma once

// Button mapping (OpenXR helper backend): any physical button -> any action,
// per controller family, with press + hold. Plan:
// docs/plans/bmvr-control-mapping.md (dev workspace). The helper publishes the
// physical inputs per hand (protocol 18, rawHands); this layer turns them
// into action states that VR::GetDigitalActionData returns for the mappable
// actions. Defaults = the layout before mapping existed; VR/config.txt holds
// only changes, as Bind.<family>.<hand>.<input>[.hold]=<Action>.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct L4D2VROpenXrInputStateDesc;

namespace bmvr
{
    enum class BindAction : uint8_t
    {
        Nothing = 0,
        Fire,
        AltFire,
        Reload,
        Jump,
        CrouchToggle,
        CrouchHold,
        Use,
        Sprint,
        SprintToggle,
        Flashlight,
        WeaponMenu,
        HandsToggle,   // bare hands <-> the weapon held before
        NextWeapon,
        PrevWeapon,
        QuickSave,
        QuickLoad,
        Pause,
        VrSettings,
        Recenter,
        Count
    };

    // Per hand. StickUp / StickDown exist on the right hand only (the left
    // stick moves).
    enum class BindInput : uint8_t
    {
        Trigger = 0,
        Grip,
        Face1,      // A / X / Cross (Index: A; Frame: A / D-pad down)
        Face2,      // B / Y / Circle (Index: B; Frame: B / D-pad right)
        StickClick,
        StickUp,
        StickDown,
        Face3,      // Steam Frame only: right X / left D-pad left
        Face4,      // Steam Frame only: right Y / left D-pad up
        Bumper,     // Steam Frame only
        Count
    };

    constexpr int kBindHands = 2; // 0 = left, 1 = right
    constexpr int kBindActionCount = static_cast<int>(BindAction::Count);
    constexpr int kBindInputCount = static_cast<int>(BindInput::Count);

    struct BindSlot
    {
        BindAction press = BindAction::Nothing;
        BindAction hold = BindAction::Nothing;
    };

    const char* BindActionToken(BindAction a);  // config value, e.g. "AltFire"
    const char* BindActionLabel(BindAction a);  // panel text, e.g. "Alt-fire"
    const char* BindInputToken(BindInput i);    // config key part, e.g. "face1"
    bool ParseBindAction(const char* token, BindAction& out);

    // Panel name of a button on the detected controller ("A", "Cross", "Y").
    const char* BindInputLabel(uint32_t family, const char* systemName, int hand, BindInput input);
    // The input exists on that controller (rows of missing ones are hidden).
    bool BindInputExists(uint32_t family, uint32_t rawButtons, int hand, BindInput input);

    // VR/config.txt: key without the "Bind." prefix, e.g. "touch.right.face1.hold".
    // Returns false for an unknown key or action (logged).
    bool SetBindFromConfig(const char* key, const char* value);

    // Built-in default for a slot (fixed; see MigrateLegacyBindSettings).
    BindSlot DefaultBind(uint32_t family, int hand, BindInput input);
    // Startup, after config.txt: the old FlashlightOnSquare / SettingsTriangleHold /
    // CrouchStickHold / SprintToggle switches (non-default values) become Bind.*
    // mappings where the player has none. `set` gets the config lines to write.
    void MigrateLegacyBindSettings(bool flashlightOnSquare, bool triangleHold, bool crouchStickHold, bool sprintToggle,
        std::vector<std::pair<std::string, std::string>>& set);
    // Some button (press or hold) opens the VR settings panel.
    bool VrSettingsMapped(uint32_t family);
    // VR/config.txt: set these keys and remove the `drop` keys, every other
    // line kept (the VR panel's writer, vr_settings_panel.cpp).
    bool WriteVrConfigValues(const std::vector<std::pair<std::string, std::string>>& kv,
        const std::vector<std::string>& drop);
    // Default unless VR/config.txt (or the panel) changed it.
    BindSlot EffectiveBind(uint32_t family, int hand, BindInput input);
    // Panel: change one half of a slot; returns the config key ("Bind.touch.right.face1")
    // and the value to save ("" = back at the default, remove the key).
    void SetBind(uint32_t family, int hand, BindInput input, bool hold, BindAction a,
        std::string& configKey, std::string& configValue);
    void ResetBinds(uint32_t family);

    // Presets (VR panel): 0 = the defaults; others change a few slots on top
    // of them. Applied to one controller family.
    int BindPresetCount();
    const char* BindPresetName(int index);
    bool BindPresetUsable(int index, uint32_t family); // its buttons exist on that controller
    int MatchBindPreset(uint32_t family);               // -1 = custom
    void ApplyBindPreset(uint32_t family, int index);
    // Every Bind.<family>.* slot key with its config value ("" = at the
    // default: remove the key), for the panel's save on close.
    void CollectBindConfig(uint32_t family, std::vector<std::pair<std::string, std::string>>& set,
        std::vector<std::string>& drop);

    struct ButtonMapContext
    {
        bool gripInGestureZone[kBindHands] = {}; // holster/shoulder zone: grip = grab
        bool stickBlocked = false;               // weapon menu: right stick selects
    };

    class ButtonMapper
    {
    public:
        // True once the helper publishes raw inputs (protocol 18) for a hand.
        static bool RawAvailable(const L4D2VROpenXrInputStateDesc& in);

        // Once per ProcessInput. Inputs still held when mapping (re)starts
        // wait for a release first, so a button pressed in a menu does not
        // fire its gameplay action when the menu closes.
        void Update(const L4D2VROpenXrInputStateDesc& in, const ButtonMapContext& ctx, uint32_t nowMs);
        // Mapping off (menus, VR panel, no raw inputs): next Update starts fresh.
        void Suspend();

        bool Held(BindAction a) const { return m_Held[static_cast<int>(a)]; }
        bool Pressed(BindAction a) const { return m_Held[static_cast<int>(a)] && !m_PrevHeld[static_cast<int>(a)]; }
        uint32_t Family() const { return m_Family; }
        bool Active() const { return m_Active; }

    private:
        struct InputState
        {
            bool down = false;
            bool waitRelease = false;
            bool holdFired = false;
            uint32_t downMs = 0;
            uint32_t pulseUntilMs = 0;
            BindAction pulseAction = BindAction::Nothing;
        };

        void LogLayout(const L4D2VROpenXrInputStateDesc& in) const;

        InputState m_State[kBindHands][kBindInputCount + 1]{}; // +1: left menu (fixed Pause)
        bool m_Held[kBindActionCount]{};
        bool m_PrevHeld[kBindActionCount]{};
        bool m_Active = false;
        uint32_t m_Family = ~0u;
        uint32_t m_LayoutGeneration = ~0u;
        bool m_StickTurned = false;
        bool m_StickCrouchHeld = false;
        bool m_TriggerDown[kBindHands]{};
        bool m_GripDown[kBindHands]{};
    };
}
