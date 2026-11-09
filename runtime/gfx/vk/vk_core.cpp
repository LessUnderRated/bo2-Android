// vk_core.cpp — the Vulkan device under the backend: loader, memory, buffers, images, barriers, frames, formats.
#include "vk_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace bo2vk {

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define BO2VK_DEFINE(f) PFN_##f f = nullptr;
BO2VK_GLOBAL_FNS(BO2VK_DEFINE)
BO2VK_INSTANCE_FNS(BO2VK_DEFINE)
BO2VK_DEVICE_FNS(BO2VK_DEFINE)
#undef BO2VK_DEFINE

Device g_dev;

namespace {

void* lib_open() {
#if defined(_WIN32)
    return (void*)LoadLibraryA("vulkan-1.dll");
#else
    void* h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return h ? h : dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}
void* lib_sym(void* lib, const char* name) {
#if defined(_WIN32)
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

// ---- Memory blocks ------------------------------------------------------------------------------------------------
constexpr VkDeviceSize kBlock = 64ull << 20;
struct Block {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t type = 0;
    bool linear = false;            // buffers and images never share a block (bufferImageGranularity)
    bool dedicated = false;
    uint8_t* mapped = nullptr;
    std::map<VkDeviceSize, VkDeviceSize> free;   // offset -> size
};
std::vector<Block> g_blocks;
std::mutex g_mem_mu;

int find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < g_dev.memprops.memoryTypeCount; i++) {
        const VkMemoryPropertyFlags f = g_dev.memprops.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return (int)i;
    }
    return -1;
}

bool block_take(Block& b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize* out) {
    for (auto it = b.free.begin(); it != b.free.end(); ++it) {
        const VkDeviceSize start = (it->first + align - 1) / align * align;
        const VkDeviceSize end = it->first + it->second;
        if (start + size > end) continue;
        const VkDeviceSize f0 = it->first, f1 = end;
        b.free.erase(it);
        if (start > f0) b.free[f0] = start - f0;
        if (start + size < f1) b.free[start + size] = f1 - (start + size);
        *out = start;
        return true;
    }
    return false;
}

Alloc alloc_in(int type, const VkMemoryRequirements& req, bool linear) {
    std::lock_guard<std::mutex> lk(g_mem_mu);
    const bool host = (g_dev.memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const bool dedicated = req.size > kBlock / 2;
    if (!dedicated)
        for (size_t i = 0; i < g_blocks.size(); i++) {
            Block& b = g_blocks[i];
            if (b.dedicated || b.type != (uint32_t)type || b.linear != linear || !b.mem) continue;
            VkDeviceSize off;
            if (block_take(b, req.size, req.alignment, &off)) {
                Alloc a;
                a.mem = b.mem; a.offset = off; a.size = req.size; a.block = (int)i; a.type = (uint32_t)type;
                a.mapped = b.mapped ? b.mapped + off : nullptr;
                return a;
            }
        }
    Block b;
    b.size = dedicated ? req.size : kBlock;
    b.type = (uint32_t)type; b.linear = linear; b.dedicated = dedicated;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = b.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(g_dev.dev, &ai, nullptr, &b.mem) != VK_SUCCESS) {
        fprintf(stderr, "[VK-MEM] vkAllocateMemory of %llu bytes (type %d) failed\n", (unsigned long long)b.size, type);
        return Alloc{};
    }
    if (host) {
        void* p = nullptr;
        if (vkMapMemory(g_dev.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) b.mapped = (uint8_t*)p;
    }
    if (!dedicated) b.free[0] = b.size;
    // Reuse an emptied slot so indices stay stable.
    int slot = -1;
    for (size_t i = 0; i < g_blocks.size(); i++) if (!g_blocks[i].mem) { slot = (int)i; break; }
    if (slot < 0) { slot = (int)g_blocks.size(); g_blocks.push_back(Block{}); }
    g_blocks[slot] = std::move(b);
    Block& nb = g_blocks[slot];
    Alloc a;
    a.mem = nb.mem; a.size = req.size; a.block = slot; a.type = (uint32_t)type;
    if (!dedicated) {
        VkDeviceSize off = 0;
        block_take(nb, req.size, req.alignment, &off);
        a.offset = off;
    }
    a.mapped = nb.mapped ? nb.mapped + a.offset : nullptr;
    return a;
}

}  // namespace

Alloc mem_alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, bool linear) {
    // Device-local first for GPU-only data; host-visible data prefers coherent memory (no flush needed).
    int t = -1;
    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        t = find_type(req.memoryTypeBits, want | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        if (t < 0) t = find_type(req.memoryTypeBits, want, 0);
    } else {
        t = find_type(req.memoryTypeBits, want, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (t < 0) t = find_type(req.memoryTypeBits, want, 0);
    }
    if (t < 0) {
        fprintf(stderr, "[VK-MEM] no memory type for bits 0x%x flags 0x%x\n", req.memoryTypeBits, (unsigned)want);
        return Alloc{};
    }
    return alloc_in(t, req, linear);
}

void mem_free(Alloc& a) {
    if (!a.mem) return;
    std::lock_guard<std::mutex> lk(g_mem_mu);
    if (a.block >= 0 && a.block < (int)g_blocks.size()) {
        Block& b = g_blocks[a.block];
        if (b.dedicated) {
            vkFreeMemory(g_dev.dev, b.mem, nullptr);
            b = Block{};
        } else {
            // Insert and merge with the neighbours.
            VkDeviceSize off = a.offset, size = a.size;
            auto next = b.free.lower_bound(off);
            if (next != b.free.end() && off + size == next->first) { size += next->second; next = b.free.erase(next); }
            if (next != b.free.begin()) {
                auto prev = std::prev(next);
                if (prev->first + prev->second == off) { off = prev->first; size += prev->second; b.free.erase(prev); }
            }
            b.free[off] = size;
            if (b.free.size() == 1 && b.free.begin()->second == b.size && g_blocks.size() > 8) {
                // An empty block beyond the first few goes back to the driver.
                vkFreeMemory(g_dev.dev, b.mem, nullptr);
                b = Block{};
            }
        }
    }
    a = Alloc{};
}

// ---- Buffers ------------------------------------------------------------------------------------------------------
Buffer buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_dev.dev, &ci, nullptr, &b.buf) != VK_SUCCESS) return Buffer{};
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev.dev, b.buf, &req);
    b.mem = mem_alloc(req, host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    if (!b.mem || vkBindBufferMemory(g_dev.dev, b.buf, b.mem.mem, b.mem.offset) != VK_SUCCESS) {
        vkDestroyBuffer(g_dev.dev, b.buf, nullptr);
        mem_free(b.mem);
        return Buffer{};
    }
    b.size = size;
    return b;
}

void buffer_destroy(Buffer& b) {
    if (b.buf) vkDestroyBuffer(g_dev.dev, b.buf, nullptr);
    mem_free(b.mem);
    b = Buffer{};
}

// ---- Images -------------------------------------------------------------------------------------------------------
static VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM: return VK_IMAGE_ASPECT_DEPTH_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

ImagePtr image_create(VkImageType type, VkImageViewType vtype, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d,
                      uint32_t layers, uint32_t mips, VkImageUsageFlags usage, VkImageCreateFlags flags) {
    auto im = std::make_shared<Image>();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = type;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = mips;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_dev.dev, &ci, nullptr, &im->img) != VK_SUCCESS) {
        fprintf(stderr, "[VK-IMG] vkCreateImage %ux%ux%u fmt %d mips %u layers %u failed\n", w, h, d, (int)fmt, mips,
                layers);
        return nullptr;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev.dev, im->img, &req);
    im->mem = mem_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    if (!im->mem || vkBindImageMemory(g_dev.dev, im->img, im->mem.mem, im->mem.offset) != VK_SUCCESS) {
        vkDestroyImage(g_dev.dev, im->img, nullptr);
        mem_free(im->mem);
        return nullptr;
    }
    im->fmt = fmt; im->vtype = vtype; im->aspect = aspect_of(fmt);
    im->w = w; im->h = h; im->d = d; im->layers = layers; im->mips = mips;
    im->swz = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
               VK_COMPONENT_SWIZZLE_IDENTITY};
    im->layout.assign((size_t)layers * mips, VK_IMAGE_LAYOUT_UNDEFINED);
    return im;
}

static VkImageView make_view(Image& im, VkImageViewType vt, uint32_t base_mip, uint32_t mips, uint32_t base_layer,
                             uint32_t layers, VkImageAspectFlags aspect, bool swizzle) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = vt;
    vi.format = im.fmt;
    if (swizzle) vi.components = im.swz;
    vi.subresourceRange = {aspect, base_mip, mips, base_layer, layers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g_dev.dev, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

// Sampled views read the depth plane of a depth-stencil image (D3D's SRV of a depth format reads depth).
static VkImageAspectFlags sampled_aspect(const Image& im) {
    return (im.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : im.aspect;
}

VkImageView image_view(Image& im, uint32_t base_mip, uint32_t mips) {
    if (base_mip == 0 && (mips == 0 || mips >= im.mips)) {
        if (!im.view) im.view = make_view(im, im.vtype, 0, im.mips, 0, im.layers, sampled_aspect(im), true);
        return im.view;
    }
    if (base_mip >= im.mips) return VK_NULL_HANDLE;
    if (!mips || base_mip + mips > im.mips) mips = im.mips - base_mip;
    const uint64_t key = (1ull << 40) | ((uint64_t)base_mip << 20) | mips;
    auto it = im.extra_views.find(key);
    if (it != im.extra_views.end()) return it->second;
    VkImageView v = make_view(im, im.vtype, base_mip, mips, 0, im.layers, sampled_aspect(im), true);
    im.extra_views[key] = v;
    return v;
}

VkImageView image_attachment_view(Image& im, uint32_t mip) {
    if (mip >= im.mips) return VK_NULL_HANDLE;
    const uint64_t key = (2ull << 40) | mip;
    auto it = im.extra_views.find(key);
    if (it != im.extra_views.end()) return it->second;
    VkImageView v = make_view(im, VK_IMAGE_VIEW_TYPE_2D, mip, 1, 0, 1, im.aspect, false);
    im.extra_views[key] = v;
    return v;
}

void image_destroy(Image& im) {
    // Kept descriptor sets naming this image's views are forgotten before the next lookup (their handles may come back).
    for (uint64_t k : im.kept_keys) descriptor_dead_keys().emplace_back(im.kept_gen, k);
    im.kept_keys.clear();
    for (auto& kv : im.extra_views) if (kv.second) vkDestroyImageView(g_dev.dev, kv.second, nullptr);
    im.extra_views.clear();
    if (im.view) vkDestroyImageView(g_dev.dev, im.view, nullptr);
    if (im.img) vkDestroyImage(g_dev.dev, im.img, nullptr);
    mem_free(im.mem);
    im.view = VK_NULL_HANDLE; im.img = VK_NULL_HANDLE;
}

void image_transition(VkCommandBuffer cb, Image& im, VkImageLayout to, uint32_t mip0, uint32_t nmips) {
    if (mip0 >= im.mips) return;
    if (nmips > im.mips - mip0) nmips = im.mips - mip0;
    VkImageMemoryBarrier2 bars[64];
    uint32_t n = 0;
    auto flush = [&] {
        if (!n) return;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = n;
        di.pImageMemoryBarriers = bars;
        vkCmdPipelineBarrier2(cb, &di);
        n = 0;
    };
    for (uint32_t layer = 0; layer < im.layers; layer++)
        for (uint32_t m = mip0; m < mip0 + nmips; m++) {
            VkImageLayout& cur = im.layout[(size_t)layer * im.mips + m];
            if (cur == to) continue;
            VkImageMemoryBarrier2& b = bars[n++];
            b = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = cur;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = im.img;
            b.subresourceRange = {im.aspect, m, 1, layer, 1};
            cur = to;
            if (n == 64) flush();
        }
    flush();
}

// ---- Frames, the ring, descriptor pools, deferred destruction --------------------------------------------------------
void destroy_later(std::function<void()> fn) {
    if (!g_dev.ok) { fn(); return; }
    g_dev.frames[g_dev.cur].deferred.push_back(std::move(fn));
}

Frame& frame_cur() { return g_dev.frames[g_dev.cur]; }

VkCommandBuffer frame_begin() {
    g_dev.cur = (int)(g_dev.serial % kFramesInFlight);
    Frame& f = g_dev.frames[g_dev.cur];
    if (f.submitted) {
        vkWaitForFences(g_dev.dev, 1, &f.done, VK_TRUE, UINT64_MAX);
        f.submitted = false;
    }
    vkResetFences(g_dev.dev, 1, &f.done);
    // Waiting on this fence also waited for every earlier submission (a fence's first synchronization scope is all
    // work submitted before it), so what was deferred while this slot was last recorded is now unused.
    for (auto& fn : f.deferred) fn();
    f.deferred.clear();
    if (!f.ring_retired.empty()) descriptor_epoch_bump();   // cached descriptor sets may name a retired ring
    for (Buffer& b : f.ring_retired) buffer_destroy(b);
    f.ring_retired.clear();
    f.ring_off = 0;
    for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(g_dev.dev, p, 0);
    f.pool_cur = 0;
    vkResetCommandPool(g_dev.dev, f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cb, &bi);
    f.serial = ++g_dev.serial;
    return f.cb;
}

VkDeviceSize ring_push(const void* src, VkDeviceSize n, VkDeviceSize align) {
    Frame& f = frame_cur();
    if (align < 16) align = 16;
    VkDeviceSize off = (f.ring_off + align - 1) / align * align;
    if (!f.ring.buf || off + n > f.ring.size) {
        VkDeviceSize want = f.ring.size ? f.ring.size * 2 : (16ull << 20);
        while (want < n + align) want *= 2;
        if (f.ring.buf) f.ring_retired.push_back(f.ring);   // still read by this frame's earlier commands
        f.ring = buffer_create(want, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               true);
        if (!f.ring.buf || !f.ring.mem.mapped) {
            fprintf(stderr, "[VK-RING] cannot grow the upload ring to %llu bytes\n", (unsigned long long)want);
            f.ring = Buffer{};
            return ~0ull;
        }
        off = 0;
    }
    memcpy(f.ring.mem.mapped + off, src, (size_t)n);
    f.ring_off = off + n;
    return off;
}

VkBuffer ring_buffer() { return frame_cur().ring.buf; }

namespace {
// Sets from a list of pools, a new pool when the current one is full.
VkDescriptorSet alloc_from(std::vector<VkDescriptorPool>& pools, size_t& cur, VkDescriptorSetLayout layout) {
    for (;;) {
        if (cur == pools.size()) {
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32768},
                                                  {VK_DESCRIPTOR_TYPE_SAMPLER, 32768},
                                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768},
                                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768}};
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            ci.maxSets = 8192;
            ci.poolSizeCount = 4;
            ci.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(g_dev.dev, &ci, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pools[cur];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet s = VK_NULL_HANDLE;
        const VkResult r = vkAllocateDescriptorSets(g_dev.dev, &ai, &s);
        if (r == VK_SUCCESS) return s;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return VK_NULL_HANDLE;
        cur++;
    }
}
std::vector<VkDescriptorPool> g_persist_pools;   // render thread only
size_t g_persist_cur = 0;
std::atomic<uint64_t> g_desc_epoch{1};
}  // namespace

VkDescriptorSet descriptor_alloc(VkDescriptorSetLayout layout) {
    Frame& f = frame_cur();
    return alloc_from(f.pools, f.pool_cur, layout);
}

VkDescriptorSet descriptor_alloc_persistent(VkDescriptorSetLayout layout) {
    return alloc_from(g_persist_pools, g_persist_cur, layout);
}

void descriptor_persistent_drop() {
    if (g_persist_pools.empty()) return;
    std::vector<VkDescriptorPool> dead;
    dead.swap(g_persist_pools);
    g_persist_cur = 0;
    destroy_later([dead] { for (VkDescriptorPool p : dead) vkDestroyDescriptorPool(g_dev.dev, p, nullptr); });
}

uint64_t descriptor_epoch() { return g_desc_epoch.load(std::memory_order_relaxed); }
void descriptor_epoch_bump() { g_desc_epoch.fetch_add(1, std::memory_order_relaxed); }
std::vector<std::pair<uint64_t, uint64_t>>& descriptor_dead_keys() {
    static std::vector<std::pair<uint64_t, uint64_t>> dead;
    return dead;
}

void submit_now(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = g_dev.queue_family;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(g_dev.dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(g_dev.dev, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    record(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g_dev.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_dev.queue);
    vkDestroyCommandPool(g_dev.dev, pool, nullptr);
}

// ---- Device creation -----------------------------------------------------------------------------------------------
static bool has_ext(const std::vector<VkExtensionProperties>& v, const char* name) {
    for (const auto& e : v) if (!strcmp(e.extensionName, name)) return true;
    return false;
}

// One [VK] line with what could take per-draw work off the CPU: push descriptors, bindless (descriptor indexing),
// descriptor buffers, imported host memory, multi-draw, and the dynamic-state extensions. Queries only: nothing here is
// enabled. A property struct is chained only when its extension is listed (the instance asks for Vulkan 1.3, so 1.4's
// own structs are out of bounds; the extensions carry the same limits).
static void log_draw_caps(const std::vector<VkExtensionProperties>& dext) {
    auto has = [&](const char* n) { for (const auto& e : dext) if (!strcmp(e.extensionName, n)) return true; return false; };
    const bool push = has("VK_KHR_push_descriptor"), dbuf = has("VK_EXT_descriptor_buffer"),
               host = has("VK_EXT_external_memory_host"), multi = has("VK_EXT_multi_draw");
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDevicePushDescriptorPropertiesKHR ppush{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT pdb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT phost{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceMultiDrawPropertiesEXT pmulti{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    void** tail = &p2.pNext;
    auto chain = [&tail](void* s, void** next) { *tail = s; tail = next; };
    chain(&p12, &p12.pNext);
    chain(&p13, &p13.pNext);
    if (push) chain(&ppush, &ppush.pNext);
    if (dbuf) chain(&pdb, &pdb.pNext);
    if (host) chain(&phost, &phost.pNext);
    if (multi) chain(&pmulti, &pmulti.pNext);
    vkGetPhysicalDeviceProperties2(g_dev.phys, &p2);
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT fdb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceMultiDrawFeaturesEXT fmulti{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    tail = &f2.pNext;
    chain(&f12, &f12.pNext);
    if (dbuf) chain(&fdb, &fdb.pNext);
    if (multi) chain(&fmulti, &fmulti.pNext);
    vkGetPhysicalDeviceFeatures2(g_dev.phys, &f2);
    std::string dyn;
    for (const char* n : {"VK_EXT_extended_dynamic_state3", "VK_EXT_vertex_input_dynamic_state",
                          "VK_EXT_graphics_pipeline_library", "VK_EXT_shader_object", "VK_KHR_maintenance5",
                          "VK_KHR_maintenance6", "VK_KHR_dynamic_rendering_local_read",
                          "VK_EXT_dynamic_rendering_unused_attachments", "VK_KHR_pipeline_binary"})
        if (has(n)) dyn += std::string(dyn.empty() ? "" : ",") + (n + 3);
    char b[160];
    std::string line = "[VK] draw-path features (queried, not enabled): push descriptors ";
    if (push) { snprintf(b, sizeof b, "KHR max %u", ppush.maxPushDescriptors); line += b; } else line += "no";
    snprintf(b, sizeof b, "; descriptor indexing %s (runtime arrays %s, partially bound %s, sampled-image update-after-bind "
                          "%s, %u per stage, %u in all pools)",
             f12.descriptorIndexing ? "yes" : "no", f12.runtimeDescriptorArray ? "yes" : "no",
             f12.descriptorBindingPartiallyBound ? "yes" : "no",
             f12.descriptorBindingSampledImageUpdateAfterBind ? "yes" : "no",
             p12.maxPerStageDescriptorUpdateAfterBindSampledImages, p12.maxUpdateAfterBindDescriptorsInAllPools);
    line += b;
    if (dbuf) {
        snprintf(b, sizeof b, "; descriptor buffer EXT %s (offset align %llu, image %zu B, sampler %zu B, UBO %zu B, push %s)",
                 fdb.descriptorBuffer ? "yes" : "listed, feature off", (unsigned long long)pdb.descriptorBufferOffsetAlignment,
                 pdb.sampledImageDescriptorSize, pdb.samplerDescriptorSize, pdb.uniformBufferDescriptorSize,
                 fdb.descriptorBufferPushDescriptors ? "yes" : "no");
        line += b;
    } else line += "; descriptor buffer no";
    if (host) {
        snprintf(b, sizeof b, "; external memory host EXT (align %llu)", (unsigned long long)phost.minImportedHostPointerAlignment);
        line += b;
    } else line += "; external memory host no";
    if (multi) {
        snprintf(b, sizeof b, "; multi draw EXT %s (max %u)", fmulti.multiDraw ? "yes" : "listed, feature off", pmulti.maxMultiDrawCount);
        line += b;
    } else line += "; multi draw no";
    snprintf(b, sizeof b, "; inline uniform block %u B; dynamic UBOs per set %u", p13.maxInlineUniformBlockSize,
             g_dev.props.limits.maxDescriptorSetUniformBuffersDynamic);
    line += b;
    line += "; also listed: " + (dyn.empty() ? std::string("none of the dynamic-state extensions") : dyn);
    fprintf(stderr, "%s\n", line.c_str());
}

static bool format_has(VkFormat f, VkFormatFeatureFlags feat, bool buffer = false) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_dev.phys, f, &p);
    return ((buffer ? p.bufferFeatures : p.optimalTilingFeatures) & feat) == feat;
}

bool device_init(bool want_surface, const std::vector<const char*>& instance_exts) {
    if (g_dev.ok) return true;
    g_dev.lib = lib_open();
    if (!g_dev.lib) { fprintf(stderr, "[VK] no Vulkan library\n"); return false; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)lib_sym(g_dev.lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) return false;
#define BO2VK_LOAD_GLOBAL(f) f = (PFN_##f)vkGetInstanceProcAddr(VK_NULL_HANDLE, #f);
    BO2VK_GLOBAL_FNS(BO2VK_LOAD_GLOBAL)
#undef BO2VK_LOAD_GLOBAL
    uint32_t api = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_3) {
        fprintf(stderr, "[VK] the Vulkan loader is %u.%u: 1.3 is required\n", VK_API_VERSION_MAJOR(api),
                VK_API_VERSION_MINOR(api));
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Black Ops II";
    app.pEngineName = "bo2-apk";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)instance_exts.size();
    ici.ppEnabledExtensionNames = instance_exts.data();
    if (vkCreateInstance(&ici, nullptr, &g_dev.instance) != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateInstance failed\n");
        return false;
    }
#define BO2VK_LOAD_INSTANCE(f) f = (PFN_##f)vkGetInstanceProcAddr(g_dev.instance, #f);
    BO2VK_INSTANCE_FNS(BO2VK_LOAD_INSTANCE)
#undef BO2VK_LOAD_INSTANCE

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_dev.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(g_dev.instance, &n, phys.data());
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(phys[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) { best = (int)i; best_score = score; }
    }
    if (best < 0) { fprintf(stderr, "[VK] no Vulkan 1.3 GPU\n"); return false; }
    g_dev.phys = phys[best];
    vkGetPhysicalDeviceProperties(g_dev.phys, &g_dev.props);
    vkGetPhysicalDeviceMemoryProperties(g_dev.phys, &g_dev.memprops);
    g_dev.gpu = g_dev.props.deviceName;

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_dev.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(g_dev.phys, &nq, qf.data());
    g_dev.queue_family = ~0u;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_dev.queue_family = i; break; }
    if (g_dev.queue_family == ~0u) return false;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f12.pNext = &f13;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(g_dev.phys, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) {
        fprintf(stderr, "[VK] %s lacks dynamicRendering or synchronization2\n", g_dev.gpu.c_str());
        return false;
    }
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.pNext = &e13;
    e2.features.textureCompressionBC = f2.features.textureCompressionBC;
    e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    e2.features.depthClamp = f2.features.depthClamp;
    e2.features.dualSrcBlend = f2.features.dualSrcBlend;
    e2.features.depthBiasClamp = f2.features.depthBiasClamp;
    // BO2_VK_ROBUST=1: out-of-range buffer reads in shaders return zeros (robustBufferAccess). The PC harness's
    // lavapipe segfaulted in a JIT-compiled shader reading past a buffer during the first frames of a zombies level;
    // real GPUs usually return garbage there. Off by default (it can cost a little on mobile GPUs).
    if (const char* e = getenv("BO2_VK_ROBUST"); e && atoi(e) && f2.features.robustBufferAccess) {
        e2.features.robustBufferAccess = VK_TRUE;
        fprintf(stderr, "[VK] robustBufferAccess on (BO2_VK_ROBUST)\n");
    }
    g_dev.bc = f2.features.textureCompressionBC;
    if (getenv("BO2_VK_NO_BC")) g_dev.bc = false;   // lever: decode BC textures on the CPU even where the GPU has BC
    g_dev.anisotropy = f2.features.samplerAnisotropy;
    g_dev.depth_clamp = f2.features.depthClamp;
    g_dev.max_aniso = g_dev.props.limits.maxSamplerAnisotropy;

    std::vector<VkExtensionProperties> dext;
    vkEnumerateDeviceExtensionProperties(g_dev.phys, nullptr, &n, nullptr);
    dext.resize(n);
    vkEnumerateDeviceExtensionProperties(g_dev.phys, nullptr, &n, dext.data());
    log_draw_caps(dext);
    std::vector<const char*> exts;
    if (want_surface) {
        if (!has_ext(dext, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return false;
        exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        // When each presented frame actually reached the screen: input latency measured and paced (vk_frame.cpp).
        if (has_ext(dext, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) {
            exts.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
            g_dev.display_timing = true;
        }
        fprintf(stderr, "[VK] display timing (VK_GOOGLE_display_timing): %s\n",
                g_dev.display_timing ? "yes: [LATENCY] and frame pacing" : "no: frames are not paced to the screen");
    }
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_dev.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &e2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)exts.size();
    dci.ppEnabledExtensionNames = exts.data();
    if (vkCreateDevice(g_dev.phys, &dci, nullptr, &g_dev.dev) != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateDevice failed\n");
        return false;
    }
#define BO2VK_LOAD_DEVICE(f) f = (PFN_##f)vkGetDeviceProcAddr(g_dev.dev, #f);
    BO2VK_DEVICE_FNS(BO2VK_LOAD_DEVICE)
#undef BO2VK_LOAD_DEVICE
    vkGetDeviceQueue(g_dev.dev, g_dev.queue_family, 0, &g_dev.queue);

    // The combined depth-stencil format (Vulkan cannot attach a separate stencil image to a pass).
    const VkFormatFeatureFlags ds = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (format_has(VK_FORMAT_D32_SFLOAT_S8_UINT, ds)) { g_dev.depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT; g_dev.depth_has_stencil = true; }
    else if (format_has(VK_FORMAT_D24_UNORM_S8_UINT, ds)) { g_dev.depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT; g_dev.depth_has_stencil = true; }
    else { g_dev.depth_fmt = VK_FORMAT_D32_SFLOAT; g_dev.depth_has_stencil = false; }
    g_dev.bgra_vertex = format_has(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, true);

    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    std::vector<uint8_t> blob;
    if (const char* dir = getenv("BO2_SHADER_CACHE_DIR"); dir && *dir) {
        if (FILE* fp = fopen((std::string(dir) + "/vk_pipeline_cache.bin").c_str(), "rb")) {
            fseek(fp, 0, SEEK_END);
            const long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz > 0) { blob.resize((size_t)sz); if (fread(blob.data(), 1, blob.size(), fp) != blob.size()) blob.clear(); }
            fclose(fp);
        }
    }
    pci.initialDataSize = blob.size();
    pci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(g_dev.dev, &pci, nullptr, &g_dev.pipeline_cache) != VK_SUCCESS) {
        pci.initialDataSize = 0; pci.pInitialData = nullptr;   // a stale cache from another driver is rejected
        vkCreatePipelineCache(g_dev.dev, &pci, nullptr, &g_dev.pipeline_cache);
    }

    for (Frame& f : g_dev.frames) {
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = g_dev.queue_family;
        cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(g_dev.dev, &cpi, nullptr, &f.pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g_dev.dev, &ai, &f.cb);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(g_dev.dev, &fci, nullptr, &f.done);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(g_dev.dev, &sci, nullptr, &f.acquired);
        vkCreateSemaphore(g_dev.dev, &sci, nullptr, &f.rendered);
    }
    g_dev.ok = true;
    fprintf(stderr, "[VK] %s, Vulkan %u.%u.%u, driver 0x%x; BC %s, depth %s, anisotropy %s (max %.0f), depth clamp %s, "
                    "BGRA vertices %s\n",
            g_dev.gpu.c_str(), VK_API_VERSION_MAJOR(g_dev.props.apiVersion), VK_API_VERSION_MINOR(g_dev.props.apiVersion),
            VK_API_VERSION_PATCH(g_dev.props.apiVersion), g_dev.props.driverVersion, g_dev.bc ? "native" : "decoded on the CPU",
            g_dev.depth_fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8" : g_dev.depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32 (no stencil)",
            g_dev.anisotropy ? "yes" : "no", g_dev.max_aniso, g_dev.depth_clamp ? "yes" : "no",
            g_dev.bgra_vertex ? "yes" : "no (read as RGBA: red and blue swap)");
    return true;
}

namespace {
std::atomic<uint64_t> g_pc_new{0};          // pipelines created since start
std::atomic<uint64_t> g_pc_saved_at{0};     // value of g_pc_new at the last save
std::atomic<int64_t> g_pc_last_new_ms{0};
std::atomic<bool> g_pc_saving{false};
int64_t pc_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Write the cache (VkPipelineCache is internally synchronized, so this may run beside pipeline creation) to a
// temporary file and rename it over the old one: a kill mid-write leaves the previous cache whole.
void pc_save_now(uint64_t upto) {
    const char* dir = getenv("BO2_SHADER_CACHE_DIR");
    if (!dir || !*dir || !g_dev.pipeline_cache) return;
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_dev.dev, g_dev.pipeline_cache, &sz, nullptr) != VK_SUCCESS || !sz) return;
    std::vector<uint8_t> blob(sz);
    if (vkGetPipelineCacheData(g_dev.dev, g_dev.pipeline_cache, &sz, blob.data()) != VK_SUCCESS) return;
    const std::string path = std::string(dir) + "/vk_pipeline_cache.bin", tmp = path + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "wb");
    if (!fp) return;
    const bool ok = fwrite(blob.data(), 1, sz, fp) == sz;
    fclose(fp);
    if (ok && rename(tmp.c_str(), path.c_str()) == 0) {
        g_pc_saved_at.store(upto);
        fprintf(stderr, "[VK] pipeline cache saved: %zu KB, %llu pipelines this session\n", sz / 1024,
                (unsigned long long)upto);
    } else {
        remove(tmp.c_str());
    }
}
}  // namespace

void pipeline_cache_note_new() {
    g_pc_new.fetch_add(1);
    g_pc_last_new_ms.store(pc_now_ms());
}

void pipeline_cache_tick(bool leaving_foreground) {
    const uint64_t n = g_pc_new.load();
    if (n == g_pc_saved_at.load() || !g_dev.ok) return;
    const int64_t quiet = pc_now_ms() - g_pc_last_new_ms.load();
    if (!leaving_foreground && quiet < 3000) return;   // still compiling a burst: wait for it to end
    if (g_pc_saving.exchange(true)) return;
    std::thread([n] {
        pc_save_now(n);
        g_pc_saving.store(false);
    }).detach();
}

void device_shutdown() {
    if (!g_dev.ok) return;
    vkDeviceWaitIdle(g_dev.dev);
    while (g_pc_saving.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (g_pc_new.load() != g_pc_saved_at.load()) pc_save_now(g_pc_new.load());
    for (Frame& f : g_dev.frames) {
        for (auto& fn : f.deferred) fn();
        f.deferred.clear();
        for (Buffer& b : f.ring_retired) buffer_destroy(b);
        buffer_destroy(f.ring);
        for (VkDescriptorPool p : f.pools) vkDestroyDescriptorPool(g_dev.dev, p, nullptr);
        f.pools.clear();
        vkDestroySemaphore(g_dev.dev, f.acquired, nullptr);
        vkDestroySemaphore(g_dev.dev, f.rendered, nullptr);
        vkDestroyFence(g_dev.dev, f.done, nullptr);
        vkDestroyCommandPool(g_dev.dev, f.pool, nullptr);
        f = Frame{};
    }
    for (VkDescriptorPool p : g_persist_pools) vkDestroyDescriptorPool(g_dev.dev, p, nullptr);
    g_persist_pools.clear();
    g_persist_cur = 0;
    descriptor_epoch_bump();
    if (g_dev.pipeline_cache) vkDestroyPipelineCache(g_dev.dev, g_dev.pipeline_cache, nullptr);
    {
        std::lock_guard<std::mutex> lk(g_mem_mu);
        for (Block& b : g_blocks) if (b.mem) vkFreeMemory(g_dev.dev, b.mem, nullptr);
        g_blocks.clear();
    }
    vkDestroyDevice(g_dev.dev, nullptr);
    vkDestroyInstance(g_dev.instance, nullptr);
    g_dev = Device{};
}

// ---- Formats ----------------------------------------------------------------------------------------------------
bool dxgi_is_bc(uint32_t f) { return f >= 70 && f <= 84; }

uint32_t dxgi_bytes_per_block(uint32_t f) {
    switch (f) {
        case 70: case 71: case 72: case 79: case 80: case 81: return 8;          // BC1, BC4
        case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83: case 84: return 16;   // BC2, BC3, BC5
        case 2: case 3: case 4: return 16;
        case 6: case 7: case 8: return 12;
        case 9: case 10: case 11: case 12: case 13: case 14: case 16: case 17: case 18: return 8;
        case 24: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 34: case 35: case 36: case 37:
        case 38: case 39: case 40: case 41: case 42: case 43: case 44: case 45: case 87: case 88: case 90: case 91:
            return 4;
        case 48: case 49: case 54: case 56: case 57: return 2;
        case 60: case 61: case 65: return 1;
        default: return 0;
    }
}

VkFormat dxgi_to_vk(uint32_t f, VkComponentMapping* swz, bool rt) {
    if (swz) *swz = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY};
    switch (f) {
        case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 9: case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 11: return VK_FORMAT_R16G16B16A16_UNORM;
        case 13: return VK_FORMAT_R16G16B16A16_SNORM;
        case 16: return VK_FORMAT_R32G32_SFLOAT;
        case 24: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;      // DXGI R10G10B10A2: R in the low bits
        case 26: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;       // DXGI R11G11B10_FLOAT
        case 27: case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 29: return VK_FORMAT_R8G8B8A8_SRGB;
        case 31: return VK_FORMAT_R8G8B8A8_SNORM;
        case 34: return VK_FORMAT_R16G16_SFLOAT;
        case 35: return VK_FORMAT_R16G16_UNORM;
        case 41: return VK_FORMAT_R32_SFLOAT;
        case 49: return VK_FORMAT_R8G8_UNORM;
        case 54: return VK_FORMAT_R16_SFLOAT;
        case 56: return VK_FORMAT_R16_UNORM;
        case 61: return VK_FORMAT_R8_UNORM;
        case 65:   // A8_UNORM: R8 seen as (0, 0, 0, R)
            if (swz) *swz = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                             VK_COMPONENT_SWIZZLE_R};
            return rt ? VK_FORMAT_UNDEFINED : VK_FORMAT_R8_UNORM;
        case 70: case 71: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case 72: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case 73: case 74: return VK_FORMAT_BC2_UNORM_BLOCK;
        case 75: return VK_FORMAT_BC2_SRGB_BLOCK;
        case 76: case 77: return VK_FORMAT_BC3_UNORM_BLOCK;
        case 78: return VK_FORMAT_BC3_SRGB_BLOCK;
        case 79: case 80: return VK_FORMAT_BC4_UNORM_BLOCK;
        case 81: return VK_FORMAT_BC4_SNORM_BLOCK;
        case 82: case 83: return VK_FORMAT_BC5_UNORM_BLOCK;
        case 84: return VK_FORMAT_BC5_SNORM_BLOCK;
        case 87: case 90: return VK_FORMAT_B8G8R8A8_UNORM;
        case 88: case 91: return VK_FORMAT_B8G8R8A8_SRGB;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat dxgi_to_vertex_format(uint32_t f) {
    switch (f) {
        case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 3: return VK_FORMAT_R32G32B32A32_UINT;
        case 4: return VK_FORMAT_R32G32B32A32_SINT;
        case 6: return VK_FORMAT_R32G32B32_SFLOAT;
        case 7: return VK_FORMAT_R32G32B32_UINT;
        case 8: return VK_FORMAT_R32G32B32_SINT;
        case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 11: return VK_FORMAT_R16G16B16A16_UNORM;
        case 12: return VK_FORMAT_R16G16B16A16_UINT;
        case 13: return VK_FORMAT_R16G16B16A16_SNORM;
        case 14: return VK_FORMAT_R16G16B16A16_SINT;
        case 16: return VK_FORMAT_R32G32_SFLOAT;
        case 17: return VK_FORMAT_R32G32_UINT;
        case 18: return VK_FORMAT_R32G32_SINT;
        case 24: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 30: return VK_FORMAT_R8G8B8A8_UINT;
        case 31: return VK_FORMAT_R8G8B8A8_SNORM;
        case 32: return VK_FORMAT_R8G8B8A8_SINT;
        case 34: return VK_FORMAT_R16G16_SFLOAT;
        case 35: return VK_FORMAT_R16G16_UNORM;
        case 36: return VK_FORMAT_R16G16_UINT;
        case 37: return VK_FORMAT_R16G16_SNORM;
        case 38: return VK_FORMAT_R16G16_SINT;
        case 41: return VK_FORMAT_R32_SFLOAT;
        case 42: return VK_FORMAT_R32_UINT;
        case 43: return VK_FORMAT_R32_SINT;
        // D3D reads B8G8R8A8 as (R, G, B, A) from bytes (2, 1, 0, 3): the shader sees red in .x.
        case 87: return g_dev.bgra_vertex ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

}  // namespace bo2vk
