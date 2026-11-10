#include "bmvr_glowtrace.h"
#include "bmvr_flags.h"
#include "game.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

namespace bmvr::glowtrace
{
    std::atomic<bool> g_Active{ false };

    namespace
    {
        struct Row
        {
            long long qpc;
            char kind;          // 'G' glow, 'P' present, 'U' published
            unsigned char eye;
            unsigned short viewId;
            uint32_t thread;
            uintptr_t handle;   // glow handle, or frame id for 'U'
            float pos[3];
            float fadeInv, raw, shown;
            float pitch, yaw, roll;
            bool havePos;
        };

        constexpr size_t kMaxRows = 600'000;  // rows are ~64 B: ~38 MB cap in a 32-bit process
        std::mutex g_Mutex;
        std::vector<Row> g_Rows;
        std::string g_Header;
        long long g_StartQpc = 0;
        long long g_EndQpc = 0;
        double g_QpcPerMs = 0.0;
        bool g_Overflow = false;

        long long Qpc()
        {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            return q.QuadPart;
        }

        void Push(const Row& r)
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (!g_Active.load(std::memory_order_relaxed))
                return;
            if (g_Rows.size() >= kMaxRows)
            {
                g_Overflow = true;
                return;
            }
            g_Rows.push_back(r);
        }

        // Per glow (handle, eye, view): how often the shown value turns from
        // rising to falling or back by more than kHyst, i.e. pulses.
        void Summarise(const std::vector<Row>& rows, double qpcPerMs, long long startQpc, const char* file)
        {
            constexpr float kHyst = 0.03f;
            struct Acc
            {
                int n = 0;
                double sum = 0;
                float mn = 1e9f, mx = -1e9f, rawMn = 1e9f, rawMx = -1e9f;
                float ext = 0;      // last extreme
                int dir = 0;        // +1 rising, -1 falling
                int turns = 0;
                long long firstQpc = 0, lastQpc = 0;
                float pos[3]{};
                bool havePos = false;
            };
            std::map<std::tuple<uintptr_t, int, int>, Acc> glows;
            int presents = 0, published = 0;
            for (const Row& r : rows)
            {
                if (r.kind == 'P') { ++presents; continue; }
                if (r.kind == 'U') { ++published; continue; }
                Acc& a = glows[{ r.handle, r.eye, r.viewId }];
                if (a.n == 0)
                {
                    a.ext = r.shown;
                    a.firstQpc = r.qpc;
                    if (r.havePos)
                    {
                        std::copy(r.pos, r.pos + 3, a.pos);
                        a.havePos = true;
                    }
                }
                ++a.n;
                a.sum += r.shown;
                a.mn = (std::min)(a.mn, r.shown);
                a.mx = (std::max)(a.mx, r.shown);
                a.rawMn = (std::min)(a.rawMn, r.raw);
                a.rawMx = (std::max)(a.rawMx, r.raw);
                a.lastQpc = r.qpc;
                if (a.dir == 0)
                {
                    if (r.shown > a.ext + kHyst) { a.dir = 1; a.ext = r.shown; }
                    else if (r.shown < a.ext - kHyst) { a.dir = -1; a.ext = r.shown; }
                }
                else if (a.dir > 0)
                {
                    if (r.shown > a.ext) a.ext = r.shown;
                    else if (r.shown < a.ext - kHyst) { ++a.turns; a.dir = -1; a.ext = r.shown; }
                }
                else
                {
                    if (r.shown < a.ext) a.ext = r.shown;
                    else if (r.shown > a.ext + kHyst) { ++a.turns; a.dir = 1; a.ext = r.shown; }
                }
            }
            const double durMs = rows.empty() ? 0.0 : (rows.back().qpc - startQpc) / qpcPerMs;
            Game::logMsg("glow trace %s: %.1f s, %zu rows, %d glow keys, game presents %.1f/s, published %.1f/s",
                file, durMs / 1000.0, rows.size(), static_cast<int>(glows.size()),
                durMs > 0 ? presents * 1000.0 / durMs : 0.0, durMs > 0 ? published * 1000.0 / durMs : 0.0);
            struct Out { float swing; std::string line; };
            std::vector<Out> out;
            for (const auto& [k, a] : glows)
            {
                const double mean = a.sum / a.n;
                if (mean < 0.02 || a.n < 10)
                    continue;
                const double span = (a.lastQpc - a.firstQpc) / qpcPerMs / 1000.0;
                char line[320];
                std::snprintf(line, sizeof(line),
                    "glow trace pulse: handle %08X eye %d view 0x%X pos %s(%.0f %.0f %.0f) samples %d (%.0f/s) shown mean %.2f min %.2f max %.2f raw min %.2f max %.2f turns %d (%.2f pulses/s)",
                    static_cast<unsigned>(std::get<0>(k)), std::get<1>(k), std::get<2>(k), a.havePos ? "" : "? ",
                    a.pos[0], a.pos[1], a.pos[2], a.n, span > 0 ? a.n / span : 0.0, mean, a.mn, a.mx, a.rawMn, a.rawMx,
                    a.turns, span > 0 ? a.turns / 2.0 / span : 0.0);
                out.push_back({ a.mx - a.mn, line });
            }
            std::sort(out.begin(), out.end(), [](const Out& a, const Out& b) { return a.swing > b.swing; });
            for (size_t i = 0; i < out.size() && i < 12; ++i)
                Game::logMsg("%s", out[i].line.c_str());
            if (out.empty())
                Game::logMsg("glow trace pulse: no lit glow in view");
        }

        void WriteFile(std::vector<Row> rows, std::string header, double qpcPerMs, long long startQpc, bool overflow)
        {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            char leaf[64];
            std::snprintf(leaf, sizeof(leaf), "glowtrace-%04u%02u%02u-%02u%02u%02u.csv", st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
            const std::string path = VrFilePath(leaf);
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f)
            {
                Game::logMsg("glow trace: cannot write %s", path.c_str());
                return;
            }
            std::fprintf(f, "# %s%s\n", header.c_str(), overflow ? " (row cap reached, trace cut short)" : "");
            std::fprintf(f, "# kind G: glow sample, P: game Present, U: frame published to the headset (handle = frame id)\n");
            std::fprintf(f, "t_ms,kind,thread,eye,view,handle,x,y,z,fade_inv,raw,shown,pitch,yaw,roll\n");
            for (const Row& r : rows)
            {
                const double t = (r.qpc - startQpc) / qpcPerMs;
                if (r.kind == 'G')
                    std::fprintf(f, "%.3f,G,%u,%u,0x%X,%08X,%.1f,%.1f,%.1f,%.3f,%.4f,%.4f,%.1f,%.1f,%.1f\n", t, r.thread,
                        r.eye, r.viewId, static_cast<unsigned>(r.handle), r.pos[0], r.pos[1], r.pos[2], r.fadeInv, r.raw,
                        r.shown, r.pitch, r.yaw, r.roll);
                else
                    std::fprintf(f, "%.3f,%c,%u,,,%u,,,,,,,,,\n", t, r.kind, r.thread, static_cast<unsigned>(r.handle));
            }
            std::fclose(f);
            Summarise(rows, qpcPerMs, startQpc, leaf);
        }
    }

    bool WantsStart()
    {
        return g_GlowTraceSeconds > 0.f && !Active();
    }

    void Start(const std::string& header)
    {
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        const float secs = (std::min)(g_GlowTraceSeconds, 120.f);
        g_GlowTraceSeconds = 0.f;   // one shot; the cvar mirrors back to 0
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_QpcPerMs = fq.QuadPart / 1000.0;
        g_StartQpc = Qpc();
        g_EndQpc = g_StartQpc + static_cast<long long>(secs * 1000.0 * g_QpcPerMs);
        g_Header = header;
        g_Rows.clear();
        g_Rows.reserve(200000);
        g_Overflow = false;
        g_Active.store(true, std::memory_order_relaxed);
        Game::logMsg("glow trace: recording %.0f s (%s)", secs, header.c_str());
    }

    void PollFinish()
    {
        if (!Active() || Qpc() < g_EndQpc)
            return;
        std::vector<Row> rows;
        std::string header;
        bool overflow;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (!g_Active.load(std::memory_order_relaxed))
                return;
            g_Active.store(false, std::memory_order_relaxed);
            rows.swap(g_Rows);
            header = g_Header;
            overflow = g_Overflow;
        }
        // Writing a few MB would hitch the present thread.
        std::thread(WriteFile, std::move(rows), std::move(header), g_QpcPerMs, g_StartQpc, overflow).detach();
    }

    void Glow(int eye, int viewId, uintptr_t handle, const float* pos, float fadeInv, float raw, float shown,
        float pitch, float yaw, float roll)
    {
        Row r{};
        r.qpc = Qpc();
        r.kind = 'G';
        r.eye = static_cast<unsigned char>(eye);
        r.viewId = static_cast<unsigned short>(viewId);
        r.thread = GetCurrentThreadId();
        r.handle = handle;
        if (pos)
        {
            std::copy(pos, pos + 3, r.pos);
            r.havePos = true;
        }
        r.fadeInv = fadeInv;
        r.raw = raw;
        r.shown = shown;
        r.pitch = pitch;
        r.yaw = yaw;
        r.roll = roll;
        Push(r);
    }

    void Event(char kind, uint32_t a)
    {
        Row r{};
        r.qpc = Qpc();
        r.kind = kind;
        r.thread = GetCurrentThreadId();
        r.handle = a;
        Push(r);
    }
}
