// See gl_program.hpp (VITA-14).
#include "rendering/gl/gl_program.hpp"

#include "core/logger.hpp"
#include "rendering/gl/shader_cache.hpp"

namespace wowee::rendering::gl {

GLuint linkProgram(const ProgramDef& def) {
    // The program's own compiler level, for the cache key (at glCompileShader) and for the compile itself (at link).
    const int level = def.compilerLevel >= 0 ? def.compilerLevel : shaderCompilerDefaultLevel();
    applyShaderCompilerLevel(level);
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &def.vertex, nullptr);
    glShaderSource(f, 1, &def.fragment, nullptr);
    glCompileShader(v);
    glCompileShader(f);
    GLuint program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    // Attached first: for a shader loaded from the cache vitaGL looks the attribute up in the loaded binary at bind time;
    // for one it translates it records the binding and applies it at link (custom_shaders.c).
    for (std::size_t i = 0; i < def.attributes.size(); ++i) {
        glBindAttribLocation(program, static_cast<GLuint>(i), def.attributes[i]);
    }
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {};
        glGetProgramInfoLog(program, sizeof log - 1, nullptr, log);
        LOG_ERROR("Shader program '", def.name, "' failed to link: ", log);
        glDeleteProgram(program);
        program = 0;
    }
    applyShaderCompilerLevel(shaderCompilerDefaultLevel());
    // The program keeps what it needs; the shader objects are no longer wanted.
    glDeleteShader(v);
    glDeleteShader(f);
    return program;
}

}  // namespace wowee::rendering::gl
