// vk_render.cpp — the render thread: gfx_pump_once replays the newest frame of draws on Vulkan and presents it.
//
// The replay follows bo2-mac's metal_backend.mm pass by pass: consecutive draws aimed at one render target are one
// pass (a stencil clear between two groups that share the plane cuts the pass, as on macOS); each pass attaches the
// target's level, a depth buffer of its size (cleared to the engine's value at its first pass of the frame) and, when
// a draw tests stencil, the stencil plane, cleared when the game cleared it. Draws with no DSV bound do not test depth.
// Where Vulkan needs more than Metal did, it is said where it happens: barriers (every sampled texture is moved to
// SHADER_READ before the pass begins, so a pass is planned before it is recorded), the vertex inputs a layout does not
// provide (read zeros), shadow samplers (a comparison twin of each game sampler).
// Differences from macOS, on purpose: the viewport and scissor are set per draw (D3D's semantics; macOS sets the
// viewport once per pass unless BO2_VP_DRAW), and the game renders into its own back buffer at the guest monitor's
// size, which is then drawn into the display plan's rectangle (macOS draws to the drawable with a letterbox).
#include "vk_render.h"

#include "vk_hooks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <numeric>
#include <string_view>
#include <thread>
#include <unordered_set>
#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#endif

extern "C" float gfx_depth_clear_value();   // remaining_dlls_impl.cpp: the value the engine clears depth with

namespace bo2vk {

namespace {

bool env_on(const char* n, bool d) { const char* e = getenv(n); return e ? (e[0] && e[0] != '0') : d; }

// ---- D3D11 state to Vulkan ---------------------------------------------------------------------------------------
VkBlendFactor blend_factor(uint32_t b) {
    switch (b) {
        case 1: return VK_BLEND_FACTOR_ZERO;
        case 2: return VK_BLEND_FACTOR_ONE;
        case 3: return VK_BLEND_FACTOR_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blend_op(uint32_t o) {
    switch (o) {
        case 2: return VK_BLEND_OP_SUBTRACT;
        case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4: return VK_BLEND_OP_MIN;
        case 5: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}
VkCompareOp cmp_op(uint32_t f) {
    static const VkCompareOp t[9] = {VK_COMPARE_OP_LESS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                     VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                     VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return f <= 8 ? t[f] : VK_COMPARE_OP_LESS;
}
VkStencilOp stencil_op(uint32_t o) {   // D3D11_STENCIL_OP 1..8 = Vulkan's order
    static const VkStencilOp t[9] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                     VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                     VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                     VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return o <= 8 ? t[o] : VK_STENCIL_OP_KEEP;
}
bool uses_src1(uint32_t f) { return f >= 16 && f <= 19; }

uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }

// ---- Pipeline layouts per shader pair, pipelines per state --------------------------------------------------------
struct Layout {
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bool dyn = false;   // constant buffers are UNIFORM_BUFFER_DYNAMIC (their ring offsets go to the bind, not the set)
    bool stable = false;   // its sets name no per-draw ring offset (dynamic or no constant buffers): cacheable across frames
};

// ---- Draw-recording switches (docs/GRAPHICS.md, "Draw-call cost") ---------------------------------------------------
// BO2_VK_EARLY_SCISSOR (on): a draw whose scissor rectangle is empty is dropped before its pipeline and descriptors are
//   looked up (and before a missing pipeline is queued for building).
// BO2_VK_STATE_FILTER (on): within a pass, a bind or a dynamic state equal to what the previous draw set is not
//   recorded again. Every pipeline here has the same dynamic states, so binding one keeps the others' values; the
//   filter starts afresh at each vkCmdBeginRendering.
// BO2_VK_RING_BASE (on): the index buffer, and the vertex buffer of a draw with one stream, are bound at their buffer's
//   start and the draw finds its data with firstIndex / vertexOffset: consecutive draws then share one binding.
// BO2_VK_SET_CACHE: 0 a descriptor set per draw; 1 sets reused within the frame; 2 (default) sets of layouts with no
//   per-draw offset in them kept across frames: a kept set is forgotten when an image it names is destroyed, and all
//   of them when a sampler or a ring buffer is (rare).
const bool g_early_scissor = env_on("BO2_VK_EARLY_SCISSOR", true);
const bool g_state_filter = env_on("BO2_VK_STATE_FILTER", true);
const bool g_ring_base = env_on("BO2_VK_RING_BASE", true);
const int g_set_cache_mode = [] {
    const char* e = getenv("BO2_VK_SET_CACHE");
    if (!e || !*e) return 2;
    return e[0] == '0' ? 0 : e[0] == '1' ? 1 : 2;
}();
uint64_t g_cmd_made = 0, g_cmd_skipped = 0;   // [VK-FRAME]: binds and state commands recorded / filtered out
std::unordered_map<uint64_t, Layout> g_layouts;
std::unordered_map<uint64_t, VkPipeline> g_pipes;   // VK_NULL_HANDLE = refused, not retried
uint64_t g_pipes_built = 0, g_pipes_refused = 0, g_draws = 0, g_skipped = 0;
uint64_t g_skip_why[12] = {0};   // 0 shader, 1 attr not in VS, 2 vertex format, 3 pipeline, 4 topology,
                                 // 5 no target, 6 scissor empty, 7 buffers, 8 descriptors, 9 mip,
                                 // 10 pipeline still being built (async)

VkDescriptorType desc_type(DescKind k) {
    switch (k) {
        case DescKind::Ubo: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescKind::Texture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        default: return VK_DESCRIPTOR_TYPE_SAMPLER;
    }
}

// Dynamic constant buffers (BO2_VK_DYN_UBO, default on). Every draw's constants sit at their own offset in the frame's
// ring, so a set that names that offset is only good for that draw: the per-frame set cache reused ~19% of sets on
// the S26. With UNIFORM_BUFFER_DYNAMIC the set names the ring buffer at offset 0 and the draw passes its offsets to
// vkCmdBindDescriptorSets: draws with the same textures, samplers and constant-buffer sizes share one set. A layout
// with more constant buffers than the device allows dynamic (maxDescriptorSetUniformBuffersDynamic) stays plain.
// The type is fixed per layout, and a layout belongs to one shader pair, as its pipelines do (pipeline_for's key
// starts with that pair), so a pipeline is never used with the other kind of layout.
const bool g_dyn_ubo = env_on("BO2_VK_DYN_UBO", true);

Layout* layout_for(uint32_t vs, uint32_t ps, const ShaderRec& v, const ShaderRec& p) {
    const uint64_t key = ((uint64_t)vs << 32) | ps;
    auto it = g_layouts.find(key);
    if (it != g_layouts.end()) return &it->second;
    Layout L;
    uint32_t ubos = 0;
    for (const ShaderRec* s : {&v, &p})
        for (const Decl& d : s->decls) ubos += d.kind == DescKind::Ubo;
    L.dyn = g_dyn_ubo && ubos > 0 && ubos <= g_dev.props.limits.maxDescriptorSetUniformBuffersDynamic &&
            ubos <= 2 * kMaxCB;
    L.stable = L.dyn || ubos == 0;
    static bool said = false;
    if (!said) {
        said = true;
        fprintf(stderr, "[VK-SETS] dynamic constant buffers %s (device: %u per set, offset alignment %llu, range %u); "
                        "set cache %s; state filter %s; ring base binding %s; early scissor %s\n",
                g_dyn_ubo ? "on" : "off (BO2_VK_DYN_UBO=0)", g_dev.props.limits.maxDescriptorSetUniformBuffersDynamic,
                (unsigned long long)g_dev.props.limits.minUniformBufferOffsetAlignment,
                g_dev.props.limits.maxUniformBufferRange,
                g_set_cache_mode >= 2 ? "across frames" : g_set_cache_mode == 1 ? "within the frame" : "off",
                g_state_filter ? "on" : "off", g_ring_base ? "on" : "off", g_early_scissor ? "on" : "off");
    }
    for (const ShaderRec* s : {&v, &p})
        for (const Decl& d : s->decls) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = d.binding;
            b.descriptorType = d.kind == DescKind::Ubo && L.dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                                 : desc_type(d.kind);
            b.descriptorCount = 1;
            b.stageFlags = s->pixel ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            L.bindings.push_back(b);
        }
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    di.bindingCount = (uint32_t)L.bindings.size();
    di.pBindings = L.bindings.data();
    vkCreateDescriptorSetLayout(g_dev.dev, &di, nullptr, &L.dsl);
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &L.dsl;
    vkCreatePipelineLayout(g_dev.dev, &pi, nullptr, &L.pl);
    return &(g_layouts[key] = std::move(L));
}

const VkDynamicState kDynamic[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};

// Vertex streams used by a draw: slot -> stride. Slot 4 is the zero stream for inputs the layout does not provide.
constexpr uint32_t kZeroStream = kMaxVBStreams;

// ---- Pipeline creation, off the render thread ----------------------------------------------------------------------
// A pipeline the driver has not seen costs tens to hundreds of milliseconds on a phone (287-541 ms render-thread stalls
// were measured on the S26 when a new effect first appeared mid-match). So a missing pipeline is built by two worker
// threads while its draws are skipped; the next frames use it. Everything the creation reads is copied into the job;
// the shader modules and the pipeline layout it names live until render_shutdown_pipelines, which joins the workers
// before destroying anything. VkPipelineCache is internally synchronized, so the workers share the device's cache.
// BO2_VK_ASYNC_PIPE=0 builds pipelines on the render thread, as before.
struct PipeJob {
    uint64_t key = 0;
    uint32_t vs = 0, ps = 0;   // the guest's shader objects, for the log
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    VkPipelineColorBlendAttachmentState ca{};
    VkFormat color = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED, stencil = VK_FORMAT_UNDEFINED;
    bool clamp = false;
    VkShaderModule vsm = VK_NULL_HANDLE, psm = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
};
struct PipeDone { uint64_t key; uint32_t vs, ps; VkPipeline p; };

VkPipeline create_pipeline(const PipeJob& j) {
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t)j.binds.size();
    vi.pVertexBindingDescriptions = j.binds.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)j.attrs.size();
    vi.pVertexAttributeDescriptions = j.attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // list or strip, set per draw (same topology class)
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = j.clamp ? VK_TRUE : VK_FALSE;   // D3D DepthClipEnable = FALSE
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = j.color != VK_FORMAT_UNDEFINED ? 1 : 0;
    cb.pAttachments = &j.ca;
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = (uint32_t)(sizeof kDynamic / sizeof kDynamic[0]);
    dy.pDynamicStates = kDynamic;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = j.color != VK_FORMAT_UNDEFINED ? 1 : 0;
    ri.pColorAttachmentFormats = &j.color;
    ri.depthAttachmentFormat = j.depth;
    ri.stencilAttachmentFormat = j.stencil;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, j.vsm, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, j.psm, "main"};
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = j.pl;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev.dev, g_dev.pipeline_cache, 1, &gi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    pipeline_cache_note_new();   // atomic: the cache's save waits for a quiet spell after the last one
    return p;
}

const bool g_async_pipes = env_on("BO2_VK_ASYNC_PIPE", true);
std::mutex g_pj_mu;
std::condition_variable g_pj_cv;
std::deque<PipeJob> g_pj_queue;
std::vector<PipeDone> g_pj_done;
std::atomic<uint32_t> g_pj_ndone{0};
std::vector<std::thread> g_pj_threads;
bool g_pj_stop = false;
std::unordered_set<uint64_t> g_pipes_pending;   // render thread only: keys a worker is building

void pj_worker() {
#if defined(__ANDROID__) || defined(__linux__)
    pthread_setname_np(pthread_self(), "bo2_vkpipe");
#endif
    for (;;) {
        PipeJob j;
        {
            std::unique_lock<std::mutex> lk(g_pj_mu);
            g_pj_cv.wait(lk, [] { return g_pj_stop || !g_pj_queue.empty(); });
            if (g_pj_stop) return;   // shutting down: jobs not started are dropped
            j = std::move(g_pj_queue.front());
            g_pj_queue.pop_front();
        }
        const VkPipeline p = create_pipeline(j);
        std::lock_guard<std::mutex> lk(g_pj_mu);
        g_pj_done.push_back({j.key, j.vs, j.ps, p});
        g_pj_ndone.fetch_add(1, std::memory_order_release);
    }
}

// A finished pipeline (or a refusal), on the render thread: into the table, counted and logged as before.
void pipe_result(uint64_t key, uint32_t vs, uint32_t ps, VkPipeline p) {
    if (!p) {
        if (++g_pipes_refused <= 8) fprintf(stderr, "[VK-PIPE] vs %08x ps %08x: vkCreateGraphicsPipelines failed\n", vs, ps);
    } else if (++g_pipes_built <= 5 || g_pipes_built % 100 == 0) {
        fprintf(stderr, "[VK-PIPE] %llu pipelines (%llu refused)%s\n", (unsigned long long)g_pipes_built,
                (unsigned long long)g_pipes_refused, g_async_pipes ? ", built off the render thread" : "");
    }
    g_pipes[key] = p;
}

void pj_drain() {
    if (!g_pj_ndone.load(std::memory_order_acquire)) return;
    static std::vector<PipeDone> take;
    {
        std::lock_guard<std::mutex> lk(g_pj_mu);
        take.swap(g_pj_done);
        g_pj_ndone.store(0, std::memory_order_relaxed);
    }
    for (const PipeDone& d : take) {
        g_pipes_pending.erase(d.key);
        pipe_result(d.key, d.vs, d.ps, d.p);
    }
    take.clear();
}

void pj_submit(PipeJob&& j) {
    if (g_pj_threads.empty())
        for (int i = 0; i < 2; i++) g_pj_threads.emplace_back(pj_worker);
    g_pipes_pending.insert(j.key);
    {
        std::lock_guard<std::mutex> lk(g_pj_mu);
        g_pj_queue.push_back(std::move(j));
    }
    g_pj_cv.notify_one();
}

void pj_stop() {
    {
        std::lock_guard<std::mutex> lk(g_pj_mu);
        g_pj_stop = true;
        g_pj_queue.clear();
    }
    g_pj_cv.notify_all();
    for (std::thread& t : g_pj_threads) t.join();
    g_pj_threads.clear();
    g_pj_ndone.store(1);
    pj_drain();               // what finished goes into g_pipes, to be destroyed with the rest
    g_pipes_pending.clear();
    std::lock_guard<std::mutex> lk(g_pj_mu);
    g_pj_stop = false;        // a backend opened again starts its workers afresh
}

// *waiting: the pipeline is being built (the draw is skipped this frame, not refused).
VkPipeline pipeline_for(const RealDrawRec& r, const ShaderRec& vs, const ShaderRec& ps, Layout& L, VkFormat color,
                        bool stencil, bool clamp, bool* reject_attr, bool* waiting) {
    pj_drain();
    uint64_t key = mix(((uint64_t)r.vs << 32) | r.ps, r.vb_stride);
    for (const GfxDrawElem& e : r.elems) {
        key = mix(key, std::hash<std::string_view>{}(std::string_view(e.semantic, strnlen(e.semantic, sizeof e.semantic))));
        key = mix(key, ((uint64_t)e.fmt << 40) | ((uint64_t)e.offset << 8) | e.slot);
    }
    for (uint32_t s = 0; s < kMaxVBStreams - 1; s++) key = mix(key, r.vb2_stride[s]);
    key = mix(key, (uint64_t)color);
    key = mix(key, ((uint64_t)stencil << 1) | (uint64_t)clamp);
    const GfxBlend& b = r.blend;
    key = mix(key, ((uint64_t)b.enable << 56) | ((uint64_t)b.src_rgb << 48) | ((uint64_t)b.dst_rgb << 40) |
                       ((uint64_t)b.op_rgb << 32) | ((uint64_t)b.src_a << 24) | ((uint64_t)b.dst_a << 16) |
                       ((uint64_t)b.op_a << 8) | b.write_mask);
    auto it = g_pipes.find(key);
    if (it != g_pipes.end()) return it->second;
    if (g_pipes_pending.count(key)) { *waiting = true; return VK_NULL_HANDLE; }

    // Vertex input: each element to the location the VS declares for its semantic (macOS refuses the pipeline when
    // the VS does not declare one, and so does this); VS inputs the layout leaves out read the zero stream.
    PipeJob j;
    j.key = key; j.vs = r.vs; j.ps = r.ps;
    bool used[kMaxVBStreams + 1] = {false};
    std::set<uint32_t> provided;
    for (const GfxDrawElem& e : r.elems) {
        const std::string sem(e.semantic, strnlen(e.semantic, sizeof e.semantic));
        auto a = vs.attrs.find(sem);
        const VkFormat vf = dxgi_to_vertex_format(e.fmt);
        if (a == vs.attrs.end()) { *reject_attr = true; g_pipes[key] = VK_NULL_HANDLE; g_skip_why[1]++; return VK_NULL_HANDLE; }
        if (vf == VK_FORMAT_UNDEFINED) { g_pipes[key] = VK_NULL_HANDLE; g_skip_why[2]++; return VK_NULL_HANDLE; }
        if (provided.count(a->second)) continue;   // two elements for one location: the first wins
        const uint32_t sl = e.slot < kMaxVBStreams ? e.slot : 0;
        j.attrs.push_back({a->second, sl, vf, e.offset});
        provided.insert(a->second);
        used[sl] = true;
    }
    for (auto& kv : vs.attrs)
        if (!provided.count(kv.second)) {
            j.attrs.push_back({kv.second, kZeroStream, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
            used[kZeroStream] = true;
        }
    for (uint32_t sl = 0; sl <= kZeroStream; sl++) {
        if (!used[sl]) continue;
        uint32_t stride = sl == 0 ? r.vb_stride : sl == kZeroStream ? 0 : r.vb2_stride[sl - 1];
        if (sl != kZeroStream && !stride) stride = 4;   // as macOS: a stream with no stride reads 4-byte steps
        j.binds.push_back({sl, stride, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    VkPipelineColorBlendAttachmentState& ca = j.ca;
    if (color != VK_FORMAT_UNDEFINED) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_dev.phys, color, &fp);
        const bool can_blend = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
        const bool src1 = uses_src1(b.src_rgb) || uses_src1(b.dst_rgb) || uses_src1(b.src_a) || uses_src1(b.dst_a);
        ca.blendEnable = b.enable && can_blend && !src1 ? VK_TRUE : VK_FALSE;
        if (b.enable && (!can_blend || src1)) {
            static int n = 0;
            if (++n <= 4) fprintf(stderr, "[VK-PIPE] blending off: %s\n", !can_blend ? "the format cannot blend here" :
                                                                           "dual-source factors (HLSLcc emits one output)");
        }
        ca.srcColorBlendFactor = blend_factor(b.src_rgb);
        ca.dstColorBlendFactor = blend_factor(b.dst_rgb);
        ca.colorBlendOp = blend_op(b.op_rgb);
        ca.srcAlphaBlendFactor = blend_factor(b.src_a);
        ca.dstAlphaBlendFactor = blend_factor(b.dst_a);
        ca.alphaBlendOp = blend_op(b.op_a);
        ca.colorWriteMask = b.write_mask & 0xF;          // D3D's R=1 G=2 B=4 A=8 are Vulkan's bits
    }
    j.color = color;
    j.depth = g_dev.depth_fmt;
    j.stencil = stencil && g_dev.depth_has_stencil ? g_dev.depth_fmt : VK_FORMAT_UNDEFINED;
    j.clamp = clamp && g_dev.depth_clamp;
    j.vsm = vs.module;
    j.psm = ps.module;
    j.pl = L.pl;
    if (g_async_pipes) {
        pj_submit(std::move(j));
        *waiting = true;
        return VK_NULL_HANDLE;
    }
    const VkPipeline p = create_pipeline(j);
    pipe_result(key, r.vs, r.ps, p);
    return p;
}

// ---- Per frame state ------------------------------------------------------------------------------------------------
std::set<uint64_t> g_depth_cleared;                 // depth buffers cleared in this frame
struct StencilPlane { int value = 0; std::set<const Image*> applied; };   // the game's last clear per DSV resource
std::unordered_map<uint32_t, StencilPlane> g_stencil;

// C50: static vertex (and, BO2_IB_RESIDENT, index) buffers, resident, keyed by the guest buffer's host address; dropped
// after 600 unused frames.
// BO2_VK_RESIDENT_CHECK (on): a static buffer the game freed, and another of the same size created later at the same
// address, would find the first one's bytes here. Once per frame per buffer, its first and last 32 bytes are compared
// with what was uploaded, and a difference uploads it again (counted on [VK-FRAME] as `stale`).
struct Resident { Buffer buf; uint32_t n = 0; uint64_t used = 0, checked = 0; uint8_t head[32], tail[32]; };
std::unordered_map<const void*, Resident> g_resident;
const bool g_resident_check = env_on("BO2_VK_RESIDENT_CHECK", true);
uint64_t g_resident_stale = 0, g_ib_resident_draws = 0;

bool resident_bind(const uint8_t* base, uint32_t n, const uint8_t* p, VkBuffer* buf, VkDeviceSize* off) {
    if (!base || !n || p < base || p >= base + n) return false;
    Resident& e = g_resident[base];
    const uint32_t fp = std::min<uint32_t>(n, 32);
    bool upload = !e.buf.buf || e.n != n;
    if (!upload && g_resident_check && e.checked != g_dev.serial) {
        e.checked = g_dev.serial;
        if (memcmp(e.head, base, fp) != 0 || memcmp(e.tail, base + n - fp, fp) != 0) { upload = true; ++g_resident_stale; }
    }
    if (upload) {
        if (e.buf.buf) { Buffer dead = e.buf; destroy_later([dead]() mutable { buffer_destroy(dead); }); }
        e.buf = buffer_create(n, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        if (!e.buf.buf || !e.buf.mem.mapped) { g_resident.erase(base); return false; }
        memcpy(e.buf.mem.mapped, base, n);   // a static buffer's bytes never change (created with initial data)
        memcpy(e.head, base, fp);
        memcpy(e.tail, base + n - fp, fp);
        e.n = n;
        e.checked = g_dev.serial;
    }
    e.used = g_dev.serial;
    *buf = e.buf.buf;
    *off = (VkDeviceSize)(p - base);
    return true;
}
void resident_evict() {
    for (auto it = g_resident.begin(); it != g_resident.end();) {
        if (g_dev.serial - it->second.used > 600) {
            Buffer dead = it->second.buf;
            destroy_later([dead]() mutable { buffer_destroy(dead); });
            it = g_resident.erase(it);
        } else ++it;
    }
}

// C47 on the GPU side: the same constant-buffer bytes (same arena slice) pushed once per frame.
struct UboMemo { const void* p = nullptr; uint32_t n = 0; uint64_t serial = 0; VkDeviceSize off = 0; VkBuffer buf = VK_NULL_HANDLE; };

// One planned draw: everything resolved before the pass begins.
struct Planned {
    const RealDrawRec* r;
    VkPipeline pipe;
    VkPipelineLayout pl;
    VkDescriptorSet set;
    VkBuffer vb[kMaxVBStreams + 1];
    VkDeviceSize vbo[kMaxVBStreams + 1];
    VkBuffer ib;                    // the ring the indices went to (it may grow, and be replaced, while a pass is planned)
    VkDeviceSize ibo;
    uint32_t first_idx;             // BO2_VK_RING_BASE: the indices' place in the ring, as firstIndex (ibo is then 0)
    int32_t vtx_off;                // vertexOffset: the draw's base vertex, plus slot 0's place in its buffer (ring base)
    bool used_slot[kMaxVBStreams + 1];
    VkRect2D scissor;
    uint32_t ndyn;                  // dynamic constant-buffer offsets, in binding order (Layout::dyn)
    uint32_t dyn[2 * kMaxCB];
};

struct Sampled { Image* im; uint32_t mip0, nmips; };
uint64_t g_pass_serial = 0;         // render passes recorded: an image's barrier is listed once per pass and range

// The scissor rectangle (rs bit 0) clamped to the target; false when it is empty: D3D draws nothing then.
bool scissor_of(const RealDrawRec& r, uint32_t tw, uint32_t th, VkRect2D* out) {
    *out = {{0, 0}, {tw, th}};
    if (!(r.rs & 1u)) return true;
    const int32_t x0 = std::clamp(r.sc[0], 0, (int32_t)tw), y0 = std::clamp(r.sc[1], 0, (int32_t)th);
    const int32_t x1 = std::clamp(r.sc[2], 0, (int32_t)tw), y1 = std::clamp(r.sc[3], 0, (int32_t)th);
    if (x1 <= x0 || y1 <= y0) return false;
    *out = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    return true;
}

// What the shader's binding reads, for one draw.
VkImageView bind_texture(const RealDrawRec& r, const ShaderRec& s, uint32_t binding, const Image* target,
                         uint32_t target_mip, std::vector<Sampled>& sampled, Image** used) {
    const auto mt = s.tex.find(binding);
    const bool declared = mt != s.tex.end();
    const uint32_t reg = declared ? mt->second : binding % kBindSpan;
    uint32_t dim = 2;
    if (auto d = s.dim.find(binding); d != s.dim.end()) dim = d->second;
    ImagePtr fallback = declared ? g_null2d : g_white;
    if (dim == 3) fallback = g_null3d;
    else if (dim == 4) fallback = g_nullcube;
    else if (dim == 5) fallback = g_nulldepth;
    const uint32_t res = s.pixel && reg < kMaxPSTex ? r.pstex[reg] : 0;   // no vertex-stage textures in GfxDraw
    Image* im = nullptr;
    if (res) {
        auto t = g_tex.find(res);
        if (t != g_tex.end() && t->second) im = t->second.get();
    }
    // The type must match the declaration, or the slot reads a typed stand-in.
    if (im) {
        const bool ok = dim == 3 ? im->vtype == VK_IMAGE_VIEW_TYPE_3D
                      : dim == 4 ? im->vtype == VK_IMAGE_VIEW_TYPE_CUBE
                      : dim == 5 ? (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0
                                 : im->vtype == VK_IMAGE_VIEW_TYPE_2D;
        if (!ok) im = nullptr;
    }
    uint32_t m0 = 0, nm = 0;
    if (im) {
        const uint32_t pm = s.pixel && reg < kMaxPSTex ? r.psmip[reg] : 0;
        if (pm && g_mip_targets) {
            m0 = pm >> 8;
            nm = pm & 0xFF;
            if (m0 >= im->mips) im = nullptr;
            else if (!nm || m0 + nm > im->mips) nm = im->mips - m0;
        } else {
            nm = im->mips;
        }
    }
    // D3D unbinds a resource bound as SRV while it is the render target: a view that contains the attached level
    // reads the declared stand-in instead (macOS P855). Other levels of the same image (downsample chains) stay.
    if (im && im == target && target_mip >= m0 && target_mip < m0 + nm) im = nullptr;
    Image* use = im ? im : fallback.get();
    *used = use;
    if (!im) { m0 = 0; nm = use->mips; }
    // Its barrier before the pass: once per pass for the same range (most draws of a pass share their textures).
    if (use->sampled_pass != g_pass_serial || use->sampled_mip0 != m0 || use->sampled_mips != nm) {
        use->sampled_pass = g_pass_serial; use->sampled_mip0 = m0; use->sampled_mips = nm;
        sampled.push_back({use, m0, nm});
    }
    return image_view(*use, m0, nm);
}

VkSampler bind_sampler(const RealDrawRec& r, const ShaderRec& s, uint32_t binding, bool shadow) {
    const auto m = s.smp.find(binding);
    const SamplerPair* p = &g_samp_default;
    if (m != s.smp.end()) {
        if (m->second < 0) p = &g_samp_point;            // HLSLcc's own point-clamp sampler, for Load
        else if (s.pixel && m->second < kMaxPSTex && r.pssamp[m->second]) {
            auto it = g_samplers.find(r.pssamp[m->second]);
            if (it != g_samplers.end()) p = &it->second;
        }
    }
    return shadow ? p->compare : p->plain;
}

// Descriptor-set caches (BO2_VK_SET_CACHE): per frame slot, reset with its pools; and across frames (stable layouts).
struct CachedSet { uint64_t h2; VkDescriptorSet set; };
std::unordered_map<uint64_t, CachedSet> g_set_frame[kFramesInFlight];
uint64_t g_set_frame_serial[kFramesInFlight] = {};
std::unordered_map<uint64_t, CachedSet> g_set_keep;
uint64_t g_set_keep_epoch = 0, g_set_keep_drops = 0, g_set_keep_gen = 1, g_set_keep_allocs = 0, g_set_keep_forgot = 0;
constexpr uint64_t kSetKeepMax = 16384;   // kept sets allocated before starting over (bounds the persistent pools)
uint64_t g_set_hits = 0, g_set_misses = 0;

void set_report() {
    const uint64_t hits = g_set_hits, misses = g_set_misses;
    fprintf(stderr, "[VK-SETS] descriptor sets: %llu draws, %llu sets written, %.0f%% reused %s%s; %zu kept, %llu "
                    "forgotten with their image, %llu full drops (sampler or ring destroyed, or the cap)\n",
            (unsigned long long)(hits + misses), (unsigned long long)misses,
            hits + misses ? 100.0 * (double)hits / (double)(hits + misses) : 0.0,
            g_set_cache_mode >= 2 ? "across frames" : "within the frame", g_dyn_ubo ? " (dynamic constant buffers)" : "",
            g_set_keep.size(), (unsigned long long)g_set_keep_forgot, (unsigned long long)g_set_keep_drops);
}

bool plan_draw(const RealDrawRec& r, const Image* target, uint32_t target_mip, VkFormat color, bool stencil,
               uint32_t tw, uint32_t th, std::vector<Sampled>& sampled, Planned& out, UboMemo memo[2][kMaxCB]) {
    if (r.topo != 4 && r.topo != 5) { g_skip_why[4]++; return false; }
    // Scissor (rs bit 0): an empty rectangle draws nothing, as D3D. Tested first (BO2_VK_EARLY_SCISSOR): such a draw
    // costs no lookups and never queues a pipeline build.
    VkRect2D sc;
    if (g_early_scissor && !scissor_of(r, tw, th, &sc)) { g_skip_why[6]++; return false; }
    auto vsi = g_shaders.find(r.vs), psi = g_shaders.find(r.ps);
    if (vsi == g_shaders.end() || psi == g_shaders.end()) { g_skip_why[0]++; return false; }
    const ShaderRec& vs = vsi->second;
    const ShaderRec& ps = psi->second;
    Layout* L = layout_for(r.vs, r.ps, vs, ps);
    bool rej = false, waiting = false;
    VkPipeline pipe = pipeline_for(r, vs, ps, *L, color, stencil, (r.rs & 4u) != 0, &rej, &waiting);
    if (!pipe) { if (waiting) g_skip_why[10]++; else if (!rej) g_skip_why[3]++; return false; }
    if (!g_early_scissor && !scissor_of(r, tw, th, &sc)) { g_skip_why[6]++; return false; }
    // Vertex and index data.
    Planned& P = out;
    P = Planned{};
    P.r = &r; P.pipe = pipe; P.pl = L->pl; P.scissor = sc;
    // BO2_VK_RING_BASE: with one stream, slot 0 goes to the ring at a multiple of its stride, so it can be bound at the
    // ring's start with the draw's place in vertexOffset (consecutive draws then share the binding).
    const bool one_stream = r.vb2[0].empty() && r.vb2[1].empty() && r.vb2[2].empty();
    const uint32_t stride0 = r.vb_stride;
    const bool base0 = g_ring_base && one_stream && stride0 && (stride0 % 4) == 0 && stride0 <= 2048;
    for (uint32_t sl = 0; sl < kMaxVBStreams; sl++) {
        const RdSpan<uint8_t>& src = sl == 0 ? r.vb : r.vb2[sl - 1];
        if (src.empty()) continue;
        if (!resident_bind(r.res[sl], r.res_bytes[sl], src.data(), &P.vb[sl], &P.vbo[sl])) {
            VkDeviceSize align = 16;
            if (sl == 0 && base0) align = (VkDeviceSize)stride0 * 16 / std::gcd<VkDeviceSize, VkDeviceSize>(stride0, 16);   // lcm
            P.vbo[sl] = ring_push(src.data(), src.size(), align);
            P.vb[sl] = ring_buffer();
            if (P.vbo[sl] == ~0ull) { g_skip_why[7]++; return false; }
        }
        P.used_slot[sl] = true;
    }
    P.vtx_off = r.base_vtx;
    if (base0 && P.used_slot[0] && P.vbo[0] % stride0 == 0) {
        const int64_t v = (int64_t)r.base_vtx + (int64_t)(P.vbo[0] / stride0);
        if (v >= INT32_MIN && v <= INT32_MAX) { P.vtx_off = (int32_t)v; P.vbo[0] = 0; }
    }
    P.vb[kZeroStream] = g_zero_vb.buf; P.vbo[kZeroStream] = 0; P.used_slot[kZeroStream] = true;
    if (r.ib.empty()) { g_skip_why[7]++; return false; }
    const VkDeviceSize isz = r.ib_fmt == 57 ? 2 : 4;
    // BO2_IB_RESIDENT: indices from a static index buffer are drawn in place from its resident copy.
    if (r.ib_res && r.ib.data() + r.ib.size() <= r.ib_res + r.ib_res_bytes &&
        resident_bind(r.ib_res, r.ib_res_bytes, r.ib.data(), &P.ib, &P.ibo) && P.ibo % isz == 0) {
        ++g_ib_resident_draws;
    } else {
        P.ibo = ring_push(r.ib.data(), r.ib.size(), 16);
        if (P.ibo == ~0ull) { g_skip_why[7]++; return false; }
        P.ib = ring_buffer();
    }
    if (g_ring_base && P.ibo / isz <= UINT32_MAX / 2) {
        P.first_idx = (uint32_t)(P.ibo / isz);   // a whole number of indices (16-aligned in the ring, checked resident)
        P.ibo = 0;
    }
    // Descriptors. The set is allocated only after its contents are known: a draw whose buffers, views and samplers
    // are the same as an earlier draw's in this frame reuses that set (see the cache below).
    // Scratch kept from draw to draw (render thread only): fresh vectors were three allocations and frees per draw.
    // The reserves keep the pointers taken into images and bufs below valid (no reallocation while they grow).
    static std::vector<VkWriteDescriptorSet> writes;
    static std::vector<VkDescriptorImageInfo> images;
    static std::vector<VkDescriptorBufferInfo> bufs;
    static std::vector<Image*> imgs;   // the images behind the texture writes (a kept set is filed under each)
    writes.clear();
    images.clear();
    bufs.clear();
    imgs.clear();
    writes.reserve(L->bindings.size());
    images.reserve(L->bindings.size());
    bufs.reserve(L->bindings.size());
    const VkDeviceSize ubo_align = std::max<VkDeviceSize>(16, g_dev.props.limits.minUniformBufferOffsetAlignment);
    uint32_t dyn_bind[2 * kMaxCB];
    for (const ShaderRec* s : {&vs, &ps})
        for (const Decl& d : s->decls) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = VK_NULL_HANDLE;   // set below, once the set is chosen
            w.dstBinding = d.binding;
            w.descriptorCount = 1;
            w.descriptorType = desc_type(d.kind);
            if (d.kind == DescKind::Ubo) {
                if (L->dyn) w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                const uint32_t idx = d.binding - (s->pixel ? kPsCbBase : kVsCbBase);
                uint32_t reg = idx;   // no map: the binding is the identity, as on macOS
                if (auto m = s->cb.find(d.binding); m != s->cb.end()) reg = m->second;
                const RdSpan<uint8_t>* src = reg < kMaxCB ? (s->pixel ? &r.pscb[reg] : &r.vscb[reg]) : nullptr;
                VkDescriptorBufferInfo bi{g_zero_ubo.buf, 0, g_zero_ubo.size};
                if (src && !src->empty()) {
                    UboMemo& mm = memo[s->pixel ? 1 : 0][idx < kMaxCB ? idx : 0];
                    if (idx < kMaxCB && mm.p == src->data() && mm.n == src->size() && mm.serial == g_dev.serial &&
                        mm.buf == ring_buffer()) {
                        bi = {mm.buf, mm.off,
                              std::min<VkDeviceSize>(src->size(), g_dev.props.limits.maxUniformBufferRange)};
                    } else {
                        const VkDeviceSize n = std::min<VkDeviceSize>(src->size(), g_dev.props.limits.maxUniformBufferRange);
                        const VkDeviceSize off = ring_push(src->data(), n, ubo_align);
                        if (off == ~0ull) { g_skip_why[7]++; return false; }
                        bi = {ring_buffer(), off, n};
                        if (idx < kMaxCB) mm = {src->data(), (uint32_t)src->size(), g_dev.serial, off, ring_buffer()};
                    }
                }
                if (L->dyn) {
                    // The set names the buffer and range; the offset goes to the bind (binding order, sorted below).
                    if (P.ndyn < 2 * kMaxCB) { dyn_bind[P.ndyn] = d.binding; P.dyn[P.ndyn++] = (uint32_t)bi.offset; }
                    bi.offset = 0;
                }
                bufs.push_back(bi);
                w.pBufferInfo = &bufs.back();
            } else if (d.kind == DescKind::Texture) {
                VkDescriptorImageInfo ii{};
                Image* used = nullptr;
                ii.imageView = bind_texture(r, *s, d.binding, target, target_mip, sampled, &used);
                imgs.push_back(used);
                ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (!ii.imageView) { g_skip_why[8]++; return false; }
                images.push_back(ii);
                w.pImageInfo = &images.back();
            } else {
                VkDescriptorImageInfo ii{};
                ii.sampler = bind_sampler(r, *s, d.binding, d.kind == DescKind::ShadowSampler);
                images.push_back(ii);
                w.pImageInfo = &images.back();
            }
            writes.push_back(w);
        }
    // Descriptor sets reused within a frame. On the S26 vkAllocateDescriptorSets (15%) and vkUpdateDescriptorSets
    // (10%) were a quarter of the render thread, one of each per draw. Constants are already deduplicated per frame
    // (the UBO memo above), so draws with the same constants, textures and samplers write identical sets. The key is
    // the layout and every write's binding and contents, hashed twice (two independent 64-bit hashes must both
    // match). The cache belongs to the frame slot: its pool is reset when the slot comes round again, so the cache
    // is dropped on the first draw of each new frame. BO2_VK_SET_CACHE=0 allocates a set per draw, as before.
    // Vulkan takes dynamic offsets in binding-number order; the declarations come vertex stage first, then pixel.
    for (uint32_t i = 1; i < P.ndyn; i++)
        for (uint32_t j = i; j > 0 && dyn_bind[j - 1] > dyn_bind[j]; j--) {
            std::swap(dyn_bind[j - 1], dyn_bind[j]);
            std::swap(P.dyn[j - 1], P.dyn[j]);
        }
    uint64_t h1 = mix(0x5bd1e995ull, (uint64_t)(uintptr_t)L->dsl), h2 = 1469598103934665603ull ^ (uint64_t)(uintptr_t)L->dsl;
    auto fnv = [&h2](uint64_t v) { for (int i = 0; i < 8; i++) { h2 ^= (v >> (8 * i)) & 0xff; h2 *= 1099511628211ull; } };
    for (const VkWriteDescriptorSet& w : writes) {
        const uint64_t head = ((uint64_t)w.dstBinding << 32) | (uint64_t)w.descriptorType;
        h1 = mix(h1, head); fnv(head);
        if (w.pBufferInfo) {
            const VkDescriptorBufferInfo& b = *w.pBufferInfo;
            h1 = mix(mix(mix(h1, (uint64_t)(uintptr_t)b.buffer), b.offset), b.range);
            fnv((uint64_t)(uintptr_t)b.buffer); fnv(b.offset); fnv(b.range);
        } else if (w.pImageInfo) {
            const VkDescriptorImageInfo& ii = *w.pImageInfo;
            h1 = mix(mix(mix(h1, (uint64_t)(uintptr_t)ii.imageView), (uint64_t)(uintptr_t)ii.sampler), ii.imageLayout);
            fnv((uint64_t)(uintptr_t)ii.imageView); fnv((uint64_t)(uintptr_t)ii.sampler); fnv(ii.imageLayout);
        }
    }
    // BO2_VK_SET_CACHE=2: a layout whose sets name no per-draw offset (stable) keeps them across frames, in pools that
    // outlive the frame: the same material draws with the same set every frame. Its key names views, samplers and
    // buffers by handle, and a destroyed handle can come back for a new object: the sets naming a destroyed image are
    // forgotten (Image::kept_keys), and everything goes when a sampler or ring is destroyed (descriptor_epoch) or past
    // kSetKeepMax sets (their pools are not freed set by set).
    const bool keep = g_set_cache_mode >= 2 && L->stable;
    std::unordered_map<uint64_t, CachedSet>* cache = nullptr;
    if (keep) {
        const uint64_t ep = descriptor_epoch();
        if (ep != g_set_keep_epoch || g_set_keep_allocs >= kSetKeepMax) {
            if (g_set_keep_allocs) ++g_set_keep_drops;
            g_set_keep.clear();
            descriptor_persistent_drop();
            g_set_keep_epoch = ep;
            g_set_keep_allocs = 0;
            ++g_set_keep_gen;   // the keys images hold from before are void
        }
        // Sets naming an image destroyed since (frame_begin): forgotten before anything can be looked up.
        if (auto& dead = descriptor_dead_keys(); !dead.empty()) {
            for (const auto& [gen, key] : dead)
                if (gen == g_set_keep_gen) g_set_keep_forgot += g_set_keep.erase(key);
            dead.clear();
        }
        cache = &g_set_keep;
    } else if (g_set_cache_mode >= 1) {
        auto& c = g_set_frame[g_dev.cur];
        if (g_set_frame_serial[g_dev.cur] != g_dev.serial) {
            c.clear();
            g_set_frame_serial[g_dev.cur] = g_dev.serial;
        }
        cache = &c;
    }
    if (cache) {
        if (auto it = cache->find(h1); it != cache->end() && it->second.h2 == h2) {
            P.set = it->second.set;
            if ((++g_set_hits + g_set_misses) % (1u << 20) == 0) set_report();
            return true;
        }
    }
    P.set = keep ? descriptor_alloc_persistent(L->dsl) : descriptor_alloc(L->dsl);
    if (!P.set) { g_skip_why[8]++; return false; }
    for (VkWriteDescriptorSet& w : writes) w.dstSet = P.set;
    if (!writes.empty()) vkUpdateDescriptorSets(g_dev.dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    if (cache) (*cache)[h1] = {h2, P.set};
    if (keep) {
        ++g_set_keep_allocs;
        for (Image* im : imgs) {
            if (im->kept_gen != g_set_keep_gen) { im->kept_keys.clear(); im->kept_gen = g_set_keep_gen; }
            im->kept_keys.push_back(h1);
        }
    }
    if ((g_set_hits + ++g_set_misses) % (1u << 20) == 0) set_report();
    return true;
}

// What the previous draw of the pass bound and set (BO2_VK_STATE_FILTER). `all`: record everything (the filter is off,
// or nothing was recorded yet in this pass).
struct CmdState {
    bool all = true;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t ndyn = 0, dyn[2 * kMaxCB] = {};
    VkBuffer vb[kMaxVBStreams + 1] = {};
    VkDeviceSize vbo[kMaxVBStreams + 1] = {};
    bool vb_set[kMaxVBStreams + 1] = {};
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize ibo = 0;
    VkIndexType it = VK_INDEX_TYPE_MAX_ENUM;
    VkViewport vp{};
    VkRect2D sc{};
    uint32_t cull = ~0u, topo = ~0u, restart = ~0u, bias_on = ~0u, ztest = ~0u, zwrite = ~0u, zcmp = ~0u, st_on = ~0u;
    float bias[3] = {};
    uint32_t st_ops[2] = {~0u, ~0u}, st_cmp = ~0u, st_write = ~0u, st_ref = ~0u;
};

void record_draw(VkCommandBuffer cb, const Planned& P, bool has_dsv_attach, bool has_stencil, CmdState& cs) {
    const RealDrawRec& r = *P.r;
    const bool all = cs.all;
    cs.all = !g_state_filter;
    // true: record this one (it differs from what is set, or everything is recorded)
    auto need = [all](bool differs) {
        if (all || differs) { ++g_cmd_made; return true; }
        ++g_cmd_skipped;
        return false;
    };
    if (need(cs.pipe != P.pipe)) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pipe);
        cs.pipe = P.pipe;
    }
    // A set stays bound across pipelines of the same layout; another layout rebinds it.
    if (need(cs.pl != P.pl || cs.set != P.set || cs.ndyn != P.ndyn ||
             (P.ndyn && memcmp(cs.dyn, P.dyn, P.ndyn * sizeof P.dyn[0]) != 0))) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl, 0, 1, &P.set, P.ndyn, P.ndyn ? P.dyn : nullptr);
        cs.pl = P.pl; cs.set = P.set; cs.ndyn = P.ndyn;
        if (P.ndyn) memcpy(cs.dyn, P.dyn, P.ndyn * sizeof P.dyn[0]);
    }
    for (uint32_t sl = 0; sl <= kZeroStream; sl++)
        if (P.used_slot[sl] && need(!cs.vb_set[sl] || cs.vb[sl] != P.vb[sl] || cs.vbo[sl] != P.vbo[sl])) {
            vkCmdBindVertexBuffers(cb, sl, 1, &P.vb[sl], &P.vbo[sl]);
            cs.vb_set[sl] = true; cs.vb[sl] = P.vb[sl]; cs.vbo[sl] = P.vbo[sl];
        }
    const VkIndexType it = r.ib_fmt == 57 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (need(cs.ib != P.ib || cs.ibo != P.ibo || cs.it != it)) {
        vkCmdBindIndexBuffer(cb, P.ib, P.ibo, it);
        cs.ib = P.ib; cs.ibo = P.ibo; cs.it = it;
    }
    // Viewport: D3D's y points down the target; a negative height gives Vulkan the same mapping (and D3D's winding).
    const float* v = r.vp;
    float x = 0, y = 0, w = (float)P.scissor.extent.width, h = (float)P.scissor.extent.height;
    if (v[2] > 0.f && v[3] > 0.f) { x = v[0]; y = v[1]; w = v[2]; h = v[3]; }
    float zn = v[4], zf = v[5];
    if (!(zf > zn) || zn < 0.f || zf > 1.f) { zn = 0.f; zf = 1.f; }
    const VkViewport vp{x, y + h, w, -h, zn, zf};
    if (need(memcmp(&cs.vp, &vp, sizeof vp) != 0)) { vkCmdSetViewport(cb, 0, 1, &vp); cs.vp = vp; }
    if (need(memcmp(&cs.sc, &P.scissor, sizeof P.scissor) != 0)) { vkCmdSetScissor(cb, 0, 1, &P.scissor); cs.sc = P.scissor; }
    const uint32_t cull = (r.rs >> 8) & 3u;
    if (need(cs.cull != cull)) {
        vkCmdSetCullMode(cb, cull == 2 ? VK_CULL_MODE_FRONT_BIT : cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
        cs.cull = cull;
    }
    if (need(false)) vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);   // the producer folded FrontCounterClockwise into the cull mode
    if (need(cs.topo != r.topo)) {
        vkCmdSetPrimitiveTopology(cb, r.topo == 5 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        cs.topo = r.topo;
    }
    const uint32_t restart = r.topo == 5;                 // D3D11 strips always restart at the cut index
    if (need(cs.restart != restart)) { vkCmdSetPrimitiveRestartEnable(cb, restart); cs.restart = restart; }
    const uint32_t bias = r.zb != 0 || r.zb_pente != 0.f;
    if (need(cs.bias_on != bias)) { vkCmdSetDepthBiasEnable(cb, bias); cs.bias_on = bias; }
    const float bv[3] = {(float)r.zb, r.zb_max, r.zb_pente};
    if (need(memcmp(cs.bias, bv, sizeof bv) != 0)) { vkCmdSetDepthBias(cb, bv[0], bv[1], bv[2]); memcpy(cs.bias, bv, sizeof bv); }
    // Depth: no DSV bound, no depth test (macOS: BO2_DSV0_Z keeps it, off by default).
    GfxDepth z = r.depth;
    if (!r.dsv || !has_dsv_attach) { z.enable = 0; z.write = 0; }
    const uint32_t ztest = z.enable != 0, zwrite = z.enable && z.write;
    const uint32_t zcmp = z.enable ? cmp_op(z.func) : VK_COMPARE_OP_ALWAYS;
    if (need(cs.ztest != ztest)) { vkCmdSetDepthTestEnable(cb, ztest); cs.ztest = ztest; }
    if (need(cs.zwrite != zwrite)) { vkCmdSetDepthWriteEnable(cb, zwrite); cs.zwrite = zwrite; }
    if (need(cs.zcmp != zcmp)) { vkCmdSetDepthCompareOp(cb, (VkCompareOp)zcmp); cs.zcmp = zcmp; }
    const uint32_t st = z.st && has_stencil;
    if (need(cs.st_on != st)) { vkCmdSetStencilTestEnable(cb, st); cs.st_on = st; }
    for (int f = 0; f < 2; f++) {
        const uint32_t o = (z.st_ops >> (16 * f)) & 0xFFFF;
        if (!need(cs.st_ops[f] != o)) continue;
        const VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilOp(cb, face, stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                          cmp_op((o >> 12) & 0xF));
        cs.st_ops[f] = o;
    }
    const uint32_t scmp = (z.st >> 8) & 0xFF, swrite = (z.st >> 16) & 0xFF, sref = z.st_ref & 0xFF;
    if (need(cs.st_cmp != scmp)) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, scmp); cs.st_cmp = scmp; }
    if (need(cs.st_write != swrite)) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, swrite); cs.st_write = swrite; }
    if (need(cs.st_ref != sref)) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sref); cs.st_ref = sref; }
    vkCmdDrawIndexed(cb, r.idx_count, 1, P.first_idx, P.vtx_off, 0);
    g_draws++;
}

// CopyResource between guest resources (the back buffer included), with a blit: same size copies texel for texel,
// a different size or format is converted, as macOS's copy pass does.
void copy_resource(VkCommandBuffer cb, uint32_t dst_id, uint32_t src_id) {
    const uint32_t bb = g_backbuffer_res_id.load();
    auto find = [&](uint32_t id) -> Image* {
        if (id && id == bb) return g_backbuffer.get();
        auto it = g_tex.find(id);
        return it != g_tex.end() && it->second ? it->second.get() : nullptr;
    };
    Image* src = find(src_id);
    Image* dst = find(dst_id);
    static uint64_t n_missing = 0;
    if (!src || !dst || src == dst || (src->aspect != VK_IMAGE_ASPECT_COLOR_BIT) || (dst->aspect != VK_IMAGE_ASPECT_COLOR_BIT)) {
        if (++n_missing <= 8) fprintf(stderr, "[VK-COPY] %08x -> %08x: %s\n", src_id, dst_id, !src ? "no source" : !dst ? "no destination" : "not a colour copy");
        return;
    }
    const bool same = src->w == dst->w && src->h == dst->h;
    image_transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    image_transition(cb, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    if (dxgi_is_bc(src->dxgi) || dxgi_is_bc(dst->dxgi)) {
        if (same && src->fmt == dst->fmt) {
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {src->w, src->h, 1};
            vkCmdCopyImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    } else {
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {(int32_t)src->w, (int32_t)src->h, 1};
        b.dstOffsets[1] = {(int32_t)dst->w, (int32_t)dst->h, 1};
        vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &b, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
}

}  // namespace

// ---- One frame ------------------------------------------------------------------------------------------------------
void render_frame(VkCommandBuffer cb, const RdFrame& frame, uint64_t seq) {
    (void)seq;
    g_depth_cleared.clear();
    resident_evict();
    const std::vector<RealDrawRec>& draws = frame.recs;
    const uint32_t bb_rtv = g_backbuffer_rtv_id.load();
    const float depth_clear = gfx_depth_clear_value();
    // The back buffer starts each frame as macOS's drawable does: dark grey (BO2_CLEAR_RED: red, to see gaps).
    {
        static const bool red = env_on("BO2_CLEAR_RED", false);
        image_transition(cb, *g_backbuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue c{};
        c.float32[0] = red ? 1.f : 0.05f; c.float32[1] = red ? 0.f : 0.05f; c.float32[2] = red ? 0.f : 0.07f; c.float32[3] = 1.f;
        VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, g_backbuffer->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rr);
    }
    UboMemo memo[2][kMaxCB] = {};
    static std::vector<Planned> plans;     // kept from frame to frame (render thread only): no per-frame allocation
    static std::vector<Sampled> sampled;
    const size_t n = draws.size();
    size_t k = 0;
    while (k < n) {
        const RealDrawRec& first = draws[k];
        if (first.is_clear == 2) {   // ClearDepthStencilView(STENCIL) between two groups
            StencilPlane& s = g_stencil[first.depth.st_res];
            s.value = (int)first.depth.st_ref; s.applied.clear();
            k++; continue;
        }
        if (first.is_clear == 3) { k++; continue; }   // BO2_DEPTH_DSV's per-resource depth clear: lever not ported
        if (first.is_copy) { copy_resource(cb, first.copy_dst, first.copy_src); k++; continue; }
        // The group: the following records aimed at the same target, up to a clear of it.
        const uint32_t rtv = first.rtv;
        uint32_t st_grp = (!first.is_clear && first.depth.st) ? first.depth.st_res : 0;
        size_t e = k + 1;
        for (; e < n; e++) {
            const RealDrawRec& d = draws[e];
            if (d.is_clear == 2) {
                if (st_grp == d.depth.st_res) break;   // the plane this pass uses is cleared: cut here
                StencilPlane& s = g_stencil[d.depth.st_res];
                s.value = (int)d.depth.st_ref; s.applied.clear();
                continue;
            }
            if (d.is_clear == 3) continue;
            if (d.is_copy || d.rtv != rtv || d.is_clear) break;
            if (d.depth.st) {
                if (!st_grp) st_grp = d.depth.st_res;
                else if (st_grp != d.depth.st_res) break;   // another stencil plane: another pass
            }
        }
        // The target.
        const bool to_screen = rtv == 0 || rtv == bb_rtv;
        Image* target = nullptr;
        uint32_t mip = 0, res = 0;
        if (to_screen) {
            target = g_backbuffer.get();
        } else {
            if (auto rr = g_rtv_res.find(rtv); rr != g_rtv_res.end()) {
                res = rr->second;
                if (auto t = g_tex.find(res); t != g_tex.end() && t->second && t->second->render_target) target = t->second.get();
            }
            if (!target) { g_skip_why[5] += e - k; g_skipped += e - k; k = e; continue; }
            if (auto m = g_rtv_mip.find(rtv); m != g_rtv_mip.end()) mip = m->second;
            if (mip >= target->mips) { g_skip_why[9] += e - k; g_skipped += e - k; k = e; continue; }
        }
        const bool first_use = !to_screen && res && !g_rt_cleared.count(res);
        if (first_use) g_rt_cleared.insert(res);
        const bool clear_first = first.is_clear || first_use;
        const uint32_t tw = std::max(1u, target->w >> mip), th = std::max(1u, target->h >> mip);
        ImagePtr depth = depth_for(tw, th, to_screen);
        const bool stencil = st_grp && depth && g_dev.depth_has_stencil;
        // Plan every draw (pipelines, descriptors, uploads) and collect what they sample.
        plans.clear();
        sampled.clear();
        ++g_pass_serial;
        for (size_t z = k; z < e; z++) {
            if (draws[z].is_clear) continue;
            Planned P;
            if (plan_draw(draws[z], target, mip, target->fmt, stencil, tw, th, sampled, P, memo)) plans.push_back(P);
            else g_skipped++;
        }
        // Barriers before the pass (layouts cannot change inside it).
        for (const Sampled& s : sampled) image_transition(cb, *s.im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s.mip0, s.nmips);
        image_transition(cb, *target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, mip, 1);
        if (depth) image_transition(cb, *depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = image_attachment_view(*target, mip);
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = clear_first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (first.is_clear) memcpy(ca.clearValue.color.float32, first.clear_rgba, sizeof first.clear_rgba);
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (depth) {
            const uint64_t dk = ((uint64_t)tw << 32) | th | ((uint64_t)to_screen << 31);
            const bool dfirst = g_depth_cleared.insert(dk).second;
            da.imageView = image_attachment_view(*depth, 0);
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            da.loadOp = dfirst ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil.depth = depth_clear;
            if (stencil) {
                StencilPlane& s = g_stencil[st_grp];
                const bool sclear = !s.applied.count(depth.get());
                s.applied.insert(depth.get());
                sa = da;
                sa.loadOp = sclear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                sa.clearValue.depthStencil.stencil = (uint32_t)s.value;
            }
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {tw, th}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = depth ? &da : nullptr;
        ri.pStencilAttachment = stencil ? &sa : nullptr;
        vkCmdBeginRendering(cb, &ri);
        CmdState cs;   // the state filter starts afresh with each pass
        for (const Planned& P : plans) record_draw(cb, P, depth != nullptr, stencil, cs);
        vkCmdEndRendering(cb);
        k = e;
    }
    static uint64_t nrep = 0;
    if ((++nrep % 600) == 1)
        fprintf(stderr, "[VK-FRAME] %zu records; %llu draws, %llu skipped (shader %llu, attr %llu, vformat %llu, "
                        "pipeline %llu, topology %llu, target %llu, scissor %llu, buffers %llu, descriptors %llu, mip %llu, "
                        "pipeline building %llu); per draw %.1f binds and state commands recorded, %.1f filtered out; "
                        "%zu resident buffers (%llu stale, uploaded again), %llu draws with resident indices\n",
                n, (unsigned long long)g_draws, (unsigned long long)g_skipped, (unsigned long long)g_skip_why[0],
                (unsigned long long)g_skip_why[1], (unsigned long long)g_skip_why[2], (unsigned long long)g_skip_why[3],
                (unsigned long long)g_skip_why[4], (unsigned long long)g_skip_why[5], (unsigned long long)g_skip_why[6],
                (unsigned long long)g_skip_why[7], (unsigned long long)g_skip_why[8], (unsigned long long)g_skip_why[9],
                (unsigned long long)g_skip_why[10], g_draws ? (double)g_cmd_made / (double)g_draws : 0.0,
                g_draws ? (double)g_cmd_skipped / (double)g_draws : 0.0, g_resident.size(),
                (unsigned long long)g_resident_stale, (unsigned long long)g_ib_resident_draws);
}

void render_shutdown_pipelines() {
    pj_stop();   // the workers use the layouts and shader modules destroyed below and after
    for (auto& kv : g_pipes) if (kv.second) vkDestroyPipeline(g_dev.dev, kv.second, nullptr);
    g_pipes.clear();
    for (auto& kv : g_layouts) {
        vkDestroyPipelineLayout(g_dev.dev, kv.second.pl, nullptr);
        vkDestroyDescriptorSetLayout(g_dev.dev, kv.second.dsl, nullptr);
    }
    g_layouts.clear();
    for (auto& kv : g_resident) buffer_destroy(kv.second.buf);
    g_resident.clear();
    for (auto& c : g_set_frame) c.clear();
    g_set_keep.clear();   // their pools go with the device (device_shutdown)
}

}  // namespace bo2vk
