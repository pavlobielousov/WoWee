#pragma once

#include "game/player_pose.hpp"

namespace wowee {
namespace rendering {

class Renderer;

/// game::IPlayerPose on top of the Vulkan renderer: the character's position and yaw live on the
/// Renderer, its attachment points on the CharacterRenderer, and the camera follows the yaw (VITA-45).
class RendererPlayerPose final : public game::IPlayerPose {
public:
    explicit RendererPlayerPose(Renderer& renderer) : renderer_(renderer) {}

    glm::vec3 position() const override;
    float facingDeg() const override;
    void setFacingDeg(float degrees) override;
    bool attachmentPosition(uint32_t attachmentId, glm::vec3& out) const override;

private:
    Renderer& renderer_;
};

} // namespace rendering
} // namespace wowee
