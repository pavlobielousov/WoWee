// Vita skeleton of WMORenderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/wmo_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/wmo_renderer.hpp"

namespace wowee::rendering {

void WMORenderer::addDoodadToInstance([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t m2InstanceId, [[maybe_unused]] const glm::mat4& localTransform) { }

void WMORenderer::clearAll() { }

void WMORenderer::clearInstances() { }

uint32_t WMORenderer::createInstance([[maybe_unused]] uint32_t modelId, [[maybe_unused]] const glm::vec3& position, [[maybe_unused]] const glm::vec3& rotation, [[maybe_unused]] float scale) { return 0; }

void WMORenderer::debugDumpGroupsAtPosition([[maybe_unused]] float glX, [[maybe_unused]] float glY, [[maybe_unused]] float glZ) const { }

const std::vector<WMORenderer::DoodadTemplate>* WMORenderer::getDoodadTemplates([[maybe_unused]] uint32_t modelId) const { return nullptr; }

bool WMORenderer::hasInstance([[maybe_unused]] uint32_t instanceId) const { return false; }

bool WMORenderer::instanceHasCollisionGeometry([[maybe_unused]] uint32_t instanceId) const { return false; }

bool WMORenderer::isModelLoaded([[maybe_unused]] uint32_t id) const { return false; }

bool WMORenderer::loadModel([[maybe_unused]] const pipeline::WMOModel& model, [[maybe_unused]] uint32_t id) { return false; }

WMORenderer::ModelLoadResult WMORenderer::loadModelIncremental([[maybe_unused]] const pipeline::WMOModel& model, [[maybe_unused]] uint32_t id, [[maybe_unused]] float budgetMs) { return ModelLoadResult::Failed; }

void WMORenderer::removeInstance([[maybe_unused]] uint32_t instanceId) { }

size_t WMORenderer::setInstanceDoodadAnimation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId, [[maybe_unused]] bool loop) { return 0; }

void WMORenderer::setInstanceHidden([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool hidden) { }

void WMORenderer::setInstanceIsTransport([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool isTransport) { }

void WMORenderer::setInstanceTransform([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] const glm::mat4& transform) { }

}  // namespace wowee::rendering
