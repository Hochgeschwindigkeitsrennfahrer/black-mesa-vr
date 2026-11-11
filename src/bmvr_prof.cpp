#include "bmvr_prof.h"
#include "game.h"
#include "hooks.h"
#include "vr.h"
#include <Windows.h>
#include <tlhelp32.h>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>

// DXVK side (dxvk_graphics.cpp): pipeline variants created / optimized
// pipelines compiled since start. Read as deltas per report.
extern "C" volatile long g_BmvrPipelineInstances = 0;
extern "C" volatile long g_BmvrPipelineCompiles = 0;

namespace bmvr::prof
{
    namespace
    {
        constexpr int kMaxSites = 256;
        constexpr int kThreads = 4;  // first three threads seen, then "other"
        constexpr int kEyes = 3;
        constexpr ULONGLONG kReportMs = 5000;

        struct Cell
        {
            std::atomic<long long> ticks{ 0 };
            std::atomic<long long> maxTicks{ 0 };
            std::atomic<int> calls{ 0 };
        };
        struct Slot
        {
            const char* name = nullptr;
            bool counter = false;
            bool gap = false;
            Cell cell[kThreads][kEyes];
        };

        Slot g_Slots[kMaxSites];
        std::atomic<int> g_SlotCount{ 0 };
        std::mutex g_RegMutex;
        std::atomic<DWORD> g_ThreadIds[kThreads - 1]{};
        std::atomic<int> g_ThreadCount{ 0 };
        thread_local int t_Thread = -1;

        // Gap tracker (per thread index): depth of nested d3d9 sites, last
        // top-level exit, the site it was, and the big gaps per site pair.
        thread_local int t_GapDepth = 0;
        thread_local long long t_GapLastExit = 0;
        thread_local int t_GapLastSite = -1;
        struct GapCell
        {
            long long ticks = 0, maxTicks = 0;
            int count = 0;
        };
        std::mutex g_GapMutex;
        std::unordered_map<uint64_t, GapCell> g_Gaps;  // (thread, prev site, next site)
        std::atomic<long long> g_GapSmall[kThreads]{};  // gaps below the threshold
        std::atomic<long long> g_GapBig[kThreads]{};
        std::atomic<long long> g_GapCalls[kThreads]{};

        long long g_Freq = 0;
        // Main thread frame state (only touched on the RenderView thread).
        int g_RvDepth = 0;
        long long g_RvEnter = 0;
        long long g_LastFrameStart = 0;
        long long g_FrameTicks = 0, g_FrameMax = 0, g_RvTicks = 0;
        int g_Frames = 0;
        ULONGLONG g_NextReport = 0;
        long g_LastInstances = 0, g_LastCompiles = 0;

        int ThreadIndex()
        {
            if (t_Thread >= 0)
                return t_Thread;
            const int n = g_ThreadCount.fetch_add(1);
            if (n < kThreads - 1)
            {
                g_ThreadIds[n].store(GetCurrentThreadId());
                t_Thread = n;
            }
            else
                t_Thread = kThreads - 1;
            return t_Thread;
        }

        int Eye()
        {
            const int e = Hooks::m_VR ? Hooks::m_VR->m_StereoEye : 0;
            return (e >= 0 && e < kEyes) ? e : 0;
        }

        double Ms(long long ticks)
        {
            return g_Freq ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_Freq) : 0.0;
        }

        // CPU time per thread of this process since the previous report: busy vs
        // waiting (a thread whose CPU ms/frame is far below the frame interval
        // spends the rest blocked).
        void ReportThreadCpu(int frames, double wallMs)
        {
            static std::unordered_map<DWORD, unsigned long long> s_last;
            std::unordered_map<DWORD, unsigned long long> now;
            struct T { DWORD tid; double ms; };
            std::vector<T> busy;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            const DWORD pid = GetCurrentProcessId();
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid)
                    continue;
                HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!th)
                    continue;
                FILETIME c, e, k, u;
                if (GetThreadTimes(th, &c, &e, &k, &u))
                {
                    const unsigned long long t = ((static_cast<unsigned long long>(k.dwHighDateTime) << 32) | k.dwLowDateTime)
                        + ((static_cast<unsigned long long>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
                    now[te.th32ThreadID] = t;
                    const auto it = s_last.find(te.th32ThreadID);
                    if (it != s_last.end() && t > it->second)
                        busy.push_back(T{ te.th32ThreadID, static_cast<double>(t - it->second) / 10000.0 });
                }
                CloseHandle(th);
            }
            CloseHandle(snap);
            s_last.swap(now);
            if (busy.empty())
                return;
            std::sort(busy.begin(), busy.end(), [](const T& a, const T& b) { return a.ms > b.ms; });
            char line[512];
            int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "prof thread CPU ms/frame (wall %.2f):", wallMs);
            double total = 0;
            for (const T& t : busy)
                total += t.ms;
            for (size_t i = 0; i < busy.size() && i < 10 && len > 0 && len < static_cast<int>(sizeof(line)); ++i)
            {
                const DWORD tid = busy[i].tid;
                const char* tag = tid == g_ThreadIds[0].load() ? "t0" : tid == g_ThreadIds[1].load() ? "t1"
                    : tid == g_ThreadIds[2].load() ? "t2" : "";
                const int w = _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %lu%s%s=%.2f",
                    static_cast<unsigned long>(tid), tag[0] ? "/" : "", tag, busy[i].ms / frames);
                if (w < 0)
                    break;
                len += w;
            }
            Game::logMsg("%s; all threads %.2f", line, total / frames);
        }

        void ReportGaps(int frames)
        {
            for (int t = 0; t < kThreads; ++t)
            {
                const long long smallTicks = g_GapSmall[t].exchange(0);
                const long long bigTicks = g_GapBig[t].exchange(0);
                const long long calls = g_GapCalls[t].exchange(0);
                if (calls > 0)
                    Game::logMsg("prof gaps t%d: outside d3d9 %.2f ms/frame (%.2f in gaps >= 0.25 ms, %.2f in %.0f small gaps/frame)",
                        t, Ms(smallTicks + bigTicks) / frames, Ms(bigTicks) / frames, Ms(smallTicks) / frames, static_cast<double>(calls) / frames);
            }
            std::vector<std::pair<uint64_t, GapCell>> rows;
            {
                std::lock_guard<std::mutex> lock(g_GapMutex);
                rows.assign(g_Gaps.begin(), g_Gaps.end());
                g_Gaps.clear();
            }
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
            for (size_t i = 0; i < rows.size() && i < 16; ++i)
            {
                const uint64_t k = rows[i].first;
                const int t = static_cast<int>(k >> 32);
                const int prev = static_cast<int>((k >> 16) & 0xFFFF);
                const int next = static_cast<int>(k & 0xFFFF);
                const GapCell& c = rows[i].second;
                Game::logMsg("prof gap t%d after %-28s before %-28s: %.3f ms/frame, %.2f/frame, avg %.2f ms, max %.2f ms",
                    t, prev < kMaxSites ? g_Slots[prev].name : "?", next < kMaxSites ? g_Slots[next].name : "?",
                    Ms(c.ticks) / frames, static_cast<double>(c.count) / frames, Ms(c.ticks) / c.count, Ms(c.maxTicks));
            }
        }

        void Report()
        {
            const int frames = (std::max)(g_Frames, 1);
            Game::logMsg("prof frame: %d main-thread frames in %.1f s, interval avg %.2f ms max %.2f, top-level RenderView avg %.2f ms, outside it %.2f ms; "
                "DXVK pipeline variants +%ld, optimized compiles +%ld",
                g_Frames, Ms(g_FrameTicks) / 1000.0, Ms(g_FrameTicks) / frames, Ms(g_FrameMax),
                Ms(g_RvTicks) / frames, Ms(g_FrameTicks - g_RvTicks) / frames,
                g_BmvrPipelineInstances - g_LastInstances, g_BmvrPipelineCompiles - g_LastCompiles);
            g_LastInstances = g_BmvrPipelineInstances;
            g_LastCompiles = g_BmvrPipelineCompiles;
            char tids[96];
            _snprintf_s(tids, sizeof(tids), _TRUNCATE, "prof threads: t0=%lu t1=%lu t2=%lu t3=other",
                static_cast<unsigned long>(g_ThreadIds[0].load()), static_cast<unsigned long>(g_ThreadIds[1].load()),
                static_cast<unsigned long>(g_ThreadIds[2].load()));
            Game::logMsg("%s", tids);
            ReportThreadCpu(frames, Ms(g_FrameTicks) / frames);

            struct Row { const char* name; bool counter; int t, e; long long ticks, maxTicks; int calls; };
            std::vector<Row> rows;
            const int n = g_SlotCount.load();
            for (int i = 0; i < n; ++i)
            {
                Slot& s = g_Slots[i];
                for (int t = 0; t < kThreads; ++t)
                    for (int e = 0; e < kEyes; ++e)
                    {
                        Cell& c = s.cell[t][e];
                        const int calls = c.calls.exchange(0);
                        const long long ticks = c.ticks.exchange(0);
                        const long long mx = c.maxTicks.exchange(0);
                        if (calls > 0)
                            rows.push_back(Row{ s.name, s.counter, t, e, ticks, mx, calls });
                    }
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                if (a.counter != b.counter)
                    return !a.counter;
                return a.counter ? a.calls > b.calls : a.ticks > b.ticks;
            });
            int shown = 0;
            for (const Row& r : rows)
            {
                if (r.counter)
                    Game::logMsg("prof count %-28s t%d e%d: %d (%.2f per frame)", r.name, r.t, r.e, r.calls,
                        static_cast<double>(r.calls) / frames);
                else if (shown++ < 48)
                    Game::logMsg("prof %-34s t%d e%d: %.3f ms/frame, %.1f calls/frame, %.2f us/call, max %.2f ms",
                        r.name, r.t, r.e, Ms(r.ticks) / frames, static_cast<double>(r.calls) / frames,
                        Ms(r.ticks) * 1000.0 / r.calls, Ms(r.maxTicks));
            }
            ReportGaps(frames);
            g_FrameTicks = g_FrameMax = g_RvTicks = 0;
            g_Frames = 0;
        }
    }

    long long Now()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    int Register(Site& site)
    {
        std::lock_guard<std::mutex> lock(g_RegMutex);
        if (site.id >= 0)
            return site.id;
        if (!g_Freq)
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_Freq = f.QuadPart;
        }
        const int n = g_SlotCount.load();
        if (n >= kMaxSites)
            return -1;
        g_Slots[n].name = site.name;
        g_Slots[n].gap = std::strncmp(site.name, "d3d9 ", 5) == 0;
        g_SlotCount.store(n + 1);
        site.id = n;
        return n;
    }

    void Add(int id, long long ticks)
    {
        if (id < 0 || id >= kMaxSites)
            return;
        Cell& c = g_Slots[id].cell[ThreadIndex()][Eye()];
        c.ticks.fetch_add(ticks, std::memory_order_relaxed);
        c.calls.fetch_add(1, std::memory_order_relaxed);
        long long prev = c.maxTicks.load(std::memory_order_relaxed);
        while (ticks > prev && !c.maxTicks.compare_exchange_weak(prev, ticks, std::memory_order_relaxed)) {}
    }

    void Count(int id)
    {
        if (id < 0 || id >= kMaxSites)
            return;
        g_Slots[id].counter = true;
        g_Slots[id].cell[ThreadIndex()][Eye()].calls.fetch_add(1, std::memory_order_relaxed);
    }

    bool IsGapSite(int id)
    {
        return id >= 0 && id < kMaxSites && g_Slots[id].gap;
    }

    void GapEnter(int id, long long now)
    {
        if (t_GapDepth++ != 0 || !t_GapLastExit)
            return;
        const int t = ThreadIndex();
        const long long gap = now - t_GapLastExit;
        g_GapCalls[t].fetch_add(1, std::memory_order_relaxed);
        if (gap < g_Freq / 4000)  // 0.25 ms
        {
            g_GapSmall[t].fetch_add(gap, std::memory_order_relaxed);
            return;
        }
        g_GapBig[t].fetch_add(gap, std::memory_order_relaxed);
        const uint64_t key = (static_cast<uint64_t>(t) << 32) | (static_cast<uint64_t>(t_GapLastSite & 0xFFFF) << 16)
            | static_cast<uint64_t>(id & 0xFFFF);
        std::lock_guard<std::mutex> lock(g_GapMutex);
        GapCell& c = g_Gaps[key];
        c.ticks += gap;
        c.maxTicks = (std::max)(c.maxTicks, gap);
        ++c.count;
    }

    void GapLeave(int id, long long now)
    {
        if (t_GapDepth > 0 && --t_GapDepth == 0)
        {
            t_GapLastExit = now;
            t_GapLastSite = id;
        }
    }

    void FrameRenderViewEnter()
    {
        if (!g_PerfLog)
            return;
        if (g_RvDepth++ != 0)
            return;
        if (!g_Freq)
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_Freq = f.QuadPart;
        }
        const long long now = Now();
        if (g_LastFrameStart)
        {
            const long long dt = now - g_LastFrameStart;
            g_FrameTicks += dt;
            g_FrameMax = (std::max)(g_FrameMax, dt);
            ++g_Frames;
        }
        g_LastFrameStart = now;
        g_RvEnter = now;
        const ULONGLONG ms = GetTickCount64();
        if (!g_NextReport)
            g_NextReport = ms + kReportMs;
        if (ms >= g_NextReport && g_Frames > 0)
        {
            Report();
            g_NextReport = ms + kReportMs;
        }
    }

    void FrameRenderViewLeave()
    {
        if (!g_PerfLog || g_RvDepth <= 0)
            return;
        if (--g_RvDepth == 0 && g_RvEnter)
            g_RvTicks += Now() - g_RvEnter;
    }
}
