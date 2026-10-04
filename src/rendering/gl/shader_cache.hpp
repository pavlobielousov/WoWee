#pragma once
// The Vita's shader cache, the vitaGL half (VITA-53). The pure logic is include/platform/vita/shader_cache_logic.hpp.
//
// Transparent: the client links with -Wl,--wrap=glCreateShader,glShaderSource,glCompileShader,glAttachShader,
// glLinkProgram,glDeleteShader (cmake/vita/wowee_client.cmake), so every GL user, the stock ImGui backend included,
// goes through it without a change. A shader whose binary is cached is loaded with glShaderBinary instead of compiled;
// after a program links, the shaders that were compiled are dumped with vglGetShaderBinary and written to
// ux0:data/wowee/shaders/<key>.bin (a temporary file, then a rename, so an interrupted write loses nothing).

#include <cstddef>
#include <cstdint>
#include <string>

namespace wowee::rendering::gl {

struct ShaderCacheStats {
    unsigned hits = 0;       ///< shaders loaded from the cache
    unsigned compiled = 0;   ///< shaders compiled from source
    unsigned stored = 0;     ///< binaries written
    unsigned failed = 0;     ///< compile or store failures
    double compileMs = 0;    ///< time spent in glLinkProgram for programs that had to compile
    double longestCompileMs = 0;
};

/// Directory of the cache. Created on first use.
const std::string& shaderCacheDir();

/// The optimisation level the run-time shader compiler was set to (a shark_opt value). Part of every cache key: a
/// different level produces a different binary. Call before the first shader is compiled.
void setShaderCompilerLevel(int level);

/// Turn the cache on/off (on by default). Off: the wrappers pass straight through.
void setShaderCacheEnabled(bool enabled);

/// Delete every cached binary (the "force a rebuild" switch, WOWEE_SHADER_REBUILD=1 in env.txt).
void clearShaderCache();

/// Remove entries no shader of this run asked for after `keepKeys` were noted (see noteKeyUsed): leftovers of an older
/// shader or build tag. Call after all programs of a run have been built.
void sweepShaderCache();

const ShaderCacheStats& shaderCacheStats();

/// Whether both shaders of a program are in the cache (a valid entry each). Used to decide whether building it will be
/// instant, so the progress screen is shown only for work that takes time.
bool shaderCacheHas(const char* vertexSource, const char* fragmentSource);

/// Whether the cache holds any entry at all (a first run).
bool shaderCacheEmpty();

}  // namespace wowee::rendering::gl
