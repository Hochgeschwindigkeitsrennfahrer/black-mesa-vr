#include "vr_controller_models.h"

#include "game.h"
#include "openxr_bridge_protocol.h"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace
{
    using bmvr::ctlmodel::kPartCount;
    using bmvr::ctlmodel::Part;

    // ---- Minimal JSON (render model .json and openvrpaths.vrpath) ----
    struct JVal
    {
        enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
        double n = 0;
        bool b = false;
        std::string s;
        std::vector<JVal> a;
        std::vector<std::pair<std::string, JVal>> o;
        const JVal* get(const char* key) const
        {
            if (t != Obj)
                return nullptr;
            for (const auto& kv : o)
                if (kv.first == key)
                    return &kv.second;
            return nullptr;
        }
    };

    struct JParser
    {
        const char* p;
        const char* e;
        void ws()
        {
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                ++p;
        }
        bool str(std::string& out)
        {
            if (p >= e || *p != '"')
                return false;
            ++p;
            out.clear();
            while (p < e && *p != '"')
            {
                if (*p == '\\' && p + 1 < e)
                {
                    ++p;
                    switch (*p)
                    {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'u': out += '?'; p = (std::min)(p + 4, e - 1); break;
                    default: out += *p; break;
                    }
                    ++p;
                    continue;
                }
                out += *p++;
            }
            if (p >= e)
                return false;
            ++p;
            return true;
        }
        bool val(JVal& v, int depth = 0)
        {
            if (depth > 32)
                return false;
            ws();
            if (p >= e)
                return false;
            if (*p == '{')
            {
                v.t = JVal::Obj;
                ++p;
                ws();
                if (p < e && *p == '}') { ++p; return true; }
                for (;;)
                {
                    ws();
                    std::string k;
                    if (!str(k))
                        return false;
                    ws();
                    if (p >= e || *p != ':')
                        return false;
                    ++p;
                    JVal child;
                    if (!val(child, depth + 1))
                        return false;
                    v.o.emplace_back(std::move(k), std::move(child));
                    ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == '}') { ++p; return true; }
                    return false;
                }
            }
            if (*p == '[')
            {
                v.t = JVal::Arr;
                ++p;
                ws();
                if (p < e && *p == ']') { ++p; return true; }
                for (;;)
                {
                    JVal child;
                    if (!val(child, depth + 1))
                        return false;
                    v.a.push_back(std::move(child));
                    ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == ']') { ++p; return true; }
                    return false;
                }
            }
            if (*p == '"')
            {
                v.t = JVal::Str;
                return str(v.s);
            }
            if (e - p >= 4 && std::strncmp(p, "true", 4) == 0) { v.t = JVal::Bool; v.b = true; p += 4; return true; }
            if (e - p >= 5 && std::strncmp(p, "false", 5) == 0) { v.t = JVal::Bool; v.b = false; p += 5; return true; }
            if (e - p >= 4 && std::strncmp(p, "null", 4) == 0) { v.t = JVal::Null; p += 4; return true; }
            char* end = nullptr;
            const double d = std::strtod(p, &end);
            if (end == p)
                return false;
            v.t = JVal::Num;
            v.n = d;
            p = end;
            return true;
        }
    };

    bool ReadFile(const fs::path& path, std::string& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    bool ParseJsonFile(const fs::path& path, JVal& out)
    {
        std::string text;
        if (!ReadFile(path, text))
            return false;
        JParser jp{ text.data(), text.data() + text.size() };
        return jp.val(out);
    }

    bool Vec3(const JVal* v, float out[3])
    {
        if (!v || v->t != JVal::Arr || v->a.size() < 3)
            return false;
        for (int i = 0; i < 3; ++i)
            out[i] = static_cast<float>(v->a[i].n);
        return true;
    }

    // ---- Where the models are ----
    fs::path FromVrPath(const std::string& s)
    {
        std::string p = s;
        if (p.rfind("\\??\\unix", 0) == 0)
            p = p.substr(8);
        if (!p.empty() && p[0] == '/')
            p = "Z:" + p;   // Proton: the Linux root is drive Z:
        return fs::path(p);
    }

    void AddRuntimeRoots(const fs::path& root, std::vector<fs::path>& out)
    {
        std::error_code ec;
        out.push_back(root / "resources" / "rendermodels");
        const fs::path drivers = root / "drivers";
        if (fs::is_directory(drivers, ec))
            for (const auto& d : fs::directory_iterator(drivers, ec))
                out.push_back(d.path() / "resources" / "rendermodels");
    }

    std::vector<fs::path> ModelRoots()
    {
        std::vector<fs::path> roots;
        // Steam library of the game: SteamVR and the PS VR2 app usually sit next to it.
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const fs::path common = fs::path(exe).parent_path().parent_path();
        AddRuntimeRoots(common / "SteamVR", roots);
        roots.push_back(common / "PlayStation VR2 App" / "SteamVR_Plug-In" / "resources" / "rendermodels");
        // SteamVR's own list of runtimes and external drivers.
        char local[MAX_PATH]{};
        if (GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH))
        {
            JVal j;
            if (ParseJsonFile(fs::path(local) / "openvr" / "openvrpaths.vrpath", j))
            {
                if (const JVal* rt = j.get("runtime"))
                    for (const JVal& r : rt->a)
                        if (r.t == JVal::Str)
                            AddRuntimeRoots(FromVrPath(r.s), roots);
                if (const JVal* ext = j.get("external_drivers"))
                    for (const JVal& r : ext->a)
                        if (r.t == JVal::Str)
                            roots.push_back(FromVrPath(r.s) / "resources" / "rendermodels");
            }
        }
        AddRuntimeRoots("C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR", roots);
        std::vector<fs::path> out;
        std::error_code ec;
        for (const auto& r : roots)
        {
            if (!fs::is_directory(r, ec))
                continue;
            if (std::find(out.begin(), out.end(), r) == out.end())
                out.push_back(r);
        }
        return out;
    }

    bool TextHas(const char* s, const char* needle) { return s && L4D2VR_TextContainsI(s, needle); }

    std::vector<std::string> Candidates(uint32_t family, const char* sys, int hand)
    {
        const std::string side = hand ? "right" : "left";
        std::vector<std::string> c;
        if (TextHas(sys, "PlayStation") || TextHas(sys, "PS VR2"))
            c.push_back("playstation_vr2_sense_" + side);
        switch (family)
        {
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_TOUCH:
            if (TextHas(sys, "Pro"))
                c.push_back("oculus_quest_pro_controller_" + side);
            if (TextHas(sys, "Quest 3") || TextHas(sys, "Plus"))
                c.push_back("oculus_quest_plus_controller_" + side);
            if (TextHas(sys, "Rift S"))
                c.push_back("oculus_rifts_controller_" + side);
            else if (TextHas(sys, "Rift"))
                c.push_back("oculus_cv1_controller_" + side);
            if (TextHas(sys, "Pico"))
            {
                c.push_back("pico_4_ultra_controller_" + side);
                c.push_back("pico_4_controller_" + side);
            }
            c.push_back("oculus_quest2_controller_" + side);
            c.push_back("oculus_quest_plus_controller_" + side);
            break;
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_KNUCKLES:
            c.push_back("valve_controller_knu_1_0_" + side);
            c.push_back("valve_controller_knu_ev2_0_" + side);
            break;
        case L4D2VR_OPENXR_CONTROLLER_FAMILY_VIVE:
            c.push_back("vr_controller_vive_1_5");
            break;
        default:
            break;
        }
        return c;
    }

    // Model parts per button. Several names: models differ (A/X, grip/squeeze, stick/pad).
    const std::vector<const char*>& PartNames(int part, int hand)
    {
        static const std::vector<const char*> trig = { "trigger" };
        // Rift CV1 names its parts a_button / x_button / grip / enter_button;
        // "grip" last: the other Touch models also have a "grip" (the whole
        // handle), after their button_grip.
        static const std::vector<const char*> gripL = { "button_grip", "squeeze", "rgrip", "lgrip", "grip" };
        static const std::vector<const char*> gripR = { "button_grip", "squeeze", "lgrip", "rgrip", "grip" };
        static const std::vector<const char*> f1 = { "button_a", "button_x", "a_button", "x_button" };
        static const std::vector<const char*> f2 = { "button_b", "button_y", "b_button", "y_button" };
        static const std::vector<const char*> stick = { "thumbstick", "trackpad" };
        static const std::vector<const char*> menu = { "button_option", "button_enter", "button", "button_menu", "enter_button" };
        static const std::vector<const char*> none;
        switch (part)
        {
        case bmvr::ctlmodel::kTrigger: return trig;
        case bmvr::ctlmodel::kGrip: return hand ? gripR : gripL;
        case bmvr::ctlmodel::kFace1: return f1;
        case bmvr::ctlmodel::kFace2: return f2;
        case bmvr::ctlmodel::kStick: return stick;
        case bmvr::ctlmodel::kMenu: return menu;
        default: return none;
        }
    }

    // ---- Mesh + texture ----
    struct Texture
    {
        int w = 0, h = 0;
        std::vector<uint32_t> px;
    };

    bool DecodePng(const fs::path& path, Texture& out)
    {
        std::string bytes;
        if (!ReadFile(path, bytes) || bytes.size() < 8)
            return false;
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) && factory
            && SUCCEEDED(factory->CreateStream(&stream)) && stream
            && SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())))
            && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) && decoder
            && SUCCEEDED(decoder->GetFrame(0, &frame)) && frame)
        {
            UINT w = 0, h = 0;
            if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 8192 && h <= 8192
                && SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv
                && SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                out.w = static_cast<int>(w);
                out.h = static_cast<int>(h);
                out.px.resize(static_cast<size_t>(w) * h);
                ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(out.px.data())));
            }
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        return ok;
    }

    struct Tri
    {
        float p[3][3];
        float uv[3][2];
        int tex = -1;
    };

    struct Mesh
    {
        std::vector<Tri> tris;
        std::vector<Texture> textures;
        std::map<std::string, int> texIndex;
        // Per part: sum of vertex positions (centre = sum / count).
        double sum[kPartCount][3] = {};
        int count[kPartCount] = {};
    };

    int LoadTexture(Mesh& m, const fs::path& path)
    {
        const std::string key = path.string();
        auto it = m.texIndex.find(key);
        if (it != m.texIndex.end())
            return it->second;
        Texture t;
        const int idx = DecodePng(path, t) ? static_cast<int>(m.textures.size()) : -1;
        if (idx >= 0)
            m.textures.push_back(std::move(t));
        m.texIndex[key] = idx;
        return idx;
    }

    std::string MtlTexture(const fs::path& mtl)
    {
        std::ifstream f(mtl);
        std::string line;
        while (std::getline(f, line))
        {
            std::istringstream ls(line);
            std::string tag;
            ls >> tag;
            if (tag == "map_Kd")
            {
                std::string name;
                std::getline(ls, name);
                const size_t b = name.find_first_not_of(" \t");
                const size_t e = name.find_last_not_of(" \t\r");
                return b == std::string::npos ? std::string() : name.substr(b, e - b + 1);
            }
        }
        return {};
    }

    // part: which button this OBJ is (-1 = body), counted for its centre.
    bool LoadObj(Mesh& m, const fs::path& dir, const std::string& file, int part)
    {
        std::string text;
        if (!ReadFile(dir / file, text))
            return false;
        std::vector<float> v, vt;
        int tex = -1;
        const char* p = text.data();
        const char* end = p + text.size();
        while (p < end)
        {
            const char* eol = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!eol)
                eol = end;
            std::string line(p, eol);
            p = eol + 1;
            if (line.size() < 2)
                continue;
            if (line[0] == 'v' && line[1] == ' ')
            {
                float x = 0, y = 0, z = 0;
                std::sscanf(line.c_str() + 2, "%f %f %f", &x, &y, &z);
                v.push_back(x); v.push_back(y); v.push_back(z);
                if (part >= 0)
                {
                    m.sum[part][0] += x; m.sum[part][1] += y; m.sum[part][2] += z;
                    ++m.count[part];
                }
            }
            else if (line[0] == 'v' && line[1] == 't')
            {
                float s = 0, t = 0;
                std::sscanf(line.c_str() + 2, "%f %f", &s, &t);
                vt.push_back(s); vt.push_back(t);
            }
            else if (line.rfind("mtllib", 0) == 0)
            {
                std::string name = line.substr(6);
                const size_t b = name.find_first_not_of(" \t");
                const size_t e = name.find_last_not_of(" \t\r");
                if (b != std::string::npos)
                {
                    const std::string png = MtlTexture(dir / name.substr(b, e - b + 1));
                    if (!png.empty())
                        tex = LoadTexture(m, dir / png);
                }
            }
            else if (line[0] == 'f' && line[1] == ' ')
            {
                int vi[16], ti[16], n = 0;
                std::istringstream ls(line.substr(2));
                std::string tok;
                while (n < 16 && (ls >> tok))
                {
                    int a = 0, b = 0;
                    a = std::atoi(tok.c_str());
                    const size_t s1 = tok.find('/');
                    if (s1 != std::string::npos && s1 + 1 < tok.size() && tok[s1 + 1] != '/')
                        b = std::atoi(tok.c_str() + s1 + 1);
                    const int nv = static_cast<int>(v.size() / 3), nt = static_cast<int>(vt.size() / 2);
                    vi[n] = a > 0 ? a - 1 : nv + a;
                    ti[n] = b > 0 ? b - 1 : (b < 0 ? nt + b : -1);
                    if (vi[n] < 0 || vi[n] >= nv)
                        break;
                    if (ti[n] >= nt)
                        ti[n] = -1;
                    ++n;
                }
                for (int k = 1; k + 1 < n; ++k)
                {
                    Tri t{};
                    const int idx[3] = { 0, k, k + 1 };
                    for (int c = 0; c < 3; ++c)
                    {
                        for (int d = 0; d < 3; ++d)
                            t.p[c][d] = v[vi[idx[c]] * 3 + d];
                        if (ti[idx[c]] >= 0)
                        {
                            t.uv[c][0] = vt[ti[idx[c]] * 2];
                            t.uv[c][1] = vt[ti[idx[c]] * 2 + 1];
                        }
                    }
                    t.tex = tex;
                    m.tris.push_back(t);
                }
            }
        }
        return true;
    }

    // ---- Small 3x3 helpers ----
    void Normalize(float v[3])
    {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-9f)
            for (int i = 0; i < 3; ++i)
                v[i] /= l;
    }
    void Cross(const float a[3], const float b[3], float o[3])
    {
        o[0] = a[1] * b[2] - a[2] * b[1];
        o[1] = a[2] * b[0] - a[0] * b[2];
        o[2] = a[0] * b[1] - a[1] * b[0];
    }
    // Rotation from SteamVR's component_local rotate_xyz (degrees), applied X, then Y, then Z.
    void EulerXyz(const float deg[3], float r[3][3])
    {
        const float k = 3.14159265f / 180.f;
        const float cx = std::cos(deg[0] * k), sx = std::sin(deg[0] * k);
        const float cy = std::cos(deg[1] * k), sy = std::sin(deg[1] * k);
        const float cz = std::cos(deg[2] * k), sz = std::sin(deg[2] * k);
        const float rx[3][3] = { { 1, 0, 0 }, { 0, cx, -sx }, { 0, sx, cx } };
        const float ry[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
        const float rz[3][3] = { { cz, -sz, 0 }, { sz, cz, 0 }, { 0, 0, 1 } };
        float t[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                t[i][j] = ry[i][0] * rx[0][j] + ry[i][1] * rx[1][j] + ry[i][2] * rx[2][j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i][j] = rz[i][0] * t[0][j] + rz[i][1] * t[1][j] + rz[i][2] * t[2][j];
    }

    // ---- CPU picture ----
    // View like the PS5 button assignment screen: from the thumb side (which
    // faces the panel), a little from above, the controller's front pointing
    // up and the ring at the bottom.
    void RenderPicture(const Mesh& m, int hand, int outW, int outH, bmvr::ctlmodel::HandModel& hm)
    {
        constexpr int kSS = 2;
        const int W = outW * kSS, H = outH * kSS;
        float z[3] = { hand ? -1.f : 1.f, 0.7f, 0.3f };
        float upHint[3] = { 0.f, -0.3f, -1.f };
        Normalize(z);
        float x[3], y[3];
        Cross(upHint, z, x);
        Normalize(x);
        Cross(z, x, y);
        auto view = [&](const float p[3], float o[3]) {
            o[0] = x[0] * p[0] + x[1] * p[1] + x[2] * p[2];
            o[1] = y[0] * p[0] + y[1] * p[1] + y[2] * p[2];
            o[2] = z[0] * p[0] + z[1] * p[1] + z[2] * p[2];
        };
        float mn[2] = { 1e9f, 1e9f }, mx[2] = { -1e9f, -1e9f };
        for (const Tri& t : m.tris)
            for (int c = 0; c < 3; ++c)
            {
                float q[3];
                view(t.p[c], q);
                for (int d = 0; d < 2; ++d)
                {
                    mn[d] = (std::min)(mn[d], q[d]);
                    mx[d] = (std::max)(mx[d], q[d]);
                }
            }
        if (!(mx[0] > mn[0]) || !(mx[1] > mn[1]))
            return;
        const float margin = 6.f * kSS;
        const float s = (std::min)((W - 2 * margin) / (mx[0] - mn[0]), (H - 2 * margin) / (mx[1] - mn[1]));
        const float cx = (mn[0] + mx[0]) * 0.5f, cy = (mn[1] + mx[1]) * 0.5f;
        auto toScreen = [&](const float q[3], float& sx, float& sy) {
            sx = (q[0] - cx) * s + W * 0.5f;
            sy = H * 0.5f - (q[1] - cy) * s;
        };

        std::vector<float> zb(static_cast<size_t>(W) * H, -1e9f);
        std::vector<uint32_t> col(static_cast<size_t>(W) * H, 0);
        float light[3] = { 0.35f, 0.65f, 0.7f };
        Normalize(light);
        for (const Tri& t : m.tris)
        {
            float q[3][3], sx[3], sy[3];
            for (int c = 0; c < 3; ++c)
            {
                view(t.p[c], q[c]);
                toScreen(q[c], sx[c], sy[c]);
            }
            float e1[3] = { q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2] };
            float e2[3] = { q[2][0] - q[0][0], q[2][1] - q[0][1], q[2][2] - q[0][2] };
            float n[3];
            Cross(e1, e2, n);
            Normalize(n);
            const float shade = 0.42f + 0.7f * std::fabs(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]);
            const float den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
            if (std::fabs(den) < 1e-6f)
                continue;
            const int x0 = (std::max)(0, static_cast<int>(std::floor((std::min)({ sx[0], sx[1], sx[2] }))));
            const int x1 = (std::min)(W - 1, static_cast<int>(std::ceil((std::max)({ sx[0], sx[1], sx[2] }))));
            const int y0 = (std::max)(0, static_cast<int>(std::floor((std::min)({ sy[0], sy[1], sy[2] }))));
            const int y1 = (std::min)(H - 1, static_cast<int>(std::ceil((std::max)({ sy[0], sy[1], sy[2] }))));
            const Texture* tex = (t.tex >= 0 && t.tex < static_cast<int>(m.textures.size())) ? &m.textures[t.tex] : nullptr;
            for (int py = y0; py <= y1; ++py)
                for (int px = x0; px <= x1; ++px)
                {
                    const float fx = px + 0.5f, fy = py + 0.5f;
                    const float w0 = ((sy[1] - sy[2]) * (fx - sx[2]) + (sx[2] - sx[1]) * (fy - sy[2])) / den;
                    const float w1 = ((sy[2] - sy[0]) * (fx - sx[2]) + (sx[0] - sx[2]) * (fy - sy[2])) / den;
                    const float w2 = 1.f - w0 - w1;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
                        continue;
                    const float depth = w0 * q[0][2] + w1 * q[1][2] + w2 * q[2][2];
                    float& zr = zb[static_cast<size_t>(py) * W + px];
                    if (depth <= zr)
                        continue;
                    zr = depth;
                    float r = 150.f, g = 150.f, b = 155.f;
                    if (tex)
                    {
                        float u = w0 * t.uv[0][0] + w1 * t.uv[1][0] + w2 * t.uv[2][0];
                        float v = 1.f - (w0 * t.uv[0][1] + w1 * t.uv[1][1] + w2 * t.uv[2][1]);
                        u -= std::floor(u);
                        v -= std::floor(v);
                        const int tx = (std::min)(tex->w - 1, static_cast<int>(u * tex->w));
                        const int ty = (std::min)(tex->h - 1, static_cast<int>(v * tex->h));
                        const uint32_t c = tex->px[static_cast<size_t>(ty) * tex->w + tx];
                        r = static_cast<float>((c >> 16) & 0xFF);
                        g = static_cast<float>((c >> 8) & 0xFF);
                        b = static_cast<float>(c & 0xFF);
                    }
                    // Dark plastic stays readable on the dark panel: a floor plus the light.
                    auto ch = [&](float v) { return static_cast<uint32_t>((std::min)(255.f, (v + 26.f) * shade)); };
                    col[static_cast<size_t>(py) * W + px] = 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
                }
        }

        // 2x2 down-sample with coverage alpha.
        bmvr::ctlmodel::Picture& pic = hm.picture;
        pic.w = outW;
        pic.h = outH;
        pic.px.assign(static_cast<size_t>(outW) * outH, 0);
        for (int py = 0; py < outH; ++py)
            for (int px = 0; px < outW; ++px)
            {
                uint32_t sr = 0, sg = 0, sb = 0, n = 0;
                for (int dy = 0; dy < kSS; ++dy)
                    for (int dx = 0; dx < kSS; ++dx)
                    {
                        const uint32_t c = col[static_cast<size_t>(py * kSS + dy) * W + px * kSS + dx];
                        if (!(c >> 24))
                            continue;
                        sr += (c >> 16) & 0xFF;
                        sg += (c >> 8) & 0xFF;
                        sb += c & 0xFF;
                        ++n;
                    }
                if (!n)
                    continue;
                const uint32_t a = n * 255 / (kSS * kSS);
                pic.px[static_cast<size_t>(py) * outW + px] = (a << 24) | ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
            }
        // Light outline so the dark controller stands out from the dark panel.
        std::vector<uint32_t> outlined = pic.px;
        for (int py = 0; py < outH; ++py)
            for (int px = 0; px < outW; ++px)
            {
                if (pic.px[static_cast<size_t>(py) * outW + px] >> 24)
                    continue;
                bool edge = false;
                for (int dy = -2; dy <= 2 && !edge; ++dy)
                    for (int dx = -2; dx <= 2 && !edge; ++dx)
                    {
                        const int nx = px + dx, ny = py + dy;
                        if (nx >= 0 && ny >= 0 && nx < outW && ny < outH && (pic.px[static_cast<size_t>(ny) * outW + nx] >> 24) > 128)
                            edge = true;
                    }
                if (edge)
                    outlined[static_cast<size_t>(py) * outW + px] = 0xC0000000u | (96u << 16) | (104u << 8) | 116u;
            }
        pic.px.swap(outlined);

        for (int part = 0; part < kPartCount; ++part)
        {
            if (!hm.has[part])
                continue;
            float q[3];
            view(hm.devicePoint[part], q);
            float sx, sy;
            toScreen(q, sx, sy);
            pic.anchorX[part] = sx / kSS;
            pic.anchorY[part] = sy / kSS;
        }
    }

    bool LoadHand(const std::vector<fs::path>& roots, const std::vector<std::string>& names, int hand,
        int picW, int picH, bmvr::ctlmodel::HandModel& hm)
    {
        std::error_code ec;
        for (const std::string& name : names)
            for (const fs::path& root : roots)
            {
                const fs::path dir = root / name;
                const fs::path json = dir / (name + ".json");
                if (!fs::exists(json, ec))
                    continue;
                JVal j;
                if (!ParseJsonFile(json, j))
                {
                    Game::logMsg("Controller model %s: bad json", json.string().c_str());
                    continue;
                }
                const JVal* comps = j.get("components");
                if (!comps || comps->t != JVal::Obj)
                    continue;
                Mesh mesh;
                // Which part each component is.
                std::map<std::string, int> partOf;
                for (int part = 0; part < kPartCount; ++part)
                    for (const char* pn : PartNames(part, hand))
                        if (comps->get(pn) && comps->get(pn)->get("filename") && !partOf.count(pn))
                        {
                            bool taken = false;
                            for (const auto& kv : partOf)
                                taken |= kv.second == part;
                            if (!taken)
                                partOf[pn] = part;
                        }
                for (const auto& kv : comps->o)
                {
                    const JVal* file = kv.second.get("filename");
                    if (!file || file->t != JVal::Str)
                        continue;
                    if (const JVal* vis = kv.second.get("visibility"))
                        if (const JVal* def = vis->get("default"))
                            if (def->t == JVal::Bool && !def->b)
                                continue;
                    auto it = partOf.find(kv.first);
                    LoadObj(mesh, dir, file->s, it == partOf.end() ? -1 : it->second);
                }
                if (mesh.tris.empty())
                    continue;

                hm.name = name;
                // OpenXR grip pose = device pose * openxr_grip (SteamVR); points
                // in grip space = R^T (p - origin).
                float gripOrigin[3] = {}, gripRot[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                if (const JVal* g = comps->get("openxr_grip"))
                    if (const JVal* local = g->get("component_local"))
                    {
                        float deg[3] = {};
                        if (Vec3(local->get("origin"), gripOrigin))
                        {
                            Vec3(local->get("rotate_xyz"), deg);
                            EulerXyz(deg, gripRot);
                            hm.gripKnown = true;
                        }
                    }
                auto toGrip = [&](const float p[3], float o[3]) {
                    const float d[3] = { p[0] - gripOrigin[0], p[1] - gripOrigin[1], p[2] - gripOrigin[2] };
                    for (int i = 0; i < 3; ++i)
                        o[i] = gripRot[0][i] * d[0] + gripRot[1][i] * d[1] + gripRot[2][i] * d[2];
                };
                for (int part = 0; part < kPartCount; ++part)
                {
                    if (!mesh.count[part])
                        continue;
                    hm.has[part] = true;
                    for (int d = 0; d < 3; ++d)
                        hm.devicePoint[part][d] = static_cast<float>(mesh.sum[part][d] / mesh.count[part]);
                    toGrip(hm.devicePoint[part], hm.gripPoint[part]);
                }
                float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
                for (const Tri& t : mesh.tris)
                    for (int c = 0; c < 3; ++c)
                        for (int d = 0; d < 3; ++d)
                        {
                            mn[d] = (std::min)(mn[d], t.p[c][d]);
                            mx[d] = (std::max)(mx[d], t.p[c][d]);
                        }
                for (int d = 0; d < 3; ++d)
                    hm.deviceCenter[d] = (mn[d] + mx[d]) * 0.5f;
                toGrip(hm.deviceCenter, hm.gripCenter);
                RenderPicture(mesh, hand, picW, picH, hm);
                hm.ok = !hm.picture.px.empty();
                Game::logMsg("Controller model %s: %s, %zu triangles, %zu textures, grip %s, parts%s%s%s%s%s%s",
                    hand ? "right" : "left", dir.string().c_str(), mesh.tris.size(), mesh.textures.size(),
                    hm.gripKnown ? "openxr_grip" : "device origin",
                    hm.has[0] ? " trigger" : "", hm.has[1] ? " grip" : "", hm.has[2] ? " face1" : "",
                    hm.has[3] ? " face2" : "", hm.has[4] ? " stick" : "", hm.has[5] ? " menu" : "");
                return hm.ok;
            }
        return false;
    }

    std::mutex g_Lock;
    std::shared_ptr<const bmvr::ctlmodel::Models> g_Current;
    std::string g_RequestedKey;
    std::atomic<int> g_Generation{ 0 };
}

void bmvr::ctlmodel::Request(uint32_t family, const char* systemName, int picW, int picH)
{
    char key[160]{};
    _snprintf(key, sizeof(key) - 1, "%u|%s|%dx%d", family, systemName ? systemName : "", picW, picH);
    {
        std::lock_guard<std::mutex> lock(g_Lock);
        if (g_RequestedKey == key)
            return;
        g_RequestedKey = key;
        if (g_Current)
        {
            g_Current.reset();
            ++g_Generation;
        }
    }
    const std::string sys = systemName ? systemName : "";
    const std::string k = key;
    std::thread([family, sys, k, picW, picH]() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const DWORD t0 = GetTickCount();
        auto models = std::make_shared<Models>();
        models->key = k;
        const std::vector<fs::path> roots = ModelRoots();
        bool any = false;
        for (int hand = 0; hand < 2; ++hand)
        {
            const std::vector<std::string> names = Candidates(family, sys.c_str(), hand);
            if (names.empty())
                continue;
            if (LoadHand(roots, names, hand, picW, picH, models->hand[hand]))
                any = true;
            else
                Game::logMsg("Controller model %s: none of %zu names found in %zu folders (family %s, system \"%s\")",
                    hand ? "right" : "left", names.size(), roots.size(), L4D2VR_ControllerFamilyName(family), sys.c_str());
        }
        Game::logMsg("Controller models %s in %lu ms", any ? "ready" : "missing (CONTROLS tab keeps the list)",
            GetTickCount() - t0);
        {
            std::lock_guard<std::mutex> lock(g_Lock);
            if (g_RequestedKey == k && any)
            {
                g_Current = models;
                ++g_Generation;
            }
        }
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

std::shared_ptr<const bmvr::ctlmodel::Models> bmvr::ctlmodel::Current()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    return g_Current;
}

int bmvr::ctlmodel::Generation() { return g_Generation.load(); }
