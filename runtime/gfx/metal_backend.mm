// Metal presentation backend — Gate B, step 1 ("first light").
//
// The port's D3D layer is entirely stubbed (fake-COM vtables, DrawIndexed is a
// no-op), so nothing has ever reached a screen. This is the smallest piece that
// proves the end-to-end presentation path can exist alongside the guest:
//
//   real process main thread  ->  NSApp + CAMetalLayer, pumped here
//   1 GB-stack worker pthread ->  the whole cooperative guest scheduler
//
// That split is already how win32_run_main_thread is built (the main thread just
// blocked in pthread_join), which is why this can be added without disturbing the
// guest's threading model at all. AppKit REQUIRES the true main thread, so the
// window must live here and never on a guest thread.
//
// Everything is behind BO2_WINDOW=1 so the default build stays byte-identical to
// the verified headless baseline.
//
// Step 1 (this file): open a window, clear it, present, stay responsive while the
// guest runs. Step 2 will feed it the real vertex/index/texture data the software
// capture (BO2_GEO_DUMP) already decodes correctly out of guest memory.

#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "../win32/bo2_getenv.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <chrono>
#include <string>
#include <algorithm>
#include <csignal>
#include <set>
#include <tuple>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

#include <atomic>
#include <cstdio>
// The guest memory accessor lives in the win32 shim; this file does not include
// its header, so declare the single function [FRAME-HANDSHAKE] needs.
extern "C" uint32_t win32_mem_read32(uint32_t);
// Prints [IMP-HIST]/[IMP-SITE]: which import, and which code, is looping.
extern "C" void win32_imp_hist_report(const char* why);
// Prints [CADENCE]: the engine's own heartbeats (server tick, script VM, imports)
// as absolutes AND deltas. Must be called from here — this is the one sampler on
// the HOST clock, so it keeps reporting when every guest counter has gone still.
extern "C" void win32_cadence_report(unsigned long long host_presents);
// Prints [MT-TREE]: whether the structure sub_87bd70 descends contains a cycle,
// which is the one way that descent can never terminate. Read-only, host-side.
extern "C" void win32_mt_tree_audit(const char* why);
// Prints [MT-SPIN]: the spinning guest thread's own eax/ecx, and whether they
// moved since the last sample — the walk observed rather than the structure.
extern "C" void win32_mt_spin_report(int tid);
extern "C" void win32_unlink_probe(int tid);
// 🎯 [IMP-HIST] at the angola stall: 100.0% of the 2,265,248 calls since the
// last presented frame are Enter/LeaveCriticalSection, perfectly paired.
// BO2_CS_WHO names the return address and the object being hammered.
extern "C" void win32_cs_report(void);
extern "C" void win32_alloc_who_report(void);
extern "C" void win32_slget_report(void);
extern "C" void win32_intern_report(void);
extern "C" void win32_slfree_who_report(void);
extern "C" void win32_drain_count_report(void);
extern "C" void win32_deq_report();
extern "C" void win32_freelist_report();
extern "C" void win32_gsc_loop_report();
extern "C" void win32_cast_report();
extern "C" void win32_obj_hist_report();
extern "C" void win32_err_catalogue_report();
extern "C" void win32_sl_band_report();
extern "C" void win32_mt_inflight_report();
extern "C" void win32_all_regs_report();
extern "C" void win32_spin_histogram(const char*);
extern "C" void win32_svframe_gate_report(void);
extern "C" void win32_frame_budget_report(void);
extern "C" void win32_iter_report();
extern "C" void win32_stack_census(const char* why);
extern "C" void win32_gsc_teardown_report();
extern "C" void win32_gsc_teardown_guard();
extern "C" void win32_skip_teardown_poll();
extern "C" void win32_gsc_chain_audit(const char* why);
extern "C" void win32_gsc_find_report();
extern "C" void win32_clamp_verdict(const char* why);
extern "C" void win32_pvar_census(const char* why);
extern "C" void win32_gsc_sibling_audit(const char* why);
// Prints [DEADLOCK-DUMP]: every guest thread's status, stack and GPRs.
extern "C" void win32_dump_all_threads();
// Prints [SPIN]: the symbolised stack of the guest scheduler thread. The one
// instrument that can see a guest looping inside a single lifted block.
extern "C" void win32_name_the_spin(const char* why);
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

NSWindow*                  g_window = nil;
static const bool          g_titre_nu = [] { const char* e = bo2_getenv("BO2_TITRE"); return e && !strcmp(e, "0"); }();
CAMetalLayer*              g_layer  = nil;
id<MTLDevice>              g_device = nil;
id<MTLCommandQueue>        g_queue  = nil;
id<MTLRenderPipelineState> g_pipe   = nil;
static id<MTLFunction> g_copy_vs = nil, g_copy_fs = nil;   // g_pipe's functions, for copy_pipe_for
id<MTLSamplerState>        g_samp   = nil;
id<MTLTexture>             g_show   = nil;      // texture currently displayed
std::atomic<bool>          g_open{false};
// P773 : rafraîchissement de l'écran de la fenêtre, pour Present(SyncInterval) (remaining_dlls_impl.cpp).
static std::atomic<double> g_refresh_hz{60.0};
extern "C" double gfx_refresh_hz() { return g_refresh_hz.load(); }
std::atomic<uint64_t>      g_frames{0};

// Textures arrive on the GUEST thread (from the stubbed CreateTexture2D), but
// MTLTexture creation and drawing belong to the main thread's pump. So the guest
// side only copies bytes into this staging slot; the pump turns it into a
// texture. Keeping the biggest one seen makes the atlas win over the little
// 4x4/128x128 helper images without needing any policy from the caller.
struct Pending {
    std::vector<uint8_t> bytes;
    uint32_t w = 0, h = 0, fmt = 0, pitch = 0;
    bool     ready = false;
};
std::mutex g_tex_mu;
Pending    g_pending;
uint64_t   g_best_pixels = 0;

// DXGI format -> Metal. Only what the frontend actually uploads.
MTLPixelFormat dxgi_to_metal(uint32_t fmt) {
    switch (fmt) {
        case 28: return MTLPixelFormatRGBA8Unorm;       // R8G8B8A8_UNORM
        case 29: return MTLPixelFormatRGBA8Unorm_sRGB;
        case 87: return MTLPixelFormatBGRA8Unorm;       // B8G8R8A8_UNORM
        case 88: return MTLPixelFormatBGRA8Unorm_sRGB;
        case 61: return MTLPixelFormatR8Unorm;
        case 65: return MTLPixelFormatA8Unorm;
        case 71: case 72: return MTLPixelFormatBC1_RGBA;
        case 74: case 75: return MTLPixelFormatBC2_RGBA;
        case 77: case 78: return MTLPixelFormatBC3_RGBA;  // DXT5 — the scene textures
        case 80: case 81: return MTLPixelFormatBC4_RUnorm;
        case 83: case 84: return MTLPixelFormatBC5_RGUnorm;
        default: return MTLPixelFormatInvalid;
    }
}

bool is_block_compressed(MTLPixelFormat f) {
    return f == MTLPixelFormatBC1_RGBA || f == MTLPixelFormatBC2_RGBA ||
           f == MTLPixelFormatBC3_RGBA || f == MTLPixelFormatBC4_RUnorm ||
           f == MTLPixelFormatBC5_RGUnorm;
}

// Fullscreen textured quad. Aspect-fit via a scale pushed from the CPU, so a
// portrait atlas (2048x3072) is shown undistorted rather than stretched.
const char* kShaderSrc = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut v_main(uint vid [[vertex_id]], constant float4& xf [[buffer(0)]]) {
    float2 p[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };
    float2 t[4] = { float2(0,1),   float2(1,1),  float2(0,0),  float2(1,0)  };
    VOut o;
    // xf = (scale.x, scale.y, offset.x, offset.y) in NDC — an offset is what the
    // render-target tiler needs, and per-tile viewports did not give it a usable
    // 3x3 (the tiles came out as two full-width bands).
    o.pos = float4(p[vid] * xf.xy + xf.zw, 0, 1);
    o.uv  = t[vid];
    return o;
}
fragment float4 f_main(VOut in [[stage_in]],
                       texture2d<float> tex [[texture(0)]],
                       sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv);
}
)";

// --- Gate B step 3: the LIVE 2D UI pass -------------------------------------
// The guest's 2D UI draws carry everything a real backend needs: positions in
// VIEWPORT PIXELS, a per-vertex RGBA dword, UVs, and a bound texture. The D3D
// shim decodes them at DrawIndexed time and hands one finished frame over here;
// this pipeline maps pixels to NDC with the viewport size and blends the result
// over whatever the background pass produced.
struct UiVert { float x, y, z, u, v; uint8_t r, g, b, a; };
struct UiBatch {                       // tex = guest texture object id
    uint32_t first, count, tex;
    uint32_t has_mtx;                  // 0 = positions already in viewport pixels
    float    mtx[16];                  // row-major worldViewProjectionMatrix
};

std::mutex               g_ui_mu;
std::vector<UiVert>      g_ui_verts;      // last complete frame, guest-side order
std::vector<UiBatch>     g_ui_batches;
float                    g_ui_vw = 1920.f, g_ui_vh = 1080.f;
// The backbuffer size the DXGI shim advertises to the guest. Screen
// viewports are expressed in it and have to be mapped onto the drawable.
float                    g_guest_bb_w = 1920.f, g_guest_bb_h = 1080.f;
bool                     g_ui_ready = false;
uint64_t                 g_ui_frames = 0;

// Textures the guest uploaded, keyed by its own object id so a batch can name
// the image it samples. Staged on the guest thread, promoted on the pump.
// `depth` > 1 means a 3D texture, `faces` == 6 a cube. Both were previously
// impossible to express: every texture the backend created was 2D, so the
// material shaders' texture3d (baked lighting volume) and texturecube
// (reflection probe) slots could only ever be served by a stand-in.
struct TexStage { std::vector<uint8_t> bytes; uint32_t w, h, fmt, pitch;
                  uint32_t depth = 1, faces = 1, slice_pitch = 0;
                  uint32_t mips = 1;                       // cube mip chain, if supplied
                  std::vector<uint32_t> mip_off, mip_pitch; };
std::mutex                                        g_texreg_mu;
std::vector<std::pair<uint32_t, TexStage>>        g_texreg_pending;
std::unordered_map<uint32_t, id<MTLTexture>>      g_texreg;
// Dimensions of every registered texture, so a diagnostic can say whether a draw
// sampled a real image or a 1x1 solid — the difference between "the menu art is
// missing" and "this fill is exactly what the engine asked for".
struct TexInfo { uint32_t w, h, fmt; uint8_t px[4]; };
std::unordered_map<uint32_t, TexInfo>            g_texinfo;
// Textures que D3D a DETRUITES (refcount modelise a zero, cote shim), chacune avec
// le numero de la premiere image publiee qui a ete enregistree tout entiere APRES
// la destruction : la copie Metal ne part que quand la pompe encode une image au
// moins aussi recente (voir gfx_release_texture). Sous g_texreg_mu.
std::vector<std::pair<uint32_t, uint64_t>>        g_texrel_pending;
// Pompe seulement. `gone` sert au temoin : un dessin qui demande une texture deja
// rendue (tex_tard dans [PRESENT]) veut dire que le modele a vu un zero trop tot.
std::unordered_set<uint32_t>                      g_texrel_gone;
uint64_t g_texrel_rendues = 0, g_texrel_tard = 0;

// Translated shaders: staged as MSL on the guest thread, compiled by the pump.
struct ShaderStage { uint32_t obj; bool is_pixel; std::string msl; std::string attrs; };
std::mutex                                     g_shader_mu;
std::vector<ShaderStage>                       g_shader_pending;
std::vector<std::pair<uint32_t,std::string>>   g_texslot_pending;
std::vector<std::pair<uint32_t,std::string>>   g_cbslot_pending;
std::vector<std::pair<uint32_t,std::string>>   g_texdim_pending;
std::vector<std::pair<uint32_t,std::string>>   g_smpslot_pending;                // P805
std::vector<std::pair<uint32_t,id<MTLSamplerState>>> g_sampler_pending;          // P805
std::unordered_map<uint32_t, id<MTLSamplerState>> g_sampreg;   // P805 : pompe seulement, objet invite -> etat
uint64_t g_smp_jeu = 0, g_smp_defaut = 0, g_smp_inconnu = 0;   // P805 : temoin [SAMPLER]
std::unordered_map<uint32_t, id<MTLFunction>>  g_shader_fn;   // kept for the fixed path
uint64_t g_msl_compiled = 0, g_msl_compile_failed = 0;
std::atomic<uint64_t> g_msl_from_cache{0};

// Notre cache de shaders (P770). Le cache Metal de macOS est partagé et tourne par générations : quand il jette
// celle d'un niveau, ses ~400 shaders repassent par le compilateur (144 ms chacun), la présentation gèle 40-70 s.
// Sous Windows le pilote compile en quelques ms. Ici : <dossier de boot>/msl_cache/<fnv64>.metallib, produit hors
// run par scripts/msl_cache.sh à partir du <fnv64>.msl que le runtime dépose quand il compile une source.
static uint64_t msl_fnv64(const std::string& msl) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : msl) { h ^= c; h *= 1099511628211ull; }
    return h;
}
static NSString* msl_cache_path(const std::string& msl, const char* ext) {
    static NSString* dir = [[[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent]
                               stringByAppendingPathComponent:@"msl_cache"];
    return [dir stringByAppendingPathComponent:
               [NSString stringWithFormat:@"%016llx.%s", (unsigned long long)msl_fnv64(msl), ext]];
}

// Shaders par lots (11-04). Sans xcrun metal (un joueur sans Xcode) aucun metallib n'existe : tout passe par
// newLibraryWithSource, ~0,44 s de CPU par shader à froid, dont ~0,37 s pour lire <metal_stdlib>. Un lot de N sources,
// chacune dans son namespace (sK::xlatMtlMain), ne lit l'en-tête qu'une fois : 40 shaders du multi, 3,0 s au lieu de
// 17,4 s sur un cœur. Mesuré sur toutes les sources des trois modes : HLSLcc n'écrit que deux #define, identiques
// partout ; une source qui en aurait un autre (il fuirait dans les suivantes) est compilée seule.
static const std::string kMslTete = "#include <metal_stdlib>\n#include <metal_texture>\nusing namespace metal;\n";
static MTLCompileOptions* msl_opts() {
    static MTLCompileOptions* o = [] { MTLCompileOptions* x = [MTLCompileOptions new]; x.preserveInvariance = YES; return x; }();   // P854
    return o;
}
static bool msl_groupable(const std::string& s) {
    if (s.compare(0, kMslTete.size(), kMslTete)) return false;
    for (size_t p = 0; (p = s.find("#define", p)) != std::string::npos; p += 7) {
        const std::string l = s.substr(p, s.find('\n', p) - p);
        if (l != "#define fma(a,b,c) ((a) * (b) + (c))" && l != "#define XLT_REMAP_O {0, 1, 2, 3, 4, 5, 6, 7}") return false;
    }
    return true;
}
// fn[i] = xlatMtlMain de *src[i] : un seul lot si possible, sinon (ou si le lot échoue) source par source.
static void msl_compile(const std::string* const* src, size_t n, __strong id<MTLFunction>* fn, std::string* why) {
    bool lot = n > 1;
    for (size_t i = 0; i < n && lot; i++) lot = msl_groupable(*src[i]);
    if (lot) {
        std::string s = kMslTete;
        for (size_t i = 0; i < n; i++)
            s += "namespace s" + std::to_string(i) + " {\n" + src[i]->substr(kMslTete.size()) + "\n}\n";
        id<MTLLibrary> l = [g_device newLibraryWithSource:@(s.c_str()) options:msl_opts() error:nil];
        for (size_t i = 0; l && i < n; i++)
            fn[i] = [l newFunctionWithName:[NSString stringWithFormat:@"s%zu::xlatMtlMain", i]];
    }
    for (size_t i = 0; i < n; i++) {
        if (fn[i]) continue;
        NSError* e = nil;
        fn[i] = [[g_device newLibraryWithSource:@(src[i]->c_str()) options:msl_opts() error:&e]
                    newFunctionWithName:@"xlatMtlMain"];
        if (!fn[i] && why) why[i] = e ? [[e localizedDescription] UTF8String] : "no function";
    }
}

// Au lancement, en tâche de fond pendant le menu : les shaders des parties précédentes qui n'ont pas de metallib
// (msl_cache/<h>.msl seul) sont compilés par lots ; la pompe les prend ici au lieu de les compiler au chargement de la
// carte. Lot = h % kMslSeaux, trié : la partie suivante soumet les mêmes sources, que le cache Metal de macOS rend en
// ~2 ms par lot, et un shader nouveau ne refroidit que son lot. BO2_MSL_WARM=0 : pas de préchauffe.
static constexpr size_t kMslSeaux = 128;
static std::mutex g_warm_mu;
static std::unordered_map<uint64_t, id<MTLFunction>> g_warm;
std::atomic<uint64_t> g_msl_from_warm{0};
static id<MTLFunction> msl_warm_get(uint64_t h) {
    std::lock_guard<std::mutex> lk(g_warm_mu);
    auto it = g_warm.find(h);
    return it == g_warm.end() ? nil : it->second;
}
static void msl_warm_start() {
    if (!bo2_env_on("BO2_MSL_WARM", true)) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        const double t0 = CACurrentMediaTime();
        NSString* dir = [msl_cache_path("", "msl") stringByDeletingLastPathComponent];
        NSFileManager* fm = [NSFileManager defaultManager];
        auto seaux = std::make_shared<std::vector<std::vector<std::pair<uint64_t, std::string>>>>(kMslSeaux);
        size_t total = 0;
        for (NSString* f in [[fm contentsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
            if (![f hasSuffix:@".msl"]) continue;
            NSString* p = [dir stringByAppendingPathComponent:f];
            if ([fm fileExistsAtPath:[[p stringByDeletingPathExtension] stringByAppendingPathExtension:@"metallib"]]) continue;
            NSString* s = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil];
            if (!s) continue;
            const uint64_t h = strtoull([f UTF8String], nullptr, 16);
            (*seaux)[h % kMslSeaux].emplace_back(h, [s UTF8String]);
            total++;
        }
        if (!total) return;
        std::atomic<size_t> ok{0};
        std::atomic<size_t>* pok = &ok;
        dispatch_apply(kMslSeaux, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t k) {
            const auto& sx = (*seaux)[k];
            if (sx.empty()) return;
            std::vector<const std::string*> src;
            for (const auto& e : sx) src.push_back(&e.second);
            std::vector<id<MTLFunction>> fn(sx.size());
            msl_compile(src.data(), src.size(), fn.data(), nullptr);
            std::lock_guard<std::mutex> lk(g_warm_mu);
            for (size_t i = 0; i < sx.size(); i++)
                if (fn[i]) { g_warm[sx[i].first] = fn[i]; ++*pok; }
        });
        fprintf(stderr, "[MSL-WARM] %zu/%zu shaders des parties precedentes compiles en tache de fond, %.1f s\n",
                ok.load(), total, CACurrentMediaTime() - t0);
    });
}

id<MTLRenderPipelineState> g_ui_pipe = nil;
id<MTLBuffer>              g_ui_vbuf = nil;
size_t                     g_ui_vbuf_cap = 0;
// 🔎 BO2_DRAW_MIN / BO2_DRAW_MAX — BISECT WHICH DRAWS MAKE AN ARTEFACT.
// The tester looked at angola and asked whether the smeared band across the lower
// third was the player's own arms and weapon. That is a draw-identity question
// and there was no way to ask it: BO2_UI_DROPLAST only trims the UI. This
// submits only draws whose per-frame index is in [MIN, MAX], so a bisection
// finds the offending range in a handful of runs.
//   BO2_DRAW_MAX=800   -> submit only the first 800 draws of each frame
//   BO2_DRAW_MIN=800   -> submit only from the 800th on
// Unset = no filtering, so the default build is untouched.
std::atomic<long>          g_draw_seq{0};       // reset at the top of each frame
// BO2_GENMIPS=0 disables host-side mip generation for uncompressed 2D textures.
// On by default: without it nothing in this port has a mip chain at all.
// ── 🎯 BC DECODE, so BLOCK-COMPRESSED textures can get a mip chain too ────────
//
// 1,419 of BO2's 2,000 textures are uncompressed and Metal fills their pyramid
// in one blit (see bo2_genmips_on). The other 581 are block-compressed, Metal
// refuses to generate mips for those, and they are exactly the rock and terrain
// surfaces whose streaking the tester photographed.
//
// The census made this tractable: DXGI71=178 (BC1), 74=75 (BC2), 77=287 (BC3),
// 83=41 (BC5) — and **no BC7**, which is the one that would have been a project
// in itself. BC1/2/3/5 all decode in a few dozen lines.
//
// Strategy: decode level 0 to RGBA8, upload as RGBA8, let Metal build the chain.
// That trades memory (4-8x for these textures) for correct minification. It is
// bounded to textures >= kBcMipMin on a side so the small UI atlases, which
// never minify, keep their compact form.
static inline void bo2_bc_color_block(const uint8_t* b, uint8_t out[16][4], bool bc1_punch) {
    const uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8));
    const uint16_t c1 = (uint16_t)(b[2] | (b[3] << 8));
    uint8_t r[4], g[4], bl[4];
    auto unpack = [](uint16_t c, uint8_t& R, uint8_t& G, uint8_t& B) {
        R = (uint8_t)(((c >> 11) & 0x1f) * 255 / 31);
        G = (uint8_t)(((c >>  5) & 0x3f) * 255 / 63);
        B = (uint8_t)(( c        & 0x1f) * 255 / 31);
    };
    unpack(c0, r[0], g[0], bl[0]);
    unpack(c1, r[1], g[1], bl[1]);
    if (!bc1_punch || c0 > c1) {
        r[2] = (uint8_t)((2*r[0] + r[1]) / 3);  g[2] = (uint8_t)((2*g[0] + g[1]) / 3);
        bl[2]= (uint8_t)((2*bl[0]+ bl[1])/ 3);
        r[3] = (uint8_t)((r[0] + 2*r[1]) / 3);  g[3] = (uint8_t)((g[0] + 2*g[1]) / 3);
        bl[3]= (uint8_t)((bl[0]+ 2*bl[1])/ 3);
    } else {
        r[2] = (uint8_t)((r[0] + r[1]) / 2);    g[2] = (uint8_t)((g[0] + g[1]) / 2);
        bl[2]= (uint8_t)((bl[0]+ bl[1])/ 2);
        r[3] = g[3] = bl[3] = 0;
    }
    const uint32_t idx = (uint32_t)(b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24));
    for (int i = 0; i < 16; i++) {
        const int k = (idx >> (i * 2)) & 3;
        out[i][0] = r[k]; out[i][1] = g[k]; out[i][2] = bl[k];
        out[i][3] = (bc1_punch && c0 <= c1 && k == 3) ? 0 : 255;
    }
}
static inline void bo2_bc_alpha_block(const uint8_t* b, uint8_t out[16]) {
    uint8_t a[8]; a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i+1] = (uint8_t)(((7-i)*a[0] + i*a[1]) / 7);
    else { for (int i = 1; i < 5; i++) a[i+1] = (uint8_t)(((5-i)*a[0] + i*a[1]) / 5);
           a[6] = 0; a[7] = 255; }
    uint64_t bits = 0; for (int i = 2; i < 8; i++) bits |= (uint64_t)b[i] << ((i-2) * 8);
    for (int i = 0; i < 16; i++) out[i] = a[(bits >> (i * 3)) & 7];
}
// Returns false when the format is not one we decode.
static bool bo2_bc_decode(uint32_t dxgi, const uint8_t* src, size_t srcBytes,
                          uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
    const bool bc1 = (dxgi >= 70 && dxgi <= 72);
    const bool bc2 = (dxgi >= 73 && dxgi <= 75);
    const bool bc3 = (dxgi >= 76 && dxgi <= 78);
    const bool bc5 = (dxgi >= 82 && dxgi <= 84);
    if (!bc1 && !bc2 && !bc3 && !bc5) return false;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const uint32_t blockBytes = bc1 ? 8u : 16u;
    if ((uint64_t)bw * bh * blockBytes > srcBytes) return false;
    rgba.assign((size_t)w * h * 4, 0);
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* blk = src + ((size_t)by * bw + bx) * blockBytes;
            uint8_t px[16][4] = {}; uint8_t al[16];
            if (bc1) {
                bo2_bc_color_block(blk, px, true);
            } else if (bc2) {
                bo2_bc_color_block(blk + 8, px, false);
                for (int i = 0; i < 16; i++) {
                    const uint8_t nib = (blk[i / 2] >> ((i & 1) ? 4 : 0)) & 0xf;
                    px[i][3] = (uint8_t)(nib * 17);
                }
            } else if (bc3) {
                bo2_bc_color_block(blk + 8, px, false);
                bo2_bc_alpha_block(blk, al);
                for (int i = 0; i < 16; i++) px[i][3] = al[i];
            } else {                      // BC5: two BC4 channels -> R,G
                uint8_t rr[16], gg[16];
                bo2_bc_alpha_block(blk, rr);
                bo2_bc_alpha_block(blk + 8, gg);
                for (int i = 0; i < 16; i++) {
                    px[i][0] = rr[i]; px[i][1] = gg[i]; px[i][2] = 255; px[i][3] = 255;
                }
            }
            for (int i = 0; i < 16; i++) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint8_t* d = rgba.data() + ((size_t)y * w + x) * 4;
                d[0] = px[i][0]; d[1] = px[i][1]; d[2] = px[i][2]; d[3] = px[i][3];
            }
        }
    }
    return true;
}

static const uint32_t kBcMipMin = 128;
// BO2_BCMIPS=1 turns on decoding block-compressed textures to RGBA8 so they can
// carry a mip chain. Off by default: it costs 4-8x memory on those textures.
static bool bo2_bcmips_on(void) {
    static const bool on = [] {
        const char* e = bo2_getenv("BO2_BCMIPS");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

static bool bo2_genmips_on(void) {
    static const bool on = [] {
        const char* e = bo2_getenv("BO2_GENMIPS");
        return !(e && e[0] == '0');
    }();
    return on;
}
std::atomic<bool>          g_window_really_key{false};  // real isKeyWindow, unmasked
extern "C" int gfx_window_is_really_key(void) {
    return g_window_really_key.load(std::memory_order_relaxed) ? 1 : 0;
}
// 🔴 C16 (2026-09-26) — LE POINTEUR N'EST TENU QU'APRÈS UN CLIC DANS LA FENÊTRE.
// Avant, « fenêtre au premier plan » suffisait : un run de mesure ouvre sa fenêtre au
// premier plan, le jeu recentre le pointeur dès qu'il s'écarte de 220 points, et le testeur
// a dû tuer le run pour reprendre sa souris (« dans le jeu c'était fluide, mais pour le
// reste de mon Mac il était bloqué dans BO2 »). Règle des jeux Mac et des navigateurs :
// on tient après un clic DANS la fenêtre ; ⌘ (seul ou ⌘-Tab) ou la perte du premier plan
// rendent. Seuls les vrais NSEvent comptent : les clics injectés par BO2_CLICKPIPE vont
// dans la file de l'invité, pas ici, et ne tiennent donc jamais le pointeur.
static std::atomic<bool> g_pointer_engaged{false};
static void pointer_engage(bool on, const char* why) {
    if (g_pointer_engaged.exchange(on, std::memory_order_relaxed) != on)
        fprintf(stderr, "[CURSOR] pointeur %s (%s)%s\n", on ? "TENU" : "rendu", why,
                on ? " — ⌘ pour le rendre" : "");
}
extern "C" int gfx_pointer_engaged(void) {
    return g_pointer_engaged.load(std::memory_order_relaxed) ? 1 : 0;
}
// What SetCursorPos (user32_impl.cpp, guest thread) needs from the window: the middle of the game area in
// global display points (origin top-left of the main display, whole points so that a warp lands exactly on
// it), published by the event pump; and it reports each recentring of a held pointer, which hides it.
// The centre is only good while the pump runs (0.5 s): the pump is also what gives the pointer back, and a
// pointer nothing can give back must not be held.
static std::atomic<double> g_centre_x{0}, g_centre_y{0}, g_centre_t{0}, g_pointeur_fixe_t{0};
static bool g_pointeur_cache = false;   // pump thread: [NSCursor hide] is in force
extern "C" int gfx_window_centre(double* x, double* y) {
    *x = g_centre_x.load(std::memory_order_relaxed); *y = g_centre_y.load(std::memory_order_relaxed);
    const double t = g_centre_t.load(std::memory_order_relaxed);
    return t > 0 && CACurrentMediaTime() - t < 0.5;
}
extern "C" void gfx_pointer_pinned(void) {
    g_pointeur_fixe_t.store(CACurrentMediaTime(), std::memory_order_relaxed);
}
id<MTLTexture>             g_white = nil;      // stand-in for an untextured batch
// D3D11 SAMPLING A NULL SHADER RESOURCE RETURNS (0,0,0,0), NOT WHITE. Our white
// stand-in was fine for a multiply-by-texture shader but catastrophic for the
// frontend's ADDITIVE draws (OMSetBlendState src=ONE dst=ONE): every slot the
// engine deliberately left unbound added full white, and the window saturated to
// pure white within a couple of draws. This is the correct fallback.
id<MTLTexture>             g_nulltex = nil;    // 1x1 transparent black
id<MTLTexture>             g_null3d = nil;     // 1x1x1, for a texture3d slot
id<MTLTexture>             g_nullcube = nil;   // 1x1 cube, for a texturecube slot
id<MTLTexture>             g_nulldepth = nil;  // 1x1 depth, for a depth2d slot
id<MTLFunction>            g_flat_fs = nil;    // BO2_UI_FLAT diagnostic shader

const char* kUiShaderSrc = R"(
#include <metal_stdlib>
using namespace metal;
struct UiIn  { float2 pos; float z; float2 uv; uchar4 col; };
struct UiOut { float4 pos [[position]]; float2 uv; float4 col; };
struct UiXform { float4 row[4]; uint has_mtx; float2 vp; };
vertex UiOut ui_v(uint vid [[vertex_id]],
                  const device UiIn* vtx [[buffer(0)]],
                  constant UiXform& x [[buffer(1)]]) {
    UiIn i = vtx[vid];
    UiOut o;
    if (x.has_mtx != 0u) {
        // Exactly what BO2's own vertex shader does, from the translated MSL:
        //   clip.x = dot(float4(POSITION0, 1), worldViewProjectionMatrix[0]); ...
        float4 p = float4(i.pos, i.z, 1.0);
        o.pos = float4(dot(p, x.row[0]), dot(p, x.row[1]),
                       dot(p, x.row[2]), dot(p, x.row[3]));
    } else {
        // No transform available: the positions are already viewport pixels
        // (origin top-left, y down).
        o.pos = float4(i.pos.x / x.vp.x * 2.0 - 1.0, 1.0 - i.pos.y / x.vp.y * 2.0, 0, 1);
    }
    o.uv  = i.uv;
    o.col = float4(i.col) / 255.0;
    return o;
}
fragment float4 ui_f(UiOut in [[stage_in]],
                     texture2d<float> tex [[texture(0)]],
                     sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv) * in.col;
}
)";

}  // namespace

// --- Gate B step 4: run BO2's OWN shaders -----------------------------------
// Everything the guest bound for one DrawIndexed, mirrored from the shim. The
// pipeline is cached on (vs, ps, layout) — building one per draw would dominate
// the frame.
struct GfxDrawElem { char semantic[24]; uint32_t fmt, offset, slot; };
// PS shader-resource slots we carry. Four was a guess and it is too few: a D3D11
// stage has 128, and a shader that declares its texture at t4 got our 1x1 white
// fallback with no way to tell. Must match the shim's copy of this struct.
enum { kMaxPSTex = 16 };
// Constant-buffer slots per stage. Must match the shim's kMaxCB.
enum { kMaxCB = 8 };
// Mirrors the shim's GfxBlend: one D3D11_RENDER_TARGET_BLEND_DESC, flattened.
// Mirrors remaining_dlls_impl.cpp's GfxDepth. See the note there: the absence of
// any depth buffer was one bug with five faces.
struct GfxDepth {
    uint32_t enable;
    uint32_t write;       // 0 = ZERO, 1 = ALL
    uint32_t func;        // D3D11_COMPARISON_FUNC 1..8
    uint32_t st;          // 0 = StencilEnable FALSE; else 1 | StencilReadMask << 8 | StencilWriteMask << 16
    uint32_t st_ops;      // 8 nibbles, D3D11 values 1..8: front FailOp, DepthFailOp, PassOp, Func; then back
    uint32_t st_ref;      // StencilRef of the last OMSetDepthStencilState (8 bits)
    uint32_t st_res;      // resource behind the bound DSV when its format has a stencil plane, else 0
};
struct GfxBlend {
    uint32_t enable;
    uint32_t src_rgb, dst_rgb, op_rgb;
    uint32_t src_a,   dst_a,   op_a;
    uint32_t write_mask;
};
struct GfxDraw {
    uint32_t vs, ps;
    const GfxDrawElem* elems; uint32_t nelems;
    const void* vb; uint32_t vb_bytes, vb_stride, vb_off;
    // EXTRA VERTEX STREAMS (slots 1..3). The input layout's elements carry the
    // slot they come from; until now every element was read out of slot 0's
    // buffer, which is only correct for single-stream draws.
    const void* vb2[3]; uint32_t vb2_bytes[3], vb2_stride[3];
    const void* ib; uint32_t ib_bytes, ib_fmt;
    uint32_t idx_count, start_idx; int32_t base_vtx;
    uint32_t topo;
    uint32_t rtv;
    uint32_t is_clear;
    uint32_t is_copy, copy_dst, copy_src;
    float    clear_rgba[4];
    // ⚠️ CETTE STRUCTURE EST DECLAREE DEUX FOIS, SANS EN-TETE PARTAGE :
    // ici (le consommateur) et dans runtime/win32/remaining_dlls_impl.cpp
    // (le producteur). Le 2026-09-16 j'ai agrandi `vp` de 4 a 6 flottants
    // cote producteur seulement : le backend a alors lu vscb, pscb, pstex,
    // blend et depth 8 octets trop bas, et l'ecran est devenu ENTIEREMENT
    // NOIR alors que le moteur emettait toujours 2670 dessins par image.
    // Rien ne l'avait signale — ni le compilateur, ni le lien, ni une sonde.
    // D'ou le controle de taille croise juste apres la structure : toute
    // divergence future tue le processus avec le message qui l'explique,
    // au lieu de rendre du noir en silence.
    float    vp[6];                     // x, y, w, h, MinDepth, MaxDepth
    const void* vscb[kMaxCB]; uint32_t vscb_bytes[kMaxCB];
    const void* pscb[kMaxCB]; uint32_t pscb_bytes[kMaxCB];
    uint32_t pstex[kMaxPSTex];
    GfxBlend blend;
    GfxDepth depth;
    uint32_t dsv;
    float vpw, vph;
    uint32_t vscb_fill[kMaxCB], pscb_fill[kMaxCB];   // C47, voir le producteur
    // C50 : pour chaque flux (0 = slot 0, 1..3 = slots supplementaires), le debut et la taille du tampon
    // STATIQUE entier dont vb / vb2 est une tranche (nullptr = flux recopie comme avant). Le consommateur
    // ne recopie pas ces octets : ils ne changent plus (cree avec pInitialData, jamais Map).
    const void* res[4]; uint32_t res_bytes[4];
    uint32_t pssamp[kMaxPSTex];         // P805 : echantillonneur du jeu par registre s (PSSetSamplers)
    uint32_t rs; int32_t sc[4];         // rasterizer state and scissor rectangle, voir le producteur
    uint32_t dsv_res, dsv_wh;           // BO2_DEPTH_DSV: depth resource behind the bound DSV and its size, voir le producteur
    int32_t zb; float zb_pente, zb_max; // BO2_DEPTH_BIAS: DepthBias, SlopeScaledDepthBias, DepthBiasClamp, voir le producteur
    uint32_t psmip[kMaxPSTex];          // niveaux de la SRV par slot (MostDetailedMip << 8 | MipLevels), voir le producteur
};

// 🎯 LE CONTROLE DE TAILLE CROISE. Le producteur exporte sizeof(GfxDraw) tel
// qu'IL le voit ; on le compare au notre a la premiere image. Un desaccord
// signifie que les deux declarations ont diverge et que tous les champs apres
// le premier ecart sont lus a la mauvaise adresse — ce qui ne produit ni
// erreur de compilation, ni erreur de lien, ni message : juste une image
// fausse. On s'arrete fort, en nommant les deux fichiers a reconcilier.
extern "C" uint32_t gfx_draw_record_size(void);
static void gfx_check_record_layout(void) {
    const uint32_t theirs = gfx_draw_record_size(), mine = (uint32_t)sizeof(GfxDraw);
    if (theirs == mine) return;
    fprintf(stderr,
        "\n[GFX-ABI] FATAL: struct GfxDraw a diverge entre ses deux declarations.\n"
        "  runtime/win32/remaining_dlls_impl.cpp (producteur) : %u octets\n"
        "  runtime/gfx/metal_backend.mm          (consommateur): %u octets\n"
        "  Les deux doivent etre identiques champ par champ. Tout champ situe\n"
        "  apres le premier ecart est lu a la mauvaise adresse par le backend.\n\n",
        theirs, mine);
    abort();
}

namespace {
// D3D11_BLEND -> MTLBlendFactor.
MTLBlendFactor d3d_blend_factor(uint32_t b) {
    switch (b) {
        case 1:  return MTLBlendFactorZero;
        case 2:  return MTLBlendFactorOne;
        case 3:  return MTLBlendFactorSourceColor;
        case 4:  return MTLBlendFactorOneMinusSourceColor;
        case 5:  return MTLBlendFactorSourceAlpha;
        case 6:  return MTLBlendFactorOneMinusSourceAlpha;
        case 7:  return MTLBlendFactorDestinationAlpha;
        case 8:  return MTLBlendFactorOneMinusDestinationAlpha;
        case 9:  return MTLBlendFactorDestinationColor;
        case 10: return MTLBlendFactorOneMinusDestinationColor;
        case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 14: return MTLBlendFactorBlendColor;
        case 15: return MTLBlendFactorOneMinusBlendColor;
        case 16: return MTLBlendFactorSource1Color;
        case 17: return MTLBlendFactorOneMinusSource1Color;
        case 18: return MTLBlendFactorSource1Alpha;
        case 19: return MTLBlendFactorOneMinusSource1Alpha;
        default: return MTLBlendFactorOne;
    }
}
// D3D11_BLEND_OP -> MTLBlendOperation.
MTLBlendOperation d3d_blend_op(uint32_t o) {
    switch (o) {
        case 1:  return MTLBlendOperationAdd;
        case 2:  return MTLBlendOperationSubtract;
        case 3:  return MTLBlendOperationReverseSubtract;
        case 4:  return MTLBlendOperationMin;
        case 5:  return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}
// D3D11_COLOR_WRITE_ENABLE bits are R=1,G=2,B=4,A=8 — the same bit order Metal
// uses, but the enum values differ, so map them explicitly.
uint64_t blend_key(const GfxBlend& b) {
    return ((uint64_t)b.enable) | ((uint64_t)b.src_rgb << 1) | ((uint64_t)b.dst_rgb << 6) |
           ((uint64_t)b.op_rgb << 11) | ((uint64_t)b.src_a << 14) | ((uint64_t)b.dst_a << 19) |
           ((uint64_t)b.op_a << 24) | ((uint64_t)b.write_mask << 27);
}
MTLColorWriteMask d3d_write_mask(uint32_t m) {
    MTLColorWriteMask r = MTLColorWriteMaskNone;
    if (m & 1) r |= MTLColorWriteMaskRed;
    if (m & 2) r |= MTLColorWriteMaskGreen;
    if (m & 4) r |= MTLColorWriteMaskBlue;
    if (m & 8) r |= MTLColorWriteMaskAlpha;
    return r;
}
// DXGI_FORMAT -> MTLVertexFormat, for the formats BO2's input layouts use.
MTLVertexFormat dxgi_to_vertex_format(uint32_t f) {
    switch (f) {
        case 2:  return MTLVertexFormatFloat4;        // R32G32B32A32_FLOAT
        case 6:  return MTLVertexFormatFloat3;        // R32G32B32_FLOAT
        case 16: return MTLVertexFormatFloat2;        // R32G32_FLOAT
        case 41: return MTLVertexFormatFloat;         // R32_FLOAT
        case 28: return MTLVertexFormatUChar4Normalized;   // R8G8B8A8_UNORM
        case 87: return MTLVertexFormatUChar4Normalized;   // B8G8R8A8_UNORM (swizzled in-shader)
        case 10: return MTLVertexFormatHalf4;         // R16G16B16A16_FLOAT
        case 34: return MTLVertexFormatHalf2;         // R16G16_FLOAT
        case 13: return MTLVertexFormatShort4Normalized;   // R16G16B16A16_SNORM
        case 37: return MTLVertexFormatShort2Normalized;   // R16G16_SNORM
        case 31: return MTLVertexFormatChar4Normalized;    // R8G8B8A8_SNORM
        case 24: return MTLVertexFormatUInt1010102Normalized;  // R10G10B10A2_UNORM
        case 30: return MTLVertexFormatUChar4;        // R8G8B8A8_UINT
        case 32: return MTLVertexFormatChar4;         // R8G8B8A8_SINT
        case 11: return MTLVertexFormatUShort4Normalized;  // R16G16B16A16_UNORM
        case 12: return MTLVertexFormatUShort4;       // R16G16B16A16_UINT
        case 14: return MTLVertexFormatShort4;        // R16G16B16A16_SINT
        case 35: return MTLVertexFormatUShort2Normalized;  // R16G16_UNORM
        case 36: return MTLVertexFormatUShort2;       // R16G16_UINT
        case 38: return MTLVertexFormatShort2;        // R16G16_SINT
        case 3:  return MTLVertexFormatUInt4;         // R32G32B32A32_UINT
        case 4:  return MTLVertexFormatInt4;          // R32G32B32A32_SINT
        case 7:  return MTLVertexFormatUInt3;         // R32G32B32_UINT
        case 8:  return MTLVertexFormatInt3;          // R32G32B32_SINT
        case 17: return MTLVertexFormatUInt2;         // R32G32_UINT
        case 18: return MTLVertexFormatInt2;          // R32G32_SINT
        case 42: return MTLVertexFormatUInt;          // R32_UINT
        case 43: return MTLVertexFormatInt;           // R32_SINT
        default: return MTLVertexFormatInvalid;
    }
}

// A shader's translated function plus, for a vertex shader, the semantic ->
// attribute-index map HLSLcc chose (parsed out of the MSL by the bridge).
struct ShaderRec {
    id<MTLFunction> fn;
    std::unordered_map<std::string, uint32_t> attrs;
    // Metal texture index -> D3D shader-resource slot. HLSLcc renumbers textures
    // from zero in declaration order, so binding slot k at index k feeds the shader
    // the wrong resource; the menu's font draws sampled nothing for exactly this
    // reason while the engine had its atlas bound at another slot.
    std::unordered_map<uint32_t, uint32_t> texslot;
    // Metal texture index -> HLSLCC_TEX_DIMENSION (TD_2D=2, TD_3D=3, TD_CUBE=4,
    // TD_2DSHADOW=5). Empty means "never reported", which we treat as 2D.
    std::unordered_map<uint32_t, uint32_t> texdim;
    // Metal buffer index -> D3D constant-buffer register, for the same reason.
    std::unordered_map<uint32_t, uint32_t> cbslot;
    // P805 : Metal sampler index -> D3D s register (HLSLcc renumbers samplers too).
    std::unordered_map<uint32_t, uint32_t> smpslot;
    // C49 : les trois tables ci-dessus aplaties par indice Metal au premier dessin (encode_real_draw
    // les lisait 48 fois par dessin). `apply` remet plat=false quand une table change.
    bool plat = false;
    bool tx_decl[kMaxPSTex] = {};
    uint32_t tx_slot[kMaxPSTex] = {}, tx_dim[kMaxPSTex] = {}, cb_reg[kMaxCB] = {};
    uint32_t sm_reg[kMaxPSTex] = {};     // P805 : ~0u = pas d'echantillonneur declare a cet indice
    bool tx_prof[kMaxPSTex] = {};        // Metal texture index declared depth2d< (HLSLcc reports TD_2D for it)
    uint64_t msl_h = 0;                  // name of its source in msl_cache/ (probes)
};
std::unordered_map<uint32_t, ShaderRec> g_shader_rec;
std::unordered_map<uint64_t, id<MTLRenderPipelineState>> g_pipe_cache;
// DEPTH. One Depth32Float texture per render-target SIZE, not per target: the
// engine's passes at a given resolution share a depth buffer in D3D too (they are
// handed the same DSV), and allocating per colour target would both waste memory
// and break the depth-prepass -> main-pass sharing the engine relies on.
std::unordered_map<uint64_t, id<MTLTexture>> g_depth_tex;
// MTLDepthStencilState is immutable and cheap to cache; key is (enable,write,func), plus the stencil
// fields when StencilEnable (st << 8 | st_ops << 32).
std::unordered_map<uint64_t, id<MTLDepthStencilState>> g_dss_cache;
// STENCIL (Z-NATIF-20). The game's stencil plane goes into a Stencil8 texture of its own, attached only to
// the passes that hold a stencil-enabled draw: depth stays Depth32Float and a frame with no such draw
// encodes exactly as before. One texture per (resource behind the DSV, size of the pass's colour target).
// Pump thread only.
struct Pochoir { id<MTLTexture> tex; int efface = 0; };   // efface >= 0: a clear to that value is pending
std::unordered_map<uint64_t, Pochoir> g_pochoir;          // (a new texture starts cleared to 0)
std::unordered_map<uint32_t, int> g_pochoir_val;          // resource -> its last clear value, for a texture made later
// Witnesses of the [PRESENT] window, printed and reset by present_report.
enum { kStEmis, kStEcrivent, kStSautes, kStPasses, kStEffacees, kStEffRecus, kStCoupeEff, kStCoupeDsv,
       kStSansAttache, kStN };
uint64_t g_st_n[kStN] = {0};
enum { kScDessins, kScRognes, kScRognesPochoir, kScVides, kScVpAutre, kScVpPlage, kScVpReposes, kScCull, kScN };
uint64_t g_sc_n[kScN] = {0};
std::atomic<uint64_t> g_cb_erreurs{0};
// Sizes we have already cleared this frame, so the FIRST pass at a size clears
// depth and later passes load it. Clearing on every pass would throw away the
// prepass; never clearing would accumulate last frame's depth forever.
std::set<uint64_t> g_depth_cleared;
uint64_t g_depth_made = 0;

extern "C" float gfx_depth_clear_value();

// ecran : l'ecran garde sa profondeur a lui. Il avait jusqu'au 10-21 la taille de la fenetre, donc une cle a lui ; a la
// taille du jeu, il partagerait celle des cibles hors ecran de meme taille (monde noir).
static id<MTLTexture> depth_for(uint32_t w, uint32_t h, bool ecran = false) {
    if (!w || !h) return nil;
    uint64_t k = ((uint64_t)w << 32) | h | ((uint64_t)ecran << 31);
    auto it = g_depth_tex.find(k);
    if (it != g_depth_tex.end()) return it->second;
    MTLTextureDescriptor* dd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                     width:w height:h mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> dt = [g_device newTextureWithDescriptor:dd];
    g_depth_tex[k] = dt;
    if (dt && ++g_depth_made <= 8)
        fprintf(stderr, "[gfx] depth buffer %ux%u created\n", w, h);
    return dt;
}

// BO2_DEPTH_DSV. The depth a pass attaches is the one behind the DSV the game bound: one Depth32Float per D3D
// RESOURCE, made at its first attachment. Pump thread only. The BGRA8 stand-in of g_texreg is left alone.
// 1 = every pass off the backbuffer; 2 = only the passes without a colour target (the lit passes keep depth_for, which
// separates the atlases and the prepass from what the lit pass then does with the prepass's depth).
const int g_dd_mode = [] { const char* e = bo2_getenv("BO2_DEPTH_DSV"); const int v = e ? atoi(e) : 0;
                           return (v == 1 || v == 2) ? v : 0; }();
struct ProfDsv { id<MTLTexture> tex; float efface = 0.f;   // efface >= 0: a clear to that value is pending…
                 bool du_jeu = false;                      // …and it is a ClearDepthStencilView, not the texture's creation
                 uint64_t n_sans = 0, n_coul = 0, n_emis = 0, n_lus = 0, n_eff_recus = 0, n_eff_faits = 0; };   // per window
std::unordered_map<uint32_t, ProfDsv> g_prof_dsv;
id<MTLTexture> g_prof_attache = nil;                       // the depth the open pass attaches
id<MTLTexture> g_coul_attache = nil;                       // P855 : the colour target the open pass attaches
NSUInteger g_coul_niveau = 0;                              // P855 : and its level
// P856 : BO2_MIP_CIBLES=0 rend le chemin d'avant 170ec815 (cibles à un niveau, RTV et SRV au niveau 0), pour l'A/B
// des cratères en carrés de 32 px (la chaîne de profondeur R32F 1920x1080 descend à 1/32).
static const bool g_mip_cibles = [] {
    const bool on = bo2_env_on("BO2_MIP_CIBLES", true);
    fprintf(stderr, "[MIP-CIBLES] niveaux de mip des cibles et des vues : %s (BO2_MIP_CIBLES)\n", on ? "ARME" : "ETEINT");
    return on; }();
enum { kDdCoupe, kDdCoupeEff, kDdCopie, kDdTaille, kDdEcranZ, kDdEffSeul, kDdLuAttache, kDdLuTex2d, kDdImgPerdues,
       kDdBiaisSans, kDdBiaisCoul, kDdPince, kDdLuVm, kDdN };   // the last three: BO2_DEPTH_BIAS, draws with a bias (depth-only / colour pass), with depth clamp
std::unordered_map<uint32_t, uint64_t> g_dd_lus_cible;   // served depth reads per colour target of the reading draw (pump only)
uint32_t g_dd_tex2d_res = 0, g_dd_tex2d_rtv = 0;        // last depth read through a texture2d slot: resource, target
float g_dd_vm_plage[2] = {0.f, 0.f};                    // MinDepth, MaxDepth of the last served draw whose range is not 0..1 (kDdLuVm)
uint64_t g_dd_n[kDdN] = {0}, g_dd_func[9] = {0};          // per window (present_report)
uint32_t g_dd_ecranz_res = 0;
static ProfDsv* prof_dsv(uint32_t res, uint32_t wh) {
    const uint32_t w = wh >> 16, h = wh & 0xFFFFu;
    if (!w || !h) return nullptr;
    ProfDsv& p = g_prof_dsv[res];
    if (!p.tex || p.tex.width != w || p.tex.height != h) {   // first attachment, or the id reused at another size
        MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                   width:w height:h mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        dd.storageMode = MTLStorageModePrivate;
        p.tex = [g_device newTextureWithDescriptor:dd];
        if (p.efface < 0.f) p.efface = 0.f;                  // undefined contents: its first pass clears
        static uint64_t n_echec = 0;   // a failed allocation comes back here at every pass
        if (p.tex || (++n_echec & (n_echec - 1)) == 0)
            fprintf(stderr, "[DEPTH-DSV] profondeur Depth32Float ressource=%08x %ux%u %s\n", res, w, h, p.tex ? "creee" : "ECHEC");
    }
    return p.tex ? &p : nullptr;
}
// ClearDepthStencilView(…, DEPTH, v): load action of the next pass that attaches it. The plane exists from this clear
// on, not from its first draw: an atlas the game clears and draws no caster into (a map without shadow geometry) is
// still sampled, and must read v, not the colour stand-in of g_texreg (every lookup "occluded", the world black).
static void prof_efface(uint32_t res, float v, uint32_t wh) {
    prof_dsv(res, wh);
    ProfDsv& p = g_prof_dsv[res];
    p.efface = v; p.du_jeu = true; ++p.n_eff_recus;
}

// D3D11_COMPARISON_FUNC (1..8) -> Metal. D3D and Metal agree on the ordering of
// the comparison semantics but not on the numbering, so this is a table, not a
// cast — a cast would silently turn LESS into NEVER.
static MTLCompareFunction d3d_cmp(uint32_t f) {
    switch (f) {
        case 1: return MTLCompareFunctionNever;
        case 2: return MTLCompareFunctionLess;
        case 3: return MTLCompareFunctionEqual;
        case 4: return MTLCompareFunctionLessEqual;
        case 5: return MTLCompareFunctionGreater;
        case 6: return MTLCompareFunctionNotEqual;
        case 7: return MTLCompareFunctionGreaterEqual;
        case 8: return MTLCompareFunctionAlways;
        default: return MTLCompareFunctionLess;
    }
}

static Pochoir& pochoir_for(uint32_t res, uint32_t w, uint32_t h) {
    Pochoir& p = g_pochoir[((uint64_t)res << 32) | ((uint64_t)w << 16) | h];
    if (!p.tex) {
        MTLTextureDescriptor* sd = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8 width:w height:h mipmapped:NO];
        sd.usage = MTLTextureUsageRenderTarget;
        sd.storageMode = MTLStorageModePrivate;
        p.tex = [g_device newTextureWithDescriptor:sd];
        auto v = g_pochoir_val.find(res);
        if (v != g_pochoir_val.end()) p.efface = v->second;
        fprintf(stderr, "[POCHOIR] tampon Stencil8 ressource=%08x %ux%u %s\n", res, w, h, p.tex ? "cree" : "ECHEC");
    }
    return p;
}
// ClearDepthStencilView(…, D3D11_CLEAR_STENCIL, v): every size of that plane, at the next pass that attaches it.
static void pochoir_efface(uint32_t res, uint32_t v) {
    g_pochoir_val[res] = (int)v;
    for (auto& p : g_pochoir) if ((uint32_t)(p.first >> 32) == res) p.second.efface = (int)v;
}
// D3D11_STENCIL_OP 1..8 (KEEP, ZERO, REPLACE, INCR_SAT, DECR_SAT, INVERT, INCR, DECR) is Metal's 0..7 in the
// same order (Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap).
static MTLStencilOperation d3d_stencil_op(uint32_t o) {
    return (o >= 1 && o <= 8) ? (MTLStencilOperation)(o - 1) : MTLStencilOperationKeep;
}

static id<MTLDepthStencilState> dss_for(const GfxDepth& z) {
    uint64_t key = (z.enable ? 1u : 0u) | ((z.write ? 1u : 0u) << 1) |
                   ((z.func & 0xFu) << 2);
    if (z.st) key |= ((uint64_t)z.st << 8) | ((uint64_t)z.st_ops << 32);
    auto it = g_dss_cache.find(key);
    if (it != g_dss_cache.end()) return it->second;
    MTLDepthStencilDescriptor* dsd = [[MTLDepthStencilDescriptor alloc] init];
    // DepthEnable=FALSE in D3D means "always pass", and it does NOT disable
    // writes on its own — but with the test off D3D also stops writing depth, so
    // both have to be forced here or a disabled-depth UI draw would stamp itself
    // into the buffer the world was just rendered into.
    dsd.depthCompareFunction = z.enable ? d3d_cmp(z.func) : MTLCompareFunctionAlways;
    dsd.depthWriteEnabled    = (z.enable && z.write) ? YES : NO;
    // Same comparison on both sides: (ref & mask) OP (stored & mask). With DepthEnable FALSE the depth test
    // above always passes, so the stencil runs PassOp, as in D3D11.
    if (z.st) for (int f = 0; f < 2; f++) {
        const uint32_t o = z.st_ops >> (16 * f);
        MTLStencilDescriptor* sd = [[MTLStencilDescriptor alloc] init];
        sd.stencilFailureOperation   = d3d_stencil_op(o & 0xFu);
        sd.depthFailureOperation     = d3d_stencil_op((o >> 4) & 0xFu);
        sd.depthStencilPassOperation = d3d_stencil_op((o >> 8) & 0xFu);
        sd.stencilCompareFunction    = d3d_cmp((o >> 12) & 0xFu);
        sd.readMask  = (z.st >> 8) & 0xFFu;
        sd.writeMask = (z.st >> 16) & 0xFFu;
        if (f) dsd.backFaceStencil = sd; else dsd.frontFaceStencil = sd;
    }
    id<MTLDepthStencilState> st = [g_device newDepthStencilStateWithDescriptor:dsd];
    g_dss_cache[key] = st;
    return st;
}
// The vertex stream goes in a high buffer index so it cannot collide with the
// constant buffers, which HLSLcc packs into buffer(0..N) in first-use order.
constexpr NSUInteger kVertexStreamIndex = 24;
// How many D3D vertex-buffer slots we carry through. The room's materials use
// two; Metal buffer indices 24..27 are reserved for them (constant buffers live
// low, so there is room).
constexpr uint32_t kMaxVBStreams = 4;
// One growable index buffer, bump-allocated per frame. Creating an MTLBuffer per
// draw (there are hundreds a frame) made the run abort inside the graphics path
// while the same build reaches HARDCAP cleanly headless.
id<MTLBuffer> g_idx_pool = nil;
NSUInteger    g_idx_pool_cap = 0, g_idx_pool_off = 0;
// Same for vertex streams and constant buffers. setVertexBytes:/setFragmentBytes:
// are limited to 4 KB — handing them a 2 MB vertex window made Metal abort the
// process with no message, which read as a mysterious signal 6 at a fixed frame.
id<MTLBuffer> g_upl_pool = nil;
NSUInteger    g_upl_pool_cap = 0, g_upl_pool_off = 0;
// C47 : change a chaque remise a zero du pool (debut d'image encodee) et a chaque reallocation ; un
// decalage memorise n'est valable que dans sa generation.
uint64_t      g_upl_gen = 1;
// 🎯 P785 — LES TAMPONS PAR IMAGE ETAIENT REECRITS PENDANT QUE LE GPU LES LISAIT. Chaque image encodee
// remettait g_upl_pool/g_idx_pool/g_ui_vbuf a zero et y recopiait ses sommets et constantes, alors que le
// CB de l'image precedente, soumis sans attente, pouvait encore s'executer : constantes et sommets
// d'une autre image au milieu d'une passe. Le film d'angola (p776_analyse) en montre 27-42 par minute :
// image par blocs (ciel noir, arbre cyan, taches magenta), ecran entier magenta ou vert, entre deux
// images intactes. Anneau de N jeux de tampons : un jeu ne resert qu'une fois termine le CB qui l'a lu.
// BO2_GPU_RING : 3 par defaut ; 0 = ancien chemin (un seul jeu, aucune attente). Temoin [ANNEAU].
struct GpuSlot {
    id<MTLBuffer> upl = nil, idx = nil, ui = nil;
    NSUInteger upl_cap = 0, idx_cap = 0;
    size_t ui_cap = 0;
    id<MTLCommandBuffer> cb = nil;   // dernier CB soumis qui lit ce jeu
};
const int g_ring_n = [] {
    const char* e = bo2_getenv("BO2_GPU_RING");
    const int n = e ? atoi(e) : 3;
    fprintf(stderr, "[ANNEAU] BO2_GPU_RING=%s : %d jeu(x) de tampons par image%s\n", e ? e : "(defaut 3)",
            n < 0 ? 0 : n > 8 ? 8 : n, n <= 0 ? " — ANCIEN chemin, aucune attente du GPU" : "");
    return n < 0 ? 0 : n > 8 ? 8 : n;
}();
GpuSlot  g_ring[8];
int      g_ring_cur = 0;
uint64_t g_ring_attentes = 0;   // attentes reellement bloquantes (CB pas encore termine)
// Debut d'une image encodee : ranger le jeu courant, passer au suivant, attendre que le GPU l'ait lu.
void ring_next() {
    if (!g_ring_n) return;
    GpuSlot& o = g_ring[g_ring_cur];
    o.upl = g_upl_pool; o.upl_cap = g_upl_pool_cap;
    o.idx = g_idx_pool; o.idx_cap = g_idx_pool_cap;
    o.ui  = g_ui_vbuf;  o.ui_cap  = g_ui_vbuf_cap;
    g_ring_cur = (g_ring_cur + 1) % g_ring_n;
    GpuSlot& s = g_ring[g_ring_cur];
    if (s.cb && s.cb.status < MTLCommandBufferStatusCompleted) {
        [s.cb waitUntilCompleted];
        const uint64_t a = ++g_ring_attentes;
        if ((a & (a - 1)) == 0)
            fprintf(stderr, "[ANNEAU] attente bloquante #%llu (le GPU avait %d images de retard)\n",
                    (unsigned long long)a, g_ring_n);
    }
    g_upl_pool = s.upl; g_upl_pool_cap = s.upl_cap;
    g_idx_pool = s.idx; g_idx_pool_cap = s.idx_cap;
    g_ui_vbuf  = s.ui;  g_ui_vbuf_cap  = s.ui_cap;
}
// Bump-allocate `n` bytes of `src` into the per-frame upload buffer; returns the
// offset, or -1 if it will not fit.
NSInteger upload(id<MTLDevice> dev, const void* src, NSUInteger n) {
    NSUInteger need = (n + 255) & ~(NSUInteger)255;      // Metal wants 256B alignment
    if (g_upl_pool_off + need > g_upl_pool_cap) {
        NSUInteger want = g_upl_pool_cap ? g_upl_pool_cap * 2 : (16u << 20);
        while (want < g_upl_pool_off + need) want *= 2;
        if (want > (192u << 20)) return -1;
        g_upl_pool = [dev newBufferWithLength:want options:MTLResourceStorageModeShared];
        g_upl_pool_cap = g_upl_pool ? want : 0;
        g_upl_pool_off = 0;
        ++g_upl_gen;
        if (!g_upl_pool) return -1;
    }
    memcpy((uint8_t*)[g_upl_pool contents] + g_upl_pool_off, src, n);
    NSInteger off = (NSInteger)g_upl_pool_off;
    g_upl_pool_off += need;
    return off;
}
// 🎯 C47 (P683) — UN CB DEJA ENVOYE N'EST PAS RENVOYE. gfx_real_draw fait partager la meme tranche
// d'arene aux dessins dont le CB n'a pas ete relu (memes octets) ; ici, la meme tranche dans la meme
// generation du pool reprend son decalage. Les octets d'une tranche ne changent pas pendant que la
// pompe encode son image (la construction ecrit dans une autre arene). BO2_CB_DEDUP=0 : ancien chemin.
bool g_c47 = [] { const char* e = bo2_getenv("BO2_CB_DEDUP"); return !e || atoi(e) != 0; }();
std::atomic<uint64_t> g_c47_envois_repris{0};
struct UplMemo { const void* p = nullptr; size_t n = 0; uint64_t gen = 0; NSInteger off = -1; };
NSInteger upload_memo(UplMemo& m, id<MTLDevice> dev, const void* src, size_t n) {
    if (g_c47 && src && m.p == src && m.n == n && m.gen == g_upl_gen) {
        g_c47_envois_repris.fetch_add(1, std::memory_order_relaxed);
        return m.off;
    }
    const NSInteger off = upload(dev, src, n);
    if (off >= 0) m = UplMemo{src, n, g_upl_gen, off};   // generation lue APRES : upload peut reallouer
    else m = UplMemo{};
    return off;
}
// 🎯 C49 (P684) — L'ETAT DEJA LIE N'EST PAS REPOSE. Le fil AppKit posait, a CHAQUE dessin, le
// pipeline, les tampons et les 16 couples texture/echantillonneur (xctrace p682 : les deux derniers
// = 21 % du fil). Un codeur Metal garde son etat ; on ne saute un appel que si l'objet lie dans CE
// codeur (g_enc_gen, +1 a chaque pe2) est le meme (reference forte) et au meme decalage. Meme
// tampon, autre decalage : set*BufferOffset. BO2_METAL_CACHE=0 : ancien chemin.
bool g_c49 = [] {
    const char* e = bo2_getenv("BO2_METAL_CACHE"); const bool on = !e || atoi(e) != 0;
    fprintf(stderr, "[C49] etat Metal deja lie non repose : %s (BO2_METAL_CACHE)\n", on ? "ARME" : "ETEINT");
    return on; }();
uint64_t g_enc_gen = 0, g_c49_n[12] = {0};
enum { kC49Tampons = 31 };
struct C49Etat {
    uint64_t gen = ~0ull;
    id<MTLRenderPipelineState> pipe; id<MTLDepthStencilState> ds;
    id<MTLBuffer> vb[kC49Tampons], fb[kC49Tampons];
    NSInteger vo[kC49Tampons] = {}, fo[kC49Tampons] = {};
    id<MTLTexture> tex[kMaxPSTex]; id<MTLSamplerState> samp[kMaxPSTex];
};
C49Etat g_c49e;
static inline void c49_codeur() {
    if (g_c49e.gen == g_enc_gen) return;
    g_c49e = C49Etat(); g_c49e.gen = g_enc_gen;
}
// n[0..1] pipelines poses/evites, n[2..4] tampons sommets poses/decales/evites,
// n[5..7] idem fragments, n[8..9] textures, n[10..11] echantillonneurs.
static inline void c49_tampon_de(id<MTLRenderCommandEncoder> enc, bool frag, id<MTLBuffer> buf, NSInteger off,
                                 NSUInteger idx) {
    id<MTLBuffer> __strong* b = frag ? g_c49e.fb : g_c49e.vb;
    NSInteger* o = frag ? g_c49e.fo : g_c49e.vo;
    uint64_t* n = g_c49_n + (frag ? 5 : 2);
    if (!g_c49 || idx >= kC49Tampons || b[idx] != buf) {
        if (frag) [enc setFragmentBuffer:buf offset:off atIndex:idx];
        else      [enc setVertexBuffer:buf offset:off atIndex:idx];
        if (idx < kC49Tampons) { b[idx] = buf; o[idx] = off; }
        ++n[0];
    } else if (o[idx] != off) {
        if (frag) [enc setFragmentBufferOffset:off atIndex:idx];
        else      [enc setVertexBufferOffset:off atIndex:idx];
        o[idx] = off; ++n[1];
    } else ++n[2];
}
static inline void c49_tampon(id<MTLRenderCommandEncoder> enc, bool frag, NSInteger off, NSUInteger idx) {
    c49_tampon_de(enc, frag, g_upl_pool, off, idx);
}
// 🎯 C50 (P685) — LES SOMMETS STATIQUES RESTENT SUR LE GPU. Le producteur marque (GfxDraw::res) les flux
// qui sont une tranche d'un tampon statique ; on cree ce tampon UNE fois (newBufferWithBytes, la memoire
// invitee est une carte plate) et on le lie au decalage de la tranche. Les tampons de commandes retiennent
// leurs ressources : evincer un tampon qu'une image en vol lit encore est sans danger.
struct C50Res { id<MTLBuffer> buf; uint32_t n = 0; uint64_t vu = 0; };
std::unordered_map<const void*, C50Res> g_c50_res;
uint64_t g_c50_n[4] = {0};   // flux lies, tampons crees, octets crees, tampons evinces
static bool c50_lier(const uint8_t* base, uint32_t n, const uint8_t* p, id<MTLBuffer> __strong* buf,
                     NSInteger* off) {
    if (!base || !n || p < base || p >= base + n) return false;
    C50Res& e = g_c50_res[base];
    if (!e.buf || e.n != n) {
        e.buf = [g_device newBufferWithBytes:base length:n options:MTLResourceStorageModeShared];
        if (!e.buf) { g_c50_res.erase(base); return false; }
        e.n = n; ++g_c50_n[1]; g_c50_n[2] += n;
    }
    e.vu = g_upl_gen; *buf = e.buf; *off = (NSInteger)(p - base); ++g_c50_n[0];
    return true;
}
// Appele a chaque remise a zero du pool (une image encodee) : tous les 4 096, on rend les tampons qu'aucune
// des 4 096 dernieres images n'a lus (niveau quitte, tampon detruit par le jeu).
static void c50_evincer() {
    if ((g_upl_gen & 4095) != 0) return;
    for (auto it = g_c50_res.begin(); it != g_c50_res.end();)
        if (it->second.vu + 4096 < g_upl_gen) { it = g_c50_res.erase(it); ++g_c50_n[3]; }
        else ++it;
}
static const ShaderRec* c49_plat(ShaderRec* s) {
    if (!g_c49 || !s || s->plat) return s;
    for (uint32_t i = 0; i < kMaxPSTex; i++) {
        auto m = s->texslot.find(i); s->tx_decl[i] = m != s->texslot.end();
        s->tx_slot[i] = s->tx_decl[i] ? m->second : i;
        auto d = s->texdim.find(i); s->tx_dim[i] = d != s->texdim.end() ? d->second : 2u;
        auto sm = s->smpslot.find(i); s->sm_reg[i] = sm != s->smpslot.end() ? sm->second : ~0u;
    }
    for (uint32_t i = 0; i < kMaxCB; i++) {
        auto m = s->cbslot.find(i); s->cb_reg[i] = m != s->cbslot.end() ? m->second : i;
    }
    s->plat = true;
    return s;
}
uint32_t g_backbuffer_rtv_id = 0;
uint32_t g_backbuffer_res_id = 0;
uint64_t g_real_draws = 0, g_real_skipped = 0, g_pipe_built = 0, g_pipe_failed = 0;
// [PRESENT] pipes= pipe_ms= msl_ms= : ce que le fil de la pompe passe à construire pipelines et shaders par fenêtre
// (saccades quand le testeur tourne la vue, P772). Fil de la pompe seulement ; remis à zéro par present_report.
uint32_t g_win_pipes = 0; double g_win_pipe_ms = 0, g_win_msl_ms = 0;
// [PRESENT] cb_vides= : indices de CB que le shader DÉCLARE et que le dessin n'a pas remplis (cumul). Metal garde alors
// le tampon du dessin précédent à cet indice ; D3D lirait le contenu du tampon lié (P772).
uint64_t g_cb_vides = 0;
// --- Live HUD -------------------------------------------------------------
// In the TITLE BAR, deliberately not drawn into the drawable. An on-screen
// overlay would be one more thing painting the surface we are trying to judge,
// and this project has already paid twice for a diagnostic that altered its own
// subject (the step-1 debug quad read as engine output; BO2_UI_FLAT forcing
// blending off). The title costs nothing and cannot lie about the picture.
// TWO frame rates matter and they are not the same number: the pump runs at
// display rate and re-encodes the last frame when the guest has none ready, so
// "window fps" is presentation and "engine fps" is the guest's own Present rate.
// BO2_HUD=0 turns it off. Only meaningful with BO2_WINDOW=1.
std::atomic<uint64_t> g_guest_frames{0};      // guest Presents
uint64_t g_img_uploaded = 0;                  // textures uploaded with real pixels
uint64_t g_img_art = 0;                       // ...of those, bigger than 4x4
size_t   g_hud_n = 0, g_hud_screen = 0, g_hud_pass = 0;
const int g_hud = [] { const char* e = bo2_getenv("BO2_HUD"); return e ? atoi(e) : 1; }();
// Texture resolution, per DECLARED shader-resource slot.
uint64_t g_tx_have = 0, g_tx_nopixels = 0, g_tx_unbound = 0;
// Slots whose DECLARED texture type (3d / cube / depth) we had to satisfy with a
// typed stand-in instead of a real resource. Counted apart from "no pixels":
// a wrong-typed bind is undefined behaviour, not an empty texture.
uint64_t g_tx_wrongtype = 0;
// ...AND BY WHICH TYPE. One aggregate number cannot distinguish "the reflection
// probes never arrive" from "the shadow map is not a depth format", and those
// have completely different fixes. The campaign's terrain renders as a flat
// sand-coloured haze with 1.18M wrong-typed binds; splitting the count is the
// difference between knowing that and guessing which of the three it is.
uint64_t g_tx_wt3d = 0, g_tx_wtcube = 0, g_tx_wtdepth = 0;
// What the depth-wanting slots ACTUALLY had, so "not a depth format" can be told
// from "nothing bound at all" without a second run.
uint64_t g_tx_wtdepth_hadtex = 0;
// A vertex stream an attribute reads from that the guest never gave a stride for.
uint64_t g_vbslot_missing = 0;
uint64_t g_tex3d_made = 0, g_tex3d_short = 0;   // 3D/cube uploads, and ones whose pixels fell short
uint64_t g_rej_attr = 0, g_rej_fmt = 0;
const int g_flat_mode = [] { const char* e = bo2_getenv("BO2_UI_FLAT");
                             return e ? atoi(e) : 0; }();
uint32_t g_flat_seq = 0;          // index of the current draw within the frame
static uint64_t g_lb_groups, g_lb_novp;   // 🎯 LETTERBOX — voir la boucle d'encodage
// 🎯 LB2 — « ECRAN parce que backbuffer » contre « ECRAN PAR DEFAUT ».
// `to_screen = (rtv == 0) || (rtv == g_backbuffer_rtv_id)` : la seconde branche
// est juste (le RTV du backbuffer est bien enregistre, `[gfx] backbuffer RTV =`
// tire dans deux runs). La PREMIERE dit « on n'a pas de cible pour ce groupe,
// donc c'est l'ecran » — une passe d'ombre dont on a rate le
// CreateRenderTargetView atterrirait sur le drawable, etiree par une
// transformation letterbox calibree sur 1920x1080.
// Et le recensement ne pouvait pas le voir : il n'imprimait que le viewport du
// groupe COURANT aux puissances de deux. Douze echantillons sur 16 384 groupes
// ne sont pas une distribution — on histogramme, on n'echantillonne pas.
static uint64_t g_lb_rtv0, g_lb_rtvbb;          // ecran par defaut / vrai backbuffer
// 🎯 LB2.d — `rtv == 0` n'est PAS « on a rate l'enregistrement ». Le shim fait
//   case 33: g_cur_rtv = pp ? win32_mem_read32(pp) : 0;   // OMSetRenderTargets
// donc `OMSetRenderTargets(0, NULL, pDSV)` — le prologue standard d'une passe
// PROFONDEUR SEULE (carte d'ombre, pre-passe, occlusion) — donne rtv = 0, et
// `to_screen` devient vrai : le groupe est encode DANS LE DRAWABLE alors que
// l'invite n'a jamais demande l'ecran. Le DSV separe les trois cas.
static uint64_t g_lb_rtv0_dsv;                  // rtv==0 ET dsv!=0 : profondeur seule
static uint64_t g_lb_rtv0_nodsv;                // rtv==0 ET dsv==0 : aucune cible du tout
static uint64_t g_lb_rtv0_avant;                // rtv==0 avant qu'un rtv non nul ait ete vu
static bool     g_lb_vu_rtv;                    // un rtv non nul a-t-il deja ete vu ?
// ⚠️ ET LA QUESTION QUI SEPARE « gachis invisible » DE « corruption visible » :
// ces dessins ECRIVENT-ILS DE LA COULEUR ? Une passe profondeur seule a
// normalement un masque d'ecriture nul. Le recensement `[LAST]` ne peut PAS y
// repondre : il n'echantillonne que le DERNIER dessin ecran d'une image sur 120,
// c'est-a-dire le composite d'interface, qui ecrit RGBA par construction (64/64
// `mask=f` en p557). On compte donc ici, sur les dessins des groupes rtv==0.
static uint64_t g_lb_rtv0_d_muet;               // masque d'ecriture NUL : invisible
static uint64_t g_lb_rtv0_d_peint;              // masque non nul : ca PEINT l'ecran
static uint32_t g_lb_vp_w[8], g_lb_vp_h[8];     // tailles de viewport distinctes
static uint64_t g_lb_vp_n[8], g_lb_vp_r0[8];    // total, et la part venue de rtv==0
static uint32_t g_lb_vp_nv;
static uint64_t g_lb_vp_deborde;                // au-dela de la 8e taille
int      g_legend_frame = 0;      // dump this frame's screen draw list
uint32_t g_screen_total = 0;      // screen draws in the frame being encoded
std::vector<size_t> g_screen_idx; // their indices, for the tail dump
uint32_t g_sweep_n = 0;           // BO2_UI_SWEEP cutoff for this frame
const int g_probe_pix = [] { return bo2_getenv("BO2_UI_PROBEPIX") ? 1 : 0; }();
id<MTLBuffer> g_pix_buf = nil;    // 16-byte slots: drawable, then render targets
uint32_t g_probe_rt[16] = {0};
// WHERE each slot was sampled. Two different loops fill g_probe_rt — one
// reads texel(0,0), the other the CENTRE — and both used to print
// "texel(0,0)". A probe whose label does not match its data is how this
// project has already named innocent shaders twice.
bool     g_probe_rt_mid[16] = {false};
int      g_probe_rt_n = 0;
uint32_t g_probe_want[12] = {0};
// The colour-grading LUT strip, sampled at 8 x-positions (see the blit that
// fills it). Slots 16..23 of g_pix_buf, so it never collides with the 16
// render-target slots above it.
uint32_t g_lut_probe_res = 0, g_lut_probe_w = 0; int g_lut_probe_n = 0;
uint32_t g_probe_want_ps[12] = {0};
uint32_t g_probe_rt_ps[16] = {0};
int      g_probe_want_n = 0;
std::unordered_map<uint32_t, uint64_t> g_screen_ps;   // ps -> indices drawn to screen
std::unordered_map<uint32_t, uint32_t> g_screen_ps_blend;  // ps -> (mask<<4)|enable
std::unordered_map<uint32_t, uint32_t> g_screen_ps_masks;
std::unordered_map<uint32_t, uint64_t> g_watch_census;   // BO2_UI_WATCHRT: ps -> indices into that target  // ps -> set of write masks seen (bit per mask)
std::unordered_map<uint32_t, uint64_t> g_tx_missing_id;
// Offscreen render targets. BO2 composites its menu out of these, so they must be
// real textures we can both render into and later sample: the guest's own
// composite draw binds the same resource as a shader resource, and g_texreg is
// keyed by that resource id, so one entry serves both roles.
struct RTStage { uint32_t obj, w, h, fmt, mips; };
std::mutex               g_rt_mu;
std::vector<RTStage>     g_rt_pending;
std::vector<std::pair<uint32_t,uint32_t>> g_rtv_pending;   // rtv -> resource
std::unordered_map<uint32_t, uint32_t>    g_rtv_res;
std::set<uint32_t>                        g_rtv_cibles;   // P908 : ressources qui ont reçu une RTV (vraies cibles, [MIP-LECTEUR])
std::vector<std::pair<uint32_t,uint32_t>> g_rtv_mip_pending;   // rtv -> MipSlice, when not 0
std::unordered_map<uint32_t, uint32_t>    g_rtv_mip;           // rtv -> MipSlice (absent = 0)
std::vector<uint32_t>                    g_rt_list;   // render targets, creation order
std::set<uint32_t>                       g_rt_cleared; // targets given their first clear
std::unordered_map<uint32_t, uint64_t>   g_rt_written; // target -> draws aimed at it
std::unordered_map<uint32_t, std::set<uint32_t>> g_rt_writers;  // target -> pixel shaders

// --- BO2_SHOT: read the presented frame back and write a PNG ------------------
//
// `screencapture -l<winid>` stopped working on this macOS (it answers "could not
// create image from window", and CGWindowListCreateImage is now unavailable in
// the SDK), so the project lost its only way to LOOK at its own output. That is
// exactly the failure mode this port has been burned by: the audio pipeline ran
// at 100% of real time carrying digital silence for a whole day because every
// metric measured the plumbing. A picture of the drawable is the pixel
// equivalent of BO2_AUDIO_WAV, it needs no OS permission, and it cannot be
// fooled by an occluded window or a locked screen.
//
// Triggered two ways, both off by default:
//   BO2_SHOT=<prefix>       where to write (default /tmp/bo2_frame)
//   BO2_SHOT_EVERY=<n>      grab every n presented frames
//   kill -USR1 <pid>        grab the very next frame
std::atomic<int> g_shot_req{0};
const char*  g_shot_prefix = nullptr;
uint64_t     g_shot_every  = 0;
uint32_t     g_shot_seq    = 0;
id<MTLBuffer> g_shot_buf   = nil;
uint32_t     g_shot_w = 0, g_shot_h = 0;
// BO2_RT_DUMP=<prefix> — the same, for every engine render target. Rides the
// same USR1, because the interesting question is always "what did the targets
// hold on the frame I am looking at", not "on some frame".
std::atomic<int> g_rt_dump_req{0};
// BO2_RT_SWEEP=<steps>: walk a cutoff on the number of draws into the watched
// target, one step per dumped frame, photographing the target at each step.
uint32_t g_rtsweep_steps = 0;      // 0 = off
uint32_t g_rtsweep_cut   = 0;      // draws allowed into the target this frame
uint32_t g_rtsweep_seen  = 0;      // draws aimed at it so far this frame
uint32_t g_rtsweep_total = 0;      // how many it wanted last frame
uint32_t g_rtsweep_step  = 0;      // which step we are on
uint32_t g_watchrt_res   = 0;      // BO2_UI_WATCHRT, parsed once here too
const char*  g_rt_dump_prefix = nullptr;
id<MTLBuffer> g_rt_buf = nil;
}  // namespace

extern "C" void bo2_gfx_request_shot() { g_shot_req.store(1); g_rt_dump_req.store(1); }
static void bo2_shot_signal(int) { g_shot_req.store(1); g_rt_dump_req.store(1); }

// Write a BGRA8 buffer out as a PNG. CoreGraphics is already linked here, and
// going through CGImageDestination avoids inventing an encoder.
// Bytes per pixel for the render-target formats this engine actually creates.
// Returns 0 for anything not handled, so the dump can SKIP it loudly instead of
// copying it with the wrong stride and producing a convincing-looking smear.
static inline size_t rt_bytes_per_pixel(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatR32Float:
        case MTLPixelFormatRG16Float:
            return 4;
        case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA16Unorm:
            return 8;
        case MTLPixelFormatRGBA32Float:
            return 16;
        case MTLPixelFormatR8Unorm:  return 1;
        case MTLPixelFormatRG8Unorm: case MTLPixelFormatR16Float: return 2;
        default: return 0;
    }
}

static std::vector<std::pair<uint32_t,size_t>> g_depth_shots;
static std::unordered_map<uint32_t, id<MTLTexture>> g_depth_tex_by_key;
static id<MTLBuffer> g_depth_buf = nil;

// 🎯 WHY ARE DRAWS SKIPPED? The [gfx] line has always printed a TOTAL
// ("1,289,328 issued, 4,634 skipped") and never a reason, so 4,634 vanished
// draws could be harmless topology filtering or could be the entire world pass
// and nothing in the log could tell them apart. One counter per site.
static uint64_t g_skip_why[16] = {0};
static const char* const g_skip_why_name[16] = {
    "early-out", "pipeline-build-failed-a", "pipeline-build-failed-b",
    "no-pipeline", "BO2_UI_PSSKIP", "unknown-1", "unknown-2", "cut-list",
    "BO2_SKIP_PS", "BO2_UI_DROPLAST", "topology-not-tri", "vertex-upload-failed",
    "", "", "", ""
};
extern "C" void gfx_skip_report() {
    uint64_t tot = 0;
    for (int i = 0; i < 16; i++) tot += g_skip_why[i];
    if (!tot) return;
    fprintf(stderr, "[SKIP-WHY] %llu skipped draw(s):", (unsigned long long)tot);
    for (int i = 0; i < 16; i++)
        if (g_skip_why[i])
            fprintf(stderr, " %s=%llu", g_skip_why_name[i][0] ? g_skip_why_name[i] : "?",
                    (unsigned long long)g_skip_why[i]);
    fprintf(stderr, "\n");
}

// Row pitch the pixel format itself implies, so a guest-supplied pitch can be
// checked instead of trusted. 0 = a format this does not model (say nothing
// rather than accuse a correct pitch).
static NSUInteger expected_bpr(MTLPixelFormat f, uint32_t w) {
    switch (f) {
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
        case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
            return (NSUInteger)w * 4;
        case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB:
        case MTLPixelFormatBC4_RUnorm:
            return (NSUInteger)((w + 3) / 4) * 8;          // 8 bytes per 4x4 block
        case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC3_RGBA_sRGB:
        case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC7_RGBAUnorm:
        case MTLPixelFormatBC7_RGBAUnorm_sRGB:
            return (NSUInteger)((w + 3) / 4) * 16;         // 16 bytes per 4x4 block
        case MTLPixelFormatR8Unorm:  return (NSUInteger)w;
        case MTLPixelFormatRG8Unorm: return (NSUInteger)w * 2;
        default: return 0;
    }
}

static void bo2_write_png(const uint8_t* bgra, uint32_t w, uint32_t h,
                          size_t bytes_per_row, const char* path) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        (void*)bgra, w, h, 8, bytes_per_row, cs,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    if (!ctx) { CGColorSpaceRelease(cs); fprintf(stderr, "[SHOT] no bitmap ctx\n"); return; }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageDestinationRef dst =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
    bool ok = false;
    if (dst && img) {
        CGImageDestinationAddImage(dst, img, NULL);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    if (img) CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    fprintf(stderr, "[SHOT] %s %ux%u %s\n", path, w, h, ok ? "ok" : "FAILED");
}

// Guest thread: register a shader, already translated to MSL, under the guest's
// object id. Compiling it into an MTLFunction has to happen on the main thread,
// so this only stages the source. What this proves today is the full chain
// DXBC -> MSL -> Metal compiler; wiring the compiled functions into a pipeline
// per bound VS/PS pair (plus input layouts, constant buffers and blend state)
// is the remaining work.
extern "C" void gfx_register_shader2(uint32_t obj, int is_pixel, const char* msl,
                                    const char* attrs) {
    if (!g_open.load() || !obj || !msl) return;
    std::lock_guard<std::mutex> lk(g_shader_mu);
    if (g_shader_pending.size() > 8192) return;
    g_shader_pending.push_back({ obj, is_pixel != 0, std::string(msl),
                                 attrs ? std::string(attrs) : std::string() });
}

// Guest thread: register an uploaded texture under the guest's object id.
extern "C" void gfx_register_texture(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt,
                                     const void* data, uint32_t bytes, uint32_t pitch) {
    if (!obj || !data || !bytes || !w || !h) return;
    if (dxgi_to_metal(fmt) == MTLPixelFormatInvalid) return;
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    if (g_texreg_pending.size() > 4096) return;          // never grow unbounded
    TexStage ts;
    ts.bytes.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
    ts.w = w; ts.h = h; ts.fmt = fmt; ts.pitch = pitch;
    g_texreg_pending.emplace_back(obj, std::move(ts));
}

// A CUBE WITH ITS OWN MIP CHAIN. Block-compressed cubes (the reflection probes
// are BC1/BC3) cannot have mips generated by Metal, and the material shader
// samples the probe at an explicit level chosen by surface roughness — so
// without the guest's own prefiltered chain every surface reflects a
// mirror-sharp level 0. `offsets` and `pitches` hold mips*6 entries, face-major
// (D3D subresource order: face*MipLevels + mip).
// 🎯 A PLAIN 2D TEXTURE WITH ITS OWN MIP CHAIN.
//
// The 2D path used to take level 0 only and the descriptor below was built
// `mipmapped:NO`, so every sample clamped to the sharpest level. On a surface
// viewed at a grazing angle that produces the long streaks the tester photographed
// all over afghanistan. These textures are BC-compressed, so Metal cannot build
// the chain — the game ships prefiltered mips and this takes them.
extern "C" void gfx_register_texture_mips(uint32_t obj, uint32_t w, uint32_t h,
                                          uint32_t fmt, uint32_t mips, const void* data,
                                          uint32_t bytes, const uint32_t* offsets,
                                          const uint32_t* pitches) {
    if (!obj || !data || !bytes || !w || !h || !mips) return;
    if (dxgi_to_metal(fmt) == MTLPixelFormatInvalid) return;
    if (mips > 16) mips = 16;
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    if (g_texreg_pending.size() > 4096) return;          // never grow unbounded
    TexStage ts;
    ts.bytes.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
    ts.w = w; ts.h = h; ts.fmt = fmt; ts.pitch = pitches[0];
    ts.depth = 1; ts.faces = 1; ts.mips = mips;
    ts.mip_off.assign(offsets, offsets + mips);
    ts.mip_pitch.assign(pitches, pitches + mips);
    g_texreg_pending.emplace_back(obj, std::move(ts));
}

extern "C" void gfx_register_cube_mips(uint32_t obj, uint32_t size, uint32_t fmt,
                                       uint32_t mips, const void* data, uint32_t bytes,
                                       const uint32_t* offsets, const uint32_t* pitches) {
    if (!obj || !data || !bytes || !size || !mips) return;
    if (mips > 16) mips = 16;
    if (dxgi_to_metal(fmt) == MTLPixelFormatInvalid) return;
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    if (g_texreg_pending.size() > 4096) return;
    TexStage ts;
    ts.bytes.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
    ts.w = size; ts.h = size; ts.fmt = fmt; ts.pitch = pitches[0];
    ts.depth = 1; ts.faces = 6; ts.mips = mips;
    ts.mip_off.assign(offsets, offsets + mips * 6);
    ts.mip_pitch.assign(pitches, pitches + mips * 6);
    g_texreg_pending.emplace_back(obj, std::move(ts));
}

// The same, for a VOLUME (depth > 1) or a CUBE (faces == 6). These are the two
// types the room's material shaders sample for their baked lighting and their
// reflections, and until now the backend could not express either — so both
// slots were served by a 1x1 stand-in and the room had no baked lighting at all.
extern "C" void gfx_register_texture_vol(uint32_t obj, uint32_t w, uint32_t h,
                                         uint32_t depth, uint32_t faces, uint32_t fmt,
                                         const void* data, uint32_t bytes,
                                         uint32_t pitch, uint32_t slice_pitch) {
    if (!obj || !data || !bytes || !w || !h) return;
    if (dxgi_to_metal(fmt) == MTLPixelFormatInvalid) return;
    if (depth < 1) depth = 1;
    if (faces != 6) faces = 1;
    if (depth == 1 && faces == 1) return;         // caller wanted the 2D entry point
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    if (g_texreg_pending.size() > 4096) return;
    TexStage ts;
    ts.bytes.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
    ts.w = w; ts.h = h; ts.fmt = fmt; ts.pitch = pitch;
    ts.depth = depth; ts.faces = faces; ts.slice_pitch = slice_pitch;
    g_texreg_pending.emplace_back(obj, std::move(ts));
}

// 🎯 C35 (2026-09-26) — LES OCTETS D'UNE IMAGE VONT DANS UNE ARENE, PAS DANS DES
// DIZAINES DE MILLIERS DE VECTEURS.
// Sample p675 222758, fil backend : gfx_real_draw 21,4 %, dont vector::assign 16,3 %
// et operator new 6,1 %, attente de g_rd_mu 3,7 %. Chaque RealDrawRec portait jusqu'a
// 22 std::vector. gfx_real_frame_end faisait `g_rd.clear()` en disant garder leur
// capacite, mais clear() DETRUIT les enregistrements : ~3 700 dessins par image
// reallouaient tout, et la pompe liberait l'image precedente sous g_rd_mu.
// Desormais chaque image (RdFrame) a son arene. Ses morceaux sont gardes d'une image a
// l'autre, et les enregistrements ne tiennent que des tranches : ils sont POD, se
// detruisent sans rien liberer, et un clear() est O(1).
template <class T> struct RdSpan {
    const T* p = nullptr;
    uint32_t n = 0;
    const T* data() const { return p; }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    const T* begin() const { return p; }
    const T* end() const { return p + n; }
    const T& operator[](size_t i) const { return p[i]; }
    void clear() { p = nullptr; n = 0; }
};
class RdArena {
public:
    // Copie `count` elements dans l'arene ; la tranche vit jusqu'au reset() de l'arene.
    template <class T> RdSpan<T> copy(const T* src, uint32_t count) {
        RdSpan<T> s;
        if (!src || !count) return s;
        const size_t bytes = (size_t)count * sizeof(T);
        uint8_t* dst = alloc(bytes);
        std::memcpy(dst, src, bytes);
        s.p = reinterpret_cast<const T*>(dst); s.n = count;
        return s;
    }
    // Garde les morceaux (c'est tout l'interet), sauf au-dela de deux de rabiot sur ce
    // que l'image qui finit a utilise : un pic ne reste pas tenu pour toujours.
    // C44 : une arene deja videe ne se retaille pas. La pompe vide l'image qu'elle rend
    // (draws_f.clear), gfx_real_frame_end la recoit par swap et la revide : avec cur_ = 0,
    // elle tombait a 3 morceaux, et chaque image (42-64 Mo) en reallouait 3 a 5 de 8 Mo.
    void reset() {
        if (cur_ == 0 && off_ == 0) return;
        const size_t garder = cur_ + 3;
        if (chunks_.size() > garder) { chunks_.resize(garder); tailles_.resize(garder); }
        cur_ = 0; off_ = 0;
    }
    uint64_t held() const { uint64_t t = 0; for (size_t z : tailles_) t += z; return t; }
    // Morceaux alloues depuis le lancement, toutes arenes (temoin C44, ligne [C35]).
    static inline std::atomic<uint64_t> g_morceaux{0};
    void swap(RdArena& o) noexcept {
        chunks_.swap(o.chunks_); tailles_.swap(o.tailles_);
        std::swap(cur_, o.cur_); std::swap(off_, o.off_);
    }
private:
    static constexpr size_t kChunk = 8u << 20;
    uint8_t* alloc(size_t bytes) {
        bytes = (bytes + 15) & ~size_t(15);
        while (cur_ < chunks_.size() && off_ + bytes > tailles_[cur_]) { ++cur_; off_ = 0; }
        if (cur_ == chunks_.size()) {
            const size_t z = std::max(kChunk, bytes);
            // C44 : sans mise a zero (make_unique<T[]> initialise) : chaque octet est ecrit
            // par copy() avant d'etre lu.
            chunks_.push_back(std::unique_ptr<uint8_t[]>(new uint8_t[z]));
            g_morceaux.fetch_add(1, std::memory_order_relaxed);
            tailles_.push_back(z);
            off_ = 0;
        }
        uint8_t* p = chunks_[cur_].get() + off_;
        off_ += bytes;
        return p;
    }
    std::vector<std::unique_ptr<uint8_t[]>> chunks_;
    std::vector<size_t> tailles_;
    size_t cur_ = 0, off_ = 0;
};

// Guest thread: one fully-described DrawIndexed. The bytes are copied here
// because the pump runs later on another thread and the guest reuses its buffers.
// C35 : copies dans l'arene de l'image (RdFrame), voir plus haut.
struct RealDrawRec {
    uint32_t vs, ps;
    RdSpan<GfxDrawElem> elems;
    RdSpan<uint8_t> vb, ib;
    uint32_t vb_stride, ib_fmt, idx_count; int32_t base_vtx; uint32_t topo, rtv;
    RdSpan<uint8_t> vb2[kMaxVBStreams - 1];
    uint32_t vb2_stride[kMaxVBStreams - 1];
    uint32_t is_clear; float clear_rgba[4];
    // ⚠️ SIX flottants, comme GfxDraw : x, y, w, h, MinDepth, MaxDepth.
    // A quatre, la plage de profondeur etait perdue a la copie et relue
    // hors du tableau (dans is_copy/copy_dst) : le modele de vue prenait
    // 0..1 et se faisait OCCULTER PAR LES HERBES. Le static_assert plus
    // bas empeche cette troisieme declaration de rederiver.
    float vp[6];
    uint32_t is_copy, copy_dst, copy_src;
    bool to_screen;
    GfxBlend blend;
    GfxDepth depth;
    uint32_t dsv;
    RdSpan<uint8_t> vscb[kMaxCB], pscb[kMaxCB];
    uint32_t pstex[kMaxPSTex];
    uint32_t pssamp[kMaxPSTex];          // P805
    float vpw, vph;
    const uint8_t* res[kMaxVBStreams]; uint32_t res_bytes[kMaxVBStreams];   // C50, voir GfxDraw::res
    uint32_t rs; int32_t sc[4];          // voir GfxDraw::rs
    uint32_t dsv_res, dsv_wh;            // voir GfxDraw::dsv_res
    int32_t zb; float zb_pente, zb_max;  // voir GfxDraw::zb
    uint32_t psmip[kMaxPSTex];           // voir GfxDraw::psmip
};
static_assert(kMaxVBStreams == 4, "C50 : GfxDraw::res a quatre flux");
static_assert(std::is_trivially_destructible<RealDrawRec>::value,
              "C35 : un RealDrawRec ne possede rien, ses octets sont dans l'arene de son image");
// Une image : ses enregistrements et l'arene qui tient leurs octets. Les deux voyagent
// ensemble (construction -> publiee -> encodee par la pompe -> rendue a la construction).
struct RdFrame {
    std::vector<RealDrawRec> recs;
    RdArena arena;
    void clear() { recs.clear(); arena.reset(); }
    void swap(RdFrame& o) noexcept { recs.swap(o.recs); arena.swap(o.arena); }
};
// 🎯 LA TROISIEME DECLARATION. GfxDraw est verifiee a l'execution contre le
// producteur (gfx_check_record_layout). RealDrawRec, elle, n'etait verifiee
// par rien — et c'est elle qui a perdu MinDepth/MaxDepth. Ce test est fait
// a la COMPILATION : toute divergence future casse le build au lieu de
// rendre un viewmodel occulte en silence.
static_assert(sizeof(RealDrawRec::vp) == sizeof(GfxDraw::vp),
              "RealDrawRec::vp doit suivre GfxDraw::vp champ pour champ "
              "(x, y, w, h, MinDepth, MaxDepth)");
namespace {
std::mutex g_rd_mu; RdFrame g_rd, g_rd_frame;
// Numero des images publiees (g_rd_seq, celui de g_rd_frame) sous g_rd_mu, et
// celui de l'image que la pompe encode (`draws`, re-encodee tant qu'aucune neuve
// n'arrive). C'est l'horloge de gfx_release_texture.
uint64_t g_rd_seq = 0, g_rd_frame_seq = 0;
std::atomic<uint64_t> g_draws_seq{0};
// A PER-FRAME RECORD CAP IS A HEAD CUT, NOT A THROTTLE. This list was bounded at
// 1024 records while the frontend submits several thousand draws per frame AND
// DRAWS ITS UI LAST — so every menu draw was thrown away here, after the shim had
// already decoded it. Exactly the bug that the shim-side cap had (200 -> 8000);
// it existed twice, in two files. Bound by BYTES as well, so a frame that decodes
// megabyte vertex windows still cannot exhaust memory, and COUNT the drops: a
// silent cap reads as "the engine drew nothing".
uint32_t g_max_recs = [] { const char* e = bo2_getenv("BO2_UI_MAXRECS");
                           return e ? (uint32_t)strtoul(e, nullptr, 10) : 65536u; }();
uint64_t g_max_bytes = [] { const char* e = bo2_getenv("BO2_UI_MAXBYTES");
                            return e ? strtoull(e, nullptr, 10) : (512ull << 20); }();
uint64_t g_rd_bytes = 0;                 // bytes held by the frame being built
// C47 : epoque de l'arene en construction (++ a chaque gfx_real_frame_end), et ce que l'image en cours
// a repris au lieu de copier. Tenus sous g_rd_mu.
uint64_t g_rd_epoch = 1, g_c47_repris = 0, g_c47_evites = 0;
uint64_t g_c47_verifs = 0, g_c47_ecarts = 0;   // cumules depuis le lancement
uint64_t g_rd_drop_recs = 0, g_rd_drop_bytes = 0;
// True when this frame can take one more record of n bytes. Caller holds g_rd_mu.
bool rd_admit(uint64_t n) {
    if (g_max_recs && g_rd.recs.size() >= g_max_recs) { g_rd_drop_recs++; return false; }
    if (g_max_bytes && g_rd_bytes + n > g_max_bytes) { g_rd_drop_bytes++; return false; }
    g_rd_bytes += n;
    return true;
}
}

namespace {
// BO2_PHOTO_TRACES=1 : les tracés de l'image photographiée (USR1), regroupés par (cible, shaders, mélange, textures)
// dans l'ordre d'apparition, imprimés avec la photo (10-21 : fond de CHOOSE CLASS gris).
struct PhotoTrace { uint32_t rtv; bool ecran; uint64_t ps, vs; GfxBlend bl; uint32_t tex0, tex1, n; uint32_t vx[9]; };
std::vector<PhotoTrace> g_photo_tr;
bool g_photo_rec = false;   // l'image en cours d'encodage est neuve (un re-encodage saute les cibles hors ecran)
void photo_trace_note(const RealDrawRec& r, uint64_t ps_h) {
    auto v = g_shader_rec.find(r.vs);
    const uint64_t vs_h = v != g_shader_rec.end() ? v->second.msl_h : 0;
    if (!g_photo_tr.empty()) {
        PhotoTrace& b = g_photo_tr.back();
        if (b.rtv == r.rtv && b.ps == ps_h && b.vs == vs_h && !memcmp(&b.bl, &r.blend, sizeof b.bl) &&
            b.tex0 == r.pstex[0] && b.tex1 == r.pstex[1]) { ++b.n; return; }
    }
    if (g_photo_tr.size() < 4000)
        g_photo_tr.push_back({ r.rtv, r.to_screen, ps_h, vs_h, r.blend, r.pstex[0], r.pstex[1], 1, {} });
    // BO2_TRACE_MSL=<hachage> : le 1er sommet brut de ce nuanceur (pas, puis 8 mots) — couleur de sommet du voile de menu.
    static const uint64_t th = [] { const char* e = bo2_getenv("BO2_TRACE_MSL"); return e ? strtoull(e, nullptr, 16) : 0ull; }();
    if (th == ps_h && !g_photo_tr.empty() && r.vb.size() >= 4) {
        uint32_t* vx = g_photo_tr.back().vx;
        vx[0] = r.vb_stride;
        memcpy(vx + 1, r.vb.data(), std::min<size_t>(32, r.vb.size()));
    }
}
// Passes et copies dans la meme liste : ps = 0xC1EA (passe qui efface), 0x10AD (passe qui charge), 0xC0B1 (CopyResource, tex0 = source).
void photo_trace_mark(uint32_t rtv, bool ecran, uint64_t tag, uint32_t src) {
    static const bool on = bo2_env_on("BO2_PHOTO_TRACES", false);
    if (on && g_photo_rec && g_photo_tr.size() < 4000) g_photo_tr.push_back({ rtv, ecran, tag, 0, {}, src, 0, 1, {} });
}
void photo_trace_flush() {
    static uint32_t nphoto = 0;
    ++nphoto;
    for (size_t i = 0; i < g_photo_tr.size(); i++) {
        const PhotoTrace& t = g_photo_tr[i];
        const auto rr = g_rtv_res.find(t.rtv);
        fprintf(stderr, "[PHOTO-TRACES] photo %u #%zu %s %08x res=%08x ps=%016llx vs=%016llx x%u melange=%u(%u,%u,%u|%u,%u,%u) "
                        "masque=%x tex0=%08x tex1=%08x\n", nphoto, i, t.ecran ? "ECRAN" : "cible", t.rtv,
                rr != g_rtv_res.end() ? rr->second : 0u, (unsigned long long)t.ps, (unsigned long long)t.vs, t.n, t.bl.enable, t.bl.src_rgb, t.bl.dst_rgb,
                t.bl.op_rgb, t.bl.src_a, t.bl.dst_a, t.bl.op_a, t.bl.write_mask, t.tex0, t.tex1);
        if (t.vx[0]) {
            const auto ti = g_texinfo.find(t.tex0);
            fprintf(stderr, "[PHOTO-TRACES] photo %u #%zu sommet pas=%u : %08x %08x %08x %08x %08x %08x %08x %08x tex0=%ux%u fmt=%u px=%u,%u,%u,%u%s\n",
                    nphoto, i, t.vx[0], t.vx[1], t.vx[2], t.vx[3], t.vx[4], t.vx[5], t.vx[6], t.vx[7], t.vx[8],
                    ti != g_texinfo.end() ? ti->second.w : 0, ti != g_texinfo.end() ? ti->second.h : 0,
                    ti != g_texinfo.end() ? ti->second.fmt : 0, ti != g_texinfo.end() ? ti->second.px[0] : 0,
                    ti != g_texinfo.end() ? ti->second.px[1] : 0, ti != g_texinfo.end() ? ti->second.px[2] : 0,
                    ti != g_texinfo.end() ? ti->second.px[3] : 0, g_texreg.count(t.tex0) ? "" : " ABSENTE-METAL");
        }
    }
    g_photo_tr.clear();
}
// Encode one already-described draw into an open render pass. Split out of the
// pump so the same code serves every render target.
void encode_real_draw(id<MTLRenderCommandEncoder> enc2, RealDrawRec& r,
                      MTLPixelFormat target_fmt, MTLPixelFormat st_fmt) {
    const ShaderRec* ps_rec = nullptr;
    { auto it = g_shader_rec.find(r.ps); if (it != g_shader_rec.end()) ps_rec = c49_plat(&it->second); }

                auto vsit = g_shader_rec.find(r.vs), psit = g_shader_rec.find(r.ps);
                if (vsit == g_shader_rec.end() || psit == g_shader_rec.end()) {
                    (g_real_skipped++, g_skip_why[0]++); return;
                }
                uint64_t key = ((uint64_t)r.vs << 32) ^ (uint64_t)r.ps ^
                               ((uint64_t)r.elems.size() << 56) ^
                               ((uint64_t)r.vb_stride << 48) ^
                               ((uint64_t)target_fmt * 0x9E3779B97F4A7C15ull) ^
                               (blend_key(r.blend) * 0xC2B2AE3D27D4EB4Full) ^
                               ((uint64_t)st_fmt * 0x94D049BB133111EBull);   // Invalid = 0: key unchanged
                id<MTLRenderPipelineState> pipe = nil;
                auto pit = g_pipe_cache.find(key);
                if (pit != g_pipe_cache.end()) pipe = pit->second;
                else {
                    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
                    int mapped = 0;
                    bool used_slot[kMaxVBStreams] = {false};
                    static uint64_t rep_map = 0;
                    // BO2_ATTR_LOG=<n> — how many pipelines to report the vertex
                    // attribute mapping for. The hardcoded 4 only ever reached the
                    // UI shaders (POSITION/COLOR/TEXCOORD0), and the question that
                    // matters is what the TERRAIN shader asks for and whether each
                    // attribute is read from the right stream at the right stride.
                    static const long attr_log = [] {
                        const char* e = bo2_getenv("BO2_ATTR_LOG");
                        return e ? atol(e) : 4L;
                    }();
                    bool tell = ((long)rep_map++ < attr_log);
                    for (const GfxDrawElem& e : r.elems) {
                        auto a = vsit->second.attrs.find(e.semantic);
                        MTLVertexFormat mf = dxgi_to_vertex_format(e.fmt);
                        // Both rejection reasons are invisible in the picture, and
                        // "1 of 3 attributes mapped" is what Metal then complains
                        // about, so name them per element.
                        if (tell)
                            fprintf(stderr, "[gfx] elem %-12s fmt=%-3u off=%-3u -> %s%s\n",
                                    e.semantic, e.fmt, e.offset,
                                    a == vsit->second.attrs.end() ? "NOT IN VS" : "attr ok",
                                    mf == MTLVertexFormatInvalid ? " / UNMAPPED FORMAT" : "");
                        // SILENT RETURNS. These two dropped the draw without
                        // counting it and without caching the decision, so the
                        // same pair was re-examined every frame and never showed
                        // up in "skipped" — a whole shader's draws could vanish
                        // and every counter still read healthy.
                        if (a == vsit->second.attrs.end()) {
                            if (++g_rej_attr <= 12)
                                fprintf(stderr, "[gfx] pipeline REJECTED vs=%08x ps=%08x:"
                                                " layout has %s, the VS does not declare"
                                                " it\n", r.vs, r.ps, e.semantic);
                            g_pipe_cache[key] = nil; (g_real_skipped++, g_skip_why[1]++); return;
                        }
                        if (mf == MTLVertexFormatInvalid) {
                            if (++g_rej_fmt <= 12)
                                fprintf(stderr, "[gfx] pipeline REJECTED vs=%08x ps=%08x:"
                                                " %s has unmapped DXGI format %u\n",
                                        r.vs, r.ps, e.semantic, e.fmt);
                            g_pipe_cache[key] = nil; (g_real_skipped++, g_skip_why[2]++); return;
                        }
                        vd.attributes[a->second].format      = mf;
                        vd.attributes[a->second].offset      = e.offset;
                        // EACH ELEMENT COMES FROM ITS OWN STREAM. Pinning every
                        // attribute to stream 0 was only correct for single-stream
                        // draws; the room's materials use two, and reading a slot-1
                        // attribute out of slot 0's buffer at slot 0's stride is how
                        // positions came out as 1e37.
                        // ⚠️ A SLOT ABOVE THE LIMIT IS NOT DROPPED — IT IS
                        // FOLDED ONTO SLOT 0, which silently reads a completely
                        // different buffer at a different stride. The guest side
                        // tracks kMaxVBSlots=8 streams; this backend carries only
                        // kMaxVBStreams=4, so elements in slots 4..7 land on slot
                        // 0's positions. That is exactly how a mesh comes out
                        // smeared, and it is the shape of the band the tester saw
                        // across the lower third of angola. COUNT IT.
                        if (e.slot >= kMaxVBStreams) {
                            static uint32_t nfold = 0; static uint32_t hislot = 0;
                            if (e.slot > hislot) hislot = e.slot;
                            if (++nfold <= 8 || (nfold % 50000) == 0)
                                fprintf(stderr, "[gfx] VB-SLOT FOLDED: element slot %u >= "
                                                "kMaxVBStreams=%u, remapped to 0 — reads the "
                                                "WRONG buffer. #%u, highest slot %u\n",
                                        e.slot, (unsigned)kMaxVBStreams, nfold, hislot);
                        }
                        // 🎯 REPORT THE RANGE OF TEXCOORD0 ON REAL VERTICES.
                        // Everything downstream is cleared — mips, pitch, sampler,
                        // attribute formats, texture bytes — so the streak has to
                        // come from the numbers fed to the sampler. A UV range of
                        // 0..1 is a healthy mesh; 0..1000 stretches one texel
                        // across a whole cliff, which is exactly what a smear is.
                        if (tell && !strncmp(e.semantic, "TEXCOORD", 8) &&
                            e.semantic[8] == '0' && !r.vb.empty() && r.vb_stride) {
                            float lo0 = 1e30f, hi0 = -1e30f, lo1 = 1e30f, hi1 = -1e30f;
                            const size_t nv = r.vb.size() / r.vb_stride;
                            for (size_t vi = 0; vi < nv && vi < 4096; vi++) {
                                const uint8_t* vp = r.vb.data() + vi * r.vb_stride + e.offset;
                                if (vp + 8 > r.vb.data() + r.vb.size()) break;
                                float u = 0, v = 0;
                                if (e.fmt == 16) {                    // R32G32_FLOAT
                                    memcpy(&u, vp, 4); memcpy(&v, vp + 4, 4);
                                } else if (e.fmt == 34) {             // R16G16_FLOAT
                                    uint16_t h0, h1; memcpy(&h0, vp, 2); memcpy(&h1, vp + 2, 2);
                                    auto half = [](uint16_t x) {
                                        const uint32_t sgn = (uint32_t)(x & 0x8000) << 16;
                                        uint32_t exp = (x >> 10) & 0x1f, man = x & 0x3ff, out;
                                        if (!exp) out = sgn;
                                        else if (exp == 31) out = sgn | 0x7f800000u | (man << 13);
                                        else out = sgn | ((exp + 112) << 23) | (man << 13);
                                        float f; memcpy(&f, &out, 4); return f;
                                    };
                                    u = half(h0); v = half(h1);
                                } else continue;
                                if (u < lo0) lo0 = u; if (u > hi0) hi0 = u;
                                if (v < lo1) lo1 = v; if (v > hi1) hi1 = v;
                            }
                            if (hi0 > -1e29f)
                                fprintf(stderr, "[UV-RANGE] %s dxgi=%u over %zu verts: "
                                                "u %.3f..%.3f  v %.3f..%.3f%s\n",
                                        e.semantic, e.fmt, nv < 4096 ? nv : (size_t)4096,
                                        lo0, hi0, lo1, hi1,
                                        (hi0 - lo0 > 64.f || hi1 - lo1 > 64.f)
                                            ? "  ⚠️ HUGE — one texel stretched over the surface"
                                            : "");
                        }
                        if (tell)
                            fprintf(stderr, "[gfx]   elem %s%u: slot=%u offset=%u dxgi=%u -> %s\n",
                                    e.semantic, 0u, e.slot, e.offset, e.fmt,
                                    mf == MTLVertexFormatInvalid ? "INVALID" : "ok");
                        uint32_t sl = e.slot < kMaxVBStreams ? e.slot : 0;
                        vd.attributes[a->second].bufferIndex = kVertexStreamIndex + sl;
                        used_slot[sl] = true;
                        mapped++;
                    }
                    if (tell)
                        for (auto& kv : vsit->second.attrs)
                            fprintf(stderr, "[gfx]   vs wants %s = attribute(%u)\n",
                                    kv.first.c_str(), kv.second);
                    if (!mapped) {
                        // Name mismatch is the only plausible cause and it is
                        // invisible from the picture, so print both sides once.
                        if (g_pipe_failed < 3) {
                            fprintf(stderr, "[gfx] pipeline: NO attribute matched. "
                                            "layout semantics:");
                            for (const GfxDrawElem& e : r.elems)
                                fprintf(stderr, " %s(fmt=%u,off=%u)", e.semantic, e.fmt, e.offset);
                            fprintf(stderr, " | vs attrs:");
                            for (auto& kv : vsit->second.attrs)
                                fprintf(stderr, " %s=%u", kv.first.c_str(), kv.second);
                            fprintf(stderr, "\n");
                        }
                        g_pipe_failed++; g_pipe_cache[key] = nil; return;
                    }
                    vd.layouts[kVertexStreamIndex].stride = r.vb_stride;
                    for (uint32_t sl = 1; sl < kMaxVBStreams; sl++)
                        if (used_slot[sl]) {
                            uint32_t st = r.vb2_stride[sl - 1];
                            // A stream an attribute reads from but the guest never
                            // gave a stride is a bug we must not paper over with 0 —
                            // Metal treats stride 0 as "every vertex reads element 0"
                            // and the geometry silently collapses.
                            if (!st) { g_vbslot_missing++; st = 4; }
                            vd.layouts[kVertexStreamIndex + sl].stride = st;
                        }
                    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
                    pd.vertexFunction   = vsit->second.fn;
                    // BO2_UI_FLAT=1 — DIAGNOSTIC. Replace every pixel shader with a
                    // constant colour. It separates the two halves of "nothing is
                    // visible": if flat quads appear, the vertex transform, the
                    // vertex format, the pass and the target are all correct and the
                    // fault is in shading (textures / pixel constant buffers); if
                    // they do not, the geometry never lands on screen.
                    static const int flat = g_flat_mode;
                    pd.fragmentFunction = (flat && g_flat_fs) ? g_flat_fs : psit->second.fn;
                    (void)0;
                    pd.vertexDescriptor = vd;
                    // The pipeline's attachment format MUST match the pass it is used
                    // in — the engine's targets are RGBA8 and R32_FLOAT, not the
                    // drawable's BGRA8.
                    pd.colorAttachments[0].pixelFormat                 = target_fmt;
                    // BO2_UI_NOBLEND=1 turns blending off entirely. The engine's real
                    // blend state is not translated yet, and straight alpha for every
                    // draw could be discarding everything; this isolates that from
                    // "the shader writes nothing".
                    // The guest's OWN blend state, including its WRITE MASK.
                    // BO2_UI_NOBLEND=1 forces blending off (kept: it isolates
                    // "blending discards it" from "the shader writes nothing").
                    static const int noblend = bo2_getenv("BO2_UI_NOBLEND") ? 1 : 0;
                    const GfxBlend& bl = r.blend;
                    // The flat diagnostic must NOT change blending or the write
                    // mask: doing so made every draw opaque, so the last one always
                    // won and the probe always named it — the instrument was
                    // answering its own question. Keep the guest's state exactly.
                    if (target_fmt != MTLPixelFormatInvalid) {   // BO2_DEPTH_DSV: a pass without a colour attachment has no blend state
                    pd.colorAttachments[0].blendingEnabled =
                        (noblend || !bl.enable) ? NO : YES;
                    pd.colorAttachments[0].sourceRGBBlendFactor        = d3d_blend_factor(bl.src_rgb);
                    pd.colorAttachments[0].destinationRGBBlendFactor   = d3d_blend_factor(bl.dst_rgb);
                    pd.colorAttachments[0].rgbBlendOperation           = d3d_blend_op(bl.op_rgb);
                    pd.colorAttachments[0].sourceAlphaBlendFactor      = d3d_blend_factor(bl.src_a);
                    pd.colorAttachments[0].destinationAlphaBlendFactor = d3d_blend_factor(bl.dst_a);
                    pd.colorAttachments[0].alphaBlendOperation         = d3d_blend_op(bl.op_a);
                    pd.colorAttachments[0].writeMask = d3d_write_mask(bl.write_mask);
                    }
                    // MUST match the pass. Metal validates this: a pipeline with
                    // MTLPixelFormatInvalid here cannot be used in a pass that has
                    // a depth attachment, and vice versa.
                    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                    pd.stencilAttachmentPixelFormat = st_fmt;   // Invalid (the descriptor's default) outside a stencil pass
                    NSError* pe = nil;
                    const double tp0 = CACurrentMediaTime();
                    pipe = [g_device newRenderPipelineStateWithDescriptor:pd error:&pe];
                    g_win_pipes++; g_win_pipe_ms += (CACurrentMediaTime() - tp0) * 1000.0;
                    g_pipe_cache[key] = pipe;
                    if (pipe) {
                        if (++g_pipe_built <= 5 || g_pipe_built % 50 == 0)
                            fprintf(stderr, "[gfx] pipeline #%llu built (vs=%08x ps=%08x, "
                                            "%d attrs, stride %u)\n",
                                    (unsigned long long)g_pipe_built, r.vs, r.ps,
                                    mapped, r.vb_stride);
                    } else if (++g_pipe_failed <= 5) {
                        fprintf(stderr, "[gfx] pipeline FAILED vs=%08x ps=%08x: %s\n",
                                r.vs, r.ps,
                                pe ? [[pe localizedDescription] UTF8String] : "?");
                    }
                }
                if (!pipe) { (g_real_skipped++, g_skip_why[3]++); return; }
                // BO2_UI_PSSKIP=<hex ps> — DIAGNOSTIC: drop every draw using that
                // pixel shader, to see what it is covering up.
                static const uint32_t psskip = [] {
                    const char* e2 = bo2_getenv("BO2_UI_PSSKIP");
                    return e2 ? (uint32_t)strtoul(e2, nullptr, 16) : 0u; }();
                if (psskip && r.ps == psskip) { (g_real_skipped++, g_skip_why[4]++); return; }
                // BO2_UI_HIDEWHITE=1 — WORKAROUND for the missing image assets,
                // not a fix. With no images loaded, the frontend's full-screen
                // BACKGROUND ART samples the engine's 1x1 $white placeholder and
                // multiplies it by a white vertex colour, producing an opaque white
                // sheet that buries the menu drawn on top of it. Skip exactly that
                // combination — a quad whose texture is a 1x1 WHITE and whose
                // COLOR0 is also pure white — so genuinely coloured UI rectangles
                // (same shader, non-white COLOR0) still draw.
                static const int hidewhite = bo2_getenv("BO2_UI_HIDEWHITE") ? 1 : 0;
                if (hidewhite && r.to_screen && r.idx_count <= 12) {
                    bool tex_white = false;
                    for (int q = 0; q < kMaxPSTex && !tex_white; q++) {
                        auto ti = g_texinfo.find(r.pstex[q]);
                        if (ti != g_texinfo.end() && ti->second.w <= 2 && ti->second.h <= 2 &&
                            ti->second.px[0] == 255 && ti->second.px[1] == 255 &&
                            ti->second.px[2] == 255 && ti->second.px[3] == 255)
                            tex_white = true;
                    }
                    if (tex_white) {
                        // Judge it by COVERAGE, not by colour: the frontend's UI
                        // positions are already in viewport pixels (verified: a UI
                        // quad's corners are (0,0)..(1920,1080)), so a quad whose
                        // bounding box spans most of the screen IS the background
                        // sheet, whatever its exact vertex colour. Small coloured
                        // rectangles use the same shader and the same 1x1 white, and
                        // must keep drawing.
                        uint32_t bvo = (r.base_vtx > 0 ? (uint32_t)r.base_vtx : 0) * r.vb_stride;
                        uint32_t nv = r.vb_stride ? (uint32_t)(r.vb.size() / r.vb_stride) : 0;
                        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                        uint32_t seen = 0;
                        for (uint32_t q = (bvo / (r.vb_stride ? r.vb_stride : 1));
                             q < nv && seen < 8; q++, seen++) {
                            const float* pv = (const float*)(r.vb.data() + q * r.vb_stride);
                            if (pv[0] < x0) x0 = pv[0];
                            if (pv[0] > x1) x1 = pv[0];
                            if (pv[1] < y0) y0 = pv[1];
                            if (pv[1] > y1) y1 = pv[1];
                        }
                        float vw = r.vp[2] > 1.f ? r.vp[2] : 1920.f;
                        float vh = r.vp[3] > 1.f ? r.vp[3] : 1080.f;
                        if (seen >= 3 && (x1 - x0) > 0.7f * vw && (y1 - y0) > 0.7f * vh) {
                            static uint64_t nhw = 0;
                            if (++nhw == 1 || (nhw % 4000) == 0)
                                fprintf(stderr, "[gfx] BO2_UI_HIDEWHITE: skipped %llu "
                                                "full-screen fills on a 1x1 white "
                                                "placeholder (missing art)\n",
                                        (unsigned long long)nhw);
                            (g_real_skipped++, g_skip_why[5]++); return;
                        }
                    }
                }
                // BO2_UI_MAXSCREEN=<n> — DIAGNOSTIC: stop after the nth screen
                // draw of the frame, to find which one covers the window.
                // BO2_UI_FLATFROM=<n> — flat-shade only from that draw on.
                static const uint32_t maxscreen_env = [] {
                    const char* e2 = bo2_getenv("BO2_UI_MAXSCREEN");
                    return e2 ? (uint32_t)strtoul(e2, nullptr, 10) : 0u; }();
                // BO2_UI_SWEEP=1 walks the cutoff up one draw at a time and the
                // pixel probe records the drawable after each step, so ONE run
                // produces the whole "which draw changes the visible pixel" curve
                // instead of one 2.5-minute run per guess.
                uint32_t maxscreen = g_sweep_n ? g_sweep_n : maxscreen_env;
                if (maxscreen && r.to_screen && g_flat_seq > maxscreen) {
                    (g_real_skipped++, g_skip_why[6]++); return;
                }
                // BO2_PS_SKIP=<hex>[,<hex>...] — drop every draw using these pixel
                // shaders. "Which shader painted that?" is the question this whole
                // washout has turned into, and the honest answer is a picture with
                // the suspect removed, not an argument about blend state. Shader
                // object ids are stable across runs here (the shim arena allocates
                // in the same order), so a hex id read out of one run can be fed
                // straight back into the next.
                static const std::vector<uint32_t> ps_skip = [] {
                    std::vector<uint32_t> v;
                    const char* e2 = bo2_getenv("BO2_PS_SKIP");
                    if (e2) for (const char* q = e2; *q; ) {
                        v.push_back((uint32_t)strtoul(q, nullptr, 16));
                        const char* c = strchr(q, ','); if (!c) break; q = c + 1;
                    }
                    if (!v.empty()) {
                        fprintf(stderr, "[PS-SKIP] dropping draws for %zu pixel shader(s):",
                                v.size());
                        for (uint32_t x : v) fprintf(stderr, " %08x", x);
                        fprintf(stderr, "\n");
                    }
                    return v; }();
                if (!ps_skip.empty()) {
                    for (uint32_t x : ps_skip)
                        if (x == r.ps) { (g_real_skipped++, g_skip_why[7]++); return; }
                }
                // BO2_UI_DROPLAST=<n> — skip the last n screen draws of the frame.
                // Counting from the END is drift-proof: the absolute index of a
                // given draw moves between frames, its distance from the tail
                // does not.
                static const uint32_t droplast = [] {
                    const char* e2 = bo2_getenv("BO2_UI_DROPLAST");
                    return e2 ? (uint32_t)strtoul(e2, nullptr, 10) : 0u; }();
                // Name the draw sitting exactly at the sweep cutoff: that is the
                // one whose addition changes the picture.
                if (g_sweep_n && r.to_screen && g_flat_seq == g_sweep_n) {
                    static uint32_t named = 0xffffffffu;
                    if (named != g_sweep_n) {
                        named = g_sweep_n;
                        fprintf(stderr, "[CUTOFF] draw #%u ps=%08x vs=%08x idx=%u "
                                        "mask=%x bl=%u(%u,%u) stride=%u",
                                g_flat_seq, r.ps, r.vs, r.idx_count,
                                r.blend.write_mask, r.blend.enable,
                                r.blend.src_rgb, r.blend.dst_rgb, r.vb_stride);
                        // An UNBOUND Metal buffer is undefined to read: a pixel
                        // shader that divides by a constant it never received
                        // produces inf/NaN, and clamp(NaN) lands on 1.0 — a white
                        // fill that looks nothing like a missing texture.
                        fprintf(stderr, " vscb=");
                        for (int q = 0; q < kMaxCB; q++)
                            fprintf(stderr, "%zu,", r.vscb[q].size());
                        fprintf(stderr, " pscb=");
                        for (int q = 0; q < kMaxCB; q++)
                            fprintf(stderr, "%zu,", r.pscb[q].size());
                        const ShaderRec* pr2 = nullptr;
                        { auto it2 = g_shader_rec.find(r.ps);
                          if (it2 != g_shader_rec.end()) pr2 = &it2->second; }
                        if (pr2) {
                            fprintf(stderr, " ps cbmap:");
                            for (auto& m : pr2->cbslot)
                                fprintf(stderr, " buf%u<-b%u", m.first, m.second);
                        }
                        fprintf(stderr, "\n");
                    }
                }
                // 🎯 BO2_SKIP_PS=<hex> — SKIP A DRAW BY ITS PIXEL SHADER, wherever it
                // falls in the frame. BO2_UI_DROPLAST drops "the last N screen
                // draws", which is only the composite in an IDLE frame; a busy
                // afghanistan frame has 411 screen draws and the last one is a UI
                // element. Every DROPLAST conclusion about "removing the
                // composite" was therefore about some other draw.
                //
                // This is the test that actually asks the question: remove
                // 466b1fb8 (the 6-index fullscreen quad sampling three render
                // targets) and see whether a correctly rendered world is
                // underneath it.
                {
                    static const uint32_t skip_ps = [] {
                        const char* e = bo2_getenv("BO2_SKIP_PS");
                        return e ? (uint32_t)strtoul(e, nullptr, 16) : 0u;
                    }();
                    if (skip_ps && r.ps == skip_ps) {
                        static uint64_t nskip = 0;
                        if (++nskip <= 3 || (nskip % 2000) == 0)
                            fprintf(stderr, "[SKIP-PS] skipped draw ps=%08x idx=%u "
                                            "to_screen=%d (skip #%llu)\n",
                                    r.ps, r.idx_count, (int)r.to_screen,
                                    (unsigned long long)nskip);
                        (g_real_skipped++, g_skip_why[8]++);
                        return;
                    }
                }
                // BO2_TRACE_MSL=<hachage MSL> : les 12 premiers tracés de ce nuanceur, cible et fenêtre (10-21).
                {
                    static const uint64_t th = [] { const char* e = bo2_getenv("BO2_TRACE_MSL");
                                                    return e ? strtoull(e, nullptr, 16) : 0ull; }();
                    static int nt = 0;
                    if (th && ps_rec && ps_rec->msl_h == th && nt < 12) {
                        ++nt;
                        fprintf(stderr, "[TRACE-MSL] rtv=%08x ecran=%d vp=%.0f,%.0f,%.0f,%.0f vpwh=%.0fx%.0f tex0=%08x "
                                        "tex1=%08x idx=%u bb=%08x\n", r.rtv, (int)r.to_screen, r.vp[0], r.vp[1], r.vp[2],
                                r.vp[3], r.vpw, r.vph, r.pstex[0], r.pstex[1], r.idx_count, g_backbuffer_rtv_id);
                    }
                }
                {
                    static const bool photo_tr = bo2_env_on("BO2_PHOTO_TRACES", false);
                    if (photo_tr && ps_rec && g_photo_rec) photo_trace_note(r, ps_rec->msl_h);
                }
                // BO2_SKIP_MSL_FILE=<chemin> : le fichier contient un hachage MSL (16 hex) ; ses tracés sont sautés.
                // Relu toutes les 300 ms : on compare avec/sans dans le même run (vitre du viseur, 10-21).
                {
                    static const char* f = bo2_getenv("BO2_SKIP_MSL_FILE");
                    static uint64_t h = 0;
                    static auto prochain = std::chrono::steady_clock::now();
                    if (f && ps_rec) {
                        auto now = std::chrono::steady_clock::now();
                        if (now >= prochain) {
                            prochain = now + std::chrono::milliseconds(300);
                            uint64_t v = 0;
                            if (FILE* fp = fopen(f, "r")) { if (fscanf(fp, "%llx", (unsigned long long*)&v) != 1) v = 0; fclose(fp); }
                            if (v != h) fprintf(stderr, "[SKIP-MSL] %016llx\n", (unsigned long long)v);
                            h = v;
                        }
                        if (h && ps_rec->msl_h == h) { (g_real_skipped++, g_skip_why[8]++); return; }
                    }
                }
                if (droplast && r.to_screen && g_screen_total >= droplast &&
                    g_flat_seq > g_screen_total - droplast) {
                    (g_real_skipped++, g_skip_why[9]++); return;
                }
                // The flat shader's colour. BO2_UI_FLAT=1 paints everything
                // magenta; BO2_UI_FLAT=2 ("rainbow") gives every screen draw its
                // own colour so the picture itself says WHICH draw owns each
                // region — the legend below maps colour back to pixel shader.
                static const uint32_t flatfrom = [] {
                    const char* e2 = bo2_getenv("BO2_UI_FLATFROM");
                    return e2 ? (uint32_t)strtoul(e2, nullptr, 10) : 0u; }();
                if (g_flat_mode && g_flat_fs) {
                    simd_float4 c = simd_make_float4(1.0f, 0.0f, 1.0f, 1.0f);
                    if (g_flat_mode >= 2) {
                        uint32_t i = g_flat_seq;
                        c = simd_make_float4(((i * 73) % 251) / 250.0f,
                                             ((i * 151) % 241) / 240.0f,
                                             ((i * 199) % 233) / 232.0f, 1.0f);
                    }
                    [enc2 setFragmentBytes:&c length:sizeof(c) atIndex:15];
                }
                // 🎯 WRITE-MASK CENSUS ON SCREEN DRAWS — check 1 of the narrowed
                // question. A pass can write depth and no colour if its colour
                // write mask is 0; that is indistinguishable on screen from a
                // shader outputting black, and only this tells them apart.
                if (r.to_screen) {
                    static uint64_t mask_hist[17] = {0};
                    static uint64_t nrep = 0;
                    mask_hist[r.blend.write_mask & 0xf]++;
                    if ((++nrep % 20000) == 0) {
                        fprintf(stderr, "[WMASK] screen draws by colour write mask:");
                        for (int q = 0; q < 16; q++)
                            if (mask_hist[q])
                                fprintf(stderr, " mask%X=%llu", q,
                                        (unsigned long long)mask_hist[q]);
                        fprintf(stderr, "  (mask0 = writes NO colour at all)\n");
                    }
                }
                if (r.topo != 4 && r.topo != 5) { (g_real_skipped++, g_skip_why[10]++); return; }

                id<MTLBuffer> vbuf = nil; NSInteger vboff = -1;   // C50 : le tampon resident, sinon le pool
                if (!c50_lier(r.res[0], r.res_bytes[0], r.vb.data(), &vbuf, &vboff)) {
                    vboff = upload(g_device, r.vb.data(), r.vb.size());
                    vbuf = g_upl_pool;
                }
                if (vboff < 0) { (g_real_skipped++, g_skip_why[11]++); return; }
                c49_codeur();
                {   static uint64_t n49 = 0;
                    if ((++n49 & 0xfffff) == 1)
                        fprintf(stderr, "[C49] %s : pipelines %llu/%llu, sommets %llu/%llu/%llu, fragments "
                                        "%llu/%llu/%llu, textures %llu/%llu, echantillonneurs %llu/%llu "
                                        "(poses/[decales/]evites, cumul)\n",
                                g_c49 ? "ARME" : "ETEINT",
                                (unsigned long long)g_c49_n[0], (unsigned long long)g_c49_n[1],
                                (unsigned long long)g_c49_n[2], (unsigned long long)g_c49_n[3],
                                (unsigned long long)g_c49_n[4], (unsigned long long)g_c49_n[5],
                                (unsigned long long)g_c49_n[6], (unsigned long long)g_c49_n[7],
                                (unsigned long long)g_c49_n[8], (unsigned long long)g_c49_n[9],
                                (unsigned long long)g_c49_n[10], (unsigned long long)g_c49_n[11]);
                    if ((n49 & 0xfffff) == 1)
                        fprintf(stderr, "[C50] flux lies residents %llu, tampons crees %llu (%.1f Mo), evinces %llu, "
                                    "tenus %zu (cumul)\n",
                            (unsigned long long)g_c50_n[0], (unsigned long long)g_c50_n[1],
                            (double)g_c50_n[2] / (1024.0 * 1024.0), (unsigned long long)g_c50_n[3],
                            g_c50_res.size()); }
                if (!g_c49 || g_c49e.pipe != pipe) {
                    [enc2 setRenderPipelineState:pipe]; g_c49e.pipe = pipe; ++g_c49_n[0];
                } else ++g_c49_n[1];
                c49_tampon_de(enc2, false, vbuf, vboff, kVertexStreamIndex);
                // ...and the extra streams, each at its own Metal buffer index.
                for (uint32_t sl = 1; sl < kMaxVBStreams; sl++) {
                    if (r.vb2[sl - 1].empty()) continue;
                    id<MTLBuffer> b2 = nil; NSInteger o2 = -1;   // C50
                    if (!c50_lier(r.res[sl], r.res_bytes[sl], r.vb2[sl - 1].data(), &b2, &o2)) {
                        o2 = upload(g_device, r.vb2[sl - 1].data(), r.vb2[sl - 1].size());
                        b2 = g_upl_pool;
                    }
                    if (o2 >= 0) c49_tampon_de(enc2, false, b2, o2, kVertexStreamIndex + sl);
                }
                // Metal buffer index i holds whichever D3D b-register the shader
                // declared there — NOT register i. HLSLcc hands out 0,1,2,... in
                // first-use order, so a shader reading b0 and b2 (the frontend's
                // font vertex shader, whose world matrix is in b2) declares them as
                // buffer(0) and buffer(1). Uploading b1 into buffer(1) fed it the
                // wrong matrix and its glyphs went nowhere.
                const ShaderRec* vs_rec = nullptr;
                { auto it = g_shader_rec.find(r.vs);
                  if (it != g_shader_rec.end()) vs_rec = c49_plat(&it->second); }
                for (int i = 0; i < kMaxCB; i++) {
                    uint32_t vreg = (uint32_t)i, freg = (uint32_t)i;
                    if (vs_rec && vs_rec->plat) vreg = vs_rec->cb_reg[i];   // C49
                    else if (vs_rec) { auto m = vs_rec->cbslot.find((uint32_t)i);
                                  if (m != vs_rec->cbslot.end()) vreg = m->second; }
                    if (ps_rec && ps_rec->plat) freg = ps_rec->cb_reg[i];
                    else if (ps_rec) { auto m = ps_rec->cbslot.find((uint32_t)i);
                                  if (m != ps_rec->cbslot.end()) freg = m->second; }
                    static UplMemo memo_v[kMaxCB], memo_f[kMaxCB];   // C47, par indice Metal
                    if (vreg < kMaxCB && !r.vscb[vreg].empty()) {
                        NSInteger o = upload_memo(memo_v[i], g_device, r.vscb[vreg].data(),
                                                  r.vscb[vreg].size());
                        if (o >= 0) c49_tampon(enc2, false, o, (NSUInteger)i);
                    } else if (vs_rec && vs_rec->cbslot.count((uint32_t)i)) g_cb_vides++;
                    if (freg < kMaxCB && !r.pscb[freg].empty()) {
                        NSInteger o = upload_memo(memo_f[i], g_device, r.pscb[freg].data(),
                                                  r.pscb[freg].size());
                        if (o >= 0) c49_tampon(enc2, true, o, (NSUInteger)i);
                    } else if (ps_rec && ps_rec->cbslot.count((uint32_t)i)) g_cb_vides++;
                }
                for (int i = 0; i < kMaxPSTex; i++) {
                    // Metal index i samples whichever D3D slot the shader declared.
                    uint32_t d3d_slot = (uint32_t)i;
                    bool declared = false;
                    if (ps_rec && ps_rec->plat) { d3d_slot = ps_rec->tx_slot[i]; declared = ps_rec->tx_decl[i]; }
                    else if (ps_rec) {
                        auto m = ps_rec->texslot.find((uint32_t)i);
                        if (m != ps_rec->texslot.end()) { d3d_slot = m->second; declared = true; }
                    }
                    uint32_t res = d3d_slot < kMaxPSTex ? r.pstex[d3d_slot] : 0u;
                    auto t = g_texreg.find(res);
                    if (t == g_texreg.end() && res && g_texrel_gone.count(res))
                        g_texrel_tard++;   // temoin : zero modelise trop tot

                    // A slot the shader DECLARES but the guest left unbound must
                    // read as transparent black; only an undeclared slot gets the
                    // white stand-in (nothing samples it anyway).
                    id<MTLTexture> fallback = declared ? g_nulltex : g_white;
                    if (!fallback) fallback = g_white;
                    // ...AND IT MUST BE THE RIGHT TEXTURE TYPE. Every texture this
                    // backend owns is 2D, so a `texture3d` / `texturecube` /
                    // `depth2d` slot was being handed a 2D texture — undefined in
                    // Metal, and `sample_compare` against a non-depth texture more
                    // so. The menu room's dominant material shader declares all
                    // three, which makes its lighting and shadow terms whatever the
                    // driver returns rather than anything the engine asked for.
                    uint32_t want_dim = 2;   // TD_2D
                    if (ps_rec && ps_rec->plat) want_dim = ps_rec->tx_dim[i];   // C49
                    else if (ps_rec) {
                        auto dm = ps_rec->texdim.find((uint32_t)i);
                        if (dm != ps_rec->texdim.end()) want_dim = dm->second;
                    }
                    id<MTLTexture> bound = (t != g_texreg.end()) ? t->second : fallback;
                    // The SRV's mip range (MostDetailedMip, MipLevels): a view of those levels, as D3D11 reads them.
                    if (const uint32_t pm = d3d_slot < kMaxPSTex ? r.psmip[d3d_slot] : 0u;
                        pm && g_mip_cibles && t != g_texreg.end() && bound.textureType == MTLTextureType2D) {
                        const NSUInteger nlv = bound.mipmapLevelCount, m0 = pm >> 8;
                        NSUInteger n = (pm & 0xFF) ? (pm & 0xFF) : nlv - (m0 < nlv ? m0 : nlv);
                        if (m0 + n > nlv) n = m0 < nlv ? nlv - m0 : 0;
                        static std::unordered_map<uint64_t, id<MTLTexture>> vues;
                        static uint64_t nvues = 0, nhors = 0;
                        if (n == 0) {
                            if (++nhors <= 4)
                                fprintf(stderr, "[SRV-MIP] %08x : niveaux %lu+%u hors de ses %lu niveaux, niveau 0 lu "
                                                "(#%llu)\n", res, (unsigned long)m0, pm & 0xFF, (unsigned long)nlv,
                                        (unsigned long long)nhors);
                        } else if (m0 != 0 || n != nlv) {
                            __strong id<MTLTexture>& v = vues[(uint64_t)res << 32 | pm];
                            if (!v || v.parentTexture != bound) {
                                v = [bound newTextureViewWithPixelFormat:bound.pixelFormat
                                                            textureType:MTLTextureType2D
                                                                 levels:NSMakeRange(m0, n)
                                                                 slices:NSMakeRange(0, 1)];
                                if (++nvues <= 8)
                                    fprintf(stderr, "[SRV-MIP] vue %08x niveaux %lu+%lu de %lu (#%llu)\n", res,
                                            (unsigned long)m0, (unsigned long)n, (unsigned long)nlv,
                                            (unsigned long long)nvues);
                            }
                            if (v) bound = v;
                        }
                    }
                    // P856 : qui lit quels niveaux d'une cible à plusieurs niveaux (une ligne par (ps, taille, plage)).
                    // P907 : clé par TAILLE, pas par ressource : la vidéo crée une cible 1920x720 neuve par image et
                    // remplissait seule les 64 lignes.
                    // P908 : vraie cible = a reçu une RTV ; l'usage Metal RenderTarget est posé aussi sur des textures chargées (police).
                    if (t != g_texreg.end() && t->second.mipmapLevelCount > 1 && g_rtv_cibles.count(res)) {
                        static std::set<std::tuple<uint64_t, uint32_t, uint32_t>> vus;
                        const uint32_t pm = d3d_slot < kMaxPSTex ? r.psmip[d3d_slot] : 0u;
                        const uint32_t taille = (uint32_t)t->second.width << 16 | (uint32_t)t->second.height;
                        if (vus.size() < 64 && vus.insert({(uint64_t)r.ps, taille, pm}).second)
                            fprintf(stderr, "[MIP-LECTEUR] ps=%016llx lit %08x (%lux%lu, %lu niv) indice %d srv=%u+%u "
                                            "-> vue %lu+%lu, cible du dessin %ux%u\n",
                                    (unsigned long long)r.ps, res, (unsigned long)t->second.width,
                                    (unsigned long)t->second.height, (unsigned long)t->second.mipmapLevelCount, i,
                                    pm >> 8, pm & 0xFF, (unsigned long)(bound.parentTexture ? bound.parentRelativeLevel : 0),
                                    (unsigned long)bound.mipmapLevelCount, g_coul_attache ? (unsigned)g_coul_attache.width >> g_coul_niveau : 0u,
                                    g_coul_attache ? (unsigned)g_coul_attache.height >> g_coul_niveau : 0u);
                    }
                    if (res && declared && !g_prof_dsv.empty()) {   // BO2_DEPTH_DSV (table empty with the lever off)
                        auto pz = g_prof_dsv.find(res);
                        if (pz != g_prof_dsv.end() && pz->second.tex) {
                            static const int lecture = [] {   // 0 off, 1 depth2d slots, 2 texture2d slots too
                                const char* e = bo2_getenv("BO2_DEPTH_DSV_SRV"); const int v = e ? atoi(e) : 1;
                                fprintf(stderr, "[DEPTH-DSV] depth2d servi par la vraie profondeur : %s (BO2_DEPTH_DSV_SRV)\n",
                                        v == 2 ? "ARME, texture2d aussi" : v == 1 ? "ARME" : "ETEINT");
                                return v; }();
                            // Attached by the open pass (Metal forbids it), or the DSV of this draw (D3D11 unbinds that SRV).
                            const bool attachee = pz->second.tex == g_prof_attache || res == r.dsv_res;
                            if (!(ps_rec && ps_rec->tx_prof[i])) {   // read through a texture2d (Load, or Sample without comparison)
                                ++g_dd_n[kDdLuTex2d]; g_dd_tex2d_res = res; g_dd_tex2d_rtv = r.rtv;
                                if (lecture == 2 && !attachee) { bound = pz->second.tex; ++g_dd_lus_cible[r.rtv]; }
                            }
                            else if (attachee) ++g_dd_n[kDdLuAttache];
                            else if (lecture) {
                                bound = pz->second.tex; ++pz->second.n_lus; ++g_dd_lus_cible[r.rtv];
                                // The view model draws in its own depth range (the scene's is 0..0.984375): do ITS draws
                                // sample this plane, and with which shaders? One line per distinct pair.
                                if (r.vp[4] != 0.f || r.vp[5] != 0.984375f) {
                                    ++g_dd_n[kDdLuVm]; g_dd_vm_plage[0] = r.vp[4]; g_dd_vm_plage[1] = r.vp[5];
                                    static std::set<uint64_t> vus;
                                    if (vus.size() < 64 && vus.insert((uint64_t)r.vs << 32 | r.ps).second) {
                                        auto v = g_shader_rec.find(r.vs);
                                        fprintf(stderr, "[DEPTH-DSV] lecture hors plage de la scene : plage %g..%g vs=%016llx.msl ps=%016llx.msl cible %08x indice de texture %d\n",
                                                r.vp[4], r.vp[5], (unsigned long long)(v != g_shader_rec.end() ? v->second.msl_h : 0),
                                                (unsigned long long)ps_rec->msl_h, r.rtv, (int)i);
                                    }
                                }
                            }
                        }
                    }
                    if (want_dim != 2 && want_dim != 0 && want_dim != 1) {
                        MTLTextureType have = bound ? [bound textureType]
                                                    : MTLTextureType2D;
                        bool ok = (want_dim == 3 && have == MTLTextureType3D) ||
                                  (want_dim == 4 && have == MTLTextureTypeCube) ||
                                  (want_dim == 5 && have == MTLTextureType2D &&
                                   [bound pixelFormat] == MTLPixelFormatDepth32Float);
                        if (!ok) {
                            g_tx_wrongtype++;
                            if (want_dim == 3) g_tx_wt3d++;
                            else if (want_dim == 4) g_tx_wtcube++;
                            else if (want_dim == 5) {
                                g_tx_wtdepth++;
                                if (t != g_texreg.end()) g_tx_wtdepth_hadtex++;
                            }
                            bound = (want_dim == 3) ? g_null3d
                                  : (want_dim == 4) ? g_nullcube
                                  : (want_dim == 5) ? g_nulldepth : bound;
                            if (!bound) bound = fallback;
                        }
                    }
                    // Where does a white screen come from — no resource bound at
                    // that slot, or a resource we hold no pixels for? They need
                    // different fixes, so count them apart instead of inferring.
                    // Record what the FINAL screen draws sample, so their inputs
                    // can be read back exactly after the frame.
                    if (declared && res && r.to_screen && g_screen_total > 30 &&
                        g_flat_seq > g_screen_total - 30 && g_probe_want_n < 12) {
                        bool dup = false;
                        for (int q = 0; q < g_probe_want_n; q++)
                            if (g_probe_want[q] == res) dup = true;
                        if (!dup) { g_probe_want_ps[g_probe_want_n] = r.ps;
                                    g_probe_want[g_probe_want_n++] = res; }
                    }
                    if (declared) {
                        if (!res) g_tx_unbound++;
                        else if (t != g_texreg.end()) g_tx_have++;
                        else { g_tx_nopixels++; g_tx_missing_id[res]++; }
                        if (g_legend_frame)
                        {
                            auto ti = g_texinfo.find(res);
                            fprintf(stderr, "[LEGEND]      tex(%d) <- D3D t%u = %08x %s",
                                    i, d3d_slot, res,
                                    !res ? "NOTHING BOUND"
                                         : (t != g_texreg.end() ? "ok" : "NO PIXELS"));
                            if (ti != g_texinfo.end())
                                fprintf(stderr, " %ux%u fmt=%u texel=%u,%u,%u,%u",
                                        ti->second.w, ti->second.h, ti->second.fmt,
                                        ti->second.px[0], ti->second.px[1],
                                        ti->second.px[2], ti->second.px[3]);
                            else if (res)
                                fprintf(stderr, " (render target, no staged pixels)");
                            fprintf(stderr, "\n");
                        }
                    }
                    // P855 : D3D11 lit NULL une SRV dont la ressource est la cible du dessin (le runtime la délie). Metal
                    // lirait la texture en cours d'écriture, par tuiles de 32 px à moitié écrites : les carrés au sol.
                    if (g_coul_attache && (bound == g_coul_attache ||   // la vue d'un AUTRE niveau (chaîne de réduction) reste lue
                            (bound.parentTexture == g_coul_attache && bound.parentRelativeLevel <= g_coul_niveau &&
                             g_coul_niveau < bound.parentRelativeLevel + bound.mipmapLevelCount))) {
                        static const bool d3d = bo2_env_on("BO2_SRV_CIBLE_NULL", true);
                        static uint64_t n = 0;
                        if (++n <= 8 || n % 5000 == 0)
                            fprintf(stderr, "[SRV-CIBLE] ps=%016llx lit sa propre cible %08x (indice %d) : %s (#%llu, BO2_SRV_CIBLE_NULL)\n",
                                    (unsigned long long)r.ps, res, i, d3d ? "NULL comme D3D11" : "lue telle quelle", (unsigned long long)n);
                        if (d3d) bound = fallback;
                    }
                    if (!g_c49 || g_c49e.tex[i] != bound) {
                        [enc2 setFragmentTexture:bound atIndex:i]; g_c49e.tex[i] = bound; ++g_c49_n[8];
                    } else ++g_c49_n[9];
                    // P805 : the game's own sampler for the s register this Metal index was declared from.
                    id<MTLSamplerState> smp = g_samp;
                    {
                        uint32_t sreg = ~0u;
                        if (ps_rec && ps_rec->plat) sreg = ps_rec->sm_reg[i];
                        else if (ps_rec) {
                            auto m = ps_rec->smpslot.find((uint32_t)i);
                            if (m != ps_rec->smpslot.end()) sreg = m->second;
                        }
                        if (sreg < kMaxPSTex) {
                            auto sm = r.pssamp[sreg] ? g_sampreg.find(r.pssamp[sreg]) : g_sampreg.end();
                            if (sm != g_sampreg.end()) { smp = sm->second; g_smp_jeu++; }
                            else if (r.pssamp[sreg]) g_smp_inconnu++;
                            else g_smp_defaut++;
                            if (((g_smp_jeu + g_smp_defaut + g_smp_inconnu) & 0xFFFFF) == 0)
                                fprintf(stderr, "[SAMPLER] jeu=%llu sans_etat=%llu inconnu=%llu etats=%zu\n",
                                        (unsigned long long)g_smp_jeu, (unsigned long long)g_smp_defaut,
                                        (unsigned long long)g_smp_inconnu, g_sampreg.size());
                        }
                    }
                    if (!g_c49 || g_c49e.samp[i] != smp) {
                        [enc2 setFragmentSamplerState:smp atIndex:i]; g_c49e.samp[i] = smp; ++g_c49_n[10];
                    } else ++g_c49_n[11];
                }
                NSUInteger need = (r.ib.size() + 15) & ~(NSUInteger)15;
                if (g_idx_pool_off + need > g_idx_pool_cap) {
                    NSUInteger want = g_idx_pool_cap ? g_idx_pool_cap * 2 : (4u << 20);
                    while (want < g_idx_pool_off + need) want *= 2;
                    if (want > (64u << 20)) { (g_real_skipped++, g_skip_why[12]++); return; }
                    g_idx_pool = [g_device newBufferWithLength:want
                                                       options:MTLResourceStorageModeShared];
                    g_idx_pool_cap = g_idx_pool ? want : 0;
                    g_idx_pool_off = 0;
                    if (!g_idx_pool) { (g_real_skipped++, g_skip_why[13]++); return; }
                }
                memcpy((uint8_t*)[g_idx_pool contents] + g_idx_pool_off,
                       r.ib.data(), r.ib.size());
                // BaseVertexLocation was carried all the way here and then
                // DROPPED: the short drawIndexedPrimitives: has no baseVertex, so
                // every draw the guest issued with a non-zero base read its
                // vertices from the wrong place. The shim already sizes the vertex
                // window to include it.
                // 🔎 BO2_DRAW_MIN / BO2_DRAW_MAX bisection (see g_draw_seq).
                {
                    static const long dmin = bo2_getenv("BO2_DRAW_MIN")
                                           ? atol(bo2_getenv("BO2_DRAW_MIN")) : -1;
                    static const long dmax = bo2_getenv("BO2_DRAW_MAX")
                                           ? atol(bo2_getenv("BO2_DRAW_MAX")) : -1;
                    const long seq = g_draw_seq.fetch_add(1, std::memory_order_relaxed);
                    // ⚠️ DRAW_MAX ALONE IS USELESS FOR FINDING AN ARTEFACT: the
                    // final composite is one of the LAST draws, so any cap that
                    // trims the tail also removes the blit to screen and the
                    // window goes black (measured: BO2_DRAW_MAX=900 on angola ->
                    // a black frame, 1940 draws still submitted upstream).
                    // What is actually needed is to HIDE A MIDDLE RANGE and keep
                    // everything around it, which is what HIDE_MIN/HIDE_MAX do.
                    static const long hmin = bo2_getenv("BO2_DRAW_HIDE_MIN")
                                           ? atol(bo2_getenv("BO2_DRAW_HIDE_MIN")) : -1;
                    static const long hmax = bo2_getenv("BO2_DRAW_HIDE_MAX")
                                           ? atol(bo2_getenv("BO2_DRAW_HIDE_MAX")) : -1;
                    const bool hidden = (hmin >= 0 || hmax >= 0) &&
                                        (hmin < 0 || seq >= hmin) &&
                                        (hmax < 0 || seq <  hmax);
                    if (hidden || (dmin >= 0 && seq < dmin) ||
                                  (dmax >= 0 && seq >= dmax)) {
                        g_real_skipped++;
                        return;
                    }
                }
                [enc2 drawIndexedPrimitives:(r.topo == 5 ? MTLPrimitiveTypeTriangleStrip
                                                        : MTLPrimitiveTypeTriangle)
                                indexCount:r.idx_count
                                 indexType:(r.ib_fmt == 57 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32)
                               indexBuffer:g_idx_pool
                         indexBufferOffset:g_idx_pool_off
                             instanceCount:1
                                baseVertex:r.base_vtx
                              baseInstance:0];
                g_idx_pool_off += need;
                g_real_draws++;

}
}  // namespace



extern "C" void gfx_real_draw(const GfxDraw* d) {
    static bool layout_checked = false;
    if (__builtin_expect(!layout_checked, 0)) { layout_checked = true; gfx_check_record_layout(); }
    if (!g_open.load() || !d) return;
    // A clear and a copy carry no vertex or index data — checking for them first
    // matters, because this guard silently swallowed every CopyResource.
    if (!d->is_clear && !d->is_copy && (!d->vb || !d->ib)) return;
    // Un enregistrement neuf est value-initialise (zeros, tranches vides), comme avant :
    // seuls ses octets ont change de proprietaire (l'arene de g_rd, C35).
    if (d->is_copy) {
        std::lock_guard<std::mutex> lk(g_rd_mu);
        if (!rd_admit(0)) return;
        RealDrawRec& c = g_rd.recs.emplace_back();
        c.is_copy = 1; c.copy_dst = d->copy_dst; c.copy_src = d->copy_src;
        c.is_clear = 0; c.rtv = 0;
        return;
    }
    if (d->is_clear) {
        std::lock_guard<std::mutex> lk(g_rd_mu);
        if (!rd_admit(0)) return;
        RealDrawRec& c = g_rd.recs.emplace_back();
        c.is_clear = d->is_clear; c.is_copy = 0; c.rtv = d->rtv;
        c.depth.st_res = d->depth.st_res; c.depth.st_ref = d->depth.st_ref;   // is_clear == 2: which plane, which value
        c.dsv_res = d->dsv_res; c.dsv_wh = d->dsv_wh;                         // is_clear == 3: which depth plane (value in clear_rgba[0]), its size
        for (int i = 0; i < 4; i++) c.clear_rgba[i] = d->clear_rgba[i];
        return;
    }
    std::lock_guard<std::mutex> lk0(g_rd_mu);
    { uint64_t n = (uint64_t)(d->res[0] ? 0u : d->vb_bytes) + d->ib_bytes;   // C50
      for (int i = 0; i < kMaxCB; i++) n += d->vscb_bytes[i] + d->pscb_bytes[i];
      if (!rd_admit(n)) return; }
    RdArena& a = g_rd.arena;
    RealDrawRec& r = g_rd.recs.emplace_back();
    r.vs = d->vs; r.ps = d->ps;
    r.elems = a.copy(d->elems, d->nelems);
    // C50 : un flux statique n'est pas copie, sa tranche pointe dans la memoire invitee (octets figes).
    auto en_place = [](const void* p, uint32_t n) { RdSpan<uint8_t> t; t.p = (const uint8_t*)p; t.n = n; return t; };
    for (uint32_t sl = 0; sl < kMaxVBStreams; sl++) {
        r.res[sl] = (const uint8_t*)d->res[sl]; r.res_bytes[sl] = d->res_bytes[sl];
    }
    r.vb = d->res[0] ? en_place(d->vb, d->vb_bytes) : a.copy((const uint8_t*)d->vb, d->vb_bytes);
    r.ib = a.copy((const uint8_t*)d->ib, d->ib_bytes);
    r.vb_stride = d->vb_stride; r.ib_fmt = d->ib_fmt;
    for (uint32_t sl = 0; sl < kMaxVBStreams - 1; sl++) {
        r.vb2_stride[sl] = d->vb2_stride[sl];
        r.vb2[sl] = d->res[sl + 1] ? en_place(d->vb2[sl], d->vb2_bytes[sl])
                                   : a.copy((const uint8_t*)d->vb2[sl], d->vb2_bytes[sl]);   // C50
    }
    r.idx_count = d->idx_count; r.base_vtx = d->base_vtx; r.topo = d->topo;
    r.rtv = d->rtv; r.is_clear = 0; r.is_copy = 0; r.blend = d->blend;
    r.depth = d->depth; r.dsv = d->dsv; r.dsv_res = d->dsv_res; r.dsv_wh = d->dsv_wh;
    r.rs = d->rs; for (int i = 0; i < 4; i++) r.sc[i] = d->sc[i];
    r.zb = d->zb; r.zb_pente = d->zb_pente; r.zb_max = d->zb_max;
    // SIX : les deux derniers sont MinDepth/MaxDepth, et sans eux le
    // modele de vue est teste en profondeur contre le monde.
    for (int i = 0; i < 6; i++) r.vp[i] = d->vp[i];
    // C47 : un CB que le shim n'a pas relu depuis le dessin precedent (meme numero de remplissage) a
    // les memes octets : on reprend sa tranche, tant que l'arene est celle de la meme image (g_rd_epoch).
    static uint32_t fill_vu[2 * kMaxCB] = {0};
    static uint64_t epoque_vue[2 * kMaxCB] = {0};
    static RdSpan<uint8_t> tranche_vue[2 * kMaxCB];
    auto cb = [&](int k, const void* src, uint32_t n, uint32_t fill) {
        if (g_c47 && fill && n && fill == fill_vu[k] && epoque_vue[k] == g_rd_epoch
            && tranche_vue[k].n == n) {
            ++g_c47_repris; g_c47_evites += n;
            // Controle d'equivalence, 1 reprise sur 16 : la tranche reprise doit etre octet pour octet la
            // source que l'ancien chemin aurait copiee. Un ecart = defaut de C47 (temoin `ecarts=`).
            if ((++g_c47_verifs & 15) == 0 && std::memcmp(tranche_vue[k].p, src, n) != 0) ++g_c47_ecarts;
            return tranche_vue[k];
        }
        const RdSpan<uint8_t> t = a.copy((const uint8_t*)src, n);
        fill_vu[k] = fill; epoque_vue[k] = g_rd_epoch; tranche_vue[k] = t;
        return t;
    };
    for (int i = 0; i < kMaxCB; i++) {
        r.vscb[i] = cb(i, d->vscb[i], d->vscb_bytes[i], d->vscb_fill[i]);
        r.pscb[i] = cb(kMaxCB + i, d->pscb[i], d->pscb_bytes[i], d->pscb_fill[i]);
    }
    for (int i = 0; i < kMaxPSTex; i++) r.pstex[i] = d->pstex[i];
    for (int i = 0; i < kMaxPSTex; i++) r.pssamp[i] = d->pssamp[i];
    for (int i = 0; i < kMaxPSTex; i++) r.psmip[i] = d->psmip[i];
    r.vpw = d->vpw; r.vph = d->vph;
}

// Called at Present: the frame's draws are complete, publish them.
extern "C" void gfx_real_frame_end() {
    std::lock_guard<std::mutex> lk(g_rd_mu);
    static uint64_t nf = 0, last_dr = 0, last_db = 0;
    if ((g_rd_drop_recs != last_dr || g_rd_drop_bytes != last_db) && (++nf % 60) == 1) {
        fprintf(stderr, "[gfx] frame records: %zu kept, %llu dropped over the count "
                        "cap, %llu over the byte cap (%llu MB held)\n",
                g_rd.recs.size(), (unsigned long long)g_rd_drop_recs,
                (unsigned long long)g_rd_drop_bytes,
                (unsigned long long)(g_rd_bytes >> 20));
        last_dr = g_rd_drop_recs; last_db = g_rd_drop_bytes;
    }
    // ⚠️ NEVER PUBLISH AN EMPTY FRAME. The pump presents whatever the last
    // published record holds, so a Present that arrives with no draws recorded
    // — which happens whenever the guest is preempted mid-build, and the
    // cooperative scheduler makes that common — clears the drawable to black for
    // one refresh. That is the "brief full-screen black flashes" the user sees
    // while the video itself is perfectly smooth.
    // MEASURED FIRST, because there were two candidate explanations: a black
    // frame arriving from the game, or an empty frame arriving from us. A
    // black-upload counter at Unmap read `uploads=1600 black=0` — not one frame
    // we handed the GPU was black — so the content is innocent and the flash is
    // an empty draw list. Keeping the previous frame is what a real driver's
    // front buffer does anyway.
    // [C35] : le temoin de l'arene. Il parle a la 1re image puis toutes les 600, meme
    // quand rien ne bouge : sans lui, « pas de C35 » et « C35 muet » se lisent pareil.
    {
        static uint64_t nc35 = 0;
        if ((nc35++ % 600) == 0)
            fprintf(stderr, "[C35] arene par image : %zu dessins, %llu Mo copies ; tenu "
                            "construction %llu Mo, publiee %llu Mo ; morceaux alloues %llu (C44) ; "
                            "C47 %s : %llu CB repris, %llu Mo evites, %llu envois repris (cumul), "
                            "controles %llu ecarts=%llu\n",
                    g_rd.recs.size(), (unsigned long long)((g_rd_bytes - g_c47_evites) >> 20),
                    (unsigned long long)(g_rd.arena.held() >> 20),
                    (unsigned long long)(g_rd_frame.arena.held() >> 20),
                    (unsigned long long)RdArena::g_morceaux.load(std::memory_order_relaxed),
                    g_c47 ? "ARME" : "ETEINT", (unsigned long long)g_c47_repris,
                    (unsigned long long)(g_c47_evites >> 20),
                    (unsigned long long)g_c47_envois_repris.load(std::memory_order_relaxed),
                    (unsigned long long)(g_c47_verifs >> 4), (unsigned long long)g_c47_ecarts);
    }
    bool contenu = false;   // a stencil or depth clear (is_clear == 2, 3) draws nothing: alone, it is still an empty frame
    for (const RealDrawRec& r : g_rd.recs) if (r.is_clear < 2) { contenu = true; break; }
    if (contenu) {
        g_rd_frame.swap(g_rd);
        g_rd_frame_seq = ++g_rd_seq;
    } else {
        static uint64_t skipped = 0;
        if (++skipped == 1 || (skipped % 500) == 0)
            fprintf(stderr, "[gfx] %llu empty frames held back (previous frame kept "
                            "instead of flashing black)\n",
                    (unsigned long long)skipped);
    }
    // Depth is cleared once per SIZE per FRAME (see the attachment note), so the
    // record of what has been cleared has to die with the frame.
    // ⚠️ NOT FROM HERE. This is the GUEST thread; g_depth_cleared is inserted
    // into by the PUMP thread with no lock on either side, and clearing a
    // std::set under a concurrent insert corrupts the heap (see the note at the
    // clear that replaced this one, in gfx_pump_once). The pump clears it at the
    // top of every frame it encodes, which is the boundary the set actually
    // describes.
    // C35 : O(1), et l'arene garde ses morceaux. g_rd est ici soit l'image que la
    // pompe a rendue (deja videe), soit une image publiee qu'elle n'a jamais prise.
    g_rd.clear();
    g_rd_bytes = 0;
    ++g_rd_epoch; g_c47_repris = 0; g_c47_evites = 0;   // C47 : les tranches de l'image finie sont mortes
    g_guest_frames.fetch_add(1);   // the guest's OWN frame rate, not the pump's
}

// Guest thread: D3D DESTROYED this texture — its refcount, modelled on the shim
// side with the references the runtime itself holds, reached zero. Under Windows
// the video memory goes back; here the MTLTexture stayed in g_texreg for ever,
// one per decoded video frame (1920x720 A8 + pyramid = 1.84 MB): 5 000 frames
// in pz2_afghanistan, ~9 of the 12.8 GB Metal. It cannot go NOW: the frame being
// recorded, and every published one the pump may still (re-)encode, can sample
// it. It goes once the pump encodes a frame recorded ENTIRELY after this call,
// i.e. published number >= (published so far) + 2. Command buffers already
// committed keep their own reference (Metal retains what an encoder binds).
extern "C" void gfx_release_texture(uint32_t obj) {
    if (!g_open.load() || !obj) return;
    uint64_t des;
    { std::lock_guard<std::mutex> lk(g_rd_mu); des = g_rd_seq + 2; }
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    g_texrel_pending.emplace_back(obj, des);
}

// Guest thread: a texture the guest created as a render target. No pixels yet —
// the engine renders into it.
extern "C" void gfx_register_rendertarget(uint32_t obj, uint32_t w, uint32_t h,
                                         uint32_t fmt, uint32_t mips) {
    if (!g_open.load() || !obj) return;
    if (!g_mip_cibles) mips = 1;
    std::lock_guard<std::mutex> lk(g_rt_mu);
    if (g_rt_pending.size() > 1024) return;
    g_rt_pending.push_back({ obj, w, h, fmt, mips });
}

extern "C" void gfx_register_texslots(uint32_t obj, const char* map) {
    if (!g_open.load() || !obj || !map) return;
    std::lock_guard<std::mutex> lk(g_shader_mu);
    g_texslot_pending.emplace_back(obj, std::string(map));
}

extern "C" void gfx_register_cbslots(uint32_t obj, const char* map) {
    if (!g_open.load() || !obj || !map) return;
    std::lock_guard<std::mutex> lk(g_shader_mu);
    g_cbslot_pending.emplace_back(obj, std::string(map));
}

// "metalIndex=HLSLCC_TEX_DIMENSION,..." — which Metal texture TYPE each slot of
// this shader was declared with. Same pending-queue shape as the slot maps.
extern "C" void gfx_register_texdims(uint32_t obj, const char* map) {
    if (!g_open.load() || !obj || !map) return;
    std::lock_guard<std::mutex> lk(g_shader_mu);
    g_texdim_pending.emplace_back(obj, std::string(map));
}

extern "C" void gfx_register_smpslots(uint32_t obj, const char* map) {
    if (!g_open.load() || !obj || !map) return;
    std::lock_guard<std::mutex> lk(g_shader_mu);
    g_smpslot_pending.emplace_back(obj, std::string(map));
}

// P805 : the game's D3D11_SAMPLER_DESC (13 dwords) as a Metal sampler state. D3D11_TEXTURE_ADDRESS_MODE
// 1..5 -> Metal; D3D11_FILTER bits: 0x01 mip linear, 0x04 mag linear, 0x10 min linear, 0x40 anisotropic,
// 0x80 comparison. MipLODBias has no Metal sampler equivalent and is dropped.
static MTLSamplerAddressMode d3d_addr(uint32_t a) {
    switch (a) {
        case 1: return MTLSamplerAddressModeRepeat;
        case 2: return MTLSamplerAddressModeMirrorRepeat;
        case 4: return MTLSamplerAddressModeClampToBorderColor;
        case 5: return MTLSamplerAddressModeMirrorClampToEdge;
        default: return MTLSamplerAddressModeClampToEdge;          // 3 = CLAMP
    }
}
extern "C" void gfx_register_sampler(uint32_t obj, const uint32_t desc[13]) {
    if (!g_open.load() || !obj || !g_device) return;
    const uint32_t f = desc[0];
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = (f & 0x10) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.magFilter = (f & 0x04) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = (f & 0x01) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    // Anisotropy stays capped by BO2_ANISO (default 1 = off): see the green-speckle note on g_samp.
    static const long cap = [] { const char* e = bo2_getenv("BO2_ANISO"); long a = e ? strtol(e, nullptr, 10) : 1;
                                 return a < 1 ? 1L : (a > 16 ? 16L : a); }();
    if ((f & 0x40) && desc[5] > 1) sd.maxAnisotropy = (NSUInteger)std::min<long>((long)desc[5], cap);
    sd.sAddressMode = d3d_addr(desc[1]);
    sd.tAddressMode = d3d_addr(desc[2]);
    sd.rAddressMode = d3d_addr(desc[3]);
    if (f & 0x80) sd.compareFunction = d3d_cmp(desc[6]);
    float bc[4], lod[2];
    memcpy(bc, desc + 7, sizeof bc); memcpy(lod, desc + 11, sizeof lod);
    sd.borderColor = bc[3] == 0.0f ? MTLSamplerBorderColorTransparentBlack
                   : (bc[0] + bc[1] + bc[2] >= 1.5f ? MTLSamplerBorderColorOpaqueWhite
                                                    : MTLSamplerBorderColorOpaqueBlack);
    if (lod[0] > 0.0f && lod[0] <= lod[1]) sd.lodMinClamp = lod[0];
    if (lod[1] >= 0.0f && lod[1] >= lod[0]) sd.lodMaxClamp = lod[1];
    id<MTLSamplerState> st = [g_device newSamplerStateWithDescriptor:sd];
    if (!st) return;
    std::lock_guard<std::mutex> lk(g_shader_mu);
    g_sampler_pending.emplace_back(obj, st);
}

extern "C" void gfx_set_backbuffer_rtv(uint32_t rtv) { g_backbuffer_rtv_id = rtv; }
extern "C" void gfx_set_backbuffer_res(uint32_t res) { g_backbuffer_res_id = res; }

static uint64_t g_copie_ecran = 0;   // [COPIE-ECRAN] backbuffer -> texture copies (P806)

// One copy pipeline per destination format (P806): g_pipe is compiled for the BGRA8
// drawable, and drawing it into an RGBA8 target is a format mismatch Metal does not
// convert. nil for a format the float copy cannot write (depth, integer): the copy is skipped.
static id<MTLRenderPipelineState> copy_pipe_for(MTLPixelFormat pf) {
    if (pf == g_layer.pixelFormat) return g_pipe;
    static std::unordered_map<NSUInteger, id<MTLRenderPipelineState>> cache;
    auto it = cache.find(pf);
    if (it != cache.end()) return it->second;
    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = g_copy_vs;
    pd.fragmentFunction = g_copy_fs;
    pd.colorAttachments[0].pixelFormat = pf;
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [g_device newRenderPipelineStateWithDescriptor:pd error:&err];
    fprintf(stderr, "[COPIE-PIPE] format Metal %lu : %s\n", (unsigned long)pf,
            p ? "pipeline cree" : [[err localizedDescription] UTF8String]);
    cache[pf] = p;
    return p;
}

extern "C" void gfx_register_rtv(uint32_t rtv, uint32_t res, uint32_t mip) {
    if (!g_open.load() || !rtv) return;
    std::lock_guard<std::mutex> lk(g_rt_mu);
    if (g_rtv_pending.size() > 1024) return;
    g_rtv_pending.emplace_back(rtv, res);
    if (mip && g_mip_cibles) g_rtv_mip_pending.emplace_back(rtv, mip);
}

// Guest thread: hand over one finished frame of UI geometry (called at Present).
extern "C" void gfx_ui_frame(const void* verts, uint32_t nverts,
                             const void* batches, uint32_t nbatches,
                             float vpw, float vph) {
    if (!g_open.load() || !nverts || !nbatches) return;
    std::lock_guard<std::mutex> lk(g_ui_mu);
    g_ui_verts.assign((const UiVert*)verts, (const UiVert*)verts + nverts);
    g_ui_batches.assign((const UiBatch*)batches, (const UiBatch*)batches + nbatches);
    if (vpw > 0) g_ui_vw = vpw;
    if (vph > 0) g_ui_vh = vph;
    g_ui_ready = true;
    if (++g_ui_frames <= 5 || g_ui_frames % 500 == 0)
        fprintf(stderr, "[gfx] ui frame #%llu: %u verts in %u batches (%.0fx%.0f)\n",
                (unsigned long long)g_ui_frames, nverts, nbatches, g_ui_vw, g_ui_vh);
}

// Called from the GUEST thread when the stubbed CreateTexture2D receives real
// fastfile pixels. Copies only; no Metal calls here.
extern "C" void gfx_stage_texture(uint32_t w, uint32_t h, uint32_t fmt,
                                  const void* data, uint32_t bytes, uint32_t pitch) {
    if (!g_open.load() || !data || !bytes || !w || !h) return;
    if (dxgi_to_metal(fmt) == MTLPixelFormatInvalid)  return;
    uint64_t px = (uint64_t)w * h;
    std::lock_guard<std::mutex> lk(g_tex_mu);
    if (px <= g_best_pixels) return;               // keep the largest seen
    g_best_pixels = px;
    g_pending.bytes.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
    g_pending.w = w; g_pending.h = h; g_pending.fmt = fmt; g_pending.pitch = pitch;
    g_pending.ready = true;
    fprintf(stderr, "[gfx] staged texture %ux%u dxgi=%u pitch=%u (%u bytes)\n",
            w, h, fmt, pitch, bytes);
}

// Open the window. MUST be called from the real process main thread.
extern "C" bool gfx_window_open(int width, int height) {
    if (g_open.load()) return true;
    // BO2_SHOT wiring. SIGUSR1 is the on-demand trigger, so a long run can be
    // photographed at any moment from the outside without restarting it.
    g_shot_prefix = bo2_getenv("BO2_SHOT");
    g_rt_dump_prefix = bo2_getenv("BO2_RT_DUMP");
    if (const char* e = bo2_getenv("BO2_RT_SWEEP")) g_rtsweep_steps = (uint32_t)strtoul(e, 0, 10);
    if (const char* e = bo2_getenv("BO2_UI_WATCHRT")) g_watchrt_res = (uint32_t)strtoul(e, 0, 16);
    if (g_rtsweep_steps && !g_watchrt_res) {
        fprintf(stderr, "[RT-SWEEP] BO2_RT_SWEEP needs BO2_UI_WATCHRT=<resource> — off\n");
        g_rtsweep_steps = 0;
    }
    if (const char* e = bo2_getenv("BO2_SHOT_EVERY")) g_shot_every = strtoull(e, 0, 10);
    if (g_shot_prefix || g_shot_every || g_rt_dump_prefix) {
        struct sigaction sa {};
        sa.sa_handler = bo2_shot_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, nullptr);
        fprintf(stderr, "[SHOT] armed: prefix=%s every=%llu (kill -USR1 %d for one now)\n",
                g_shot_prefix ? g_shot_prefix : "/tmp/bo2_frame",
                (unsigned long long)g_shot_every, (int)getpid());
    }
    @autoreleasepool {
        g_device = MTLCreateSystemDefaultDevice();
        if (!g_device) {
            fprintf(stderr, "[gfx] no Metal device available\n");
            return false;
        }
        g_queue = [g_device newCommandQueue];
        msl_warm_start();

        // A plain executable is not a bundled .app; Regular activation policy is
        // what lets it own a real window and take focus anyway.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // A REAL APP, not a process called "boot". Without a main menu AppKit
        // labels the menu bar with the executable name and gives the window no
        // standard shortcuts at all — no Cmd+F, no Cmd+M, no Cmd+Q. Building one
        // is pure setup at startup: it runs once, before the guest starts, and
        // costs exactly nothing per frame.
        {
            // The menu-bar name comes from the PROCESS name, not from the menu's
            // title — with no .app bundle AppKit uses argv[0], which is why it
            // said "boot". setProcessName is the documented way to change it.
            NSString* appName = @"Black Ops II";
            [[NSProcessInfo processInfo] setProcessName:appName];
            NSMenu* bar = [[NSMenu alloc] init];
            NSMenuItem* appItem = [[NSMenuItem alloc] init];
            [bar addItem:appItem];
            NSMenu* appMenu = [[NSMenu alloc] initWithTitle:appName];
            [appMenu addItemWithTitle:@"Native ARM64 · static recompilation"
                               action:nil keyEquivalent:@""];
            [appMenu addItem:[NSMenuItem separatorItem]];
            [[appMenu addItemWithTitle:@"Hide"
                                action:@selector(hide:) keyEquivalent:@"h"]
                setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
            [[appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                                action:@selector(terminate:) keyEquivalent:@"q"]
                setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
            [appItem setSubmenu:appMenu];

            NSMenuItem* viewItem = [[NSMenuItem alloc] init];
            [bar addItem:viewItem];
            NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
            // Cmd+F for full screen, next to the standard Cmd+Ctrl+F that
            // toggleFullScreen: already answers.
            [[viewMenu addItemWithTitle:@"Enter Full Screen"
                                 action:@selector(toggleFullScreen:) keyEquivalent:@"f"]
                setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
            [[viewMenu addItemWithTitle:@"Minimise"
                                 action:@selector(performMiniaturize:) keyEquivalent:@"m"]
                setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
            [viewItem setSubmenu:viewMenu];
            [NSApp setMainMenu:bar];
        }

        NSRect frame = NSMakeRect(0, 0, width, height);
        g_window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        // FullScreenPrimary is what makes the green button and toggleFullScreen:
        // give a real Space rather than just zooming the window.
        [g_window setCollectionBehavior:(NSWindowCollectionBehaviorFullScreenPrimary |
                                         [g_window collectionBehavior])];
        // BO2_TITRE=0 (Black Ops II.app) : titre nu, sans les compteurs de mesure.
#if defined(BO2_TARGET_MPS)
        [g_window setTitle:g_titre_nu ? @"Call of Duty: Black Ops II - Multiplayer" : @"Call of Duty: Black Ops II — native ARM64"];
#elif defined(BO2_TARGET_ZMS)
        [g_window setTitle:g_titre_nu ? @"Call of Duty: Black Ops II - Zombies" : @"Call of Duty: Black Ops II — native ARM64"];
#else
        [g_window setTitle:g_titre_nu ? @"Call of Duty: Black Ops II" : @"Call of Duty: Black Ops II — native ARM64"];
#endif
        [g_window center];

        g_layer = [CAMetalLayer layer];
        g_layer.device          = g_device;
        g_layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
        // BO2_UI_PROBEPIX=1 needs to READ the drawable back. Looking at a
        // screenshot goes through the display colour profile, so a colour written
        // as (0.6,0.9,0.5) comes back as something else and cannot be matched to
        // the draw that wrote it. Reading the texture in-process is exact.
        // ...and BO2_SHOT reads the WHOLE drawable, which framebufferOnly=YES
        // forbids outright (the blit would be rejected, silently producing no
        // picture at all — the exact class of quiet failure this tool exists to
        // rule out).
        // ...and the GAME reads it too (P806): it CopyResources the backbuffer into the
        // texture its scope lenses and refraction sample (ResolvedPostSun). Always NO.
        g_layer.framebufferOnly = NO;
        g_layer.drawableSize    = CGSizeMake(width, height);
        // Plein écran 16:10 d'une image 16:9 : les bandes montraient le fond blanc de la fenêtre (vu par le testeur 10-21).
        g_layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        [g_window setBackgroundColor:[NSColor blackColor]];

        NSView* view = [g_window contentView];
        [view setWantsLayer:YES];
        [view setLayer:g_layer];

        // Textured-quad pipeline, compiled from source at startup (no offline
        // shader toolchain in this build).
        NSError* err = nil;
        id<MTLLibrary> lib = [g_device newLibraryWithSource:@(kShaderSrc)
                                                    options:nil
                                                      error:&err];
        if (!lib) {
            fprintf(stderr, "[gfx] shader compile failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return false;
        }
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction   = g_copy_vs = [lib newFunctionWithName:@"v_main"];
        pd.fragmentFunction = g_copy_fs = [lib newFunctionWithName:@"f_main"];
        pd.colorAttachments[0].pixelFormat = g_layer.pixelFormat;
        g_pipe = [g_device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!g_pipe) {
            fprintf(stderr, "[gfx] pipeline failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return false;
        }
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        // 🎯 TWO OMISSIONS THAT BOTH SHOW UP AS GROUND STREAKING.
        //
        // mipFilter defaults to MTLSamplerMipFilterNotMipmapped, which clamps
        // EVERY implicit sample to level 0 — even on the textures that do carry a
        // chain (the reflection cubes, and any 2D built through the mip path).
        // The cube probe survived only because its shader asks for a level
        // explicitly: `reflectionProbeSampler.sample(s, dir, level(u_xlat3.z))`.
        // Anything sampling normally got level 0 regardless. So this is also a
        // prerequisite: without it, generating mips would change nothing.
        //
        // maxAnisotropy is what the original game exposes as its "Anisotropic
        // Filtering" setting, and grazing-angle ground is exactly the case it
        // exists for. It helps even on the single-level textures BO2 hands us —
        // measured here as MipLevels 0=308 / 1=1692, with no chain in
        // pInitialData — because it takes several samples along the axis of
        // compression instead of one.
        sd.mipFilter = MTLSamplerMipFilterLinear;
        {
            // ⚠️ DEFAULTS TO OFF (1). Enabling anisotropy by default was a
            // VISIBLE REGRESSION: the tester photographed afghanistan on 2026-08-26
            // covered in green speckle/hatching that was not there the day
            // before, and `BO2_ANISO=1` removes it completely — same scene, same
            // terrain, no green at all.
            //
            // The reason is the comment above this block being wrong: BO2 hands
            // us textures with NO mip chain (MipLevels 0=308 / 1=1692, nothing in
            // pInitialData). Anisotropic filtering takes several samples along
            // the axis of compression ACROSS MIP LEVELS — with no chain to walk
            // it samples undefined levels, and that garbage is the green.
            //
            // So this is worth re-enabling ONLY once mips are actually generated
            // (see the mip work in 3d44527 / 7b9a5d3). Until then it trades a
            // real, photographed artefact for a sharpness gain nobody can see
            // under the beige wash. Pass BO2_ANISO=16 to experiment.
            const char* e = bo2_getenv("BO2_ANISO");
            long a_ = e ? strtol(e, nullptr, 10) : 1;    // 1 = off, until mips exist
            if (a_ < 1) a_ = 1;
            if (a_ > 16) a_ = 16;
            sd.maxAnisotropy = (NSUInteger)a_;
            // 🎯 BO2_LOD_MIN=<n> — A DECISIVE TEST, NOT A FEATURE. Two correct mip
            // systems left the streaking untouched, so the question is whether the
            // chains are being SAMPLED at all. Clamping the sampler's minimum LOD
            // makes every fetch use level n or coarser: if the picture visibly
            // blurs, the sampler's LOD path works and the streaking is something
            // else entirely; if NOTHING changes, the shaders are choosing their
            // level explicitly (sample_level) and the sampler's clamp is bypassed.
            if (const char* lm = bo2_getenv("BO2_LOD_MIN")) {
                sd.lodMinClamp = (float)atof(lm);
                fprintf(stderr, "[gfx] sampler: lodMinClamp=%.1f (BO2_LOD_MIN) — "
                                "a diagnostic, not a setting\n", sd.lodMinClamp);
            }
            fprintf(stderr, "[gfx] sampler: mipFilter=linear maxAnisotropy=%ld "
                            "(default 1 = OFF; BO2_ANISO=16 re-enables, needs mips)\n", a_);
        }
        g_samp = [g_device newSamplerStateWithDescriptor:sd];

        // 2D UI pipeline (Gate B step 3), alpha-blended over the background.
        id<MTLLibrary> uilib = [g_device newLibraryWithSource:@(kUiShaderSrc)
                                                     options:nil error:&err];
        if (!uilib) {
            fprintf(stderr, "[gfx] ui shader compile failed: %s\n",
                    [[err localizedDescription] UTF8String]);
        } else {
            MTLRenderPipelineDescriptor* up = [[MTLRenderPipelineDescriptor alloc] init];
            up.vertexFunction   = [uilib newFunctionWithName:@"ui_v"];
            up.fragmentFunction = [uilib newFunctionWithName:@"ui_f"];
            up.colorAttachments[0].pixelFormat                 = g_layer.pixelFormat;
            up.colorAttachments[0].blendingEnabled             = YES;
            up.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
            up.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
            up.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
            up.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            g_ui_pipe = [g_device newRenderPipelineStateWithDescriptor:up error:&err];
            if (!g_ui_pipe)
                fprintf(stderr, "[gfx] ui pipeline failed: %s\n",
                        [[err localizedDescription] UTF8String]);
        }
        // BO2_UI_FLAT's stand-in pixel shader. Compiled unconditionally (it costs
        // nothing) so the diagnostic is one env var away.
        {
            NSError* fe = nil;
            id<MTLLibrary> fl = [g_device newLibraryWithSource:
                @"#include <metal_stdlib>\n using namespace metal;\n"
                 "fragment float4 bo2_flat(constant float4& c [[buffer(15)]])"
                 " { return c; }"
                                                      options:nil error:&fe];
            if (fl) g_flat_fs = [fl newFunctionWithName:@"bo2_flat"];
            if (!g_flat_fs)
                fprintf(stderr, "[gfx] BO2_UI_FLAT shader unavailable: %s\n",
                        fe ? [[fe localizedDescription] UTF8String] : "?");
        }
        // A 1x1 opaque white texture so an untextured batch still shows its
        // vertex colours through the same shader.
        MTLTextureDescriptor* wd = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:1 height:1 mipmapped:NO];
        g_white = [g_device newTextureWithDescriptor:wd];
        const uint8_t wpx[4] = { 255, 255, 255, 255 };
        [g_white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                     withBytes:wpx bytesPerRow:4];
        // ...and the transparent black one D3D hands back for an unbound slot.
        g_nulltex = [g_device newTextureWithDescriptor:wd];
        const uint8_t npx[4] = { 0, 0, 0, 0 };
        [g_nulltex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                       withBytes:npx bytesPerRow:4];
        // TYPED STAND-INS. Everything above is a 2D texture, and until now every
        // slot got one of them — including the slots the 3D material shaders
        // declare as `texture3d` (modelLightingSampler), `texturecube`
        // (reflectionProbeSampler) and `depth2d` (shadowmapSamplerSpot). Binding
        // a 2D texture to those is UNDEFINED behaviour in Metal, and
        // `sample_compare` against a non-depth texture doubly so — which is not
        // "the lighting is missing", it is "the lighting is whatever the driver
        // felt like". The dominant menu-room shader uses all three.
        {
            MTLTextureDescriptor* d3 = [[MTLTextureDescriptor alloc] init];
            d3.textureType = MTLTextureType3D;
            d3.pixelFormat = MTLPixelFormatRGBA8Unorm;
            d3.width = 1; d3.height = 1; d3.depth = 1;
            g_null3d = [g_device newTextureWithDescriptor:d3];
            [g_null3d replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0
                          withBytes:npx bytesPerRow:4 bytesPerImage:4];

            MTLTextureDescriptor* dc = [MTLTextureDescriptor
                textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                size:1 mipmapped:NO];
            g_nullcube = [g_device newTextureWithDescriptor:dc];
            for (int f = 0; f < 6; f++)
                [g_nullcube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:f
                                withBytes:npx bytesPerRow:4 bytesPerImage:4];

            // A shadow map stand-in must read as FULLY LIT (depth 1.0 = nothing
            // occludes), not 0 — a zeroed shadow map shadows the entire world.
            MTLTextureDescriptor* dd = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                             width:1 height:1 mipmapped:NO];
            dd.usage = MTLTextureUsageShaderRead;
            dd.storageMode = MTLStorageModePrivate;   // depth textures cannot be Shared here
            g_nulldepth = [g_device newTextureWithDescriptor:dd];
            // 🪤 ...AND IT WAS NEVER WRITTEN. The comment four lines up states the
            // requirement — "must read as FULLY LIT (depth 1.0 = nothing
            // occludes), not 0 — a zeroed shadow map shadows the entire world" —
            // and the code created a PRIVATE depth texture and stopped. Private
            // storage cannot take replaceRegion:, so it kept Metal's zero fill:
            // depth 0.0, i.e. EVERY shadow lookup reports fully occluded, for
            // every draw that declares a depth2d slot. The menu room's dominant
            // material shader declares one.
            // A depth attachment with clearDepth 1.0 and storeAction Store is the
            // only way to initialise a private depth texture, so do that once.
            if (g_nulldepth) {
                MTLRenderPassDescriptor* zp = [MTLRenderPassDescriptor renderPassDescriptor];
                zp.depthAttachment.texture     = g_nulldepth;
                zp.depthAttachment.loadAction  = MTLLoadActionClear;
                zp.depthAttachment.storeAction = MTLStoreActionStore;
                zp.depthAttachment.clearDepth  = 1.0;
                id<MTLCommandBuffer> zcb = [g_queue commandBuffer];
                [[zcb renderCommandEncoderWithDescriptor:zp] endEncoding];
                [zcb commit];
                [zcb waitUntilCompleted];
            }
            // BO2_NULLLIGHT=<0..255> — fill the texture3d (modelLighting) and
            // texturecube (reflectionProbe) stand-ins with a level instead of 0.
            // Zero there means "no bounced light, no reflection", which is a
            // legitimate value and also indistinguishable from the flat dark
            // image being investigated. This makes the difference measurable in
            // one run instead of a rebuild. Default stays 0 — faithful.
            {
                const char* nl = bo2_getenv("BO2_NULLLIGHT");
                const int lvl = nl ? atoi(nl) : 0;
                if (lvl > 0) {
                    const uint8_t lpx[4] = { (uint8_t)lvl, (uint8_t)lvl,
                                             (uint8_t)lvl, 255 };
                    [g_null3d replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1)
                                mipmapLevel:0 slice:0
                                  withBytes:lpx bytesPerRow:4 bytesPerImage:4];
                    for (int f = 0; f < 6; f++)
                        [g_nullcube replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                      mipmapLevel:0 slice:f
                                        withBytes:lpx bytesPerRow:4 bytesPerImage:4];
                    fprintf(stderr, "[gfx] BO2_NULLLIGHT=%d — 3d/cube stand-ins filled\n", lvl);
                }
            }
            fprintf(stderr, "[gfx] typed texture stand-ins: 3d=%s cube=%s depth=%s"
                            " (depth cleared to 1.0 = unoccluded)\n",
                    g_null3d ? "ok" : "FAILED", g_nullcube ? "ok" : "FAILED",
                    g_nulldepth ? "ok" : "FAILED");
        }

        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp finishLaunching];

        if (NSInteger hz = [[g_window screen] maximumFramesPerSecond]; hz > 0) g_refresh_hz.store((double)hz);
        g_open.store(true);
        fprintf(stderr, "[gfx] Metal window %dx%d on %s, écran %.0f Hz\n", width, height,
                [[g_device name] UTF8String], g_refresh_hz.load());
    }
    return true;
}

// P773 : température CPU et ventilateur lus dans le SMC (sans sudo). Le gel à 0-4 img/s que le testeur voit est le
// Mac qui se bride : le budget thermique du noyau tombe au plancher quand les cœurs P passent ~113 °C, et le
// ventilateur reste au minimum (1 200 tr/min) jusque-là. NSProcessInfo.thermalState et `pmset -g therm` restent
// muets (thermalState=0 pendant tout un effondrement), d'où la lecture directe. Clés relevées sur ce M1 :
// TCMz = la plus chaude des sondes CPU, F0Ac = vitesse réelle du ventilateur. Relevé dans present_report (2 s).
static double g_smc_cpu_c = 0.0, g_smc_ventilo = 0.0;
static double smc_lire(const char* k) {
    struct Cle {
        uint32_t key;
        struct { char major, minor, build, reserved; uint16_t release; } vers;
        struct { uint16_t version, length; uint32_t cpu, gpu, mem; } plimit;
        struct { uint32_t size, type; char attr; } info;
        char result, status, data8;
        uint32_t data32;
        unsigned char bytes[32];
    };
    static io_connect_t conn = 0;
    static bool essaye = false;
    if (!essaye) {
        essaye = true;
        io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
        if (s) { IOServiceOpen(s, mach_task_self(), 0, &conn); IOObjectRelease(s); }
    }
    if (!conn) return 0.0;
    auto appel = [](Cle* in, Cle* out) {
        size_t n = sizeof *out;
        return IOConnectCallStructMethod(conn, 2, in, sizeof *in, out, &n) == kIOReturnSuccess && !out->result;
    };
    Cle in = {}, out = {};
    in.key = (uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | (uint32_t)k[3];
    in.data8 = 9;                                         // descripteur de la clé
    if (!appel(&in, &out) || out.info.size != 4) return 0.0;
    in.info.size = 4; in.data8 = 5;                       // ses octets : un float ('flt ') sur ce M1
    out = {};
    if (!appel(&in, &out)) return 0.0;
    float f; memcpy(&f, out.bytes, 4);
    return f;
}

// Refresh the title-bar HUD, at most twice a second. Main thread only (the pump
// is the main thread, so no dispatch is needed). Rates are measured over the
// interval actually elapsed rather than assumed, so a stall reads as a low
// number instead of a stale one.
void hud_tick() {
    if (!g_hud || !g_window) return;
    static double t0 = 0; static uint64_t f0 = 0, g0 = 0;
    double now = CACurrentMediaTime();
    uint64_t f = g_frames.load(), g = g_guest_frames.load();
    if (t0 == 0.0) { t0 = now; f0 = f; g0 = g; return; }
    double dt = now - t0;
    if (dt < 0.5) return;
    char buf[400], chaleur[96] = "";
    // P773 : à ≥ 110 °C le noyau va brider le CPU (plancher vu à 113-115 °C) — l'image s'effondre alors pour une
    // raison qui n'est pas le jeu. Le dire à côté des fps.
    if (g_smc_cpu_c > 0)
        snprintf(chaleur, sizeof chaleur, "  |  CPU %.0f °C, fan %.0f rpm%s", g_smc_cpu_c, g_smc_ventilo,
                 g_smc_cpu_c >= 110 ? " : MAC TOO HOT" : "");
    snprintf(buf, sizeof buf,
             "Black Ops II · native ARM64  —  %.0f fps window · %.1f fps engine  |  "
             "%zu draws (%zu screen, %zu passes)  |  %llu img (%llu art)  |  "
             "%llu pipe%s%s%s",
             (double)(f - f0) / dt, (double)(g - g0) / dt,
             g_hud_n, g_hud_screen, g_hud_pass,
             (unsigned long long)g_img_uploaded, (unsigned long long)g_img_art,
             (unsigned long long)g_pipe_built,
             g_pipe_failed ? "  |  PIPE FAILURES" : "",
             g_rd_drop_recs ? "  |  DRAWS DROPPED" : "",
             chaleur);
    if (!g_titre_nu) [g_window setTitle:[NSString stringWithUTF8String:buf]];
    // AND PUT THE TWO RATES IN THE LOG. "It used to be perfectly fluid and now it
    // stutters" is a real regression report and there was no way to answer it
    // from a log: the only place the frame rates existed was a window title
    // nobody records. Every 10th HUD tick is ~5 s, which is cheap enough to leave
    // on always and dense enough to show a stall as a dip rather than a gap.
    // ⚠️ The two numbers answer DIFFERENT questions. "window" is the pump
    // re-presenting — it stays high even when the guest has died, so it is not
    // evidence of anything running. "engine" is guest Presents, and that is the
    // one that reads as smooth or not.
    static uint32_t hud_n = 0;
    if ((hud_n++ % 10) == 0) {
        fprintf(stderr, "[FPS] window=%.0f engine=%.1f  (%zu draws, %zu screen, %zu passes)\n",
                (double)(f - f0) / dt, (double)(g - g0) / dt,
                g_hud_n, g_hud_screen, g_hud_pass);
        // ── WHY IS Present NOT BEING CALLED? ────────────────────────────────
        // Measured on angola, 2026-08-15: [D3D-PRESENT] frozen at frame 3500
        // for 30 s while the log grew 850 lines and `[gfx] real draws` kept
        // reporting a full 538-record, 504-screen-draw, 16-pass frame. So the
        // engine BUILDS a frame and never presents it, which is the render
        // completion handshake never closing — the `rendStarted=1 frameDone=0`
        // the [DEADLOCK-DUMP] showed.
        //
        // Those globals were already named by that dump, but it only fires from
        // a SIGHUP deferred to the next Win32 IMPORT — and a guest that has
        // stopped calling imports never reaches it, so the one dump that could
        // answer this cannot be triggered when it matters. Measured: SIGHUP to
        // the frozen process produced nothing at all.
        //
        // Reading them here costs four guest loads per ten HUD samples, on the
        // thread that is already awake, and works precisely when the guest has
        // gone quiet. Printed unconditionally next to the frame counters,
        // because "which flag is stuck" is only meaningful beside "how many
        // frames were presented".
        // ── AND THE HEARTBEATS, ON THIS SAME HOST CLOCK ────────────────────
        // [FRAME-HANDSHAKE] answers "which render flag is stuck". It cannot
        // answer the question one level up: is the ENGINE still running at all?
        // Every progress counter in the runtime prints on its own cadence
        // (`n % 500 == 0`), so a counter that stops just stops printing, and on
        // angola [SV-TICK] going quiet after six lines meant anything between 7
        // and 499 further ticks. Read from here the numbers are exact and, more
        // importantly, REPEATED — two equal samples are the proof of a stop that
        // a sparse print can never give.
        win32_cadence_report((unsigned long long)g);
        // The teardown guard runs on THIS clock, not on the freeze report's
        // exponential one — see its comment: it fired once and then waited.
        win32_gsc_teardown_guard();
        win32_skip_teardown_poll();
        fprintf(stderr, "[FRAME-HANDSHAKE] rendStarted[0x338f850]=%u frameDone[0x338f85c]=%u "
                        "depth[0x338f860]=%u backendFlag[0x338f858]=%u owner[0x2925588]=0x%08x\n",
                win32_mem_read32(0x338f850u), win32_mem_read32(0x338f85cu),
                win32_mem_read32(0x338f860u), win32_mem_read32(0x338f858u),
                win32_mem_read32(0x2925588u));
        // ── 🎯 GIVE THE CYCLE A BIRTHDAY — BO2_MT_AUDIT_EVERY=<n> ───────────
        // The audit below runs only once the guest is ALREADY wedged. That can
        // say "the free-block tree has a cycle now"; it can never say WHEN it
        // appeared, and it can never run at all on afghanistan, which does not
        // freeze — so the control this project's rule 1 demands (does the level
        // that WORKS grow the same cycle?) was structurally impossible to take.
        // Same shape as the [DEADLOCK-DUMP] deferred to the next import: a probe
        // that can only fire on the symptom cannot describe the run-up to it.
        //
        // The corruption is deterministic (two runs, byte-identical: 1 cycle,
        // 4 nodes with two parents, 340 nodes, node 65535), so the window
        // between the last clean audit and the first dirty one names the
        // culprit directly. n=1 audits every ~5 s, which is ~131k guest byte
        // reads per audit — off by default, cheap enough to leave on for a
        // diagnostic run.
        {
            static const int period = [] {
                const char* e = bo2_getenv("BO2_MT_AUDIT_EVERY");
                return e ? atoi(e) : 0;
            }();
            static int nsample = 0;
            if (period > 0 && (++nsample % period) == 0) {
                win32_mt_tree_audit("periodic — BO2_MT_AUDIT_EVERY");
                // Which starved assets turned out to be merely late — the
                // exhaustive form of how weapons (type 25) were caught.
                win32_clamp_verdict("periodic");
                win32_pvar_census("periodic");
                win32_gsc_sibling_audit("periodic");
                // The teardown report only ran from the [FROZEN] block, whose
                // cadence widens exponentially (samples 1, 4, 16, 64…), so the
                // server instance — the one with 1,865 queued entries — was
                // rarely sampled at all. Its completeness scan is the measurement
                // that matters now, so put it on the regular clock too.
                win32_gsc_teardown_report();
                // 🎯 AND THE DRAIN-LOOP COUNTERS, on the regular clock for the
                // same reason: the run that first showed element 0x4c2 coming
                // back died with imports still CLIMBING, so the repeat happens
                // while the game is alive and the freeze-clocked summary never
                // printed. A counter that only reports at the freeze cannot say
                // whether the loop recovers.
                win32_iter_report();
                win32_stack_census("periodic");
                // 🎯 AND THE VARIABLE CHAINS, on the same clock and for the same
                // reason. The freeze-triggered audit measured angola at 1,714
                // cycles / 5,191 self-links against 1 / 195 during load — but the
                // only afghanistan number available was ALSO from load, because a
                // level that never freezes never triggers a freeze-clocked probe.
                // Comparing "angola in play" against "afghanistan at load" is not
                // a control. This makes both measurable in the same state.
                win32_gsc_chain_audit("periodic — BO2_MT_AUDIT_EVERY");
            }
        }
        // ── REPORT THE SPIN FROM A SITE THAT ACTUALLY FIRES ─────────────────
        // [IMP-HIST]/[IMP-SITE] name which import and which code is looping, and
        // they were wired to print from [RUNAWAY]. But [RUNAWAY] keys on 3,000,000
        // IMPORTS without a new frame, and angola's freeze does not call imports
        // fast enough to reach it: measured, 50,000 log lines with presents
        // pinned at 3500 and no [RUNAWAY] at all. The one report that names the
        // loop could not fire in the case it was written for — the same shape as
        // the SIGHUP dump deferred to the next import.
        //
        // So report on the condition the question is actually about: guest
        // Presents have not advanced between HUD samples. Once per freeze
        // episode, re-armed when frames move again, so a long freeze does not
        // paper the log.
        {
            // ⚠️ REPORT AGAIN AS THE WINDOW GROWS. The histogram resets on every
            // presented frame, so during a freeze it accumulates — and the FIRST
            // sample after the freeze starts covers only a second or two, which
            // is far too short to tell a spin from ordinary per-frame work. The
            // first report was read as a ranking and it was a snapshot. So
            // report at 1, then at widening intervals, and SAY which sample it
            // is: the later ones cover a window orders of magnitude longer.
            static uint64_t last_g = 0;
            static int said = 0;
            if (g == last_g) {
                ++said;
                if (said == 1 || said == 4 || said == 16 || said == 64 ||
                    (said % 256) == 0) {
                    fprintf(stderr, "[FROZEN #%d] guest Presents have not advanced for %d "
                                    "HUD sample(s) — the window below is that long:\n",
                            said, said);
                    // FIRST, AND BEFORE ANY HISTOGRAM: what is the guest actually
                    // executing right now. Every other report below answers "what
                    // did it do recently", and a thread spinning inside one lifted
                    // block did nothing recently — which is exactly why two angola
                    // freezes could only be named by running macOS `sample` by
                    // hand. This is that answer, in the log, every time.
                    win32_name_the_spin("frozen: no new guest Present");
                    win32_imp_hist_report("frozen: no new guest Present");
                    // AND THE ONE STRUCTURE THAT CAN TRAP THE GUEST FOREVER.
                    // `sample` on the frozen angola puts tid 0 at 2009/2009
                    // samples inside sub_87bd70, a binary-tree descent whose
                    // only exit is a zero child. Audit it here, on the host,
                    // while the guest is wedged — the guest cannot audit a
                    // structure it is currently spinning inside.
                    // Only on the first and fourth reports: the walk is
                    // ~131,072 guest byte reads and the answer does not change
                    // while nothing runs.
                    if (said == 1 || said == 4)
                        win32_mt_tree_audit("frozen: no new guest Present");
                    if (said == 1 || said == 4)
                        win32_gsc_chain_audit("frozen: no new guest Present");
                    // And the walk itself, every report: if eax/ecx move while
                    // presents do not, the guest is walking and never arriving.
                    win32_mt_spin_report(0);
                    // 🎯 And WHY that walk never ends: [SPIN] names sub_87c8e0
                    // (the GSC table's remove) and no longer names sub_4773c0
                    // at all. Cycle or absent key are opposite defects with
                    // opposite fixes, so replay the walk and say which.
                    win32_unlink_probe(0);
                    win32_iter_report();
                    // 🎯 And WHO holds the lock loop: [IMP-HIST] says 100% of the
                    // 2.2 million calls since the last frame are Enter/Leave
                    // CriticalSection, perfectly paired. BO2_CS_WHO names the
                    // return address and the object.
                    win32_cs_report();
                    win32_alloc_who_report();
                    win32_slget_report();
                    win32_intern_report();
                    win32_slfree_who_report();
                    win32_svframe_gate_report();
                    win32_frame_budget_report();
                    win32_drain_count_report();
                    win32_deq_report();
                    win32_gsc_loop_report();
                    win32_cast_report();
                    win32_obj_hist_report();
                    win32_err_catalogue_report();
                    win32_sl_band_report();
                    win32_mt_inflight_report();
                    win32_all_regs_report();
                    win32_spin_histogram("frozen: no new guest Present");
                    // And, for the fourth loop: is the value it hunts even in the chain?
                    win32_gsc_find_report();
                    // And the list the teardown loop drains — the layer the
                    // freeze moved to once the MT guards took the allocator out
                    // of the picture. Two guest words say whether it is draining.
                    win32_gsc_teardown_report();
                    // EVERY guest thread's registers. Which thread is spinning
                    // varies run to run — `sample` caught tid 0 inside the MT
                    // allocator's treap insert on one freeze and a sound thread
                    // inside sub_4a98c0's counted mix loop on the next — so the
                    // dump must cover all of them rather than the one suspected
                    // last time. Two consecutive dumps give the deltas.
                    win32_dump_all_threads();
                }
            } else {
                said = 0;
            }
            last_g = g;
        }
    }
    t0 = now; f0 = f; g0 = g;
}

// --- INPUT: Cocoa events -> a queue of Win32 messages ------------------------
// OUR WINDOW IS THE GAME'S WINDOW, so we are the thing Windows would be: the
// faithful translation is NSEvent -> (uMsg, wParam, lParam) delivered to the
// guest's own WNDPROC. Measured first, before writing any of it: over a whole
// run the guest calls PeekMessageA, GetCursorPos and XInputGetState ZERO times.
// It is not that input arrives and is mistranslated — nothing arrives, because
// the engine learns its own HWND only from WM_CREATE and learns it has focus
// only from WM_ACTIVATE*, and it had never received either.
//
// The queue exists because the two sides are different threads: AppKit events
// can only be read on the real main thread, and the guest's WNDPROC may only be
// entered on the guest thread that owns the State. The consumer is the input
// frame (FUN_005fc160, tid 0) — exactly where Windows would have pumped.
namespace {
struct WinMsg { uint32_t msg, wp, lp; };
std::mutex g_in_mu;
std::vector<WinMsg> g_in_q;
bool g_in_focus = false;

void in_push(uint32_t msg, uint32_t wp, uint32_t lp) {
    // Bit 24 de lParam = touche « étendue » sous Windows (flèches, Pg↑/Pg↓, Début/Fin, Inser/Suppr). Le jeu le lit
    // (MapKey 0x840830, table 0xbe85d8 : une entrée par VK et par bit) : sans lui ↑ = 8 du pavé (K_KP_UPARROW 183,
    // que les menus ignorent) au lieu de K_UPARROW 154.
    if ((msg == 0x0100 || msg == 0x0101) && ((wp >= 0x21 && wp <= 0x28) || wp == 0x2D || wp == 0x2E)) lp |= 1u << 24;
    std::lock_guard<std::mutex> lk(g_in_mu);
    if (g_in_q.size() > 256) return;      // a stalled guest must not grow this
    g_in_q.push_back({ msg, wp, lp });
}

// macOS virtual keycode -> Windows VK, for the keys with no character form.
// Everything that HAS a character (letters, digits) is mapped from
// charactersIgnoringModifiers instead, which keeps this table short and avoids
// baking in a US layout.
uint32_t vk_from_keycode(unsigned short kc);

// ── BO2_KEYPIPE — DRIVEN INPUT THAT DOES NOT GO THROUGH THE WINDOW SERVER ──
//
// WHY. `in_translate` above is the ONLY producer of key events in this runtime,
// and it is fed exclusively by [NSApp nextEventMatchingMask:]. An inactive
// application does not receive key events there, and on macOS 14+ an app cannot
// make itself active from the outside — `NSRunningApplication
// activateWithOptions:` and `System Events … set frontmost` both return success
// and change nothing (measured, 2026-08-02). So on a Mac somebody is actually
// using, `scripts/sendkey` posts a key with CGEventPostToPid, the key is
// accepted by the system, and it never becomes a WM_KEYDOWN. FOUR consecutive
// runs died at Press Start that way, and none of them printed anything: every
// probe downstream of the message queue is silent when the queue is empty, so
// the failure is indistinguishable from a frontend that ignores input.
// (The proof it never arrived: `[CFN-DIFF] armed on vk=…`, which fires from the
//  WM_KEYDOWN pump, appears in the last known-good run and in NONE of mine.)
//
// BO2_KEYPIPE=<path> creates a FIFO and injects straight into the same queue
// in_translate writes to — same messages, same order, same consumer. It takes
// macOS virtual keycodes, not VKs, so scripts/sendkey's interface is unchanged
// and 0x24/0x7D/0x7E keep meaning Return/Down/Up everywhere.
//
//     line format:  "<mac keycode hex or dec> <hold ms>"    e.g. "0x24 400"
//
// This is measurement plumbing, not a gameplay path: it is off unless the
// variable is set, and it never touches what the user has in front of them.
void in_push(uint32_t msg, uint32_t wp, uint32_t lp);
static std::atomic<char> g_fenetre_cmd{0};   // P774 : 3e lettre de « reduire », « masquer », « rendre »

void keypipe_thread(std::string path) {
    // O_RDWR, not O_RDONLY: a FIFO opened read-only blocks until a writer
    // appears and then returns EOF every time the last writer closes, so a
    // reader loop spins. Holding a writer end ourselves keeps it open forever.
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[KEYPIPE] cannot open %s: %s\n", path.c_str(), strerror(errno));
        return;
    }
    fprintf(stderr, "[KEYPIPE] listening on %s\n", path.c_str());
    std::string buf;
    char chunk[256];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        buf.append(chunk, (size_t)n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            // P774 : « reduire », « masquer » (⌘H), « rendre » — appliqués par la pompe (fil principal).
            if (line == "reduire" || line == "masquer" || line == "rendre") {
                g_fenetre_cmd.store(line[2]);
                fprintf(stderr, "[KEYPIPE] fenetre %s\n", line.c_str());
                continue;
            }
            unsigned kc = 0, hold = 60;
            // « G 300 » : une lettre majuscule = sa VK. Les lettres n'ont pas de keycode dans vk_from_keycode,
            // in_translate les prend du caractère (vk_from_event) ; sans ça G (grenade) était injoignable (P791).
            const bool lettre = line.size() >= 2 && line[0] >= 'A' && line[0] <= 'Z' && line[1] == ' ';
            if (lettre) { kc = (unsigned char)line[0]; sscanf(line.c_str() + 2, "%u", &hold); }
            else if (sscanf(line.c_str(), "%i %u", &kc, &hold) < 1) {
                fprintf(stderr, "[KEYPIPE] unparsed line: \"%s\"\n", line.c_str());
                continue;
            }
            uint32_t vk = lettre ? kc : vk_from_keycode((unsigned short)kc);
            if (!vk) {
                fprintf(stderr, "[KEYPIPE] keycode 0x%x has no VK mapping — ignored\n", kc);
                continue;
            }
            if (hold < 1) hold = 1;
            if (hold > 5000) hold = 5000;
            // SAY IT. A driven keystroke that produced nothing must be
            // distinguishable from one that was never sent; that distinction is
            // the entire reason this exists.
            fprintf(stderr, "[KEYPIPE] key kc=0x%x -> vk=0x%x, hold %ums\n", kc, vk, hold);
            in_push(0x0100, vk, 0x00000001u);
            std::this_thread::sleep_for(std::chrono::milliseconds(hold));
            in_push(0x0101, vk, 0xC0000001u);
        }
    }
    fprintf(stderr, "[KEYPIPE] reader ended\n");
    close(fd);
}

void keypipe_start_once() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* p = bo2_getenv("BO2_KEYPIPE");
    if (!p || !*p) return;
    unlink(p);
    if (mkfifo(p, 0600) != 0 && errno != EEXIST) {
        fprintf(stderr, "[KEYPIPE] mkfifo %s failed: %s\n", p, strerror(errno));
        return;
    }
    std::thread(keypipe_thread, std::string(p)).detach();
}

// ── BO2_CLICKPIPE — DRIVEN MOUSE INPUT, for the same reason BO2_KEYPIPE exists ──
//
// WHY. Everything the BO2_KEYPIPE comment above says about an inactive
// application applies unchanged to the mouse: `in_translate` is the only producer
// of mouse messages and it is fed exclusively by [NSApp nextEventMatchingMask:],
// so a `CGEventPostToPid` left click at the right screen point is accepted by the
// system and never becomes a WM_LBUTTONDOWN. The game reached a state that ASKS
// for a click — the "click to continue" pictogram the tester saw and flagged as new —
// and there was no way at all to answer it from a script. A key could be driven;
// a click could not.
//
// WHY IT IS A SECOND PIPE rather than more syntax on the first: a click line
// carries two to four numbers and a key line one to two, so bare numbers cannot
// be told apart. Explicit verbs, and `scripts/sendkey`'s interface is untouched.
//
//     click  <x> <y> [hold ms]     left  press/release at client (x, y)
//     rclick <x> <y> [hold ms]     right press/release
//     move   <x> <y>               motion only, no button
//
// x and y are CLIENT pixels, top-left origin — exactly the space mouse_lparam
// packs, so a driven click is indistinguishable from a real one downstream.
// Written WITH A '.' they are read as FRACTIONS of the client area instead
// (`click 0.5 0.5` is dead centre). That matters because the caller is working
// from a screenshot of a window whose size it does not know, and because this
// runtime already resizes the drawable underneath itself.
//
// Measurement plumbing, like the keypipe: off unless the variable is set.
std::atomic<int> g_client_w{0}, g_client_h{0};   // client POINTS, refreshed per pump

uint32_t mouse_lparam_xy(int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    return ((uint32_t)(y & 0xffff) << 16) | (uint32_t)(x & 0xffff);
}

// Returns false when the token is unparseable, or when it is fractional and we do
// not yet know the client size — SAYING SO rather than silently clicking at 0,0,
// which would look exactly like a click the engine ignored.
bool clickpipe_coord(const char* tok, int extent, int* out) {
    if (!tok || !*tok) return false;
    char* end = nullptr;
    double v = strtod(tok, &end);
    if (end == tok) return false;
    if (strchr(tok, '.')) {
        if (extent <= 0) return false;
        v *= (double)extent;
    }
    *out = (int)(v < 0 ? 0 : v + 0.5);
    return true;
}

void clickpipe_thread(std::string path) {
    // O_RDWR for the same reason as the keypipe: holding a writer end ourselves
    // stops the reader loop spinning on EOF each time a writer closes.
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[CLICKPIPE] cannot open %s: %s\n", path.c_str(), strerror(errno));
        return;
    }
    fprintf(stderr, "[CLICKPIPE] listening on %s  (client %dx%d)\n",
            path.c_str(), g_client_w.load(), g_client_h.load());
    std::string buf;
    char chunk[256];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        buf.append(chunk, (size_t)n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty() || line[0] == '#') continue;
            char verb[16] = {0}, xs[32] = {0}, ys[32] = {0};
            unsigned hold = 60;
            int got = sscanf(line.c_str(), "%15s %31s %31s %u", verb, xs, ys, &hold);
            if (got < 3) {
                fprintf(stderr, "[CLICKPIPE] unparsed line: \"%s\"\n", line.c_str());
                continue;
            }
            const int cw = g_client_w.load(), ch = g_client_h.load();
            int x = 0, y = 0;
            if (!clickpipe_coord(xs, cw, &x) || !clickpipe_coord(ys, ch, &y)) {
                fprintf(stderr, "[CLICKPIPE] bad coords \"%s %s\" (client %dx%d — "
                                "fractions need a presented frame first)\n", xs, ys, cw, ch);
                continue;
            }
            if (hold < 1) hold = 1;
            if (hold > 5000) hold = 5000;
            const uint32_t lp = mouse_lparam_xy(x, y);
            const bool right = (strcmp(verb, "rclick") == 0);
            const bool move  = (strcmp(verb, "move")   == 0);
            if (!right && !move && strcmp(verb, "click") != 0) {
                fprintf(stderr, "[CLICKPIPE] unknown verb \"%s\" "
                                "(want click|rclick|move)\n", verb);
                continue;
            }
            // SAY IT — a driven click that produced nothing must be
            // distinguishable from one that was never sent. That distinction is
            // the entire reason this exists.
            fprintf(stderr, "[CLICKPIPE] %s at client (%d,%d) of %dx%d%s\n",
                    verb, x, y, cw, ch, move ? "" : (right ? " [right]" : " [left]"));
            // Motion first even for a click: engines that hit-test against a
            // CACHED cursor position, rather than the message's own lParam, see
            // nothing at all from a button message that arrives out of nowhere.
            in_push(0x0200, 0, lp);                       // WM_MOUSEMOVE
            if (move) continue;
            in_push(right ? 0x0204u : 0x0201u, right ? 2u : 1u, lp);   // *BUTTONDOWN
            std::this_thread::sleep_for(std::chrono::milliseconds(hold));
            in_push(right ? 0x0205u : 0x0202u, 0u, lp);                // *BUTTONUP
        }
    }
    fprintf(stderr, "[CLICKPIPE] reader ended\n");
    close(fd);
}

void clickpipe_start_once() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* p = bo2_getenv("BO2_CLICKPIPE");
    if (!p || !*p) return;
    unlink(p);
    if (mkfifo(p, 0600) != 0 && errno != EEXIST) {
        fprintf(stderr, "[CLICKPIPE] mkfifo %s failed: %s\n", p, strerror(errno));
        return;
    }
    std::thread(clickpipe_thread, std::string(p)).detach();
}

// ── BO2_KB_PHYSICAL — MOVEMENT KEYS BY POSITION, NOT BY LETTER ───────────────
//
// WHY. The tester, on a French Mac, played afghanistan and said the controls were
// "pas intuitives" (2026-08-14). They would be. Letters reach the guest through
// `charactersIgnoringModifiers`, which is LAYOUT-DEPENDENT: on AZERTY the key in
// the top-left of the movement cluster reports 'a', and the one above it 'z'. BO2
// binds forward to W and strafe-left to A, so on that keyboard "forward" is a key
// two rows away from where the hand sits and the cluster is not a cluster at all.
//
// macOS virtual key CODES are positional and identical on every layout: 13, 0, 1,
// 2 are the four keys that form the WASD diamond wherever you are. Mapping those
// four by code makes the physical ZQSD cluster on AZERTY (and WASD on QWERTY)
// mean what the hand expects, without touching any other key.
//
// ⚠️ OFF BY DEFAULT, because it is a real trade-off, not a free win: forcing
// these four by position means the same four keys can no longer TYPE their letter
// (an AZERTY 'q' would arrive as VK_A). That does not matter while moving and
// does matter in a console or a name field, so it is a choice, not a default.
//   BO2_KB_PHYSICAL=1 scripts/play.sh
uint32_t vk_from_keycode(unsigned short kc) {
    static const int physical = bo2_env_on("BO2_KB_PHYSICAL") ? 1 : 0;
    // ⚠️ MAP THE WHOLE CLUSTER, NOT JUST WASD. The first version forced only
    // 13/0/1/2, which silently made the LETTER Q UNREACHABLE: on AZERTY 'q' is
    // produced by keycode 0, and forcing that to VK_A left no key anywhere that
    // sends VK_Q. The tester was hunting for a descend key at the time, and if it
    // had been Q I had just removed it while claiming to improve the controls.
    // Mapping the surrounding keys as well keeps every letter reachable AND
    // positional, which is what "play it like a QWERTY game" actually means.
    if (physical) switch (kc) {
        case 12: return 'Q';      // AZERTY A — keeps VK_Q reachable
        case 13: return 'W';      // AZERTY Z — forward
        case 14: return 'E';      // AZERTY E — ascend (the tester found this one)
        case  0: return 'A';      // AZERTY Q — strafe left
        case  1: return 'S';      // AZERTY S — back
        case  2: return 'D';      // AZERTY D — strafe right
        case  6: return 'Z';      // AZERTY W — keeps VK_Z reachable
        case  7: return 'X';      // AZERTY X
        case  8: return 'C';      // AZERTY C — the usual descend/crouch
        default: break;
    }
    // Rangée des chiffres : sous Windows elle envoie VK_1..VK_0 quelle que soit la disposition (AZERTY
    // compris, sans Maj) ; ici le caractère non majuscule (& é " ' …) ne donnait aucune VK.
    switch (kc) {
        case 18: return '1'; case 19: return '2'; case 20: return '3'; case 21: return '4';
        case 23: return '5'; case 22: return '6'; case 26: return '7'; case 28: return '8';
        case 25: return '9'; case 29: return '0';
        default: break;
    }
    switch (kc) {
        case 0x24: return 0x0D;   // Return      -> VK_RETURN
        case 0x4C: return 0x0D;   // Keypad Enter
        case 0x30: return 0x09;   // Tab         -> VK_TAB
        case 0x31: return 0x20;   // Space       -> VK_SPACE
        case 0x33: return 0x08;   // Delete      -> VK_BACK
        case 0x35: return 0x1B;   // Escape      -> VK_ESCAPE
        case 0x7B: return 0x25;   // Left        -> VK_LEFT
        case 0x7E: return 0x26;   // Up          -> VK_UP
        case 0x7C: return 0x27;   // Right       -> VK_RIGHT
        case 0x7D: return 0x28;   // Down        -> VK_DOWN
        case 0x74: return 0x21;   // PageUp
        case 0x79: return 0x22;   // PageDown
        case 0x77: return 0x23;   // End
        case 0x73: return 0x24;   // Home
        case 0x75: return 0x2E;   // Fwd Delete  -> VK_DELETE
        case 0x7A: return 0x70;   // F1 .. F12
        case 0x78: return 0x71;
        case 0x63: return 0x72;
        case 0x76: return 0x73;
        case 0x60: return 0x74;
        case 0x61: return 0x75;
        case 0x62: return 0x76;
        case 0x64: return 0x77;
        case 0x65: return 0x78;
        case 0x6D: return 0x79;
        case 0x67: return 0x7A;
        case 0x6F: return 0x7B;
        default:   return 0;
    }
}

uint32_t vk_from_event(NSEvent* e) {
    uint32_t vk = vk_from_keycode([e keyCode]);
    if (vk) return vk;
    NSString* s = [e charactersIgnoringModifiers];
    if ([s length] == 0) return 0;
    unichar c = [s characterAtIndex:0];
    if (c >= 'a' && c <= 'z') return (uint32_t)(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (uint32_t)c;
    return 0;
}

// Client-relative mouse position, packed the way lParam wants it. The window is
// the drawable, so "client" is the content view in flipped (top-left) space.
uint32_t mouse_lparam(NSEvent* e) {
    if (!g_window) return 0;
    NSPoint p = [e locationInWindow];
    NSView* v = [g_window contentView];
    NSPoint c = [v convertPoint:p fromView:nil];
    int x = (int)c.x, y = (int)(v.bounds.size.height - c.y);   // flip to Win32
    if (x < 0) x = 0; if (y < 0) y = 0;
    return ((uint32_t)(y & 0xffff) << 16) | (uint32_t)(x & 0xffff);
}

// Translate one AppKit event. Returns true when the event was consumed as game
// input and must NOT also go to AppKit (key events, so the system does not beep
// at every unhandled keypress).
bool in_translate(NSEvent* e) {
    switch ([e type]) {
        case NSEventTypeKeyDown: {
            uint32_t vk = vk_from_event(e);
            if (!vk) return false;
            // bit 30 = "key was already down": the engine uses it for repeat.
            in_push(0x0100, vk, [e isARepeat] ? 0x40000001u : 0x00000001u);
            NSString* ch = [e characters];
            if ([ch length]) {
                unichar c = [ch characterAtIndex:0];
                // TranslateMessage donne aussi un WM_CHAR aux touches de contrôle : Retour arrière 0x08
                // (AppKit dit 0x7F), Entrée 0x0D, Tab 0x09, Échap 0x1B. Sans 0x08, aucun champ de texte
                // ne s'efface (nom de classe, 10-21).
                if (c == 0x7f) c = 0x08;
                if ((c >= 0x20 && c < 0x7f) || c == 0x08 || c == 0x0D || c == 0x09 || c == 0x1B)
                    in_push(0x0102, c, 1);   // WM_CHAR
            }
            return true;
        }
        case NSEventTypeKeyUp: {
            uint32_t vk = vk_from_event(e);
            if (!vk) return false;
            in_push(0x0101, vk, 0xC0000001u);
            return true;
        }
        // ── MODIFIERS. Ctrl = crouch and Shift = sprint, and NEITHER HAS EVER
        // WORKED IN THIS PORT, because AppKit does not deliver them as KeyDown
        // or KeyUp at all: a modifier is an NSEventTypeFlagsChanged event, and
        // this switch had no case for it, so the events fell through to
        // `[NSApp sendEvent:]` and the guest never saw a thing.
        //
        // Found 2026-08-14 while the tester was looking for a key to descend and I
        // was listing Ctrl and Shift as things to try — they could not have
        // worked. ⚠️ The failure is silent and total, and it looks exactly like
        // "the engine ignores crouch", which is the same shape as the four runs
        // lost to BO2_KEYPIPE's absence.
        //
        // FlagsChanged carries no up/down flag, only the new modifier mask, so
        // the transition has to be derived by comparing against the previous
        // mask. Each modifier is tracked separately: pressing Shift while Ctrl
        // is held must not re-send Ctrl.
        case NSEventTypeFlagsChanged: {
            static NSEventModifierFlags prev = 0;
            const NSEventModifierFlags now = [e modifierFlags];
            const struct { NSEventModifierFlags bit; uint32_t vk; } mods[] = {
                { NSEventModifierFlagShift,   0x10 },   // VK_SHIFT
                { NSEventModifierFlagControl, 0x11 },   // VK_CONTROL
                { NSEventModifierFlagOption,  0x12 },   // VK_MENU (alt)
            };
            for (auto& m : mods) {
                const bool was = (prev & m.bit) != 0, is = (now & m.bit) != 0;
                if (was == is) continue;
                if (is) in_push(0x0100, m.vk, 0x00000001u);   // WM_KEYDOWN
                else    in_push(0x0101, m.vk, 0xC0000001u);   // WM_KEYUP
            }
            prev = now;
            return true;      // consumed: AppKit must not also beep at it
        }
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
            in_push(0x0200, 0, mouse_lparam(e));  return false;
        case NSEventTypeLeftMouseDown:
            in_push(0x0201, 1, mouse_lparam(e));  return false;
        case NSEventTypeLeftMouseUp:
            in_push(0x0202, 0, mouse_lparam(e));  return false;
        case NSEventTypeRightMouseDown:
            in_push(0x0204, 2, mouse_lparam(e));  return false;
        case NSEventTypeRightMouseUp:
            in_push(0x0205, 0, mouse_lparam(e));  return false;
        // Molette / pavé tactile -> WM_MOUSEWHEEL (120 par cran, mot haut du wParam). Le pavé donne des
        // points continus : on les cumule, un cran tous les 24 points ; l'inertie est ignorée pour qu'un
        // geste ne fasse pas défiler dix armes.
        case NSEventTypeScrollWheel: {
            static double acc = 0;
            if ([e momentumPhase] != NSEventPhaseNone) return false;
            const double step = [e hasPreciseScrollingDeltas] ? 24.0 : 1.0;
            acc += [e scrollingDeltaY];
            while (acc >= step || acc <= -step) {
                const int16_t d = acc > 0 ? 120 : -120;
                acc -= acc > 0 ? step : -step;
                in_push(0x020A, (uint32_t)(uint16_t)d << 16, mouse_lparam(e));
            }
            return false;
        }
        default:
            return false;
    }
}
}  // namespace

// Consumer side, called on the guest thread. Returns 0 when the queue is empty.
extern "C" int gfx_input_pop(uint32_t* msg, uint32_t* wp, uint32_t* lp) {
    std::lock_guard<std::mutex> lk(g_in_mu);
    if (g_in_q.empty()) return 0;
    *msg = g_in_q.front().msg; *wp = g_in_q.front().wp; *lp = g_in_q.front().lp;
    g_in_q.erase(g_in_q.begin());
    return 1;
}

// ── 🎯 [PRESENT] — CE QUI ATTEINT REELLEMENT L'ECRAN ─────────────────────────
// Les compteurs historiques comptent ce que l'invite PRODUIT (guestFrames,
// [VB-COPIE]) ou ce que la pompe ACQUIERT (g_frames, « fps window » : incremente
// apres nextDrawable, AVANT la decision de sauter un re-encodage, donc il compte
// aussi des drawables jamais presentes). Aucun ne dit ce que l'ecran a montre.
// Ici chaque drawable soumis porte un addPresentedHandler : presentedTime > 0 =
// affiche, a cet instant (meme base que CACurrentMediaTime) ; 0 = abandonne.
// On separe les images NEUVES (une liste de dessins que l'invite vient de finir)
// des RE-ENCODAGES de la precedente, que la pompe presente une fois sur
// BO2_REPUMP_EVERY quand l'invite n'a rien de neuf. Le HUD Metal
// (MTL_HUD_ENABLED=1) compte les deux ; l'oeil ne voit bouger que les neuves.
// Une ligne toutes les 2 s d'hote, sans condition. Cout : un bloc et un verrou
// par drawable presente, soit au plus la frequence d'affichage.
//
// ⚠️ DEUX COMPTES, PARCE QUE « AFFICHE » DEPEND DE LA FENETRE, PAS DU JEU
// (runs/pz2_afghanistan, 2026-09-24) : fenetre cachee derriere une autre,
// presentedTime vaut 0 pour CHAQUE drawable — 79 a 195 « non affiches » par
// fenetre de 2 s, 0 affiche — et au premier plan le meme run donne 121 affichees
// sur 121 soumises, 60,02/s, ce que lisait le HUD Metal. D'ou :
//   soumis*   compte a presentDrawable: — ce que la pompe envoie, que la fenetre
//             soit visible ou non. C'est le DEBIT (img/s = soumis_neufs / fen).
//   affiches* presentedTime > 0 — ce que l'ecran a montre. Egal a soumis quand
//             la fenetre est devant : c'est le temoin du calage, pas le debit.
// Les intervalles sont pris a la SOUMISSION des images neuves (horloge hote),
// donc disponibles fenetre cachee aussi.
static bool g_present_fresh = false;
// [PRESENT] visible=/actif= : la fenetre est-elle composee par macOS (occlusionState) et l'app au premier plan ?
// Sans eux, aff/s=0 ne distingue pas une fenetre cachee d'un vrai defaut de presentation (Z-NATIF-18a). -1 = pas encore lu.
static std::atomic<int> g_fen_visible{-1}, g_app_active{-1};
static struct {
    std::mutex mu;
    uint32_t soumis = 0, soumis_neufs = 0, affiches = 0, affiches_neufs = 0, non_affiches = 0;
    double dernier_neuf = 0.0;          // CACurrentMediaTime de la derniere soumission neuve
    std::vector<double> iv_ms;          // intervalles entre soumissions neuves consecutives
} g_pst;
static uint64_t g_pst_cumul_neufs = 0, g_pst_cumul_affiches = 0;

static void present_track(id<CAMetalDrawable> drawable, bool neuf) {
    {   const double t = CACurrentMediaTime();
        std::lock_guard<std::mutex> lk(g_pst.mu);
        g_pst.soumis++;
        if (neuf) {
            g_pst.soumis_neufs++;
            if (g_pst.dernier_neuf > 0.0)
                g_pst.iv_ms.push_back((t - g_pst.dernier_neuf) * 1000.0);
            g_pst.dernier_neuf = t;
        } }
    [drawable addPresentedHandler:^(id<MTLDrawable> d) {
        std::lock_guard<std::mutex> lk(g_pst.mu);
        if (d.presentedTime <= 0.0) { g_pst.non_affiches++; return; }
        g_pst.affiches++;
        if (neuf) g_pst.affiches_neufs++;
    }];
}

static double present_quantile(std::vector<double>& v, double q) {
    if (v.empty()) return 0.0;
    const size_t k = std::min(v.size() - 1, (size_t)(q * (double)(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

// Main thread (la pompe). svsTime est lu ici pour que chaque fenetre dise
// d'elle-meme si le monde avancait : c'est le critere « en jeu », sans avoir a
// recoudre deux cadences d'impression differentes apres coup.
static void present_report() {
    static double t0 = 0.0;
    const double now = CACurrentMediaTime();
    if (t0 == 0.0) { t0 = now; return; }
    const double fen = now - t0;
    if (fen < 2.0) return;
    uint32_t so, sn, af, an, na;
    std::vector<double> iv;
    {   std::lock_guard<std::mutex> lk(g_pst.mu);
        so = g_pst.soumis; sn = g_pst.soumis_neufs; af = g_pst.affiches;
        an = g_pst.affiches_neufs; na = g_pst.non_affiches;
        iv.swap(g_pst.iv_ms);
        g_pst.soumis = g_pst.soumis_neufs = g_pst.affiches = 0;
        g_pst.affiches_neufs = g_pst.non_affiches = 0; }
    g_pst_cumul_neufs += sn;
    g_pst_cumul_affiches += af;
    const size_t niv = iv.size();
    const double med = present_quantile(iv, 0.50), p95 = present_quantile(iv, 0.95), ivmax = present_quantile(iv, 1.0);
    g_smc_cpu_c = smc_lire("TCMz");
    g_smc_ventilo = smc_lire("F0Ac");
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    // Memoire : `rss=` de [CADENCE] sous-estime d'un facteur ~9 ce que voit Jetsam.
    // phys_footprint EST le chiffre de Jetsam (le « App » du HUD Metal) ;
    // currentAllocatedSize est la part Metal (le « Metal » du HUD). Le HUD lisait
    // 14,25 Go / 12,82 Go sur pz2_afghanistan, sur une machine de 16 Go.
    unsigned long long empreinte_mo = 0;
    {   task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS)
            empreinte_mo = (unsigned long long)(vi.phys_footprint >> 20); }
    const unsigned long long metal_mo =
        g_device ? (unsigned long long)([g_device currentAllocatedSize] >> 20) : 0ULL;
    size_t attente;
    { std::lock_guard<std::mutex> lk(g_texreg_mu); attente = g_texrel_pending.size(); }
    fprintf(stderr, "[PRESENT] hote=%.3f unix=%.3f fen=%.3fs soumis=%u soumis_neufs=%u "
                    "affiches=%u affiches_neufs=%u non_affiches=%u img/s=%.2f aff/s=%.2f "
                    "iv_med=%.1fms iv_p95=%.1fms iv_n=%zu svsTime=%u dessins=%zu "
                    "metal=%lluMo empreinte=%lluMo cumul_neufs=%llu cumul_affiches=%llu "
                    "texreg=%zu tex_rendues=%llu tex_attente=%zu tex_tard=%llu iv_max=%.1fms pipes=%u pipe_ms=%.1f "
                    "msl_ms=%.1f cb_vides=%llu cpu_c=%.1f ventilo=%.0f visible=%d actif=%d\n",
            now, (double)tv.tv_sec + (double)tv.tv_usec / 1e6, fen, so, sn, af, an, na,
            (double)sn / fen, (double)af / fen, med, p95, niv,
            win32_mem_read32(0x02c6f294u), g_hud_n, metal_mo, empreinte_mo,
            (unsigned long long)g_pst_cumul_neufs, (unsigned long long)g_pst_cumul_affiches,
            g_texreg.size(), (unsigned long long)g_texrel_rendues, attente,
            (unsigned long long)g_texrel_tard, ivmax, g_win_pipes, g_win_pipe_ms, g_win_msl_ms,
            (unsigned long long)g_cb_vides, g_smc_cpu_c, g_smc_ventilo, g_fen_visible.load(), g_app_active.load());
    g_win_pipes = 0; g_win_pipe_ms = g_win_msl_ms = 0;
    {   // Z-NATIF-20 : pochoir et ciseau de la fenetre. Les fenetres 1, 2, 4, 8… sortent meme a zero.
        static uint64_t nfen = 0;
        ++nfen;
        const bool echo = (nfen & (nfen - 1)) == 0;
        bool actif = false;
        for (uint64_t v : g_st_n) actif |= (v != 0);
        if (actif || echo)
            fprintf(stderr, "[POCHOIR] fen #%llu : emis=%llu (ecrivent=%llu) sautes=%llu passes=%llu effacements "
                            "recus=%llu appliques=%llu coupes_effacement=%llu coupes_plan=%llu sans_attache=%llu "
                            "tampons=%zu cb_erreurs=%llu\n", (unsigned long long)nfen,
                    (unsigned long long)g_st_n[kStEmis], (unsigned long long)g_st_n[kStEcrivent],
                    (unsigned long long)g_st_n[kStSautes], (unsigned long long)g_st_n[kStPasses],
                    (unsigned long long)g_st_n[kStEffRecus], (unsigned long long)g_st_n[kStEffacees],
                    (unsigned long long)g_st_n[kStCoupeEff], (unsigned long long)g_st_n[kStCoupeDsv],
                    (unsigned long long)g_st_n[kStSansAttache], g_pochoir.size(),
                    (unsigned long long)g_cb_erreurs.load());
        if (g_sc_n[kScRognes] || g_sc_n[kScVides] || g_sc_n[kScVpAutre] || g_sc_n[kScVpPlage] ||
            g_sc_n[kScVpReposes] || g_sc_n[kScCull] || echo)
            fprintf(stderr, "[CISEAU] fen #%llu : dessins a ciseau=%llu dont rectangle plus petit que le viewport=%llu "
                            "(a pochoir=%llu) entierement rognes=%llu ; viewport autre que celui pose : rectangle=%llu "
                            "(ciseau non applique) plage de profondeur seule=%llu ; viewports reposes=%llu ; "
                            "dessins a face cachee=%llu\n",
                    (unsigned long long)nfen, (unsigned long long)g_sc_n[kScDessins],
                    (unsigned long long)g_sc_n[kScRognes], (unsigned long long)g_sc_n[kScRognesPochoir],
                    (unsigned long long)g_sc_n[kScVides], (unsigned long long)g_sc_n[kScVpAutre],
                    (unsigned long long)g_sc_n[kScVpPlage], (unsigned long long)g_sc_n[kScVpReposes],
                    (unsigned long long)g_sc_n[kScCull]);
        if (g_dd_mode || g_dd_n[kDdBiaisSans] || g_dd_n[kDdBiaisCoul] || g_dd_n[kDdPince]) {   // one line per window, and it can say ZERO
            bool dd_actif = false; std::string par, cibles;
            for (auto& kv : g_dd_lus_cible) { char b[48]; snprintf(b, sizeof b, " %08x=%llu", kv.first, (unsigned long long)kv.second); cibles += b; }
            g_dd_lus_cible.clear();
            for (uint64_t v : g_dd_n) dd_actif |= (v != 0);
            for (auto& kv : g_prof_dsv) {
                ProfDsv& p = kv.second; char b[256];
                dd_actif |= (p.n_sans | p.n_coul | p.n_lus | p.n_eff_recus) != 0;
                if (p.tex) snprintf(b, sizeof b, " %08x %lux%lu sans_couleur=%llu (dessins emis=%llu) couleur=%llu effacements recus=%llu "
                                                 "appliques=%llu depth2d_servis=%llu ;", kv.first, (unsigned long)p.tex.width,
                                    (unsigned long)p.tex.height, (unsigned long long)p.n_sans, (unsigned long long)p.n_emis,
                                    (unsigned long long)p.n_coul, (unsigned long long)p.n_eff_recus, (unsigned long long)p.n_eff_faits,
                                    (unsigned long long)p.n_lus);
                else snprintf(b, sizeof b, " %08x JAMAIS attachee, effacements recus=%llu ;", kv.first, (unsigned long long)p.n_eff_recus);
                par += b;
                p.n_sans = p.n_coul = p.n_emis = p.n_lus = p.n_eff_recus = p.n_eff_faits = 0;
            }
            char fn[200]; int o = 0;
            for (int i = 1; i <= 8; i++) o += snprintf(fn + o, sizeof fn - (size_t)o, "%s%llu", i > 1 ? "/" : "", (unsigned long long)g_dd_func[i]);
            if (dd_actif || echo)
                fprintf(stderr, "[DEPTH-DSV] fen #%llu : coupes dsv=%llu effacement=%llu copie=%llu taille_differente=%llu | effacements seuls=%llu | "
                                "images publiees jamais encodees=%llu | dessins ecran avec test de profondeur=%llu (ressource %08x) | "
                                "fonction de profondeur des dessins couleur, 1..8=%s | depth2d lues dans la passe qui l'attache=%llu "
                                "par un texture2d=%llu (ressource %08x, cible %08x) | lectures servies par cible :%s dont hors plage de la scene=%llu "
                                "(%g..%g) | dessins avec biais : "
                                "sans couleur=%llu couleur=%llu, profondeur pincee=%llu | par ressource :%s\n",
                        (unsigned long long)nfen, (unsigned long long)g_dd_n[kDdCoupe], (unsigned long long)g_dd_n[kDdCoupeEff],
                        (unsigned long long)g_dd_n[kDdCopie], (unsigned long long)g_dd_n[kDdTaille], (unsigned long long)g_dd_n[kDdEffSeul],
                        (unsigned long long)g_dd_n[kDdImgPerdues], (unsigned long long)g_dd_n[kDdEcranZ], g_dd_ecranz_res, fn,
                        (unsigned long long)g_dd_n[kDdLuAttache], (unsigned long long)g_dd_n[kDdLuTex2d], g_dd_tex2d_res, g_dd_tex2d_rtv,
                        cibles.empty() ? " aucune" : cibles.c_str(), (unsigned long long)g_dd_n[kDdLuVm], g_dd_vm_plage[0], g_dd_vm_plage[1],
                        (unsigned long long)g_dd_n[kDdBiaisSans],
                        (unsigned long long)g_dd_n[kDdBiaisCoul], (unsigned long long)g_dd_n[kDdPince], par.empty() ? " AUCUNE" : par.c_str());
            memset(g_dd_n, 0, sizeof g_dd_n); memset(g_dd_func, 0, sizeof g_dd_func);
            g_dd_ecranz_res = g_dd_tex2d_res = g_dd_tex2d_rtv = 0; g_dd_vm_plage[0] = g_dd_vm_plage[1] = 0.f;
        }
        memset(g_st_n, 0, sizeof g_st_n); memset(g_sc_n, 0, sizeof g_sc_n);
    }
    t0 = now;
}

// One pass of: drain pending AppKit events, then clear+present one frame.
// MUST be called from the real process main thread. Returns false once the user
// closes the window, so the caller can stop pumping.
static uint64_t g_drawables_nil = 0;   // P774 : nextDrawable sans drawable (fenêtre réduite ?)
extern "C" bool gfx_pump_once() {
    if (!g_open.load()) return false;
    keypipe_start_once();
    clickpipe_start_once();
    @autoreleasepool {
        // Non-blocking event drain — the guest is running on the worker thread and
        // must never be stalled waiting on the UI.
        for (;;) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (!e) break;
            {   // C16 : tenir / rendre le pointeur (avant in_translate, qui consomme ⌘).
                const NSEventType t = [e type];
                if ((t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown) &&
                    [e window] == g_window &&      // la zone de jeu, pas la barre de titre
                    NSPointInRect([e locationInWindow], [[g_window contentView] frame]))
                    pointer_engage(true, "clic dans la fenetre");
                else if (t == NSEventTypeFlagsChanged &&
                         ([e modifierFlags] & NSEventModifierFlagCommand))
                    pointer_engage(false, "touche commande");
            }
            // Key events are consumed here: passing an unhandled keyDown on to
            // AppKit makes the system beep on every keypress.
            if (!in_translate(e)) [NSApp sendEvent:e];
        }
        switch (g_fenetre_cmd.exchange(0)) {   // P774 : commandes de fenêtre du tube clavier
            case 'd': [g_window miniaturize:nil]; break;
            case 's': [NSApp hide:nil]; break;
            case 'n': [NSApp unhide:nil]; [g_window deminiaturize:nil]; break;
        }
        // P774 : réduite (Dock) ou app masquée (⌘H), isVisible répond NO aussi ; sous Windows le
        // jeu continue et revient à l'écran. Seule la fermeture arrête la pompe.
        if (![g_window isVisible] && ![g_window isMiniaturized] && ![NSApp isHidden]) {
            pointer_engage(false, "fenetre fermee");   // the pump stops here: nothing would give it back later
            if (g_pointeur_cache) { g_pointeur_cache = false; [NSCursor unhide]; }
            g_open.store(false); return false;
        }
        if (static bool cachee = false; cachee != ![g_window isVisible]) {
            cachee = !cachee;
            fprintf(stderr, "[FENETRE] %s, la pompe continue (drawables_nil=%llu)\n",
                    !cachee ? "revenue" : [NSApp isHidden] ? "masquée" : "réduite",
                    (unsigned long long)g_drawables_nil);
        }
        {   // Focus changes become WM_ACTIVATEAPP/WM_ACTIVATE/WM_SETFOCUS. The
            // engine's input frame refuses to poll anything until it believes it
            // is the foreground window, so this is not cosmetic.
            //
            // ⚠️ AND THAT MAKES A CLICK IN ANOTHER APP LOOK EXACTLY LIKE DEAD
            // KEYBOARD INPUT. The runtime asserts focus exactly once, with the
            // activateIgnoringOtherApps: at window creation, and nothing ever
            // re-asserts it. If anything else becomes frontmost during a run —
            // the user clicking into a browser, on their own desk, during a
            // fifteen-minute measurement — every later keystroke is delivered to
            // the process, translated by in_translate, pushed onto the queue,
            // and then ignored by the guest, in silence.
            //
            // It cannot be fixed from outside the process on this OS:
            // NSRunningApplication activateWithOptions: returns YES and does
            // nothing (NSApplicationActivateIgnoringOtherApps is deprecated and
            // inert since macOS 14), and `System Events … set frontmost` returns
            // without error and leaves the other app frontmost. Both measured.
            //
            // So: SAY IT, ALWAYS. One line per transition, no env gate — a
            // transition is rare and the absence of this line is itself the
            // evidence that focus never moved.
            bool key = [g_window isKeyWindow] != NO;
            // ⚠️ PUBLISH THE **REAL** KEY STATE, BEFORE BO2_FORCE_FOCUS masks it.
            // The cursor recentring in user32_impl.cpp must obey the window
            // server, not the forced-foreground lie we tell the guest: the tester had
            // to KILL the game twice to get his pointer back, because the warp
            // kept firing while he was typing in another app. Forced focus is for
            // the GUEST's activation messages; it must never hold the user's mouse.
            g_window_really_key.store(key, std::memory_order_relaxed);
            if (!key) pointer_engage(false, "fenetre plus au premier plan");   // C16
            {
                const NSRect r = [g_window convertRectToScreen:[[g_window contentView] frame]];
                g_centre_x.store(floor(NSMidX(r)), std::memory_order_relaxed);
                g_centre_y.store(floor(CGDisplayBounds(CGMainDisplayID()).size.height - NSMidY(r)),
                                 std::memory_order_relaxed);
                g_centre_t.store(CACurrentMediaTime(), std::memory_order_relaxed);
                // The pointer is hidden while the game keeps putting it back in the middle (mouse look), and
                // shown again 0.5 s after it stops (menus) or as soon as it is given back. The guest's
                // ShowCursor count cannot decide this: it flips between 0 and -1 at frame rate in the menus
                // ([CURSOR] ShowCursor, run 011331).
                const bool veut = key && gfx_pointer_engaged() &&
                    CACurrentMediaTime() - g_pointeur_fixe_t.load(std::memory_order_relaxed) < 0.5;
                if (veut != g_pointeur_cache) {
                    g_pointeur_cache = veut;
                    if (veut) [NSCursor hide]; else [NSCursor unhide];
                    static uint64_t n = 0;
                    if (++n <= 8 || (n & (n - 1)) == 0)
                        fprintf(stderr, "[CURSOR] pointeur %s (#%llu)\n", veut ? "cache : le jeu le fixe au centre" : "montre",
                                (unsigned long long)n);
                }
            }
            // BO2_FORCE_FOCUS=1 — tell the guest it is foreground no matter what
            // the window server thinks. This is for DRIVEN runs: it lets the
            // machine be used while a measurement is in flight instead of
            // requiring nobody to touch the Mac for fifteen minutes. It changes
            // only the activation messages, never the key events themselves.
            // Value-checked, not presence-checked: mp_run.sh sets this by
            // default, so BO2_FORCE_FOCUS=0 has to be a real way out.
            static const char* fs = bo2_getenv("BO2_FORCE_FOCUS");
            static const int force = (fs && fs[0] && fs[0] != '0') ? 1 : 0;
            if (force) key = true;
            // ⚠️ REPORT THE INITIAL STATE, NOT ONLY THE TRANSITIONS. g_in_focus
            // starts false, so a window that is NEVER key produces no transition
            // and therefore no line at all — and "no [FOCUS] line" would read as
            // "focus was fine". That is precisely the run this was written for:
            // activateIgnoringOtherApps: at window creation is inert on macOS 14+,
            // so if another app is frontmost when the game starts, the guest
            // spends the entire run in background mode and nothing says so.
            static bool announced = false;
            if (!announced) {
                announced = true;
                fprintf(stderr, "[FOCUS] initial state: window is %s%s\n",
                        key ? "KEY (foreground)" : "NOT key — THE GUEST WILL IGNORE INPUT",
                        force ? " [BO2_FORCE_FOCUS]" : "");
            }
            if (key != g_in_focus) {
                g_in_focus = key;
                fprintf(stderr, "[FOCUS] window %s foreground%s\n",
                        key ? "GAINED" : "LOST", force ? " (forced)" : "");
                in_push(0x001C, key ? 1 : 0, 0);              // WM_ACTIVATEAPP
                in_push(0x0006, key ? 1 : 0, 0);              // WM_ACTIVATE
                in_push(key ? 0x0007 : 0x0008, 0, 0);         // WM_SETFOCUS/KILLFOCUS
            }
        }

        // Promote any texture the guest staged into a real MTLTexture (main
        // thread only).
        {
            std::lock_guard<std::mutex> lk(g_tex_mu);
            if (g_pending.ready) {
                g_pending.ready = false;
                MTLPixelFormat pf = dxgi_to_metal(g_pending.fmt);
                MTLTextureDescriptor* td = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:pf
                                                 width:g_pending.w
                                                height:g_pending.h
                                             mipmapped:NO];
                id<MTLTexture> t = [g_device newTextureWithDescriptor:td];
                if (t) {
                    // Block-compressed rows cover 4 texels; the guest's pitch is
                    // already in bytes-per-row for whichever layout it used.
                    NSUInteger rows = is_block_compressed(pf) ? (g_pending.h + 3) / 4
                                                              : g_pending.h;
                    NSUInteger bpr  = g_pending.pitch;
                    if (bpr && (uint64_t)bpr * rows <= g_pending.bytes.size()) {
                        [t replaceRegion:MTLRegionMake2D(0, 0, g_pending.w, g_pending.h)
                             mipmapLevel:0
                               withBytes:g_pending.bytes.data()
                             bytesPerRow:bpr];
                        g_show = t;
                        fprintf(stderr, "[gfx] displaying %ux%u texture\n",
                                g_pending.w, g_pending.h);
                    }
                }
            }
        }

        // KEEP drawableSize IN STEP WITH THE VIEW, OR FULL SCREEN STRETCHES.
        // drawableSize was set once at window creation and never again, so on a
        // resize — and above all on full screen — AppKit simply scaled the layer's
        // contents to the new frame. Metal kept rendering at the original size and
        // CoreAnimation stretched the result, which is exactly what the tester saw: the
        // picture distorted instead of gaining black bars. The letterbox viewport
        // added alongside this could never fire, because it compares against a
        // drawableSize that never changed. Re-sync it here, once per frame, which is
        // also the only place guaranteed to run on the render thread.
        if (NSView* v = [g_window contentView]) {
            const CGFloat sc = g_layer.contentsScale > 0 ? g_layer.contentsScale : 1;
            // 10-21 : l'image a la taille du tampon arriere du JEU, pas celle de la fenetre ; CoreAnimation
            // l'agrandit en gardant les proportions. A la taille de la fenetre (2560x1440 Retina pour un jeu
            // en 1920x1080), les shaders qui lisent une cible d'apres la position du pixel (profondeur du
            // reticule, scanner, copie d'ecran) lisaient au mauvais endroit. BO2_DRAWABLE_FENETRE=1 : l'ancien.
            static const bool fenetre = bo2_env_on("BO2_DRAWABLE_FENETRE", false);
            if (!fenetre) g_layer.contentsGravity = kCAGravityResizeAspect;
            const CGSize want = fenetre ? CGSizeMake(v.bounds.size.width * sc, v.bounds.size.height * sc)
                                        : CGSizeMake(g_guest_bb_w, g_guest_bb_h);
            // BO2_CLICKPIPE reads these to turn fractional coordinates into client
            // pixels. Published from here because this is the one place that is
            // both on the real main thread (where AppKit geometry may be read) and
            // guaranteed to run every frame, so they track a resize.
            g_fen_visible.store(([g_window occlusionState] & NSWindowOcclusionStateVisible) ? 1 : 0);
            g_app_active.store([NSApp isActive] ? 1 : 0);
            g_client_w.store((int)v.bounds.size.width);
            g_client_h.store((int)v.bounds.size.height);
            if (want.width > 0 && want.height > 0 &&
                (fabs(want.width  - g_layer.drawableSize.width)  > 0.5 ||
                 fabs(want.height - g_layer.drawableSize.height) > 0.5)) {
                g_layer.drawableSize = want;
                fprintf(stderr, "[gfx] drawable resized to %.0fx%.0f\n",
                        want.width, want.height);
            }
        }
        id<CAMetalDrawable> drawable = [g_layer nextDrawable];
        if (!drawable) { g_drawables_nil++; return true; }   // transient; try again next pump

        g_frames.fetch_add(1);
        g_present_fresh = false;      // [PRESENT] : pose a vrai plus bas seulement si l'image est neuve

        // 🪤 THIS PASS USED TO CLEAR, AND IT IS ENCODED *AFTER* THE ENGINE'S OWN
        // PASSES — so every frame the engine drew to the drawable was wiped by us,
        // and the window showed nothing but this clear colour. That flat dark fill
        // was OUR paint, not the engine failing to draw. Same shape as the debug
        // quad that fooled me earlier: a leftover step-1 presentation path sitting
        // on top of the real implementation. It now LOADS, and the drawable gets
        // its initial clear in its own pass before anything else.
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture     = [drawable texture];
        rp.colorAttachments[0].loadAction  = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer>        cb  = [g_queue commandBuffer];
        {   // Initial clear of the drawable, first thing in the command buffer.
            MTLRenderPassDescriptor* c0 = [MTLRenderPassDescriptor renderPassDescriptor];
            c0.colorAttachments[0].texture     = [drawable texture];
            c0.colorAttachments[0].loadAction  = MTLLoadActionClear;
            c0.colorAttachments[0].storeAction = MTLStoreActionStore;
            // BO2_CLEAR_RED=1 — THE ONE BISECT THAT SEPARATES TWO STORIES.
            // The frontend captures as PURE BLACK (0,0,0 measured off the PNG),
            // NOT this clear colour (which is 13,13,18 in 8-bit). Two readings
            // fit that equally well: (a) the engine's 1812 screen draws land and
            // paint black over the clear, or (b) nothing this pass encodes ever
            // reaches the window at all. They need opposite fixes, and I was
            // about to assume (a). A bright clear tells them apart in one run: if
            // the window is RED, presentation works and the black is content; if
            // it stays black, the drawable is not reaching the screen and every
            // conclusion about draws is moot.
            static const char* cr = bo2_getenv("BO2_CLEAR_RED");
            const bool red = cr && cr[0] && cr[0] != '0';
            c0.colorAttachments[0].clearColor  = red
                ? MTLClearColorMake(1.0, 0.0, 0.0, 1.0)
                : MTLClearColorMake(0.05, 0.05, 0.07, 1.0);
            [[cb renderCommandEncoderWithDescriptor:c0] endEncoding];
        }
        // --- BO2's own shaders, one Metal render pass per render target -------
        // The frontend renders its menu into offscreen targets and composites
        // them, so a single pass to the drawable can never show it. Walk the
        // frame's draws in submission order and start a new pass whenever the
        // bound target changes; the composite draw then samples the offscreen
        // texture the earlier passes filled, because g_texreg is keyed by the
        // guest resource id that both the RTV and the SRV name.
        {
            // KEEP THE LAST FRAME when the guest has not finished a new one. This
            // pump runs at display rate; the guest presents far slower. Every pump
            // iteration that found an empty list used to clear the drawable and
            // present nothing — so the window alternated between the engine's
            // picture and the clear colour, which is exactly the black/magenta
            // flashing the diagnostic showed. Re-encoding the last frame costs a
            // few hundred draws and makes the image stable.
            // C35 : l'image encodee garde son arene jusqu'a l'arrivee de la suivante ;
            // la precedente repart, videe mais avec ses morceaux, vers la construction.
            static RdFrame draws_f;
            std::vector<RealDrawRec>& draws = draws_f.recs;
            bool fresh = false;
            { std::lock_guard<std::mutex> lk(g_rd_mu);
              if (!g_rd_frame.recs.empty()) { draws_f.clear(); draws_f.swap(g_rd_frame); fresh = true;
                                         g_draws_seq.store(g_rd_frame_seq);
                                         // BO2_DEPTH_DSV witness: depth now persists across frames, D3D11 never skips one, we do.
                                         static uint64_t seq_prise = 0;
                                         if (seq_prise && g_rd_frame_seq > seq_prise + 1)
                                             g_dd_n[kDdImgPerdues] += g_rd_frame_seq - seq_prise - 1;
                                         seq_prise = g_rd_frame_seq; } }
            g_present_fresh = fresh;   // [PRESENT] : image NEUVE ou re-encodage de la precedente
            // BO2_PHOTO_TRACES : la derniere image neuve QUI DESSINE LE MONDE (> 100 tracés) ; les images
            // d'interface seule (13-26 tracés, menus) s'intercalent et cachaient la chaine du monde.
            g_photo_rec = fresh && draws.size() > 100;
            if (g_photo_rec) g_photo_tr.clear();
            // 🎯 DO NOT RE-ENCODE A STALE FRAME EVERY SINGLE PUMP.
            //
            // Measured with `sample` on a stalled angola: the main thread spends
            // 843 of 1215 samples in win32_run_main_thread, 779 of them inside
            // gfx_pump_once, and 228 in upload()'s memmove. The guest coroutines
            // run on THIS SAME THREAD (the scheduler is cooperative ucontext), so
            // a pump that re-encodes ~1,900 draws at display rate starves them:
            // the guest never finishes a frame, the frame stays stale, the pump
            // re-encodes it again. **The starvation sustains itself**, and from
            // outside it looks exactly like a hang — guestFrames pinned, every
            // [CADENCE] counter +0, most threads reading RUN.
            //
            // Keeping the last frame is still right (an empty list used to clear
            // and present nothing, which flashed). But it does not have to be done
            // sixty times a second. Re-encode a stale frame only every Nth pump
            // and give the rest of the time back to the guest.
            static const long repump = [] {
                const char* e = bo2_getenv("BO2_REPUMP_EVERY");
                return e ? atol(e) : 16L;     // 0 or 1 = old behaviour ; 8 jusqu'au 26/10, 16 depuis P944 (+5 %, 3/3)
            }();
            if (!fresh && repump > 1) {
                static long skip = 0;
                if (++skip % repump) {
                    static uint64_t nskip = 0;
                    if (++nskip == 1 || (nskip % 20000) == 0)
                        fprintf(stderr, "[REPUMP] stale frame: skipped %llu re-encodes "
                                        "(1 in %ld kept) — this time goes back to the guest\n",
                                (unsigned long long)nskip, repump);
                    return true;    // the pump ran; it simply had nothing new to draw
                }
            }
        // P777 : ressources AVANT l'encodage. Ces trois blocs tournaient apres la boucle des dessins :
        // une texture, un shader ou une cible crees pour l'image N n'existaient pas quand N etait
        // encodee (texture noire, dessin saute, cible absente) — un flash noir par ressource neuve, et
        // une image sur deux noire dans les cinematiques (une texture neuve par image video, P776).
        // Apres la prise de l'image : tout ce que l'invite a cree avant de la finir est deja en attente.
        // Promote any textures the guest registered, then draw the UI frame on
        // top. Both must happen on this thread — MTLTexture/MTLBuffer creation
        // is never safe from the guest coroutines.
        {
            std::vector<std::pair<uint32_t, TexStage>> take;
            { std::lock_guard<std::mutex> lk(g_texreg_mu); take.swap(g_texreg_pending); }
            for (auto& kv : take) {
                // A 3D/cube volume is REWRITTEN every frame (the lighting volume
                // is copied in per frame), so "already registered" must not mean
                // "never update again" for those — only immutable 2D images are
                // upload-once.
                if (g_texreg.count(kv.first) &&
                    kv.second.depth == 1 && kv.second.faces == 1) continue;
                MTLPixelFormat pf = dxgi_to_metal(kv.second.fmt);
                // 🎯 DECODE A LARGE BLOCK-COMPRESSED TEXTURE TO RGBA8 so it can
                // have a mip chain. Metal will not generate mips for a compressed
                // format, and these are precisely the rock and terrain surfaces
                // whose streaking is visible. The census says the game uses BC1
                // (178), BC2 (75), BC3 (287) and BC5 (41) — and no BC7 — so the
                // decode is a few dozen lines rather than a project.
                //
                // Bounded to >= kBcMipMin on a side: small UI atlases never
                // minify, so paying 4-8x memory for them would be waste. Off by
                // default until it has been looked at on screen (BO2_BCMIPS=1).
                std::vector<uint8_t> bc_rgba;
                if (bo2_bcmips_on() && kv.second.faces == 1 && kv.second.depth <= 1 &&
                    is_block_compressed(pf) &&
                    kv.second.w >= kBcMipMin && kv.second.h >= kBcMipMin &&
                    bo2_bc_decode(kv.second.fmt, kv.second.bytes.data(),
                                  kv.second.bytes.size(), kv.second.w, kv.second.h, bc_rgba)) {
                    kv.second.bytes.swap(bc_rgba);
                    kv.second.pitch = kv.second.w * 4;
                    kv.second.fmt   = 28;              // DXGI_FORMAT_R8G8B8A8_UNORM
                    pf = MTLPixelFormatRGBA8Unorm;
                    // 🎯 BO2_TEX_DUMP=<n> — write the first n decoded textures as
                    // PPM so the SOURCE can be looked at directly. Everything
                    // downstream has now been cleared (mips, pitch, sampler, the
                    // vertex-attribute mapping), so the question left is whether
                    // the streak is already in the bytes.
                    {
                        static const long ndump = [] {
                            const char* e = bo2_getenv("BO2_TEX_DUMP");
                            return e ? atol(e) : 0L;
                        }();
                        static long done = 0;
                        if (done < ndump && kv.second.w >= 256) {
                            char path[256];
                            snprintf(path, sizeof path, "runs/texdump_%08x_%ux%u.ppm",
                                     kv.first, kv.second.w, kv.second.h);
                            if (FILE* f = fopen(path, "wb")) {
                                fprintf(f, "P6\n%u %u\n255\n", kv.second.w, kv.second.h);
                                const uint8_t* src = kv.second.bytes.data();
                                for (size_t i = 0; i < (size_t)kv.second.w * kv.second.h; i++)
                                    fwrite(src + i * 4, 1, 3, f);
                                fclose(f);
                                ++done;
                                fprintf(stderr, "[TEX-DUMP] wrote %s\n", path);
                            }
                        }
                    }
                    static uint64_t nbc = 0;
                    if (++nbc <= 6 || (nbc % 200) == 0)
                        fprintf(stderr, "[BCMIPS] %08x %ux%u decoded to RGBA8 for a mip chain "
                                        "(#%llu)\n", kv.first, kv.second.w, kv.second.h,
                                (unsigned long long)nbc);
                }
                const TexStage& ts = kv.second;
                MTLTextureDescriptor* td = nil;
                if (ts.depth > 1) {
                    td = [[MTLTextureDescriptor alloc] init];
                    td.textureType = MTLTextureType3D;
                    td.pixelFormat = pf;
                    td.width = ts.w; td.height = ts.h; td.depth = ts.depth;
                } else if (ts.faces == 6 && ts.mips > 1) {
                    td = [MTLTextureDescriptor
                        textureCubeDescriptorWithPixelFormat:pf
                                                        size:ts.w
                                                   mipmapped:YES];
                    td.mipmapLevelCount = ts.mips;
                } else if (ts.faces == 6) {
                    // MIPMAPPED, and this is the whole difference between a
                    // reflection and a mirror. The material shader samples the
                    // probe with an EXPLICIT level:
                    //     reflectionProbeSampler.sample(s, dir, level(u_xlat3.z))
                    // i.e. a rougher surface asks for a blurrier mip. A cube
                    // created `mipmapped:NO` clamps every one of those lookups to
                    // level 0, so every surface in the room reflected a
                    // mirror-sharp sky — measured, scene mean 71 -> 149 and
                    // visibly a hall of mirrors.
                    // P773 : sauf en BC — Metal ne génère pas ses niveaux, ils restaient
                    // indéfinis et l'échantillonnage à niveau explicite lisait du rebut.
                    // Un niveau, comme sous Windows quand MipLevels=1.
                    td = [MTLTextureDescriptor
                        textureCubeDescriptorWithPixelFormat:pf
                                                        size:ts.w
                                                   mipmapped:!is_block_compressed(pf)];
                } else if (ts.mips > 1 && ts.faces == 1 &&
                           ts.mip_off.size() == ts.mips) {
                    // We have the guest's own chain — build a real mipmapped
                    // texture. mipmapped:YES alone would size the chain to the
                    // full pyramid; mipmapLevelCount pins it to what we actually
                    // have, so no level is ever left undefined.
                    td = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:pf
                                                     width:ts.w
                                                    height:ts.h
                                                 mipmapped:YES];
                    td.mipmapLevelCount = ts.mips;
                } else if (!is_block_compressed(pf) && ts.faces == 1 && ts.depth <= 1 &&
                           ts.w >= 8 && ts.h >= 8 && bo2_genmips_on()) {
                    // 🎯 NO TEXTURE IN THIS GAME ARRIVES WITH A MIP CHAIN, so
                    // build one. Measured on afghanistan: every one of ~2000
                    // CreateTexture2D calls declares MipLevels 0 or 1, no
                    // UpdateSubresource ever touches a 2D target, Map carries
                    // only video planes, and GenerateMips is never called — so
                    // grazing-angle surfaces sample level 0 forever and smear.
                    // That streaking is the artefact the tester photographed across
                    // angola's lower third.
                    //
                    // 1,419 of those 2,000 textures are UNCOMPRESSED, and for
                    // those a chain costs one blit call. (581 are BC, where
                    // Metal refuses and a real fix means
                    // decompress→downsample→recompress — left alone.)
                    td = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:pf
                                                     width:ts.w
                                                    height:ts.h
                                                 mipmapped:YES];
                    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                    { static uint64_t nd = 0;
                      if (++nd <= 4 || (nd % 500) == 0)
                          fprintf(stderr, "[GENMIPS-DESC] %08x %ux%u fmt=%u — asked for a full "
                                          "pyramid (#%llu)\n", kv.first, ts.w, ts.h, ts.fmt,
                                  (unsigned long long)nd); }
                } else {
                    td = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:pf
                                                     width:ts.w
                                                    height:ts.h
                                                 mipmapped:NO];
                }
                bool bo2_fill_mips_after_upload = false;
                id<MTLTexture> t = nil;
                {   // Reuse the existing MTLTexture when one is already registered
                    // with the same shape: making a new one every frame for a
                    // per-frame volume would leak a texture per frame.
                    auto ex = g_texreg.find(kv.first);
                    if (ex != g_texreg.end() && ex->second &&
                        [ex->second width] == ts.w && [ex->second height] == ts.h &&
                        [ex->second mipmapLevelCount] == (ts.mips > 1 ? ts.mips : 1))
                        t = ex->second;
                }
                if (!t) t = [g_device newTextureWithDescriptor:td];
                if (!t) continue;
                if (ts.depth > 1 || ts.faces == 6) {
                    // A 3D texture is one upload with a slice pitch; a cube is six
                    // uploads, one per face, laid out consecutively (D3D's array
                    // order +X -X +Y -Y +Z -Z is Metal's slice order too).
                    NSUInteger bpr = ts.pitch;
                    NSUInteger rows = is_block_compressed(pf) ? (ts.h + 3) / 4 : ts.h;
                    NSUInteger bpi = ts.slice_pitch ? ts.slice_pitch : bpr * rows;
                    NSUInteger nslices = ts.depth > 1 ? ts.depth : 6;
                    if (!bpr || (uint64_t)bpi * nslices > ts.bytes.size()) {
                        g_tex3d_short++;
                        continue;
                    }
                    if (ts.depth > 1) {
                        [t replaceRegion:MTLRegionMake3D(0, 0, 0, ts.w, ts.h, ts.depth)
                             mipmapLevel:0 slice:0
                               withBytes:ts.bytes.data()
                             bytesPerRow:bpr bytesPerImage:bpi];
                    } else if (ts.mips > 1 && ts.mip_off.size() == ts.mips * 6) {
                        // The guest's OWN prefiltered chain, face-major.
                        bool bad = false;
                        for (uint32_t f = 0; f < 6 && !bad; f++)
                            for (uint32_t m = 0; m < ts.mips && !bad; m++) {
                                uint32_t lw = ts.w >> m, lh = ts.h >> m;
                                if (!lw) lw = 1;
                                if (!lh) lh = 1;
                                uint32_t off = ts.mip_off[f * ts.mips + m];
                                uint32_t lp  = ts.mip_pitch[f * ts.mips + m];
                                uint32_t lrows = is_block_compressed(pf) ? (lh + 3) / 4 : lh;
                                if (!lp || (uint64_t)off + (uint64_t)lp * lrows >
                                           ts.bytes.size()) { bad = true; break; }
                                [t replaceRegion:MTLRegionMake2D(0, 0, lw, lh)
                                     mipmapLevel:m slice:f
                                       withBytes:ts.bytes.data() + off
                                     bytesPerRow:lp bytesPerImage:lp * lrows];
                            }
                        if (bad) { g_tex3d_short++; continue; }
                    } else {
                        for (NSUInteger f = 0; f < 6; f++)
                            [t replaceRegion:MTLRegionMake2D(0, 0, ts.w, ts.h)
                                 mipmapLevel:0 slice:f
                                   withBytes:ts.bytes.data() + f * bpi
                                 bytesPerRow:bpr bytesPerImage:bpi];
                    }
                    g_texreg[kv.first] = t;
                    g_img_uploaded++;
                    g_texinfo[kv.first] = TexInfo{ ts.w, ts.h, ts.fmt, {0,0,0,0} };
                    // Build the rest of the chain from level 0. The engine ships
                    // its own PREFILTERED mips and this is not them — a box filter
                    // is not a cosine-lobe convolution — so the roughness falloff
                    // is approximate. It is however the difference between "has a
                    // falloff" and "is a mirror", and uploading the guest's own
                    // mip chain (MipLevels x 6 subresources) is the faithful
                    // version to do next.
                    if (ts.faces == 6 && [t mipmapLevelCount] > 1 &&
                        !is_block_compressed(pf)) {
                        id<MTLCommandBuffer> mb = [g_queue commandBuffer];
                        id<MTLBlitCommandEncoder> mbe = [mb blitCommandEncoder];
                        [mbe generateMipmapsForTexture:t];
                        [mbe endEncoding];
                        [mb commit];
                    }
                    if (++g_tex3d_made <= 12)
                        fprintf(stderr, "[gfx] %s texture %08x %ux%ux%u fmt=%u mips=%u "
                                        "uploaded\n",
                                ts.depth > 1 ? "3D" : "CUBE", kv.first,
                                ts.w, ts.h, (unsigned)nslices, ts.fmt,
                                (unsigned)[t mipmapLevelCount]);
                    continue;
                }
                NSUInteger rows = is_block_compressed(pf) ? (kv.second.h + 3) / 4
                                                          : kv.second.h;
                NSUInteger bpr  = kv.second.pitch;
                // 🎯 DOES THE GUEST'S PITCH MATCH THE FORMAT? The tester's gameplay
                // screenshot shows crates whose texture SMEARS in diagonal
                // streaks — the signature of rows read at the wrong stride. A
                // pitch that is too LARGE fails the bounds test below and the
                // texture is skipped; a pitch that is too SMALL passes it and
                // uploads a sheared image, which is what streaking looks like.
                // Neither case says anything today.
                {
                    const NSUInteger want = expected_bpr(pf, kv.second.w);
                    if (want && bpr && bpr != want) {
                        static uint64_t nbad = 0;
                        if (++nbad <= 10 || (nbad % 500) == 0)
                            fprintf(stderr, "[TEX-PITCH] %08x %ux%u fmt=%u: guest pitch=%lu but "
                                            "the format wants %lu (%s) — rows are read at the "
                                            "wrong stride, which SHEARS the image (bad #%llu)\n",
                                    kv.first, kv.second.w, kv.second.h, kv.second.fmt,
                                    (unsigned long)bpr, (unsigned long)want,
                                    bpr < want ? "too SMALL: uploads sheared"
                                               : "too LARGE: upload will be skipped",
                                    (unsigned long long)nbad);
                    }
                }
                if (!(bpr && (uint64_t)bpr * rows <= kv.second.bytes.size())) {
                    // ⚠️ SILENT SKIP MADE LOUD. This branch used to just fall
                    // through: the texture was never uploaded, g_texreg never got
                    // an entry, and the shader sampled a stand-in — a rendering
                    // fault with no log line anywhere.
                    static uint64_t nskip = 0;
                    if (++nskip <= 10 || (nskip % 500) == 0)
                        fprintf(stderr, "[TEX-SKIP] %08x %ux%u fmt=%u pitch=%lu rows=%lu needs "
                                        "%llu bytes, has %zu — NOT UPLOADED (skip #%llu)\n",
                                kv.first, kv.second.w, kv.second.h, kv.second.fmt,
                                (unsigned long)bpr, (unsigned long)rows,
                                (unsigned long long)((uint64_t)bpr * rows),
                                kv.second.bytes.size(), (unsigned long long)nskip);
                }
                // 🎯 UPLOAD EVERY LEVEL WE WERE GIVEN, not just the sharpest.
                // A level left undefined is worse than no chain at all — the
                // sampler would read garbage as soon as a surface minifies — so
                // this walks the whole chain and bails loudly if any level does
                // not fit its buffer.
                if (kv.second.mips > 1 && kv.second.faces == 1 &&
                    kv.second.mip_off.size() == kv.second.mips &&
                    [t mipmapLevelCount] == kv.second.mips) {
                    bool bad = false;
                    for (uint32_t m = 0; m < kv.second.mips && !bad; m++) {
                        uint32_t lw = kv.second.w >> m, lh = kv.second.h >> m;
                        if (!lw) lw = 1;
                        if (!lh) lh = 1;
                        uint32_t off = kv.second.mip_off[m];
                        uint32_t lp  = kv.second.mip_pitch[m];
                        uint32_t lrows = is_block_compressed(pf) ? (lh + 3) / 4 : lh;
                        if (!lp || (uint64_t)off + (uint64_t)lp * lrows >
                                   kv.second.bytes.size()) { bad = true; break; }
                        [t replaceRegion:MTLRegionMake2D(0, 0, lw, lh)
                             mipmapLevel:m
                               withBytes:kv.second.bytes.data() + off
                             bytesPerRow:lp];
                    }
                    if (bad) {
                        static uint64_t nbadmip = 0;
                        if (++nbadmip <= 8)
                            fprintf(stderr, "[TEX-MIPS] %08x %ux%u levels=%u — a level does not "
                                            "fit its buffer; NOT uploaded rather than left with "
                                            "an undefined level (bad #%llu)\n",
                                    kv.first, kv.second.w, kv.second.h, kv.second.mips,
                                    (unsigned long long)nbadmip);
                    } else {
                        // 🎯 GENERATE THE CHAIN WE JUST ALLOCATED. The descriptor
                        // above asks for a full pyramid on uncompressed 2D
                        // textures, but a pyramid whose levels are never filled
                        // is WORSE than none — the sampler reads undefined data
                        // the moment a surface minifies, which is exactly how the
                        // green speckle appeared when anisotropy was switched on
                        // over chainless textures. So fill it here, in one blit.
                        if ([t mipmapLevelCount] > 1 && kv.second.mips <= 1 &&
                            !is_block_compressed([t pixelFormat])) {
                            id<MTLCommandBuffer> mb = [g_queue commandBuffer];
                            id<MTLBlitCommandEncoder> mbe = [mb blitCommandEncoder];
                            [mbe generateMipmapsForTexture:t];
                            [mbe endEncoding];
                            [mb commit];
                            static uint64_t ngen = 0;
                            if (++ngen <= 6 || (ngen % 500) == 0)
                                fprintf(stderr, "[GENMIPS] %08x %ux%u → %u levels generated "
                                                "(#%llu)\n", kv.first, kv.second.w,
                                        kv.second.h, (unsigned)[t mipmapLevelCount],
                                        (unsigned long long)ngen);
                        }
                        g_texreg[kv.first] = t;
                        g_img_uploaded++;
                        if (kv.second.w > 4 && kv.second.h > 4) g_img_art++;
                        TexInfo ti2{ kv.second.w, kv.second.h, kv.second.fmt, {0,0,0,0} };
                        for (int q = 0; q < 4 && q < (int)kv.second.bytes.size(); q++)
                            ti2.px[q] = kv.second.bytes[q];
                        g_texinfo[kv.first] = ti2;
                        static uint64_t nmipok = 0;
                        if (++nmipok <= 6)
                            fprintf(stderr, "[TEX-MIPS] %08x %ux%u fmt=%u: %u levels uploaded "
                                            "(was level 0 only — this is the streaking fix)\n",
                                    kv.first, kv.second.w, kv.second.h, kv.second.fmt,
                                    kv.second.mips);
                    }
                } else if (bpr && (uint64_t)bpr * rows <= kv.second.bytes.size()) {
                    // 🎯 IS THE ROW PITCH THE NATURAL ONE? Long smooth streaks
                    // across a surface are NOT what missing mips look like (that
                    // shimmers); they are what a WRONG PITCH looks like, because
                    // each row starts a little off from the last and the image
                    // shears. The "texture smearing = missing mipmaps" attribution
                    // from 7b9a5d3 was a hypothesis and was never checked against
                    // the pitch. Check it: compare what the guest handed us with
                    // the pitch the width implies, and say so when they differ.
                    {
                        const bool bc = is_block_compressed(pf);
                        const uint32_t blocks = bc ? (kv.second.w + 3) / 4 : kv.second.w;
                        // ⚠️ NOT EVERY UNCOMPRESSED FORMAT IS 4 BYTES. The first
                        // version assumed it and reported 12 "MISMATCH" lines,
                        // every one of them fmt=65 — DXGI A8_UNORM, ONE byte per
                        // pixel — whose pitch was correct all along. The port
                        // already knows this (dxgi_bytes_per_pixel: case 61/65
                        // return 1). A probe that invents a mismatch is worse
                        // than no probe: it nearly convicted the upload path.
                        const uint32_t bpe = bc ? ((pf == MTLPixelFormatBC1_RGBA ||
                                                    pf == MTLPixelFormatBC1_RGBA_sRGB ||
                                                    pf == MTLPixelFormatBC4_RUnorm) ? 8u : 16u)
                                                : (kv.second.fmt == 61 || kv.second.fmt == 65 ? 1u
                                                 : kv.second.fmt == 56 || kv.second.fmt == 57 ? 2u
                                                 : 4u);
                        const uint64_t natural = (uint64_t)blocks * bpe;
                        if (kv.second.w >= 256 && (uint64_t)bpr != natural) {
                            static uint64_t nmis = 0;
                            if (++nmis <= 10 || (nmis % 500) == 0)
                                fprintf(stderr, "[TEX-PITCH] %08x %ux%u fmt=%u bc=%d: guest pitch "
                                                "%lu vs natural %llu — MISMATCH (#%llu)\n",
                                        kv.first, kv.second.w, kv.second.h, kv.second.fmt,
                                        bc ? 1 : 0, (unsigned long)bpr,
                                        (unsigned long long)natural, (unsigned long long)nmis);
                        } else if (kv.second.w >= 256) {
                            static uint64_t nok = 0;
                            if (++nok <= 3)
                                fprintf(stderr, "[TEX-PITCH] %08x %ux%u fmt=%u: pitch %lu matches "
                                                "the natural pitch\n", kv.first, kv.second.w,
                                        kv.second.h, kv.second.fmt, (unsigned long)bpr);
                        }
                    }
                    // 🎯 THIS is the single-level path — the one every BO2 texture
                    // actually takes, since none of them ships a chain. The first
                    // version of this fix put the generateMipmaps call in the
                    // OTHER branch (the one that uploads a guest-supplied chain)
                    // and it fired 0 times while [GENMIPS-DESC] showed the
                    // pyramid being allocated 5 times. Allocating a pyramid and
                    // not filling it is worse than no pyramid at all.
                    bo2_fill_mips_after_upload = true;
                    [t replaceRegion:MTLRegionMake2D(0, 0, kv.second.w, kv.second.h)
                         mipmapLevel:0
                           withBytes:kv.second.bytes.data()
                         bytesPerRow:bpr];
                    if (bo2_fill_mips_after_upload && [t mipmapLevelCount] > 1 &&
                        !is_block_compressed([t pixelFormat])) {
                        id<MTLCommandBuffer> mb = [g_queue commandBuffer];
                        id<MTLBlitCommandEncoder> mbe = [mb blitCommandEncoder];
                        [mbe generateMipmapsForTexture:t];
                        [mbe endEncoding];
                        [mb commit];
                        static uint64_t ngen = 0;
                        if (++ngen <= 6 || (ngen % 500) == 0)
                            fprintf(stderr, "[GENMIPS] %08x %ux%u → %u levels generated (#%llu)\n",
                                    kv.first, kv.second.w, kv.second.h,
                                    (unsigned)[t mipmapLevelCount], (unsigned long long)ngen);
                    }
                    g_texreg[kv.first] = t;
                    g_img_uploaded++;
                    if (kv.second.w > 4 && kv.second.h > 4) g_img_art++;
                    TexInfo ti{ kv.second.w, kv.second.h, kv.second.fmt, {0,0,0,0} };
                    for (int q = 0; q < 4 && q < (int)kv.second.bytes.size(); q++)
                        ti.px[q] = kv.second.bytes[q];
                    g_texinfo[kv.first] = ti;
                    if (kv.second.w <= 2 && kv.second.h <= 2)
                        fprintf(stderr, "[gfx] solid texture %08x %ux%u fmt=%u "
                                        "texel=%u,%u,%u,%u\n", kv.first,
                                kv.second.w, kv.second.h, kv.second.fmt,
                                ti.px[0], ti.px[1], ti.px[2], ti.px[3]);
                }
            }
        }
        {   // Compile any staged MSL. Metal's compiler is only safe here, and a
            // failure must be visible: a silently dropped shader would look like
            // a rendering bug much later.
            std::vector<ShaderStage> take;
            std::vector<std::pair<uint32_t,std::string>> tslots, cbslots, tdims, smpslots;
            std::vector<std::pair<uint32_t,id<MTLSamplerState>>> smps;
            { std::lock_guard<std::mutex> lk(g_shader_mu);
              take.swap(g_shader_pending); tslots.swap(g_texslot_pending);
              cbslots.swap(g_cbslot_pending); tdims.swap(g_texdim_pending);
              smpslots.swap(g_smpslot_pending); smps.swap(g_sampler_pending); }
            for (auto& kv : smps) g_sampreg[kv.first] = kv.second;   // P805
            // Le lot en parallèle (3,85× sur 8 cœurs, P770) : le fil principal le compilait shader par shader.
            // D'abord le metallib (msl_cache.sh) ou la préchauffe ; le reste est compilé par lots (msl_compile), au
            // plus huit lots pour garder les cœurs occupés, au plus 32 sources par lot.
            std::vector<id<MTLFunction>> fns(take.size());
            std::vector<std::string> why(take.size());
            const ShaderStage* tk = take.data();
            __strong id<MTLFunction>* pf = fns.data();
            const double tm0 = CACurrentMediaTime();
            dispatch_apply(take.size(), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
                if (g_shader_fn.count(tk[i].obj)) return;
                id<MTLLibrary> l = [g_device newLibraryWithURL:[NSURL fileURLWithPath:msl_cache_path(tk[i].msl, "metallib")]
                                                         error:nil];
                if ((pf[i] = [l newFunctionWithName:@"xlatMtlMain"])) { g_msl_from_cache++; return; }
                if ((pf[i] = msl_warm_get(msl_fnv64(tk[i].msl)))) g_msl_from_warm++;
            });
            std::vector<size_t> at;
            std::vector<const std::string*> src;
            for (size_t i = 0; i < take.size(); i++)
                if (!fns[i] && !g_shader_fn.count(take[i].obj)) { at.push_back(i); src.push_back(&take[i].msl); }
            if (!src.empty()) {
                const size_t lot = std::min<size_t>((src.size() + 7) / 8, 32);
                std::vector<id<MTLFunction>> got(src.size());
                std::vector<std::string> gwhy(src.size());
                const std::string* const* ps = src.data();
                __strong id<MTLFunction>* pg = got.data();
                std::string* pw = gwhy.data();
                const size_t n = src.size();
                dispatch_apply((n + lot - 1) / lot, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t k) {
                    const size_t b = k * lot, m = std::min(lot, n - b);
                    msl_compile(ps + b, m, pg + b, pw + b);
                    NSFileManager* fm = [NSFileManager defaultManager];
                    for (size_t j = b; j < b + m; j++) {
                        if (!pg[j]) continue;
                        NSString* f = msl_cache_path(*ps[j], "msl");
                        [fm createDirectoryAtPath:[f stringByDeletingLastPathComponent]
                      withIntermediateDirectories:YES attributes:nil error:nil];
                        [@(ps[j]->c_str()) writeToFile:f atomically:YES encoding:NSUTF8StringEncoding error:nil];
                    }
                });
                for (size_t j = 0; j < n; j++) { fns[at[j]] = got[j]; why[at[j]] = gwhy[j]; }
            }
            g_win_msl_ms += (CACurrentMediaTime() - tm0) * 1000.0;
            for (size_t i = 0; i < take.size(); i++) {
                auto& st = take[i];
                if (g_shader_fn.count(st.obj)) continue;
                id<MTLFunction> fn = fns[i];
                if (fn) {
                    g_shader_fn[st.obj] = fn;
                    ShaderRec rec; rec.fn = fn; rec.msl_h = msl_fnv64(st.msl);
                    if (st.is_pixel)   // "    depth2d<float, access::sample > shadowmapSamplerSpot [[ texture(5) ]] ," on one line
                        for (size_t p = 0; (p = st.msl.find("depth2d<", p)) != std::string::npos; p += 8) {
                            const size_t q = st.msl.find("[[ texture(", p);
                            if (q == std::string::npos || q > st.msl.find('\n', p)) continue;
                            const unsigned ix = (unsigned)atoi(st.msl.c_str() + q + 11);
                            if (ix < kMaxPSTex) rec.tx_prof[ix] = true;
                        }
                    // attrs is "POSITION0=0,COLOR0=1,TEXCOORD0=2"
                    size_t i = 0;
                    while (i < st.attrs.size()) {
                        size_t eq = st.attrs.find('=', i);
                        if (eq == std::string::npos) break;
                        size_t cm = st.attrs.find(',', eq);
                        if (cm == std::string::npos) cm = st.attrs.size();
                        rec.attrs[st.attrs.substr(i, eq - i)] =
                            (uint32_t)atoi(st.attrs.substr(eq + 1, cm - eq - 1).c_str());
                        i = cm + 1;
                    }
                    g_shader_rec[st.obj] = rec;
                    if (++g_msl_compiled <= 3 || g_msl_compiled % 100 == 0)
                        fprintf(stderr, "[MSL] compiled %s obj=%08x (%llu ok, %llu failed, %llu du cache, %llu prechauffes)\n",
                                st.is_pixel ? "PS" : "VS", st.obj,
                                (unsigned long long)g_msl_compiled,
                                (unsigned long long)g_msl_compile_failed,
                                (unsigned long long)g_msl_from_cache.load(),
                                (unsigned long long)g_msl_from_warm.load());
                } else {
                    if (++g_msl_compile_failed <= 5)
                        fprintf(stderr, "[MSL] Metal REJECTED %s obj=%08x: %s\n",
                                st.is_pixel ? "PS" : "VS", st.obj,
                                why[i].empty() ? "no function" : why[i].c_str());
                }
            }
            // AFTER the records exist, not before: `g_shader_rec[obj] = rec` above
            // replaces the whole record, so applying the maps first silently threw
            // them away — which is why the texture mapping looked like it had no
            // effect when it was measured.
            auto apply = [](const std::vector<std::pair<uint32_t,std::string>>& src,
                            std::unordered_map<uint32_t,uint32_t> ShaderRec::* field) {
                for (const auto& kv : src) {
                    auto rit = g_shader_rec.find(kv.first);
                    if (rit == g_shader_rec.end()) continue;   // shader never compiled
                    rit->second.plat = false;                  // C49 : tables aplaties perimees
                    auto& m = rit->second.*field;
                    size_t i = 0;
                    while (i < kv.second.size()) {
                        size_t eq = kv.second.find('=', i);
                        if (eq == std::string::npos) break;
                        size_t cm = kv.second.find(',', eq);
                        if (cm == std::string::npos) cm = kv.second.size();
                        m[(uint32_t)atoi(kv.second.substr(i, eq - i).c_str())] =
                            (uint32_t)atoi(kv.second.substr(eq + 1, cm - eq - 1).c_str());
                        i = cm + 1;
                    }
                }
            };
            apply(tslots,  &ShaderRec::texslot);
            apply(cbslots, &ShaderRec::cbslot);
            apply(tdims,   &ShaderRec::texdim);
            apply(smpslots, &ShaderRec::smpslot);   // P805
        }
        std::vector<id<MTLTexture>> zero_me;
        {   // Promote render targets and RTV bindings (main thread only).
            std::vector<RTStage> rts;
            std::vector<std::pair<uint32_t,uint32_t>> rtvs, rtv_mips;
            {
                std::lock_guard<std::mutex> lk(g_rt_mu);
                rts.swap(g_rt_pending); rtvs.swap(g_rtv_pending); rtv_mips.swap(g_rtv_mip_pending);
            }
            for (auto& kv : rtvs) { g_rtv_res[kv.first] = kv.second; g_rtv_cibles.insert(kv.second); }
            for (auto& kv : rtv_mips) g_rtv_mip[kv.first] = kv.second;
            for (const RTStage& t : rts) {
                if (g_texreg.count(t.obj)) continue;
                MTLPixelFormat pf = dxgi_to_metal(t.fmt);
                if (pf == MTLPixelFormatInvalid) pf = MTLPixelFormatBGRA8Unorm;
                MTLTextureDescriptor* td = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:pf width:t.w height:t.h
                                             mipmapped:NO];
                // The guest's MipLevels (0 = the full chain): the multiplayer's blur renders each level
                // of its scene copy through a MipSlice RTV and reads it back through a one-level SRV.
                {   uint32_t full = 1;
                    for (uint32_t m = t.w > t.h ? t.w : t.h; m > 1; m >>= 1) ++full;
                    const uint32_t nm = t.mips == 0 || t.mips > full ? full : t.mips;
                    if (nm > 1) td.mipmapLevelCount = nm; }
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModePrivate;
                id<MTLTexture> tex = [g_device newTextureWithDescriptor:td];
                if (tex) {
                    // ZERO IT AT CREATION. A fresh MTLTexture's contents are
                    // undefined, and clearing only on first render leaves any
                    // target the engine never draws into full of garbage. The
                    // frontend's colour-grading LUT is exactly such a target, and
                    // the tone-map composite's whole output is a LUT sample — so
                    // an uninitialised LUT painted the entire window that garbage
                    // (it read as solid white).
                    // Cleared just below, before any draw of this frame can
                    // render into it (no encoder is open here, P777).
                    if (pf != MTLPixelFormatDepth32Float) {
                        zero_me.push_back(tex);
                        g_rt_cleared.insert(t.obj);
                    }
                    g_texreg[t.obj] = tex;
                    g_rt_list.push_back(t.obj);
                    static uint32_t nrt = 0;
                    if (++nrt <= 8 || tex.mipmapLevelCount > 1)
                        fprintf(stderr, "[gfx] render target %08x %ux%u fmt=%u mips=%lu\n",
                                t.obj, t.w, t.h, t.fmt, (unsigned long)tex.mipmapLevelCount);
                }
            }
        }
        for (id<MTLTexture> zt : zero_me)
            for (NSUInteger lv = 0; lv < zt.mipmapLevelCount; lv++) {
                MTLRenderPassDescriptor* z = [MTLRenderPassDescriptor renderPassDescriptor];
                z.colorAttachments[0].texture     = zt;
                z.colorAttachments[0].level       = lv;
                z.colorAttachments[0].loadAction  = MTLLoadActionClear;
                z.colorAttachments[0].storeAction = MTLStoreActionStore;
                z.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 0);
                [[cb renderCommandEncoderWithDescriptor:z] endEncoding];
            }
            ring_next();   // P785 : le jeu de tampons que le GPU a fini de lire
            g_idx_pool_off = 0;
            g_upl_pool_off = 0;
            ++g_upl_gen;   // C47
            c50_evincer();
            g_screen_idx.clear();
            // 🪤 THIS USED TO BE CLEARED BY THE GUEST THREAD, in gfx_real_frame_end,
            // while THIS thread was inserting into it 130 lines below — a plain
            // data race on a std::set with no lock on either side. clear() frees
            // every tree node while insert() is walking and linking them, so the
            // damage lands in the ALLOCATOR, not here: the next heap read hands
            // back a garbage pointer, and the one that crashed was depth_for()'s
            // `return it->second` — an ARC objc_retain on a bogus id, SIGSEGV
            // fault_addr=0x420 in gfx_pump_once, one run in five for weeks.
            // The set means "sizes already cleared in the frame being ENCODED",
            // and encoding happens here, once per pump — so this is also the only
            // place whose frame boundary it actually tracks. `draws` persists
            // across pumps (see above), so a re-encoded frame must clear again.
            g_depth_cleared.clear();
            size_t n = draws.size(), k = 0, n_screen = 0, n_pass = 0;
            g_draw_seq.store(0, std::memory_order_relaxed);
            g_flat_seq = 0;
            // Trigger the dump on CONTENT, not on a frame number: an early frame
            // has two screen draws and would have told me nothing. Wait for the
            // frame where the frontend is actually compositing its menu.
            static int legend_done = 0;
            size_t pre_screen = 0;
            for (size_t z = 0; z < n; z++)
                if (!draws[z].is_copy &&
                    ((draws[z].rtv == 0 && !draws[z].dsv_res) || draws[z].rtv == g_backbuffer_rtv_id))
                    pre_screen++;
            g_screen_total = (uint32_t)pre_screen;
            int last_screen = -1;
            // BO2_UI_SWEEP=<first cutoff>. Starting the walk near the region of
            // interest instead of at 1 turns a 6-minute sweep into seconds.
            static const uint32_t sweep = [] {
                const char* e2 = bo2_getenv("BO2_UI_SWEEP");
                return e2 ? (uint32_t)strtoul(e2, nullptr, 10) : 0u; }();
            // Advance the render-target sweep once per frame and ask for a dump.
            if (g_rtsweep_steps) {
                g_rtsweep_total = g_rtsweep_seen ? g_rtsweep_seen : g_rtsweep_total;
                g_rtsweep_seen = 0;
                uint32_t tot = g_rtsweep_total ? g_rtsweep_total : 1;
                g_rtsweep_cut = (uint32_t)((uint64_t)tot *
                                (g_rtsweep_step % (g_rtsweep_steps + 1)) / g_rtsweep_steps);
                if (g_rtsweep_cut > tot) g_rtsweep_cut = tot;
                g_rtsweep_step++;
                g_rt_dump_req.store(1);
            }
            static uint32_t sweep_step = 0;
            if (sweep && pre_screen > sweep) {
                uint32_t span = (uint32_t)pre_screen - sweep + 1;
                g_sweep_n = sweep + (sweep_step / 8) % span;
                sweep_step++;
            }
            g_legend_frame = (g_flat_mode >= 2 && !legend_done && pre_screen > 100);
            if (g_legend_frame) legend_done = 1;
            while (k < n) {
                if (draws[k].is_clear == 2) {     // ClearDepthStencilView(…, STENCIL, …) between two groups
                    pochoir_efface(draws[k].depth.st_res, draws[k].depth.st_ref);
                    ++g_st_n[kStEffRecus]; k++; continue;
                }
                if (draws[k].is_clear == 3) {     // ClearDepthStencilView(…, DEPTH, v) between two groups
                    prof_efface(draws[k].dsv_res, draws[k].clear_rgba[0], draws[k].dsv_wh); k++; continue;
                }
                if (draws[k].is_copy) {
                    photo_trace_mark(draws[k].copy_dst, draws[k].copy_dst == g_backbuffer_res_id, 0xC0B1, draws[k].copy_src);
                    // CopyResource: the engine's own "put this target on screen".
                    // Done as a fullscreen textured quad rather than a blit,
                    // because the source is RGBA8 and the drawable is BGRA8 and a
                    // blit between different formats fails.
                    id<MTLTexture> src = nil, dst = nil;
                    auto si = g_texreg.find(draws[k].copy_src);
                    if (si != g_texreg.end()) src = si->second;
                    // P806: the backbuffer is the drawable. The game copies it into
                    // ResolvedPostSun (scope lenses, refraction); unresolved, they read a stale picture.
                    const bool src_screen = (draws[k].copy_src == g_backbuffer_res_id);
                    if (src_screen) src = [drawable texture];
                    bool dst_screen = (draws[k].copy_dst == g_backbuffer_res_id);
                    if (!dst_screen) {
                        auto di = g_texreg.find(draws[k].copy_dst);
                        if (di != g_texreg.end()) dst = di->second;
                    }
                    id<MTLRenderPipelineState> cpipe =
                        dst_screen ? g_pipe : (dst ? copy_pipe_for(dst.pixelFormat) : nil);
                    if (src_screen && (++g_copie_ecran <= 3 || g_copie_ecran % 2000 == 0))
                        fprintf(stderr, "[COPIE-ECRAN] #%llu backbuffer -> %08x (format Metal %lu)%s\n",
                                (unsigned long long)g_copie_ecran, draws[k].copy_dst,
                                dst ? (unsigned long)dst.pixelFormat : 0ul, cpipe ? "" : " SANS PIPELINE");
                    if (src && (dst_screen || dst) && cpipe && !(src_screen && dst_screen)) {
                        MTLRenderPassDescriptor* cp = [MTLRenderPassDescriptor renderPassDescriptor];
                        cp.colorAttachments[0].texture     = dst_screen ? [drawable texture] : dst;
                        cp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
                        cp.colorAttachments[0].storeAction = MTLStoreActionStore;
                        id<MTLRenderCommandEncoder> ce = [cb renderCommandEncoderWithDescriptor:cp];
                        simd_float4 one = simd_make_float4(1.0f, 1.0f, 0.0f, 0.0f);
                        if (src_screen && g_guest_bb_w > 0.f && g_guest_bb_h > 0.f) {
                            // Plein ecran : l'image de l'invite n'occupe que le rectangle letterbox du
                            // drawable. Copier le drawable entier ecrasait l'image, bandes noires comprises
                            // (visee et pause en plein ecran, 2026-10-09). On agrandit le quad pour que ce
                            // rectangle seul couvre la cible ; fenetre 16:9 : facteur 1, rien ne change.
                            CGSize ds = g_layer.drawableSize;
                            const double sx = ds.width / (double)g_guest_bb_w, sy = ds.height / (double)g_guest_bb_h;
                            const double s  = (sx < sy) ? sx : sy;
                            one.x = (float)(sx / s); one.y = (float)(sy / s);
                        }
                        [ce setRenderPipelineState:cpipe];
                        [ce setVertexBytes:&one length:sizeof(one) atIndex:0];
                        [ce setFragmentTexture:src atIndex:0];
                        [ce setFragmentSamplerState:g_samp atIndex:0];
                        [ce drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                        [ce endEncoding];
                        // A COPY FILLS A TARGET JUST AS A DRAW DOES. Counting only
                        // draws made g_rt_written report "0" for a copy-filled
                        // target, which reads as "nothing ever wrote this" — the
                        // exact wrong conclusion when chasing an empty target.
                        if (!dst_screen) g_rt_written[draws[k].copy_dst] += 1;
                        static uint64_t ncp = 0;
                        if (++ncp <= 5 || ncp % 500 == 0)
                            fprintf(stderr, "[gfx] CopyResource #%llu src=%08x -> %s %08x\n",
                                    (unsigned long long)ncp, draws[k].copy_src,
                                    dst_screen ? "SCREEN" : "offscreen", draws[k].copy_dst);
                    } else {
                        static uint64_t nmiss = 0;
                        if (++nmiss <= 5)
                            fprintf(stderr, "[gfx] CopyResource unresolved: src=%08x (%s) "
                                            "dst=%08x (%s)\n",
                                    draws[k].copy_src, src ? "have" : "MISSING",
                                    draws[k].copy_dst, dst_screen ? "screen" : (dst ? "have" : "MISSING"));
                    }
                    k++; continue;
                }
                uint32_t rtv = draws[k].rtv;
                size_t e = k + 1;
                // 🎯 UN EFFACEMENT TERMINE LE GROUPE — le commentaire douze lignes
                // plus bas l'affirmait deja, le code ne le faisait pas. Sans cette
                // coupure, un ClearRenderTargetView emis apres un dessin visant la
                // MEME cible reste dans le groupe : il n'est pas la tete, donc il
                // ne devient pas le loadAction, et la boucle d'encodage saute les
                // enregistrements d'effacement. Personne ne l'execute.
                // Pour l'ecran c'est fatal : `first_use` n'est calcule que
                // `if (!to_screen)`, donc l'ecran n'a aucun second filet et
                // retombe sur Load, qui recharge le drawable RECYCLE du pool de
                // CAMetalLayer — une image d'il y a deux ou trois presentations.
                // D'ou le fond de menu dans les bandes autour de la video.
                // A stencil clear inside the group is executed where it stands: it becomes the load action of
                // the next pass that attaches that plane, and ends this pass only if the pass has already drawn
                // with it (Metal cannot clear in the middle of a pass). One pass carries ONE stencil plane.
                uint32_t st_grp = (!draws[k].is_clear && draws[k].depth.st) ? draws[k].depth.st_res : 0;
                // BO2_DEPTH_DSV (dsv_res is 0 everywhere with the lever off). One pass carries ONE depth resource. The
                // backbuffer keeps its drawable-sized depth: the drawable is not the size of the guest's backbuffer.
                // (g_backbuffer_rtv_id is 0 until the RTV exists: test rtv == 0 first.)
                const bool dp_suit = rtv == 0 || (rtv != g_backbuffer_rtv_id && g_dd_mode != 2);
                uint32_t dp_grp = dp_suit ? draws[k].dsv_res : 0, dp_wh = draws[k].dsv_wh;
                bool vu = !draws[k].is_clear;     // a draw is already in the group
                for (; e < n; e++) {
                    const RealDrawRec& d = draws[e];
                    if (d.is_clear == 2) {
                        if (st_grp == d.depth.st_res) { ++g_st_n[kStCoupeEff]; break; }
                        pochoir_efface(d.depth.st_res, d.depth.st_ref); ++g_st_n[kStEffRecus];
                        continue;
                    }
                    if (d.is_clear == 3) {        // before any draw of the group it is exact to run it ahead of the pass
                        if (vu) { ++g_dd_n[kDdCoupeEff]; break; }
                        prof_efface(d.dsv_res, d.clear_rgba[0], d.dsv_wh);
                        continue;
                    }
                    if (d.is_copy && dp_grp && rtv == 0) { ++g_dd_n[kDdCopie]; break; }   // a CopyResource runs at its place, not swallowed by a depth pass
                    if (d.rtv != rtv || d.is_clear) break;
                    if (dp_suit && d.dsv_res && d.dsv_res != dp_grp) {        // before the stencil block: st_grp is never adopted from an excluded draw
                        if (dp_grp) { ++g_dd_n[kDdCoupe]; break; }
                        dp_grp = d.dsv_res; dp_wh = d.dsv_wh;
                    }
                    if (d.depth.st) {
                        if (!st_grp) st_grp = d.depth.st_res;
                        else if (st_grp != d.depth.st_res) { ++g_st_n[kStCoupeDsv]; break; }
                    }
                    vu = true;
                }
                // ⚠️ LE TEMOIN, ET IL DOIT POUVOIR DIRE ZERO. Ce compteur est
                // exactement le nombre d'effacements qui etaient AVALES avant ce
                // correctif. S'il reste a 0, la these « le jeu emet un Clear qu'on
                // jette » est fausse et les bandes viennent d'ailleurs.
                if (e < n && draws[e].rtv == rtv && draws[e].is_clear) {
                    static uint64_t nsplit = 0, nsplit_screen = 0;
                    const bool scr = (rtv == 0) || (rtv == g_backbuffer_rtv_id);
                    ++nsplit; if (scr) ++nsplit_screen;
                    if (nsplit <= 8 || (nsplit % 2000) == 0)
                        fprintf(stderr, "[CLEAR-AVALE] effacement en MILIEU de groupe "
                                        "rtv=%08x %s — coupe en une passe a lui "
                                        "(#%llu, dont %llu sur l'ECRAN). Avant ce "
                                        "correctif il n'etait execute par personne.\n",
                                rtv, scr ? "ECRAN" : "hors-ecran",
                                (unsigned long long)nsplit,
                                (unsigned long long)nsplit_screen);
                }
                // Where does this group render?
                id<MTLTexture> target = nil;
                bool to_screen = (rtv == 0) || (rtv == g_backbuffer_rtv_id);
                ProfDsv* prof = dp_grp ? prof_dsv(dp_grp, dp_wh) : nullptr;
                const bool sans_couleur = prof && rtv == 0;   // OMSetRenderTargets(1, {NULL}, pDSV): depth only, no colour attachment
                if (sans_couleur) to_screen = false;
                if (!to_screen && !sans_couleur) {
                    auto rr = g_rtv_res.find(rtv);
                    if (rr != g_rtv_res.end()) {
                        auto tt = g_texreg.find(rr->second);
                        if (tt != g_texreg.end()) target = tt->second;
                    }
                    if (!target) { g_real_skipped += (e - k); k = e; continue; }
                }
                // The level this RTV renders into (D3D11 MipSlice). A level the texture lacks cannot be attached.
                uint32_t rt_mip = 0;
                if (target) {
                    auto rm = g_rtv_mip.find(rtv);
                    if (rm != g_rtv_mip.end()) rt_mip = rm->second;
                    if (rt_mip >= target.mipmapLevelCount) {
                        static uint64_t nhors = 0;
                        if (++nhors <= 4)
                            fprintf(stderr, "[RTV-MIP] rtv %08x vise le niveau %u, la texture n'en a que %lu : "
                                            "groupe saute (#%llu)\n", rtv, rt_mip,
                                    (unsigned long)target.mipmapLevelCount, (unsigned long long)nhors);
                        g_real_skipped += (e - k); k = e; continue;
                    }
                }
                // A clear at the head of the group becomes the pass's load action,
                // which is both correct and free; a clear in the middle would need
                // its own pass, so it just ends this group.
                // A NEWLY CREATED MTLTexture HAS UNDEFINED CONTENTS, and our
                // passes use LoadActionLoad unless the guest cleared. So the very
                // first pass into a target read garbage, and the frontend's
                // tone-map composite turns garbage into white (its filmic curve
                // saturates for any large input). Clear each target once, the
                // first time it is rendered into.
                bool first_use = false;
                if (!to_screen) {
                    auto rr2 = g_rtv_res.find(rtv);
                    uint32_t res2 = rr2 != g_rtv_res.end() ? rr2->second : 0;
                    // BO2_UI_WATCHRT=<hex resource> — dump every draw aimed at one
                    // target: its shader, its constant buffers and its textures.
                    // "The output of this target is wrong" is a question about the
                    // draws that produced it, and nothing else answers it.
                    static const uint32_t watchrt = [] {
                        const char* e3 = bo2_getenv("BO2_UI_WATCHRT");
                        return e3 ? (uint32_t)strtoul(e3, nullptr, 16) : 0u; }();
                    if (res2 && res2 == watchrt) {
                        // ONE WHOLE FRAME, PERIODICALLY — not the first N draws
                        // of the run. A flat `nwr < 12` cap meant this only ever
                        // showed the opening draws of frame 1, and a histogram
                        // built from it looked like a census of the 600 draws a
                        // frame while being nothing of the kind. Same family as
                        // the 200-draw head cut. The heavy per-draw detail is
                        // limited to the first few of the frame; the rest get one
                        // line each so the shape of the frame is visible.
                        static uint64_t wframe = 0;
                        static bool wseen = false;
                        if (!wseen) { wseen = true; wframe = g_frames.load(); }
                        // A 600-frame cadence dumps SOME frame; at 4 fps that is one
                        // arbitrary frame two minutes in, and it caught a single draw.
                        // The frame worth dumping is the one being PHOTOGRAPHED, so
                        // ride the same USR1 as the shot and the RT dump — then the
                        // draw list and the picture are the same frame by
                        // construction instead of by luck.
                        bool wdump = ((g_frames.load() - wframe) % 600) == 0 ||
                                     g_rt_dump_req.load() || g_shot_req.load();
                        uint32_t nwr = 0;
                        for (size_t z2 = k; z2 < e && wdump && nwr < 900; z2++) {
                            if (draws[z2].is_clear) continue;
                            nwr++;
                            const RealDrawRec& W = draws[z2];
                            // VIEWPORT AND THE FIRST POSITION, because "1.5M draws
                            // and the target is still black even with every pixel
                            // shader forced to flat magenta" is a question about
                            // RASTERISATION, not shading: either the viewport is
                            // degenerate or the vertices are.
                            fprintf(stderr, "[WATCH] -> %08x ps=%08x vs=%08x idx=%u "
                                            "vp=%.0f,%.0f %.0fx%.0f stride=%u topo=%u",
                                    res2, W.ps, W.vs, W.idx_count,
                                    W.vp[0], W.vp[1], W.vp[2], W.vp[3],
                                    W.vb_stride, W.topo);
                            {   // first POSITION vertex, as floats
                                uint32_t bvo = (W.base_vtx > 0 ? (uint32_t)W.base_vtx : 0)
                                               * W.vb_stride;
                                for (const GfxDrawElem& el : W.elems)
                                    if (!strncmp(el.semantic, "POSITION", 8) &&
                                        bvo + el.offset + 12 <= W.vb.size()) {
                                        float f[3];
                                        memcpy(f, W.vb.data() + bvo + el.offset, 12);
                                        fprintf(stderr, " pos0=%.2f,%.2f,%.2f",
                                                f[0], f[1], f[2]);
                                        break;
                                    }
                            }
                            {   // LE PREMIER SOMMET EN ENTIER, avec sa disposition : une couleur
                                // ou une normale nulle noircit un nuanceur éclairé autant
                                // qu'un soleil nul, et la position seule ne le dit pas.
                                uint32_t bvo = (W.base_vtx > 0 ? (uint32_t)W.base_vtx : 0)
                                               * W.vb_stride;
                                fprintf(stderr, "\n[WATCH]      sommet0:");
                                for (const GfxDrawElem& el : W.elems)
                                    fprintf(stderr, " %s@%u", el.semantic, (unsigned)el.offset);
                                fprintf(stderr, " |");
                                for (uint32_t q = 0; q < W.vb_stride && q < 48 &&
                                                     bvo + q < W.vb.size(); q++)
                                    fprintf(stderr, "%s%02x", (q % 4) ? "" : " ", W.vb[bvo + q]);
                            }
                            // THE VERTEX CONSTANT BUFFERS, as floats. "600 draws a
                            // frame and nothing rasterises, even with shading
                            // eliminated" is a transform question, and a matrix of
                            // zeros (or garbage) is visible at a glance. Print the
                            // first 16 floats of each bound VS buffer = one 4x4.
                            for (int q = 0; q < kMaxCB && nwr <= 3; q++) {
                                if (W.vscb[q].size() < 64) continue;
                                const float* m = (const float*)W.vscb[q].data();
                                // THE MATRIX IS AT FLOAT 142, NOT FLOAT 0. Computed
                                // from the shader's own MSL struct: PerSceneConsts is
                                // 2404 bytes, projectionMatrix at byte 504 (float
                                // 126) and viewProjectionMatrix at byte 568 (float
                                // 142). The head of that buffer is windDirection /
                                // adsZScale / variantWindSpring — reading floats
                                // 0..31 and calling them "the matrix" nearly produced
                                // a confident false root cause.
                                if (W.vscb[q].size() >= 2404) {
                                    // A WINDOW, not a point. My struct offsets are
                                    // computed from the MSL with a naive sequential
                                    // model, and HLSL cbuffer PACKING rules are not
                                    // sequential — the first read landed ~3 floats
                                    // off and made a healthy matrix look like
                                    // garbage. Print the whole region and find the
                                    // matrix by its shape.
                                    for (int r2 = 120; r2 < 168; r2++) {
                                        if ((r2 - 120) % 8 == 0)
                                            fprintf(stderr, "\n[WATCH]      f%3d:", r2);
                                        fprintf(stderr, " %8.3f", m[r2]);
                                    }
                                    // sunPosition, sunDiffuse, hdrControl0, hdrControl1 :
                                    // flottants 72 à 87 (fogColor est bien lu à l'octet 416).
                                    fprintf(stderr, "\n[WATCH]      soleil f72:");
                                    for (int r2 = 72; r2 < 88; r2++)
                                        fprintf(stderr, "%s%.4f", (r2 % 4) ? " " : " | ", m[r2]);
                                }
                                // TWO matrices, not one. BO2's PerObjectConsts is
                                // worldViewMatrix at float 0 and
                                // worldViewProjectionMatrix at float 16 — and it is
                                // the SECOND one that decides whether a vertex lands
                                // in the viewport. Printing only the first showed a
                                // healthy identity and said nothing at all.
                                int nf = W.vscb[q].size() >= 128 ? 32 : 16;
                                fprintf(stderr, "\n[WATCH]      vscb%d (%zu B) m0..%d:",
                                        q, W.vscb[q].size(), nf - 1);
                                for (int r2 = 0; r2 < nf; r2++) {
                                    if (r2 == 16) fprintf(stderr, "  | WVP:");
                                    fprintf(stderr, " %.3f", m[r2]);
                                }
                            }
                            fprintf(stderr, "\n[WATCH]      pscb=");
                            for (int q = 0; q < kMaxCB; q++)
                                fprintf(stderr, "%zu,", W.pscb[q].size());
                            const ShaderRec* wr2 = nullptr;
                            { auto i2 = g_shader_rec.find(W.ps);
                              if (i2 != g_shader_rec.end()) wr2 = &i2->second; }
                            if (wr2) {
                                for (auto& m : wr2->cbslot)
                                    fprintf(stderr, " buf%u<-b%u", m.first, m.second);
                                for (auto& m : wr2->texslot) {
                                    uint32_t tr = m.second < kMaxPSTex ? W.pstex[m.second] : 0;
                                    fprintf(stderr, " tex%u<-t%u=%08x(%s)", m.first,
                                            m.second, tr,
                                            !tr ? "UNBOUND"
                                                : (g_texreg.count(tr) ? "ok" : "no pixels"));
                                    // TAILLE, FORMAT ET UN TEXEL : « ok » dit qu'une texture
                                    // existe, pas qu'elle porte autre chose que du noir.
                                    auto tg = g_texreg.find(tr);
                                    if (tr && tg != g_texreg.end() && tg->second) {
                                        id<MTLTexture> tx = tg->second;
                                        unsigned long pf = (unsigned long)tx.pixelFormat;
                                        fprintf(stderr, "[%lux%lux%lu f%lu", (unsigned long)tx.width,
                                                (unsigned long)tx.height, (unsigned long)tx.depth, pf);
                                        // texel du milieu (2D) ou du coin lointain (3D : c'est là
                                        // que tombe une coordonnée d'éclairage hors bornes)
                                        const bool rgba8 = pf == 70 || pf == 71 || pf == 80 || pf == 81;
                                        if (rgba8 && tx.storageMode != MTLStorageModePrivate &&
                                            (tx.textureType == MTLTextureType2D ||
                                             tx.textureType == MTLTextureType3D)) {
                                            const bool t3 = tx.textureType == MTLTextureType3D;
                                            uint8_t px[4] = {0, 0, 0, 0};
                                            [tx getBytes:px bytesPerRow:4 bytesPerImage:4
                                              fromRegion:MTLRegionMake3D(t3 ? tx.width - 1 : tx.width / 2,
                                                                         t3 ? tx.height - 1 : tx.height / 2,
                                                                         t3 ? tx.depth - 1 : 0, 1, 1, 1)
                                             mipmapLevel:0 slice:0];
                                            fprintf(stderr, " px=%02x%02x%02x%02x", px[0], px[1], px[2], px[3]);
                                        }
                                        fprintf(stderr, "]");
                                    }
                                }
                            }
                            fprintf(stderr, "\n");
                        }
                        // AND A CENSUS, because a per-draw dump of 900 draws is not
                        // a summary and the question is "what paints most of this
                        // target". One accumulator across every group aimed at the
                        // target this frame, flushed at present.
                        if (wdump)
                            for (size_t z2 = k; z2 < e; z2++)
                                if (!draws[z2].is_clear)
                                    g_watch_census[draws[z2].ps] += draws[z2].idx_count;
                    }
                    if (res2) {
                        g_rt_written[res2] += (e - k);
                        // Remember WHO writes each target: an output that is wrong
                        // is a question about the shader that produced it, and the
                        // target id alone does not name it.
                        auto& w = g_rt_writers[res2];
                        for (size_t z2 = k; z2 < e && w.size() < 6; z2++)
                            if (!draws[z2].is_clear && draws[z2].ps) w.insert(draws[z2].ps);
                    }
                    if (res2 && !g_rt_cleared.count(res2)) {
                        g_rt_cleared.insert(res2);
                        first_use = true;
                    }
                }
                bool clear_first = draws[k].is_clear || first_use;
                // 🎯 DOES A CLEAR LAND ON THE DRAWABLE *AFTER* THE WORLD DREW?
                //
                // The depth path already guards this (g_depth_cleared: the first
                // pass at a size clears, later ones load). Colour has no such
                // guard — a drawable pass clears whenever the engine's draw
                // carries is_clear, wherever that falls in OUR ordering. If one
                // lands late, it wipes the frame, and "black with the composite
                // removed" is exactly what that looks like.
                //
                // Print the pass ordinal, so late clears are visible as late.
                if (to_screen) {
                    static uint64_t frame_no = 0, pass_ord = 0, last_frame = ~0ull;
                    if (g_frames.load() != last_frame) { last_frame = g_frames.load();
                                                         pass_ord = 0; ++frame_no; }
                    ++pass_ord;
                    if (clear_first) {
                        static uint64_t nclear = 0;
                        if (++nclear <= 12 || (nclear % 500) == 0)
                            fprintf(stderr, "[SCREEN-CLEAR] drawable pass #%llu of this frame "
                                            "CLEARS (is_clear=%u first_use=%d) rgba=%.2f,%.2f,"
                                            "%.2f,%.2f — clear #%llu\n",
                                    (unsigned long long)pass_ord, draws[k].is_clear,
                                    (int)first_use, draws[k].clear_rgba[0],
                                    draws[k].clear_rgba[1], draws[k].clear_rgba[2],
                                    draws[k].clear_rgba[3], (unsigned long long)nclear);
                    } else {
                        // ⚠️ LE TEMOIN NEGATIF, ET IL MANQUAIT. Cette sonde ne
                        // parlait que quand ca EFFACE. Or c'est l'image qui
                        // n'efface PAS qui laisse le drawable RECYCLE a
                        // l'ecran : loadAction = Load recharge la texture que
                        // CAMetalLayer vient de rendre au pool, c'est-a-dire
                        // une image d'il y a deux ou trois presentations. Le
                        // quad video etant letterboxe, il ne recouvre pas les
                        // bandes — d'ou le fond de menu visible autour de la
                        // video, et tres probablement le flash noir quand le
                        // drawable recycle etait, lui, vide.
                        static uint64_t nload = 0;
                        if (++nload <= 12 || (nload % 500) == 0)
                            fprintf(stderr, "[SCREEN-CLEAR] drawable pass #%llu of this frame "
                                            "LOADS (no clear) — %zu records this frame — "
                                            "charge #%llu : le drawable RECYCLE reste "
                                            "visible partout ou rien ne dessine\n",
                                    (unsigned long long)pass_ord, draws.size(),
                                    (unsigned long long)nload);
                    }
                }
                MTLRenderPassDescriptor* rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
                if (!sans_couleur) {
                rp2.colorAttachments[0].texture     = to_screen ? [drawable texture] : target;
                rp2.colorAttachments[0].level       = rt_mip;
                rp2.colorAttachments[0].loadAction  = clear_first ? MTLLoadActionClear
                                                                 : MTLLoadActionLoad;
                if (clear_first)
                    rp2.colorAttachments[0].clearColor = draws[k].is_clear
                        ? MTLClearColorMake(draws[k].clear_rgba[0], draws[k].clear_rgba[1],
                                            draws[k].clear_rgba[2], draws[k].clear_rgba[3])
                        : MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
                rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
                }
                // THE DEPTH ATTACHMENT. Sized from the colour target this pass
                // writes, shared by every pass at that size. The FIRST pass at a
                // size each frame clears; later ones load, so a depth prepass is
                // still there for the main pass to test against — clearing every
                // pass would discard it, never clearing would carry last frame's
                // depth forward forever.
                MTLPixelFormat st_fmt = MTLPixelFormatInvalid;   // Stencil8 when this pass attaches a stencil plane
                MTLScissorRect sc_plein = {0, 0, 0, 0};          // the whole target: a new encoder's scissor
                {
                    id<MTLTexture> ctex = sans_couleur ? prof->tex : to_screen ? [drawable texture] : target;
                    uint32_t dw = (uint32_t)ctex.width, dh = (uint32_t)ctex.height;
                    if (!sans_couleur && rt_mip) {   // the attached level, not level 0
                        dw = dw >> rt_mip ? dw >> rt_mip : 1; dh = dh >> rt_mip ? dh >> rt_mip : 1;
                    }
                    sc_plein.width = dw; sc_plein.height = dh;
                    if (prof && (prof->tex.width != dw || prof->tex.height != dh)) { ++g_dd_n[kDdTaille]; prof = nullptr; }   // counted fallback: D3D11 requires equal sizes
                    id<MTLTexture> dtex = prof ? prof->tex : depth_for(dw, dh, to_screen);
                    // 🎯 WHICH COLOUR ATTACHMENT OWNS THIS DEPTH BUFFER?
                    //
                    // The 1280x720 depth holds 466,489 real depth values, so a
                    // pass at that size rasterises real geometry — yet removing
                    // the composite leaves a black drawable. Those cannot both be
                    // true without knowing what that pass writes its COLOUR to.
                    // Print the pairing once per (size, target) seen.
                    {
                        static std::set<uint64_t> seen_pair;
                        const uint64_t key = ((uint64_t)dw << 32) | dh;
                        const uint64_t pk  = key ^ ((uint64_t)(uintptr_t)ctex >> 4);
                        if (seen_pair.insert(pk).second)
                            fprintf(stderr, "[PASS-PAIR] depth %ux%u  <-  colour %ux%u %s "
                                            "(to_screen=%d)\n",
                                    dw, dh, (unsigned)ctex.width, (unsigned)ctex.height,
                                    to_screen ? "DRAWABLE" : sans_couleur ? "SANS-COULEUR" : "offscreen-target",
                                    (int)to_screen);
                    }
                    if (dtex) {
                        uint64_t dk = ((uint64_t)dw << 32) | dh | ((uint64_t)to_screen << 31);
                        bool first = prof ? prof->efface >= 0.f : g_depth_cleared.insert(dk).second;
                        rp2.depthAttachment.texture     = dtex;
                        rp2.depthAttachment.loadAction  = first ? MTLLoadActionClear
                                                               : MTLLoadActionLoad;
                        rp2.depthAttachment.storeAction = MTLStoreActionStore;
                        // NOT a hardcoded 1.0. BO2 renders reversed-Z (its depth
                        // states are DepthFunc=GREATER_EQUAL), so 1.0 would reject
                        // almost every fragment and the world would vanish. Use the
                        // value the guest itself passes to ClearDepthStencilView.
                        rp2.depthAttachment.clearDepth  = (prof && first) ? prof->efface : gfx_depth_clear_value();
                        if (prof) {
                            if (first && prof->du_jeu) ++prof->n_eff_faits;
                            prof->efface = -1.f; prof->du_jeu = false;
                            ++(sans_couleur ? prof->n_sans : prof->n_coul);
                        }
                    }
                    if (st_grp) {
                        Pochoir& p = pochoir_for(st_grp, dw, dh);
                        if (p.tex) {
                            rp2.stencilAttachment.texture      = p.tex;
                            rp2.stencilAttachment.loadAction   = p.efface >= 0 ? MTLLoadActionClear : MTLLoadActionLoad;
                            rp2.stencilAttachment.clearStencil = p.efface >= 0 ? (uint32_t)p.efface : 0;
                            rp2.stencilAttachment.storeAction  = MTLStoreActionStore;
                            if (p.efface >= 0) ++g_st_n[kStEffacees];
                            p.efface = -1; st_fmt = MTLPixelFormatStencil8; ++g_st_n[kStPasses];
                        }
                    }
                }
                // What this encoder has set (new encoder: whole-target scissor, reference 0, no culling), and
                // the guest-backbuffer -> drawable mapping the viewport below applies, for the scissor.
                MTLScissorRect sc_now = sc_plein;
                uint32_t sref_now = 0;
                MTLCullMode cull_now = MTLCullModeNone;
                int32_t zb_now = 0; float zbp_now = 0.f, zbm_now = 0.f; bool pince_now = false;
                const float* vp_ref = nullptr;
                double lb_s = 1.0, lb_ox = 0.0, lb_oy = 0.0;
                if (to_screen && g_guest_bb_w > 0.f && g_guest_bb_h > 0.f) {
                    CGSize ds = g_layer.drawableSize;
                    const double sx = ds.width / (double)g_guest_bb_w, sy = ds.height / (double)g_guest_bb_h;
                    lb_s  = (sx < sy) ? sx : sy;
                    lb_ox = (ds.width  - (double)g_guest_bb_w * lb_s) * 0.5;
                    lb_oy = (ds.height - (double)g_guest_bb_h * lb_s) * 0.5;
                }
                // BO2_DEPTH_DSV: a clear still pending on ANOTHER depth that this pass samples runs before it (atlas cleared,
                // no caster drawn). Table empty with the lever off.
                for (auto& kv : g_prof_dsv) {
                    ProfDsv& o = kv.second;
                    if (&o == prof || !o.tex || o.efface < 0.f) continue;
                    bool lue = false;
                    for (size_t z = k; z < e && !lue; z++)
                        for (int s = 0; s < kMaxPSTex && !lue; s++) lue = draws[z].pstex[s] == kv.first;
                    if (!lue) continue;
                    MTLRenderPassDescriptor* rz = [MTLRenderPassDescriptor renderPassDescriptor];
                    rz.depthAttachment.texture = o.tex; rz.depthAttachment.loadAction = MTLLoadActionClear;
                    rz.depthAttachment.clearDepth = o.efface; rz.depthAttachment.storeAction = MTLStoreActionStore;
                    [[cb renderCommandEncoderWithDescriptor:rz] endEncoding];
                    if (o.du_jeu) ++o.n_eff_faits;
                    o.efface = -1.f; o.du_jeu = false; ++g_dd_n[kDdEffSeul];
                }
                g_prof_attache = prof ? prof->tex : nil;
                g_coul_attache = sans_couleur ? nil : rp2.colorAttachments[0].texture;
                g_coul_niveau = rt_mip;
                id<MTLRenderCommandEncoder> pe2 = [cb renderCommandEncoderWithDescriptor:rp2];
                photo_trace_mark(rtv, to_screen, clear_first ? 0xC1EA : 0x10AD, 0);
                ++g_enc_gen;   // C49 : codeur neuf, rien n'y est lie
                n_pass++;
                if (to_screen) {
                    n_screen += (e - k);
                    // WHAT is painting the screen? A histogram by pixel shader,
                    // in submission order, names the big fills and the UI draws
                    // without having to guess from the picture.
                    // Dump ONE whole frame's screen draws, in order. Capping the
                    // list or sampling every Nth pass hides exactly the draw that
                    // matters — the last one to cover the window.
                    if (g_flat_mode >= 2 && g_legend_frame) {
                        uint32_t sq = g_flat_seq;
                        for (size_t z = k; z < e; z++, sq++) {
                            fprintf(stderr, "[LEGEND] #%u ps=%08x vs=%08x idx=%u "
                                            "mask=%x bl=%u(%u,%u) vp=%.0fx%.0f\n", sq,
                                    draws[z].ps, draws[z].vs, draws[z].idx_count,
                                    draws[z].blend.write_mask, draws[z].blend.enable,
                                    draws[z].blend.src_rgb, draws[z].blend.dst_rgb,
                                    draws[z].vp[2], draws[z].vp[3]);
                            // Decode vertex 0 through the layout the guest bound:
                            // a white opaque fill and a transparent one differ only
                            // in COLOR0, and no amount of looking at the window
                            // distinguishes them.
                            const RealDrawRec& dz = draws[z];
                            // PerSceneConsts.viewProjectionMatrix is at byte 576 of
                            // cbuffer b0 (counted off the struct HLSLcc emitted).
                            // Print it with every vertex position, so the quad's
                            // real clip-space extent can be computed instead of
                            // guessed from the picture.
                            if (dz.idx_count <= 12 && dz.vscb[0].size() >= 640) {
                                const float* m = (const float*)(dz.vscb[0].data() + 576);
                                fprintf(stderr, "[LEGEND]      viewProj rows:");
                                for (int q = 0; q < 16; q++) fprintf(stderr, " %.3f", m[q]);
                                fprintf(stderr, "\n");
                                uint32_t nv = dz.vb_stride ?
                                    (uint32_t)(dz.vb.size() / dz.vb_stride) : 0;
                                uint32_t bv = dz.base_vtx > 0 ? (uint32_t)dz.base_vtx : 0;
                                if (bv) fprintf(stderr, "[LEGEND]      baseVertex=%u\n", bv);
                                for (uint32_t q = bv; q < nv && q < bv + 6; q++) {
                                    const float* pv = (const float*)(dz.vb.data() +
                                                                     q * dz.vb_stride);
                                    float x = pv[0], y = pv[1], z2 = pv[2];
                                    float cx = x*m[0]+y*m[1]+z2*m[2]+m[3];
                                    float cy = x*m[4]+y*m[5]+z2*m[6]+m[7];
                                    float cw = x*m[12]+y*m[13]+z2*m[14]+m[15];
                                    fprintf(stderr, "[LEGEND]      v%u (%.1f %.1f %.1f)"
                                                    " -> clip %.3f %.3f w=%.3f%s\n",
                                            q, x, y, z2, cx, cy, cw,
                                            cw != 0.f ? "" : " [w=0]");
                                }
                            }
                            uint32_t bvo = (dz.base_vtx > 0 ? (uint32_t)dz.base_vtx : 0)
                                           * dz.vb_stride;
                            for (const GfxDrawElem& el : dz.elems) {
                                if (bvo + el.offset + 16 > dz.vb.size()) continue;
                                const uint8_t* v = dz.vb.data() + bvo + el.offset;
                                if (el.fmt == 28 || el.fmt == 87)      // R8G8B8A8/B8G8R8A8
                                    fprintf(stderr, "[LEGEND]      %s = %u,%u,%u,%u\n",
                                            el.semantic, v[0], v[1], v[2], v[3]);
                                else if (el.fmt == 2 || el.fmt == 6 || el.fmt == 16) {
                                    float f[4] = {0,0,0,0};
                                    int nf = (el.fmt == 2) ? 4 : (el.fmt == 6 ? 3 : 2);
                                    memcpy(f, v, nf * 4);
                                    fprintf(stderr, "[LEGEND]      %s = %.2f %.2f %.2f %.2f\n",
                                            el.semantic, f[0], f[1], f[2], f[3]);
                                }
                            }
                        }
                    }
                    for (size_t z = k; z < e; z++)
                        if (!draws[z].is_clear) {
                            g_screen_ps[draws[z].ps] += draws[z].idx_count;
                            g_screen_ps_blend[draws[z].ps] =
                                (draws[z].blend.write_mask << 4) | draws[z].blend.enable;
                            // LAST-WINS IS NOT A SUMMARY. The line above records the
                            // most recent blend state for a shader, and reading
                            // "mask0" off it says nothing about the thousands of
                            // other draws with the same shader — the same
                            // capped/sampled-view trap that has cost this project
                            // two retracted conclusions. Accumulate the SET of write
                            // masks actually seen instead, one bit per mask value.
                            g_screen_ps_masks[draws[z].ps] |=
                                1u << (draws[z].blend.write_mask & 0xF);
                        }
                }
                uint32_t res_this_group = 0;
                if (!to_screen) {
                    auto rrg = g_rtv_res.find(rtv);
                    if (rrg != g_rtv_res.end()) res_this_group = rrg->second;
                }
                // 🎯 LETTERBOX — MESURER AVANT DE CORRIGER.
                // Hypothese : `vp_set = true` ci-dessous est pose MEME quand on n'a
                // pose aucun viewport (viewport enregistre degenere). Metal retombe
                // alors sur son defaut, qui est le drawable ENTIER, et tout le groupe
                // peint plein ecran — bandes comprises, par-dessus l'effacement.
                // C'est « le menu visible dans les bandes noires » que le testeur decrit.
                // Ce compteur ne change RIEN au rendu ; il imprime aussi quand le
                // compte est zero, sinon « jamais vu » serait indiscernable de
                // « la sonde n'a pas tourne ».
                bool vp_set = false;
                // The viewport is set ONCE per group, from its first draw; D3D11 applies the one bound at each
                // draw. BO2_VP_DRAW=1 sets it again whenever a draw's differs ([CISEAU] fen counts those draws).
                static const bool vp_par_dessin = [] {
                    const bool on = bo2_env_on("BO2_VP_DRAW", false);
                    fprintf(stderr, "[VP] viewport repose quand il change dans un groupe : %s (BO2_VP_DRAW)\n",
                            on ? "ARME" : "ETEINT");
                    return on; }();
                for (size_t z = k; z < e; z++) {
                    if (draws[z].is_clear) continue;      // handled by the load action
                    if (!vp_set || ((vp_par_dessin || prof) && vp_ref && memcmp(draws[z].vp, vp_ref, sizeof draws[z].vp) != 0)) {
                        const float* v = draws[z].vp;
                        if (v[2] > 0.f && v[3] > 0.f) {
                            // THE GUEST THINKS ITS BACKBUFFER IS 1920x1080 — the DXGI
                            // shim advertises exactly that (BufferDesc 1920x1080, the
                            // one display mode, the output rect). The drawable is
                            // whatever size the window is. Setting the guest's
                            // viewport verbatim therefore draws a 1920x1080 image into
                            // a 1280x720 surface: Metal clips it, so everything comes
                            // out 1.5x too big and the CENTRE of the picture lands at
                            // (960,540) instead of (640,360) — which is exactly the
                            // "Dolby logo cropped into the bottom-right" the user
                            // reported. Map the guest's backbuffer onto the drawable
                            // instead. Offscreen targets keep the viewport verbatim:
                            // those textures really are the size the guest asked for.
                            double x = v[0], y = v[1], w = v[2], h = v[3];
                            if (to_screen && g_guest_bb_w > 0.f && g_guest_bb_h > 0.f) {
                                // LETTERBOX, do not stretch. Scaling x and y by
                                // independent factors distorts the picture the moment
                                // the window's aspect differs from the guest's — which
                                // is exactly what full screen does, and what the tester
                                // reported: the image stretched instead of gaining
                                // black bars. One uniform scale preserves the aspect,
                                // and centring the result puts the leftover space in
                                // equal bars on the two short sides.
                                CGSize ds = g_layer.drawableSize;
                                const double sx = ds.width  / (double)g_guest_bb_w;
                                const double sy = ds.height / (double)g_guest_bb_h;
                                const double s  = (sx < sy) ? sx : sy;
                                const double ox = (ds.width  - (double)g_guest_bb_w * s) * 0.5;
                                const double oy = (ds.height - (double)g_guest_bb_h * s) * 0.5;
                                x = x * s + ox; y = y * s + oy; w *= s; h *= s;
                            }
                            // La plage de profondeur de l'invite, plus codee en
                            // dur : c'est elle qui met le modele de vue devant le
                            // monde. Metal exige [0,1] et une plage non vide ;
                            // un enregistrement anterieur au premier
                            // RSSetViewports vaut deja 0..1 par construction.
                            double zn = (double)v[4], zf = (double)v[5];
                            if (!(zf > zn) || zn < 0.0 || zf > 1.0) { zn = 0.0; zf = 1.0; }
                            MTLViewport mvp = { x, y, w, h, zn, zf };
                            [pe2 setViewport:mvp];
                            if (vp_set) ++g_sc_n[kScVpReposes];
                        }
                        if (to_screen && !vp_set) {
                            ++g_lb_groups;
                            if (!(v[2] > 0.f && v[3] > 0.f)) ++g_lb_novp;
                            // 🎯 LB2 — par quelle branche ce groupe est-il « ecran » ?
                            const bool par_defaut = (rtv == 0);
                            if (par_defaut) {
                                ++g_lb_rtv0;
                                if (draws[z].dsv) ++g_lb_rtv0_dsv; else ++g_lb_rtv0_nodsv;
                                if (!g_lb_vu_rtv) ++g_lb_rtv0_avant;
                                for (size_t y = k; y < e; y++) {
                                    if (draws[y].is_clear) continue;
                                    if (draws[y].blend.write_mask) ++g_lb_rtv0_d_peint;
                                    else                           ++g_lb_rtv0_d_muet;
                                }
                            } else { ++g_lb_rtvbb; g_lb_vu_rtv = true; }
                            {   // histogramme des tailles de viewport, pas un echantillon
                                const uint32_t vw = (uint32_t)v[2], vh = (uint32_t)v[3];
                                uint32_t t = 0;
                                for (; t < g_lb_vp_nv; t++)
                                    if (g_lb_vp_w[t] == vw && g_lb_vp_h[t] == vh) break;
                                if (t < g_lb_vp_nv) { ++g_lb_vp_n[t]; if (par_defaut) ++g_lb_vp_r0[t]; }
                                else if (g_lb_vp_nv < 8u) {
                                    g_lb_vp_w[t] = vw; g_lb_vp_h[t] = vh;
                                    g_lb_vp_n[t] = 1u; g_lb_vp_r0[t] = par_defaut ? 1u : 0u;
                                    ++g_lb_vp_nv;
                                } else ++g_lb_vp_deborde;
                            }
                            if ((g_lb_groups & (g_lb_groups - 1)) == 0) {
                                fprintf(stderr, "[LETTERBOX] groupes ecran=%llu  dont SANS viewport "
                                        "pose=%llu%s  (vp enregistre de ce groupe = %g x %g)\n",
                                        (unsigned long long)g_lb_groups,
                                        (unsigned long long)g_lb_novp,
                                        g_lb_novp ? "   🎯 <<< ceux-la peignent dans les bandes"
                                                  : "   — aucun, le mecanisme est refute",
                                        (double)v[2], (double)v[3]);
                                fprintf(stderr, "[LB2] ecran par DEFAUT (rtv=0)=%llu   ecran parce que "
                                        "BACKBUFFER=%llu%s\n",
                                        (unsigned long long)g_lb_rtv0,
                                        (unsigned long long)g_lb_rtvbb,
                                        g_lb_rtv0
                                            ? "   🎯 <<< LB2.a : des groupes peignent l'ecran faute "
                                              "de cible connue"
                                            : "   — aucun : LB2.a REFUTEE, tout groupe ecran vise le "
                                              "vrai backbuffer");
                                if (g_lb_rtv0)
                                    fprintf(stderr, "[LB2]   parmi eux : dsv NON NUL=%llu%s   "
                                            "dsv nul=%llu   avant tout rtv connu=%llu\n",
                                            (unsigned long long)g_lb_rtv0_dsv,
                                            g_lb_rtv0_dsv
                                                ? "  🎯 <<< LB2.d : PASSE PROFONDEUR SEULE envoyee a "
                                                  "l'ecran — divergence D3D"
                                                : "  (LB2.d refutee : aucune)",
                                            (unsigned long long)g_lb_rtv0_nodsv,
                                            (unsigned long long)g_lb_rtv0_avant);
                                if (g_lb_rtv0)
                                    fprintf(stderr, "[LB2]   leurs dessins : masque d'ecriture NUL "
                                            "(invisible)=%llu   masque NON NUL=%llu%s\n",
                                            (unsigned long long)g_lb_rtv0_d_muet,
                                            (unsigned long long)g_lb_rtv0_d_peint,
                                            g_lb_rtv0_d_peint
                                                ? "  🎯 <<< ceux-la PEIGNENT vraiment l'ecran"
                                                : "  — aucun ne peint : LB2 est un gachis, pas une "
                                                  "corruption visible");
                                for (uint32_t t = 0; t < g_lb_vp_nv; t++)
                                    fprintf(stderr, "[LB2]   viewport %5u x %-5u  x%llu  dont rtv=0 : "
                                            "%llu%s\n", g_lb_vp_w[t], g_lb_vp_h[t],
                                            (unsigned long long)g_lb_vp_n[t],
                                            (unsigned long long)g_lb_vp_r0[t],
                                            (g_lb_vp_w[t] == (uint32_t)g_guest_bb_w &&
                                             g_lb_vp_h[t] == (uint32_t)g_guest_bb_h)
                                                ? "   (la taille declaree a l'invite)"
                                                : "   🎯 <<< PAS la taille declaree : etire par le letterbox");
                                if (g_lb_vp_deborde)
                                    fprintf(stderr, "[LB2]   ⚠️ %llu groupe(s) au-dela de la 8e taille "
                                            "distincte — INOBSERVES, le compte de tailles est un PLANCHER\n",
                                            (unsigned long long)g_lb_vp_deborde);
                            }
                        }
                        vp_set = true; vp_ref = draws[z].vp;
                    }
                    if (to_screen) { g_flat_seq++; last_screen = (int)z;
                                     g_screen_idx.push_back(z); }
                    draws[z].to_screen = to_screen;
                    // BO2_RT_SWEEP=<steps> with BO2_UI_WATCHRT=<res>: draw only the
                    // first `cutoff` draws aimed at the watched target, advancing the
                    // cutoff every dumped frame, and photograph the target each time.
                    // ONE run then produces the whole "which draw paints this region"
                    // film. INDEX COUNT IS NOT SCREEN AREA — the four heaviest
                    // shaders by index count turned out to be dense little geometry
                    // and skipping all four changed the picture by nothing, which is
                    // why a coverage question needs a coverage instrument.
                    if (g_rtsweep_steps && !to_screen && res_this_group &&
                        res_this_group == g_watchrt_res) {
                        // NAME THE DRAW SITTING AT THE CUTOFF. Working the index out
                        // afterwards from the per-draw log does not survive the draw
                        // list changing length between frames — it varied 826..1500
                        // here, so "the 373rd line" was a different draw every frame.
                        // The one draw whose addition changes the picture has to
                        // identify itself, in the same log, at the moment it is
                        // included.
                        if (g_rtsweep_seen == g_rtsweep_cut && g_rtsweep_cut) {
                            const RealDrawRec& C = draws[z];
                            fprintf(stderr, "[RT-CUT] cut=%u of=%u ps=%08x vs=%08x "
                                            "idx=%u stride=%u mask=%x bl=%u(%u,%u)\n",
                                    g_rtsweep_cut, g_rtsweep_total, C.ps, C.vs,
                                    C.idx_count, C.vb_stride, C.blend.write_mask,
                                    C.blend.enable, C.blend.src_rgb, C.blend.dst_rgb);
                        }
                        if (g_rtsweep_seen++ >= g_rtsweep_cut) continue;
                    }
                    // D3D11 SCISSOR. Every rasterizer state the game creates has ScissorEnable = 1, so the
                    // rectangle of RSSetScissorRects clips each draw (before the stencil test, there as here).
                    // It is in the guest's target pixels: mapped onto the drawable like the viewport, then
                    // clamped to the target, which Metal requires.
                    {
                        const RealDrawRec& D = draws[z];
                        // A draw whose viewport rectangle is not the one set (set once per group unless
                        // BO2_VP_DRAW=1) keeps the full rectangle: its own rectangle goes with its own viewport.
                        const bool vp_autre = vp_ref && memcmp(D.vp, vp_ref, 4 * sizeof(float)) != 0;
                        if (vp_autre) ++g_sc_n[kScVpAutre];
                        else if (vp_ref && memcmp(D.vp, vp_ref, sizeof D.vp) != 0) ++g_sc_n[kScVpPlage];
                        MTLScissorRect veut = sc_plein;
                        if ((D.rs & 1u) && !vp_autre) {
                            const long W = (long)sc_plein.width, H = (long)sc_plein.height;
                            const long x0 = std::clamp(lround(D.sc[0] * lb_s + lb_ox), 0L, W);
                            const long y0 = std::clamp(lround(D.sc[1] * lb_s + lb_oy), 0L, H);
                            const long x1 = std::clamp(lround(D.sc[2] * lb_s + lb_ox), 0L, W);
                            const long y1 = std::clamp(lround(D.sc[3] * lb_s + lb_oy), 0L, H);
                            ++g_sc_n[kScDessins];
                            if (D.sc[0] > D.vp[0] || D.sc[1] > D.vp[1] ||
                                D.sc[2] < D.vp[0] + D.vp[2] || D.sc[3] < D.vp[1] + D.vp[3]) {
                                ++g_sc_n[kScRognes]; if (D.depth.st) ++g_sc_n[kScRognesPochoir];
                            }
                            if (x1 <= x0 || y1 <= y0) { ++g_sc_n[kScVides]; continue; }   // D3D11 draws nothing
                            veut = (MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0,
                                                     (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
                        }
                        if (memcmp(&veut, &sc_now, sizeof veut) != 0) { [pe2 setScissorRect:veut]; sc_now = veut; }
                        // D3D11_CULL_MODE 1/2/3 = NONE/FRONT/BACK is Metal's 0/1/2; the game's states are all
                        // FrontCounterClockwise = 0, Metal's default winding. 0 = BO2_CULL off.
                        const uint32_t cull = (D.rs >> 8) & 3u;
                        const MTLCullMode cm = cull ? (MTLCullMode)(cull - 1) : MTLCullModeNone;
                        if (cm != cull_now) { [pe2 setCullMode:cm]; cull_now = cm; }
                        if (cm != MTLCullModeNone) ++g_sc_n[kScCull];
                        // D3D11 DepthBias / SlopeScaledDepthBias / DepthBiasClamp, and DepthClipEnable = 0 as depth
                        // clamp (BO2_DEPTH_BIAS; all zero with the lever off). Same formula on a float depth plane.
                        if (D.zb != zb_now || D.zb_pente != zbp_now || D.zb_max != zbm_now) {
                            [pe2 setDepthBias:(float)D.zb slopeScale:D.zb_pente clamp:D.zb_max];
                            zb_now = D.zb; zbp_now = D.zb_pente; zbm_now = D.zb_max;
                        }
                        const bool pince = (D.rs & 4u) != 0;
                        if (pince != pince_now) { [pe2 setDepthClipMode:pince ? MTLDepthClipModeClamp : MTLDepthClipModeClip]; pince_now = pince; }
                        if (D.zb || D.zb_pente != 0.f) ++g_dd_n[sans_couleur ? kDdBiaisSans : kDdBiaisCoul];
                        if (pince) ++g_dd_n[kDdPince];
                    }
                    // The guest's own depth state for THIS draw. Set per draw, not
                    // per pass: a pass mixes depth-writing world geometry with
                    // depth-disabled overlays, and hoisting this would make the
                    // last state in a pass apply to all of it.
                    //
                    // P652/C7 — SANS DSV LIE, D3D11 NE TESTE RIEN. Un dessin emis
                    // apres OMSetRenderTargets(…, pDSV = NULL) n'a pas de tampon
                    // de profondeur : l'etage de sortie le laisse passer et
                    // n'ecrit rien, quel que soit l'etat de profondeur lie. Nous,
                    // nous attachons depth_for(taille couleur) a chaque passe :
                    // un tel dessin se testait contre la profondeur laissee par
                    // le monde ou le menu. BO2_DSV0_Z=1 remet l'ancien test.
                    bool a_pochoir = false;
                    {
                        static const bool garder_z = [] {
                            const char* e = bo2_getenv("BO2_DSV0_Z");
                            return e && e[0] == '1'; }();
                        GfxDepth dz = draws[z].depth;
                        if (!draws[z].dsv && dz.enable && !garder_z) {
                            dz.enable = 0; dz.write = 0;
                            static uint64_t n_ecran = 0, n_hors = 0, n_peint = 0;
                            if (to_screen) ++n_ecran; else ++n_hors;
                            if (draws[z].blend.write_mask) ++n_peint;
                            const uint64_t n = n_ecran + n_hors;
                            if ((n & (n - 1)) == 0)
                                fprintf(stderr, "[DSV0-Z #%llu] profondeur eteinte faute de DSV : "
                                                "ecran=%llu hors-ecran=%llu dont peignent=%llu "
                                                "(ps=%08x func=%u)\n",
                                        (unsigned long long)n, (unsigned long long)n_ecran,
                                        (unsigned long long)n_hors, (unsigned long long)n_peint,
                                        draws[z].ps, (unsigned)draws[z].depth.func);
                        }
                        // A stencil state needs the plane attached (only its texture failing can break that).
                        if (to_screen && draws[z].dsv_res && dz.enable) {   // tested against the drawable's depth, not its DSV's
                            ++g_dd_n[kDdEcranZ]; g_dd_ecranz_res = draws[z].dsv_res;
                        }
                        if (!to_screen && !sans_couleur && draws[z].dsv_res && dz.enable) ++g_dd_func[dz.func <= 8 ? dz.func : 0];   // what the colour draws ask of the game's DSV, in both modes
                        if (dz.st && st_fmt == MTLPixelFormatInvalid) { dz.st = 0; ++g_st_n[kStSansAttache]; }
                        id<MTLDepthStencilState> ds = dss_for(dz);
                        if (ds) {
                            c49_codeur();
                            if (!g_c49 || g_c49e.ds != ds) { [pe2 setDepthStencilState:ds]; g_c49e.ds = ds; }
                            if (dz.st && sref_now != dz.st_ref) {
                                [pe2 setStencilReferenceValue:dz.st_ref]; sref_now = dz.st_ref;
                            }
                        }
                        a_pochoir = dz.st != 0;
                    }
                    const uint64_t emis_avant = g_real_draws;
                    encode_real_draw(pe2, draws[z],
                                     to_screen ? g_layer.pixelFormat : [target pixelFormat], st_fmt);   // target nil (no colour) = Invalid
                    if (sans_couleur && g_real_draws != emis_avant) ++prof->n_emis;   // really encoded
                    if (a_pochoir) {   // counted AFTER the call: encode_real_draw can still drop the draw
                        if (g_real_draws == emis_avant) ++g_st_n[kStSautes];
                        else { ++g_st_n[kStEmis]; if (draws[z].depth.st >> 16) ++g_st_n[kStEcrivent]; }
                    }
                }
                [pe2 endEncoding];
                k = e;
            }
            // The LAST screen draw is the one that owns the visible pixel, and
            // the one-frame legend kept landing on a different frame than the
            // pixel probe. Report it every time, from the frame actually shown.
            // BO2_UI_LASTN=<n> widens this to the last n screen draws. ONE draw
            // is not enough once the tail matters: in the dark post-title menu
            // the final draw is a transparent cinematic quad while the draw that
            // actually owns the pixel — the tone-map composite, blending off —
            // sits just before it. Dumping only the last one named the innocent
            // draw twice today.
            static const size_t lastn = [] {
                const char* e4 = bo2_getenv("BO2_UI_LASTN");
                size_t v = e4 ? (size_t)strtoul(e4, nullptr, 10) : 1;
                return v ? v : 1; }();
            static uint64_t rep3 = 0;
            bool dump_tail = last_screen >= 0 && (rep3++ % 120) == 0;
            for (size_t q = g_screen_idx.size() > lastn ? g_screen_idx.size() - lastn : 0;
                 dump_tail && q < g_screen_idx.size(); q++) {
                const RealDrawRec& L = draws[g_screen_idx[q]];
                fprintf(stderr, "[LAST] screen draw #%zu/%u ps=%08x vs=%08x idx=%u "
                                "mask=%x bl=%u(%u,%u)", q + 1, g_screen_total,
                        L.ps, L.vs, L.idx_count, L.blend.write_mask, L.blend.enable,
                        L.blend.src_rgb, L.blend.dst_rgb);
                uint32_t bvo = (L.base_vtx > 0 ? (uint32_t)L.base_vtx : 0) * L.vb_stride;
                for (const GfxDrawElem& el : L.elems)
                    if (bvo + el.offset + 4 <= L.vb.size() &&
                        (el.fmt == 28 || el.fmt == 87)) {
                        const uint8_t* v = L.vb.data() + bvo + el.offset;
                        fprintf(stderr, " %s=%u,%u,%u,%u", el.semantic,
                                v[0], v[1], v[2], v[3]);
                    }
                const ShaderRec* pr = nullptr;
                { auto it = g_shader_rec.find(L.ps);
                  if (it != g_shader_rec.end()) pr = &it->second; }
                if (pr)
                    for (auto& m : pr->texslot) {
                        uint32_t res = m.second < kMaxPSTex ? L.pstex[m.second] : 0;
                        auto ti = g_texinfo.find(res);
                        fprintf(stderr, " | tex(%u)<-t%u=%08x", m.first, m.second, res);
                        if (ti != g_texinfo.end())
                            fprintf(stderr, "[%ux%u %u,%u,%u,%u]", ti->second.w,
                                    ti->second.h, ti->second.px[0], ti->second.px[1],
                                    ti->second.px[2], ti->second.px[3]);
                        else fprintf(stderr, "[%s]",
                                     g_texreg.count(res) ? "RT" : "NO TEXTURE");
                    }
                fprintf(stderr, "\n");
            }
            g_hud_n = n; g_hud_screen = n_screen; g_hud_pass = n_pass;
            static uint64_t rep2 = 0;
            if (n && (++rep2 % 200) == 1)
                gfx_skip_report();
        fprintf(stderr, "[gfx] real draws: %llu issued, %llu skipped, "
                                "%llu pipelines (%llu failed); this frame %zu records, "
                                "%zu of them to the SCREEN in %zu passes\n",
                        (unsigned long long)g_real_draws, (unsigned long long)g_real_skipped,
                        (unsigned long long)g_pipe_built, (unsigned long long)g_pipe_failed,
                        n, n_screen, n_pass);
        // 🎯 A COUNTER NOBODY READ. g_vbslot_missing is incremented at line ~1173
        // whenever an attribute reads a vertex stream the guest never gave a
        // stride for — and the fallback there is `st = 4`, four bytes. If the real
        // stream is wider, every vertex after the first is read from the wrong
        // offset, which is exactly the stretched-geometry smear the tester reported on
        // the terrain (and not on the player or the viewmodel, which would use
        // streams whose strides DID arrive). It was declared and incremented and
        // printed nowhere, so it could never have been noticed.
        if (g_vbslot_missing)
            fprintf(stderr, "[gfx] ⚠️ %llu attribute read(s) from a stream with NO stride "
                    "(fell back to 4 bytes) — stretched geometry comes from here\n",
                    (unsigned long long)g_vbslot_missing);
            if (n && (rep2 % 200) == 1 && (g_rej_attr || g_rej_fmt))
                fprintf(stderr, "[gfx] pipelines rejected: %llu for a layout element the"
                                " VS does not declare, %llu for an unmapped format\n",
                        (unsigned long long)g_rej_attr, (unsigned long long)g_rej_fmt);
            if (n && (rep2 % 200) == 1) {
                // Rank the resources the shaders ask for and we have no pixels
                // for: that list names the path that is not being tracked.
                std::vector<std::pair<uint64_t,uint32_t>> top;
                for (auto& kv : g_tx_missing_id) top.push_back({kv.second, kv.first});
                std::sort(top.begin(), top.end(), std::greater<>());
                fprintf(stderr, "[gfx] textures: %llu resolved, %llu bound but NO PIXELS, "
                                "%llu slots with nothing bound, %llu WRONG TYPE "
                                "(%zu distinct missing)",
                        (unsigned long long)g_tx_have, (unsigned long long)g_tx_nopixels,
                        (unsigned long long)g_tx_unbound,
                        (unsigned long long)g_tx_wrongtype, g_tx_missing_id.size());
                fprintf(stderr, " [wrongtype: 3d=%llu cube=%llu depth=%llu"
                                " (of which %llu had a real texture bound)]",
                        (unsigned long long)g_tx_wt3d, (unsigned long long)g_tx_wtcube,
                        (unsigned long long)g_tx_wtdepth,
                        (unsigned long long)g_tx_wtdepth_hadtex);
                for (size_t q = 0; q < top.size() && q < 6; q++)
                    fprintf(stderr, " %08x(x%llu)", top[q].second,
                            (unsigned long long)top[q].first);
                fprintf(stderr, "\n");
                std::vector<std::pair<uint64_t,uint32_t>> ps;
                for (auto& kv : g_screen_ps) ps.push_back({kv.second, kv.first});
                std::sort(ps.begin(), ps.end(), std::greater<>());
                fprintf(stderr, "[gfx] screen painters (ps -> indices), %zu distinct:",
                        ps.size());
                for (size_t q = 0; q < ps.size() && q < 8; q++)
                    fprintf(stderr, " %08x=%llu[masks%04x,bl%u]", ps[q].second,
                            (unsigned long long)ps[q].first,
                            g_screen_ps_masks[ps[q].second],
                            g_screen_ps_blend[ps[q].second] & 1);
                fprintf(stderr, "\n");
                g_screen_ps.clear();
                g_screen_ps_masks.clear();
            }
        }

        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        // The step-1 debug quad (largest staged texture, aspect-fit) must NOT be
        // drawn once the real pipeline is running: it is what has been showing the
        // streaming atlas all along, and I mistook it for engine output while
        // chasing four different "why is the picture wrong" theories. Keep it only
        // for the fixed-function comparison path.
        static const int show_debug_quad = bo2_getenv("BO2_UI_FIXED") ? 1 : 0;
        if (g_show && show_debug_quad) {
            // Aspect-fit the image inside the window.
            CGSize  ds = g_layer.drawableSize;
            float   ia = (float)g_show.width / (float)g_show.height;
            float   wa = (float)ds.width / (float)ds.height;
            simd_float4 scale = (ia > wa) ? simd_make_float4(1.0f, wa / ia, 0.f, 0.f)
                                          : simd_make_float4(ia / wa, 1.0f, 0.f, 0.f);
            [enc setRenderPipelineState:g_pipe];
            [enc setVertexBytes:&scale length:sizeof(scale) atIndex:0];
            [enc setFragmentTexture:g_show atIndex:0];
            [enc setFragmentSamplerState:g_samp atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }

        {   // Textures D3D a detruites (gfx_release_texture) : APRES les
            // enregistrements ci-dessus, pour qu'un envoi en attente d'un objet ne
            // le fasse pas renaitre, et seulement si l'image encodee a ete
            // enregistree tout entiere apres la destruction.
            const uint64_t vu = g_draws_seq.load();
            std::lock_guard<std::mutex> lk(g_texreg_mu);
            size_t garde = 0;
            for (auto& r : g_texrel_pending) {
                if (r.second > vu) { g_texrel_pending[garde++] = r; continue; }
                if (g_texreg.erase(r.first)) g_texrel_rendues++;
                g_texinfo.erase(r.first);
                g_texrel_gone.insert(r.first);
            }
            g_texrel_pending.resize(garde);
        }
        if (g_ui_pipe) {
            std::vector<UiVert>  verts;
            std::vector<UiBatch> batches;
            float vw, vh;
            {
                std::lock_guard<std::mutex> lk(g_ui_mu);
                if (g_ui_ready) { verts = g_ui_verts; batches = g_ui_batches; }
                vw = g_ui_vw; vh = g_ui_vh;
            }
            if (!verts.empty() && !batches.empty()) {
                size_t need = verts.size() * sizeof(UiVert);
                if (!g_ui_vbuf || g_ui_vbuf_cap < need) {
                    g_ui_vbuf_cap = need * 2;
                    g_ui_vbuf = [g_device newBufferWithLength:g_ui_vbuf_cap
                                                      options:MTLResourceStorageModeShared];
                }
                if (g_ui_vbuf) {
                    memcpy([g_ui_vbuf contents], verts.data(), need);
                    [enc setRenderPipelineState:g_ui_pipe];
                    [enc setVertexBuffer:g_ui_vbuf offset:0 atIndex:0];
                    [enc setFragmentSamplerState:g_samp atIndex:0];
                    struct { float row[16]; uint32_t has_mtx; float vp[2]; } xf;
                    xf.vp[0] = vw; xf.vp[1] = vh;
                    // Is the picture missing textures because nothing is bound,
                    // or because we never got the PIXELS for what is bound? Count
                    // both instead of inferring from the image.
                    static uint64_t bt_named = 0, bt_have = 0, bt_none = 0, rep = 0;
                    for (const UiBatch& b : batches) {
                        if (!b.count || b.first + b.count > verts.size()) continue;
                        auto it = g_texreg.find(b.tex);
                        if (!b.tex)                        bt_none++;
                        else if (it != g_texreg.end())     bt_have++;
                        else                               bt_named++;
                        [enc setFragmentTexture:(it != g_texreg.end() ? it->second : g_white)
                                        atIndex:0];
                        memcpy(xf.row, b.mtx, sizeof(xf.row));
                        xf.has_mtx = b.has_mtx;
                        [enc setVertexBytes:&xf length:sizeof(xf) atIndex:1];
                        [enc drawPrimitives:MTLPrimitiveTypeTriangle
                                vertexStart:b.first
                                vertexCount:b.count];
                    }
                    if ((++rep % 300) == 1)
                        fprintf(stderr, "[gfx] batches: %llu textured, %llu named-but-"
                                        "no-pixels, %llu untextured (%zu textures held)\n",
                                (unsigned long long)bt_have, (unsigned long long)bt_named,
                                (unsigned long long)bt_none, g_texreg.size());
                }
            }
        }
        [enc endEncoding];

        // BO2_UI_SHOWRT=1 — DIAGNOSTIC, off by default. The frontend renders its UI
        // into offscreen targets and has not begun compositing them to the screen,
        // so the question is whether the menu EXISTS in one of them. Tile up to four
        // of the engine's render targets over the drawable. This is a debug view and
        // must never be mistaken for engine output (that mistake cost most of a
        // session), hence the explicit env gate and the log line.
        static const int show_rt = bo2_getenv("BO2_UI_SHOWRT") ? atoi(bo2_getenv("BO2_UI_SHOWRT")) : 0;
        if (show_rt && g_pipe && !g_texreg.empty()) {
            std::vector<id<MTLTexture>> big;
            for (uint32_t obj : g_rt_list) {
                auto it = g_texreg.find(obj);
                if (it != g_texreg.end()) big.push_back(it->second);
            }
            if (!big.empty()) {
                // Print periodically, NOT once: a one-shot line reported "tiling 1"
                // because it fired on the first frame, before the engine had created
                // the rest of its targets — and I read that as "only one target
                // registered" and went looking for a registration bug that did not
                // exist.
                static uint64_t nrep = 0;
                if ((nrep++ % 300) == 0)
                    fprintf(stderr, "[gfx] BO2_UI_SHOWRT: tiling %zu of %zu RENDER "
                                    "TARGETS (creation order, 3x3) — DEBUG VIEW, not "
                                    "engine output\n",
                            big.size() > 9 ? (size_t)9 : big.size(), big.size());
                MTLRenderPassDescriptor* dp = [MTLRenderPassDescriptor renderPassDescriptor];
                dp.colorAttachments[0].texture     = [drawable texture];
                dp.colorAttachments[0].loadAction  = MTLLoadActionClear;
                dp.colorAttachments[0].clearColor  = MTLClearColorMake(0.02, 0.02, 0.03, 1.0);
                dp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> de = [cb renderCommandEncoderWithDescriptor:dp];
                [de setRenderPipelineState:g_pipe];
                [de setFragmentSamplerState:g_samp atIndex:0];
                size_t shown = big.size() > 9 ? 9 : big.size();
                for (size_t q = 0; q < shown; q++) {
                    // Tile q of a 3x3 grid, in NDC: a third the size, centred on
                    // the tile. Viewports were doing this wrong and produced two
                    // full-width bands instead of a grid.
                    float cx = (float)(q % 3), cy = (float)(q / 3);
                    simd_float4 xf = simd_make_float4(
                        1.0f / 3.0f, 1.0f / 3.0f,
                        -2.0f / 3.0f + cx * (2.0f / 3.0f),
                         2.0f / 3.0f - cy * (2.0f / 3.0f));
                    [de setVertexBytes:&xf length:sizeof(xf) atIndex:0];
                    [de setFragmentTexture:big[q] atIndex:0];
                    [de drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                }
                [de endEncoding];
            }
        }
        // BO2_SHOT — the whole drawable, not one pixel. Staged into a shared
        // buffer inside this command buffer so it captures EXACTLY the frame
        // being presented, then encoded after the GPU is done with it.
        bool shot_now = false;
        {
            uint64_t f = g_frames.load();
            if (g_shot_every && f && (f % g_shot_every) == 0) g_shot_req.store(1);
            if (g_shot_req.load()) {
                id<MTLTexture> dt = [drawable texture];
                if (dt.width >= 4 && dt.height >= 4) {
                    size_t need = (size_t)dt.width * dt.height * 4;
                    if (!g_shot_buf || g_shot_buf.length < need)
                        g_shot_buf = [g_device newBufferWithLength:need
                                                          options:MTLResourceStorageModeShared];
                    if (g_shot_buf) {
                        id<MTLBlitCommandEncoder> sb = [cb blitCommandEncoder];
                        [sb copyFromTexture:dt sourceSlice:0 sourceLevel:0
                               sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:MTLSizeMake(dt.width, dt.height, 1)
                                   toBuffer:g_shot_buf destinationOffset:0
                          destinationBytesPerRow:dt.width * 4
                       destinationBytesPerImage:need];
                        [sb endEncoding];
                        g_shot_w = (uint32_t)dt.width;
                        g_shot_h = (uint32_t)dt.height;
                        shot_now = true;
                        photo_trace_flush();
                    }
                }
                g_shot_req.store(0);
            }
        }
        // BO2_RT_DUMP=<prefix> — the same capture, but for the engine's OWN
        // render targets instead of the drawable. A centre texel says a target is
        // "126,113,102"; it cannot say whether that target holds a correctly
        // exposed room or an already-blown-out one, and this project has been
        // wrong three times by preferring a number it could read to a picture it
        // could not. Fires on the same USR1 as BO2_SHOT, one PNG per target.
        std::vector<std::pair<uint32_t, size_t>> rt_shots;   // obj -> offset
        if (g_rt_dump_prefix && g_rt_dump_req.load()) {
            size_t off = 0;
            std::vector<std::pair<uint32_t, size_t>> want;
            std::vector<std::pair<uint32_t, size_t>> depth_want;
            size_t doff = 0;
            // ⚠️ THE BUDGET USED TO BE 64 MB AND THE `break` SAID NOTHING, WHICH
            // MADE THIS INSTRUMENT LIE. Thirteen targets on afghanistan need
            // ~65 MB — six 1920x1080 (49.8), a 1024x2048 (8.4), a 1024x1024
            // (4.2), five 480x270 (2.6) — so it truncated, and because g_rt_list
            // is in creation order the ones dropped are the NEWEST. Those are
            // exactly the two the final composite samples: the 2026-08-14 dump
            // covered 477b9880..477ba140 and the screen draw's own log line
            // reads `t2=477bb5c0 t1=477bb2c0 t0=477b9a40`. Two of the three
            // inputs to the thing painting the screen were missing from the
            // measurement of what paints the screen, in silence.
            // 256 MB is nothing on this machine, and the skip is now LOUD —
            // GUIDE rule "a cap announces itself", paid for again.
            const size_t kBudget = (size_t)256u << 20;
            size_t skipped = 0, skipped_bytes = 0;
            for (uint32_t obj : g_rt_list) {
                auto it = g_texreg.find(obj);
                if (it == g_texreg.end() || !it->second) continue;
                id<MTLTexture> rt = it->second;
                if (rt.width < 4 || rt.height < 4) continue;
                // 🎯 DEPTH IS THE YES/NO FOR "DO THESE DRAWS PRODUCE FRAGMENTS".
                //
                // The scene colour target takes 5,081 draws and reads uniform.
                // That is either a constant-writing pixel shader or geometry that
                // never rasterises, and those need OPPOSITE fixes. Depth settles
                // it without guessing: if anything rasterised, depth VARIES; if
                // nothing did, depth is its clear value everywhere.
                //
                // It used to be skipped outright — so the one measurement that
                // separates the two cases was the one never taken. Collected as
                // float stats, no PNG: a depth image is unreadable anyway and the
                // numbers are the whole answer.
                if (rt.pixelFormat == MTLPixelFormatDepth32Float) continue;
                // 🎯 THE BUG THAT MADE THIS INSTRUMENT REPORT 49 OF 50 TARGETS FLAT.
                //
                // `need` and the blit below both assume 4 bytes per pixel. An HDR
                // render target is RGBA16Float — EIGHT bytes — so the copy's
                // destinationBytesPerRow is half a row, every row lands on top of
                // the previous one's second half, and the PNG comes out as a
                // near-uniform smear. That is exactly what "min == max == 7" is.
                // The 1024x32 colour LUT dumped correctly because it IS 8-bit,
                // which is why one good reading did not validate the rest.
                //
                // Measured against the working centre-texel probe
                // (BO2_UI_PROBEPIX), which blits a 1x1 region and reads
                // 477b9a40 as B,G,R,A = 113,100,86,255 with draws-into=2386 —
                // i.e. the target has real content the whole-texture dump could
                // not see.
                const size_t bpp = rt_bytes_per_pixel(rt.pixelFormat);
                // ⚠️ ONLY DUMP WHAT THE PNG WRITER BELOW CAN ACTUALLY READ. It
                // indexes px[i*4 + 0..3], i.e. 8-bit BGRA. A 16-float target
                // copied at the right stride is still not readable that way, and
                // dumping it anyway is how this probe produced 49 convincing
                // "flat" images. Skip it, and SAY the format, so the gap in the
                // dump is visible instead of looking like an empty buffer.
                if (bpp != 4) {
                    fprintf(stderr, "[RT-DUMP] NOT DUMPED 0x%08x %ux%u — pixel format %u is "
                                    "%zu bytes/px; the PNG path reads 8-bit BGRA only. This "
                                    "target is MISSING from the dump, not empty.\n",
                            obj, (unsigned)rt.width, (unsigned)rt.height,
                            (unsigned)rt.pixelFormat, bpp);
                    continue;
                }
                size_t need = (size_t)rt.width * rt.height * bpp;
                if (off + need > kBudget) {
                    ++skipped; skipped_bytes += need;
                    continue;   // keep counting, so the report is a real total
                }
                want.emplace_back(obj, off);
                off += need;
            }
            if (skipped)
                fprintf(stderr, "[RT-DUMP] ⚠️  SKIPPED %zu target(s) (%zu MB) — over the "
                                "%zu MB budget. THE DUMP IS INCOMPLETE; raise kBudget.\n",
                        skipped, skipped_bytes >> 20, kBudget >> 20);
            // ⚠️ THE DEPTH TEXTURES ARE NOT IN g_rt_list. They are created by
            // depth_for() into g_depth_tex, ONE PER SIZE, and the first version of
            // this collected them from g_rt_list — which printed nothing at all
            // and would have read as "there is no depth to look at". Iterate the
            // map that actually holds them.
            for (auto& dk : g_depth_tex) {
                id<MTLTexture> dt2 = dk.second;
                if (!dt2 || dt2.width < 4 || dt2.height < 4) continue;
                depth_want.emplace_back((uint32_t)(dk.first & 0xffffffffu), doff);
                g_depth_tex_by_key[(uint32_t)(dk.first & 0xffffffffu)] = dt2;
                doff += (size_t)dt2.width * dt2.height * 4;
            }
            for (auto& kv : g_prof_dsv) {   // BO2_DEPTH_DSV: the per-resource depths, keyed by their D3D resource
                id<MTLTexture> dt2 = kv.second.tex;
                if (!dt2) continue;
                depth_want.emplace_back(kv.first, doff);
                g_depth_tex_by_key[kv.first] = dt2;
                doff += (size_t)dt2.width * dt2.height * 4;
            }
            fprintf(stderr, "[RT-DUMP] dumping %zu target(s), %zu MB; %zu depth texture(s)\n",
                    want.size(), off >> 20, depth_want.size());
            // 🎯 THE ONE LINE THAT EITHER EXPLAINS THE FLAT DUMPS OR KILLS THE IDEA.
            // A MTLStorageModeMemoryless texture lives only in tile memory and
            // CANNOT be blitted from — the copy silently yields nothing, which is
            // exactly what "every target reads flat while the screen has an
            // image" looks like. Private is copyable; Memoryless is not.
            {
                int memless = 0, priv = 0, shared_ = 0, managed = 0;
                for (auto& kv : want) {
                    id<MTLTexture> t = g_texreg[kv.first];
                    switch (t.storageMode) {
                        case MTLStorageModeShared:     ++shared_; break;
                        case MTLStorageModePrivate:    ++priv;    break;
                        case MTLStorageModeMemoryless: ++memless; break;
                        default:                       ++managed; break;
                    }
                }
                fprintf(stderr, "[RT-DUMP] storage modes: memoryless=%d private=%d shared=%d "
                                "other=%d — %s\n", memless, priv, shared_, managed,
                        memless ? "🎯 MEMORYLESS TARGETS CANNOT BE BLITTED FROM: that is why the "
                                  "dump reads flat"
                                : "no memoryless targets, so storage mode is NOT the reason the "
                                  "dump reads flat");
            }
            if (off) {
                if (!g_rt_buf || g_rt_buf.length < off)
                    g_rt_buf = [g_device newBufferWithLength:off
                                                     options:MTLResourceStorageModeShared];
                if (g_rt_buf) {
                    id<MTLBlitCommandEncoder> rb = [cb blitCommandEncoder];
                    for (auto& kv : want) {
                        id<MTLTexture> rt = g_texreg[kv.first];
                        [rb copyFromTexture:rt sourceSlice:0 sourceLevel:0
                               sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:MTLSizeMake(rt.width, rt.height, 1)
                                   toBuffer:g_rt_buf destinationOffset:kv.second
                          destinationBytesPerRow:rt.width * rt_bytes_per_pixel(rt.pixelFormat)
                       destinationBytesPerImage:(size_t)rt.width * rt.height *
                                                rt_bytes_per_pixel(rt.pixelFormat)];
                    }
                    [rb endEncoding];
                    rt_shots = want;
                }
            }
            // Depth into its own shared buffer, read after the same wait.
            if (doff) {
                static id<MTLBuffer> s_dbuf = nil;
                if (!s_dbuf || s_dbuf.length < doff)
                    s_dbuf = [g_device newBufferWithLength:doff
                                                   options:MTLResourceStorageModeShared];
                if (s_dbuf) {
                    id<MTLBlitCommandEncoder> db = [cb blitCommandEncoder];
                    for (auto& kv : depth_want) {
                        id<MTLTexture> dt2 = g_depth_tex_by_key[kv.first];
                        if (!dt2) continue;
                        [db copyFromTexture:dt2 sourceSlice:0 sourceLevel:0
                               sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:MTLSizeMake(dt2.width, dt2.height, 1)
                                   toBuffer:s_dbuf destinationOffset:kv.second
                          destinationBytesPerRow:dt2.width * 4
                       destinationBytesPerImage:(size_t)dt2.width * dt2.height * 4];
                    }
                    [db endEncoding];
                    g_depth_shots = depth_want;
                    g_depth_buf = s_dbuf;
                }
            }
            g_rt_dump_req.store(0);
        }
        // Exact readback of what actually landed on the drawable, in the
        // drawable's own pixel values — no screenshot, no colour profile.
        if (g_probe_pix) {
            if (!g_pix_buf)
                g_pix_buf = [g_device newBufferWithLength:16 * 24
                                                  options:MTLResourceStorageModeShared];
            id<MTLTexture> dt = [drawable texture];
            if (g_pix_buf && dt.width > 4 && dt.height > 4) {
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                [be copyFromTexture:dt sourceSlice:0 sourceLevel:0
                       sourceOrigin:MTLOriginMake(dt.width / 2, dt.height / 2, 0)
                         sourceSize:MTLSizeMake(1, 1, 1)
                           toBuffer:g_pix_buf destinationOffset:0
                  destinationBytesPerRow:16 destinationBytesPerImage:16];
                // ...and the centre of the engine's own render targets, in
                // creation order. This replaces the tiled debug view, which never
                // produced a readable grid and, being a picture, could not be
                // compared against a number anyway.
                int slotn = 1;
                for (int wq = 0; wq < g_probe_want_n && slotn < 16; wq++) {
                    auto it = g_texreg.find(g_probe_want[wq]);
                    if (it == g_texreg.end()) continue;
                    id<MTLTexture> rt = it->second;
                    if (rt.width < 2 || rt.height < 2) continue;
                    if (rt.pixelFormat == MTLPixelFormatDepth32Float) continue;
                    // Probe the ORIGIN, not the centre: the tone-map composite
                    // samples its colour LUT near (0,0) (a 32x32-tile strip, and
                    // an empty scene indexes the first entry), so the centre pixel
                    // says nothing about the value that actually reaches the screen.
                    [be copyFromTexture:rt sourceSlice:0 sourceLevel:0
                           sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(1, 1, 1)
                               toBuffer:g_pix_buf destinationOffset:16 * slotn
                      destinationBytesPerRow:16 destinationBytesPerImage:16];
                    g_probe_rt[slotn] = g_probe_want[wq];
                    g_probe_rt_ps[slotn] = g_probe_want_ps[wq];
                    g_probe_rt_mid[slotn] = false;          // sampled at (0,0)
                    slotn++;
                }
                g_probe_want_n = 0;
                // THE COLOUR-GRADING LUT, ACROSS ITS WIDTH. One texel cannot tell
                // a correct LUT from a collapsed one: both are "some colour at
                // (0,0)". A 32x32x32 LUT unrolled to a 1024x32 strip must vary
                // smoothly along x, so eight samples spread across it settle it in
                // one run. Everything feeding this texture is already exonerated
                // (real 1024x96 base image, correct resource reaching the
                // composite, populated postFxControl*), so its CONTENT is the last
                // thing left between "the arithmetic is mistranslated" and "the
                // composite misuses a correct LUT".
                // 🪤 "width >= 256 && height == 32" took the FIRST match out of an
                // unordered_map and landed on a 256x32 texture that is not the LUT
                // at all. The LUT is a 32x32x32 cube unrolled to 1024x32, so pick
                // the WIDEST height-32 target rather than any of them.
                g_lut_probe_n = 0;
                uint32_t best = 0, bestw = 0;
                for (auto& kv : g_texreg) {
                    id<MTLTexture> c = kv.second;
                    if (!c || c.height != 32 || c.width < 512) continue;
                    if (c.pixelFormat == MTLPixelFormatDepth32Float) continue;
                    if ((uint32_t)c.width > bestw) { bestw = (uint32_t)c.width; best = kv.first; }
                }
                for (auto& kv : g_texreg) {
                    id<MTLTexture> lt = kv.second;
                    if (!best || kv.first != best) continue;
                    for (int s = 0; s < 8; s++) {
                        uint32_t x = (uint32_t)((lt.width - 1) * s / 7);
                        [be copyFromTexture:lt sourceSlice:0 sourceLevel:0
                               sourceOrigin:MTLOriginMake(x, 16, 0)
                                 sourceSize:MTLSizeMake(1, 1, 1)
                                   toBuffer:g_pix_buf destinationOffset:16 * (16 + s)
                          destinationBytesPerRow:16 destinationBytesPerImage:16];
                    }
                    g_lut_probe_res = kv.first;
                    g_lut_probe_w = (uint32_t)lt.width;
                    g_lut_probe_n = 8;
                    break;
                }
                for (uint32_t obj : g_rt_list) {
                    if (slotn >= 16) break;
                    auto it = g_texreg.find(obj);
                    if (it == g_texreg.end()) continue;
                    id<MTLTexture> rt = it->second;
                    if (rt.width < 4 || rt.height < 4) continue;
                    if (rt.pixelFormat == MTLPixelFormatDepth32Float) continue;
                    [be copyFromTexture:rt sourceSlice:0 sourceLevel:0
                           sourceOrigin:MTLOriginMake(rt.width / 2, rt.height / 2, 0)
                             sourceSize:MTLSizeMake(1, 1, 1)
                               toBuffer:g_pix_buf destinationOffset:16 * slotn
                      destinationBytesPerRow:16 destinationBytesPerImage:16];
                    g_probe_rt[slotn] = obj;
                    g_probe_rt_ps[slotn] = 0;
                    g_probe_rt_mid[slotn] = true;           // sampled at the CENTRE
                    slotn++;
                }
                g_probe_rt_n = slotn;
                [be endEncoding];
            }
        }
        // 🎯 THROTTLE WHEN THE GUEST IS FROZEN — this is what panicked the machine.
        //
        // /Library/Logs/DiagnosticReports panic-full-2026-08-31-110240:
        //   panic: busy timeout[0], (60s): 'IOSurfaceAcceleratorClient'
        //   Panicked task ... pid 393: watchdogd
        // When the guest stops producing frames, this loop kept re-submitting the
        // SAME frame at full rate — one run reached 491 MILLION draws issued — and
        // an IOSurface accelerator client stayed busy past the kernel's 60 s
        // watchdog. A frozen guest must not drive the GPU flat out; there is
        // nothing new to show.
        //
        // Detected from the same counter the frozen-present report already uses, so
        // no new state: if the guest's Present counter has not moved for a second,
        // pace this loop at ~20 Hz instead of unbounded.
        {
            static uint64_t last_seen = 0;
            static double   last_change = 0.0;
            const uint64_t now_n = g_guest_frames.load();   // the counter the freeze report uses
            const double now_t = CACurrentMediaTime();
            if (now_n != last_seen) { last_seen = now_n; last_change = now_t; }
            else if (last_change > 0.0 && (now_t - last_change) > 1.0)
                usleep(50000);   // 20 Hz while nothing new is being produced
            if (last_change == 0.0) last_change = now_t;
        }
        present_track(drawable, g_present_fresh);
        if (g_ring_n) g_ring[g_ring_cur].cb = cb;   // P785 : ce CB lit le jeu courant
        [cb presentDrawable:drawable];
        // Nothing read a command buffer's error: a pass Metal rejects would only show as a missing image.
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            if (b.status == MTLCommandBufferStatusError && ++g_cb_erreurs <= 8)
                fprintf(stderr, "[gfx] tampon de commandes EN ERREUR : %s\n",
                        b.error ? b.error.localizedDescription.UTF8String : "?");
        }];
        [cb commit];
        hud_tick();
        present_report();
        if (shot_now && g_shot_buf) {
            [cb waitUntilCompleted];
            char path[512];
            snprintf(path, sizeof path, "%s_%03u.png",
                     g_shot_prefix ? g_shot_prefix : "/tmp/bo2_frame", g_shot_seq++);
            // Report a NON-BLACK pixel count with the file: a picture nobody
            // looks at is how this project shipped a black image as proof of a
            // result. The number lands in the log next to the path.
            const uint8_t* px = (const uint8_t*)[g_shot_buf contents];
            uint64_t lit = 0, peak = 0;
            for (uint64_t i = 0; i < (uint64_t)g_shot_w * g_shot_h; i++) {
                uint32_t m = std::max(std::max(px[i*4], px[i*4+1]), px[i*4+2]);
                if (m > 8) lit++;
                if (m > peak) peak = m;
            }
            fprintf(stderr, "[SHOT] frame %llu: %llu/%llu pixels lit (>8), peak %llu\n",
                    (unsigned long long)g_frames.load(), (unsigned long long)lit,
                    (unsigned long long)((uint64_t)g_shot_w * g_shot_h),
                    (unsigned long long)peak);
            bo2_write_png(px, g_shot_w, g_shot_h, g_shot_w * 4, path);
        }
        if (!g_watch_census.empty()) {
            std::vector<std::pair<uint64_t,uint32_t>> cs;
            for (auto& kv : g_watch_census) cs.push_back({kv.second, kv.first});
            std::sort(cs.begin(), cs.end(), std::greater<>());
            fprintf(stderr, "[WATCH-CENSUS] %zu distinct pixel shaders paint the watched "
                            "target this frame:", cs.size());
            for (auto& c : cs)
                fprintf(stderr, " %08x=%llu", c.second, (unsigned long long)c.first);
            fprintf(stderr, "\n");
            g_watch_census.clear();
        }
        if (!g_depth_shots.empty() && g_depth_buf) {
            [cb waitUntilCompleted];
            const float* dbp = (const float*)[g_depth_buf contents];
            for (auto& kv : g_depth_shots) {
                auto it = g_depth_tex_by_key.find(kv.first);
                if (it == g_depth_tex_by_key.end() || !it->second) continue;
                id<MTLTexture> dt2 = it->second;
                const uint64_t n = (uint64_t)dt2.width * dt2.height;
                const float* q = dbp + (kv.second / 4);
                float mn = 1e30f, mx = -1e30f; double sum = 0; uint64_t mid = 0;
                for (uint64_t i = 0; i < n; i++) {
                    const float v = q[i];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                    sum += v;
                    if (v > 0.0f && v < 1.0f) mid++;
                }
                fprintf(stderr, "[RT-DEPTH] %08x %ux%u min=%.6f max=%.6f mean=%.6f  "
                                "strictly-between-0-and-1: %llu/%llu — %s\n",
                        kv.first, (unsigned)dt2.width, (unsigned)dt2.height, mn, mx,
                        sum / (double)n, (unsigned long long)mid, (unsigned long long)n,
                        (mx > mn) ? "🎯 DEPTH VARIES: geometry IS rasterising, so the uniform "
                                    "colour is a SHADER or ATTACHMENT problem"
                                  : "🎯 DEPTH UNIFORM: NOTHING rasterised — the geometry never "
                                    "becomes fragments (degenerate transform / all culled)");
                if (g_rt_dump_prefix && mx > mn) {   // the plane itself, grey over its own range, next to the colour dumps
                    std::vector<uint8_t> gris(n * 4, 255);
                    for (uint64_t i = 0; i < n; i++)
                        gris[i * 4] = gris[i * 4 + 1] = gris[i * 4 + 2] = (uint8_t)(255.f * (q[i] - mn) / (mx - mn));
                    char chemin[512];
                    snprintf(chemin, sizeof chemin, "%s_z_%08x_%ux%u.png", g_rt_dump_prefix, kv.first, (unsigned)dt2.width, (unsigned)dt2.height);
                    bo2_write_png(gris.data(), (uint32_t)dt2.width, (uint32_t)dt2.height, (uint32_t)dt2.width * 4, chemin);
                }
            }
            g_depth_shots.clear();
        }
        if (!rt_shots.empty() && g_rt_buf) {
            [cb waitUntilCompleted];
            const uint8_t* base = (const uint8_t*)[g_rt_buf contents];
            for (auto& kv : rt_shots) {
                auto it = g_texreg.find(kv.first);
                if (it == g_texreg.end()) continue;
                id<MTLTexture> rt = it->second;
                uint32_t w = (uint32_t)rt.width, h = (uint32_t)rt.height;
                const uint8_t* px = base + kv.second;
                // The same lit/peak line as BO2_SHOT, next to the dimensions and
                // the resource id, so a target can be matched to a [LUT-USE] slot
                // without opening the file — and then opened anyway.
                uint64_t lit = 0, peak = 0, sum = 0;
                for (uint64_t i = 0; i < (uint64_t)w * h; i++) {
                    uint32_t m = std::max(std::max(px[i*4], px[i*4+1]), px[i*4+2]);
                    if (m > 8) lit++;
                    if (m > peak) peak = m;
                    sum += m;
                }
                char path[512];
                if (g_rtsweep_steps) {
                    if (kv.first != g_watchrt_res) continue;   // one target, many steps
                    snprintf(path, sizeof path, "%s_cut%05u_of%05u.png",
                             g_rt_dump_prefix, g_rtsweep_cut, g_rtsweep_total);
                } else {
                    snprintf(path, sizeof path, "%s_%08x_%ux%u.png",
                             g_rt_dump_prefix, kv.first, w, h);
                }
                // A BLACK TARGET HAS TWO COMPLETELY DIFFERENT CAUSES and the
                // pixels cannot tell them apart: the engine never renders into it
                // (a pass it decided to skip — nothing for us to fix), or it does
                // and we drop or misroute the draws (ours). draws-into and the
                // list of pixel shaders that ever targeted it separate the two,
                // and eleven of thirteen targets are black, so guessing wrong here
                // costs the whole next direction.
                auto wr = g_rt_written.find(kv.first);
                uint64_t nin = (wr == g_rt_written.end()) ? 0 : wr->second;
                fprintf(stderr, "[RT-DUMP] %08x %ux%u fmt=%u  lit %llu/%llu  peak %llu"
                                "  mean %llu  draws-into=%llu%s",
                        kv.first, w, h, (unsigned)rt.pixelFormat,
                        (unsigned long long)lit, (unsigned long long)((uint64_t)w * h),
                        (unsigned long long)peak,
                        (unsigned long long)(sum / ((uint64_t)w * h ? (uint64_t)w * h : 1)),
                        (unsigned long long)nin,
                        nin ? "" : "  <== ENGINE NEVER RENDERED HERE");
                auto wp2 = g_rt_writers.find(kv.first);
                if (wp2 != g_rt_writers.end() && !wp2->second.empty()) {
                    fprintf(stderr, "  ps:");
                    int shown2 = 0;
                    for (uint32_t w3 : wp2->second) {
                        if (++shown2 > 6) { fprintf(stderr, " ..."); break; }
                        fprintf(stderr, " %08x", w3);
                    }
                }
                fprintf(stderr, "  -> %s\n", path);
                bo2_write_png(px, w, h, w * 4, path);
            }
        }
        if (g_probe_pix && g_pix_buf) {
            [cb waitUntilCompleted];
            const uint8_t* px = (const uint8_t*)[g_pix_buf contents];
            static uint64_t np = 0;
            if (g_sweep_n) {
                // Label with the cutoff THIS frame was rendered at. The earlier
                // version printed the previous cutoff against the current pixel,
                // an off-by-one that named the wrong draw — take the last reading
                // per cutoff when reading this back.
                fprintf(stderr, "[SWEEP] cutoff %u of %u -> centre B,G,R,A = "
                                "%u,%u,%u,%u\n",
                        g_sweep_n, g_screen_total, px[0], px[1], px[2], px[3]);
            } else if ((np++ % 120) == 0) {
                fprintf(stderr, "[PIXEL] drawable centre = %u,%u,%u,%u "
                                "(%u screen draws)\n",
                        px[0], px[1], px[2], px[3], g_screen_total);
                if (g_lut_probe_n) {
                    fprintf(stderr, "[LUT-PIX] res %08x (%ux32) across x, B,G,R,A:",
                            g_lut_probe_res, g_lut_probe_w);
                    int varies = 0;
                    const uint8_t* f = px + 16 * 16;
                    for (int s = 0; s < 8; s++) {
                        const uint8_t* q2 = px + 16 * (16 + s);
                        fprintf(stderr, "  %u,%u,%u,%u", q2[0], q2[1], q2[2], q2[3]);
                        if (q2[0] != f[0] || q2[1] != f[1] || q2[2] != f[2]) varies = 1;
                    }
                    fprintf(stderr, "   -> %s\n",
                            varies ? "VARIES (a real gradient)"
                                   : "CONSTANT — the LUT is collapsed");
                }
                for (int q = 1; q < g_probe_rt_n; q++) {
                    const uint8_t* rp = px + 16 * q;
                    // How many draws were ever AIMED at this target? A target that
                    // is sampled but never written is the interesting case, and it
                    // is invisible from the pixel value alone.
                    auto wr = g_rt_written.find(g_probe_rt[q]);
                    fprintf(stderr, "[PIXEL]   tex %08x (ps %08x) %s B,G,R,A "
                                    "= %u,%u,%u,%u  draws-into=%llu\n",
                            g_probe_rt[q], g_probe_rt_ps[q],
                            g_probe_rt_mid[q] ? "CENTRE " : "texel(0,0)",
                            rp[0], rp[1], rp[2], rp[3],
                            (unsigned long long)(wr == g_rt_written.end() ? 0 : wr->second));
                    auto wp = g_rt_writers.find(g_probe_rt[q]);
                    if (wp != g_rt_writers.end()) {
                        fprintf(stderr, "[PIXEL]      written by ps:");
                        for (uint32_t w2 : wp->second) fprintf(stderr, " %08x", w2);
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
    }
    return true;
}

extern "C" void gfx_shutdown() {
    if (!g_open.exchange(false)) return;
    @autoreleasepool {
        [g_window orderOut:nil];
        g_window = nil;
        g_layer  = nil;
        g_queue  = nil;
        g_device = nil;
    }
    fprintf(stderr, "[gfx] window closed after %llu presented frames\n",
            (unsigned long long)g_frames.load());
}

// Dimensions of a registered texture, so the shim side can tell a real asset
// from the engine's 1x1 placeholder without duplicating the registry.
// The recorded HLSLcc constant-buffer renumbering for a shader, or null if it
// has none (in which case the binding is identity).
extern "C" const char* gfx_cbmap_for(uint32_t obj) {
    static std::string out;
    auto it = g_shader_rec.find(obj);
    if (it == g_shader_rec.end()) return nullptr;
    out.clear();
    char b[32];
    for (auto& kv : it->second.cbslot) {
        snprintf(b, sizeof b, "mtl%u<-d3d%u ", kv.first, kv.second);
        out += b;
    }
    if (out.empty()) out = "(empty — identity binding)";
    return out.c_str();
}

// The same, for TEXTURES. HLSLcc renumbers texture registers exactly as it
// renumbers constant buffers — the tone-map composite declares codeTexture0/1/2
// and gets texture(1)/texture(2)/texture(0) — so "which resource does this
// shader's LUT sampler actually read" cannot be answered without this map.
extern "C" const char* gfx_texmap_for(uint32_t obj) {
    static std::string out;
    auto it = g_shader_rec.find(obj);
    if (it == g_shader_rec.end()) return nullptr;
    out.clear();
    char b[32];
    for (auto& kv : it->second.texslot) {
        snprintf(b, sizeof b, "mtl%u<-d3d%u ", kv.first, kv.second);
        out += b;
    }
    if (out.empty()) out = "(empty — identity binding)";
    return out.c_str();
}

// Walk a shader's texture map so a caller can iterate it without parsing a
// string. Returns 0 when `i` is past the end.
// Which D3D b-register feeds this shader's Metal buffer(mtl)? Returns 0xffffffff
// when the shader has no map (the binding is then the identity).
extern "C" uint32_t gfx_cbreg_for_mtl(uint32_t obj, uint32_t mtl) {
    auto it = g_shader_rec.find(obj);
    if (it == g_shader_rec.end()) return 0xffffffffu;
    auto m = it->second.cbslot.find(mtl);
    return m == it->second.cbslot.end() ? 0xffffffffu : m->second;
}

extern "C" int gfx_texslot_at(uint32_t obj, int i, uint32_t* mtl, uint32_t* d3d) {
    auto it = g_shader_rec.find(obj);
    if (it == g_shader_rec.end()) return 0;
    for (auto& kv : it->second.texslot) {
        if (i-- == 0) { if (mtl) *mtl = kv.first; if (d3d) *d3d = kv.second; return 1; }
    }
    return 0;
}

extern "C" int gfx_texture_dims(uint32_t obj, uint32_t* w, uint32_t* h) {
    auto it = g_texreg.find(obj);
    if (it == g_texreg.end() || !it->second) return 0;
    if (w) *w = (uint32_t)it->second.width;
    if (h) *h = (uint32_t)it->second.height;
    return 1;
}

extern "C" uint64_t gfx_presented_frames() { return g_frames.load(); }
extern "C" uint64_t gfx_guest_frames() { return g_guest_frames.load(); }   // [A-COUP] : le veilleur (kernel32_impl.cpp)
