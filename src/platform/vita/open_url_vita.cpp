// The Vita has no browser to hand a link to (src/core/open_url.cpp spawns one with posix_spawn, which newlib
// lacks). The callers treat false as "could not open" and show the address instead.
#include "core/open_url.hpp"

namespace wowee::core {

bool openExternalUrl(const std::string&) { return false; }

}  // namespace wowee::core
