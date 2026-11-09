// graphics.cpp — see graphics.hpp.
#include "graphics.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace bo2gfx {

namespace {

std::mutex g_mu;
Settings g_settings;

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

}  // namespace

Settings preset(Preset p, const Settings& current) {
    Settings s;
    s.preset = p;
    s.frame_cap = current.frame_cap;
    s.fov = current.fov;  // a matter of taste, not of cost: kept across presets
    s.aim_assist = current.aim_assist;   // a control, not a cost: kept too
    switch (p) {
        case Preset::Low:
            // Off: on the S26 the shadow passes cost the game's main core (a second culling of the world for the
            // sun, ~4% of it), the bottleneck, while the GPU idles. Low is for that core; the GPU-side knobs barely
            // matter on this phone.
            s.shadows = 0;
            s.ssao = 0;
            s.dof = 0;
            s.fxaa = 1;         // cheap edges, instead of MSAA
            s.msaa = 1;
            s.textures = 1;     // memory matters more than sharpness at 720p on a 6" screen
            s.anisotropy = 2;
            s.decals = 1;
            // The culling fog at ~125 m: on the big maps the far scenery is most of what the main core walks and
            // culls each frame, and the open ones show the most of it.
            s.view_distance = 5000;
            break;
        case Preset::Medium:
            s.shadows = 1;
            s.ssao = 0;
            s.dof = 1;
            s.fxaa = 1;
            s.msaa = 1;
            s.textures = 0;
            s.anisotropy = 4;
            s.decals = 1;
            s.view_distance = 8000;   // ~200 m
            break;
        case Preset::High:
            s.shadows = 2;
            s.dof = 1;
            s.fxaa = 1;
            s.msaa = 1;
            s.textures = 0;
            s.anisotropy = 8;
            s.decals = 1;
            break;
        case Preset::Own:
            break;
    }
    return s;
}

int aim_wanted(int aim_assist) {
    if (aim_assist <= 0) return -1;
    return 1 | 2 | (aim_assist >= 2 ? 4 : 0);   // and never 8: the scripts' disable switch cleared
}

float cull_distance(int view_distance, float map_distance) {
    if (view_distance <= 0 || map_distance < 0.f) return 0.f;
    if (map_distance > 0.f && map_distance <= static_cast<float>(view_distance)) return 0.f;
    return static_cast<float>(view_distance);
}

std::map<std::string, std::string> parse_config(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        std::string word;
        if (line.rfind("seta ", 0) == 0) word = "seta ";
        else if (line.rfind("set ", 0) == 0) word = "set ";
        else continue;
        std::string rest = trim(line.substr(word.size()));
        const size_t space = rest.find_first_of(" \t");
        if (space == std::string::npos) continue;
        std::string name = rest.substr(0, space), value = trim(rest.substr(space));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        out[name] = value;
    }
    return out;
}

Commands commands(const Settings& s, const std::map<std::string, std::string>& baseline) {
    Commands c;
    // One dvar: the forced value when the knob is set, else the player's own value back.
    auto dvar = [&](const char* name, int knob, const std::string& forced) {
        std::string key(name);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return std::tolower(ch); });
        const auto found = baseline.find(key);
        if (found == baseline.end()) {
            if (knob >= 0) c.skipped.push_back(name);
            return;
        }
        // Known to exist (in the game binary) but with no recorded value of the player's: "the game's own" can only
        // mean leaving it alone.
        if (knob < 0 && found->second == kExistsNoValue) return;
        const std::string value = knob >= 0 ? forced : found->second;
        std::string line = std::string("set ") + name + " ";
        line += value.find(' ') != std::string::npos ? "\"" + value + "\"" : value;
        if (line.size() <= 80) c.send.push_back(line);
    };
    auto num = [](int v) { return std::to_string(v); };

    // The game's own limiter a little above the cap: the frame queue (runtime/gfx/vk/vk_capture.cpp) paces the game
    // by the display, and a second 60 Hz clock in the game would only beat against it. But the margin is input
    // latency: a game faster than the display is held in the frame queue after it has read the pad ([QUEUE] in the
    // log), where its own limiter would have slept before reading it. BO2_MAXFPS_MARGIN (remote env, default 10)
    // trades the two; the game rounds its limit to whole milliseconds (60 + 2 is 16 ms, 62.5 fps).
    static const int margin = [] {
        const char* e = std::getenv("BO2_MAXFPS_MARGIN");
        const int m = e && *e ? std::atoi(e) : 10;
        return m < 0 ? 0 : m > 60 ? 60 : m;
    }();
    dvar("com_maxfps", s.frame_cap >= 0 ? 1 : -1, num(s.frame_cap > 0 ? s.frame_cap + margin : 0));
    dvar("sm_enable", s.shadows, s.shadows > 0 ? "1" : "0");
    dvar("sm_sunShadowSmall", s.shadows > 0 ? 1 : -1, s.shadows == 1 ? "1" : "0");
    if (s.ssao == 0) dvar("r_ssao", 0, "0");
    else dvar("r_ssao", -1, "");
    dvar("r_dof_enable", s.dof, num(s.dof));
    dvar("r_fxaa", s.fxaa, num(s.fxaa));
    dvar("r_aaSamples", s.msaa, num(std::max(1, s.msaa)));
    // Texture detail: picmip is read-only unless r_picmip_manual is 1.
    dvar("r_picmip_manual", s.textures >= 0 ? 1 : -1, "1");
    dvar("r_picmip", s.textures, num(s.textures));
    dvar("r_picmip_bump", s.textures, num(s.textures));
    dvar("r_picmip_spec", s.textures, num(s.textures));
    dvar("r_texFilterAnisoMax", s.anisotropy, num(s.anisotropy));
    // fx_marks_draw and cg_fov are cheat-protected (the console refuses them outside a devmap); these two are not.
    // Decals: blood and bullet marks only within this distance (the game's default is 1500).
    dvar("fx_marks_range", s.decals, s.decals == 0 ? "0" : s.preset == Preset::Low ? "500" : "800");
    dvar("cg_fov_default", s.fov, num(std::clamp(s.fov, 65, 90)));

    // The crowd: what a horde costs the game's main thread, the phone's bottleneck (the GPU idles at a low clock
    // while the main thread is pinned once zombies are about). A preset trims it; Own puts the game's defaults
    // back (several of these are archived, so a forced value would otherwise stick in config.cfg).
    const bool own = s.preset == Preset::Own, low = s.preset == Preset::Low;
    auto crowd = [&](const char* name, const char* trimmed, const char* trimmed_low, const char* game_default) {
        dvar(name, 1, own ? game_default : low ? trimmed_low : trimmed);
    };
    crowd("ai_corpseCount", "4", "2", "8");                              // dead zombies left as bodies
    crowd("ragdoll_max_life", "4000", "2500", "9000");                   // ms a ragdoll simulates
    crowd("dynEnt_spawnedLimit", "25", "15", "50");                      // loose physics debris
    crowd("phys_piecesSpawnDistanceCutoff", "600", "400", "1000");       // debris spawned only this close
    crowd("phys_impact_distance_cutoff", "500", "300", "800");           // impact effects only this close
    crowd("sm_maxLights", "2", "1", "4");                                // shadow-casting lights at once
    crowd("r_dlightLimit", "2", "1", "4");                               // dynamic lights (muzzle flashes, fx)
    // Theater recording: the server builds a demo snapshot of every solo match (about a tenth of the server thread
    // in a zm_transit profile). Nothing on a phone plays it back.
    crowd("demo_enabled", "0", "0", "1");
    // Hit effects culled once they are this far outside the view (the game's default is 1000).
    crowd("fxfrustumCutoff", "500", "250", "1000");

    // Model level of detail, the big maps' lever: "Scale the level of detail distance ... (larger reduces detail)",
    // 1..4. A model switches to its lower LODs sooner, and one past its last LOD is not drawn at all, so it is also a
    // draw distance: across a big map, fewer models to cull, sort, skin and draw on the main core. Static props and
    // the world's models (rigid) go further than characters (skinned: zombies stay visible at range). Both are
    // cheat-protected in the game; the runtime lifts that flag on these two before the settings are sent
    // (bo2_dvar_unlock_lod, game_host.cpp). The game's own settings put 1 back.
    const bool medium = s.preset == Preset::Medium;
    dvar("r_lodScaleRigid", 1, own ? "1" : low ? "2.5" : medium ? "1.75" : "1.25");
    dvar("r_lodScaleSkinned", 1, own ? "1" : low ? "1.5" : medium ? "1.25" : "1");
    // The game's strict culling of shadow casters (off by default; cheat-protected, unlocked with the LOD scales):
    // fewer models in the sun's shadow pass, which is a second walk of the world on the main core. Low and Medium;
    // High and the game's own keep the default. (The view distance, r_zfar, is not sent here: it depends on the map's
    // own distance and follows it, bo2_view_distance_tick.)
    dvar("sm_strictCull", 1, low || medium ? "1" : "0");

    // Aim assist (graphics.hpp, aim_assist). The three switches are cheat-protected like the LOD scales and unlocked
    // with them; the scripts' disable switch is not protected. The game's own: nothing sent (none of the four is
    // archived, so the game starts each run at its defaults: slowdown and catch on, lock-on off, scripts' switch off).
    if (const int want = aim_wanted(s.aim_assist); want > 0) {
        dvar("aim_slowdown_enabled", 1, "1");
        dvar("aim_autoaim_enabled", 1, "1");
        dvar("aim_lockon_enabled", 1, (want & 4) ? "1" : "0");
        dvar("aim_assist_script_disable", 1, "0");
    }
    return c;
}

const std::vector<std::string>& known_dvars() {
    static const std::vector<std::string> names = {
        "com_maxfps", "sm_enable", "sm_sunShadowSmall", "r_ssao", "r_dof_enable", "r_fxaa", "r_aaSamples",
        "r_picmip_manual", "r_picmip", "r_picmip_bump", "r_picmip_spec", "r_texFilterAnisoMax", "fx_marks_range",
        "cg_fov_default", "ai_corpseCount", "ragdoll_max_life", "dynEnt_spawnedLimit",
        "phys_piecesSpawnDistanceCutoff", "phys_impact_distance_cutoff", "sm_maxLights", "r_dlightLimit", "demo_enabled",
        "fxfrustumCutoff", "r_lodScaleRigid", "r_lodScaleSkinned", "sm_strictCull",
        "aim_slowdown_enabled", "aim_autoaim_enabled", "aim_lockon_enabled", "aim_assist_script_disable",
    };
    return names;
}

std::map<std::string, std::string> dvars_in_image(const std::string& pe_sections) {
    std::map<std::string, std::string> out;
    FILE* f = std::fopen(pe_sections.c_str(), "rb");
    if (!f) return out;
    std::string image;
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) image.append(buf, n);
    std::fclose(f);
    for (const std::string& name : known_dvars()) {
        // As a C string of its own: preceded by a NUL (or nothing) and followed by one.
        const std::string needle = name + std::string(1, '\0');
        for (size_t at = image.find(needle); at != std::string::npos; at = image.find(needle, at + 1)) {
            if (at == 0 || image[at - 1] == '\0') {
                std::string key(name);
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
                out[key] = kExistsNoValue;
                break;
            }
        }
    }
    return out;
}

void set_settings(const Settings& s) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_settings = s;
}

Settings settings() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_settings;
}

}  // namespace bo2gfx
