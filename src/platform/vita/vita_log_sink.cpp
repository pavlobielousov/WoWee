// UDP net-log for the Vita (VITA-6): each log line also goes out as one datagram when
// WOWEE_LOG_UDP=<ip>[:port] is set in env.txt. Lossy by design; the file is the source of truth.
// View on the dev machine with tools/vita/logsink.sh.
#include "platform/vita/vita_platform.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace wowee::platform::vita {

namespace {

struct Sink {
    bool tried = false;
    int fd = -1;
    sockaddr_in addr{};
};
Sink g_sink;

void open(Sink& s) {
    s.tried = true;
    const char* spec = std::getenv("WOWEE_LOG_UDP");
    if (!spec || !*spec) return;
    char host[64];
    std::size_t hostLen = std::strcspn(spec, ":");
    if (hostLen == 0 || hostLen >= sizeof host) return;
    std::memcpy(host, spec, hostLen);
    host[hostLen] = '\0';
    long port = 9999;
    if (spec[hostLen] == ':') port = std::strtol(spec + hostLen + 1, nullptr, 10);
    if (port <= 0 || port > 65535) return;

    s.addr.sin_family = AF_INET;
    s.addr.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, host, &s.addr.sin_addr) != 1) return;
    s.fd = socket(AF_INET, SOCK_DGRAM, 0);
}

}  // namespace

// Runs under the logger's mutex: nothing here may log.
void sendLogLine(const std::string& line) {
    if (!g_sink.tried) open(g_sink);
    if (g_sink.fd < 0) return;
    // One datagram per line, kept under a typical MTU; a longer line is cut.
    const std::size_t n = line.size() < 1400 ? line.size() : 1400;
    sendto(g_sink.fd, line.data(), n, 0, reinterpret_cast<const sockaddr*>(&g_sink.addr), sizeof g_sink.addr);
}

}  // namespace wowee::platform::vita
