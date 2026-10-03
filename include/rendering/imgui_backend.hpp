#pragma once

// The renderer's half of ImGui (VITA-12, ADR-001): what turns ImGui's draw data into GPU work, and the platform half
// that feeds it input, started and stopped together. UIManager used to call ImGui_ImplVulkan_* and
// ImGui_ImplSDL3_InitForVulkan itself, which put Vulkan into the interface layer; it now asks the window for this.
// No Vulkan, no ImGui and no SDL header: SDL_Window is only named.

struct SDL_Window;

namespace wowee::rendering {

class IImGuiBackend {
public:
    virtual ~IImGuiBackend() = default;

    /// Start both halves for `window`. The ImGui context and its style must already exist. False if the renderer has
    /// nothing to draw with (its context is gone).
    virtual bool init(SDL_Window* window) = 0;

    /// Once per frame, before ImGui::NewFrame(): the renderer half, then the platform half.
    virtual void newFrame() = 0;

    /// Wait for the GPU if it may still be reading ImGui's resources, then stop both halves. Safe when the renderer's
    /// context is already gone: it skips the wait and still stops them. The ImGui context is the caller's to destroy.
    virtual void shutdown() = 0;
};

}  // namespace wowee::rendering
