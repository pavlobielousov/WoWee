#pragma once
// The Vita renderer's own shader programs (VITA-14), GLSL ES 1.00 embedded as strings: no file I/O on the device, and the
// cache key is a hash of exactly these strings. Variants come from one source with a #define prefix (vitaGL's
// translator has a preprocessor), so a program is a name, a cost class and a define.
//
// What is cut from upstream's terrain.frag / m2.frag, per ADR-001: shadows, the 64 local lights, volumetric fog, ray-traced
// lighting, derivative normal mapping, the alpha-map seam blur, foliage sway, specular and the SSBO instancing. Lighting
// and fog are per vertex; the fragment shader only mixes textures. Lighting terms are upstream's own (ambient + Lambert,
// ambient only for unlit), the fog is upstream's linear one.
#include "rendering/gl/shader_manifest.hpp"

namespace wowee::rendering::gl {

/// Terrain with `layers` (0..3) texture layers over the base, blended in upstream's order (each mix on the previous
/// result). Samplers: uBase, uLayer1..3, and uAlpha: ONE RGBA texture per chunk with the alpha maps of layers 1..3 in
/// r, g, b (packed when the chunk is uploaded, VITA-18). TexCoord tiles the layers; LayerUV is 0..1 across the chunk.
const ProgramDef& terrainProgram(int layers);

/// Static (unskinned) doodads. Opaque: no test. AlphaTest: `discard` below uParams.x (the cutoff) or below the
/// colour-key luminance uParams.y. Blend: texture alpha times uParams.z (the instance fade), no discard.
/// The GL blend state is the caller's, as upstream's pipelines are: Opaque none; AlphaTest and Blend
/// SRC_ALPHA, ONE_MINUS_SRC_ALPHA; additive (upstream blend modes 3 and 4) SRC_ALPHA, ONE with the Blend program and
/// uParams.w = 0, which fades the fog to black instead of to the fog colour, as m2.frag does for blendMode >= 3.
enum class M2Kind { Opaque, AlphaTest, Blend };
const ProgramDef& m2Program(M2Kind kind);

// Building groups: the same three classes with a vertex colour attribute and the WMO light modes.
const ProgramDef& wmoProgram(M2Kind kind);

// Copies the off-screen scene to the screen, scaled.
const ProgramDef& blitProgram();

// Terrain and building water: flat colour, shimmer, fog.
const ProgramDef& waterProgram();

}  // namespace wowee::rendering::gl
