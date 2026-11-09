// dxbc_to_vk.cpp — the game's DXBC shaders to Vulkan GLSL, for the Android Vulkan backend.
//
// The Android counterpart of bo2-mac's runtime/gfx/dxbc_to_msl.cpp, with the same four entry points the runtime calls
// (bo2_dxbc_translate2, bo2_dxbc_cbuffer_layout, bo2_msl_attributes, bo2_msl_free), so remaining_dlls_impl.cpp does
// not change. The "msl" the runtime carries around is Vulkan GLSL here; the backend compiles it to SPIR-V.
//
// The same translator, HLSLcc (bo2-mac's vendored copy), in its Vulkan mode: LANG_440 with HLSLCC_FLAG_VULKAN_BINDINGS.
// What this file adds on top of HLSLcc's output, each for a measured reason (docs/GRAPHICS_VULKAN.md):
//
//   constant buffers  HLSLcc declares std140 blocks. std140 agrees with D3D's packing for arrays, matrices and structs
//                     (all start on a 16-byte register in both) but not for a vec2 or vec3 that D3D packs after a
//                     scalar: `float Time; float2 StaticScale;` puts StaticScale at 4 in D3D and at 8 in std140 (the
//                     bug bo2-mac fixed on Metal, F5). Every member gets its D3D offset from the reflection as an
//                     explicit layout(offset = N), which the compiler then checks; a vector D3D left misaligned is
//                     split into scalars and rebuilt by a #define, so the shader body is untouched.
//   texel offsets     HLSLcc writes ivec3(dx, dy) as the offset of a 2D texelFetchOffset: an error in GLSL. The
//                     offset is resized to the sampler's dimension (the twin of bo2-mac's 10-21 MSL fix).
//   precision         mediump/lowp become highp. On Vulkan, mediump is RelaxedPrecision, and Adreno does compute it
//                     at 16 bits; D3D is 32-bit everywhere.
//   varyings          HLSLCC_FLAG_KEEP_VARYING_LOCATIONS: locations from the signature registers, as D3D links them,
//                     not declaration order (a pixel shader reading a subset of the outputs would otherwise shift).
//   invariance        gl_Position is invariant (bo2-mac's P854: same position, same depth, across shaders).
//
// The binding maps the backend needs are read off the declarations HLSLcc emits, as bo2-mac reads its sampler map:
// Everything is in set 0 (rebase_bindings): pixel shaders at 0..31 (textures, samplers) and 32..39 (constant
// buffers), vertex shaders at 64..95 and 96..103.
//   cbmap   "binding=b register,..."   constant buffers
//   texmap  "binding=t register,..."   textures
//   smpmap  "binding=s register,..."   samplers; -1 is HLSLcc's own point-clamp sampler (for Load)
//   texdim  "binding=HLSLCC_TEX_DIMENSION,..."  (a 2D texture sampled with a comparison sampler is TD_2DSHADOW)
#include "hlslcc.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <functional>
#include <mutex>
#include <thread>
#if !defined(_WIN32)
#include <dirent.h>
#endif

namespace {

const char* env(const char* name) { return getenv(name); }
bool env_on(const char* name, bool dflt) {
    const char* v = env(name);
    return v ? atoi(v) != 0 : dflt;
}

class Diag : public HLSLccReflection {
public:
    void OnDiagnostics(const std::string& msg, int, bool isError) override {
        static const bool verbose = env("BO2_MSL_VERBOSE") != nullptr;
        if (verbose || isError) fprintf(stderr, "[VKSL%s] %s\n", isError ? "-ERR" : "", msg.c_str());
        if (isError) errors++;
    }
    int errors = 0;
};

// As dxbc_to_msl.cpp: HLSLcc turns a `$Globals` buffer's leading '$' into another letter, so compare the rest.
bool same_res_name(const std::string& out, const std::string& dxbc) {
    if (out == dxbc) return true;
    if (dxbc.size() == out.size() && !dxbc.empty() && dxbc[0] == '$')
        return dxbc.compare(1, std::string::npos, out, 1, std::string::npos) == 0;
    return false;
}

bool is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

// Replace whole words only.
void replace_word(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos;) {
        const bool left = p == 0 || !is_ident(s[p - 1]);
        const bool right = p + from.size() >= s.size() || !is_ident(s[p + from.size()]);
        if (left && right) {
            s.replace(p, from.size(), to);
            p += to.size();
        } else {
            p += from.size();
        }
    }
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// ---- Constant buffers: D3D offsets on std140 blocks ---------------------------------------------------------------
struct VecInfo { int comps; const char* scalar; };
// "vec2" -> {2, "float"}, "ivec3" -> {3, "int"}, "float" -> {1, "float"}; comps 0 = not a scalar or vector.
VecInfo vec_info(const std::string& t) {
    static const struct { const char* prefix; const char* scalar; } k[] = {
        {"vec", "float"}, {"ivec", "int"}, {"uvec", "uint"}, {"bvec", "bool"}, {"dvec", "double"}};
    if (t == "float" || t == "int" || t == "uint" || t == "bool") return {1, nullptr};
    for (const auto& e : k) {
        const size_t n = strlen(e.prefix);
        if (t.size() == n + 1 && t.compare(0, n, e.prefix) == 0 && t[n] >= '2' && t[n] <= '4') return {t[n] - '0', e.scalar};
    }
    return {0, nullptr};
}

// std140 base alignment of a member that is not an array: scalars 4, vec2 8, vec3/vec4 16; anything else
// (matrices, structs) 16.
uint32_t std140_align(const std::string& type) {
    const VecInfo v = vec_info(type);
    if (v.comps == 1) return 4;
    if (v.comps == 2) return 8;
    return 16;
}

int match_d3d_offsets(std::string& src, const ShaderInfo& refl) {
    int rewritten = 0;
    for (size_t p = src.find(") uniform "); p != std::string::npos; p = src.find(") uniform ", p + 10)) {
        const size_t line = src.rfind('\n', p) + 1;
        if (src.compare(line, 7, "layout(") != 0 || src.find("std140", line) > p) continue;
        const size_t brace = src.find('{', p);
        const size_t eol = src.find('\n', p);
        if (brace == std::string::npos || brace > eol) continue;   // a sampler or texture, not a block
        const std::string name = trim(src.substr(p + 10, brace - p - 10));
        const ConstantBuffer* cb = nullptr;
        for (const auto& c : refl.psConstantBuffers)
            if (same_res_name(name, c.name)) { cb = &c; break; }
        const size_t end = src.find("\n};", brace);
        if (!cb || end == std::string::npos) {
            fprintf(stderr, "[CB-VK] block %s: no reflection, left as std140\n", name.c_str());
            continue;
        }
        std::string body, defines;
        bool ok = true;
        size_t q = brace + 2;
        while (q < end + 1 && ok) {
            const size_t nl = src.find('\n', q);
            std::string m = trim(src.substr(q, nl - q));
            q = nl + 1;
            if (m.empty()) continue;
            // "[UNITY_UNIFORM] [precision] type name[N];"
            if (m.back() != ';') { ok = false; break; }
            m.pop_back();
            std::vector<std::string> tok;
            for (size_t a = 0; a < m.size();) {
                while (a < m.size() && isspace((unsigned char)m[a])) a++;
                size_t b = a;
                while (b < m.size() && !isspace((unsigned char)m[b])) b++;
                if (b > a) tok.push_back(m.substr(a, b - a));
                a = b;
            }
            while (!tok.empty() && (tok[0] == "UNITY_UNIFORM" || tok[0] == "highp" || tok[0] == "mediump" ||
                                    tok[0] == "lowp" || tok[0] == "uniform"))
                tok.erase(tok.begin());
            if (tok.size() != 2) { ok = false; break; }
            const std::string type = tok[0];
            std::string var = tok[1], array;
            if (const size_t br = var.find('['); br != std::string::npos) { array = var.substr(br); var.resize(br); }
            const ShaderVar* sv = nullptr;
            for (const auto& v : cb->asVars)
                if (v.name == var) { sv = &v; break; }
            if (!sv) { ok = false; break; }
            const uint32_t off = sv->ui32StartOffset;
            // A matrix outside an array becomes its D3D registers: `vecN name[registers]`. The DXBC reads it one
            // register at a time, and HLSLcc indexes it so (name[i]), but it declares a row_major matrix with the
            // wrong GLSL type: a row_major float3x4 (three 16-byte rows) came out as mat4x3, whose name[i] is a vec3
            // and whose size is 64 bytes, not 48. An array of registers has D3D's stride (std140 rounds every array
            // stride to 16) and the type the code expects, for either orientation.
            if (array.empty() && type.compare(0, 3, "mat") == 0 &&
                (sv->sType.Class == SVC_MATRIX_ROWS || sv->sType.Class == SVC_MATRIX_COLUMNS)) {
                const bool rows = sv->sType.Class == SVC_MATRIX_ROWS;
                const uint32_t regs = rows ? sv->sType.Rows : sv->sType.Columns;
                const uint32_t comps = rows ? sv->sType.Columns : sv->sType.Rows;
                const std::string reg = comps == 1 ? "float" : "vec" + std::to_string(comps);
                body += "\tlayout(offset = " + std::to_string(off) + ") " + reg + " " + var + "[" +
                        std::to_string(regs) + "];\n";
                continue;
            }
            const uint32_t align = array.empty() ? std140_align(type) : 16;
            if (off % align == 0) {
                body += "\tlayout(offset = " + std::to_string(off) + ") " + type + " " + var + array + ";\n";
                continue;
            }
            const VecInfo v = vec_info(type);
            if (!array.empty() || v.comps < 2) { ok = false; break; }   // D3D never misaligns these
            // A vector D3D packed after a scalar: one scalar member per component, the vector rebuilt by name.
            std::string ctor = type + "(";
            for (int c = 0; c < v.comps; c++) {
                const std::string part = var + "_d3d" + std::to_string(c);
                body += "\tlayout(offset = " + std::to_string(off + 4u * c) + ") " + v.scalar + " " + part + ";\n";
                ctor += (c ? ", " : "") + part;
            }
            defines += "#define " + var + " " + ctor + ")\n";
        }
        if (!ok) {
            fprintf(stderr, "[CB-VK] block %s: a member this cannot place; left as std140 (may differ from D3D)\n",
                    name.c_str());
            continue;
        }
        src.replace(brace + 2, end + 1 - (brace + 2), body);
        const size_t close = src.find("\n};", brace) + 3;
        if (!defines.empty()) src.insert(close + 1, defines);
        rewritten++;
    }
    return rewritten;
}

// ---- Texel offsets sized to the sampler -----------------------------------------------------------------------------
// Index (0-based) of the offset argument in each *Offset texture function.
int offset_arg(const std::string& fn) {
    if (fn == "texelFetchOffset" || fn == "textureLodOffset") return 3;
    if (fn == "textureGradOffset") return 4;
    if (fn == "textureOffset" || fn == "textureProjOffset") return 2;
    return -1;
}
int sampler_dims(const std::string& ctor) {
    if (ctor.compare(0, 9, "sampler1D") == 0) return 1;
    if (ctor.compare(0, 9, "sampler2D") == 0) return 2;
    if (ctor.compare(0, 9, "sampler3D") == 0) return 3;
    return 0;
}
// Top-level comma-separated arguments of the call whose '(' is at `open`; returns the index of the matching ')'.
size_t split_args(const std::string& s, size_t open, std::vector<std::pair<size_t, size_t>>& args) {
    int depth = 0;
    size_t start = open + 1;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
            if (--depth == 0) { args.push_back({start, i}); return i; }
        } else if (s[i] == ',' && depth == 1) {
            args.push_back({start, i});
            start = i + 1;
        }
    }
    return std::string::npos;
}

int fix_texel_offsets(std::string& src) {
    int fixed = 0;
    for (const char* fn : {"texelFetchOffset", "textureLodOffset", "textureGradOffset", "textureOffset",
                           "textureProjOffset"}) {
        const std::string f = std::string(fn) + "(";
        for (size_t p = 0; (p = src.find(f, p)) != std::string::npos; p += f.size()) {
            if (p > 0 && is_ident(src[p - 1])) continue;
            std::vector<std::pair<size_t, size_t>> args;
            if (split_args(src, p + f.size() - 1, args) == std::string::npos) continue;
            const int k = offset_arg(fn);
            if (k < 0 || (int)args.size() <= k) continue;
            const int dims = sampler_dims(trim(src.substr(args[0].first, args[0].second - args[0].first)));
            const std::string arg = trim(src.substr(args[k].first, args[k].second - args[k].first));
            if (dims < 1 || arg.compare(0, 4, "ivec") != 0 || arg.size() < 6 || arg[5] != '(') continue;
            if (arg[4] - '0' == dims) continue;
            std::vector<std::pair<size_t, size_t>> comps;
            split_args(arg, 5, comps);
            if ((int)comps.size() < dims) continue;
            std::string repl = dims == 1 ? "int(" : "ivec" + std::to_string(dims) + "(";
            for (int c = 0; c < dims; c++) repl += (c ? "," : "") + arg.substr(comps[c].first, comps[c].second - comps[c].first);
            repl += ")";
            src.replace(args[k].first, args[k].second - args[k].first, " " + repl);
            fixed++;
        }
    }
    return fixed;
}

// ---- Binding maps from the declarations -----------------------------------------------------------------------------
struct Decl { int set, binding; std::string type, name; bool block; };

std::vector<Decl> declarations(const std::string& src) {
    std::vector<Decl> out;
    for (size_t p = src.find("layout(set = "); p != std::string::npos; p = src.find("layout(set = ", p + 13)) {
        if (p > 0 && src[p - 1] != '\n') continue;
        Decl d{};
        if (sscanf(src.c_str() + p, "layout(set = %d, binding = %d", &d.set, &d.binding) != 2) continue;
        const size_t close = src.find(')', p);
        const size_t eol = src.find('\n', p);
        if (close == std::string::npos || close > eol) continue;
        std::vector<std::string> tok;
        std::string rest = src.substr(close + 1, eol - close - 1);
        for (size_t a = 0; a < rest.size();) {
            while (a < rest.size() && (isspace((unsigned char)rest[a]) || rest[a] == ';')) a++;
            size_t b = a;
            while (b < rest.size() && !isspace((unsigned char)rest[b]) && rest[b] != ';') b++;
            if (b > a) tok.push_back(rest.substr(a, b - a));
            a = b;
        }
        if (tok.empty() || tok[0] != "uniform") continue;
        tok.erase(tok.begin());
        while (!tok.empty() && (tok[0] == "highp" || tok[0] == "mediump" || tok[0] == "lowp")) tok.erase(tok.begin());
        if (tok.empty()) continue;
        if (tok.back() == "{") {
            d.block = true;
            d.name = tok[0];
        } else if (tok.size() >= 2) {
            d.type = tok[0];
            d.name = tok[1];
        } else {
            continue;
        }
        out.push_back(d);
    }
    return out;
}

int texture_dim(const std::string& type, const std::string& name, const std::string& src) {
    std::string t = type;
    if (!t.empty() && (t[0] == 'i' || t[0] == 'u')) return TD_INT;
    if (t == "texture3D") return TD_3D;
    if (t == "textureCube") return TD_CUBE;
    if (t == "textureCubeArray") return TD_CUBEARRAY;
    if (t == "texture2DArray") return TD_2DARRAY;
    if (t.compare(0, 9, "texture2D") == 0)
        return src.find("sampler2DShadow(" + name + ",") != std::string::npos ? TD_2DSHADOW : TD_2D;
    return TD_FLOAT;
}

int binding_register(const ShaderInfo& refl, ResourceType want, const std::string& name) {
    for (const auto& rb : refl.psResourceBindings) {
        if (rb.eType != want) continue;
        if (same_res_name(name, rb.name)) return (int)rb.ui32BindPoint;
        if (want == RTYPE_SAMPLER && name == "sampler" + rb.name) return (int)rb.ui32BindPoint;
    }
    return INT32_MIN;
}

void append(std::string& s, int a, int b) {
    if (!s.empty()) s += ",";
    s += std::to_string(a) + "=" + std::to_string(b);
}

char* dup(const std::string& s) {
    if (s.empty()) return nullptr;
    char* p = (char*)malloc(s.size() + 1);
    if (p) memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// One descriptor set per draw, so both stages' declarations must live in set 0 without colliding. HLSLcc numbers
// each shader on its own (constant buffers in set 1, textures and samplers in set 0, from 0 in first-use order), so a
// vertex shader's binding 0 and a pixel shader's binding 0 are different things. Rebase by stage:
//   pixel   textures/samplers 0..31,   constant buffers 32..39
//   vertex  textures/samplers 64..95,  constant buffers 96..103
constexpr int kPsTexBase = 0, kPsCbBase = 32, kVsTexBase = 64, kVsCbBase = 96, kBaseSpan = 32;
int rebase_bindings(std::string& src, bool pixel) {
    int bad = 0;
    for (size_t p = src.find("layout(set = "); p != std::string::npos; p = src.find("layout(set = ", p + 1)) {
        if (p > 0 && src[p - 1] != '\n') continue;
        int set = -1, binding = -1, n = 0;
        if (sscanf(src.c_str() + p, "layout(set = %d, binding = %d%n", &set, &binding, &n) != 2) continue;
        const bool cb = set == 1;
        if ((set != 0 && set != 1) || binding < 0 || binding >= (cb ? 8 : kBaseSpan)) {
            fprintf(stderr, "[VK-SLOT] set %d binding %d outside the rebased range\n", set, binding);
            bad++;
            continue;
        }
        const int base = pixel ? (cb ? kPsCbBase : kPsTexBase) : (cb ? kVsCbBase : kVsTexBase);
        src.replace(p, (size_t)n, "layout(set = 0, binding = " + std::to_string(base + binding));
    }
    return bad;
}

int translate_calc(const void* dxbc, uint32_t len, std::string out[5]) {
    GLSLShader result;
    Diag diag;
    HLSLccSamplerPrecisionInfo precisions;
    GLSLCrossDependencyData deps;
    GlExtensions ext = {};
    ext.ARB_explicit_attrib_location = 1;
    ext.ARB_explicit_uniform_location = 1;
    ext.ARB_shading_language_420pack = 1;
    const unsigned flags = HLSLCC_FLAG_VULKAN_BINDINGS | HLSLCC_FLAG_UNIFORM_BUFFER_OBJECT |
                           HLSLCC_FLAG_KEEP_VARYING_LOCATIONS;
    (void)len;
    if (!TranslateHLSLFromMem((const char*)dxbc, flags, LANG_440, &ext, &deps, precisions, diag, &result) ||
        result.sourceCode.empty())
        return 0;
    std::string& src = result.sourceCode;
    replace_word(src, "mediump", "highp");
    replace_word(src, "lowp", "highp");
    fix_texel_offsets(src);
    match_d3d_offsets(src, result.reflection);
    if (rebase_bindings(src, result.shaderType == 0x8B30 /* GL_FRAGMENT_SHADER */)) return 0;
    if (src.find("gl_Position") != std::string::npos) {
        size_t at = 0;
        for (size_t p = 0; (p = src.find("\n#extension", p)) != std::string::npos; p += 11) at = src.find('\n', p + 1);
        if (!at) at = src.find('\n');
        src.insert(at + 1, "invariant gl_Position;\n");
    }
    std::string texmap, cbmap, texdim, smpmap;
    for (const Decl& d : declarations(src)) {
        if (d.block) {
            const int reg = binding_register(result.reflection, RTYPE_CBUFFER, d.name);
            if (reg == INT32_MIN) fprintf(stderr, "[VK-SLOT] cbuffer '%s' binding %d has NO binding entry\n", d.name.c_str(), d.binding);
            else append(cbmap, d.binding, reg);
        } else if (d.type == "sampler" || d.type == "samplerShadow") {
            int reg = d.name == "hlslcc_dummyPointClamp" ? -1 : binding_register(result.reflection, RTYPE_SAMPLER, d.name);
            if (reg == INT32_MIN) fprintf(stderr, "[VK-SLOT] sampler '%s' binding %d has NO binding entry\n", d.name.c_str(), d.binding);
            else append(smpmap, d.binding, reg);
        } else if (d.type.find("texture") != std::string::npos) {
            const int reg = binding_register(result.reflection, RTYPE_TEXTURE, d.name);
            if (reg == INT32_MIN) fprintf(stderr, "[VK-SLOT] texture '%s' binding %d has NO binding entry\n", d.name.c_str(), d.binding);
            else append(texmap, d.binding, reg);
            append(texdim, d.binding, texture_dim(d.type, d.name, src));
        }
    }
    out[0] = src;
    out[1] = texmap;
    out[2] = cbmap;
    out[3] = texdim;
    out[4] = smpmap;
    return 1;
}

// The translation cache: <BO2_SHADER_CACHE_DIR>/vk_tr_<fnv64 of this file's build time and the DXBC>.txt, five
// "<length or -1>\n<bytes>" records, as bo2-mac's msl_cache. The Android shell sets BO2_SHADER_CACHE_DIR to the app's
// cache directory. No directory, or BO2_TR_CACHE=0: always translate.
// The translator's version: package-game.sh passes a hash of dxbc_to_vk.cpp and HLSLcc (BO2_TR_VERSION), so an APK
// whose translator did not change keeps the cache; without it, the build time (every build starts over).
#if defined(BO2_TR_VERSION)
constexpr const char* kTrVersion = BO2_TR_VERSION;
#else
constexpr const char* kTrVersion = "t" __DATE__ __TIME__;
#endif

std::string tr_tag() {
    std::string t;
    for (const char* p = kTrVersion; *p && t.size() < 12; p++)
        if (isalnum((unsigned char)*p)) t += *p;
    return t;
}

// Once per run, in the background: delete translations made by another translator version (and the older
// vk_tr_<hash>.txt names), which no build will ever read again.
void prune_stale(const std::string& dir, const std::string& keep_prefix) {
#if defined(_WIN32)
    (void)dir; (void)keep_prefix;
#else
    std::thread([dir, keep_prefix] {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        unsigned removed = 0;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.compare(0, 6, "vk_tr_") != 0 || n.compare(0, keep_prefix.size(), keep_prefix) == 0) continue;
            if (remove((dir + "/" + n).c_str()) == 0) removed++;
        }
        closedir(d);
        if (removed) fprintf(stderr, "[VK-SHADER] removed %u stale translations\n", removed);
    }).detach();
#endif
}

std::string cache_path(const void* dxbc, uint32_t len) {
    const char* dir = env("BO2_SHADER_CACHE_DIR");
    if (!dir || !*dir) return std::string();
    static const std::string tag = tr_tag();
    static std::once_flag pruned;
    std::call_once(pruned, [&] { prune_stale(dir, "vk_tr_" + tag + "_"); });
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : std::string("vk1 ") + kTrVersion) { h ^= c; h *= 1099511628211ull; }
    for (uint32_t i = 0; i < len; i++) { h ^= ((const unsigned char*)dxbc)[i]; h *= 1099511628211ull; }
    char nm[64];
    snprintf(nm, sizeof nm, "/vk_tr_%s_%016llx.txt", tag.c_str(), (unsigned long long)h);
    return std::string(dir) + nm;
}

}  // namespace

extern "C" int bo2_dxbc_translate2(const void* dxbc, uint32_t len, char** out_msl, char** out_texmap,
                                   char** out_cbmap, char** out_texdim, char** out_smpmap) {
    char** outs[5] = {out_msl, out_texmap, out_cbmap, out_texdim, out_smpmap};
    for (char** o : outs) if (o) *o = nullptr;
    if (!dxbc || len < 32 || !out_msl) return 0;
    std::string f[5];
    bool has[5] = {};
    static const bool use_cache = env_on("BO2_TR_CACHE", true);
    const std::string path = use_cache ? cache_path(dxbc, len) : std::string();
    bool hit = false;
    if (FILE* fp = path.empty() ? nullptr : fopen(path.c_str(), "rb")) {
        hit = true;
        for (int i = 0; i < 5 && hit; i++) {
            long n;
            if (fscanf(fp, "%ld", &n) != 1 || fgetc(fp) != '\n') { hit = false; break; }
            if (n < 0) continue;
            f[i].resize((size_t)n);
            has[i] = true;
            if (n && fread(&f[i][0], 1, (size_t)n, fp) != (size_t)n) hit = false;
        }
        fclose(fp);
    }
    if (!hit) {
        if (!translate_calc(dxbc, len, f)) return 0;
        for (int i = 0; i < 5; i++) has[i] = !f[i].empty();
        if (!path.empty()) {
            const std::string tmp = path + ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            if (FILE* fp = fopen(tmp.c_str(), "wb")) {
                bool ok = true;
                for (int i = 0; i < 5; i++)
                    ok = ok && fprintf(fp, "%ld\n", has[i] ? (long)f[i].size() : -1L) > 0 &&
                         fwrite(f[i].data(), 1, f[i].size(), fp) == f[i].size();
                ok = (fclose(fp) == 0) && ok;
                if (!ok || rename(tmp.c_str(), path.c_str()) != 0) remove(tmp.c_str());
            }
        }
    }
    if (!has[0]) return 0;
    for (int i = 0; i < 5; i++)
        if (outs[i] && has[i]) *outs[i] = dup(f[i]);
    return 1;
}

// As dxbc_to_msl.cpp: the byte offset of every variable of a named constant buffer, from the reflection (an offset
// computed by hand into an HLSL cbuffer is wrong). Translator-independent; the Metal language is irrelevant here.
extern "C" int bo2_dxbc_cbuffer_layout(const void* dxbc, uint32_t len, const char* cbname, char** out) {
    if (!dxbc || !len || !cbname || !out) return 0;
    *out = nullptr;
    GLSLShader result;
    Diag diag;
    HLSLccSamplerPrecisionInfo precisions;
    GLSLCrossDependencyData deps;
    GlExtensions ext = {};
    if (!TranslateHLSLFromMem((const char*)dxbc, HLSLCC_FLAG_VULKAN_BINDINGS | HLSLCC_FLAG_UNIFORM_BUFFER_OBJECT,
                              LANG_440, &ext, &deps, precisions, diag, &result))
        return 0;
    std::string s;
    for (const auto& cb : result.reflection.psConstantBuffers) {
        if (cb.name != cbname && !same_res_name(cbname, cb.name)) continue;
        for (const auto& v : cb.asVars) {
            if (!s.empty()) s += ",";
            s += v.name + "=" + std::to_string(v.ui32StartOffset);
        }
        break;
    }
    *out = dup(s);
    return *out != nullptr;
}

extern "C" void bo2_msl_free(char* p) { free(p); }

// The vertex inputs, "SEMANTIC=location,...", from HLSLcc's declarations
//     layout(location = 0) in highp vec4 in_POSITION0;
// (the semantic with its index, after "in_"). Null for a shader with no vertex inputs.
extern "C" char* bo2_msl_attributes(const char* src) {
    if (!src) return nullptr;
    std::string out;
    for (const char* p = src; (p = strstr(p, "layout(location = ")) != nullptr; p += 18) {
        if (p > src && p[-1] != '\n') continue;
        int loc = -1;
        if (sscanf(p, "layout(location = %d)", &loc) != 1) continue;
        const char* e = strchr(p, '\n');
        const std::string line(p, e ? (size_t)(e - p) : strlen(p));
        const size_t in = line.find(") in ");
        const size_t nm = line.find(" in_");
        if (in == std::string::npos || nm == std::string::npos) continue;
        size_t b = nm + 4, q = b;
        while (q < line.size() && is_ident(line[q])) q++;
        if (q == b) continue;
        if (!out.empty()) out += ",";
        out += line.substr(b, q - b) + "=" + std::to_string(loc);
    }
    return dup(out);
}
