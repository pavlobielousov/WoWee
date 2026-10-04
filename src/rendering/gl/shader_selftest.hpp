#pragma once
// On-device checks of the renderer's own shader programs (VITA-14): pixel read-back of every variant against known colours,
// a depth test, the GL limits, fill rate per program and per-draw CPU cost. Developer switch: WOWEE_GL_SELFTEST=1 in env.txt
// runs it once at startup (results are warning-level lines "GLTEST ..." in wowee.log; add WOWEE_LOG_FLUSH_MS=0 so a
// stall cannot lose the last lines). WOWEE_GL_TEST_ORDER=1 creates the textures after the DXT probe instead of before it.
namespace wowee::rendering::gl {

/// Run the checks if WOWEE_GL_SELFTEST is set. Call on the GL thread after the shader build.
void runShaderSelfTest();

}  // namespace wowee::rendering::gl
