// vk_render.h — the render thread's state: what the pump built from the registrations (vk_resources.cpp) and uses to
// replay a frame (vk_render.cpp). Only the thread that calls gfx_pump_once touches it.
#pragma once
#include "vk_state.h"

#include <set>

namespace bo2vk {

// One shader: its module, its vertex inputs, its declared bindings (set 0) and the maps from dxbc_to_vk.cpp.
enum class DescKind : uint8_t { Ubo, Texture, Sampler, ShadowSampler };
struct Decl { uint32_t binding; DescKind kind; };
struct ShaderRec {
    bool pixel = false;
    VkShaderModule module = VK_NULL_HANDLE;
    std::unordered_map<std::string, uint32_t> attrs;   // "TEXCOORD0" -> location
    std::vector<Decl> decls;                           // from the source: every binding it declares
    std::unordered_map<uint32_t, uint32_t> cb, tex;     // binding -> D3D register
    std::unordered_map<uint32_t, int> smp;              // binding -> s register, -1 = point clamp
    std::unordered_map<uint32_t, uint32_t> dim;         // binding -> HLSLCC_TEX_DIMENSION
};
extern std::unordered_map<uint32_t, ShaderRec> g_shaders;

// Textures and render targets by guest object id (one registry, as the macOS backend's g_texreg).
extern std::unordered_map<uint32_t, ImagePtr> g_tex;
extern std::set<uint32_t> g_rt_cleared;              // render targets that had their first use
extern std::set<uint32_t> g_rtv_targets;             // resources some RTV renders into
extern std::unordered_map<uint32_t, uint32_t> g_rtv_res, g_rtv_mip;
extern std::set<uint32_t> g_tex_gone;                // released (late uses are counted)

// Samplers: each game sampler as declared (comparison off) and as a comparison sampler (for samplerShadow).
struct SamplerPair { VkSampler plain = VK_NULL_HANDLE, compare = VK_NULL_HANDLE; };
extern std::unordered_map<uint32_t, SamplerPair> g_samplers;
extern SamplerPair g_samp_default, g_samp_point;

// Stand-ins: white (an untextured slot), transparent black (a declared slot with nothing bound), and typed
// ones for 3D, cube and depth slots, so a slot never reads an image of the wrong type.
extern ImagePtr g_white, g_null2d, g_null3d, g_nullcube, g_nulldepth;
extern Buffer g_zero_ubo, g_zero_vb;

// The game's back buffer, at the guest monitor's size, and the depth buffers keyed (w, h, screen).
extern ImagePtr g_backbuffer;
extern std::unordered_map<uint64_t, ImagePtr> g_depth;

// vk_resources.cpp
bool resources_init();
void resources_shutdown();
void apply_pending(VkCommandBuffer cb);              // textures, render targets, RTVs, shaders, maps, samplers
void upload_texture(VkCommandBuffer cb, uint32_t obj, TexStage& ts);
void publish_texture_dims(uint32_t obj, const Image* im);
ImagePtr depth_for(uint32_t w, uint32_t h, bool screen);

// vk_render.cpp
void render_frame(VkCommandBuffer cb, const RdFrame& frame, uint64_t seq);
void ui_pass(VkCommandBuffer cb);
void present(VkCommandBuffer cb, bool fresh);

// vk_window.cpp: the swapchain, if there is a window.
struct Swapchain {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR chain = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageLayout> layouts;
    std::vector<VkSemaphore> rendered;   // one per image: signalled by the submit that renders it, waited by present
    bool needs_rebuild = false;
};
extern Swapchain g_swap;
bool swapchain_build();
void swapchain_destroy();

}  // namespace bo2vk
