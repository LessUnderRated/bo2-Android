// DXBC -> Metal Shading Language, via the vendored HLSLcc.
//
// Kept in its own translation unit so the shim does not have to include HLSLcc's
// C++ headers (and so this stays the single place that knows the translator's
// API). See third_party/HLSLcc/README.bo2.md for why HLSLcc and not DXVK/DXMT.
#include "hlslcc.h"
#include "../win32/bo2_getenv.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <mach-o/dyld.h>
#include <pthread.h>

namespace {
// HLSLcc reports parse problems through this interface. Diagnostics are dropped
// unless BO2_MSL_VERBOSE is set: with 2518 shaders, per-shader chatter buries the
// run's real output.
//
// It is also how the BINDING SLOTS the translator chose are recovered.
// BindingSlotAllocator::GetBindingSlot hands out 0, 1, 2, ... in first-request
// order and keeps the D3D register only as a lookup key, so `cbuffer x : b2`
// becomes `[[ buffer(1) ]]` in a shader that does not also use b1. Uploading the
// guest's constant buffer k to Metal buffer k is therefore WRONG for any shader
// whose set is not contiguous from b0 — and the frontend's font vertex shader is
// exactly that case (it reads b0 and b2 only, and its world matrix lives in b2).
class Diag : public HLSLccReflection {
public:
    void OnDiagnostics(const std::string& msg, int line, bool isError) override {
        static const int verbose = bo2_getenv("BO2_MSL_VERBOSE") ? 1 : 0;
        if (verbose || isError)
            fprintf(stderr, "[MSL%s] %s\n", isError ? "-ERR" : "", msg.c_str());
    }
    void OnConstantBufferBinding(const std::string& name, int bindIndex) override {
        cb_slots.emplace_back(name, bindIndex);
    }
    void OnTextureBinding(const std::string& name, int bindIndex, int samplerIndex,
                          bool multisampled, HLSLCC_TEX_DIMENSION dim,
                          bool isUAV) override {
        tex_slots.emplace_back(name, bindIndex);
        // THE DECLARED TEXTURE TYPE, which this used to throw away. Every texture
        // the Metal backend creates is a plain 2D one, but the 3D material
        // shaders declare `texture3d` (modelLightingSampler), `texturecube`
        // (reflectionProbeSampler) and `depth2d` (shadowmapSamplerSpot) — and
        // binding a 2D texture to any of those slots is UNDEFINED in Metal, not
        // merely empty. Keep the dimension so the binder can supply a correctly
        // typed stand-in instead of whatever undefined sampling returns.
        tex_dims.emplace_back(bindIndex, (int)dim);
    }
    std::vector<std::pair<std::string,int>> cb_slots;    // name -> Metal buffer()
    std::vector<std::pair<std::string,int>> tex_slots;   // name -> Metal texture()
    std::vector<std::pair<int,int>> tex_dims;            // Metal texture() -> HLSLCC_TEX_DIMENSION
};

// "metalIndex=d3dRegister,..." for every binding the translator emitted, by
// matching the name it reported against the DXBC resource-binding table.
// HLSLcc mangles a `$Globals` buffer's leading '$' to 'V' or 'F' by shader stage,
// so compare on everything after the first character when either side starts with
// one of those markers.
bool same_res_name(const std::string& msl, const std::string& dxbc) {
    if (msl == dxbc) return true;
    if (dxbc.size() == msl.size() && !dxbc.empty() && dxbc[0] == '$')
        return dxbc.compare(1, std::string::npos, msl, 1, std::string::npos) == 0;
    return false;
}

int emit_slot_map(const std::vector<std::pair<std::string,int>>& reported,
                  const ShaderInfo& refl, ResourceType want, char** out) {
    std::string s;
    static const int verbose = bo2_getenv("BO2_MSL_DUMP") ? 1 : 0;
    for (const auto& r : reported) {
        bool matched = false;
        for (const auto& rb : refl.psResourceBindings) {
            if (rb.eType != want || !same_res_name(r.first, rb.name)) continue;
            if (!s.empty()) s += ",";
            s += std::to_string(r.second) + "=" + std::to_string(rb.ui32BindPoint);
            matched = true;
            break;
        }
        // A name the translator reported but the binding table does not carry is
        // a HOLE in the map, and a hole silently becomes the identity downstream —
        // which is the same wrong answer we are trying to detect. Say so.
        if (!matched)
            fprintf(stderr, "[MSL-SLOT] %s '%s' -> metal %d has NO binding entry\n",
                    want == RTYPE_TEXTURE ? "texture" : "cbuffer",
                    r.first.c_str(), r.second);
        else if (verbose)
            fprintf(stderr, "[MSL-SLOT] %s '%s' metal %d\n",
                    want == RTYPE_TEXTURE ? "texture" : "cbuffer",
                    r.first.c_str(), r.second);
    }
    if (s.empty()) return 0;
    char* p = (char*)malloc(s.size() + 1);
    if (!p) return 0;
    memcpy(p, s.c_str(), s.size() + 1);
    *out = p;
    return 1;
}

// D3D packs a cbuffer member right after the previous one unless it would cross
// a 16-byte boundary; Metal aligns float2 to 8 and float3 to 16 (size 16). HLSLcc
// declares the natural Metal types (Unity repacks on the CPU side, we upload the
// game's bytes as-is), so `float a; float2 b;` puts b at 24 where the game wrote
// it at 20 — the air-support gun-cam filter of angola read StaticScale.y from the
// padding and drew its noise in vertical bands. Give each member its D3D offset,
// taken from the reflection: packed_floatN where the natural alignment or size
// disagrees, padding where D3D left a hole. A member this cannot place (struct,
// array of scalars) ends the rewrite of its buffer, untouched from there on.
void match_d3d_offsets(std::string& msl, const ShaderInfo& refl) {
    for (const auto& cb : refl.psConstantBuffers) {
        size_t st = std::string::npos;
        for (size_t p = msl.find("struct "); p != std::string::npos; p = msl.find("struct ", p + 7)) {
            size_t e = msl.find("_Type\n{\n", p);
            if (e == std::string::npos) break;
            std::string name = msl.substr(p + 7, e - p - 7);
            if (name.find_first_of(" \n") == std::string::npos && same_res_name(name, cb.name)) {
                st = e + 8;
                break;
            }
        }
        if (st == std::string::npos) continue;
        size_t end = msl.find("};", st);
        if (end == std::string::npos) continue;
        std::string body;
        uint32_t cur = 0;
        bool changed = false, stop = false;
        size_t p = st;
        while (p < end) {
            size_t nl = msl.find('\n', p);
            std::string line = msl.substr(p, nl - p + 1);
            p = nl + 1;
            if (stop) { body += line; continue; }
            // "    float2 StaticScale;\n"
            char ty[64], nm[128];
            unsigned arr = 0;
            int nf = sscanf(line.c_str(), " %63s %127[A-Za-z0-9_][%u]", ty, nm, &arr);
            const ShaderVar* v = nullptr;
            size_t k = 0;
            for (; nf >= 2 && k < cb.asVars.size(); k++)
                if (cb.asVars[k].name == nm) { v = &cb.asVars[k]; break; }
            if (!v) { stop = true; body += line; continue; }
            const uint32_t off = v->ui32StartOffset;
            const uint32_t next = k + 1 < cb.asVars.size() ? cb.asVars[k + 1].ui32StartOffset
                                                           : cb.ui32TotalSizeInBytes;
            // Natural Metal size/alignment of the declared type.
            std::string t = ty;
            size_t b = t.find_first_of("0123456789");
            std::string base = t.substr(0, b);
            unsigned c = 1, r = 1;
            if (b != std::string::npos) sscanf(t.c_str() + b, "%ux%u", &c, &r);
            const bool scalar_base = base == "float" || base == "int" || base == "uint";
            const bool matrix = t.find('x', b == std::string::npos ? t.size() : b) != std::string::npos;
            if (!scalar_base || (arr && !(c == 4 || matrix))) { stop = true; body += line; continue; }
            auto vec = [](unsigned n) { return n == 2 ? 8u : n == 1 ? 4u : 16u; };
            uint32_t align = matrix ? vec(r) : vec(c);
            uint32_t size = matrix ? c * vec(r) : vec(c);
            if (arr) size *= arr;
            if (!matrix && !arr && (c == 2 || c == 3) &&
                (off % align != 0 || off + size > next)) {
                line.replace(line.find(t), t.size(), "packed_" + t);
                align = 4;
                size = 4 * c;
                changed = true;
            }
            // Padding only where Metal's own alignment does not already land there.
            if ((cur + align - 1) / align * align < off) {
                body += "    float _d3d_pad" + std::to_string(cur) + "[" +
                        std::to_string((off - cur) / 4) + "];\n";
                cur = off;
                changed = true;
            }
            if ((cur + align - 1) / align * align != off) { stop = true; body += line; continue; }
            body += line;
            cur = off + size;
        }
        if (!changed) continue;
        static int nlog = 0;
        if (++nlog <= 40)
            fprintf(stderr, "[CB-D3D] %s : membres replacés sur leurs offsets D3D\n", cb.name.c_str());
        msl.replace(st, end - st, body);
    }
}
}  // namespace

// Translate once and hand back everything the backend needs: the MSL source, the
// Metal-texture-index -> D3D-t-register map, and the Metal-buffer-index ->
// D3D-b-register map. One translation, because doing it twice (as the separate
// texture-slot entry point used to) doubles the cost of the most expensive
// per-shader work in the run. Every out-param is optional; each is malloc'd and
// freed with bo2_msl_free.
// P805 : "metalSampler=d3dSamplerRegister,..." read off the declarations HLSLcc emits,
//     sampler samplerNAME [[ sampler (N) ]]
// where NAME is the DXBC sampler's name, prefixed with "sampler" unless it already began with it
// (toMetalDeclaration.cpp, OPCODE_DCL_SAMPLER). The slot N is HLSLcc's own numbering, not the s register.
static void emit_sampler_map(const std::string& msl, const ShaderInfo& refl, char** out) {
    std::string s;
    for (size_t p = 0; (p = msl.find("[[ sampler (", p)) != std::string::npos; p += 12) {
        int idx = atoi(msl.c_str() + p + 12);
        size_t e = p;
        while (e > 0 && (msl[e - 1] == ' ' || msl[e - 1] == '\t')) e--;
        size_t b = e;
        while (b > 0 && (isalnum((unsigned char)msl[b - 1]) || msl[b - 1] == '_')) b--;
        const std::string name = msl.substr(b, e - b);
        bool matched = false;
        for (const auto& rb : refl.psResourceBindings) {
            if (rb.eType != RTYPE_SAMPLER || (name != rb.name && name != "sampler" + rb.name)) continue;
            if (!s.empty()) s += ",";
            s += std::to_string(idx) + "=" + std::to_string(rb.ui32BindPoint);
            matched = true;
            break;
        }
        if (!matched)
            fprintf(stderr, "[MSL-SLOT] sampler '%s' -> metal %d has NO binding entry\n", name.c_str(), idx);
    }
    if (s.empty()) return;
    char* q = (char*)malloc(s.size() + 1);
    if (q) { memcpy(q, s.c_str(), s.size() + 1); *out = q; }
}

// 10-21 : HLSLcc écrit le décalage d'une lecture de texel (`ld` aoffimmi) en GLSL, `coord + ivec2(dx, dy)`
// (toMetalInstruction.cpp, TranslateTexelFetchOffset) : Metal rejette le nuanceur (« undeclared identifier 'ivec2' »,
// PS 42b2e144 du multi) et ses tracés disparaissent. La coordonnée est un uint : `uint2(int2(dx, dy))`, l'addition
// modulo 2^32 donne le même texel que l'entier signé de D3D.
static void fix_ivec_offsets(std::string& msl) {
    for (const char* n : {"2", "3"}) {
        const std::string from = std::string("ivec") + n + "(";
        const std::string to = std::string("uint") + n + "(int" + n + "(";
        for (size_t p = 0; (p = msl.find(from, p)) != std::string::npos;) {
            const size_t close = msl.find(')', p);
            if (close == std::string::npos) break;
            msl.insert(close, ")");
            msl.replace(p, from.size(), to);
            p += to.size();
        }
    }
}

static int translate2_calc(const void* dxbc, uint32_t len, char** out_msl,
                           char** out_texmap, char** out_cbmap,
                           char** out_texdim, char** out_smpmap) {
    if (!dxbc || !len || !out_msl) return 0;
    *out_msl = nullptr;
    if (out_texmap) *out_texmap = nullptr;
    if (out_cbmap)  *out_cbmap  = nullptr;
    if (out_texdim) *out_texdim = nullptr;
    if (out_smpmap) *out_smpmap = nullptr;
    GLSLShader result;
    Diag diag;
    HLSLccSamplerPrecisionInfo precisions;
    GLSLCrossDependencyData deps;
    // HLSLcc reads the bytecode straight out of the buffer; flags 0 = defaults.
    int ok = TranslateHLSLFromMem((const char*)dxbc, 0, LANG_METAL, nullptr,
                                  &deps, precisions, diag, &result);
    if (!ok || result.sourceCode.empty()) return 0;
    match_d3d_offsets(result.sourceCode, result.reflection);
    fix_ivec_offsets(result.sourceCode);
    // P854 : sous D3D, deux shaders qui calculent la même position tombent au même z ; Metal (calcul rapide)
    // peut les contracter autrement. [[invariant]] + preserveInvariance à la compilation rendent la garantie.
    // Ce n'était PAS la cause des décalques coupés (cratères, sang) : c'était le biais de profondeur (BO2_DEPTH_BIAS).
    static const std::string pos = "float4 mtl_Position [[ position ]];";
    const size_t at = result.sourceCode.find(pos);
    if (at != std::string::npos)
        result.sourceCode.replace(at, pos.size(), "float4 mtl_Position [[ position, invariant ]];");
    char* p = (char*)malloc(result.sourceCode.size() + 1);
    if (!p) return 0;
    memcpy(p, result.sourceCode.c_str(), result.sourceCode.size() + 1);
    *out_msl = p;
    if (out_texmap)
        emit_slot_map(diag.tex_slots, result.reflection, RTYPE_TEXTURE, out_texmap);
    if (out_cbmap)
        emit_slot_map(diag.cb_slots, result.reflection, RTYPE_CBUFFER, out_cbmap);
    if (out_smpmap)
        emit_sampler_map(result.sourceCode, result.reflection, out_smpmap);
    // "metalIndex=HLSLCC_TEX_DIMENSION,..." — no reflection lookup needed, the
    // translator reports the dimension it emitted the declaration with.
    if (out_texdim && !diag.tex_dims.empty()) {
        std::string s;
        for (const auto& d : diag.tex_dims) {
            if (!s.empty()) s += ",";
            s += std::to_string(d.first) + "=" + std::to_string(d.second);
        }
        char* q = (char*)malloc(s.size() + 1);
        if (q) { memcpy(q, s.c_str(), s.size() + 1); *out_texdim = q; }
    }
    return 1;
}

// 10-21 : la traduction se faisait à chaque lancement, au premier bind, sur le fil de rendu : jusqu'à ~190 ms
// sans image en pleine partie ([A-COUP], multi, run 132549). Ses cinq sorties sont gardées dans
// <dossier du binaire>/msl_cache/tr_<fnv64 du DXBC>.txt ; la clé contient l'heure de compilation de ce fichier, donc
// un changement de la traduction ne relit jamais un ancien résultat. BO2_TR_CACHE=0 : retraduit toujours.
namespace {
std::string tr_cache_path(const void* dxbc, uint32_t len) {
    static const std::string dir = [] {
        char exe[4096]; uint32_t n = sizeof exe;
        if (_NSGetExecutablePath(exe, &n) != 0) return std::string();
        std::string d(exe);
        d.resize(d.rfind('/') + 1);
        return d + "msl_cache/";
    }();
    if (dir.empty()) return dir;
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : std::string(__DATE__ " " __TIME__)) { h ^= c; h *= 1099511628211ull; }
    for (uint32_t i = 0; i < len; i++) { h ^= ((const unsigned char*)dxbc)[i]; h *= 1099511628211ull; }
    char nm[40]; snprintf(nm, sizeof nm, "tr_%016llx.txt", (unsigned long long)h);
    return dir + nm;
}
char* tr_dup(const std::string& s) { char* p = (char*)malloc(s.size() + 1); memcpy(p, s.c_str(), s.size() + 1); return p; }
}  // namespace

extern "C" int bo2_dxbc_translate2(const void* dxbc, uint32_t len, char** out_msl,
                                   char** out_texmap, char** out_cbmap,
                                   char** out_texdim, char** out_smpmap) {
    static const bool on = bo2_env_on("BO2_TR_CACHE", true);
    if (!on || !dxbc || !len || !out_msl) return translate2_calc(dxbc, len, out_msl, out_texmap, out_cbmap,
                                                                  out_texdim, out_smpmap);
    char** outs[5] = {out_msl, out_texmap, out_cbmap, out_texdim, out_smpmap};
    for (char** o : outs) if (o) *o = nullptr;
    const std::string path = tr_cache_path(dxbc, len);
    std::string f[5]; bool has[5] = {};
    bool hit = false;
    if (FILE* fp = path.empty() ? nullptr : fopen(path.c_str(), "rb")) {   // 5 × "<longueur ou -1>\n<octets>"
        hit = true;
        for (int i = 0; i < 5 && hit; i++) {
            long n;
            if (fscanf(fp, "%ld", &n) != 1 || fgetc(fp) != '\n') { hit = false; break; }
            if (n < 0) continue;
            f[i].resize((size_t)n); has[i] = true;
            if (fread(&f[i][0], 1, (size_t)n, fp) != (size_t)n) hit = false;
        }
        fclose(fp);
    }
    if (!hit) {
        char* r[5] = {};
        if (!translate2_calc(dxbc, len, &r[0], &r[1], &r[2], &r[3], &r[4])) return 0;
        for (int i = 0; i < 5; i++) { has[i] = r[i] != nullptr; if (r[i]) f[i] = r[i]; free(r[i]); }
        const std::string tmp = path + ".tmp" + std::to_string((unsigned long)pthread_self());
        if (FILE* fp = path.empty() ? nullptr : fopen(tmp.c_str(), "wb")) {
            bool ok = true;
            for (int i = 0; i < 5; i++)
                ok = ok && fprintf(fp, "%ld\n", has[i] ? (long)f[i].size() : -1L) > 0
                        && fwrite(f[i].data(), 1, f[i].size(), fp) == f[i].size();
            ok = (fclose(fp) == 0) && ok;
            if (!ok || rename(tmp.c_str(), path.c_str()) != 0) remove(tmp.c_str());
        }
    }
    if (!has[0] || f[0].empty()) return 0;
    for (int i = 0; i < 5; i++) if (outs[i] && has[i]) *outs[i] = tr_dup(f[i]);
    return 1;
}

// The BYTE OFFSET of a named variable inside a named constant buffer, straight
// out of the reflection table (ShaderVar::ui32StartOffset).
//
// This exists because of a rule this project has already paid for: AN OFFSET
// COMPUTED BY HAND INTO AN HLSL CBUFFER IS WRONG. Packing is not sequential —
// a float3 still occupies a 16-byte slot, arrays round up, and a float2 may or
// may not share one — so counting struct members off the translated MSL gives a
// number that looks plausible and points at the wrong floats. The translator
// already knows the real answer; ask it.
//
// Writes "name=offset,name=offset,..." for every variable of the buffer.
// Returns 0 if the shader has no such buffer.
extern "C" int bo2_dxbc_cbuffer_layout(const void* dxbc, uint32_t len,
                                       const char* cbname, char** out) {
    if (!dxbc || !len || !cbname || !out) return 0;
    *out = nullptr;
    GLSLShader result;
    Diag diag;
    HLSLccSamplerPrecisionInfo precisions;
    GLSLCrossDependencyData deps;
    if (!TranslateHLSLFromMem((const char*)dxbc, 0, LANG_METAL, nullptr,
                              &deps, precisions, diag, &result))
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
    if (s.empty()) return 0;
    char* p = (char*)malloc(s.size() + 1);
    if (!p) return 0;
    memcpy(p, s.c_str(), s.size() + 1);
    *out = p;
    return 1;
}

// The original three-out-param form, so existing callers need no change.
extern "C" int bo2_dxbc_translate(const void* dxbc, uint32_t len, char** out_msl,
                                  char** out_texmap, char** out_cbmap) {
    return bo2_dxbc_translate2(dxbc, len, out_msl, out_texmap, out_cbmap, nullptr, nullptr);
}

// Kept for callers that only want the source.
extern "C" int bo2_dxbc_to_msl(const void* dxbc, uint32_t len, char** out_msl) {
    return bo2_dxbc_translate(dxbc, len, out_msl, nullptr, nullptr);
}

extern "C" void bo2_msl_free(char* p) { free(p); }

// The vertex-attribute mapping a MTLVertexDescriptor needs, read straight out of
// the generated MSL rather than through HLSLcc's reflection API: the translator
// emits one line per input,
//     float4 POSITION0 [[ attribute(0) ]] ;
// so the semantic (name + index, already concatenated by HLSLcc) and the Metal
// attribute number are both right there. Returns a comma-separated
// "POSITION0=0,COLOR0=1,TEXCOORD0=2" in a malloc'd string, or null if the shader
// declares no inputs (a pixel shader, or a VS that only uses SV_ values).
extern "C" char* bo2_msl_attributes(const char* msl) {
    if (!msl) return nullptr;
    std::string out;
    const char* p = msl;
    while ((p = strstr(p, "[[ attribute(")) != nullptr) {
        // Walk back over " NAME " to the start of the semantic token.
        const char* q = p;
        while (q > msl && (q[-1] == ' ' || q[-1] == '\t')) q--;
        const char* end = q;
        while (q > msl && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) q--;
        std::string name(q, end);
        int idx = atoi(p + strlen("[[ attribute("));
        if (!name.empty()) {
            if (!out.empty()) out += ",";
            out += name; out += "="; out += std::to_string(idx);
        }
        p += 13;
    }
    if (out.empty()) return nullptr;
    char* r = (char*)malloc(out.size() + 1);
    if (r) memcpy(r, out.c_str(), out.size() + 1);
    return r;
}
