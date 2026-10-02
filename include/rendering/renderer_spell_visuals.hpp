#pragma once

#include "game/spell_visuals.hpp"

namespace wowee {
namespace rendering {

class Renderer;

/// game::ISpellVisuals on top of the Vulkan renderer's SpellVisualSystem (VITA-45). The system is
/// looked up on every call, as the game code did, because it does not exist until the renderer
/// has initialised.
class RendererSpellVisuals final : public game::ISpellVisuals {
public:
    explicit RendererSpellVisuals(Renderer& renderer) : renderer_(renderer) {}

    void playSpellVisual(uint32_t visualId, const glm::vec3& position, bool useImpactKit,
                         uint32_t attachInstanceId) override;
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& position, uint32_t castTimeMs,
                                uint32_t attachInstanceId) override;
    void playPhysicalProjectile(const std::string& modelPath, const std::string& texturePath,
                                const glm::vec3& start, const glm::vec3& end, float duration,
                                bool spin) override;
    void cancelAllPrecastVisuals() override;

private:
    Renderer& renderer_;
};

} // namespace rendering
} // namespace wowee
