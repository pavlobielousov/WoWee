// wowee_headless (VITA-48): a console client built on wowee_core only. Settings come from the
// environment (see headless_client.hpp); the password is never taken from the command line,
// where `ps` would show it, and nothing here has a default server.
#include "headless_client.hpp"

#include "core/logger.hpp"

#include <csignal>
#include <cstdio>

int main() {
#ifndef _WIN32
    // A server that drops the connection must not kill the driver with SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    using namespace wowee::headless;

    Options options;
    if (const std::string err = optionsFromEnv(options); !err.empty()) {
        std::fprintf(stderr,
                     "wowee_headless: %s\n"
                     "Set WOWEE_HEADLESS_HOST, _ACCOUNT, _PASSWORD and _EXPANSION (and optionally\n"
                     "_PORT, _REALM, _TIMEOUT, _CHARACTER, _SECONDS, WOW_DATA_PATH, WOWEE_REALM_HOST_OVERRIDE).\n",
                     err.c_str());
        return kUsage;
    }

    Client client(options);
    int rc = client.initialize();
    if (rc == kOk) rc = client.loginAndListCharacters();
    if (rc == kOk) rc = client.enterWorldAndObserve();
    client.shutdown();
    if (rc != kOk) {
        std::fprintf(stderr, "wowee_headless: failed (exit code %d), see the log above\n", rc);
    }
    return rc;
}
