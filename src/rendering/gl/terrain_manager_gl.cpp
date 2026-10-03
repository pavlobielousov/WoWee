// Vita skeleton of TerrainManager (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/terrain_manager.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/terrain_manager.hpp"

namespace wowee::rendering {

std::optional<float> TerrainManager::getHeightAt([[maybe_unused]] float glX, [[maybe_unused]] float glY) const { return {}; }

void TerrainManager::precacheTiles([[maybe_unused]] const std::vector<std::pair<int, int>>& tiles) { }

void TerrainManager::processOneReadyTile() { }

void TerrainManager::processReadyTiles() { }

void TerrainManager::softReset() { }

void TerrainManager::update([[maybe_unused]] const Camera& camera, [[maybe_unused]] float deltaTime) { }

}  // namespace wowee::rendering
