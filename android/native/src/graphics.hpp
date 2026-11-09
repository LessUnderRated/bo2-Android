// graphics.hpp — graphics settings for a phone: what the game is told to draw, and how fast.
//
// Two kinds of knob:
//   - the port's own: render resolution and screen shape (display_plan.hpp), and the frame cap, which
//     the presentation enforces (and com_maxfps asks the game for);
//   - the game's own dvars, sent as console commands through bo2-mac's command pipe (bo2_cmd_queue,
//     which feeds the game's Cbuf_AddText; 80 characters a line).
//
// The dvar names come from a community list of Black Ops II dvars (docs/GRAPHICS.md has the source),
// not from the game. So every dvar is checked against the player's own config files (players/*.cfg,
// which the game writes with `seta <name> <value>` for its archived dvars) before it is sent:
//   - a dvar not in them is not sent, and is reported: it may not exist in this build;
//   - the value found there is the baseline. "Game's own" means: put the baseline back. A forced value
//     can be written into config.cfg by the game, so leaving a dvar alone would not undo it.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace bo2gfx {

enum class Preset : int { Low = 0, Medium = 1, High = 2, Own = 3 };  // Own: the game's own settings, untouched

// Each knob; -1 means "the game's own" (the baseline from the player's config).
struct Settings {
    Preset preset = Preset::Medium;
    int frame_cap = 60;    // 30, 40, 45, 60, 90, 120; 0 = uncapped (the panel's 120 Hz still limits it)
    int shadows = -1;      // 0 off, 1 quarter-resolution sun shadows, 2 full
    int ssao = -1;         // 0 off (only "off" is forced; on is the player's own mode)
    int dof = -1;          // 0 off, 1 on
    int fxaa = -1;         // 0 off, 1 on
    int msaa = -1;         // r_aaSamples: 1 (off), 2, 4
    int textures = -1;     // 0 high, 1 medium, 2 low (picmip levels)
    int anisotropy = -1;   // max anisotropy: 1, 2, 4, 8, 16
    int decals = -1;       // 0 off, 1 on
    int fov = -1;          // cg_fov in degrees, 65..90
    // The port's own, not a dvar: how the presentation scales a lower render resolution up to the screen
    // (runtime/gfx/vk/vk_frame.cpp, bo2vk_set_upscaler). 0 bilinear, 1 SGSR (Snapdragon Game Super Resolution 1).
    // Every preset uses SGSR; at native resolution nothing is scaled either way.
    int upscaler = 1;
    // View distance, in game units (inches): the game's culling fog turns opaque there and nothing past it is drawn,
    // or even walked by the game's visibility pass (r_zfar). -1: the map's own distance (most maps have one from their
    // fog). Never further than the map's own: see cull_distance().
    int view_distance = -1;
    // Aim assist for a controller: the game's own system, which this Zombies build carries whole (aim_* dvars). Not a
    // cost, so presets keep it. -1 the game's own (left alone), 1 on: the aim slows over a target and catches it when
    // aiming down the sights, 2 strong: the aim also follows a moving target (lock-on). On and strong also clear
    // aim_assist_script_disable, which turns the slowdown and lock-on off when set. Kept so while the game runs
    // (bo2_aim_tick, graphics_apply.cpp).
    int aim_assist = -1;
};

// The aim assist switches as bo2_aim_state reports them (win32_runtime.cpp): 1 slowdown, 2 catch on aiming down the
// sights, 4 lock-on, 8 the scripts' disable switch. The state a setting asks for; -1 for the game's own (no demand).
int aim_wanted(int aim_assist);

// The r_zfar to ask of the game for a view distance (-1: the map's own) on a map whose own culling distance is
// `map_distance` (0: none; <0: unknown). r_zfar replaces the map's distance rather than capping it, so 0 (off, the
// map's own applies) whenever the map's own is already nearer than the setting, or cannot be read.
float cull_distance(int view_distance, float map_distance);

// The knobs a preset sets; Own leaves every knob at -1. frame_cap is kept from `current`.
Settings preset(Preset p, const Settings& current = Settings{});

// `seta name value` and `set name value` lines of a config file; later lines win.
std::map<std::string, std::string> parse_config(const std::string& text);

// Every dvar commands() may send.
const std::vector<std::string>& known_dvars();

// A baseline entry for a dvar whose name is in the game binary but whose value is not known: it may be sent,
// and "the game's own" leaves it alone.
constexpr const char* kExistsNoValue = "\x01exists";

// The dvars of known_dvars() whose names are in the game image (pe_sections.bin: the strings the game registers
// its dvars with), each as kExistsNoValue; empty if the file cannot be read.
std::map<std::string, std::string> dvars_in_image(const std::string& pe_sections);

struct Commands {
    std::vector<std::string> send;     // console commands, each at most 80 characters
    std::vector<std::string> skipped;  // dvars not in the player's config: not sent
};

// The commands that bring the game to `s`, given the baseline from the player's config files.
Commands commands(const Settings& s, const std::map<std::string, std::string>& baseline);

// Process-wide, for the menu (any thread) and the runtime at launch.
void set_settings(const Settings& s);
Settings settings();

}  // namespace bo2gfx
