// graphics_apply.cpp — the graphics settings to the game, through bo2-mac's command pipe.
//
// bo2_graphics_apply() is for the runtime to call once the game has executed its own config files (the
// same moment pad_gc.mm/pad_sdl.cpp re-enable gpad_enabled, bo2_pad_top), and again when the player
// changes a setting. Until the game core is linked, bo2_cmd_queue is absent and the commands are only
// logged, prefixed "[GFX] would send", so the result can be read on the phone before anything runs.
#include <dirent.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "graphics.hpp"

#include "core_loader.hpp"

// The game core's entry points (core_loader.cpp): null until it loads, as the weak symbols they replace were.
#define bo2_cmd_queue (bo2core::api().cmd_queue)
#define bo2_map_cull_distance (bo2core::api().map_cull_distance)
#define bo2_dvar_unlock_lod (bo2core::api().dvar_unlock)
extern "C" void bo2vk_set_fx_hidden(int on) __attribute__((weak));    // runtime/gfx/vk: particle-like draws dropped

namespace {
// Set by the first bo2_graphics_apply: the game has run its config, its console takes commands.
std::atomic<bool> g_applied{false};
}  // namespace

namespace bo2gfx {

// Every players/*.cfg under the imported game folder, in name order (later files win): the game's own
// archived values, the baseline the settings are checked and restored against.
std::map<std::string, std::string> baseline_from(const std::string& game_dir) {
    std::map<std::string, std::string> merged;
    const std::string dir = game_dir + "/players";
    DIR* d = opendir(dir.c_str());
    if (!d) return merged;
    std::map<std::string, std::string> files;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".cfg") == 0) files[name] = dir + "/" + name;
    }
    closedir(d);
    for (const auto& [name, path] : files) {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        for (auto& [k, v] : parse_config(text.str())) merged[k] = v;
        std::fprintf(stderr, "[GFX] baseline from players/%s\n", name.c_str());
    }
    return merged;
}

}  // namespace bo2gfx

extern "C" void bo2_graphics_apply(const char* game_dir) {
    // Which dvars exist: their names in the game binary (scanned once), and the player's config files, which also
    // give values (the current Steam build writes none: docs/GRAPHICS.md).
    static const std::map<std::string, std::string> in_image = [] {
        const char* pe = getenv("BO2_PE_SECTIONS");
        auto m = bo2gfx::dvars_in_image(pe ? pe : "");
        std::fprintf(stderr, "[GFX] %zu of %zu graphics dvars found in the game binary\n", m.size(),
                     bo2gfx::known_dvars().size());
        return m;
    }();
    std::map<std::string, std::string> baseline = in_image;
    for (auto& [k, v] : bo2gfx::baseline_from(game_dir ? game_dir : ".")) baseline[k] = v;
    const bo2gfx::Commands c = bo2gfx::commands(bo2gfx::settings(), baseline);
    if (baseline.empty())
        std::fprintf(stderr, "[GFX] no dvar could be checked (no game binary, no players/*.cfg in %s): nothing is "
                             "sent\n", game_dir ? game_dir : ".");
    for (const std::string& name : c.skipped)
        std::fprintf(stderr, "[GFX] not sent: %s is not in the game binary (it does not exist in this build)\n",
                     name.c_str());
    for (const std::string& line : c.send) {
        if (bo2_cmd_queue) {
            bo2_cmd_queue(line.c_str());
        } else {
            std::fprintf(stderr, "[GFX] would send: %s\n", line.c_str());
        }
    }
    g_applied.store(true);
}

// The view distance (graphics.hpp, view_distance), kept in step with the map. r_zfar replaces the map's own culling
// distance instead of capping it, and that distance changes with the map (and with its scripted fog), so every 2 s
// the map's own is read and r_zfar set to the setting only while the setting is nearer (cull_distance), 0 otherwise.
// Called on each presented frame (game_host.cpp); a clock comparison unless 2 s have passed. BO2_CULL_FOG=0: never
// touches r_zfar (puts 0 back once if it had set it).
extern "C" void bo2_view_distance_tick(int64_t now_ms) {
    static int64_t next_ms = 0;
    static int sent = -1;          // the r_zfar this sent last; -1 nothing yet (the game's default is 0)
    static float last_map = -2.f;  // for the log
    if (now_ms < next_ms || !g_applied.load() || !bo2_cmd_queue || !bo2_map_cull_distance) return;
    next_ms = now_ms + 2000;
    static const bool off = [] {
        const char* e = getenv("BO2_CULL_FOG");
        return e && std::atoi(e) == 0;
    }();
    const float map = bo2_map_cull_distance();
    const int want = off ? 0 : static_cast<int>(bo2gfx::cull_distance(bo2gfx::settings().view_distance, map));
    if (std::fabs(map - last_map) > 50.f) {   // scripted fog can glide: a line per real change
        std::fprintf(stderr, "[CULL] the map's own view distance %.0f, the setting %d: r_zfar %d%s\n", map,
                     bo2gfx::settings().view_distance, want, off ? " (BO2_CULL_FOG=0)" : "");
        last_map = map;
    }
    if (want == sent || (sent < 0 && want == 0)) return;
    if (bo2_dvar_unlock_lod) bo2_dvar_unlock_lod();   // cheat-protected: lifted again in case the game set it back
    const std::string line = "set r_zfar " + std::to_string(want);
    bo2_cmd_queue(line.c_str());
    std::fprintf(stderr, "[CULL] r_zfar %d (the map's own %.0f, the setting %d)\n", want, map,
                 bo2gfx::settings().view_distance);
    sent = want;
}

// BO2_FX_AB=<seconds> (remote env, for one measurement session; docs/GRAPHICS.md, "Effects"): the effects cycle
// through three states, each held that long, so one session compares them under the same play and heat:
//   drawn  — as usual;
//   frozen — fx_freeze 1: the game stops updating its effects (what their simulation costs the CPU);
//   hidden — the particle-like draws dropped in the backend (what drawing them costs our layer and the GPU).
// [FX-AB] marks each change; scripts/phone/fx-ab.sh splits [PERF], [THREADS], [SYSINFO] and [FX-DRAWS] by state.
// Unset or 0: nothing (the default).
extern "C" void bo2_fx_ab_tick(int64_t now_ms) {
    static const int64_t period_ms = [] {
        const char* e = getenv("BO2_FX_AB");
        const int s = e ? std::atoi(e) : 0;
        return s > 0 ? static_cast<int64_t>(s < 10 ? 10 : s) * 1000 : 0;
    }();
    static int64_t next_ms = 0;
    static int state = -1;
    if (!period_ms || now_ms < next_ms || !g_applied.load() || !bo2_cmd_queue) return;
    next_ms = now_ms + period_ms;
    state = (state + 1) % 3;
    if (bo2_dvar_unlock_lod) bo2_dvar_unlock_lod();   // fx_freeze is cheat-protected
    bo2_cmd_queue(state == 1 ? "set fx_freeze 1" : "set fx_freeze 0");
    if (bo2vk_set_fx_hidden) bo2vk_set_fx_hidden(state == 2 ? 1 : 0);
    static const char* const kStates[] = {"drawn", "frozen", "hidden"};
    std::fprintf(stderr, "[FX-AB] effects %s for %lld s\n", kStates[state], (long long)(period_ms / 1000));
}
