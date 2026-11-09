// vk_frame.cpp — one turn of the pump: take the newest frame, apply registrations, replay, draw the UI overlay,
// present (or keep the image, headless), pace. Plus the two small pipelines that are the backend's own: the live UI
// pass (macOS's ui_v / ui_f) and the presentation (the back buffer drawn into the display plan's rectangle, rotated
// to the surface's transform: Android's pre-rotation, so the compositor does not rotate every frame).
#include "vk_render.h"

#include "vk_hooks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

namespace bo2vk {

void render_shutdown_pipelines();
extern std::atomic<bool> g_surface_changed;
void surface_rebuild();
extern VkSurfaceTransformFlagBitsKHR g_swap_transform;
extern std::atomic<bool> g_foreground;
void push_focus(bool key);

namespace {

const char* kUiVs = R"(#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in float z;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 col;
layout(push_constant) uniform X { vec4 row[4]; uint has_mtx; float pad; vec2 vp; } x;
layout(location = 0) out vec2 o_uv;
layout(location = 1) out vec4 o_col;
void main() {
    if (x.has_mtx != 0u) {
        // Exactly what BO2's own vertex shader does: clip = (dot(p, m[0]), ..., dot(p, m[3])).
        vec4 p = vec4(pos, z, 1.0);
        gl_Position = vec4(dot(p, x.row[0]), dot(p, x.row[1]), dot(p, x.row[2]), dot(p, x.row[3]));
    } else {
        // Viewport pixels, origin top left, y down (the viewport's negative height makes NDC y point up).
        gl_Position = vec4(pos.x / x.vp.x * 2.0 - 1.0, 1.0 - pos.y / x.vp.y * 2.0, 0.0, 1.0);
    }
    o_uv = uv;
    o_col = col;
}
)";
const char* kUiFs = R"(#version 450
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler smp;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 o;
void main() { o = texture(sampler2D(tex, smp), uv) * col; }
)";
// The presentation: a quad over the destination rectangle (in the unrotated panel's NDC), rotated by the surface's
// transform, sampling the whole back buffer.
const char* kPresentVs = R"(#version 450
layout(push_constant) uniform P { vec4 rect; vec2 rot0; vec2 rot1; } p;   // rect: x0, y0, x1, y1 in NDC
layout(location = 0) out vec2 uv;
void main() {
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    uv = c;
    vec2 q = mix(p.rect.xy, p.rect.zw, c);
    gl_Position = vec4(q.x * p.rot0 + q.y * p.rot1, 0.0, 1.0);
}
)";
const char* kPresentFs = R"(#version 450
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler smp;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(texture(sampler2D(tex, smp), uv).rgb, 1.0); }
)";

// The presentation with upscaling: Snapdragon Game Super Resolution 1 (SGSR 1), Qualcomm's single-pass, 12-tap,
// edge-directed upscaler with built-in sharpening, made for Adreno. From SnapdragonStudios/snapdragon-gsr,
// sgsr/v1/include/glsl/sgsr1_shader_mobile.frag (commit d926f07), RGBA mode (OperationMode 1), edge threshold 8/255
// and sharpness 2 as the repository recommends for mobile. Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc.,
// BSD-3-Clause: THIRD_PARTY_NOTICES.md. Adapted to this pass: separate texture and sampler bindings, the source's
// 1/w, 1/h, w, h from a push constant (after the vertex shader's 32 bytes) instead of a uniform, and uv in place of
// in_TEXCOORD0. The vertex shader is the presentation's: uv is in the source's space whatever the rotation.
const char* kSgsrFs = R"(#version 450
precision mediump float;
precision highp int;
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler smp;
layout(push_constant) uniform P { layout(offset = 32) highp vec4 vi; } p;
layout(location = 0) in highp vec2 uv;
layout(location = 0) out vec4 o;
float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
vec2 weightY(float dx, float dy, float c, float stdv) {
    float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * stdv, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}
void main() {
    const float edgeThreshold = 8.0 / 255.0;
    const float edgeSharpness = 2.0;
    vec4 color;
    color.xyz = textureLod(sampler2D(tex, smp), uv, 0.0).xyz;
    highp vec2 imgCoord = uv * p.vi.zw + vec2(-0.5, 0.5);
    highp vec2 imgCoordPixel = floor(imgCoord);
    highp vec2 coord = imgCoordPixel * p.vi.xy;
    vec2 pl = imgCoord - imgCoordPixel;
    vec4 left = textureGather(sampler2D(tex, smp), coord, 1);
    float edgeVote = abs(left.z - left.y) + abs(color.y - left.y) + abs(color.y - left.z);
    if (edgeVote > edgeThreshold) {
        coord.x += p.vi.x;
        vec4 right = textureGather(sampler2D(tex, smp), coord + vec2(p.vi.x, 0.0), 1);
        vec4 upDown;
        upDown.xy = textureGather(sampler2D(tex, smp), coord + vec2(0.0, -p.vi.y), 1).wz;
        upDown.zw = textureGather(sampler2D(tex, smp), coord + vec2(0.0, p.vi.y), 1).yx;
        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color.y - mean;
        float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) +
                      (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) +
                     (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
        float stdv = 2.181818 / sum;
        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, stdv);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, stdv);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, stdv);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, stdv);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, stdv);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, stdv);
        aWY += weightY(pl.x, pl.y, left.z, stdv);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, stdv);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, stdv);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, stdv);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, stdv);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, stdv);
        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(edgeSharpness * finalY, minY, maxY);
        float deltaY = finalY - color.w;
        deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);   // smooth high-contrast input
        color.x = clamp(color.x + deltaY, 0.0, 1.0);
        color.y = clamp(color.y + deltaY, 0.0, 1.0);
        color.z = clamp(color.z + deltaY, 0.0, 1.0);
    }
    o = vec4(color.xyz, 1.0);
}
)";

struct Small {
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    std::unordered_map<VkFormat, VkPipeline> pipes;
};
Small g_ui, g_pres, g_sgsr;
// The upscaler (Graphics -> Upscaler, bo2vk_set_upscaler): 0 bilinear, 1 SGSR. SGSR only runs when the back buffer is
// smaller than the rectangle it is drawn into; at the same size, or larger, the plain pass draws it.
std::atomic<int> g_upscaler{1};

VkShaderModule module_from(bool pixel, const char* src) {
    std::vector<uint32_t> spv;
    std::string log;
    if (!glsl_to_spirv(pixel, src, spv, log)) {
        fprintf(stderr, "[VK] built-in shader does not compile: %s\n", log.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spv.size() * 4;
    mi.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_dev.dev, &mi, nullptr, &m);
    return m;
}

bool small_init(Small& s, const char* vs, const char* fs, uint32_t push_bytes,
                VkShaderStageFlags push_stages = VK_SHADER_STAGE_VERTEX_BIT) {
    s.vs = module_from(false, vs);
    s.fs = module_from(true, fs);
    if (!s.vs || !s.fs) return false;
    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    di.bindingCount = 2;
    di.pBindings = b;
    vkCreateDescriptorSetLayout(g_dev.dev, &di, nullptr, &s.dsl);
    VkPushConstantRange pr{push_stages, 0, push_bytes};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &s.dsl;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    return vkCreatePipelineLayout(g_dev.dev, &pi, nullptr, &s.pl) == VK_SUCCESS;
}

VkPipeline small_pipe(Small& s, VkFormat color, bool ui) {
    auto it = s.pipes.find(color);
    if (it != s.pipes.end()) return it->second;
    VkVertexInputBindingDescription vb{0, (uint32_t)sizeof(UiVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                               {1, 0, VK_FORMAT_R32_SFLOAT, 8},
                                               {2, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                                               {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 20}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (ui) { vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = va; }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = ui ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ca{};
    ca.colorWriteMask = 0xF;
    if (ui) {   // macOS's UI pipeline: source-over
        ca.blendEnable = VK_TRUE;
        ca.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ca.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.colorBlendOp = VK_BLEND_OP_ADD;
        ca.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ca.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ca;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, s.vs, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, s.fs, "main"};
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2; gi.pStages = st;
    gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy;
    gi.layout = s.pl;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev.dev, g_dev.pipeline_cache, 1, &gi, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    else pipeline_cache_note_new();
    s.pipes[color] = p;
    return p;
}

void small_destroy(Small& s) {
    for (auto& kv : s.pipes) if (kv.second) vkDestroyPipeline(g_dev.dev, kv.second, nullptr);
    s.pipes.clear();
    if (s.pl) vkDestroyPipelineLayout(g_dev.dev, s.pl, nullptr);
    if (s.dsl) vkDestroyDescriptorSetLayout(g_dev.dev, s.dsl, nullptr);
    if (s.vs) vkDestroyShaderModule(g_dev.dev, s.vs, nullptr);
    if (s.fs) vkDestroyShaderModule(g_dev.dev, s.fs, nullptr);
    s = Small{};
}

VkDescriptorSet texture_set(Small& s, VkImageView view, VkSampler smp) {
    VkDescriptorSet set = descriptor_alloc(s.dsl);
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[2]{};
    ii[0].imageView = view;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].sampler = smp;
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; i++) {
        w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(g_dev.dev, 2, w, 0, nullptr);
    return set;
}

void raw_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

// The staged texture of BO2_UI_FIXED (a debug view: the largest texture the game made, over the frame).
ImagePtr g_staged_img;

uint64_t g_frame_seq_drawn = 0;
std::chrono::steady_clock::time_point g_last_fresh, g_last_present;

}  // namespace

// The extra state's lifetime follows the device.
bool frame_init() {
    if (!small_init(g_ui, kUiVs, kUiFs, 80) || !small_init(g_pres, kPresentVs, kPresentFs, 32)) return false;
    // SGSR: the presentation's vertex shader, 16 more push-constant bytes for the fragment shader. If it does not build
    // here, the plain presentation stays (logged once).
    if (!small_init(g_sgsr, kPresentVs, kSgsrFs, 48, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) {
        fprintf(stderr, "[VK] SGSR upscaler unavailable: bilinear presentation only\n");
        small_destroy(g_sgsr);
    }
    if (const char* e = getenv("BO2_UPSCALER")) g_upscaler.store(atoi(e));
    return true;
}
void frame_shutdown() {
    small_destroy(g_ui);
    small_destroy(g_pres);
    small_destroy(g_sgsr);
    if (g_staged_img) { image_destroy(*g_staged_img); g_staged_img.reset(); }
    render_shutdown_pipelines();
}

// The live 2D UI pass over the back buffer (positions are the guest viewport's pixels).
void ui_pass(VkCommandBuffer cb) {
    // Kept from frame to frame (render thread only): the UI's vertices are often past the allocator's 64 KB line, so a
    // fresh copy each frame was an mmap and a munmap per frame. Copy-assignment reuses the capacity.
    static std::vector<UiVert> verts;
    static std::vector<UiBatch> batches;
    verts.clear();
    batches.clear();
    float vw, vh;
    {
        std::lock_guard<std::mutex> lk(g_ui_mu);
        if (g_ui_ready) { verts = g_ui_verts; batches = g_ui_batches; }
        vw = g_ui_vw; vh = g_ui_vh;
    }
    static const bool show_staged = getenv("BO2_UI_FIXED") != nullptr;
    if (show_staged && g_staged_img) {   // aspect-fit over the frame, below the UI
        image_transition(cb, *g_staged_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_transition(cb, *g_backbuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const float ia = (float)g_staged_img->w / g_staged_img->h, wa = (float)g_backbuffer->w / g_backbuffer->h;
        const int32_t dw = ia > wa ? (int32_t)g_backbuffer->w : (int32_t)(g_backbuffer->h * ia);
        const int32_t dh = ia > wa ? (int32_t)(g_backbuffer->w / ia) : (int32_t)g_backbuffer->h;
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {(int32_t)g_staged_img->w, (int32_t)g_staged_img->h, 1};
        b.dstOffsets[0] = {((int32_t)g_backbuffer->w - dw) / 2, ((int32_t)g_backbuffer->h - dh) / 2, 0};
        b.dstOffsets[1] = {b.dstOffsets[0].x + dw, b.dstOffsets[0].y + dh, 1};
        vkCmdBlitImage(cb, g_staged_img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_backbuffer->img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    if (verts.empty() || batches.empty()) return;
    VkPipeline pipe = small_pipe(g_ui, g_backbuffer->fmt, true);
    if (!pipe) return;
    const VkDeviceSize vo = ring_push(verts.data(), verts.size() * sizeof(UiVert), 16);
    if (vo == ~0ull) return;
    static std::vector<VkDescriptorSet> sets;
    sets.assign(batches.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < batches.size(); i++) {
        const UiBatch& b = batches[i];
        if (!b.count || b.first + b.count > verts.size()) continue;
        Image* im = g_white.get();
        if (auto t = g_tex.find(b.tex); b.tex && t != g_tex.end() && t->second && t->second->vtype == VK_IMAGE_VIEW_TYPE_2D)
            im = t->second.get();
        image_transition(cb, *im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        sets[i] = texture_set(g_ui, image_view(*im, 0, 0), g_samp_default.plain);
    }
    image_transition(cb, *g_backbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = image_attachment_view(*g_backbuffer, 0);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {g_backbuffer->w, g_backbuffer->h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const VkViewport vp{0, (float)g_backbuffer->h, (float)g_backbuffer->w, -(float)g_backbuffer->h, 0, 1};
    const VkRect2D sc{{0, 0}, {g_backbuffer->w, g_backbuffer->h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkBuffer rb = ring_buffer();
    vkCmdBindVertexBuffers(cb, 0, 1, &rb, &vo);
    for (size_t i = 0; i < batches.size(); i++) {
        if (!sets[i]) continue;
        const UiBatch& b = batches[i];
        struct { float row[16]; uint32_t has_mtx; float pad; float vp[2]; } xf;
        memcpy(xf.row, b.mtx, sizeof xf.row);
        xf.has_mtx = b.has_mtx; xf.pad = 0; xf.vp[0] = vw; xf.vp[1] = vh;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ui.pl, 0, 1, &sets[i], 0, nullptr);
        vkCmdPushConstants(cb, g_ui.pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof xf, &xf);
        vkCmdDraw(cb, b.count, 1, b.first, 0);
    }
    vkCmdEndRendering(cb);
}

// ---- Input latency: when frames reach the screen, and pacing to it (VK_GOOGLE_display_timing) -----------------------
// Under FIFO the presentation queue fills: the pump presents as fast as the game delivers and only blocks in acquire
// once every image is taken, so on the S26 (4 images) each frame waited behind two or three others before it was
// shown: tens of milliseconds between the controller and the screen. With display timing, each present carries an id
// and Android reports when it was actually shown; the pump then takes the game's next frame only while at most
// BO2_VK_PACE_AHEAD (default 2) presented frames are not shown yet. The game's frame queue (vk_capture.cpp) holds the
// game until its frame is taken, so the game starts its next frame, and reads the pad, that much later. A wait never
// lasts more than 33 ms, and pacing stays off while Android has not reported a shown frame in the last half second.
// BO2_VK_PACE=0 turns pacing off; [LATENCY] is logged either way (present to screen, frames waiting, time paced).
namespace {
const bool g_pace = [] { const char* e = getenv("BO2_VK_PACE"); return !e || *e != '0'; }();
const uint32_t g_pace_ahead = [] {
    const char* e = getenv("BO2_VK_PACE_AHEAD");
    const int v = e ? atoi(e) : 2;
    return (uint32_t)(v < 1 ? 1 : v > 6 ? 6 : v);
}();
constexpr uint32_t kPresentRing = 64;
uint32_t g_present_id = 0;                    // ids handed to the swapchain (0: none yet)
uint32_t g_shown_id = 0;                      // the newest id Android reported shown
uint64_t g_present_ns[kPresentRing] = {};     // CLOCK_MONOTONIC at each id's vkQueuePresentKHR
VkSwapchainKHR g_timed_chain = VK_NULL_HANDLE;
int64_t g_last_report_ns = 0;
// [LATENCY] window
uint64_t g_lat_n = 0, g_lat_sum_us = 0, g_lat_max_us = 0, g_ahead_sum = 0, g_ahead_n = 0, g_paced_us = 0, g_pace_turns = 0;
uint32_t g_lat_hist[8] = {};                  // present to screen: <8, <16, <25, <33, <50, <67, <100, >=100 ms
int64_t g_lat_t0_ns = 0;
// The pump's own stage: from taking the game's frame to presenting it (recording, the GPU work it waits on, acquire).
int64_t g_take_ns = 0;
uint64_t g_tp_n = 0, g_tp_sum_us = 0, g_tp_max_us = 0;

int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Collects what Android reports shown since the last call (the swapchain's thread: the pump's).
void timing_poll() {
    if (!g_dev.display_timing || !g_swap.chain || !vkGetPastPresentationTimingGOOGLE) return;
    if (g_swap.chain != g_timed_chain) {   // a new swapchain: the old one's pending frames are never reported
        g_timed_chain = g_swap.chain;
        g_shown_id = g_present_id;
        g_last_report_ns = 0;
    }
    VkPastPresentationTimingGOOGLE t[16];
    for (int round = 0; round < 4; round++) {
        uint32_t n = 16;
        const VkResult r = vkGetPastPresentationTimingGOOGLE(g_dev.dev, g_swap.chain, &n, t);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) return;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t id = t[i].presentID;
            if (id > g_shown_id && id <= g_present_id) g_shown_id = id;
            if (id + kPresentRing > g_present_id && g_present_ns[id % kPresentRing] &&
                t[i].actualPresentTime > g_present_ns[id % kPresentRing]) {
                const uint64_t us = (t[i].actualPresentTime - g_present_ns[id % kPresentRing]) / 1000;
                if (us < 1000000) {
                    ++g_lat_n; g_lat_sum_us += us; g_lat_max_us = std::max(g_lat_max_us, us);
                    const uint64_t ms = us / 1000;
                    g_lat_hist[ms < 8 ? 0 : ms < 16 ? 1 : ms < 25 ? 2 : ms < 33 ? 3 : ms < 50 ? 4 : ms < 67 ? 5 : ms < 100 ? 6 : 7]++;
                }
            }
        }
        if (n) g_last_report_ns = mono_ns();
        if (r != VK_INCOMPLETE) break;
    }
}

// Before the pump takes the game's next frame: wait while too many presented frames are not on screen yet.
void pace_before_take() {
    if (!g_dev.display_timing || !g_swap.chain) return;
    timing_poll();
    const int64_t start = mono_ns();
    if (g_pace && g_last_report_ns && start - g_last_report_ns < 500000000LL) {
        while (g_present_id - g_shown_id > g_pace_ahead && mono_ns() - start < 33000000LL) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            timing_poll();
        }
        g_paced_us += (uint64_t)((mono_ns() - start) / 1000);
        ++g_pace_turns;
    }
    g_ahead_sum += g_present_id - g_shown_id;
    ++g_ahead_n;
    const int64_t now = mono_ns();
    if (!g_lat_t0_ns) g_lat_t0_ns = now;
    if (now - g_lat_t0_ns >= 10000000000LL) {
        fprintf(stderr, "[LATENCY] present to screen: %.1f ms average, %.1f max over %llu frames (<8 %u, <16 %u, <25 %u, "
                        "<33 %u, <50 %u, <67 %u, <100 %u, more %u); presented but not shown %.2f on average; pacing %s "
                        "(ahead %u): %.2f ms waited per frame; frame taken to presented %.1f ms on average, %.1f at most\n",
                g_lat_n ? g_lat_sum_us / 1000.0 / g_lat_n : 0.0, g_lat_max_us / 1000.0, (unsigned long long)g_lat_n,
                g_lat_hist[0], g_lat_hist[1], g_lat_hist[2], g_lat_hist[3], g_lat_hist[4], g_lat_hist[5], g_lat_hist[6],
                g_lat_hist[7], g_ahead_n ? (double)g_ahead_sum / g_ahead_n : 0.0,
                !g_pace ? "off (BO2_VK_PACE=0)" : g_last_report_ns ? "on" : "waiting for Android's reports",
                g_pace_ahead, g_pace_turns ? g_paced_us / 1000.0 / g_pace_turns : 0.0,
                g_tp_n ? g_tp_sum_us / 1000.0 / g_tp_n : 0.0, g_tp_max_us / 1000.0);
        g_lat_n = g_lat_sum_us = g_lat_max_us = g_ahead_sum = g_ahead_n = g_paced_us = g_pace_turns = 0;
        g_tp_n = g_tp_sum_us = g_tp_max_us = 0;
        memset(g_lat_hist, 0, sizeof g_lat_hist);
        g_lat_t0_ns = now;
    }
}
}  // namespace

// Present the back buffer (or, headless, submit and keep it). Ends and submits the frame's command buffer.
void present(VkCommandBuffer cb, bool fresh) {
    (void)fresh;
    Frame& f = frame_cur();
    uint32_t idx = ~0u;
    bool have_image = false;
    if (g_swap.chain && !g_swap.needs_rebuild) {
        const VkResult ar = vkAcquireNextImageKHR(g_dev.dev, g_swap.chain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &idx);
        if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) have_image = true;
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) g_swap.needs_rebuild = true;
        if (ar == VK_ERROR_SURFACE_LOST_KHR) g_surface_changed.store(true);
    }
    if (have_image) {
        // The panel as the user sees it (landscape): the surface's extent, unrotated.
        const bool rot = g_swap_transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                         g_swap_transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
        const int pw = (int)(rot ? g_swap.extent.height : g_swap.extent.width);
        const int ph = (int)(rot ? g_swap.extent.width : g_swap.extent.height);
        int dx = 0, dy = 0, dw = pw, dh = ph;
        if (const ShellHooks* h = hooks(); h && h->dest_rect) {
            h->dest_rect(pw, ph, &dx, &dy, &dw, &dh);
        } else {   // aspect fit, centred
            const double s = std::min((double)pw / g_backbuffer->w, (double)ph / g_backbuffer->h);
            dw = (int)(g_backbuffer->w * s); dh = (int)(g_backbuffer->h * s);
            dx = (pw - dw) / 2; dy = (ph - dh) / 2;
        }
        // Upscaling (the back buffer drawn into a bigger rectangle, in the panel's pixels: the rotation does not change
        // which is bigger) goes through SGSR when it is chosen and built; the same size, or smaller, is a plain copy.
        const bool up = (uint32_t)dw > g_backbuffer->w || (uint32_t)dh > g_backbuffer->h;
        const bool sgsr = up && g_upscaler.load(std::memory_order_relaxed) == 1 && g_sgsr.pl != VK_NULL_HANDLE;
        Small& ps = sgsr ? g_sgsr : g_pres;
        VkPipeline pipe = small_pipe(ps, g_swap.fmt, false);
        // NDC of the rectangle (Vulkan: y down), then the rotation the presentation engine expects us to have done.
        // vi (SGSR's fragment shader only): 1/w, 1/h, w, h of the source.
        struct { float rect[4]; float rot0[2]; float rot1[2]; float vi[4]; } pc;
        pc.vi[0] = 1.f / g_backbuffer->w; pc.vi[1] = 1.f / g_backbuffer->h;
        pc.vi[2] = (float)g_backbuffer->w; pc.vi[3] = (float)g_backbuffer->h;
        pc.rect[0] = 2.f * dx / pw - 1.f; pc.rect[1] = 2.f * dy / ph - 1.f;
        pc.rect[2] = 2.f * (dx + dw) / pw - 1.f; pc.rect[3] = 2.f * (dy + dh) / ph - 1.f;
        float c = 1, s = 0;   // rotate clip space by the transform's angle
        if (g_swap_transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) { c = 0; s = 1; }
        else if (g_swap_transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) { c = -1; s = 0; }
        else if (g_swap_transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) { c = 0; s = -1; }
        pc.rot0[0] = c; pc.rot0[1] = s; pc.rot1[0] = -s; pc.rot1[1] = c;
        image_transition(cb, *g_backbuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkDescriptorSet set = texture_set(ps, image_view(*g_backbuffer, 0, 0), g_samp_default.plain);
        VkImage sw = g_swap.images[idx];
        raw_barrier(cb, sw, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = sw;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_swap.fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(g_dev.dev, &vi, nullptr, &view);
        destroy_later([view] { vkDestroyImageView(g_dev.dev, view, nullptr); });
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // bars (16:9 on a wider panel) are black
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{0.f, 0.f, 0.f, 1.f}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, g_swap.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cb, &ri);
        if (pipe && set) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            const VkViewport vp{0, 0, (float)g_swap.extent.width, (float)g_swap.extent.height, 0, 1};
            const VkRect2D sc{{0, 0}, g_swap.extent};
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps.pl, 0, 1, &set, 0, nullptr);
            if (sgsr)
                vkCmdPushConstants(cb, ps.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            else
                vkCmdPushConstants(cb, ps.pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, &pc);
            vkCmdDraw(cb, 4, 1, 0, 0);
        }
        vkCmdEndRendering(cb);
        raw_barrier(cb, sw, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (have_image) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &g_swap.rendered[idx];
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (vkQueueSubmit(g_dev.queue, 1, &si, f.done) != VK_SUCCESS) {
        static int n = 0;
        if (++n <= 4) fprintf(stderr, "[VK] vkQueueSubmit failed\n");
        return;
    }
    f.submitted = true;
    if (have_image) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g_swap.rendered[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_swap.chain;
        pi.pImageIndices = &idx;
        // Display timing: an id per present, so Android can say when it was shown (pace_before_take).
        VkPresentTimeGOOGLE when{};
        VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
        if (g_dev.display_timing) {
            when.presentID = ++g_present_id;
            when.desiredPresentTime = 0;   // as soon as possible: FIFO order, as without timing
            times.swapchainCount = 1;
            times.pTimes = &when;
            pi.pNext = &times;
            g_present_ns[g_present_id % kPresentRing] = (uint64_t)mono_ns();
        }
        const VkResult pr = vkQueuePresentKHR(g_dev.queue, &pi);
        if (fresh && g_take_ns) {
            const uint64_t us = (uint64_t)(mono_ns() - g_take_ns) / 1000;
            ++g_tp_n;
            g_tp_sum_us += us;
            g_tp_max_us = std::max(g_tp_max_us, us);
            g_take_ns = 0;
        }
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g_swap.needs_rebuild = true;
    }
    g_frames.fetch_add(1);
    if (const ShellHooks* h = hooks(); h && h->frame_presented) h->frame_presented();
}

}  // namespace bo2vk

using namespace bo2vk;

// BO2_VK_WATCH=1 (diagnostics): the pump loop names the step it is in; a watchdog thread reports any step that
// lasts more than 10 s (a frame that never presents: waiting on the GPU, a long render, an acquire that blocks).
namespace {
std::atomic<const char*> g_watch_stage{"idle"};
std::atomic<int64_t> g_watch_since{0};
int64_t watch_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void watch_stage(const char* name) {
    static const bool on = [] {
        const char* e = getenv("BO2_VK_WATCH");
        if (!e || atoi(e) == 0) return false;
        std::thread([] {
            const char* last = nullptr;
            int64_t last_since = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                const char* st = g_watch_stage.load();
                const int64_t since = g_watch_since.load();
                const int64_t dt = watch_now_ms() - since;
                if (dt > 10000 && (st != last || since != last_since || dt % 60000 < 5000)) {
                    fprintf(stderr, "[VK-WATCH] pump step '%s' for %lld s (%llu frames presented)\n", st,
                            (long long)(dt / 1000), (unsigned long long)g_frames.load());
                    last = st;
                    last_since = since;
                }
            }
        }).detach();
        return true;
    }();
    if (!on) return;
    g_watch_stage.store(name);
    g_watch_since.store(watch_now_ms());
}
}  // namespace

extern "C" bool gfx_pump_once() {
    if (!g_open.load()) return false;
    const ShellHooks* h = hooks();
    watch_stage("shell pump");
    if (h && h->pump && !h->pump()) return false;   // the app is closing
    if (h && h->foreground) {
        const bool fg = h->foreground() != 0;
        if (fg != g_foreground.exchange(fg)) {
            push_focus(fg);
            if (!fg) pipeline_cache_tick(true);   // backgrounded: Android may kill the app from here on
        }
    }
    if (g_surface_changed.load()) surface_rebuild();
    if (g_swap.needs_rebuild) swapchain_build();
    watch_stage("pace (frames presented but not shown yet)");
    pace_before_take();
    // The newest complete frame, if one came since the last turn.
    static RdFrame draws;
    bool fresh = false;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(g_rd_mu);
        if (!g_rd_frame.recs.empty() && g_rd_frame_seq != g_frame_seq_drawn) {
            draws.clear();
            draws.swap(g_rd_frame);
            seq = g_frame_seq_drawn = g_rd_frame_seq;
            fresh = true;
            g_rd_taken_seq.store(seq);
        }
    }
    if (fresh) g_rd_cv.notify_all();   // the frame queue: the game's render thread may publish the next frame
    if (fresh) g_take_ns = mono_ns();
    const auto now = std::chrono::steady_clock::now();
    if (fresh) g_last_fresh = now;
    else if (now - g_last_fresh > std::chrono::seconds(1))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));   // 20 Hz while the game produces nothing
    pipeline_cache_tick(false);
    watch_stage("frame_begin (waits for the frame slot's fence)");
    VkCommandBuffer cb = frame_begin();
    watch_stage("apply_pending");
    apply_pending(cb);
    {
        std::lock_guard<std::mutex> lk(g_stage_mu);
        if (g_staged.ready) {
            g_staged.ready = false;
            TexStage ts;
            ts.bytes.swap(g_staged.bytes);
            ts.w = g_staged.w; ts.h = g_staged.h; ts.fmt = g_staged.fmt; ts.pitch = g_staged.pitch;
            const uint32_t kStagedId = 0xFFFFFFF0u;
            g_tex.erase(kStagedId);
            upload_texture(cb, kStagedId, ts);
            if (auto it = g_tex.find(kStagedId); it != g_tex.end()) { g_staged_img = it->second; g_tex.erase(it); }
        }
    }
    if (fresh) {
        watch_stage("render_frame");
        render_frame(cb, draws, seq);
        watch_stage("ui_pass");
        ui_pass(cb);
        // Textures D3D released: gone once the frame that may read them is drawn (the GPU's use waits on destroy_later).
        std::lock_guard<std::mutex> lk(g_texreg_mu);
        size_t keep = 0;
        for (auto& r : g_texrel_pending) {
            if (r.second > seq) { g_texrel_pending[keep++] = r; continue; }
            if (auto it = g_tex.find(r.first); it != g_tex.end()) {
                ImagePtr dead = it->second;
                g_tex.erase(it);
                if (dead) destroy_later([dead] { image_destroy(*dead); });
            }
            publish_texture_dims(r.first, nullptr);
            g_tex_gone.insert(r.first);
        }
        g_texrel_pending.resize(keep);
    }
    watch_stage("present");
    present(cb, fresh);
    watch_stage("frame cap");
    // The frame cap (the shell's graphics setting); v-sync bounds it above.
    const int cap = h && h->frame_cap ? h->frame_cap() : 0;
    if (cap > 0) {
        const auto period = std::chrono::nanoseconds(1000000000LL / cap);
        const auto next = g_last_present + period;
        const auto t = std::chrono::steady_clock::now();
        if (next > t && next - t < period * 2) std::this_thread::sleep_until(next);
        g_last_present = std::max(next, std::chrono::steady_clock::now() - period);
    }
    return true;
}

// Graphics -> Upscaler (vk_hooks.h).
extern "C" void bo2vk_set_upscaler(int mode) {
    g_upscaler.store(mode, std::memory_order_relaxed);
}

// Tests: the back buffer as RGBA8.
extern "C" int bo2vk_read_backbuffer(uint8_t* rgba, uint32_t capacity, uint32_t* w, uint32_t* h) {
    if (!g_dev.ok || !g_backbuffer) return 0;
    vkDeviceWaitIdle(g_dev.dev);
    const uint32_t bw = g_backbuffer->w, bh = g_backbuffer->h;
    if (w) *w = bw;
    if (h) *h = bh;
    const VkDeviceSize n = (VkDeviceSize)bw * bh * 4;
    if (!rgba || capacity < n) return 0;
    Buffer rb = buffer_create(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!rb.buf || !rb.mem.mapped) return 0;
    submit_now([&](VkCommandBuffer cb) {
        image_transition(cb, *g_backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {bw, bh, 1};
        vkCmdCopyImageToBuffer(cb, g_backbuffer->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
    });
    VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    mr.memory = rb.mem.mem; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(g_dev.dev, 1, &mr);
    const uint8_t* s = rb.mem.mapped;
    for (VkDeviceSize i = 0; i < n; i += 4) {   // BGRA -> RGBA
        rgba[i] = s[i + 2]; rgba[i + 1] = s[i + 1]; rgba[i + 2] = s[i]; rgba[i + 3] = s[i + 3];
    }
    buffer_destroy(rb);
    return 1;
}
