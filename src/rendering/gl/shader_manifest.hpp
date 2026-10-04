#pragma once
// The list of shader programs the Vita renderer builds (VITA-53). See shader_manifest.cpp.
#include "platform/vita/shader_cache_logic.hpp"

#include <vector>

namespace wowee::rendering::gl {

using ProgramClass = platform::vita::shadercache::ProgramClass;

struct ProgramDef {
    const char* name;      ///< shown on the progress screen
    ProgramClass cls;      ///< what it costs to compile (the bar's weight)
    const char* vertex;    ///< GLSL ES 1.00 sources
    const char* fragment;
    /// Attribute names in location order: attribute i is bound to location i before the program is linked, by
    /// the same function (linkProgram) for the first-run build and the renderer, so a program loaded from the cache
    /// is always linked the way it is used. Empty = let vitaGL assign.
    std::vector<const char*> attributes = {};
    /// The run-time shader compiler's optimisation level for this program (a shark_opt value), or -1 for the client's
    /// default (O2). Part of the cache key. A higher level costs compile time (once, on the first run) and, measured on
    /// the device, only paid off for the discard-heavy alpha-test shader (DEV_SETUP section 22).
    int compilerLevel = -1;
};

/// Every program of the renderer, in build order (cheap and the login screen's first, characters last).
const std::vector<ProgramDef>& shaderManifest();

}  // namespace wowee::rendering::gl
