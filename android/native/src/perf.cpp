// perf.cpp — see perf.hpp.
#include "perf.hpp"

#include "core_loader.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

namespace bo2perf {

namespace {

std::mutex g_mu;
std::vector<double> g_frames_ms;
double g_window_start = 0, g_last = 0;
Summary g_last_summary;
std::atomic<float> g_watts{-1.0f};
std::atomic<int> g_thermal_status{-1};
std::atomic<float> g_thermal_headroom{-1.0f};

double now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return v[k];
}

}  // namespace

void set_watts(float watts) { g_watts.store(watts); }

void set_thermal(int status, float headroom) {
    g_thermal_status.store(status);
    g_thermal_headroom.store(headroom);
}

void frame_presented() {
    const double t = now_ms();
    std::lock_guard<std::mutex> lock(g_mu);
    // The first frame, or the first after a pause (the app in the background, a dialog): a new window,
    // so the time away is not counted as one giant frame.
    if (g_last == 0 || t - g_last > 1000) {
        g_frames_ms.clear();
        g_last = g_window_start = t;
        return;
    }
    g_frames_ms.push_back(t - g_last);
    g_last = t;
    if (t - g_window_start < 5000) return;
    Summary s;
    s.seconds = (t - g_window_start) / 1000.0;
    s.fps = g_frames_ms.size() / s.seconds;
    s.max_ms = *std::max_element(g_frames_ms.begin(), g_frames_ms.end());
    s.over_33ms = static_cast<int>(std::count_if(g_frames_ms.begin(), g_frames_ms.end(), [](double f) { return f > 33.4; }));
    s.p99_ms = percentile(g_frames_ms, 0.99);
    s.p50_ms = percentile(g_frames_ms, 0.50);
    s.watts = g_watts.load();
    s.thermal_status = g_thermal_status.load();
    s.thermal_headroom = g_thermal_headroom.load();
    g_last_summary = s;
    std::fprintf(stderr,
                 "[PERF] %.1f s: %.1f fps, frame ms p50 %.1f p99 %.1f max %.1f, >33 ms: %d, %.1f W, thermal %d "
                 "headroom %.2f\n",
                 s.seconds, s.fps, s.p50_ms, s.p99_ms, s.max_ms, s.over_33ms, s.watts, s.thermal_status,
                 s.thermal_headroom);
    g_frames_ms.clear();
    g_window_start = t;
    // Every 30 s, the runtime's counters for the guest's `lock` instructions (sequences, contended, all-stripe).
    static int windows = 0;
    if (++windows % 6 == 0 && bo2core::api().hw_atomic_report) bo2core::api().hw_atomic_report("perf");
}

Summary last() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_last_summary;
}

}  // namespace bo2perf
