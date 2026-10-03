// Vita skeleton of CharacterPreview (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/character_preview.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/character_preview.hpp"

#include "rendering/camera.hpp"
#include "rendering/character_renderer.hpp"

namespace wowee::rendering {

bool CharacterPreview::applyEquipment([[maybe_unused]] const std::vector<game::EquipmentItem>& equipment) { return false; }

CharacterPreview::CharacterPreview() {}

CharacterPreview::~CharacterPreview() = default;

bool CharacterPreview::loadCreature([[maybe_unused]] const std::string& m2Path,
                                    [[maybe_unused]] const std::vector<std::pair<uint32_t, std::string>>& skins) {
    return false;
}

UiTexture CharacterPreview::getTextureId() const { return {}; }

bool CharacterPreview::initialize([[maybe_unused]] pipeline::AssetManager* am, [[maybe_unused]] int width, [[maybe_unused]] int height) { return false; }

bool CharacterPreview::loadCharacter([[maybe_unused]] game::Race race, [[maybe_unused]] game::Gender gender, [[maybe_unused]] uint8_t skin, [[maybe_unused]] uint8_t face, [[maybe_unused]] uint8_t hairStyle, [[maybe_unused]] uint8_t hairColor, [[maybe_unused]] uint8_t facialHair, [[maybe_unused]] bool useFemaleModel) { return false; }

void CharacterPreview::render() { }

void CharacterPreview::resetView() { }

void CharacterPreview::rotate([[maybe_unused]] float yawDelta) { }

bool CharacterPreview::setBakedSkin([[maybe_unused]] const std::string& bakePath) { return false; }

void CharacterPreview::setPortraitFraming() { }

void CharacterPreview::setTransparentBackground([[maybe_unused]] bool transparent) { }

void CharacterPreview::update([[maybe_unused]] float deltaTime) { }

void CharacterPreview::zoom([[maybe_unused]] float wheelDelta) { }

}  // namespace wowee::rendering
