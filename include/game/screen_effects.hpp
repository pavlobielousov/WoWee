#pragma once

namespace wowee {
namespace game {

/// Full-screen effects the game logic asks for, without knowing what draws them (VITA-45).
///
/// The game side says what is happening to the player; the renderer decides how it looks. A client
/// with no renderer (the headless core) leaves GameServices::screenEffects null and the calls are
/// skipped.
class IScreenEffects {
public:
    virtual ~IScreenEffects() = default;

    /// 0 = sober, 1 = as drunk as the game gets. Clamped by the implementation.
    virtual void setIntoxication(float amount) = 0;
};

} // namespace game
} // namespace wowee
