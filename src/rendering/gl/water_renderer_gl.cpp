// Vita skeleton of WaterRenderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/water_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/water_renderer.hpp"

namespace wowee::rendering {

std::optional<float> WaterRenderer::getWaterHeightAt([[maybe_unused]] float glX, [[maybe_unused]] float glY) const { return {}; }

}  // namespace wowee::rendering
