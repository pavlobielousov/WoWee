// UiTexture, the opaque id the interface holds for a texture ImGui can draw (VITA-12), and its conversions to and
// from the Vulkan descriptor set it stands for.
//
// The interface used to hold the descriptor set itself. These pin the two things the change must not lose: the
// number ImGui draws is exactly the descriptor set the renderer registered (a texture that comes back different
// is a blank or wrong picture, found only by looking), and "no texture" stays "no texture" both ways.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "rendering/ui_texture.hpp"
#include "rendering/vk_ui_texture_service.hpp"

using wowee::rendering::kNoUiTexture;
using wowee::rendering::toDescriptorSet;
using wowee::rendering::toUiTexture;
using wowee::rendering::UiTexture;

// The strong-type properties that make a leftover `== VK_NULL_HANDLE` or a stray conversion a compile error
// instead of a silent change of meaning. A weakening of any of these fails the build, not just this test.
static_assert(!std::is_convertible_v<uint64_t, UiTexture>, "an integer must not become a texture by accident");
static_assert(!std::is_convertible_v<VkDescriptorSet, UiTexture>, "a descriptor set must go through toUiTexture");
static_assert(!std::is_convertible_v<UiTexture, bool>, "truth is explicit: `if (tex)`, never `bool b = tex`");
static_assert(!std::is_convertible_v<UiTexture, uint64_t>, "the number ImGui takes is imguiId(), asked for by name");
static_assert(std::is_trivially_copyable_v<UiTexture> && sizeof(UiTexture) == sizeof(uint64_t));

TEST_CASE("a default texture is no texture", "[ui_texture]") {
    UiTexture none;
    CHECK_FALSE(static_cast<bool>(none));
    CHECK(none == kNoUiTexture);
    CHECK(none.id == 0);
    CHECK(none.imguiId() == 0);
    CHECK(static_cast<bool>(UiTexture{1}));
}

TEST_CASE("null maps to no texture and back", "[ui_texture]") {
    CHECK(toUiTexture(static_cast<VkDescriptorSet>(VK_NULL_HANDLE)) == kNoUiTexture);
    CHECK(toDescriptorSet(kNoUiTexture) == static_cast<VkDescriptorSet>(VK_NULL_HANDLE));
}

TEST_CASE("every descriptor set round-trips exactly, high bits included", "[ui_texture]") {
    // Real handles are pointers on 64-bit builds (heap addresses, possibly with bits set up high on some
    // platforms) and 64-bit integers on 32-bit builds. Whatever the build, the value must come back unchanged.
    const uint64_t values[] = {
        0x1ull,
        0x7ull,
        0x00007f12deadbee0ull,
        0x0000000100000000ull,
        0x7fffffffffffff00ull,
        0xffff800000001000ull,
        0xfffffffffffffff0ull,
    };
    for (uint64_t v : values) {
        // Only what the build's handle can hold: a 32-bit pointer cannot carry the upper half, and does not need to.
        if (sizeof(VkDescriptorSet) == 4 && (v >> 32) != 0) continue;
        VkDescriptorSet original = toDescriptorSet(UiTexture{v});
        UiTexture id = toUiTexture(original);
        INFO("value 0x" << std::hex << v);
        CHECK(id.id == v);
        CHECK(id.imguiId() == v);
        CHECK(toDescriptorSet(id) == original);
        CHECK(static_cast<bool>(id));
    }
}

TEST_CASE("textures can key a map and a set", "[ui_texture]") {
    std::unordered_map<UiTexture, int> map;
    map[UiTexture{5}] = 1;
    map[UiTexture{6}] = 2;
    map[UiTexture{5}] += 10;
    CHECK(map.size() == 2);
    CHECK(map[UiTexture{5}] == 11);
    std::unordered_set<UiTexture> set{UiTexture{1}, UiTexture{2}, UiTexture{1}};
    CHECK(set.size() == 2);
    CHECK(UiTexture{1} != UiTexture{2});
}
