#pragma once

// PS Vita platform layer (VITA-6). Process setup, env.txt, the UDP log sink and the startup
// report. Everything here is Vita-only code; shared files call it from small `__vita__` arms.

#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::platform::vita {

inline constexpr const char* kAppDir = "ux0:data/wowee";
inline constexpr const char* kEnvFile = "ux0:data/wowee/env.txt";
inline constexpr const char* kDefaultDataRoot = "ux0:data/wowee/Data";
inline constexpr const char* kDefaultConfigRoot = "ux0:data/wowee/config";

// One KEY=VALUE line of env.txt. Pure string work, unit-tested on the host
// (tools/vita/depcheck/envfile_test.cpp). Accepts LF or CRLF, "#" comment lines, blanks around the
// key and the value, and an optional "export " prefix. Returns false for blank lines, comments
// and lines without a key or without "=".
bool parseEnvLine(std::string_view line, std::string& key, std::string& value);

// Reads a KEY=VALUE file and applies each line with setenv (overwriting). Returns the keys set, in
// file order; values are kept out of anything reported because they may be credentials.
// `fileFound` is false when the file does not exist.
std::vector<std::string> loadEnvFile(const char* path, bool& fileFound);

// Process setup, the first call in main(): clocks, sysmodules, sceNet, ux0:data/wowee, env.txt
// (setenv) and the data-path default. Needs no logger and never logs; what happened is kept for
// logStartupReport(). Safe to call once only.
void initProcess();

// Sends one log line as a UDP datagram when WOWEE_LOG_UDP=<ip>[:port] (from env.txt) is set.
// Called by the logger under its mutex: must never log. Cheap no-op otherwise.
void sendLogLine(const std::string& line);

// Writes one log line to the log file on a thread of its own (VITA-20). The memory card is one queue: while workers stream reads,
// a write from the main thread waited 150 ms on average and up to 1.1 s (74 of them in a two-minute walk, 11 s in all), and the
// logger writes a line at a time. The first call starts the writer; it owns `stream` from then on, writes what has gathered as one
// batch and flushes it. Called by the logger under its mutex: must never log.
void asyncLogWrite(std::ofstream& stream, std::string line);

// Logs (at WARNING, so the default log level shows it) what initProcess did and the resolved
// data and config roots. The last step of the Vita startup path until the Application can be
// built (VITA-9, VITA-12).
void logStartupReport();

// The threads that have called core::enterThread so far (id and role name), up to `max`; returns
// how many. For the CPU statistics of a long run (tools/headless/run_stats.hpp, VITA-7).
struct RegisteredThread {
    int tid;
    const char* roleName;
};
size_t registeredThreads(RegisteredThread* out, size_t max);

}  // namespace wowee::platform::vita
