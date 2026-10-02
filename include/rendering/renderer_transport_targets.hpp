#pragma once

#include "game/transport_render_target.hpp"

namespace wowee {
namespace rendering {

class Renderer;

/// game::ITransportWmoTarget on the renderer's WMORenderer (VITA-45). The WMORenderer is looked up on
/// every call and the call skipped when there is none, because it exists only while a world is loaded.
class RendererTransportWmoTarget final : public game::ITransportWmoTarget {
public:
    explicit RendererTransportWmoTarget(Renderer& renderer) : renderer_(renderer) {}

    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) override;
    void setInstanceIsTransport(uint32_t instanceId, bool isTransport) override;
    void setInstanceHidden(uint32_t instanceId, bool hidden) override;
    size_t setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId, bool loop) override;
    bool instanceHasCollisionGeometry(uint32_t instanceId) const override;
    std::optional<float> getInstanceFloorHeight(uint32_t instanceId, float glX, float glY, float glZ,
                                                float* outNormalZ) const override;

private:
    Renderer& renderer_;
};

/// game::ITransportM2Target on the renderer's M2Renderer, looked up per call in the same way.
class RendererTransportM2Target final : public game::ITransportM2Target {
public:
    explicit RendererTransportM2Target(Renderer& renderer) : renderer_(renderer) {}

    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) override;
    bool getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin, glm::vec3& outMax) const override;
    std::optional<float> getInstanceFloorHeight(uint32_t instanceId, float glX, float glY,
                                                float glZ) const override;

private:
    Renderer& renderer_;
};

} // namespace rendering
} // namespace wowee
