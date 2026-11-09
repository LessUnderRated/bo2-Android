// runtime/harness/harness_main.cpp — the recompiled game on the PC, for testing without a phone (scripts/harness/).
//
// The same arm64 game core as the APK (lifted.o, bo2-mac's runtime, this port's Vulkan backend), linked for Linux
// glibc and run under qemu-user in WSL, rendering headless with Mesa's lavapipe. This file stands in for the Android
// shell (android/native/src/game_host.cpp): it runs bo2_game_main on a 64 MB stack, fills the Vulkan backend's
// hooks, and is the game's XInput pad. The pad and the screenshots are driven through a control FIFO
// (BO2_HARNESS_CTL), one command per line:
//   a | b | x | y | start | back | up | down | left | right [ms]   press, hold ms (default 150), release
//   cmd <console command>                                           bo2_cmd_queue
//   shot <file.ppm>                                                 the next presented frame, as a PPM
//   quit                                                            exit at once
// The environment (BO2_GAME_DIR, BO2_PE_SECTIONS, ...) must be set before the process starts: the runtime reads some
// of it in static initializers and caches every variable on first lookup (scripts/harness/run.sh sets it).
#include "vk_hooks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int bo2_game_main(int argc, char** argv);
extern "C" void bo2_cmd_queue(const char* cmd);
extern "C" uint8_t* bo2_g_base;   // the runtime's flat 4 GB guest map (win32_runtime.cpp)

namespace {

// ---- The pad: XINPUT_STATE as XInputGetState returns it ----------------------------------------------------------
std::mutex g_pad_mu;
uint32_t g_packet = 1;
uint16_t g_buttons = 0;

void set_buttons(uint16_t b) {
    std::lock_guard<std::mutex> lock(g_pad_mu);
    if (b != g_buttons) {
        g_buttons = b;
        g_packet++;
    }
}

uint16_t button_bit(const std::string& name) {
    static const struct { const char* name; uint16_t bit; } kButtons[] = {
        {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010}, {"back", 0x0020},
        {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000},
    };
    for (const auto& k : kButtons)
        if (name == k.name) return k.bit;
    return 0;
}

// ---- The renderer hooks: headless, 1920x1080 -----------------------------------------------------------------------
std::atomic<uint64_t> g_frames{0};
std::mutex g_shot_mu;
std::string g_shot_path;   // non-empty: save the next presented frame

void* hook_native_window() { return nullptr; }
void hook_panel_size(int* w, int* h) { *w = 1920; *h = 1080; }
void hook_dest_rect(int pw, int ph, int* x, int* y, int* w, int* h) { *x = 0; *y = 0; *w = pw; *h = ph; }
int hook_frame_cap() { return 0; }
int hook_pump() { return 1; }
int hook_foreground() { return 1; }
double hook_refresh_hz() { return 60.0; }

void hook_frame_presented() {
    const uint64_t n = ++g_frames;
    if (n % 100 == 0) fprintf(stderr, "[HARNESS] %llu frames presented\n", (unsigned long long)n);
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_shot_mu);
        path.swap(g_shot_path);
    }
    if (path.empty()) return;
    std::vector<uint8_t> rgba(3840u * 2160u * 4u);
    uint32_t w = 0, h = 0;
    if (!bo2vk_read_backbuffer(rgba.data(), (uint32_t)rgba.size(), &w, &h) || !w || !h) {
        fprintf(stderr, "[HARNESS] shot %s: no back buffer\n", path.c_str());
        return;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "[HARNESS] shot %s: cannot write\n", path.c_str()); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; i++) fwrite(&rgba[4 * i], 1, 3, f);
    fclose(f);
    fprintf(stderr, "[HARNESS] shot %s (%ux%u, frame %llu)\n", path.c_str(), w, h, (unsigned long long)n);
}

// ---- The control FIFO -------------------------------------------------------------------------------------------
void control_thread(std::string path) {
    mkfifo(path.c_str(), 0600);
    for (;;) {
        FILE* f = fopen(path.c_str(), "r");   // blocks until a writer opens it
        if (!f) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (s.empty()) continue;
            fprintf(stderr, "[HARNESS] < %s\n", s.c_str());
            const size_t sp = s.find(' ');
            const std::string verb = s.substr(0, sp), rest = sp == std::string::npos ? "" : s.substr(sp + 1);
            if (verb == "quit") { fflush(stderr); _exit(0); }
            if (verb == "cmd") { bo2_cmd_queue(rest.c_str()); continue; }
            if (verb == "shot") { std::lock_guard<std::mutex> lock(g_shot_mu); g_shot_path = rest; continue; }
            // find <text>: guest addresses holding <text> (the flat guest map reads untouched pages as zeros).
            if (verb == "find" && !rest.empty() && bo2_g_base) {
                int hits = 0;
                const uint64_t lo = 0x10000, hi = 0xF0000000ull, chunk = 64ull << 20;
                for (uint64_t a = lo; a < hi && hits < 40; a += chunk) {
                    const uint64_t n = std::min<uint64_t>(chunk + rest.size(), hi - a);
                    const uint8_t* p = bo2_g_base + a;
                    const uint8_t* q = p;
                    while (hits < 40 && (q = (const uint8_t*)memmem(q, (size_t)(p + n - q), rest.data(), rest.size()))) {
                        const uint32_t va = (uint32_t)(a + (uint64_t)(q - p));
                        std::string ctx;
                        for (int k = -24; k < 56; k++) {
                            const uint8_t c = bo2_g_base[(uint32_t)(va + k)];
                            ctx += (c >= 32 && c < 127) ? (char)c : '.';
                        }
                        fprintf(stderr, "[HARNESS] find \"%s\" @0x%08x: %s\n", rest.c_str(), va, ctx.c_str());
                        hits++;
                        q++;
                    }
                }
                fprintf(stderr, "[HARNESS] find \"%s\": %d hit(s)\n", rest.c_str(), hits);
                continue;
            }
            // peek <hexva> [count]: guest dwords (state fields read live, e.g. a client's connection state).
            if (verb == "peek" && bo2_g_base) {
                unsigned va = 0, n = 1;
                if (sscanf(rest.c_str(), "%x %u", &va, &n) >= 1) {
                    std::string out;
                    for (unsigned i = 0; i < n && i < 64; i++) {
                        uint32_t v;
                        memcpy(&v, bo2_g_base + va + 4 * i, 4);
                        char b[16];
                        snprintf(b, sizeof b, " %08x", v);
                        out += b;
                    }
                    fprintf(stderr, "[HARNESS] peek 0x%08x:%s\n", va, out.c_str());
                }
                continue;
            }
            // strings <hexva> <len>: printable runs of 4+ characters in a guest range (a script chunk's constants).
            if (verb == "strings" && bo2_g_base) {
                unsigned va = 0, len = 0;
                if (sscanf(rest.c_str(), "%x %u", &va, &len) == 2) {
                    std::string run;
                    uint32_t start = va;
                    for (uint32_t i = 0; i <= len; i++) {
                        const uint8_t c = i < len ? bo2_g_base[va + i] : 0;
                        if (c >= 32 && c < 127) { if (run.empty()) start = va + i; run += (char)c; continue; }
                        if (run.size() >= 4) fprintf(stderr, "[HARNESS] str @0x%08x %s\n", start, run.c_str());
                        run.clear();
                    }
                }
                continue;
            }
            if (const uint16_t bit = button_bit(verb)) {
                const int ms = rest.empty() ? 150 : atoi(rest.c_str());
                set_buttons(bit);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                set_buttons(0);
                continue;
            }
            fprintf(stderr, "[HARNESS] unknown command: %s\n", s.c_str());
        }
        fclose(f);
    }
}

struct GameArgs { int argc; char** argv; int status; };
void* game_thread(void* p) {
    auto* a = static_cast<GameArgs*>(p);
    a->status = bo2_game_main(a->argc, a->argv);
    return nullptr;
}

}  // namespace

// ---- What the runtime asks of the shell's pad bridge (runtime/input/pad_sdl.cpp on Android) ----------------------
extern "C" uint32_t bo2_pad_etat(uint32_t idx, uint8_t out[16]) {
    if (idx != 0) return 0x48F;   // ERROR_DEVICE_NOT_CONNECTED
    std::lock_guard<std::mutex> lock(g_pad_mu);
    memset(out, 0, 16);
    memcpy(out, &g_packet, 4);
    memcpy(out + 4, &g_buttons, 2);
    return 0;
}
extern "C" uint32_t bo2_pad_capacites(uint32_t idx, uint8_t out[20]) {
    if (idx != 0) return 0x48F;
    memset(out, 0, 20);
    out[0] = 1;   // XINPUT_DEVTYPE_GAMEPAD
    out[1] = 1;   // XINPUT_DEVSUBTYPE_GAMEPAD
    const uint16_t all = 0xF3FF;
    memcpy(out + 4, &all, 2);
    return 0;
}
extern "C" uint32_t bo2_pad_vibrer(uint32_t idx, uint16_t, uint16_t) { return idx == 0 ? 0 : 0x48F; }
extern "C" void bo2_pad_top() {}

int main(int argc, char** argv) {
    setvbuf(stderr, nullptr, _IOLBF, 0);
    for (const char* need : {"BO2_GAME_DIR", "BO2_PE_SECTIONS", "BO2_HARNESS_CTL"})
        if (!getenv(need)) { fprintf(stderr, "[HARNESS] %s is not set (scripts/harness/run.sh sets it)\n", need); return 2; }
    if (chdir(getenv("BO2_GAME_DIR")) != 0) { perror("[HARNESS] chdir"); return 2; }
    Bo2VkShellHooks h{};
    h.native_window = hook_native_window;
    h.panel_size = hook_panel_size;
    h.dest_rect = hook_dest_rect;
    h.frame_cap = hook_frame_cap;
    h.frame_presented = hook_frame_presented;
    h.pump = hook_pump;
    h.foreground = hook_foreground;
    h.refresh_hz = hook_refresh_hz;
    bo2vk_set_hooks(&h);
    std::thread(control_thread, std::string(getenv("BO2_HARNESS_CTL"))).detach();
    std::thread([] {   // the pad is plugged in: tell the game, as the shell's bridge does on connect
        std::this_thread::sleep_for(std::chrono::seconds(20));
        bo2_cmd_queue("set gpad_enabled 1");
    }).detach();
    GameArgs args{argc, argv, 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t game;
    if (pthread_create(&game, &attr, game_thread, &args) != 0) { fprintf(stderr, "[HARNESS] no game thread\n"); return 1; }
    pthread_join(game, nullptr);
    fprintf(stderr, "[HARNESS] bo2_game_main returned %d after %llu frames\n", args.status,
            (unsigned long long)g_frames.load());
    return args.status;
}
