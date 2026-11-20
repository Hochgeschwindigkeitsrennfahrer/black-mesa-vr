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
#include "bmvr_seh.h"
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
    static std::atomic<int> g_HeadsetHz{ 0 };
    void NoteHeadsetHz(int hz)
    {
        if (hz >= 60 && hz <= 240)
            g_HeadsetHz.store(hz, std::memory_order_relaxed);
    }
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

// ---------------------------------------------------------------------------
// CPU sampler (test aid, cvar bmvr_cpu_sample <seconds>; independent of
// PerfLog): a background thread suspends the main and the render thread
// about every 1 ms, records the instruction pointer and the return addresses
// found in the top 512 bytes of the stack, resumes. Report: leaf module
// share, and per module the top leaf RVAs and top inclusive return RVAs
// (resolve d3d9.dll RVAs with a /Z7 build and llvm-symbolizer).
// Suspending can inflate lock contention, so this answers "where is CPU
// time spent", not "who waits on whom".
// ---------------------------------------------------------------------------
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <thread>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace bmvr::sampler
{
    namespace
    {
        std::atomic<DWORD> g_CpuTid[2]{};
        std::atomic<bool> g_CpuRunning{ false };

        struct CpuAgg
        {
            int samples = 0;
            std::map<int, int> leafMod;                         // module (-1 unknown) -> samples
            std::map<std::pair<int, uint32_t>, int> leaf;       // (module, rva & ~0xF) -> samples
            std::map<std::pair<int, uint32_t>, int> incl;       // (module, return rva) -> samples (once per sample)
            // leaf in a system module / unknown: (leaf rva & ~0xF, first 2 non-system return addresses) -> samples
            std::map<std::tuple<int, uint32_t, int, uint32_t, int, uint32_t>, int> sysCallers;
        };

        // SEH helpers (no C++ objects in these frames).
        size_t CopyStack(uintptr_t esp, uintptr_t* out)
        {
            size_t n = 0;
            BMVR_TRY
            {
                const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(esp);
                for (; n < 128; ++n)
                    out[n] = sp[n];
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return n;
        }

        bool SafeLooksLikeReturn(uintptr_t a, int m)
        {
            bool ret = false;
            BMVR_TRY { ret = LooksLikeReturn(a, g_Mods[m]); }
            __except (EXCEPTION_EXECUTE_HANDLER) { ret = false; }
            return ret;
        }

        void CpuSampleRun(double seconds)
        {
            LoadModules();
            timeBeginPeriod(1);
            CpuAgg agg[2];
            const DWORD tid[2] = { g_CpuTid[0].load(), g_CpuTid[1].load() };
            HANDLE th[2] = {};
            // Render thread only: suspending the main thread while it holds the d3d9
            // device lock made the render thread spin on it (an artifact).
            for (int t = 1; t < 2; ++t)
                if (tid[t])
                    th[t] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid[t]);
            const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
            uintptr_t stack[128];
            while (GetTickCount64() < end)
            {
                for (int t = 0; t < 2; ++t)
                {
                    if (!th[t])
                        continue;
                    if (SuspendThread(th[t]) == static_cast<DWORD>(-1))
                        continue;
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    const bool ok = GetThreadContext(th[t], &ctx) != 0;
                    const size_t n = ok ? CopyStack(ctx.Esp, stack) : 0;
                    ResumeThread(th[t]);
                    if (!ok)
                        continue;
                    CpuAgg& a = agg[t];
                    ++a.samples;
                    const int m = FindModule(ctx.Eip);
                    a.leafMod[m]++;
                    a.leaf[{ m, m >= 0 ? static_cast<uint32_t>(ctx.Eip - g_Mods[m].base) & ~0xFu : 0u }]++;
                    if (m >= 0)
                        a.incl[{ m, static_cast<uint32_t>(ctx.Eip - g_Mods[m].base) }];  // leaf itself not inclusive
                    if (m < 0 || g_Mods[m].system)
                    {
                        int cm[2] = { -1, -1 };
                        uint32_t cr[2] = { 0, 0 };
                        int got = 0;
                        for (size_t i = 0; i < n && i < 48 && got < 2; ++i)
                        {
                            const int rm = FindModule(stack[i]);
                            if (rm < 0 || g_Mods[rm].system || !SafeLooksLikeReturn(stack[i], rm))
                                continue;
                            cm[got] = rm;
                            cr[got] = static_cast<uint32_t>(stack[i] - g_Mods[rm].base);
                            ++got;
                        }
                        a.sysCallers[{ m, m >= 0 ? static_cast<uint32_t>(ctx.Eip - g_Mods[m].base) & ~0xFu : 0u, cm[0], cr[0], cm[1], cr[1] }]++;
                    }
                    std::set<std::pair<int, uint32_t>> seen;
                    int found = 0;
                    for (size_t i = 0; i < n && found < 24; ++i)
                    {
                        const int rm = FindModule(stack[i]);
                        if (rm < 0)
                            continue;
                        if (!SafeLooksLikeReturn(stack[i], rm))
                            continue;
                        ++found;
                        const std::pair<int, uint32_t> k{ rm, static_cast<uint32_t>(stack[i] - g_Mods[rm].base) };
                        if (seen.insert(k).second)
                            a.incl[k]++;
                    }
                }
                Sleep(4);  // ~200 Hz: suspend/resume under Wine is costly; 1 kHz slowed frames ~35 %
            }
            timeEndPeriod(1);
            for (int t = 0; t < 2; ++t)
                if (th[t])
                    CloseHandle(th[t]);
            static const char* kName[2] = { "main", "render" };
            for (int t = 0; t < 2; ++t)
            {
                const CpuAgg& a = agg[t];
                if (!a.samples)
                    continue;
                std::vector<std::pair<int, int>> mods;
                for (auto& kv : a.leafMod)
                    mods.push_back({ kv.second, kv.first });
                std::sort(mods.rbegin(), mods.rend());
                std::string line;
                for (auto& mv : mods)
                {
                    char one[64];
                    _snprintf_s(one, sizeof(one), _TRUNCATE, " %s %.1f%%", mv.second >= 0 ? g_Mods[mv.second].name : "?",
                        100.0 * mv.first / a.samples);
                    line += one;
                }
                Game::logMsg("cpu sample %s (tid %lu): %d samples over %.1f s; leaf module:%s",
                    kName[t], static_cast<unsigned long>(tid[t]), a.samples, seconds, line.c_str());
                std::vector<std::pair<int, std::pair<int, uint32_t>>> rows;
                for (auto& kv : a.leaf)
                    rows.push_back({ kv.second, kv.first });
                std::sort(rows.rbegin(), rows.rend());
                for (size_t i = 0; i < rows.size() && i < 40; ++i)
                    Game::logMsg("cpu sample %s leaf %5.2f%% %s+0x%X", kName[t], 100.0 * rows[i].first / a.samples,
                        rows[i].second.first >= 0 ? g_Mods[rows[i].second.first].name : "?", rows[i].second.second);
                {
                    std::vector<std::pair<int, std::tuple<int, uint32_t, int, uint32_t, int, uint32_t>>> sc;
                    for (auto& kv : a.sysCallers)
                        sc.push_back({ kv.second, kv.first });
                    std::sort(sc.rbegin(), sc.rend());
                    for (size_t i = 0; i < sc.size() && i < 40; ++i)
                    {
                        const auto& k = sc[i].second;
                        auto nm = [](int mm) { return mm >= 0 ? g_Mods[mm].name : "?"; };
                        Game::logMsg("cpu sample %s syscaller %5.2f%% %s+0x%X <- %s+0x%X <- %s+0x%X", kName[t],
                            100.0 * sc[i].first / a.samples, nm(std::get<0>(k)), std::get<1>(k), nm(std::get<2>(k)), std::get<3>(k),
                            nm(std::get<4>(k)), std::get<5>(k));
                    }
                }
                rows.clear();
                for (auto& kv : a.incl)
                    if (kv.second > 0)
                        rows.push_back({ kv.second, kv.first });
                std::sort(rows.rbegin(), rows.rend());
                for (size_t i = 0; i < rows.size() && i < 80; ++i)
                    Game::logMsg("cpu sample %s incl %5.2f%% %s+0x%X", kName[t], 100.0 * rows[i].first / a.samples,
                        g_Mods[rows[i].second.first].name, rows[i].second.second);
            }
        }
    }

    void CpuNote(int which)
    {
        if (which == 0 || which == 1)
            g_CpuTid[which].store(GetCurrentThreadId(), std::memory_order_relaxed);
    }

    bool CpuSampleStart(double seconds)
    {
        bool expected = false;
        if (seconds <= 0 || !g_CpuRunning.compare_exchange_strong(expected, true))
            return false;
        std::thread([seconds]() {
            CpuSampleRun(seconds);
            g_CpuRunning.store(false);
        }).detach();
        return true;
    }
}

// ---------------------------------------------------------------------------
// Frame budget line (always on, every 10 s): frame interval and CPU ms per
// frame of the main (RenderView) thread, the render (Present) thread and all
// other threads together, from GetThreadTimes (no PerfLog, no hooks). With
// GpuPassTiming on, the "GPU pass timing" lines give the GPU side.
// ---------------------------------------------------------------------------
namespace bmvr::sampler
{
    namespace
    {
        unsigned long long ThreadCpu100ns(DWORD tid)
        {
            HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (!th)
                return 0;
            FILETIME c, e, k, u;
            unsigned long long t = 0;
            if (GetThreadTimes(th, &c, &e, &k, &u))
                t = ((static_cast<unsigned long long>(k.dwHighDateTime) << 32) | k.dwLowDateTime)
                    + ((static_cast<unsigned long long>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
            CloseHandle(th);
            return t;
        }

        unsigned long long ProcessCpu100ns()
        {
            FILETIME c, e, k, u;
            if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
                return 0;
            return ((static_cast<unsigned long long>(k.dwHighDateTime) << 32) | k.dwLowDateTime)
                + ((static_cast<unsigned long long>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
        }
    }

    namespace
    {
        double g_PaceWaitMs = 0.0;  // render thread only
        std::atomic<long long> g_PoseWaitUs{ 0 };  // main thread, read on the render thread
    }

    void NoteMainPoseWait(double ms)
    {
        g_PoseWaitUs.fetch_add(static_cast<long long>(ms * 1000.0), std::memory_order_relaxed);
    }

    void NoteRenderPaceWait(double ms)
    {
        g_PaceWaitMs += ms;
    }

    void BudgetTick()
    {
        static ULONGLONG s_startMs = 0;
        static int s_frames = 0;
        static unsigned long long s_main = 0, s_render = 0, s_proc = 0;
        static DWORD s_mainTid = 0, s_renderTid = 0;
        const ULONGLONG now = GetTickCount64();
        ++s_frames;
        // Frame-time distribution (what a headset feels: frames over the refresh interval get
        // reprojected). One QPC read per frame; up to 4096 intervals per 10 s window.
        static LARGE_INTEGER s_qpf{}, s_lastQpc{};
        static float s_intervals[4096];
        static int s_nIntervals = 0;
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        if (s_qpf.QuadPart == 0)
            QueryPerformanceFrequency(&s_qpf);
        if (s_lastQpc.QuadPart != 0 && s_nIntervals < 4096)
            s_intervals[s_nIntervals++] = static_cast<float>((q.QuadPart - s_lastQpc.QuadPart) * 1000.0 / s_qpf.QuadPart);
        s_lastQpc = q;
        if (s_startMs != 0 && now - s_startMs < 10000)
            return;
        const DWORD mainTid = g_CpuTid[0].load(std::memory_order_relaxed);
        const DWORD renderTid = GetCurrentThreadId();
        const unsigned long long m = mainTid ? ThreadCpu100ns(mainTid) : 0;
        const unsigned long long r = ThreadCpu100ns(renderTid);
        const unsigned long long p = ProcessCpu100ns();
        if (s_startMs != 0 && s_frames > 1 && mainTid == s_mainTid && renderTid == s_renderTid)
        {
            const double f = static_cast<double>(s_frames);
            const double wall = static_cast<double>(now - s_startMs) / f;
            const double mm = (m - s_main) / 10000.0 / f;
            const double rr = (r - s_render) / 10000.0 / f;
            const double pp = (p - s_proc) / 10000.0 / f;
            const bool same = mainTid == renderTid;
            const double pace = g_PaceWaitMs / f;
            const double pose = g_PoseWaitUs.exchange(0, std::memory_order_relaxed) / 1000.0 / f;
            // render: CPU = work + GPU polling; idle = blocked (waiting for the main thread's
            // commands, Present, ...). main: idle = blocked (pose wait shown, the rest is mostly
            // the material queue = waiting for the render thread).
            Game::logMsg("Frame budget (%.0f s): %d frames, %.2f ms/frame (%.0f fps) | render thread: work %.2f (%.0f %%), polling GPU %.2f, idle %.2f | main thread: CPU %.2f, pose wait %.2f, other idle %.2f%s | other threads CPU %.2f",
                (now - s_startMs) / 1000.0, s_frames, wall, 1000.0 / wall, rr - pace, 100.0 * (rr - pace) / wall, pace,
                (std::max)(0.0, wall - rr), same ? 0.0 : mm, pose, same ? 0.0 : (std::max)(0.0, wall - mm - pose),
                same ? " (same thread as render)" : "", pp - rr - (same ? 0.0 : mm));
            if (s_nIntervals >= 20)
            {
                // Ignore hitches longer than 250 ms (loads, alt-tab) for the percentiles' sake.
                // On time = within the headset's frame plus ~4 % slack (11.6 ms
                // at 90 Hz, as before); it was fixed at 90 Hz, so a 72 Hz Quest
                // read as ~10 % on time while locked at its refresh.
                const int hz = g_HeadsetHz.load(std::memory_order_relaxed);
                const int refHz = hz > 0 ? hz : 90;
                const float onTimeMs = 1000.f / refHz * 1.044f;
                int n = 0, fast = 0;
                for (int i = 0; i < s_nIntervals; ++i)
                    if (s_intervals[i] < 250.f)
                    {
                        s_intervals[n++] = s_intervals[i];
                        if (s_intervals[i] <= onTimeMs)
                            ++fast;
                    }
                if (n >= 20)
                {
                    std::sort(s_intervals, s_intervals + n);
                    Game::logMsg("Frame times (%.0f s): %d frames, %.1f %% <= %.1f ms (%d Hz%s), p50 %.2f, p95 %.2f, p99 %.2f, max %.1f ms",
                        (now - s_startMs) / 1000.0, n, 100.0 * fast / n, onTimeMs, refHz, hz > 0 ? " headset" : ", headset rate not known yet",
                        s_intervals[n / 2], s_intervals[n * 95 / 100],
                        s_intervals[n * 99 / 100], s_intervals[n - 1]);
                }
            }
        }
        s_nIntervals = 0;
        s_startMs = now;
        s_frames = 0;
        g_PaceWaitMs = 0.0;
        g_PoseWaitUs.store(0, std::memory_order_relaxed);
        s_main = m;
        s_render = r;
        s_proc = p;
        s_mainTid = mainTid;
        s_renderTid = renderTid;
    }
}
