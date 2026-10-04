// Vita skeleton of CharacterRenderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/character_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/character_renderer.hpp"

namespace wowee::rendering {

bool CharacterRenderer::attachWeapon([[maybe_unused]] uint32_t charInstanceId, [[maybe_unused]] uint32_t attachmentId, [[maybe_unused]] const pipeline::M2Model& weaponModel, [[maybe_unused]] uint32_t weaponModelId, [[maybe_unused]] const std::string& texturePath, [[maybe_unused]] const glm::mat4& localTransform) { return false; }

bool CharacterRenderer::attachWeaponEffect([[maybe_unused]] uint32_t charInstanceId, [[maybe_unused]] uint32_t attachmentId, [[maybe_unused]] uint32_t visualSlot, [[maybe_unused]] const pipeline::M2Model& effectModel, [[maybe_unused]] uint32_t effectModelId) { return false; }

void CharacterRenderer::clear() { }

void CharacterRenderer::clearCompositeCache() { }

void CharacterRenderer::clearTextureSlotOverride([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint16_t textureSlot) { }

GpuTexture* CharacterRenderer::compositeTextures([[maybe_unused]] const std::vector<std::string>& layerPaths) { return nullptr; }

GpuTexture* CharacterRenderer::compositeWithRegions([[maybe_unused]] const std::string& basePath, [[maybe_unused]] const std::vector<std::string>& baseLayers, [[maybe_unused]] const std::vector<std::pair<int, std::string>>& regionLayers) { return nullptr; }

uint32_t CharacterRenderer::createInstance([[maybe_unused]] uint32_t modelId, [[maybe_unused]] const glm::vec3& position, [[maybe_unused]] const glm::vec3& rotation, [[maybe_unused]] float scale) { return 0; }

void CharacterRenderer::detachWeapon([[maybe_unused]] uint32_t charInstanceId, [[maybe_unused]] uint32_t attachmentId) { }

void CharacterRenderer::detachWeaponEffects([[maybe_unused]] uint32_t charInstanceId, [[maybe_unused]] uint32_t attachmentId) { }

bool CharacterRenderer::getAnimationState([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t& animationId, [[maybe_unused]] float& animationTimeMs, [[maybe_unused]] float& animationDurationMs) const { return false; }

bool CharacterRenderer::getAttachmentTransform([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t attachmentId, [[maybe_unused]] glm::mat4& outTransform) { return false; }

bool CharacterRenderer::getInstanceBounds([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] glm::vec3& outCenter, [[maybe_unused]] float& outRadius) const { return false; }

bool CharacterRenderer::getInstanceFootZ([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] float& outFootZ) const { return false; }

const pipeline::M2Model* CharacterRenderer::getInstanceModelData([[maybe_unused]] uint32_t instanceId) const { return nullptr; }

bool CharacterRenderer::getInstancePosition([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] glm::vec3& outPos) const { return false; }

const pipeline::M2Model* CharacterRenderer::getModelData([[maybe_unused]] uint32_t modelId) const { return nullptr; }

bool CharacterRenderer::hasAnimation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId) const { return false; }

bool CharacterRenderer::loadModel([[maybe_unused]] const pipeline::M2Model& model, [[maybe_unused]] uint32_t id) { return false; }

GpuTexture* CharacterRenderer::loadTexture([[maybe_unused]] const std::string& path) { return nullptr; }

void CharacterRenderer::moveInstanceTo([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] const glm::vec3& destination, [[maybe_unused]] float durationSeconds) { }

void CharacterRenderer::playAnimation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint32_t animationId, [[maybe_unused]] bool loop, [[maybe_unused]] uint32_t oneShotReturnAnim) { }

void CharacterRenderer::processPendingNormalMaps([[maybe_unused]] int budget) { }

void CharacterRenderer::removeInstance([[maybe_unused]] uint32_t instanceId) { }

void CharacterRenderer::setActiveGeosets([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] const std::unordered_set<uint16_t>& geosets) { }

void CharacterRenderer::setDrawSkinExtra([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool enabled) { }

void CharacterRenderer::setGroupTextureOverride([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint16_t geosetGroup, [[maybe_unused]] GpuTexture* texture) { }

void CharacterRenderer::setInstanceOpacity([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] float opacity) { }

void CharacterRenderer::setInstancePosition([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] const glm::vec3& position) { }

void CharacterRenderer::setInstanceRotation([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] const glm::vec3& rotation) { }

void CharacterRenderer::setInstanceVisible([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] bool visible) { }

void CharacterRenderer::setModelTexture([[maybe_unused]] uint32_t modelId, [[maybe_unused]] uint32_t textureSlot, [[maybe_unused]] GpuTexture* texture) { }

void CharacterRenderer::setTextureSlotOverride([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] uint16_t textureSlot, [[maybe_unused]] GpuTexture* texture) { }

void CharacterRenderer::startFadeIn([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] float durationSeconds) { }

void CharacterRenderer::unloadModelIfUnused([[maybe_unused]] uint32_t modelId) { }

bool CharacterRenderer::getInstanceHeight([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] float& outHeight) const { return false; }

bool CharacterRenderer::getInstanceKeyBonePivotZ([[maybe_unused]] uint32_t instanceId, [[maybe_unused]] int32_t keyBoneId, [[maybe_unused]] float& outZ) const { return false; }

}  // namespace wowee::rendering
