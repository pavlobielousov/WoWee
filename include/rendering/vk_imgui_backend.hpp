#pragma once

// IImGuiBackend over Vulkan (VITA-12): the ImGui_ImplVulkan_* and ImGui_ImplSDL3_* calls that UIManager made itself.

#include "rendering/imgui_backend.hpp"

namespace wowee::rendering {

class VkContext;

class VkImGuiBackend final : public IImGuiBackend {
public:
    explicit VkImGuiBackend(VkContext* ctx) : ctx_(ctx) {}

    bool init(SDL_Window* window) override;
    void newFrame() override;
    void shutdown() override;

    /// The Vulkan context is being destroyed. The backend object outlives it (as the Window does), and shutdown()
    /// after this still stops ImGui's halves, only without the wait: exactly what the interface did when the window's
    /// context pointer had already been cleared.
    void contextGone() { ctx_ = nullptr; }

private:
    VkContext* ctx_ = nullptr;
};

}  // namespace wowee::rendering
