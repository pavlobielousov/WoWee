#include "rendering/vk_imgui_backend.hpp"

#include "core/logger.hpp"
#include "rendering/vk_context.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

namespace wowee::rendering {

bool VkImGuiBackend::init(SDL_Window* window) {
    if (!ctx_) return false;

    // Initialize ImGui for SDL + Vulkan
    ImGui_ImplSDL3_InitForVulkan(window);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_1;
    initInfo.Instance = ctx_->getInstance();
    initInfo.PhysicalDevice = ctx_->getPhysicalDevice();
    initInfo.Device = ctx_->getDevice();
    initInfo.QueueFamily = ctx_->getGraphicsQueueFamily();
    initInfo.Queue = ctx_->getGraphicsQueue();
    initInfo.DescriptorPool = ctx_->getImGuiDescriptorPool();
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = ctx_->getSwapchainImageCount();
    // The UI renders in the overlay pass, which is single-sampled on purpose:
    // ImGui draws axis-aligned rects and pre-antialiased glyphs, so MSAA buys
    // almost nothing there and costs fill rate at the sample count the scene uses.
    initInfo.PipelineInfoMain.RenderPass = ctx_->getOverlayRenderPass();
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS)
            LOG_ERROR("ImGui Vulkan error: ", static_cast<int>(err));
    };

    ImGui_ImplVulkan_Init(&initInfo);
    return true;
}

void VkImGuiBackend::newFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
}

void VkImGuiBackend::shutdown() {
    if (ctx_) {
        vkDeviceWaitIdle(ctx_->getDevice());
    }

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
}

}  // namespace wowee::rendering
