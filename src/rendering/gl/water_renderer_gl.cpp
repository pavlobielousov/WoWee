// Vita skeleton of WaterRenderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/water_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/water_renderer.hpp"

namespace wowee::rendering {

std::optional<float> WaterRenderer::getWaterHeightAt([[maybe_unused]] float glX, [[maybe_unused]] float glY) const { return {}; }

void WaterRenderer::clear() { }

std::optional<float> WaterRenderer::getNearestWaterHeightAt([[maybe_unused]] float glX, [[maybe_unused]] float glY, [[maybe_unused]] float queryZ, [[maybe_unused]] float maxAbove) const { return {}; }

std::optional<uint16_t> WaterRenderer::getWaterTypeAt([[maybe_unused]] float glX, [[maybe_unused]] float glY) const { return {}; }

void WaterRenderer::loadFromTerrain([[maybe_unused]] const pipeline::ADTTerrain& terrain, [[maybe_unused]] bool append, [[maybe_unused]] int tileX, [[maybe_unused]] int tileY) { }

void WaterRenderer::loadFromWMO([[maybe_unused]] const pipeline::WMOLiquid& liquid, [[maybe_unused]] const glm::mat4& modelMatrix, [[maybe_unused]] uint32_t wmoId) { }

void WaterRenderer::removeTile([[maybe_unused]] int tileX, [[maybe_unused]] int tileY) { }

void WaterRenderer::removeWMO([[maybe_unused]] uint32_t wmoId) { }

}  // namespace wowee::rendering
