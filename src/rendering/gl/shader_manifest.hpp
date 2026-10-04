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
};

/// Every program of the renderer, in build order (cheap and the login screen's first, characters last).
const std::vector<ProgramDef>& shaderManifest();

}  // namespace wowee::rendering::gl
