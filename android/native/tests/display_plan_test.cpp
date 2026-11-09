// Host tests for the display plan (scripts/test-host.sh runs them).
#include "display_plan.hpp"

#include <cmath>
#include <cstdio>

using namespace bo2display;

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

static void s26_fill_covers_the_whole_panel() {
    Plan p = plan(2340, 1080, Settings{Aspect::Fill, 720});
    CHECK(p.dest.x == 0 && p.dest.y == 0 && p.dest.w == 2340 && p.dest.h == 1080);
    CHECK(p.guest_h == 720 && p.guest_w == 1560);  // 19.5:9 at 720 lines
    CHECK(std::fabs(p.aspect - 2340.0f / 1080.0f) < 0.01f);
}

static void s26_native_is_one_to_one() {
    Plan p = plan(2340, 1080, Settings{Aspect::Fill, 0});
    CHECK(p.guest_w == 2336 || p.guest_w == 2340);  // multiples of 8: 2336 x 1080 is the nearest
    CHECK(p.guest_h == 1080);
    CHECK(p.guest_w <= 2340 && p.guest_h <= 1080);
}

static void ultra_and_plus_qhd_panels() {
    Plan p = plan(3120, 1440, Settings{Aspect::Fill, 900});
    CHECK(p.dest.w == 3120 && p.dest.h == 1440);
    CHECK(p.guest_h == 904 || p.guest_h == 896 || p.guest_h == 900);
    CHECK(std::fabs(p.aspect - 3120.0f / 1440.0f) < 0.02f);
}

static void wide_16_9_is_bo2_mac_with_bars() {
    Plan p = plan(2340, 1080, Settings{Aspect::Wide16x9, 720});
    CHECK(p.guest_w == 1280 && p.guest_h == 720);
    CHECK(p.dest.h == 1080 && p.dest.w == 1920 && p.dest.x == 210 && p.dest.y == 0);
}

static void portrait_report_is_taken_as_landscape() {
    Plan p = plan(1080, 2340, Settings{Aspect::Fill, 720});
    CHECK(p.panel_w == 2340 && p.panel_h == 1080 && p.guest_w == 1560);
}

static void render_never_exceeds_what_is_shown() {
    Plan p = plan(1920, 1080, Settings{Aspect::Fill, 1440});
    CHECK(p.guest_h == 1080 && p.guest_w == 1920);
    Plan z = plan(0, 0, Settings{});
    CHECK(z.guest_w > 0 && z.guest_h > 0);
}

int main() {
    s26_fill_covers_the_whole_panel();
    s26_native_is_one_to_one();
    ultra_and_plus_qhd_panels();
    wide_16_9_is_bo2_mac_with_bars();
    portrait_report_is_taken_as_landscape();
    render_never_exceeds_what_is_shown();
    if (failures) {
        std::fprintf(stderr, "display_plan_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("display_plan_test: all passed");
    return 0;
}
