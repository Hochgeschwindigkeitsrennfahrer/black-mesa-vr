#include "vr.h"
#include "game.h"
#include "sdk.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>

#include <cstring>
#include <cmath>

// One SteamVR user loses all audio on alt-tab and does not get it back.
// Black Mesa's snd_mute_losefocus (default 1) omits DSBCAPS_GLOBALFOCUS, so
// DirectSound goes silent while the window is inactive and is supposed to
// resume on activation. Everyone else already resumes. This check only
// writes when a focus return is still stuck:
//   * the process session was unmuted before the loss and is still muted
//   * the session volume collapsed while the engine volume cvar did not
//   * the default render endpoint changed and the meter is still silent
//   * the engine mix buffer was playing and is now lost or stopped
// A session that already recovered is left alone. snd_mute_losefocus is not
// changed, so tab-out still mutes for everyone.

namespace
{
    constexpr DWORD kRecoverDelayMs = 1500;
    constexpr DWORD kHealthyIntervalMs = 2000;
    constexpr DWORD kRestartGapMs = 60000;
    constexpr DWORD kDsPlaying = 0x00000001;
    constexpr DWORD kDsLost = 0x00000002;
    constexpr DWORD kDsPlayLooping = 0x00000001;
    // engine.dll image 0x10000000, Play(0,0,DSBPLAY_LOOPING) of the mix buffer.
    constexpr uintptr_t kMixPlayInsnRva = 0x5FA73; // beta 26234270 (25844216: 0x5FA43)

    struct ComUse
    {
        bool ok = false;
        bool uninit = false;
        ComUse()
        {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (hr == S_OK || hr == S_FALSE)
            {
                ok = true;
                uninit = true;
            }
            else if (hr == RPC_E_CHANGED_MODE)
                ok = true;
        }
        ~ComUse()
        {
            if (uninit)
                CoUninitialize();
        }
    };

    struct Snap
    {
        bool ok = false;
        bool anySession = false;
        bool anyUnmuted = false;
        float volume = 0.f;
        float peak = 0.f;
        bool peakOk = false;
        wchar_t endpoint[512]{};
        bool endpointOk = false;
        bool dsOk = false;
        bool dsPlaying = false;
        bool dsLost = false;
    };

    struct Loss
    {
        bool valid = false;
        Snap snap{};
        DWORD armedAt = 0;
        bool sessionDone = false;
    };

    struct Crit
    {
        CRITICAL_SECTION cs;
        Crit() { InitializeCriticalSection(&cs); }
    };

    CRITICAL_SECTION& Mu()
    {
        static Crit crit;
        return crit.cs;
    }

    struct Lock
    {
        Lock() { EnterCriticalSection(&Mu()); }
        ~Lock() { LeaveCriticalSection(&Mu()); }
    };

    wchar_t g_stableEndpoint[512]{};
    DWORD g_stableEndpointMs = 0;
    bool g_stableEndpointOk = false;
    Loss g_loss{};
    bool g_recoverPending = false;
    DWORD g_recoverNotBefore = 0;
    DWORD g_lastRestartMs = 0;

    template <typename T>
    void Release(T*& p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    bool Executable(void* fn)
    {
        if (!fn)
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(fn, &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        const DWORD prot = mbi.Protect & 0xFF;
        return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ
            || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    }

    void** MixBufferSlot()
    {
        if (!g_Game || !g_Game->m_BaseEngine)
            return nullptr;
        const auto* insn = reinterpret_cast<const uint8_t*>(g_Game->m_BaseEngine + kMixPlayInsnRva);
        __try
        {
            if (insn[0] != 0xA1)
                return nullptr;
            const uint8_t prefix[] = { 0x83, 0xC4, 0x0C, 0x8B, 0x08, 0x6A, 0x00, 0x6A, 0x00 };
            if (std::memcmp(insn + 5, prefix, sizeof(prefix)) != 0)
                return nullptr;
            const uint8_t play[] = {
                0x6A, 0x01, 0x6A, 0x00, 0x6A, 0x00, 0x8B, 0x08, 0x50, 0xFF, 0x51, 0x30 };
            if (std::memcmp(insn + 34, play, sizeof(play)) != 0)
                return nullptr;
            const auto slot = *reinterpret_cast<const uint32_t*>(insn + 1);
            if (slot < 0x10000u)
                return nullptr;
            return reinterpret_cast<void**>(static_cast<uintptr_t>(slot));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    struct DsView
    {
        bool ok = false;
        DWORD status = 0;
    };

    DsView ReadMixStatus(void** slot)
    {
        DsView v{};
        if (!slot)
            return v;
        __try
        {
            void* buf = *slot;
            if (!buf)
                return v;
            void** vt = *reinterpret_cast<void***>(buf);
            if (!vt || !Executable(vt[9]))
                return v;
            DWORD st = 0;
            using GetStatusFn = HRESULT(__stdcall*)(void*, DWORD*);
            if (FAILED(reinterpret_cast<GetStatusFn>(vt[9])(buf, &st)))
                return v;
            v.ok = true;
            v.status = st;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return v;
    }

    bool RestartMix(void** slot, bool restore)
    {
        if (!slot)
            return false;
        __try
        {
            void* buf = *slot;
            if (!buf)
                return false;
            void** vt = *reinterpret_cast<void***>(buf);
            if (!vt)
                return false;
            if (restore)
            {
                if (!Executable(vt[20]))
                    return false;
                using RestoreFn = HRESULT(__stdcall*)(void*);
                reinterpret_cast<RestoreFn>(vt[20])(buf);
            }
            if (!Executable(vt[12]))
                return false;
            using PlayFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
            return SUCCEEDED(reinterpret_cast<PlayFn>(vt[12])(buf, 0, 0, kDsPlayLooping));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ReadEndpoint(IMMDevice* dev, Snap& snap)
    {
        if (!dev)
            return;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id)) || !id)
            return;
        wcsncpy_s(snap.endpoint, id, _TRUNCATE);
        CoTaskMemFree(id);
        snap.endpointOk = snap.endpoint[0] != L'\0';
    }

    bool Sample(Snap& snap)
    {
        snap = Snap{};
        ComUse com;
        if (!com.ok)
            return false;

        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioSessionManager2* mgr = nullptr;
        IAudioSessionEnumerator* sessions = nullptr;
        const HRESULT created = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
        if (FAILED(created) || !en)
            return false;

        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev)
            ReadEndpoint(dev, snap);

        if (!dev || FAILED(dev->Activate(
                __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&mgr))) || !mgr)
        {
            Release(dev);
            Release(en);
            snap.ok = snap.endpointOk;
            return snap.ok;
        }

        if (FAILED(mgr->GetSessionEnumerator(&sessions)) || !sessions)
        {
            Release(mgr);
            Release(dev);
            Release(en);
            snap.ok = snap.endpointOk;
            return snap.ok;
        }

        int count = 0;
        sessions->GetCount(&count);
        const DWORD pid = GetCurrentProcessId();
        for (int i = 0; i < count; ++i)
        {
            IAudioSessionControl* ctrl = nullptr;
            IAudioSessionControl2* ctrl2 = nullptr;
            ISimpleAudioVolume* vol = nullptr;
            IAudioMeterInformation* meter = nullptr;
            if (FAILED(sessions->GetSession(i, &ctrl)) || !ctrl)
                continue;
            if (FAILED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                    reinterpret_cast<void**>(&ctrl2))) || !ctrl2)
            {
                Release(ctrl);
                continue;
            }
            AudioSessionState state = AudioSessionStateExpired;
            if (FAILED(ctrl2->GetState(&state)) || state == AudioSessionStateExpired)
            {
                Release(ctrl2);
                Release(ctrl);
                continue;
            }
            DWORD sessionPid = 0;
            if (FAILED(ctrl2->GetProcessId(&sessionPid)) || sessionPid != pid)
            {
                Release(ctrl2);
                Release(ctrl);
                continue;
            }
            snap.anySession = true;
            if (SUCCEEDED(ctrl2->QueryInterface(__uuidof(ISimpleAudioVolume),
                    reinterpret_cast<void**>(&vol))) && vol)
            {
                BOOL muted = FALSE;
                float level = 0.f;
                if (SUCCEEDED(vol->GetMute(&muted)) && !muted)
                    snap.anyUnmuted = true;
                if (SUCCEEDED(vol->GetMasterVolume(&level)) && level > snap.volume)
                    snap.volume = level;
            }
            if (SUCCEEDED(ctrl2->QueryInterface(__uuidof(IAudioMeterInformation),
                    reinterpret_cast<void**>(&meter))) && meter)
            {
                float peak = 0.f;
                if (SUCCEEDED(meter->GetPeakValue(&peak)))
                {
                    snap.peakOk = true;
                    if (peak > snap.peak)
                        snap.peak = peak;
                }
            }
            Release(meter);
            Release(vol);
            Release(ctrl2);
            Release(ctrl);
        }

        Release(sessions);
        Release(mgr);
        Release(dev);
        Release(en);
        snap.ok = true;

        if (void** slot = MixBufferSlot())
        {
            const DsView ds = ReadMixStatus(slot);
            snap.dsOk = ds.ok;
            snap.dsPlaying = ds.ok && (ds.status & kDsPlaying) != 0;
            snap.dsLost = ds.ok && (ds.status & kDsLost) != 0;
        }
        return true;
    }

    struct RepairResult
    {
        bool unmuted = false;
        bool volumeRestored = false;
    };

    struct LiveSession
    {
        ISimpleAudioVolume* vol = nullptr;
        bool muted = false;
        float level = 1.f;
    };

    RepairResult RepairSessions(const Snap& before)
    {
        RepairResult result{};
        if (!before.ok || !before.anySession || !before.anyUnmuted)
            return result;

        float engineVolume = 1.f;
        const bool volumeKnown = g_Game && g_Game->GetConVarFloat("volume", engineVolume)
            && engineVolume >= 0.f && engineVolume <= 1.5f;
        const bool engineSilent = volumeKnown && engineVolume <= 0.001f;

        ComUse com;
        if (!com.ok)
            return result;

        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioSessionManager2* mgr = nullptr;
        IAudioSessionEnumerator* sessions = nullptr;
        if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en))) || !en)
            return result;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev)
        {
            Release(en);
            return result;
        }
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&mgr))) || !mgr
            || FAILED(mgr->GetSessionEnumerator(&sessions)) || !sessions)
        {
            Release(mgr);
            Release(dev);
            Release(en);
            return result;
        }

        LiveSession live[8]{};
        int liveCount = 0;
        bool anyUnmuted = false;
        float maxLevel = 0.f;
        int count = 0;
        sessions->GetCount(&count);
        const DWORD pid = GetCurrentProcessId();
        for (int i = 0; i < count && liveCount < 8; ++i)
        {
            IAudioSessionControl* ctrl = nullptr;
            IAudioSessionControl2* ctrl2 = nullptr;
            if (FAILED(sessions->GetSession(i, &ctrl)) || !ctrl)
                continue;
            if (FAILED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                    reinterpret_cast<void**>(&ctrl2))) || !ctrl2)
            {
                Release(ctrl);
                continue;
            }
            DWORD sessionPid = 0;
            if (FAILED(ctrl2->GetProcessId(&sessionPid)) || sessionPid != pid)
            {
                Release(ctrl2);
                Release(ctrl);
                continue;
            }
            ISimpleAudioVolume* vol = nullptr;
            if (FAILED(ctrl2->QueryInterface(__uuidof(ISimpleAudioVolume),
                    reinterpret_cast<void**>(&vol))) || !vol)
            {
                Release(ctrl2);
                Release(ctrl);
                continue;
            }
            Release(ctrl2);
            Release(ctrl);
            BOOL muted = FALSE;
            float level = 1.f;
            vol->GetMute(&muted);
            vol->GetMasterVolume(&level);
            if (!muted)
                anyUnmuted = true;
            if (level > maxLevel)
                maxLevel = level;
            live[liveCount].vol = vol;
            live[liveCount].muted = muted != FALSE;
            live[liveCount].level = level;
            ++liveCount;
        }
        Release(sessions);
        Release(mgr);
        Release(dev);
        Release(en);

        // A session that is already audible is the healthy path. Do not
        // unmute or boost any other session the user may have set aside.
        if (!anyUnmuted)
        {
            for (int i = 0; i < liveCount; ++i)
            {
                if (live[i].muted && SUCCEEDED(live[i].vol->SetMute(FALSE, nullptr)))
                    result.unmuted = true;
            }
        }
        if (!engineSilent && before.volume > 0.2f && maxLevel < 0.02f)
        {
            const float restore = before.volume > 1.f ? 1.f : before.volume;
            for (int i = 0; i < liveCount; ++i)
            {
                if (live[i].level < 0.02f
                    && SUCCEEDED(live[i].vol->SetMasterVolume(restore, nullptr)))
                    result.volumeRestored = true;
            }
        }
        for (int i = 0; i < liveCount; ++i)
            Release(live[i].vol);
        return result;
    }

    bool EndpointChanged(const wchar_t* before, bool beforeOk, const Snap& now)
    {
        if (!beforeOk || !now.endpointOk || !before || !before[0])
            return false;
        return _wcsicmp(before, now.endpoint) != 0;
    }

    bool LoseFocusMuteEnabled()
    {
        int v = 1;
        if (!g_Game || !g_Game->GetConVarInt("snd_mute_losefocus", v))
            return false;
        return v != 0;
    }

    bool ReadDefaultEndpoint(wchar_t* out, size_t outChars)
    {
        if (!out || outChars < 2)
            return false;
        out[0] = L'\0';
        ComUse com;
        if (!com.ok)
            return false;
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en))) || !en)
            return false;
        const HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        Release(en);
        if (FAILED(hr) || !dev)
            return false;
        LPWSTR id = nullptr;
        const HRESULT idHr = dev->GetId(&id);
        Release(dev);
        if (FAILED(idHr) || !id || !id[0])
        {
            CoTaskMemFree(id);
            return false;
        }
        wcsncpy_s(out, outChars, id, _TRUNCATE);
        CoTaskMemFree(id);
        return out[0] != L'\0';
    }
}

void VR::NoteAudioFocusTransition(bool activated)
{
    if (!m_IsVREnabled)
        return;

    if (!activated)
    {
        Snap snap{};
        Sample(snap);
        Lock lock;
        g_loss.snap = snap;
        g_loss.valid = snap.ok || snap.dsOk;
        g_loss.armedAt = 0;
        g_loss.sessionDone = false;
        g_recoverPending = false;
        return;
    }

    Lock lock;
    g_loss.armedAt = GetTickCount();
    g_loss.sessionDone = false;
    g_recoverNotBefore = g_loss.armedAt + kRecoverDelayMs;
    g_recoverPending = g_loss.valid;
}

void VR::TickAudioFocusRecover()
{
    if (!m_IsVREnabled || !m_Game)
        return;

    const DWORD now = GetTickCount();
    bool pending = false;
    {
        Lock lock;
        pending = g_recoverPending;
    }

    if (!pending)
    {
        if (!AppActivated())
            return;
        DWORD lastMs = 0;
        {
            Lock lock;
            lastMs = g_stableEndpointMs;
        }
        if (lastMs != 0 && now - lastMs < kHealthyIntervalMs)
            return;
        wchar_t endpoint[512]{};
        if (!ReadDefaultEndpoint(endpoint, 512))
            return;
        Lock lock;
        wcsncpy_s(g_stableEndpoint, endpoint, _TRUNCATE);
        g_stableEndpointOk = true;
        g_stableEndpointMs = now;
        return;
    }

    if (now < g_recoverNotBefore || !AppActivated())
        return;

    const bool pause = WantPauseWorldOverlay();
    const bool loading = m_GameplayEligible && m_Game->m_EngineClient
        && !m_Game->m_EngineClient->IsInGame();

    Loss loss{};
    {
        Lock lock;
        loss = g_loss;
    }
    if (!loss.valid)
    {
        Lock lock;
        g_recoverPending = false;
        return;
    }

    if (!loss.sessionDone)
    {
        const RepairResult repaired = RepairSessions(loss.snap);
        if (repaired.unmuted)
            Game::logMsg("Audio focus: session still muted after tab-in; unmuted");
        if (repaired.volumeRestored)
            Game::logMsg("Audio focus: session volume collapsed after tab-in; restored %.2f",
                loss.snap.volume);
        Lock lock;
        g_loss.sessionDone = true;
        loss.sessionDone = true;
    }

    // Pause and map load keep the mix stopped on purpose. Recheck once
    // gameplay is drawing again; the status read then is the no-op for
    // anyone whose buffer already resumed.
    if (pause || loading)
        return;

    Snap nowSnap{};
    Sample(nowSnap);

    wchar_t baselineEp[512]{};
    bool baselineOk = false;
    float baselinePeak = 0.f;
    bool baselinePeakOk = false;
    bool wasPlaying = false;
    {
        Lock lock;
        if (g_stableEndpointOk && g_stableEndpointMs != 0 && now - g_stableEndpointMs < 15000)
        {
            wcsncpy_s(baselineEp, g_stableEndpoint, _TRUNCATE);
            baselineOk = true;
        }
        else if (loss.snap.endpointOk)
        {
            wcsncpy_s(baselineEp, loss.snap.endpoint, _TRUNCATE);
            baselineOk = true;
        }
        if (loss.snap.peakOk)
        {
            baselinePeak = loss.snap.peak;
            baselinePeakOk = true;
        }
        wasPlaying = loss.snap.dsPlaying;
        g_recoverPending = false;
    }

    const bool meterSilent = nowSnap.peakOk && nowSnap.peak < 0.001f;
    const bool wasLoud = baselinePeakOk && baselinePeak >= 0.08f;
    if (EndpointChanged(baselineEp, baselineOk, nowSnap) && wasLoud && meterSilent
        && (g_lastRestartMs == 0 || now - g_lastRestartMs >= kRestartGapMs))
    {
        const bool ok = m_Game->ClientCmd_Unrestricted("snd_restart")
            || m_Game->ClientCmd("snd_restart");
        if (ok)
        {
            g_lastRestartMs = now;
            Game::logMsg("Audio focus: output device changed and sound stayed silent; snd_restart");
            return;
        }
    }

    if (!wasPlaying || !LoseFocusMuteEnabled())
        return;
    void** slot = MixBufferSlot();
    const DsView ds = ReadMixStatus(slot);
    if (!ds.ok)
        return;
    const bool playing = (ds.status & kDsPlaying) != 0;
    const bool lost = (ds.status & kDsLost) != 0;
    if (playing && !lost)
        return;
    if (RestartMix(slot, lost))
        Game::logMsg("Audio focus: mix buffer %s after tab-in; restarted playback",
            lost ? "lost" : "stopped");
}
