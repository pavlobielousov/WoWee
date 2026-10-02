#include "rendering/renderer_screen_effects.hpp"

#include "rendering/camera_controller.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/renderer.hpp"

namespace wowee {
namespace rendering {

void RendererScreenEffects::setIntoxication(float amount) {
    // Either part can be missing (no camera yet, post-processing off), as before.
    if (auto* camera = renderer_.getCameraController()) camera->setIntoxication(amount);
    if (auto* post = renderer_.getPostProcessPipeline()) post->setIntoxication(amount);
}

} // namespace rendering
} // namespace wowee
