#pragma once
// Compile and link one ProgramDef (VITA-14): the single place a program is built, for the first-run build and for the
// renderer alike. Attribute i of def.attributes is bound to location i after the shaders are attached and before the link.
#include "rendering/gl/shader_manifest.hpp"

#include <vitaGL.h>

namespace wowee::rendering::gl {

/// 0 on failure (the compile/link log is in the client log). The shaders come from the cache when it has them.
GLuint linkProgram(const ProgramDef& def);

}  // namespace wowee::rendering::gl
