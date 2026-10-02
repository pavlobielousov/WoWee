#include "rendering/renderer_transport_targets.hpp"

#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/wmo_renderer.hpp"

namespace wowee {
namespace rendering {

void RendererTransportWmoTarget::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    if (auto* wmo = renderer_.getWMORenderer()) wmo->setInstanceTransform(instanceId, transform);
}

void RendererTransportWmoTarget::setInstanceIsTransport(uint32_t instanceId, bool isTransport) {
    if (auto* wmo = renderer_.getWMORenderer()) wmo->setInstanceIsTransport(instanceId, isTransport);
}

void RendererTransportWmoTarget::setInstanceHidden(uint32_t instanceId, bool hidden) {
    if (auto* wmo = renderer_.getWMORenderer()) wmo->setInstanceHidden(instanceId, hidden);
}

size_t RendererTransportWmoTarget::setInstanceDoodadAnimation(uint32_t instanceId, uint32_t animationId,
                                                              bool loop) {
    auto* wmo = renderer_.getWMORenderer();
    return wmo ? wmo->setInstanceDoodadAnimation(instanceId, animationId, loop) : 0;
}

bool RendererTransportWmoTarget::instanceHasCollisionGeometry(uint32_t instanceId) const {
    auto* wmo = renderer_.getWMORenderer();
    return wmo && wmo->instanceHasCollisionGeometry(instanceId);
}

std::optional<float> RendererTransportWmoTarget::getInstanceFloorHeight(uint32_t instanceId, float glX,
                                                                        float glY, float glZ,
                                                                        float* outNormalZ) const {
    auto* wmo = renderer_.getWMORenderer();
    return wmo ? wmo->getInstanceFloorHeight(instanceId, glX, glY, glZ, outNormalZ) : std::nullopt;
}

void RendererTransportM2Target::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    if (auto* m2 = renderer_.getM2Renderer()) m2->setInstanceTransform(instanceId, transform);
}

bool RendererTransportM2Target::getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin,
                                                       glm::vec3& outMax) const {
    auto* m2 = renderer_.getM2Renderer();
    return m2 && m2->getInstanceWorldBounds(instanceId, outMin, outMax);
}

std::optional<float> RendererTransportM2Target::getInstanceFloorHeight(uint32_t instanceId, float glX,
                                                                       float glY, float glZ) const {
    auto* m2 = renderer_.getM2Renderer();
    return m2 ? m2->getInstanceFloorHeight(instanceId, glX, glY, glZ) : std::nullopt;
}

} // namespace rendering
} // namespace wowee
