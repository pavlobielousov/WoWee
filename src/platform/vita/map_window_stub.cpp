// The detached world map window has no Vita counterpart: a second OS window with its own swapchain, for a
// second monitor (VITA-52). The shared Application builds a MapWindow and asks it to open; this one never
// does, so every call that follows is a no-op. src/ui/map_window.cpp is not part of the Vita build.
#include "ui/map_window.hpp"

#include "rendering/world_map/world_map_facade.hpp"

// MapWindow owns an AuxSwapchain through unique_ptr, so its destructor needs the type complete. The Vita has no
// second swapchain; an empty definition satisfies the deleter (the shadow header for it is not usable here).
namespace wowee::rendering { class AuxSwapchain {}; }

namespace wowee::ui {

MapWindow::MapWindow() = default;
MapWindow::~MapWindow() = default;

bool MapWindow::open(SDL_Window*, rendering::Renderer*, rendering::VkContext*, pipeline::AssetManager*,
                     const UIManager*) {
    return false;
}

void MapWindow::close() {}
bool MapWindow::takeClosedByPlayer() { return false; }
bool MapWindow::handleEvent(SDL_Event&) { return false; }
void MapWindow::withContext(const std::function<void()>&) {}
void MapWindow::buildFrame(bool, const glm::vec3&, float, uint32_t) {}

}  // namespace wowee::ui
