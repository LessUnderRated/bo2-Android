// vk_resources.cpp — the render thread applies what the guest registered: textures, render targets, RTVs, samplers,
// shaders and their maps; plus the stand-ins, the back buffer and the depth buffers.
#include "vk_render.h"

#include "bc_decode.h"
#include "guest_monitor.h"   // bo2-mac: the monitor the game is told about (its back buffer's size)

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace bo2vk {

std::unordered_map<uint32_t, ShaderRec> g_shaders;
std::unordered_map<uint32_t, ImagePtr> g_tex;
std::set<uint32_t> g_rt_cleared, g_rtv_targets, g_tex_gone;
std::unordered_map<uint32_t, uint32_t> g_rtv_res, g_rtv_mip;
std::unordered_map<uint32_t, SamplerPair> g_samplers;
SamplerPair g_samp_default, g_samp_point;
ImagePtr g_white, g_null2d, g_null3d, g_nullcube, g_nulldepth, g_backbuffer;
Buffer g_zero_ubo, g_zero_vb;
std::unordered_map<uint64_t, ImagePtr> g_depth;

namespace {

bool env_on(const char* n, bool d) { const char* e = getenv(n); return e ? (e[0] && e[0] != '0') : d; }
long env_long(const char* n, long d) { const char* e = getenv(n); return e ? strtol(e, nullptr, 10) : d; }

// BO2_GENMIPS (default on): an uncompressed 2D texture without mips gets a chain (Metal generateMipmaps' place).
// BO2_BCMIPS (default off): a BC texture of 128 or more a side, without mips, is decoded to RGBA8 to get one.
const bool g_genmips = env_on("BO2_GENMIPS", true);
const bool g_bcmips = env_on("BO2_BCMIPS", false);
constexpr uint32_t kBcMipMin = 128;

uint32_t full_chain(uint32_t w, uint32_t h, uint32_t d = 1) {
    uint32_t n = 1;
    for (uint32_t m = std::max(w, std::max(h, d)); m > 1; m >>= 1) n++;
    return n;
}

bool format_ok(VkFormat f, VkFormatFeatureFlags feat) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_dev.phys, f, &p);
    return (p.optimalTilingFeatures & feat) == feat;
}

// A 1x1 (x1) image of `fmt` filled with `texel` (4 bytes), in SHADER_READ_ONLY_OPTIMAL.
ImagePtr solid(VkImageType type, VkImageViewType vtype, uint32_t layers, const uint8_t texel[4], VkImageCreateFlags fl) {
    ImagePtr im = image_create(type, vtype, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, layers, 1,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, fl);
    if (!im) return nullptr;
    Buffer stage = buffer_create(4 * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    for (uint32_t l = 0; l < layers; l++) memcpy(stage.mem.mapped + 4 * l, texel, 4);
    submit_now([&](VkCommandBuffer cb) {
        image_transition(cb, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (uint32_t l = 0; l < layers; l++) {
            VkBufferImageCopy c{};
            c.bufferOffset = 4 * l;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
            c.imageExtent = {1, 1, 1};
            vkCmdCopyBufferToImage(cb, stage.buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        image_transition(cb, *im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    buffer_destroy(stage);
    return im;
}

VkCompareOp d3d_cmp(uint32_t f) {
    switch (f) {
        case 1: return VK_COMPARE_OP_NEVER;
        case 2: return VK_COMPARE_OP_LESS;
        case 3: return VK_COMPARE_OP_EQUAL;
        case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 5: return VK_COMPARE_OP_GREATER;
        case 6: return VK_COMPARE_OP_NOT_EQUAL;
        case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case 8: return VK_COMPARE_OP_ALWAYS;
        default: return VK_COMPARE_OP_LESS;
    }
}

VkSamplerAddressMode d3d_addr(uint32_t a) {
    switch (a) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;   // core since 1.2
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;          // 3 = CLAMP
    }
}

// BO2_ANISO caps the game's anisotropy (default 1 = off), as on macOS.
const float g_aniso_cap = [] { long a = env_long("BO2_ANISO", 1); return (float)(a < 1 ? 1 : a > 16 ? 16 : a); }();

SamplerPair make_sampler(const VkSamplerCreateInfo& base, VkCompareOp cmp) {
    SamplerPair p;
    VkSamplerCreateInfo ci = base;
    ci.compareEnable = VK_FALSE;
    vkCreateSampler(g_dev.dev, &ci, nullptr, &p.plain);
    ci.compareEnable = VK_TRUE;
    ci.compareOp = cmp;
    vkCreateSampler(g_dev.dev, &ci, nullptr, &p.compare);
    return p;
}

// D3D11_SAMPLER_DESC as flattened by the producer: Filter, AddressU, AddressV, AddressW, MipLODBias (float),
// MaxAnisotropy, ComparisonFunc, BorderColor[4] (floats), MinLOD, MaxLOD (floats).
SamplerPair sampler_from_d3d(const std::array<uint32_t, 13>& d) {
    const uint32_t f = d[0];
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.minFilter = (f & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.magFilter = (f & 0x04) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (f & 0x01) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if ((f & 0x40) && d[5] > 1 && g_dev.anisotropy && g_aniso_cap > 1.f) {   // D3D11_FILTER_ANISOTROPIC
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min({(float)d[5], g_aniso_cap, g_dev.max_aniso});
        ci.minFilter = ci.magFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
    ci.addressModeU = d3d_addr(d[1]);
    ci.addressModeV = d3d_addr(d[2]);
    ci.addressModeW = d3d_addr(d[3]);
    float bias, bc[4], lod[2];
    memcpy(&bias, &d[4], 4);
    memcpy(bc, &d[7], sizeof bc);
    memcpy(lod, &d[11], sizeof lod);
    const float maxbias = g_dev.props.limits.maxSamplerLodBias;
    ci.mipLodBias = std::isfinite(bias) ? std::max(-maxbias, std::min(maxbias, bias)) : 0.f;
    // Vulkan's border colours are fixed: the nearest of transparent black, opaque black and opaque white (as macOS).
    ci.borderColor = bc[3] == 0.f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                   : (bc[0] + bc[1] + bc[2] >= 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                    : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
    ci.minLod = 0.f;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    if (lod[0] > 0.f && lod[0] <= lod[1]) ci.minLod = lod[0];
    if (lod[1] >= 0.f && lod[1] >= lod[0] && lod[1] < 1e30f) ci.maxLod = lod[1];
    return make_sampler(ci, (f & 0x80) ? d3d_cmp(d[6]) : VK_COMPARE_OP_LESS_OR_EQUAL);
}

void publish_shader(uint32_t obj, const ShaderRec& r) {
    ShaderMaps m;
    m.pixel = r.pixel;
    const uint32_t cb_base = r.pixel ? kPsCbBase : kVsCbBase, tex_base = r.pixel ? kPsTexBase : kVsTexBase;
    for (auto& kv : r.cb) if (kv.first >= cb_base) m.cb[kv.first - cb_base] = kv.second;
    for (auto& kv : r.tex) if (kv.first >= tex_base) m.tex[kv.first - tex_base] = kv.second;
    std::lock_guard<std::mutex> lk(g_query_mu);
    g_query_shaders[obj] = std::move(m);
}

// Every "layout(set = 0, binding = N...) uniform ..." HLSLcc emitted (after dxbc_to_vk.cpp rebased them).
std::vector<Decl> parse_decls(const std::string& src) {
    std::vector<Decl> out;
    for (size_t p = src.find("layout(set = 0, binding = "); p != std::string::npos;
         p = src.find("layout(set = 0, binding = ", p + 1)) {
        if (p > 0 && src[p - 1] != '\n') continue;
        const uint32_t b = (uint32_t)atoi(src.c_str() + p + 26);
        const size_t eol = src.find('\n', p);
        const std::string line = src.substr(p, eol - p);
        Decl d{b, DescKind::Texture};
        if (line.find('{') != std::string::npos) d.kind = DescKind::Ubo;
        else if (line.find(" samplerShadow ") != std::string::npos) d.kind = DescKind::ShadowSampler;
        else if (line.find(" sampler ") != std::string::npos) d.kind = DescKind::Sampler;
        else if (line.find("texture") == std::string::npos) continue;
        out.push_back(d);
    }
    return out;
}

void apply_map(const std::vector<std::pair<uint32_t, std::string>>& src, int which) {
    for (const auto& kv : src) {
        auto it = g_shaders.find(kv.first);
        if (it == g_shaders.end()) continue;   // never compiled
        ShaderRec& r = it->second;
        for (const auto& pr : parse_pairs(kv.second)) {
            if (pr.first < 0) continue;
            switch (which) {
                case 0: r.tex[(uint32_t)pr.first] = (uint32_t)pr.second; break;
                case 1: r.cb[(uint32_t)pr.first] = (uint32_t)pr.second; break;
                case 2: r.dim[(uint32_t)pr.first] = (uint32_t)pr.second; break;
                default: r.smp[(uint32_t)pr.first] = pr.second; break;
            }
        }
        publish_shader(kv.first, r);
    }
}

// Mips 1.. of `im` from level 0 by successive linear blits; everything ends SHADER_READ_ONLY.
void generate_mips(VkCommandBuffer cb, Image& im) {
    for (uint32_t m = 1; m < im.mips; m++) {
        image_transition(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m - 1, 1);
        image_transition(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, m, 1);
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, im.layers};
        b.srcOffsets[1] = {(int32_t)std::max(1u, im.w >> (m - 1)), (int32_t)std::max(1u, im.h >> (m - 1)),
                           (int32_t)std::max(1u, im.d >> (m - 1))};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, im.layers};
        b.dstOffsets[1] = {(int32_t)std::max(1u, im.w >> m), (int32_t)std::max(1u, im.h >> m),
                           (int32_t)std::max(1u, im.d >> m)};
        vkCmdBlitImage(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &b, VK_FILTER_LINEAR);
    }
    image_transition(cb, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

uint64_t g_tex_uploaded = 0, g_tex_bad = 0, g_tex_decoded = 0;

}  // namespace

void publish_texture_dims(uint32_t obj, const Image* im) {
    std::lock_guard<std::mutex> lk(g_query_mu);
    if (im) g_query_texdims[obj] = {im->w, im->h};
    else g_query_texdims.erase(obj);
}

ImagePtr depth_for(uint32_t w, uint32_t h, bool screen) {
    if (!w || !h) return nullptr;
    const uint64_t k = ((uint64_t)w << 32) | h | ((uint64_t)screen << 31);
    auto it = g_depth.find(k);
    if (it != g_depth.end()) return it->second;
    ImagePtr d = image_create(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, g_dev.depth_fmt, w, h, 1, 1, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    g_depth[k] = d;
    static int n = 0;
    if (d && ++n <= 8) fprintf(stderr, "[VK] depth buffer %ux%u%s\n", w, h, screen ? " (screen)" : "");
    return d;
}

bool resources_init() {
    const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 0};
    g_white = solid(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, white, 0);
    g_null2d = solid(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, black, 0);
    g_null3d = solid(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, black, 0);
    g_nullcube = solid(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, black, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    // A 1x1 depth at 1.0 (the far plane), for a slot sampled with a comparison sampler and nothing depth-like bound.
    g_nulldepth = image_create(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, g_dev.depth_fmt, 1, 1, 1, 1, 1,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!g_white || !g_null2d || !g_null3d || !g_nullcube || !g_nulldepth) return false;
    submit_now([](VkCommandBuffer cb) {
        image_transition(cb, *g_nulldepth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue v{1.f, 0};
        VkImageSubresourceRange r{g_nulldepth->aspect, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, g_nulldepth->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        image_transition(cb, *g_nulldepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    // Constant buffers with nothing bound read zeros; so do vertex inputs the input layout does not provide.
    const VkDeviceSize ubo = std::min<VkDeviceSize>(65536, g_dev.props.limits.maxUniformBufferRange);
    g_zero_ubo = buffer_create(ubo, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    g_zero_vb = buffer_create(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    if (!g_zero_ubo.buf || !g_zero_vb.buf || !g_zero_ubo.mem.mapped || !g_zero_vb.mem.mapped) return false;
    memset(g_zero_ubo.mem.mapped, 0, (size_t)ubo);
    memset(g_zero_vb.mem.mapped, 0, 64);
    // The default sampler: trilinear, anisotropy BO2_ANISO (default off), as macOS's g_samp.
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.minFilter = ci.magFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    if (g_aniso_cap > 1.f && g_dev.anisotropy) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(g_aniso_cap, g_dev.max_aniso);
    }
    g_samp_default = make_sampler(ci, VK_COMPARE_OP_LESS_OR_EQUAL);
    ci.anisotropyEnable = VK_FALSE;
    ci.minFilter = ci.magFilter = VK_FILTER_NEAREST;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    g_samp_point = make_sampler(ci, VK_COMPARE_OP_LESS_OR_EQUAL);
    // The back buffer: the guest monitor's size (guest_monitor.h), BGRA8 as the macOS drawable.
    g_backbuffer = image_create(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, kGuestMonitorW,
                                kGuestMonitorH, 1, 1, 1,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!g_backbuffer) return false;
    g_backbuffer->render_target = true;
    g_backbuffer->dxgi = 87;
    return true;
}

void resources_shutdown() {
    for (auto& kv : g_samplers) { vkDestroySampler(g_dev.dev, kv.second.plain, nullptr); vkDestroySampler(g_dev.dev, kv.second.compare, nullptr); }
    g_samplers.clear();
    for (SamplerPair* p : {&g_samp_default, &g_samp_point}) {
        if (p->plain) vkDestroySampler(g_dev.dev, p->plain, nullptr);
        if (p->compare) vkDestroySampler(g_dev.dev, p->compare, nullptr);
        *p = SamplerPair{};
    }
    for (auto& kv : g_shaders) if (kv.second.module) vkDestroyShaderModule(g_dev.dev, kv.second.module, nullptr);
    g_shaders.clear();
    auto drop = [](ImagePtr& p) { if (p) { image_destroy(*p); p.reset(); } };
    for (auto& kv : g_tex) drop(kv.second);
    g_tex.clear();
    for (auto& kv : g_depth) drop(kv.second);
    g_depth.clear();
    for (ImagePtr* p : {&g_white, &g_null2d, &g_null3d, &g_nullcube, &g_nulldepth, &g_backbuffer}) drop(*p);
    buffer_destroy(g_zero_ubo);
    buffer_destroy(g_zero_vb);
}

// One registered texture into a VkImage: format, CPU BC decode where needed, own mips or a generated chain.
void upload_texture(VkCommandBuffer cb, uint32_t obj, TexStage& ts) {
    const bool cube = ts.faces == 6, vol = ts.depth > 1;
    if (!cube && !vol && g_tex.count(obj)) return;   // as macOS: a 2D object registered once keeps its first image
    VkComponentMapping swz;
    VkFormat vf = dxgi_to_vk(ts.fmt, &swz, false);
    if (vf == VK_FORMAT_UNDEFINED) return;
    uint32_t mips = ts.mips > 1 ? ts.mips : 1;
    const bool bc = dxgi_is_bc(ts.fmt);
    // Levels as (offset, pitch) per subresource, layer-major as D3D (face * mips + mip).
    const uint32_t layers = cube ? 6 : 1;
    static std::vector<uint32_t> off, pitch;   // render thread only: kept from upload to upload
    off.assign((size_t)layers * mips, 0);
    pitch.assign((size_t)layers * mips, 0);
    if (ts.mip_off.size() == (size_t)layers * mips) {
        for (size_t i = 0; i < off.size(); i++) { off[i] = ts.mip_off[i]; pitch[i] = ts.mip_pitch[i]; }
    } else {
        mips = 1;
        off.assign(layers, 0); pitch.assign(layers, ts.pitch);
        const uint32_t rows = bc ? (ts.h + 3) / 4 : ts.h;
        const uint32_t slice = ts.slice_pitch ? ts.slice_pitch : ts.pitch * rows;
        for (uint32_t l = 0; l < layers; l++) off[l] = l * slice;
    }
    // CPU decode: a GPU without BC, or BO2_BCMIPS for a large BC texture without its own chain.
    std::vector<std::vector<uint8_t>> decoded;
    bool decode = bc && (!g_dev.bc || (g_bcmips && !cube && !vol && mips == 1 && ts.w >= kBcMipMin && ts.h >= kBcMipMin));
    if (decode) {
        bool sgn = false;
        decoded.resize(off.size());
        for (uint32_t l = 0; l < layers && decode; l++)
            for (uint32_t m = 0; m < mips && decode; m++) {
                const size_t i = (size_t)l * mips + m;
                const uint32_t lw = std::max(1u, ts.w >> m), lh = std::max(1u, ts.h >> m);
                if (off[i] > ts.bytes.size() ||
                    !bc_decode(ts.fmt, ts.bytes.data() + off[i], ts.bytes.size() - off[i], lw, lh, pitch[i], decoded[i], &sgn))
                    decode = false;
                else { off[i] = 0; pitch[i] = lw * 4; }
            }
        if (!decode) { g_tex_bad++; return; }
        vf = sgn ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
        g_tex_decoded++;
    }
    const bool block = bc && !decode;
    const uint32_t bpb = block ? dxgi_bytes_per_block(ts.fmt) : (decode ? 4 : dxgi_bytes_per_block(ts.fmt));
    if (!bpb) return;
    // A generated chain: uncompressed (after decode too) and without its own levels, 8 or more a side (2D), or a cube.
    uint32_t gen = 1;
    const bool blit_ok = format_ok(vf, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (!block && mips == 1 && blit_ok &&
        ((cube) || (!vol && g_genmips && ts.w >= 8 && ts.h >= 8) || (decode && g_bcmips)))
        gen = full_chain(ts.w, ts.h);
    const uint32_t total_mips = std::max(mips, gen);
    if (!format_ok(vf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        static int n = 0;
        if (++n <= 8) fprintf(stderr, "[VK-TEX] %08x: format %d (DXGI %u) cannot be sampled here\n", obj, (int)vf, ts.fmt);
        g_tex_bad++;
        return;
    }
    // A volume or cube the game rewrites (one 3D texture is updated every frame) goes into its existing image when
    // the shape is unchanged: a new image, memory and view per frame, and the old one's destruction, cost the render
    // thread more than the copy. The layout change below is a barrier after every earlier use on this queue.
    ImagePtr im;
    bool reused = false;
    if (cube || vol) {
        if (auto old = g_tex.find(obj); old != g_tex.end() && old->second) {
            const Image& o = *old->second;
            if (!o.render_target && o.fmt == vf && o.w == ts.w && o.h == ts.h && o.d == (vol ? ts.depth : 1) &&
                o.layers == layers && o.mips == total_mips && o.dxgi == ts.fmt && o.swz.r == swz.r && o.swz.g == swz.g &&
                o.swz.b == swz.b && o.swz.a == swz.a) {
                im = old->second;
                reused = true;
            }
        }
    }
    if (!im)
        im = image_create(vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                          vol ? VK_IMAGE_VIEW_TYPE_3D : cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, vf,
                          ts.w, ts.h, vol ? ts.depth : 1, layers, total_mips,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
    if (!im) { g_tex_bad++; return; }
    im->swz = swz;
    im->dxgi = ts.fmt;
    static std::vector<VkBufferImageCopy> copies;
    copies.clear();
    for (uint32_t l = 0; l < layers; l++)
        for (uint32_t m = 0; m < mips; m++) {
            const size_t i = (size_t)l * mips + m;
            const uint32_t lw = std::max(1u, ts.w >> m), lh = std::max(1u, ts.h >> m);
            const uint32_t ld = vol ? std::max(1u, ts.depth >> m) : 1;
            const uint32_t rows = block ? (lh + 3) / 4 : lh;
            const uint32_t row_min = block ? (lw + 3) / 4 * bpb : lw * bpb;
            const uint32_t p = pitch[i] ? pitch[i] : row_min;
            const uint32_t slice = vol && ts.slice_pitch ? ts.slice_pitch : p * rows;
            const uint8_t* src = decode ? decoded[i].data() : ts.bytes.data() + off[i];
            const size_t avail = decode ? decoded[i].size() : (off[i] <= ts.bytes.size() ? ts.bytes.size() - off[i] : 0);
            const size_t need = (size_t)slice * (ld - 1) + (size_t)p * (rows - 1) + row_min;
            if (p < row_min || p % bpb || need > avail) {
                static int n = 0;
                if (++n <= 12)
                    fprintf(stderr, "[VK-TEX] %08x %ux%u DXGI %u level %u: pitch %u for rows of %u, %zu of %zu bytes: "
                                    "skipped (would shear)\n", obj, lw, lh, ts.fmt, m, p, row_min, avail, need);
                g_tex_bad++;
                return;
            }
            const VkDeviceSize at = ring_push(src, need, 16);
            if (at == ~0ull) { g_tex_bad++; return; }
            VkBufferImageCopy c{};
            c.bufferOffset = at;
            c.bufferRowLength = block ? p / bpb * 4 : p / bpb;
            c.bufferImageHeight = vol ? slice / p * (block ? 4 : 1) : 0;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1};
            c.imageExtent = {lw, lh, ld};
            copies.push_back(c);
        }
    image_transition(cb, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, ring_buffer(), im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(),
                           copies.data());
    if (total_mips > mips) generate_mips(cb, *im);
    else image_transition(cb, *im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (auto old = g_tex.find(obj); !reused && old != g_tex.end() && old->second) {
        ImagePtr dead = old->second;
        destroy_later([dead] { image_destroy(*dead); });
    }
    g_tex[obj] = im;
    publish_texture_dims(obj, im.get());
    g_tex_uploaded++;
}

void apply_pending(VkCommandBuffer cb) {
    // Textures.
    {
        // `take` keeps its capacity (render thread only), and the uploaded bytes (already copied into the ring) go
        // back to the producers: no allocation per registration in the steady state.
        // A per-frame budget (BO2_VK_TEX_BUDGET_MS, default 4; 0 = none): Tranzit streams an area's assets in the
        // middle of a match, and hundreds of uploads (copy, image, mip chain) in one frame stalled the render thread
        // for 300-500 ms on the S26. Past the budget the rest waits for the next frames, in order, at the FRONT of
        // the queue; a draw that needs one of them meanwhile sees it a few frames late (pop-in, not a freeze).
        // An upload queued before its texture was released is dropped (texstage_stale), never resurrected.
        static const double budget_ms = [] {
            const char* e = getenv("BO2_VK_TEX_BUDGET_MS");
            return e ? atof(e) : 4.0;
        }();
        static std::vector<std::pair<uint32_t, TexStage>> take;
        { std::lock_guard<std::mutex> lk(g_texreg_mu); take.swap(g_texreg_pending); }
        static std::vector<uint8_t> stale;
        stale.assign(take.size(), 0);
        if (!take.empty()) {
            std::lock_guard<std::mutex> lk(g_texreg_mu);
            for (size_t i = 0; i < take.size(); i++) stale[i] = texstage_stale(take[i].first, take[i].second.gen);
        }
        const auto t0 = std::chrono::steady_clock::now();
        size_t done = 0, uploaded = 0;
        static std::vector<uint8_t> ok;   // 1: this entry's image now holds its bytes
        ok.assign(take.size(), 0);
        for (; done < take.size(); done++) {
            if (stale[done]) continue;
            if (uploaded > 0 && budget_ms > 0 &&
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budget_ms)
                break;
            const uint64_t before = g_tex_uploaded;
            upload_texture(cb, take[done].first, take[done].second);
            ok[done] = g_tex_uploaded != before;
            uploaded++;
        }
        if (!take.empty()) {
            std::lock_guard<std::mutex> lk(g_texreg_mu);
            const size_t ndone = done;
            if (done < take.size()) {
                // What the game registered meanwhile goes after the leftovers (first: texstage_keep must see them).
                g_texreg_pending.insert(g_texreg_pending.begin(), std::make_move_iterator(take.begin() + done),
                                        std::make_move_iterator(take.end()));
            }
            for (size_t i = 0; i < ndone; i++) {
                TexStage& t = take[i].second;
                const bool keepable = t.depth > 1 || t.faces == 6;
                if (!keepable || !texstage_keep(take[i].first, t.gen, ok[i] != 0, std::move(t.bytes)))
                    texstage_recycle(std::move(t.bytes));
            }
            if (done < take.size()) {
                static uint64_t deferred_frames = 0;
                const uint64_t k = ++deferred_frames;
                if ((k & (k - 1)) == 0)
                    fprintf(stderr, "[VK-TEX] upload budget %.1f ms: %zu uploaded this frame, %zu wait (deferral #%llu)\n",
                            budget_ms, uploaded, take.size() - done, (unsigned long long)k);
            }
        }
        take.clear();
        static uint64_t last = 0;
        if (g_tex_uploaded / 256 != last) {
            last = g_tex_uploaded / 256;
            fprintf(stderr, "[VK-TEX] %llu textures uploaded (%llu decoded on the CPU, %llu refused)\n",
                    (unsigned long long)g_tex_uploaded, (unsigned long long)g_tex_decoded, (unsigned long long)g_tex_bad);
        }
    }
    // Render targets and RTVs.
    {
        std::vector<RTStage> rts;
        std::vector<std::pair<uint32_t, uint32_t>> rtvs, rtv_mips;
        {
            std::lock_guard<std::mutex> lk(g_rt_mu);
            rts.swap(g_rt_pending); rtvs.swap(g_rtv_pending); rtv_mips.swap(g_rtv_mip_pending);
        }
        for (auto& kv : rtvs) { g_rtv_res[kv.first] = kv.second; g_rtv_targets.insert(kv.second); }
        for (auto& kv : rtv_mips) g_rtv_mip[kv.first] = kv.second;
        for (const RTStage& t : rts) {
            if (g_tex.count(t.obj) || !t.w || !t.h) continue;
            VkFormat vf = dxgi_to_vk(t.fmt, nullptr, true);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if (vf == VK_FORMAT_UNDEFINED || !format_ok(vf, need)) {
                static int n = 0;
                if (++n <= 8) fprintf(stderr, "[VK-RT] %08x DXGI %u: no colour attachment format here, BGRA8 instead\n", t.obj, t.fmt);
                vf = VK_FORMAT_B8G8R8A8_UNORM;
            }
            const uint32_t full = full_chain(t.w, t.h);
            const uint32_t mips = t.mips == 0 || t.mips > full ? full : t.mips;
            ImagePtr im = image_create(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, vf, t.w, t.h, 1, 1, mips,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (!im) continue;
            im->render_target = true;
            im->dxgi = t.fmt;
            // Zeroed at creation, every level (macOS clears new render targets too).
            image_transition(cb, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue zero{};
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
            vkCmdClearColorImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &r);
            image_transition(cb, *im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_tex[t.obj] = im;
            g_rt_cleared.insert(t.obj);
            publish_texture_dims(t.obj, im.get());
        }
    }
    // Shaders (compiled in parallel), then their maps, then samplers.
    {
        std::vector<ShaderStage> take;
        std::vector<std::pair<uint32_t, std::string>> tslots, cbslots, tdims, smpslots;
        std::vector<std::pair<uint32_t, std::array<uint32_t, 13>>> smps;
        {
            std::lock_guard<std::mutex> lk(g_shader_mu);
            take.swap(g_shader_pending); tslots.swap(g_texslot_pending); cbslots.swap(g_cbslot_pending);
            tdims.swap(g_texdim_pending); smpslots.swap(g_smpslot_pending); smps.swap(g_sampler_pending);
        }
        std::vector<std::vector<uint32_t>> spv(take.size());
        std::vector<std::string> why(take.size());
        std::vector<char> ok(take.size(), 0);
        {
            const unsigned nthreads = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), 8));
            std::atomic<size_t> next{0};
            auto work = [&] {
                for (size_t i; (i = next.fetch_add(1)) < take.size();)
                    if (!g_shaders.count(take[i].obj)) ok[i] = glsl_to_spirv(take[i].is_pixel, take[i].src, spv[i], why[i]);
            };
            std::vector<std::thread> pool;
            for (unsigned t = 1; t < nthreads && t < take.size(); t++) pool.emplace_back(work);
            work();
            for (auto& t : pool) t.join();
        }
        static uint64_t n_ok = 0, n_bad = 0;
        for (size_t i = 0; i < take.size(); i++) {
            const ShaderStage& st = take[i];
            if (g_shaders.count(st.obj)) continue;
            if (!ok[i]) {
                if (++n_bad <= 8)
                    fprintf(stderr, "[VK-SHADER] %s %08x does not compile: %.400s\n", st.is_pixel ? "PS" : "VS", st.obj,
                            why[i].c_str());
                continue;
            }
            ShaderRec r;
            r.pixel = st.is_pixel;
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = spv[i].size() * 4;
            mi.pCode = spv[i].data();
            if (vkCreateShaderModule(g_dev.dev, &mi, nullptr, &r.module) != VK_SUCCESS) continue;
            r.decls = parse_decls(st.src);
            for (size_t a = 0; a < st.attrs.size();) {
                const size_t eq = st.attrs.find('=', a);
                if (eq == std::string::npos) break;
                size_t cm = st.attrs.find(',', eq);
                if (cm == std::string::npos) cm = st.attrs.size();
                r.attrs[st.attrs.substr(a, eq - a)] = (uint32_t)atoi(st.attrs.substr(eq + 1, cm - eq - 1).c_str());
                a = cm + 1;
            }
            g_shaders[st.obj] = std::move(r);
            publish_shader(st.obj, g_shaders[st.obj]);
            if (++n_ok <= 3 || n_ok % 200 == 0)
                fprintf(stderr, "[VK-SHADER] %llu compiled (%llu failed)\n", (unsigned long long)n_ok, (unsigned long long)n_bad);
        }
        apply_map(tslots, 0);
        apply_map(cbslots, 1);
        apply_map(tdims, 2);
        apply_map(smpslots, 3);
        for (auto& kv : smps) {
            auto old = g_samplers.find(kv.first);
            if (old != g_samplers.end()) {
                SamplerPair dead = old->second;
                destroy_later([dead] {
                    descriptor_epoch_bump();   // cached descriptor sets may name these samplers
                    vkDestroySampler(g_dev.dev, dead.plain, nullptr);
                    vkDestroySampler(g_dev.dev, dead.compare, nullptr);
                });
            }
            g_samplers[kv.first] = sampler_from_d3d(kv.second);
        }
    }
}

}  // namespace bo2vk
