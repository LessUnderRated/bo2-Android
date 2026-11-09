// Tests for runtime/gfx/dxbc_to_vk.cpp on the PC: our own test shaders (runtime/tests/shaders/*.hlsl, compiled to
// DXBC by make_dxbc.cpp) are translated to Vulkan GLSL and checked against what D3D says. scripts/test-shaders.sh then
// compiles every output to SPIR-V with the NDK's glslc and validates it with spirv-val.
//
//   shader_vk_test <shaders dir> <out dir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" int bo2_dxbc_translate2(const void*, uint32_t, char**, char**, char**, char**, char**);
extern "C" int bo2_dxbc_cbuffer_layout(const void*, uint32_t, const char*, char**);
extern "C" char* bo2_msl_attributes(const char*);
extern "C" void bo2_msl_free(char*);

static int g_failed = 0;
#define CHECK(c)                                                                               \
    do {                                                                                       \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_failed; } \
    } while (0)

static std::string read(const std::string& p) {
    std::string s;
    if (FILE* f = fopen(p.c_str(), "rb")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        fclose(f);
    }
    return s;
}

struct Out { bool ok = false; std::string src, tex, cb, dim, smp, attrs; };

static Out translate(const std::string& dir, const std::string& name, const std::string& out_dir) {
    const std::string dxbc = read(dir + "/" + name + ".dxbc");
    Out o;
    CHECK(!dxbc.empty());
    char *src = nullptr, *tex = nullptr, *cb = nullptr, *dim = nullptr, *smp = nullptr;
    o.ok = bo2_dxbc_translate2(dxbc.data(), (uint32_t)dxbc.size(), &src, &tex, &cb, &dim, &smp) && src;
    auto take = [](char* p) { std::string s = p ? p : ""; bo2_msl_free(p); return s; };
    o.src = take(src); o.tex = take(tex); o.cb = take(cb); o.dim = take(dim); o.smp = take(smp);
    if (o.ok) o.attrs = take(bo2_msl_attributes(o.src.c_str()));
    const std::string stage = name.find("_vs") != std::string::npos ? "vert" : "frag";
    if (FILE* f = fopen((out_dir + "/" + name + "." + stage).c_str(), "wb")) { fputs(o.src.c_str(), f); fclose(f); }
    printf("%-12s %s  cb[%s] tex[%s] dim[%s] smp[%s] attrs[%s]\n", name.c_str(), o.ok ? "ok" : "FAILED", o.cb.c_str(),
           o.tex.c_str(), o.dim.c_str(), o.smp.c_str(), o.attrs.c_str());
    return o;
}

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

// "a=1,b=2" -> {a:1, b:2}
static std::map<std::string, long> parse_map(const std::string& s) {
    std::map<std::string, long> m;
    for (size_t p = 0; p < s.size();) {
        size_t e = s.find('=', p), c = s.find(',', p);
        if (e == std::string::npos) break;
        m[s.substr(p, e - p)] = strtol(s.c_str() + e + 1, nullptr, 10);
        if (c == std::string::npos) break;
        p = c + 1;
    }
    return m;
}

// The register a binding of the map points at, by the declared name at that binding.
static long reg_of(const Out& o, const std::string& map, const std::string& decl_name) {
    const std::string key = " " + decl_name + ";";
    for (size_t p = o.src.find("layout(set = "); p != std::string::npos; p = o.src.find("layout(set = ", p + 1)) {
        const size_t eol = o.src.find('\n', p);
        const std::string line = o.src.substr(p, eol - p);
        if (line.find(key) == std::string::npos && line.find(" " + decl_name + " {") == std::string::npos) continue;
        int set, binding;
        if (sscanf(line.c_str(), "layout(set = %d, binding = %d", &set, &binding) != 2) return -999;
        const auto m = parse_map(map);
        const auto it = m.find(std::to_string(binding));
        return it == m.end() ? -999 : it->second;
    }
    return -998;
}

// Every member of a constant buffer sits at its D3D offset: an explicit layout(offset = N) on it, or on its first
// component when the translator split it.
static void check_offsets(const std::string& dxbc_path, const std::string& src, const char* cbname) {
    const std::string dxbc = read(dxbc_path);
    char* lay = nullptr;
    CHECK(bo2_dxbc_cbuffer_layout(dxbc.data(), (uint32_t)dxbc.size(), cbname, &lay) && lay);
    if (!lay) return;
    for (const auto& [name, off] : parse_map(lay)) {
        const std::string whole = "layout(offset = " + std::to_string(off) + ") ";
        bool found = false;
        for (size_t p = src.find(whole); p != std::string::npos; p = src.find(whole, p + 1)) {
            const size_t eol = src.find('\n', p);
            const std::string line = src.substr(p, eol - p);
            if (line.find(" " + name + ";") != std::string::npos || line.find(" " + name + "[") != std::string::npos ||
                line.find(" " + name + "_d3d0;") != std::string::npos)
                found = true;
        }
        if (!found) fprintf(stderr, "  member %s.%s: no layout(offset = %ld)\n", cbname, name.c_str(), off);
        CHECK(found);
    }
    bo2_msl_free(lay);
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "shader_vk_test <shaders dir> <out dir>\n"); return 2; }
    const std::string dir = argv[1], out = argv[2];

    const Out vs = translate(dir, "basic_vs", out);
    CHECK(vs.ok);
    CHECK(reg_of(vs, vs.cb, "Misc") == 0 && reg_of(vs, vs.cb, "Object") == 2);   // b0 and b2: not contiguous
    {   // Every vertex input is in the map, at the location its declaration has, and no two share one. (The numbers
        // are HLSLcc's: with KEEP_VARYING_LOCATIONS they need not be contiguous. The backend binds by this map.)
        const auto a = parse_map(vs.attrs);
        CHECK(a.size() == 4 && a.count("POSITION0") && a.count("COLOR0") && a.count("TEXCOORD0") && a.count("NORMAL0"));
        std::map<long, int> used;
        for (const auto& [sem, loc] : a) {
            CHECK(used[loc]++ == 0);
            CHECK(has(vs.src, "layout(location = " + std::to_string(loc) + ") in highp ") &&
                  has(vs.src, " in_" + sem + ";"));
        }
    }
    CHECK(has(vs.src, "invariant gl_Position;"));
    CHECK(has(vs.src, "#define StaticScale vec2(StaticScale_d3d0, StaticScale_d3d1)"));   // D3D offset 4
    CHECK(!has(vs.src, "mediump") && !has(vs.src, "lowp"));
    check_offsets(dir + "/basic_vs.dxbc", vs.src, "Misc");
    check_offsets(dir + "/basic_vs.dxbc", vs.src, "Object");

    const Out ps = translate(dir, "basic_ps", out);
    CHECK(ps.ok);
    CHECK(reg_of(ps, ps.tex, "Diffuse") == 3 && reg_of(ps, ps.tex, "Reflection") == 0);
    CHECK(reg_of(ps, ps.tex, "Lighting") == 5 && reg_of(ps, ps.tex, "Shadow") == 7);
    CHECK(reg_of(ps, ps.smp, "samplerDiffuseSampler") == 1 && reg_of(ps, ps.smp, "samplerShadowSampler") == 4);
    CHECK(reg_of(ps, ps.smp, "hlslcc_dummyPointClamp") == -1);   // the point-clamp sampler HLSLcc adds for Load
    {   // dimensions, by binding: 2D, cube, 3D, and the 2D texture read through a comparison sampler
        const auto dims = parse_map(ps.dim);
        auto dim_of = [&](const char* n) {
            for (size_t p = ps.src.find("layout(set = 0"); p != std::string::npos; p = ps.src.find("layout(set = 0", p + 1)) {
                const std::string line = ps.src.substr(p, ps.src.find('\n', p) - p);
                int s, b;
                if (line.find(std::string(" ") + n + ";") != std::string::npos &&
                    sscanf(line.c_str(), "layout(set = %d, binding = %d", &s, &b) == 2)
                    return dims.count(std::to_string(b)) ? dims.at(std::to_string(b)) : -1L;
            }
            return -2L;
        };
        CHECK(dim_of("Diffuse") == 2 && dim_of("Reflection") == 4 && dim_of("Lighting") == 3 && dim_of("Shadow") == 5);
    }
    CHECK(has(ps.src, "ivec2( 1, -1)") || has(ps.src, "ivec2(1, -1)") || has(ps.src, "ivec2(1,-1)"));
    CHECK(!has(ps.src, "ivec3(1, -1)"));
    CHECK(!has(ps.src, "mediump"));
    CHECK(ps.attrs.empty());   // pixel shader inputs are varyings, not vertex attributes
    {   // The varyings link as D3D links them: each pixel shader input at the location of the vertex output of the
        // same name.
        auto varyings = [](const std::string& src, const char* dir) {
            std::map<std::string, int> m;
            for (size_t p = src.find("layout(location = "); p != std::string::npos; p = src.find("layout(location = ", p + 1)) {
                const std::string line = src.substr(p, src.find('\n', p) - p);
                int loc;
                const size_t v = line.find(" vs_");
                if (line.find(std::string(") ") + dir + " ") == std::string::npos || v == std::string::npos) continue;
                if (sscanf(line.c_str(), "layout(location = %d)", &loc) == 1)
                    m[line.substr(v + 1, line.find(';', v) - v - 1)] = loc;
            }
            return m;
        };
        const auto out = varyings(vs.src, "out"), in = varyings(ps.src, "in");
        CHECK(in.size() == 3);
        for (const auto& [name, loc] : in) CHECK(out.count(name) && out.at(name) == loc);
    }

    const Out lay = translate(dir, "layout_ps", out);
    CHECK(lay.ok);
    check_offsets(dir + "/layout_ps.dxbc", lay.src, "Hard");
    CHECK(has(lay.src, "#define packedv3 vec3(packedv3_d3d0, packedv3_d3d1, packedv3_d3d2)"));

    // HLSLcc's known gap: it still emits something, which glslc then rejects (scripts/test-shaders.sh expects that).
    const Out dyn = translate(dir, "dynindex_ps", out);
    (void)dyn;

    if (g_failed) { fprintf(stderr, "shader_vk_test: %d failed\n", g_failed); return 1; }
    printf("shader_vk_test: all passed\n");
    return 0;
}
