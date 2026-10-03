// Vita skeleton of FootprintRenderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/footprint_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/footprint_renderer.hpp"

namespace wowee::rendering {

void FootprintRenderer::clear() { }

}  // namespace wowee::rendering
