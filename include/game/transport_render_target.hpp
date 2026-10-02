#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>

namespace wowee {
namespace game {

/// What the transport logic needs from the WMO side of the renderer: a moving ship or zeppelin is a
/// WMO instance that is moved, hidden, animated and asked about its deck (VITA-45). Instance ids
/// are the renderer's. A client with no renderer leaves GameServices::transportWmo null.
class ITransportWmoTarget {
public:
    virtual ~ITransportWmoTarget() = default;

    virtual void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) = 0;
    /// Marks the deck as in motion, so the static-world floor query only counts it when underfoot.
    virtual void setInstanceIsTransport(uint32_t instanceId, bool isTransport) = 0;
    virtual void setInstanceHidden(uint32_t instanceId, bool hidden) = 0;
    /// Plays the ship's doodad animation (moving or stopped); returns how many doodads took it.
    virtual size_t setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId, bool loop) = 0;
    virtual bool instanceHasCollisionGeometry(uint32_t instanceId) const = 0;
    virtual std::optional<float> getInstanceFloorHeight(uint32_t instanceId, float glX, float glY,
                                                        float glZ, float* outNormalZ = nullptr) const = 0;
};

/// The same for transports that are M2 models (tram cars, lifts).
class ITransportM2Target {
public:
    virtual ~ITransportM2Target() = default;

    virtual void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) = 0;
    virtual bool getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin, glm::vec3& outMax) const = 0;
    virtual std::optional<float> getInstanceFloorHeight(uint32_t instanceId, float glX, float glY,
                                                        float glZ) const = 0;
};

} // namespace game
} // namespace wowee
