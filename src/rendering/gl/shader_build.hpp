#pragma once
// The first-run shader build with its progress screen (VITA-53, design in the item).
namespace wowee::rendering::gl {

struct ShaderBuildResult {
    unsigned programs = 0;   ///< programs in the manifest, the interface's own included
    unsigned compiled = 0;   ///< compiled from source this run
    unsigned cached = 0;     ///< already in the cache
    unsigned failed = 0;
    double totalMs = 0;
    double longestStallMs = 0;  ///< the longest time the screen could not be redrawn: one program's compile
};

/// Build every program of the manifest (cache hits cost nothing). While anything has to be compiled a determinate
/// progress screen is up and redrawn between programs; if everything is cached no frame is drawn. Call once, on the
/// GL thread, after vitaGL is initialised and before the interface starts.
ShaderBuildResult buildShaders();

}  // namespace wowee::rendering::gl
