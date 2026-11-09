// core_loader.cpp — see core_loader.hpp.
#include "core_loader.hpp"

#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdio>
#include <mutex>

#include "gfx_abi.h"

namespace bo2core {

namespace {
Api g_api;
bool g_tried = false, g_ok = false;
std::string g_why;
std::mutex g_mu;

template <typename F>
bool sym(void* h, const char* name, F& out, std::string* missing) {
    out = reinterpret_cast<F>(dlsym(h, name));
    if (!out) *missing += std::string(missing->empty() ? "" : ", ") + name;
    return out != nullptr;
}
}  // namespace

const Api& api() { return g_api; }

bool load(const char* path, std::string* why) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_tried) {
        g_tried = true;
        struct stat st {};
        if (!path || !*path || ::stat(path, &st) != 0) {
            g_why = std::string("no game core at ") + (path && *path ? path : "(BO2_CORE unset)");
        } else if (void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
            Api a;
            std::string missing;
            bool all = true;
            all &= sym(h, "bo2_game_main", a.game_main, &missing);
            all &= sym(h, "bo2_cmd_queue", a.cmd_queue, &missing);
            all &= sym(h, "bo2_dvar_unlock_lod", a.dvar_unlock, &missing);
            all &= sym(h, "bo2_map_cull_distance", a.map_cull_distance, &missing);
            all &= sym(h, "bo2_hw_atomic_report", a.hw_atomic_report, &missing);
            all &= sym(h, "bo2core_draw_record_size", a.draw_record_size, &missing);
            all &= sym(h, "bo2core_depth_clear_value", a.depth_clear_value, &missing);
            a.aim_state = reinterpret_cast<int (*)()>(dlsym(h, "bo2_aim_state"));   // optional: older cores lack it
            if (all) {
                g_api = a;
                g_ok = true;
                std::fprintf(stderr, "[CORE] game core loaded: %s (%lld bytes)\n", path, (long long)st.st_size);
            } else {
                g_why = "the game core at " + std::string(path) + " lacks " + missing +
                        " (built for another version of the app: rebuild it with scripts/package-game.sh)";
                dlclose(h);
            }
        } else {
            const char* e = dlerror();
            g_why = std::string("the game core did not load: ") + (e ? e : "?");
        }
        if (!g_ok) std::fprintf(stderr, "[CORE] %s\n", g_why.c_str());
    }
    if (why) *why = g_why;
    return g_ok;
}

}  // namespace bo2core

// The renderer asks the producer for these two (vk_capture.cpp, vk_render.cpp); the producer is in the core.
extern "C" uint32_t gfx_draw_record_size(void) {
    const auto f = bo2core::api().draw_record_size;
    return f ? f() : (uint32_t)sizeof(bo2vk::GfxDraw);
}
extern "C" float gfx_depth_clear_value() {
    const auto f = bo2core::api().depth_clear_value;
    return f ? f() : 1.0f;
}
