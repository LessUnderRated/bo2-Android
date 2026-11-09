// vk_state.h — what the guest threads hand the render thread (vk_capture.cpp writes, vk_render.cpp reads).
//
// The same hand-over as bo2-mac's metal_backend.mm: registrations are queued under a mutex and applied by the pump;
// each frame's draws are copied into an arena (C35) and handed over whole at gfx_real_frame_end, so the render thread
// never reads guest memory while the game writes it.
#pragma once
#include "vk_backend.h"

#include <cstring>
#include <array>
#include <set>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace bo2vk {

extern std::atomic<bool> g_open;                     // gfx_window_open succeeded (and not yet gfx_shutdown)

// ---- Registrations, queued by the guest, applied by the pump --------------------------------------------------
struct TexStage {
    std::vector<uint8_t> bytes;
    uint32_t w = 0, h = 0, fmt = 0, pitch = 0;
    uint32_t depth = 1, faces = 1, slice_pitch = 0, mips = 1;
    std::vector<uint32_t> mip_off, mip_pitch;        // mips (x 6 for a cube, face-major) entries
    uint32_t gen = 0;                                // the texture's release generation at registration
};
struct RTStage { uint32_t obj, w, h, fmt, mips; };
struct ShaderStage { uint32_t obj; bool is_pixel; std::string src, attrs; };

extern std::mutex g_texreg_mu;
extern std::vector<std::pair<uint32_t, TexStage>> g_texreg_pending;
// Byte buffers of uploaded textures, handed back by the render thread for the next registration (under g_texreg_mu).
// The game rewrites one volume texture every frame: a fresh buffer each time went through the allocator's mmap path
// (munmap was 5% of the render thread). Bounded by texstage_recycle.
extern std::vector<std::vector<uint8_t>> g_texbytes_free;
void texstage_recycle(std::vector<uint8_t>&& bytes);   // caller holds g_texreg_mu
bool texstage_stale(uint32_t obj, uint32_t gen);      // released since it was queued; caller holds g_texreg_mu
// A volume/cube's staging buffer back to its object after apply_pending (caller holds g_texreg_mu); false: not kept.
bool texstage_keep(uint32_t obj, uint32_t gen, bool uploaded, std::vector<uint8_t>&& bytes);
extern std::vector<std::pair<uint32_t, uint64_t>> g_texrel_pending;   // obj, frame sequence it may go after
extern std::mutex g_rt_mu;
extern std::vector<RTStage> g_rt_pending;
extern std::vector<std::pair<uint32_t, uint32_t>> g_rtv_pending, g_rtv_mip_pending;
extern std::mutex g_shader_mu;
extern std::vector<ShaderStage> g_shader_pending;
extern std::vector<std::pair<uint32_t, std::string>> g_texslot_pending, g_cbslot_pending, g_texdim_pending,
    g_smpslot_pending;
extern std::vector<std::pair<uint32_t, std::array<uint32_t, 13>>> g_sampler_pending;
extern std::atomic<uint32_t> g_backbuffer_rtv_id, g_backbuffer_res_id;
extern const bool g_mip_targets;                     // BO2_MIP_CIBLES (default on): render targets keep their mips

// The live 2D UI pass and the staged-texture debug view (gfx_ui_frame, gfx_stage_texture).
extern std::mutex g_ui_mu;
extern std::vector<UiVert> g_ui_verts;
extern std::vector<UiBatch> g_ui_batches;
extern float g_ui_vw, g_ui_vh;
extern bool g_ui_ready;
extern std::mutex g_stage_mu;
struct StagedTexture { std::vector<uint8_t> bytes; uint32_t w = 0, h = 0, fmt = 0, pitch = 0; bool ready = false; };
extern StagedTexture g_staged;

// ---- One frame of draws ---------------------------------------------------------------------------------------
template <class T> struct RdSpan {
    const T* p = nullptr;
    uint32_t n = 0;
    const T* data() const { return p; }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    const T* begin() const { return p; }
    const T* end() const { return p + n; }
    const T& operator[](size_t i) const { return p[i]; }
};

// C35: the frame's bytes in 8 MiB chunks kept from frame to frame (no per-draw allocation).
class RdArena {
public:
    template <class T> RdSpan<T> copy(const T* src, uint32_t count) {
        RdSpan<T> s;
        if (!src || !count) return s;
        const size_t bytes = (size_t)count * sizeof(T);
        uint8_t* dst = alloc(bytes);
        memcpy(dst, src, bytes);
        s.p = reinterpret_cast<const T*>(dst);
        s.n = count;
        return s;
    }
    void reset() {
        const size_t keep = cur_ + 3;
        if (chunks_.size() > keep) { chunks_.resize(keep); sizes_.resize(keep); }
        cur_ = 0; off_ = 0;
    }
    uint64_t held() const { uint64_t t = 0; for (size_t z : sizes_) t += z; return t; }
    void swap(RdArena& o) noexcept {
        chunks_.swap(o.chunks_); sizes_.swap(o.sizes_);
        std::swap(cur_, o.cur_); std::swap(off_, o.off_);
    }
private:
    static constexpr size_t kChunk = 8u << 20;
    uint8_t* alloc(size_t bytes) {
        bytes = (bytes + 15) & ~size_t(15);
        while (cur_ < chunks_.size() && off_ + bytes > sizes_[cur_]) { ++cur_; off_ = 0; }
        if (cur_ == chunks_.size()) {
            const size_t z = bytes > kChunk ? bytes : kChunk;
            chunks_.push_back(std::unique_ptr<uint8_t[]>(new uint8_t[z]));
            sizes_.push_back(z);
            off_ = 0;
        }
        uint8_t* p = chunks_[cur_].get() + off_;
        off_ += bytes;
        return p;
    }
    std::vector<std::unique_ptr<uint8_t[]>> chunks_;
    std::vector<size_t> sizes_;
    size_t cur_ = 0, off_ = 0;
};

struct RealDrawRec {
    uint32_t vs, ps;
    RdSpan<GfxDrawElem> elems;
    RdSpan<uint8_t> vb, ib;
    uint32_t vb_stride, ib_fmt, idx_count; int32_t base_vtx; uint32_t topo, rtv;
    RdSpan<uint8_t> vb2[kMaxVBStreams - 1];
    uint32_t vb2_stride[kMaxVBStreams - 1];
    uint32_t is_clear; float clear_rgba[4];
    float vp[6];
    uint32_t is_copy, copy_dst, copy_src;
    GfxBlend blend;
    GfxDepth depth;
    uint32_t dsv;
    RdSpan<uint8_t> vscb[kMaxCB], pscb[kMaxCB];
    uint32_t pstex[kMaxPSTex], pssamp[kMaxPSTex], psmip[kMaxPSTex];
    float vpw, vph;
    const uint8_t* res[kMaxVBStreams]; uint32_t res_bytes[kMaxVBStreams];   // C50: static buffer behind each stream
    const uint8_t* ib_res; uint32_t ib_res_bytes;                             // BO2_IB_RESIDENT: static index buffer
    uint32_t rs; int32_t sc[4];
    uint32_t dsv_res, dsv_wh;
    int32_t zb; float zb_pente, zb_max;
};
struct RdFrame {
    std::vector<RealDrawRec> recs;
    RdArena arena;
    void clear() { recs.clear(); arena.reset(); }
    void swap(RdFrame& o) noexcept { recs.swap(o.recs); arena.swap(o.arena); }
};
extern std::mutex g_rd_mu;
extern RdFrame g_rd_frame;           // the newest complete frame, taken by the pump
extern uint64_t g_rd_seq, g_rd_frame_seq;
// Frame queue (BO2_FRAME_QUEUE, on unless 0): the sequence number of the last frame the pump took, and the
// condition the game's render thread waits on before it publishes the next one (gfx_real_frame_end).
extern std::atomic<uint64_t> g_rd_taken_seq;
extern std::condition_variable g_rd_cv;
extern std::atomic<uint64_t> g_guest_frames;   // the game's own Present count
extern std::atomic<uint64_t> g_frames;         // presents of the pump

// ---- What the render thread publishes for the guest's queries (gfx_cbmap_for, gfx_texture_dims, ...) -----------
// Maps keyed by the translator's own index (binding minus the stage's base), the numbering the Metal backend's
// "buffer(n)" / "texture(n)" indices had, so the runtime's probes mean the same thing on both backends.
struct ShaderMaps {
    bool pixel = false;
    std::map<uint32_t, uint32_t> cb, tex;   // translator index -> D3D register
};
extern std::mutex g_query_mu;
extern std::unordered_map<uint32_t, ShaderMaps> g_query_shaders;
extern std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_query_texdims;

// Binding bases of dxbc_to_vk.cpp's rebase_bindings.
constexpr uint32_t kPsTexBase = 0, kPsCbBase = 32, kVsTexBase = 64, kVsCbBase = 96, kBindSpan = 32;

// "a=b,c=d" -> pairs
inline std::vector<std::pair<int, int>> parse_pairs(const std::string& s) {
    std::vector<std::pair<int, int>> out;
    for (size_t i = 0; i < s.size();) {
        const size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        size_t cm = s.find(',', eq);
        if (cm == std::string::npos) cm = s.size();
        out.push_back({atoi(s.substr(i, eq - i).c_str()), atoi(s.substr(eq + 1, cm - eq - 1).c_str())});
        i = cm + 1;
    }
    return out;
}

}  // namespace bo2vk
