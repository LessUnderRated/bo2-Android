// prof.hpp — the app measures itself: per-thread CPU, what the system exposes (GPU, clocks, battery), and a sampling
// profile of the busiest threads, as log lines ([THREADS], [SYSINFO], [PROF]) that reach the log server with the rest.
// No adb, no root, no simpleperf: a run on the phone anywhere can be profiled. prof.cpp has the details.
#pragma once

namespace bo2prof {

// Starts the profiler thread (once; later calls do nothing). BO2_PROF=0 turns the sampling off ([THREADS] and
// [SYSINFO] stay); BO2_PROF_HZ sets the sampling rate per thread (default 250, in samples per CPU-second).
void start();

}  // namespace bo2prof
