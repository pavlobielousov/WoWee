// Vita skeleton of AnimationController (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/animation_controller.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/animation_controller.hpp"

namespace wowee::rendering {

void AnimationController::clearMount() { }

void AnimationController::emitChargeEffect([[maybe_unused]] const glm::vec3& position, [[maybe_unused]] const glm::vec3& direction) { }

uint32_t AnimationController::getEmoteAnimByEmotesId([[maybe_unused]] uint32_t emoteId) { return 0; }

uint32_t AnimationController::getEmoteDbcId([[maybe_unused]] const std::string& emoteName) { return 0; }

std::string AnimationController::getEmoteText([[maybe_unused]] const std::string& emoteName, [[maybe_unused]] const std::string* targetName) { return {}; }

void AnimationController::playEmote([[maybe_unused]] const std::string& emoteName) { }

void AnimationController::playWeaponSheathAnimation([[maybe_unused]] SheathSpot mainHand, [[maybe_unused]] SheathSpot offHand) { }

void AnimationController::setCharging([[maybe_unused]] bool charging) { }

void AnimationController::setEquippedRangedType([[maybe_unused]] RangedWeaponType type) { }

void AnimationController::setEquippedWeaponType([[maybe_unused]] uint32_t inventoryType, [[maybe_unused]] bool is2HLoose, [[maybe_unused]] bool isFist, [[maybe_unused]] bool isDagger, [[maybe_unused]] bool hasOffHand, [[maybe_unused]] bool hasShield, [[maybe_unused]] bool offHandIsFist, [[maybe_unused]] bool offHandIsDagger) { }

void AnimationController::setInCombat([[maybe_unused]] bool combat) { }

void AnimationController::setLowHealth([[maybe_unused]] bool low) { }

void AnimationController::setMounted([[maybe_unused]] uint32_t mountInstId, [[maybe_unused]] uint32_t mountDisplayId, [[maybe_unused]] float heightOffset, [[maybe_unused]] const std::string& modelPath) { }

void AnimationController::setRangedWeaponActive([[maybe_unused]] bool active) { }

void AnimationController::setSeatedLoopAnimation([[maybe_unused]] uint32_t animationId) { }

void AnimationController::setSprintAuraActive([[maybe_unused]] bool active) { }

void AnimationController::setStandState([[maybe_unused]] uint8_t state) { }

void AnimationController::setStealthed([[maybe_unused]] bool stealth) { }

void AnimationController::setStunned([[maybe_unused]] bool stunned) { }

void AnimationController::setTargetPosition([[maybe_unused]] const glm::vec3* pos) { }

void AnimationController::startChargeEffect([[maybe_unused]] const glm::vec3& position, [[maybe_unused]] const glm::vec3& direction) { }

void AnimationController::startLooting() { }

void AnimationController::startSpellCast([[maybe_unused]] uint32_t precastAnimId, [[maybe_unused]] uint32_t castAnimId, [[maybe_unused]] bool castLoop, [[maybe_unused]] uint32_t finalizeAnimId) { }

void AnimationController::stopChargeEffect() { }

void AnimationController::stopLooting() { }

void AnimationController::stopSpellCast() { }

void AnimationController::triggerHitReaction([[maybe_unused]] uint32_t animId) { }

void AnimationController::triggerLevelUpEffect([[maybe_unused]] const glm::vec3& position) { }

void AnimationController::triggerMeleeSwing() { }

void AnimationController::triggerRangedShot() { }

void AnimationController::triggerSpecialAttack([[maybe_unused]] uint32_t spellId) { }

}  // namespace wowee::rendering
