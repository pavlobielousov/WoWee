// Vita skeleton of LensFlare (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/lens_flare.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/lens_flare.hpp"

namespace wowee::rendering {

void LensFlare::setIntensity([[maybe_unused]] float intensity) { }

}  // namespace wowee::rendering
