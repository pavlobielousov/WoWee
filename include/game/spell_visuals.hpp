#pragma once

#include <cstdint>
#include <glm/vec3.hpp>
#include <string>

namespace wowee {
namespace game {

/// What the spell logic asks the renderer to show (VITA-45). Positions are in render space.
/// A client with no renderer (the headless core) leaves GameServices::spellVisuals null and the
/// callers skip the effect.
class ISpellVisuals {
public:
    virtual ~ISpellVisuals() = default;

    /// A cast or impact effect (SpellVisual.dbc id). useImpactKit false = cast kit, true = impact
    /// kit. attachInstanceId is the caster's model instance for hand/chest/head tracking; 0 = a
    /// static effect at the position.
    virtual void playSpellVisual(uint32_t visualId, const glm::vec3& position,
                                 bool useImpactKit = false, uint32_t attachInstanceId = 0) = 0;

    /// The effect that plays while a spell is being cast. castTimeMs 0 = the animation's length.
    virtual void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& position,
                                        uint32_t castTimeMs = 0, uint32_t attachInstanceId = 0) = 0;

    /// A weapon projectile (arrow, bullet, thrown item), outside the spell visual pipeline.
    virtual void playPhysicalProjectile(const std::string& modelPath, const std::string& texturePath,
                                        const glm::vec3& start, const glm::vec3& end,
                                        float duration, bool spin) = 0;

    /// Remove the precast effects still showing (the cast was cancelled or interrupted).
    virtual void cancelAllPrecastVisuals() = 0;
};

} // namespace game
} // namespace wowee
