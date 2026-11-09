// perf.hpp — frame timing and power draw, measured where the frame is presented.
//
// bo2-mac counts drawables actually presented, checked against the Metal HUD. Here the platform layer
// calls frame_presented() once per vkQueuePresentKHR (today: once per SDL_RenderPresent in the shell),
// and every five seconds one line goes to the log:
//   [PERF] 5.0 s: 59.8 fps, frame ms p50 16.7 p99 18.1 max 33.4, >33 ms: 1, 6.1 W
// The watts come from the battery (Power.java), so a run on the charger reads -1. The thermal status and
// headroom come from PowerManager: a phone's sustained frame rate is set by heat, not by the first minute.
#pragma once

#include <cstdint>

namespace bo2perf {

void frame_presented();
void set_watts(float watts);
// PowerManager's thermal status (0 none .. 6 shutdown) and headroom (1.0 = throttling starts; -1 unknown).
void set_thermal(int status, float headroom);

struct Summary {
    double seconds = 0, fps = 0, p50_ms = 0, p99_ms = 0, max_ms = 0;
    int over_33ms = 0;
    float watts = -1;
    int thermal_status = -1;
    float thermal_headroom = -1;
};
// The last five-second window (zeros before the first one closes).
Summary last();

}  // namespace bo2perf
