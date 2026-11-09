// vk_window.cpp — the window side of the Vulkan backend: gfx_window_open / gfx_shutdown, the swapchain, the Win32
// input queue, focus, and the pointer the runtime's user32 asks about.
//
// On Android the shell owns the window, the events and the touch controls (vk_hooks.h); this file turns the window
// into a Vulkan surface and keeps the game's input queue. Without hooks (the PC tests) there is no window: frames are
// rendered into the back buffer and kept.
#include "vk_render.h"

#include "vk_hooks.h"

#include <chrono>
#include <algorithm>
#include <deque>

#include "guest_monitor.h"

#if defined(__ANDROID__)
#include <vulkan/vulkan_android.h>
#endif

namespace bo2vk {

bool frame_init();
void frame_shutdown();

Swapchain g_swap;
VkSurfaceTransformFlagBitsKHR g_swap_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
std::atomic<bool> g_foreground{false};

namespace {
bool g_have_hooks = false;
ShellHooks g_hooks{};

std::mutex g_in_mu;
struct Msg { uint32_t msg, wp, lp; };
std::deque<Msg> g_in;

// The pointer the game sees: there is no system cursor on a phone, so it is where the last mouse message put it
// (a Bluetooth mouse), and a warp moves it there.
std::atomic<double> g_cursor_x{0}, g_cursor_y{0}, g_pinned_t{0};

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

const ShellHooks* hooks() { return g_have_hooks ? &g_hooks : nullptr; }

void push_focus(bool key) {
    // As macOS's backend: focus changes are WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS / WM_KILLFOCUS. The game ignores
    // input without them.
    fprintf(stderr, "[FOCUS] window %s foreground\n", key ? "GAINED" : "LOST");
    bo2vk_input_push(0x001C, key ? 1 : 0, 0);
    bo2vk_input_push(0x0006, key ? 1 : 0, 0);
    bo2vk_input_push(key ? 0x0007 : 0x0008, 0, 0);
}

bool swapchain_build() {
    if (!g_swap.surface) return false;
    vkDeviceWaitIdle(g_dev.dev);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_dev.phys, g_swap.surface, &caps) != VK_SUCCESS) return false;
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) { g_swap.needs_rebuild = true; return false; }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_dev.phys, g_swap.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_dev.phys, g_swap.surface, &n, fmts.data());
    // UNORM: the game does its own gamma, and the back buffer is UNORM.
    VkSurfaceFormatKHR pick = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (const auto& f : fmts)
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = f; break; }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_swap.surface;
    // Android's swapchain allocates (minImageCount - 1) + the compositor's undequeued buffers (usually 2): asking for 3
    // gave 4 images on the S26, and the game, CPU-bound at the display rate, keeps them all queued, so each extra
    // image is a frame of input latency. The reported minimum gives triple buffering there. BO2_VK_SWAP_MIN overrides.
    {
        const char* e = getenv("BO2_VK_SWAP_MIN");
        const uint32_t want = e && atoi(e) > 0 ? (uint32_t)atoi(e) : std::max(caps.minImageCount, 2u);
        ci.minImageCount = std::max(want, caps.minImageCount);
    }
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    fprintf(stderr, "[VK] swapchain request: %u images (surface min %u, max %u)\n", ci.minImageCount, caps.minImageCount,
            caps.maxImageCount);
    ci.imageFormat = pick.format;
    ci.imageColorSpace = pick.colorSpace;
    // Android reports currentExtent as the window is seen now (landscape, 2340x1080 on the S26) while currentTransform
    // says the panel is natively portrait (ROTATE_90). A pre-rotated swapchain is made in the panel's native
    // orientation, so the extent is swapped (Android's pre-rotation guide); vk_frame.cpp then rotates into it. Kept
    // as reported, the compositor rotated a landscape image as if it were portrait: the game squeezed into the top
    // 46% (= 1080/2340) of the screen on the first run on the S26.
    VkExtent2D extent = caps.currentExtent;
    if (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
        caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        std::swap(extent.width, extent.height);
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;   // pre-rotation: vk_frame.cpp's presentation rotates
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                          : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // v-sync, always available
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swap.chain;
    VkSwapchainKHR chain = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(g_dev.dev, &ci, nullptr, &chain);
    if (g_swap.chain) vkDestroySwapchainKHR(g_dev.dev, g_swap.chain, nullptr);
    for (VkSemaphore s : g_swap.rendered) vkDestroySemaphore(g_dev.dev, s, nullptr);
    g_swap.rendered.clear();
    g_swap.chain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { fprintf(stderr, "[VK] vkCreateSwapchainKHR failed (%d)\n", (int)r); return false; }
    g_swap.chain = chain;
    g_swap.fmt = pick.format;
    g_swap.extent = extent;
    g_swap_transform = caps.currentTransform;
    vkGetSwapchainImagesKHR(g_dev.dev, chain, &n, nullptr);
    g_swap.images.resize(n);
    vkGetSwapchainImagesKHR(g_dev.dev, chain, &n, g_swap.images.data());
    g_swap.layouts.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);
    for (uint32_t i = 0; i < n; i++) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        vkCreateSemaphore(g_dev.dev, &sci, nullptr, &s);
        g_swap.rendered.push_back(s);
    }
    g_swap.needs_rebuild = false;
    fprintf(stderr, "[VK] swapchain %ux%u, %u images, format %d, transform 0x%x\n", g_swap.extent.width,
            g_swap.extent.height, n, (int)g_swap.fmt, (unsigned)g_swap_transform);
    return true;
}

void swapchain_destroy() {
    if (!g_dev.ok) return;
    vkDeviceWaitIdle(g_dev.dev);
    for (VkSemaphore s : g_swap.rendered) vkDestroySemaphore(g_dev.dev, s, nullptr);
    if (g_swap.chain) vkDestroySwapchainKHR(g_dev.dev, g_swap.chain, nullptr);
    if (g_swap.surface) vkDestroySurfaceKHR(g_dev.instance, g_swap.surface, nullptr);
    g_swap = Swapchain{};
}

}  // namespace bo2vk

using namespace bo2vk;

extern "C" void bo2vk_set_hooks(const struct Bo2VkShellHooks* h) {
    if (h) { static_cast<Bo2VkShellHooks&>(g_hooks) = *h; g_have_hooks = true; }
    else g_have_hooks = false;
}

extern "C" void bo2vk_input_push(uint32_t msg, uint32_t wp, uint32_t lp) {
    if (msg == 0x0200 /* WM_MOUSEMOVE */) {
        g_cursor_x.store((double)(int16_t)(lp & 0xFFFF));
        g_cursor_y.store((double)(int16_t)(lp >> 16));
    }
    std::lock_guard<std::mutex> lk(g_in_mu);
    if (g_in.size() > 4096) g_in.pop_front();   // a stalled game must not grow it without bound
    g_in.push_back({msg, wp, lp});
}

namespace bo2vk {
std::atomic<bool> g_surface_changed{false};

// A new surface for the window the shell has now (after a return from the background), and its swapchain.
void surface_rebuild() {
#if defined(__ANDROID__)
    g_surface_changed.store(false);
    const ShellHooks* h = hooks();
    void* native = h && h->native_window ? h->native_window() : nullptr;
    vkDeviceWaitIdle(g_dev.dev);
    for (VkSemaphore s : g_swap.rendered) vkDestroySemaphore(g_dev.dev, s, nullptr);
    g_swap.rendered.clear();
    if (g_swap.chain) vkDestroySwapchainKHR(g_dev.dev, g_swap.chain, nullptr);
    if (g_swap.surface) vkDestroySurfaceKHR(g_dev.instance, g_swap.surface, nullptr);
    g_swap.chain = VK_NULL_HANDLE;
    g_swap.surface = VK_NULL_HANDLE;
    if (!native) { g_swap.needs_rebuild = false; return; }   // no window now: frames are rendered and kept
    auto create = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(g_dev.instance, "vkCreateAndroidSurfaceKHR");
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = (ANativeWindow*)native;
    if (!create || create(g_dev.instance, &si, nullptr, &g_swap.surface) != VK_SUCCESS) {
        fprintf(stderr, "[VK] the new window's surface could not be created\n");
        g_swap.surface = VK_NULL_HANDLE;
        return;
    }
    if (!swapchain_build()) g_swap.needs_rebuild = true;
    fprintf(stderr, "[VK] surface rebuilt for the new window\n");
#endif
}
}  // namespace bo2vk

extern "C" void bo2vk_surface_changed(void) { g_surface_changed.store(true); }

extern "C" int gfx_input_pop(uint32_t* msg, uint32_t* wp, uint32_t* lp) {
    std::lock_guard<std::mutex> lk(g_in_mu);
    if (g_in.empty()) return 0;
    const Msg m = g_in.front();
    g_in.pop_front();
    if (msg) *msg = m.msg;
    if (wp) *wp = m.wp;
    if (lp) *lp = m.lp;
    return 1;
}

extern "C" double gfx_refresh_hz() {
    const ShellHooks* h = hooks();
    const double hz = h && h->refresh_hz ? h->refresh_hz() : 60.0;
    return hz > 0 ? hz : 60.0;
}

extern "C" int gfx_window_is_really_key(void) { return g_foreground.load() ? 1 : 0; }

// The pointer is never held on a phone (no cursor to capture): user32's mouse-look recentring then never warps.
extern "C" int gfx_pointer_engaged(void) { return 0; }
extern "C" void gfx_pointer_pinned(void) { g_pinned_t.store(now_s()); }

extern "C" int gfx_window_centre(double* x, double* y) {
    int w = (int)kGuestMonitorW, h = (int)kGuestMonitorH;
    if (const ShellHooks* hk = hooks(); hk && hk->panel_size) hk->panel_size(&w, &h);
    if (x) *x = w / 2.0;
    if (y) *y = h / 2.0;
    return g_open.load() ? 1 : 0;
}

extern "C" void gfx_cursor_location(double* x, double* y) {
    if (x) *x = g_cursor_x.load();
    if (y) *y = g_cursor_y.load();
}

// Relative pointer motion (touch drag look): the game's mouse look reads the pointer every frame and puts it back in
// the middle, so moving the pointer by (dx, dy) turns the view by that many mouse counts, through the game's own
// mouse sensitivity, with no stick curve or acceleration.
extern "C" void bo2vk_mouse_rel(int dx, int dy) {
    if (!dx && !dy) return;
    double x = g_cursor_x.load(), y = g_cursor_y.load();
    while (!g_cursor_x.compare_exchange_weak(x, x + dx)) {}
    while (!g_cursor_y.compare_exchange_weak(y, y + dy)) {}
}

extern "C" int gfx_cursor_warp(double x, double y) {
    g_cursor_x.store(x);
    g_cursor_y.store(y);
    return 0;
}

extern "C" bool gfx_window_open(int width, int height) {
    if (g_open.load()) return true;
    const ShellHooks* h = hooks();
    void* native = h && h->native_window ? h->native_window() : nullptr;
    std::vector<const char*> iext;
#if defined(__ANDROID__)
    if (native) { iext.push_back(VK_KHR_SURFACE_EXTENSION_NAME); iext.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME); }
#else
    native = nullptr;   // the PC build is headless (tests)
#endif
    if (!device_init(native != nullptr, iext)) {
        fprintf(stderr, "[gfx] no Vulkan 1.3 device: the game cannot draw\n");
        return false;
    }
#if defined(__ANDROID__)
    if (native) {
        auto create = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(g_dev.instance, "vkCreateAndroidSurfaceKHR");
        VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        si.window = (ANativeWindow*)native;
        if (!create || create(g_dev.instance, &si, nullptr, &g_swap.surface) != VK_SUCCESS) {
            fprintf(stderr, "[gfx] vkCreateAndroidSurfaceKHR failed\n");
            return false;
        }
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_dev.phys, g_dev.queue_family, g_swap.surface, &ok);
        if (!ok) { fprintf(stderr, "[gfx] the graphics queue cannot present to this window\n"); return false; }
    }
#endif
    if (!resources_init() || !frame_init()) {
        fprintf(stderr, "[gfx] backend resources could not be created\n");
        return false;
    }
    if (g_swap.surface && !swapchain_build()) g_swap.needs_rebuild = true;
    g_open.store(true);
    const bool fg = !h || !h->foreground || h->foreground();
    g_foreground.store(fg);
    push_focus(fg);
    fprintf(stderr, "[gfx] Vulkan backend open on %s (asked for a %dx%d window; the game renders %ux%u)%s\n",
            g_dev.gpu.c_str(), width, height, kGuestMonitorW, kGuestMonitorH, native ? "" : ", headless");
    return true;
}

extern "C" void gfx_shutdown() {
    if (!g_open.exchange(false)) return;
    if (g_dev.ok) vkDeviceWaitIdle(g_dev.dev);
    frame_shutdown();
    resources_shutdown();
    swapchain_destroy();
    device_shutdown();
    fprintf(stderr, "[gfx] Vulkan backend closed after %llu presented frames\n", (unsigned long long)g_frames.load());
}
