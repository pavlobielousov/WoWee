#include "rendering/renderer_spell_visuals.hpp"

#include "rendering/renderer.hpp"
#include "rendering/spell_visual_system.hpp"

namespace wowee {
namespace rendering {

void RendererSpellVisuals::playSpellVisual(uint32_t visualId, const glm::vec3& position,
                                           bool useImpactKit, uint32_t attachInstanceId) {
    if (auto* svs = renderer_.getSpellVisualSystem())
        svs->playSpellVisual(visualId, position, useImpactKit, attachInstanceId);
}

void RendererSpellVisuals::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& position,
                                                  uint32_t castTimeMs, uint32_t attachInstanceId) {
    if (auto* svs = renderer_.getSpellVisualSystem())
        svs->playSpellVisualPrecast(visualId, position, castTimeMs, attachInstanceId);
}

void RendererSpellVisuals::playPhysicalProjectile(const std::string& modelPath,
                                                  const std::string& texturePath,
                                                  const glm::vec3& start, const glm::vec3& end,
                                                  float duration, bool spin) {
    if (auto* svs = renderer_.getSpellVisualSystem())
        svs->playPhysicalProjectile(modelPath, texturePath, start, end, duration, spin);
}

void RendererSpellVisuals::cancelAllPrecastVisuals() {
    if (auto* svs = renderer_.getSpellVisualSystem()) svs->cancelAllPrecastVisuals();
}

} // namespace rendering
} // namespace wowee
