#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// Glow trace (cvar bmvr_glow_trace <seconds>, VR panel DEBUG "Record glow
// trace (20 s)"): records every light glow's visibility sample (the engine's
// value and the value shown after GlowHold) together with the game's Presents
// and the frames published to the headset, into VR/glowtrace-*.csv, and logs
// a per-glow pulse summary. Playtest aid for glows that pulse in a steady
// rhythm: the CSV shows whether the period follows the frame/present timing.
namespace bmvr::glowtrace
{
    extern std::atomic<bool> g_Active;
    inline bool Active() { return g_Active.load(std::memory_order_relaxed); }

    // Present thread, every Present. Starts a capture when g_GlowTraceSeconds
    // is set (header = one line of settings for the file), ends it on time.
    bool WantsStart();
    void Start(const std::string& header);
    void PollFinish();

    // One GetFractionVisible call. pos may be null (layout check failed).
    void Glow(int eye, int viewId, uintptr_t handle, const float* pos, float fadeInv, float raw, float shown,
        float pitch, float yaw, float roll);
    // 'P' = game Present, 'U' = frame published to the headset (a = frame id).
    void Event(char kind, uint32_t a);
}
