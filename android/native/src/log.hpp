// log.hpp — see log.cpp.
#pragma once

namespace bo2log {

constexpr const char* kCurrent = "bo2.log";
constexpr const char* kPrevious = "bo2-previous.log";

// Starts this run's log in dir (the last run's kept beside it), takes over stdout and stderr, and
// installs the crash handler. The first call does it; the setup screen and the game both call it.
void open(const char* dir);

// A line (or several) to logcat and the file.
void write(const char* line);

// The launcher's Logging option: 0 Full (logcat, the PC log, the profiler with its sampling), 1 Normal (no sampling
// profiler: [THREADS]/[SYSINFO] only), 2 Minimal (the file only: no logcat copy, nothing sent to the PC, no profiler;
// crash reports still work). Starts as BO2_DIAG (Bo2App, from the saved choice); the launcher changes it live.
int diagnostics();
void set_diagnostics(int level);

// Called (on the log thread) with every line the game prints, before any filtering: the shell watches it for
// moments it must act on (the main menu reached, a map loading). Keep it cheap; it must not print.
void set_line_observer(void (*observer)(const char* line));

}  // namespace bo2log
