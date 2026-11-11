#pragma once
// PerfLog CPU profiler for the mod layer (logging only; PerfLog=true).
//
//   BMVR_PROF("name");        // times the rest of the enclosing scope
//   BMVR_PROF_COUNT("name");  // counts an event (render target created, ...)
//
// Times are inclusive (a hook's scope includes the game's original call) and
// split by the eye being drawn (Hooks::m_VR->m_StereoEye: 0 none, 1 left,
// 2 right) and by thread. Every ~5 s bmvr_log.txt gets one line per
// (name, thread, eye) with calls and ms per frame, plus the main thread's frame
// interval and the time inside / outside the top-level RenderView. Disabled =
// one load and branch per site.
#include <cstdint>

namespace bmvr
{
    extern bool g_PerfLog;
    namespace prof
    {
        struct Site
        {
            const char* name;
            int id;  // -1 until registered
        };
        int Register(Site& site);
        long long Now();
        void Add(int id, long long ticks);
        // Idle gaps between top-level d3d9 calls on a thread (sites named
        // "d3d9 ..."): time the thread spent outside d3d9 = engine CPU work
        // or waiting. Gaps >= 0.25 ms are logged per (previous call, next call).
        void GapEnter(int id, long long now);
        void GapLeave(int id, long long now);
        bool IsGapSite(int id);
        void Count(int id);
        // Main-thread frame boundary: top-level RenderView enter / leave.
        void FrameRenderViewEnter();
        void FrameRenderViewLeave();

        struct Scope
        {
            int id = -1;
            long long t0 = 0;
            explicit Scope(Site& s)
            {
                if (!g_PerfLog)
                    return;
                id = s.id >= 0 ? s.id : Register(s);
                t0 = Now();
                if (id >= 0 && IsGapSite(id))
                    GapEnter(id, t0);
            }
            ~Scope()
            {
                if (id >= 0)
                {
                    const long long t1 = Now();
                    Add(id, t1 - t0);
                    if (IsGapSite(id))
                        GapLeave(id, t1);
                }
            }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;
        };
    }
}

#define BMVR_PROF_CAT2(a, b) a##b
#define BMVR_PROF_CAT(a, b) BMVR_PROF_CAT2(a, b)
#define BMVR_PROF(nameLiteral)                                                              \
    static ::bmvr::prof::Site BMVR_PROF_CAT(s_bmvrProfSite_, __LINE__){ nameLiteral, -1 }; \
    ::bmvr::prof::Scope BMVR_PROF_CAT(bmvrProfScope_, __LINE__)(BMVR_PROF_CAT(s_bmvrProfSite_, __LINE__))
#define BMVR_PROF_COUNT(nameLiteral)                                                          \
    do {                                                                                      \
        static ::bmvr::prof::Site s_bmvrProfCount{ nameLiteral, -1 };                         \
        if (::bmvr::g_PerfLog)                                                                \
            ::bmvr::prof::Count(s_bmvrProfCount.id >= 0 ? s_bmvrProfCount.id                  \
                                                        : ::bmvr::prof::Register(s_bmvrProfCount)); \
    } while (0)
