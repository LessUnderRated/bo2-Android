// remote_log.hpp — see remote_log.cpp.
#pragma once

#include <cstddef>
#include <string>

namespace bo2remote {

// This run's session id (device model, local start time, pid); stable for the process.
const std::string& session();

// Starts the sender if BO2_LOGD is "host:port" (unset, empty or "0": off). log_dir holds the previous run's log,
// whose tail is uploaded to that run's session (a crash report written after its last batch still arrives).
// With BO2_DIAG=2 (the launcher's Logging: Minimal) the server is remembered but nothing is sent until set_enabled.
void start(const std::string& log_dir, const char* previous_log_name);

// The launcher's Logging option, live: false stops queueing and sending (what is queued is dropped), true starts the
// sender if it never ran. Nothing without a log server (BO2_LOGD).
void set_enabled(bool on);

// One line (or several), each ending in '\n', as written to the log file. Cheap: a copy under a mutex.
void enqueue(const char* text, size_t n);

}  // namespace bo2remote
