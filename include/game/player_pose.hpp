#pragma once

#include <cstdint>
#include <glm/vec3.hpp>

namespace wowee {
namespace game {

/// Where the player's character is and which way it faces, as the renderer has it (VITA-45).
///
/// Positions are in render space, the one the renderer draws in. A client with no renderer (the
/// headless core) leaves GameServices::playerPose null and the callers do without.
class IPlayerPose {
public:
    virtual ~IPlayerPose() = default;

    /// The character's position in render space.
    virtual glm::vec3 position() const = 0;

    /// Facing in the renderer's convention, in degrees.
    virtual float facingDeg() const = 0;

    /// Turn the character, and the camera that follows it, to this facing (degrees).
    virtual void setFacingDeg(float degrees) = 0;

    /// Where a model attachment point (1 = main hand) is in render space. False when the character
    /// has no model or no such point yet.
    virtual bool attachmentPosition(uint32_t attachmentId, glm::vec3& out) const = 0;
};

} // namespace game
} // namespace wowee
