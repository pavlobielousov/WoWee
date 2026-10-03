// Vita skeleton of PostProcessPipeline (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/post_process_pipeline.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/post_process_pipeline.hpp"

namespace wowee::rendering {

void PostProcessPipeline::setAmdFsr3FramegenEnabled([[maybe_unused]] bool enabled) { }

void PostProcessPipeline::setFSR2DebugTuning([[maybe_unused]] float jitterSign, [[maybe_unused]] float motionVecScaleX, [[maybe_unused]] float motionVecScaleY) { }

void PostProcessPipeline::setFSRQuality([[maybe_unused]] float scaleFactor) { }

void PostProcessPipeline::setFSRSharpness([[maybe_unused]] float sharpness) { }

void PostProcessPipeline::setFXAAEnabled([[maybe_unused]] bool enabled) { }

}  // namespace wowee::rendering
