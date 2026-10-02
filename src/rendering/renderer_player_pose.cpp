#include "rendering/renderer_player_pose.hpp"

#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/renderer.hpp"

#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

glm::vec3 RendererPlayerPose::position() const { return renderer_.getCharacterPosition(); }

float RendererPlayerPose::facingDeg() const { return renderer_.getCharacterYaw(); }

void RendererPlayerPose::setFacingDeg(float degrees) {
    // The renderer owns facing; the camera is told as well, as faceCanonicalYaw always did.
    renderer_.setCharacterYaw(degrees);
    if (auto* camera = renderer_.getCameraController()) camera->setFacingYaw(degrees);
}

bool RendererPlayerPose::attachmentPosition(uint32_t attachmentId, glm::vec3& out) const {
    auto* characters = renderer_.getCharacterRenderer();
    if (!characters) return false;
    glm::mat4 transform(1.0f);
    if (!characters->getAttachmentTransform(renderer_.getCharacterInstanceId(), attachmentId, transform))
        return false;
    out = glm::vec3(transform[3]);
    return true;
}

} // namespace rendering
} // namespace wowee
