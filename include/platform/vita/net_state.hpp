#pragma once

// PS Vita network helpers for src/network (VITA-7). Header-only so that every Vita executable that
// links the core gets them without another source file; sceNetCtl is already in the link line
// (cmake/vita/Vita.cmake, wowee_vita_executable). Included from net_platform.hpp's __vita__ arms.

#include <psp2/net/netctl.h>

#include <string>

namespace wowee::platform::vita {

// Whether the Wi-Fi link is up. `state` receives sceNetCtlInetGetState's value (0 disconnected,
// 1 connecting, 2 finalizing, 3 connected). If the call itself fails the answer is "assume up" and
// `state` is -1: a failed query must not block a connection that would have worked.
inline bool wifiConnected(int& state) {
    state = -1;
    if (sceNetCtlInetGetState(&state) < 0) {
        state = -1;
        return true;
    }
    return state == SCE_NETCTL_STATE_CONNECTED;
}

// A getsockopt(SO_ERROR) value, in words. The Vita reports sceNet's numbers there, which follow the
// BSD numbering (a refused connection is 61, measured on Vita3K), not newlib's errno values (111),
// so strerror() would name the wrong error. Only 61 has been observed; the others follow the BSD
// numbers and are unverified on hardware (VITA-34). The SDK has no named constants for them.
inline std::string socketErrorString(int soError) {
    switch (soError) {
        case 50: return "network is down";
        case 51: return "network is unreachable";
        case 54: return "connection reset by peer";
        case 60: return "connection timed out";
        case 61: return "connection refused";
        case 64: return "host is down";
        case 65: return "no route to host";
        default: return "sceNet socket error " + std::to_string(soError);
    }
}

}  // namespace wowee::platform::vita
