// Vita skeleton of M2Renderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/m2_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/m2_renderer.hpp"

namespace wowee::rendering {

void M2Renderer::clear() { }

void M2Renderer::clearInstanceHighlights() { }

uint32_t M2Renderer::createInstance([[maybe_unused]] uint32_t modelId, [[maybe_unused]] const glm::vec3& position, [[maybe_unused]] const glm::vec3& rotation, [[maybe_unused]] float scale, [[maybe_unused]] bool allowPositionDedup) { return 0; }

uint32_t M2Renderer::createInstanceWithMatrix([[maybe_unused]] uint32_t modelId, [[maybe_unused]] const glm::mat4& modelMatrix, [[maybe_unused]] const glm::vec3& position) { return 0; }

float M2Renderer::getInstanceAnimDuration([[maybe_unused]] uint32_t instanceId) const { return 0; }

bool M2Renderer::getInstanceBounds([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] glm::vec3& outCenter, [[maybe_unused]] float& outRadius) const { return false; }

bool M2Renderer::hasAnimation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId) const { return false; }

bool M2Renderer::hasModel([[maybe_unused]] uint32_t modelId) const { return false; }

bool M2Renderer::loadModel([[maybe_unused]] const pipeline::M2Model& model, [[maybe_unused]] uint32_t modelId) { return false; }

void M2Renderer::markModelAsSpellEffect([[maybe_unused]] uint32_t modelId) { }

void M2Renderer::removeInstance([[maybe_unused]] uint32_t instanceId) { }

void M2Renderer::restartInstanceAnimation([[maybe_unused]] uint32_t instanceId) { }

void M2Renderer::setInstanceAnimation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId, [[maybe_unused]] bool loop) { }

void M2Renderer::setInstanceAnimationFrozen([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool frozen) { }

void M2Renderer::setInstanceAnimationHeld([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId, [[maybe_unused]] bool skipToEnd) { }

void M2Renderer::setInstanceHighlight([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] float amount) { }

void M2Renderer::setInstanceIsGameObject([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool isGameObject) { }

void M2Renderer::setInstancePosition([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] const glm::vec3& position) { }

void M2Renderer::setInstanceTransform([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] const glm::mat4& transform) { }

void M2Renderer::setModelPinned([[maybe_unused]] uint32_t modelId, [[maybe_unused]] bool pinned) { }

void M2Renderer::setSkipCollision([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool skip) { }

void M2Renderer::setSkipWallCollision([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool skip) { }

std::optional<uint32_t> M2Renderer::soleSequenceId([[maybe_unused]] uint32_t instanceId) const { return {}; }

std::vector<uint32_t> M2Renderer::drainReapedModelIds() { return {}; }

void M2Renderer::removeInstances([[maybe_unused]] const std::vector<uint32_t>& instanceIds) { }

}  // namespace wowee::rendering
