// vk_backend.h — internals of the Vulkan backend (milestone M5; docs/GRAPHICS_VULKAN.md).
//
// The backend implements the 35 gfx_* functions bo2-mac's runtime calls, as bo2-mac's metal_backend.mm does on
// macOS, in three parts:
//   vk_capture.cpp  the guest-thread side: every gfx_register_*, gfx_real_draw / gfx_real_frame_end (each frame's
//                   draws copied into an arena and handed over whole), releases, and the queries the runtime makes.
//   vk_render.cpp   gfx_pump_once on the host's main thread: uploads what was registered, replays the newest frame
//                   (passes, pipelines, descriptors, draws, copies, clears) and presents it.
//   vk_window.cpp   the window, the swapchain, input, the pointer, gfx_window_open / gfx_shutdown.
//   vk_core.cpp     the Vulkan device: the loader, memory, buffers, images, barriers, frames in flight.
// Vulkan 1.3 is required (dynamic rendering, synchronization2, extended dynamic state): every S26 GPU has it.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx_abi.h"

namespace bo2vk {

// ---- Loader: every entry point, resolved at run time (libvulkan.so on Android, vulkan-1.dll on the PC) ------------
#define BO2VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceVersion)
#define BO2VK_INSTANCE_FNS(X)                                                                                     \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                        \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice)            \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkDestroySurfaceKHR)                      \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define BO2VK_DEVICE_FNS(X)                                                                                         \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle)               \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers)             \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)          \
    X(vkResetFences) X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkAllocateMemory)          \
    X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges)                                 \
    X(vkInvalidateMappedMemoryRanges) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)      \
    X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)                     \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler)       \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout)                              \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)                         \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreatePipelineCache) X(vkDestroyPipelineCache)         \
    X(vkGetPipelineCacheData) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool)      \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdPipelineBarrier2) X(vkCmdBeginRendering)        \
    X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers)               \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor)              \
    X(vkCmdSetDepthBias) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference)   \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable)             \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp)      \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdCopyBufferToImage)                       \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdBlitImage) X(vkCmdCopyImage) X(vkCmdClearColorImage)     \
    X(vkCmdClearDepthStencilImage) X(vkCmdPushConstants) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)        \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)                                     \
    X(vkGetPastPresentationTimingGOOGLE) X(vkGetRefreshCycleDurationGOOGLE)

#define BO2VK_DECLARE(f) extern PFN_##f f;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
BO2VK_GLOBAL_FNS(BO2VK_DECLARE)
BO2VK_INSTANCE_FNS(BO2VK_DECLARE)
BO2VK_DEVICE_FNS(BO2VK_DECLARE)
#undef BO2VK_DECLARE

// ---- Memory: blocks of 64 MiB per memory type, sub-allocated first-fit (mobile drivers allow ~4096 allocations,
// fewer than BO2's textures) ----------------------------------------------------------------------------------------
struct Alloc {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    uint8_t* mapped = nullptr;    // host-visible blocks are mapped once, whole
    int block = -1;
    uint32_t type = 0;
    explicit operator bool() const { return mem != VK_NULL_HANDLE; }
};
// linear: a buffer (true) or an optimally tiled image (false); the two never share a block.
Alloc mem_alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, bool linear);
void mem_free(Alloc& a);

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    Alloc mem;
    VkDeviceSize size = 0;
};
Buffer buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
void buffer_destroy(Buffer& b);   // immediately: callers defer through destroy_later

// An image and the layout of every subresource (layer-major: layer * mips + mip), so barriers are exact.
struct Image {
    VkImage img = VK_NULL_HANDLE;
    Alloc mem;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkImageViewType vtype = VK_IMAGE_VIEW_TYPE_2D;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t w = 0, h = 0, d = 1, layers = 1, mips = 1;
    uint32_t dxgi = 0;                         // the guest's format
    VkComponentMapping swz{};                  // A8 is R8 seen through a swizzle
    bool render_target = false;
    VkImageView view = VK_NULL_HANDLE;         // all mips, all layers, sampled
    std::vector<VkImageLayout> layout;         // per subresource
    uint64_t sampled_pass = 0;                 // vk_render.cpp: the pass that last listed this image as sampled,
    uint32_t sampled_mip0 = 0, sampled_mips = 0;   // and the range it listed (its barrier is needed once per pass)
    uint64_t kept_gen = 0;                     // vk_render.cpp's kept descriptor sets naming this image's views: their
    std::vector<uint64_t> kept_keys;           // cache keys, dropped when the image is destroyed (descriptor_dead_keys)
    std::unordered_map<uint64_t, VkImageView> extra_views;   // (kind, base mip, mip count) -> view
};
using ImagePtr = std::shared_ptr<Image>;
ImagePtr image_create(VkImageType type, VkImageViewType vtype, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d,
                      uint32_t layers, uint32_t mips, VkImageUsageFlags usage, VkImageCreateFlags flags = 0);
void image_destroy(Image& im);
VkImageView image_view(Image& im, uint32_t base_mip, uint32_t mips);        // sampled view of a mip range
VkImageView image_attachment_view(Image& im, uint32_t mip);                 // one mip, for rendering
// Move subresources [mip0, mip0 + nmips) x all layers to `to`, with a barrier that orders everything before against
// everything after (correct first; tiled GPUs pay little for it between passes).
void image_transition(VkCommandBuffer cb, Image& im, VkImageLayout to, uint32_t mip0 = 0, uint32_t nmips = ~0u);

// Everything destroyed while the GPU may still use it waits until the frames in flight have finished.
void destroy_later(std::function<void()> fn);

// ---- The device -------------------------------------------------------------------------------------------------
// Two, not three: the game is CPU-bound at the display rate, so every buffer between it and the screen fills, and each
// one is a frame of input latency (a Bluetooth controller felt it). The GPU finishes a frame well inside one refresh.
constexpr int kFramesInFlight = 2;
struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    bool submitted = false;
    uint64_t serial = 0;                                   // frame number of the last submit
    // Per-frame upload ring for vertices, indices and constants: grows by doubling, never shrinks.
    Buffer ring;
    VkDeviceSize ring_off = 0;
    std::vector<Buffer> ring_retired;                      // the smaller rings it replaced, freed with the frame
    std::vector<VkDescriptorPool> pools;                   // descriptor pools, reset with the frame
    size_t pool_cur = 0;
    std::vector<std::function<void()>> deferred;           // destroy_later work owed to this frame
};

struct Device {
    bool ok = false;
    void* lib = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memprops{};
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    bool bc = false;                 // textureCompressionBC: else BC textures are decoded on the CPU
    bool depth_clamp = false, anisotropy = false, bgra_vertex = false;
    bool display_timing = false;     // VK_GOOGLE_display_timing: when each frame reached the screen (vk_frame.cpp pacing)
    float max_aniso = 1.f;
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;   // combined depth + stencil
    bool depth_has_stencil = false;
    Frame frames[kFramesInFlight];
    int cur = 0;
    uint64_t serial = 0;             // frames begun
    std::string gpu;
};
extern Device g_dev;

bool device_init(bool want_surface_extensions, const std::vector<const char*>& instance_exts);
void device_shutdown();
// The driver's pipeline cache on disk (<BO2_SHADER_CACHE_DIR>/vk_pipeline_cache.bin). Android kills the app rather
// than shutting it down, so it is saved while running: pipeline_cache_note_new() after each new pipeline,
// pipeline_cache_tick() once per pump (saves in the background when pipelines stopped arriving for a few seconds, or at
// once when the app leaves the foreground).
void pipeline_cache_note_new();
void pipeline_cache_tick(bool leaving_foreground);
// One-off work outside a frame (texture creation before the first frame, readbacks for tests): submit and wait.
void submit_now(const std::function<void(VkCommandBuffer)>& record);
// Ring allocation in the current frame: copies `n` bytes and returns their offset (aligned), or ~0 on failure.
VkDeviceSize ring_push(const void* src, VkDeviceSize n, VkDeviceSize align);
VkBuffer ring_buffer();
VkDescriptorSet descriptor_alloc(VkDescriptorSetLayout layout);
// Descriptor sets that outlive the frame (vk_render.cpp's persistent set cache, BO2_VK_SET_CACHE=2): allocated from
// pools that are not reset with the frame. A set names image views, samplers and buffers by handle, and a destroyed
// handle can come back for a new object, so a kept set must be forgotten when what it names is destroyed:
//  - an image: image_destroy hands the keys of the kept sets naming it (Image::kept_keys) to descriptor_dead_keys,
//    which the cache erases before its next lookup (images die at frame_begin, before anything is planned);
//  - a sampler or a retired ring (rare): the epoch moves, and the cache drops everything.
// descriptor_persistent_drop hands the pools to destroy_later (they go once the frames in flight are done).
VkDescriptorSet descriptor_alloc_persistent(VkDescriptorSetLayout layout);
void descriptor_persistent_drop();
uint64_t descriptor_epoch();
void descriptor_epoch_bump();
std::vector<std::pair<uint64_t, uint64_t>>& descriptor_dead_keys();   // (Image::kept_gen, key), render thread only
// Frame boundaries for vk_render.cpp: begin waits for the frame slot and resets its ring and pools.
VkCommandBuffer frame_begin();
Frame& frame_cur();

// ---- Formats ----------------------------------------------------------------------------------------------------
VkFormat dxgi_to_vk(uint32_t dxgi, VkComponentMapping* swizzle, bool render_target);
bool dxgi_is_bc(uint32_t dxgi);
uint32_t dxgi_bytes_per_block(uint32_t dxgi);   // bytes per pixel, or per 4x4 block for BC formats
VkFormat dxgi_to_vertex_format(uint32_t dxgi);

// ---- Shaders: GLSL from dxbc_to_vk.cpp to SPIR-V (glslang), and what the maps say ------------------------------
bool glsl_to_spirv(bool pixel, const std::string& src, std::vector<uint32_t>& spirv, std::string& log);

// ---- Hooks the Android shell provides (vk_hooks.h); null in tests ------------------------------------------------
struct ShellHooks;
const ShellHooks* hooks();

}  // namespace bo2vk
