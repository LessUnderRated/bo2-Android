// vk_capture.cpp — the guest-thread half of the Vulkan backend: everything the D3D11 layer calls between Presents.
//
// The semantics are bo2-mac's metal_backend.mm, kept as they were measured there: registrations are queued and
// applied by the pump; each draw's bytes are copied into the frame's arena (C35) with the constant-buffer dedup (C47)
// and static vertex buffers referenced in place (C50); a frame with only depth or stencil clears is held back rather
// than shown black; released textures go once the frames that may read them are drawn.
#include "vk_state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>

extern "C" uint32_t gfx_draw_record_size(void);   // the producer's sizeof(GfxDraw) (remaining_dlls_impl.cpp)

namespace bo2vk {

std::atomic<bool> g_open{false};
std::mutex g_texreg_mu;
std::vector<std::pair<uint32_t, TexStage>> g_texreg_pending;
std::vector<std::vector<uint8_t>> g_texbytes_free;
std::vector<std::pair<uint32_t, uint64_t>> g_texrel_pending;
std::mutex g_rt_mu;
std::vector<RTStage> g_rt_pending;
std::vector<std::pair<uint32_t, uint32_t>> g_rtv_pending, g_rtv_mip_pending;
std::mutex g_shader_mu;
std::vector<ShaderStage> g_shader_pending;
std::vector<std::pair<uint32_t, std::string>> g_texslot_pending, g_cbslot_pending, g_texdim_pending, g_smpslot_pending;
std::vector<std::pair<uint32_t, std::array<uint32_t, 13>>> g_sampler_pending;
std::atomic<uint32_t> g_backbuffer_rtv_id{0}, g_backbuffer_res_id{0};
static bool env_on(const char* n, bool d) { const char* e = getenv(n); return e ? (e[0] && e[0] != '0') : d; }
const bool g_mip_targets = env_on("BO2_MIP_CIBLES", true);

std::mutex g_ui_mu;
std::vector<UiVert> g_ui_verts;
std::vector<UiBatch> g_ui_batches;
float g_ui_vw = 1920.f, g_ui_vh = 1080.f;
bool g_ui_ready = false;
std::mutex g_stage_mu;
StagedTexture g_staged;
static uint64_t g_best_pixels = 0;

std::mutex g_rd_mu;
static RdFrame g_rd;            // the frame being built
RdFrame g_rd_frame;
uint64_t g_rd_seq = 0, g_rd_frame_seq = 0;
std::atomic<uint64_t> g_rd_taken_seq{0};
std::condition_variable g_rd_cv;
std::atomic<uint64_t> g_guest_frames{0}, g_frames{0};

std::mutex g_query_mu;
std::unordered_map<uint32_t, ShaderMaps> g_query_shaders;
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_query_texdims;

namespace {
// Caps on one frame (BO2_UI_MAXRECS, BO2_UI_MAXBYTES), as the macOS backend: past them a draw is dropped and counted.
const uint32_t g_max_recs = [] { const char* e = getenv("BO2_UI_MAXRECS"); return e ? (uint32_t)strtoul(e, nullptr, 10) : 65536u; }();
const uint64_t g_max_bytes = [] { const char* e = getenv("BO2_UI_MAXBYTES"); return e ? strtoull(e, nullptr, 10) : (512ull << 20); }();
uint64_t g_rd_bytes = 0, g_rd_epoch = 1, g_rd_drop_recs = 0, g_rd_drop_bytes = 0;
uint64_t g_c47_reused = 0, g_c47_saved = 0, g_c47_checks = 0, g_c47_mismatch = 0;
const bool g_c47 = env_on("BO2_CB_DEDUP", true);

// Particle-like draws (docs/GRAPHICS.md, "Effects"): blended and depth-tested without writing depth, into a 3D pass
// (a depth buffer bound), from vertices the game rebuilt on the CPU for this frame (no static buffer behind them):
// the effects' sprites and trails (smoke, fire, sparks, blood, muzzle flashes). Translucent draws from static buffers
// (the particle clouds, which the GPU animates; glass; water) are counted apart. [FX-DRAWS] gives per-frame averages
// every 10 s. bo2vk_set_fx_hidden(1) (the BO2_FX_AB measurement, graphics_apply.cpp) drops the particle-like ones
// here, before any copy: what they cost this layer and the GPU, against the same play with them drawn.
std::atomic<bool> g_fx_hidden{false};
uint64_t g_fx_dyn = 0, g_fx_dyn_bytes = 0, g_fx_static = 0, g_fx_all = 0, g_fx_frames = 0;   // under g_rd_mu
uint64_t g_q_frames = 0, g_q_waited = 0, g_q_wait_sum_us = 0, g_q_wait_max_us = 0;          // [QUEUE], under g_rd_mu

bool rd_admit(uint64_t n) {
    if (g_max_recs && g_rd.recs.size() >= g_max_recs) { g_rd_drop_recs++; return false; }
    if (g_max_bytes && g_rd_bytes + n > g_max_bytes) { g_rd_drop_bytes++; return false; }
    g_rd_bytes += n;
    return true;
}

void check_record_layout() {
    const uint32_t theirs = gfx_draw_record_size(), mine = (uint32_t)sizeof(GfxDraw);
    if (theirs == mine) return;
    fprintf(stderr,
            "\n[GFX-ABI] FATAL: struct GfxDraw differs between its two declarations.\n"
            "  runtime/win32/remaining_dlls_impl.cpp (producer): %u bytes\n"
            "  runtime/gfx/vk/gfx_abi.h              (consumer): %u bytes\n"
            "  Every field after the first difference would be read at the wrong address.\n\n",
            theirs, mine);
    abort();
}
}  // namespace

}  // namespace bo2vk

using namespace bo2vk;

// ---- Shaders and their maps -----------------------------------------------------------------------------------------
// Before the window opens, shaders and maps are dropped: the runtime translates a shader again at its first bind
// (gfx_shaders_open, bo2-mac's BO2_SHADER_CREE).
extern "C" void gfx_register_shader2(uint32_t obj, int is_pixel, const char* src, const char* attrs) {
    if (!g_open.load() || !obj || !src) return;
    std::lock_guard<std::mutex> lk(g_shader_mu);
    if (g_shader_pending.size() > 8192) return;
    g_shader_pending.push_back({obj, is_pixel != 0, std::string(src), attrs ? std::string(attrs) : std::string()});
}
extern "C" bool gfx_shaders_open(void) { return g_open.load(); }

#define BO2VK_SLOTS(fn, queue)                                                      \
    extern "C" void fn(uint32_t obj, const char* map) {                             \
        if (!g_open.load() || !obj || !map) return;                                 \
        std::lock_guard<std::mutex> lk(g_shader_mu);                                \
        queue.emplace_back(obj, std::string(map));                                  \
    }
BO2VK_SLOTS(gfx_register_texslots, g_texslot_pending)
BO2VK_SLOTS(gfx_register_cbslots, g_cbslot_pending)
BO2VK_SLOTS(gfx_register_texdims, g_texdim_pending)
BO2VK_SLOTS(gfx_register_smpslots, g_smpslot_pending)
#undef BO2VK_SLOTS

// D3D11_SAMPLER_DESC flattened by the producer: Filter, AddressU/V/W, MipLODBias, MaxAnisotropy, ComparisonFunc,
// BorderColor[4] (floats), MinLOD, MaxLOD (floats). The VkSampler is made on the render thread.
extern "C" void gfx_register_sampler(uint32_t obj, const uint32_t desc[13]) {
    if (!g_open.load() || !obj || !desc) return;
    std::array<uint32_t, 13> d;
    memcpy(d.data(), desc, sizeof(uint32_t) * 13);
    std::lock_guard<std::mutex> lk(g_shader_mu);
    g_sampler_pending.emplace_back(obj, d);
}

// ---- Textures ---------------------------------------------------------------------------------------------------------
static bool known_format(uint32_t fmt) { return dxgi_to_vk(fmt, nullptr, false) != VK_FORMAT_UNDEFINED; }

// Uploads may wait a few frames (apply_pending's per-frame budget), so the queue holds more than one frame's worth:
// a map load registers thousands of textures at once, and a registration past the cap is lost for good.
constexpr size_t kTexPendingMax = 16384;
// A texture's release generation (g_texreg_mu held): an upload queued before the texture was released is dropped
// when its turn comes, instead of creating an image for a dead (or reused) object.
std::unordered_map<uint32_t, uint32_t> g_tex_gen;
static uint32_t tex_gen(uint32_t obj) {
    const auto it = g_tex_gen.find(obj);
    return it == g_tex_gen.end() ? 0 : it->second;
}
bool bo2vk::texstage_stale(uint32_t obj, uint32_t gen) { return tex_gen(obj) != gen; }

// Volumes and cubes the game rewrites keep their staging buffer between uploads (g_texreg_mu held). `uploaded`: the
// bytes are what the GPU image holds now, so an identical rewrite can be skipped. At most kVolKeepMax objects.
struct VolKeep {
    std::vector<uint8_t> bytes;
    uint32_t gen = 0;
    bool uploaded = false;
};
constexpr size_t kVolKeepMax = 16;
std::unordered_map<uint32_t, VolKeep> g_vol_keep;
uint64_t g_vol_same = 0;   // rewrites skipped because the bytes had not changed
static bool vol_pending(uint32_t obj) {
    for (const auto& kv : g_texreg_pending)
        if (kv.first == obj) return true;
    return false;
}
// After apply_pending uploaded (or dropped) a volume or cube: its bytes go back to that object, not the shared pool.
bool bo2vk::texstage_keep(uint32_t obj, uint32_t gen, bool uploaded, std::vector<uint8_t>&& bytes) {
    auto it = g_vol_keep.find(obj);
    if (it == g_vol_keep.end()) {
        if (g_vol_keep.size() >= kVolKeepMax) return false;
        it = g_vol_keep.emplace(obj, VolKeep{}).first;
    }
    VolKeep& k = it->second;
    if (k.bytes.capacity() > bytes.capacity() && !uploaded) return false;   // keep the bigger buffer it already has
    std::vector<uint8_t> old;
    old.swap(k.bytes);
    k.bytes = std::move(bytes);
    k.gen = gen;
    // Uploads run in registration order, so the last one returned is what the image holds; but only if nothing for
    // this object is still queued behind it.
    k.uploaded = uploaded && tex_gen(obj) == gen && !vol_pending(obj);
    if (old.capacity()) bo2vk::texstage_recycle(std::move(old));
    static uint64_t last_said = 0;
    if (g_vol_same && (g_vol_same >> 10) != last_said) {
        last_said = g_vol_same >> 10;
        fprintf(stderr, "[VK-TEX] %llu volume rewrites skipped: unchanged since their last upload\n",
                (unsigned long long)g_vol_same);
    }
    return true;
}

// The bytes of a registration, into a buffer the render thread handed back when one is big enough (g_texreg_mu held).
static void stage_bytes(TexStage& ts, const void* data, uint32_t bytes) {
    size_t best = g_texbytes_free.size();
    for (size_t i = 0; i < g_texbytes_free.size(); i++)
        if (g_texbytes_free[i].capacity() >= bytes &&
            (best == g_texbytes_free.size() || g_texbytes_free[i].capacity() < g_texbytes_free[best].capacity()))
            best = i;
    if (best != g_texbytes_free.size()) {
        ts.bytes.swap(g_texbytes_free[best]);
        g_texbytes_free[best].swap(g_texbytes_free.back());
        g_texbytes_free.pop_back();
    }
    ts.bytes.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
}

// Only buffers worth keeping (past the allocator's 64 KB mmap line), at most 8 of them and 64 MB in all.
void bo2vk::texstage_recycle(std::vector<uint8_t>&& bytes) {
    const size_t cap = bytes.capacity();
    if (cap < (64u << 10) || g_texbytes_free.size() >= 8) return;
    size_t held = 0;
    for (const auto& b : g_texbytes_free) held += b.capacity();
    if (held + cap > (64u << 20)) return;
    bytes.clear();
    g_texbytes_free.push_back(std::move(bytes));
}

extern "C" void gfx_register_texture(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt, const void* data,
                                     uint32_t bytes, uint32_t pitch) {
    if (!obj || !data || !bytes || !w || !h || !known_format(fmt)) return;
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    if (g_texreg_pending.size() > kTexPendingMax) return;   // never unbounded
    TexStage ts;
    ts.gen = tex_gen(obj);
    stage_bytes(ts, data, bytes);
    ts.w = w; ts.h = h; ts.fmt = fmt; ts.pitch = pitch;
    g_texreg_pending.emplace_back(obj, std::move(ts));
}

// A 2D texture with its own mip chain (BC textures: the game's prefiltered mips). offsets/pitches: mips entries.
extern "C" void gfx_register_texture_mips(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt, uint32_t mips,
                                          const void* data, uint32_t bytes, const uint32_t* offsets,
                                          const uint32_t* pitches) {
    if (!obj || !data || !bytes || !w || !h || !mips || !offsets || !pitches || !known_format(fmt)) return;
    if (mips > 16) mips = 16;
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    if (g_texreg_pending.size() > kTexPendingMax) return;
    TexStage ts;
    ts.gen = tex_gen(obj);
    stage_bytes(ts, data, bytes);
    ts.w = w; ts.h = h; ts.fmt = fmt; ts.pitch = pitches[0]; ts.mips = mips;
    ts.mip_off.assign(offsets, offsets + mips);
    ts.mip_pitch.assign(pitches, pitches + mips);
    g_texreg_pending.emplace_back(obj, std::move(ts));
}

// A cube with its mip chain: offsets/pitches hold mips * 6 entries, face-major (D3D subresource order).
extern "C" void gfx_register_cube_mips(uint32_t obj, uint32_t size, uint32_t fmt, uint32_t mips, const void* data,
                                       uint32_t bytes, const uint32_t* offsets, const uint32_t* pitches) {
    if (!obj || !data || !bytes || !size || !mips || !offsets || !pitches || !known_format(fmt)) return;
    if (mips > 16) mips = 16;
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    if (g_texreg_pending.size() > kTexPendingMax) return;
    TexStage ts;
    ts.gen = tex_gen(obj);
    stage_bytes(ts, data, bytes);
    ts.w = size; ts.h = size; ts.fmt = fmt; ts.pitch = pitches[0]; ts.faces = 6; ts.mips = mips;
    ts.mip_off.assign(offsets, offsets + (size_t)mips * 6);
    ts.mip_pitch.assign(pitches, pitches + (size_t)mips * 6);
    g_texreg_pending.emplace_back(obj, std::move(ts));
}

// A volume (depth > 1) or a cube (faces == 6) with level 0 only.
extern "C" void gfx_register_texture_vol(uint32_t obj, uint32_t w, uint32_t h, uint32_t depth, uint32_t faces,
                                         uint32_t fmt, const void* data, uint32_t bytes, uint32_t pitch,
                                         uint32_t slice_pitch) {
    if (!obj || !data || !bytes || !w || !h || !known_format(fmt)) return;
    if (depth < 1) depth = 1;
    if (faces != 6) faces = 1;
    if (depth == 1 && faces == 1) return;   // the caller wanted the 2D entry point
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    if (g_texreg_pending.size() > kTexPendingMax) return;
    TexStage ts;
    ts.gen = tex_gen(obj);
    // The game rewrites one 2 MB volume every frame. Its last uploaded bytes come back here (texstage_keep), so:
    //  - unchanged since that upload, with nothing newer queued for it: the GPU image already holds them; skip;
    //  - otherwise the copy lands in that same buffer: no allocation, no fresh pages to fault in, no munmap later
    //    (the fresh 2 MB buffer per frame was 14% of the backend thread and the render thread's munmap on the S26).
    auto kept = g_vol_keep.find(obj);
    if (kept != g_vol_keep.end()) {
        VolKeep& k = kept->second;
        if (k.uploaded && k.gen == ts.gen && k.bytes.size() == bytes && !vol_pending(obj) &&
            memcmp(k.bytes.data(), data, bytes) == 0) {
            ++g_vol_same;
            return;
        }
        k.uploaded = false;   // newer bytes are on their way: never compare against the old ones again
        if (k.bytes.capacity() >= bytes) ts.bytes.swap(k.bytes);
    }
    if (ts.bytes.capacity() >= bytes) ts.bytes.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
    else stage_bytes(ts, data, bytes);
    ts.w = w; ts.h = h; ts.fmt = fmt; ts.pitch = pitch; ts.depth = depth; ts.faces = faces; ts.slice_pitch = slice_pitch;
    g_texreg_pending.emplace_back(obj, std::move(ts));
}

// D3D destroyed it: it goes once the frames that may still read it (the one being built and the next) are drawn.
extern "C" void gfx_release_texture(uint32_t obj) {
    if (!g_open.load() || !obj) return;
    uint64_t after;
    { std::lock_guard<std::mutex> lk(g_rd_mu); after = g_rd_seq + 2; }
    std::lock_guard<std::mutex> lk(g_texreg_mu);
    g_texrel_pending.emplace_back(obj, after);
    ++g_tex_gen[obj];
    g_vol_keep.erase(obj);
}

extern "C" void gfx_register_rendertarget(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt, uint32_t mips) {
    if (!g_open.load() || !obj) return;
    if (!g_mip_targets) mips = 1;
    std::lock_guard<std::mutex> lk(g_rt_mu);
    if (g_rt_pending.size() > 1024) return;
    g_rt_pending.push_back({obj, w, h, fmt, mips});
}

extern "C" void gfx_register_rtv(uint32_t rtv, uint32_t res, uint32_t mip) {
    if (!g_open.load() || !rtv) return;
    std::lock_guard<std::mutex> lk(g_rt_mu);
    if (g_rtv_pending.size() > 1024) return;
    g_rtv_pending.emplace_back(rtv, res);
    if (mip && g_mip_targets) g_rtv_mip_pending.emplace_back(rtv, mip);
}

extern "C" void gfx_set_backbuffer_rtv(uint32_t rtv) { g_backbuffer_rtv_id.store(rtv); }
extern "C" void gfx_set_backbuffer_res(uint32_t res) { g_backbuffer_res_id.store(res); }

// The live 2D UI pass: the last complete frame of UI triangles, drawn over the game's image.
extern "C" void gfx_ui_frame(const void* verts, uint32_t nverts, const void* batches, uint32_t nbatches, float vpw,
                             float vph) {
    if (!g_open.load() || !nverts || !nbatches || !verts || !batches) return;
    std::lock_guard<std::mutex> lk(g_ui_mu);
    g_ui_verts.assign((const UiVert*)verts, (const UiVert*)verts + nverts);
    g_ui_batches.assign((const UiBatch*)batches, (const UiBatch*)batches + nbatches);
    if (vpw > 0) g_ui_vw = vpw;
    if (vph > 0) g_ui_vh = vph;
    g_ui_ready = true;
}

// The largest texture the guest created so far, for the BO2_UI_FIXED debug view.
extern "C" void gfx_stage_texture(uint32_t w, uint32_t h, uint32_t fmt, const void* data, uint32_t bytes,
                                  uint32_t pitch) {
    if (!g_open.load() || !data || !bytes || !w || !h || !known_format(fmt)) return;
    const uint64_t px = (uint64_t)w * h;
    std::lock_guard<std::mutex> lk(g_stage_mu);
    if (px <= g_best_pixels) return;
    g_best_pixels = px;
    g_staged.bytes.assign((const uint8_t*)data, (const uint8_t*)data + bytes);
    g_staged.w = w; g_staged.h = h; g_staged.fmt = fmt; g_staged.pitch = pitch; g_staged.ready = true;
}

// ---- Draws ------------------------------------------------------------------------------------------------------------
extern "C" void gfx_real_draw(const GfxDraw* d) {
    static bool layout_checked = false;
    if (__builtin_expect(!layout_checked, 0)) { layout_checked = true; check_record_layout(); }
    if (!g_open.load() || !d) return;
    if (!d->is_clear && !d->is_copy && (!d->vb || !d->ib)) return;
    std::lock_guard<std::mutex> lk(g_rd_mu);
    if (d->is_copy) {
        if (!rd_admit(0)) return;
        RealDrawRec& c = g_rd.recs.emplace_back();
        memset(&c, 0, sizeof c);
        c.is_copy = 1; c.copy_dst = d->copy_dst; c.copy_src = d->copy_src;
        return;
    }
    if (d->is_clear) {
        if (!rd_admit(0)) return;
        RealDrawRec& c = g_rd.recs.emplace_back();
        memset(&c, 0, sizeof c);
        c.is_clear = d->is_clear; c.rtv = d->rtv;
        c.depth.st_res = d->depth.st_res; c.depth.st_ref = d->depth.st_ref;   // is_clear 2: which plane, which value
        c.dsv_res = d->dsv_res; c.dsv_wh = d->dsv_wh;                         // is_clear 3: which depth, its size
        for (int i = 0; i < 4; i++) c.clear_rgba[i] = d->clear_rgba[i];
        return;
    }
    ++g_fx_all;
    if (d->blend.enable && d->depth.enable && !d->depth.write && d->dsv) {
        if (d->res[0]) {
            ++g_fx_static;
        } else {
            ++g_fx_dyn;
            g_fx_dyn_bytes += d->vb_bytes;
            if (g_fx_hidden.load(std::memory_order_relaxed)) return;
        }
    }
    {
        uint64_t n = (uint64_t)(d->res[0] ? 0u : d->vb_bytes) + (d->ib_res ? 0u : d->ib_bytes);
        for (int i = 0; i < kMaxCB; i++) n += d->vscb_bytes[i] + d->pscb_bytes[i];
        if (!rd_admit(n)) return;
    }
    RdArena& a = g_rd.arena;
    RealDrawRec& r = g_rd.recs.emplace_back();
    memset(&r, 0, sizeof r);
    r.vs = d->vs; r.ps = d->ps;
    r.elems = a.copy(d->elems, d->nelems);
    auto in_place = [](const void* p, uint32_t n) { RdSpan<uint8_t> t; t.p = (const uint8_t*)p; t.n = n; return t; };
    for (uint32_t sl = 0; sl < kMaxVBStreams; sl++) { r.res[sl] = (const uint8_t*)d->res[sl]; r.res_bytes[sl] = d->res_bytes[sl]; }
    // C50: a slice of a static buffer is referenced where it is (its bytes never change); anything else is copied.
    r.vb = d->res[0] ? in_place(d->vb, d->vb_bytes) : a.copy((const uint8_t*)d->vb, d->vb_bytes);
    // BO2_IB_RESIDENT: the indices of a static index buffer are referenced where they are, as C50's vertices.
    r.ib_res = (const uint8_t*)d->ib_res; r.ib_res_bytes = d->ib_res ? d->ib_res_bytes : 0;
    r.ib = d->ib_res ? in_place(d->ib, d->ib_bytes) : a.copy((const uint8_t*)d->ib, d->ib_bytes);
    r.vb_stride = d->vb_stride; r.ib_fmt = d->ib_fmt;
    for (uint32_t sl = 0; sl < kMaxVBStreams - 1; sl++) {
        r.vb2_stride[sl] = d->vb2_stride[sl];
        r.vb2[sl] = d->res[sl + 1] ? in_place(d->vb2[sl], d->vb2_bytes[sl])
                                   : a.copy((const uint8_t*)d->vb2[sl], d->vb2_bytes[sl]);
    }
    r.idx_count = d->idx_count; r.base_vtx = d->base_vtx; r.topo = d->topo;
    r.rtv = d->rtv; r.blend = d->blend; r.depth = d->depth; r.dsv = d->dsv;
    r.dsv_res = d->dsv_res; r.dsv_wh = d->dsv_wh;
    r.rs = d->rs; for (int i = 0; i < 4; i++) r.sc[i] = d->sc[i];
    r.zb = d->zb; r.zb_pente = d->zb_pente; r.zb_max = d->zb_max;
    for (int i = 0; i < 6; i++) r.vp[i] = d->vp[i];
    // C47: a constant buffer the producer did not refill since the last draw (same fill counter, same size, same
    // frame) shares that draw's copy; one check in 16 compares the bytes, and a difference is counted.
    static uint32_t fill_seen[2 * kMaxCB] = {0};
    static uint64_t epoch_seen[2 * kMaxCB] = {0};
    static RdSpan<uint8_t> slice_seen[2 * kMaxCB];
    auto cb = [&](int k, const void* src, uint32_t n, uint32_t fill) {
        if (g_c47 && fill && n && fill == fill_seen[k] && epoch_seen[k] == g_rd_epoch && slice_seen[k].n == n) {
            ++g_c47_reused; g_c47_saved += n;
            if ((++g_c47_checks & 15) == 0 && memcmp(slice_seen[k].p, src, n) != 0) ++g_c47_mismatch;
            return slice_seen[k];
        }
        const RdSpan<uint8_t> t = a.copy((const uint8_t*)src, n);
        fill_seen[k] = fill; epoch_seen[k] = g_rd_epoch; slice_seen[k] = t;
        return t;
    };
    for (int i = 0; i < kMaxCB; i++) {
        r.vscb[i] = cb(i, d->vscb[i], d->vscb_bytes[i], d->vscb_fill[i]);
        r.pscb[i] = cb(kMaxCB + i, d->pscb[i], d->pscb_bytes[i], d->pscb_fill[i]);
    }
    for (int i = 0; i < kMaxPSTex; i++) { r.pstex[i] = d->pstex[i]; r.pssamp[i] = d->pssamp[i]; r.psmip[i] = d->psmip[i]; }
    r.vpw = d->vpw; r.vph = d->vph;
}

extern "C" void gfx_real_frame_end() {
    std::unique_lock<std::mutex> lk(g_rd_mu);
    static uint64_t nf = 0, last_dr = 0, last_db = 0, nlog = 0;
    if ((g_rd_drop_recs != last_dr || g_rd_drop_bytes != last_db) && (++nf % 60) == 1) {
        fprintf(stderr, "[gfx] frame records: %zu kept, %llu dropped over the count cap, %llu over the byte cap\n",
                g_rd.recs.size(), (unsigned long long)g_rd_drop_recs, (unsigned long long)g_rd_drop_bytes);
        last_dr = g_rd_drop_recs; last_db = g_rd_drop_bytes;
    }
    // One frame's copies into the arena (vertices, indices, constants), every 600 frames; this line reaches the phone's
    // log (the measure of BO2_CB_DIRECT / BO2_IB_RESIDENT / BO2_RD_SCISSOR_EARLY, docs/GRAPHICS.md).
    if ((nlog++ % 600) == 0)
        fprintf(stderr, "[C35] frame arena: %zu records, %.2f MB copied; C47 %s: %llu buffers reused, %.2f MB saved, "
                        "checks %llu mismatches %llu\n",
                g_rd.recs.size(), (double)(g_rd_bytes - g_c47_saved) / 1048576.0, g_c47 ? "on" : "off",
                (unsigned long long)g_c47_reused, (double)g_c47_saved / 1048576.0,
                (unsigned long long)(g_c47_checks >> 4), (unsigned long long)g_c47_mismatch);
    ++g_fx_frames;
    static auto fx_t0 = std::chrono::steady_clock::now();
    const auto fx_now = std::chrono::steady_clock::now();
    if (fx_now - fx_t0 >= std::chrono::seconds(10)) {
        const double f = (double)g_fx_frames;
        fprintf(stderr, "[FX-DRAWS] per frame over %llu frames: %.0f particle-like draws (%.0f KB of vertices built on "
                        "the CPU), %.0f translucent from static buffers, of %.0f draws%s\n",
                (unsigned long long)g_fx_frames, g_fx_dyn / f, g_fx_dyn_bytes / f / 1024.0, g_fx_static / f,
                g_fx_all / f, g_fx_hidden.load() ? " (particle-like hidden: BO2_FX_AB)" : "");
        g_fx_dyn = g_fx_dyn_bytes = g_fx_static = g_fx_all = g_fx_frames = 0;
        fprintf(stderr, "[QUEUE] the game's render thread waited for the display on %llu of %llu frames: %.1f ms on "
                        "average, %.1f ms at most\n",
                (unsigned long long)g_q_waited, (unsigned long long)g_q_frames,
                g_q_waited ? g_q_wait_sum_us / 1000.0 / g_q_waited : 0.0, g_q_wait_max_us / 1000.0);
        g_q_frames = g_q_waited = g_q_wait_sum_us = g_q_wait_max_us = 0;
        fx_t0 = fx_now;
    }
    // A frame of only depth or stencil clears draws nothing: keep the previous one instead of flashing black.
    bool content = false;
    for (const RealDrawRec& r : g_rd.recs) if (r.is_clear < 2) { content = true; break; }
    if (content) {
        // The frame queue: the previous frame not shown yet, wait for the pump to take it (at most 50 ms, so a stalled
        // pump, in the background, costs speed and never a hang). Before, the newest frame simply replaced it: the
        // game paced itself (com_maxfps) and the pump picked the newest frame at each refresh, two 60 Hz clocks that
        // drift, so some frames were shown twice and others never (judder at an average of 60 fps). Now each frame
        // is shown once and the display paces the game.
        static const bool queue = [] { const char* e = getenv("BO2_FRAME_QUEUE"); return !e || *e != '0'; }();
        ++g_q_frames;
        if (queue && g_rd_frame_seq > g_rd_taken_seq.load()) {
            // [QUEUE]: the game's render thread held here is the game running ahead of the display; the game read the
            // pad for the frames behind this one already, so every millisecond here is a millisecond of input age.
            const auto w0 = std::chrono::steady_clock::now();
            g_rd_cv.wait_for(lk, std::chrono::milliseconds(50), [] { return g_rd_frame_seq <= g_rd_taken_seq.load(); });
            const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - w0).count();
            ++g_q_waited;
            g_q_wait_sum_us += us;
            g_q_wait_max_us = std::max(g_q_wait_max_us, us);
        }
        g_rd_frame.swap(g_rd);
        g_rd_frame_seq = ++g_rd_seq;
    } else {
        static uint64_t skipped = 0;
        if (++skipped == 1 || (skipped % 500) == 0)
            fprintf(stderr, "[gfx] %llu empty frames held back (previous frame kept)\n", (unsigned long long)skipped);
    }
    g_rd.clear();
    g_rd_bytes = 0;
    ++g_rd_epoch; g_c47_reused = 0; g_c47_saved = 0;
    g_guest_frames.fetch_add(1);
}

// ---- Queries from the runtime (its probes), answered from what the render thread published -------------------------
extern "C" void bo2vk_set_fx_hidden(int on) { g_fx_hidden.store(on != 0); }

extern "C" const char* gfx_cbmap_for(uint32_t obj) {
    static thread_local std::string out;
    std::lock_guard<std::mutex> lk(g_query_mu);
    auto it = g_query_shaders.find(obj);
    if (it == g_query_shaders.end()) return nullptr;
    out.clear();
    char b[32];
    for (auto& kv : it->second.cb) { snprintf(b, sizeof b, "mtl%u<-d3d%u ", kv.first, kv.second); out += b; }
    if (out.empty()) out = "(empty — identity binding)";
    return out.c_str();
}

extern "C" const char* gfx_texmap_for(uint32_t obj) {
    static thread_local std::string out;
    std::lock_guard<std::mutex> lk(g_query_mu);
    auto it = g_query_shaders.find(obj);
    if (it == g_query_shaders.end()) return nullptr;
    out.clear();
    char b[32];
    for (auto& kv : it->second.tex) { snprintf(b, sizeof b, "mtl%u<-d3d%u ", kv.first, kv.second); out += b; }
    if (out.empty()) out = "(empty — identity binding)";
    return out.c_str();
}

extern "C" uint32_t gfx_cbreg_for_mtl(uint32_t obj, uint32_t idx) {
    std::lock_guard<std::mutex> lk(g_query_mu);
    auto it = g_query_shaders.find(obj);
    if (it == g_query_shaders.end()) return 0xffffffffu;
    auto m = it->second.cb.find(idx);
    return m == it->second.cb.end() ? 0xffffffffu : m->second;
}

extern "C" int gfx_texslot_at(uint32_t obj, int i, uint32_t* idx, uint32_t* d3d) {
    std::lock_guard<std::mutex> lk(g_query_mu);
    auto it = g_query_shaders.find(obj);
    if (it == g_query_shaders.end()) return 0;
    for (auto& kv : it->second.tex)
        if (i-- == 0) { if (idx) *idx = kv.first; if (d3d) *d3d = kv.second; return 1; }
    return 0;
}

extern "C" int gfx_texture_dims(uint32_t obj, uint32_t* w, uint32_t* h) {
    std::lock_guard<std::mutex> lk(g_query_mu);
    auto it = g_query_texdims.find(obj);
    if (it == g_query_texdims.end()) return 0;
    if (w) *w = it->second.first;
    if (h) *h = it->second.second;
    return 1;
}

extern "C" uint64_t gfx_presented_frames() { return g_frames.load(); }
extern "C" uint64_t gfx_guest_frames() { return g_guest_frames.load(); }
