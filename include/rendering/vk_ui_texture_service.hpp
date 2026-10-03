#pragma once

// The Vulkan side of the UI texture handle (VITA-12): conversion between ImGui's descriptor sets and UiTexture,
// and the IUiTextureService that the interface talks to, over a VkContext. Everything outside rendering/ sees
// only rendering/ui_texture.hpp; the Vulkan types stop here.

#include "rendering/ui_texture.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <type_traits>

namespace wowee::rendering {

class VkContext;

/// A descriptor set from ImGui_ImplVulkan_AddTexture, as the id the interface holds. 64-bit builds make the handle
/// a pointer and 32-bit builds a 64-bit integer; either way it round-trips exactly (tests/test_ui_texture.cpp).
/// (Templates only so that `if constexpr` discards the branch that does not apply to this build's handle type.)
template <typename Handle = VkDescriptorSet>
inline UiTexture toUiTexture(Handle set) {
    if constexpr (std::is_pointer_v<Handle>) {
        return UiTexture{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(set))};
    } else {
        return UiTexture{static_cast<uint64_t>(set)};
    }
}

/// The descriptor set behind an id, for rendering/ code that has to hand it back to Vulkan. VK_NULL_HANDLE for none.
template <typename Handle = VkDescriptorSet>
inline Handle toDescriptorSet(UiTexture texture) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(static_cast<uintptr_t>(texture.id));
    } else {
        return static_cast<Handle>(texture.id);
    }
}

/// IUiTextureService over a VkContext. Does not own it: the Window creates one next to the context and drops it first.
class VkUiTextureService final : public IUiTextureService {
public:
    explicit VkUiTextureService(VkContext& ctx) : ctx_(ctx) {}

    UiTexture upload(const uint8_t* rgba, int width, int height) override;
    [[nodiscard]] uint32_t generation() const override;
    void beginUploadBatch() override;
    void endUploadBatchSync() override;

private:
    VkContext& ctx_;
};

}  // namespace wowee::rendering
