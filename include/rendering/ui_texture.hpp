#pragma once

// A texture the interface can draw, and the service that makes them (VITA-12, ADR-001).
//
// The interface used to hold these as VkDescriptorSet, the thing ImGui's Vulkan backend hands back for a
// texture, which put a Vulkan type into every panel that draws an icon. Outside rendering/ a texture is only
// ever something to keep, compare with "none", and give to ImGui, so it is exactly that here: an opaque id.
// No Vulkan, no ImGui, no other header.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace wowee::rendering {

/// An id the renderer gave out for a texture ImGui can draw. Zero is "no texture".
///
/// A struct rather than a typedef of an integer on purpose: the old handle compared against VK_NULL_HANDLE and
/// came back from functions as a pointer, and with a bare integer a leftover `== VK_NULL_HANDLE` or a stray
/// conversion would still compile and mean something else. Here every such spot is a compile error.
struct UiTexture {
    uint64_t id = 0;

    constexpr UiTexture() = default;
    constexpr explicit UiTexture(uint64_t value) : id(value) {}

    /// True when there is a texture: `if (UiTexture t = icon(...))` works as it did with the pointer.
    constexpr explicit operator bool() const { return id != 0; }
    constexpr bool operator==(const UiTexture&) const = default;

    /// What ImGui::Image and ImDrawList take (ImTextureID is a 64-bit integer by default). Not a cast of the id:
    /// the id IS what the renderer registered with ImGui, so the two are the same number by construction.
    [[nodiscard]] constexpr uint64_t imguiId() const { return id; }
};

inline constexpr UiTexture kNoUiTexture{};

/// What the interface needs from a renderer to put pictures on screen: upload a decoded image, ask whether
/// earlier uploads are still good, and batch uploads while a loading screen is up. The widget renderer used to
/// reach all four through VkContext. Implemented over Vulkan in rendering/ today (vk_ui_texture_service.hpp);
/// the Vita implements it over vitaGL.
class IUiTextureService {
public:
    virtual ~IUiTextureService() = default;

    /// Upload 8-bit RGBA pixels and register the result with ImGui. kNoUiTexture on failure. The caller does not
    /// free it; the renderer tracks it and releases it with itself.
    virtual UiTexture upload(const uint8_t* rgba, int width, int height) = 0;

    /// Which incarnation of the UI textures is current. A holder caches it beside its ids and drops its cache when
    /// this changes: the renderer rebuilt them (device reset, swapchain recreation) and the old ids are dead.
    [[nodiscard]] virtual uint32_t generation() const = 0;

    /// Several uploads under one wait instead of one each: begin, upload any number, end. End waits (loading screens).
    virtual void beginUploadBatch() = 0;
    virtual void endUploadBatchSync() = 0;
};

}  // namespace wowee::rendering

template <>
struct std::hash<wowee::rendering::UiTexture> {
    size_t operator()(const wowee::rendering::UiTexture& t) const noexcept { return std::hash<uint64_t>{}(t.id); }
};
