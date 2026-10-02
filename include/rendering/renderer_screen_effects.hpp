#pragma once

#include "game/screen_effects.hpp"

namespace wowee {
namespace rendering {

class Renderer;

/// game::IScreenEffects on top of the Vulkan renderer: the intoxication wobble is split between the
/// camera controller and the post-process pipeline, and this is the one place that knows it (VITA-45).
class RendererScreenEffects final : public game::IScreenEffects {
public:
    explicit RendererScreenEffects(Renderer& renderer) : renderer_(renderer) {}

    void setIntoxication(float amount) override;

private:
    Renderer& renderer_;
};

} // namespace rendering
} // namespace wowee
