// core_loader.hpp — the game core (libbo2core.so: the runtime and the game recompiled from the player's own exe),
// loaded at run time. The APK carries none of it: scripts/package-game.sh builds it on the player's machine, the app
// copies it from the game folder into its own storage (Bo2App, BO2_CORE), and load() opens it there. The core names
// libmain.so as a dependency, so its calls into the renderer, the pad bridge and the performance hints bind to this
// library when it is opened. Every entry point is null until a load succeeds.
#pragma once

#include <cstdint>
#include <string>

namespace bo2core {

struct Api {
    int (*game_main)(int argc, char** argv) = nullptr;
    void (*cmd_queue)(const char* cmd) = nullptr;        // a console command to the game (its command pipe)
    int (*dvar_unlock)() = nullptr;                      // lifts the cheat flag on the dvars the settings set
    float (*map_cull_distance)() = nullptr;              // the map's own culling distance; <0 unknown
    void (*hw_atomic_report)(const char* why) = nullptr; // the guest `lock` stripes' counters, to the log
    uint32_t (*draw_record_size)() = nullptr;            // the producer's sizeof(GfxDraw), for the renderer's check
    float (*depth_clear_value)() = nullptr;              // the value the engine clears depth with
    // Optional (a core built before it lacks it, and still loads): the aim assist switches as bits, -1 unreadable
    // (win32_runtime.cpp, bo2_aim_state).
    int (*aim_state)() = nullptr;
};

const Api& api();

// Opens the core at `path` once (later calls return the first result). False, with the reason in `why`, if the file
// is missing, does not load, or lacks an entry point; the app then runs without the game (the diagnostics screen).
bool load(const char* path, std::string* why);

}  // namespace bo2core
