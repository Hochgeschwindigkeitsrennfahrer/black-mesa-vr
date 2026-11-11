// PerfLog wait profiler: where the main and render threads block, including
// waits inside the engine where the mod has no hooks.
//
// Hooks the Win32 wait entry points in kernelbase (WaitForSingleObject(Ex),
// WaitForMultipleObjects(Ex), Sleep(Ex), SwitchToThread, SleepConditionVariable
// SRW/CS) and times each outermost call on the two tracked threads. The first
// caller is exact (_ReturnAddress); two more come from a stack scan above our
// frame (values that point just after a call into a non-system module), so
// they can occasionally be stale. Every PerfLog report prints the top
// (api, caller chain) rows per thread as module+RVA, to look up in Ghidra.
//
// (The first version suspended threads ~1 kHz to sample them; that paused
// lock holders and invented contention, and its stack chains were stale.)
#include "bmvr_sampler.h"
#include "bmvr_prof.h"
#include "game.h"
#include "MinHook.h"
#include <Windows.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace bmvr::sampler
{
    namespace
    {
        struct Module
        {
            uintptr_t base, end;
            char name[32];
            bool system;
        };
        Module g_Mods[256];
        int g_ModCount = 0;

        enum { kMain = 0, kRender = 1, kThreads = 2 };
        std::atomic<DWORD> g_Tid[kThreads]{};
        std::atomic<bool> g_Started{ false };
        std::atomic<bool> g_Ready{ false };

        enum Api { aWFSO, aWFSOEx, aWFMO, aWFMOEx, aSleep, aSleepEx, aSwitch, aCondSRW, aCondCS, aCount };
        const char* kApi[aCount] = { "WaitForSingleObject", "WaitForSingleObjectEx", "WaitForMultipleObjects",
            "WaitForMultipleObjectsEx", "Sleep", "SleepEx", "SwitchToThread", "SleepConditionVariableSRW",
            "SleepConditionVariableCS" };

        constexpr int kDepth = 6;
        struct Key
        {
            uint16_t mod[kDepth];
            uint32_t rva[kDepth];
            int16_t outer, inner;  // active BMVR_PROF sites (exact)
            uint8_t thread, api;
            bool operator==(const Key& o) const
            {
                if (thread != o.thread || api != o.api || outer != o.outer || inner != o.inner)
                    return false;
                for (int i = 0; i < kDepth; ++i)
                    if (mod[i] != o.mod[i] || rva[i] != o.rva[i])
                        return false;
                return true;
            }
        };
        struct Entry
        {
            Key key;
            long long ticks, maxTicks;
            uint32_t count;
            bool used;
        };
        constexpr int kTable = 4096;
        Entry g_Table[kTable];
        long long g_Total[kThreads];
        std::mutex g_Lock;
        thread_local int t_Depth = 0;
        long long g_Freq = 1;

        int FindModule(uintptr_t a)
        {
            for (int i = 0; i < g_ModCount; ++i)
                if (a >= g_Mods[i].base && a < g_Mods[i].end)
                    return i;
            return -1;
        }

        bool IsSystemName(const char* n)
        {
            static const char* kSys[] = { "ntdll", "kernel32", "kernelbase", "win32u", "msvcrt", "ucrtbase" };
            for (const char* s : kSys)
                if (_strnicmp(n, s, std::strlen(s)) == 0)
                    return true;
            return false;
        }

        void LoadModules()
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
            if (snap == INVALID_HANDLE_VALUE)
                return;
            MODULEENTRY32 me{};
            me.dwSize = sizeof(me);
            g_ModCount = 0;
            for (BOOL ok = Module32First(snap, &me); ok && g_ModCount < 256; ok = Module32Next(snap, &me))
            {
                Module& m = g_Mods[g_ModCount++];
                m.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                m.end = m.base + me.modBaseSize;
                strncpy_s(m.name, me.szModule, _TRUNCATE);
                char* dot = std::strrchr(m.name, '.');
                if (dot)
                    *dot = 0;
                m.system = IsSystemName(m.name);
            }
            CloseHandle(snap);
        }

        bool LooksLikeReturn(uintptr_t a, const Module& m)
        {
            if (a < m.base + 0x1000 + 8 || a >= m.end)
                return false;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
            if (p[-5] == 0xE8)
                return true;
            if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2)
                return true;
            if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2)
                return true;
            if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2)
                return true;
            if (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2)
                return true;
            return false;
        }

        int Tracked()
        {
            if (!g_Ready.load(std::memory_order_relaxed) || !::bmvr::g_PerfLog)
                return -1;
            const DWORD me = GetCurrentThreadId();
            if (me == g_Tid[kMain].load(std::memory_order_relaxed))
                return kMain;
            if (me == g_Tid[kRender].load(std::memory_order_relaxed))
                return kRender;
            return -1;
        }

        // Records one wait. ret = our hook's return address (the real caller),
        // frame = address of that return address (stack scan starts above it).
        void Record(int t, int api, long long ticks, uintptr_t ret, const uintptr_t* frame)
        {
            Key k{};
            k.thread = static_cast<uint8_t>(t);
            k.api = static_cast<uint8_t>(api);
            for (int i = 0; i < kDepth; ++i)
                k.mod[i] = 0xFFFF;
            {
                int o = -1, in = -1;
                ::bmvr::prof::CurrentSites(o, in);
                k.outer = static_cast<int16_t>(o);
                k.inner = static_cast<int16_t>(in);
            }
            int found = 0;
            const int m0 = FindModule(ret);
            if (m0 >= 0)
            {
                k.mod[0] = static_cast<uint16_t>(m0);
                k.rva[0] = static_cast<uint32_t>(ret - g_Mods[m0].base);
                found = 1;
            }
            // NT_TIB.StackBase of this thread bounds the scan.
            const uintptr_t stackBase = reinterpret_cast<const uintptr_t*>(__readfsdword(0x18))[1];
            const uintptr_t* p = frame + 1;
            for (int n = 0; found < kDepth && n < 2048 && reinterpret_cast<uintptr_t>(p + 1) <= stackBase; ++p, ++n)
            {
                const uintptr_t v = *p;
                const int m = FindModule(v);
                if (m < 0 || g_Mods[m].system || !LooksLikeReturn(v, g_Mods[m]))
                    continue;
                k.mod[found] = static_cast<uint16_t>(m);
                k.rva[found] = static_cast<uint32_t>(v - g_Mods[m].base);
                ++found;
            }
            std::lock_guard<std::mutex> lock(g_Lock);
            g_Total[t] += ticks;
            uint32_t h = (k.api << 3) ^ k.thread ^ (static_cast<uint32_t>(k.outer) << 8) ^ (static_cast<uint32_t>(k.inner) << 20);
            for (int i = 0; i < kDepth; ++i)
                h = h * 2654435761u ^ k.rva[i];
            for (int i = 0; i < kTable; ++i)
            {
                Entry& e = g_Table[(h + i) & (kTable - 1)];
                if (!e.used)
                {
                    e.used = true;
                    e.key = k;
                    e.ticks = e.maxTicks = ticks;
                    e.count = 1;
                    return;
                }
                if (e.key == k)
                {
                    e.ticks += ticks;
                    e.maxTicks = (std::max)(e.maxTicks, ticks);
                    ++e.count;
                    return;
                }
            }
        }

        // Wraps one hooked call: only the outermost wait on a tracked thread
        // is timed (kernelbase calls its own Ex variants internally).
        struct WaitScope
        {
            int t = -1, api;
            long long t0 = 0;
            uintptr_t ret;
            const uintptr_t* frame;
            WaitScope(int a, void* r, void* f) : api(a), ret(reinterpret_cast<uintptr_t>(r)),
                frame(reinterpret_cast<const uintptr_t*>(f))
            {
                if (t_Depth++ != 0)
                    return;
                t = Tracked();
                if (t >= 0)
                    t0 = ::bmvr::prof::Now();
            }
            ~WaitScope()
            {
                --t_Depth;
                if (t >= 0)
                {
                    ++t_Depth;  // Record may take a lock: do not record waits inside it
                    Record(t, api, ::bmvr::prof::Now() - t0, ret, frame);
                    --t_Depth;
                }
            }
        };

        typedef DWORD(WINAPI* tWFSO)(HANDLE, DWORD);
        typedef DWORD(WINAPI* tWFSOEx)(HANDLE, DWORD, BOOL);
        typedef DWORD(WINAPI* tWFMO)(DWORD, const HANDLE*, BOOL, DWORD);
        typedef DWORD(WINAPI* tWFMOEx)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
        typedef void(WINAPI* tSleep)(DWORD);
        typedef DWORD(WINAPI* tSleepEx)(DWORD, BOOL);
        typedef BOOL(WINAPI* tSwitch)();
        typedef BOOL(WINAPI* tCondSRW)(PCONDITION_VARIABLE, PSRWLOCK, DWORD, ULONG);
        typedef BOOL(WINAPI* tCondCS)(PCONDITION_VARIABLE, PCRITICAL_SECTION, DWORD);
        tWFSO oWFSO; tWFSOEx oWFSOEx; tWFMO oWFMO; tWFMOEx oWFMOEx; tSleep oSleep; tSleepEx oSleepEx;
        tSwitch oSwitch; tCondSRW oCondSRW; tCondCS oCondCS;

#define BMVR_WAIT_SCOPE(api) WaitScope ws_(api, _ReturnAddress(), _AddressOfReturnAddress())
        DWORD WINAPI hWFSO(HANDLE h, DWORD ms) { BMVR_WAIT_SCOPE(aWFSO); return oWFSO(h, ms); }
        DWORD WINAPI hWFSOEx(HANDLE h, DWORD ms, BOOL a) { BMVR_WAIT_SCOPE(aWFSOEx); return oWFSOEx(h, ms, a); }
        DWORD WINAPI hWFMO(DWORD n, const HANDLE* h, BOOL all, DWORD ms) { BMVR_WAIT_SCOPE(aWFMO); return oWFMO(n, h, all, ms); }
        DWORD WINAPI hWFMOEx(DWORD n, const HANDLE* h, BOOL all, DWORD ms, BOOL a) { BMVR_WAIT_SCOPE(aWFMOEx); return oWFMOEx(n, h, all, ms, a); }
        void WINAPI hSleep(DWORD ms) { BMVR_WAIT_SCOPE(aSleep); oSleep(ms); }
        DWORD WINAPI hSleepEx(DWORD ms, BOOL a) { BMVR_WAIT_SCOPE(aSleepEx); return oSleepEx(ms, a); }
        BOOL WINAPI hSwitch() { BMVR_WAIT_SCOPE(aSwitch); return oSwitch(); }
        BOOL WINAPI hCondSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG f) { BMVR_WAIT_SCOPE(aCondSRW); return oCondSRW(c, l, ms, f); }
        BOOL WINAPI hCondCS(PCONDITION_VARIABLE c, PCRITICAL_SECTION l, DWORD ms) { BMVR_WAIT_SCOPE(aCondCS); return oCondCS(c, l, ms); }
#undef BMVR_WAIT_SCOPE

        void HookOne(HMODULE mod, const char* name, LPVOID detour, LPVOID* orig)
        {
            LPVOID target = mod ? reinterpret_cast<LPVOID>(GetProcAddress(mod, name)) : nullptr;
            if (!target)
            {
                Game::logMsg("prof waits: %s not found", name);
                return;
            }
            if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK)
                Game::logMsg("prof waits: hook %s failed", name);
        }

        void InstallHooks()
        {
            HMODULE kb = GetModuleHandleA("kernelbase.dll");
            if (!kb)
                kb = GetModuleHandleA("kernel32.dll");
            HookOne(kb, "WaitForSingleObject", reinterpret_cast<LPVOID>(&hWFSO), reinterpret_cast<LPVOID*>(&oWFSO));
            HookOne(kb, "WaitForSingleObjectEx", reinterpret_cast<LPVOID>(&hWFSOEx), reinterpret_cast<LPVOID*>(&oWFSOEx));
            HookOne(kb, "WaitForMultipleObjects", reinterpret_cast<LPVOID>(&hWFMO), reinterpret_cast<LPVOID*>(&oWFMO));
            HookOne(kb, "WaitForMultipleObjectsEx", reinterpret_cast<LPVOID>(&hWFMOEx), reinterpret_cast<LPVOID*>(&oWFMOEx));
            HookOne(kb, "Sleep", reinterpret_cast<LPVOID>(&hSleep), reinterpret_cast<LPVOID*>(&oSleep));
            HookOne(kb, "SleepEx", reinterpret_cast<LPVOID>(&hSleepEx), reinterpret_cast<LPVOID*>(&oSleepEx));
            HookOne(kb, "SwitchToThread", reinterpret_cast<LPVOID>(&hSwitch), reinterpret_cast<LPVOID*>(&oSwitch));
            HookOne(kb, "SleepConditionVariableSRW", reinterpret_cast<LPVOID>(&hCondSRW), reinterpret_cast<LPVOID*>(&oCondSRW));
            HookOne(kb, "SleepConditionVariableCS", reinterpret_cast<LPVOID>(&hCondCS), reinterpret_cast<LPVOID*>(&oCondCS));
        }

        void Fmt(char* out, size_t n, uint16_t mod, uint32_t rva)
        {
            if (mod == 0xFFFF)
                _snprintf_s(out, n, _TRUNCATE, "-");
            else
                _snprintf_s(out, n, _TRUNCATE, "%s+0x%X", g_Mods[mod].name, rva);
        }
    }

    void NoteMainThread()
    {
        g_Tid[kMain].store(GetCurrentThreadId(), std::memory_order_relaxed);
        bool expected = false;
        if (g_Started.compare_exchange_strong(expected, true))
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_Freq = f.QuadPart;
            LoadModules();
            InstallHooks();
            g_Ready.store(true, std::memory_order_release);
            Game::logMsg("prof waits: %d modules, wait hooks on main + render thread", g_ModCount);
        }
    }

    void NoteRenderThread()
    {
        const DWORD me = GetCurrentThreadId();
        if (me != g_Tid[kMain].load(std::memory_order_relaxed))
            g_Tid[kRender].store(me, std::memory_order_relaxed);
    }

    void Report(int frames)
    {
        if (!g_Ready.load())
            return;
        frames = (std::max)(frames, 1);
        std::vector<Entry> rows;
        long long total[kThreads];
        {
            ++t_Depth;
            std::lock_guard<std::mutex> lock(g_Lock);
            for (Entry& e : g_Table)
                if (e.used)
                {
                    rows.push_back(e);
                    e = Entry{};
                }
            for (int i = 0; i < kThreads; ++i)
            {
                total[i] = g_Total[i];
                g_Total[i] = 0;
            }
            --t_Depth;
        }
        auto ms = [](long long t) { return static_cast<double>(t) * 1000.0 / static_cast<double>(g_Freq); };
        static const char* kName[kThreads] = { "main", "render" };
        for (int t = 0; t < kThreads; ++t)
        {
            std::vector<Entry> mine;
            for (const Entry& e : rows)
                if (e.key.thread == t)
                    mine.push_back(e);
            std::sort(mine.begin(), mine.end(), [](const Entry& a, const Entry& b) { return a.ticks > b.ticks; });
            Game::logMsg("prof waits %s (tid %lu): %.2f ms/frame blocked in Win32 waits",
                kName[t], static_cast<unsigned long>(g_Tid[t].load()), ms(total[t]) / frames);
            for (size_t i = 0; i < mine.size() && i < 6; ++i)
            {
                const Entry& e = mine[i];
                char chain[400];
                int len = 0;
                for (int d = 0; d < kDepth && len < static_cast<int>(sizeof(chain)) - 48; ++d)
                {
                    char one[48];
                    Fmt(one, sizeof(one), e.key.mod[d], e.key.rva[d]);
                    len += _snprintf_s(chain + len, sizeof(chain) - len, _TRUNCATE, "%s%s", d ? " < " : "", one);
                }
                Game::logMsg("prof wait %s %-26s %.3f ms/frame, %.2f/frame, avg %.3f max %.2f ms; in [%s / %s]: %s",
                    kName[t], kApi[e.key.api], ms(e.ticks) / frames, static_cast<double>(e.count) / frames,
                    ms(e.ticks) / e.count, ms(e.maxTicks), ::bmvr::prof::SiteName(e.key.outer),
                    ::bmvr::prof::SiteName(e.key.inner), chain);
            }
        }
    }
}
