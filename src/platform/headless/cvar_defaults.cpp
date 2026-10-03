// CVars for wowee_core (VITA-47): there is no store, so every setting is its default.
//
// game/ asks addons::storedCVarValue() for a handful of user settings (threat warning, loot spam,
// auto-dismount, ...). The real one is in src/addons/lua_system_api.cpp, next to the Lua CVar store,
// and the core does not have the Lua layer yet (VITA-49). Until it joins, the fallback each caller
// passes is the answer. When src/addons/ becomes part of wowee_core, drop this file from
// cmake/wowee_core.cmake: both define the same function.
#include <string>

#include "addons/lua_api_registrations.hpp"

namespace wowee {
namespace addons {

std::string storedCVarValue(const std::string&, const std::string& fallback) { return fallback; }

} // namespace addons
} // namespace wowee
