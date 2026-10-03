// Vita skeleton of QuestMarkerRenderer (VITA-52, ADR-001): every method the shared code links against, doing nothing yet.
// The real bodies come with VITA-13/18/19/20. The class is declared by the shadow header
// (cmake/vita/shadow/rendering/quest_marker_renderer.hpp) or, where upstream's header is Vulkan-free, by upstream's own.
#include "rendering/quest_marker_renderer.hpp"

namespace wowee::rendering {

void QuestMarkerRenderer::clear() { }

void QuestMarkerRenderer::setMarker([[maybe_unused]] uint64_t guid, [[maybe_unused]] const glm::vec3& position, [[maybe_unused]] int markerType, [[maybe_unused]] float boundingHeight, [[maybe_unused]] float grayscale) { }

}  // namespace wowee::rendering
